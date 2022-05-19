// Copyright Epic Games, Inc. All Rights Reserved. 

#include "InterchangeGenericScenesPipeline.h"

#include "InterchangeActorFactoryNode.h"
#include "InterchangeCameraNode.h"
#include "InterchangeCineCameraFactoryNode.h"
#include "InterchangeCommonPipelineDataFactoryNode.h"
#include "InterchangeLightNode.h"
#include "InterchangeMeshActorFactoryNode.h"
#include "InterchangeMeshNode.h"
#include "InterchangePipelineLog.h"
#include "InterchangePipelineMeshesUtilities.h"
#include "InterchangeSceneNode.h"

#include "Animation/SkeletalMeshActor.h"
#include "CineCameraActor.h"
#include "Engine/DirectionalLight.h"
#include "Engine/PointLight.h"
#include "Engine/RectLight.h"
#include "Engine/SpotLight.h"
#include "Engine/StaticMeshActor.h"

void UInterchangeGenericLevelPipeline::ExecutePreImportPipeline(UInterchangeBaseNodeContainer* InBaseNodeContainer, const TArray<UInterchangeSourceData*>& InSourceDatas)
{
	if (!InBaseNodeContainer)
	{
		UE_LOG(LogInterchangePipeline, Warning, TEXT("UInterchangeGenericAssetsPipeline: Cannot execute pre-import pipeline because InBaseNodeContrainer is null"));
		return;
	}

	FTransform GlobalOffsetTransform = FTransform::Identity;
	if (UInterchangeCommonPipelineDataFactoryNode* CommonPipelineDataFactoryNode = UInterchangeCommonPipelineDataFactoryNode::GetUniqueInstance(InBaseNodeContainer))
	{
		CommonPipelineDataFactoryNode->GetCustomGlobalOffsetTransform(GlobalOffsetTransform);
	}

	TArray<UInterchangeSceneNode*> SceneNodes;

	//Find all translated node we need for this pipeline
	InBaseNodeContainer->IterateNodes([&SceneNodes](const FString& NodeUid, UInterchangeBaseNode* Node)
	{
		switch(Node->GetNodeContainerType())
		{
		case EInterchangeNodeContainerType::TranslatedScene:
		{
			if (UInterchangeSceneNode* SceneNode = Cast<UInterchangeSceneNode>(Node))
			{
				SceneNodes.Add(SceneNode);
			}
		}
		break;
		}
	});

	for (const UInterchangeSceneNode* SceneNode : SceneNodes)
	{
		if (SceneNode)
		{
			if (SceneNode->GetSpecializedTypeCount() > 0)
			{
				TArray<FString> SpecializeTypes;
				SceneNode->GetSpecializedTypes(SpecializeTypes);
				if (!SpecializeTypes.Contains(UE::Interchange::FSceneNodeStaticData::GetTransformSpecializeTypeString()))
				{
					//Skip any scene node that have specialized types but not the "Transform" type.
					continue;
				}
			}
			ExecuteSceneNodePreImport(InBaseNodeContainer, GlobalOffsetTransform, SceneNode, InBaseNodeContainer);
		}
	}
}

void UInterchangeGenericLevelPipeline::ExecuteSceneNodePreImport(UInterchangeBaseNodeContainer* InBaseNodeContainer, const FTransform& GlobalOffsetTransform, const UInterchangeSceneNode* SceneNode, UInterchangeBaseNodeContainer* FactoryNodeContainer)
{
	if (!SceneNode)
	{
		return;
	}

	const UInterchangeBaseNode* TranslatedAssetNode = nullptr;

	FString AssetInstanceUid;
	if (SceneNode->GetCustomAssetInstanceUid(AssetInstanceUid))
	{
		TranslatedAssetNode = FactoryNodeContainer->GetNode(AssetInstanceUid);
	}

	UInterchangeActorFactoryNode* ActorFactoryNode = CreateActorFactoryNode(SceneNode, TranslatedAssetNode, FactoryNodeContainer);

	if (!ensure(ActorFactoryNode))
	{
		return;
	}

	ActorFactoryNode->InitializeNode(TEXT("Factory_") + SceneNode->GetUniqueID(), SceneNode->GetDisplayLabel(), EInterchangeNodeContainerType::FactoryData);
	const FString ActorFactoryNodeUid = FactoryNodeContainer->AddNode(ActorFactoryNode);
	if (!SceneNode->GetParentUid().IsEmpty())
	{
		const FString ParentFactoryNodeUid = TEXT("Factory_") + SceneNode->GetParentUid();
		FactoryNodeContainer->SetNodeParentUid(ActorFactoryNodeUid, ParentFactoryNodeUid);
		ActorFactoryNode->AddFactoryDependencyUid(ParentFactoryNodeUid);
	}

	ActorFactoryNode->AddTargetNodeUid(SceneNode->GetUniqueID());
	SceneNode->AddTargetNodeUid(ActorFactoryNode->GetUniqueID());

	//TODO move this code to the factory, a stack over pipeline can change the global offset transform which will affect this value.
	FTransform GlobalTransform;
	if (SceneNode->GetCustomGlobalTransform(InBaseNodeContainer, GlobalOffsetTransform, GlobalTransform))
	{
		ActorFactoryNode->SetCustomGlobalTransform(GlobalTransform);
	}
	
	ActorFactoryNode->SetCustomMobility(EComponentMobility::Static);

	if (TranslatedAssetNode)
	{
		SetUpFactoryNode(ActorFactoryNode, SceneNode, TranslatedAssetNode, FactoryNodeContainer);
	}
}

UInterchangeActorFactoryNode* UInterchangeGenericLevelPipeline::CreateActorFactoryNode(const UInterchangeSceneNode* SceneNode, const UInterchangeBaseNode* TranslatedAssetNode, UInterchangeBaseNodeContainer* FactoryNodeContainer) const
{
	if (TranslatedAssetNode && TranslatedAssetNode->IsA<UInterchangeCameraNode>())
	{
		return NewObject<UInterchangeCineCameraFactoryNode>(FactoryNodeContainer, NAME_None);
	}
	else if (TranslatedAssetNode && TranslatedAssetNode->IsA<UInterchangeMeshNode>())
	{
		return NewObject<UInterchangeMeshActorFactoryNode>(FactoryNodeContainer, NAME_None);
	}
	else
	{
		return NewObject<UInterchangeActorFactoryNode>(FactoryNodeContainer, NAME_None);
	}
}

void UInterchangeGenericLevelPipeline::SetUpFactoryNode(UInterchangeActorFactoryNode* ActorFactoryNode, const UInterchangeSceneNode* SceneNode, const UInterchangeBaseNode* TranslatedAssetNode, UInterchangeBaseNodeContainer* FactoryNodeContainer) const
{
	if (!ensure(ActorFactoryNode && SceneNode && TranslatedAssetNode && FactoryNodeContainer))
	{
		return;
	}

	if (const UInterchangeMeshNode* MeshNode = Cast<UInterchangeMeshNode>(TranslatedAssetNode))
	{
		if (MeshNode->IsSkinnedMesh())
		{
			ActorFactoryNode->SetCustomActorClassName(ASkeletalMeshActor::StaticClass()->GetPathName());
			ActorFactoryNode->SetCustomMobility(EComponentMobility::Movable);
		}
		else
		{
			ActorFactoryNode->SetCustomActorClassName(AStaticMeshActor::StaticClass()->GetPathName());
		}

		if (UInterchangeMeshActorFactoryNode* MeshActorFactoryNode = Cast<UInterchangeMeshActorFactoryNode>(ActorFactoryNode))
		{
			TMap<FString, FString> SlotMaterialDependencies;
			SceneNode->GetSlotMaterialDependencies(SlotMaterialDependencies);

			UE::Interchange::MeshesUtilities::ApplySlotMaterialDependencies(*MeshActorFactoryNode, SlotMaterialDependencies, *FactoryNodeContainer);
		}
	}
	else if (const UInterchangeLightNode* LightNode = Cast<UInterchangeLightNode>(TranslatedAssetNode))
	{
		//Test for spot before point since a spot light is a point light
		if (LightNode->IsA<UInterchangeSpotLightNode>())
		{
			ActorFactoryNode->SetCustomActorClassName(ASpotLight::StaticClass()->GetPathName());
		}
		else if (LightNode->IsA<UInterchangePointLightNode>())
		{
			ActorFactoryNode->SetCustomActorClassName(APointLight::StaticClass()->GetPathName());
		}
		else if (LightNode->IsA<UInterchangeRectLightNode>())
		{
			ActorFactoryNode->SetCustomActorClassName(ARectLight::StaticClass()->GetPathName());
		}
		else if (LightNode->IsA<UInterchangeDirectionalLightNode>())
		{
			ActorFactoryNode->SetCustomActorClassName(ADirectionalLight::StaticClass()->GetPathName());
		}
		else
		{
			ActorFactoryNode->SetCustomActorClassName(APointLight::StaticClass()->GetPathName());
		}
	}
	else if (const UInterchangeCameraNode* CameraNode = Cast<UInterchangeCameraNode>(TranslatedAssetNode))
	{
		ActorFactoryNode->SetCustomActorClassName(ACineCameraActor::StaticClass()->GetPathName());
		ActorFactoryNode->SetCustomMobility(EComponentMobility::Movable);

		if (UInterchangeCineCameraFactoryNode* CineCameraFactoryNode = Cast<UInterchangeCineCameraFactoryNode>(ActorFactoryNode))
		{
			float FocalLength;
			if (CameraNode->GetCustomFocalLength(FocalLength))
			{
				CineCameraFactoryNode->SetCustomFocalLength(FocalLength);
			}

			float SensorHeight;
			if (CameraNode->GetCustomSensorHeight(SensorHeight))
			{
				CineCameraFactoryNode->SetCustomSensorHeight(SensorHeight);
			}

			float SensorWidth;
			if (CameraNode->GetCustomSensorWidth(SensorWidth))
			{
				CineCameraFactoryNode->SetCustomSensorWidth(SensorWidth);
			}
		}
	}
}
