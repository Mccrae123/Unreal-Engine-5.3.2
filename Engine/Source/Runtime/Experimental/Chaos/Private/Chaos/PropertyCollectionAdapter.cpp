// Copyright Epic Games, Inc. All Rights Reserved.

#include "Chaos/PropertyCollectionAdapter.h"
#include "GeometryCollection/ManagedArrayCollection.h"

namespace Chaos::Softs
{
	const FName FPropertyCollectionConstAdapter::PropertyGroup("Property");
	const FName FPropertyCollectionConstAdapter::KeyName("Key");
	const FName FPropertyCollectionConstAdapter::LowValueName("LowValue");
	const FName FPropertyCollectionConstAdapter::HighValueName("HighValue");
	const FName FPropertyCollectionConstAdapter::StringValueName("StringValue");
	const FName FPropertyCollectionConstAdapter::FlagsName("Flags");
	
	FPropertyCollectionConstAdapter::FPropertyCollectionConstAdapter(const TSharedPtr<const FManagedArrayCollection>& InManagedArrayCollection)
		: ManagedArrayCollection(InManagedArrayCollection)
	{
		Initialize();
	}

	FPropertyCollectionConstAdapter::FPropertyCollectionConstAdapter(const TSharedPtr<const FManagedArrayCollection>& InManagedArrayCollection, ENoInit)
		: ManagedArrayCollection(InManagedArrayCollection)
	{
	}

	void FPropertyCollectionConstAdapter::UpdateArrays()
	{
		KeyArray = GetArray<FString>(KeyName);
		LowValueArray = GetArray<FVector3f>(LowValueName);
		HighValueArray = GetArray<FVector3f>(HighValueName);
		StringValueArray = GetArray<FString>(StringValueName);
		FlagsArray = GetArray<uint8>(FlagsName);
	}
	
	void FPropertyCollectionConstAdapter::Initialize()
	{
		// Update arrays
		UpdateArrays();

		// Create a fast access search map (although it might only be faster for a large enough number of properties)
		const int32 NumKeys = KeyArray.Num();
		KeyIndices.Empty(NumKeys);
		for (int32 Index = 0; Index < NumKeys; ++Index)
		{
			KeyIndices.Emplace(KeyArray[Index], Index);
		}
	}

	template<typename T, typename ElementType>
	T FPropertyCollectionConstAdapter::GetValue(int32 KeyIndex, const TConstArrayView<ElementType>& ValueArray) const
	{
		return ValueArray[KeyIndex];
	}
	template FString FPropertyCollectionConstAdapter::GetValue<FString, FString>(int32 KeyIndex, const TConstArrayView<FString>& ValueArray) const;
	template uint8 FPropertyCollectionConstAdapter::GetValue<uint8, uint8>(int32 KeyIndex, const TConstArrayView<uint8>& ValueArray) const;

	template<>
	bool FPropertyCollectionConstAdapter::GetValue<bool, FVector3f>(int32 KeyIndex, const TConstArrayView<FVector3f>& ValueArray) const
	{
		return (bool)ValueArray[KeyIndex].X;
	}

	template<>
	int32 FPropertyCollectionConstAdapter::GetValue<int32, FVector3f>(int32 KeyIndex, const TConstArrayView<FVector3f>& ValueArray) const
	{
		return (int32)ValueArray[KeyIndex].X;
	}

	template<>
	float FPropertyCollectionConstAdapter::GetValue<float, FVector3f>(int32 KeyIndex, const TConstArrayView<FVector3f>& ValueArray) const
	{
		return ValueArray[KeyIndex].X;
	}

	template <typename T>
	TConstArrayView<T> FPropertyCollectionConstAdapter::GetArray(const FName& Name) const
	{
		const TManagedArray<T>* ManagedArray = ManagedArrayCollection->FindAttributeTyped<T>(Name, PropertyGroup);
		return ManagedArray ? TConstArrayView<T>(ManagedArray->GetConstArray()) : TConstArrayView<T>();
	}

	FPropertyCollectionAdapter::FPropertyCollectionAdapter(const TSharedPtr<FManagedArrayCollection>& InManagedArrayCollection)
		: FPropertyCollectionConstAdapter(InManagedArrayCollection, NoInit)
	{
		Construct();
		Initialize();
	}

	void FPropertyCollectionAdapter::Construct()
	{
		// ClothProperty Group
		GetManagedArrayCollection()->AddAttribute<FString>(KeyName, PropertyGroup);
		GetManagedArrayCollection()->AddAttribute<FVector3f>(LowValueName, PropertyGroup);
		GetManagedArrayCollection()->AddAttribute<FVector3f>(HighValueName, PropertyGroup);
		GetManagedArrayCollection()->AddAttribute<FString>(StringValueName, PropertyGroup);
		GetManagedArrayCollection()->AddAttribute<uint8>(FlagsName, PropertyGroup);
	}

	void FPropertyCollectionAdapter::EnableFlag(int32 KeyIndex, EPropertyFlag Flag, bool bEnable)
	{
		if (bEnable)
		{
			EnumAddFlags(GetFlagsArray()[KeyIndex], (uint8)Flag);
		}
		else
		{
			EnumRemoveFlags(GetFlagsArray()[KeyIndex], (uint8)Flag);
		}
	}

	int32 FPropertyCollectionAdapter::EnableFlag(const FString& Key, EPropertyFlag Flag, bool bEnable)
	{
		const int32 KeyIndex = GetKeyIndex(Key);
		if (KeyIndex != INDEX_NONE)
		{
			EnableFlag(KeyIndex, Flag, bEnable);
		}
		return KeyIndex;
	}

	int32 FPropertyCollectionAdapter::AddProperty(const FString& Key, bool bEnabled, bool bAnimatable)
	{
		const int32 Index = GetManagedArrayCollection()->AddElements(1, PropertyGroup);
		const uint8 Flags = (bEnabled ? (uint8)EPropertyFlag::Enabled : 0) | (bAnimatable ? (uint8)EPropertyFlag::Animatable : 0);

		// Setup the new element's default value and enable the property by default
		GetKeyArray()[Index] = Key;
		GetLowValueArray()[Index] = GetHighValueArray()[Index] = FVector3f::ZeroVector;
		GetFlagsArray()[Index] = Flags;

		// Update search map
		KeyIndices.Emplace(KeyArray[Index], Index);

		// Update the array view count
		UpdateArrays();

		return Index;
	}

	int32 FPropertyCollectionAdapter::AddProperties(const TArray<FString>& Keys, bool bEnabled, bool bAnimatable)
	{
		if (const int32 NumProperties = Keys.Num())
		{
			const int32 StartIndex = GetManagedArrayCollection()->AddElements(NumProperties, PropertyGroup);
			const uint8 Flags = (bEnabled ? (uint8)EPropertyFlag::Enabled : 0) | (bAnimatable ? (uint8)EPropertyFlag::Animatable : 0);

			for (int32 Index = StartIndex; Index < NumProperties + StartIndex; ++Index)
			{
				// Setup the new elements' default value and enable the property by default
				GetKeyArray()[Index] = Keys[Index - StartIndex];
				GetLowValueArray()[Index] = GetHighValueArray()[Index] = FVector3f::ZeroVector;
				GetFlagsArray()[Index] = Flags;

				// Update search map
				KeyIndices.Emplace(KeyArray[Index], Index);
			}

			// Update the array view count
			UpdateArrays();

			return StartIndex;
		}
		return INDEX_NONE;
	}
}  // End namespace Chaos
