// Copyright Epic Games, Inc. All Rights Reserved.

#include "SyncDatabase.h"

#include "MaterialsDatabase.h"
#include "TexturesCache.h"
#include "ElementID.h"
#include "ElementTools.h"
#include "GeometryUtil.h"

#include "DatasmithUtils.h"

#include "ModelMeshBody.hpp"
#include "Light.hpp"
#include "AttributeIndex.hpp"

BEGIN_NAMESPACE_UE_AC

#if defined(DEBUG) && 0
	#define UE_AC_DO_TRACE 1
#else
	#define UE_AC_DO_TRACE 0
#endif

// Constructor
FSyncDatabase::FSyncDatabase(const TCHAR* InSceneName, const TCHAR* InSceneLabel, const TCHAR* InAssetsPath)
	: Scene(FDatasmithSceneFactory::CreateScene(*FDatasmithUtils::SanitizeObjectName(InSceneName)))
	, AssetsFolderPath(InAssetsPath)
	, MaterialsDatabase(new FMaterialsDatabase())
	, TexturesCache(new FTexturesCache())
{
	Scene->SetLabel(InSceneLabel);
}

// Destructor
FSyncDatabase::~FSyncDatabase()
{
	// Delete all sync data content by simulatin an emptying a 3d model
	ResetBeforeScan();
	CleanAfterScan();
	size_t RemainingCount = ElementsSyncDataMap.size();
	if (RemainingCount != 0)
	{
		UE_AC_DebugF("FSyncDatabase::~FSyncDatabase - Database not emptied - %lu Remaining\n", RemainingCount);
		for (FMapGuid2SyncData::iterator Iter = ElementsSyncDataMap.begin(); Iter != ElementsSyncDataMap.end(); ++Iter)
		{
			delete Iter->second;
			Iter->second = nullptr;
		}
	}

	delete MaterialsDatabase;
	MaterialsDatabase = nullptr;
	delete TexturesCache;
	TexturesCache = nullptr;
}

// Return the asset file path
const TCHAR* FSyncDatabase::GetAssetsFolderPath() const
{
	return *AssetsFolderPath;
}

// Scan all elements, to determine if they need to be synchronized
void FSyncDatabase::Synchronize(const FSyncContext& InSyncContext)
{
	ResetBeforeScan();

	UInt32 ModifiedCount = ScanElements(InSyncContext);

	InSyncContext.NewPhase(kCommonSetUpLights, 0);

	// Cameras from all cameras set
	InSyncContext.NewPhase(kCommonSetUpCameras, 0);
	ScanCameras(InSyncContext);

	// Cameras from the current view
	FSyncData*& CameraSyncData = GetSyncData(FSyncData::FCamera::CurrentViewGUID);
	if (CameraSyncData == nullptr)
	{
		CameraSyncData = new FSyncData::FCamera(FSyncData::FCamera::CurrentViewGUID, 0);
		CameraSyncData->SetParent(&GetSceneSyncData());
		CameraSyncData->MarkAsExisting();
		CameraSyncData->MarkAsModified();
	}

	CleanAfterScan();

	InSyncContext.NewPhase(kCommonConvertElements, ModifiedCount);
	FSyncData::FProcessInfo ProcessInfo(InSyncContext);
	GetSceneSyncData().ProcessTree(&ProcessInfo);
}

// Before a scan we reset our sync data, so we can detect when an element has been modified or destroyed
void FSyncDatabase::ResetBeforeScan()
{
	FMapGuid2SyncData::iterator MapEnd = ElementsSyncDataMap.end();
	for (FMapGuid2SyncData::iterator SyncData = ElementsSyncDataMap.begin(); SyncData != MapEnd; ++SyncData)
	{
		SyncData->second->ResetBeforeScan();
	}
}

// After a scan, but before syncing, we delete obsolete syncdata (and it's Datasmith Element)
void FSyncDatabase::CleanAfterScan()
{
	FMapGuid2SyncData::iterator ItSyncData = ElementsSyncDataMap.find(FSyncData::FScene::SceneGUID);
	if (ItSyncData != ElementsSyncDataMap.end())
	{
		ItSyncData->second->CleanAfterScan(this);
	}
}

// Get existing sync data for the specified guid
FSyncData*& FSyncDatabase::GetSyncData(const GS::Guid& InGuid)
{
	std::pair< GS::Guid, FSyncData* >			   value(InGuid, nullptr);
	std::pair< FMapGuid2SyncData::iterator, bool > result = ElementsSyncDataMap.insert(value);
	return result.first->second;
}

FSyncData& FSyncDatabase::GetSceneSyncData()
{
	FSyncData*& SceneSyncData = GetSyncData(FSyncData::FScene::SceneGUID);
	if (SceneSyncData == nullptr)
	{
		SceneSyncData = new FSyncData::FScene();
	}
	return *SceneSyncData;
}

FSyncData& FSyncDatabase::GetLayerSyncData(short InLayer)
{
	FSyncData*& Layer = GetSyncData(FSyncData::FLayer::GetLayerGUID(InLayer));
	if (Layer == nullptr)
	{
		Layer = new FSyncData::FLayer(FSyncData::FLayer::GetLayerGUID(InLayer));
		Layer->SetParent(&GetSceneSyncData());
	}
	return *Layer;
}

// Delete obsolete syncdata (and it's Datasmith Element)
void FSyncDatabase::DeleteSyncData(const GS::Guid& InGuid)
{
	FMapGuid2SyncData::iterator ItSyncData = ElementsSyncDataMap.find(InGuid);
	if (ItSyncData != ElementsSyncDataMap.end())
	{
		ElementsSyncDataMap.erase(ItSyncData);
	}
	else
	{
		UE_AC_DebugF("FSyncDatabase::Delete {%s}\n", InGuid.ToUniString().ToUtf8());
	}
}

// Return the name of the specified layer
const FString& FSyncDatabase::GetLayerName(short InLayerIndex)
{
	FMapLayerIndex2Name::iterator found = LayerIndex2Name.find(InLayerIndex);
	if (found == LayerIndex2Name.end())
	{
		LayerIndex2Name[InLayerIndex] = GSStringToUE(UE_AC::GetLayerName(InLayerIndex));
		found = LayerIndex2Name.find(InLayerIndex);
	}
	return found->second;
}

// Set the mesh in the handle and take care of mesh life cycle.
bool FSyncDatabase::SetMesh(TSharedPtr< IDatasmithMeshElement >*	   Handle,
							const TSharedPtr< IDatasmithMeshElement >& InMesh)
{
	if (Handle->IsValid())
	{
		if (InMesh.IsValid() && FCString::Strcmp(Handle->Get()->GetName(), InMesh->GetName()) == 0)
		{
			return false; // No change : Same name (hash) --> Same mesh
		}

		FMapHashToMeshInfo::iterator Older = HashToMeshInfo.find(Handle->Get()->GetName());
		UE_AC_Assert(Older != HashToMeshInfo.end());
		if (--Older->second.Count == 0)
		{
			Scene->RemoveMesh(Older->second.Mesh);
			HashToMeshInfo.erase(Older);
		}
		Handle->Reset();
	}
	else
	{
		if (!InMesh.IsValid())
		{
			return false; // No change : No mesh before and no mesh after
		}
	}

	if (InMesh.IsValid())
	{
		FMeshInfo& MeshInfo = HashToMeshInfo[InMesh->GetName()];
		if (!MeshInfo.Mesh.IsValid())
		{
			MeshInfo.Mesh = InMesh;
			Scene->AddMesh(InMesh);
		}
		++MeshInfo.Count;
		*Handle = InMesh;
	}

	return true;
}

// SetSceneInfo
void FSyncDatabase::SetSceneInfo()
{
	IDatasmithScene& TheScene = *Scene;

	// Set up basics scene informations
	TheScene.SetHost(TEXT("ARCHICAD"));
	TheScene.SetVendor(TEXT("Graphisoft"));
	TheScene.SetProductName(TEXT("ARCHICAD"));
	TheScene.SetProductVersion(UTF8_TO_TCHAR(UE_AC_STRINGIZE(AC_VERSION)));
}

// Scan all elements, to determine if they need to be synchronized
UInt32 FSyncDatabase::ScanElements(const FSyncContext& InSyncContext)
{
	// We create this objects here to not construct/destroy at each iteration
	FElementID ElementID(InSyncContext);

	// Loop on all 3D elements
	UInt32	  ModifiedCount = 0;
	GS::Int32 NbElements = InSyncContext.GetModel().GetElementCount();
	UE_AC_STAT(InSyncContext.Stats.TotalElements = NbElements);
	InSyncContext.NewPhase(kCommonCollectElements, NbElements);
	for (GS::Int32 IndexElement = 1; IndexElement <= NbElements; IndexElement++)
	{
		InSyncContext.NewCurrentValue(IndexElement);

		// Get next valid 3d element
		ElementID.InitElement(IndexElement);
		if (ElementID.IsInvalid())
		{
#if UE_AC_DO_TRACE && 1
			UE_AC_TraceF("FSynchronizer::ScanElements - Element Index=%d Is invalid\n", IndexElement);
#endif
			continue;
		}

		API_Guid ElementGuid = GSGuid2APIGuid(ElementID.Element3D.GetElemGuid());
		if (ElementGuid == APINULLGuid)
		{
#if UE_AC_DO_TRACE && 1
			UE_AC_TraceF("FSynchronizer::ScanElements - Element Index=%d hasn't id\n", IndexElement);
#endif
			continue;
		}

#if UE_AC_DO_TRACE && 1
		// Get the name of the element (To help debugging)
		GS::UniString ElemenInfo;
		FElementTools::GetInfoString(ElementGuid, &ElemenInfo);

	#if 0
		// Print element info in debugger view
		FElement2String::DumpInfo(ElementGuid);
	#endif
#endif

		// Check 3D geometry bounding box
		Box3D box = ElementID.Element3D.GetBounds(ModelerAPI::CoordinateSystem::ElemLocal);

		// Bonding box is empty, must not happen, but it happen
		if (box.xMin > box.xMax || box.yMin > box.yMax || box.zMin > box.zMax)
		{
#if UE_AC_DO_TRACE && 1
			UE_AC_TraceF("FSynchronizer::ScanElements - EmptyBox for %s \"%s\" %d %s", ElementID.GetTypeName(),
						 ElemenInfo.ToUtf8(), IndexElement, APIGuidToString(ElementGuid).ToUtf8());
#endif
			continue; // Object is invisible (hidden layer or cutting plane)
		}

		// Get the header (modification time, layer, floor, element type...)
		if (!ElementID.InitHeader())
		{
#if UE_AC_DO_TRACE && 1
			UE_AC_DebugF("FSynchronizer::ScanElements - Can't get header for %d %s", IndexElement,
						 APIGuidToString(ElementGuid).ToUtf8());
#endif
			continue;
		}

		UE_AC_STAT(InSyncContext.Stats.TotalElementsWithGeometry++);

		// Get sync data for this element (Create or reuse already existing)
		FSyncData*& SyncData = GetSyncData(APIGuid2GSGuid(ElementID.ElementHeader.guid));
		if (SyncData == nullptr)
		{
			SyncData = new FSyncData::FElement(APIGuid2GSGuid(ElementID.ElementHeader.guid));
		}
		ElementID.SyncData = SyncData;
		SyncData->Update(ElementID);
		if (SyncData->IsModified())
		{
			++ModifiedCount;
		}

		// Add lights
		if (ElementID.Element3D.GetLightCount() > 0)
		{
			ScanLights(ElementID);
		}
	}

	UE_AC_STAT(InSyncContext.Stats.TotalElementsModified = ModifiedCount);

	InSyncContext.NewCurrentValue(NbElements);

	return ModifiedCount;
}

// Scan all lights of this element
void FSyncDatabase::ScanLights(const FElementID& InElementID)
{
	ModelerAPI::Light Light;
	//	API_Component3D	API_Light;

	GS::Int32 LightsCount = InElementID.Element3D.GetLightCount();
	for (GS::Int32 LightIndex = 1; LightIndex < LightsCount; ++LightIndex)
	{
		InElementID.Element3D.GetLight(LightIndex, &Light);
		ModelerAPI::Light::Type LightType = Light.GetType();
		if (LightType == ModelerAPI::Light::DirectionLight || LightType == ModelerAPI::Light::SpotLight ||
			LightType == ModelerAPI::Light::PointLight)
		{
			API_Guid	LightId = CombineGuid(InElementID.ElementHeader.guid, GuidFromMD5(LightIndex));
			FSyncData*& SyncData = FSyncDatabase::GetSyncData(APIGuid2GSGuid(LightId));
			if (SyncData == nullptr)
			{
				SyncData = new FSyncData::FLight(APIGuid2GSGuid(LightId), LightIndex);
				SyncData->SetParent(InElementID.SyncData);
				SyncData->MarkAsExisting();
				SyncData->MarkAsModified();
			}
			FSyncData::FLight& LightSyncData = static_cast< FSyncData::FLight& >(*SyncData);

			const float		  InnerConeAngle = float(Light.GetFalloffAngle1() * 180.0f / PI);
			const float		  OuterConeAngle = float(Light.GetFalloffAngle2() * 180.0f / PI);
			ModelerAPI::Color color = Light.GetColor();
			FLinearColor	  LinearColor(float(color.red), float(color.green), float(color.blue));
			LightSyncData.SetValues(LightType, InnerConeAngle, OuterConeAngle, LinearColor);
			LightSyncData.Placement(FGeometryUtil::GetTranslationVector(Light.GetPosition()),
									FGeometryUtil::GetRotationQuat(Light.GetDirection()));
		}
	}
}

// Scan all cameras
void FSyncDatabase::ScanCameras(const FSyncContext& /* InSyncContext */)
{
	GS::Array< API_Guid > ElemList;
	GSErrCode			  GSErr = ACAPI_Element_GetElemList(API_CamSetID, &ElemList);
	if (GSErr != NoError)
	{
		UE_AC_DebugF("FSyncDatabase::ScanCameras - ACAPI_Element_GetElemList return %d", GSErr);
		return;
	}
	Int32	 IndexCamera = 0;
	API_Guid NextCamera = APINULLGuid;

	for (API_Guid ElemGuid : ElemList)
	{
		// Get info on this element
		API_Element cameraSet;
		Zap(&cameraSet);
		cameraSet.header.guid = ElemGuid;
		GSErr = ACAPI_Element_Get(&cameraSet);
		if (GSErr != NoError)
		{
			if (GSErr != APIERR_DELETED)
			{
				UE_AC_DebugF("FSyncDatabase::ScanCameras - ACAPI_Element_Get return %d", GSErr);
			}
			continue;
		}
		if (cameraSet.camset.firstCam == APINULLGuid)
		{
			continue;
		}

		FSyncData*& CameraSetSyncData = FSyncDatabase::GetSyncData(APIGuid2GSGuid(cameraSet.header.guid));
		if (CameraSetSyncData == nullptr)
		{
			CameraSetSyncData = new FSyncData::FCameraSet(APIGuid2GSGuid(cameraSet.header.guid), cameraSet.camset.name,
														  cameraSet.camset.perspPars.openedPath);
			CameraSetSyncData->SetParent(&GetSceneSyncData());
		}

		IndexCamera = 0;
		NextCamera = cameraSet.camset.firstCam;
		while (GSErr == NoError && NextCamera != APINULLGuid)
		{
			API_Element camera;
			Zap(&camera);
			camera.header.guid = NextCamera;
			GSErr = ACAPI_Element_Get(&camera);
			if (GSErr != NoError)
			{
				FSyncData*& CameraSyncData = FSyncDatabase::GetSyncData(APIGuid2GSGuid(cameraSet.header.guid));
				if (CameraSyncData == nullptr)
				{
					CameraSyncData = new FSyncData::FCamera(APIGuid2GSGuid(camera.header.guid), ++IndexCamera);
					CameraSyncData->SetParent(CameraSetSyncData);
				}
				CameraSyncData->MarkAsExisting();
				CameraSyncData->CheckModificationStamp(camera.header.modiStamp);
			}
			NextCamera = camera.camera.perspCam.nextCam;
		}

		if (GSErr != NoError && GSErr != APIERR_DELETED)
		{
			UE_AC_DebugF("FSyncDatabase::ScanCameras - ACAPI_Element_Get return %d", GSErr);
		}
	}
}

END_NAMESPACE_UE_AC
