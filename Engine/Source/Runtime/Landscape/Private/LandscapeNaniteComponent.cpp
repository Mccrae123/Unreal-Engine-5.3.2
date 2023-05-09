// Copyright Epic Games, Inc. All Rights Reserved.

#include "LandscapeNaniteComponent.h"
#include "LandscapeEdit.h"
#include "LandscapeRender.h"
#include "MaterialDomain.h"
#include "Materials/Material.h"
#include "NaniteSceneProxy.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshSourceData.h"
#include "NaniteDefinitions.h"
#include "UObject/Package.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(LandscapeNaniteComponent)

#if WITH_EDITOR
#include "StaticMeshAttributes.h"
#include "StaticMeshDescription.h"
#include "StaticMeshOperations.h"
#include "MeshUtilitiesCommon.h"
#include "OverlappingCorners.h"
#include "MeshBuild.h"
#include "StaticMeshBuilder.h"
#include "NaniteBuilder.h"
#include "Rendering/NaniteResources.h"
#include "PhysicsEngine/BodySetup.h"
#include "StaticMeshCompiler.h"
#include "LandscapePrivate.h"
#include "LandscapeDataAccess.h"
#include "LandscapeSubsystem.h"
#include "MeshDescriptionHelper.h"
#include "Async/Async.h"
#include "Async/TaskGraphInterfaces.h"
#include "EditorFramework/AssetImportData.h"
#endif

extern float LandscapeNaniteAsyncDebugWait;

ULandscapeNaniteComponent::ULandscapeNaniteComponent(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
	, bEnabled(true)
{
}

void ULandscapeNaniteComponent::PostLoad()
{
	Super::PostLoad();

#if WITH_EDITOR
	if (UStaticMesh* NaniteStaticMesh = GetStaticMesh())
	{
		UPackage* CurrentPackage = GetPackage();
		check(CurrentPackage);
		// At one point, the Nanite mesh was outered to the component, which leads the mesh to be duplicated when entering PIE. If we outer the mesh to the package instead, 
		//  PIE duplication will simply reference that mesh, preventing the expensive copy to occur when entering PIE: 
		if (!(CurrentPackage->GetPackageFlags() & PKG_PlayInEditor)  // No need to do it on PIE, since the outer should already have been changed in the original object 
			&& (NaniteStaticMesh->GetOuter() != CurrentPackage))
		{
			// Change the outer : 
			NaniteStaticMesh->Rename(nullptr, CurrentPackage, REN_ForceNoResetLoaders);
		}
	}
#endif // WITH_EDITOR

	ALandscapeProxy* LandscapeProxy = GetLandscapeProxy();
	if (ensure(LandscapeProxy))
	{
		// Ensure that the component lighting and shadow settings matches the actor
		UpdatedSharedPropertiesFromActor();
	}
}

void ULandscapeNaniteComponent::CollectPSOPrecacheData(const FPSOPrecacheParams& BasePrecachePSOParams, FComponentPSOPrecacheParamsList& OutParams)
{
	Super::CollectPSOPrecacheData(BasePrecachePSOParams, OutParams);
	
	// Mark high priority
	for (FComponentPSOPrecacheParams& Params : OutParams)
	{
		Params.Priority = EPSOPrecachePriority::High;
	}
}

ALandscapeProxy* ULandscapeNaniteComponent::GetLandscapeProxy() const
{
	return CastChecked<ALandscapeProxy>(GetOuter());
}

ALandscape* ULandscapeNaniteComponent::GetLandscapeActor() const
{
	ALandscapeProxy* Landscape = GetLandscapeProxy();
	if (Landscape)
	{
		return Landscape->GetLandscapeActor();
	}
	return nullptr;
}

void ULandscapeNaniteComponent::UpdatedSharedPropertiesFromActor()
{
	ALandscapeProxy* LandscapeProxy = GetLandscapeProxy();

	CastShadow = LandscapeProxy->CastShadow;
	bCastDynamicShadow = LandscapeProxy->bCastDynamicShadow;
	bCastStaticShadow = LandscapeProxy->bCastStaticShadow;
	bCastContactShadow = LandscapeProxy->bCastContactShadow;
	bCastFarShadow = LandscapeProxy->bCastFarShadow;
	bCastHiddenShadow = LandscapeProxy->bCastHiddenShadow;
	bCastShadowAsTwoSided = LandscapeProxy->bCastShadowAsTwoSided;
	bAffectDistanceFieldLighting = LandscapeProxy->bAffectDistanceFieldLighting;
	bRenderCustomDepth = LandscapeProxy->bRenderCustomDepth;
	CustomDepthStencilWriteMask = LandscapeProxy->CustomDepthStencilWriteMask;
	CustomDepthStencilValue = LandscapeProxy->CustomDepthStencilValue;
	SetCullDistance(LandscapeProxy->LDMaxDrawDistance);
	LightingChannels = LandscapeProxy->LightingChannels;

	// We don't want Nanite representation in ray tracing
	bVisibleInRayTracing = false;

	// We don't want WPO evaluation enabled on landscape meshes
	bEvaluateWorldPositionOffset = false;
}

void ULandscapeNaniteComponent::SetEnabled(bool bValue)
{
	if (bValue != bEnabled)
	{
		bEnabled = bValue;
		MarkRenderStateDirty();
	}
}

bool ULandscapeNaniteComponent::IsHLODRelevant() const
{
	// This component doesn't need to be included in HLOD, as we're already including the non-nanite LS components
	return false;
}

#if WITH_EDITOR

FGraphEventRef ULandscapeNaniteComponent::InitializeForLandscapeAsync(ALandscapeProxy* Landscape, const FGuid& NewProxyContentId, bool InbIsAsync)
{
	UWorld* World = Landscape->GetWorld();
	
	ULandscapeSubsystem* LandscapeSubSystem = World->GetSubsystem<ULandscapeSubsystem>();
	check(LandscapeSubSystem);
	LandscapeSubSystem->IncNaniteBuild();

	FGraphEventRef StaticMeshBuildCompleteEvent = FGraphEvent::CreateGraphEvent();
	TSharedRef<UE::Landscape::Nanite::FAsyncBuildData> AsyncBuildData = Landscape->MakeAsyncNaniteBuildData();
	
	FGraphEventRef ExportMeshEvent = FFunctionGraphTask::CreateAndDispatchWhenReady([AsyncBuildData, Name = Landscape->GetActorNameOrLabel()]()
		{			
			TRACE_CPUPROFILER_EVENT_SCOPE(ULandscapeNaniteComponent::ExportLandscapeAsync-ExportMeshTask);
			double StartTimeSeconds = FPlatformTime::Seconds();

			if (!AsyncBuildData->LandscapeWeakRef.IsValid() || AsyncBuildData->bCancelled)
			{
				AsyncBuildData->bCancelled = true;
				return;
			}

			UPackage* Package = AsyncBuildData->LandscapeWeakRef->GetPackage();
			AsyncBuildData->NaniteStaticMesh = NewObject<UStaticMesh>(/*Outer = */Package, MakeUniqueObjectName(/*Parent = */Package, UStaticMesh::StaticClass(), TEXT("LandscapeNaniteMesh")));
			AsyncBuildData->SourceModel = &AsyncBuildData->NaniteStaticMesh->AddSourceModel();
			AsyncBuildData->NaniteMeshDescription = AsyncBuildData->NaniteStaticMesh->CreateMeshDescription(0);

			// Don't allow the engine to recalculate normals
			AsyncBuildData->SourceModel->BuildSettings.bRecomputeNormals = false;
			AsyncBuildData->SourceModel->BuildSettings.bRecomputeTangents = false;
			AsyncBuildData->SourceModel->BuildSettings.bRemoveDegenerates = false;
			AsyncBuildData->SourceModel->BuildSettings.bUseHighPrecisionTangentBasis = false;
			AsyncBuildData->SourceModel->BuildSettings.bUseFullPrecisionUVs = false;

			FMeshNaniteSettings& NaniteSettings = AsyncBuildData->NaniteStaticMesh->NaniteSettings;
			NaniteSettings.bEnabled = true;
			NaniteSettings.FallbackPercentTriangles = 0.01f; // Keep effectively no fallback mesh triangles
			NaniteSettings.FallbackRelativeError = 1.0f;

			int32 LOD = AsyncBuildData->LOD;
			
			ALandscapeProxy::FRawMeshExportParams ExportParams;
			ExportParams.ComponentsToExport = MakeArrayView(AsyncBuildData->InputComponents.GetData(), AsyncBuildData->InputComponents.Num());
			ExportParams.ComponentsMaterialSlotName = MakeArrayView(AsyncBuildData->InputMaterialSlotNames.GetData(), AsyncBuildData->InputMaterialSlotNames.Num());
			ExportParams.ExportLOD = LOD;
			ExportParams.ExportCoordinatesType = ALandscapeProxy::FRawMeshExportParams::EExportCoordinatesType::RelativeToProxy;
			ExportParams.UVConfiguration.ExportUVMappingTypes.SetNumZeroed(4);
			ExportParams.UVConfiguration.ExportUVMappingTypes[0] = ALandscapeProxy::FRawMeshExportParams::EUVMappingType::TerrainCoordMapping_XY; // In LandscapeVertexFactory, Texcoords0 = ETerrainCoordMappingType::TCMT_XY (or ELandscapeCustomizedCoordType::LCCT_CustomUV0)
			ExportParams.UVConfiguration.ExportUVMappingTypes[1] = ALandscapeProxy::FRawMeshExportParams::EUVMappingType::TerrainCoordMapping_XZ; // In LandscapeVertexFactory, Texcoords1 = ETerrainCoordMappingType::TCMT_XZ (or ELandscapeCustomizedCoordType::LCCT_CustomUV1)
			ExportParams.UVConfiguration.ExportUVMappingTypes[2] = ALandscapeProxy::FRawMeshExportParams::EUVMappingType::TerrainCoordMapping_YZ; // In LandscapeVertexFactory, Texcoords2 = ETerrainCoordMappingType::TCMT_YZ (or ELandscapeCustomizedCoordType::LCCT_CustomUV2)
			ExportParams.UVConfiguration.ExportUVMappingTypes[3] = ALandscapeProxy::FRawMeshExportParams::EUVMappingType::WeightmapUV; // In LandscapeVertexFactory, Texcoords3 = ELandscapeCustomizedCoordType::LCCT_WeightMapUV
			// COMMENT [jonathan.bard] ATM Nanite meshes only support up to 4 UV sets so we cannot support those 2 : 
			//ExportParams.UVConfiguration.ExportUVMappingTypes[4] = ALandscapeProxy::FRawMeshExportParams::EUVMappingType::LightmapUV; // In LandscapeVertexFactory, Texcoords4 = lightmap UV
			//ExportParams.UVConfiguration.ExportUVMappingTypes[5] = ALandscapeProxy::FRawMeshExportParams::EUVMappingType::HeightmapUV; // // In LandscapeVertexFactory, Texcoords5 = heightmap UV

			bool bSuccess = AsyncBuildData->LandscapeWeakRef->ExportToRawMeshDataCopy(ExportParams, *AsyncBuildData->NaniteMeshDescription, AsyncBuildData.Get());

			// Apply the mesh description cleanup/optimization here instead of during DDC build (avoids expensive large mesh copies)
			{
				FMeshDescriptionHelper MeshDescriptionHelper(&AsyncBuildData->SourceModel->BuildSettings);
				MeshDescriptionHelper.SetupRenderMeshDescription(AsyncBuildData->NaniteStaticMesh, *AsyncBuildData->NaniteMeshDescription, true /* Is Nanite */, false /* bNeedTangents */);
			}

			const FPolygonGroupArray& PolygonGroups = AsyncBuildData->NaniteMeshDescription->PolygonGroups();
			checkf(bSuccess && (PolygonGroups.Num() == AsyncBuildData->InputComponents.Num()), TEXT("Invalid landscape static mesh raw mesh export for actor %s (%i components)"), *Name, AsyncBuildData->InputComponents.Num());
			check(AsyncBuildData->InputMaterials.Num() == AsyncBuildData->InputComponents.Num());
			AsyncBuildData->MeshAttributes = MakeShared<FStaticMeshAttributes>(*AsyncBuildData->NaniteMeshDescription);

			UE_LOG(LogLandscape, Verbose, TEXT("Successful export of raw static mesh for Nanite landscape (%i components) for actor %s"), AsyncBuildData->InputComponents.Num(), *Name);

			UStaticMesh::FCommitMeshDescriptionParams CommitParams;
			CommitParams.bMarkPackageDirty = false;
			CommitParams.bUseHashAsGuid = true;

			AsyncBuildData->NaniteStaticMesh->CommitMeshDescription(0u, CommitParams);
			AsyncBuildData->bExportResult = true;

			const  double DurationSeconds = FPlatformTime::Seconds() - StartTimeSeconds;
			if (const double ExtraWait = FMath::Max(LandscapeNaniteAsyncDebugWait - DurationSeconds, 0.0); ExtraWait > 0.0)
			{
				FPlatformProcess::Sleep(ExtraWait);
			}

		}, TStatId(), nullptr, 
			ENamedThreads::AnyBackgroundHiPriTask);

	FGraphEventArray CommitDependencies{ ExportMeshEvent };

	FGraphEventRef BatchBuildEvent = FFunctionGraphTask::CreateAndDispatchWhenReady([AsyncBuildData, Component = this, NewProxyContentId, InbIsAsync, Name = Landscape->GetActorNameOrLabel(), StaticMeshBuildCompleteEvent]()
		{
			TRACE_CPUPROFILER_EVENT_SCOPE(ULandscapeNaniteComponent::ExportLandscapeAsync-BatchBuildTask);
			AsyncBuildData->NaniteStaticMesh->ImportVersion = EImportStaticMeshVersion::LastVersion;

			auto CompleteStaticMesh = [AsyncBuildData, Component, NewProxyContentId, Name, StaticMeshBuildCompleteEvent, InbIsAsync](UStaticMesh* InStaticMesh)
			{
				// this is as horror as we have to mark all the objects created in the background thread as not async 
				AsyncBuildData->NaniteStaticMesh->ClearInternalFlags(EInternalObjectFlags::Async);
				AsyncBuildData->NaniteStaticMesh->AssetImportData->ClearInternalFlags(EInternalObjectFlags::Async);

				AsyncBuildData->NaniteStaticMesh->GetHiResSourceModel().StaticMeshDescriptionBulkData->ClearInternalFlags(EInternalObjectFlags::Async);
				AsyncBuildData->NaniteStaticMesh->GetHiResSourceModel().StaticMeshDescriptionBulkData->CreateMeshDescription()->ClearInternalFlags(EInternalObjectFlags::Async);

				AsyncBuildData->NaniteStaticMesh->GetSourceModel(0).StaticMeshDescriptionBulkData->ClearInternalFlags(EInternalObjectFlags::Async);
				AsyncBuildData->NaniteStaticMesh->GetSourceModel(0).StaticMeshDescriptionBulkData->GetMeshDescription()->ClearInternalFlags(EInternalObjectFlags::Async);

				if (!AsyncBuildData->LandscapeWeakRef.IsValid() || AsyncBuildData->bCancelled)
				{
					if (AsyncBuildData->LandscapeSubSystemWeakRef.IsValid())
					{
						AsyncBuildData->LandscapeSubSystemWeakRef->DecNaniteBuild();
					}
					StaticMeshBuildCompleteEvent->DispatchSubsequents();
					AsyncBuildData->bCancelled = true;
					return;
				}

				ON_SCOPE_EXIT
				{
					if (InbIsAsync)
					{
						// only deregister only myself.
						InStaticMesh->OnPostMeshBuild().Clear();
					}
				};

				check(AsyncBuildData->NaniteStaticMesh == InStaticMesh);


				// Proxy has been updated since and this nanite calculation is out of date.
				if (AsyncBuildData->LandscapeWeakRef->GetNaniteContentId() != NewProxyContentId)
				{
					AsyncBuildData->bIsComplete = true;
					
					if (AsyncBuildData->LandscapeSubSystemWeakRef.IsValid())
					{
						AsyncBuildData->LandscapeSubSystemWeakRef->DecNaniteBuild();
					}
					StaticMeshBuildCompleteEvent->DispatchSubsequents();
					return;
				}				
				
				AsyncBuildData->NaniteStaticMesh->MarkPackageDirty();

				TRACE_CPUPROFILER_EVENT_SCOPE(ULandscapeNaniteComponent::ExportLandscapeAsync - FinalizeOnComponent);
				if (UBodySetup* BodySetup = InStaticMesh->GetBodySetup())
				{
					BodySetup->DefaultInstance.SetCollisionProfileName(UCollisionProfile::NoCollision_ProfileName);
					BodySetup->CollisionTraceFlag = CTF_UseSimpleAsComplex;
					// We won't ever enable collisions (since collisions are handled by ULandscapeHeightfieldCollisionComponent), ensure we don't even cook or load any collision data on this mesh: 
					BodySetup->bNeverNeedsCookedCollisionData = true;
				}

				Component->SetStaticMesh(InStaticMesh);
				Component->SetProxyContentId(NewProxyContentId);
				Component->SetEnabled(!Component->IsEnabled());
				AsyncBuildData->LandscapeWeakRef->UpdateRenderingMethod();
				AsyncBuildData->LandscapeWeakRef->NaniteComponent->MarkRenderStateDirty();
				AsyncBuildData->LandscapeWeakRef->NaniteComponent = Component;
				AsyncBuildData->bIsComplete = true;
		
				if (AsyncBuildData->LandscapeSubSystemWeakRef.IsValid())
				{
					AsyncBuildData->LandscapeSubSystemWeakRef->DecNaniteBuild();
				}
				StaticMeshBuildCompleteEvent->DispatchSubsequents();
			};

			if (!InbIsAsync)
			{
				CompleteStaticMesh(AsyncBuildData->NaniteStaticMesh);
			}
			else
			{
				// On StaticMesh Build complete set the static mesh 
				AsyncBuildData->NaniteStaticMesh->OnPostMeshBuild().AddLambda(CompleteStaticMesh);
			}

			TPolygonGroupAttributesRef<FName> PolygonGroupMaterialSlotNames = AsyncBuildData->MeshAttributes->GetPolygonGroupMaterialSlotNames();
			int32 ComponentIndex = 0;
			for (UMaterialInterface* Material : AsyncBuildData->InputMaterials)
			{
				check(Material != nullptr);
				const FName MaterialSlotName = AsyncBuildData->InputMaterialSlotNames[ComponentIndex];
				check(PolygonGroupMaterialSlotNames.GetRawArray().Contains(MaterialSlotName));
				AsyncBuildData->NaniteStaticMesh->GetStaticMaterials().Add(FStaticMaterial(Material, MaterialSlotName));
				++ComponentIndex;
			}
			
			AsyncBuildData->NaniteStaticMesh->MarkAsNotHavingNavigationData();
			UStaticMesh::FBuildParameters BuildParameters;
			BuildParameters.bInSilent = true;
		
			UStaticMesh::BatchBuild({ AsyncBuildData->NaniteStaticMesh});
		},
	TStatId(),
	& CommitDependencies,
	ENamedThreads::GameThread);


	LandscapeSubSystem->AddAsyncEvent(StaticMeshBuildCompleteEvent);


	return StaticMeshBuildCompleteEvent;
}

bool ULandscapeNaniteComponent::InitializeForLandscape(ALandscapeProxy* Landscape, const FGuid& NewProxyContentId)
{
	FGraphEventRef GraphEvent = InitializeForLandscapeAsync(Landscape, NewProxyContentId, true);
	while (!GraphEvent->IsComplete())
	{
		ENamedThreads::Type CurrentThread = FTaskGraphInterface::Get().GetCurrentThreadIfKnown();
		FTaskGraphInterface::Get().ProcessThreadUntilIdle(CurrentThread);
		FAssetCompilingManager::Get().ProcessAsyncTasks();
	}
	
	return true;
}

bool ULandscapeNaniteComponent::InitializePlatformForLandscape(ALandscapeProxy* Landscape, const ITargetPlatform* TargetPlatform)
{
	// This is a workaround. IsCachedCookedPlatformDataLoaded needs to return true to ensure that StreamablePages are loaded from DDC
	if (TargetPlatform)
	{
		if (UStaticMesh* NaniteStaticMesh = GetStaticMesh())
		{
			NaniteStaticMesh->BeginCacheForCookedPlatformData(TargetPlatform);
			FStaticMeshCompilingManager::Get().FinishCompilation({ NaniteStaticMesh });

			const double StartTime = FPlatformTime::Seconds();

			while (!NaniteStaticMesh->IsCachedCookedPlatformDataLoaded(TargetPlatform))
			{
				FAssetCompilingManager::Get().ProcessAsyncTasks(true);
				FPlatformProcess::Sleep(0.01);

				constexpr double MaxWaitSeconds = 240.0;
				if (FPlatformTime::Seconds() - StartTime > MaxWaitSeconds)
				{
					UE_LOG(LogLandscape, Error, TEXT("ULandscapeNaniteComponent::InitializePlatformForLandscape waited more than %f seconds for IsCachedCookedPlatformDataLoaded to return true"), MaxWaitSeconds);
					return false;
				}
			}
		}		
	}

	return true;
}

#endif
