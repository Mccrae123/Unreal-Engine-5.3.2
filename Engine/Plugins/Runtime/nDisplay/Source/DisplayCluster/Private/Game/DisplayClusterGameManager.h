// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

#include "Game/IPDisplayClusterGameManager.h"

class AActor;
class ADisplayClusterRootActor;
class USceneComponent;
class UDisplayClusterCameraComponent;
class UDisplayClusterRootComponent;
class UDisplayClusterSceneComponent;
class UDisplayClusterScreenComponent;


/**
 * Game manager. Responsible for building VR object hierarchy from a config file. Implements some in-game logic.
 */
class FDisplayClusterGameManager
	: public IPDisplayClusterGameManager
{
public:
	FDisplayClusterGameManager();
	virtual ~FDisplayClusterGameManager();

public:
	//////////////////////////////////////////////////////////////////////////////////////////////
	// IPDisplayClusterManager
	//////////////////////////////////////////////////////////////////////////////////////////////
	virtual bool Init(EDisplayClusterOperationMode OperationMode) override;
	virtual void Release() override;
	virtual bool StartSession(const FString& InConfigPath, const FString& InNodeId) override;
	virtual void EndSession() override;
	virtual bool StartScene(UWorld* World) override;
	virtual void EndScene() override;

public:
	//////////////////////////////////////////////////////////////////////////////////////////////
	// IDisplayClusterGameManager
	//////////////////////////////////////////////////////////////////////////////////////////////
	virtual ADisplayClusterRootActor*     GetRootActor() const override;
	virtual UDisplayClusterRootComponent* GetRootComponent() const override;

	virtual UWorld* GetWorld() const override
	{ return CurrentWorld; }

public:
	//////////////////////////////////////////////////////////////////////////////////////////////
	// IPDisplayClusterGameManager
	//////////////////////////////////////////////////////////////////////////////////////////////
	virtual bool IsDisplayClusterActive() const override
	{ return CurrentOperationMode != EDisplayClusterOperationMode::Disabled; }

private:
	ADisplayClusterRootActor* FindDisplayClusterRootActor(UWorld* InWorld);

private:
	// Active DisplayCluster root
	ADisplayClusterRootActor* DisplayClusterRootActor = nullptr;

	EDisplayClusterOperationMode CurrentOperationMode;
	FString ConfigPath;
	FString ClusterNodeId;
	UWorld* CurrentWorld;

	mutable FCriticalSection InternalsSyncScope;
};
