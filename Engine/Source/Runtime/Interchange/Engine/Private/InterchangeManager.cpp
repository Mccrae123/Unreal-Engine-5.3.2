// Copyright Epic Games, Inc. All Rights Reserved.

#include "InterchangeManager.h"

#include "AssetDataTagMap.h"
#include "AssetRegistryModule.h"
#include "CoreMinimal.h"
#include "Engine/Blueprint.h"
#include "Framework/Notifications/NotificationManager.h"
#include "InterchangeFactoryBase.h"
#include "InterchangeEngineLogPrivate.h"
#include "InterchangeSourceData.h"
#include "InterchangeTranslatorBase.h"
#include "InterchangeWriterBase.h"
#include "Internationalization/Internationalization.h"
#include "Misc/App.h"
#include "Misc/AsyncTaskNotification.h"
#include "Misc/MessageDialog.h"
#include "Misc/ScopedSlowTask.h"
#include "Nodes/InterchangeBaseNodeContainer.h"
#include "PackageUtils/PackageUtils.h"
#include "Tasks/InterchangeTaskParsing.h"
#include "Tasks/InterchangeTaskPipeline.h"
#include "Tasks/InterchangeTaskTranslator.h"
#include "UObject/Class.h"
#include "UObject/GarbageCollection.h"
#include "UObject/NameTypes.h"
#include "UObject/Object.h"
#include "UObject/ObjectMacros.h"
#include "UObject/UObjectIterator.h"
#include "UObject/WeakObjectPtrTemplates.h"
#include "Widgets/Notifications/SNotificationList.h"

namespace InternalInterchangePrivate
{
	const FLogCategoryBase* GetLogInterchangePtr()
	{
#if NO_LOGGING
		return nullptr;
#else
		return &LogInterchangeEngine;
#endif
	}
}

UE::Interchange::FScopedSourceData::FScopedSourceData(const FString& Filename)
{
	//Found the translator
	SourceDataPtr = TStrongObjectPtr<UInterchangeSourceData>(UInterchangeManager::GetInterchangeManager().CreateSourceData(Filename));
	check(SourceDataPtr.IsValid());
}

UInterchangeSourceData* UE::Interchange::FScopedSourceData::GetSourceData() const
{
	return SourceDataPtr.Get();
}

UE::Interchange::FScopedTranslator::FScopedTranslator(const UInterchangeSourceData* SourceData)
{
	//Found the translator
	ScopedTranslatorPtr = TStrongObjectPtr<UInterchangeTranslatorBase>(UInterchangeManager::GetInterchangeManager().GetTranslatorForSourceData(SourceData));
}

UInterchangeTranslatorBase* UE::Interchange::FScopedTranslator::GetTranslator()
{
	return ScopedTranslatorPtr.Get();
}

UE::Interchange::FImportAsyncHelper::FImportAsyncHelper()
{
	RootObjectCompletionEvent = FGraphEvent::CreateGraphEvent();
	bCancel = false;
}

void UE::Interchange::FImportAsyncHelper::AddReferencedObjects(FReferenceCollector& Collector)
{
	for (UInterchangeSourceData* SourceData : SourceDatas)
	{
		Collector.AddReferencedObject(SourceData);
	}
	for (UInterchangeTranslatorBase* Translator : Translators)
	{
		Collector.AddReferencedObject(Translator);
	}
	for (UInterchangePipelineBase* Pipeline : Pipelines)
	{
		Collector.AddReferencedObject(Pipeline);
	}
	for (UInterchangeFactoryBase* Factory : Factories)
	{
		Collector.AddReferencedObject(Factory);
	}
}

void UE::Interchange::FImportAsyncHelper::ReleaseTranslatorsSource()
{
	for (UInterchangeTranslatorBase* BaseTranslator : Translators)
	{
		if (BaseTranslator)
		{
			BaseTranslator->ReleaseSource();
		}
	}
}

void UE::Interchange::FImportAsyncHelper::InitCancel()
{
	bCancel = true;
	ReleaseTranslatorsSource();
}

void UE::Interchange::FImportAsyncHelper::CancelAndWaitUntilDoneSynchronously()
{
	bCancel = true;

	FGraphEventArray TasksToComplete;

	if (TranslatorTasks.Num())
	{
		TasksToComplete.Append(TranslatorTasks);
	}
	if (PipelinePreImportTasks.Num())
	{
		TasksToComplete.Append(PipelinePreImportTasks);
	}
	if (ParsingTask.GetReference())
	{
		TasksToComplete.Add(ParsingTask);
	}
	if (CreatePackageTasks.Num())
	{
		TasksToComplete.Append(CreatePackageTasks);
	}
	if (CreateAssetTasks.Num())
	{
		TasksToComplete.Append(CreateAssetTasks);
	}
	if (PipelinePostImportTasks.Num())
	{
		TasksToComplete.Append(PipelinePostImportTasks);
	}
	if (CompletionTask.GetReference())
	{
		//Completion task will make sure any created asset before canceling will be mark for delete
		TasksToComplete.Add(CompletionTask);
	}

	//Block until all task are completed, it should be fast since bCancel is true
	if (TasksToComplete.Num())
	{
		FTaskGraphInterface::Get().WaitUntilTasksComplete(TasksToComplete, ENamedThreads::GameThread);
	}
	//Async import result in a null object when we cancel
	if (!RootObjectCompletionEvent->IsComplete())
	{
		RootObject.SetValue(nullptr);
		RootObjectCompletionEvent->DispatchSubsequents();
	}
}

void UE::Interchange::FImportAsyncHelper::CleanUp()
{
	//Release the graph
	BaseNodeContainers.Empty();

	for (UInterchangeSourceData* SourceData : SourceDatas)
	{
		if (SourceData)
		{
			SourceData->RemoveFromRoot();
			SourceData->MarkPendingKill();
			SourceData = nullptr;
		}
	}
	SourceDatas.Empty();

	for (UInterchangeTranslatorBase* Translator : Translators)
	{
		if(Translator)
		{
			Translator->ImportFinish();
			Translator->RemoveFromRoot();
			Translator->MarkPendingKill();
			Translator = nullptr;
		}
	}
	Translators.Empty();

	for (UInterchangePipelineBase* Pipeline : Pipelines)
	{
		if(Pipeline)
		{
			Pipeline->RemoveFromRoot();
			Pipeline->MarkPendingKill();
			Pipeline = nullptr;
		}
	}
	Pipelines.Empty();

	//Factories are not instantiate, we use the registered one directly
	Factories.Empty();
}

UE::Interchange::FAsyncImportResult::FAsyncImportResult( TFuture< UObject* >&& InFutureObject, const FGraphEventRef& InGraphEvent )
	: FutureObject( MoveTemp( InFutureObject ) )
	, GraphEvent( InGraphEvent )
{
}

bool UE::Interchange::FAsyncImportResult::IsValid() const
{
	return FutureObject.IsValid();
}

UObject* UE::Interchange::FAsyncImportResult::Get() const
{
	if ( !FutureObject.IsReady() )
	{
		// Tick the task graph until our FutureObject is ready
		FTaskGraphInterface::Get().WaitUntilTaskCompletes( GraphEvent );
	}

	return FutureObject.Get();
}

UE::Interchange::FAsyncImportResult UE::Interchange::FAsyncImportResult::Next( TFunction< UObject*( UObject* ) > Continuation )
{
	return UE::Interchange::FAsyncImportResult{ FutureObject.Next( Continuation ), GraphEvent };
}

void UE::Interchange::SanitizeInvalidChar(FString& String)
{
	const TCHAR* InvalidChar = INVALID_OBJECTPATH_CHARACTERS;
	while (*InvalidChar)
	{
		String.ReplaceCharInline(*InvalidChar, TCHAR('_'), ESearchCase::CaseSensitive);
		++InvalidChar;
	}
}

UInterchangeManager& UInterchangeManager::GetInterchangeManager()
{
	static TStrongObjectPtr<UInterchangeManager> InterchangeManager = nullptr;
	
	//This boolean will be true after we delete the singleton
	static bool InterchangeManagerScopeOfLifeEnded = false;

	if (!InterchangeManager.IsValid())
	{
		//We cannot create a TStrongObjectPtr outside of the main thread, we also need a valid Transient package
		check(IsInGameThread() && GetTransientPackage());

		//Avoid hard crash if someone call the manager after we delete it, but send a callstack to the crash manager
		ensure(!InterchangeManagerScopeOfLifeEnded);

		InterchangeManager = TStrongObjectPtr<UInterchangeManager>(NewObject<UInterchangeManager>(GetTransientPackage(), NAME_None, EObjectFlags::RF_NoFlags));
		
		//We cancel any running task when we pre exit the engine
		FCoreDelegates::OnEnginePreExit.AddLambda([]()
		{
			//In editor the user cannot exit the editor if the interchange manager has active task.
			//But if we are not running the editor its possible to get here, so block the main thread until all
			//cancel tasks are done.
			if(GIsEditor)
			{
				ensure(InterchangeManager->ImportTasks.Num() == 0);
			}
			else
			{
				InterchangeManager->CancelAllTasksSynchronously();
			}
		});

		//We release the singleton here so all module was able to unhook there delegates
		FCoreDelegates::OnExit.AddLambda([]()
		{
			//Task should have been cancel in the Engine pre exit callback
			ensure(InterchangeManager->ImportTasks.Num() == 0);
			//Release the InterchangeManager object
			InterchangeManager.Reset();
			InterchangeManagerScopeOfLifeEnded = true;
		});
	}

	//When we get here we should be valid
	check(InterchangeManager.IsValid());

	return *(InterchangeManager.Get());
}

bool UInterchangeManager::RegisterTranslator(const UClass* TranslatorClass)
{
	if(!TranslatorClass)
	{
		return false;
	}

	if(RegisteredTranslators.Contains(TranslatorClass))
	{
		return true;
	}
	UInterchangeTranslatorBase* TranslatorToRegister = NewObject<UInterchangeTranslatorBase>(GetTransientPackage(), TranslatorClass, NAME_None);
	if(!TranslatorToRegister)
	{
		return false;
	}
	RegisteredTranslators.Add(TranslatorClass, TranslatorToRegister);
	return true;
}

bool UInterchangeManager::RegisterFactory(const UClass* FactoryClass)
{
	if (!FactoryClass)
	{
		return false;
	}

	UInterchangeFactoryBase* FactoryToRegister = NewObject<UInterchangeFactoryBase>(GetTransientPackage(), FactoryClass, NAME_None);
	if (!FactoryToRegister)
	{
		return false;
	}
	if (FactoryToRegister->GetFactoryClass() == nullptr || RegisteredFactories.Contains(FactoryToRegister->GetFactoryClass()))
	{
		FactoryToRegister->MarkPendingKill();
		return FactoryToRegister->GetFactoryClass() == nullptr ? false : true;
	}
	RegisteredFactories.Add(FactoryToRegister->GetFactoryClass(), FactoryToRegister);
	return true;
}

bool UInterchangeManager::RegisterWriter(const UClass* WriterClass)
{
	if (!WriterClass)
	{
		return false;
	}

	if (RegisteredFactories.Contains(WriterClass))
	{
		return true;
	}
	UInterchangeWriterBase* WriterToRegister = NewObject<UInterchangeWriterBase>(GetTransientPackage(), WriterClass, NAME_None);
	if (!WriterToRegister)
	{
		return false;
	}
	RegisteredWriters.Add(WriterClass, WriterToRegister);
	return true;
}

bool UInterchangeManager::CanTranslateSourceData(const UInterchangeSourceData* SourceData) const
{
	//Found the translator
	UE::Interchange::FScopedTranslator ScopeDataTranslator(SourceData);
	const UInterchangeTranslatorBase* SourceDataTranslator = ScopeDataTranslator.GetTranslator();
	if (SourceDataTranslator)
	{
		return true;
	}
	return false;
}

bool UInterchangeManager::ImportAsset(const FString& ContentPath, const UInterchangeSourceData* SourceData, const FImportAssetParameters& ImportAssetParameters)
{
	return ImportAssetAsync( ContentPath, SourceData, ImportAssetParameters ).IsValid();
}

UE::Interchange::FAsyncImportResult UInterchangeManager::ImportAssetAsync(const FString& ContentPath, const UInterchangeSourceData* SourceData, const FImportAssetParameters& ImportAssetParameters)
{
	FString PackageBasePath = ContentPath;
	if(!ImportAssetParameters.ReimportAsset)
	{
		UE::Interchange::SanitizeInvalidChar(PackageBasePath);
	}

	bool bCanShowDialog = !ImportAssetParameters.bIsAutomated && !IsAttended();

	//Create a task for every source data
	UE::Interchange::FImportAsyncHelperData TaskData;
	TaskData.bIsAutomated = ImportAssetParameters.bIsAutomated;
	TaskData.ImportType = UE::Interchange::EImportType::ImportType_Asset;
	TaskData.ReimportObject = ImportAssetParameters.ReimportAsset;
	TWeakPtr<UE::Interchange::FImportAsyncHelper, ESPMode::ThreadSafe> WeakAsyncHelper = CreateAsyncHelper(TaskData);
	TSharedPtr<UE::Interchange::FImportAsyncHelper, ESPMode::ThreadSafe> AsyncHelper = WeakAsyncHelper.Pin();
	check(AsyncHelper.IsValid());

	FText TitleText = NSLOCTEXT("Interchange", "Asynchronous_import_start", "Importing");
	if(!Notification.IsValid())
	{
		FAsyncTaskNotificationConfig NotificationConfig;
		NotificationConfig.bIsHeadless = false;
		NotificationConfig.bKeepOpenOnFailure = true;
		NotificationConfig.TitleText = TitleText;
		NotificationConfig.LogCategory = InternalInterchangePrivate::GetLogInterchangePtr();
		NotificationConfig.bCanCancel.Set(true);
		NotificationConfig.bKeepOpenOnFailure.Set(true);

		Notification = MakeShared<FAsyncTaskNotification>(NotificationConfig);
		Notification->SetNotificationState(FAsyncNotificationStateData(TitleText, FText::GetEmpty(), EAsyncTaskNotificationState::Pending));
	}
	
	//Create a duplicate of the source data, we need to be multithread safe so we copy it to control the life cycle. The async helper will hold it and delete it when the import task will be completed.
	UInterchangeSourceData* DuplicateSourceData = Cast<UInterchangeSourceData>(StaticDuplicateObject(SourceData, GetTransientPackage()));
	//Array of source data to build one graph per source
	AsyncHelper->SourceDatas.Add(DuplicateSourceData);
		

	//Get all the translators for the source datas
	for (int32 SourceDataIndex = 0; SourceDataIndex < AsyncHelper->SourceDatas.Num(); ++SourceDataIndex)
	{
		ensure(AsyncHelper->Translators.Add(GetTranslatorForSourceData(AsyncHelper->SourceDatas[SourceDataIndex])) == SourceDataIndex);
	}

	//Create the node graphs for each source data (StrongObjectPtr has to be created on the main thread)
	for (int32 SourceDataIndex = 0; SourceDataIndex < AsyncHelper->SourceDatas.Num(); ++SourceDataIndex)
	{
		AsyncHelper->BaseNodeContainers.Add(TStrongObjectPtr<UInterchangeBaseNodeContainer>(NewObject<UInterchangeBaseNodeContainer>(GetTransientPackage(), NAME_None)));
		check(AsyncHelper->BaseNodeContainers[SourceDataIndex].IsValid());
	}

	if ( ImportAssetParameters.OverridePipeline == nullptr )
	{
		//Get all pipeline candidate we want for this import
		TArray<UClass*> PipelineCandidates;
		FindPipelineCandidate(PipelineCandidates);

		// Stack all pipelines, for this import proto. TODO: We need to be able to control which pipeline we use for the import
		// It can be set in the project settings and can also be set by a UI where the user create the pipeline stack he want.
		// This should be a list of available pipelines that can be drop into a stack where you can control the order.
		for (int32 GraphPipelineIndex = 0; GraphPipelineIndex < PipelineCandidates.Num(); ++GraphPipelineIndex)
		{
			UInterchangePipelineBase* GeneratedPipeline = NewObject<UInterchangePipelineBase>(GetTransientPackage(), PipelineCandidates[GraphPipelineIndex], NAME_None, RF_NoFlags);
			AsyncHelper->Pipelines.Add(GeneratedPipeline);
		}
	}
	else
	{
		AsyncHelper->Pipelines.Add(ImportAssetParameters.OverridePipeline);
	}

	//Create/Start import tasks
	FGraphEventArray PipelinePrerequistes;
	check(AsyncHelper->Translators.Num() == AsyncHelper->SourceDatas.Num());
	for (int32 SourceDataIndex = 0; SourceDataIndex < AsyncHelper->SourceDatas.Num(); ++SourceDataIndex)
	{
		int32 TranslatorTaskIndex = AsyncHelper->TranslatorTasks.Add(TGraphTask<UE::Interchange::FTaskTranslator>::CreateTask().ConstructAndDispatchWhenReady(SourceDataIndex, WeakAsyncHelper));
		PipelinePrerequistes.Add(AsyncHelper->TranslatorTasks[TranslatorTaskIndex]);
	}
		
	FGraphEventArray GraphParsingPrerequistes;
	for(int32 GraphPipelineIndex = 0; GraphPipelineIndex < AsyncHelper->Pipelines.Num(); ++GraphPipelineIndex)
	{
		UInterchangePipelineBase* GraphPipeline = AsyncHelper->Pipelines[GraphPipelineIndex];
		TWeakObjectPtr<UInterchangePipelineBase> WeakPipelinePtr = GraphPipeline;
		int32 GraphPipelineTaskIndex = INDEX_NONE;
		GraphPipelineTaskIndex = AsyncHelper->PipelinePreImportTasks.Add(TGraphTask<UE::Interchange::FTaskPipelinePreImport>::CreateTask(&PipelinePrerequistes).ConstructAndDispatchWhenReady(WeakPipelinePtr, WeakAsyncHelper));
		//Ensure we run the pipeline in the same order we create the task, since pipeline modify the node container, its important that its not process in parallel, Adding the one we start to the prerequisites
		//is the way to go here
		PipelinePrerequistes.Add(AsyncHelper->PipelinePreImportTasks[GraphPipelineTaskIndex]);

		//Add pipeline to the graph parsing prerequisites
		GraphParsingPrerequistes.Add(AsyncHelper->PipelinePreImportTasks[GraphPipelineTaskIndex]);
	}

	if (GraphParsingPrerequistes.Num() > 0)
	{
		AsyncHelper->ParsingTask = TGraphTask<UE::Interchange::FTaskParsing>::CreateTask(&GraphParsingPrerequistes).ConstructAndDispatchWhenReady(this, PackageBasePath, WeakAsyncHelper);
	}
	else
	{
		//Fallback on the translator pipeline prerequisites (translator must be done if there is no pipeline)
		AsyncHelper->ParsingTask = TGraphTask<UE::Interchange::FTaskParsing>::CreateTask(&PipelinePrerequistes).ConstructAndDispatchWhenReady(this, PackageBasePath, WeakAsyncHelper);
	}

	//The graph parsing task will create the FCreateAssetTask that will run after them, the FAssetImportTask will call the appropriate Post asset import pipeline when the asset is completed

	return UE::Interchange::FAsyncImportResult{ AsyncHelper->RootObject.GetFuture(), AsyncHelper->RootObjectCompletionEvent };
}

bool UInterchangeManager::ImportScene(const FString& ImportContext, const UInterchangeSourceData* SourceData, bool bIsReimport, bool bIsAutomated)
{
	return false;
}

bool UInterchangeManager::ExportAsset(const UObject* Asset, bool bIsAutomated)
{
	return false;
}

bool UInterchangeManager::ExportScene(const UObject* World, bool bIsAutomated)
{
	return false;
}

UInterchangeSourceData* UInterchangeManager::CreateSourceData(const FString& InFileName)
{
	UInterchangeSourceData* SourceDataAsset = NewObject<UInterchangeSourceData>(GetTransientPackage(), NAME_None);
	if(!InFileName.IsEmpty())
	{
		SourceDataAsset->SetFilename(InFileName);
	}
	return SourceDataAsset;
}

TWeakPtr<UE::Interchange::FImportAsyncHelper, ESPMode::ThreadSafe> UInterchangeManager::CreateAsyncHelper(const UE::Interchange::FImportAsyncHelperData& Data)
{
	TSharedPtr<UE::Interchange::FImportAsyncHelper, ESPMode::ThreadSafe> AsyncHelper = MakeShared<UE::Interchange::FImportAsyncHelper, ESPMode::ThreadSafe>();
	//Copy the task data
	AsyncHelper->TaskData = Data;
	int32 AsyncHelperIndex = ImportTasks.Add(AsyncHelper);
	SetActiveMode(true);
	//Update the asynchronous notification
	if (Notification.IsValid())
	{
		int32 ImportTaskNumber = ImportTasks.Num();
		FString ImportTaskNumberStr = TEXT(" (") + FString::FromInt(ImportTaskNumber) + TEXT(")");
		Notification->SetProgressText(FText::FromString(ImportTaskNumberStr));
	}
	
	return ImportTasks[AsyncHelperIndex];
}

void UInterchangeManager::ReleaseAsyncHelper(TWeakPtr<UE::Interchange::FImportAsyncHelper, ESPMode::ThreadSafe> AsyncHelper)
{
	check(AsyncHelper.IsValid());
	ImportTasks.RemoveSingle(AsyncHelper.Pin());
	//Make sure the async helper is destroy, if not destroy its because we are canceling the import and we still have a shared ptr on it
	{
		TSharedPtr<UE::Interchange::FImportAsyncHelper, ESPMode::ThreadSafe> AsyncHelperSharedPtr = AsyncHelper.Pin();
		check(!AsyncHelperSharedPtr.IsValid() || AsyncHelperSharedPtr->bCancel);
	}

	int32 ImportTaskNumber = ImportTasks.Num();
	FString ImportTaskNumberStr = TEXT(" (") + FString::FromInt(ImportTaskNumber) + TEXT(")");
	if (ImportTaskNumber == 0)
	{
		SetActiveMode(false);

		if (Notification.IsValid())
		{
			FText TitleText = NSLOCTEXT("Interchange", "Asynchronous_import_end", "Import Done");
			//TODO make sure any error are reported so we can control success or not
			const bool bSuccess = true;
			Notification->SetComplete(TitleText, FText::GetEmpty(), bSuccess);
			Notification = nullptr; //This should delete the notification
		}
	}
	else if(Notification.IsValid())
	{
		Notification->SetProgressText(FText::FromString(ImportTaskNumberStr));
	}
}

UInterchangeTranslatorBase* UInterchangeManager::GetTranslatorForSourceData(const UInterchangeSourceData* SourceData) const
{
	if (RegisteredTranslators.Num() == 0)
	{
		return nullptr;
	}
	//Found the translator
	for (const auto Kvp : RegisteredTranslators)
	{
		if (Kvp.Value->CanImportSourceData(SourceData))
		{
			UInterchangeTranslatorBase* SourceDataTranslator = NewObject<UInterchangeTranslatorBase>(GetTransientPackage(), Kvp.Key, NAME_None);
			return SourceDataTranslator;
		}
	}
	return nullptr;
}

bool UInterchangeManager::WarnIfInterchangeIsActive()
{
	if (!bIsActive)
	{
		return false;
	}
	//Tell user he have to cancel the import before closing the editor
	FNotificationInfo Info(NSLOCTEXT("InterchangeManager", "WarnCannotProceed", "An import process is currently underway! Please cancel it to proceed!"));
	Info.ExpireDuration = 5.0f;
	TSharedPtr<SNotificationItem> WarnNotification = FSlateNotificationManager::Get().AddNotification(Info);
	if (WarnNotification.IsValid())
	{
		WarnNotification->SetCompletionState(SNotificationItem::CS_Fail);
	}
	return true;
}

bool UInterchangeManager::IsAttended()
{
	if (FApp::IsGame())
	{
		return false;
	}
	if (FApp::IsUnattended())
	{
		return false;
	}
	return true;
}

void UInterchangeManager::FindPipelineCandidate(TArray<UClass*>& PipelineCandidates)
{
	//Find in memory pipeline class
	for (TObjectIterator< UClass > ClassIt; ClassIt; ++ClassIt)
	{
		UClass* Class = *ClassIt;
		// Only interested in native C++ classes
// 		if (!Class->IsNative())
// 		{
// 			continue;
// 		}
		// Ignore deprecated
		if (Class->HasAnyClassFlags(CLASS_Deprecated | CLASS_NewerVersionExists))
		{
			continue;
		}

		// Check this class is a subclass of Base and not the base itself
		if (Class == UInterchangePipelineBase::StaticClass() || !Class->IsChildOf(UInterchangePipelineBase::StaticClass()))
		{
			continue;
		}

		//We found a candidate
		PipelineCandidates.AddUnique(Class);
	}

//Blueprint and python script discoverability is available only if we compile with the engine
	// Load the asset registry module
	FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked< FAssetRegistryModule >(FName("AssetRegistry"));
	IAssetRegistry& AssetRegistry = AssetRegistryModule.Get();

	TArray< FString > ContentPaths;
	ContentPaths.Add(TEXT("/Game"));
	//TODO do we have an other alternative, this call is synchronous and will wait unitl the registry database have finish the initial scan. If there is a lot of asset it can take multiple second the first time we call it.
	AssetRegistry.ScanPathsSynchronous(ContentPaths);

	FName BaseClassName = UInterchangePipelineBase::StaticClass()->GetFName();

	// Use the asset registry to get the set of all class names deriving from Base
	TSet< FName > DerivedNames;
	{
		TArray< FName > BaseNames;
		BaseNames.Add(BaseClassName);

		TSet< FName > Excluded;
		AssetRegistry.GetDerivedClassNames(BaseNames, Excluded, DerivedNames);
	}

	FARFilter Filter;
	Filter.ClassNames.Add(UBlueprint::StaticClass()->GetFName());
	Filter.bRecursiveClasses = true;
	Filter.bRecursivePaths = true;

	TArray< FAssetData > AssetList;
	AssetRegistry.GetAssets(Filter, AssetList);

	// Iterate over retrieved blueprint assets
	for (const FAssetData& Asset : AssetList)
	{
		//Only get the asset with the native parent class using UInterchangePipelineBase
		FAssetDataTagMapSharedView::FFindTagResult GeneratedClassPath = Asset.TagsAndValues.FindTag(TEXT("GeneratedClass"));
		if (GeneratedClassPath.IsSet())
		{
			// Convert path to just the name part
			const FString ClassObjectPath = FPackageName::ExportTextPathToObjectPath(*GeneratedClassPath.GetValue());
			const FString ClassName = FPackageName::ObjectPathToObjectName(ClassObjectPath);

			// Check if this class is in the derived set
			if (!DerivedNames.Contains(*ClassName))
			{
				continue;
			}

			UBlueprint* Blueprint = Cast<UBlueprint>(Asset.GetAsset());
			check(Blueprint);
			check(Blueprint->ParentClass == UInterchangePipelineBase::StaticClass());
			PipelineCandidates.AddUnique(Blueprint->GeneratedClass);
		}
	}
}

void UInterchangeManager::CancelAllTasks()
{
	check(IsInGameThread());

	//Set the cancel state on all task
	int32 ImportTaskCount = ImportTasks.Num();
	for (int32 TaskIndex = 0; TaskIndex < ImportTaskCount; ++TaskIndex)
	{
		TSharedPtr<UE::Interchange::FImportAsyncHelper, ESPMode::ThreadSafe> AsyncHelper = ImportTasks[TaskIndex];
		if (AsyncHelper.IsValid())
		{
			AsyncHelper->InitCancel();
		}
	}

	//Tasks should all finish quite fast now
};

void UInterchangeManager::CancelAllTasksSynchronously()
{
	//Start the cancel process by cancelling all current task
	CancelAllTasks();

	//Now wait for each task to be completed on the main thread
	while (ImportTasks.Num() > 0)
	{
		int32 ImportTaskCount = ImportTasks.Num();
		TSharedPtr<UE::Interchange::FImportAsyncHelper, ESPMode::ThreadSafe> AsyncHelper = ImportTasks[0];
		if (AsyncHelper.IsValid())
		{
			//Cancel any on going interchange activity this is blocking but necessary.
			AsyncHelper->CancelAndWaitUntilDoneSynchronously();
			ensure(ImportTaskCount > ImportTasks.Num());
			TWeakPtr<UE::Interchange::FImportAsyncHelper, ESPMode::ThreadSafe> WeakAsyncHelper = AsyncHelper;
			//free the async helper
			AsyncHelper = nullptr;
			//We verify that the weak pointer is invalid after releasing the async helper
			ensure(!WeakAsyncHelper.IsValid());
		}
	}
}

void UInterchangeManager::SetActiveMode(bool IsActive)
{
	if (bIsActive == IsActive)
	{
		return;
	}

	bIsActive = IsActive;
	if (bIsActive)
	{
		ensure(!NotificationTickHandle.IsValid());
		NotificationTickHandle = FTicker::GetCoreTicker().AddTicker(TEXT("InterchangeManagerTickHandle"), 0.1f, [this](float)
		{
			if (Notification.IsValid() && Notification->GetPromptAction() == EAsyncTaskNotificationPromptAction::Cancel)
			{
				CancelAllTasks();
			}
			return true;
		});

		//Block GC in a different thread then game thread
		FString ThreadName = FString(TEXT("InterchangeGCGuard"));
		GcGuardThread = FThread(*ThreadName, [this]()
		{
			FGCScopeGuard GCScopeGuard;
			while (bIsActive && ImportTasks.Num() > 0)
			{
				FPlatformProcess::Sleep(0.01f);
			}
		});
	}
	else
	{
		FTicker::GetCoreTicker().RemoveTicker(NotificationTickHandle);
		NotificationTickHandle.Reset();

		if (GcGuardThread.IsJoinable())
		{
			//Finish the thread
			GcGuardThread.Join();
		}
	}
}