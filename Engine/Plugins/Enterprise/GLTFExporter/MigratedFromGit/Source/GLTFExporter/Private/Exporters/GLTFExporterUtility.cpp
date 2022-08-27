// Copyright Epic Games, Inc. All Rights Reserved.

#include "Exporters/GLTFExporterUtility.h"
#include "Materials/MaterialInstance.h"
#include "AssetRegistryModule.h"

void FGLTFExporterUtility::GetSelectedActors(TSet<AActor*>& OutSelectedActors)
{
#if WITH_EDITOR
	const TMap<const UObjectBase*, FBoolAnnotation>& AnnotationMap = ((const FUObjectAnnotationSparse<FBoolAnnotation, true>&)GSelectedActorAnnotation).GetAnnotationMap();
	OutSelectedActors.Reserve(AnnotationMap.Num());

	for (const TPair<const UObjectBase*, FBoolAnnotation>& Pair : AnnotationMap)
	{
		if (Pair.Value.Mark)
		{
			OutSelectedActors.Add(static_cast<AActor*>(const_cast<UObjectBase*>(Pair.Key)));
		}
	}
#endif
}

const UStaticMesh* FGLTFExporterUtility::GetPreviewMesh(const UMaterialInterface* Material)
{
#if WITH_EDITORONLY_DATA
	do
	{
		const UStaticMesh* PreviewMesh = Cast<UStaticMesh>(Material->PreviewMesh.TryLoad());
		if (PreviewMesh != nullptr)
		{
			return PreviewMesh;
		}

		const UMaterialInstance* MaterialInstance = Cast<UMaterialInstance>(Material);
		Material = MaterialInstance != nullptr ? MaterialInstance->Parent : nullptr;
	} while (Material != nullptr);
#endif

	return nullptr;
}

const USkeletalMesh* FGLTFExporterUtility::GetPreviewMesh(const UAnimSequence* AnimSequence)
{
	const USkeletalMesh* PreviewMesh = AnimSequence->GetPreviewMesh();
	if (PreviewMesh == nullptr)
	{
		const USkeleton* Skeleton = AnimSequence->GetSkeleton();
		if (Skeleton != nullptr)
		{
			PreviewMesh = Skeleton->GetPreviewMesh();
			if (PreviewMesh == nullptr)
			{
				PreviewMesh = FindCompatibleMesh(Skeleton);
			}
		}
	}

	return PreviewMesh;
}

const USkeletalMesh* FGLTFExporterUtility::FindCompatibleMesh(const USkeleton *Skeleton)
{
#if (ENGINE_MAJOR_VERSION > 4 || ENGINE_MINOR_VERSION >= 27)
	const FName SkeletonMemberName = USkeletalMesh::GetSkeletonMemberName();
#else
	const FName SkeletonMemberName = GET_MEMBER_NAME_CHECKED(USkeletalMesh, Skeleton);
#endif

	FARFilter Filter;
	Filter.ClassNames.Add(USkeletalMesh::StaticClass()->GetFName());
	Filter.TagsAndValues.Add(SkeletonMemberName, FAssetData(Skeleton).GetExportTextName());

	TArray<FAssetData> FilteredAssets;
	FAssetRegistryModule::GetRegistry().GetAssets(Filter, FilteredAssets);

	for (const FAssetData& Asset : FilteredAssets)
	{
		const USkeletalMesh* SkeletalMesh = Cast<USkeletalMesh>(Asset.GetAsset());
		if (SkeletalMesh != nullptr)
		{
			return SkeletalMesh;
		}
	}

	return nullptr;
}

TArray<UWorld*> FGLTFExporterUtility::GetAssociatedWorlds(const UObject* Object)
{
	TArray<UWorld*> Worlds;
	TArray<FAssetIdentifier> Dependencies;

	const FName OuterPathName = *Object->GetOutermost()->GetPathName();
	FAssetRegistryModule::GetRegistry().GetDependencies(OuterPathName, Dependencies);

	for (FAssetIdentifier& Dependency : Dependencies)
	{
		FString PackageName = Dependency.PackageName.ToString();
		UWorld* World = LoadObject<UWorld>(nullptr, *PackageName, nullptr, LOAD_NoWarn);
		if (World != nullptr)
		{
			Worlds.AddUnique(World);
		}
	}

	return Worlds;
}
