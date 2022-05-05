// Copyright Epic Games, Inc. All Rights Reserved.

#include "DisplayClusterLightCardEditorViewportClient.h"

#include "DisplayClusterLightCardEditorWidget.h"
#include "DisplayClusterConfigurationTypes.h"
#include "DisplayClusterProjectionStrings.h"
#include "DisplayClusterRootActor.h"
#include "DisplayClusterLightCardActor.h"
#include "Components/DisplayClusterPreviewComponent.h"
#include "Components/DisplayClusterCameraComponent.h"
#include "Components/DisplayClusterScreenComponent.h"

#include "AudioDevice.h"
#include "EngineUtils.h"
#include "EditorModes.h"
#include "EditorModeManager.h"
#include "PreviewScene.h"
#include "UnrealEdGlobals.h"
#include "ScopedTransaction.h"
#include "SDisplayClusterLightCardEditor.h"
#include "UnrealWidget.h"
#include "RayTracingDebugVisualizationMenuCommands.h"
#include "Components/DirectionalLightComponent.h"
#include "Components/LineBatchComponent.h"
#include "Components/PostProcessComponent.h"
#include "Components/SkyLightComponent.h"
#include "Components/PrimitiveComponent.h"
#include "Editor/UnrealEdEngine.h"
#include "Kismet/GameplayStatics.h"
#include "Slate/SceneViewport.h"
#include "Widgets/Docking/SDockTab.h"
#include "CustomEditorStaticScreenPercentage.h"
#include "LegacyScreenPercentageDriver.h"
#include "EngineModule.h"
#include "Debug/DebugDrawService.h"
#include "Engine/Canvas.h"
#include "Engine/TextureRenderTarget2D.h"
#include "CameraController.h"
#include "ImageUtils.h"
#include "Kismet/KismetMathLibrary.h"


#include "Renderer/Private/SceneRendering.h"

#define LOCTEXT_NAMESPACE "DisplayClusterLightCardEditorViewportClient"


//////////////////////////////////////////////////////////////////////////
// FNormalMap

const int32 FDisplayClusterLightCardEditorViewportClient::FNormalMap::NormalMapSize = 512;
const float FDisplayClusterLightCardEditorViewportClient::FNormalMap::NormalMapFOV = 2.0f * FMath::RadiansToDegrees(FMath::Atan(0.55 * PI)); // Equation for FOV from desired angle from north pole;

void FDisplayClusterLightCardEditorViewportClient::FNormalMap::Init(const FSceneViewInitOptions& InSceneViewInitOptions)
{
	SizeX = InSceneViewInitOptions.GetViewRect().Width();
	SizeY = InSceneViewInitOptions.GetViewRect().Height();

	ViewMatrices = FViewMatrices(InSceneViewInitOptions);

	if (NormalMapTexture.IsValid())
	{
		NormalMapTexture->MarkAsGarbage();
		NormalMapTexture = nullptr;
	}

	ENQUEUE_RENDER_COMMAND(InitRHIResourcesCommand)([this](FRHICommandListImmediate& RHICmdList)
		{
			const FRHITextureCreateDesc Desc =
				FRHITextureCreateDesc::Create2D(TEXT("NormalMapTexture"))
				.SetExtent(SizeX, SizeY)
				.SetFormat(PF_FloatRGBA)
				.SetClearValue(FClearValueBinding::Black);

			RHICreateTargetableShaderResource(Desc, ETextureCreateFlags::RenderTargetable, RenderTargetTextureRHI);
		});
}

void FDisplayClusterLightCardEditorViewportClient::FNormalMap::Release()
{
	ENQUEUE_RENDER_COMMAND(ReleaseRHIResourcesCommand)([this](FRHICommandListImmediate& RHICmdList)
		{
			RenderTargetTextureRHI.SafeRelease();
		});
}

bool FDisplayClusterLightCardEditorViewportClient::FNormalMap::GetNormalAndDistanceAtPosition(FVector Position, FVector& OutNormal, float& OutDistance)
{
	auto GetPixel = [this](uint32 InX, uint32 InY)
	{
		uint32 WrappedX = InX % SizeX;
		uint32 WrappedY = InY % SizeY;

		return CachedNormalData[WrappedY * SizeX + WrappedX].GetFloats();
	};

	const FVector ViewPos = FVector(ViewMatrices.GetViewMatrix().TransformFVector4(FVector4(Position, 1)));
	const FVector ProjectedViewPos = FDisplayClusterMeshProjectionRenderer::ProjectViewPosition(ViewPos, EDisplayClusterMeshProjectionType::Azimuthal);

	const FVector4 ScreenPos = ViewMatrices.GetProjectionMatrix().TransformFVector4(FVector4(ProjectedViewPos, 1));

	if (ScreenPos.W != 0.0)
	{
		const float InvW = (ScreenPos.W > 0.0f ? 1.0f : -1.0f) / ScreenPos.W;
		const float Y = (GProjectionSignY > 0.0f) ? ScreenPos.Y : 1.0f - ScreenPos.Y;
		const FVector2D PixelPos = FVector2D((0.5f + ScreenPos.X * 0.5f * InvW) * SizeX, (0.5f - Y * 0.5f * InvW) * SizeY);

		// Perform a bilinear interpolation on the computed pixel position to ensure a continuous normal regardless of the resolution of the normal map
		const uint32 PixelX = FMath::Floor(PixelPos.X - 0.5f);
		const uint32 PixelY = FMath::Floor(PixelPos.Y - 0.5f);
		const float PixelXFrac = FMath::Frac(PixelPos.X);
		const float PixelYFrac = FMath::Frac(PixelPos.X);

		FLinearColor NormalData;
		NormalData = FMath::Lerp(
			FMath::Lerp(GetPixel(PixelX, PixelY), GetPixel(PixelX + 1, PixelY), PixelXFrac),
			FMath::Lerp(GetPixel(PixelX, PixelY + 1), GetPixel(PixelX + 1, PixelY + 1), PixelXFrac),
			PixelYFrac);

		const FVector NormalVector = 2.f * FVector(NormalData.R, NormalData.G, NormalData.B) - 1.f;
		OutNormal = NormalVector.GetSafeNormal();

		// Make sure the depth value is not 0, as that will cause a divide by zero when transformed, resulting in an NaN distance being returned
		const float Depth = FMath::Max(0.001f, NormalData.A);

		FVector4 DepthPos = ViewMatrices.GetInvProjectionMatrix().TransformFVector4(FVector4(ScreenPos.X * InvW, ScreenPos.Y * InvW, Depth, 1.0f));
		DepthPos /= DepthPos.W;

		const FVector UnprojectedDepthPos = FDisplayClusterMeshProjectionRenderer::UnprojectViewPosition(DepthPos, EDisplayClusterMeshProjectionType::Azimuthal);
		OutDistance = UnprojectedDepthPos.Length();

		return true;
	}
	else
	{
		OutNormal = FVector::ZeroVector;
		OutDistance = 0.0;
		return false;
	}
}

UTexture2D* FDisplayClusterLightCardEditorViewportClient::FNormalMap::GenerateNormalMapTexture(const FString& TextureName)
{
	if (NormalMapTexture.IsValid())
	{
		NormalMapTexture->MarkAsGarbage();
		NormalMapTexture = nullptr;
	}

	if (CachedNormalData.Num())
	{
		FCreateTexture2DParameters Params;
		Params.bDeferCompression = true;

		TArray<FColor> Bitmap;
		Bitmap.AddZeroed(CachedNormalData.Num());

		for (int32 Index = 0; Index < CachedNormalData.Num(); ++Index)
		{
			Bitmap[Index] = CachedNormalData[Index].GetFloats().ToFColor(false);
		}

		NormalMapTexture = FImageUtils::CreateTexture2D(SizeX, SizeY, Bitmap, GetTransientPackage(), TextureName, RF_Transient, Params);
	}

	return GetNormalMapTexture();
}


//////////////////////////////////////////////////////////////////////////
// FDisplayClusterLightCardEditorViewportClient

FDisplayClusterLightCardEditorViewportClient::FDisplayClusterLightCardEditorViewportClient(FAdvancedPreviewScene& InPreviewScene,
                                                           const TWeakPtr<SEditorViewport>& InEditorViewportWidget,
                                                           TWeakPtr<SDisplayClusterLightCardEditor> InLightCardEditor) :
	FEditorViewportClient(nullptr, &InPreviewScene, InEditorViewportWidget)
{
	check (InLightCardEditor.IsValid());
	
	LightCardEditorPtr = InLightCardEditor;
	
	MeshProjectionRenderer = MakeShared<FDisplayClusterMeshProjectionRenderer>();
	MeshProjectionRenderer->ActorSelectedDelegate = FDisplayClusterMeshProjectionRenderer::FSelection::CreateRaw(this, &FDisplayClusterLightCardEditorViewportClient::IsLightCardSelected);
	MeshProjectionRenderer->RenderSimpleElementsDelegate = FDisplayClusterMeshProjectionRenderer::FSimpleElementPass::CreateRaw(this, &FDisplayClusterLightCardEditorViewportClient::Draw);

	bDraggingActor = false;
	DragWidgetOffset = FVector::ZeroVector;
	
	EditorWidget = MakeShared<FDisplayClusterLightCardEditorWidget>();

	// Setup defaults for the common draw helper.
	bUsesDrawHelper = false;

	EngineShowFlags.SetSelectionOutline(true);
	 
	check(Widget);
	Widget->SetSnapEnabled(true);
	
	ShowWidget(true);

	SetViewMode(VMI_Unlit);
	
	ViewportType = LVT_Perspective;
	bSetListenerPosition = false;
	bUseNumpadCameraControl = false;
	SetRealtime(true);
	SetShowStats(true);

	ResetFOVs();

	//This seems to be needed to get the correct world time in the preview.
	SetIsSimulateInEditorViewport(true);
	
	UpdatePreviewActor(LightCardEditorPtr.Pin()->GetActiveRootActor().Get());

	SetProjectionMode(EDisplayClusterMeshProjectionType::Azimuthal);
}

FDisplayClusterLightCardEditorViewportClient::~FDisplayClusterLightCardEditorViewportClient()
{
	EndTransaction();
	if (RootActorLevelInstance.IsValid())
	{
		RootActorLevelInstance->UnsubscribeFromPostProcessRenderTarget(reinterpret_cast<uint8*>(this));
	}
}

FLinearColor FDisplayClusterLightCardEditorViewportClient::GetBackgroundColor() const
{
	return FLinearColor::Gray;
}

void FDisplayClusterLightCardEditorViewportClient::Tick(float DeltaSeconds)
{
	FEditorViewportClient::Tick(DeltaSeconds);

	// Camera position is locked to a specific location
	ResetCamera(/* bLocationOnly */ true);

	CalcEditorWidgetTransform(CachedEditorWidgetTransformBeforeMapProjection, CachedEditorWidgetTransformAfterMapProjection);

	// Tick the preview scene world.
	if (!GIntraFrameDebuggingGameThread)
	{
		// Allow full tick only if preview simulation is enabled and we're not currently in an active SIE or PIE session
		if (GEditor->PlayWorld == nullptr && !GEditor->bIsSimulatingInEditor)
		{
			PreviewScene->GetWorld()->Tick(IsRealtime() ? LEVELTICK_All : LEVELTICK_TimeOnly, DeltaSeconds);
		}
		else
		{
			PreviewScene->GetWorld()->Tick(IsRealtime() ? LEVELTICK_ViewportsOnly : LEVELTICK_TimeOnly, DeltaSeconds);
		}
	}

	if (RootActorProxy.IsValid() && RootActorLevelInstance.IsValid())
	{
		// Pass the preview render targets from the level instance root actor to the preview root actor
		UDisplayClusterConfigurationData* Config = RootActorLevelInstance->GetConfigData();

		for (const TPair<FString, UDisplayClusterConfigurationClusterNode*>& NodePair : Config->Cluster->Nodes)
		{
			const UDisplayClusterConfigurationClusterNode* Node = NodePair.Value;
			for (const TPair<FString, UDisplayClusterConfigurationViewport*>& ViewportPair : Node->Viewports)
			{
				UDisplayClusterPreviewComponent* LevelInstancePreviewComp = RootActorLevelInstance->GetPreviewComponent(NodePair.Key, ViewportPair.Key);
				UDisplayClusterPreviewComponent* PreviewComp = RootActorProxy->GetPreviewComponent(NodePair.Key, ViewportPair.Key);

				if (PreviewComp && LevelInstancePreviewComp)
				{
					PreviewComp->SetOverrideTexture(LevelInstancePreviewComp->GetRenderTargetTexturePostProcess());
				}
			}
		}
	}

	// EditorViewportClient sets the cursor settings based on the state of the built in FWidget, which isn't being used here, so
	// force a software cursor if we are dragging an actor so that the correct mouse cursor shows up
	if (bDraggingActor)
	{
		SetRequiredCursor(false, true);
		SetRequiredCursorOverride(true, EMouseCursor::CardinalCross);

		ApplyRequiredCursorVisibility(true);
	}

	if (DesiredLookAtLocation.IsSet())
	{
		const FRotator LookAtRotation = UKismetMathLibrary::FindLookAtRotation(GetViewLocation(), *DesiredLookAtLocation);
		const FRotator NewRotation = FMath::RInterpTo(GetViewRotation(), LookAtRotation, DeltaSeconds, DesiredLookAtSpeed);
		SetViewRotation(NewRotation);

		if (NewRotation.Equals(LookAtRotation, 2.f) ||
			!IsLocationCloseToEdge(CachedEditorWidgetTransformAfterMapProjection.GetTranslation()))
		{
			DesiredLookAtLocation.Reset();
		}
	}
}

void FDisplayClusterLightCardEditorViewportClient::Draw(FViewport* InViewport, FCanvas* Canvas)
{
	if (bNormalMapInvalid)
	{
		RenderNormalMap(NorthNormalMap, FVector::UpVector);
		RenderNormalMap(SouthNormalMap, -FVector::UpVector);
		bNormalMapInvalid = false;
	}

	FViewport* ViewportBackup = Viewport;
	Viewport = InViewport ? InViewport : Viewport;

	UWorld* World = GetWorld();
	FGameTime Time;
	if (!World || (GetScene() != World->Scene) || UseAppTime()) 
	{
		Time = FGameTime::GetTimeSinceAppStart();
	}
	else
	{
		Time = World->GetTime();
	}

	FEngineShowFlags UseEngineShowFlags = EngineShowFlags;
	if (OverrideShowFlagsFunc)
	{
		OverrideShowFlagsFunc(UseEngineShowFlags);
	}

	// Setup a FSceneViewFamily/FSceneView for the viewport.
	FSceneViewFamilyContext ViewFamily(FSceneViewFamily::ConstructionValues(
		Canvas->GetRenderTarget(),
		GetScene(),
		UseEngineShowFlags)
		.SetTime(Time)
		.SetRealtimeUpdate(IsRealtime() && FSlateThrottleManager::Get().IsAllowingExpensiveTasks())
	);

	ViewFamily.DebugDPIScale = GetDPIScale();
	ViewFamily.bIsHDR = Viewport->IsHDRViewport();

	ViewFamily.EngineShowFlags = UseEngineShowFlags;
	ViewFamily.EngineShowFlags.CameraInterpolation = 0;
	ViewFamily.EngineShowFlags.SetScreenPercentage(false);

	ViewFamily.ViewExtensions = GEngine->ViewExtensions->GatherActiveExtensions(FSceneViewExtensionContext(InViewport));

	for (auto ViewExt : ViewFamily.ViewExtensions)
	{
		ViewExt->SetupViewFamily(ViewFamily);
	}

	ViewFamily.ViewMode = VMI_Unlit;

	EngineShowFlagOverride(ESFIM_Editor, ViewFamily.ViewMode, ViewFamily.EngineShowFlags, false);
	EngineShowFlagOrthographicOverride(IsPerspective(), ViewFamily.EngineShowFlags);

	ViewFamily.ExposureSettings = ExposureSettings;

	// Setup the screen percentage and upscaling method for the view family.
	{
		checkf(ViewFamily.GetScreenPercentageInterface() == nullptr,
			TEXT("Some code has tried to set up an alien screen percentage driver, that could be wrong if not supported very well by the RHI."));

		if (SupportsLowDPIPreview() && IsLowDPIPreview() && ViewFamily.SupportsScreenPercentage())
		{
			ViewFamily.SecondaryViewFraction = GetDPIDerivedResolutionFraction();
		}
	}

	FSceneView* View = nullptr;

	View = CalcSceneView(&ViewFamily, INDEX_NONE);
	SetupViewForRendering(ViewFamily,*View);

	FSlateRect SafeFrame;
	View->CameraConstrainedViewRect = View->UnscaledViewRect;
	if (CalculateEditorConstrainedViewRect(SafeFrame, Viewport, Canvas->GetDPIScale()))
	{
		View->CameraConstrainedViewRect = FIntRect(SafeFrame.Left, SafeFrame.Top, SafeFrame.Right, SafeFrame.Bottom);
	}

	{
		// If a screen percentage interface was not set by one of the view extension, then set the legacy one.
		if (ViewFamily.GetScreenPercentageInterface() == nullptr)
		{
			float GlobalResolutionFraction = 1.0f;

			if (SupportsPreviewResolutionFraction() && ViewFamily.SupportsScreenPercentage())
			{
				GlobalResolutionFraction = GetDefaultPrimaryResolutionFractionTarget();

				// Force screen percentage's engine show flag to be turned on for preview screen percentage.
				ViewFamily.EngineShowFlags.ScreenPercentage = (GlobalResolutionFraction != 1.0);
			}

			// In editor viewport, we ignore r.ScreenPercentage and FPostProcessSettings::ScreenPercentage by design.
			ViewFamily.SetScreenPercentageInterface(new FLegacyScreenPercentageDriver(ViewFamily, GlobalResolutionFraction));
		}

		check(ViewFamily.GetScreenPercentageInterface() != nullptr);
	}

	Canvas->Clear(FLinearColor::Black);

	FSceneViewInitOptions ViewInitOptions;
	GetSceneViewInitOptions(ViewInitOptions);

	MeshProjectionRenderer->Render(Canvas, GetScene(), ViewInitOptions, EngineShowFlags, ProjectionMode);

	if (View)
	{
		DrawCanvas(*Viewport, *View, *Canvas);
	}

	if (bDisplayNormalMapVisualization)
	{
		auto DrawNormalMap = [Canvas](FNormalMap& NormalMap, const FString& TextureName, FVector2D Position)
		{
			UTexture2D* NormalMapTexture = NormalMap.GetNormalMapTexture();

			if (!NormalMapTexture)
			{
				NormalMapTexture = NormalMap.GenerateNormalMapTexture(TextureName);
			}

			if (NormalMapTexture)
			{
				Canvas->DrawTile(Position.X, Position.Y, 512, 512, 0, 0, 1, 1, FLinearColor::White, NormalMapTexture->GetResource());
			}
		};

		DrawNormalMap(NorthNormalMap, TEXT("DisplayClusterLightCardEditor.NorthNormalMap"), FVector2D(0.0f, 0.0f));
		DrawNormalMap(SouthNormalMap, TEXT("DisplayClusterLightCardEditor.SouthNormalMap"), FVector2D(0.0f, 512.0f));
	}

	// Remove temporary debug lines.
	// Possibly a hack. Lines may get added without the scene being rendered etc.
	if (World->LineBatcher != NULL && (World->LineBatcher->BatchedLines.Num() || World->LineBatcher->BatchedPoints.Num() || World->LineBatcher->BatchedMeshes.Num() ) )
	{
		World->LineBatcher->Flush();
	}

	if (World->ForegroundLineBatcher != NULL && (World->ForegroundLineBatcher->BatchedLines.Num() || World->ForegroundLineBatcher->BatchedPoints.Num() || World->ForegroundLineBatcher->BatchedMeshes.Num() ) )
	{
		World->ForegroundLineBatcher->Flush();
	}

	// Draw the widget.
	/*if (Widget && bShowWidget)
	{
		Widget->DrawHUD( Canvas );
	}*/

	// Axes indicators
	if (bDrawAxes && !ViewFamily.EngineShowFlags.Game && !GLevelEditorModeTools().IsViewportUIHidden() && !IsVisualizeCalibrationMaterialEnabled())
	{
		// TODO: Figure out how we want the axes widget to be drawn
		DrawAxes(Viewport, Canvas);
	}

	// NOTE: DebugCanvasObject will be created by UDebugDrawService::Draw() if it doesn't already exist.
	FCanvas* DebugCanvas = Viewport->GetDebugCanvas();
	UDebugDrawService::Draw(ViewFamily.EngineShowFlags, Viewport, View, DebugCanvas);
	UCanvas* DebugCanvasObject = FindObjectChecked<UCanvas>(GetTransientPackage(),TEXT("DebugCanvasObject"));
	DebugCanvasObject->Canvas = DebugCanvas;
	DebugCanvasObject->Init( Viewport->GetSizeXY().X, Viewport->GetSizeXY().Y, View , DebugCanvas);

	// Stats display
	if( IsRealtime() && ShouldShowStats() && DebugCanvas)
	{
		const int32 XPos = 4;
		TArray< FDebugDisplayProperty > EmptyPropertyArray;
		DrawStatsHUD( World, Viewport, DebugCanvas, NULL, EmptyPropertyArray, GetViewLocation(), GetViewRotation() );
	}

	if(!IsRealtime())
	{
		// Wait for the rendering thread to finish drawing the view before returning.
		// This reduces the apparent latency of dragging the viewport around.
		FlushRenderingCommands();
	}

	Viewport = ViewportBackup;
}

void FDisplayClusterLightCardEditorViewportClient::Draw(const FSceneView* View, FPrimitiveDrawInterface* PDI)
{
	if (SelectedLightCards.Num())
	{
		EditorWidget->SetTransform(CachedEditorWidgetTransformAfterMapProjection);
		EditorWidget->Draw(View, PDI);
	}
}

FSceneView* FDisplayClusterLightCardEditorViewportClient::CalcSceneView(FSceneViewFamily* ViewFamily, const int32 StereoViewIndex)
{
	FSceneViewInitOptions ViewInitOptions;
	GetSceneViewInitOptions(ViewInitOptions);

	ViewInitOptions.ViewFamily = ViewFamily;

	TimeForForceRedraw = 0.0;

	FSceneView* View = new FSceneView(ViewInitOptions);

	View->SubduedSelectionOutlineColor = GEngine->GetSubduedSelectionOutlineColor();

	int32 FamilyIndex = ViewFamily->Views.Add(View);
	check(FamilyIndex == View->StereoViewIndex || View->StereoViewIndex == INDEX_NONE);

	View->StartFinalPostprocessSettings(View->ViewLocation);

	OverridePostProcessSettings(*View);

	View->EndFinalPostprocessSettings(ViewInitOptions);

	for (int ViewExt = 0; ViewExt < ViewFamily->ViewExtensions.Num(); ViewExt++)
	{
		ViewFamily->ViewExtensions[ViewExt]->SetupView(*ViewFamily, *View);
	}

	return View;
}

bool FDisplayClusterLightCardEditorViewportClient::InputKey(FViewport* InViewport, int32 ControllerId, FKey Key, EInputEvent Event,
                                            float AmountDepressed, bool bGamepad)
{
	if ((Key == EKeys::MouseScrollUp || Key == EKeys::MouseScrollDown) && Event == IE_Pressed)
	{
		const int32 Sign = Key == EKeys::MouseScrollUp ? -1 : 1;
		const float CurrentFOV = GetProjectionModeFOV(ProjectionMode);
		const float NewFOV = FMath::Clamp(CurrentFOV + Sign * FOVScrollIncrement, CameraController->GetConfig().MinimumAllowedFOV, CameraController->GetConfig().MaximumAllowedFOV);

		SetProjectionModeFOV(ProjectionMode, NewFOV);
		return true;
	}

	return FEditorViewportClient::InputKey(InViewport, ControllerId, Key, Event, AmountDepressed, bGamepad);
}

bool FDisplayClusterLightCardEditorViewportClient::InputWidgetDelta(FViewport* InViewport,
	EAxisList::Type CurrentAxis,
	FVector& Drag,
	FRotator& Rot,
	FVector& Scale)
{
	bool bHandled = false;
	if (FEditorViewportClient::InputWidgetDelta(InViewport, CurrentAxis, Drag, Rot, Scale))
	{
		bHandled = true;
	}
	else
	{
		if (CurrentAxis != EAxisList::Type::None && SelectedLightCards.Num())
		{
			MoveSelectedLightCards(InViewport, CurrentAxis);
			bHandled = true;
		}
	}

	return bHandled;
}

void FDisplayClusterLightCardEditorViewportClient::TrackingStarted(const FInputEventState& InInputState, bool bIsDraggingWidget,
	bool bNudge)
{
	if (!bDraggingActor && bIsDraggingWidget && InInputState.IsLeftMouseButtonPressed() && SelectedLightCards.Num())
	{
		GEditor->DisableDeltaModification(true);
		{
			// The pivot location won't update properly and the actor will rotate / move around the original selection origin
			// so update it here to fix that.
			GUnrealEd->UpdatePivotLocationForSelection();
			GUnrealEd->SetPivotMovedIndependently(false);
		}

		BeginTransaction(LOCTEXT("MoveLightCard", "Move Light Card"));
		bDraggingActor = true;
		
		DesiredLookAtLocation.Reset();

		// Compute and store the delta between the widget's origin and the place the user clicked on it,
		// in order to factor it out when transforming the selected actor
		FIntPoint MousePos;
		InInputState.GetViewport()->GetMousePos(MousePos);

		FSceneViewFamilyContext ViewFamily(FSceneViewFamily::ConstructionValues(
			InInputState.GetViewport(),
			GetScene(),
			EngineShowFlags)
			.SetRealtimeUpdate(IsRealtime()));
		FSceneView* View = CalcSceneView(&ViewFamily);

		FVector Origin;
		FVector Direction;
		PixelToWorld(*View, MousePos, Origin, Direction);

		DragWidgetOffset = Direction - (CachedEditorWidgetTransformBeforeMapProjection.GetTranslation() - Origin).GetSafeNormal();
	}

	FEditorViewportClient::TrackingStarted(InInputState, bIsDraggingWidget, bNudge);
}

void FDisplayClusterLightCardEditorViewportClient::TrackingStopped()
{
	bDraggingActor = false;
	DragWidgetOffset = FVector::ZeroVector;
	EndTransaction();

	if (SelectedLightCards.Num())
	{
		GEditor->DisableDeltaModification(false);
	}
	
	FEditorViewportClient::TrackingStopped();
}

void FDisplayClusterLightCardEditorViewportClient::ProcessClick(FSceneView& View, HHitProxy* HitProxy, FKey Key, EInputEvent Event, uint32 HitX, uint32 HitY)
{
	UWorld* PreviewWorld = PreviewScene->GetWorld();
	check(PreviewWorld);

	const bool bIsCtrlKeyDown = Viewport->KeyState(EKeys::LeftControl) || Viewport->KeyState(EKeys::RightControl);

	const bool bMultiSelect = Key == EKeys::LeftMouseButton && bIsCtrlKeyDown;

	if (HitProxy)
	{
		if (HitProxy->IsA(HActor::StaticGetType()))
		{
			HActor* ActorHitProxy = static_cast<HActor*>(HitProxy);
			if (ActorHitProxy->Actor == RootActorProxy.Get())
			{
				if (ActorHitProxy->PrimComponent && ActorHitProxy->PrimComponent->IsA<UStaticMeshComponent>())
				{
					ADisplayClusterLightCardActor* TracedLightCard = TraceScreenForLightCard(View, HitX, HitY);
					SelectLightCard(TracedLightCard, bMultiSelect);
				}
			}
			else if (ActorHitProxy->Actor->IsA<ADisplayClusterLightCardActor>() && LightCardProxies.Contains(ActorHitProxy->Actor))
			{
				SelectLightCard(Cast<ADisplayClusterLightCardActor>(ActorHitProxy->Actor), bMultiSelect);
			}
			else if (!bMultiSelect)
			{
				SelectLightCard(nullptr);
			}
		}
	}
	else
	{
		SelectLightCard(nullptr);
	}
	
	PropagateLightCardSelection();

	FEditorViewportClient::ProcessClick(View, HitProxy, Key, Event, HitX, HitY);
}

EMouseCursor::Type FDisplayClusterLightCardEditorViewportClient::GetCursor(FViewport* InViewport, int32 X, int32 Y)
{
	EMouseCursor::Type MouseCursor = EMouseCursor::Default;

	if (RequiredCursorVisibiltyAndAppearance.bOverrideAppearance &&
		RequiredCursorVisibiltyAndAppearance.bHardwareCursorVisible)
	{
		 MouseCursor = RequiredCursorVisibiltyAndAppearance.RequiredCursor;
	}
	else if (!RequiredCursorVisibiltyAndAppearance.bHardwareCursorVisible)
	{
		MouseCursor = EMouseCursor::None;
	}
	else if (InViewport->IsCursorVisible() && !bWidgetAxisControlledByDrag)
	{
		EditorWidget->SetHighlightedAxis(EAxisList::Type::None);

		HHitProxy* HitProxy = InViewport->GetHitProxy(X,Y);
		if (HitProxy)
		{
			bShouldCheckHitProxy = true;

			if (HitProxy->IsA(HActor::StaticGetType()))
			{
				HActor* ActorHitProxy = static_cast<HActor*>(HitProxy);
				if (ActorHitProxy->Actor == RootActorProxy.Get())
				{
					if (ActorHitProxy->PrimComponent && ActorHitProxy->PrimComponent->IsA<UStaticMeshComponent>())
					{
						FSceneViewFamilyContext ViewFamily(FSceneViewFamily::ConstructionValues(
							InViewport,
							GetScene(),
							EngineShowFlags)
							.SetRealtimeUpdate(IsRealtime()));
						FSceneView* View = CalcSceneView(&ViewFamily);

						ADisplayClusterLightCardActor* TracedLightCard = TraceScreenForLightCard(*View, X, Y);
						if (TracedLightCard)
						{
							MouseCursor = EMouseCursor::Crosshairs;
						}
					}
				}
				else if (LightCardProxies.Contains(ActorHitProxy->Actor))
				{
					MouseCursor = EMouseCursor::Crosshairs;
				}
			}
			else if (HitProxy->IsA(HWidgetAxis::StaticGetType()))
			{
				HWidgetAxis* AxisHitProxy = static_cast<HWidgetAxis*>(HitProxy);
				
				MouseCursor = AxisHitProxy->GetMouseCursor();
				EditorWidget->SetHighlightedAxis(AxisHitProxy->Axis);
			}
		}
	}

	CachedMouseX = X;
	CachedMouseY = Y;

	return MouseCursor;
}

void FDisplayClusterLightCardEditorViewportClient::UpdatePreviewActor(ADisplayClusterRootActor* RootActor, bool bForce,
                                                                      EDisplayClusterLightCardEditorProxyType ProxyType)
{
	if (!bForce && RootActor == RootActorLevelInstance.Get())
	{
		return;
	}

	auto Finalize = [this]()
	{
		Viewport->InvalidateHitProxy();
		bShouldCheckHitProxy = true;
		InvalidateNormalMap();
	};
	
	if (RootActor == nullptr)
	{
		DestroyProxies(ProxyType);
		Finalize();
	}
	else
	{
		UWorld* PreviewWorld = PreviewScene->GetWorld();
		check(PreviewWorld);
		
		// Schedule for the next tick so CDO changes get propagated first in the event of config editor skeleton
		// regeneration & compiles. nDisplay's custom propagation may have issues if the archetype isn't correct.
		PreviewWorld->GetTimerManager().SetTimerForNextTick([=]()
		{
			TSet<AActor*> LastSelectedLightCardLevelInstances;
			for (TWeakObjectPtr<ADisplayClusterLightCardActor>& SelectedLightCard : SelectedLightCards)
			{
				if (SelectedLightCard.IsValid())
				{
					if (FLightCardProxy* FoundProxy = LightCardProxies.FindByKey(SelectedLightCard.Get()))
					{
						if (FoundProxy->LevelInstance.IsValid())
						{
							LastSelectedLightCardLevelInstances.Add(FoundProxy->LevelInstance.Get());
						}
					}
				}
			}
			
			DestroyProxies(ProxyType);
			RootActor->SubscribeToPostProcessRenderTarget(reinterpret_cast<uint8*>(this));
			RootActorLevelInstance = RootActor;
			
			if (ProxyType == EDisplayClusterLightCardEditorProxyType::All ||
				ProxyType == EDisplayClusterLightCardEditorProxyType::RootActor)
			{
				{
					FObjectDuplicationParameters DupeActorParameters(RootActor, PreviewWorld->GetCurrentLevel());
					DupeActorParameters.FlagMask = RF_AllFlags & ~(RF_ArchetypeObject | RF_Transactional); // Keeps archetypes correct in config data.
					DupeActorParameters.PortFlags = PPF_DuplicateVerbatim;
			
					RootActorProxy = CastChecked<ADisplayClusterRootActor>(StaticDuplicateObjectEx(DupeActorParameters));
				}
			
				PreviewWorld->GetCurrentLevel()->AddLoadedActor(RootActorProxy.Get());

				// Spawned actor will take the transform values from the template, so manually reset them to zero here
				RootActorProxy->SetActorLocation(FVector::ZeroVector);
				RootActorProxy->SetActorRotation(FRotator::ZeroRotator);

				FindProjectionOriginComponent();

				RootActorProxy->UpdatePreviewComponents();
				RootActorProxy->EnableEditorRender(false);

				RootActorBoundingRadius = 0.5f * RootActorProxy->GetComponentsBoundingBox().GetSize().Length();
			}

			// Filter out any primitives hidden in game except screen components
			MeshProjectionRenderer->AddActor(RootActorProxy.Get(), [](const UPrimitiveComponent* PrimitiveComponent)
			{
				return !PrimitiveComponent->bHiddenInGame || PrimitiveComponent->IsA<UDisplayClusterScreenComponent>();
			});

			if (ProxyType == EDisplayClusterLightCardEditorProxyType::All ||
				ProxyType == EDisplayClusterLightCardEditorProxyType::LightCards)
			{
				TArray<TWeakObjectPtr<ADisplayClusterLightCardActor>> LightCards;
				FindLightCardsForRootActor(RootActor, LightCards);

				SelectLightCard(nullptr);
				
				for (const TWeakObjectPtr<ADisplayClusterLightCardActor>& LightCard : LightCards)
				{
					FObjectDuplicationParameters DupeActorParameters(LightCard.Get(), PreviewWorld->GetCurrentLevel());
					DupeActorParameters.FlagMask = RF_AllFlags & ~(RF_ArchetypeObject | RF_Transactional);
					DupeActorParameters.PortFlags = PPF_DuplicateVerbatim;
				
					ADisplayClusterLightCardActor* LightCardProxy = CastChecked<ADisplayClusterLightCardActor>(StaticDuplicateObjectEx(DupeActorParameters));
					PreviewWorld->GetCurrentLevel()->AddLoadedActor(LightCardProxy);
				
					LightCardProxy->SetActorLocation(LightCard->GetActorLocation() - RootActor->GetActorLocation());
					LightCardProxy->SetActorRotation(LightCard->GetActorRotation() - RootActor->GetActorRotation());
					LightCardProxy->bIsProxy = true;

					LightCardProxies.Add(FLightCardProxy(LightCard.Get(), LightCardProxy));
					
					if (LastSelectedLightCardLevelInstances.Contains(LightCard.Get()))
					{
						SelectLightCard(LightCardProxy, true);
					}
				}
			}

			for (const FLightCardProxy& LightCardProxy : LightCardProxies)
			{
				MeshProjectionRenderer->AddActor(LightCardProxy.Proxy.Get());
			}

			Finalize();
		});
	}
}

void FDisplayClusterLightCardEditorViewportClient::UpdateProxyTransforms()
{
	if (RootActorLevelInstance.IsValid())
	{
		if (RootActorProxy.IsValid())
		{
			// Only update scale for the root actor.
			RootActorProxy->SetActorScale3D(RootActorLevelInstance->GetActorScale3D());
		}
		
		for (const FLightCardProxy& LightCardProxy : LightCardProxies)
		{
			if (LightCardProxy.LevelInstance.IsValid() && LightCardProxy.Proxy.IsValid())
			{
				LightCardProxy.Proxy->SetActorLocation(LightCardProxy.LevelInstance->GetActorLocation() - RootActorLevelInstance->GetActorLocation());
				LightCardProxy.Proxy->SetActorRotation(LightCardProxy.LevelInstance->GetActorRotation() - RootActorLevelInstance->GetActorRotation());
				LightCardProxy.Proxy->SetActorScale3D(LightCardProxy.LevelInstance->GetActorScale3D());
			}
		}
	}
}

void FDisplayClusterLightCardEditorViewportClient::DestroyProxies(
EDisplayClusterLightCardEditorProxyType ProxyType)
{
	MeshProjectionRenderer->ClearScene();

	UWorld* PreviewWorld = PreviewScene->GetWorld();
	check(PreviewWorld);
	
	if (ProxyType == EDisplayClusterLightCardEditorProxyType::All ||
		ProxyType == EDisplayClusterLightCardEditorProxyType::RootActor)
	{
		if (RootActorProxy.IsValid())
		{
			PreviewWorld->EditorDestroyActor(RootActorProxy.Get(), false);
			RootActorProxy.Reset();
		}

		if (RootActorLevelInstance.IsValid())
		{
			RootActorLevelInstance->UnsubscribeFromPostProcessRenderTarget(reinterpret_cast<uint8*>(this));
			RootActorLevelInstance.Reset();
		}
	}
	
	if (ProxyType == EDisplayClusterLightCardEditorProxyType::All ||
		ProxyType == EDisplayClusterLightCardEditorProxyType::LightCards)
	{
		for (const FLightCardProxy& LightCardProxy : LightCardProxies)
		{
			if (LightCardProxy.Proxy.IsValid())
			{
				PreviewWorld->EditorDestroyActor(LightCardProxy.Proxy.Get(), false);
			}
		}

		LightCardProxies.Empty();	
	}
}

void FDisplayClusterLightCardEditorViewportClient::SelectLightCards(const TArray<AActor*>& LightCardsToSelect)
{
	SelectLightCard(nullptr);
	for (AActor* LightCard : LightCardsToSelect)
	{
		if (FLightCardProxy* FoundProxy = LightCardProxies.FindByKey(LightCard))
		{
			if (FoundProxy->Proxy.IsValid())
			{
				SelectLightCard(FoundProxy->Proxy.Get(), true);
			}
		}
	}
}

void FDisplayClusterLightCardEditorViewportClient::SetProjectionMode(EDisplayClusterMeshProjectionType InProjectionMode)
{
	ProjectionMode = InProjectionMode;

	if (ProjectionMode == EDisplayClusterMeshProjectionType::Perspective)
	{
		// TODO: Do we want to cache the perspective rotation and restore it when the user switches back?
		SetViewRotation(FVector::ForwardVector.Rotation());
		EditorWidget->SetWidgetScale(1.f);
	}
	else if (ProjectionMode == EDisplayClusterMeshProjectionType::Azimuthal)
	{
		SetViewRotation(FVector::UpVector.Rotation());
		EditorWidget->SetWidgetScale(0.5f);
	}

	FindProjectionOriginComponent();

	if (Viewport)
	{
		Viewport->InvalidateHitProxy();
	}

	bShouldCheckHitProxy = true;
}

float FDisplayClusterLightCardEditorViewportClient::GetProjectionModeFOV(EDisplayClusterMeshProjectionType InProjectionMode) const
{
	int32 ProjectionModeIndex = (int32)InProjectionMode;
	if (ProjectionFOVs.Num() > ProjectionModeIndex)
	{
		return ProjectionFOVs[ProjectionModeIndex];
	}
	else
	{
		return ViewFOV;
	}
}

void FDisplayClusterLightCardEditorViewportClient::SetProjectionModeFOV(EDisplayClusterMeshProjectionType InProjectionMode, float NewFOV)
{
	int32 ProjectionModeIndex = (int32)InProjectionMode;
	if (ProjectionFOVs.Num() > ProjectionModeIndex)
	{
		ProjectionFOVs[ProjectionModeIndex] = NewFOV;
	}
	else
	{
		ViewFOV = NewFOV;
	}

	Viewport->InvalidateHitProxy();
	bShouldCheckHitProxy = true;
}

void FDisplayClusterLightCardEditorViewportClient::ResetCamera(bool bLocationOnly)
{
	FVector Location = FVector::ZeroVector;
	if (ProjectionOriginComponent.IsValid())
	{
		Location = ProjectionOriginComponent->GetComponentLocation();
	}

	SetViewLocation(Location);

	if (bLocationOnly)
	{
		return;
	}
	
	SetProjectionMode(GetProjectionMode());

	ResetFOVs();
}

void FDisplayClusterLightCardEditorViewportClient::BeginTransaction(const FText& Description)
{
	GEditor->BeginTransaction(Description);
}

void FDisplayClusterLightCardEditorViewportClient::EndTransaction()
{
	GEditor->EndTransaction();
}

void FDisplayClusterLightCardEditorViewportClient::GetScenePrimitiveComponents(TArray<UPrimitiveComponent*>& OutPrimitiveComponents)
{
	RootActorProxy->ForEachComponent<UPrimitiveComponent>(true, [&](UPrimitiveComponent* PrimitiveComponent)
	{
		OutPrimitiveComponents.Add(PrimitiveComponent);
	});
}

void FDisplayClusterLightCardEditorViewportClient::GetSceneViewInitOptions(FSceneViewInitOptions& OutViewInitOptions)
{
	FSceneViewInitOptions ViewInitOptions;

	FViewportCameraTransform& ViewTransform = GetViewTransform();

	ViewInitOptions.ViewLocation = ViewTransform.GetLocation();
	ViewInitOptions.ViewRotation = ViewTransform.GetRotation();
	ViewInitOptions.ViewOrigin = ViewInitOptions.ViewLocation;

	FIntPoint ViewportSize = Viewport->GetSizeXY();
	ViewportSize.X = FMath::Max(ViewportSize.X, 1);
	ViewportSize.Y = FMath::Max(ViewportSize.Y, 1);
	FIntPoint ViewportOffset(0, 0);

	ViewInitOptions.SetViewRectangle(FIntRect(ViewportOffset, ViewportOffset + ViewportSize));

	AWorldSettings* WorldSettings = nullptr;
	if (GetScene() != nullptr && GetScene()->GetWorld() != nullptr)
	{
		WorldSettings = GetScene()->GetWorld()->GetWorldSettings();
	}

	if (WorldSettings != nullptr)
	{
		ViewInitOptions.WorldToMetersScale = WorldSettings->WorldToMeters;
	}

	// Rotate view 90 degrees
	ViewInitOptions.ViewRotationMatrix = CalcViewRotationMatrix(ViewInitOptions.ViewRotation) * FMatrix(
		FPlane(0, 0, 1, 0),
		FPlane(1, 0, 0, 0),
		FPlane(0, 1, 0, 0),
		FPlane(0, 0, 0, 1));

	const float MinZ = GetNearClipPlane();
	const float MaxZ = MinZ;
	const float FieldOfView = GetProjectionModeFOV(ProjectionMode);

	// Avoid zero ViewFOV's which cause divide by zero's in projection matrix
	const float MatrixFOV = FMath::Max(0.001f, FieldOfView) * (float)PI / 360.0f;

	float XAxisMultiplier;
	float YAxisMultiplier;

	EAspectRatioAxisConstraint AspectRatioAxisConstraint = GetDefault<ULevelEditorViewportSettings>()->AspectRatioAxisConstraint;

	if (((ViewportSize.X > ViewportSize.Y) && (AspectRatioAxisConstraint == AspectRatio_MajorAxisFOV)) || (AspectRatioAxisConstraint == AspectRatio_MaintainXFOV))
	{
		//if the viewport is wider than it is tall
		XAxisMultiplier = 1.0f;
		YAxisMultiplier = ViewportSize.X / (float)ViewportSize.Y;
	}
	else
	{
		//if the viewport is taller than it is wide
		XAxisMultiplier = ViewportSize.Y / (float)ViewportSize.X;
		YAxisMultiplier = 1.0f;
	}

	if ((bool)ERHIZBuffer::IsInverted)
	{
		ViewInitOptions.ProjectionMatrix = FReversedZPerspectiveMatrix(
			MatrixFOV,
			MatrixFOV,
			XAxisMultiplier,
			YAxisMultiplier,
			MinZ,
			MaxZ
			);
	}
	else
	{
		ViewInitOptions.ProjectionMatrix = FPerspectiveMatrix(
			MatrixFOV,
			MatrixFOV,
			XAxisMultiplier,
			YAxisMultiplier,
			MinZ,
			MaxZ
			);
	}

	if (!ViewInitOptions.IsValidViewRectangle())
	{
		// Zero sized rects are invalid, so fake to 1x1 to avoid asserts later on
		ViewInitOptions.SetViewRectangle(FIntRect(0, 0, 1, 1));
	}

	ViewInitOptions.SceneViewStateInterface = ViewState.GetReference();
	ViewInitOptions.ViewElementDrawer = this;

	ViewInitOptions.BackgroundColor = GetBackgroundColor();

	ViewInitOptions.EditorViewBitflag = (uint64)1 << ViewIndex, // send the bit for this view - each actor will check it's visibility bits against this

	// for ortho views to steal perspective view origin
	ViewInitOptions.OverrideLODViewOrigin = FVector::ZeroVector;
	ViewInitOptions.bUseFauxOrthoViewPos = true;

	ViewInitOptions.FOV = FieldOfView;
	ViewInitOptions.OverrideFarClippingPlaneDistance = GetFarClipPlaneOverride();
	ViewInitOptions.CursorPos = CurrentMousePos;

	OutViewInitOptions = ViewInitOptions;
}

void FDisplayClusterLightCardEditorViewportClient::GetNormalMapSceneViewInitOptions(FIntPoint NormalMapSize, float NormalMapFOV, const FVector& ViewDirection, FSceneViewInitOptions& OutViewInitOptions)
{
	FViewportCameraTransform& ViewTransform = GetViewTransform();

	OutViewInitOptions.ViewLocation = ProjectionOriginComponent.IsValid() ? ProjectionOriginComponent->GetComponentLocation() : FVector::ZeroVector;
	OutViewInitOptions.ViewRotation = ViewDirection.Rotation();
	OutViewInitOptions.ViewOrigin = OutViewInitOptions.ViewLocation;

	OutViewInitOptions.SetViewRectangle(FIntRect(0, 0, NormalMapSize.X, NormalMapSize.Y));

	AWorldSettings* WorldSettings = nullptr;
	if (GetScene() != nullptr && GetScene()->GetWorld() != nullptr)
	{
		WorldSettings = GetScene()->GetWorld()->GetWorldSettings();
	}

	if (WorldSettings != nullptr)
	{
		OutViewInitOptions.WorldToMetersScale = WorldSettings->WorldToMeters;
	}

	// Rotate view 90 degrees
	OutViewInitOptions.ViewRotationMatrix = FInverseRotationMatrix(OutViewInitOptions.ViewRotation) * FMatrix(
		FPlane(0, 0, 1, 0),
		FPlane(1, 0, 0, 0),
		FPlane(0, 1, 0, 0),
		FPlane(0, 0, 0, 1));

	const float MinZ = GetNearClipPlane();
	const float MaxZ = FMath::Max(RootActorBoundingRadius, MinZ);

	// Avoid zero ViewFOV's which cause divide by zero's in projection matrix
	const float MatrixFOV = FMath::Max(0.001f, NormalMapFOV) * (float)PI / 360.0f;

	const float XAxisMultiplier = 1.0f;
	const float YAxisMultiplier = 1.0f;

	if ((bool)ERHIZBuffer::IsInverted)
	{
		OutViewInitOptions.ProjectionMatrix = FReversedZPerspectiveMatrix(
			MatrixFOV,
			MatrixFOV,
			XAxisMultiplier,
			YAxisMultiplier,
			MinZ,
			MaxZ);
	}
	else
	{
		OutViewInitOptions.ProjectionMatrix = FPerspectiveMatrix(
			MatrixFOV,
			MatrixFOV,
			XAxisMultiplier,
			YAxisMultiplier,
			MinZ,
			MaxZ);
	}

	OutViewInitOptions.SceneViewStateInterface = ViewState.GetReference();
	OutViewInitOptions.ViewElementDrawer = this;

	OutViewInitOptions.EditorViewBitflag = (uint64)1 << ViewIndex, // send the bit for this view - each actor will check it's visibility bits against this

	OutViewInitOptions.FOV = NormalMapFOV;
	OutViewInitOptions.OverrideFarClippingPlaneDistance = GetFarClipPlaneOverride();
}

UDisplayClusterConfigurationViewport* FDisplayClusterLightCardEditorViewportClient::FindViewportForPrimitiveComponent(UPrimitiveComponent* PrimitiveComponent)
{
	if (RootActorProxy.IsValid())
	{
		const FString PrimitiveComponentName = PrimitiveComponent->GetName();
		UDisplayClusterConfigurationData* Config = RootActorProxy->GetConfigData();
		
		for (const TPair<FString, UDisplayClusterConfigurationClusterNode*>& NodePair : Config->Cluster->Nodes)
		{
			const UDisplayClusterConfigurationClusterNode* Node = NodePair.Value;
			for (const TPair<FString, UDisplayClusterConfigurationViewport*>& ViewportPair : Node->Viewports)
			{
				UDisplayClusterConfigurationViewport* CfgViewport = ViewportPair.Value;

				FString ComponentName;
				if (CfgViewport->ProjectionPolicy.Type.Equals(DisplayClusterProjectionStrings::projection::Simple, ESearchCase::IgnoreCase)
					&& CfgViewport->ProjectionPolicy.Parameters.Contains(DisplayClusterProjectionStrings::cfg::simple::Screen))
				{
					ComponentName = CfgViewport->ProjectionPolicy.Parameters[DisplayClusterProjectionStrings::cfg::simple::Screen];
				}
				else if (CfgViewport->ProjectionPolicy.Type.Equals(DisplayClusterProjectionStrings::projection::Mesh, ESearchCase::IgnoreCase)
					&& CfgViewport->ProjectionPolicy.Parameters.Contains(DisplayClusterProjectionStrings::cfg::mesh::Component))
				{
					ComponentName = CfgViewport->ProjectionPolicy.Parameters[DisplayClusterProjectionStrings::cfg::mesh::Component];
				}

				if (ComponentName == PrimitiveComponentName)
				{
					return CfgViewport;
				}
			}
		}
	}

	return nullptr;
}

void FDisplayClusterLightCardEditorViewportClient::FindProjectionOriginComponent()
{
	if (RootActorProxy.IsValid())
	{
		TArray<UDisplayClusterCameraComponent*> ViewOriginComponents;
		RootActorProxy->GetComponents<UDisplayClusterCameraComponent>(ViewOriginComponents);

		if (ViewOriginComponents.Num())
		{
			ProjectionOriginComponent = ViewOriginComponents[0];
		}
		else
		{
			ProjectionOriginComponent = RootActorProxy->GetRootComponent();
		}
	}
	else
	{
		ProjectionOriginComponent.Reset();
	}
}

void FDisplayClusterLightCardEditorViewportClient::FindLightCardsForRootActor(ADisplayClusterRootActor* RootActor, TArray<TWeakObjectPtr<ADisplayClusterLightCardActor>>& OutLightCards)
{
	if (RootActor)
	{
		FDisplayClusterConfigurationICVFX_VisibilityList& RootActorLightCards = RootActor->GetConfigData()->StageSettings.Lightcard.ShowOnlyList;

		for (const TSoftObjectPtr<AActor>& LightCardActor : RootActorLightCards.Actors)
		{
			if (LightCardActor.IsValid() && LightCardActor->IsA<ADisplayClusterLightCardActor>())
			{
				OutLightCards.Add(Cast<ADisplayClusterLightCardActor>(LightCardActor.Get()));
			}
		}

		// If there are any layers that are specified as light card layers, iterate over all actors in the world and 
		// add any that are members of any of the light card layers to the list. Only add an actor once, even if it is
		// in multiple layers
		if (RootActorLightCards.ActorLayers.Num())
		{
			if (UWorld* World = RootActor->GetWorld())
			{
				for (const TWeakObjectPtr<AActor> WeakActor : FActorRange(World))
				{
					if (WeakActor.IsValid() && WeakActor->IsA<ADisplayClusterLightCardActor>())
					{
						for (const FActorLayer& ActorLayer : RootActorLightCards.ActorLayers)
						{
							if (WeakActor->Layers.Contains(ActorLayer.Name))
							{
								OutLightCards.Add(Cast<ADisplayClusterLightCardActor>(WeakActor));
								break;
							}
						}
					}
				}
			}
		}
	}
}

bool FDisplayClusterLightCardEditorViewportClient::IsLightCardSelected(const AActor* Actor)
{
	return SelectedLightCards.Contains(Actor);
}

void FDisplayClusterLightCardEditorViewportClient::SelectLightCard(ADisplayClusterLightCardActor* Actor, bool bAddToSelection)
{
	TArray<ADisplayClusterLightCardActor*> UpdatedActors;

	if (!bAddToSelection)
	{
		for (const TWeakObjectPtr<ADisplayClusterLightCardActor>& LightCard : SelectedLightCards)
		{
			if (LightCard.IsValid())
			{
				UpdatedActors.Add(LightCard.Get());
			}
		}

		SelectedLightCards.Empty();
	}

	if (Actor)
	{
		SelectedLightCards.Add(Actor);
		UpdatedActors.Add(Actor);
	}

	for (AActor* UpdatedActor : UpdatedActors)
	{
		UpdatedActor->PushSelectionToProxies();
	}
}

void FDisplayClusterLightCardEditorViewportClient::PropagateLightCardSelection()
{
	TArray<AActor*> SelectedLevelInstances;
	for (const TWeakObjectPtr<ADisplayClusterLightCardActor>& SelectedLightCard : SelectedLightCards)
	{
		if (FLightCardProxy* FoundProxy = LightCardProxies.FindByKey(SelectedLightCard.Get()))
		{
			if (FoundProxy->LevelInstance.IsValid())
			{
				SelectedLevelInstances.Add(FoundProxy->LevelInstance.Get());
			}
		}
	}

	LightCardEditorPtr.Pin()->SelectLightCards(SelectedLevelInstances);
}

void FDisplayClusterLightCardEditorViewportClient::PropagateLightCardTransform(ADisplayClusterLightCardActor* LightCardProxy)
{
	const FLightCardProxy* FoundProxy = LightCardProxies.FindByKey(LightCardProxy);
	if (FoundProxy && FoundProxy->Proxy == LightCardProxy && FoundProxy->LevelInstance.IsValid())
	{
		ADisplayClusterLightCardActor* LevelInstance = FoundProxy->LevelInstance.Get();

		LevelInstance->Modify();

		TArray<const FProperty*> ChangedProperties;
		
		// Set the level instance property value to our proxy property value.
		auto TryChangeProperty = [&](FName InPropertyName) -> void
		{
			const FProperty* Property = FindFProperty<FProperty>(LevelInstance->GetClass(), InPropertyName);
			check(Property);
			
			// Only change if values are different.
			if (!Property->Identical_InContainer(LightCardProxy, LevelInstance))
			{
				void* NewValue = nullptr;
				Property->GetValue_InContainer(LightCardProxy, &NewValue);
				Property->SetValue_InContainer(LevelInstance, &NewValue);

				ChangedProperties.Add(Property);
			}
		};
		
		TryChangeProperty(GET_MEMBER_NAME_CHECKED(ADisplayClusterLightCardActor, Longitude));
		TryChangeProperty(GET_MEMBER_NAME_CHECKED(ADisplayClusterLightCardActor, Latitude));
		TryChangeProperty(GET_MEMBER_NAME_CHECKED(ADisplayClusterLightCardActor, DistanceFromCenter));
		TryChangeProperty(GET_MEMBER_NAME_CHECKED(ADisplayClusterLightCardActor, Spin));
		TryChangeProperty(GET_MEMBER_NAME_CHECKED(ADisplayClusterLightCardActor, Pitch));
		TryChangeProperty(GET_MEMBER_NAME_CHECKED(ADisplayClusterLightCardActor, Yaw));
		
		const FVector RootActorLevelInstanceLocation = RootActorLevelInstance.IsValid() ? RootActorLevelInstance->GetActorLocation() : FVector::ZeroVector;
		LevelInstance->SetActorLocation(RootActorLevelInstanceLocation + LightCardProxy->GetActorLocation());

		// Snapshot the changed properties so multi-user can update while dragging.
		if (ChangedProperties.Num() > 0)
		{
			SnapshotTransactionBuffer(LevelInstance, MakeArrayView(ChangedProperties));
		}
	}
}

void FDisplayClusterLightCardEditorViewportClient::MoveSelectedLightCards(FViewport* InViewport, EAxisList::Type CurrentAxis)
{
	UWorld* PreviewWorld = PreviewScene->GetWorld();
	check(PreviewWorld);

	FIntPoint MousePos;
	InViewport->GetMousePos(MousePos);

	FSceneViewFamilyContext ViewFamily(FSceneViewFamily::ConstructionValues(
		InViewport,
		GetScene(),
		EngineShowFlags)
		.SetRealtimeUpdate(IsRealtime()));
	FSceneView* View = CalcSceneView(&ViewFamily);

	FVector Origin;
	FVector Direction;
	PixelToWorld(*View, MousePos, Origin, Direction);

	const FVector CursorRayStart = Origin;
	const FVector CursorRayEnd = CursorRayStart + Direction * HALF_WORLD_MAX;

	const TWeakObjectPtr<ADisplayClusterLightCardActor>& LastSelectedLightCard = SelectedLightCards.Last();
	if (LastSelectedLightCard.IsValid())
	{
		const FSphericalCoordinates DeltaCoords = GetLightCardTranslationDelta(InViewport, LastSelectedLightCard.Get(), CurrentAxis);

		for (const TWeakObjectPtr<ADisplayClusterLightCardActor>& LightCard : SelectedLightCards)
		{
			if (!LightCard.IsValid())
			{
				continue;
			}

			// Light cards should be centered on the current view origin, so set the light card position to match the current view origin. Update the light card
			// spherical coordinates to match its current coordinates
			if (ProjectionOriginComponent.IsValid() && ProjectionOriginComponent->GetComponentLocation() != LightCard->GetActorLocation())
			{
				const FVector DesiredLightCardOffset = LightCard->GetLightCardTransform().GetTranslation() - ProjectionOriginComponent->GetComponentLocation();

				LightCard->SetActorLocation(ProjectionOriginComponent->GetComponentLocation());

				FSphericalCoordinates SphericalCoords(DesiredLightCardOffset);

				LightCard->DistanceFromCenter = SphericalCoords.Radius;
				LightCard->Longitude = FMath::RadiansToDegrees(SphericalCoords.Radius) - 180;
				LightCard->Latitude = 90.f - FMath::RadiansToDegrees(SphericalCoords.Radius);
			}

			const FSphericalCoordinates CurrentCoords = GetLightCardCoordinates(LightCard.Get());

			FSphericalCoordinates NewCoords;
			NewCoords.Radius = CurrentCoords.Radius + DeltaCoords.Radius;
			NewCoords.Azimuth = CurrentCoords.Azimuth + DeltaCoords.Azimuth;
			NewCoords.Inclination = CurrentCoords.Inclination + DeltaCoords.Inclination;

			LightCard->DistanceFromCenter = NewCoords.Radius;
			LightCard->Longitude = FRotator::ClampAxis(FMath::RadiansToDegrees(NewCoords.Azimuth) - 180);
			LightCard->Latitude = 90.f - FMath::RadiansToDegrees(NewCoords.Inclination);

			{
				const FVector LightCardPosition = LightCard->GetLightCardTransform().GetTranslation();

				FVector DesiredNormal;
				float DesiredDistance;

				// If the light card is in the southern hemisphere of the view origin, use the southern normal map; otherwise, use the north normal map
				if (LightCardPosition.Z < Origin.Z)
				{
					SouthNormalMap.GetNormalAndDistanceAtPosition(LightCardPosition, DesiredNormal, DesiredDistance);
				}
				else
				{
					NorthNormalMap.GetNormalAndDistanceAtPosition(LightCardPosition, DesiredNormal, DesiredDistance);
				}

				const FRotator Rotation = FRotationMatrix::MakeFromX(-DesiredNormal).Rotator();

				LightCard->Pitch = Rotation.Pitch;
				LightCard->Yaw = Rotation.Yaw;
				LightCard->DistanceFromCenter = FMath::Min(DesiredDistance, RootActorBoundingRadius) + LightCardFlushOffset;
			}

			PropagateLightCardTransform(LightCard.Get());
		}

		FVector LightCardWorldLocation = CachedEditorWidgetTransformAfterMapProjection.GetTranslation();
		FVector2D ScreenPercentage;
		if (IsLocationCloseToEdge(LightCardWorldLocation, InViewport, View, &ScreenPercentage))
		{
			DesiredLookAtSpeed = FMath::Max(ScreenPercentage.X, ScreenPercentage.Y) * MaxDesiredLookAtSpeed;
			DesiredLookAtLocation = LightCardWorldLocation;
		}
		else
		{
			DesiredLookAtLocation.Reset();
		}
	}
}

FDisplayClusterLightCardEditorViewportClient::FSphericalCoordinates FDisplayClusterLightCardEditorViewportClient::GetLightCardTranslationDelta(
	FViewport* InViewport, 
	ADisplayClusterLightCardActor* LightCard,
	EAxisList::Type CurrentAxis)
{
	FIntPoint MousePos;
	InViewport->GetMousePos(MousePos);

	FSceneViewFamilyContext ViewFamily(FSceneViewFamily::ConstructionValues(
		InViewport,
		GetScene(),
		EngineShowFlags)
		.SetRealtimeUpdate(IsRealtime()));
	FSceneView* View = CalcSceneView(&ViewFamily);

	FVector Origin;
	FVector Direction;
	PixelToWorld(*View, MousePos, Origin, Direction);

	Direction = (Direction - DragWidgetOffset).GetSafeNormal();

	const FVector LocalDirection = LightCard->GetActorRotation().RotateVector(Direction);
	const FVector LightCardLocation = LightCard->GetLightCardTransform().GetTranslation() - Origin;

	FVector Normal;
	float Distance;

	// If the light card is in the southern hemisphere of the view origin, use the southern normal map; otherwise, use the north normal map
	if (LightCardLocation.Z < 0.0f)
	{
		SouthNormalMap.GetNormalAndDistanceAtPosition(LightCard->GetLightCardTransform().GetTranslation(), Normal, Distance);
	}
	else
	{
		NorthNormalMap.GetNormalAndDistanceAtPosition(LightCard->GetLightCardTransform().GetTranslation(), Normal, Distance);
	}

	FSphericalCoordinates LightCardCoords = GetLightCardCoordinates(LightCard);
	FSphericalCoordinates RequestedCoords(LocalDirection * Distance);
	
	FSphericalCoordinates DeltaCoords;
	DeltaCoords.Radius = RequestedCoords.Radius - LightCardCoords.Radius;
	DeltaCoords.Azimuth = RequestedCoords.Azimuth - LightCardCoords.Azimuth;
	DeltaCoords.Inclination = RequestedCoords.Inclination - LightCardCoords.Inclination;

	if (CurrentAxis == EAxisList::Type::X)
	{
		DeltaCoords.Inclination = 0;
	}
	else if (CurrentAxis == EAxisList::Type::Y)
	{
		// Convert the inclination to Cartesian coordinates, project it to the x-z plane, and convert back to spherical coordinates. This ensures that the motion in the inclination
		// plane always lines up with the mouse's projected location along that plane
		float FixedInclination = FMath::Abs(FMath::Atan2(FMath::Cos(DeltaCoords.Azimuth) * FMath::Sin(RequestedCoords.Inclination), FMath::Cos(RequestedCoords.Inclination)));

		// When translating along the inclination axis, the azimuth delta can only be intervals of pi
		float FixedAzimuth = FMath::RoundToInt(DeltaCoords.Azimuth / PI) * PI;

		DeltaCoords.Azimuth = FixedAzimuth;
		DeltaCoords.Inclination = FixedInclination - LightCardCoords.Inclination;
	}

	return DeltaCoords;
}

FDisplayClusterLightCardEditorViewportClient::FSphericalCoordinates FDisplayClusterLightCardEditorViewportClient::GetLightCardCoordinates(ADisplayClusterLightCardActor* LightCard) const
{
	const FVector LightCardLocation = LightCard->GetLightCardTransform().GetTranslation() - LightCard->GetActorLocation();

	FSphericalCoordinates LightCardCoords(LightCardLocation);

	// If the light card inclination is 0 or 180, the spherical coordinates will have an
	// "undefined" azimuth value. For continuity when dragging a light card positioned there, we can manually
	// set the azimuthal value to match the light card's configured longitude
	if (LightCardCoords.Inclination == 0.f ||LightCardCoords.Inclination == PI)
	{
		LightCardCoords.Azimuth = FMath::DegreesToRadians(LightCard->Longitude + 180);
	}

	return LightCardCoords;
}

ADisplayClusterLightCardActor* FDisplayClusterLightCardEditorViewportClient::TraceScreenForLightCard(const FSceneView& View, int32 HitX, int32 HitY)
{
	UWorld* PreviewWorld = PreviewScene->GetWorld();
	check(PreviewWorld);

	FVector Origin;
	FVector Direction;
	PixelToWorld(View, FIntPoint(HitX, HitY), Origin, Direction);

	const FVector CursorRayStart = Origin;
	const FVector CursorRayEnd = CursorRayStart + Direction * HALF_WORLD_MAX;

	FCollisionQueryParams Param(SCENE_QUERY_STAT(DragDropTrace), true);

	bool bHitLightCard = false;
	FHitResult ScreenHitResult;
	if (PreviewWorld->LineTraceSingleByObjectType(ScreenHitResult, CursorRayStart, CursorRayEnd, FCollisionObjectQueryParams(FCollisionObjectQueryParams::InitType::AllObjects), Param))
	{
		if (AActor* HitActor = ScreenHitResult.GetActor())
		{
			if (RootActorProxy.Get() == HitActor && ScreenHitResult.Component.IsValid())
			{
				if (UDisplayClusterConfigurationViewport* CfgViewport = FindViewportForPrimitiveComponent(ScreenHitResult.Component.Get()))
				{
					FString ViewOriginName = CfgViewport->Camera;
					UDisplayClusterCameraComponent* ViewOrigin = nullptr;

					// If the view origin name is empty, use the first found view origin in the root actor
					if (ViewOriginName.IsEmpty())
					{
						ViewOrigin = RootActorProxy->GetDefaultCamera();
					}
					else
					{
						ViewOrigin = RootActorProxy->GetComponentByName<UDisplayClusterCameraComponent>(ViewOriginName);
					}

					if (ViewOrigin)
					{
						const FVector ViewOriginRayStart = ViewOrigin->GetComponentLocation();
						const FVector ViewOriginRayEnd = ViewOriginRayStart + (ScreenHitResult.Location - ViewOriginRayStart) * HALF_WORLD_MAX;

						TArray<FHitResult> HitResults;
						if (PreviewWorld->LineTraceMultiByObjectType(HitResults, ViewOriginRayStart, ViewOriginRayEnd, FCollisionObjectQueryParams(FCollisionObjectQueryParams::InitType::AllObjects), Param))
						{
							for (FHitResult& HitResult : HitResults)
							{
								if (ADisplayClusterLightCardActor* LightCardActor = Cast<ADisplayClusterLightCardActor>(HitResult.GetActor()))
								{
									if (LightCardProxies.Contains(LightCardActor))
									{
										return LightCardActor;
									}
								}
							}
						}
					}
				}
			}
			else if (HitActor->IsA<ADisplayClusterLightCardActor>() && LightCardProxies.Contains(HitActor))
			{
				return Cast<ADisplayClusterLightCardActor>(HitActor);
			}
		}
	}

	return nullptr;
}

void FDisplayClusterLightCardEditorViewportClient::PixelToWorld(const FSceneView& View, const FIntPoint& PixelPos, FVector& OutOrigin, FVector& OutDirection)
{
	const FMatrix& InvProjMatrix = View.ViewMatrices.GetInvProjectionMatrix();
	const FMatrix& InvViewMatrix = View.ViewMatrices.GetInvViewMatrix();

	const FVector4 ScreenPos = View.PixelToScreen(PixelPos.X, PixelPos.Y, 0);
	const FVector ViewPos = FVector(InvProjMatrix.TransformFVector4(FVector4(ScreenPos.X * GNearClippingPlane, ScreenPos.Y * GNearClippingPlane, 0.0f, GNearClippingPlane)));
	const FVector UnprojectedViewPos = FDisplayClusterMeshProjectionRenderer::UnprojectViewPosition(ViewPos, ProjectionMode);

	OutOrigin = View.ViewMatrices.GetViewOrigin();
	OutDirection = InvViewMatrix.TransformVector(UnprojectedViewPos).GetSafeNormal();
}

bool FDisplayClusterLightCardEditorViewportClient::CalcEditorWidgetTransform(FTransform& WidgetTransformBeforeMapProjection, FTransform& WidgetTransformAfterMapProjection)
{
	if (!SelectedLightCards.Num())
	{
		return false;
	}

	TWeakObjectPtr<ADisplayClusterLightCardActor> LastSelected = SelectedLightCards.Last();

	if (!LastSelected.IsValid())
	{
		return false;
	}

	FVector LightCardPosition = LastSelected->GetLightCardTransform().GetTranslation();

	WidgetTransformAfterMapProjection = WidgetTransformBeforeMapProjection = FTransform(FRotator::ZeroRotator, LightCardPosition, FVector::OneVector);

	if (ProjectionMode != EDisplayClusterMeshProjectionType::Perspective)
	{
		FSceneViewInitOptions SceneVewInitOptions;
		GetSceneViewInitOptions(SceneVewInitOptions);
		FViewMatrices ViewMatrices(SceneVewInitOptions);

		const FVector ViewPos = ViewMatrices.GetViewMatrix().TransformPosition(LightCardPosition);
		const FVector ProjectedViewPos = FDisplayClusterMeshProjectionRenderer::ProjectViewPosition(ViewPos, ProjectionMode);
		const FVector ProjectedPosition = ViewMatrices.GetInvViewMatrix().TransformPosition(ProjectedViewPos);

		WidgetTransformAfterMapProjection.SetTranslation(ProjectedPosition);
	}

	const FVector ProjectionOrigin = ProjectionOriginComponent.IsValid() ? ProjectionOriginComponent->GetComponentLocation() : FVector::ZeroVector;
	const FVector RadialVector = (LightCardPosition - ProjectionOrigin).GetSafeNormal();
	const FVector AzimuthalVector = (FVector::ZAxisVector ^ RadialVector).GetSafeNormal();
	const FVector InclinationVector = RadialVector ^ AzimuthalVector;

	FRotator Orientation = FMatrix(AzimuthalVector, InclinationVector, RadialVector, FVector::ZeroVector).Rotator();

	WidgetTransformBeforeMapProjection.SetRotation(Orientation.Quaternion());
	WidgetTransformAfterMapProjection.SetRotation(Orientation.Quaternion());

	return true;
}

void FDisplayClusterLightCardEditorViewportClient::RenderNormalMap(FNormalMap& NormalMap, const FVector& NormalMapDirection)
{
	FSceneViewInitOptions ViewInitOptions;
	GetNormalMapSceneViewInitOptions(FIntPoint(FNormalMap::NormalMapSize), FNormalMap::NormalMapFOV, NormalMapDirection, ViewInitOptions);

	NormalMap.Init(ViewInitOptions);

	// Only render primitive components from the stage actor for the normal map
	FDisplayClusterMeshProjectionPrimitiveFilter PrimitiveFilter;
	PrimitiveFilter.PrimitiveFilterDelegate = FDisplayClusterMeshProjectionPrimitiveFilter::FPrimitiveFilter::CreateLambda([this](const UPrimitiveComponent* PrimitiveComponent)
	{
		return PrimitiveComponent->GetOwner() == RootActorProxy;
	});

	FCanvas Canvas(&NormalMap, nullptr, GetWorld(), GetScene()->GetFeatureLevel(), FCanvas::CDM_DeferDrawing, 1.0f);
	{
		Canvas.Clear(FLinearColor::Black);

		MeshProjectionRenderer->RenderNormals(&Canvas, GetScene(), ViewInitOptions, EngineShowFlags, EDisplayClusterMeshProjectionType::Azimuthal, &PrimitiveFilter);
	}
	Canvas.Flush_GameThread();

	NormalMap.ReadFloat16Pixels(NormalMap.GetCachedNormalData());
	NormalMap.Release();

	FlushRenderingCommands();
}

void FDisplayClusterLightCardEditorViewportClient::InvalidateNormalMap()
{
	bNormalMapInvalid = true;
}

bool FDisplayClusterLightCardEditorViewportClient::IsLocationCloseToEdge(const FVector& InPosition, const FViewport* InViewport,
                                                                         const FSceneView* InView, FVector2D* OutPercentageToEdge)
{
	if (InViewport == nullptr)
	{
		InViewport = Viewport;
	}

	check(InViewport);
	const FIntPoint ViewportSize = InViewport->GetSizeXY();

	FPlane Projection;
	if (InView == nullptr)
	{
		FSceneViewFamilyContext ViewFamily(FSceneViewFamily::ConstructionValues(
		Viewport,
		GetScene(),
		EngineShowFlags)
		.SetRealtimeUpdate(IsRealtime()));
		
		InView = CalcSceneView(&ViewFamily);
		Projection = InView->Project(InPosition);

		// InView will be deleted here
	}
	else
	{
		Projection = InView->Project(InPosition);
	}
	
	if (Projection.W > 0)
	{
		const float HighThreshold = 1.f - EdgePercentageLookAtThreshold;
		
		const int32 HalfX = 0.5f * Viewport->GetSizeXY().X;
		const int32 HalfY = 0.5f * Viewport->GetSizeXY().Y;
		const int32 XPos = HalfX + (HalfX * Projection.X);
		const int32 YPos = HalfY + (HalfY * (Projection.Y * -1));
			
		auto GetPercentToEdge = [&](int32 CurrentPos, int32 MaxPos) -> float
		{
			const float Center = static_cast<float>(MaxPos) / 2.f;
			const float RelativePosition = FMath::Abs(CurrentPos - Center);
			return RelativePosition / Center;
		};
			
		const float XPercent = GetPercentToEdge(XPos, ViewportSize.X);
		const float YPercent = GetPercentToEdge(YPos, ViewportSize.Y);
			
		if (OutPercentageToEdge)
		{
			*OutPercentageToEdge = FVector2D(XPercent, YPercent);
		}
			
		return XPercent >= HighThreshold || YPercent >= HighThreshold;
	}

	return false;
}

void FDisplayClusterLightCardEditorViewportClient::ResetFOVs()
{
	constexpr int32 MaxFOVs = 2;
	if (ProjectionFOVs.Num() < MaxFOVs)
	{
		ProjectionFOVs.AddDefaulted(MaxFOVs - ProjectionFOVs.Num());
	}
	ProjectionFOVs[static_cast<int32>(EDisplayClusterMeshProjectionType::Perspective)] = 90.0f;
	ProjectionFOVs[static_cast<int32>(EDisplayClusterMeshProjectionType::Azimuthal)] = 130.0f;
}

#undef LOCTEXT_NAMESPACE
