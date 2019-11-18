// Copyright 1998-2019 Epic Games, Inc. All Rights Reserved.

#include "Serialization/UnversionedPropertySerializationTest.h"
#include "ProfilingDebugging/CookStats.h"
#include "Serialization/MemoryReader.h"
#include "Serialization/MemoryWriter.h"
#include "UObject/UnrealType.h"

#if UE_ENABLE_UNVERSIONED_PROPERTY_TEST

namespace PropertySerializationStats
{
	static TAtomic<uint64> Structs = 0;
	static TAtomic<uint64> VersionedBytes = 0;
	static TAtomic<uint64> UnversionedBytes = 0;
	static TAtomic<uint64> UselessBytes = 0;

#if ENABLE_COOK_STATS
	static FCookStatsManager::FAutoRegisterCallback RegisterCookStats([](FCookStatsManager::AddStatFuncRef AddStat)
	{
		AddStat(TEXT("UnversionedProperties"), 
				FCookStatsManager::CreateKeyValueArray(
					TEXT("SavedStructs"), Structs.Load(),
					TEXT("SavedMB"), static_cast<uint32>(UnversionedBytes.Load() >> 20),
					TEXT("EquivalentTaggedMB"), static_cast<uint32>(VersionedBytes.Load() >> 20),
					TEXT("CompressionRatio"), static_cast<float>(VersionedBytes.Load()) / UnversionedBytes.Load(),
					TEXT("BitfieldWasteKB"), static_cast<uint32>(UselessBytes.Load()) >> 10));
	});
#endif
}

// Serializes a UStruct to memory using both unversioned and versioned tagged property serialization,
// then creates two struct instances, loads the data back and compares that they are identical.
struct FUnversionedPropertyTest : public FUnversionedPropertyTestInput
{
	explicit FUnversionedPropertyTest(const FUnversionedPropertyTestInput& Input) : FUnversionedPropertyTestInput(Input) {}

	class FTestLinker : public FArchiveProxy
	{
	public:
		using FArchiveProxy::FArchiveProxy;

		virtual FArchive& operator<<(FName& Value) override
		{
			uint32 UnstableInt = Value.GetDisplayIndex().ToUnstableInt();
			int32 Number = Value.GetNumber();
			InnerArchive << UnstableInt << Number;

			if (IsLoading())
			{
				Value = FName::CreateFromDisplayId(FNameEntryId::FromUnstableInt(UnstableInt), Number);
			}

			return *this;
		}

		virtual FArchive& operator<<(UObject*& Value) override
		{
			return InnerArchive << reinterpret_cast<UPTRINT&>(Value);
		}

		virtual FArchive& operator<<(FLazyObjectPtr& Value) override { return FArchiveUObject::SerializeLazyObjectPtr(*this, Value); }
		virtual FArchive& operator<<(FSoftObjectPtr& Value) override { return FArchiveUObject::SerializeSoftObjectPtr(*this, Value); }
		virtual FArchive& operator<<(FSoftObjectPath& Value) override { return FArchiveUObject::SerializeSoftObjectPath(*this, Value); }
		virtual FArchive& operator<<(FWeakObjectPtr& Value) override { return FArchiveUObject::SerializeWeakObjectPtr(*this, Value); }
	};

	enum class EPath { Versioned, Unversioned };

	static const TCHAR* ToString(EPath Path)
	{
		return Path == EPath::Unversioned ? TEXT("unversioned") : TEXT("versioned");
	}

	struct FSaveResult
	{
		TArray<uint8> Data;
		TArray<UProperty*> Properties;
		EPath Path;
	};

	static thread_local FSaveResult* TlsSaveResult;

	FSaveResult Save(EPath Path)
	{
		FSaveResult Result;
		Result.Path = Path;
		
		FMemoryWriter Writer(Result.Data);
		Writer.SetUseUnversionedPropertySerialization(Path == EPath::Unversioned);
		FTestLinker Linker(Writer);
		FBinaryArchiveFormatter Formatter(Linker);
		FStructuredArchive StructuredArchive(Formatter);
		FStructuredArchive::FSlot Slot = StructuredArchive.Open();

		TlsSaveResult = &Result;
		Struct->SerializeTaggedProperties(Slot, OriginalInstance, DefaultsStruct, Defaults);
		check(TlsSaveResult == nullptr);

		return Result;
	}

	struct FTestInstance
	{
		explicit FTestInstance(const UStruct* InType)
			: Type(InType)
		{
			Instance = FMemory::Malloc(Type->GetStructureSize(), Type->GetMinAlignment());
			Type->InitializeStruct(Instance);
		}

		FTestInstance(FTestInstance&& Other)
			: Type(Other.Type)
			, Instance(Other.Instance)
		{
			Other.Instance = nullptr;
		}

		~FTestInstance()
		{
			if (Instance)
			{
				Type->DestroyStruct(Instance);
				FMemory::Free(Instance);
			}
		}

		const UStruct* Type;
		void* Instance;
	};

	FTestInstance Load(const FSaveResult& Saved)
	{
		FMemoryReader Reader(Saved.Data);
		Reader.SetUseUnversionedPropertySerialization(Saved.Path == EPath::Unversioned);
		FTestLinker Linker(Reader);
		FBinaryArchiveFormatter Formatter(Linker);
		FStructuredArchive StructuredArchive(Formatter);
		FStructuredArchive::FSlot Slot = StructuredArchive.Open();

		TGuardValue<bool> Guard(GIsSavingPackage, false);
		FTestInstance Result(Struct);
		// Call UStruct::SerializeTaggedProperties() directly to bypass
		// UUserDefinedStruct::SerializeTaggedProperties() for test loading,
		// since that is what the test saving does.
		Struct->UStruct::SerializeTaggedProperties(Slot, (uint8*)Result.Instance, DefaultsStruct, Defaults);

		checkf(Reader.Tell() == Saved.Data.Num(), TEXT("Failed to consume all %s saved property data"), ToString(Saved.Path));

		return Result;
	}

	static constexpr uint32 EqualsPortFlags = 0;

	// UProperty::Identical() flavor suited to comparing loaded instances
	static bool Equals(const UProperty* Property, const void* A, const void* B)
	{
		if (Property->GetPropertyFlags() & (CPF_EditorOnly | CPF_Transient))
		{
			return true;
		}
		else if (const UStructProperty* StructProperty = Cast<UStructProperty>(Property))
		{
			return Equals(StructProperty, A, B);
		}
		else if (const UArrayProperty* ArrayProperty = Cast<UArrayProperty>(Property))
		{
			return Equals(ArrayProperty, A, B);
		}
		else if (const USetProperty* SetProperty = Cast<USetProperty>(Property))
		{
			return Equals(SetProperty, A, B);
		}
		else if (const UMapProperty* MapProperty = Cast<UMapProperty>(Property))
		{
			return Equals(MapProperty, A, B);
		}

		return Property->Identical(A, B, EqualsPortFlags);
	}

	static bool Equals(const UArrayProperty* Property, const void* A, const void* B)
	{
		FScriptArrayHelper HelperA(Property, A);
		FScriptArrayHelper HelperB(Property, B);

		if (HelperA.Num() != HelperB.Num())
		{
			return false;
		}

		for (int32 Idx = 0, Num = HelperA.Num(); Idx < Num; ++Idx)
		{
			if (!Equals(Property->Inner, HelperA.GetRawPtr(Idx), HelperB.GetRawPtr(Idx)))
			{
				return false;
			}
		}

		return true;
	}
	
	static bool Equals(const USetProperty* Property, const void* A, const void* B)
	{
		FScriptSetHelper HelperA(Property, A);
		FScriptSetHelper HelperB(Property, B);
		const UProperty* ElemProperty   = HelperA.GetElementProperty();

		if (HelperA.Num() != HelperB.Num())
		{
			return false;
		}

		for (int32 Num = HelperA.Num(), IndexA = 0; Num > 0; --Num)
		{
			while (!HelperA.IsValidIndex(IndexA))
			{
				++IndexA;
			}
			
			const uint8* ElemA = HelperA.GetElementPtr(IndexA);
			const uint8* ElemB = HelperB.FindElementPtrFromHash(ElemA);
		
			if (!ElemB || !Equals(ElemProperty, ElemA, ElemB))
			{
				return false;
			} 
		}

		return true;
	}
	
	static bool Equals(const UMapProperty* Property, const void* A, const void* B)
	{
		FScriptMapHelper HelperA(Property, A);
		FScriptMapHelper HelperB(Property, B);
		const UProperty* KeyProp   = HelperA.GetKeyProperty();
		const UProperty* ValueProp = HelperA.GetValueProperty();
		int32 ValueOffset = HelperA.MapLayout.ValueOffset;

		if (HelperA.Num() != HelperB.Num())
		{
			return false;
		}
		
		for (int32 Num = HelperA.Num(), IndexA = 0; Num > 0; --Num)
		{
			while (!HelperA.IsValidIndex(IndexA))
			{
				++IndexA;
			}
			
			const uint8* PairA = HelperA.GetPairPtr(IndexA);
			const uint8* PairB = HelperB.FindMapPairPtrFromHash(PairA);
		
			if (!PairB || !Equals(KeyProp, PairA, PairB))
			{
				return false;
			} 

			if (!Equals(ValueProp, PairA + ValueOffset, PairB + ValueOffset))
			{
				return false;
			}
		}

		return true;
	}

	static bool Equals(const UStructProperty* Property, const void* A, const void* B)
	{
		UScriptStruct* Struct = Property->Struct;
		if (Struct->StructFlags & STRUCT_IdenticalNative)
		{
			bool bResult = false;
			if (Struct->GetCppStructOps()->Identical(A, B, EqualsPortFlags, bResult))
			{
				return bResult;
			}
		}

		// Skip deprecated fields
		for (TFieldIterator<UProperty> It(Struct, EFieldIteratorFlags::IncludeSuper, EFieldIteratorFlags::ExcludeDeprecated); It; ++It)
		{
			for (int32 Idx = 0, MaxIdx = It->ArrayDim; Idx < MaxIdx; ++Idx)
			{
				if (!Equals_InContainer(*It, A, B, Idx))
				{
					return false;
				}
			}
		}
		return true;
	}

	static bool Equals_InContainer(const UProperty* Property, const void* A, const void* B, uint32 Idx)
	{
		return Equals(Property, Property->ContainerPtrToValuePtr<void>(A, Idx), Property->ContainerPtrToValuePtr<void>(B, Idx));
	}

	static FString GetValueAsText(UProperty* Property, uint32 ArrayIdx, void* StructInstance)
	{
		FString Value;
		Property->ExportText_InContainer(ArrayIdx, Value, StructInstance, nullptr, nullptr, 0);
		return Value;
	}


	void CheckEqual(UProperty* Property, void* VersionedInstance, void* UnversionedInstance)
	{
		for (int32 Idx = 0, Num = Property->ArrayDim; Idx < Num; ++Idx)
		{
			if (!Equals_InContainer(Property, VersionedInstance, UnversionedInstance, Idx))
			{
				bool VersionedOk = Equals_InContainer(Property, VersionedInstance, OriginalInstance, Idx);
				bool UnversionedOk = Equals_InContainer(Property, UnversionedInstance, OriginalInstance, Idx);
				const TCHAR* OkPaths = VersionedOk ? (UnversionedOk ? TEXT("Both paths") : TEXT("Versioned path"))
													: (UnversionedOk ? TEXT("Unversioned path") : TEXT("Neither path"));

				// When debugging test failures, put a breakpoint inside this if statement
				if (FPlatformMisc::IsDebuggerPresent())
				{
					// These strings might be too long to fit in the assert message.
					// We could write traversal code that identifies which nested property differs
					// and only generates a text representation for that value. 
					FString VersionedValue = GetValueAsText(Property, Idx, VersionedInstance);
					FString UnversionedValue = GetValueAsText(Property, Idx, UnversionedInstance);
					FString OriginalValue = GetValueAsText(Property, Idx, OriginalInstance);

					FSaveResult VersionedSaved2 = Save(EPath::Versioned);
					FSaveResult UnversionedSaved2 = Save(EPath::Unversioned);

					FTestInstance VersionedLoaded2 = Load(VersionedSaved2);
					FTestInstance UnversionedLoaded2 = Load(UnversionedSaved2);
				}		

				checkf(false, TEXT("The %s %s.%s roundtripped differently in versioned vs unversioned tagged property serialization. "
					"%s loaded an instance equal to the original."), *Property->GetClass()->GetName(), *Struct->GetName(), *Property->GetName(), OkPaths);
			}
		}		
	}

	static TArray<UProperty*> ExcludeEditorOnlyProperties(const TArray<UProperty*>& Properties)
	{
		TArray<UProperty*> Out;
		Out.Reserve(Properties.Num());

		for (UProperty* Property : Properties)
		{
			if (!Property->IsEditorOnly())
			{
				Out.Add(Property);
			}
		}

		return Out;
	}

	void Run()
	{
		FSaveResult VersionedSaved = Save(EPath::Versioned);
		FSaveResult UnversionedSaved = Save(EPath::Unversioned);	

		check(ExcludeEditorOnlyProperties(VersionedSaved.Properties) == UnversionedSaved.Properties);

		FTestInstance VersionedLoaded = Load(VersionedSaved);
		FTestInstance UnversionedLoaded = Load(UnversionedSaved);

		for (UProperty* Property : UnversionedSaved.Properties)
		{
			CheckEqual(Property, VersionedLoaded.Instance, UnversionedLoaded.Instance);
			PropertySerializationStats::UselessBytes += Property->IsA<UBoolProperty>() && !Cast<UBoolProperty>(Property)->IsNativeBool();
		}
		
		++PropertySerializationStats::Structs;
		PropertySerializationStats::VersionedBytes += VersionedSaved.Data.Num();
		PropertySerializationStats::UnversionedBytes += UnversionedSaved.Data.Num();
	}
};

thread_local FUnversionedPropertyTest::FSaveResult* FUnversionedPropertyTest::TlsSaveResult;
thread_local bool FUnversionedPropertyTestRunner::bTlsTesting;

void RunUnversionedPropertyTest(const FUnversionedPropertyTestInput& Input)
{
	FUnversionedPropertyTest Test(Input);
	Test.Run();
}

FUnversionedPropertyTestCollector::FUnversionedPropertyTestCollector()
{
	if (FUnversionedPropertyTest::FSaveResult* Result = FUnversionedPropertyTest::TlsSaveResult)
	{
		Out = &Result->Properties;

		// Nested SerializeTaggedProperties() call should not record nested properties
		FUnversionedPropertyTest::TlsSaveResult = nullptr;
	}
	else
	{
		Out = nullptr;
	}
}

#endif