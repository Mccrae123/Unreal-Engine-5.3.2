// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "MassEntityTypes.h"
#include "MassExternalSubsystemTraits.h"
#include "MassEntityQuery.h"


struct MASSENTITY_API FMassExecutionContext
{
private:

	template< typename ViewType >
	struct TFragmentView 
	{
		FMassFragmentRequirement Requirement;
		ViewType FragmentView;

		TFragmentView() {}
		explicit TFragmentView(const FMassFragmentRequirement& InRequirement) : Requirement(InRequirement) {}

		bool operator==(const UScriptStruct* FragmentType) const { return Requirement.StructType == FragmentType; }
	};
	using FFragmentView = TFragmentView<TArrayView<FMassFragment>>;
	TArray<FFragmentView, TInlineAllocator<8>> FragmentViews;

	using FChunkFragmentView = TFragmentView<FStructView>;
	TArray<FChunkFragmentView, TInlineAllocator<4>> ChunkFragmentViews;

	using FConstSharedFragmentView = TFragmentView<FConstStructView>;
	TArray<FConstSharedFragmentView, TInlineAllocator<4>> ConstSharedFragmentViews;

	using FSharedFragmentView = TFragmentView<FStructView>;
	TArray<FSharedFragmentView, TInlineAllocator<4>> SharedFragmentViews;

	FMassExternalSubystemBitSet ConstSubsystemsBitSet;
	FMassExternalSubystemBitSet MutableSubsystemsBitSet;
	TArray<TObjectPtr<USubsystem>> Subsystems;
	
	// mz@todo make this shared ptr thread-safe and never auto-flush in MT environment. 
	TSharedPtr<FMassCommandBuffer> DeferredCommandBuffer;
	TArrayView<FMassEntityHandle> EntityListView;
	
	/** If set this indicates the exact archetype and its chunks to be processed. 
	 *  @todo this data should live somewhere else, preferably be just a parameter to Query.ForEachEntityChunk function */
	FMassArchetypeEntityCollection EntityCollection;
	
	/** @todo rename to "payload" */
	FInstancedStruct AuxData;
	float DeltaTimeSeconds = 0.0f;
	int32 ChunkSerialModificationNumber = -1;
	FMassTagBitSet CurrentArchetypesTagBitSet;

#if WITH_MASSENTITY_DEBUG
	FString DebugExecutionDescription;
#endif
	
	/** Used to control when the context is allowed to flush commands collected in DeferredCommandBuffer. This mechanism 
	 * is mainly utilized to avoid numerous small flushes in favor of fewer larger ones. */
	bool bFlushDeferredCommands = true;

	/** A temporary variable letting us access subsystems via a FMassExecutionContext outside of MassEntityQuery's 
	 * execution. Will go away once entity-independent calculations as well as query-bound calculations are being 
	 * performed with a common tasks interface */
	bool bSubsystemRequirementsSet = false;

	TArrayView<FFragmentView> GetMutableRequirements() { return FragmentViews; }
	TArrayView<FChunkFragmentView> GetMutableChunkRequirements() { return ChunkFragmentViews; }
	TArrayView<FConstSharedFragmentView> GetMutableConstSharedRequirements() { return ConstSharedFragmentViews; }
	TArrayView<FSharedFragmentView> GetMutableSharedRequirements() { return SharedFragmentViews; }

	friend FMassArchetypeData;
	friend FMassEntityQuery;

public:
	explicit FMassExecutionContext(const float InDeltaTimeSeconds, const bool bInFlushDeferredCommands = true)
		: DeltaTimeSeconds(InDeltaTimeSeconds)
		, bFlushDeferredCommands(bInFlushDeferredCommands)
	{
		Subsystems.AddZeroed(FMassExternalSubystemBitSet::GetMaxNum());
	}

	FMassExecutionContext()
		: FMassExecutionContext(/*InDeltaTimeSeconds=*/0.f)
	{}

#if WITH_MASSENTITY_DEBUG
	const FString& DebugGetExecutionDesc() const { return DebugExecutionDescription; }
	void DebugSetExecutionDesc(const FString& Description) { DebugExecutionDescription = Description; }
#endif

	/** Sets bFlushDeferredCommands. Note that setting to True while the system is being executed doesn't result in
	 *  immediate commands flushing */
	void SetFlushDeferredCommands(const bool bNewFlushDeferredCommands) { bFlushDeferredCommands = bNewFlushDeferredCommands; } 
	void SetDeferredCommandBuffer(const TSharedPtr<FMassCommandBuffer>& InDeferredCommandBuffer) { DeferredCommandBuffer = InDeferredCommandBuffer; }
	void SetEntityCollection(const FMassArchetypeEntityCollection& InEntityCollection);
	void SetEntityCollection(FMassArchetypeEntityCollection&& InEntityCollection);
	void ClearEntityCollection() { EntityCollection.Reset(); }
	void SetAuxData(const FInstancedStruct& InAuxData) { AuxData = InAuxData; }

	float GetDeltaTimeSeconds() const
	{
		return DeltaTimeSeconds;
	}

	TSharedPtr<FMassCommandBuffer> GetSharedDeferredCommandBuffer() const { return DeferredCommandBuffer; }
	FMassCommandBuffer& Defer() const { checkSlow(DeferredCommandBuffer.IsValid()); return *DeferredCommandBuffer.Get(); }

	TConstArrayView<FMassEntityHandle> GetEntities() const { return EntityListView; }
	int32 GetNumEntities() const { return EntityListView.Num(); }

	FMassEntityHandle GetEntity(const int32 Index) const
	{
		return EntityListView[Index];
	}

	template<typename T>
	bool DoesArchetypeHaveTag() const
	{
		static_assert(TIsDerivedFrom<T, FMassTag>::IsDerived, "Given struct is not of a valid fragment type.");
		return CurrentArchetypesTagBitSet.Contains<T>();
	}

	/** Chunk related operations */
	void SetCurrentChunkSerialModificationNumber(const int32 SerialModificationNumber) { ChunkSerialModificationNumber = SerialModificationNumber; }
	int32 GetChunkSerialModificationNumber() const { return ChunkSerialModificationNumber; }

	template<typename T>
	T* GetMutableChunkFragmentPtr() const
	{
		static_assert(TIsDerivedFrom<T, FMassChunkFragment>::IsDerived, "Given struct doesn't represent a valid chunk fragment type. Make sure to inherit from FMassChunkFragment or one of its child-types.");

		const UScriptStruct* Type = T::StaticStruct();
		const FChunkFragmentView* FoundChunkFragmentData = ChunkFragmentViews.FindByPredicate([Type](const FChunkFragmentView& Element) { return Element.Requirement.StructType == Type; } );
		return FoundChunkFragmentData ? FoundChunkFragmentData->FragmentView.GetMutablePtr<T>() : static_cast<T*>(nullptr);
	}
	
	template<typename T>
	T& GetMutableChunkFragment() const
	{
		T* ChunkFragment = GetMutableChunkFragmentPtr<T>();
		checkf(ChunkFragment, TEXT("Chunk Fragment requirement not found: %s"), *T::StaticStruct()->GetName());
		return *ChunkFragment;
	}

	template<typename T>
	const T* GetChunkFragmentPtr() const
	{
		return GetMutableChunkFragmentPtr<T>();
	}
	
	template<typename T>
	const T& GetChunkFragment() const
	{
		const T* ChunkFragment = GetMutableChunkFragmentPtr<T>();
		checkf(ChunkFragment, TEXT("Chunk Fragment requirement not found: %s"), *T::StaticStruct()->GetName());
		return *ChunkFragment;
	}

	/** Shared fragment related operations */
	template<typename T>
	const T* GetConstSharedFragmentPtr() const
	{
		static_assert(TIsDerivedFrom<T, FMassSharedFragment>::IsDerived, "Given struct doesn't represent a valid Shared fragment type. Make sure to inherit from FMassSharedFragment or one of its child-types.");

		const FConstSharedFragmentView* FoundSharedFragmentData = ConstSharedFragmentViews.FindByPredicate([](const FConstSharedFragmentView& Element) { return Element.Requirement.StructType == T::StaticStruct(); });
		return FoundSharedFragmentData ? FoundSharedFragmentData->FragmentView.GetPtr<T>() : static_cast<const T*>(nullptr);
	}

	template<typename T>
	const T& GetConstSharedFragment() const
	{
		const T* SharedFragment = GetConstSharedFragmentPtr<T>();
		checkf(SharedFragment, TEXT("Shared Fragment requirement not found: %s"), *T::StaticStruct()->GetName());
		return *SharedFragment;
	}


	template<typename T>
	T* GetMutableSharedFragmentPtr() const
	{
		static_assert(TIsDerivedFrom<T, FMassSharedFragment>::IsDerived, "Given struct doesn't represent a valid shared fragment type. Make sure to inherit from FMassSharedFragment or one of its child-types.");

		const FSharedFragmentView* FoundSharedFragmentData = SharedFragmentViews.FindByPredicate([](const FSharedFragmentView& Element) { return Element.Requirement.StructType == T::StaticStruct(); });
		return FoundSharedFragmentData ? FoundSharedFragmentData->FragmentView.GetMutablePtr<T>() : static_cast<T*>(nullptr);
	}

	template<typename T>
	T& GetMutableSharedFragment() const
	{
		T* SharedFragment = GetMutableSharedFragmentPtr<T>();
		checkf(SharedFragment, TEXT("Shared Fragment requirement not found: %s"), *T::StaticStruct()->GetName());
		return *SharedFragment;
	}

	template<typename T>
	const T* GetSharedFragmentPtr() const
	{
		return GetMutableSharedFragmentPtr<T>();
	}

	template<typename T>
	const T& GetSharedFragment() const
	{
		const T* SharedFragment = GetSharedFragmentPtr<T>();
		checkf(SharedFragment, TEXT("Shared Fragment requirement not found: %s"), *T::StaticStruct()->GetName());
		return *SharedFragment;
	}

	/* Fragments related operations */
	template<typename TFragment>
	TArrayView<TFragment> GetMutableFragmentView()
	{
		const UScriptStruct* FragmentType = TFragment::StaticStruct();
		const FFragmentView* View = FragmentViews.FindByPredicate([FragmentType](const FFragmentView& Element) { return Element.Requirement.StructType == FragmentType; });
		//checkfSlow(View != nullptr, TEXT("Requested fragment type not bound"));
		//checkfSlow(View->Requirement.AccessMode == EMassFragmentAccess::ReadWrite, TEXT("Requested fragment has not been bound for writing"));
		return MakeArrayView<TFragment>((TFragment*)View->FragmentView.GetData(), View->FragmentView.Num());
	}

	template<typename TFragment>
	TConstArrayView<TFragment> GetFragmentView() const
	{
		const UScriptStruct* FragmentType = TFragment::StaticStruct();
		const FFragmentView* View = FragmentViews.FindByPredicate([FragmentType](const FFragmentView& Element) { return Element.Requirement.StructType == FragmentType; });
		//checkfSlow(View != nullptr, TEXT("Requested fragment type not bound"));
		return TConstArrayView<TFragment>((const TFragment*)View->FragmentView.GetData(), View->FragmentView.Num());
	}

	TConstArrayView<FMassFragment> GetFragmentView(const UScriptStruct* FragmentType) const
	{
		const FFragmentView* View = FragmentViews.FindByPredicate([FragmentType](const FFragmentView& Element) { return Element.Requirement.StructType == FragmentType; });
		checkSlow(View);
		return TConstArrayView<FMassFragment>((const FMassFragment*)View->FragmentView.GetData(), View->FragmentView.Num());;
	}

	TArrayView<FMassFragment> GetMutableFragmentView(const UScriptStruct* FragmentType) 
	{
		const FFragmentView* View = FragmentViews.FindByPredicate([FragmentType](const FFragmentView& Element) { return Element.Requirement.StructType == FragmentType; });
		checkSlow(View);
		return View->FragmentView;
	}

	template<typename T>
	T* GetMutableSubsystem(const UWorld* World)
	{
		// @todo consider getting this directly from entity subsystem - it should cache all the used system
		const uint32 SystemIndex = FMassExternalSubystemBitSet::GetTypeIndex<T>();
		if (bSubsystemRequirementsSet == false || ensure(MutableSubsystemsBitSet.IsBitSet(SystemIndex)))
		{
			return GetSubsystemInternal<T>(World, SystemIndex);
		}

		return nullptr;
	}

	template<typename T>
	T& GetMutableSubsystemChecked(const UWorld* World)
	{
		T* InstancePtr = GetMutableSubsystem<T>(World);
		check(InstancePtr);
		return *InstancePtr;
	}

	template<typename T>
	const T* GetSubsystem(const UWorld* World)
	{
		const uint32 SystemIndex = FMassExternalSubystemBitSet::GetTypeIndex<T>();
		if (bSubsystemRequirementsSet == false || ensure(ConstSubsystemsBitSet.IsBitSet(SystemIndex) || MutableSubsystemsBitSet.IsBitSet(SystemIndex)))
		{
			return GetSubsystemInternal<T>(World, SystemIndex);
		}
		return nullptr;
	}

	template<typename T>
	const T& GetSubsystemChecked(const UWorld* World)
	{
		const T* InstancePtr = GetSubsystem<T>(World);
		check(InstancePtr);
		return *InstancePtr;
	}
	

	void SetSubsystemRequirements(const FMassExternalSubystemBitSet& RequiredConstSubsystems, const FMassExternalSubystemBitSet& RequiredMutableSubsystems);

	/** Sparse chunk related operation */
	const FMassArchetypeEntityCollection& GetEntityCollection() const { return EntityCollection; }

	const FInstancedStruct& GetAuxData() const { return AuxData; }
	FInstancedStruct& GetMutableAuxData() { return AuxData; }
	
	template<typename TFragment>
	bool ValidateAuxDataType() const
	{
		const UScriptStruct* FragmentType = GetAuxData().GetScriptStruct();
		return FragmentType != nullptr && FragmentType == TFragment::StaticStruct();
	}

	void FlushDeferred(UMassEntitySubsystem& EntitySystem) const;

	void ClearExecutionData();
	void SetCurrentArchetypesTagBitSet(const FMassTagBitSet& BitSet)
	{
		CurrentArchetypesTagBitSet = BitSet;
	}

protected:
	void SetRequirements(TConstArrayView<FMassFragmentRequirement> InRequirements, 
		TConstArrayView<FMassFragmentRequirement> InChunkRequirements, 
		TConstArrayView<FMassFragmentRequirement> InConstSharedRequirements, 
		TConstArrayView<FMassFragmentRequirement> InSharedRequirements);

	void ClearFragmentViews()
	{
		for (FFragmentView& View : FragmentViews)
		{
			View.FragmentView = TArrayView<FMassFragment>();
		}
		for (FChunkFragmentView& View : ChunkFragmentViews)
		{
			View.FragmentView.Reset();
		}
		for (FConstSharedFragmentView& View : ConstSharedFragmentViews)
		{
			View.FragmentView.Reset();
		}
		for (FSharedFragmentView& View : SharedFragmentViews)
		{
			View.FragmentView.Reset();
		}
	}

	template<typename T>
	T* GetSubsystemInternal(const UWorld* World, const uint32 SystemIndex)
	{
		if (UNLIKELY(Subsystems.IsValidIndex(SystemIndex) == false))
		{
			Subsystems.AddZeroed(Subsystems.Num() - SystemIndex + 1);
		}

		T* SystemInstance = (T*)Subsystems[SystemIndex].Get();
		if (SystemInstance == nullptr)
		{
			SystemInstance = FMassExternalSubsystemTraits::GetInstance<typename TRemoveConst<T>::Type>(World);
			Subsystems[SystemIndex] = SystemInstance;
		}
		return SystemInstance;
	}
};
