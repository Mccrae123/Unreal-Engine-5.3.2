// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "UObject/SoftObjectPtr.h"
#include "UObject/WeakObjectPtr.h"
#include "Misc/BlacklistNames.h"

/** Struct, OwnerName */
DECLARE_MULTICAST_DELEGATE_TwoParams(FWhitelistUpdated, TSoftObjectPtr<UStruct>, FName);

/**
 * A hierarchical set of rules that can be used to whitelist all properties of specific Structs without
 * having to manually add every single property in those Structs. These rules are applied in order from
 * the base Struct to the leaf Struct. UseExistingWhitelist has dual-functionality to alternatively
 * inherit the parent Struct's rule if no whitelist is manually defined.
 * 
 * For example, if you have:
 * class A - (UseExistingWhitelist "MyProp")						Whitelist = "MyProp"
 * class B : public class A - (WhitelistAllProperties)				Whitelist = "MyProp","PropA1","PropA2"
 * class C : public class B - (UseExistingWhitelist "AnotherProp")	Whitelist = "MyProp","PropA1","PropA2","AnotherProp"
 * class D : public class B - (UseExistingWhitelist)				Whitelist = "MyProp","PropA1","PropA2","PropD1","PropD2"
 * Note that because class C manually defines a whitelist, it does not inherit the WhitelistAllProperties rule from class B, while
 * class D does not define a whitelist, so it does inherit the rule, causing all of class D's properties to also get added to the whitelist.
 */
enum class EPropertyEditorWhitelistRules : uint8
{
	// If a whitelist is manually defined for this struct, whitelist those properties. Otherwise, use the parent Struct's rule.
	UseExistingWhitelist,
	// If no whitelist is manually defined for this Struct, whitelist all properties from this Struct and its subclasses
	WhitelistAllProperties,
	// If a whitelist is manually defined for this Struct, whitelist all properties from this Struct's subclasses.
	// If this functionality is needed without any properties to whitelist, a fake property must be whitelisted instead.
	WhitelistAllSubclassProperties
};

struct FPropertyEditorWhitelistEntry
{
    FBlacklistNames Whitelist;
    EPropertyEditorWhitelistRules Rules = EPropertyEditorWhitelistRules::UseExistingWhitelist;
};

class PROPERTYEDITOR_API FPropertyEditorWhitelist
{
public:
	static FPropertyEditorWhitelist& Get()
	{
		static FPropertyEditorWhitelist Whitelist;
		return Whitelist;
	}

	/** Add a set of rules for a specific base UStruct to determine which properties are visible in all details panels */
	void AddWhitelist(TSoftObjectPtr<UStruct> Struct, const FBlacklistNames& Whitelist, EPropertyEditorWhitelistRules Rules = EPropertyEditorWhitelistRules::UseExistingWhitelist);
	/** Remove a set of rules for a specific base UStruct to determine which properties are visible in all details panels */
	void RemoveWhitelist(TSoftObjectPtr<UStruct> Struct);
	/** Remove all rules */
	void ClearWhitelist();

	/** Add a specific property to a UStruct's whitelist */
	void AddToWhitelist(TSoftObjectPtr<UStruct> Struct, const FName PropertyName, const FName Owner = NAME_None);
	/** Remove a specific property from a UStruct's whitelist */
	void RemoveFromWhitelist(TSoftObjectPtr<UStruct> Struct, const FName PropertyName, const FName Owner = NAME_None);
	/** Add a specific property to a UStruct's blacklist */
	void AddToBlacklist(TSoftObjectPtr<UStruct> Struct, const FName PropertyName, const FName Owner = NAME_None);
	/** Remove a specific property from a UStruct's blacklist */
    void RemoveFromBlacklist(TSoftObjectPtr<UStruct> Struct, const FName PropertyName, const FName Owner = NAME_None);

	/** When the whitelist or blacklist for any struct was added to or removed from. */
    FWhitelistUpdated WhitelistUpdatedDelegate;
    
	/** When the entire whitelist is enabled or disabled */
	FSimpleMulticastDelegate WhitelistEnabledDelegate;

	/** Controls whether DoesPropertyPassFilter always returns true or performs property-based filtering. */
	bool IsEnabled() const { return bEnablePropertyEditorWhitelist; }
	/** Turn on or off the property editor whitelist. DoesPropertyPassFilter will always return true if disabled. */
	void SetEnabled(bool bEnable);

	/** Whether the Details View should show special menu entries to add/remove items in the whitelist */
	bool ShouldShowMenuEntries() const { return bShouldShowMenuEntries;}
	/** Turn on or off menu entries to modify the whitelist from a Details View */
	void SetShouldShowMenuEntries(bool bShow) { bShouldShowMenuEntries = bShow; }

	/**
	 * Checks if a property passes the whitelist/blacklist filtering specified by PropertyEditorWhitelists
	 * This should be relatively fast as it maintains a flattened cache of all inherited whitelists for every UStruct (which is generated lazily).
	 */
	bool DoesPropertyPassFilter(const UStruct* ObjectStruct, FName PropertyName) const;

	/** Check whether a property exists on the whitelist for a specific Struct - this will return false if the property is whitelisted on a parent Struct */
	bool IsSpecificPropertyWhitelisted(const UStruct* ObjectStruct, FName PropertyName) const;
	/** Check whether a property exists on the blacklist for a specific Struct - this will return false if the property is blacklisted on a parent Struct */
	bool IsSpecificPropertyBlacklisted(const UStruct* ObjectStruct, FName PropertyName) const;

	/** Gets a read-only copy of the original, un-flattened whitelist. */
	const TMap<TSoftObjectPtr<UStruct>, FPropertyEditorWhitelistEntry>& GetRawWhitelist() const { return RawPropertyEditorWhitelist; }

private:
	/** Whether DoesPropertyPassFilter should perform its whitelist check or always return true */
	bool bEnablePropertyEditorWhitelist = false;
	/** Whether SDetailSingleItemRow should add menu items to add/remove properties to/from the whitelist */
	bool bShouldShowMenuEntries = false;
	
	/** Stores assigned whitelists from AddWhitelist(), which are later flattened and stored in CachedPropertyEditorWhitelist. */
	TMap<TSoftObjectPtr<UStruct>, FPropertyEditorWhitelistEntry> RawPropertyEditorWhitelist;

	/** Lazily-constructed combined cache of both the flattened class whitelist and struct whitelist */
	mutable TMap<TWeakObjectPtr<const UStruct>, FBlacklistNames> CachedPropertyEditorWhitelist;

	/** Get or create the cached whitelist for a specific UStruct */
	const FBlacklistNames& GetCachedWhitelistForStruct(const UStruct* Struct) const;
	const FBlacklistNames& GetCachedWhitelistForStructHelper(const UStruct* Struct, bool& bInOutShouldWhitelistAllProperties) const;
};
