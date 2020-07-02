// Copyright Epic Games, Inc. All Rights Reserved.

#include "GizmoEdMode.h"
#include "DefaultAssetEditorGizmoFactory.h"
#include "Components/SceneComponent.h"
#include "EditorModeManager.h"
#include "EdModeInteractiveToolsContext.h"
#include "InteractiveGizmo.h"
#include "BaseGizmos/TransformGizmo.h"
#include "UnrealWidget.h"
#include "Utils.h"

#define LOCTEXT_NAMESPACE "FGizmoEdMode"

UGizmoEdMode::UGizmoEdMode()
	:Super()
{
	Info = FEditorModeInfo(
		FName(TEXT("GizmoMode")),
		LOCTEXT("ModeName", "Gizmo"),
		FSlateIcon(),
		false,
		600
	);
	SettingsClass = UGizmoEdModeSettings::StaticClass();
	ToolsContextClass = UEdModeInteractiveToolsContext::StaticClass();

	AddFactory(NewObject<UDefaultAssetEditorGizmoFactory>());
}

void UGizmoEdMode::AddFactory(TScriptInterface<IAssetEditorGizmoFactory> GizmoFactory)
{
	GizmoFactories.Add(MoveTemp(GizmoFactory));
	GizmoFactories.StableSort(
	    [](const TScriptInterface<IAssetEditorGizmoFactory>& a, const TScriptInterface<IAssetEditorGizmoFactory>& b) {
		    return a->GetPriority() > b->GetPriority();
	    });
}

void UGizmoEdMode::ActorSelectionChangeNotify()
{
	RecreateGizmo();
}

void UGizmoEdMode::RecreateGizmo()
{
	DestroyGizmo();
	for ( auto& Factory : GizmoFactories )
	{
		if (Factory->CanBuildGizmoForSelection(GetModeManager()))
		{
			TransformGizmo = Factory->BuildGizmoForSelection(GetModeManager(), ToolsContext->GizmoManager);
			LastFactory = &(*Factory);
			return;
		}
	}
}

void UGizmoEdMode::DestroyGizmo()
{
	LastFactory = nullptr;
	if ( TransformGizmo )
	{
		ToolsContext->GizmoManager->DestroyGizmo(TransformGizmo);
		TransformGizmo = nullptr;
	}
}

void UGizmoEdMode::Enter()
{
	Super::Enter();
	RecreateGizmo();
	WidgetModeChangedHandle =
	    GetModeManager()->OnWidgetModeChanged().AddLambda([this](FWidget::EWidgetMode) { RecreateGizmo(); });
	GetModeManager()->SetShowWidget(false);
}

void UGizmoEdMode::Exit()
{
	DestroyGizmo();
	GetModeManager()->OnWidgetModeChanged().Remove(WidgetModeChangedHandle);
	WidgetModeChangedHandle.Reset();
	GetModeManager()->SetShowWidget(true);
	Super::Exit();
}

bool UGizmoEdMode::InputKey(FEditorViewportClient* ViewportClient, FViewport* Viewport, FKey Key, EInputEvent Event)
{
	return ToolsContext->InputKey(ViewportClient, Viewport, Key, Event);
}

void UGizmoEdMode::Tick(FEditorViewportClient* ViewportClient, float DeltaTime)
{
	Super::Tick(ViewportClient, DeltaTime);
	if ( LastFactory )
	{
		LastFactory->ConfigureGridSnapping(GetDefault<ULevelEditorViewportSettings>()->GridEnabled,
		                                   GetDefault<ULevelEditorViewportSettings>()->RotGridEnabled, TransformGizmo);
	}
}

#undef LOCTEXT_NAMESPACE
