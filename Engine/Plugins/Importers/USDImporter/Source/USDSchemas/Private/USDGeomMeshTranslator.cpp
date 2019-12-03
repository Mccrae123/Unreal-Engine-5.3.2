// Copyright 1998-2019 Epic Games, Inc. All Rights Reserved.

#include "USDGeomMeshTranslator.h"

#if USE_USD_SDK

#include "USDConversionUtils.h"
#include "USDGeomMeshConversion.h"
#include "USDSchemasModule.h"
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
#include "PhysicsEngine/BodySetup.h"
#include "StaticMeshAttributes.h"

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

				//bHasAttributesTimeSamples = Attribute.ValueMightBeTimeVarying();

				double DesiredTime = TimeCode.GetValue();
				double MinTime = 0.0;
				double MaxTime = 0.0;
				bool bHasTimeSamples = false;

				Attribute.GetBracketingTimeSamples( DesiredTime, &MinTime, &MaxTime, &bHasTimeSamples );
				if ( bHasTimeSamples && DesiredTime >= MinTime && DesiredTime <= MaxTime )
				{
					bHasAttributesTimeSamples = true;
					break;
				}
			}
		}

		return bHasAttributesTimeSamples;
	}

	void ProcessMaterials( const pxr::UsdPrim& UsdPrim, UStaticMesh& StaticMesh, TMap< FString, UObject* >& MaterialsCache, bool bHasPrimDisplayColor, float Time )
	{
		const FMeshDescription* MeshDescription = StaticMesh.GetMeshDescription( 0 );

		if ( !MeshDescription )
		{
			return;
		}

		FStaticMeshConstAttributes StaticMeshAttributes( *MeshDescription );

		for ( const FPolygonGroupID PolygonGroupID : MeshDescription->PolygonGroups().GetElementIDs() )
		{
			const FName& ImportedMaterialSlotName = StaticMeshAttributes.GetPolygonGroupMaterialSlotNames()[ PolygonGroupID ];
			const FName MaterialSlotName = ImportedMaterialSlotName;

			int32 MaterialIndex = INDEX_NONE;
			int32 MeshMaterialIndex = 0;

			for ( FStaticMaterial& StaticMaterial : StaticMesh.StaticMaterials )
			{
				if ( StaticMaterial.MaterialSlotName.IsEqual( ImportedMaterialSlotName ) )
				{
					MaterialIndex = MeshMaterialIndex;
					break;
				}

				++MeshMaterialIndex;
			}

			if ( MaterialIndex == INDEX_NONE )
			{
				MaterialIndex = PolygonGroupID.GetValue();
			}

			UMaterialInterface* Material = nullptr;
			TUsdStore< pxr::UsdPrim > MaterialPrim = UsdPrim.GetStage()->GetPrimAtPath( UnrealToUsd::ConvertPath( *ImportedMaterialSlotName.ToString() ).Get() );

			if ( MaterialPrim.Get() )
			{
				UObject*& CachedMaterial = MaterialsCache.FindOrAdd( UsdToUnreal::ConvertPath( MaterialPrim.Get().GetPrimPath() ) );

				if ( !CachedMaterial )
				{
					UMaterial* NewMaterial = NewObject< UMaterial >();

					if ( UsdToUnreal::ConvertMaterial( pxr::UsdShadeMaterial( MaterialPrim.Get() ), *NewMaterial ) )
					{
						//UMaterialEditingLibrary::RecompileMaterial( CachedMaterial ); // Too slow
						NewMaterial->PostEditChange();
					}
					else
					{
						NewMaterial = nullptr;
					}

					Material = NewMaterial;
				}
				else
				{
					Material = Cast< UMaterialInterface >( CachedMaterial );
				}
			}

			if ( Material == nullptr && bHasPrimDisplayColor )
			{
				UMaterialInstanceConstant* MaterialInstance = NewObject< UMaterialInstanceConstant >();
				if ( UsdToUnreal::ConvertDisplayColor( pxr::UsdGeomMesh( UsdPrim ), *MaterialInstance, pxr::UsdTimeCode( Time ) ) )
				{
					Material = MaterialInstance;
				}
			}

			FStaticMaterial StaticMaterial( Material, MaterialSlotName, ImportedMaterialSlotName );
				StaticMesh.StaticMaterials.Add( StaticMaterial );

			FMeshSectionInfo MeshSectionInfo;
			MeshSectionInfo.MaterialIndex = MaterialIndex;

			StaticMesh.GetSectionInfoMap().Set( 0, PolygonGroupID.GetValue(), MeshSectionInfo );
		}
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

	UStaticMesh* CreateStaticMesh( const pxr::UsdGeomMesh& UsdMesh, FMeshDescription&& MeshDescription, FUsdSchemaTranslationContext& Context, bool& bOutIsNew )
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

	void PreBuildStaticMesh( const pxr::UsdGeomMesh& UsdMesh, UStaticMesh& StaticMesh, TMap< FString, UObject* >& MaterialsCache, float Time )
	{
		const bool bHasPrimDisplayColor = UsdMesh.GetDisplayColorPrimvar().IsDefined();
		ProcessMaterials( UsdMesh.GetPrim(), StaticMesh, MaterialsCache, bHasPrimDisplayColor, Time );

		StaticMesh.RenderData = MakeUnique< FStaticMeshRenderData >();
		//StaticMesh.BodySetup = NewObject< UBodySetup >( &StaticMesh );
	}

	bool BuildStaticMesh( UStaticMesh& StaticMesh, IMeshBuilderModule& MeshBuilderModule )
	{
		//StaticMesh.BodySetup->DefaultInstance.SetCollisionProfileName( UCollisionProfile::BlockAll_ProfileName );

		ITargetPlatformManagerModule& TargetPlatformManager = GetTargetPlatformManagerRef();
		ITargetPlatform* RunningPlatform = TargetPlatformManager.GetRunningTargetPlatform();
		check(RunningPlatform);

		const FStaticMeshLODSettings& LODSettings = RunningPlatform->GetStaticMeshLODSettings();

		const FStaticMeshLODGroup& LODGroup = LODSettings.GetLODGroup(StaticMesh.LODGroup);

		if ( !MeshBuilderModule.BuildMesh( *StaticMesh.RenderData, &StaticMesh, LODGroup ) )
		{
			return false;
		}

		for ( FStaticMeshLODResources& LODResources : StaticMesh.RenderData->LODResources )
		{
			LODResources.bHasColorVertexData = true;
		}

		return true;
	}

	void PostBuildStaticMesh( UStaticMesh& StaticMesh )
	{
		//StaticMesh.CreateBodySetup();
		StaticMesh.InitResources();

		if ( const FMeshDescription* MeshDescription = StaticMesh.GetMeshDescription( 0 ) )
		{
			StaticMesh.RenderData->Bounds = MeshDescription->GetBounds();
		}
	
		StaticMesh.CalculateExtendedBounds();
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
			MeshDescription = UsdGeomMeshTranslatorImpl::LoadMeshDescription( GeomMesh.Get(), pxr::UsdTimeCode( Context->Time ) );

			return !MeshDescription.IsEmpty();
		} );
		
	if ( !UsdGeomMeshTranslatorImpl::IsGeometryAnimated( GeomMesh.Get(), pxr::UsdTimeCode( Context->Time ) ) )
	{
		IMeshBuilderModule* MeshBuilderModule = &FModuleManager::LoadModuleChecked< IMeshBuilderModule >( TEXT("MeshBuilder") );

		// Create static mesh (Main thread)
		Then( !bIsAsyncTask,
			[ this ]()
			{
				bool bIsNew = true;
				StaticMesh = UsdGeomMeshTranslatorImpl::CreateStaticMesh( GeomMesh.Get(), MoveTemp( MeshDescription ), *Context, bIsNew );

				FScopeLock Lock( &Context->CriticalSection );
				{
					Context->PrimPathsToAssets.Add( UsdToUnreal::ConvertPath( GeomMesh.Get().GetPrim().GetPrimPath() ), StaticMesh );
				}

				if ( !bIsNew )
				{
					StaticMesh = nullptr;
				}

				return bIsNew;
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
				UsdGeomMeshTranslatorImpl::PreBuildStaticMesh( GeomMesh.Get(), *StaticMesh, Context->AssetsCache, Context->Time );

				return true;
			} );

		// Build static mesh (Async)
		Then( bIsAsyncTask,
			[ this, MeshBuilderModule ]() mutable
			{
				if ( !UsdGeomMeshTranslatorImpl::BuildStaticMesh( *StaticMesh, *MeshBuilderModule ) )
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

void FUsdGeomMeshTranslator::RegisterTranslator()
{
	IUsdSchemasModule& UsdSchemasModule = FModuleManager::Get().LoadModuleChecked< IUsdSchemasModule >( TEXT("USDSchemas") );
	UsdSchemasModule.GetTranslatorRegistry().Register< FUsdGeomMeshTranslator >( TEXT("UsdGeomMesh") );
}

void FUsdGeomMeshTranslator::CreateAssets()
{
	TSharedRef< FGeomMeshCreateAssetsTaskChain > AssetsTaskChain = MakeShared< FGeomMeshCreateAssetsTaskChain >( Context, pxr::UsdGeomMesh( Schema.Get() ) );

	Context->TranslatorTasks.Add( MoveTemp( AssetsTaskChain ) );
}

USceneComponent* FUsdGeomMeshTranslator::CreateComponents()
{
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
