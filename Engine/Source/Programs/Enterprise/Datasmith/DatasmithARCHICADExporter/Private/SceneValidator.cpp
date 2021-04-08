// Copyright Epic Games, Inc. All Rights Reserved.

#include "SceneValidator.h"
#include "DatasmithUtils.h"

BEGIN_NAMESPACE_UE_AC

FSceneValidator::FSceneValidator(const TSharedRef< IDatasmithScene >& InScene)
	: Scene(InScene)
{
}

FString FSceneValidator::GetElementTypes(const IDatasmithElement& InElement)
{
	const int32 ReserveForFastCatenation = 100;
	FString		TypesString(TEXT(""), ReserveForFastCatenation);
	if (InElement.IsA(EDatasmithElementType::StaticMesh))
	{
		TypesString += TEXT(", StaticMesh");
	}
	if (InElement.IsA(EDatasmithElementType::Actor))
	{
		TypesString += TEXT(", Actor");
	}
	if (InElement.IsA(EDatasmithElementType::StaticMeshActor))
	{
		TypesString += TEXT(", StaticMeshActor");
	}
	if (InElement.IsA(EDatasmithElementType::Light))
	{
		TypesString += TEXT(", Light");
	}
	if (InElement.IsA(EDatasmithElementType::PointLight))
	{
		TypesString += TEXT(", PointLight");
	}
	if (InElement.IsA(EDatasmithElementType::SpotLight))
	{
		TypesString += TEXT(", SpotLight");
	}
	if (InElement.IsA(EDatasmithElementType::DirectionalLight))
	{
		TypesString += TEXT(", DirectionalLight");
	}
	if (InElement.IsA(EDatasmithElementType::AreaLight))
	{
		TypesString += TEXT(", AreaLight");
	}
	if (InElement.IsA(EDatasmithElementType::LightmassPortal))
	{
		TypesString += TEXT(", LightmassPortal");
	}
	if (InElement.IsA(EDatasmithElementType::EnvironmentLight))
	{
		TypesString += TEXT(", EnvironmentLight");
	}
	if (InElement.IsA(EDatasmithElementType::Camera))
	{
		TypesString += TEXT(", Camera");
	}
	if (InElement.IsA(EDatasmithElementType::Shader))
	{
		TypesString += TEXT(", Shader");
	}
	if (InElement.IsA(EDatasmithElementType::BaseMaterial))
	{
		TypesString += TEXT(", BaseMaterial");
	}
	if (InElement.IsA(EDatasmithElementType::MasterMaterial))
	{
		TypesString += TEXT(", MasterMaterial");
	}
	if (InElement.IsA(EDatasmithElementType::KeyValueProperty))
	{
		TypesString += TEXT(", KeyValueProperty");
	}
	if (InElement.IsA(EDatasmithElementType::Texture))
	{
		TypesString += TEXT(", Texture");
	}
	if (InElement.IsA(EDatasmithElementType::MaterialId))
	{
		TypesString += TEXT(", MaterialId");
	}
	if (InElement.IsA(EDatasmithElementType::Scene))
	{
		TypesString += TEXT(", Scene");
	}
	if (InElement.IsA(EDatasmithElementType::MetaData))
	{
		TypesString += TEXT(", MetaData");
	}
	if (InElement.IsA(EDatasmithElementType::CustomActor))
	{
		TypesString += TEXT(", CustomActor");
	}
	if (InElement.IsA(EDatasmithElementType::Material))
	{
		TypesString += TEXT(", Material");
	}
	if (InElement.IsA(EDatasmithElementType::Landscape))
	{
		TypesString += TEXT(", Landscape");
	}
	if (InElement.IsA(EDatasmithElementType::UEPbrMaterial))
	{
		TypesString += TEXT(", UEPbrMaterial");
	}
	if (InElement.IsA(EDatasmithElementType::PostProcessVolume))
	{
		TypesString += TEXT(", PostProcessVolume");
	}
	if (InElement.IsA(EDatasmithElementType::LevelSequence))
	{
		TypesString += TEXT(", LevelSequence");
	}
	if (InElement.IsA(EDatasmithElementType::Animation))
	{
		TypesString += TEXT(", Animation");
	}
	if (InElement.IsA(EDatasmithElementType::HierarchicalInstanceStaticMesh))
	{
		TypesString += TEXT(", HierarchicalInstanceStaticMesh");
	}
	if (InElement.IsA(EDatasmithElementType::Variant))
	{
		TypesString += TEXT(", Variant");
	}
	if (InElement.IsA(EDatasmithElementType::Variant))
	{
		TypesString += TEXT(", Variant");
	}
	if (InElement.IsA(EDatasmithElementType::Decal))
	{
		TypesString += TEXT(", Decal");
	}
	if (InElement.IsA(EDatasmithElementType::DecalMaterial))
	{
		TypesString += TEXT(", DecalMaterial");
	}
	if (InElement.IsA(EDatasmithElementType::MaterialExpression))
	{
		TypesString += TEXT(", MaterialExpression");
	}
	if (InElement.IsA(EDatasmithElementType::MaterialExpressionInput))
	{
		TypesString += TEXT(", MaterialExpressionInput");
	}
	if (InElement.IsA(EDatasmithElementType::MaterialExpressionOutput))
	{
		TypesString += TEXT(", MaterialExpressionOutput");
	}

	if (TypesString.Len() < 2)
	{
		AddMessage(kBug, TEXT("FSceneValidator::GetElementTypes - Unknown element types \"%s\""), InElement.GetName());
		TypesString = TEXT("Unknown type");
	}
	else
	{
		TypesString.RightChopInline(2);
	}

	return TypesString;
}

FString FSceneValidator::GetElementsDescription(const IDatasmithElement& InElement)
{
	FString Types = GetElementTypes(InElement);
	return FString::Printf(TEXT("Types(%s), Name=\"%s\", Label=\"%s\""), *Types, InElement.GetName(),
						   InElement.GetLabel());
}

void FSceneValidator::AddElements(const IDatasmithElement& InElement, FMapNameToUsage* IOMap)
{
	if (IOMap != nullptr)
	{
		FUsage& Usage = (*IOMap)[InElement.GetName()];
		if (Usage.bExist == true)
		{
			AddMessage(kBug, TEXT("Element duplicated %s"), *GetElementsDescription(InElement));
		}
		Usage.bExist = true;
	}

	std::pair< const TCHAR*, const IDatasmithElement* > value(InElement.GetName(), &InElement);
	std::pair< MapNameToElement::iterator, bool >		result = NameToElementMap.insert(value);
	if (result.second == false)
	{
		AddMessage(kError, TEXT("Elements with same\n\tNew Element %s\n\tOld Element %s"),
				   *GetElementsDescription(InElement), *GetElementsDescription(*result.first->second));
	}
	else
	{
		if (FCString::Strcmp(InElement.GetName(), *FDatasmithUtils::SanitizeObjectName(InElement.GetName())) != 0)
		{
			AddMessage(kError, TEXT("Elements name isn't Sanitized %s"), *GetElementsDescription(InElement));
		}
	}
}

void FSceneValidator::AddMessageImpl(TInfoLevel Level, const FString& Message)
{
	UE_AC_Assert(Level >= kBug && Level < kInfoLevelMax);
	++MessagesCounts[Level];
	Messages.push_back(FMessage(Level, Message));
}

const utf8_t* FSceneValidator::LevelName(TInfoLevel Level)
{
	return Level == kBug	   ? "Bug"
		   : Level == kError   ? "Error"
		   : Level == kWarning ? "Warning"
		   : Level == kVerbose ? "Verbose"
							   : "???????";
}

void FSceneValidator::PrintReports(TInfoLevel InLevel)
{
	// Reports counts for each level
	const TInfoLevel Levels[] = {TInfoLevel::kBug, TInfoLevel::kError, TInfoLevel::kWarning, TInfoLevel::kVerbose};
	for (const auto Level : Levels)
	{
		if (Level <= InLevel && MessagesCounts[Level] != 0)
		{
			UE_AC_TraceF("%d %ss collected\n", MessagesCounts[Level], LevelName(Level));
		}
	}

	// Reports messages
	for (const FMessage& Message : Messages)
	{
		if (Message.Level <= InLevel)
		{
			UE_AC_TraceF("%-7s:%s\n", LevelName(Message.Level), TCHAR_TO_UTF8(*Message.Message));
		}
	}
}

void FSceneValidator::CheckElementsName()
{
	const IDatasmithScene& MyScene = *Scene;

	int32 Index;
	int32 Count;

	Count = MyScene.GetTexturesCount();
	for (Index = 0; Index < Count; ++Index)
	{
		const TSharedPtr< IDatasmithTextureElement >& Texture = MyScene.GetTexture(Index);
		if (Texture.IsValid())
		{
			AddElements(*Texture, &TexturesUsages);
		}
		else
		{
			AddMessage(kBug, TEXT("Texture %d is invalid"), Index);
		}
	}

	Count = MyScene.GetMaterialsCount();
	for (Index = 0; Index < Count; ++Index)
	{
		const TSharedPtr< IDatasmithBaseMaterialElement >& Material = MyScene.GetMaterial(Index);
		if (Material.IsValid())
		{
			AddElements(*Material, &MaterialsUsages);
		}
		else
		{
			AddMessage(kBug, TEXT("Material %d is invalid"), Index);
		}
	}

	Count = MyScene.GetMeshesCount();
	for (Index = 0; Index < Count; ++Index)
	{
		const TSharedPtr< IDatasmithMeshElement >& Mesh = MyScene.GetMesh(Index);
		if (Mesh.IsValid())
		{
			AddElements(*Mesh, &MeshesUsages);
		}
		else
		{
			AddMessage(kBug, TEXT("Mesh %d is invalid"), Index);
		}
	}

	Count = MyScene.GetActorsCount();
	for (Index = 0; Index < Count; ++Index)
	{
		const TSharedPtr< IDatasmithActorElement >& Actor = MyScene.GetActor(Index);
		if (Actor.IsValid())
		{
			CheckActorsName(*Actor);
		}
		else
		{
			AddMessage(kBug, TEXT("Actor %d is invalid"), Index);
		}
	}

	if (MyScene.GetPostProcess().IsValid())
	{
		AddElements(*MyScene.GetPostProcess());
	}

	if (MyScene.GetPostProcess().IsValid())
	{
		AddElements(*MyScene.GetPostProcess());
	}

	Count = MyScene.GetMetaDataCount();
	for (Index = 0; Index < Count; ++Index)
	{
		const TSharedPtr< IDatasmithMetaDataElement >& MetaData = MyScene.GetMetaData(Index);
		if (MetaData.IsValid())
		{
			AddElements(*MetaData);
		}
		else
		{
			AddMessage(kBug, TEXT("MetaData %d is invalid"), Index);
		}
	}

	Count = MyScene.GetLevelSequencesCount();
	for (Index = 0; Index < Count; ++Index)
	{
		const TSharedPtr< IDatasmithLevelSequenceElement >& LevelSequence =
			Scene->GetLevelSequence(Index); // No const getter
		if (LevelSequence.IsValid())
		{
			AddElements(*LevelSequence, &LevelSequencesUsages);
		}
		else
		{
			AddMessage(kBug, TEXT("LevelSequence %d is invalid"), Index);
		}
	}

	Count = MyScene.GetLevelVariantSetsCount();
	for (Index = 0; Index < Count; ++Index)
	{
		const TSharedPtr< IDatasmithLevelVariantSetsElement >& LevelVariant =
			Scene->GetLevelVariantSets(Index); // No const getter
		if (LevelVariant.IsValid())
		{
			AddElements(*LevelVariant);
		}
		else
		{
			AddMessage(kBug, TEXT("LevelVariant %d is invalid"), Index);
		}
	}
}

void FSceneValidator::CheckActorsName(const IDatasmithActorElement& InActor)
{
	AddElements(InActor, &ActorsUsages);
	int32 Count = InActor.GetChildrenCount();
	for (int32 Index = 0; Index < Count; ++Index)
	{
		const TSharedPtr< IDatasmithActorElement >& Child = InActor.GetChild(Index);
		if (Child.IsValid())
		{
			CheckActorsName(*Child);
		}
		else
		{
			AddMessage(kBug, TEXT("Child Actor %d is invalid. Parent is %s"), Index, *GetElementsDescription(InActor));
		}
	}
}

void FSceneValidator::CheckDependances()
{
	const IDatasmithScene& MyScene = *Scene;
	int32				   Index;
	int32				   Count;

	Count = MyScene.GetMetaDataCount();
	for (Index = 0; Index < Count; ++Index)
	{
		const TSharedPtr< IDatasmithMetaDataElement >& MetaData = MyScene.GetMetaData(Index);
		if (MetaData.IsValid())
		{
			if (MetaData->GetAssociatedElement().IsValid())
			{
				ActorsUsages[MetaData->GetAssociatedElement()->GetName()].bIsRefered = true;
			}
			else
			{
				AddMessage(kError, TEXT("Metadata without actor %d %s"), Index, *GetElementsDescription(*MetaData));
			}
		}
	}

	Count = MyScene.GetActorsCount();
	for (Index = 0; Index < Count; ++Index)
	{
		const TSharedPtr< IDatasmithActorElement >& Actor = MyScene.GetActor(Index);
		if (Actor.IsValid())
		{
			CheckActorsDependances(*Actor);
		}
	}
}

void FSceneValidator::CheckActorsDependances(const IDatasmithActorElement& InActor)
{
	int32 Count;
	int32 Index;

	{
		std::set< FNamePtr > Tags;
		Count = InActor.GetTagsCount();
		for (Index = 0; Index < Count; ++Index)
		{
			const TCHAR*									  Tag = InActor.GetTag(Index);
			std::pair< std::set< FNamePtr >::iterator, bool > insertedTag = Tags.insert(Tag);
			if (insertedTag.second)
			{
				AddMessage(kError, TEXT("Tag \"%s\" present twice for actor %s"), Tag,
						   *GetElementsDescription(InActor));
			}
		}
	}

	if (InActor.IsA(EDatasmithElementType::StaticMeshActor))
	{
		const IDatasmithMeshActorElement& MeshActor = static_cast< const IDatasmithMeshActorElement& >(InActor);
		Count = MeshActor.GetMaterialOverridesCount();

		// Validate the refered mesh
		const IDatasmithMeshElement* MeshPtr = nullptr;
		const TCHAR*				 MeshName = MeshActor.GetStaticMeshPathName();
		if (*MeshName)
		{
			FUsage& MeshUsage = MeshesUsages[MeshName];
			if (!MeshUsage.bIsRefered)
			{
				MeshUsage.bIsRefered = true;
				if (!MeshUsage.bExist)
				{
					AddMessage(kError, TEXT("Unknown mesh \"%s\" for actor %s"), MeshName,
							   *GetElementsDescription(InActor));
				}
			}
			if (MeshUsage.bExist && Count != 0)
			{
				MapNameToElement::iterator elementFound = NameToElementMap.find(MeshName);
				if (elementFound != NameToElementMap.end() &&
					elementFound->second->IsA(EDatasmithElementType::StaticMesh))
				{
					MeshPtr = static_cast< const IDatasmithMeshElement* >(elementFound->second);
				}
			}
		}
		else
		{
			AddMessage(kWarning, TEXT("Mesh actor without mesh %s"), *GetElementsDescription(MeshActor));
		}

		// Validate overload id
		std::set< int32 > MaterialIds;
		for (Index = 0; Index < Count; ++Index)
		{
			TSharedPtr< const IDatasmithMaterialIDElement > MaterialOverride = MeshActor.GetMaterialOverride(Index);
			if (MaterialOverride.IsValid())
			{
				int32										   MaterialId = MaterialOverride->GetId();
				std::pair< std::set< int32 >::iterator, bool > insertedTag = MaterialIds.insert(MaterialId);
				if (insertedTag.second)
				{
					AddMessage(kError, TEXT("Multiple overload for same id (%d) for actor %d %s"), MaterialId, Index,
							   *GetElementsDescription(MeshActor));
				}
				else
				{
					if (MeshPtr != nullptr)
					{
						// We have a mesh, so we can validate the override id...
					}
				}
				FUsage& MaterialUsage = MaterialsUsages[MaterialOverride->GetName()];
				if (!MaterialUsage.bIsRefered)
				{
					MaterialUsage.bIsRefered = true;
					if (!MaterialUsage.bExist)
					{
						AddMessage(kError, TEXT("Unknown material \"%s\" for material overloaded for actor %d %s"),
								   MaterialOverride->GetName(), Index, *GetElementsDescription(MeshActor));
					}
				}
			}
			else
			{
				AddMessage(kError, TEXT("Invalid material override %d %s"), Index, *GetElementsDescription(MeshActor));
			}
		}
	}

	// Validate childrens
	Count = InActor.GetChildrenCount();
	for (Index = 0; Index < Count; ++Index)
	{
		const TSharedPtr< IDatasmithActorElement >& Child = InActor.GetChild(Index);
		if (Child.IsValid())
		{
			CheckActorsDependances(*Child);
		}
	}
}

END_NAMESPACE_UE_AC
