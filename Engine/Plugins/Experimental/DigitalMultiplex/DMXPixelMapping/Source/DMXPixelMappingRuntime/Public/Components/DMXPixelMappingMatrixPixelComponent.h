// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "Components/DMXPixelMappingOutputDMXComponent.h"
#include "DMXProtocolTypes.h"
#include "Library/DMXEntityReference.h"
#include "DMXPixelMappingMatrixPixelComponent.generated.h"

class SUniformGridPanel;
class UTextureRenderTarget2D;
class FProperty;
enum class EDMXColorMode : uint8;

/**
 * Matrix pixel component
 */
UCLASS()
class DMXPIXELMAPPINGRUNTIME_API UDMXPixelMappingMatrixPixelComponent
	: public UDMXPixelMappingOutputDMXComponent
{
	GENERATED_BODY()

public:
	/** Default Constructor */
	UDMXPixelMappingMatrixPixelComponent();

	//~ Begin UObject implementation
	virtual void PostLoad() override;

#if WITH_EDITOR
	virtual void PostEditChangeChainProperty(FPropertyChangedChainEvent& PropertyChangedChainEvent) override;

	virtual bool CanEditChange(const FProperty* InProperty) const override;
	//~ End UObject implementation
#endif // WITH_EDITOR

	//~ Begin UDMXPixelMappingBaseComponent implementation
	virtual const FName& GetNamePrefix() override;
	virtual void ResetDMX() override;
	virtual void SendDMX() override;
	virtual void Render() override;
	virtual void RenderAndSendDMX() override;
	virtual void PostParentAssigned() override;
	//~ End UDMXPixelMappingBaseComponent implementation

	//~ Begin UDMXPixelMappingOutputComponent implementation
#if WITH_EDITOR
	virtual TSharedRef<SWidget> BuildSlot(TSharedRef<SCanvas> InCanvas) override;
	virtual void ToggleHighlightSelection(bool bIsSelected) override;

	virtual bool IsVisibleInDesigner() const override;

	virtual void UpdateWidget() override;

	virtual bool IsLockInDesigner() const override;
#endif // WITH_EDITOR	
	virtual UTextureRenderTarget2D* GetOutputTexture() override;
	virtual FVector2D GetSize() override;
	virtual FVector2D GetPosition() override;
	virtual void SetPosition(const FVector2D& InPosition) override;
	virtual void SetSize(const FVector2D& InSize) override;
	//~ End UDMXPixelMappingOutputComponent implementation

	//~ Begin UDMXPixelMappingOutputDMXComponent implementation
	virtual void RenderWithInputAndSendDMX() override;
	virtual void RendererOutputTexture() override;
	//~ End UDMXPixelMappingOutputDMXComponent implementation

	void SetPositionFromParent(const FVector2D& InPosition);
	void SetSizeFromParent(const FVector2D& InSize);

	void SetPixelCoordinate(FIntPoint InPixelCoordinate) { PixelCoordinate = InPixelCoordinate; }
	const FIntPoint& GetPixelCoordinate() { return PixelCoordinate; }

	/** Check if a Component can be moved under another one (used for copy/move/duplicate) */
	virtual bool CanBeMovedTo(const UDMXPixelMappingBaseComponent* Component) const override;

private:
	void SetPositionInBoundaryBox(const FVector2D& InPosition);

	void SetSizeWithinBoundaryBox(const FVector2D& InSize);

public:
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Pixel Settings")
	int32 PixelIndex;

	UPROPERTY()
	FDMXEntityFixturePatchRef FixturePatchMatrixRef;

private:
	UPROPERTY(Transient)
	UTextureRenderTarget2D* OutputTarget;

	UPROPERTY()
	FIntPoint PixelCoordinate;

#if WITH_EDITORONLY_DATA
	FSlateBrush Brush;
#endif

private:
	static const FVector2D MixPixelSize;
};
