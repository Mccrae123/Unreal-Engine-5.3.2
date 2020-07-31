// Copyright Epic Games, Inc. All Rights Reserved.

#include "TextureCompiler.h"
#include "Engine/Texture.h"

#if WITH_EDITOR

#include "Framework/Notifications/NotificationManager.h"
#include "Settings/EditorExperimentalSettings.h"
#include "Widgets/Notifications/SNotificationList.h"
#include "Misc/QueuedThreadPoolWrapper.h"
#include "RendererInterface.h"
#include "EngineModule.h"
#include "Misc/ScopedSlowTask.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/UObjectIterator.h"
#include "Materials/MaterialInterface.h"
#include "Materials/Material.h"
#include "TextureDerivedDataTask.h"
#include "Misc/IQueuedWork.h"

#define LOCTEXT_NAMESPACE "TextureCompiler"

static TAutoConsoleVariable<int32> CVarAsyncTextureCompilation(
	TEXT("Editor.AsyncTextureCompilation"),
	0,
	TEXT("0 - Async texture compilation is disabled.\n")
	TEXT("1 - Async texture compilation is enabled.\n")
	TEXT("2 - Async texture compilation is enabled but on pause (for debugging).\n")
	TEXT("When enabled, textures will be replaced by placeholders until they are ready\n")
	TEXT("to reduce stalls on the game thread and improve overall editor performance."),
	ECVF_Default);

static TAutoConsoleVariable<int32> CVarAsyncTextureCompilationMaxConcurrency(
	TEXT("Editor.AsyncTextureCompilationMaxConcurrency"),
	-1,
	TEXT("Set the maximum number of concurrent texture compilation, -1 for unlimited."),
	ECVF_Default);

static FAutoConsoleCommand CVarAsyncTextureCompilationFinishAll(
	TEXT("Editor.AsyncTextureCompilationFinishAll"),
	TEXT("Finish all texture compilations"),
	FConsoleCommandWithArgsDelegate::CreateLambda([](const TArray<FString>& Args)
	{
		FTextureCompilingManager::Get().FinishAllCompilation();
	})
);

static TAutoConsoleVariable<int32> CVarAsyncTextureCompilationResume(
	TEXT("Editor.AsyncTextureCompilationResume"),
	0,
	TEXT("Number of queued work to resume while paused."),
	ECVF_Default);

namespace TextureCompilingManagerImpl
{
	static FString GetLODGroupName(UTexture* Texture)
	{
		return StaticEnum<TextureGroup>()->GetMetaData(TEXT("DisplayName"), Texture->LODGroup);
	}

	static TMultiMap<UObject*, UMaterialInterface*> GetTexturesAffectingMaterialInterfaces()
	{
		TRACE_CPUPROFILER_EVENT_SCOPE(GetTexturesAffectingMaterials);

		// Update any material that uses this texture
		TMultiMap<UObject*, UMaterialInterface*> TexturesRequiringMaterialUpdate;

		TArray<UTexture*> UsedTextures;
		for (TObjectIterator<UMaterialInterface> It; It; ++It)
		{
			UsedTextures.Reset();

			UMaterialInterface* MaterialInterface = *It;
			for (UObject* Texture : MaterialInterface->GetReferencedTextures())
			{
				TexturesRequiringMaterialUpdate.Emplace(Texture, MaterialInterface);
			}
		}

		return TexturesRequiringMaterialUpdate;
	}

	static EQueuedWorkPriority GetBasePriority(UTexture* InTexture)
	{
		switch (InTexture->LODGroup)
		{
		case TEXTUREGROUP_UI:
			return EQueuedWorkPriority::High;
		case TEXTUREGROUP_Terrain_Heightmap:
			return EQueuedWorkPriority::Normal;
		default:
			return EQueuedWorkPriority::Lowest;
		}
	}

	static EQueuedWorkPriority GetBoostPriority(UTexture* InTexture)
	{
		return (EQueuedWorkPriority)((uint8)GetBasePriority(InTexture) - 1);
	}

	static const TCHAR* GetPriorityName(EQueuedWorkPriority Priority)
	{
		switch (Priority)
		{
			case EQueuedWorkPriority::Highest:
				return TEXT("Highest");
			case EQueuedWorkPriority::High:
				return TEXT("High");
			case EQueuedWorkPriority::Normal:
				return TEXT("Normal");
			case EQueuedWorkPriority::Low:
				return TEXT("Low");
			case EQueuedWorkPriority::Lowest:
				return TEXT("Lowest");
			default:
				return TEXT("Unknown");
		}
	}

	static void EnsureInitializedCVars()
	{
		static bool bIsInitialized = false;

		if (!bIsInitialized)
		{
			bIsInitialized = true;
			GetMutableDefault<UEditorExperimentalSettings>()->OnSettingChanged().AddLambda(
				[](FName Name)
				{
					if (Name == TEXT("bEnableAsyncTextureCompilation"))
					{
						CVarAsyncTextureCompilation->Set(GetDefault<UEditorExperimentalSettings>()->bEnableAsyncTextureCompilation ? 1 : 0, ECVF_SetByProjectSetting);
					}
				}
			);

			CVarAsyncTextureCompilation->Set(GetDefault<UEditorExperimentalSettings>()->bEnableAsyncTextureCompilation ? 1 : 0, ECVF_SetByProjectSetting);

			FString Value;
			if (FParse::Value(FCommandLine::Get(), TEXT("-asynctexturecompilation="), Value))
			{
				int32 AsyncTextureCompilationValue = 0;
				if (Value == TEXT("1") || Value == TEXT("on"))
				{
					AsyncTextureCompilationValue = 1;
				}

				if (Value == TEXT("2") || Value == TEXT("paused"))
				{
					AsyncTextureCompilationValue = 2;
				}

				CVarAsyncTextureCompilation->Set(AsyncTextureCompilationValue, ECVF_SetByCommandline);
			}

			int32 MaxConcurrency = -1;
			if (FParse::Value(FCommandLine::Get(), TEXT("-asynctexturecompilationmaxconcurrency="), MaxConcurrency))
			{
				CVarAsyncTextureCompilationMaxConcurrency->Set(MaxConcurrency, ECVF_SetByCommandline);
			}
		}
	}
}

EQueuedWorkPriority FTextureCompilingManager::GetBasePriority(UTexture* InTexture) const
{
	return TextureCompilingManagerImpl::GetBasePriority(InTexture);
}

FQueuedThreadPool* FTextureCompilingManager::GetThreadPool() const
{
	static FQueuedThreadPoolWrapper* GTextureThreadPool = nullptr;
	if (GTextureThreadPool == nullptr)
	{
		TextureCompilingManagerImpl::EnsureInitializedCVars();

		// Wrapping GLargeThreadPool to give TextureThreadPool it's own set of priorities and allow Pausable functionality
		// All texture priorities will resolve to a Low priority once being scheduled in the LargeThreadPool.
		const int32 MaxConcurrency = CVarAsyncTextureCompilationMaxConcurrency.GetValueOnAnyThread();
		GTextureThreadPool = new FQueuedThreadPoolWrapper(GLargeThreadPool, MaxConcurrency, [](EQueuedWorkPriority) { return EQueuedWorkPriority::Low; });

		CVarAsyncTextureCompilation->SetOnChangedCallback(
			FConsoleVariableDelegate::CreateLambda(
				[](IConsoleVariable* Variable)
				{
					if (Variable->GetInt() == 2)
					{
						GTextureThreadPool->Pause();
					}
					else
					{
						GTextureThreadPool->Resume();
					}
				}
				)
			);

		CVarAsyncTextureCompilationResume->SetOnChangedCallback(
			FConsoleVariableDelegate::CreateLambda(
				[](IConsoleVariable* Variable)
				{
					if (Variable->GetInt() > 0)
					{
						GTextureThreadPool->Resume(Variable->GetInt());
					}
				}
				)
			);

		CVarAsyncTextureCompilationMaxConcurrency->SetOnChangedCallback(
			FConsoleVariableDelegate::CreateLambda(
				[](IConsoleVariable* Variable)
				{
					GTextureThreadPool->SetMaxConcurrency(Variable->GetInt());
				}
				)
			);

		if (CVarAsyncTextureCompilation->GetInt() == 2)
		{
			GTextureThreadPool->Pause();
		}
	}

	return GTextureThreadPool;
}

bool FTextureCompilingManager::IsAsyncTextureCompilationEnabled() const
{
	TextureCompilingManagerImpl::EnsureInitializedCVars();

	return CVarAsyncTextureCompilation.GetValueOnAnyThread() != 0;
}

void FTextureCompilingManager::UpdateCompilationNotification()
{
	check(IsInGameThread());
	static TWeakPtr<SNotificationItem> TextureCompilationPtr;

	TSharedPtr<SNotificationItem> NotificationItem = TextureCompilationPtr.Pin();

	const int32 NumRemainingCompilations = GetNumRemainingTextures();
	if (NumRemainingCompilations == 0)
	{
		if (NotificationItem.IsValid())
		{
			NotificationItem->SetText(NSLOCTEXT("TextureBuild", "TextureBuildFinished", "Finished building Textures!"));
			NotificationItem->SetCompletionState(SNotificationItem::CS_Success);
			NotificationItem->ExpireAndFadeout();

			TextureCompilationPtr.Reset();
		}
	}
	else
	{
		if (!NotificationItem.IsValid())
		{
			FNotificationInfo Info(NSLOCTEXT("TextureBuild", "TextureBuildInProgress", "Building Textures"));
			Info.bFireAndForget = false;

			// Setting fade out and expire time to 0 as the expire message is currently very obnoxious
			Info.FadeOutDuration = 0.0f;
			Info.ExpireDuration = 0.0f;

			NotificationItem = FSlateNotificationManager::Get().AddNotification(Info);
			TextureCompilationPtr = NotificationItem;
		}

		FFormatNamedArguments Args;
		Args.Add(TEXT("BuildTasks"), FText::AsNumber(NumRemainingCompilations));
		FText ProgressMessage = FText::Format(NSLOCTEXT("TextureBuild", "TextureBuildInProgressFormat", "Building Textures ({BuildTasks})"), Args);

		NotificationItem->SetCompletionState(SNotificationItem::CS_Pending);
		NotificationItem->SetVisibility(EVisibility::HitTestInvisible);
		NotificationItem->SetText(ProgressMessage);
	}
}

void FTextureCompilingManager::FinishTextureCompilation(UTexture* Texture)
{
	using namespace TextureCompilingManagerImpl;

	check(IsInGameThread());
	TRACE_CPUPROFILER_EVENT_SCOPE(FinishTextureCompilation);

	UE_LOG(LogTexture, Display, TEXT("UpdateResource for %s (%s) due to async texture compilation"), *Texture->GetName(), *GetLODGroupName(Texture));

	Texture->FinishCachePlatformData();
	Texture->UpdateResource();

	GetRendererModule().FlushVirtualTextureCache();

	// Generate an empty property changed event, to force the asset registry tag
	// to be refreshed now that pixel format and alpha channels are available.
	FPropertyChangedEvent EmptyPropertyChangedEvent(nullptr);
	FCoreUObjectDelegates::OnObjectPropertyChanged.Broadcast(Texture, EmptyPropertyChangedEvent);
}

bool FTextureCompilingManager::IsAsyncCompilationAllowed(UTexture* Texture) const
{
	return 
		// @todo Same requirement as FUntypedBulkData::LoadBulkDataWithFileReader() for now
		// because if we can't load bulk data properly from texture building thread,
		// every texture compilation will effectively be single-threaded anyway...
		// -game mode is extremely slow when texture compilation is required because
		// of this limitation in the loader. Fix the loader and then remove this here!
		GIsEditor && !GEventDrivenLoaderEnabled &&
		IsAsyncTextureCompilationEnabled();
}

FTextureCompilingManager& FTextureCompilingManager::Get()
{
	static FTextureCompilingManager Singleton;
	return Singleton;
}

int32 FTextureCompilingManager::GetNumRemainingTextures() const
{
	int32 Num = 0;
	for (const TSet<TWeakObjectPtr<UTexture>>& Bucket : RegisteredTextureBuckets)
	{
		Num += Bucket.Num();
	}

	return Num;
}

void FTextureCompilingManager::AddTextures(const TArray<UTexture*>& InTextures)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(FTextureCompilingManager::AddTextures)
	check(IsInGameThread());

	// We might not get ticked very often during load time
	// so this will allow us to refresh compiled textures
	// of the highest priority to improve the UI experience.
	ProcessTextures(1 /* Maximum Priority */);

	// Register new textures after ProcessTextures to avoid
	// potential reentrant calls to CreateResource on the
	// textures being added. This would cause multiple
	// TextureResource to be created and assigned to the same Owner
	// which would obviously be bad and causing leaks including
	// in the RHI.
	for (UTexture* Texture : InTextures)
	{
		int32 TexturePriority = 2;
		switch (Texture->LODGroup)
		{
			case TEXTUREGROUP_UI:
				TexturePriority = 0;
			break;
			case TEXTUREGROUP_Terrain_Heightmap:
				TexturePriority = 1;
			break;
		}

		if (RegisteredTextureBuckets.Num() <= TexturePriority)
		{
			RegisteredTextureBuckets.SetNum(TexturePriority + 1);
		}
		RegisteredTextureBuckets[TexturePriority].Emplace(Texture);
	}
}

void FTextureCompilingManager::FinishCompilation(const TArray<UTexture*>& InTextures)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(FTextureCompilingManager::FinishCompilation);

	using namespace TextureCompilingManagerImpl;
	check(IsInGameThread());

	TSet<UTexture*> PendingTextures;
	PendingTextures.Reserve(InTextures.Num());

	int32 TextureIndex = 0;
	for (UTexture* Texture : InTextures)
	{
		for (TSet<TWeakObjectPtr<UTexture>>& Bucket : RegisteredTextureBuckets)
		{
			if (Bucket.Contains(Texture))
			{
				PendingTextures.Add(Texture);
			}
		}
	}

	if (PendingTextures.Num())
	{
		FScopedSlowTask SlowTask((float)PendingTextures.Num(), LOCTEXT("FinishTextureCompilation", "Waiting on texture compilation"), true);
		SlowTask.MakeDialogDelayed(1.0f);

		struct FTextureTask : public IQueuedWork
		{
			TStrongObjectPtr<UTexture> Texture;
			FEvent* Event;
			FTextureTask() { Event = FPlatformProcess::GetSynchEventFromPool(true); }
			~FTextureTask() { FPlatformProcess::ReturnSynchEventToPool(Event); }
			void DoThreadedWork() override { Texture->FinishCachePlatformData(); Event->Trigger(); };
			void Abandon() override { }
		};

		// Perform forced compilation on as many thread as possible in high priority since the game-thread is waiting
		TArray<FTextureTask> PendingTasks;
		PendingTasks.SetNum(PendingTextures.Num());
		
		int32 PendingTaskIndex = 0;
		for (UTexture* Texture : PendingTextures)
		{
			PendingTasks[PendingTaskIndex].Texture.Reset(Texture);
			GLargeThreadPool->AddQueuedWork(&PendingTasks[PendingTaskIndex], EQueuedWorkPriority::High);
			PendingTaskIndex++;
		}

		auto UpdateProgress =
			[&SlowTask](float Progress, int32 Done, int32 Total, const FString& CurrentObjectsName)
			{
				return SlowTask.EnterProgressFrame(Progress, FText::FromString(FString::Printf(TEXT("Waiting on texture compilation %d/%d (%s) ..."), Done, Total, *CurrentObjectsName)));
			};

		for (FTextureTask& PendingTask : PendingTasks)
		{
			UTexture* Texture = PendingTask.Texture.Get();
			const FString TextureName = Texture->GetName();
			// Be nice with the game thread and tick the progress at 60 fps even when no progress is being made...
			while (!PendingTask.Event->Wait(16))
			{
				UpdateProgress(0.0f, TextureIndex, InTextures.Num(), TextureName);
			}
			UpdateProgress(1.f, TextureIndex++, InTextures.Num(), TextureName);
			UE_LOG(LogTexture, Display, TEXT("FinishCompilation requested for %s (%s)"), *TextureName, *GetLODGroupName(Texture));
			FinishTextureCompilation(Texture);

			for (TSet<TWeakObjectPtr<UTexture>>& Bucket : RegisteredTextureBuckets)
			{
				Bucket.Remove(Texture);
			}
		}
	}
}

void FTextureCompilingManager::FinishAllCompilation()
{
	check(IsInGameThread());
	TRACE_CPUPROFILER_EVENT_SCOPE(FTextureCompilingManager::FinishAllCompilation)

	if (GetNumRemainingTextures())
	{
		TArray<UTexture*> PendingTextures;
		PendingTextures.Reserve(GetNumRemainingTextures());

		for (TSet<TWeakObjectPtr<UTexture>>& Bucket : RegisteredTextureBuckets)
		{
			for (TWeakObjectPtr<UTexture>& Texture : Bucket)
			{
				if (Texture.IsValid())
				{
					PendingTextures.Add(Texture.Get());
				}
			}
		}

		FinishCompilation(PendingTextures);
	}
}

void FTextureCompilingManager::ProcessTextures(int32 MaximumPriority)
{
	using namespace TextureCompilingManagerImpl;
	TRACE_CPUPROFILER_EVENT_SCOPE(FTextureCompilingManager::ProcessTextures);
	const double MaxSecondsPerFrame = 0.016;

	if (GetNumRemainingTextures())
	{
		TArray<UTexture*> ProcessedTextures;
		{
			TRACE_CPUPROFILER_EVENT_SCOPE(ProcessFinishedTextures);

			double TickStartTime = FPlatformTime::Seconds();

			if (MaximumPriority == -1 || MaximumPriority > RegisteredTextureBuckets.Num())
			{
				MaximumPriority = RegisteredTextureBuckets.Num();
			}
			
			for (int32 PriorityIndex = 0; PriorityIndex < MaximumPriority; ++PriorityIndex)
			{
				TSet<TWeakObjectPtr<UTexture>>& TexturesToProcess = RegisteredTextureBuckets[PriorityIndex];
				if (TexturesToProcess.Num())
				{
					const bool bIsHighestPrio = PriorityIndex == 0;
			
					TSet<TWeakObjectPtr<UTexture>> TexturesToPostpone;
					for (TWeakObjectPtr<UTexture>& Texture : TexturesToProcess)
					{
						if (Texture.IsValid())
						{
							const bool bHasTimeLeft = (FPlatformTime::Seconds() - TickStartTime) < MaxSecondsPerFrame;
							if ((bIsHighestPrio || bHasTimeLeft) && Texture->IsAsyncCacheComplete())
							{
								FinishTextureCompilation(Texture.Get());
								ProcessedTextures.Add(Texture.Get());
							}
							else
							{
								TexturesToPostpone.Emplace(MoveTemp(Texture));
							}
						}
					}

					RegisteredTextureBuckets[PriorityIndex] = MoveTemp(TexturesToPostpone);
				}
			}
		}

		if (ProcessedTextures.Num())
		{
			TRACE_CPUPROFILER_EVENT_SCOPE(RecacheUniformExpressions);

			TMultiMap<UObject*, UMaterialInterface*> TexturesAffectingMaterials = GetTexturesAffectingMaterialInterfaces();

			TArray<UMaterialInterface*> MaterialsToUpdate;
			for (UTexture* Texture : ProcessedTextures)
			{
				TexturesAffectingMaterials.MultiFind(Texture, MaterialsToUpdate);
			}

			if (MaterialsToUpdate.Num())
			{
				TRACE_CPUPROFILER_EVENT_SCOPE(UpdateMaterials);
				
				for (UMaterialInterface* MaterialToUpdate : MaterialsToUpdate)
				{
					MaterialToUpdate->RecacheUniformExpressions(false);
				}
			}
		}

		{
			TRACE_CPUPROFILER_EVENT_SCOPE(FTextureCompilingManager::Reschedule);

			// Reschedule higher priority if they have been rendered
			for (TSet<TWeakObjectPtr<UTexture>>& Bucket : RegisteredTextureBuckets)
			{
				for (TWeakObjectPtr<UTexture>& WeakPtr : Bucket)
				{
					if (UTexture* Texture = WeakPtr.Get())
					{
						// Reschedule any texture that have been rendered with slightly higher priority 
						// to improve the editor experience for low-core count.
						//
						// Keep in mind that some textures are only accessed once during the construction
						// of a virtual texture, so we can't count on the LastRenderTime to be updated
						// continuously for those even if they're in view.
						if ((Texture->Resource && Texture->Resource->LastRenderTime != -FLT_MAX) ||
							Texture->TextureReference.GetLastRenderTime() != -FLT_MAX)
						{
							FTexturePlatformData** Data = Texture->GetRunningPlatformData();
							if (Data && *Data)
							{
								FTextureAsyncCacheDerivedDataTask* AsyncTask = (*Data)->AsyncTask;
								if (AsyncTask && AsyncTask->GetPriority() == GetBasePriority(Texture))
								{
									if (AsyncTask->Reschedule(GetThreadPool(), GetBoostPriority(Texture)))
									{
										UE_LOG(
											LogTexture, 
											Display, 
											TEXT("Boosting priority of %s (%s) from %s to %s because of it's last render time"), 
											*Texture->GetName(), 
											*GetLODGroupName(Texture), 
											GetPriorityName(GetBasePriority(Texture)),
											GetPriorityName(GetBoostPriority(Texture))
										);
									}
								}
							}
						}
					}
				}
			}
		}
	}
}

void FTextureCompilingManager::Tick(float DeltaTime)
{
	ProcessTextures();

	UpdateCompilationNotification();
}

#endif // #if WITH_EDITOR

#undef LOCTEXT_NAMESPACE