// Copyright Epic Games, Inc. All Rights Reserved.

#include "AssetSearchManager.h"
#include "IAssetRegistry.h"
#include "AssetRegistryModule.h"
#include "AssetSearchDatabase.h"
#include "Async/Async.h"
#include "DerivedDataCacheInterface.h"

#include "Indexers/DataAssetIndexer.h"
#include "Indexers/DataTableIndexer.h"
#include "Indexers/BlueprintIndexer.h"
#include "Indexers/WidgetBlueprintIndexer.h"
#include "Containers/StringConv.h"
#include "Containers/Ticker.h"
#include "Misc/Paths.h"
#include "HAL/RunnableThread.h"
#include "StudioAnalytics.h"
#include "AnalyticsEventAttribute.h"
#include "Misc/FeedbackContext.h"
#include "WidgetBlueprint.h"
#include "Engine/Blueprint.h"
#include "Engine/DataTable.h"
#include "Engine/DataAsset.h"
#include "Indexers/DialogueWaveIndexer.h"
#include "Sound/DialogueWave.h"
#include "Indexers/LevelIndexer.h"
#include "Settings/AssetSearchDeveloperSettings.h"
#include "Indexers/SoundCueIndexer.h"
#include "Sound/SoundCue.h"
#include "Misc/ScopedSlowTask.h"
#include "Misc/StringBuilder.h"
#include "Engine/World.h"
#include "Editor.h"

PRAGMA_DISABLE_OPTIMIZATION

#define LOCTEXT_NAMESPACE "FAssetSearchManager"

static bool bIndexUnindexAssetsOnLoad = false;
FAutoConsoleVariableRef CVarIndexUnindexAssetsOnLoad(
	TEXT("Search.IndexUnindexAssetsOnLoad"),
	bIndexUnindexAssetsOnLoad,
	TEXT("Index Unindex Assets On Load")
);

static int32 PendingDownloadsMax = 100;
FAutoConsoleVariableRef CVarPendingDownloadsMax(
	TEXT("Search.PendingDownloadsMax"),
	PendingDownloadsMax,
	TEXT("")
);

static int32 GameThread_DownloadProcessLimit = 30;
FAutoConsoleVariableRef CVarGameThread_DownloadProcessLimit(
	TEXT("Search.GameThread_DownloadProcessLimit"),
	GameThread_DownloadProcessLimit,
	TEXT("")
);

static int32 GameThread_AssetScanLimit = 1000;
FAutoConsoleVariableRef CVarGameThread_AssetScanLimit(
	TEXT("Search.GameThread_AssetScanLimit"),
	GameThread_AssetScanLimit,
	TEXT("")
);

FAssetSearchManager::FAssetSearchManager()
{
	PendingDatabaseUpdates = 0;
	PendingDownloads = 0;
	TotalSearchRecords = 0;
	LastRecordCountUpdateSeconds = 0;

	RunThread = false;
}

FAssetSearchManager::~FAssetSearchManager()
{
	RunThread = false;
	DatabaseThread->WaitForCompletion();

	{
		FScopeLock ScopedLock(&SearchDatabaseCS);
		FCoreUObjectDelegates::OnObjectSaved.RemoveAll(this);
		FCoreUObjectDelegates::OnAssetLoaded.RemoveAll(this);
		UObject::FAssetRegistryTag::OnGetExtraObjectTags.RemoveAll(this);

		//FModuleManager::GetModule<FAssetRegistryModule>("AssetRegistry");

		FTicker::GetCoreTicker().RemoveTicker(TickerHandle);
	}
}

void FAssetSearchManager::Start()
{
	RegisterAssetIndexer(UDataAsset::StaticClass(), MakeUnique<FDataAssetIndexer>());
	RegisterAssetIndexer(UDataTable::StaticClass(), MakeUnique<FDataTableIndexer>());
	RegisterAssetIndexer(UBlueprint::StaticClass(), MakeUnique<FBlueprintIndexer>());
	RegisterAssetIndexer(UWidgetBlueprint::StaticClass(), MakeUnique<FWidgetBlueprintIndexer>());
	RegisterAssetIndexer(UDialogueWave::StaticClass(), MakeUnique<FDialogueWaveIndexer>());
	RegisterAssetIndexer(UWorld::StaticClass(), MakeUnique<FLevelIndexer>());
	RegisterAssetIndexer(USoundCue::StaticClass(), MakeUnique<FSoundCueIndexer>());


	const FString SessionPath = FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("Search")));
	SearchDatabase.Open(SessionPath);

	FCoreUObjectDelegates::OnObjectSaved.AddRaw(this, &FAssetSearchManager::OnObjectSaved);
	FCoreUObjectDelegates::OnAssetLoaded.AddRaw(this, &FAssetSearchManager::OnAssetLoaded);

	IAssetRegistry& AssetRegistry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry").Get();
	AssetRegistry.OnAssetAdded().AddRaw(this, &FAssetSearchManager::OnAssetAdded);
	AssetRegistry.OnAssetRemoved().AddRaw(this, &FAssetSearchManager::OnAssetRemoved);
	AssetRegistry.OnFilesLoaded().AddRaw(this, &FAssetSearchManager::OnAssetScanFinished);
	
	TArray<FAssetData> TempAssetData;
	AssetRegistry.GetAllAssets(TempAssetData, true);

	for (const FAssetData& Data : TempAssetData)
	{
		OnAssetAdded(Data);
	}

	TickerHandle = FTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateRaw(this, &FAssetSearchManager::Tick_GameThread), 0);

	RunThread = true;
	DatabaseThread = FRunnableThread::Create(this, TEXT("UniversalSearch"), 0, TPri_BelowNormal);
}

FSearchStats FAssetSearchManager::GetStats() const
{
	FSearchStats Stats;
	Stats.Scanning = ProcessAssetQueue.Num();
	Stats.Downloading = PendingDownloads;
	Stats.PendingDatabaseUpdates = PendingDatabaseUpdates;
	Stats.TotalRecords = TotalSearchRecords;
	Stats.AssetsMissingIndex = FailedDDCRequests.Num();
	return Stats;
}

void FAssetSearchManager::RegisterAssetIndexer(const UClass* AssetClass, TUniquePtr<IAssetIndexer>&& Indexer)
{
	check(IsInGameThread());

	Indexers.Add(AssetClass->GetFName(), MoveTemp(Indexer));
}

void FAssetSearchManager::OnAssetAdded(const FAssetData& InAssetData)
{
	check(IsInGameThread());

	static const FString DeveloperPathWithSlash = FPackageName::FilenameToLongPackageName(FPaths::GameDevelopersDir());
	static const FString UsersDeveloperPathWithSlash = FPackageName::FilenameToLongPackageName(FPaths::GameUserDeveloperDir());
	
	// Don't process stuff in the other developer folders.
	FString PackageName = InAssetData.PackageName.ToString();
	if (PackageName.StartsWith(DeveloperPathWithSlash))
	{
		if (!PackageName.StartsWith(UsersDeveloperPathWithSlash))
		{
			return;
		}
	}

	// 
	const UAssetSearchDeveloperSettings* Settings = GetDefault<UAssetSearchDeveloperSettings>();
	for (const FDirectoryPath& IgnoredPath : Settings->IgnoredPaths)
	{
		if (PackageName.StartsWith(IgnoredPath.Path))
		{
			return;
		}
	}

	// Don't index redirectors, just act like they don't exist.
	if (InAssetData.IsRedirector())
	{
		return;
	}

	FAssetOperation Operation;
	Operation.Asset = InAssetData;
	ProcessAssetQueue.Add(Operation);
}

void FAssetSearchManager::OnAssetRemoved(const FAssetData& InAssetData)
{
	check(IsInGameThread());

	FAssetOperation Operation;
	Operation.Asset = InAssetData;
	Operation.bRemoval = true;
	ProcessAssetQueue.Add(Operation);
}

void FAssetSearchManager::OnAssetScanFinished()
{
	check(IsInGameThread());

	TArray<FAssetData> AllAssets;
	IAssetRegistry& AssetRegistry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry").Get();
	AssetRegistry.GetAllAssets(AllAssets, false);
	
	PendingDatabaseUpdates++;
	UpdateOperations.Enqueue([this, AssetsAvailable = MoveTemp(AllAssets)]() mutable {
		FScopeLock ScopedLock(&SearchDatabaseCS);
		SearchDatabase.RemoveAssetsNotInThisSet(AssetsAvailable);
		PendingDatabaseUpdates--;
	});
}

void FAssetSearchManager::OnObjectSaved(UObject* InObject)
{
	check(IsInGameThread());

	if (!GIsCookerLoadingPackage)
	{
		RequestIndexAsset(InObject);
	}
}

void FAssetSearchManager::OnAssetLoaded(UObject* InObject)
{
	check(IsInGameThread());

	if (bIndexUnindexAssetsOnLoad)
	{
		RequestIndexAsset(InObject);
	}
}

bool FAssetSearchManager::RequestIndexAsset(UObject* InAsset)
{
	check(IsInGameThread());

	if (GEditor->IsAutosaving())
	{
		return false;
	}

	if (IsAssetIndexable(InAsset))
	{
		TWeakObjectPtr<UObject> AssetWeakPtr = InAsset;
		FAssetData AssetData(InAsset);

		return AsyncGetDerivedDataKey(AssetData, [this, AssetData, AssetWeakPtr](FString InDDCKey) {
			UpdateOperations.Enqueue([this, AssetData, AssetWeakPtr, InDDCKey]() {
				FScopeLock ScopedLock(&SearchDatabaseCS);
				if (!SearchDatabase.IsAssetUpToDate(AssetData, InDDCKey))
				{
					AsyncTask(ENamedThreads::GameThread, [this, AssetWeakPtr]() {
						StoreIndexForAsset(AssetWeakPtr.Get());
					});
				}
			});
		});
	}

	return false;
}

bool FAssetSearchManager::IsAssetIndexable(UObject* InAsset)
{
	if (InAsset && InAsset->IsAsset())
	{
		// If it's not a permanent package, and one we just loaded for diffing, don't index it.
		UPackage* Package = InAsset->GetOutermost();
		if (Package->HasAnyPackageFlags(/*LOAD_ForDiff | */LOAD_PackageForPIE | LOAD_ForFileDiff))
		{
			return false;
		}

		if (InAsset->HasAnyFlags(RF_Transient))
		{
			return false;
		}

		return true;
	}

	return false;
}

bool FAssetSearchManager::TryLoadIndexForAsset(const FAssetData& InAssetData)
{
	return AsyncGetDerivedDataKey(InAssetData, [this, InAssetData](FString InDDCKey) {
		FeedOperations.Enqueue([this, InAssetData, InDDCKey]() {
			FScopeLock ScopedLock(&SearchDatabaseCS);
			if (!SearchDatabase.IsAssetUpToDate(InAssetData, InDDCKey))
			{
				PendingDownloads++;

				FAssetDDCRequest DDCRequest;
				DDCRequest.AssetData = InAssetData;
				DDCRequest.DDCKey_IndexDataHash = InDDCKey;
				DDCRequest.DDCHandle = GetDerivedDataCacheRef().GetAsynchronous(*InDDCKey, InAssetData.ObjectPath.ToString());
				ProcessDDCQueue.Enqueue(DDCRequest);
			}
		});
	});
}

bool FAssetSearchManager::AsyncGetDerivedDataKey(const FAssetData& InAssetData, TFunction<void(FString)> DDCKeyCallback)
{
	check(IsInGameThread());

	FString IndexersNamesAndVersions = GetIndexerVersion(InAssetData.GetClass());

	// If the indexer names and versions is empty, then we know it's not possible to index this type of thing.
	if (IndexersNamesAndVersions.IsEmpty())
	{
		return false;
	}

	UpdateOperations.Enqueue([this, InAssetData, IndexersNamesAndVersions, DDCKeyCallback]() {
		FScopeLock ScopedLock(&SearchDatabaseCS);

		FAssetFileInfo FileInfo;
		SearchDatabase.AddOrUpdateFileInfo(InAssetData, FileInfo);

		if (FileInfo.Hash.IsValid())
		{
			// The universal key for content is:
			// AssetSearch_V{SerializerVersion}_{IndexersNamesAndVersions}_{ObjectPathHash}_{FileOnDiskHash}

			const FString ObjectPathString = InAssetData.ObjectPath.ToString();

			FSHAHash ObjectPathHash;
			FSHA1::HashBuffer(*ObjectPathString, ObjectPathString.Len() * sizeof(FString::ElementType), ObjectPathHash.Hash);

			TStringBuilder<512> DDCKey;
			DDCKey.Append(TEXT("AssetSearch_V"));
			DDCKey.Append(LexToString(FSearchSerializer::GetVersion()));
			DDCKey.Append(TEXT("_"));
			DDCKey.Append(IndexersNamesAndVersions);
			DDCKey.Append(TEXT("_"));
			DDCKey.Append(ObjectPathHash.ToString());
			DDCKey.Append(TEXT("_"));
			DDCKey.Append(LexToString(FileInfo.Hash));

			const FString DDCKeyString = DDCKey.ToString();
			AsyncTask(ENamedThreads::GameThread, [this, DDCKeyString, DDCKeyCallback]() {
				DDCKeyCallback(DDCKeyString);
			});
		}
	});

	return true;
}

bool FAssetSearchManager::HasIndexerForClass(const UClass* InAssetClass) const
{
	const UClass* IndexableClass = InAssetClass;
	while (IndexableClass)
	{
		if (Indexers.Contains(IndexableClass->GetFName()))
		{
			return true;
		}

		IndexableClass = IndexableClass->GetSuperClass();
	}

	return false;
}

FString FAssetSearchManager::GetIndexerVersion(const UClass* InAssetClass) const
{
	TStringBuilder<256> VersionString;

	TArray<UClass*> NestedIndexedTypes;

	const UClass* IndexableClass = InAssetClass;
	while (IndexableClass)
	{
		if (const TUniquePtr<IAssetIndexer>* IndexerPtr = Indexers.Find(IndexableClass->GetFName()))
		{
			IAssetIndexer* Indexer = IndexerPtr->Get();
			VersionString.Append(Indexer->GetName());
			VersionString.Append(TEXT("_"));
			VersionString.Append(LexToString(Indexer->GetVersion()));

			Indexer->GetNestedAssetTypes(NestedIndexedTypes);
		}

		IndexableClass = IndexableClass->GetSuperClass();
	}

	for (UClass* NestedIndexedType : NestedIndexedTypes)
	{
		VersionString.Append(GetIndexerVersion(NestedIndexedType));
	}

	return VersionString.ToString();
}

void FAssetSearchManager::StoreIndexForAsset(UObject* InAsset)
{
	check(IsInGameThread());

	if (IsAssetIndexable(InAsset) && HasIndexerForClass(InAsset->GetClass()))
	{
		FAssetData InAssetData(InAsset);

		FString IndexedJson;
		bool bWasIndexed = false;
		{
			FSearchSerializer Serializer(InAssetData, &IndexedJson);
			bWasIndexed = Serializer.IndexAsset(InAsset, Indexers);
		}

		if (bWasIndexed && !IndexedJson.IsEmpty())
		{
			AsyncGetDerivedDataKey(InAssetData, [this, InAssetData, IndexedJson](FString InDDCKey) {
				check(IsInGameThread());

				FTCHARToUTF8 IndexedJsonUTF8(*IndexedJson);
				TArrayView<const uint8> IndexedJsonUTF8View((const uint8*)IndexedJsonUTF8.Get(), IndexedJsonUTF8.Length() * sizeof(UTF8CHAR));
				GetDerivedDataCacheRef().Put(*InDDCKey, IndexedJsonUTF8View, InAssetData.ObjectPath.ToString(), false);

				AddOrUpdateAsset(InAssetData, IndexedJson, InDDCKey);
			});
		}
	}
}

void FAssetSearchManager::LoadDDCContentIntoDatabase(const FAssetData& InAsset, const TArray<uint8>& Content, const FString& DerivedDataKey)
{
	FUTF8ToTCHAR WByteBuffer((const ANSICHAR*)Content.GetData(), Content.Num());
	FString IndexedJson(WByteBuffer.Length(), WByteBuffer.Get());

	AddOrUpdateAsset(InAsset, IndexedJson, DerivedDataKey);
}

void FAssetSearchManager::AddOrUpdateAsset(const FAssetData& InAssetData, const FString& IndexedJson, const FString& DerivedDataKey)
{
	check(IsInGameThread());

	PendingDatabaseUpdates++;
	UpdateOperations.Enqueue([this, InAssetData, IndexedJson, DerivedDataKey]() {
		FScopeLock ScopedLock(&SearchDatabaseCS);
		SearchDatabase.AddOrUpdateAsset(InAssetData, IndexedJson, DerivedDataKey);
		PendingDatabaseUpdates--;
	});
}

bool FAssetSearchManager::Tick_GameThread(float DeltaTime)
{
	check(IsInGameThread());

	//if (0)
	//{
	//	IAssetRegistry& AssetRegistry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry").Get();
	//	TArray<FAssetData> TempAssetData;
	//	AssetRegistry.GetAllAssets(TempAssetData, true);

	//	for (const FAssetData& Data : TempAssetData)
	//	{
	//		OnAssetAdded(Data);
	//	}
	//}

	int32 ScanLimit = GameThread_AssetScanLimit;
	while (ProcessAssetQueue.Num() > 0 && ScanLimit > 0 && PendingDownloads < PendingDownloadsMax)
	{
		FAssetOperation Operation = ProcessAssetQueue.Pop(false);
		FAssetData Asset = Operation.Asset;

		if (Operation.bRemoval)
		{
			PendingDatabaseUpdates++;
			UpdateOperations.Enqueue([this, Asset]() {
				FScopeLock ScopedLock(&SearchDatabaseCS);
				SearchDatabase.RemoveAsset(Asset);
				PendingDatabaseUpdates--;
			});
		}
		else
		{
			if (TryLoadIndexForAsset(Asset))
			{
				ScanLimit -= 10;
			}
		}

		ScanLimit--;
	}

	int32 DownloadProcessLimit = GameThread_DownloadProcessLimit;
	while (!ProcessDDCQueue.IsEmpty() && DownloadProcessLimit > 0)
	{
		const FAssetDDCRequest* PendingRequest = ProcessDDCQueue.Peek();
		if (GetDerivedDataCacheRef().PollAsynchronousCompletion(PendingRequest->DDCHandle))
		{
			bool bDataWasBuilt;

			TArray<uint8> OutContent;
			bool bGetSuccessful = GetDerivedDataCacheRef().GetAsynchronousResults(PendingRequest->DDCHandle, OutContent, &bDataWasBuilt);
			if (bGetSuccessful)
			{
				LoadDDCContentIntoDatabase(PendingRequest->AssetData, OutContent, PendingRequest->DDCKey_IndexDataHash);
			}
			else
			{
				FailedDDCRequests.Add(*PendingRequest);
			}

			ProcessDDCQueue.Pop();
			PendingDownloads--;
			DownloadProcessLimit--;
			continue;
		}
		break;
	}

	if ((FPlatformTime::Seconds() - LastRecordCountUpdateSeconds) > 30)
	{
		LastRecordCountUpdateSeconds = FPlatformTime::Seconds();

		ImmediateOperations.Enqueue([this]() {
			FScopeLock ScopedLock(&SearchDatabaseCS);
			TotalSearchRecords = SearchDatabase.GetTotalSearchRecords();
		});
	}

	return true;
}

uint32 FAssetSearchManager::Run()
{
	Tick_DatabaseOperationThread();
	return 0;
}

void FAssetSearchManager::Tick_DatabaseOperationThread()
{
	while (RunThread)
	{
		TFunction<void()> Operation;
		if (ImmediateOperations.Dequeue(Operation) || FeedOperations.Dequeue(Operation) || UpdateOperations.Dequeue(Operation))
		{
			Operation();
		}
		else
		{
			FPlatformProcess::Sleep(0.1);
		}
	}
}

void FAssetSearchManager::ForceIndexOnAssetsMissingIndex()
{
	check(IsInGameThread());

	FScopedSlowTask IndexingTask(FailedDDCRequests.Num(), LOCTEXT("ForceIndexOnAssetsMissingIndex", "Indexing Assets"));
	IndexingTask.MakeDialog(true);

	int32 RemovedCount = 0;
	for (const FAssetDDCRequest& Request : FailedDDCRequests)
	{
		if (IndexingTask.ShouldCancel())
		{
			break;
		}

		IndexingTask.EnterProgressFrame(1, FText::Format(LOCTEXT("ForceIndexOnAssetsMissingIndexFormat", "Indexing Asset ({0} of {1})"), RemovedCount + 1, FailedDDCRequests.Num()));
		if (UObject* AssetToIndex = Request.AssetData.GetAsset())
		{
			StoreIndexForAsset(AssetToIndex);
		}

		RemovedCount++;
	}

	FailedDDCRequests.RemoveAtSwap(0, RemovedCount);
}

void FAssetSearchManager::Search(const FSearchQuery& Query, TFunction<void(TArray<FSearchRecord>&&)> InCallback)
{
	check(IsInGameThread());

	FStudioAnalytics::ReportEvent(TEXT("AssetSearch"), {
		FAnalyticsEventAttribute(TEXT("QueryString"), Query.Query)
	});

	ImmediateOperations.Enqueue([this, Query, InCallback]() {

		TArray<FSearchRecord> Results;

		{
			FScopeLock ScopedLock(&SearchDatabaseCS);
			SearchDatabase.EnumerateSearchResults(Query, [&Results](FSearchRecord&& InResult) {
				Results.Add(MoveTemp(InResult));
				return true;
			});
		}

		AsyncTask(ENamedThreads::GameThread, [ResultsFwd = MoveTemp(Results), InCallback]() mutable {
			InCallback(MoveTemp(ResultsFwd));
		});
	});
}

#undef LOCTEXT_NAMESPACE

PRAGMA_ENABLE_OPTIMIZATION