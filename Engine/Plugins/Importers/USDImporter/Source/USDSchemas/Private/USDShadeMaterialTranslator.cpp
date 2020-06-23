// Copyright Epic Games, Inc. All Rights Reserved.

#include "USDShadeMaterialTranslator.h"

#include "USDAssetImportData.h"
#include "USDShadeConversion.h"
#include "USDTypesConversion.h"

#include "UsdWrappers/SdfPath.h"

#include "Materials/Material.h"
#include "Misc/SecureHash.h"

#if USE_USD_SDK

#include "USDIncludesStart.h"
	#include "pxr/usd/usdShade/material.h"
#include "USDIncludesEnd.h"


void FUsdShadeMaterialTranslator::CreateAssets()
{
	pxr::UsdShadeMaterial ShadeMaterial( Schema );

	if ( !ShadeMaterial )
	{
		return;
	}

	FString MaterialHashString = UsdUtils::HashShadeMaterial( ShadeMaterial ).ToString();

	UObject*& CachedMaterial = Context->AssetsCache.FindOrAdd( MaterialHashString );

	if ( !CachedMaterial )
	{
		UMaterial* NewMaterial = NewObject< UMaterial >( GetTransientPackage(), NAME_None, Context->ObjectFlags );

		UUsdAssetImportData* ImportData = NewObject< UUsdAssetImportData >( NewMaterial, TEXT("USDAssetImportData") );
		ImportData->PrimPath = Schema.GetPath().GetString();
		NewMaterial->AssetImportData = ImportData;

		if ( UsdToUnreal::ConvertMaterial( ShadeMaterial, *NewMaterial, Context->AssetsCache ) )
		{
			//UMaterialEditingLibrary::RecompileMaterial( CachedMaterial ); // Too slow
			NewMaterial->PostEditChange();
		}
		else
		{
			NewMaterial = nullptr;
		}

		// ConvertMaterial may have added other items to AssetsCache, so lets update the reference to make sure its ok
		CachedMaterial = Context->AssetsCache.Add( MaterialHashString, NewMaterial );
	}

	FScopeLock Lock( &Context->CriticalSection );
	{
		Context->PrimPathsToAssets.Add( Schema.GetPath().GetString(), CachedMaterial );
	}
}

#endif // #if USE_USD_SDK
