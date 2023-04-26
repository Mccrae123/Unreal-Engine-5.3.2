// Copyright Epic Games, Inc. All Rights Reserved.
#include "InterchangeTaskImportObject.h"

#if WITH_EDITOR
#include "AssetToolsModule.h"
#endif //WITH_EDITOR
#include "Async/TaskGraphInterfaces.h"
#include "CoreMinimal.h"
#include "InterchangeAssetImportData.h"
#include "InterchangeEngineLogPrivate.h"
#include "InterchangeFactoryBase.h"
#include "InterchangeImportCommon.h"
#include "InterchangeManager.h"
#include "InterchangeResult.h"
#include "InterchangeSourceData.h"
#include "InterchangeTranslatorBase.h"
#include "Misc/NamePermissionList.h"
#include "Misc/Paths.h"
#include "PackageUtils/PackageUtils.h"
#include "Stats/Stats.h"
#include "Templates/SharedPointer.h"
#include "UObject/Object.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/ObjectMacros.h"
#include "UObject/ObjectRedirector.h"
#include "UObject/WeakObjectPtrTemplates.h"

namespace UE::Interchange::Private
{
	void InternalGetPackageName(const UE::Interchange::FImportAsyncHelper& AsyncHelper, const int32 SourceIndex, const FString& PackageBasePath, const UInterchangeFactoryBaseNode* FactoryNode, FString& OutPackageName, FString& OutAssetName)
	{
		TRACE_CPUPROFILER_EVENT_SCOPE("UE::Interchange::Private::InternalGetPackageName")
		const UInterchangeSourceData* SourceData = AsyncHelper.SourceDatas[SourceIndex];
		check(SourceData);
		FString NodeDisplayName = FactoryNode->GetAssetName();

		// Set the asset name and the package name
		OutAssetName = NodeDisplayName;
		SanitizeObjectName(OutAssetName);

		FString SanitizedPackageBasePath = PackageBasePath;
		SanitizeObjectPath(SanitizedPackageBasePath);

		FString SubPath;
		if (FactoryNode->GetCustomSubPath(SubPath))
		{
			SanitizeObjectPath(SubPath);
		}

		OutPackageName = FPaths::Combine(*SanitizedPackageBasePath, *SubPath, *OutAssetName);
	}
	bool ShouldReimportFactoryNode(UInterchangeFactoryBaseNode* FactoryNode, const UInterchangeBaseNodeContainer* NodeContainer, UObject* ReimportObject)
	{
		TRACE_CPUPROFILER_EVENT_SCOPE("UE::Interchange::Private::ShouldReimportFactoryNode")

		if (!NodeContainer)
		{
			return false;
		}
		//Find all potential factory node
		TArray<UInterchangeFactoryBaseNode*> PotentialFactoryNodes;
		UClass* FactoryClass = FactoryNode->GetObjectClass();
		NodeContainer->IterateNodesOfType<UInterchangeFactoryBaseNode>([FactoryClass, &PotentialFactoryNodes](const FString& NodeUniqueID, UInterchangeFactoryBaseNode* CurrentFactoryNode)
			{
				if (UClass* CurrentFactoryClass = CurrentFactoryNode->GetObjectClass())
				{
					if (CurrentFactoryClass->IsChildOf(FactoryClass))
					{
						PotentialFactoryNodes.Add(CurrentFactoryNode);
					}
				}
			});
		
		if (PotentialFactoryNodes.Num() == 1)
		{
			//There is only one factory node that will generate this class of UObject, no need to match the unique id or the name.
			ensure(PotentialFactoryNodes[0] == FactoryNode);
			//The class of the object must fit with the factory object class.
			return ReimportObject->GetClass()->IsChildOf(FactoryClass);
		}

		//See if the FactoryNode match the ReimportObject.
		TArray<UObject*> SubObjects;
		GetObjectsWithOuter(ReimportObject, SubObjects);
		for (UObject* SubObject : SubObjects)
		{
			if(UInterchangeAssetImportData* OriginalAssetImportData = Cast<UInterchangeAssetImportData>(SubObject))
			{
				if (UInterchangeBaseNodeContainer* OriginalNodeContainer = OriginalAssetImportData->NodeContainer)
				{
					//Find the original factory node used by the last ReimportObject import
					if (UInterchangeFactoryBaseNode* OriginalFactoryNode = OriginalNodeContainer->GetFactoryNode(OriginalAssetImportData->NodeUniqueID))
					{
						//Compare the original factory node UObject class to the factory node UObject class
						if (OriginalFactoryNode->GetObjectClass()->IsChildOf(FactoryClass))
						{
							//Compare the original factory node, unique id to the factory node unique id
							if (OriginalFactoryNode->GetUniqueID().Equals(FactoryNode->GetUniqueID()))
							{
								return true;
							}
							//Compare the original factory node, name to the factory node name
							if (OriginalFactoryNode->GetDisplayLabel().Equals(FactoryNode->GetDisplayLabel()))
							{
								FString PackageSubPath;
								FactoryNode->GetCustomSubPath(PackageSubPath);
								FString OriginalPackageSubPath;
								OriginalFactoryNode->GetCustomSubPath(PackageSubPath);
								//Make sure both sub path are equal
								if (PackageSubPath.Equals(OriginalPackageSubPath))
								{
									return true;
								}
							}
						}
					}
				}
			}
		}
		return false;
	}




	bool CanImportClass(UE::Interchange::FImportAsyncHelper& AsyncHelper, UInterchangeFactoryBaseNode& FactoryNode, int32 SourceIndex)
	{
	#if WITH_EDITOR
		if (UClass* Class = FactoryNode.GetObjectClass())
		{
			if (AsyncHelper.AllowedClasses.Contains(Class))
			{
				return true;
			}
			else if (AsyncHelper.DeniedClasses.Contains(Class))
			{
				return false;
			}
			else
			{
				IAssetTools& AssetTools = FAssetToolsModule::GetModule().Get();
				TSharedPtr<FPathPermissionList> AssetClassPermissionList = AssetTools.GetAssetClassPathPermissionList(EAssetClassAction::ImportAsset);
				if (AssetClassPermissionList && AssetClassPermissionList->HasFiltering())
				{
					if (!AssetClassPermissionList->PassesFilter(Class->GetPathName()))
					{
						UE_LOG(LogInterchangeEngine, Display, TEXT("The creation of asset of class '%s' is not allowed in this project."), *Class->GetName());
						AsyncHelper.DeniedClasses.Add(Class);
						return false;
					}
				}

				AsyncHelper.AllowedClasses.Add(Class);

				return true;
			}
		}
		else
		{
			return false;
		}
	#endif //WITH_EDITOR

		return true;
	}

	UInterchangeFactoryBase::FImportAssetResult InternalImportObjectStartup(TSharedPtr<FImportAsyncHelper, ESPMode::ThreadSafe> AsyncHelper
		, UInterchangeFactoryBaseNode* FactoryNode
		, int32 SourceIndex
		, const FString& PackageBasePath
		, TFunction<UInterchangeFactoryBase::FImportAssetResult(UInterchangeFactoryBase::FImportAssetObjectParams&)>FactoryOperationLambda)
	{
		check(AsyncHelper.IsValid());
		UInterchangeFactoryBase::FImportAssetResult ErrorImportAssetResult;

		//Verify if the task was cancel
		if (AsyncHelper->bCancel || !FactoryNode || !Private::CanImportClass(*AsyncHelper, *FactoryNode, SourceIndex))
		{
			return ErrorImportAssetResult;
		}

		UInterchangeFactoryBase* Factory = nullptr;
		{
			FScopeLock Lock(&AsyncHelper->CreatedFactoriesLock);
			Factory = AsyncHelper->CreatedFactories.FindChecked(FactoryNode->GetUniqueID());
		}

		UPackage* Pkg = nullptr;
		FString PackageName;
		FString AssetName;
		Private::InternalGetPackageName(*AsyncHelper, SourceIndex, PackageBasePath, FactoryNode, PackageName, AssetName);
		bool bSkipAsset = false;
		UObject* ReimportObject = UE::Interchange::FFactoryCommon::GetObjectToReimport(AsyncHelper->TaskData.ReimportObject, PackageName, AssetName);
		if (ReimportObject)
		{
			UInterchangeBaseNodeContainer* NodeContainer = nullptr;
			if (AsyncHelper->BaseNodeContainers.IsValidIndex(SourceIndex))
			{
				NodeContainer = AsyncHelper->BaseNodeContainers[SourceIndex].Get();
			}

			if (Private::ShouldReimportFactoryNode(FactoryNode, NodeContainer, ReimportObject))
			{
				Pkg = ReimportObject->GetPackage();
				PackageName = Pkg->GetPathName();
				AssetName = ReimportObject->GetName();
			}
			else
			{
				bSkipAsset = true;
			}
		}
		else
		{
			FScopeLock Lock(&AsyncHelper->CreatedPackagesLock);
			UPackage** PkgPtr = AsyncHelper->CreatedPackages.Find(PackageName);

			if (!PkgPtr || !(*PkgPtr))
			{
				UInterchangeResultError_Generic* Message = Factory->AddMessage<UInterchangeResultError_Generic>();
				Message->SourceAssetName = AsyncHelper->SourceDatas[SourceIndex]->GetFilename();
				Message->DestinationAssetName = AssetName;
				Message->AssetType = FactoryNode->GetObjectClass();
				Message->Text = NSLOCTEXT("Interchange", "BadPackage", "It was not possible to create the asset as its package was not created correctly.");

				return ErrorImportAssetResult;
			}

			if (!AsyncHelper->SourceDatas.IsValidIndex(SourceIndex) || !AsyncHelper->Translators.IsValidIndex(SourceIndex))
			{
				UInterchangeResultError_Generic* Message = Factory->AddMessage<UInterchangeResultError_Generic>();
				Message->DestinationAssetName = AssetName;
				Message->AssetType = FactoryNode->GetObjectClass();
				Message->Text = NSLOCTEXT("Interchange", "SourceDataOrTranslatorInvalid", "It was not possible to create the asset as its translator was not created correctly.");

				return ErrorImportAssetResult;
			}

			Pkg = *PkgPtr;
		}

		if (!bSkipAsset)
		{
			if (AsyncHelper->TaskData.bFollowRedirectors)
			{
				// Check to see if we were redirected. If so, and if the asset name matched the package name, change the name of the asset to match the new package as well
				if (Pkg->GetName() != PackageName && FPackageName::GetLongPackageAssetName(PackageName) == AssetName)
				{
					AssetName = FPackageName::GetLongPackageAssetName(Pkg->GetName());
				}
			}

			UInterchangeTranslatorBase* Translator = AsyncHelper->Translators[SourceIndex];
			//Import Asset describe by the node
			UInterchangeFactoryBase::FImportAssetObjectParams CreateAssetParams;
			CreateAssetParams.AssetName = AssetName;
			CreateAssetParams.AssetNode = FactoryNode;
			CreateAssetParams.Parent = Pkg;
			CreateAssetParams.SourceData = AsyncHelper->SourceDatas[SourceIndex];
			CreateAssetParams.Translator = Translator;
			if (AsyncHelper->BaseNodeContainers.IsValidIndex(SourceIndex))
			{
				CreateAssetParams.NodeContainer = AsyncHelper->BaseNodeContainers[SourceIndex].Get();
			}
			CreateAssetParams.ReimportObject = ReimportObject;

			return FactoryOperationLambda(CreateAssetParams);
		}
		return ErrorImportAssetResult;
	}
}//ns UE::Interchange::Private

void UE::Interchange::FTaskImportObject_GameThread::DoTask(ENamedThreads::Type CurrentThread, const FGraphEventRef& MyCompletionGraphEvent)
{
	TRACE_CPUPROFILER_EVENT_SCOPE("UE::Interchange::FTaskImportObject_GameThread::DoTask")
#if INTERCHANGE_TRACE_ASYNCHRONOUS_TASK_ENABLED
	INTERCHANGE_TRACE_ASYNCHRONOUS_TASK(TaskImportObject_GameThread)
#endif
	using namespace UE::Interchange;

	TSharedPtr<FImportAsyncHelper, ESPMode::ThreadSafe> AsyncHelper = WeakAsyncHelper.Pin();
	check(AsyncHelper.IsValid());

	//Verify if the task was cancel or class to import is denied
	if (AsyncHelper->bCancel || !FactoryNode || !Private::CanImportClass(*AsyncHelper, *FactoryNode, SourceIndex))
	{
		return;
	}

	//The create package thread must always execute on the game thread
	check(IsInGameThread());

	// Create factory
	UInterchangeFactoryBase* Factory = NewObject<UInterchangeFactoryBase>(GetTransientPackage(), FactoryClass);
	Factory->SetResultsContainer(AsyncHelper->AssetImportResult->GetResults());

	{
		FScopeLock Lock(&AsyncHelper->CreatedFactoriesLock);
		AsyncHelper->CreatedFactories.Add(FactoryNode->GetUniqueID(), Factory);
	}

	UPackage* Pkg = nullptr;
	FString PackageName;
	FString AssetName;
	Private::InternalGetPackageName(*AsyncHelper, SourceIndex, PackageBasePath, FactoryNode, PackageName, AssetName);

	UObject* ReimportObject = FFactoryCommon::GetObjectToReimport(AsyncHelper->TaskData.ReimportObject, PackageName, AssetName);
	//If we do a reimport no need to create a package
	if (ReimportObject)
	{
		UInterchangeBaseNodeContainer* NodeContainer = nullptr;
		if (AsyncHelper->BaseNodeContainers.IsValidIndex(SourceIndex))
		{
			NodeContainer = AsyncHelper->BaseNodeContainers[SourceIndex].Get();
		}
		
		if (Private::ShouldReimportFactoryNode(FactoryNode, NodeContainer, ReimportObject))
		{
			FactoryNode->SetDisplayLabel(ReimportObject->GetName());
			FactoryNode->SetAssetName(ReimportObject->GetName());
			Pkg = ReimportObject->GetPackage();
			PackageName = Pkg->GetPathName();
			AssetName = ReimportObject->GetName();
			//Import Asset describe by the node
			UInterchangeFactoryBase::FImportAssetObjectParams CreateAssetParams;
			CreateAssetParams.AssetName = AssetName;
			CreateAssetParams.AssetNode = FactoryNode;
			CreateAssetParams.Parent = Pkg;
			CreateAssetParams.SourceData = AsyncHelper->SourceDatas[SourceIndex];
			CreateAssetParams.Translator = AsyncHelper->Translators[SourceIndex];
			CreateAssetParams.NodeContainer = NodeContainer;
			CreateAssetParams.ReimportObject = ReimportObject;
			FactoryNode->SetCustomReferenceObject(FSoftObjectPath(ReimportObject));
			//We call CreateEmptyAsset to ensure any resource use by an existing UObject is released on the game thread
			Factory->BeginImportAsset_GameThread(CreateAssetParams);
		}
		else
		{
			//Skip this asset, reimport object original factory node does not match this factory node
			return;
		}
	}
	else
	{
		Private::InternalGetPackageName(*AsyncHelper, SourceIndex, PackageBasePath, FactoryNode, PackageName, AssetName);
		// We can not create assets that share the name of a map file in the same location
		if (FPackageUtils::IsMapPackageAsset(PackageName))
		{
			UInterchangeResultError_Generic* Message = Factory->AddMessage<UInterchangeResultError_Generic>();
			Message->SourceAssetName = AsyncHelper->SourceDatas[SourceIndex]->GetFilename();
			Message->DestinationAssetName = AssetName;
			Message->AssetType = FactoryNode->GetObjectClass();
			Message->Text = NSLOCTEXT("Interchange", "MapExistsWithSameName", "You cannot create an asset with this name, as there is already a map file with the same name in this folder.");

			//Skip this asset
			return;
		}

		//If the package already exist we must load it so factory can find any existing asset and decide or not to override it
		{
			//Try to find the package in memory
			bool bPackageWasCreated = false;
			Pkg = FindPackage(nullptr, *PackageName);
			if (!Pkg)
			{
				//Try to load the package from disk
				Pkg = LoadPackage(nullptr, *PackageName, LOAD_NoWarn | LOAD_Quiet);
				if (!Pkg)
				{
					//Create the package
					Pkg = CreatePackage(*PackageName);
					Pkg->SetPackageFlags(PKG_NewlyCreated);
					bPackageWasCreated = true;
				}
			}

			if (Pkg == nullptr)
			{
				UInterchangeResultError_Generic* Message = Factory->AddMessage<UInterchangeResultError_Generic>();
				Message->SourceAssetName = AsyncHelper->SourceDatas[SourceIndex]->GetFilename();
				Message->DestinationAssetName = AssetName;
				Message->AssetType = FactoryNode->GetObjectClass();
				Message->Text = FText::Format(NSLOCTEXT("Interchange", "CouldntCreatePackage", "It was not possible to create a package named '{0}'; the asset will not be imported."), FText::FromString(PackageName));

				//Skip this asset
				return;
			}

			if (!bPackageWasCreated && AsyncHelper->TaskData.bFollowRedirectors)
			{
				if (UObjectRedirector* Redirector = FindObject<UObjectRedirector>(Pkg, *AssetName))
				{
					if (Redirector->DestinationObject)
					{
						Pkg = Redirector->DestinationObject->GetPackage();
						if (FPackageName::GetLongPackageAssetName(PackageName) == AssetName)
						{
							AssetName = FPackageName::GetLongPackageAssetName(Pkg->GetName());
						}
					}
				}
			}
		}

		//Import Asset describe by the node
		UInterchangeFactoryBase::FImportAssetObjectParams CreateAssetParams;
		CreateAssetParams.AssetName = AssetName;
		CreateAssetParams.AssetNode = FactoryNode;
		CreateAssetParams.Parent = Pkg;
		CreateAssetParams.SourceData = AsyncHelper->SourceDatas[SourceIndex];
		CreateAssetParams.Translator = AsyncHelper->Translators[SourceIndex];
		if (AsyncHelper->BaseNodeContainers.IsValidIndex(SourceIndex))
		{
			CreateAssetParams.NodeContainer = AsyncHelper->BaseNodeContainers[SourceIndex].Get();
		}
		CreateAssetParams.ReimportObject = ReimportObject;
		//Make sure the asset UObject is created with the correct type on the main thread
		UInterchangeFactoryBase::FImportAssetResult ImportAssetResult = Factory->BeginImportAsset_GameThread(CreateAssetParams);
		if (ImportAssetResult.ImportedObject)
		{
			//If the factory skip the asset, we simply set the node custom reference object
			if (!ImportAssetResult.bIsFactorySkipAsset)
			{
				if (!ImportAssetResult.ImportedObject->HasAnyInternalFlags(EInternalObjectFlags::Async))
				{
					//Since the async flag is not set we must be in the game thread
					ensure(IsInGameThread());
					ImportAssetResult.ImportedObject->SetInternalFlags(EInternalObjectFlags::Async);
				}
				FScopeLock Lock(&AsyncHelper->ImportedAssetsPerSourceIndexLock);
				TArray<FImportAsyncHelper::FImportedObjectInfo>& ImportedInfos = AsyncHelper->ImportedAssetsPerSourceIndex.FindOrAdd(SourceIndex);
				FImportAsyncHelper::FImportedObjectInfo& AssetInfo = ImportedInfos.AddDefaulted_GetRef();
				AssetInfo.ImportedObject = ImportAssetResult.ImportedObject;
				AssetInfo.Factory = Factory;
				AssetInfo.FactoryNode = FactoryNode;
				AssetInfo.bIsReimport = bool(ReimportObject != nullptr);
			}
			FactoryNode->SetCustomReferenceObject(FSoftObjectPath(ImportAssetResult.ImportedObject));
		}
	}

	// Make sure the destination package is loaded
	Pkg->FullyLoad();
	
	{
		FScopeLock Lock(&AsyncHelper->CreatedPackagesLock);
		AsyncHelper->CreatedPackages.Add(PackageName, Pkg);
	}
}

void UE::Interchange::FTaskImportObject_Async::DoTask(ENamedThreads::Type CurrentThread, const FGraphEventRef& MyCompletionGraphEvent)
{
#if INTERCHANGE_TRACE_ASYNCHRONOUS_TASK_ENABLED
	INTERCHANGE_TRACE_ASYNCHRONOUS_TASK(TaskImportObject_Async)
#endif
	using namespace UE::Interchange;

	TSharedPtr<FImportAsyncHelper, ESPMode::ThreadSafe> AsyncHelper = WeakAsyncHelper.Pin();
	check(AsyncHelper.IsValid());

	//Verify if the task was cancel
	if (AsyncHelper->bCancel || !FactoryNode || !Private::CanImportClass(*AsyncHelper, *FactoryNode, SourceIndex))
	{
		return;
	}

	UInterchangeFactoryBase* Factory = nullptr;
	{
		FScopeLock Lock(&AsyncHelper->CreatedFactoriesLock);
		Factory = AsyncHelper->CreatedFactories.FindChecked(FactoryNode->GetUniqueID());
	}

	Private::InternalImportObjectStartup(AsyncHelper
		, FactoryNode
		, SourceIndex
		, PackageBasePath
		, [&Factory](UInterchangeFactoryBase::FImportAssetObjectParams& ImportAssetObjectParams)
		{
			return Factory->ImportAsset_Async(ImportAssetObjectParams);
		});
}

void UE::Interchange::FTaskImportObjectFinalize_GameThread::DoTask(ENamedThreads::Type CurrentThread, const FGraphEventRef& MyCompletionGraphEvent)
{
#if INTERCHANGE_TRACE_ASYNCHRONOUS_TASK_ENABLED
	INTERCHANGE_TRACE_ASYNCHRONOUS_TASK(TaskImportObjectFinalize_GameThread)
#endif
	using namespace UE::Interchange;

	TSharedPtr<FImportAsyncHelper, ESPMode::ThreadSafe> AsyncHelper = WeakAsyncHelper.Pin();
	check(AsyncHelper.IsValid());

	//Verify if the task was cancel
	if (AsyncHelper->bCancel || !FactoryNode || !Private::CanImportClass(*AsyncHelper, *FactoryNode, SourceIndex))
	{
		return;
	}

	UInterchangeFactoryBase* Factory = nullptr;
	{
		FScopeLock Lock(&AsyncHelper->CreatedFactoriesLock);
		Factory = AsyncHelper->CreatedFactories.FindChecked(FactoryNode->GetUniqueID());
	}
	UInterchangeFactoryBase::FImportAssetResult ImportAssetResult = Private::InternalImportObjectStartup(AsyncHelper
		, FactoryNode
		, SourceIndex
		, PackageBasePath
		, [&Factory](UInterchangeFactoryBase::FImportAssetObjectParams& ImportAssetObjectParams)
		{
			return Factory->EndImportAsset_GameThread(ImportAssetObjectParams);
		});

	if (ImportAssetResult.ImportedObject)
	{
		//If the factory skip the asset, we simply set the node custom reference object
		if (!ImportAssetResult.bIsFactorySkipAsset)
		{
			FScopeLock Lock(&AsyncHelper->ImportedAssetsPerSourceIndexLock);
			TArray<FImportAsyncHelper::FImportedObjectInfo>& ImportedInfos = AsyncHelper->ImportedAssetsPerSourceIndex.FindOrAdd(SourceIndex);
			FImportAsyncHelper::FImportedObjectInfo* AssetInfoPtr = ImportedInfos.FindByPredicate([&ImportAssetResult](const FImportAsyncHelper::FImportedObjectInfo& CurInfo)
				{
					return CurInfo.ImportedObject == ImportAssetResult.ImportedObject;
				});

			if (!AssetInfoPtr)
			{
				FImportAsyncHelper::FImportedObjectInfo& AssetInfo = ImportedInfos.AddDefaulted_GetRef();
				AssetInfo.ImportedObject = ImportAssetResult.ImportedObject;
				AssetInfo.Factory = Factory;
				AssetInfo.FactoryNode = FactoryNode;

				const UObject* ReimportObject = FFactoryCommon::GetObjectToReimport(AsyncHelper->TaskData.ReimportObject, ImportAssetResult.ImportedObject->GetPathName(), ImportAssetResult.ImportedObject->GetName());
				AssetInfo.bIsReimport = bool(ReimportObject != nullptr);
			}

			// Fill in destination asset and type in any results which have been added previously by a translator or pipeline, now that we have a corresponding factory.
			UInterchangeResultsContainer* Results = AsyncHelper->AssetImportResult->GetResults();
			for (UInterchangeResult* Result : Results->GetResults())
			{
				if (!Result->InterchangeKey.IsEmpty() && (Result->DestinationAssetName.IsEmpty() || Result->AssetType == nullptr))
				{
					TArray<FString> TargetAssets;
					FactoryNode->GetTargetNodeUids(TargetAssets);
					if (TargetAssets.Contains(Result->InterchangeKey))
					{
						Result->DestinationAssetName = ImportAssetResult.ImportedObject->GetPathName();
						Result->AssetType = ImportAssetResult.ImportedObject->GetClass();
					}
				}
			}
		}

		FactoryNode->SetCustomReferenceObject(FSoftObjectPath(ImportAssetResult.ImportedObject));
	}
}