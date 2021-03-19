// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "AddonTools.h"

#include "Light.hpp"

#include <vector>

namespace ModelerAPI
{
class Transformation;
}

BEGIN_NAMESPACE_UE_AC

class FElementID;
class FSyncContext;
class FSyncDatabase;

/* Class that keep synchronization data of Archicad elements.
 * Take care of object hierarchy (By synthetizing layer an scene) */
class FSyncData
{
  public:
	class FScene;
	class FActor;
	class FLayer;
	class FElement;
	class FCameraSet;
	class FCamera;
	class FLight;

	class FHotLinksRoot;
	class FHotLinkNode;
	class FHotLinkInstance;

	// Constructor
	FSyncData(const GS::Guid& InGuid);

	// Destructor
	virtual ~FSyncData();

	// Update data from a 3d element
	void Update(const FElementID& IoElementId);

	// Return the element index in current 3d context
	GS::Int32 GetIndex3D() const { return Index3D; }

	// Return true if object have been modified (3d or API)
	bool IsModified() const { return bIsModified; }

	// Check modification stamp
	bool CheckModificationStamp(UInt64 InModificationStamp)
	{
		if (ModificationStamp != InModificationStamp)
		{
			ModificationStamp = InModificationStamp;
			bIsModified = true;
		}
		return bIsModified;
	}

	// Set modification state to true
	void MarkAsModified() { bIsModified = true; }

	// Before a scan, we presume object as deletable and not modified
	void ResetBeforeScan()
	{
		Index3D = 0;
		bIsModified = false;
	}

	void MarkAsExisting() { Index3D = -1; }

	// Recursively clean. Delete element that hasn't 3d geometry related to it
	void CleanAfterScan(FSyncDatabase* IOSyncDatabase);

	void SetParent(FSyncData* InParent);

	bool HasParent() const { return Parent != nullptr; }

	void SetDefaultParent(const FElementID& InElementID);

	// Working class that contain data to process elements and it's childs
	class FProcessInfo;

	// Sync Datasmith elements from Archicad elements (for this syncdata and it's childs)
	void ProcessTree(FProcessInfo* IOProcessInfo);

	// Delete this sync data
	virtual TSharedPtr< IDatasmithElement > GetElement() const = 0;

  protected:
	// Add a child
	virtual void AddChildActor(const TSharedPtr< IDatasmithActorElement >& InActor) = 0;

	// Remove a child
	virtual void RemoveChildActor(const TSharedPtr< IDatasmithActorElement >& InActor) = 0;

	// Set (or replace) datasmith actor element related to this sync data
	virtual void SetActorElement(const TSharedPtr< IDatasmithActorElement >& InElement) = 0;

	// Return Element as an actor
	virtual const TSharedPtr< IDatasmithActorElement >& GetActorElement() const = 0;

	// Process (Sync Datasmith element from Archicad element)
	virtual void Process(FProcessInfo* IOProcessInfo) = 0;

	// Delete this sync data
	virtual void DeleteMe(FSyncDatabase* IOSyncDatabase);

	// Add a child to this sync data
	void AddChild(FSyncData* InChild);

	// Remove a child from this sync data
	void RemoveChild(FSyncData* InChild);

	// Permanent id of the element (Synthethized elements, like layers, have synthetized guid).
	GS::Guid ElementId = GS::NULLGuid;

	// Temporary 3d index of the element
	GS::Int32 Index3D = 0;

	// 3d generation id, change when 3d geometry of the object is changed
	GS::UInt32 GenId = 0;

	// modification stamp
	UInt64 ModificationStamp = 0;

	// If GenId have changed or object is newly renderered
	bool bIsModified = false;

	// Parent of this element
	FSyncData* Parent = nullptr;

	// Childs of this element
	std::vector< FSyncData* > Childs;
};

class FSyncData::FScene : public FSyncData
{
  public:
	// Guid given to the scene element.
	static const GS::Guid SceneGUID;

	FScene();

	// Delete this sync data
	virtual TSharedPtr< IDatasmithElement > GetElement() const override { return SceneElement; };

  protected:
	virtual void Process(FProcessInfo* IOProcessInfo) override;

	// Delete this sync data
	virtual void DeleteMe(FSyncDatabase* IOSyncDatabase) override;

	void UpdateInfo(FProcessInfo* IOProcessInfo);

	// Add an child actor to my scene
	virtual void AddChildActor(const TSharedPtr< IDatasmithActorElement >& InActor) override;

	// Remove an child actor from my scene
	virtual void RemoveChildActor(const TSharedPtr< IDatasmithActorElement >& InActor) override;

	// Call this a FScene has no meaning... calling it will throw an exception.
	virtual void SetActorElement(const TSharedPtr< IDatasmithActorElement >& InActor) override;

	// Return an invalid actor shared ptr
	virtual const TSharedPtr< IDatasmithActorElement >& GetActorElement() const override;

	// The mesh element if this element is a mesh actor
	TSharedPtr< IDatasmithScene > SceneElement;

	// Empty actor that will contain matedata info on the scene
	TSharedPtr< IDatasmithActorElement > SceneInfoActorElement;

	// The mesh element if this element is a mesh actor
	TSharedPtr< IDatasmithMetaDataElement > SceneInfoMetaData;
};

class FSyncData::FActor : public FSyncData
{
	// Delete this sync data
	virtual TSharedPtr< IDatasmithElement > GetElement() const override { return ActorElement; };

  protected:
	FActor(const GS::Guid& InGuid);

	// Delete this sync data
	virtual void DeleteMe(FSyncDatabase* IOSyncDatabase) override;

	// Add an child actor to my element
	virtual void AddChildActor(const TSharedPtr< IDatasmithActorElement >& InActor) override;

	// Remove an child actor to my element
	virtual void RemoveChildActor(const TSharedPtr< IDatasmithActorElement >& InActor) override;

	// Set (or replace) datasmith actor element related to this sync data
	virtual void SetActorElement(const TSharedPtr< IDatasmithActorElement >& InActor) override;

	// Return Element as an actor
	virtual const TSharedPtr< IDatasmithActorElement >& GetActorElement() const override { return ActorElement; }

	// Add meta data
	void AddTags(const FElementID& InElementID);

	void ReplaceMetaData(IDatasmithScene& IOScene, const TSharedPtr< IDatasmithMetaDataElement >& InNewMetaData);

	TSharedPtr< IDatasmithActorElement > ActorElement;

	// The mesh element if this element is a mesh actor
	TSharedPtr< IDatasmithMetaDataElement > MetaData;
};

class FSyncData::FLayer : public FSyncData::FActor
{
  public:
	// Guid used to synthetize layer guid
	static const GS::Guid LayerGUID;

	// Return the synthetized layer guid.
	static GS::Guid GetLayerGUID(short Layer);

	// Return true if this guid is for a layer
	static short IsLayerGUID(GS::Guid LayerID);

	// Return the layer index
	static short GetLayerIndex(const GS::Guid& InLayerID);

	FLayer(const GS::Guid& InGuid);

  protected:
	virtual void Process(FProcessInfo* IOProcessInfo) override;
};

class FSyncData::FElement : public FSyncData::FActor
{
  public:
	FElement(const GS::Guid& InGuid);

  protected:
	virtual void Process(FProcessInfo* IOProcessInfo) override;

	// Create/Update mesh of this element
	bool CreateMesh(FElementID* IOElementID, const ModelerAPI::Transformation& InLocalToWorld);

	// Rebuild the meta data of this element
	void UpdateMetaData(IDatasmithScene& IOScene);

	// The mesh element if this element is a mesh actor
	TSharedPtr< IDatasmithMeshElement > MeshElement;
};

class FSyncData::FCameraSet : public FSyncData::FActor
{
  public:
	FCameraSet(const GS::Guid& InGuid, const GS::UniString& InName, bool bInOpenedPath)
		: FSyncData::FActor(InGuid)
		, Name(InName)
		, bOpenedPath(bInOpenedPath)
	{
	}

  protected:
	virtual void Process(FProcessInfo* IOProcessInfo) override;

	const GS::UniString& Name;
	bool				 bOpenedPath;
};

class FSyncData::FCamera : public FSyncData::FActor
{
  public:
	// Guid given to the current view.
	static const GS::Guid CurrentViewGUID;

	FCamera(const GS::Guid& InGuid, GS::Int32 InIndex)
		: FSyncData::FActor(InGuid)
		, Index(InIndex)
	{
	}

  protected:
	virtual void Process(FProcessInfo* IOProcessInfo) override;

	void InitWithCurrentView();

	void InitWithCameraElement();

	GS::Int32 Index;
};

class FSyncData::FLight : public FSyncData::FActor
{
  public:
	FLight(const GS::Guid& InGuid, GS::Int32 InIndex)
		: FSyncData::FActor(InGuid)
		, Index(InIndex)
	{
	}

	void SetValues(ModelerAPI::Light::Type InType, float InInnerConeAngle, float InOuterConeAngle,
				   const FLinearColor& InColor)
	{
		if (Type != InType || InnerConeAngle != InInnerConeAngle || OuterConeAngle != InOuterConeAngle ||
			Color != InColor)
		{
			Type = InType;
			InnerConeAngle = InInnerConeAngle;
			OuterConeAngle = InOuterConeAngle;
			Color = InColor;
			bIsModified = true;
		}
	}

	void Placement(const FVector& InPosition, const FQuat& InRotation)
	{
		if (Position != InPosition || Rotation != InRotation)
		{
			Position = InPosition;
			Rotation = InRotation;
			bIsModified = true;
		}
	}

  protected:
	virtual void Process(FProcessInfo* IOProcessInfo) override;

	GS::Int32				Index;
	ModelerAPI::Light::Type Type;
	float					InnerConeAngle;
	float					OuterConeAngle;
	FLinearColor			Color;
	FVector					Position;
	FQuat					Rotation;
};

class FSyncData::FHotLinksRoot : public FSyncData::FActor
{
  public:
	// Guid given to the current view.
	static const GS::Guid HotLinksRootGUID;

	FHotLinksRoot()
		: FSyncData::FActor(HotLinksRootGUID)
	{
	}

  protected:
	virtual void Process(FProcessInfo* IOProcessInfo) override;
};

class FSyncData::FHotLinkNode : public FSyncData::FActor
{
  public:
	FHotLinkNode(const GS::Guid& InGuid)
		: FSyncData::FActor(InGuid)
	{
	}

  protected:
	virtual void Process(FProcessInfo* IOProcessInfo) override;
};

class FSyncData::FHotLinkInstance : public FSyncData::FActor
{
  public:
	FHotLinkInstance(const GS::Guid& InGuid, FSyncDatabase* IOSyncDatabase);

	const API_Tranmat& GetTransformation() { return Transformation; }

  protected:
	virtual void Process(FProcessInfo* IOProcessInfo) override;

	API_Tranmat Transformation;
};

END_NAMESPACE_UE_AC
