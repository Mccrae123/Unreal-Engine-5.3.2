// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "Json/GLTFJsonCore.h"
#include "Json/GLTFJsonCameraControl.h"

struct GLTFEXPORTER_API FGLTFJsonOrthographic : IGLTFJsonObject
{
	float XMag; // horizontal magnification of the view
	float YMag; // vertical magnification of the view
	float ZFar;
	float ZNear;

	FGLTFJsonOrthographic()
		: XMag(0)
		, YMag(0)
		, ZFar(0)
		, ZNear(0)
	{
	}

	virtual void WriteObject(IGLTFJsonWriter& Writer) const override;
};

struct GLTFEXPORTER_API FGLTFJsonPerspective : IGLTFJsonObject
{
	float AspectRatio; // aspect ratio of the field of view
	float YFov; // vertical field of view in radians
	float ZFar;
	float ZNear;

	FGLTFJsonPerspective()
		: AspectRatio(0)
		, YFov(0)
		, ZFar(0)
		, ZNear(0)
	{
	}

	virtual void WriteObject(IGLTFJsonWriter& Writer) const override;
};

struct GLTFEXPORTER_API FGLTFJsonCamera : IGLTFJsonIndexedObject
{
	FString Name;

	EGLTFJsonCameraType               Type;
	TOptional<FGLTFJsonCameraControl> CameraControl;

	FGLTFJsonOrthographic Orthographic;
	FGLTFJsonPerspective  Perspective;

	FGLTFJsonCamera(int32 Index = INDEX_NONE)
		: IGLTFJsonIndexedObject(Index)
		, Type(EGLTFJsonCameraType::None)
	{
	}

	virtual void WriteObject(IGLTFJsonWriter& Writer) const override;
};
