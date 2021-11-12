// Copyright Epic Games, Inc. All Rights Reserved.

#include "BakeMeshAttributeTool.h"
#include "InteractiveToolManager.h"
#include "ModelingToolTargetUtil.h"

using namespace UE::Geometry;

#define LOCTEXT_NAMESPACE "UBakeMeshAttributeTool"


void UBakeMeshAttributeTool::Setup()
{
	Super::Setup();

	// Setup preview materials
	UMaterial* WorkingMaterial = LoadObject<UMaterial>(nullptr, TEXT("/MeshModelingToolsetExp/Materials/InProgressMaterial"));
	check(WorkingMaterial);
	if (WorkingMaterial != nullptr)
	{
		WorkingPreviewMaterial = UMaterialInstanceDynamic::Create(WorkingMaterial, GetToolManager());
	}
}


void UBakeMeshAttributeTool::SetWorld(UWorld* World)
{
	TargetWorld = World;
}


int UBakeMeshAttributeTool::SelectColorTextureToBake(const TArray<UTexture*>& Textures)
{
	TArray<int> TextureVotes;
	TextureVotes.Init(0, Textures.Num());

	for (int TextureIndex = 0; TextureIndex < Textures.Num(); ++TextureIndex)
	{
		UTexture* Tex = Textures[TextureIndex];
		UTexture2D* Tex2D = Cast<UTexture2D>(Tex);

		if (Tex2D)
		{
			// Texture uses SRGB
			if (Tex->SRGB != 0)
			{
				++TextureVotes[TextureIndex];
			}

#if WITH_EDITORONLY_DATA
			// Texture has multiple channels
			ETextureSourceFormat Format = Tex->Source.GetFormat();
			if (Format == TSF_BGRA8 || Format == TSF_BGRE8 || Format == TSF_RGBA16 || Format == TSF_RGBA16F)
			{
				++TextureVotes[TextureIndex];
			}
#endif

			// What else? Largest texture? Most layers? Most mipmaps?
		}
	}

	int MaxIndex = -1;
	int MaxVotes = -1;
	for (int TextureIndex = 0; TextureIndex < Textures.Num(); ++TextureIndex)
	{
		if (TextureVotes[TextureIndex] > MaxVotes)
		{
			MaxIndex = TextureIndex;
			MaxVotes = TextureVotes[TextureIndex];
		}
	}

	return MaxIndex;
}

void UBakeMeshAttributeTool::UpdateMultiTextureMaterialIDs(
	UToolTarget* Target,
	TArray<TObjectPtr<UTexture2D>>& AllSourceTextures,
	TArray<TObjectPtr<UTexture2D>>& MaterialIDTextures)
{
	ProcessComponentTextures(UE::ToolTarget::GetTargetComponent(Target),
		[&AllSourceTextures, &MaterialIDTextures](const int NumMaterials, const int MaterialID, const TArray<UTexture*>& Textures)
	{
		MaterialIDTextures.SetNumZeroed(NumMaterials);
			
		for (UTexture* Tex : Textures)
		{
			UTexture2D* Tex2D = Cast<UTexture2D>(Tex);
			if (Tex2D)
			{
				AllSourceTextures.Add(Tex2D);
			}
		}

		UTexture2D* Tex2D = nullptr;
		constexpr bool bGuessAtTextures = true;
		if constexpr (bGuessAtTextures)
		{
			const int SelectedTextureIndex = SelectColorTextureToBake(Textures);
			if (SelectedTextureIndex >= 0)
			{
				Tex2D = Cast<UTexture2D>(Textures[SelectedTextureIndex]);	
			}
		}
		MaterialIDTextures[MaterialID] = Tex2D;
	});
}

#undef LOCTEXT_NAMESPACE

