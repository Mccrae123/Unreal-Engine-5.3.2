// Copyright Epic Games, Inc. All Rights Reserved.

#include "USDGeomMeshTranslator.h"

#if USE_USD_SDK

#include "UnrealUSDWrapper.h"
#include "USDConversionUtils.h"
#include "USDGeomMeshConversion.h"
#include "USDTypesConversion.h"

#include "Async/Async.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/CollisionProfile.h"
#include "Engine/StaticMesh.h"
#include "IMeshBuilderModule.h"
#include "Interfaces/ITargetPlatform.h"
#include "Interfaces/ITargetPlatformManagerModule.h"
#include "Materials/Material.h"
#include "Materials/MaterialInterface.h"
#include "Materials/MaterialInstanceConstant.h"
#include "MeshDescriptionOperations.h"
#include "Misc/SecureHash.h"
#include "Modules/ModuleManager.h"
#include "PhysicsEngine/BodySetup.h"
#include "StaticMeshAttributes.h"
#include "UObject/SoftObjectPath.h"

#include "USDIncludesStart.h"
	#include "pxr/usd/usd/prim.h"
	#include "pxr/usd/usd/stage.h"
	#include "pxr/usd/usdGeom/mesh.h"
	#include "pxr/usd/usdGeom/xformable.h"
	#include "pxr/usd/usdShade/material.h"
#include "USDIncludesEnd.h"


namespace UsdGeomMeshTranslatorImpl
{
	bool IsGeometryAnimated( const pxr::UsdGeomMesh& GeomMesh, const pxr::UsdTimeCode TimeCode )
	{
		FScopedUsdAllocs UsdAllocs;

		bool bHasAttributesTimeSamples = false;
		{
			constexpr bool bIncludeInherited = false;
			pxr::TfTokenVector GeomMeshAttributeNames = pxr::UsdGeomMesh::GetSchemaAttributeNames( bIncludeInherited );
			pxr::TfTokenVector GeomPointBasedAttributeNames = pxr::UsdGeomPointBased::GetSchemaAttributeNames( bIncludeInherited );

			GeomMeshAttributeNames.reserve( GeomMeshAttributeNames.size() + GeomPointBasedAttributeNames.size() );
			GeomMeshAttributeNames.insert( GeomMeshAttributeNames.end(), GeomPointBasedAttributeNames.begin(), GeomPointBasedAttributeNames.end() );

			for ( const pxr::TfToken& AttributeName : GeomMeshAttributeNames )
			{
				const pxr::UsdAttribute& Attribute = GeomMesh.GetPrim().GetAttribute( AttributeName );

				double DesiredTime = TimeCode.GetValue();
				double MinTime = 0.0;
				double MaxTime = 0.0;
				bool bHasTimeSamples = false;

				if ( bHasTimeSamples && DesiredTime >= MinTime && DesiredTime <= MaxTime && Attribute.ValueMightBeTimeVarying())
				{
					bHasAttributesTimeSamples = true;
					break;
				}
			}
		}

		return bHasAttributesTimeSamples;
	}

	/** Returns true if material infos have changed on the StaticMesh */
	bool ProcessMaterials( const pxr::UsdPrim& UsdPrim, UStaticMesh& StaticMesh, TMap< FString, UObject* >& PrimPathsToAssets, float Time )
	{
		const FMeshDescription* MeshDescription = StaticMesh.GetMeshDescription( 0 );

		if ( !MeshDescription )
		{
			return false ;
		}

		bool bMaterialAssignementsHaveChanged = false;

		FStaticMeshConstAttributes StaticMeshAttributes( *MeshDescription );

		auto FetchUEMaterialsAttribute = []( const pxr::UsdPrim& UsdPrim, float Time ) -> TArray< FString >
		{
			if ( !UsdPrim )
			{
				return {};
			}

			TArray< FString > UEMaterialsAttribute;

			FScopedUsdAllocs UsdAllocs;

			if ( pxr::UsdAttribute MaterialsAttribute = UsdPrim.GetAttribute( UnrealIdentifiers::MaterialAssignments ) )
			{
				pxr::VtStringArray UEMaterials;
				MaterialsAttribute.Get( &UEMaterials, pxr::UsdTimeCode( Time ) );

				for ( const std::string& UEMaterial : UEMaterials )
				{
					UEMaterialsAttribute.Emplace( ANSI_TO_TCHAR( UEMaterial.c_str() ) );
				}
			}

			return UEMaterialsAttribute;
		};

		TArray< FString > MainPrimUEMaterialsAttribute = FetchUEMaterialsAttribute( UsdPrim, Time );

		TPolygonGroupAttributesConstRef< FName > PolygonGroupUsdPrimPaths = MeshDescription->PolygonGroupAttributes().GetAttributesRef< FName >( "UsdPrimPath" );

		int32 PolygonGroupPrimMaterialIndex = 0;

		for ( const FPolygonGroupID PolygonGroupID : MeshDescription->PolygonGroups().GetElementIDs() )
		{
			const FName& ImportedMaterialSlotName = StaticMeshAttributes.GetPolygonGroupMaterialSlotNames()[ PolygonGroupID ];
			const FName MaterialSlotName = ImportedMaterialSlotName;

			const int32 MaterialIndex = PolygonGroupID.GetValue();

			TUsdStore< pxr::UsdPrim > PolygonGroupPrim = UsdPrim;

			if ( PolygonGroupUsdPrimPaths.IsValid() )
			{
				const FName& UsdPrimPath = PolygonGroupUsdPrimPaths[ PolygonGroupID ];

				TUsdStore< pxr::SdfPath > PrimPath = UnrealToUsd::ConvertPath( *UsdPrimPath.ToString() );

				if ( PolygonGroupPrim.Get() && PolygonGroupPrim.Get().GetPrimPath() != PrimPath.Get() )
				{
					PolygonGroupPrim = UsdPrim.GetStage()->GetPrimAtPath( PrimPath.Get() );
					PolygonGroupPrimMaterialIndex = 0; // We've moved to a new sub prim
				}
				else
				{
					++PolygonGroupPrimMaterialIndex; // This polygon group is part of the same sub prim
				}
			}

			UMaterialInterface* Material = nullptr;

			if ( MainPrimUEMaterialsAttribute.IsValidIndex( MaterialIndex ))
			{
				Material = Cast< UMaterialInterface >( FSoftObjectPath( MainPrimUEMaterialsAttribute[ MaterialIndex ] ).TryLoad() );
			}
			else
			{
				TArray< FString > PolygonGroupPrimUEMaterialsAttribute = FetchUEMaterialsAttribute( PolygonGroupPrim.Get(), Time );

				if ( PolygonGroupPrimUEMaterialsAttribute.IsValidIndex( PolygonGroupPrimMaterialIndex ) )
				{
					Material = Cast< UMaterialInterface >( FSoftObjectPath( PolygonGroupPrimUEMaterialsAttribute[ PolygonGroupPrimMaterialIndex ] ).TryLoad() );
				}
				else
				{
					TUsdStore< pxr::UsdPrim > MaterialPrim = UsdPrim.GetStage()->GetPrimAtPath( UnrealToUsd::ConvertPath( *ImportedMaterialSlotName.ToString() ).Get() );

					if ( MaterialPrim.Get() )
					{
						Material = Cast< UMaterialInterface >( PrimPathsToAssets.FindRef( UsdToUnreal::ConvertPath( MaterialPrim.Get().GetPrimPath() ) ) );
					}
				}
			}

			if ( Material == nullptr )
			{
				UMaterialInstanceConstant* MaterialInstance = NewObject< UMaterialInstanceConstant >();
				if ( UsdToUnreal::ConvertDisplayColor( pxr::UsdGeomMesh( PolygonGroupPrim.Get() ), *MaterialInstance, pxr::UsdTimeCode( Time ) ) )
				{
					Material = MaterialInstance;
				}
			}
			
			FStaticMaterial StaticMaterial( Material, MaterialSlotName );

			if ( !StaticMesh.StaticMaterials.IsValidIndex( MaterialIndex ) )
			{
				StaticMesh.StaticMaterials.Add( MoveTemp( StaticMaterial ) );
				bMaterialAssignementsHaveChanged = true;
			}
			else if ( !( StaticMesh.StaticMaterials[ MaterialIndex ] == StaticMaterial ) )
			{
				StaticMesh.StaticMaterials[ MaterialIndex ] = MoveTemp( StaticMaterial );
				bMaterialAssignementsHaveChanged = true;
			}

			if ( StaticMesh.GetSectionInfoMap().IsValidSection( 0, PolygonGroupID.GetValue() ) )
			{
				FMeshSectionInfo MeshSectionInfo = StaticMesh.GetSectionInfoMap().Get( 0, PolygonGroupID.GetValue() );

				if ( MeshSectionInfo.MaterialIndex != MaterialIndex )
				{
					MeshSectionInfo.MaterialIndex = MaterialIndex;
					StaticMesh.GetSectionInfoMap().Set( 0, PolygonGroupID.GetValue(), MeshSectionInfo );

					bMaterialAssignementsHaveChanged = true;
				}
			}
			else
			{
				FMeshSectionInfo MeshSectionInfo;
				MeshSectionInfo.MaterialIndex = MaterialIndex;

				StaticMesh.GetSectionInfoMap().Set( 0, PolygonGroupID.GetValue(), MeshSectionInfo );

				bMaterialAssignementsHaveChanged = true;
			}
		}

		return bMaterialAssignementsHaveChanged;
	}

	FMeshDescription LoadMeshDescription( const pxr::UsdGeomMesh& UsdMesh, const pxr::UsdTimeCode TimeCode )
	{
		if ( !UsdMesh )
		{
			return {};
		}

		FMeshDescription MeshDescription;
		FStaticMeshAttributes StaticMeshAttributes( MeshDescription );
		StaticMeshAttributes.Register();

		UsdToUnreal::ConvertGeomMesh( UsdMesh, MeshDescription, TimeCode );

		return MeshDescription;
	}

	UStaticMesh* CreateStaticMesh( FMeshDescription&& MeshDescription, FUsdSchemaTranslationContext& Context, bool& bOutIsNew )
	{
		UStaticMesh* StaticMesh = nullptr;

		FSHAHash MeshHash = FMeshDescriptionOperations::ComputeSHAHash( MeshDescription );

		StaticMesh = Cast< UStaticMesh >( Context.AssetsCache.FindRef( MeshHash.ToString() ) );

		if ( !StaticMesh && !MeshDescription.IsEmpty() )
		{
			bOutIsNew = true;

			StaticMesh = NewObject< UStaticMesh >( GetTransientPackage(), NAME_None, Context.ObjectFlags | EObjectFlags::RF_Public );

			FStaticMeshSourceModel& SourceModel = StaticMesh->AddSourceModel();
			SourceModel.BuildSettings.bGenerateLightmapUVs = false;
			SourceModel.BuildSettings.bRecomputeNormals = false;
			SourceModel.BuildSettings.bRecomputeTangents = false;
			SourceModel.BuildSettings.bBuildAdjacencyBuffer = false;
			SourceModel.BuildSettings.bBuildReversedIndexBuffer = false;

			FMeshDescription* StaticMeshDescription = StaticMesh->CreateMeshDescription(0);
			check( StaticMeshDescription );
			*StaticMeshDescription = MoveTemp( MeshDescription );

			Context.AssetsCache.Add( MeshHash.ToString() ) = StaticMesh;
		}
		else
		{
			//FPlatformMisc::LowLevelOutputDebugStringf( TEXT("Mesh found in cache %s\n"), *StaticMesh->GetName() );
			bOutIsNew = false;
		}

		return StaticMesh;
	}

	void PreBuildStaticMesh( const pxr::UsdPrim& RootPrim, UStaticMesh& StaticMesh, TMap< FString, UObject* >& PrimPathsToAssets, float Time )
	{
		TRACE_CPUPROFILER_EVENT_SCOPE( UsdGeomMeshTranslatorImpl::PreBuildStaticMesh );

		if ( StaticMesh.RenderData )
		{
			StaticMesh.ReleaseResources();
			StaticMesh.ReleaseResourcesFence.Wait();
		}

		StaticMesh.RenderData = MakeUnique< FStaticMeshRenderData >();
		StaticMesh.CreateBodySetup();
	}

	bool BuildStaticMesh( UStaticMesh& StaticMesh )
	{
		TRACE_CPUPROFILER_EVENT_SCOPE( UsdGeomMeshTranslatorImpl::BuildStaticMesh );

		ITargetPlatformManagerModule& TargetPlatformManager = GetTargetPlatformManagerRef();
		ITargetPlatform* RunningPlatform = TargetPlatformManager.GetRunningTargetPlatform();
		check(RunningPlatform);

		const FStaticMeshLODSettings& LODSettings = RunningPlatform->GetStaticMeshLODSettings();
		StaticMesh.RenderData->Cache( &StaticMesh, LODSettings );

		if ( StaticMesh.BodySetup )
		{
			StaticMesh.BodySetup->CreatePhysicsMeshes();
		}

		return true;
	}

	void PostBuildStaticMesh( UStaticMesh& StaticMesh )
	{
		TRACE_CPUPROFILER_EVENT_SCOPE( UsdGeomMeshTranslatorImpl::PostBuildStaticMesh );
		StaticMesh.InitResources();

		if ( const FMeshDescription* MeshDescription = StaticMesh.GetMeshDescription( 0 ) )
		{
			StaticMesh.RenderData->Bounds = MeshDescription->GetBounds();
		}

		StaticMesh.CalculateExtendedBounds();
	}
}

FBuildStaticMeshTaskChain::FBuildStaticMeshTaskChain( const TSharedRef< FUsdSchemaTranslationContext >& InContext, const TUsdStore< pxr::UsdTyped >& InSchema, FMeshDescription&& InMeshDescription )
	: Schema( InSchema )
	, Context( InContext )
	, MeshDescription( MoveTemp( InMeshDescription ) )
{
	SetupTasks();
}

void FBuildStaticMeshTaskChain::SetupTasks()
{
	// Ignore meshes from disabled purposes
	if ( !EnumHasAllFlags( Context->PurposesToLoad, IUsdPrim::GetPurpose( Schema.Get().GetPrim() ) ) )
	{
		return;
	}

	//if ( !UsdGeomMeshTranslatorImpl::IsGeometryAnimated( GeomMesh.Get(), pxr::UsdTimeCode( Context->Time ) ) ) // TODO: This test is now invalid since we can have multiple meshes collapsed into one
	{
		constexpr bool bIsAsyncTask = true;

		// Create static mesh (Main thread)
		Do( !bIsAsyncTask,
			[ this ]()
			{
				// Force load MeshBuilderModule so that it's ready for the async tasks
				FModuleManager::LoadModuleChecked< IMeshBuilderModule >( TEXT("MeshBuilder") );

				bool bIsNew = true;
				StaticMesh = UsdGeomMeshTranslatorImpl::CreateStaticMesh( MoveTemp( MeshDescription ), *Context, bIsNew );

				FScopeLock Lock( &Context->CriticalSection );
				{
					Context->PrimPathsToAssets.Add( UsdToUnreal::ConvertPath( Schema.Get().GetPrim().GetPrimPath() ), StaticMesh );
				}

				bool bMaterialsHaveChanged = false;
				if ( StaticMesh )
				{
					bMaterialsHaveChanged = UsdGeomMeshTranslatorImpl::ProcessMaterials( Schema.Get().GetPrim(), *StaticMesh, Context->PrimPathsToAssets, Context->Time );
				}

				const bool bContinueTaskChain = ( bIsNew || bMaterialsHaveChanged );
				return bContinueTaskChain;
			} );

		// Commit mesh description (Async)
		Then( bIsAsyncTask,
			[ this ]()
			{
				UStaticMesh::FCommitMeshDescriptionParams Params;
				Params.bMarkPackageDirty = false;
				Params.bUseHashAsGuid = true;

				StaticMesh->CommitMeshDescription( 0, Params );

				return true;
			} );

		// PreBuild static mesh (Main thread)
		Then( !bIsAsyncTask,
			[ this ]()
			{
				UsdGeomMeshTranslatorImpl::PreBuildStaticMesh( Schema.Get().GetPrim(), *StaticMesh, Context->PrimPathsToAssets, Context->Time );

				return true;
			} );

		// Build static mesh (Async)
		Then( bIsAsyncTask,
			[ this ]() mutable
			{
				if ( !UsdGeomMeshTranslatorImpl::BuildStaticMesh( *StaticMesh ) )
				{
					// Build failed, discard the mesh
					StaticMesh = nullptr;

					return false;
				}

				return true;
			} );

		// PostBuild static mesh (Main thread)
		Then( !bIsAsyncTask,
			[ this ]()
			{
				UsdGeomMeshTranslatorImpl::PostBuildStaticMesh( *StaticMesh );

				return true;
			} );
	}
}

void FGeomMeshCreateAssetsTaskChain::SetupTasks()
{
	FScopedUnrealAllocs UnrealAllocs;

	// Create mesh description (Async)
	constexpr bool bIsAsyncTask = true;
	Do( bIsAsyncTask, 
		[ this ]() -> bool
		{
			MeshDescription = UsdGeomMeshTranslatorImpl::LoadMeshDescription( pxr::UsdGeomMesh( Schema.Get() ), pxr::UsdTimeCode( Context->Time ) );

			return !MeshDescription.IsEmpty();
		} );

	FBuildStaticMeshTaskChain::SetupTasks();
}

void FUsdGeomMeshTranslator::CreateAssets()
{
	TRACE_CPUPROFILER_EVENT_SCOPE( FUsdGeomMeshTranslator::CreateAssets );

	TSharedRef< FGeomMeshCreateAssetsTaskChain > AssetsTaskChain = MakeShared< FGeomMeshCreateAssetsTaskChain >( Context, pxr::UsdGeomMesh( Schema.Get() ) );

	Context->TranslatorTasks.Add( MoveTemp( AssetsTaskChain ) );
}

USceneComponent* FUsdGeomMeshTranslator::CreateComponents()
{
	TRACE_CPUPROFILER_EVENT_SCOPE( FUsdGeomMeshTranslator::CreateComponents );

	USceneComponent* RootComponent = FUsdGeomXformableTranslator::CreateComponents();

	if ( UStaticMeshComponent* StaticMeshComponent = Cast< UStaticMeshComponent >( RootComponent ) )
	{
		UStaticMesh* PrimStaticMesh = Cast< UStaticMesh >( Context->PrimPathsToAssets.FindRef( UsdToUnreal::ConvertPath( Schema.Get().GetPath() ) ) );

		if ( PrimStaticMesh != StaticMeshComponent->GetStaticMesh() )
		{
			if ( StaticMeshComponent->IsRegistered() )
			{
				StaticMeshComponent->UnregisterComponent();
			}

			StaticMeshComponent->SetStaticMesh( PrimStaticMesh );

			StaticMeshComponent->RegisterComponent();
		}
	}

	return RootComponent;
}

#endif // #if USE_USD_SDK
