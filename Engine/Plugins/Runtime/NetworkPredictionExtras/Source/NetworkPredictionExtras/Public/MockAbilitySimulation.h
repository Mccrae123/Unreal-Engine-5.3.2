// Copyright 1998-2019 Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "UObject/ObjectMacros.h"
#include "UObject/UObjectGlobals.h"
#include "Engine/NetSerialization.h"
#include "Engine/EngineTypes.h"
#include "Engine/EngineBaseTypes.h"
#include "WorldCollision.h"
#include "Components/ActorComponent.h"
#include "Components/SceneComponent.h"
#include "Misc/OutputDevice.h"
#include "Misc/CoreDelegates.h"

#include "NetworkedSimulationModel.h"
#include "NetworkPredictionComponent.h"
#include "Movement/FlyingMovement.h"

#include "MockAbilitySimulation.generated.h"

// -------------------------------------------------------------------------------------------------------------------------------
// Mock Ability Simulation
//	This is meant to illustrate how a higher level simulation can build off an existing one. While something like GameplayAbilities
//	is more generic and data driven, this illustrates how it will need to solve core issues.
//
//	This implements:
//	1. Stamina "attribute": basic attribute with max + regen value that is consumed by abilities.
//	2. Sprint: Increased max speed while sprint button is held. Drains stamina each frame.
//	3. Dash: Immediate acceleration to a high speed for X seconds.
//	4. Blink: Teleport to location X units ahead
//
//
//	Notes/Todo:
//	-Cooldown/timers for easier tracking (currently relying on high stamina cost to avoid activating each frame)
//	-Outward events for gameplay cue type: tying cosmetic events to everything
//	-More efficient packing of input/pressed buttons. Input handling in general should detect up/down presses rather than just current state
//
// -------------------------------------------------------------------------------------------------------------------------------

// -------------------------------------------------------
// MockAbility Data structures
// -------------------------------------------------------

struct FMockAbilityInputCmd : public FFlyingMovementInputCmd
{
	bool bSprintPressed = false;
	bool bDashPressed = false;
	bool bBlinkPressed = false;

	void NetSerialize(const FNetSerializeParams& P)
	{
		P.Ar << bSprintPressed;
		P.Ar << bDashPressed;
		P.Ar << bBlinkPressed;
		FFlyingMovementInputCmd::NetSerialize(P);
	}

	void Log(FStandardLoggingParameters& P) const
	{
		FFlyingMovementInputCmd::Log(P);
		if (P.Context == EStandardLoggingContext::Full)
		{
			P.Ar->Logf(TEXT("bSprintPressed: %d"), bSprintPressed);
			P.Ar->Logf(TEXT("bDashPressed: %d"), bDashPressed);
			P.Ar->Logf(TEXT("bBlinkPressed: %d"), bBlinkPressed);
		}
	}
};

struct FMockAbilitySyncState : public FFlyingMovementSyncState
{
	float Stamina = 0.f;
	
	void NetSerialize(const FNetSerializeParams& P)
	{
		P.Ar << Stamina;
		FFlyingMovementSyncState::NetSerialize(P);
	}

	void Log(FStandardLoggingParameters& P) const
	{
		FFlyingMovementSyncState::Log(P);
		if (P.Context == EStandardLoggingContext::Full)
		{
			P.Ar->Logf(TEXT("Stamina: %.2f"), Stamina);
		}
	}
};

struct FMockAbilityAuxState : public FFlyingMovementAuxState
{
	float MaxStamina = 100.f;
	float StaminaRegenRate = 20.f;
	int16 DashTimeLeft = 0;
	int16 BlinkWarmupLeft = 0;
	bool bIsSprinting = false;

	void NetSerialize(const FNetSerializeParams& P)
	{
		P.Ar << MaxStamina;
		P.Ar << StaminaRegenRate;
		P.Ar << DashTimeLeft;
		P.Ar << BlinkWarmupLeft;
		P.Ar << bIsSprinting;
		FFlyingMovementAuxState::NetSerialize(P);
	}

	void Log(FStandardLoggingParameters& P) const
	{
		FFlyingMovementAuxState::Log(P);
		if (P.Context == EStandardLoggingContext::Full)
		{
			P.Ar->Logf(TEXT("MaxStamina: %.2f"), MaxStamina);
			P.Ar->Logf(TEXT("StaminaRegenRate: %.2f"), StaminaRegenRate);
			P.Ar->Logf(TEXT("DashTimeLeft: %d"), DashTimeLeft);
			P.Ar->Logf(TEXT("BlinkWarmupLeft: %d"), BlinkWarmupLeft);
			P.Ar->Logf(TEXT("bIsSprinting: %d"), bIsSprinting);
		}
	}
};

// -------------------------------------------------------
// MockAbility NetSimCues - events emitted by the sim
// -------------------------------------------------------

#define LOG_BLINK_CUE 1 // During development, its useful to sanity check that we aren't doing more construction or moves than we expect

// Cue for blink activation.
struct FMockAbilityBlinkCue
{
	FMockAbilityBlinkCue()
	{ 
		UE_CLOG(LOG_BLINK_CUE, LogTemp, Warning, TEXT("  Default Constructor 0x%X"), this);
	}

	FMockAbilityBlinkCue(FVector Start, FVector Stop) : StartLocation(Start), StopLocation(Stop)
	{ 
		UE_CLOG(LOG_BLINK_CUE, LogTemp, Warning, TEXT("  Custom Constructor 0x%X"), this);
	}

	~FMockAbilityBlinkCue()
	{ 
		UE_CLOG(LOG_BLINK_CUE, LogTemp, Warning, TEXT("  Destructor 0x%X"), this);
	}
	
	FMockAbilityBlinkCue(FMockAbilityBlinkCue&& Other)
		: StartLocation(MoveTemp(Other.StartLocation)), StopLocation(Other.StopLocation)
	{
		UE_CLOG(LOG_BLINK_CUE, LogTemp, Warning, TEXT("  Move Constructor 0x%X (Other: 0x%X)"), this, &Other);
	}
	
	FMockAbilityBlinkCue& operator=(FMockAbilityBlinkCue&& Other)
	{
		UE_CLOG(LOG_BLINK_CUE, LogTemp, Warning, TEXT("  Move assignment 0x%X (Other: 0x%X)"), this, &Other);
		StartLocation = MoveTemp(Other.StartLocation);
		StopLocation = MoveTemp(Other.StopLocation);
		return *this;
	}

	FMockAbilityBlinkCue(const FMockAbilityBlinkCue& Other) = delete;
	FMockAbilityBlinkCue& operator=(const FMockAbilityBlinkCue& Other) = delete;

	NETSIMCUE_BODY();

	FVector_NetQuantize10 StartLocation;
	FVector_NetQuantize10 StopLocation;
	
	void NetSerialize(FArchive& Ar)
	{
		bool b = false;
		StartLocation.NetSerialize(Ar, nullptr, b);
		StopLocation.NetSerialize(Ar, nullptr, b);
	}
	
	bool NetUnique(const FMockAbilityBlinkCue& Other) const
	{
		const float ErrorTolerance = 1.f;
		return !StartLocation.Equals(Other.StartLocation, ErrorTolerance) || !StopLocation.Equals(Other.StopLocation, ErrorTolerance);
	}
};

template<>
struct TCueHandlerTraits<FMockAbilityBlinkCue> : public TNetSimCueTraits_Strong { };

// -----------------------------------------------------------------------------------------------------
// Subtypes of the BlinkCue - this is not an expected setup! This is done for testing/debugging so we can 
// see the differences between the cue type traits in a controlled setup. See FMockAbilitySimulation::SimulationTick
// -----------------------------------------------------------------------------------------------------
#define DECLARE_BLINKCUE_SUBTYPE(TYPE, TRAITS) \
 struct TYPE : public FMockAbilityBlinkCue { \
 template <typename... ArgsType> TYPE(ArgsType&&... Args) : FMockAbilityBlinkCue(Forward<ArgsType>(Args)...) { } \
 NETSIMCUE_BODY(); }; \
 template<> struct TCueHandlerTraits<TYPE> : public TRAITS { };

DECLARE_BLINKCUE_SUBTYPE(FMockAbilityBlinkCue_Weak, TNetSimCueTraits_Weak);
DECLARE_BLINKCUE_SUBTYPE(FMockAbilityBlinkCue_ReplicatedNonPredicted, TNetSimCueTraits_ReplicatedNonPredicted);
DECLARE_BLINKCUE_SUBTYPE(FMockAbilityBlinkCue_ReplicatedXOrPredicted, TNetSimCueTraits_ReplicatedXOrPredicted);
DECLARE_BLINKCUE_SUBTYPE(FMockAbilityBlinkCue_Strong, TNetSimCueTraits_Strong);

// The set of Cues the MockAbility simulation will invoke
struct FMockAbilityCueSet
{
	template<typename TDispatchTable>
	static void RegisterNetSimCueTypes(TDispatchTable& DispatchTable)
	{
		DispatchTable.template RegisterType<FMockAbilityBlinkCue>();

		// (Again, not a normal setup, just for debugging/testing purposes)
		DispatchTable.template RegisterType<FMockAbilityBlinkCue_Weak>();
		DispatchTable.template RegisterType<FMockAbilityBlinkCue_ReplicatedNonPredicted>();
		DispatchTable.template RegisterType<FMockAbilityBlinkCue_ReplicatedXOrPredicted>();
		DispatchTable.template RegisterType<FMockAbilityBlinkCue_Strong>();
	}
};


// -------------------------------------------------------
// MockAbilitySimulation definition
// -------------------------------------------------------

using TMockAbilityBufferTypes = TNetworkSimBufferTypes<FMockAbilityInputCmd, FMockAbilitySyncState, FMockAbilityAuxState>;

class FMockAbilitySimulation : public FFlyingMovementSimulation
{
public:

	/** Main update function */
	void SimulationTick(const FNetSimTimeStep& TimeStep, const TNetSimInput<TMockAbilityBufferTypes>& Input, const TNetSimOutput<TMockAbilityBufferTypes>& Output);
};

class FMockAbilityNetSimModelDef : public FNetSimModelDefBase
{
public:

	using Simulation = FMockAbilitySimulation;
	using BufferTypes = TMockAbilityBufferTypes;

	static const FName GroupName;

	// Compare this state with AuthorityState. return true if a reconcile (correction) should happen
	static bool ShouldReconcile(const FMockAbilitySyncState& AuthoritySync, const FMockAbilityAuxState& AuthorityAux, const FMockAbilitySyncState& PredictedSync, const FMockAbilityAuxState& PredictedAux)
	{
		return FFlyingMovementNetSimModelDef::ShouldReconcile(AuthoritySync, AuthorityAux, PredictedSync, PredictedAux);
	}

	static void Interpolate(const TInterpolatorParameters<FMockAbilitySyncState, FMockAbilityAuxState>& Params)
	{
		Params.Out.Sync = Params.To.Sync;
		Params.Out.Aux = Params.To.Aux;

		FFlyingMovementNetSimModelDef::Interpolate(Params.Cast<FFlyingMovementSyncState, FFlyingMovementAuxState>());
	}
};

/** Additional specialized types of the Parametric Movement NetSimModel */
class FMockAbilityNetSimModelDef_Fixed30hz : public FMockAbilityNetSimModelDef
{ 
public:
	using TickSettings = TNetworkSimTickSettings<33>;
};

// -------------------------------------------------------------------------------------------------------------------------------
// ActorComponent for running Mock Ability Simulation 
// -------------------------------------------------------------------------------------------------------------------------------

UCLASS(BlueprintType, meta=(BlueprintSpawnableComponent))
class NETWORKPREDICTIONEXTRAS_API UMockFlyingAbilityComponent : public UFlyingMovementComponent, public TNetworkedSimulationModelDriver<TMockAbilityBufferTypes>
{
	GENERATED_BODY()

public:

	UMockFlyingAbilityComponent();

	DECLARE_DELEGATE_TwoParams(FProduceMockAbilityInput, const FNetworkSimTime /*SimTime*/, FMockAbilityInputCmd& /*Cmd*/)
	FProduceMockAbilityInput ProduceInputDelegate;

	TNetSimStateAccessor<FMockAbilitySyncState> AbilitySyncState;
	TNetSimStateAccessor<FMockAbilityAuxState> AbilityAuxState;

	// IMockFlyingAbilitySystemDriver
	FString GetDebugName() const override;
	const AActor* GetVLogOwner() const override;
	void VisualLog(const FMockAbilityInputCmd* Input, const FMockAbilitySyncState* Sync, const FMockAbilityAuxState* Aux, const FVisualLoggingParameters& SystemParameters) const override;

	void ProduceInput(const FNetworkSimTime SimTime, FMockAbilityInputCmd& Cmd) override;
	void FinalizeFrame(const FMockAbilitySyncState& SyncState, const FMockAbilityAuxState& AuxState) override;

	// Required to supress -Woverloaded-virtual warning. We are effectively hiding UFlyingMovementComponent's methods
	using UFlyingMovementComponent::VisualLog;
	using UFlyingMovementComponent::ProduceInput;
	using UFlyingMovementComponent::FinalizeFrame;

	// NetSimCues
	void HandleCue(FMockAbilityBlinkCue& BlinkCue, const FNetSimCueSystemParamemters& SystemParameters);

	// -------------------------------------------------------------------------------------
	//	Ability State and Notifications
	//		-This allows user code/blueprints to respond to state changes.
	//		-These values always reflect the latest simulation state
	//		-StateChange events are just that: when the state changes. They are not emitted from the sim themselves.
	//			-This means they "work" for interpolated simulations and are resilient to packet loss and crazy network conditions
	//			-That said, its "latest" only. There is NO guarantee that you will receive every state transition
	//
	// -------------------------------------------------------------------------------------

	DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FMockAbilityNotifyStateChange, bool, bNewStateValue);

	// Notifies when Sprint state changes
	UPROPERTY(BlueprintAssignable, Category="Mock AbilitySystem")
	FMockAbilityNotifyStateChange OnSprintStateChange;

	// Notifies when Dash state changes
	UPROPERTY(BlueprintAssignable, Category="Mock AbilitySystem")
	FMockAbilityNotifyStateChange OnDashStateChange;

	// Notifies when Blink Changes
	UPROPERTY(BlueprintAssignable, Category="Mock AbilitySystem")
	FMockAbilityNotifyStateChange OnBlinkStateChange;

	// Are we currently in the sprinting state
	UFUNCTION(BlueprintCallable, Category="Mock AbilitySystem")
	bool IsSprinting() const { return bIsSprinting; }

	// Are we currently in the dashing state
	UFUNCTION(BlueprintCallable, Category="Mock AbilitySystem")
	bool IsDashing() const { return bIsDashing; }

	// Are we currently in the blinking (startup) state
	UFUNCTION(BlueprintCallable, Category="Mock AbilitySystem")
	bool IsBlinking() const { return bIsBlinking; }

private:
	
	// Local cached values for detecting state changes from the sim in ::FinalizeFrame
	// Its tempting to think ::FinalizeFrame could pass in the previous frames values but this could
	// not be reliable if buffer sizes are small and network conditions etc - you may not always know
	// what was the "last finalized frame" or even have it in the buffers anymore.
	bool bIsSprinting = false;
	bool bIsDashing = false;
	bool bIsBlinking = false;

protected:

	// Network Prediction
	virtual INetworkedSimulationModel* InstantiateNetworkedSimulation() override;
	FMockAbilitySimulation* MockAbilitySimulation = nullptr;

	template<typename TSimulation>
	void InitMockAbilitySimulation(TSimulation* Simulation, FMockAbilitySyncState& InitialSyncState, FMockAbilityAuxState& InitialAuxState)
	{
		check(MockAbilitySimulation == nullptr);
		MockAbilitySimulation = Simulation;	
		InitFlyingMovementSimulation(Simulation, InitialSyncState, InitialAuxState);
	}

	template<typename TNetSimModel>
	void InitMockAbilityNetSimModel(TNetSimModel* Model)
	{
		AbilitySyncState.Bind(Model);
		MovementAuxState.Bind(Model);
		InitFlyingMovementNetSimModel(Model);
	}
};