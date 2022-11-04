// Copyright Epic Games, Inc. All Rights Reserved.

#include "ChaosFlesh/Cmd/ChaosFleshCommands.h"

#include "ChaosFlesh/Asset/FleshAssetFactory.h"
#include "ChaosFlesh/FleshAsset.h"
#include "ChaosFlesh/FleshActor.h"
#include "ChaosFlesh/FleshComponent.h"
#include "ChaosFlesh/FleshCollectionUtility.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Editor.h"
#include "EditorSupportDelegates.h"
#include "Engine/Selection.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "Internationalization/Regex.h"
#include "Logging/LogMacros.h"
#include "Misc/Paths.h"
#include "SceneOutlinerDelegates.h"


DEFINE_LOG_CATEGORY_STATIC(UChaosFleshCommandsLogging, NoLogging, All);



void FChaosFleshCommands::ImportFile(const TArray<FString>& Args, UWorld* World)
{
	if (Args.Num() == 1)
	{
		//FString BasePath = FPaths::ProjectDir() + "Imports/default.geo";// +FString(Args[0]);
		if (FPaths::FileExists(Args[0]))
		{
			auto Factory = NewObject<UFleshAssetFactory>();
			UPackage* Package = CreatePackage(TEXT("/Game/FleshAsset"));
			//UPackage* Package = CreatePackage( *FPaths::ProjectContentDir() );

			UFleshAsset* FleshAsset = static_cast<UFleshAsset*>(Factory->FactoryCreateNew(UFleshAsset::StaticClass(), Package, FName("FleshAsset"), RF_Standalone | RF_Public, NULL, GWarn));
			FAssetRegistryModule::AssetCreated(FleshAsset);
			{
				FFleshAssetEdit EditObject = FleshAsset->EditCollection();
				if (FFleshCollection* Collection = EditObject.GetFleshCollection())
				{
					UE_LOG(UChaosFleshCommandsLogging, Log, TEXT("FChaosFleshCommands::ImportFile"));
					if (TUniquePtr<FFleshCollection> InCollection = ChaosFlesh::ImportTetFromFile(Args[0]))
					{
						Collection->CopyMatchingAttributesFrom(*InCollection);
					}
				}
				Package->SetDirtyFlag(true);
			}
		}
	}
	else
	{
		UE_LOG(UChaosFleshCommandsLogging, Error, TEXT("Failed to import file for flesh asset."));
	}
}
