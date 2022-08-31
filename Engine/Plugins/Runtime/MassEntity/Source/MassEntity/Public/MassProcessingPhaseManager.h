// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/Object.h"
#include "UObject/GCObject.h"
#include "Engine/EngineBaseTypes.h"
#include "MassProcessingTypes.h"
#include "MassProcessor.h"
#include "MassProcessingPhaseManager.generated.h"


struct FMassProcessingPhaseManager;
class UMassProcessor;
class UMassCompositeProcessor;
struct FMassEntityManager;
struct FMassCommandBuffer;
struct FMassProcessingPhaseConfig;


USTRUCT()
struct MASSENTITY_API FMassProcessingPhaseConfig
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, Category = Mass, config)
	FName PhaseName;

	UPROPERTY(EditAnywhere, Category = Mass, config, NoClear)
	TSubclassOf<UMassCompositeProcessor> PhaseGroupClass = UMassCompositeProcessor::StaticClass();

	UPROPERTY(Transient)
	TArray<TObjectPtr<UMassProcessor>> ProcessorCDOs;

#if WITH_EDITORONLY_DATA
	// this processor is available only in editor since it's used to present the user the order in which processors
	// will be executed when given processing phase gets triggered
	UPROPERTY(Transient)
	TObjectPtr<UMassCompositeProcessor> PhaseProcessor = nullptr;

	UPROPERTY(VisibleAnywhere, Category = Mass, Transient)
	FText Description;
#endif //WITH_EDITORONLY_DATA
};


struct MASSENTITY_API FMassProcessingPhase : public FTickFunction
{
	DECLARE_MULTICAST_DELEGATE_OneParam(FOnPhaseEvent, const float /*DeltaSeconds*/);

	FMassProcessingPhase();
	FMassProcessingPhase(const FMassProcessingPhase& Other) = delete;
	FMassProcessingPhase& operator=(const FMassProcessingPhase& Other) = delete;

protected:
	// FTickFunction interface
	virtual void ExecuteTick(float DeltaTime, ELevelTick TickType, ENamedThreads::Type CurrentThread, const FGraphEventRef& MyCompletionGraphEvent) override;
	virtual FString DiagnosticMessage() override;
	virtual FName DiagnosticContext(bool bDetailed) override;
	// End of FTickFunction interface

	void OnParallelExecutionDone(const float DeltaTime);

	bool IsConfiguredForParallelMode() const { return bRunInParallelMode; }
	void ConfigureForParallelMode() { bRunInParallelMode = true; }
	void ConfigureForSingleThreadMode() { bRunInParallelMode = false; }

public:
	bool IsDuringMassProcessing() const { return bIsDuringMassProcessing; }

	void Initialize(FMassProcessingPhaseManager& InPhaseManager, const EMassProcessingPhase InPhase, const ETickingGroup InTickGroup, UMassCompositeProcessor& InPhaseProcessor);

protected:
	friend FMassProcessingPhaseManager;

	// composite processor representing work to be performed. GC-referenced via AddReferencedObjects
	UMassCompositeProcessor* PhaseProcessor = nullptr;

	EMassProcessingPhase Phase = EMassProcessingPhase::MAX;
	FOnPhaseEvent OnPhaseStart;
	FOnPhaseEvent OnPhaseEnd;

private:
	FMassProcessingPhaseManager* PhaseManager = nullptr;
	bool bRunInParallelMode = false;
	std::atomic<bool> bIsDuringMassProcessing = false;
};


/** 
 * MassProcessingPhaseManager owns separate FMassProcessingPhase instances for every ETickingGroup. When activated
 * via Start function it registers and enables the FMassProcessingPhase instances which themselves are tick functions 
 * that host UMassCompositeProcessor which they trigger as part of their Tick function. 
 * MassProcessingPhaseManager serves as an interface to said FMassProcessingPhase instances and allows initialization
 * with collections of processors (via Initialize function) as well as registering arbitrary functions to be called 
 * when a particular phase starts or ends (via GetOnPhaseStart and GetOnPhaseEnd functions). 
 */
struct MASSENTITY_API FMassProcessingPhaseManager : public FGCObject
{
public:
	FMassProcessingPhaseManager() = default;
	FMassProcessingPhaseManager(const FMassProcessingPhaseManager& Other) = delete;
	FMassProcessingPhaseManager& operator=(const FMassProcessingPhaseManager& Other) = delete;

	FMassEntityManager& GetEntityManagerRef() { check(EntityManager); return *EntityManager.Get(); }

	/** Retrieves OnPhaseStart multicast delegate's reference for a given Phase */
	FMassProcessingPhase::FOnPhaseEvent& GetOnPhaseStart(const EMassProcessingPhase Phase) { return ProcessingPhases[uint8(Phase)].OnPhaseStart; }
	/** Retrieves OnPhaseEnd multicast delegate's reference for a given Phase */
	FMassProcessingPhase::FOnPhaseEvent& GetOnPhaseEnd(const EMassProcessingPhase Phase) { return ProcessingPhases[uint8(Phase)].OnPhaseEnd; }

	/** 
	 *  Populates hosted FMassProcessingPhase instances with Processors read from MassEntitySettings configuration.
	 *  Calling this function overrides previous configuration of Phases.
	 */
	void Initialize(UObject& InOwner, TConstArrayView<FMassProcessingPhaseConfig> ProcessingPhasesConfig, const FString& DependencyGraphFileName = TEXT(""));

	/** Needs to be called before destruction, ideally before owner's BeginDestroy (a FGCObject's limitation) */
	void Deinitialize();

	/** 
	 *  Stores EntityManager associated with given world's MassEntitySubsystem and kicks off phase ticking.
	 */
	void Start(UWorld& World);
	
	/**
	 *  Stores InEntityManager as the entity manager and kicks off phase ticking.
	 */
	void Start(const TSharedPtr<FMassEntityManager>& InEntityManager);
	void Stop();
	bool IsRunning() const { return EntityManager.IsValid(); }

	/** 
	 *  returns true when called while any of the ProcessingPhases is actively executing its processors. Used to 
	 *  determine whether it's safe to do entity-related operations like adding fragments.
	 *  Note that the function will return false while the OnPhaseStart or OnPhaseEnd are being broadcast,
	 *  the value returned will be `true` only when the entity subsystem is actively engaged 
	 */
	bool IsDuringMassProcessing() const { return CurrentPhase != EMassProcessingPhase::MAX && ProcessingPhases[int(CurrentPhase)].IsDuringMassProcessing(); }

	FString GetName() const;

protected:
	// FGCObject interface
	virtual void AddReferencedObjects(FReferenceCollector& Collector) override;
	virtual FString GetReferencerName() const override
	{
		return TEXT("FMassProcessingPhaseManager");
	}
	// End of FGCObject interface

	void EnableTickFunctions(const UWorld& World);

	/** Creates phase processors instances for each declared phase name, based on MassEntitySettings */
	void CreatePhases();

	friend FMassProcessingPhase;

	/** 
	 *  Called by the given Phase at the very start of its execution function (the FMassProcessingPhase::ExecuteTick),
	 *  even before the FMassProcessingPhase.OnPhaseStart broadcast delegate
	 */
	void OnPhaseStart(const FMassProcessingPhase& Phase);

	/**
	 *  Called by the given Phase at the very end of its execution function (the FMassProcessingPhase::ExecuteTick),
	 *  after the FMassProcessingPhase.OnPhaseEnd broadcast delegate
	 */
	void OnPhaseEnd(FMassProcessingPhase& Phase);

protected:	

	FMassProcessingPhase ProcessingPhases[(uint8)EMassProcessingPhase::MAX];

	TSharedPtr<FMassEntityManager> EntityManager;

	EMassProcessingPhase CurrentPhase = EMassProcessingPhase::MAX;

	TWeakObjectPtr<UObject> Owner;
};
