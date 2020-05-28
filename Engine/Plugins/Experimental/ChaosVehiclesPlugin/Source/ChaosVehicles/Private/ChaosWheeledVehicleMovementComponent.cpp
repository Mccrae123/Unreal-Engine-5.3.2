// Copyright Epic Games, Inc. All Rights Reserved.

#include "ChaosWheeledVehicleMovementComponent.h"
#include "Components/PrimitiveComponent.h"
#include "Components/SkinnedMeshComponent.h"
#include "Components/SkeletalMeshComponent.h"

#include "CanvasItem.h"
#include "Engine/Canvas.h"
#include "DrawDebugHelpers.h"
#include "DisplayDebugHelpers.h"
#include "PhysicalMaterials/PhysicalMaterial.h"
#include "ChaosVehicleManager.h"

using namespace Chaos;

#if VEHICLE_DEBUGGING_ENABLED
PRAGMA_DISABLE_OPTIMIZATION
#endif

#if WITH_CHAOS

struct VehicleDebugParams
{
	bool ShowCOM = false;
	bool ShowModelOrigin = false;
	bool ShowWheelCollisionNormal = false;
	bool ShowWheelRaycasts = false;
	bool ShowWheelForces = false;
	bool ShowSuspensionForces = false;

	bool DisableSuspensionForces = false;
	bool DisableFrictionForces = false;
	bool DisableRollbarForces = true;

	float ThrottleOverride = 0.f;
	float SteeringOverride = 0.f;
};


VehicleDebugParams GVehicleDebugParams;
EDebugPages UChaosWheeledVehicleMovementComponent::DebugPage = EDebugPages::BasicPage;

FAutoConsoleVariableRef CVarChaosVehiclesShowCOM(TEXT("p.Vehicles.ShowCOM"), GVehicleDebugParams.ShowCOM, TEXT("Enable/Disable Center Of Mass Debug Visualisation."));
FAutoConsoleVariableRef CVarChaosVehiclesShowModelAxis(TEXT("p.Vehicles.ShowModelOrigin"), GVehicleDebugParams.ShowModelOrigin, TEXT("Enable/Disable Model Origin Visualisation."));
FAutoConsoleVariableRef CVarChaosVehiclesShowWheelCollisionNormal(TEXT("p.Vehicles.ShowWheelCollisionNormal"), GVehicleDebugParams.ShowWheelCollisionNormal, TEXT("Enable/Disable Wheel Collision Normal Visualisation."));
FAutoConsoleVariableRef CVarChaosVehiclesShowWheelRaycasts(TEXT("p.Vehicles.ShowWheelRaycasts"), GVehicleDebugParams.ShowWheelRaycasts, TEXT("Enable/Disable Wheel Raycast Visualisation."));
FAutoConsoleVariableRef CVarChaosVehiclesShowWheelForces(TEXT("p.Vehicles.ShowWheelForces"), GVehicleDebugParams.ShowWheelForces, TEXT("Enable/Disable Wheel Forces Visualisation."));
FAutoConsoleVariableRef CVarChaosVehiclesShowSuspensionForces(TEXT("p.Vehicles.ShowSuspensionForces"), GVehicleDebugParams.ShowSuspensionForces, TEXT("Enable/Disable Suspension Forces Visualisation."));
FAutoConsoleVariableRef CVarChaosVehiclesDisableSuspensionForces(TEXT("p.Vehicles.DisableSuspensionForces"), GVehicleDebugParams.DisableSuspensionForces, TEXT("Enable/Disable Suspension Forces."));
FAutoConsoleVariableRef CVarChaosVehiclesDisableFrictionForces(TEXT("p.Vehicles.DisableFrictionForces"), GVehicleDebugParams.DisableFrictionForces, TEXT("Enable/Disable Wheel Fristion Forces."));

FAutoConsoleVariableRef CVarChaosVehiclesThrottleOverride(TEXT("p.Vehicles.ThrottleOverride"), GVehicleDebugParams.ThrottleOverride, TEXT("Hard code throttle input on."));
FAutoConsoleVariableRef CVarChaosVehiclesSteeringOverride(TEXT("p.Vehicles.SteeringOverride"), GVehicleDebugParams.SteeringOverride, TEXT("Hard code steering input on."));


FAutoConsoleCommand CVarCommandVehiclesNextDebugPage(
	TEXT("p.Vehicles.NextDebugPage"),
	TEXT("Display the next page of vehicle debug data."),
	FConsoleCommandDelegate::CreateStatic(UChaosWheeledVehicleMovementComponent::NextDebugPage));

FAutoConsoleCommand CVarCommandVehiclesPrevDebugPage(
	TEXT("p.Vehicles.PrevDebugPage"),
	TEXT("Display the previous page of vehicle debug data."),
	FConsoleCommandDelegate::CreateStatic(UChaosWheeledVehicleMovementComponent::PrevDebugPage));


UChaosWheeledVehicleMovementComponent::UChaosWheeledVehicleMovementComponent(const FObjectInitializer& ObjectInitializer) : Super(ObjectInitializer)
{
	// default values setup	
	EngineSetup.MaxRPM = 6000.f;
	EngineSetup.MaxTorque = 10000.f;
	EngineSetup.EngineIdleRPM = 1200.f;
	EngineSetup.EngineBrakeEffect = 0.001f;
	
	TransmissionSetup.ForwardGearRatios.Add(4.0f);
	TransmissionSetup.ForwardGearRatios.Add(3.0f);
	TransmissionSetup.ForwardGearRatios.Add(2.0f);
	TransmissionSetup.ForwardGearRatios.Add(1.0f);
	TransmissionSetup.FinalRatio = 4.0f;

	TransmissionSetup.ReverseGearRatios.Add(3.0f);
}


#if WITH_EDITOR
void UChaosWheeledVehicleMovementComponent::PostEditChangeProperty(struct FPropertyChangedEvent& PropertyChangedEvent)
{
	Super::PostEditChangeProperty(PropertyChangedEvent);
	const FName PropertyName = PropertyChangedEvent.Property ? PropertyChangedEvent.Property->GetFName() : NAME_None;

	if (PropertyName == TEXT("SteeringCurve"))
	{
		// make sure values are capped between 0 and 1
		TArray<FRichCurveKey> SteerKeys = SteeringCurve.GetRichCurve()->GetCopyOfKeys();
		for (int32 KeyIdx = 0; KeyIdx < SteerKeys.Num(); ++KeyIdx)
		{
			float NewValue = FMath::Clamp(SteerKeys[KeyIdx].Value, 0.f, 1.f);
			SteeringCurve.GetRichCurve()->UpdateOrAddKey(SteerKeys[KeyIdx].Time, NewValue);
		}
	}
}
#endif

float FVehicleEngineConfig::FindPeakTorque() const
{
	// Find max torque
	float PeakTorque = 0.f;
	TArray<FRichCurveKey> TorqueKeys = TorqueCurve.GetRichCurveConst()->GetCopyOfKeys();
	for (int32 KeyIdx = 0; KeyIdx < TorqueKeys.Num(); KeyIdx++)
	{
		FRichCurveKey& Key = TorqueKeys[KeyIdx];
		PeakTorque = FMath::Max(PeakTorque, Key.Value);
	}
	return PeakTorque;
}

//void UChaosWheeledVehicleMovementComponent::UpdateEngineSetup(const FVehicleEngineConfig& NewEngineSetup)
//{
//
//}
//
//void UChaosWheeledVehicleMovementComponent::UpdateDifferentialSetup(const FVehicleDifferentialConfig& NewDifferentialSetup)
//{
//}
//
//void UChaosWheeledVehicleMovementComponent::UpdateTransmissionSetup(const FVehicleTransmissionConfig& NewTransmissionSetup)
//{
//
//}

void UChaosWheeledVehicleMovementComponent::Serialize(FArchive & Ar)
{
	Super::Serialize(Ar);

	// custom serialization goes here..
}

void UChaosWheeledVehicleMovementComponent::ComputeConstants()
{
	Super::ComputeConstants();
}


void UChaosWheeledVehicleMovementComponent::CreateWheels()
{
	// Wheels num is getting copied when blueprint recompiles, so we have to manually reset here
	Wheels.Reset();

	// Instantiate the wheels
	for (int32 WheelIdx = 0; WheelIdx < WheelSetups.Num(); ++WheelIdx)
	{
		UChaosVehicleWheel* Wheel = NewObject<UChaosVehicleWheel>(this, WheelSetups[WheelIdx].WheelClass);
		check(Wheel);

		Wheels.Add(Wheel);
	}

	// Initialize the wheels
	for (int32 WheelIdx = 0; WheelIdx < Wheels.Num(); ++WheelIdx)
	{
		// #todo: any additional setup required for PVehicle->Wheels

		Wheels[WheelIdx]->Init(this, WheelIdx);
	}
}

void UChaosWheeledVehicleMovementComponent::DestroyWheels()
{
	for (int32 i = 0; i < Wheels.Num(); ++i)
	{
		Wheels[i]->Shutdown();
	}

	Wheels.Reset();
}

bool UChaosWheeledVehicleMovementComponent::CanSimulate() const
{
	if (Super::CanSimulate() == false)
	{
		return false;
	}

	return (PVehicle && PVehicle.IsValid()
		&& PVehicle->Engine.Num() > 0 && PVehicle->Engine.Num() == PVehicle->Transmission.Num()
		&& Wheels.Num() > 0 &&Wheels.Num() == PVehicle->Suspension.Num());
}

void UChaosWheeledVehicleMovementComponent::UpdateSimulation(float DeltaTime)
{
	bool IsClient = false;
	if (PawnOwner && PawnOwner->IsNetMode(NM_Client))
	{
		IsClient = true;
	}

	if (DeltaTime > 0.1f)
	{
		DeltaTime = 0.1f;
	}
	FBodyInstance* TargetInstance = UpdatedPrimitive ? UpdatedPrimitive->GetBodyInstance() : nullptr;

	if (CanSimulate() && TargetInstance)
	{
		// #todo: param to say use own gravity or not
		TargetInstance->AddForce(GetGravity(), true, true);

		// #todo: these more traditional assumptions might not be true later when we allow for any wild configuration of systems
		// possibly access via PVehicle. PVehicle.GetEngine().
		check(Wheels.Num() == PVehicle->Suspension.Num());
		check(Wheels.Num() == PVehicle->Wheels.Num());
		ensure(PVehicle->Engine.Num() == PVehicle->Transmission.Num());
		ensure(PVehicle->Engine.Num() == 1);

		///////////////////////////////////////////////////////////////////////
		// Vehicle Space

		// work in vehicle local space
		FTransform VehicleWorldTransform = TargetInstance->GetUnrealWorldTransform();
		FVector VehicleWorldVelocity = TargetInstance->GetUnrealWorldVelocity();

		FVector VehicleUpAxis = VehicleWorldTransform.GetUnitAxis(EAxis::Z);
		FVector VehicleForwardAxis = VehicleWorldTransform.GetUnitAxis(EAxis::X);
		FVector VehicleRightdAxis = VehicleWorldTransform.GetUnitAxis(EAxis::Y);
		float VehicleSpeed = FVector::DotProduct(VehicleWorldVelocity, VehicleForwardAxis); // [cm/s]
		
		// cache some ueful data
		ForwardSpeed = VehicleSpeed; 
		ForwardsAcceleration = (ForwardSpeed - PrevForwardSpeed) / DeltaTime;
		PrevForwardSpeed = ForwardSpeed;

		///////////////////////////////////////////////////////////////////////
		// Aerodynamics
		Chaos::FSimpleAerodynamicsSim& PAerodynamics = PVehicle->Aerodynamics[0];
		FVector LocalDragLiftForce = /*MToCm*/(PAerodynamics.GetCombinedForces(CmToM(VehicleSpeed)));
		FVector WorldLiftDragForce = VehicleWorldTransform.TransformVector(LocalDragLiftForce);
		TargetInstance->AddForce(WorldLiftDragForce); // applied whether the vehicle is on th ground or not


		auto& PEngine = PVehicle->Engine[0];
		auto& PTransmission = PVehicle->Transmission[0];

		///////////////////////////////////////////////////////////////////////
		// Wheel World Location

		TArray<FVector> WheelWorldLocation;
		TArray<FVector> WorldWheelVelocity;
		TArray<FVector> LocalWheelVelocity;
		WheelWorldLocation.SetNum(Wheels.Num());
		WorldWheelVelocity.SetNum(Wheels.Num());
		LocalWheelVelocity.SetNum(Wheels.Num());
		for (int WheelIdx = 0; WheelIdx < Wheels.Num(); WheelIdx++)
		{
			const FVector WheelOffset = GetWheelRestingPosition(WheelSetups[WheelIdx]);
			WheelWorldLocation[WheelIdx] = VehicleWorldTransform.TransformPosition(WheelOffset);

			WorldWheelVelocity[WheelIdx] = TargetInstance->GetUnrealWorldVelocityAtPoint(WheelWorldLocation[WheelIdx]);
			LocalWheelVelocity[WheelIdx] = VehicleWorldTransform.InverseTransformVector(WorldWheelVelocity[WheelIdx]);
		}

		///////////////////////////////////////////////////////////////////////
		// Wheel Raycast/Shapecast

		TArray<AActor*> ActorsToIgnore;
		ActorsToIgnore.Add(GetPawnOwner()); // ignore self in scene query

		FCollisionQueryParams TraceParams(NAME_None, FCollisionQueryParams::GetUnknownStatId(), false, nullptr);
		TraceParams.bReturnPhysicalMaterial = true;	// we need this to get the surface friction coefficient
		TraceParams.AddIgnoredActors(ActorsToIgnore);


		TArray<FHitResult> OutHits;
		OutHits.SetNum(Wheels.Num());
		for (int WheelIdx = 0; WheelIdx < Wheels.Num(); WheelIdx++)
		{
			TraceParams.bTraceComplex = (Wheels[WheelIdx]->SweepType == ESweepType::ComplexSweep || Wheels[WheelIdx]->SweepType == ESweepType::SimpleAndComplexSweep);
			float TraceLength = 80.0;
			float TopOffset = 0.0f;
			FVector TraceStart = WheelWorldLocation[WheelIdx] + (VehicleUpAxis * TopOffset);
			FVector TraceEnd = TraceStart - (VehicleUpAxis * TraceLength); // # todo: should use suspension length data
			
			// #todo: should select method/shape from options passed in
			bool MadeContact = GetWorld()->LineTraceSingleByChannel(OutHits[WheelIdx], TraceStart, TraceEnd, ECollisionChannel::ECC_WorldDynamic, TraceParams, FCollisionResponseParams::DefaultResponseParam);
			//float WheelRadius = PVehicle->Wheels[WheelIdx].GetEffectiveRadius();
			//bool MadeContact = GetWorld()->SweepSingleByChannel(OutHits[WheelIdx]
			//	, TraceStart + VehicleUpAxis*WheelRadius
			//	, TraceEnd + VehicleUpAxis*WheelRadius
			//	, FQuat::Identity, ECollisionChannel::ECC_WorldDynamic
			//	, FCollisionShape::MakeSphere(WheelRadius), TraceParams
			//	, FCollisionResponseParams::DefaultResponseParam);

			if (GVehicleDebugParams.ShowWheelRaycasts)
			{
				// push the visualization out a bit sideways from the wheel model so we can actually see it
				FVector VehicleRightAxis = GetOwner()->GetTransform().GetUnitAxis(EAxis::Y) * 50.0f;
				const FVector WheelOffset = GetWheelRestingPosition(WheelSetups[WheelIdx]); // #todo: cache this
				if (WheelOffset.Y < 0.0f)
				{
					VehicleRightAxis = VehicleRightAxis * -1.0f;
				}

				DrawDebugLine(GetWorld(), TraceStart + VehicleRightAxis, TraceEnd + VehicleRightAxis, MadeContact ? FColor::Green : FColor::Red, false, -1.f, 0, 2.f);
			}

			// #todo: move this
			// tell systems who care that wheel is touching the ground
			PVehicle->Wheels[WheelIdx].SetOnGround(MadeContact);

			//PVehicle->Wheels[WheelIdx].SetMaxOmega(RPMToOmega(PTransmission.GetTransmissionRPM(PEngine.Setup().MaxRPM, PTransmission.GetCurrentGear())));

			// #todo: move this
			FChaosWheelSetup& WheelSetup = WheelSetups[WheelIdx];
			UChaosVehicleWheel* Wheel = WheelSetup.WheelClass.GetDefaultObject();
			Wheel->SetInAir(!MadeContact); // #todo - no
		}


		///////////////////////////////////////////////////////////////////////
		// Input
		if (bRawGearUpInput)
		{
			PTransmission.ChangeUp();
			bRawGearUpInput = false;
		}

		if (bRawGearDownInput)
		{
			PTransmission.ChangeDown();
			bRawGearDownInput = false;
		}

		if (GVehicleDebugParams.ThrottleOverride > 0.f)
		{
			PTransmission.SetGear(1); // TEMP

			PEngine.SetThrottle(GVehicleDebugParams.ThrottleOverride); // TEMP
		}
		else
		{
			PEngine.SetThrottle(RawThrottleInput);
		}
		PEngine.Simulate(DeltaTime);

		///////////////////////////////////////////////////////////////////////
		// Engine/Transmission

		// SET SPEED FROM A DRIVEN WHEEL!!! - average all driven wheel speeds??
		PEngine.SetEngineRPM(PTransmission.GetEngineRPMFromWheelRPM(FMath::Abs(PVehicle->Wheels[2].GetWheelRPM())));
		PTransmission.SetEngineRPM(PEngine.GetEngineRPM()); // needs engine RPM to decide when to change gear (automatic gearbox)
		float GearRatio = PTransmission.GetGearRatio(PTransmission.GetCurrentGear());

		float TransmissionTorque = PTransmission.GetTransmissionTorque(PEngine.GetEngineTorque());
		PTransmission.Simulate(DeltaTime);

		// #todo: engine should take throttle input
		float FinalTorque = TransmissionTorque * RawThrottleInput;

		///////////////////////////////////////////////////////////////////////
		// Suspension

		if (!GVehicleDebugParams.DisableSuspensionForces)
		{
			TArray<float> SusForces;
			SusForces.Init(0.f, Wheels.Num());

			for (int WheelIdx = 0; WheelIdx < Wheels.Num(); WheelIdx++)
			{
				// cheat suspension length in
				FChaosWheelSetup& WheelSetup = WheelSetups[WheelIdx];
				UChaosVehicleWheel* Wheel = WheelSetup.WheelClass.GetDefaultObject();

				float NewDesiredLength = 1.0f; // suspension max length
				float ForceMagnitude2 = 0.f;
				float SuspensionMovePosition = -20.0f;
				auto& PWheel = PVehicle->Wheels[WheelIdx];
				auto& PSuspension = PVehicle->Suspension[WheelIdx];

				if (PWheel.InContact())
				{
					NewDesiredLength = OutHits[WheelIdx].Time;

					// #todo: is this actually correct??
					SuspensionMovePosition = -FVector::DotProduct(WheelWorldLocation[WheelIdx] - OutHits[WheelIdx].ImpactPoint, VehicleUpAxis) + Wheel->WheelRadius;

					//FVector WorldWheelVelocity = TargetInstance->GetUnrealWorldVelocityAtPoint(WheelWorldLocation[WheelIdx]);
					//FVector LocalWheelVelocity = VehicleWorldTransform.InverseTransformVector(WorldWheelVelocity);

					PSuspension.SetDesiredLength(NewDesiredLength);
					PSuspension.SetLocalVelocity(LocalWheelVelocity[WheelIdx]);
					PSuspension.Simulate(DeltaTime);

					float ForceMagnitude = PSuspension.GetSuspensionForce();


					FVector GroundZVector = OutHits[WheelIdx].Normal;
					FVector SuspensionForceVector = /*VehicleUpAxis*/ GroundZVector * ForceMagnitude;

					FVector SusApplicationPoint = WheelWorldLocation[WheelIdx] + PVehicle->Suspension[WheelIdx].Setup().SuspensionForceOffset;

					check(PWheel.InContact());
					TargetInstance->AddForceAtPosition(SuspensionForceVector, SusApplicationPoint);

					if (GVehicleDebugParams.ShowSuspensionForces)
					{
						DrawDebugLine(GetWorld()
							, SusApplicationPoint
							, SusApplicationPoint + SuspensionForceVector * 0.0005f
							, FColor::Blue, false, -1.0f, 0, 5);

						DrawDebugLine(GetWorld()
							, SusApplicationPoint
							, SusApplicationPoint + GroundZVector * 140.f
							, FColor::Yellow, false, -1.0f, 0, 5);
					}
					          
					PWheel.SetWheelLoadForce(ForceMagnitude);
					SusForces[WheelIdx] = ForceMagnitude;

				}

				UChaosVehicleWheel* VehicleWheel = Wheels[WheelIdx];

				// put this in the wheel or suspension component
				CurrentMovementOffset[WheelIdx] += (SuspensionMovePosition - CurrentMovementOffset[WheelIdx]) * 0.5f;
				VehicleWheel->SetSuspensionOffset(CurrentMovementOffset[WheelIdx]); // TEMP
			}

			if (!GVehicleDebugParams.DisableRollbarForces)
			{
				// anti-roll forces
				static float FV = 0.01f; // 0.1f better
				float ForceDiffOnAxleF = SusForces[0] - SusForces[1];
				FVector SuspensionForceVector0 = VehicleUpAxis * ForceDiffOnAxleF * FV;
				FVector SuspensionForceVector1 = VehicleUpAxis * ForceDiffOnAxleF * -FV;

				FVector SusApplicationPoint0 = WheelWorldLocation[0] + PVehicle->Suspension[0].Setup().SuspensionForceOffset;
				TargetInstance->AddForceAtPosition(SuspensionForceVector0, SusApplicationPoint0);

				FVector SusApplicationPoint1 = WheelWorldLocation[1] + PVehicle->Suspension[1].Setup().SuspensionForceOffset;
				TargetInstance->AddForceAtPosition(SuspensionForceVector1, SusApplicationPoint1);


				float ForceDiffOnAxleR = SusForces[2] - SusForces[3];

				FVector SuspensionForceVector2 = VehicleUpAxis * ForceDiffOnAxleR * FV;
				FVector SuspensionForceVector3 = VehicleUpAxis * ForceDiffOnAxleR * -FV;

				FVector SusApplicationPoint2 = WheelWorldLocation[2] + PVehicle->Suspension[2].Setup().SuspensionForceOffset;
				TargetInstance->AddForceAtPosition(SuspensionForceVector2, SusApplicationPoint2);

				FVector SusApplicationPoint3 = WheelWorldLocation[3] + PVehicle->Suspension[3].Setup().SuspensionForceOffset;
				TargetInstance->AddForceAtPosition(SuspensionForceVector3, SusApplicationPoint3);
			}

		}

#ifdef MOVE_DEBUG_DISPLAY
		if (GVehicleDebugParams.ShowWheelCollisionNormal)
		{
			for (auto& Hit : OutHits)
			{
				DrawDebugLine(GetWorld(), Hit.ImpactPoint, Hit.ImpactPoint + Hit.Normal * 20.0f, FColor::Yellow, true, 1.0f, 0, 1.0f);
			}
		}
#endif


		///////////////////////////////////////////////////////////////////////
		// Wheel Friction

		if (!GVehicleDebugParams.DisableFrictionForces)
		{
			for (int WheelIdx = 0; WheelIdx < Wheels.Num(); WheelIdx++)
			{
				auto& PWheel = PVehicle->Wheels[WheelIdx]; // Physics Wheel

				UChaosVehicleWheel* VehicleWheel = Wheels[WheelIdx];
				if (PWheel.Setup().SteeringEnabled)
				{
					// cheap Ackerman steering - outside wheel steers more than inside wheel
					bool OutsideWheel = ((SteeringInput > 0.f) && (WheelIdx == 1)) || ((SteeringInput < 0.f) && (WheelIdx == 0));
					float MaxAngle = OutsideWheel ? PWheel.Setup().MaxSteeringAngle : PWheel.Setup().MaxSteeringAngle * 0.6f;
					{
						float SpeedScaling = 1.0f - (VehicleSpeed * 0.0001f); // #todo: do this properly
						SpeedScaling = FMath::Min(1.f, FMath::Max(SpeedScaling, 0.2f));
						if (FMath::Abs(GVehicleDebugParams.SteeringOverride) > 0.01f)
						{
							VehicleWheel->SetSteerAngle(PWheel.Setup().MaxSteeringAngle * GVehicleDebugParams.SteeringOverride); // TEMP
						}
						else
						{
							VehicleWheel->SetSteerAngle(SteeringInput * MaxAngle * SpeedScaling); // TEMP
						}
					}

				}
				else
				{
					VehicleWheel->SetSteerAngle(0.0f); // TEMP
				}

				float EngineBraking = 0.f;
				if (PWheel.Setup().EngineEnabled)
				{
					PWheel.SetDriveTorque(FinalTorque);	
					if (RawThrottleInput < SMALL_NUMBER)
					{
						EngineBraking = 0.0f;// PEngine.GetEngineRPM()* PEngine.Setup().EngineBrakeEffect;
					}		
				}

				float BrakeForce = PWheel.Setup().MaxBrakeTorque * RawBrakeInput;


				PWheel.SetBrakeTorque(BrakeForce + EngineBraking);	

				//OutHits[WheelIdx].PhysMaterial->FrictionCombineMode;a
				if (OutHits[WheelIdx].PhysMaterial.IsValid())
				{
					PWheel.SetSurfaceFriction(OutHits[WheelIdx].PhysMaterial->Friction);
				}


				// #todo: combine inputs? brake + Handbrake ?
				if (bRawHandbrakeInput && PWheel.Setup().HandbrakeEnabled)
				{
					PWheel.SetBrakeTorque(bRawHandbrakeInput * PWheel.Setup().HandbrakeTorque);
				}

				if (PWheel.InContact())
				{
					//FVector WorldWheelVelocity = TargetInstance->GetUnrealWorldVelocityAtPoint(WheelWorldLocation[WheelIdx])/* * 0.01f*/;
					//FVector LocalWheelVelocity = VehicleWorldTransform.InverseTransformVector(WorldWheelVelocity);

					// take into account steering angle
					float SteerAngleDegrees = VehicleWheel->GetSteerAngle(); // temp
					FRotator SteeringRotator(0.f, SteerAngleDegrees, 0.f);
					FVector SteerLocalWheelVelocity = SteeringRotator.UnrotateVector(LocalWheelVelocity[WheelIdx]);

					PWheel.SetVehicleGroundSpeed(SteerLocalWheelVelocity);
					PWheel.Simulate(DeltaTime);

					float RotationAngle = FMath::RadiansToDegrees(PWheel.GetAngularPosition());
					FVector FrictionForceLocal = PWheel.GetForceFromFriction();
					FrictionForceLocal = SteeringRotator.RotateVector(FrictionForceLocal);

					FVector GroundZVector = OutHits[WheelIdx].Normal;
					FVector GroundXVector = FVector::CrossProduct(VehicleRightdAxis, GroundZVector);
					FVector GroundYVector = FVector::CrossProduct(GroundZVector, GroundXVector);

					// the force should be applied along the ground surface not along vehicle forward vector?
					// NEW FVector FrictionForceVector = VehicleWorldTransform.TransformVector(FrictionForceLocal);
					FMatrix Mat(GroundXVector, GroundYVector, GroundZVector, VehicleWorldTransform.GetLocation());
					FVector FrictionForceVector = Mat.TransformVector(FrictionForceLocal);
	
					if (GVehicleDebugParams.ShowWheelForces)
					{
						// show longitudinal drive force
						{
							DrawDebugLine(GetWorld()
								, WheelWorldLocation[WheelIdx]
								, WheelWorldLocation[WheelIdx] + FrictionForceVector * 0.001f
								, FColor::Yellow, false, -1.0f, 0, 2);
						}

					}

					// #todo: combine all wheel forces into one?
					check(PWheel.InContact());
					TargetInstance->AddForceAtPosition(FrictionForceVector * PWheel.Setup().CheatFrictionForce, WheelWorldLocation[WheelIdx]);
				}
				else
				{
					PWheel.SetVehicleGroundSpeed(LocalWheelVelocity[WheelIdx]);
					PWheel.Simulate(DeltaTime);
				}

			}
		}
	}

}

void UChaosWheeledVehicleMovementComponent::SetupVehicleShapes()
{
	if (!UpdatedPrimitive)
	{
		return;
	}

	//static PxMaterial* WheelMaterial = GPhysXSDK->createMaterial(0.0f, 0.0f, 0.0f);
	//FBodyInstance* TargetInstance = UpdatedPrimitive->GetBodyInstance();

	//FPhysicsCommand::ExecuteWrite(TargetInstance->ActorHandle, [&](const FPhysicsActorHandle& Actor)
	//	{
	//		PxRigidActor* PActor = FPhysicsInterface::GetPxRigidActor_AssumesLocked(Actor);
	//		if (!PActor)
	//		{
	//			return;
	//		}

	//		if (PxRigidDynamic* PVehicleActor = PActor->is<PxRigidDynamic>())
	//		{
	//			// Add wheel shapes to actor
	for (int32 WheelIdx = 0; WheelIdx < WheelSetups.Num(); ++WheelIdx)
	{
		FChaosWheelSetup& WheelSetup = WheelSetups[WheelIdx];
		UChaosVehicleWheel* Wheel = WheelSetup.WheelClass.GetDefaultObject();
		check(Wheel);

		const FVector WheelOffset = GetWheelRestingPosition(WheelSetup);

		//				const PxTransform PLocalPose = PxTransform(U2PVector(WheelOffset));
		//				PxShape* PWheelShape = NULL;

		//				// Prepare shape
		//				const UBodySetup* WheelBodySetup = nullptr;
		//				FVector MeshScaleV(1.f, 1.f, 1.f);
		//				if (Wheel->bDontCreateShape)
		//				{
		//					//don't create shape so grab it directly from the bodies associated with the vehicle
		//					if (USkinnedMeshComponent* SkinnedMesh = GetMesh())
		//					{
		//						if (const FBodyInstance* WheelBI = SkinnedMesh->GetBodyInstance(WheelSetup.BoneName))
		//						{
		//							WheelBodySetup = WheelBI->BodySetup.Get();
		//						}
		//					}

		//				}
		//				else if (Wheel->CollisionMesh && Wheel->CollisionMesh->BodySetup)
		//				{
		//					WheelBodySetup = Wheel->CollisionMesh->BodySetup;

		//					FBoxSphereBounds MeshBounds = Wheel->CollisionMesh->GetBounds();
		//					if (Wheel->bAutoAdjustCollisionSize)
		//					{
		//						MeshScaleV.X = Wheel->WheelRadius / MeshBounds.BoxExtent.X;
		//						MeshScaleV.Y = Wheel->WheelWidth / MeshBounds.BoxExtent.Y;
		//						MeshScaleV.Z = Wheel->WheelRadius / MeshBounds.BoxExtent.Z;
		//					}
		//				}

		//				if (WheelBodySetup)
		//				{
		//					PxMeshScale MeshScale(U2PVector(UpdatedComponent->GetRelativeScale3D() * MeshScaleV), PxQuat(physx::PxIdentity));

		//					if (WheelBodySetup->AggGeom.ConvexElems.Num() == 1)
		//					{
		//						PxConvexMesh* ConvexMesh = WheelBodySetup->AggGeom.ConvexElems[0].GetConvexMesh();
		//						PWheelShape = GPhysXSDK->createShape(PxConvexMeshGeometry(ConvexMesh, MeshScale), *WheelMaterial, /*bIsExclusive=*/true);
		//						PVehicleActor->attachShape(*PWheelShape);
		//						PWheelShape->release();
		//					}
		//					else if (WheelBodySetup->TriMeshes.Num())
		//					{
		//						PxTriangleMesh* TriMesh = WheelBodySetup->TriMeshes[0];

		//						// No eSIMULATION_SHAPE flag for wheels
		//						PWheelShape = GPhysXSDK->createShape(PxTriangleMeshGeometry(TriMesh, MeshScale), *WheelMaterial, /*bIsExclusive=*/true, PxShapeFlag::eSCENE_QUERY_SHAPE | PxShapeFlag::eVISUALIZATION);
		//						PWheelShape->setLocalPose(PLocalPose);
		//						PVehicleActor->attachShape(*PWheelShape);
		//						PWheelShape->release();
		//					}
		//				}

		//				if (!PWheelShape)
		//				{
		//					//fallback onto simple spheres
		//					PWheelShape = GPhysXSDK->createShape(PxSphereGeometry(Wheel->WheelRadius), *WheelMaterial, /*bIsExclusive=*/true);
		//					PWheelShape->setLocalPose(PLocalPose);
		//					PVehicleActor->attachShape(*PWheelShape);
		//					PWheelShape->release();
		//				}

		//				// Init filter data
		//				FCollisionResponseContainer CollisionResponse;
		//				CollisionResponse.SetAllChannels(ECR_Ignore);

		//				FCollisionFilterData WheelQueryFilterData, DummySimData;
		//				CreateShapeFilterData(ECC_Vehicle, FMaskFilter(0), UpdatedComponent->GetOwner()->GetUniqueID(), CollisionResponse, UpdatedComponent->GetUniqueID(), 0, WheelQueryFilterData, DummySimData, false, false, false);

		//				if (Wheel->SweepType != EWheelSweepType::Complex)
		//				{
		//					WheelQueryFilterData.Word3 |= EPDF_SimpleCollision;
		//				}

		//				if (Wheel->SweepType != EWheelSweepType::Simple)
		//				{
		//					WheelQueryFilterData.Word3 |= EPDF_ComplexCollision;
		//				}

		//				//// Give suspension raycasts the same group ID as the chassis so that they don't hit each other
		//				PWheelShape->setQueryFilterData(U2PFilterData(WheelQueryFilterData));
	}
	//		}
	//	});
}


void UChaosWheeledVehicleMovementComponent::OnCreatePhysicsState()
{
	Super::OnCreatePhysicsState();

	VehicleSetupTag = FChaosVehicleManager::VehicleSetupTag;

	// only create physx vehicle in game
	UWorld* World = GetWorld();
	if (World->IsGameWorld())
	{
		FPhysScene* PhysScene = World->GetPhysicsScene();

		if (PhysScene && FChaosVehicleManager::GetVehicleManagerFromScene(PhysScene))
		{
			//FixupSkeletalMesh();
			CreateVehicle();

			if (PVehicle)
			{
				FChaosVehicleManager* VehicleManager = FChaosVehicleManager::GetVehicleManagerFromScene(PhysScene);
				VehicleManager->AddVehicle(this);

				CreateWheels();

			//	//LogVehicleSettings( PVehicle );
			//	SCOPED_SCENE_WRITE_LOCK(VehicleManager->GetScene());
			//	PVehicle->getRigidDynamicActor()->wakeUp();

			//	// Need to bind to the notify delegate on the mesh incase physics state is changed
			//	if (USkeletalMeshComponent* MeshComp = Cast<USkeletalMeshComponent>(GetMesh()))
			//	{
			//		MeshOnPhysicsStateChangeHandle = MeshComp->RegisterOnPhysicsCreatedDelegate(FOnSkelMeshPhysicsCreated::CreateUObject(this, &UVehicleMovementComponent::RecreatePhysicsState));
			//		if (UVehicleAnimInstance* VehicleAnimInstance = Cast<UVehicleAnimInstance>(MeshComp->GetAnimInstance()))
			//		{
			//			VehicleAnimInstance->SetWheeledVehicleComponent(this);
			//		}
			//	}
			}
		}
	}
}

void UChaosWheeledVehicleMovementComponent::OnDestroyPhysicsState()
{
	Super::OnDestroyPhysicsState();

	if (PVehicle.IsValid())
	{
		// TODO: need OnDestroyPhysicsState extended in UChaosWheeledVehicleMovementComponent
		//DestroyWheels();

		FChaosVehicleManager* VehicleManager = FChaosVehicleManager::GetVehicleManagerFromScene(GetWorld()->GetPhysicsScene());
		VehicleManager->RemoveVehicle(this);
		PVehicle.Reset(nullptr);

		//if (MeshOnPhysicsStateChangeHandle.IsValid())
		//{
		//	if (USkeletalMeshComponent* MeshComp = Cast<USkeletalMeshComponent>(GetMesh()))
		//	{
		//		MeshComp->UnregisterOnPhysicsCreatedDelegate(MeshOnPhysicsStateChangeHandle);
		//	}
		//}

		//if (UpdatedComponent)
		//{
		//	UpdatedComponent->RecreatePhysicsState();
		//}
	}
}

void UChaosWheeledVehicleMovementComponent::TickVehicle(float DeltaTime)
{
	Super::TickVehicle(DeltaTime);

	// #todo: move some of this to base class

	//if (AvoidanceLockTimer > 0.0f)
	//{
	//	AvoidanceLockTimer -= DeltaTime;
	//}

	// movement updates and replication
	if (PVehicle && UpdatedComponent)
	{
		APawn* MyOwner = Cast<APawn>(UpdatedComponent->GetOwner());
		if (MyOwner)
		{
			UpdateSimulation(DeltaTime);
		}
	}

	// update wheels
	for (int32 i = 0; i < Wheels.Num(); i++)
	{
		UChaosVehicleWheel* VehicleWheel = Wheels[i];

		//VehicleWheel->GetRotationAngle();
		Wheels[i]->Tick(DeltaTime);
	}

	DrawDebug3D();
}


void UChaosWheeledVehicleMovementComponent::SetupVehicle()
{
	check(PVehicle);

	Super::SetupVehicle();

	// we are allowed any number of wheels not limited to only 4
	for (int32 WheelIdx = 0; WheelIdx < WheelSetups.Num(); ++WheelIdx)
	{
		UChaosVehicleWheel* Wheel = WheelSetups[WheelIdx].WheelClass.GetDefaultObject();

		// create Dynamic states passing in pointer to their Static setup data
		Chaos::FSimpleWheelSim WheelSim(&Wheel->GetPhysicsWheelConfig());
		WheelSim.SetWheelRadius(Wheel->WheelRadius); // initial radius
		PVehicle->Wheels.Add(WheelSim);

		Chaos::FSimpleSuspensionSim SuspensionSim(&Wheel->GetPhysicsSuspensionConfig());
		PVehicle->Suspension.Add(SuspensionSim);
	}

	Chaos::FSimpleEngineSim EngineSim(&EngineSetup.GetPhysicsEngineConfig());
	PVehicle->Engine.Add(EngineSim);

	Chaos::FSimpleTransmissionSim TransmissionSim(&TransmissionSetup.GetPhysicsTransmissionConfig());
	PVehicle->Transmission.Add(TransmissionSim);

	Chaos::FSimpleAerodynamicsSim AerodynamicsSim(&GetAerodynamicsConfig());
	PVehicle->Aerodynamics.Add(AerodynamicsSim);


	// Setup the chassis and wheel shapes
	SetupVehicleShapes();

	// Setup mass properties
	SetupVehicleMass();
}

bool UChaosWheeledVehicleMovementComponent::CanCreateVehicle() const
{
	if (!Super::CanCreateVehicle())
		return false;

	check(GetOwner());
	FString ActorName = GetOwner()->GetName();

	for (int32 WheelIdx = 0; WheelIdx < WheelSetups.Num(); ++WheelIdx)
	{
		const FChaosWheelSetup& WheelSetup = WheelSetups[WheelIdx];

		if (WheelSetup.WheelClass == NULL)
		{
			UE_LOG(LogVehicles, Warning, TEXT("Can't create vehicle %s (%s). Wheel %d is not set."), *ActorName, *GetPathName(), WheelIdx);
			return false;
		}

		if (WheelSetup.BoneName == NAME_None)
		{
			UE_LOG(LogVehicles, Warning, TEXT("Can't create vehicle %s (%s). Bone name for wheel %d is not set."), *ActorName, *GetPathName(), WheelIdx);
			return false;
		}

	}

	return true;
}


float UChaosWheeledVehicleMovementComponent::GetEngineRotationSpeed() const
{
	float EngineRPM = 0.f;

	if (PVehicle.IsValid())
	{
		EngineRPM = PVehicle->Engine[0].GetEngineRPM();
	}

	return EngineRPM;
}

float UChaosWheeledVehicleMovementComponent::GetEngineMaxRotationSpeed() const
{
	float MaxEngineRPM = 0.f;
	
	if (PVehicle.IsValid())
	{
		MaxEngineRPM = PVehicle->Engine[0].Setup().MaxRPM;
	}

	return MaxEngineRPM;
}

int32 UChaosWheeledVehicleMovementComponent::GetCurrentGear() const
{
	int32 CurrentGear = 0;

	if (PVehicle.IsValid())
	{
		CurrentGear = PVehicle->Transmission[0].GetCurrentGear();
	}

	return CurrentGear;
}

int32 UChaosWheeledVehicleMovementComponent::GetTargetGear() const
{
	int32 TargetGear = 0;

	if (PVehicle.IsValid())
	{
		TargetGear = PVehicle->Transmission[0].GetTargetGear();
	}

	return TargetGear;
}

bool UChaosWheeledVehicleMovementComponent::GetUseAutoGears() const
{
	bool UseAutoGears = 0;

	if (PVehicle.IsValid())
	{
		UseAutoGears = PVehicle->Transmission[0].Setup().TransmissionType == Chaos::ETransmissionType::Automatic;
	}

	return UseAutoGears;
}

float UChaosWheeledVehicleMovementComponent::GetMaxSpringForce() const
{
	// #todo:
	check(false);
	return 0.0f;
}


////////////////////////////////////////////////////////////////////////////////
// Debug
void UChaosWheeledVehicleMovementComponent::DrawDebug(UCanvas* Canvas, float& YL, float& YPos)
{
	FChaosVehicleManager* MyVehicleManager = FChaosVehicleManager::GetVehicleManagerFromScene(GetWorld()->GetPhysicsScene());
	FBodyInstance* TargetInstance = UpdatedPrimitive ? UpdatedPrimitive->GetBodyInstance() : nullptr;

	if (!PVehicle.IsValid() || TargetInstance == nullptr || MyVehicleManager == nullptr)
	{
		return;
	}

	float ForwardSpeedKmH = CmSToKmH(GetForwardSpeed());
	float ForwardSpeedMPH = CmSToMPH(GetForwardSpeed());
	float ForwardSpeedMSec = CmToM(GetForwardSpeed());
	auto& PTransmission = PVehicle->Transmission[0];
	auto& PEngine = PVehicle->Engine[0];
	auto& PTransmissionSetup = PTransmission.Setup();

	// always draw this even on (DebugPage == EDebugPages::BasicPage)
	{
		UFont* RenderFont = GEngine->GetLargeFont();
		Canvas->SetDrawColor(FColor::Yellow);

		// draw MPH, RPM and current gear
		float X, Y;
		Canvas->GetCenter(X, Y);
		float YLine = Y * 2.f - 50.f;
		float Scaling = 2.f;
		Canvas->DrawText(RenderFont, FString::Printf(TEXT("%d mph"), (int)ForwardSpeedMPH), X-100, YLine, Scaling, Scaling);
		Canvas->DrawText(RenderFont, FString::Printf(TEXT("[%d]"), (int)PTransmission.GetCurrentGear()), X, YLine, Scaling, Scaling);
		Canvas->DrawText(RenderFont, FString::Printf(TEXT("%d rpm"), (int)PEngine.GetEngineRPM()), X+50, YLine, Scaling, Scaling);
	}

	UFont* RenderFont = GEngine->GetMediumFont();
	// draw drive data
	{
		Canvas->SetDrawColor(FColor::White);
		YPos += 16;

		if (TargetInstance)
		{
			YPos += Canvas->DrawText(RenderFont, FString::Printf(TEXT("Mass (Kg): %.1f"), TargetInstance->GetBodyMass()), 4, YPos);
			YPos += Canvas->DrawText(RenderFont, FString::Printf(TEXT("Inertia : %s"), *TargetInstance->GetBodyInertiaTensor().ToString()), 4, YPos);
		}

		YPos += Canvas->DrawText(RenderFont, FString::Printf(TEXT("Speed (km/h): %.1f  (MPH): %.1f  (m/s): %.1f"), ForwardSpeedKmH, ForwardSpeedMPH, ForwardSpeedMSec), 4, YPos);
		YPos += Canvas->DrawText(RenderFont, FString::Printf(TEXT("Acceleration (m/s-2): %.1f"), CmToM(GetForwardAcceleration())), 4, YPos);
		YPos += Canvas->DrawText(RenderFont, FString::Printf(TEXT("Steering: %.1f (RAW %.1f)"), SteeringInput, RawSteeringInput), 4, YPos);
		YPos += Canvas->DrawText(RenderFont, FString::Printf(TEXT("Throttle: %.1f (RAW %.1f)"), ThrottleInput, RawThrottleInput), 4, YPos);
		YPos += Canvas->DrawText(RenderFont, FString::Printf(TEXT("Brake: %.1f (RAW %.1f)"), BrakeInput, RawBrakeInput), 4, YPos);
		YPos += Canvas->DrawText(RenderFont, FString::Printf(TEXT("RPM: %.1f (ChangeUp RPM %d, ChangeDown RPM %d)"), GetEngineRotationSpeed(), PTransmissionSetup.ChangeUpRPM, PTransmissionSetup.ChangeDownRPM), 4, YPos);
		YPos += Canvas->DrawText(RenderFont, FString::Printf(TEXT("Gear: %d (Target %d)"), GetCurrentGear(), GetTargetGear()), 4, YPos);
		//YPos += Canvas->DrawText(RenderFont, FString::Printf(TEXT("Drag: %.1f"), DebugDragMagnitude), 4, YPos);

		YPos += 16;
		for (int i = 0; i < PVehicle->Wheels.Num(); i++)
		{
			YPos += Canvas->DrawText(RenderFont, FString::Printf(TEXT("WheelLoad: [%d] %1.f N"), i, CmToM(PVehicle->Wheels[i].GetWheelLoadForce())), 4, YPos);
		}

		YPos += 16;
		for (int i = 0; i < PVehicle->Wheels.Num(); i++)
		{
			YPos += Canvas->DrawText(RenderFont, FString::Printf(TEXT("SurfaceFriction: [%d] %.2f"), i, PVehicle->Wheels[i].GetSurfaceFriction()), 4, YPos);
		}
		
	}

	// draw wheel layout
	if (DebugPage == EDebugPages::FrictionPage)
	{
		FVector2D MaxSize = GetWheelLayoutDimensions();

		// Draw a top down representation of the wheels in position, with the directionl forces being shown
		for (int32 WheelIdx = 0; WheelIdx < WheelSetups.Num(); ++WheelIdx)
		{

			auto& PWheel = PVehicle->Wheels[WheelIdx];
			FVector Forces = PWheel.GetForceFromFriction();

			FChaosWheelSetup& WheelSetup = WheelSetups[WheelIdx];
			UChaosVehicleWheel* Wheel = WheelSetup.WheelClass.GetDefaultObject();
			check(Wheel);
			UPhysicalMaterial* ContactMat = Wheel->GetContactSurfaceMaterial();

			const FVector WheelOffset = GetWheelRestingPosition(WheelSetup);

			float DrawScale = 100;
			FVector2D CentreDrawPosition(350, 400);
			FVector2D WheelDrawPosition(WheelOffset.Y, -WheelOffset.X);
			WheelDrawPosition *= DrawScale;
			WheelDrawPosition /= MaxSize;
			WheelDrawPosition += CentreDrawPosition;

			FVector2D WheelDimensions(Wheel->WheelWidth, Wheel->WheelRadius * 2.0f);
			FVector2D HalfDimensions = WheelDimensions * 0.5f;
			FCanvasBoxItem BoxItem(WheelDrawPosition - HalfDimensions, WheelDimensions);
			BoxItem.SetColor(FColor::Green);
			Canvas->DrawItem(BoxItem);

			float VisualScaling = 0.0001f;
			FVector2D Force2D(Forces.Y * VisualScaling, -Forces.X * VisualScaling);
			DrawLine2D(Canvas, WheelDrawPosition, WheelDrawPosition + Force2D, FColor::Red);

			float SlipAngle = FMath::Abs(PWheel.GetSlipAngle());
			float X = FMath::Sin(SlipAngle) * 50.f;
			float Y = FMath::Cos(SlipAngle) * 50.f;

			int Xpos = WheelDrawPosition.X + 20;
			int Ypos = WheelDrawPosition.Y - 75.f;
			DrawLine2D(Canvas, WheelDrawPosition, WheelDrawPosition - FVector2D(X, Y), FColor::White);
			Ypos += Canvas->DrawText(RenderFont, FString::Printf(TEXT("Slip Angle : %d %"), (int)RadToDeg(SlipAngle)), Xpos, Ypos);

			Ypos += Canvas->DrawText(RenderFont, FString::Printf(TEXT("AccelT : %.1f"), PWheel.GetDriveTorque()), Xpos, Ypos);
			Ypos += Canvas->DrawText(RenderFont, FString::Printf(TEXT("BrakeT : %.1f"), PWheel.GetBrakeTorque()), Xpos, Ypos);

			Ypos += Canvas->DrawText(RenderFont, FString::Printf(TEXT("Omega : %.2f"), PWheel.GetAngularVelocity()), Xpos, Ypos);

			Ypos += Canvas->DrawText(RenderFont, FString::Printf(TEXT("GroundV : %.1f"), PWheel.GetRoadSpeed()), Xpos, Ypos);
			Ypos += Canvas->DrawText(RenderFont, FString::Printf(TEXT("WheelV : %.1f"), PWheel.GetWheelGroundSpeed()), Xpos, Ypos);
			Ypos += Canvas->DrawText(RenderFont, FString::Printf(TEXT("Sx : %.2f"), PWheel.GetNormalizedLongitudinalSlip()), Xpos, Ypos);

			if (PWheel.Setup().EngineEnabled)
			{
				Ypos += Canvas->DrawText(RenderFont, FString::Printf(TEXT("RPM        : %.1f"), PWheel.GetWheelRPM()), Xpos, Ypos);
				Ypos += Canvas->DrawText(RenderFont, FString::Printf(TEXT("Geared RPM : %.1f"), PTransmission.GetEngineRPMFromWheelRPM(PWheel.GetWheelRPM())), Xpos, Ypos);

			}

			if (ContactMat)
			{
				Canvas->DrawText(RenderFont
					, FString::Printf(TEXT("Friction %d"), ContactMat->Friction)
					, WheelDrawPosition.X, WheelDrawPosition.Y-95.f);
			}

			// ground velocity
			// wheel ground velocity
			// Sx
			// Accel Torque
			// Brake Torque

		}

	}

	// draw longitudinal friction slip curve for each wheel
	if (DebugPage == EDebugPages::FrictionPage)
	{
		for (int WheelIdx=0; WheelIdx < PVehicle->Wheels.Num(); WheelIdx++)
		{
			int GraphWidth = 200; int GraphHeight = 120; int Spacing = 50;
			int GraphXPos = 500 + (GraphWidth+Spacing) * (int)(WheelIdx % 2); 
			int GraphYPos = 50 + (GraphHeight + Spacing) * (int)(WheelIdx / 2);
			float XSample = PVehicle->Wheels[WheelIdx].GetNormalizedLongitudinalSlip();
			
			FVector2D CurrentValue(XSample, Chaos::FSimpleWheelSim::GetNormalisedFrictionFromSlipAngle(XSample));
			Canvas->DrawDebugGraph(FString::Printf(TEXT("Longitudinal Slip Graph [%d]"), WheelIdx)
					, CurrentValue.X, CurrentValue.Y
					, GraphXPos, GraphYPos
					, GraphWidth, GraphHeight
					, FVector2D(0, 1), FVector2D(1, 0));

			float Step = 0.02f;
			FVector2D LastPoint;
			for (float X = 0; X < 1.0f; X += Step)
			{
				float Y = Chaos::FSimpleWheelSim::GetNormalisedFrictionFromSlipAngle(X);
				FVector2D NextPoint(GraphXPos + GraphWidth * X, GraphYPos + GraphHeight - GraphHeight * Y);
				if (X > SMALL_NUMBER)
				{
					DrawLine2D(Canvas, LastPoint, NextPoint, FColor::Cyan);
				}
				LastPoint = NextPoint;
			}	
		}

	}

	// draw lateral friction slip curve for each wheel
	if (DebugPage == EDebugPages::FrictionPage)
	{
		for (int WheelIdx = 0; WheelIdx < PVehicle->Wheels.Num(); WheelIdx++)
		{
			int GraphWidth = 200; int GraphHeight = 120; int Spacing = 50;
			int GraphXPos = 500 + (GraphWidth + Spacing) * (int)(WheelIdx % 2);
			int GraphYPos = 350 + (GraphHeight + Spacing) * (int)(WheelIdx / 2);
			float XSample = PVehicle->Wheels[WheelIdx].GetNormalizedLateralSlip();

			FVector2D CurrentValue(XSample, Chaos::FSimpleWheelSim::GetNormalisedFrictionFromSlipAngle(XSample));
			Canvas->DrawDebugGraph(FString::Printf(TEXT("Lateral Slip Graph [%d]"), WheelIdx)
				, CurrentValue.X, CurrentValue.Y
				, GraphXPos, GraphYPos
				, GraphWidth, GraphHeight
				, FVector2D(0, 1), FVector2D(1, 0));

			float Step = 0.02f;
			FVector2D LastPoint;
			for (float X = 0; X < 1.0f; X += Step)
			{
				float Y = Chaos::FSimpleWheelSim::GetNormalisedFrictionFromSlipAngle(X);
				FVector2D NextPoint(GraphXPos + GraphWidth * X, GraphYPos + GraphHeight - GraphHeight * Y);
				if (X > SMALL_NUMBER)
				{
					DrawLine2D(Canvas, LastPoint, NextPoint, FColor::Cyan);
				}
				LastPoint = NextPoint;
			}
		}

	}

	// draw engine torque curve - just putting engine under transmission
	if (DebugPage == EDebugPages::TransmissionPage)
	{
		float MaxTorque = PEngine.Setup().MaxTorque; //GetPeakTorque();
		int CurrentRPM = (int)PEngine.GetEngineRPM();
		FVector2D CurrentValue(CurrentRPM, PEngine.GetEngineTorque());
		int GraphWidth = 200; int GraphHeight = 120;
		int GraphXPos = 200; int GraphYPos = 100;

		Canvas->DrawDebugGraph(FString("Engine Torque Graph")
			, CurrentValue.X, CurrentValue.Y
			, GraphXPos, GraphYPos, 
			GraphWidth, GraphHeight, 
			FVector2D(0, PEngine.Setup().MaxRPM), FVector2D(MaxTorque, 0));

		FVector2D LastPoint;
		for (float RPM = 0; RPM <= PEngine.Setup().MaxRPM; RPM += 10.f)
		{
			float X = RPM / PEngine.Setup().MaxRPM;
			float Y = PEngine.GetTorqueFromRPM(RPM, false) / MaxTorque;
			FVector2D NextPoint(GraphXPos + GraphWidth * X, GraphYPos + GraphHeight - GraphHeight * Y);
			if (RPM > SMALL_NUMBER)
			{
				DrawLine2D(Canvas, LastPoint, NextPoint, FColor::Cyan);
			}
			LastPoint = NextPoint;
		}
	}

	// draw transmission torque curve
	if (DebugPage == EDebugPages::TransmissionPage)
	{
		auto& ESetup = PEngine.Setup();
		auto& TSetup = PTransmission.Setup();
		float MaxTorque = ESetup.MaxTorque;// GetPeakTorque();
		float MaxGearRatio = TSetup.ForwardRatios[0] * TSetup.FinalDriveRatio; // 1st gear always has the highest multiplier
		float LongGearRatio = TSetup.ForwardRatios[TSetup.ForwardRatios.Num()-1] * TSetup.FinalDriveRatio;
		int GraphWidth = 400; int GraphHeight = 240;
		int GraphXPos = 500; int GraphYPos = 150;

		{
			float X = PTransmission.GetTransmissionRPM();
			float Y = PTransmission.GetTransmissionTorque(PEngine.GetTorqueFromRPM(false));

			FVector2D CurrentValue(X, Y);
			Canvas->DrawDebugGraph(FString("Transmission Torque Graph")
				, CurrentValue.X, CurrentValue.Y
				, GraphXPos, GraphYPos
				, GraphWidth, GraphHeight
				, FVector2D(0, ESetup.MaxRPM / LongGearRatio), FVector2D(MaxTorque* MaxGearRatio, 0));
		}

		FVector2D LastPoint;

		for (int Gear = 1; Gear <= TSetup.ForwardRatios.Num(); Gear++)
		{
			for (int EngineRPM = 0; EngineRPM <= ESetup.MaxRPM; EngineRPM += 10)
			{
				float RPMOut = PTransmission.GetTransmissionRPM(EngineRPM, Gear);

				float X = RPMOut / (ESetup.MaxRPM / LongGearRatio);
				float Y = PEngine.GetTorqueFromRPM(EngineRPM, false) * PTransmission.GetGearRatio(Gear) / (MaxTorque*MaxGearRatio);
				FVector2D NextPoint(GraphXPos + GraphWidth * X, GraphYPos + GraphHeight - GraphHeight * Y);
				if (EngineRPM > 0)
				{
					DrawLine2D(Canvas, LastPoint, NextPoint, FColor::Cyan);
				}
				LastPoint = NextPoint;
			}
		}
	}


	// for each of the wheel positions, draw the expected suspension movement limits and the current length
	if (DebugPage == EDebugPages::SuspensionPage)
	{
		FVector2D MaxSize = GetWheelLayoutDimensions();

		for (int32 WheelIdx = 0; WheelIdx < WheelSetups.Num(); ++WheelIdx)
		{
			auto& PSuspension = PVehicle->Suspension[WheelIdx];
			FChaosWheelSetup& WheelSetup = WheelSetups[WheelIdx];
			UChaosVehicleWheel* Wheel = WheelSetup.WheelClass.GetDefaultObject();
			check(Wheel);
			UChaosVehicleWheel* VehicleWheel = Wheels[WheelIdx];

			const FVector WheelOffset = GetWheelRestingPosition(WheelSetup);

			float DrawScale = 100;
			FVector2D CentreDrawPosition(500, 300);
			FVector2D WheelDrawPosition(WheelOffset.Y, -WheelOffset.X);
			WheelDrawPosition *= DrawScale;
			WheelDrawPosition /= MaxSize;
			WheelDrawPosition += CentreDrawPosition;

			{
				// suspension resting position
				FVector2D Start = WheelDrawPosition + FVector2D(-10.f, 0.f);
				FVector2D End = Start + FVector2D(20.f, 0.f);
				DrawLine2D(Canvas, Start, End, FColor::Yellow, 2.f);
			}

			float Raise = PSuspension.Setup().SuspensionMaxRaise;
			float Drop = PSuspension.Setup().SuspensionMaxDrop;

			{
				// suspension compression limit
				FVector2D Start = WheelDrawPosition + FVector2D(-20.f, -Raise * 5.f);
				FVector2D End = Start + FVector2D(40.f, 0.f);
				DrawLine2D(Canvas, Start, End, FColor::White, 2.f);
				Canvas->DrawText(RenderFont, FString::Printf(TEXT("Raise Limit %.1f"), Raise), Start.X, Start.Y-16);
			}

			{
				// suspension extension limit
				FVector2D Start = WheelDrawPosition + FVector2D(-20.f, Drop * 5.f);
				FVector2D End = Start + FVector2D(40.f, 0.f);
				DrawLine2D(Canvas, Start, End, FColor::White, 2.f);
				Canvas->DrawText(RenderFont, FString::Printf(TEXT("Drop Limit %.1f"), Drop), Start.X, Start.Y);
			}

			{
				// current suspension length
				FVector2D Start = WheelDrawPosition + FVector2D(0.f, -Raise * 5.f);
				FVector2D End = Start + FVector2D(0.f, VehicleWheel->GetSuspensionOffset() * 5.f);
				DrawLine2D(Canvas, Start, End, FColor::Green, 4.f);
			}

		}
	}

}

void UChaosWheeledVehicleMovementComponent::DrawDebug3D()
{
	FBodyInstance* TargetInstance = UpdatedPrimitive ? UpdatedPrimitive->GetBodyInstance() : nullptr;

	if (GVehicleDebugParams.ShowCOM)
	{
		FVector COMWorld = TargetInstance->GetCOMPosition();
		DrawDebugCoordinateSystem(GetWorld(), COMWorld, FRotator(TargetInstance->GetUnrealWorldTransform().GetRotation()), 200.f, false, -1.f, 0, 2.f);
	}

	if (GVehicleDebugParams.ShowModelOrigin)
	{
		DrawDebugCoordinateSystem(GetWorld(), TargetInstance->GetUnrealWorldTransform().GetLocation(), FRotator(TargetInstance->GetUnrealWorldTransform().GetRotation()), 200.f, false, -1.f, 0, 2.f);
	}
}

FVector2D UChaosWheeledVehicleMovementComponent::GetWheelLayoutDimensions()
{
	FVector2D MaxSize(0.f, 0.f);

	for (int32 WheelIdx = 0; WheelIdx < WheelSetups.Num(); ++WheelIdx)
	{
		FChaosWheelSetup& WheelSetup = WheelSetups[WheelIdx];
		UChaosVehicleWheel* Wheel = WheelSetup.WheelClass.GetDefaultObject();
		check(Wheel);

		const FVector WheelOffset = GetWheelRestingPosition(WheelSetup);
		if (FMath::Abs(WheelOffset.Y) > MaxSize.Y)
		{
			MaxSize.Y = FMath::Abs(WheelOffset.Y);
		}

		if (FMath::Abs(WheelOffset.X) > MaxSize.X)
		{
			MaxSize.X = FMath::Abs(WheelOffset.X);
		}
	}

	return MaxSize;
}

FVector UChaosWheeledVehicleMovementComponent::GetWheelRestingPosition(const FChaosWheelSetup& WheelSetup)
{
	FVector Offset = WheelSetup.WheelClass.GetDefaultObject()->Offset + WheelSetup.AdditionalOffset;

	if (WheelSetup.BoneName != NAME_None)
	{
		USkinnedMeshComponent* Mesh = GetMesh();
		if (Mesh && Mesh->SkeletalMesh)
		{
			const FVector BonePosition = Mesh->SkeletalMesh->GetComposedRefPoseMatrix(WheelSetup.BoneName).GetOrigin() * Mesh->GetRelativeScale3D();
			//BonePosition is local for the root BONE of the skeletal mesh - however, we are using the Root BODY which may have its own transform, so we need to return the position local to the root BODY
			const FMatrix RootBodyMTX = Mesh->SkeletalMesh->GetComposedRefPoseMatrix(Mesh->GetBodyInstance()->BodySetup->BoneName);
			const FVector LocalBonePosition = RootBodyMTX.InverseTransformPosition(BonePosition);
			Offset += LocalBonePosition;

		}
	}

	return Offset;
}

void UChaosWheeledVehicleMovementComponent::NextDebugPage()
{
	int PageAsInt = (int)DebugPage;
	PageAsInt++;
	if (PageAsInt >= EDebugPages::MaxDebugPages)
	{
		PageAsInt = 0;
	}
	DebugPage = (EDebugPages)PageAsInt;
}

void UChaosWheeledVehicleMovementComponent::PrevDebugPage()
{
	int PageAsInt = (int)DebugPage;
	PageAsInt--;
	if (PageAsInt < 0)
	{
		PageAsInt = EDebugPages::MaxDebugPages - 1;
	}
	DebugPage = (EDebugPages)PageAsInt;
}


FChaosWheelSetup::FChaosWheelSetup()
	: WheelClass(UChaosVehicleWheel::StaticClass())
	, BoneName(NAME_None)
	, AdditionalOffset(0.0f)
{

}

#endif // WITH_CHAOS

#if VEHICLE_DEBUGGING_ENABLED
PRAGMA_ENABLE_OPTIMIZATION
#endif
