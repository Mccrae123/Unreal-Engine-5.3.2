// Copyright Epic Games, Inc. All Rights Reserved.

#include "USDConversionBlueprintLibrary.h"

#include "UnrealUSDWrapper.h"
#include "USDConversionUtils.h"
#include "USDLayerUtils.h"
#include "USDLog.h"

#include "UsdWrappers/SdfLayer.h"
#include "UsdWrappers/SdfPath.h"
#include "UsdWrappers/UsdPrim.h"
#include "UsdWrappers/UsdStage.h"

FString UUsdConversionBlueprintLibrary::MakePathRelativeToLayer( const FString& AnchorLayerPath, const FString& PathToMakeRelative )
{
#if USE_USD_SDK
	if ( UE::FSdfLayer Layer = UE::FSdfLayer::FindOrOpen( *AnchorLayerPath ) )
	{
		FString Path = PathToMakeRelative;
		UsdUtils::MakePathRelativeToLayer( Layer, Path );
		return Path;
	}
	else
	{
		UE_LOG(LogUsd, Error, TEXT("Failed to find a layer with path '%s' to make the path '%s' relative to"), *AnchorLayerPath, *PathToMakeRelative );
		return PathToMakeRelative;
	}
#else
	return FString();
#endif // USE_USD_SDK
}

void UUsdConversionBlueprintLibrary::InsertSubLayer( const FString& ParentLayerPath, const FString& SubLayerPath, int32 Index /*= -1 */ )
{
#if USE_USD_SDK
	if ( UE::FSdfLayer Layer = UE::FSdfLayer::FindOrOpen( *ParentLayerPath ) )
	{
		UsdUtils::InsertSubLayer( Layer, *SubLayerPath, Index );
	}
	else
	{
		UE_LOG( LogUsd, Error, TEXT( "Failed to find a parent layer '%s' when trying to insert sublayer '%s'" ), *ParentLayerPath, *SubLayerPath );
	}
#endif // USE_USD_SDK
}

void UUsdConversionBlueprintLibrary::AddPayload( const FString& ReferencingStagePath, const FString& ReferencingPrimPath, const FString& TargetStagePath )
{
#if USE_USD_SDK
	UE::FUsdStage ReferencingStage = UnrealUSDWrapper::OpenStage( *ReferencingStagePath, EUsdInitialLoadSet::LoadAll );
	UE::FUsdStage TargetStage = UnrealUSDWrapper::OpenStage( *TargetStagePath, EUsdInitialLoadSet::LoadAll );
	if ( !ReferencingStage || !TargetStage )
	{
		return;
	}

	UE::FUsdPrim ReferencingPrim = ReferencingStage.GetPrimAtPath( UE::FSdfPath( *ReferencingPrimPath ) );
	if ( !ReferencingPrim )
	{
		return;
	}

	UsdUtils::AddPayload( ReferencingPrim, *TargetStagePath );
#endif // USE_USD_SDK
}
