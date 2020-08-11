// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "IDetailCustomization.h"
#include "Widgets/Text/STextBlock.h"
#include "Input/Reply.h"

/** UI customization for URuntimeVirtualTexture */
class FRuntimeVirtualTextureDetailsCustomization : public IDetailCustomization
{
public:
	static TSharedRef<IDetailCustomization> MakeInstance();

protected:
	FRuntimeVirtualTextureDetailsCustomization();

	/** Callback for updating values after and edit. */
	void RefreshDetails();

	//~ Begin IDetailCustomization Interface.
	virtual void CustomizeDetails(IDetailLayoutBuilder& DetailBuilder) override;
	//~ End IDetailCustomization Interface.

private:
	class URuntimeVirtualTexture* VirtualTexture;

	TSharedPtr<class STextBlock> TileCountText;
	TSharedPtr<class STextBlock> TileSizeText;
	TSharedPtr<class STextBlock> TileBorderSizeText;

	TSharedPtr<class STextBlock> SizeText;
	TSharedPtr<class STextBlock> PageTableTextureMemoryText;
	TSharedPtr<class STextBlock> PhysicalTextureMemoryText;
};


/** UI customization for URuntimeVirtualTextureComponent */
class FRuntimeVirtualTextureComponentDetailsCustomization : public IDetailCustomization
{
public:
	static TSharedRef<IDetailCustomization> MakeInstance();

protected:
	FRuntimeVirtualTextureComponentDetailsCustomization();

	/** Callback for Set Bounds button */
	FReply SetBounds();

	/** Callback for Build button */
	FReply BuildStreamedMips();
	/** Callback for Build Debug button */
	FReply BuildLowMipsDebug();
	/** Shared implementation for Build callbacks */
	FReply BuildStreamedMipsInternal(bool bDebug);

	/** Returns true if MinMax texture build button is enabled */
	bool IsMinMaxTextureEnabled() const;
	/** Callback for Build MinMax Texture button */
	FReply BuildMinMaxTexture();

	//~ Begin IDetailCustomization Interface.
	virtual void CustomizeDetails(IDetailLayoutBuilder& DetailBuilder) override;
	//~ End IDetailCustomization Interface.

private:
	class URuntimeVirtualTextureComponent* RuntimeVirtualTextureComponent;
};
