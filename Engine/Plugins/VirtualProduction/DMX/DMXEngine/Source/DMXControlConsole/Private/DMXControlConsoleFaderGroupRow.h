// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"

#include "DMXControlConsoleFaderGroupRow.generated.h"

class UDMXControlConsole;
class UDMXControlConsoleFaderGroup;


/** A Row of Fader Groups in the DMX Control Console */
UCLASS()
class UDMXControlConsoleFaderGroupRow 
	: public UObject
{
	GENERATED_BODY()

public:
	/** Adds a Fader Group to this Fader Group Row */
	UDMXControlConsoleFaderGroup* AddFaderGroup(const int32 Index);

	/** Deletes the given Fader Group. If it's the last Fader Group in this Fader Group Row, requests it to be deleted  */
	void DeleteFaderGroup(UDMXControlConsoleFaderGroup* FaderGroup);

	/** Clears FaderGroups array */
	void ClearFaderGroups();

	/** Gets the Fader Group array of this Fader Group Row */
	const TArray<UDMXControlConsoleFaderGroup*>& GetFaderGroups() const { return FaderGroups; }
	
	/** Gets the Index of this Row according to the DMX Control Console */
	int32 GetRowIndex() const;

	/** Returns a reference to the DMX Control Console */
	UDMXControlConsole& GetOwnerControlConsoleChecked() const;

	/** Destroys this Fader Group Row */
	void Destroy();

	// Property Name getters
	FORCEINLINE static FName GetFaderGroupsPropertyName() { return GET_MEMBER_NAME_CHECKED(UDMXControlConsoleFaderGroupRow, FaderGroups); }

private:
	/** Fader Groups array of this Fader Group Row */
	UPROPERTY(VisibleAnywhere, Category = "DMX Fader Group Row")
	TArray<TObjectPtr<UDMXControlConsoleFaderGroup>> FaderGroups;
};
