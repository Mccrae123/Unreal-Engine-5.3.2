// Copyright 1998-2019 Epic Games, Inc. All Rights Reserved.

#include "MockAbilitySimulation.h"
#include "DrawDebugHelpers.h"
#include "VisualLogger/VisualLogger.h"

namespace MockAbilityCVars
{
	NETSIM_DEVCVAR_SHIPCONST_FLOAT(DefaultMaxSpeed, 1200.f, "mockability.DefaultMaxSpeed", "Default Speed");
	NETSIM_DEVCVAR_SHIPCONST_FLOAT(DefaultAcceleration, 4000.f, "mockability.DefaultAcceleration", "Default Acceleration");

	NETSIM_DEVCVAR_SHIPCONST_FLOAT(SprintMaxSpeed, 5000.f, "mockability.SprintMaxSpeed", "Max Speed when sprint is applied.");

	NETSIM_DEVCVAR_SHIPCONST_FLOAT(DashMaxSpeed, 7500.f, "mockability.DashMaxSpeed", "Max Speed when dashing.");
	NETSIM_DEVCVAR_SHIPCONST_FLOAT(DashAcceleration, 100000.f, "mockability.DashAcceleration", "Acceleration when dashing.");
}

// -------------------------------------------------------------------------------------------------------------
//
// -------------------------------------------------------------------------------------------------------------


const FName FMockAbilitySimulation::GroupName(TEXT("Ability"));

void FMockAbilitySimulation::Update(const float DeltaTimeSeconds, const FMockAbilityInputCmd& InputCmd, const FMockAbilitySyncState& InputState, FMockAbilitySyncState& OutputState, const FMockAbilityAuxstate& AuxState, const TNetSimLazyWriter<FMockAbilityAuxstate>& OutAuxStateAccessor)
{
	// Stamina passes through. Some code paths will modify this again, but if we don't set the output state it will be garbage/stale
	// (considering implicit copying of old state to new state by the NetworkedSimModel code, but that could be undesired/inefficient in some cases)
	OutputState.Stamina = InputState.Stamina;

	static float BlinkCost = 75.f;
	const bool bBlink = (InputCmd.bBlinkPressed && InputState.Stamina > BlinkCost);

	if (bBlink)
	{
		static float BlinkDist = 1000.f;
		static float OverlapCheckInflation = 0.100f;

		FVector DestLocation = InputState.Location + InputState.Rotation.RotateVector( FVector(BlinkDist, 0.f, 0.f) );
		AActor* OwningActor = UpdatedComponent->GetOwner();
		check(OwningActor);

		// DrawDebugLine(OwningActor->GetWorld(), InputState.Location, DestLocation, FColor::Red, false);	

		// Its unfortunate teleporting is so complicated. It may make sense for a new movement simulation to define this themselves, 
		// but for this mock one, we will just use the engine's AActor teleport.
		if (OwningActor->TeleportTo(DestLocation, InputState.Rotation))
		{
			OutputState = InputState;

			// Component now has the final location
			const FTransform UpdateComponentTransform = GetUpdateComponentTransform();
			OutputState.Location = UpdateComponentTransform.GetLocation();
			OutputState.Stamina = InputState.Stamina - BlinkCost;

			// And we skip the normal update simulation for this frame. This is just a choice. We could still allow it to run.
			return;
		}
	}

	// Dash is implemented in the following way:
	//	-Stamina consumed on initial press
	//	-MaxSpeed/Acceleration are jacked up
	//	-Dash lasts for 400ms (DashDurationMS)
	//		-division of frame times can cause you to dash for longer. We would have to break up simulation steps to support this 100% accurately.
	//	-Movement input is synthesized while in dash state. That is, we force forward movement and ignore what was actually fed into the simulation.
	//
	// This is just a simple/interesting way of implementing dash in the this system. A real movement/ability system will probably have some concept of
	// root motion/sources that drive movement forward.

	static float DashCost = 75.f;
	static int16 DashDurationMS = 400;
	int16 DashTimeLeft = AuxState.DashTimeLeft;
	bool bIsDashing = AuxState.DashTimeLeft > 0;
	
	if (InputCmd.bDashPressed && InputState.Stamina > DashCost && bIsDashing == false)
	{
		// Start dashing
		DashTimeLeft = DashDurationMS;
		OutputState.Stamina -= DashCost;
		bIsDashing = true;
	}

	if (bIsDashing)
	{
		FMockAbilityAuxstate* OutAuxState = OutAuxStateAccessor.Get();
		OutAuxState->DashTimeLeft = FMath::Max<int16>(DashTimeLeft - (int16)(DeltaTimeSeconds * 1000.f), 0);

		FMockAbilityAuxstate LocalAuxState = AuxState;
		LocalAuxState.MaxSpeed = MockAbilityCVars::DashMaxSpeed();
		LocalAuxState.Acceleration = MockAbilityCVars::DashAcceleration();

		FMockAbilityInputCmd LocalInputCmd = InputCmd;
		LocalInputCmd.MovementInput = FVector(1.f, 0.f, 0.f);
		
		FlyingMovement::FMovementSimulation::Update(DeltaTimeSeconds, LocalInputCmd, InputState, OutputState, LocalAuxState, OutAuxStateAccessor);

		if (OutAuxState->DashTimeLeft == 0)
		{
			// Stop when dash is over
			OutputState.Velocity = FVector::ZeroVector;
		}
	}
	else
	{
		// Sprint (mutually exclusive from Dash state)		
		static float SprintBaseCost = 100.f;
		const float SprintCostThisFrame = SprintBaseCost * DeltaTimeSeconds;
		const bool bIsSprinting = (InputCmd.bSprintPressed && InputState.Stamina > SprintCostThisFrame);

		// Set our max speed. This is an interesting case.
		//	-Our input states are already "final". It doesn't make sense to modify the input AuxState data.
		//	-But, we want to feed this locally calculated max speed into the base movement simulation. So, it is an "input" in that sense.
		//	-Creating a local copy of the aux state, modifying it, and passing that into the base movement sim is clean, though a bit inefficient.
		//	-This leaves us with a weird side effect that the calculated MaxSpeed is never written to the AuxBuffer. Though we could write it to the
		//	output AuxState, that doesn't really do anything useful: the base move sim won't use it and it'll just be left there for next frame (to be overriden again).
		//
		// The way the base movement simulation is written, MaxSpeed is a clearly defined input. But in the MockAbility sim, MaxSpeed is a derived
		// value from other input state. This difference in how the simulations treat the variable is what causes the need to do this.
		//
		// It would be possible to write the base movement sim in a way that MaxSpeed is transient value on the sim class (FlyingMovement::FMovementSimulation).
		// Something like, "MaxSpeed" really means "Base max speed" and their would be a "GetCurrentMaxSpeed" virtual on the sim.
		// This would make things a bit more awkward in the base case with no ability system. So, for now, this seems like a good pattern/precedence.

		FMockAbilityAuxstate LocalAuxState = AuxState;
		LocalAuxState.MaxSpeed = bIsSprinting ? MockAbilityCVars::SprintMaxSpeed() : MockAbilityCVars::DefaultMaxSpeed();

		if (bIsSprinting)
		{
			OutputState.Stamina = FMath::Max<float>( InputState.Stamina - SprintCostThisFrame, 0.f );
		}
		else if (OutputState.Stamina < AuxState.MaxStamina)
		{
			OutputState.Stamina = FMath::Min<float>( InputState.Stamina + (DeltaTimeSeconds * AuxState.StaminaRegenRate), AuxState.MaxStamina );
		}
		else
		{
			OutputState.Stamina = InputState.Stamina;
		}

		FlyingMovement::FMovementSimulation::Update(DeltaTimeSeconds, InputCmd, InputState, OutputState, LocalAuxState, OutAuxStateAccessor);
	}
}

// -------------------------------------------------------------------------------------------------------------
//
// -------------------------------------------------------------------------------------------------------------

UMockFlyingAbilityComponent::UMockFlyingAbilityComponent()
{

}

INetworkSimulationModel* UMockFlyingAbilityComponent::InstantiateNetworkSimulation()
{
	check(UpdatedComponent);
	
	// FIXME: init sim helper on super?
	MockAbilitySimulation.Reset(new FMockAbilitySimulation());
	MockAbilitySimulation->UpdatedComponent = UpdatedComponent;
	MockAbilitySimulation->UpdatedPrimitive = UpdatedPrimitive;
	
	FMockAbilitySyncState InitialSyncState;
	InitialSyncState.Location = UpdatedComponent->GetComponentLocation();
	InitialSyncState.Rotation = UpdatedComponent->GetComponentQuat().Rotator();	

	FMockAbilityAuxstate InitialAuxState;
	InitialAuxState.MaxSpeed = MockAbilityCVars::DefaultMaxSpeed();

	auto NewModel = new FMockAbilitySystem<0>(MockAbilitySimulation.Get(), this, InitialSyncState, InitialAuxState);

	MovementSyncState.Init(NewModel);
	MovementAuxState.Init(NewModel);
	return NewModel;
}

void UMockFlyingAbilityComponent::ProduceInput(const FNetworkSimTime SimTime, FMockAbilityInputCmd& Cmd)
{
	// This isn't ideal. It probably makes sense for the component to do all the input binding rather.
	ProduceInputDelegate.ExecuteIfBound(SimTime, Cmd);
}

void UMockFlyingAbilityComponent::FinalizeFrame(const FMockAbilitySyncState& SyncState, const FMockAbilityAuxstate& AuxState)
{
	Super::FinalizeFrame(SyncState, AuxState);
}

FString UMockFlyingAbilityComponent::GetDebugName() const
{
	return FString::Printf(TEXT("MockAbility. %s. %s"), *UEnum::GetValueAsString(TEXT("Engine.ENetRole"), GetOwnerRole()), *GetName());
}

const AActor* UMockFlyingAbilityComponent::GetVLogOwner() const
{
	return GetOwner();
}

void UMockFlyingAbilityComponent::VisualLog(const FMockAbilityInputCmd* Input, const FMockAbilitySyncState* Sync, const FMockAbilityAuxstate* Aux, const FVisualLoggingParameters& SystemParameters) const
{
	Super::VisualLog(Input, Sync, Aux, SystemParameters);
}

// ---------------------------------------------------------------------------------
