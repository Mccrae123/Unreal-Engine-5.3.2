// Copyright Epic Games, Inc. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "Modules/ModuleInterface.h"
#include "Templates/SharedPointer.h"

class IPropertyHandle;
class IDetailCategoryBuilder;
class IDetailLayoutBuilder;
class URemoteControlPreset;

DECLARE_MULTICAST_DELEGATE_OneParam(FOnGenerateExtensions, TArray<TSharedRef<class SWidget>>& /*OutExtensions*/);

/**
 * Filter queried in order to determine if a property should be displayed.
 */
DECLARE_DELEGATE_RetVal_OneParam(bool, FOnDisplayExposeIcon, TSharedRef<IPropertyHandle> /*PropertyHandle*/);

/**
 * Callback called to customize the display of a metadata entry for entities.
 */
DECLARE_DELEGATE_FourParams(FOnCustomizeMetadataEntry, URemoteControlPreset* /*Preset*/, const FGuid& /*DisplayedEntityId*/, IDetailLayoutBuilder& /*LayoutBuilder*/, IDetailCategoryBuilder& /*CategoryBuilder*/);

/**
 * A Remote Control module that allows exposing objects and properties from the editor.
 */
class IRemoteControlUIModule : public IModuleInterface
{
public:
	/** 
	 * Get the toolbar extension generators.
	 * Usage: Bind a handler that adds a widget to the out array parameter.
	 */
	virtual FOnGenerateExtensions& GetExtensionGenerators() = 0;

	/**
	 * Add a property filter that indicates if the property handle should be displayed or not.
	 * When queried, returning true will allow the expose icon to be displayed in the details panel, false will hide it.
	 * @Note This filter will be queried after the RemoteControlModule's own filters.
	 * @param OnDisplayExposeIcon The delegate called to determine whether to display the icon or not.
	 * @return A handle to the delegate, used to unregister the delegate with the module.
	 */
	virtual FDelegateHandle AddPropertyFilter(FOnDisplayExposeIcon OnDisplayExposeIcon) = 0;

	/**
	 * Remove a property filter using its id.
	 */
	virtual void RemovePropertyFilter(const FDelegateHandle& FilterDelegateHandle) = 0;

	/**
	 * Register a delegate to customize how an entry is displayed in the entity details panel.
	 * @param MetadataKey The metadata map entry to customize.
	 * @param OnCustomizeCallback The handler called to handle customization for the entry's details panel row.
	 */
	virtual void RegisterMetadataCustomization(FName MetadataKey, FOnCustomizeMetadataEntry OnCustomizeCallback) = 0;

	/**
     * Unregister the delegate used to customize how an entry is displayed in the entity details panel.
     * @param MetadataKey The metadata map entry to unregister the customization for.
     */
	virtual void UnregisterMetadataCustomization(FName MetadataKey) = 0;
};
