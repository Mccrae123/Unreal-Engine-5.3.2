// Copyright Epic Games, Inc. All Rights Reserved.

#include "USDIntegrationUtils.h"

#include "UnrealUSDWrapper.h"
#include "USDTypesConversion.h"
#include "UsdWrappers/SdfChangeBlock.h"

#include "Animation/AnimInstance.h"
#include "Components/SkeletalMeshComponent.h"
#include "LiveLinkComponentController.h"

#if USE_USD_SDK
#include "USDIncludesStart.h"
	#include "pxr/usd/usd/attribute.h"
	#include "pxr/usd/usd/prim.h"
#include "USDIncludesEnd.h"

#define LOCTEXT_NAMESPACE "USDIntegrationUtils"

bool UsdUtils::PrimHasLiveLinkSchema( const pxr::UsdPrim& Prim )
{
	if ( !Prim )
	{
		return false;
	}

    FScopedUsdAllocs Allocs;

    pxr::TfType Schema = pxr::UsdSchemaRegistry::GetTypeFromSchemaTypeName( UnrealIdentifiers::LiveLinkAPI );
    return Prim.HasAPI( Schema );
}

void UnrealToUsd::ConvertLiveLinkProperties( const UActorComponent* InComponent, pxr::UsdPrim& InOutPrim )
{
#if WITH_EDITOR
	if ( !InOutPrim || !UsdUtils::PrimHasLiveLinkSchema( InOutPrim ) )
	{
		return;
	}

	FScopedUsdAllocs Allocs;

	UE::FSdfChangeBlock ChangeBlock;

	// Skeletal LiveLink case
	if ( const USkeletalMeshComponent* SkeletalComponent = Cast<USkeletalMeshComponent>( InComponent ) )
	{
		if ( pxr::UsdAttribute Attr = InOutPrim.CreateAttribute( UnrealIdentifiers::UnrealLiveLinkAnimBlueprintPath, pxr::SdfValueTypeNames->String ) )
		{
			FString AnimBPPath = SkeletalComponent->AnimClass && SkeletalComponent->AnimClass->ClassGeneratedBy
				? SkeletalComponent->AnimClass->ClassGeneratedBy->GetPathName()
				: FString();

			Attr.Set( UnrealToUsd::ConvertString( *AnimBPPath ).Get(), pxr::UsdTimeCode::Default() );
		}

		if ( pxr::UsdAttribute Attr = InOutPrim.CreateAttribute( UnrealIdentifiers::UnrealLiveLinkEnabled, pxr::SdfValueTypeNames->Bool ) )
		{
			const bool bEnabled = SkeletalComponent->GetAnimationMode() == EAnimationMode::AnimationBlueprint;
			Attr.Set( bEnabled, pxr::UsdTimeCode::Default() );
		}
	}
	// Non-skeletal LiveLink case
	else if ( const ULiveLinkComponentController* InController = Cast<ULiveLinkComponentController>( InComponent ) )
	{
		if ( pxr::UsdAttribute Attr = InOutPrim.CreateAttribute( UnrealIdentifiers::UnrealLiveLinkSubjectName, pxr::SdfValueTypeNames->String ) )
		{
			Attr.Set( UnrealToUsd::ConvertString( *InController->SubjectRepresentation.Subject.Name.ToString() ).Get(), pxr::UsdTimeCode::Default() );
		}

		if ( pxr::UsdAttribute Attr = InOutPrim.CreateAttribute( UnrealIdentifiers::UnrealLiveLinkEnabled, pxr::SdfValueTypeNames->Bool ) )
		{
			Attr.Set( InController->bEvaluateLiveLink, pxr::UsdTimeCode::Default() );
		}
	}
#endif // WITH_EDITOR
}

#undef LOCTEXT_NAMESPACE
#endif // USE_USD_SDK