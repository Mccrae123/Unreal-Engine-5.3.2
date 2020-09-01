// Copyright Epic Games, Inc. All Rights Reserved.
#include "InterchangeTaskCompletion.h"

#include "Async/TaskGraphInterfaces.h"
#include "CoreMinimal.h"
#include "InterchangeFactoryBase.h"
#include "InterchangeManager.h"
#include "Stats/Stats.h"
#include "Templates/SharedPointer.h"
#include "UObject/Object.h"
#include "UObject/ObjectMacros.h"
#include "UObject/WeakObjectPtrTemplates.h"

#if WITH_ENGINE
#include "AssetRegistryModule.h"
#endif //WITH_ENGINE

void Interchange::FTaskCompletion::DoTask(ENamedThreads::Type CurrentThread, const FGraphEventRef& MyCompletionGraphEvent)
{
	TSharedPtr<Interchange::FImportAsyncHelper, ESPMode::ThreadSafe> AsyncHelper = WeakAsyncHelper.Pin();
	check(AsyncHelper.IsValid());

	for(TPair<int32, TArray<Interchange::FImportAsyncHelper::FImportedAssetInfo>>& AssetInfosPerSourceIndexPair : AsyncHelper->ImportedAssetsPerSourceIndex)
	{
		const int32 SourceIndex = AssetInfosPerSourceIndexPair.Key;
		const bool bCallPostImportGameThreadCallback = ensure(AsyncHelper->SourceDatas.IsValidIndex(SourceIndex));
		for (const Interchange::FImportAsyncHelper::FImportedAssetInfo& AssetInfo : AssetInfosPerSourceIndexPair.Value)
		{
			UObject* Asset = AssetInfo.ImportAsset;
			//In case Some factory code cannot run outside of the main thread we offer this callback to finish the work before calling post edit change (building the asset)
			if(bCallPostImportGameThreadCallback && AssetInfo.Factory)
			{
				UInterchangeFactoryBase::FPostImportGameThreadCallbackParams Arguments;
				Arguments.ReimportObject = Asset;
				Arguments.SourceData = AsyncHelper->SourceDatas[SourceIndex];
				AssetInfo.Factory->PostImportGameThreadCallback(Arguments);
			}

			//Clear any async flag from the created asset
			Asset->ClearInternalFlags(EInternalObjectFlags::Async);
			//Make sure the package is dirty
			Asset->MarkPackageDirty();
#if WITH_EDITOR
			//Make sure the asset is built correctly
			Asset->PostEditChange();
#endif
			//Post import broadcast
			if (AsyncHelper->TaskData.ReimportObject)
			{
				InterchangeManager->OnAssetPostReimport.Broadcast(Asset);
			}
			else
			{
				InterchangeManager->OnAssetPostImport.Broadcast(Asset);
			}
#if WITH_ENGINE
			//Notify the asset registry
			FAssetRegistryModule::AssetCreated(Asset);
#endif //WITH_ENGINE

			if (SourceIndex == 0)
			{
				AsyncHelper->RootObject.SetValue(Asset);
				AsyncHelper->RootObjectCompletionEvent->DispatchSubsequents();
			}
		}
	}
	//Release the async helper
	AsyncHelper = nullptr;
	InterchangeManager->ReleaseAsyncHelper(WeakAsyncHelper);
}