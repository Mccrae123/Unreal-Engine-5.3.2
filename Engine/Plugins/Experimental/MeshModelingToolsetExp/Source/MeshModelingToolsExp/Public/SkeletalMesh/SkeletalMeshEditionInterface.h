﻿// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "UObject/Interface.h"
#include "UObject/WeakInterfacePtr.h"
#include "SkeletalMeshNotifier.h"

#include "HitProxies.h"
#include "ToolContextInterfaces.h"
#include "GenericPlatform/ICursor.h"

#include "SkeletalMeshEditionInterface.generated.h"

class FSkeletalMeshToolNotifier;
class UTransformProxy;
class UInteractiveToolManager;
class IGizmoStateTarget;
struct FInputDeviceRay;

/**
 * USkeletalMeshEditionInterface
 */

UINTERFACE()
class MESHMODELINGTOOLSEXP_API USkeletalMeshEditionInterface : public UInterface
{
	GENERATED_BODY()
};

/**
 * ISkeletalMeshEditionInterface
 */

class MESHMODELINGTOOLSEXP_API ISkeletalMeshEditionInterface
{
	GENERATED_BODY()

public:
	ISkeletalMeshNotifier& GetNotifier();
	bool NeedsNotification() const;

	virtual TArray<FName> GetSelectedBones() const;
	
	void BindTo(TSharedPtr<ISkeletalMeshEditorBinding> InBinding);
	void Unbind();
	
protected:
	virtual void HandleSkeletalMeshModified(const TArray<FName>& BoneNames, const ESkeletalMeshNotifyType InNotifyType) = 0;
	TOptional<FName> GetBoneName(HHitProxy* InHitProxy) const;
	
	TWeakPtr<ISkeletalMeshEditorBinding> Binding;

private:
	TUniquePtr<FSkeletalMeshToolNotifier> Notifier;
	
	friend FSkeletalMeshToolNotifier;
};

/**
 * FSkeletalMeshToolNotifier
 */

class FSkeletalMeshToolNotifier: public ISkeletalMeshNotifier
{
public:
	FSkeletalMeshToolNotifier(TWeakInterfacePtr<ISkeletalMeshEditionInterface> InInterface);
	virtual void HandleNotification(const TArray<FName>& BoneNames, const ESkeletalMeshNotifyType InNotifyType) override;
	
protected:
	TWeakInterfacePtr<ISkeletalMeshEditionInterface> Interface;
};

/**
 * HBoneHitProxy
 */

struct HBoneHitProxy : public HHitProxy
{
	DECLARE_HIT_PROXY()

	int32 BoneIndex;
	FName BoneName;

	HBoneHitProxy(int32 InBoneIndex, FName InBoneName)
		: HHitProxy(HPP_Foreground)
		, BoneIndex(InBoneIndex)
		, BoneName(InBoneName)
	{}

	virtual EMouseCursor::Type GetMouseCursor() override { return EMouseCursor::Crosshairs; }
};

/**
 * USkeletalMeshGizmoContextObjectBase
 */

UCLASS(Abstract)
class MESHMODELINGTOOLSEXP_API USkeletalMeshGizmoContextObjectBase : public UObject
{
	GENERATED_BODY()
	
public:
	virtual USkeletalMeshGizmoWrapperBase* GetNewWrapper(UInteractiveToolManager* InToolManager, UObject* Outer = nullptr, IGizmoStateTarget* InStateTarget = nullptr)
	PURE_VIRTUAL(UGizmoContextObject::GetNewWrapper, return nullptr;);
};

/**
 * USkeletalMeshGizmoWrapperBase
 */

UCLASS(Abstract)
class MESHMODELINGTOOLSEXP_API USkeletalMeshGizmoWrapperBase : public UObject
{
	GENERATED_BODY()

protected:
	using FGetTransform = TUniqueFunction<FTransform(void)>;
	using FSetTransform = TUniqueFunction<void(const FTransform&)>;
	
public:
	virtual void Initialize(const FTransform& InTransform = FTransform::Identity, const EToolContextCoordinateSystem& InTransformMode = EToolContextCoordinateSystem::Local)
	PURE_VIRTUAL(USkeletalMeshGizmoWrapperBase::Initialize, return;)
	
	virtual void HandleBoneTransform(FGetTransform GetTransformFunc, FSetTransform SetTransformFunc)
	PURE_VIRTUAL(USkeletalMeshGizmoWrapperBase::HandleBoneTransform, return;)

	virtual void Clear() PURE_VIRTUAL(USkeletalMeshGizmoWrapperBase::Clear, return;)
	
	virtual bool IsGizmoHit(const FInputDeviceRay& PressPos) const PURE_VIRTUAL(USkeletalMeshGizmoWrapperBase::IsGizmoHit, return false;)

	UPROPERTY()
	TWeakObjectPtr<USceneComponent> Component;
};