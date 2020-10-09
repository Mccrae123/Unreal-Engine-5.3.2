// Copyright Epic Games, Inc. All Rights Reserved.

#include "Commandlets/WorldPartitionBuilderCommandlet.h"
#include "WorldPartition/WorldPartitionBuilder.h"

#include "CoreMinimal.h"
#include "EngineUtils.h"
#include "Editor.h"
#include "Logging/LogMacros.h"
#include "Misc/ConfigCacheIni.h"
#include "HAL/PlatformFileManager.h"
#include "ProfilingDebugging/ScopedTimers.h"
#include "WorldPartition/WorldPartition.h"
#include "WorldPartition/WorldPartitionSubsystem.h"

DEFINE_LOG_CATEGORY_STATIC(LogWorldPartitionBuilderCommandlet, All, All);

UWorldPartitionBuilderCommandlet::UWorldPartitionBuilderCommandlet(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{}

int32 UWorldPartitionBuilderCommandlet::Main(const FString& Params)
{
	UE_SCOPED_TIMER(TEXT("Execution"), LogWorldPartitionBuilderCommandlet, Display);

	TArray<FString> Tokens, Switches;
	ParseCommandLine(*Params, Tokens, Switches);

	if (Tokens.Num() != 1)
	{
		UE_LOG(LogWorldPartitionBuilderCommandlet, Error, TEXT("Missing world name"));
		return 1;
	}

	if (Switches.Contains(TEXT("Verbose")))
	{
		LogWorldPartitionBuilderCommandlet.SetVerbosity(ELogVerbosity::Verbose);
	}

	// This will convert incomplete package name to a fully qualifed path
	FString WorldFilename;
	if (!FPackageName::SearchForPackageOnDisk(Tokens[0], &Tokens[0], &WorldFilename))
	{
		UE_LOG(LogWorldPartitionBuilderCommandlet, Error, TEXT("Unknown world '%s'"), *Tokens[0]);
		return 1;
	}

	// Load configuration file
	FString WorldConfigFilename = FPaths::ChangeExtension(WorldFilename, TEXT("ini"));
	if (FPlatformFileManager::Get().GetPlatformFile().FileExists(*WorldConfigFilename))
	{
		LoadConfig(GetClass(), *WorldConfigFilename);
	}

	// Load the map package
	UPackage* MapPackage = LoadPackage(NULL, *Tokens[0], LOAD_None);
	if (!MapPackage)
	{
		UE_LOG(LogWorldPartitionBuilderCommandlet, Error, TEXT("Couldn't load package %s."), *Tokens[0]);
		return 1;
	}

	// Find the world in the given package
	UWorld* World = UWorld::FindWorldInPackage(MapPackage);
	if (!World)
	{
		UE_LOG(LogWorldPartitionBuilderCommandlet, Error, TEXT("No world in specified package %s."));
		return 1;
	}

	// Setup the world.
	World->WorldType = EWorldType::Editor;
	World->AddToRoot();
	if (!World->bIsWorldInitialized)
	{
		UWorld::InitializationValues IVS;
		IVS.RequiresHitProxies(false);
		IVS.ShouldSimulatePhysics(false);
		IVS.EnableTraceCollision(false);
		IVS.CreateNavigation(false);
		IVS.CreateAISystem(false);
		IVS.AllowAudioPlayback(false);
		IVS.CreatePhysicsScene(true);

		World->InitWorld(UWorld::InitializationValues(IVS));
		World->PersistentLevel->UpdateModelComponents();
		World->UpdateWorldComponents(true /*bRerunConstructionScripts*/, false /*bCurrentLevelOnly*/);
	}

	// Make sure the world is partitioned.
	if (!World->HasSubsystem<UWorldPartitionSubsystem>())
	{
		UE_LOG(LogWorldPartitionBuilderCommandlet, Error, TEXT("Commandlet only works on partitioned maps."));
		return 1;
	}

	// Retrieve the world partition.
	UWorldPartition* WorldPartition = World->GetWorldPartition();
	check(WorldPartition);

	FWorldContext& WorldContext = GEditor->GetEditorWorldContext(true /*bEnsureIsGWorld*/);
	WorldContext.SetCurrentWorld(World);
	GWorld = World;
	
	//========================================================================================================================
	// RUN BUILDERS
	//========================================================================================================================
	FString BuilderList;
	if (!FParse::Value(*Params, TEXT("Builders="), BuilderList, false))
	{
		UE_LOG(LogWorldPartitionBuilderCommandlet, Error, TEXT("Invalid builders list."));
		return 1;
	}

	BuilderList.ReplaceInline(TEXT(","), TEXT(" "));
	
	bool bNeedToSave = false;	// Does any builder needs the map to be saved.
	bool bAllowedToSave = true; // Does all builders are ok with the map being saved.
	FString BuilderClassName;
	const TCHAR* BuilderListPtr = *BuilderList;
	while (FParse::Token(BuilderListPtr, BuilderClassName, false))
	{
		UClass* BuilderClass = FindObject<UClass>(ANY_PACKAGE, *BuilderClassName);

		if (!BuilderClass)
		{
			UE_LOG(LogWorldPartitionBuilderCommandlet, Error, TEXT("Unknown builder %s."), *BuilderClassName);
			return 1;
		}

		UWorldPartitionBuilder* Builder = NewObject<UWorldPartitionBuilder>(this, BuilderClass);
		check(Builder);

		if (Builder->RequiresCommandletRendering() && !IsAllowCommandletRendering())
		{
			UE_LOG(LogWorldPartitionBuilderCommandlet, Error, TEXT("The option \"-AllowCommandletRendering\" must be provided for the %s process to work"), *BuilderClassName);
			return 1;
		}

		bNeedToSave |= Builder->RequiresMapSaving();
		bAllowedToSave &= Builder->AllowsMapSaving();

		// Load builder configuration
		if (FPlatformFileManager::Get().GetPlatformFile().FileExists(*WorldConfigFilename))
		{
			Builder->LoadConfig(BuilderClass, *WorldConfigFilename);
		}

		Builders.Add(Builder);
	}

	if (bNeedToSave && !bAllowedToSave)
	{
		UE_LOG(LogWorldPartitionBuilderCommandlet, Error, TEXT("Some builders are incompatible and must be run separately."));
		return 1;
	}

	// For now, load all cells
	// In the future, we'll want the commandlet to be able to perform partial updates of the map
	// to allow builders to be distributed on multiple machines or run incremental builds.
	const FBox LoadBox(FVector(-WORLD_MAX, -WORLD_MAX, -WORLD_MAX), FVector(WORLD_MAX, WORLD_MAX, WORLD_MAX));
	WorldPartition->LoadEditorCells(LoadBox);

	// Run builders
	for (UWorldPartitionBuilder* Builder: Builders)
	{
		if (!Builder->Run(World, *this))
		{
			return 1;
		}
	}

	// Save the world
	if (bNeedToSave && bAllowedToSave)
	{
		if (!UPackage::SavePackage(MapPackage, nullptr, RF_Standalone, *WorldFilename))
		{
			UE_LOG(LogWorldPartitionBuilderCommandlet, Error, TEXT("Error saving map package %s."), *MapPackage->GetName());
			return 1;
		}
	}

	// Save default configuration
	if (!FPlatformFileManager::Get().GetPlatformFile().FileExists(*WorldConfigFilename) ||
		!FPlatformFileManager::Get().GetPlatformFile().IsReadOnly(*WorldConfigFilename))
	{
		SaveConfig(CPF_Config, *WorldConfigFilename);

		for (UWorldPartitionBuilder* Builder: Builders)
		{
			Builder->SaveConfig(CPF_Config, *WorldConfigFilename);
		}
	}

	// Cleanup
	World->RemoveFromRoot();
	WorldContext.SetCurrentWorld(nullptr);
	GWorld = nullptr;

	return 0;
}
