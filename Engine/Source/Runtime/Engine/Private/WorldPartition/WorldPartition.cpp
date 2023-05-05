// Copyright Epic Games, Inc. All Rights Reserved.

/*=============================================================================
	WorldPartition.cpp: UWorldPartition implementation
=============================================================================*/
#include "WorldPartition/WorldPartition.h"
#include "Misc/PackageName.h"
#include "UObject/UObjectIterator.h"
#include "Misc/Paths.h"
#include "WorldPartition/WorldPartitionLog.h"
#include "Misc/StringFormatArg.h"
#include "WorldPartition/WorldPartitionLevelStreamingPolicy.h"
#include "WorldPartition/WorldPartitionReplay.h"
#include "WorldPartition/HLOD/HLODSubsystem.h"
#include "WorldPartition/DataLayer/DataLayerManager.h"
#include "GameFramework/WorldSettings.h"
#include "ProfilingDebugging/ScopedTimers.h"
#include "UObject/UE5MainStreamObjectVersion.h"
#include "LandscapeProxy.h"
#include "Engine/LevelStreaming.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(WorldPartition)

#if WITH_EDITOR
#include "LevelUtils.h"
#include "Selection.h"
#include "HAL/FileManager.h"
#include "LevelEditorViewport.h"
#include "ScopedTransaction.h"
#include "LocationVolume.h"
#include "Engine/LevelScriptBlueprint.h"
#include "ActorReferencesUtils.h"
#include "WorldPartition/DataLayer/WorldDataLayersActorDesc.h"
#include "WorldPartition/IWorldPartitionEditorModule.h"
#include "WorldPartition/WorldPartitionLevelHelper.h"
#include "WorldPartition/WorldPartitionEditorHash.h"
#include "WorldPartition/WorldPartitionRuntimeHash.h"
#include "WorldPartition/WorldPartitionMiniMap.h"
#include "WorldPartition/WorldPartitionMiniMapHelper.h"
#include "WorldPartition/DataLayer/WorldDataLayers.h"
#include "WorldPartition/LoaderAdapter/LoaderAdapterShape.h"
#include "WorldPartition/LoaderAdapter/LoaderAdapterPinnedActors.h"
#include "WorldPartition/HLOD/HLODLayer.h"
#include "WorldPartition/Cook/WorldPartitionCookPackageContextInterface.h"
#include "Modules/ModuleManager.h"
#include "GameDelegates.h"
#else
#include "Engine/Level.h"
#endif //WITH_EDITOR

#define LOCTEXT_NAMESPACE "WorldPartition"

namespace WorldPartition
{
#if WITH_EDITOR
	static const EConsoleVariableFlags ECVF_Runtime_ReadOnly = ECVF_Default;
#else
	static const EConsoleVariableFlags ECVF_Runtime_ReadOnly = ECVF_ReadOnly;
#endif
}

#if WITH_EDITOR
int32 UWorldPartition::LoadingRangeBugItGo = 12800;
FAutoConsoleVariableRef UWorldPartition::CVarLoadingRangeBugItGo(
	TEXT("wp.Editor.LoadingRangeBugItGo"),
	UWorldPartition::LoadingRangeBugItGo,
	TEXT("Loading range for BugItGo command."),
	ECVF_Default);

int32 UWorldPartition::WorldExtentToEnableStreaming = 400000;
FAutoConsoleVariableRef UWorldPartition::CVarWorldExtentToEnableStreaming(
	TEXT("wp.Editor.WorldExtentToEnableStreaming"),
	UWorldPartition::WorldExtentToEnableStreaming,
	TEXT("World extend to justify enabling streaming."),
	ECVF_Default);

bool UWorldPartition::DebugDedicatedServerStreaming = false;
FAutoConsoleVariableRef UWorldPartition::CVarDebugDedicatedServerStreaming(
	TEXT("wp.Runtime.DebugDedicatedServerStreaming"),
	UWorldPartition::DebugDedicatedServerStreaming,
	TEXT("Turn on/off to debug of server streaming."),
	ECVF_Default);

int32 UWorldPartition::EnableSimulationStreamingSource = 1;
FAutoConsoleVariableRef UWorldPartition::CVarEnableSimulationStreamingSource(
	TEXT("wp.Runtime.EnableSimulationStreamingSource"),
	UWorldPartition::EnableSimulationStreamingSource,
	TEXT("Set to 0 to if you want to disable the simulation/ejected camera streaming source."),
	ECVF_Default);
#endif

int32 UWorldPartition::GlobalEnableServerStreaming = 0;
FAutoConsoleVariableRef UWorldPartition::CVarEnableServerStreaming(
	TEXT("wp.Runtime.EnableServerStreaming"),
	UWorldPartition::GlobalEnableServerStreaming,
	TEXT("Set to 1 to enable server streaming, set to 2 to only enable it in PIE.\n")
	TEXT("Changing the value while the game is running won't be considered."),
	WorldPartition::ECVF_Runtime_ReadOnly);

bool UWorldPartition::bGlobalEnableServerStreamingOut = false;
FAutoConsoleVariableRef UWorldPartition::CVarEnableServerStreamingOut(
	TEXT("wp.Runtime.EnableServerStreamingOut"),
	UWorldPartition::bGlobalEnableServerStreamingOut,
	TEXT("Turn on/off to allow or not the server to stream out levels (only relevant when server streaming is enabled)\n")
	TEXT("Changing the value while the game is running won't be considered."),
	WorldPartition::ECVF_Runtime_ReadOnly);

bool UWorldPartition::bUseMakingVisibleTransactionRequests = false;
FAutoConsoleVariableRef UWorldPartition::CVarUseMakingVisibleTransactionRequests(
	TEXT("wp.Runtime.UseMakingVisibleTransactionRequests"),
	UWorldPartition::bUseMakingVisibleTransactionRequests,
	TEXT("Whether the client should wait for the server to acknowledge visibility update before making partitioned world streaming levels visible.\n")
	TEXT("Changing the value while the game is running won't be considered."),
	WorldPartition::ECVF_Runtime_ReadOnly);

bool UWorldPartition::bUseMakingInvisibleTransactionRequests = false;
FAutoConsoleVariableRef UWorldPartition::CVarUseMakingInvisibleTransactionRequests(
	TEXT("wp.Runtime.UseMakingInvisibleTransactionRequests"),
	UWorldPartition::bUseMakingInvisibleTransactionRequests,
	TEXT("Whether the client should wait for the server to acknowledge visibility update before making partitioned world streaming levels invisible.\n")
	TEXT("Changing the value while the game is running won't be considered."),
	WorldPartition::ECVF_Runtime_ReadOnly);

#if WITH_EDITOR
TMap<FName, FString> GetDataLayersDumpString(const UWorldPartition* WorldPartition)
{
	TMap<FName, FString> DataLayersDumpString;
	const UDataLayerManager* DataLayerManager = WorldPartition->GetDataLayerManager();
	DataLayerManager->ForEachDataLayerInstance([&DataLayersDumpString](const UDataLayerInstance* DataLayerInstance)
	{
		DataLayersDumpString.FindOrAdd(DataLayerInstance->GetDataLayerFName()) = FString::Format(TEXT("{0}{1})"), { DataLayerInstance->GetDataLayerShortName(), DataLayerInstance->GetDataLayerFName().ToString() });
		return true;
	});
	
	return DataLayersDumpString;
}

FString GetActorDescDumpString(const FWorldPartitionActorDesc* ActorDesc, const TMap<FName, FString>& DataLayersDumpString)
{
	auto GetDataLayerString = [&DataLayersDumpString](const TArray<FName>& DataLayerNames)
	{
		if (DataLayerNames.IsEmpty())
		{
			return FString("None");
		}

		return FString::JoinBy(DataLayerNames, TEXT(", "), [&DataLayersDumpString](const FName& DataLayerName) 
		{ 
			if (const FString* DumpString = DataLayersDumpString.Find(DataLayerName))
			{
				return *DumpString;
			}
			return DataLayerName.ToString(); 
		});
	};

	check(ActorDesc);
	return FString::Printf(
		TEXT("%s DataLayerNames:%s") LINE_TERMINATOR, 
		*ActorDesc->ToString(FWorldPartitionActorDesc::EToStringMode::Full), 
		*GetDataLayerString(ActorDesc->GetDataLayerInstanceNames())
	);
}

static FAutoConsoleCommand DumpActorDesc(
	TEXT("wp.Editor.DumpActorDesc"),
	TEXT("Dump a specific actor descriptor on the console."),
	FConsoleCommandWithArgsDelegate::CreateLambda([](const TArray<FString>& Args)
	{
		TArray<FString> ActorPaths;
		if (Args.Num() > 0)
		{
			ActorPaths.Add(Args[0]);
		}
		else
		{
			for (FSelectionIterator SelectionIt(*GEditor->GetSelectedActors()); SelectionIt; ++SelectionIt)
			{
				if (const AActor* Actor = CastChecked<AActor>(*SelectionIt))
				{
					ActorPaths.Add(Actor->GetPathName());
				}
			}
		}

		if (!ActorPaths.IsEmpty())
		{
			if (UWorld* World = GEditor->GetEditorWorldContext().World())
			{
				if (!World->IsGameWorld())
				{
					if (UWorldPartition* WorldPartition = World->GetWorldPartition())
					{
						TMap<FName, FString> DataLayersDumpString = GetDataLayersDumpString(WorldPartition);
						for (const FString& ActorPath : ActorPaths)
						{
							if (const FWorldPartitionActorDesc* ActorDesc = WorldPartition->GetActorDescByName(ActorPath))
							{
								UE_LOG(LogWorldPartition, Log, TEXT("%s"), *GetActorDescDumpString(ActorDesc, DataLayersDumpString));
							}
						}
					}
				}
			}
		}
	})
);

static FAutoConsoleCommand DumpActorDescs(
	TEXT("wp.Editor.DumpActorDescs"),
	TEXT("Dump the list of actor descriptors in a CSV file."),
	FConsoleCommandWithArgsDelegate::CreateLambda([](const TArray<FString>& Args)
	{
		if (Args.Num() > 0)
		{
			if (UWorld* World = GEditor->GetEditorWorldContext().World())
			{
				if (!World->IsGameWorld())
				{
					if (UWorldPartition* WorldPartition = World->GetWorldPartition())
					{
						WorldPartition->DumpActorDescs(Args[0]);
					}
				}
			}
		}
	})
);

static FAutoConsoleCommand SetLogWorldPartitionVerbosity(
	TEXT("wp.Editor.SetLogWorldPartitionVerbosity"),
	TEXT("Change the WorldPartition verbosity log verbosity."),
	FConsoleCommandWithArgsDelegate::CreateLambda([](const TArray<FString>& Args)
	{
		if (Args.Num() == 1)
		{
			if (Args[0].Contains(TEXT("Verbose")))
			{
				LogWorldPartition.SetVerbosity(ELogVerbosity::Verbose);
			}
			else
			{
				LogWorldPartition.SetVerbosity(LogWorldPartition.GetCompileTimeVerbosity());
			}
		}
	})
);

class FLoaderAdapterAlwaysLoadedActors : public FLoaderAdapterShape
{
public:
	FLoaderAdapterAlwaysLoadedActors(UWorld* InWorld)
		: FLoaderAdapterShape(InWorld, FBox(FVector(-HALF_WORLD_MAX, -HALF_WORLD_MAX, -HALF_WORLD_MAX), FVector(HALF_WORLD_MAX, HALF_WORLD_MAX, HALF_WORLD_MAX)), TEXT("Always Loaded"))
	{
		bIncludeSpatiallyLoadedActors = false;
		bIncludeNonSpatiallyLoadedActors = true;
	}
};

#endif

UWorldPartition::UWorldPartition(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
#if WITH_EDITOR
	, EditorHash(nullptr)
	, AlwaysLoadedActors(nullptr)
	, PinnedActors(nullptr)
	, WorldPartitionEditor(nullptr)
	, bStreamingWasEnabled(true)
	, bShouldCheckEnableStreamingWarning(false)
	, bCanBeUsedByLevelInstance(false)
	, bForceGarbageCollection(false)
	, bForceGarbageCollectionPurge(false)
	, bEnablingStreamingJustified(false)
	, bIsPIE(false)
	, NumUserCreatedLoadedRegions(0)
	, bForceEnableStreamingInEditor(false)
#endif
	, InitState(EWorldPartitionInitState::Uninitialized)
	, bStreamingInEnabled(true)
	, DataLayerManager(nullptr)
	, StreamingPolicy(nullptr)
	, Replay(nullptr)
{
	bEnableStreaming = true;
	ServerStreamingMode = EWorldPartitionServerStreamingMode::ProjectDefault;
	ServerStreamingOutMode = EWorldPartitionServerStreamingOutMode::ProjectDefault;

#if WITH_EDITOR
	WorldPartitionStreamingPolicyClass = UWorldPartitionLevelStreamingPolicy::StaticClass();
#endif
}

#if WITH_EDITOR
void UWorldPartition::OnGCPostReachabilityAnalysis()
{
	const TIndirectArray<FWorldContext>& WorldContextList = GEngine->GetWorldContexts();

	// Avoid running this process while a game world is live
	for (const FWorldContext& WorldContext : WorldContextList)
	{
		if (WorldContext.World() != nullptr && WorldContext.World()->IsGameWorld())
		{
			return;
		}
	}

	for (FRawObjectIterator It; It; ++It)
	{
		if (AActor* Actor = Cast<AActor>(static_cast<UObject*>(It->Object)))
		{
			if (Actor->IsUnreachable() && !Actor->GetClass()->HasAnyClassFlags(CLASS_NewerVersionExists) && Actor->IsMainPackageActor())
			{
				ForEachObjectWithPackage(Actor->GetPackage(), [this, Actor](UObject* Object)
				{
					if (Object->HasAnyFlags(RF_Standalone))
					{
						UE_LOG(LogWorldPartition, Log, TEXT("Actor %s is unreachable without properly detaching object %s in its package"), *Actor->GetPathName(), *Object->GetPathName());

						Object->ClearFlags(RF_Standalone);

						// Make sure we trigger a second GC at the next tick to properly destroy packages there were fixed in this pass
						bForceGarbageCollection = true;
						bForceGarbageCollectionPurge = true;
					}
					return true;
				}, false);
			}
		}
	}
}

void UWorldPartition::OnPackageDirtyStateChanged(UPackage* Package)
{
	auto ShouldHandleActor = [this](AActor* Actor) {return Actor && Actor->IsMainPackageActor() && (Actor->GetLevel() != nullptr) && IsActorDescHandled(Actor);};

	if (AActor* Actor = AActor::FindActorInPackage(Package); ShouldHandleActor(Actor))
	{
		if (FWorldPartitionHandle ActorHandle(this, Actor->GetActorGuid()); ActorHandle.IsValid())
		{
			if (Package->IsDirty())
			{
				DirtyActors.Add(ActorHandle.ToReference(), Actor);
			}
		}
	}
}

// Returns whether the memory package is part of the known/valid package names
// used by World Partition for PIE/-game streaming.
bool UWorldPartition::IsValidPackageName(const FString& InPackageName)
{
	if (FPackageName::IsMemoryPackage(InPackageName))
	{
		// Remove PIE prefix
		FString PackageName = UWorld::RemovePIEPrefix(InPackageName);
		// Test if package is a valid world partition PIE package
		return GeneratedStreamingPackageNames.Contains(PackageName);
	}
	return false;
}

void UWorldPartition::OnPreBeginPIE(bool bStartSimulate)
{
	check(!bIsPIE);
	bIsPIE = true;

	OnBeginPlay();
}

void UWorldPartition::OnPrePIEEnded(bool bWasSimulatingInEditor)
{
	check(bIsPIE);
	bIsPIE = false;
}

void UWorldPartition::OnBeginPlay()
{
	FGenerateStreamingParams Params;

	TArray<FString> OutGeneratedStreamingPackageNames;
	FGenerateStreamingContext Context = FGenerateStreamingContext()
		.SetPackagesToGenerate((bIsPIE || IsRunningGame()) ? &OutGeneratedStreamingPackageNames : nullptr);

	GenerateStreaming(Params, Context);

	// Prepare GeneratedStreamingPackages
	check(GeneratedStreamingPackageNames.IsEmpty());
	for (const FString& PackageName : OutGeneratedStreamingPackageNames)
	{
		// Set as memory package to avoid wasting time in UWorldPartition::IsValidPackageName (GenerateStreaming for PIE runs on the editor world)
		FString Package = FPaths::RemoveDuplicateSlashes(FPackageName::IsMemoryPackage(PackageName) ? PackageName : TEXT("/Memory/") + PackageName);
		GeneratedStreamingPackageNames.Add(Package);
	}

	RuntimeHash->OnBeginPlay();
}

void UWorldPartition::OnCancelPIE()
{
	// No check here since CancelPIE can be called after PrePIEEnded
	bIsPIE = false;
	// Call OnEndPlay here since EndPlayMapDelegate is not called when cancelling PIE
	OnEndPlay();
}

void UWorldPartition::OnEndPlay()
{
	FlushStreaming();
	RuntimeHash->OnEndPlay();
}

bool UWorldPartition::CanEditChange(const FProperty* InProperty) const
{
	if (!Super::CanEditChange(InProperty))
	{
		return false;
	}

	if (InProperty->GetFName() == GET_MEMBER_NAME_CHECKED(UWorldPartition, ServerStreamingOutMode))
	{
		return bEnableStreaming && (ServerStreamingMode != EWorldPartitionServerStreamingMode::Disabled);
	}
	else if (InProperty->GetFName() == GET_MEMBER_NAME_CHECKED(UWorldPartition, ServerStreamingMode))
	{
		return bEnableStreaming;
	}

	return true;
}

FName UWorldPartition::GetWorldPartitionEditorName() const
{
	if (SupportsStreaming())
	{
		return EditorHash->GetWorldPartitionEditorName();
	}
	return NAME_None;
}
#endif

void UWorldPartition::Initialize(UWorld* InWorld, const FTransform& InTransform)
{
	UE_SCOPED_TIMER(TEXT("WorldPartition initialize"), LogWorldPartition, Display);
	TRACE_CPUPROFILER_EVENT_SCOPE(UWorldPartition::Initialize);

	check(!World || (World == InWorld));
	if (!ensure(!IsInitialized()))
	{
		return;
	}

	if (IsTemplate())
	{
		return;
	}

	check(InWorld);
	World = InWorld;

	if (!InTransform.Equals(FTransform::Identity))
	{
		InstanceTransform = InTransform;
	}

	check(InitState == EWorldPartitionInitState::Uninitialized);
	InitState = EWorldPartitionInitState::Initializing;

	UWorld* OuterWorld = GetTypedOuter<UWorld>();
	check(OuterWorld);

	RegisterDelegates();

	if (IsMainWorldPartition())
	{
		AWorldPartitionReplay::Initialize(World);
	}

#if WITH_EDITOR
	const bool bIsGame = IsRunningGame();
	const bool bIsEditor = !World->IsGameWorld();
	const bool bIsCooking = IsRunningCookCommandlet();
	const bool bIsDedicatedServer = IsRunningDedicatedServer();
	const bool bPIEWorldTravel = (World->WorldType == EWorldType::PIE) && !StreamingPolicy;

	UE_LOG(LogWorldPartition, Log, TEXT("UWorldPartition::Initialize(Asset=%s, IsEditor=%d, bPIEWorldTravel=%d IsGame=%d, IsCooking=%d)"), *OuterWorld->GetName(), bIsEditor ? 1 : 0, bPIEWorldTravel ? 1 : 0, bIsGame ? 1 : 0, bIsCooking ? 1 : 0);

	if (bEnableStreaming)
	{
		bStreamingWasEnabled = true;
	}

	if (bIsGame || bIsCooking)
	{
		// Don't rely on the editor hash for cooking or -game
		EditorHash = nullptr;
		AlwaysLoadedActors = nullptr;
	}
	else if (bIsEditor)
	{
		CreateOrRepairWorldPartition(OuterWorld->GetWorldSettings());

		check(!StreamingPolicy);
		check(EditorHash);

		EditorHash->Initialize();

		AlwaysLoadedActors = new FLoaderAdapterAlwaysLoadedActors(OuterWorld);

		if (IsMainWorldPartition())
		{			
			PinnedActors = new FLoaderAdapterPinnedActors(OuterWorld);
		}
	}

	check(RuntimeHash);
	RuntimeHash->SetFlags(RF_Transactional);

	if (bIsEditor || bIsGame || bPIEWorldTravel || bIsDedicatedServer)
	{
		UPackage* LevelPackage = OuterWorld->PersistentLevel->GetOutermost();

		// Duplicated worlds (ex: WorldPartitionRenameDuplicateBuilder) will not have a loaded path 
		const FName PackageName = LevelPackage->GetLoadedPath().GetPackageFName().IsNone() ? LevelPackage->GetFName() : LevelPackage->GetLoadedPath().GetPackageFName();

		// Currently known Instancing use cases:
		//	- World Partition map template (New Level)
		//	- PIE World Travel
		FString SourceWorldPath, RemappedWorldPath;
		const bool bIsInstanced = OuterWorld->GetSoftObjectPathMapping(SourceWorldPath, RemappedWorldPath);

		// Follow the world's streaming enabled value most of the times, except:
		//	- World is instanced and from a Level Instance that supports partial loading
		const bool bIsStreamingEnabled = bForceEnableStreamingInEditor || IsStreamingEnabled();

		if (bIsInstanced)
		{
			InstancingContext.AddPackageMapping(PackageName, LevelPackage->GetFName());

			// SoftObjectPaths: Specific case for new maps (/Temp/Untitled) where we need to remap the AssetPath and not just the Package name because the World gets renamed (See UWorld::PostLoad)
			InstancingContext.AddPathMapping(
				FSoftObjectPath(*FString::Format(TEXT("{0}.{1}"), {PackageName.ToString(), FPackageName::GetShortName(PackageName)})),
				FSoftObjectPath(OuterWorld)
			);
		}

		FContainerRegistrationParams ContainerInitParams(PackageName);
		ActorDescContainer = RegisterActorDescContainer(ContainerInitParams);

		{
			TRACE_CPUPROFILER_EVENT_SCOPE(UActorDescContainer::Hash);
			for (FActorDescContainerCollection::TIterator<> ActorDescIterator(this); ActorDescIterator; ++ActorDescIterator)
			{
				if (bIsInstanced)
				{
					const FString LongActorPackageName = ActorDescIterator->GetActorPackage().ToString();
					const FString InstancedName = ULevel::GetExternalActorPackageInstanceName(LevelPackage->GetName(), LongActorPackageName);

					InstancingContext.AddPackageMapping(*LongActorPackageName, *InstancedName);

					ActorDescIterator->TransformInstance(SourceWorldPath, RemappedWorldPath);
				}

				ActorDescIterator->bIsForcedNonSpatiallyLoaded = !bIsStreamingEnabled;

				if (bIsEditor && !bIsCooking)
				{
					HashActorDesc(*ActorDescIterator);
				}
			}
		}
	}
#endif

	// Here's it's safe to initialize the DataLayerManager
	DataLayerManager = NewObject<UDataLayerManager>(this, TEXT("DataLayerManager"), RF_Transient);
	DataLayerManager->Initialize();

#if WITH_EDITOR
	if (bIsEditor)
	{
		// Apply level transform on actors already part of the level
		if (!GetInstanceTransform().Equals(FTransform::Identity))
		{
			TRACE_CPUPROFILER_EVENT_SCOPE(ApplyLevelTransform);

			check(!OuterWorld->PersistentLevel->bAlreadyMovedActors);
			for (AActor* Actor : OuterWorld->PersistentLevel->Actors)
			{
				if (Actor)
				{
					FLevelUtils::FApplyLevelTransformParams TransformParams(Actor->GetLevel(), GetInstanceTransform());
					TransformParams.Actor = Actor;
					TransformParams.bDoPostEditMove = true;
					FLevelUtils::ApplyLevelTransform(TransformParams);
				}
			}
			// Flag Level's bAlreadyMovedActors to true so that ULevelStreaming::PrepareLoadedLevel won't reapply the same transform again.
			OuterWorld->PersistentLevel->bAlreadyMovedActors = true;
		}
	}

	if (bIsEditor && !bIsCooking)
	{
		// Load the always loaded cell
		if (AlwaysLoadedActors)
		{
			TRACE_CPUPROFILER_EVENT_SCOPE(LoadAlwaysLoaded);
			AlwaysLoadedActors->Load();
		}

		// Load more cells depending on the user's settings
		// Skipped when running from a commandlet and for subpartitions
		if (IsMainWorldPartition() && IsStreamingEnabled() && !IsRunningCommandlet() && !GIsAutomationTesting)
		{
			// Load last loaded regions
			if (GetMutableDefault<UWorldPartitionEditorPerProjectUserSettings>()->GetEnableLoadingOfLastLoadedRegions())
			{
				TRACE_CPUPROFILER_EVENT_SCOPE(LoadLastLoadedRegions);
				LoadLastLoadedRegions();
			}
		}
	}
#endif //WITH_EDITOR

	InitState = EWorldPartitionInitState::Initialized;

#if WITH_EDITOR
	if (!bIsEditor)
	{
		if (bIsGame || bPIEWorldTravel || bIsDedicatedServer)
		{
			if (bPIEWorldTravel)
			{
				check(!bIsPIE);
				bIsPIE = true;
			}

			if (StreamingPolicy)
			{
				UE_LOG(LogWorldPartition, Warning, TEXT("StreamingPolicy was set when initializing the world partition object"));
				StreamingPolicy = nullptr;
			}

			OnBeginPlay();
		}

		// Apply remapping of Persistent Level's SoftObjectPaths
		// Here we remap SoftObjectPaths so that they are mapped from the PersistentLevel Package to the Cell Packages using the mapping built by the policy
		FWorldPartitionLevelHelper::RemapLevelSoftObjectPaths(OuterWorld->PersistentLevel, this);
	}
#endif

	FWorldPartitionEvents::BroadcastWorldPartitionInitialized(World, this);
}

void UWorldPartition::Uninitialize()
{
	if (IsInitialized())
	{
		check(World);

		InitState = EWorldPartitionInitState::Uninitializing;

		if (IsMainWorldPartition())
		{
			AWorldPartitionReplay::Uninitialize(World);
		}

		UnregisterDelegates();
		
		// Unload all loaded cells
		if (World->IsGameWorld())
		{
			UpdateStreamingState();
		}

#if WITH_EDITOR
		if (IsMainWorldPartition())
		{
			SavePerUserSettings();
		}

		if (World->IsGameWorld())
		{
			OnEndPlay();
		}
		
		if (AlwaysLoadedActors)
		{
			delete AlwaysLoadedActors;
			AlwaysLoadedActors = nullptr;
		}

		if (PinnedActors)
		{
			delete PinnedActors;
			PinnedActors = nullptr;
		}

		if (RegisteredEditorLoaderAdapters.Num())
		{
			for (UWorldPartitionEditorLoaderAdapter* RegisteredEditorLoaderAdapter : RegisteredEditorLoaderAdapters)
			{
				RegisteredEditorLoaderAdapter->Release();
			}

			RegisteredEditorLoaderAdapters.Empty();
		}

		DirtyActors.Empty();

		UninitializeActorDescContainers();
		ActorDescContainer = nullptr;

		EditorHash = nullptr;
		bIsPIE = false;
#endif		

		if (DataLayerManager)
		{
			DataLayerManager->DeInitialize();
			DataLayerManager = nullptr;
		}

		InitState = EWorldPartitionInitState::Uninitialized;

		FWorldPartitionEvents::BroadcastWorldPartitionUninitialized(World, this);

		World = nullptr;
	}
}

UDataLayerManager* UWorldPartition::GetDataLayerManager() const
{
	return DataLayerManager;
}

bool UWorldPartition::IsInitialized() const
{
	return InitState == EWorldPartitionInitState::Initialized;
}

void UWorldPartition::Update()
{
#if WITH_EDITOR
	UWorld* OuterWorld = GetTypedOuter<UWorld>();
	check(OuterWorld);
	check(!OuterWorld->IsInstanced());

	ForEachActorDescContainer([](UActorDescContainer* InActorDescContainer)
	{
		InActorDescContainer->Update();
	});
#endif
}

bool UWorldPartition::SupportsStreaming() const
{
	return World ? World->GetWorldSettings()->SupportsWorldPartitionStreaming() : false;
}

bool UWorldPartition::IsStreamingEnabled() const
{
	return bEnableStreaming && SupportsStreaming();
}

bool UWorldPartition::CanStream() const
{
	if (!IsInitialized())
	{
		return false;
	}

	const ULevel* PersistentLevel = GetTypedOuter<UWorld>()->PersistentLevel;
	// Is it a level streamed World Partition that was removed from its owning world
	// or is the World requesting unloading of all streaming levels.
	if (!PersistentLevel->GetWorld() || PersistentLevel->GetWorld()->GetShouldForceUnloadStreamingLevels())
	{
		return false;
	}
		
	// Is it part of a Sub-level that should be visible.
	if (ULevelStreaming* LevelStreaming = ULevelStreaming::FindStreamingLevel(PersistentLevel))
	{
		return !LevelStreaming->GetIsRequestingUnloadAndRemoval() && LevelStreaming->ShouldBeVisible();
	}

	return true;
}

bool UWorldPartition::IsMainWorldPartition() const
{
	check(World);
	return World == GetTypedOuter<UWorld>();
}

void UWorldPartition::OnPostBugItGoCalled(const FVector& Loc, const FRotator& Rot)
{
#if WITH_EDITOR
	if (GetMutableDefault<UWorldPartitionEditorPerProjectUserSettings>()->GetBugItGoLoadRegion())
	{
		const FVector LoadExtent(UWorldPartition::LoadingRangeBugItGo, UWorldPartition::LoadingRangeBugItGo, HALF_WORLD_MAX);
		const FBox LoadCellsBox(Loc - LoadExtent, Loc + LoadExtent);

		IWorldPartitionEditorModule& WorldPartitionEditorModule = FModuleManager::LoadModuleChecked<IWorldPartitionEditorModule>("WorldPartitionEditor");
		UWorldPartitionEditorLoaderAdapter* EditorLoaderAdapter = CreateEditorLoaderAdapter<FLoaderAdapterShape>(World, LoadCellsBox, TEXT("BugItGo"));
		EditorLoaderAdapter->GetLoaderAdapter()->Load();

		if (WorldPartitionEditor)
		{
			WorldPartitionEditor->FocusBox(LoadCellsBox);
		}
	}
#endif
}

void UWorldPartition::RegisterDelegates()
{
	check(World); 

#if WITH_EDITOR
	if (GEditor && !IsTemplate() && !World->IsGameWorld())
	{
		if (IsMainWorldPartition())
		{
			FEditorDelegates::PreBeginPIE.AddUObject(this, &UWorldPartition::OnPreBeginPIE);
			FEditorDelegates::PrePIEEnded.AddUObject(this, &UWorldPartition::OnPrePIEEnded);
			FEditorDelegates::CancelPIE.AddUObject(this, &UWorldPartition::OnCancelPIE);
			FGameDelegates::Get().GetEndPlayMapDelegate().AddUObject(this, &UWorldPartition::OnEndPlay);
			FCoreUObjectDelegates::PostReachabilityAnalysis.AddUObject(this, &UWorldPartition::OnGCPostReachabilityAnalysis);
			GEditor->OnPostBugItGoCalled().AddUObject(this, &UWorldPartition::OnPostBugItGoCalled);
			GEditor->OnEditorClose().AddUObject(this, &UWorldPartition::SavePerUserSettings);
			FWorldDelegates::OnPostWorldRename.AddUObject(this, &UWorldPartition::OnWorldRenamed);

			if (!IsRunningCommandlet())
			{
				UPackage::PackageDirtyStateChangedEvent.AddUObject(this, &UWorldPartition::OnPackageDirtyStateChanged);
			}
		}
	}
#endif

	if (World->IsGameWorld())
	{
		if (IsMainWorldPartition())
		{
			World->OnWorldMatchStarting.AddUObject(this, &UWorldPartition::OnWorldMatchStarting);

#if !UE_BUILD_SHIPPING
			FCoreDelegates::OnGetOnScreenMessages.AddUObject(this, &UWorldPartition::GetOnScreenMessages);
#endif
		}
		else
		{
			FWorldDelegates::LevelRemovedFromWorld.AddUObject(this, &UWorldPartition::OnLevelRemovedFromWorld);
		}
	}
}

void UWorldPartition::UnregisterDelegates()
{
	check(World);

#if WITH_EDITOR
	if (GEditor && !IsTemplate() && !World->IsGameWorld())
	{
		if (IsMainWorldPartition())
		{
			FWorldDelegates::OnPostWorldRename.RemoveAll(this);
			FEditorDelegates::PreBeginPIE.RemoveAll(this);
			FEditorDelegates::PrePIEEnded.RemoveAll(this);
			FEditorDelegates::CancelPIE.RemoveAll(this);
			FGameDelegates::Get().GetEndPlayMapDelegate().RemoveAll(this);

			if (!IsEngineExitRequested())
			{
				FCoreUObjectDelegates::PostReachabilityAnalysis.RemoveAll(this);
			}

			GEditor->OnPostBugItGoCalled().RemoveAll(this);
			GEditor->OnEditorClose().RemoveAll(this);

			if (!IsRunningCommandlet())
			{
				UPackage::PackageDirtyStateChangedEvent.RemoveAll(this);
			}
		}
	}
#endif

	if (World->IsGameWorld())
	{
		if (IsMainWorldPartition())
		{
			World->OnWorldMatchStarting.RemoveAll(this);

#if !UE_BUILD_SHIPPING
			FCoreDelegates::OnGetOnScreenMessages.RemoveAll(this);
#endif
		}
		else
		{
			FWorldDelegates::LevelRemovedFromWorld.RemoveAll(this);
		}
	}
}

void UWorldPartition::OnLevelRemovedFromWorld(ULevel* InLevel, UWorld* InWorld)
{
	check(!IsMainWorldPartition());
	if ((World == InWorld) && (InLevel == GetTypedOuter<UWorld>()->PersistentLevel))
	{
		check(!CanStream());
		Uninitialize();
	}
}

#if !UE_BUILD_SHIPPING
void UWorldPartition::GetOnScreenMessages(FCoreDelegates::FSeverityMessageMap& OutMessages)
{
	if (StreamingPolicy)
	{
		StreamingPolicy->GetOnScreenMessages(OutMessages);
	}
}
#endif

void UWorldPartition::OnWorldMatchStarting()
{
	check(GetWorld()->IsGameWorld());
	// Wait for any level streaming to complete
	GetWorld()->BlockTillLevelStreamingCompleted();
}

#if WITH_EDITOR
UWorldPartition* UWorldPartition::CreateOrRepairWorldPartition(AWorldSettings* WorldSettings, TSubclassOf<UWorldPartitionEditorHash> EditorHashClass, TSubclassOf<UWorldPartitionRuntimeHash> RuntimeHashClass)
{
	UWorld* OuterWorld = WorldSettings->GetTypedOuter<UWorld>();
	UWorldPartition* WorldPartition = WorldSettings->GetWorldPartition();

	if (!WorldPartition)
	{
		WorldPartition = NewObject<UWorldPartition>(WorldSettings);
		WorldSettings->SetWorldPartition(WorldPartition);

		// New maps should include GridSize in name
		WorldSettings->bIncludeGridSizeInNameForFoliageActors = true;
		WorldSettings->bIncludeGridSizeInNameForPartitionedActors = true;

		if (IWorldPartitionEditorModule* WorldPartitionEditorModulePtr = FModuleManager::GetModulePtr<IWorldPartitionEditorModule>("WorldPartitionEditor"))
		{
			WorldSettings->InstancedFoliageGridSize = WorldPartitionEditorModulePtr->GetInstancedFoliageGridSize();
			WorldSettings->DefaultPlacementGridSize = WorldPartitionEditorModulePtr->GetPlacementGridSize();
		}

		WorldSettings->MarkPackageDirty();

		WorldPartition->DefaultHLODLayer = UHLODLayer::GetEngineDefaultHLODLayersSetup();

		AWorldDataLayers* WorldDataLayers = OuterWorld->GetWorldDataLayers();
		if (!WorldDataLayers)
		{
			WorldDataLayers = AWorldDataLayers::Create(OuterWorld);
			OuterWorld->SetWorldDataLayers(WorldDataLayers);
		}

		FWorldPartitionMiniMapHelper::GetWorldPartitionMiniMap(OuterWorld, true);
	}

	if (!WorldPartition->EditorHash)
	{
		if (!EditorHashClass)
		{
			EditorHashClass = FindObject<UClass>(nullptr, TEXT("/Script/Engine.WorldPartitionEditorSpatialHash"));
		}

		WorldPartition->EditorHash = NewObject<UWorldPartitionEditorHash>(WorldPartition, EditorHashClass);
		WorldPartition->EditorHash->SetDefaultValues();
	}

	if (!WorldPartition->RuntimeHash)
	{
		if (!RuntimeHashClass)
		{
			RuntimeHashClass = FindObject<UClass>(nullptr, TEXT("/Script/Engine.WorldPartitionRuntimeSpatialHash"));
		}

		WorldPartition->RuntimeHash = NewObject<UWorldPartitionRuntimeHash>(WorldPartition, RuntimeHashClass, NAME_None, RF_Transactional);
		WorldPartition->RuntimeHash->SetDefaultValues();
	}

	OuterWorld->PersistentLevel->bIsPartitioned = true;

	return WorldPartition;
}

bool UWorldPartition::RemoveWorldPartition(AWorldSettings* WorldSettings)
{
	if (UWorldPartition* WorldPartition = WorldSettings->GetWorldPartition())
	{
		if (!WorldPartition->IsStreamingEnabled())
		{
			FWorldPartitionLoadingContext::FNull LoadingContext;

			WorldSettings->Modify();
			
			ULevel* PersistentLevel = WorldSettings->GetLevel();
			for (AActor* Actor : PersistentLevel->Actors)
			{
				if (Actor && (Cast<AWorldDataLayers>(Actor) || Cast<AWorldPartitionMiniMap>(Actor)))
				{
					Actor->Destroy();
				}
			}

			WorldPartition->Uninitialize();
			WorldSettings->SetWorldPartition(nullptr);
			PersistentLevel->bIsPartitioned = false;

			if (WorldPartition->WorldPartitionEditor)
			{
				WorldPartition->WorldPartitionEditor->Reconstruct();
			}
			
			return true;
		}
	}
	return false;
}
#endif

const TArray<FWorldPartitionStreamingSource>& UWorldPartition::GetStreamingSources() const
{
	if (StreamingPolicy && GetWorld()->IsGameWorld())
	{
		return StreamingPolicy->GetStreamingSources();
	}

	static TArray<FWorldPartitionStreamingSource> EmptyStreamingSources;
	return EmptyStreamingSources;
}

bool UWorldPartition::IsServer() const
{
	if (UWorld* OwningWorld = GetWorld())
	{
		const ENetMode NetMode = OwningWorld->GetNetMode();
		return NetMode == NM_DedicatedServer || NetMode == NM_ListenServer;
	}
	return false;
}

bool UWorldPartition::IsServerStreamingEnabled() const
{
	// Resolve once (we don't allow changing the state at runtime)
	if (!bCachedIsServerStreamingEnabled.IsSet())
	{
		bool bIsEnabled = false;
		if (ServerStreamingMode == EWorldPartitionServerStreamingMode::ProjectDefault)
		{
			switch (UWorldPartition::GlobalEnableServerStreaming)
			{
			case 1:	bIsEnabled = true;
#if WITH_EDITOR
			case 2: bIsEnabled = bIsPIE;
#endif
			}
		}
		else
		{
			if ((ServerStreamingMode == EWorldPartitionServerStreamingMode::Enabled)
#if WITH_EDITOR
				|| (bIsPIE && (ServerStreamingMode == EWorldPartitionServerStreamingMode::EnabledInPIE))
#endif
				)
			{
				bIsEnabled = true;
			}
		}

		UWorld* OwningWorld = GetWorld();
		bCachedIsServerStreamingEnabled = (OwningWorld && OwningWorld->IsGameWorld() && bIsEnabled);
	}
	
	return bCachedIsServerStreamingEnabled.Get(false);
}

bool UWorldPartition::IsServerStreamingOutEnabled() const
{
	// Resolve once (we don't allow changing the state at runtime)
	if (!bCachedIsServerStreamingOutEnabled.IsSet())
	{
		UWorld* OwningWorld = GetWorld();
		const bool bEnableServerStreamingOut = (ServerStreamingOutMode == EWorldPartitionServerStreamingOutMode::ProjectDefault) ? UWorldPartition::bGlobalEnableServerStreamingOut : (ServerStreamingOutMode == EWorldPartitionServerStreamingOutMode::Enabled);
		bCachedIsServerStreamingOutEnabled = OwningWorld && OwningWorld->IsGameWorld() && IsServerStreamingEnabled() && bEnableServerStreamingOut;
	}

	return bCachedIsServerStreamingOutEnabled.Get(false);
}

bool UWorldPartition::UseMakingVisibleTransactionRequests() const
{
	// Resolve once (we don't allow changing the state at runtime)
	if (!bCachedUseMakingVisibleTransactionRequests.IsSet())
	{
		UWorld* OwningWorld = GetWorld();
		bCachedUseMakingVisibleTransactionRequests = OwningWorld && OwningWorld->IsGameWorld() && UWorldPartition::bUseMakingVisibleTransactionRequests;
	}
	return bCachedUseMakingVisibleTransactionRequests.Get(false);
}

bool UWorldPartition::UseMakingInvisibleTransactionRequests() const
{
	// Resolve once (we don't allow changing the state at runtime)
	if (!bCachedUseMakingInvisibleTransactionRequests.IsSet())
	{
		UWorld* OwningWorld = GetWorld();
		bCachedUseMakingInvisibleTransactionRequests = OwningWorld && OwningWorld->IsGameWorld() && UWorldPartition::bUseMakingInvisibleTransactionRequests;
	}
	return bCachedUseMakingInvisibleTransactionRequests.Get(false);
}

bool UWorldPartition::IsSimulating(bool bIncludeTestEnableSimulationStreamingSource)
{
#if WITH_EDITOR
	return GEditor && GEditor->bIsSimulatingInEditor && GCurrentLevelEditingViewportClient && GCurrentLevelEditingViewportClient->IsSimulateInEditorViewport() && (!bIncludeTestEnableSimulationStreamingSource || UWorldPartition::EnableSimulationStreamingSource);
#else
	return false;
#endif
}

#if WITH_EDITOR
void UWorldPartition::OnActorDescAdded(FWorldPartitionActorDesc* NewActorDesc)
{
	NewActorDesc->bIsForcedNonSpatiallyLoaded = !IsStreamingEnabled();

	HashActorDesc(NewActorDesc);

	if (AActor* NewActor = NewActorDesc->GetActor())
	{
		DirtyActors.Add(FWorldPartitionReference(NewActorDesc->GetContainer(), NewActorDesc->GetGuid()), NewActor);
	}

	if (WorldPartitionEditor)
	{
		WorldPartitionEditor->Refresh();
	}
}

void UWorldPartition::OnActorDescRemoved(FWorldPartitionActorDesc* ActorDesc)
{	
	UnhashActorDesc(ActorDesc);

	if (WorldPartitionEditor)
	{
		WorldPartitionEditor->Refresh();
	}
}

void UWorldPartition::OnActorDescUpdating(FWorldPartitionActorDesc* ActorDesc)
{
	UnhashActorDesc(ActorDesc);
}

void UWorldPartition::OnActorDescUpdated(FWorldPartitionActorDesc* ActorDesc)
{
	HashActorDesc(ActorDesc);

	if (WorldPartitionEditor)
	{
		WorldPartitionEditor->Refresh();
	}
}

bool UWorldPartition::GetInstancingContext(const FLinkerInstancingContext*& OutInstancingContext) const
{
	if (InstancingContext.IsInstanced())
	{
		OutInstancingContext = &InstancingContext;
		return true;
	}
	return false;
}
#endif

const FTransform& UWorldPartition::GetInstanceTransform() const
{
	return InstanceTransform.IsSet() ? InstanceTransform.GetValue() : FTransform::Identity;
}

#if WITH_EDITOR
void UWorldPartition::SetEnableStreaming(bool bInEnableStreaming)
{
	if (bEnableStreaming != bInEnableStreaming)
	{
		const FScopedTransaction Transaction(LOCTEXT("EditorWorldPartitionSetEnableStreaming", "Set WorldPartition EnableStreaming"));

		SetFlags(RF_Transactional);
		Modify();
		bEnableStreaming = bInEnableStreaming;
		OnEnableStreamingChanged();
	}
}

bool UWorldPartition::CanBeUsedByLevelInstance() const
{
	return bCanBeUsedByLevelInstance && !IsStreamingEnabled();
}

void UWorldPartition::SetCanBeUsedByLevelInstance(bool bInCanBeUsedByLevelInstance)
{
	if (bCanBeUsedByLevelInstance != bInCanBeUsedByLevelInstance)
	{
		const FScopedTransaction Transaction(LOCTEXT("EditorWorldPartitionCanBeUsedByLevelInstance", "Set WorldPartition CanBeUsedByLevelInstance"));

		SetFlags(RF_Transactional);
		Modify();
		bCanBeUsedByLevelInstance = bInCanBeUsedByLevelInstance;
		if (bCanBeUsedByLevelInstance)
		{
			bEnableStreaming = false;
		}
	}
}

void UWorldPartition::OnEnableStreamingChanged()
{
	for (FActorDescContainerCollection::TIterator<> ActorDescIterator(this); ActorDescIterator; ++ActorDescIterator)
	{
		UnhashActorDesc(*ActorDescIterator);
		ActorDescIterator->bIsForcedNonSpatiallyLoaded = !IsStreamingEnabled();
		HashActorDesc(*ActorDescIterator);
	}

	FLoaderAdapterAlwaysLoadedActors* OldAlwaysLoadedActors = AlwaysLoadedActors;

	AlwaysLoadedActors = new FLoaderAdapterAlwaysLoadedActors(GetTypedOuter<UWorld>());
	AlwaysLoadedActors->Load();

	OldAlwaysLoadedActors->Unload();
	delete OldAlwaysLoadedActors;

	if (WorldPartitionEditor)
	{
		WorldPartitionEditor->Reconstruct();
	}
}

void UWorldPartition::HashActorDesc(FWorldPartitionActorDesc* ActorDesc)
{
	check(ActorDesc);
	check(EditorHash);

	FWorldPartitionHandle ActorHandle(this, ActorDesc->GetGuid());
	EditorHash->HashActor(ActorHandle);

	bShouldCheckEnableStreamingWarning = IsMainWorldPartition();
}

void UWorldPartition::UnhashActorDesc(FWorldPartitionActorDesc* ActorDesc)
{
	check(ActorDesc);
	check(EditorHash);

	FWorldPartitionHandle ActorHandle(this, ActorDesc->GetGuid());
	EditorHash->UnhashActor(ActorHandle);
}
#endif

void UWorldPartition::Serialize(FArchive& Ar)
{
	Ar.UsingCustomVersion(FUE5MainStreamObjectVersion::GUID);

	Super::Serialize(Ar);

	if (Ar.GetPortFlags() & PPF_DuplicateForPIE)
	{
		Ar << StreamingPolicy;

#if WITH_EDITORONLY_DATA
		Ar << GeneratedStreamingPackageNames;
#endif

#if WITH_EDITOR
		Ar << bIsPIE;
#endif
	}
	else if (Ar.CustomVer(FUE5MainStreamObjectVersion::GUID) >= FUE5MainStreamObjectVersion::WorldPartitionSerializeStreamingPolicyOnCook)
	{
		bool bCooked = Ar.IsCooking();
		Ar << bCooked;

		if (bCooked)
		{
			Ar << StreamingPolicy;
		}
	}
}

UWorld* UWorldPartition::GetWorld() const
{
	if (World)
	{
		return World;
	}
	return Super::GetWorld();
}

bool UWorldPartition::ResolveSubobject(const TCHAR* SubObjectPath, UObject*& OutObject, bool bLoadIfExists)
{
	if (GetWorld())
	{
		if (GetWorld()->IsGameWorld())
		{
			if (StreamingPolicy)
			{
				if (UObject* SubObject = StreamingPolicy->GetSubObject(SubObjectPath))
				{
					OutObject = SubObject;
					return true;
				}
				else
				{
					OutObject = nullptr;
				}
			}
		}
#if WITH_EDITOR
		else
		{
			// Support for subobjects such as Actor.Component
			FString SubObjectName;
			FString SubObjectContext;	
			if (!FString(SubObjectPath).Split(TEXT("."), &SubObjectContext, &SubObjectName))
			{
				SubObjectName = SubObjectPath;
			}

			if (const FWorldPartitionActorDesc* ActorDesc = GetActorDescByName(SubObjectName))
			{
				if (bLoadIfExists)
				{
					LoadedSubobjects.Emplace(this, ActorDesc->GetGuid());
				}

				OutObject = StaticFindObject(UObject::StaticClass(), GetWorld()->PersistentLevel, SubObjectPath);
				return true;
			}
		}
#endif
	}

	return false;
}

void UWorldPartition::BeginDestroy()
{
	check(InitState == EWorldPartitionInitState::Uninitialized);
	Super::BeginDestroy();
}

void UWorldPartition::AddReferencedObjects(UObject* InThis, FReferenceCollector& Collector)
{
#if WITH_EDITOR
	UWorldPartition* This = CastChecked<UWorldPartition>(InThis);

	// We need to keep all dirty actors alive, mainly for deleted actors. Normally, these actors are only referenced
	// by the transaction buffer, but we clear it when unloading regions, etc. and we don't want these actors to die.
	// Also, we must avoid reporting these references when not collecting garbage, as code such as package deletion
	// will skip packages with actors still referenced (via GatherObjectReferencersForDeletion).
	if (IsGarbageCollecting())
	{
		Collector.AllowEliminatingReferences(false);
		for (auto& [ActorReference, Actor] : This->DirtyActors)
		{
			Collector.AddReferencedObject(Actor);
		}
		Collector.AllowEliminatingReferences(true);
	}

	for (const UActorDescContainer* Container : This->ActorDescContainerCollection)
	{
		Collector.AddReferencedObject(Container);
	}
#endif

	Super::AddReferencedObjects(InThis, Collector);
}

void UWorldPartition::Tick(float DeltaSeconds)
{
#if WITH_EDITOR
	if (EditorHash)
	{
		EditorHash->Tick(DeltaSeconds);
	}

	if (PinnedActors)
	{
		for (TMap<FWorldPartitionReference, AActor*>::TIterator DirtyActorIt(DirtyActors); DirtyActorIt; ++DirtyActorIt)
		{
			if (!DirtyActorIt.Key().IsValid() || !DirtyActorIt.Value()->GetPackage()->IsDirty())
			{
				// If we hold the last reference to that actor (or no reference are held at all), pin it to avoid unloading
				if (DirtyActorIt.Key().IsValid() && DirtyActorIt.Key()->GetHardRefCount() <= 1)
				{
					PinnedActors->AddActors({ DirtyActorIt.Key().ToHandle() });
				}

				DirtyActorIt.RemoveCurrent();
			}
		}
	}

	if (bForceGarbageCollection)
	{
		GEngine->ForceGarbageCollection(bForceGarbageCollectionPurge);

		bForceGarbageCollection = false;
		bForceGarbageCollectionPurge = false;
	}

	if (bShouldCheckEnableStreamingWarning)
	{
		bShouldCheckEnableStreamingWarning = false;

		if (!IsStreamingEnabled() && SupportsStreaming())
		{
			bEnablingStreamingJustified = false;

			FBox AllActorsBounds(ForceInit);
			for (FActorDescContainerCollection::TConstIterator<> ActorDescIterator(this); ActorDescIterator; ++ActorDescIterator)
			{
				if (ActorDescIterator->GetIsSpatiallyLoadedRaw() || ActorDescIterator->GetActorNativeClass()->IsChildOf<ALandscapeProxy>())
				{
					const FBox EditorBounds = ActorDescIterator->GetEditorBounds();
					if (EditorBounds.IsValid)
					{
						AllActorsBounds += EditorBounds;

						// Warn the user if the world becomes larger that 4km in any axis
						if (AllActorsBounds.GetSize().GetMax() >= UWorldPartition::WorldExtentToEnableStreaming)
						{
							bEnablingStreamingJustified = true;
							break;
						}
					}
				}
			}
		}
	}
#endif
}

DECLARE_CYCLE_STAT(TEXT("World Partition Update Streaming"), STAT_WorldPartitionUpdateStreaming, STATGROUP_Engine);

void UWorldPartition::UpdateStreamingState()
{
	SCOPE_CYCLE_COUNTER(STAT_WorldPartitionUpdateStreaming);

	if (GetWorld()->IsGameWorld())
	{
		if (StreamingPolicy)
		{
			StreamingPolicy->UpdateStreamingState();
		}
	}
}

bool UWorldPartition::InjectExternalStreamingObject(URuntimeHashExternalStreamingObjectBase* ExternalStreamingObject)
{
	bool bInjected = RuntimeHash->InjectExternalStreamingObject(ExternalStreamingObject);
	if (bInjected)
	{
		StreamingPolicy->InjectExternalStreamingObject(ExternalStreamingObject);
		GetWorld()->GetSubsystem<UHLODSubsystem>()->OnExternalStreamingObjectInjected(ExternalStreamingObject);
	}

	return bInjected;
}

bool UWorldPartition::RemoveExternalStreamingObject(URuntimeHashExternalStreamingObjectBase* ExternalStreamingObject)
{
	bool bRemoved = RuntimeHash->RemoveExternalStreamingObject(ExternalStreamingObject);
	if (bRemoved)
	{
		if (StreamingPolicy)
		{
			StreamingPolicy->RemoveExternalStreamingObject(ExternalStreamingObject);
		}
		
		GetWorld()->GetSubsystem<UHLODSubsystem>()->OnExternalStreamingObjectRemoved(ExternalStreamingObject);
	}

	return bRemoved;
}

bool UWorldPartition::GetIntersectingCells(const TArray<FWorldPartitionStreamingQuerySource>& InSources, TArray<const IWorldPartitionCell*>& OutCells) const
{
	if (StreamingPolicy)
	{
		return StreamingPolicy->GetIntersectingCells(InSources, OutCells);
	}
	return false;
}

bool UWorldPartition::CanAddLoadedLevelToWorld(class ULevel* InLevel) const
{
	if (GetWorld()->IsGameWorld() && StreamingPolicy)
	{
		return StreamingPolicy->CanAddLoadedLevelToWorld(InLevel);
	}
	return true;
}

bool UWorldPartition::IsStreamingCompleted(const TArray<FWorldPartitionStreamingSource>* InStreamingSources) const
{
	if (GetWorld()->IsGameWorld() && StreamingPolicy)
	{
		return StreamingPolicy->IsStreamingCompleted(InStreamingSources);
	}
	return true;
}

bool UWorldPartition::IsStreamingCompleted(EWorldPartitionRuntimeCellState QueryState, const TArray<FWorldPartitionStreamingQuerySource>& QuerySources, bool bExactState) const
{
	if (GetWorld()->IsGameWorld() && StreamingPolicy)
	{
		return StreamingPolicy->IsStreamingCompleted(QueryState, QuerySources, bExactState);
	}

	return true;
}

void UWorldPartition::OnCellShown(const UWorldPartitionRuntimeCell* InCell)
{
	check(IsInitialized());
	// Discard Cell's LevelStreaming notification when once WorldPartition is unitialized (can happen for instanced WorldPartition)
	if (GetWorld()->IsGameWorld())
	{
		if (IsStreamingEnabled())
		{
			GetWorld()->GetSubsystem<UHLODSubsystem>()->OnCellShown(InCell);
		}
		StreamingPolicy->OnCellShown(InCell);
	}
}

void UWorldPartition::OnCellHidden(const UWorldPartitionRuntimeCell* InCell)
{
	check(IsInitialized());
	// Discard Cell's LevelStreaming notification when once WorldPartition is unitialized (can happen for instanced WorldPartition)
	if (GetWorld()->IsGameWorld())
	{
		if (IsStreamingEnabled())
		{
			GetWorld()->GetSubsystem<UHLODSubsystem>()->OnCellHidden(InCell);
		}
		StreamingPolicy->OnCellHidden(InCell);
	}
}

bool UWorldPartition::DrawRuntimeHash2D(FWorldPartitionDraw2DContext& DrawContext)
{
	return StreamingPolicy->DrawRuntimeHash2D(DrawContext);
}

void UWorldPartition::DrawRuntimeHash3D()
{
	StreamingPolicy->DrawRuntimeHash3D();
}

void UWorldPartition::DrawRuntimeCellsDetails(class UCanvas* Canvas, FVector2D& Offset)
{
	StreamingPolicy->DrawRuntimeCellsDetails(Canvas, Offset);
}

EWorldPartitionStreamingPerformance UWorldPartition::GetStreamingPerformance() const
{
	return StreamingPolicy->GetStreamingPerformance();
}

bool UWorldPartition::IsStreamingInEnabled() const
{
	return bStreamingInEnabled;
}

void UWorldPartition::DisableStreamingIn()
{
	check(bStreamingInEnabled);
	bStreamingInEnabled = false;
}

void UWorldPartition::EnableStreamingIn()
{
	check(!bStreamingInEnabled);
	bStreamingInEnabled = true;
}

bool UWorldPartition::ConvertEditorPathToRuntimePath(const FSoftObjectPath& InPath, FSoftObjectPath& OutPath) const
{
	return StreamingPolicy ? StreamingPolicy->ConvertEditorPathToRuntimePath(InPath, OutPath) : false;
}

#if WITH_EDITOR
void UWorldPartition::DrawRuntimeHashPreview()
{
	RuntimeHash->DrawPreview();
}

void UWorldPartition::BeginCook(IWorldPartitionCookPackageContext& CookContext)
{
	OnBeginCook.Broadcast(CookContext);

	CookContext.RegisterPackageCookPackageGenerator(this);
}

bool UWorldPartition::GatherPackagesToCook(IWorldPartitionCookPackageContext& CookContext)
{
	FGenerateStreamingParams Params = FGenerateStreamingParams()
		.SetActorDescContainer(ActorDescContainer);

	TArray<FString> PackagesToCook;
	FGenerateStreamingContext Context = FGenerateStreamingContext()
		.SetPackagesToGenerate(&PackagesToCook);

	if (GenerateContainerStreaming(Params, Context))
	{
		FString PackageName = GetPackage()->GetName();
		for (const FString& PackageToCook : PackagesToCook)
		{
			CookContext.AddLevelStreamingPackageToGenerate(this, PackageName, PackageToCook);
		}
	
		return true;
	}

	return false;
}

bool UWorldPartition::PrepareGeneratorPackageForCook(IWorldPartitionCookPackageContext& CookContext, TArray<UPackage*>& OutModifiedPackages)
{
	check(RuntimeHash);
	return RuntimeHash->PrepareGeneratorPackageForCook(OutModifiedPackages);
}

bool UWorldPartition::PopulateGeneratorPackageForCook(IWorldPartitionCookPackageContext& CookContext, const TArray<FWorldPartitionCookPackage*>& InPackagesToCook, TArray<UPackage*>& OutModifiedPackages)
{
	check(RuntimeHash);
	return RuntimeHash->PopulateGeneratorPackageForCook(InPackagesToCook, OutModifiedPackages);
}

bool UWorldPartition::PopulateGeneratedPackageForCook(IWorldPartitionCookPackageContext& CookContext, const FWorldPartitionCookPackage& InPackagesToCook, TArray<UPackage*>& OutModifiedPackages)
{
	check(RuntimeHash);
	return RuntimeHash->PopulateGeneratedPackageForCook(InPackagesToCook, OutModifiedPackages);
}

UWorldPartitionRuntimeCell* UWorldPartition::GetCellForPackage(const FWorldPartitionCookPackage& PackageToCook) const
{
	check(RuntimeHash);
	return RuntimeHash->GetCellForPackage(PackageToCook);
}

TArray<FBox> UWorldPartition::GetUserLoadedEditorRegions() const
{
	TArray<FBox> Result;

	for (UWorldPartitionEditorLoaderAdapter* EditorLoaderAdapter : RegisteredEditorLoaderAdapters)
	{
		IWorldPartitionActorLoaderInterface::ILoaderAdapter* LoaderAdapter = EditorLoaderAdapter->GetLoaderAdapter();
		check(LoaderAdapter);
		if (LoaderAdapter->IsLoaded() && LoaderAdapter->GetUserCreated())
		{
			Result.Add(*LoaderAdapter->GetBoundingBox());
		}
	}

	return Result;
}

void UWorldPartition::SavePerUserSettings()
{
	check(IsMainWorldPartition());

	if (GIsEditor && !World->IsGameWorld() && !IsRunningCommandlet() && !IsEngineExitRequested())
	{
		GetMutableDefault<UWorldPartitionEditorPerProjectUserSettings>()->SetEditorLoadedRegions(GetWorld(), GetUserLoadedEditorRegions());

		TArray<FName> EditorLoadedLocationVolumes;
		for (FActorDescContainerCollection::TConstIterator<> ActorDescIterator(this); ActorDescIterator; ++ActorDescIterator)
		{
			if (ALocationVolume* LocationVolume = Cast<ALocationVolume>(ActorDescIterator->GetActor()); IsValid(LocationVolume))
			{
				check(LocationVolume->GetClass()->ImplementsInterface(UWorldPartitionActorLoaderInterface::StaticClass()));

				IWorldPartitionActorLoaderInterface::ILoaderAdapter* LoaderAdapter = Cast<IWorldPartitionActorLoaderInterface>(LocationVolume)->GetLoaderAdapter();
				check(LoaderAdapter);

				if (LoaderAdapter->IsLoaded() && LoaderAdapter->GetUserCreated())
				{
					EditorLoadedLocationVolumes.Add(LocationVolume->GetFName());
				}
			}
		}
		GetMutableDefault<UWorldPartitionEditorPerProjectUserSettings>()->SetEditorLoadedLocationVolumes(GetWorld(), EditorLoadedLocationVolumes);
	}
}

void UWorldPartition::DumpActorDescs(const FString& Path)
{
	if (FArchive* LogFile = IFileManager::Get().CreateFileWriter(*Path))
	{
		TArray<const FWorldPartitionActorDesc*> ActorDescs;
		TMap<FName, FString> DataLayersDumpString = GetDataLayersDumpString(this);
		for (FActorDescContainerCollection::TConstIterator<> ActorDescIterator(this); ActorDescIterator; ++ActorDescIterator)
		{
			ActorDescs.Add(*ActorDescIterator);
		}
		ActorDescs.Sort([](const FWorldPartitionActorDesc& A, const FWorldPartitionActorDesc& B)
		{
			return A.GetGuid() < B.GetGuid();
		});
		for (const FWorldPartitionActorDesc* ActorDescIterator : ActorDescs)
		{
			FString LineEntry = GetActorDescDumpString(ActorDescIterator, DataLayersDumpString);
			LogFile->Serialize(TCHAR_TO_ANSI(*LineEntry), LineEntry.Len());
		}

		LogFile->Close();
		delete LogFile;
	}
}

void UWorldPartition::AppendAssetRegistryTags(TArray<FAssetRegistryTag>& OutTags) const
{
	static const FName NAME_LevelIsPartitioned(TEXT("LevelIsPartitioned"));
	OutTags.Add(FAssetRegistryTag(NAME_LevelIsPartitioned, TEXT("1"), FAssetRegistryTag::TT_Hidden));

	if (!IsStreamingEnabled())
	{
		static const FName NAME_LevelHasStreamingDisabled(TEXT("LevelHasStreamingDisabled"));
		OutTags.Add(FAssetRegistryTag(NAME_LevelHasStreamingDisabled, TEXT("1"), FAssetRegistryTag::TT_Hidden));
	}

	if (CanBeUsedByLevelInstance())
	{
		static const FName NAME_PartitionedLevelCanBeUsedByLevelInstance(TEXT("PartitionedLevelCanBeUsedByLevelInstance"));
		OutTags.Add(FAssetRegistryTag(NAME_PartitionedLevelCanBeUsedByLevelInstance, TEXT("1"), FAssetRegistryTag::TT_Hidden));
	}

	// Append level script references so we can perform changelists validations without loading the world
	if (const ULevelScriptBlueprint* LevelScriptBlueprint = GetWorld()->PersistentLevel->GetLevelScriptBlueprint(true))
	{
		TArray<AActor*> LevelScriptExternalActorReferences = ActorsReferencesUtils::GetExternalActorReferences((UObject*)LevelScriptBlueprint);
		
		if (LevelScriptExternalActorReferences.Num())
		{
			FStringBuilderBase StringBuilder;
			for (const AActor* Actor : LevelScriptExternalActorReferences)
			{
				StringBuilder.Append(Actor->GetActorGuid().ToString(EGuidFormats::Short));
				StringBuilder.AppendChar(TEXT(','));
			}
			StringBuilder.RemoveSuffix(1);

			static const FName NAME_LevelScriptExternalActorsReferences(TEXT("LevelScriptExternalActorsReferences"));
			OutTags.Add(FAssetRegistryTag(NAME_LevelScriptExternalActorsReferences, StringBuilder.ToString(), FAssetRegistryTag::TT_Hidden));
		}
	}
}

UActorDescContainer* UWorldPartition::RegisterActorDescContainer(const FContainerRegistrationParams& InRegistrationParameters)
{
	if (!Contains(InRegistrationParameters.PackageName))
	{	
		UActorDescContainer::FInitializeParams ContainerInitParams(GetWorld(), InRegistrationParameters.PackageName);

		const FWorldDataLayersActorDesc* WorldDataLayerActorsDesc = nullptr;
		ContainerInitParams.FilterActorDesc = [this, &WorldDataLayerActorsDesc, &InRegistrationParameters](const FWorldPartitionActorDesc* ActorDesc)
		{
			if (InRegistrationParameters.FilterActorDescFunc && !InRegistrationParameters.FilterActorDescFunc(ActorDesc))
			{
				return false;
			}

			// Filter duplicate WorldDataLayers
			if (ActorDesc->GetActorNativeClass()->IsChildOf<AWorldDataLayers>())
			{
				const FWorldDataLayersActorDesc* FoundWorldDataLayerActorsDesc = StaticCast<const FWorldDataLayersActorDesc*>(ActorDesc);
				if (FoundWorldDataLayerActorsDesc != nullptr && WorldDataLayerActorsDesc != nullptr)
				{
					UE_LOG(LogWorldPartition, Warning, TEXT("Extra World Data Layer '%s' actor found. Clean up invalid actors to remove the error."), *ActorDesc->GetActorPackage().ToString());
					return false;
				}

				WorldDataLayerActorsDesc = FoundWorldDataLayerActorsDesc;
			}

			// Filter actors with duplicated GUID in WorldPartition
			return !GetActorDesc(ActorDesc->GetGuid());
		};

		UActorDescContainer* ContainerToRegister = NewObject<UActorDescContainer>(this);
		ContainerToRegister->Initialize(ContainerInitParams);

		AddContainer(ContainerToRegister);

		if (IsInitialized() && EditorHash != nullptr)
		{
			FWorldPartitionReference WDLReference;
			for (FActorDescList::TIterator<> ActorDescIterator(ContainerToRegister); ActorDescIterator; ++ActorDescIterator)
			{
				if (ActorDescIterator->GetActorNativeClass()->IsChildOf<AWorldDataLayers>())
				{
					WDLReference = FWorldPartitionReference(this, ActorDescIterator->GetGuid());
					break;
				}
			}

			for (UActorDescContainer::TIterator<> It(ContainerToRegister); It; ++It)
			{
				HashActorDesc(*It);
			}
		}

		OnActorDescContainerRegistered.Broadcast(ContainerToRegister);

		return ContainerToRegister;
	}
	
	return nullptr;
}

bool UWorldPartition::UnregisterActorDescContainer(UActorDescContainer* InActorDescContainer)
{
	if (Contains(InActorDescContainer->GetContainerPackage()))
	{
		TArray<FGuid> ActorGuids;
		for (UActorDescContainer::TIterator<> It(InActorDescContainer); It; ++It)
		{
			FWorldPartitionHandle ActorHandle(this, It->GetGuid());
			if (ActorHandle.IsValid())
			{
				ActorGuids.Add(It->GetGuid());

				for (TMap<FWorldPartitionReference, AActor*>::TIterator DirtyActorIt(DirtyActors); DirtyActorIt; ++DirtyActorIt)
				{
					if (DirtyActorIt.Key() == ActorHandle)
					{
						DirtyActorIt.RemoveCurrent();
					}
				}
			}
		}

		UnpinActors(ActorGuids);

		OnActorDescContainerUnregistered.Broadcast(InActorDescContainer);

		if (IsInitialized() && EditorHash != nullptr)
		{
			for (UActorDescContainer::TIterator<> It(InActorDescContainer); It; ++It)
			{
				UnhashActorDesc(*It);
			}
		}

		InActorDescContainer->Uninitialize();

		verify(RemoveContainer(InActorDescContainer));

		return true;
	}

	return false;
}

void UWorldPartition::UninitializeActorDescContainers()
{
	for (UActorDescContainer* Container : ActorDescContainerCollection)
	{
		Container->Uninitialize();
	}

	Empty();
}

void UWorldPartition::PinActors(const TArray<FGuid>& ActorGuids)
{
	if (PinnedActors)
	{
		PinnedActors->AddActors(ActorGuids);
	}
}

void UWorldPartition::UnpinActors(const TArray<FGuid>& ActorGuids)
{
	if (PinnedActors)
	{
		PinnedActors->RemoveActors(ActorGuids);
	}
}

bool UWorldPartition::IsActorPinned(const FGuid& ActorGuid) const
{
	if (PinnedActors)
	{
		return PinnedActors->ContainsActor(ActorGuid);
	}
	return false;
}

void UWorldPartition::LoadLastLoadedRegions(const TArray<FBox>& EditorLastLoadedRegions)
{
	for (const FBox& EditorLastLoadedRegion : EditorLastLoadedRegions)
	{
		UWorldPartitionEditorLoaderAdapter* EditorLoaderAdapter = CreateEditorLoaderAdapter<FLoaderAdapterShape>(World, EditorLastLoadedRegion, TEXT("Last Loaded Region"));
		IWorldPartitionActorLoaderInterface::ILoaderAdapter* LoaderAdapter = EditorLoaderAdapter->GetLoaderAdapter();
		check(LoaderAdapter);
		LoaderAdapter->SetUserCreated(true);
		LoaderAdapter->Load();
	}
}

void UWorldPartition::LoadLastLoadedRegions()
{
	check(IsMainWorldPartition());

	TArray<FBox> EditorLastLoadedRegions = GetMutableDefault<UWorldPartitionEditorPerProjectUserSettings>()->GetEditorLoadedRegions(World);
	LoadLastLoadedRegions(EditorLastLoadedRegions);

	TArray<FName> EditorLoadedLocationVolumes = GetMutableDefault<UWorldPartitionEditorPerProjectUserSettings>()->GetEditorLoadedLocationVolumes(World);
	for (const FName& EditorLoadedLocationVolume : EditorLoadedLocationVolumes)
	{
		if (ALocationVolume* LocationVolume = FindObject<ALocationVolume>(World->PersistentLevel, *EditorLoadedLocationVolume.ToString()))
		{
			LocationVolume->bIsAutoLoad = true;
		}
	}
}

void UWorldPartition::OnWorldRenamed(UWorld* RenamedWorld)
{
	if (GetWorld() == RenamedWorld)
	{
		ActorDescContainer->SetContainerPackage(GetWorld()->GetPackage()->GetFName());

		// World was renamed so existing context is invalid.
		InstancingContext = FLinkerInstancingContext();
	}
}

void UWorldPartition::RemapSoftObjectPath(FSoftObjectPath& ObjectPath) const
{
	if (StreamingPolicy)
	{
		StreamingPolicy->RemapSoftObjectPath(ObjectPath);
	}
}

FBox UWorldPartition::GetEditorWorldBounds() const
{
	check(EditorHash);

	if (IsStreamingEnabled())
	{
		const FBox EditorWorldBounds = EditorHash->GetEditorWorldBounds();
		
		if (EditorWorldBounds.IsValid)
		{
			return EditorWorldBounds;
		}
	}

	return EditorHash->GetNonSpatialBounds();
}

FBox UWorldPartition::GetRuntimeWorldBounds() const
{
	check(EditorHash);

	if (IsStreamingEnabled())
	{
		const FBox RuntimeWorldBounds = EditorHash->GetRuntimeWorldBounds();
		
		if (RuntimeWorldBounds.IsValid)
		{
			return RuntimeWorldBounds;
		}
	}

	return EditorHash->GetNonSpatialBounds();
}
#endif

#undef LOCTEXT_NAMESPACE

