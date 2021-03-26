// Copyright Epic Games, Inc. All Rights Reserved.

#include "DatasmithSketchUpExportContext.h"
#include "DatasmithSketchUpUtils.h"
#include "DatasmithSketchUpComponent.h"
#include "DatasmithSketchUpCamera.h"
#include "DatasmithSketchUpMaterial.h"
#include "DatasmithSketchUpMaterial.h"
#include "DatasmithSketchUpMesh.h"
#include "DatasmithSketchUpMetadata.h"
#include "DatasmithSketchUpString.h"
#include "DatasmithSketchUpSummary.h"

#include "DatasmithExportOptions.h"
#include "DatasmithMesh.h"
#include "DatasmithMeshExporter.h"
#include "DatasmithSceneExporter.h"
#include "DatasmithSceneFactory.h"
#include "DatasmithUtils.h"
#include "IDatasmithSceneElements.h"

#include "Misc/SecureHash.h"
#include "Misc/Paths.h"


// SketchUp SDK.
#include "DatasmithSketchUpSDKBegins.h"

#include "SketchUpAPI/model/camera.h"
#include "SketchUpAPI/model/component_definition.h"
#include "SketchUpAPI/model/component_instance.h"
#include "SketchUpAPI/model/drawing_element.h"
#include "SketchUpAPI/model/edge.h"
#include "SketchUpAPI/model/entities.h"
#include "SketchUpAPI/model/entity.h"
#include "SketchUpAPI/model/face.h"
#include "SketchUpAPI/model/geometry.h"
#include "SketchUpAPI/model/group.h"
#include "SketchUpAPI/model/layer.h"
#include "SketchUpAPI/model/mesh_helper.h"
#include "SketchUpAPI/model/model.h"
#include "SketchUpAPI/model/scene.h"
#include "SketchUpAPI/model/texture.h"
#include "SketchUpAPI/model/uv_helper.h"

#include "SketchUpAPI/application/application.h"

#include "DatasmithSketchUpSDKCeases.h"

using namespace DatasmithSketchUp;


FExportContext::FExportContext()
	: ComponentDefinitions(*this)
	, ComponentInstances(*this)
	, EntitiesObjects(*this)
	, Materials(*this)
	, Scenes(*this)
	, Textures(*this)

{

}

const TCHAR* FExportContext::GetAssetsOutputPath() const
{
	return SceneExporter->GetAssetsOutputPath();
}

void FExportContext::Populate()
{
	// Get Active Model
	SUResult su_api_result = SUApplicationGetActiveModel(&ModelRef);
	if (SUIsInvalid(ModelRef)) {
		return;
	}

	SUTransformation WorldTransform = { 1.0, 0.0, 0.0, 0.0,
										0.0, 1.0, 0.0, 0.0,
										0.0, 0.0, 1.0, 0.0,
										0.0, 0.0, 0.0, 1.0 };

	// Set up root 'Node'
	ModelDefinition = MakeShared<DatasmithSketchUp::FModelDefinition>(ModelRef);
	ModelDefinition->Parse(*this);

	// Retrieve the default layer in the SketchUp model.
	SULayerRef DefaultLayerRef = SU_INVALID;
	SUModelGetDefaultLayer(ModelRef, &DefaultLayerRef);

	// Setup root Node, based on Model
	Model = MakeShared<FModel>(*ModelDefinition);
	RootNode = MakeShared<FNodeOccurence>(*Model);
	RootNode->WorldTransform = WorldTransform;
	RootNode->EffectiveLayerRef = DefaultLayerRef;
	// Name and label for root loose mesh actors
	RootNode->DatasmithActorName = TEXT("SU");
	RootNode->DatasmithActorLabel = TEXT("Model");

	// Parse/convert Model
	Materials.PopulateFromModel(ModelRef);
	Scenes.PopulateFromModel(ModelRef);
	ComponentDefinitions.PopulateFromModel(ModelRef);

	// Add the model metadata into the dictionary of metadata definitions.
	FDatasmithSketchUpMetadata::AddMetadataDefinition(ModelRef);

	RootNode->ToDatasmith(*this);

	Textures.ConvertToDatasmith();
}

void FExportContext::Update()
{
	// Invalidate occurrences for changed instances first
	Model->UpdateEntityProperties(*this);
	ComponentInstances.UpdateProperties(); 

	// Update occurrences visibility(before updating meshes to make sure to skip updating unused meshes)
	RootNode->UpdateVisibility(*this);

	// Update Datasmith Meshes after their usage was refreshed(in visibility update) and before node hierarchy update(where Mesh Actors are updated for meshes)
	ModelDefinition->UpdateDefinition(*this);
	ComponentDefinitions.Update();

	// ComponentInstances will invalidate occurrences 
	Model->UpdateEntityGeometry(*this);
	ComponentInstances.UpdateGeometry();

	// Update transforms/names for Datasmith Actors and MeshActors, create these actors if needed
	RootNode->Update(*this);
}

FDefinition* FExportContext::GetEntityDefinition(SUEntityRef Entity)
{
	// No Entity means Model
	if (SUIsInvalid(Entity))
	{
		return ModelDefinition.Get();
	}
	else
	{
		return ComponentDefinitions.GetComponentDefinition(SUComponentDefinitionFromEntity(Entity)).Get();
	}
}

void FComponentDefinitionCollection::Update()
{
	for (const auto& IdValue : ComponentDefinitionMap)
	{
		TSharedPtr<FComponentDefinition> Definition = IdValue.Value;
		Definition->UpdateDefinition(Context);
	}
}

void FSceneCollection::PopulateFromModel(SUModelRef InModelRef)
{
	// Get the number of scenes in the SketchUp model.
	size_t SceneCount = 0;
	SUModelGetNumScenes(InModelRef, &SceneCount); // we can ignore the returned SU_RESULT

	if (SceneCount > 0)
	{
		// Retrieve the scenes in the SketchUp model.
		TArray<SUSceneRef> Scenes;
		Scenes.Init(SU_INVALID, SceneCount);
		SUResult SResult = SUModelGetScenes(InModelRef, SceneCount, Scenes.GetData(), &SceneCount);
		Scenes.SetNum(SceneCount);
		// Make sure the SketchUp model has scenes to retrieve (no SU_ERROR_NO_DATA).
		if (SResult == SU_ERROR_NONE)
		{
			for (SUSceneRef SceneRef : Scenes)
			{
				// Make sure the SketchUp scene uses a camera.
				bool bSceneUseCamera = false;
				SUSceneGetUseCamera(SceneRef, &bSceneUseCamera); // we can ignore the returned SU_RESULT

				if (bSceneUseCamera)
				{
					TSharedPtr<DatasmithSketchUp::FCamera> Camera = FCamera::Create(Context, SceneRef);
					SceneIdToCameraMap.Add(DatasmithSketchUpUtils::GetSceneID(SceneRef), Camera);
				}
			}
		}
	}
}


void FComponentDefinitionCollection::PopulateFromModel(SUModelRef InModelRef)
{
	// Get the number of normal component definitions in the SketchUp model.
	size_t SComponentDefinitionCount = 0;
	SUModelGetNumComponentDefinitions(InModelRef, &SComponentDefinitionCount); // we can ignore the returned SU_RESULT

	if (SComponentDefinitionCount > 0)
	{
		// Retrieve the normal component definitions in the SketchUp model.
		TArray<SUComponentDefinitionRef> SComponentDefinitions;
		SComponentDefinitions.Init(SU_INVALID, SComponentDefinitionCount);
		SUModelGetComponentDefinitions(InModelRef, SComponentDefinitionCount, SComponentDefinitions.GetData(), &SComponentDefinitionCount); // we can ignore the returned SU_RESULT
		SComponentDefinitions.SetNum(SComponentDefinitionCount);

		// Add the normal component definitions to our dictionary.
		for (SUComponentDefinitionRef SComponentDefinitionRef : SComponentDefinitions)
		{
			AddComponentDefinition(SComponentDefinitionRef);

			// Add the normal component definition metadata into the dictionary of metadata definitions.
			FDatasmithSketchUpMetadata::AddMetadataDefinition(SComponentDefinitionRef);
		}
	}

	// Get the number of group component definitions in the SketchUp model.
	size_t SGroupDefinitionCount = 0;
	SUModelGetNumGroupDefinitions(InModelRef, &SGroupDefinitionCount); // we can ignore the returned SU_RESULT

	if (SGroupDefinitionCount > 0)
	{
		// Retrieve the group component definitions in the SketchUp model.
		TArray<SUComponentDefinitionRef> SGroupDefinitions;
		SGroupDefinitions.Init(SU_INVALID, SGroupDefinitionCount);
		SUModelGetGroupDefinitions(InModelRef, SGroupDefinitionCount, SGroupDefinitions.GetData(), &SGroupDefinitionCount); // we can ignore the returned SU_RESULT
		SGroupDefinitions.SetNum(SGroupDefinitionCount);

		// Add the group component definitions to our dictionary.
		for (SUComponentDefinitionRef SGroupDefinitionRef : SGroupDefinitions)
		{
			AddComponentDefinition(SGroupDefinitionRef);
		}
	}
}

TSharedPtr<FComponentDefinition> FComponentDefinitionCollection::AddComponentDefinition(SUComponentDefinitionRef InComponentDefinitionRef)
{
	TSharedPtr<FComponentDefinition> Definition = MakeShared<FComponentDefinition>(InComponentDefinitionRef);
	Definition->Parse(Context);
	ComponentDefinitionMap.Add(Definition->SketchupSourceID, Definition);
	return Definition;
}

TSharedPtr<FComponentDefinition> FComponentDefinitionCollection::GetComponentDefinition(
	SUComponentInstanceRef InComponentInstanceRef
)
{
	// Retrieve the component definition of the SketchUp component instance.
	SUComponentDefinitionRef SComponentDefinitionRef = SU_INVALID;
	SUComponentInstanceGetDefinition(InComponentInstanceRef, &SComponentDefinitionRef); // we can ignore the returned SU_RESULT
	return GetComponentDefinition(SComponentDefinitionRef);
}

TSharedPtr<FComponentDefinition> FComponentDefinitionCollection::GetComponentDefinition(SUComponentDefinitionRef ComponentDefinitionRef)
{
	FComponentDefinitionIDType ComponentDefinitionID = DatasmithSketchUpUtils::GetComponentID(ComponentDefinitionRef);
	// Make sure the SketchUp component definition exists in our dictionary of component definitions.
	if (TSharedPtr<FComponentDefinition>* Ptr = ComponentDefinitionMap.Find(ComponentDefinitionID))
	{
		return *Ptr;
	}

	return AddComponentDefinition(ComponentDefinitionRef);
}

void FEntitiesObjectCollection::RegisterEntitiesFaces(DatasmithSketchUp::FEntities& Entities, const TSet<int32>& FaceIds)
{
	for (int32 FaceId : FaceIds)
	{
		FaceIdForEntitiesMap.Add(FaceId, &Entities);
	}
}

TSharedPtr<FEntities> FEntitiesObjectCollection::AddEntities(FDefinition& InDefinition, SUEntitiesRef EntitiesRef)
{
	TSharedPtr<FEntities> Entities = MakeShared<FEntities>(InDefinition);

	Entities->EntitiesRef = EntitiesRef;
	return Entities;
}

DatasmithSketchUp::FEntities* FEntitiesObjectCollection::FindFace(int32 FaceId)
{
	if (DatasmithSketchUp::FEntities** PtrPtr = FaceIdForEntitiesMap.Find(FaceId))
	{
		return *PtrPtr;
	}
	return nullptr;
}


TSharedPtr<FComponentInstance> FComponentInstanceCollection::AddComponentInstance(SUComponentInstanceRef InComponentInstanceRef)
{
	FComponentInstanceIDType ComponentInstanceId = DatasmithSketchUpUtils::GetComponentInstanceID(InComponentInstanceRef);

	if (TSharedPtr<FComponentInstance>* Ptr = ComponentInstanceMap.Find(ComponentInstanceId))
	{
		return *Ptr;
	}

	TSharedPtr<FComponentDefinition> Definition = Context.ComponentDefinitions.GetComponentDefinition(InComponentInstanceRef);

	TSharedPtr<FComponentInstance> ComponentInstance = MakeShared<FComponentInstance>(SUComponentInstanceToEntity(InComponentInstanceRef), *Definition);

	Definition->LinkComponentInstance(ComponentInstance.Get());

	ComponentInstanceMap.Add(ComponentInstanceId, ComponentInstance);
	return ComponentInstance;
}

bool FComponentInstanceCollection::RemoveComponentInstance(FComponentInstanceIDType ComponentInstanceId)
{
	const TSharedPtr<FComponentInstance>* ComponentInstancePtr = ComponentInstanceMap.Find(ComponentInstanceId);
	if (!ComponentInstancePtr)
	{
		return false;
	}
	const TSharedPtr<FComponentInstance>& ComponentInstance = *ComponentInstancePtr;

	ComponentInstance->RemoveComponentInstance(Context);

	ComponentInstanceMap.Remove(ComponentInstanceId);

	return true;
}

void FComponentInstanceCollection::InvalidateComponentInstanceGeometry(FComponentInstanceIDType ComponentInstanceID)
{
	if (TSharedPtr<FComponentInstance>* Ptr = FindComponentInstance(ComponentInstanceID))
	{
		(*Ptr)->InvalidateEntityGeometry();
	}
	else
	{
		// todo: implement. This could happen if
		//  - component instance was previously skipped because it doesn't contain meaningful data(probably it's not needed to process it it's still empty)
		//  - was removed recently. Not sure if 'changed' can code after 'removed'
		//  - addition wasn't handled
		// - anything else?
	}
}

void FComponentInstanceCollection::InvalidateComponentInstanceProperties(FComponentInstanceIDType ComponentInstanceID)
{
	if (TSharedPtr<FComponentInstance>* Ptr = FindComponentInstance(ComponentInstanceID))
	{
		(*Ptr)->InvalidateEntityProperties();
	}
	else
	{
		// todo: implement. This could happen if
		//  - component instance was previously skipped because it doesn't contain meaningful data(probably it's not needed to process it it's still empty)
		//  - was removed recently. Not sure if 'changed' can code after 'removed'
		//  - addition wasn't handled
		// - anything else?
	}
}

void FComponentInstanceCollection::UpdateProperties()
{
	for (const auto& KeyValue : ComponentInstanceMap)
	{
		TSharedPtr<FComponentInstance> ComponentInstance = KeyValue.Value;
		ComponentInstance->UpdateEntityProperties(Context);
	}
}

void FComponentInstanceCollection::UpdateGeometry()
{
	for (const auto& KeyValue : ComponentInstanceMap)
	{
		TSharedPtr<FComponentInstance> ComponentInstance = KeyValue.Value;
		ComponentInstance->UpdateEntityGeometry(Context);
	}
}

void FMaterialCollection::PopulateFromModel(SUModelRef InModelRef)
{
	DefaultMaterial = FMaterial::CreateDefaultMaterial(Context);

	// Get the number of material definitions in the SketchUp model.
	size_t SMaterialDefinitionCount = 0;
	SUModelGetNumMaterials(InModelRef, &SMaterialDefinitionCount); // we can ignore the returned SU_RESULT

	if (SMaterialDefinitionCount > 0)
	{
		// Retrieve the material definitions in the SketchUp model.
		TArray<SUMaterialRef> SMaterialDefinitions;
		SMaterialDefinitions.Init(SU_INVALID, SMaterialDefinitionCount);
		SUModelGetMaterials(InModelRef, SMaterialDefinitionCount, SMaterialDefinitions.GetData(), &SMaterialDefinitionCount); // we can ignore the returned SU_RESULT
		SMaterialDefinitions.SetNum(SMaterialDefinitionCount);

		// Add the material definitions to our dictionary.
		for (SUMaterialRef SMaterialDefinitionRef : SMaterialDefinitions)
		{

			CreateMaterial(SMaterialDefinitionRef);
		}
	}
}

FMaterialOccurrence* FMaterialCollection::RegisterInstance(FMaterialIDType MaterialID, FNodeOccurence* NodeOccurrence)
{
	if (const TSharedPtr<DatasmithSketchUp::FMaterial>* Ptr = Find(MaterialID))
	{
		const TSharedPtr<DatasmithSketchUp::FMaterial>& Material = *Ptr;
		return &Material->RegisterInstance(NodeOccurrence);
	}
	return DefaultMaterial.Get();
}

FMaterialOccurrence* FMaterialCollection::RegisterGeometry(FMaterialIDType MaterialID, DatasmithSketchUp::FEntitiesGeometry* EntitiesGeometry)
{
	if (const TSharedPtr<DatasmithSketchUp::FMaterial>* Ptr = Find(MaterialID))
	{
		EntitiesGeometry->MaterialsUsed.Add(MaterialID);

		const TSharedPtr<DatasmithSketchUp::FMaterial>& Material = *Ptr;
		return &Material->RegisterGeometry(EntitiesGeometry);
	}
	return DefaultMaterial.Get();
}

void FMaterialCollection::UnregisterGeometry(DatasmithSketchUp::FEntitiesGeometry* EntitiesGeometry)
{
	TSet<FMaterialIDType>& MaterialsUsed = EntitiesGeometry->MaterialsUsed;

	for (FMaterialIDType MaterialID : MaterialsUsed)
	{
		if (const TSharedPtr<DatasmithSketchUp::FMaterial>* Ptr = Find(MaterialID))
		{
			const TSharedPtr<DatasmithSketchUp::FMaterial>& Material = *Ptr;
			Material->UnregisterGeometry(EntitiesGeometry);
		}
	}

	EntitiesGeometry->MaterialsUsed.Reset();
}

TSharedPtr<FMaterial> FMaterialCollection::CreateMaterial(SUMaterialRef MaterialDefinitionRef)
{
	TSharedPtr<FMaterial> Material = FMaterial::Create(Context, MaterialDefinitionRef);
	MaterialDefinitionMap.Emplace(DatasmithSketchUpUtils::GetMaterialID(MaterialDefinitionRef), Material);
	return Material;
}

void FMaterialCollection::InvalidateMaterial(SUMaterialRef MaterialDefinitionRef)
{
	FMaterialIDType MateriadId = DatasmithSketchUpUtils::GetMaterialID(MaterialDefinitionRef);

	if (TSharedPtr<FMaterial>* Ptr = MaterialDefinitionMap.Find(MateriadId))
	{
		FMaterial& Material = **Ptr;

		Material.Update(Context);
		return;
	}
	CreateMaterial(MaterialDefinitionRef);
}

bool FMaterialCollection::RemoveMaterial(FEntityIDType EntityId)
{
	TSharedPtr<FMaterial> Material;
	if (MaterialDefinitionMap.RemoveAndCopyValue(EntityId, Material))
	{
		Material->Remove(Context);
		return true;
	}
	return false;
}
