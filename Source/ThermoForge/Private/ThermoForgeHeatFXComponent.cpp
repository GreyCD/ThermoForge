#include "ThermoForgeHeatFXComponent.h"
#include "ThermoForgeSubsystem.h"
#include "ThermoForgeSourceComponent.h"

#include "Components/PrimitiveComponent.h"
#include "GameFramework/Actor.h"
#include "Engine/World.h"
#include "TimerManager.h"
#include "EngineUtils.h"

static constexpr float TF_EPS_DIR   = 1e-3f;
static constexpr float TF_EPS_TEMP  = 1e-2f;
static constexpr float TF_EPS_DIST  = 0.5f;
static constexpr float TF_EPS_STR   = 1e-3f;

UThermoForgeHeatFXComponent::UThermoForgeHeatFXComponent()
{
	PrimaryComponentTick.bCanEverTick = false; // timer + transform callback
}

void UThermoForgeHeatFXComponent::BeginPlay()
{
	Super::BeginPlay();

	EnsureTargetPrimitive();

	if (AActor* Owner = GetOwner())
	{
		if (USceneComponent* Root = Owner->GetRootComponent())
		{
			Root->TransformUpdated.AddUObject(this, &UThermoForgeHeatFXComponent::HandleTransformUpdated);
		}
	}

	if (UWorld* W = GetWorld())
	{
		W->GetTimerManager().SetTimer(
			Timer, this, &UThermoForgeHeatFXComponent::TickHeat,
			UpdateRateSec, /*bLoop=*/true, /*FirstDelay=*/0.0f);
	}

	TickHeat(); // immediate, so materials/events are ready pre-gameplay
}

void UThermoForgeHeatFXComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	if (UWorld* W = GetWorld())
	{
		W->GetTimerManager().ClearTimer(Timer);
	}
	Super::EndPlay(EndPlayReason);
}

void UThermoForgeHeatFXComponent::HandleTransformUpdated(USceneComponent* Updated, EUpdateTransformFlags, ETeleportType)
{
	TickHeat();
}

void UThermoForgeHeatFXComponent::EnsureTargetPrimitive()
{
	// Try override first
	if (UPrimitiveComponent* Prim = ResolveOverridePrimitive())
	{
		TargetPrim = Prim;
		return;
	}

	// Fallback: first primitive on owner
	if (AActor* Owner = GetOwner())
	{
		if (UPrimitiveComponent* Found = Owner->FindComponentByClass<UPrimitiveComponent>())
		{
			TargetPrim = Found;
			return;
		}
	}

	TargetPrim = nullptr;
}

UPrimitiveComponent* UThermoForgeHeatFXComponent::ResolveOverridePrimitive() const
{
	if (AActor* Owner = GetOwner())
	{
		if (UActorComponent* Comp = OverridePrimitive.GetComponent(Owner))
		{
			return Cast<UPrimitiveComponent>(Comp);
		}
	}
	return nullptr;
}

float UThermoForgeHeatFXComponent::SampleTempAt(const FVector& P) const
{
	if (const UWorld* W = GetWorld())
	{
		if (const auto* TF = W->GetSubsystem<UThermoForgeSubsystem>())
		{
			// Wire your season/time/weather later as needed
			return TF->ComputeCurrentTemperatureAt(P, /*bWinter=*/false, /*TimeHours=*/12.f, /*WeatherAlpha01=*/0.3f);
		}
	}
	return 0.f;
}

bool UThermoForgeHeatFXComponent::ResolveOrigin_NearestSource(const FVector& CenterWS, FVector& OutOriginWS, float& OutStrength)
{
	float BestDistSq = TNumericLimits<float>::Max();
	FVector BestPos  = FVector::ZeroVector;
	bool bFound = false;

	for (TActorIterator<AActor> It(GetWorld()); It; ++It)
	{
		if (It->FindComponentByClass<UThermoForgeSourceComponent>())
		{
			const FVector Pos = It->GetActorLocation();
			const float Ds = FVector::DistSquared(CenterWS, Pos);
			if (Ds < BestDistSq)
			{
				BestDistSq = Ds;
				BestPos    = Pos;
				bFound     = true;
			}
		}
	}

	if (bFound)
	{
		const float Dist = FMath::Sqrt(BestDistSq);
		OutOriginWS  = BestPos;
		// A simple strength proxy: inverse distance (clamped)
		OutStrength  = (Dist > TF_EPS_DIST) ? (1.f / Dist) : 1.f;
		return true;
	}
	return false;
}

bool UThermoForgeHeatFXComponent::ResolveOrigin_Probe(const FVector& CenterWS, bool bFindHottest, FVector& OutOriginWS, float& OutStrength)
{
	if (!GetWorld()) return false;

	const float R   = FMath::Max(ProbeRadiusCm, 10.f);
	const int32 N   = FMath::Clamp(ProbeSamples, 4, 64);
	float BestTemp  = bFindHottest ? -FLT_MAX : +FLT_MAX;
	FVector BestPos = CenterWS;

	// Include center sample as well
	const float TCenter = SampleTempAt(CenterWS);
	BestTemp = TCenter;
	BestPos  = CenterWS;

	for (int32 i = 0; i < N; ++i)
	{
		const float Ang = (2.f * PI) * (float(i) / float(N));
		const FVector Offset = FVector(FMath::Cos(Ang), FMath::Sin(Ang), 0.f) * R;
		const FVector P = CenterWS + Offset;
		const float T  = SampleTempAt(P);

		if (bFindHottest)
		{
			if (T > BestTemp) { BestTemp = T; BestPos = P; }
		}
		else
		{
			if (T < BestTemp) { BestTemp = T; BestPos = P; }
		}
	}

	OutOriginWS = BestPos;

	// Strength proxy: temperature difference magnitude to center (non-negative)
	OutStrength = FMath::Abs(BestTemp - TCenter);

	return true;
}

void UThermoForgeHeatFXComponent::TickHeat()
{
	if (!GetWorld() || !GetOwner()) return;

	const FVector Center = GetOwner()->GetActorLocation();

	// 1) Temperature at owner
	const float TNow = SampleTempAt(Center);

	// 2) Resolve origin based on mode
	FVector OriginWS = FVector::ZeroVector;
	float Strength   = 0.f;
	bool  bOriginOK  = false;
	EThermoOriginMode UsedMode = OriginMode;

	switch (OriginMode)
	{
	case EThermoOriginMode::NearestSourceActor:
		bOriginOK = ResolveOrigin_NearestSource(Center, OriginWS, Strength);
		break;
	case EThermoOriginMode::HottestPoint:
		bOriginOK = ResolveOrigin_Probe(Center, /*bFindHottest=*/true,  OriginWS, Strength);
		break;
	case EThermoOriginMode::ColdestPoint:
		bOriginOK = ResolveOrigin_Probe(Center, /*bFindHottest=*/false, OriginWS, Strength);
		break;
	default:
		bOriginOK = false; break;
	}

	// 3) Build direction/distance
	FVector Dir = FVector::ZeroVector;
	float   Dist= 0.f;
	if (bOriginOK)
	{
		Dir  = (OriginWS - Center).GetSafeNormal();
		Dist = FVector::Distance(Center, OriginWS);
	}

	// 4) Determine if anything “meaningful” changed
	const bool bTempChanged   = !FMath::IsNearlyEqual(TNow, PrevTemperatureC, TF_EPS_TEMP);
	const bool bDirChanged    = !Dir.Equals(PrevHeatDirWS, 1e-3f);
	const bool bPosChanged    = !OriginWS.Equals(PrevSourcePosWS, 0.5f);
	const bool bDistChanged   = !FMath::IsNearlyEqual(Dist, PrevDistanceCm, TF_EPS_DIST);
	const bool bStrChanged    = !FMath::IsNearlyEqual(Strength, PrevStrength, TF_EPS_STR);
	const bool bAnyChanged    = bTempChanged || bDirChanged || bPosChanged || bDistChanged || bStrChanged || !bHadInitialFire;

	// 5) Update state & fire events
	if (bAnyChanged)
	{
		TemperatureC    = TNow;
		SourcePosWS     = OriginWS;
		HeatDirWS       = Dir;
		DistanceCm      = Dist;
		HeatStrength    = Strength;
		RuntimeOriginMode = UsedMode;
		bHasOrigin      = bOriginOK;

		// Big jump?
		const float DeltaC = TNow - PrevTemperatureC;
		if (FMath::Abs(DeltaC) >= ChangeThresholdC)
		{
			OnHeatJump.Broadcast(this, TNow, PrevTemperatureC, DeltaC, HeatStrength, HeatDirWS, DistanceCm, SourcePosWS);
		}

		// Always notify the “updated” event when anything changed (or first fire)
		OnHeatUpdated.Broadcast(this, TemperatureC, HeatStrength, HeatDirWS, DistanceCm, SourcePosWS, RuntimeOriginMode);

		// Cache previous
		PrevTemperatureC = TNow;
		PrevSourcePosWS  = OriginWS;
		PrevHeatDirWS    = Dir;
		PrevDistanceCm   = Dist;
		PrevStrength     = Strength;
		bHadInitialFire  = true;

		// Write CPD
		WriteCustomPrimitiveData();
	}

	// Optional: fire a one-shot at begin play (if nothing changed but user asked)
	if (!bHadInitialFire && bFireInitialEventOnBeginPlay)
	{
		// Pretend an update
		OnHeatUpdated.Broadcast(this, TNow, Strength, Dir, Dist, OriginWS, UsedMode);
		PrevTemperatureC = TNow;
		PrevSourcePosWS  = OriginWS;
		PrevHeatDirWS    = Dir;
		PrevDistanceCm   = Dist;
		PrevStrength     = Strength;
		bHadInitialFire  = true;
		WriteCustomPrimitiveData();
	}
}

void UThermoForgeHeatFXComponent::WriteCustomPrimitiveData()
{
	if (!bWriteCustomPrimitiveData) return;
	EnsureTargetPrimitive();
	if (!TargetPrim.IsValid()) return;

	const int32 I = CPDBaseIndex;

	// [0..2] HeatDirWS  -> Direction
	TargetPrim->SetCustomPrimitiveDataFloat(I + 0, HeatDirWS.X);
	TargetPrim->SetCustomPrimitiveDataFloat(I + 1, HeatDirWS.Y);
	TargetPrim->SetCustomPrimitiveDataFloat(I + 2, HeatDirWS.Z);

	// [3] Temperature   -> Temperature (used as Intensity in the MF)
	TargetPrim->SetCustomPrimitiveDataFloat(I + 3, TemperatureC);

	// [4..6] SourcePosWS -> Position
	TargetPrim->SetCustomPrimitiveDataFloat(I + 4, SourcePosWS.X);
	TargetPrim->SetCustomPrimitiveDataFloat(I + 5, SourcePosWS.Y);
	TargetPrim->SetCustomPrimitiveDataFloat(I + 6, SourcePosWS.Z);

	// [7] HeatStrength   -> HeatStrength
	TargetPrim->SetCustomPrimitiveDataFloat(I + 7, HeatStrength);

	// [8] ReferenceRadiusCm -> Radius
	TargetPrim->SetCustomPrimitiveDataFloat(I + 8, ReferenceRadiusCm);

}
