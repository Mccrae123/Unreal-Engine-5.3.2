// Copyright Epic Games, Inc. All Rights Reserved.

#include "DMXControlConsoleFaderGroup.h"

#include "DMXAttribute.h"
#include "DMXControlConsoleFaderBase.h"
#include "DMXControlConsoleFaderGroupRow.h"
#include "DMXControlConsoleFixturePatchCellAttributeFader.h"
#include "DMXControlConsoleFixturePatchFunctionFader.h"
#include "DMXControlConsoleFixturePatchMatrixCell.h"
#include "DMXControlConsoleRawFader.h"
#include "DMXSubsystem.h"
#include "Library/DMXEntityFixturePatch.h"
#include "Library/DMXEntityFixtureType.h"

#include "Algo/Sort.h"


#define LOCTEXT_NAMESPACE "DMXControlConsoleFaderGroup"

UDMXControlConsoleRawFader* UDMXControlConsoleFaderGroup::AddRawFader()
{
	int32 Universe;
	int32 Address;
	GetNextAvailableUniverseAndAddress(Universe, Address);

	UDMXControlConsoleRawFader* Fader = NewObject<UDMXControlConsoleRawFader>(this, NAME_None, RF_Transactional);
	Fader->SetUniverseID(Universe);
	Fader->SetAddressRange(Address);
	Elements.Add(Fader);

	return Fader;
}

UDMXControlConsoleFixturePatchFunctionFader* UDMXControlConsoleFaderGroup::AddFixturePatchFunctionFader(const FDMXFixtureFunction& FixtureFunction, const int32 InUniverseID, const int32 StartingChannel)
{
	UDMXControlConsoleFixturePatchFunctionFader* Fader = NewObject<UDMXControlConsoleFixturePatchFunctionFader>(this, NAME_None, RF_Transactional);
	Fader->SetPropertiesFromFixtureFunction(FixtureFunction, InUniverseID, StartingChannel);
	Elements.Add(Fader);

	return Fader;
}

UDMXControlConsoleFixturePatchMatrixCell* UDMXControlConsoleFaderGroup::AddFixturePatchMatrixCell(const FDMXCell& Cell, const int32 InUniverseID, const int32 StartingChannel)
{
	UDMXControlConsoleFixturePatchMatrixCell* MatrixCell = NewObject<UDMXControlConsoleFixturePatchMatrixCell>(this, NAME_None, RF_Transactional);
	MatrixCell->SetPropertiesFromCell(Cell, InUniverseID, StartingChannel);
	Elements.Add(MatrixCell);

	return MatrixCell;
}

void UDMXControlConsoleFaderGroup::DeleteElement(const TScriptInterface<IDMXControlConsoleFaderGroupElement>& Element)
{
	if (!ensureMsgf(Element, TEXT("Invalid element, cannot delete from '%s'."), *GetName()))
	{
		return;
	}

	if (!ensureMsgf(Elements.Contains(Element), TEXT("'%s' fader group is not owner of '%s'. Cannot delete element correctly."), *GetName(), *Element.GetObject()->GetName()))
	{
		return;
	}

	Elements.Remove(Element);
}

void UDMXControlConsoleFaderGroup::ClearElements()
{
	Elements.Reset();
}

TArray<UDMXControlConsoleFaderBase*> UDMXControlConsoleFaderGroup::GetAllFaders() const
{
	TArray<UDMXControlConsoleFaderBase*> AllFaders;

	for (const TScriptInterface<IDMXControlConsoleFaderGroupElement>& Element : Elements)
	{
		if (!Element)
		{
			continue;
		}

		AllFaders.Append(Element->GetFaders());
	}

	return AllFaders;
}

int32 UDMXControlConsoleFaderGroup::GetIndex() const
{
	const UDMXControlConsoleFaderGroupRow& FaderGroupRow = GetOwnerFaderGroupRowChecked();

	const TArray<UDMXControlConsoleFaderGroup*> FaderGroups = FaderGroupRow.GetFaderGroups();
	return FaderGroups.IndexOfByKey(this);
}

UDMXControlConsoleFaderGroupRow& UDMXControlConsoleFaderGroup::GetOwnerFaderGroupRowChecked() const
{
	UDMXControlConsoleFaderGroupRow* Outer = Cast<UDMXControlConsoleFaderGroupRow>(GetOuter());
	checkf(Outer, TEXT("Invalid outer for '%s', cannot get fader group owner correctly."), *GetName());

	return *Outer;
}

void UDMXControlConsoleFaderGroup::SetFaderGroupName(const FString& NewName)
{
	FaderGroupName = NewName;
}

void UDMXControlConsoleFaderGroup::GenerateFromFixturePatch(UDMXEntityFixturePatch* InFixturePatch)
{
	if (!InFixturePatch)
	{
		return;
	}

	Modify();

	SoftFixturePatchPtr = InFixturePatch;
	CachedWeakFixturePatch = InFixturePatch;

	FaderGroupName = InFixturePatch->GetDisplayName();

#if WITH_EDITOR
	EditorColor = InFixturePatch->EditorColor;
#endif // WITH_EDITOR
	
	ClearElements();

	const int32 UniverseID = InFixturePatch->GetUniverseID();
	const int32 StartingChannel = InFixturePatch->GetStartingChannel();

	// Generate Faders from Fixture Functions
	const TMap<FDMXAttributeName, FDMXFixtureFunction> FunctionsMap = InFixturePatch->GetAttributeFunctionsMap();
	for (const TTuple<FDMXAttributeName, FDMXFixtureFunction>& FunctionTuple : FunctionsMap)
	{
		const FDMXFixtureFunction FixtureFunction = FunctionTuple.Value;
		AddFixturePatchFunctionFader(FixtureFunction, UniverseID, StartingChannel);
	}

	// Generate Faders from Fixture Matrices
	FDMXFixtureMatrix FixtureMatrix;
	if (InFixturePatch->GetMatrixProperties(FixtureMatrix))
	{
		TArray<FDMXCell> Cells;
		InFixturePatch->GetAllMatrixCells(Cells);
		for (const FDMXCell& Cell : Cells)
		{
			AddFixturePatchMatrixCell(Cell, UniverseID, StartingChannel);
		}
	}

	auto SortElementsByEndingAddressLambda = [](const TScriptInterface<IDMXControlConsoleFaderGroupElement>& ItemA, const TScriptInterface<IDMXControlConsoleFaderGroupElement>& ItemB)
	{
		const int32 EndingAddressA = ItemA->GetStartingAddress();
		const int32 EndingAddressB = ItemB->GetStartingAddress();

		return EndingAddressA < EndingAddressB;
	};

	Algo::Sort(Elements, SortElementsByEndingAddressLambda);

	bForceRefresh = true;
}

bool UDMXControlConsoleFaderGroup::HasFixturePatch() const
{
	return GetFixturePatch() != nullptr;
}

bool UDMXControlConsoleFaderGroup::HasMatrixProperties() const
{
	if (!HasFixturePatch())
	{
		return false;
	}

	FDMXFixtureMatrix FixtureMatrix;
	const UDMXEntityFixturePatch* FixturePatch = GetFixturePatch();
	return FixturePatch->GetMatrixProperties(FixtureMatrix);
}

TMap<int32, TMap<int32, uint8>> UDMXControlConsoleFaderGroup::GetUniverseToFragmentMap() const
{
	TMap<int32, TMap<int32, uint8>> UniverseToFragmentMap;

	if (HasFixturePatch())
	{
		return UniverseToFragmentMap;
	}

	UDMXSubsystem* DMXSubsystem = UDMXSubsystem::GetDMXSubsystem_Pure();
	check(DMXSubsystem);

	for (const UDMXControlConsoleFaderBase* Fader : GetAllFaders())
	{
		if (!Fader || Fader->IsMuted())
		{
			continue;
		}

		const UDMXControlConsoleRawFader* RawFader = Cast<UDMXControlConsoleRawFader>(Fader);
		if (!RawFader)
		{
			continue;
		}
		
		TMap<int32, uint8>& FragmentMapRef = UniverseToFragmentMap.FindOrAdd(RawFader->GetUniverseID());

		TArray<uint8> ByteArray;
		DMXSubsystem->IntValueToBytes(RawFader->GetValue(), RawFader->GetDataType(), ByteArray, RawFader->GetUseLSBMode());

		for (int32 ByteIndex = 0; ByteIndex < ByteArray.Num(); ByteIndex++)
		{
			const int32 CurrentAddress = RawFader->GetStartingAddress() + ByteIndex;
			FragmentMapRef.FindOrAdd(CurrentAddress) = ByteArray[ByteIndex];
		}
	}

	return UniverseToFragmentMap;
}

TMap<FDMXAttributeName, int32> UDMXControlConsoleFaderGroup::GetAttributeMap() const
{
	TMap<FDMXAttributeName, int32> AttributeMap;

	if (!HasFixturePatch())
	{
		return AttributeMap;
	}

	for (const UDMXControlConsoleFaderBase* Fader : GetAllFaders())
	{
		if (!Fader || Fader->IsMuted())
		{
			continue;
		}

		const UDMXControlConsoleFixturePatchFunctionFader* FixturePatchFunctionFader = Cast<UDMXControlConsoleFixturePatchFunctionFader>(Fader);
		if (!FixturePatchFunctionFader)
		{
			continue;
		}
		
		const FDMXAttributeName& AttributeName = FixturePatchFunctionFader->GetAttributeName();
		const uint32 Value = FixturePatchFunctionFader->GetValue();
		AttributeMap.Add(AttributeName, Value);
	}

	return AttributeMap;
}

TMap<FIntPoint, TMap<FDMXAttributeName, float>> UDMXControlConsoleFaderGroup::GetMatrixCoordinateToAttributeMap() const
{
	TMap<FIntPoint, TMap<FDMXAttributeName, float>> CoordinateToMatrixAttributeMap;

	if (!HasFixturePatch() || !HasMatrixProperties())
	{
		return CoordinateToMatrixAttributeMap;
	}

	for (const TScriptInterface<IDMXControlConsoleFaderGroupElement>& Element : Elements)
	{
		const UDMXControlConsoleFixturePatchMatrixCell* MatrixCell = Cast<UDMXControlConsoleFixturePatchMatrixCell>(Element.GetObject());
		if (!MatrixCell)
		{
			continue;
		}

		// Get cell coordinates
		const int32 CellX = MatrixCell->GetCellX();
		const int32 CellY = MatrixCell->GetCellY();
		const FIntPoint CellCoordinates(CellX, CellY);
		TMap<FDMXAttributeName, float>& AttributeValueMapRef = CoordinateToMatrixAttributeMap.FindOrAdd(CellCoordinates);

		//Get attribute to value map
		const TArray<UDMXControlConsoleFaderBase*>& MatrixCellFaders = MatrixCell->GetFaders();
		for (const UDMXControlConsoleFaderBase* Fader : MatrixCellFaders)
		{
			const UDMXControlConsoleFixturePatchCellAttributeFader* CellAttributeFader = CastChecked<UDMXControlConsoleFixturePatchCellAttributeFader>(Fader);
			const FDMXAttributeName& AttributeName = CellAttributeFader->GetAttributeName();
			const float ValueRange = CellAttributeFader->GetMaxValue() - CellAttributeFader->GetMinValue();
			const uint32 Value = CellAttributeFader->GetValue();
			const float RelativeValue = (Value - CellAttributeFader->GetMinValue()) / ValueRange;

			AttributeValueMapRef.FindOrAdd(AttributeName) = RelativeValue;
		}
	}

	return CoordinateToMatrixAttributeMap;
}

void UDMXControlConsoleFaderGroup::Reset()
{
	FaderGroupName = GetName();

	SoftFixturePatchPtr.Reset();
	CachedWeakFixturePatch.Reset();

#if WITH_EDITOR
	EditorColor = FLinearColor::White;
#endif 

	ClearElements();
}

void UDMXControlConsoleFaderGroup::Destroy()
{
	UDMXControlConsoleFaderGroupRow& FaderGroupRow = GetOwnerFaderGroupRowChecked();

#if WITH_EDITOR
	FaderGroupRow.PreEditChange(UDMXControlConsoleFaderGroupRow::StaticClass()->FindPropertyByName(UDMXControlConsoleFaderGroupRow::GetFaderGroupsPropertyName()));
#endif // WITH_EDITOR

	FaderGroupRow.DeleteFaderGroup(this);

#if WITH_EDITOR
	FaderGroupRow.PostEditChange();
#endif // WITH_EDITOR
}

void UDMXControlConsoleFaderGroup::ForceRefresh()
{
	bForceRefresh = false;
}

void UDMXControlConsoleFaderGroup::PostInitProperties()
{
	Super::PostInitProperties();

	FaderGroupName = GetName();
}

void UDMXControlConsoleFaderGroup::PostLoad()
{
	Super::PostLoad();

	CachedWeakFixturePatch = Cast<UDMXEntityFixturePatch>(SoftFixturePatchPtr.ToSoftObjectPath().TryLoad());
}

#if WITH_EDITOR
void UDMXControlConsoleFaderGroup::PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent)
{
	Super::PostEditChangeProperty(PropertyChangedEvent);

	const FName PropertyName = PropertyChangedEvent.GetPropertyName();
	if (PropertyName == GET_MEMBER_NAME_CHECKED(UDMXControlConsoleFaderGroup, SoftFixturePatchPtr))
	{
		CachedWeakFixturePatch = Cast<UDMXEntityFixturePatch>(SoftFixturePatchPtr.ToSoftObjectPath().TryLoad());
	}
}
#endif // WITH_EDITOR

void UDMXControlConsoleFaderGroup::GetNextAvailableUniverseAndAddress(int32& OutUniverse, int32& OutAddress) const
{
	if (!Elements.IsEmpty())
	{
		const UDMXControlConsoleRawFader* LastFader = Cast<UDMXControlConsoleRawFader>(Elements.Last().GetObject());
		if (LastFader)
		{
			OutAddress = LastFader->GetEndingAddress() + 1;
			OutUniverse = LastFader->GetUniverseID();
			if (OutAddress > DMX_MAX_ADDRESS)
			{
				OutAddress = 1;
				OutUniverse++;
			}
		}
	}
}

#undef LOCTEXT_NAMESPACE
