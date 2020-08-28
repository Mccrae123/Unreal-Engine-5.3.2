// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include <type_traits>
#include "CoreMinimal.h"
#include "TypedElementData.h"
#include "TypedElementHandle.generated.h"

/**
 * Base typed used to represent element interfaces.
 * @note Top-level element interfaces that inherit from this should also specialize TTypedElement for their API.
 */
UCLASS(Abstract)
class TYPEDELEMENTFRAMEWORK_API UTypedElementInterface : public UObject
{
	GENERATED_BODY()
};

/**
 * The most minimal representation of an element - its ID!
 * This type is not immediately useful on its own, but can be used to find an element from the element registry or an element list.
 * @note This is ref-counted like handles themselves are, so as long as an ID is available, the handle will be too.
 * @note IDs lack the information needed to auto-release on destruction, so must be manually released, either via the corresponding handle or their owner element registry.
 */
struct TYPEDELEMENTFRAMEWORK_API FTypedElementId
{
public:
	FTypedElementId()
		: CombinedId(0)
	{
	}

	FTypedElementId(const FTypedElementId&) = delete;
	FTypedElementId& operator=(const FTypedElementId&) = delete;
	
	FTypedElementId(FTypedElementId&& InOther)
		: CombinedId(InOther.CombinedId)
	{
		InOther.Private_DestroyNoRef();
	}

	FTypedElementId& operator=(FTypedElementId&& InOther)
	{
		if (this != &InOther)
		{
			CombinedId = InOther.CombinedId;

			InOther.Private_DestroyNoRef();
		}
		return *this;
	}

	~FTypedElementId()
	{
		checkf(!IsSet(), TEXT("Element ID was still set during destruction! This will leak an element reference, and you should release this ID prior to destruction!"));
	}

	FORCEINLINE explicit operator bool() const
	{
		return IsSet();
	}

	/**
	 * Has this ID been initialized to a valid element?
	 */
	FORCEINLINE bool IsSet() const
	{
		return TypeId != 0;
	}

	/**
	 * Access the type ID portion of this element ID.
	 */
	FORCEINLINE FTypedHandleTypeId GetTypeId() const
	{
		return TypeId;
	}

	/**
	 * Access the element ID portion of this element ID.
	 */
	FORCEINLINE FTypedHandleElementId GetElementId() const
	{
		return ElementId;
	}

	/**
	 * Access the combined value of this element ID.
	 * @note You typically don't want to store this directly as the element ID could be re-used.
	 *       It is primarily useful as a secondary cache where something is keeping a reference to an element ID or element handle (eg, how FTypedElementList uses it internally).
	 */
	FORCEINLINE FTypedHandleCombinedId GetCombinedId() const
	{
		return CombinedId;
	}

	FORCEINLINE friend bool operator==(const FTypedElementId& InLHS, const FTypedElementId& InRHS)
	{
		return InLHS.CombinedId == InRHS.CombinedId;
	}

	FORCEINLINE friend bool operator!=(const FTypedElementId& InLHS, const FTypedElementId& InRHS)
	{
		return !(InLHS == InRHS);
	}

	friend inline uint32 GetTypeHash(const FTypedElementId& InElementId)
	{
		return GetTypeHash(InElementId.CombinedId);
		
	}

	FORCEINLINE void Private_InitializeNoRef(const FTypedHandleTypeId InTypeId, const FTypedHandleElementId InElementId)
	{
		TypeId = InTypeId;
		ElementId = InElementId;
	}

	FORCEINLINE void Private_DestroyNoRef()
	{
		CombinedId = 0;
	}

	/** An unset element ID */
	static const FTypedElementId Unset;

private:
	union
	{
		struct
		{
			// Note: These are arranged in this order to give CombinedId better hash distribution for GetTypeHash!
			FTypedHandleCombinedId ElementId : TypedHandleElementIdBits;
			FTypedHandleCombinedId TypeId : TypedHandleTypeIdBits;
		};
		FTypedHandleCombinedId CombinedId;
	};
};

/**
 * A representation of an element that includes its handle data.
 * This type is the most standard way that an element is passed through to interfaces, and also the type that is stored in element lists.
 * C++ code may choose to use TTypedElement instead, which is a combination of an element handle and its associated element interface.
 * @note Handles auto-release on destruction.
 */
USTRUCT(BlueprintType)
struct TYPEDELEMENTFRAMEWORK_API FTypedElementHandle
{
	GENERATED_BODY()

public:
	FTypedElementHandle() = default;

	FTypedElementHandle(const FTypedElementHandle& InOther)
	{
		if (InOther)
		{
			Private_InitializeAddRef(InOther.Id.GetTypeId(), InOther.Id.GetElementId(), *InOther.DataPtr);
		}
	}

	FTypedElementHandle& operator=(const FTypedElementHandle& InOther)
	{
		if (this != &InOther)
		{
			Private_DestroyReleaseRef();

			if (InOther)
			{
				Private_InitializeAddRef(InOther.Id.GetTypeId(), InOther.Id.GetElementId(), *InOther.DataPtr);
			}
		}
		return *this;
	}

	FTypedElementHandle(FTypedElementHandle&& InOther)
		: Id(MoveTemp(InOther.Id))
		, DataPtr(InOther.DataPtr)
	{
		InOther.DataPtr = nullptr;
		checkSlow(!InOther.IsSet());
	}

	FTypedElementHandle& operator=(FTypedElementHandle&& InOther)
	{
		if (this != &InOther)
		{
			Private_DestroyReleaseRef();

			Id = MoveTemp(InOther.Id);
			DataPtr = InOther.DataPtr;

			InOther.DataPtr = nullptr;
			checkSlow(!InOther.IsSet());
		}
		return *this;
	}

	~FTypedElementHandle()
	{
		Private_DestroyReleaseRef();
	}

	FORCEINLINE explicit operator bool() const
	{
		return IsSet();
	}

	/**
	 * Has this handle been initialized to a valid element?
	 */
	FORCEINLINE bool IsSet() const
	{
		return Id.IsSet();
	}

	/**
	 * Release this handle and set it back to an empty state.
	 */
	FORCEINLINE void Release()
	{
		Private_DestroyReleaseRef();
	}

	/**
	 * Get the ID that this element represents.
	 */
	FORCEINLINE const FTypedElementId& GetId() const
	{
		return Id;
	}

	/**
	 * Test to see whether the data stored within this handle is of the given type.
	 * @note This is not typically something you'd want to query outside of data access within an interface implementation.
	 */
	template <typename ElementDataType>
	FORCEINLINE bool IsDataOfType() const
	{
		return Id.GetTypeId() == ElementDataType::StaticTypeId();
	}

	/**
	 * Attempt to access the data stored within this handle as the given type, returning null if it isn't possible and logging an access error for scripting.
	 * @note This is not typically something you'd want to use outside of data access within an interface implementation.
	 */
	template <typename ElementDataType>
	const ElementDataType* GetData() const
	{
		if (!DataPtr)
		{
			FFrame::KismetExecutionMessage(TEXT("Element handle data is null!"), ELogVerbosity::Error);
			return nullptr;
		}

		if (!IsDataOfType<ElementDataType>())
		{
			FFrame::KismetExecutionMessage(*FString::Printf(TEXT("Element handle data type is '%d', but '%d' (%s) was requested!"), Id.GetTypeId(), ElementDataType::StaticTypeId(), *ElementDataType::StaticTypeName().ToString()), ELogVerbosity::Error);
			return nullptr;
		}

		return static_cast<const ElementDataType*>(DataPtr->GetUntypedData());
	}

	/**
	 * Attempt to access the data stored within this handle as the given type, asserting if it isn't possible.
	 * @note This is not typically something you'd want to use outside of data access within an interface implementation.
	 */
	template <typename ElementDataType>
	FORCEINLINE const ElementDataType& GetDataChecked() const
	{
		checkf(DataPtr, TEXT("Element handle data is null!"));
		checkf(IsDataOfType<ElementDataType>(), TEXT("Element handle data type is '%d', but '%d' (%s) was requested!"), Id.GetTypeId(), ElementDataType::StaticTypeId(), *ElementDataType::StaticTypeName().ToString());
		return *static_cast<const ElementDataType*>(DataPtr->GetUntypedData());
	}
	
	/**
	 * Acquire a copy of the ID that this element represents.
	 * @note This must be paired with a call to ReleaseId.
	 */
	FTypedElementId AcquireId() const
	{
		FTypedElementId ElementId;
		if (IsSet())
		{
			AddRef();
			ElementId.Private_InitializeNoRef(Id.GetTypeId(), Id.GetElementId());
		}
		return ElementId;
	}

	/**
	 * Release a copy of the ID that this element represents.
	 * @note This should have come from a call to AcquireId.
	 */
	void ReleaseId(FTypedElementId& InOutElementId) const
	{
		checkf(InOutElementId == Id, TEXT("Element ID does not match this handle!"));
		if (InOutElementId)
		{
			ReleaseRef();
			InOutElementId.Private_DestroyNoRef();
		}
	}

	FORCEINLINE friend bool operator==(const FTypedElementHandle& InLHS, const FTypedElementHandle& InRHS)
	{
		return InLHS.Id == InRHS.Id;
	}

	FORCEINLINE friend bool operator!=(const FTypedElementHandle& InLHS, const FTypedElementHandle& InRHS)
	{
		return !(InLHS == InRHS);
	}

	friend inline uint32 GetTypeHash(const FTypedElementHandle& InElementHandle)
	{
		return GetTypeHash(InElementHandle.Id);
	}

	FORCEINLINE void Private_InitializeNoRef(const FTypedHandleTypeId InTypeId, const FTypedHandleElementId InElementId, const FTypedElementInternalData& InData)
	{
		Id.Private_InitializeNoRef(InTypeId, InElementId);
		DataPtr = &InData;
	}

	FORCEINLINE void Private_InitializeAddRef(const FTypedHandleTypeId InTypeId, const FTypedHandleElementId InElementId, const FTypedElementInternalData& InData)
	{
		Private_InitializeNoRef(InTypeId, InElementId, InData);
		AddRef();
	}

	FORCEINLINE void Private_DestroyNoRef()
	{
		Id.Private_DestroyNoRef();
		DataPtr = nullptr;
	}

	FORCEINLINE void Private_DestroyReleaseRef()
	{
		ReleaseRef();
		Private_DestroyNoRef();
	}

	FORCEINLINE const FTypedElementInternalData* Private_GetInternalData() const
	{
		return DataPtr;
	}

private:
	FORCEINLINE void AddRef() const
	{
#if UE_TYPED_ELEMENT_HAS_REFCOUNT
		if (DataPtr)
		{
			DataPtr->AddRef();
		}
#endif	// UE_TYPED_ELEMENT_HAS_REFCOUNT
	}

	FORCEINLINE void ReleaseRef() const
	{
#if UE_TYPED_ELEMENT_HAS_REFCOUNT
		if (DataPtr)
		{
			DataPtr->ReleaseRef();
		}
#endif	// UE_TYPED_ELEMENT_HAS_REFCOUNT
	}

	FTypedElementId Id;
	const FTypedElementInternalData* DataPtr = nullptr;
};

/**
 * Common implementation of TTypedElement that is inherited by all specializations.
 */
template <typename BaseInterfaceType>
struct TTypedElementBase : public FTypedElementHandle
{
public:
	TTypedElementBase() = default;

	TTypedElementBase(const TTypedElementBase&) = default;
	TTypedElementBase& operator=(const TTypedElementBase&) = default;

	TTypedElementBase(TTypedElementBase&& InOther)
		: FTypedElementHandle(MoveTemp(InOther))
		, InterfacePtr(InOther.InterfacePtr)
	{
		InOther.InterfacePtr = nullptr;
		checkSlow(!InOther.IsSet());
	}

	TTypedElementBase& operator=(TTypedElementBase&& InOther)
	{
		if (this != &InOther)
		{
			FTypedElementHandle::operator=(MoveTemp(InOther));
			InterfacePtr = InOther.InterfacePtr;

			InOther.InterfacePtr = nullptr;
			checkSlow(!InOther.IsSet());
		}
		return *this;
	}

	~TTypedElementBase()
	{
		Private_DestroyReleaseRef();
	}

	FORCEINLINE explicit operator bool() const
	{
		return IsSet();
	}

	/**
	 * Has this element been initialized to a valid handle and interface?
	 */
	FORCEINLINE bool IsSet() const
	{
		return FTypedElementHandle::IsSet()
			&& InterfacePtr;
	}

	/**
	 * Release this element and set it back to an empty state.
	 */
	FORCEINLINE void Release()
	{
		Private_DestroyReleaseRef();
	}

	/**
	 * Test to see whether the interface stored within this element is of the given type.
	 */
	template <typename InterfaceType>
	FORCEINLINE bool HasInterface() const
	{
		return HasInterface(InterfaceType::StaticClass());
	}

	/**
	 * Test to see whether the interface stored within this element is of the given type.
	 */
	FORCEINLINE bool HasInterface(const UClass* InInterfaceType) const
	{
		return InterfacePtr
			&& InterfacePtr->IsA(InInterfaceType);
	}

	/**
	 * Attempt to access the interface stored within this element, returning null if it isn't set.
	 */
	FORCEINLINE BaseInterfaceType* GetInterface() const
	{
		return InterfacePtr;
	}

	/**
	 * Attempt to access the interface stored within this element, asserting if it isn't set.
	 */
	FORCEINLINE BaseInterfaceType& GetInterfaceChecked() const
	{
		checkf(InterfacePtr, TEXT("Interface is null!"));
		return *InterfacePtr;
	}

	FORCEINLINE friend bool operator==(const TTypedElementBase& InLHS, const TTypedElementBase& InRHS)
	{
		return static_cast<const FTypedElementHandle&>(InLHS) == static_cast<const FTypedElementHandle&>(InRHS)
			&& InLHS.InterfacePtr == InRHS.InterfacePtr;
	}

	FORCEINLINE friend bool operator!=(const TTypedElementBase& InLHS, const TTypedElementBase& InRHS)
	{
		return !(InLHS == InRHS);
	}

	friend inline uint32 GetTypeHash(const TTypedElementBase& InElement)
	{
		return HashCombine(GetTypeHash(static_cast<const FTypedElementHandle&>(InElement)), GetTypeHash(InElement.InterfacePtr));
	}

	FORCEINLINE void Private_InitializeNoRef(const FTypedHandleTypeId InTypeId, const FTypedHandleElementId InElementId, const FTypedElementInternalData& InData, BaseInterfaceType* InInterfacePtr)
	{
		FTypedElementHandle::Private_InitializeNoRef(InTypeId, InElementId, InData);
		InterfacePtr = InInterfacePtr;
	}

	FORCEINLINE void Private_InitializeAddRef(const FTypedHandleTypeId InTypeId, const FTypedHandleElementId InElementId, const FTypedElementInternalData& InData, BaseInterfaceType* InInterfacePtr)
	{
		FTypedElementHandle::Private_InitializeAddRef(InTypeId, InElementId, InData);
		InterfacePtr = InInterfacePtr;
	}

	FORCEINLINE void Private_DestroyNoRef()
	{
		FTypedElementHandle::Private_DestroyNoRef();
		InterfacePtr = nullptr;
	}

	FORCEINLINE void Private_DestroyReleaseRef()
	{
		FTypedElementHandle::Private_DestroyReleaseRef();
		InterfacePtr = nullptr;
	}

protected:
	BaseInterfaceType* InterfacePtr = nullptr;
};

/**
 * A combination of an element handle and its associated element interface.
 * @note This should be specialized for top-level element interfaces to include their interface API.
 * @note Elements auto-release on destruction.
 */
template <typename BaseInterfaceType>
struct TTypedElement : public TTypedElementBase<BaseInterfaceType>
{
};
using FTypedElement = TTypedElement<UTypedElementInterface>;

template <typename OtherInterfaceType, typename ThisInterfaceType>
FORCEINLINE TTypedElement<OtherInterfaceType> CastTypedElement(const TTypedElement<ThisInterfaceType>& InElement)
{
	static_assert(sizeof(TTypedElement<ThisInterfaceType>) == sizeof(TTypedElement<OtherInterfaceType>), "All TTypedElement instances must be the same size for this cast implementation to work!");
	if (InElement && InElement.template HasInterface<OtherInterfaceType>())
	{
		return reinterpret_cast<const TTypedElement<OtherInterfaceType>&>(InElement);
	}
	return TTypedElement<OtherInterfaceType>();
}

template <typename OtherInterfaceType, typename ThisInterfaceType>
FORCEINLINE void CastTypedElement(const TTypedElement<ThisInterfaceType>& InElement, TTypedElement<OtherInterfaceType>& OutCastElement)
{
	static_assert(sizeof(TTypedElement<ThisInterfaceType>) == sizeof(TTypedElement<OtherInterfaceType>), "All TTypedElement instances must be the same size for this cast implementation to work!");
	if (InElement && InElement.template HasInterface<OtherInterfaceType>())
	{
		OutCastElement = reinterpret_cast<const TTypedElement<OtherInterfaceType>&>(InElement);
	}
	else
	{
		OutCastElement.Private_DestroyReleaseRef();
	}
}

template <typename OtherInterfaceType, typename ThisInterfaceType>
FORCEINLINE TTypedElement<OtherInterfaceType> CastTypedElement(TTypedElement<ThisInterfaceType>&& InElement)
{
	static_assert(sizeof(TTypedElement<ThisInterfaceType>) == sizeof(TTypedElement<OtherInterfaceType>), "All TTypedElement instances must be the same size for this cast implementation to work!");
	if (InElement && InElement.template HasInterface<OtherInterfaceType>())
	{
		return reinterpret_cast<TTypedElement<OtherInterfaceType>&&>(InElement);
	}
	return TTypedElement<OtherInterfaceType>();
}

template <typename OtherInterfaceType, typename ThisInterfaceType>
FORCEINLINE void CastTypedElement(TTypedElement<ThisInterfaceType>&& InElement, TTypedElement<OtherInterfaceType>& OutCastElement)
{
	static_assert(sizeof(TTypedElement<ThisInterfaceType>) == sizeof(TTypedElement<OtherInterfaceType>), "All TTypedElement instances must be the same size for this cast implementation to work!");
	if (InElement && InElement.template HasInterface<OtherInterfaceType>())
	{
		OutCastElement = reinterpret_cast<TTypedElement<OtherInterfaceType>&&>(InElement);
	}
	else
	{
		OutCastElement.Private_DestroyReleaseRef();
	}
}

template <typename OtherInterfaceType, typename ThisInterfaceType>
FORCEINLINE TTypedElement<OtherInterfaceType> CastTypedElementChecked(const TTypedElement<ThisInterfaceType>& InElement)
{
	static_assert(sizeof(TTypedElement<ThisInterfaceType>) == sizeof(TTypedElement<OtherInterfaceType>), "All TTypedElement instances must be the same size for this cast implementation to work!");
	if (InElement)
	{
		checkf(InElement.template HasInterface<OtherInterfaceType>(), TEXT("Element does not implement the required interface for this cast!"));
		return reinterpret_cast<const TTypedElement<OtherInterfaceType>&>(InElement);
	}
	return TTypedElement<OtherInterfaceType>();
}

template <typename OtherInterfaceType, typename ThisInterfaceType>
FORCEINLINE void CastTypedElementChecked(const TTypedElement<ThisInterfaceType>& InElement, TTypedElement<OtherInterfaceType>& OutCastElement)
{
	static_assert(sizeof(TTypedElement<ThisInterfaceType>) == sizeof(TTypedElement<OtherInterfaceType>), "All TTypedElement instances must be the same size for this cast implementation to work!");
	if (InElement)
	{
		checkf(InElement.template HasInterface<OtherInterfaceType>(), TEXT("Element does not implement the required interface for this cast!"));
		OutCastElement = reinterpret_cast<const TTypedElement<OtherInterfaceType>&>(InElement);
	}
	else
	{
		OutCastElement.Private_DestroyReleaseRef();
	}
}

template <typename OtherInterfaceType, typename ThisInterfaceType>
FORCEINLINE TTypedElement<OtherInterfaceType> CastTypedElementChecked(TTypedElement<ThisInterfaceType>&& InElement)
{
	static_assert(sizeof(TTypedElement<ThisInterfaceType>) == sizeof(TTypedElement<OtherInterfaceType>), "All TTypedElement instances must be the same size for this cast implementation to work!");
	if (InElement)
	{
		checkf(InElement.template HasInterface<OtherInterfaceType>(), TEXT("Element does not implement the required interface for this cast!"));
		return reinterpret_cast<TTypedElement<OtherInterfaceType>&&>(InElement);
	}
	return TTypedElement<OtherInterfaceType>();
}

template <typename OtherInterfaceType, typename ThisInterfaceType>
FORCEINLINE void CastTypedElementChecked(TTypedElement<ThisInterfaceType>&& InElement, TTypedElement<OtherInterfaceType>& OutCastElement)
{
	static_assert(sizeof(TTypedElement<ThisInterfaceType>) == sizeof(TTypedElement<OtherInterfaceType>), "All TTypedElement instances must be the same size for this cast implementation to work!");
	if (InElement)
	{
		checkf(InElement.template HasInterface<OtherInterfaceType>(), TEXT("Element does not implement the required interface for this cast!"));
		OutCastElement = reinterpret_cast<TTypedElement<OtherInterfaceType>&&>(InElement);
	}
	else
	{
		OutCastElement.Private_DestroyReleaseRef();
	}
}

/**
 * A representation of the owner of an element that includes its mutable handle data.
 * This type is returned when creating an element, and should be used to populate its internal payload data (if any).
 * @note Owners do not auto-release on destruction, and must be manually destroyed via their owner element registry.
 */
template <typename ElementDataType>
struct TTypedElementOwner
{
public:
	TTypedElementOwner() = default;

	TTypedElementOwner(const TTypedElementOwner&) = delete;
	TTypedElementOwner& operator=(const TTypedElementOwner&) = delete;

	TTypedElementOwner(TTypedElementOwner&& InOther)
		: Id(MoveTemp(InOther.Id))
		, DataPtr(InOther.DataPtr)
	{
		InOther.DataPtr = nullptr;
		checkSlow(!InOther.IsSet());
	}

	TTypedElementOwner& operator=(TTypedElementOwner&& InOther)
	{
		if (this != &InOther)
		{
			Id = MoveTemp(InOther.Id);
			DataPtr = InOther.DataPtr;

			InOther.DataPtr = nullptr;
			checkSlow(!InOther.IsSet());
		}
		return *this;
	}

	~TTypedElementOwner()
	{
		checkf(!IsSet(), TEXT("Element owner was still set during destruction! This will leak an element, and you should destroy this element prior to destruction!"));
	}

	FORCEINLINE explicit operator bool() const
	{
		return IsSet();
	}

	/**
	 * Has this owner been initialized to a valid element?
	 */
	FORCEINLINE bool IsSet() const
	{
		return Id.IsSet();
	}

	/**
	 * Get the ID that this element represents.
	 */
	FORCEINLINE const FTypedElementId& GetId() const
	{
		return Id;
	}

	/**
	 * Attempt to access the mutable data stored within this owner, returning null if it isn't possible.
	 */
	template <typename U = ElementDataType, std::enable_if_t<!std::is_void<U>::value>* = nullptr>
	FORCEINLINE U* GetData() const
	{
		static_assert(std::is_same<U, ElementDataType>::value, "Don't explicitly specify U!");
		return DataPtr
			? &DataPtr->GetMutableData()
			: nullptr;
	}

	/**
	 * Attempt to access the mutable data stored within this owner, asserting if it isn't possible.
	 */
	template <typename U = ElementDataType, std::enable_if_t<!std::is_void<U>::value>* = nullptr>
	FORCEINLINE U& GetDataChecked() const
	{
		static_assert(std::is_same<U, ElementDataType>::value, "Don't explicitly specify U!");
		checkf(DataPtr, TEXT("Handle data is null!"));
		return DataPtr->GetMutableData();
	}

	/**
	 * Acquire a copy of the ID that this element represents.
	 * @note This must be paired with a call to ReleaseId.
	 */
	FTypedElementId AcquireId() const
	{
		FTypedElementId ElementId;
		if (IsSet())
		{
			AddRef();
			ElementId.Private_InitializeNoRef(Id.GetTypeId(), Id.GetElementId());
		}
		return ElementId;
	}

	/**
	 * Release a copy of the ID that this element represents.
	 * @note This should have come from a call to AcquireId.
	 */
	void ReleaseId(FTypedElementId& InOutElementId) const
	{
		checkf(InOutElementId == Id, TEXT("Element ID does not match this owner!"));
		if (InOutElementId)
		{
			ReleaseRef();
			InOutElementId.Private_DestroyNoRef();
		}
	}

	/**
	 * Acquire a copy of the handle that this element represents.
	 * @note This must be paired with a call to ReleaseHandle (or a call to Release on the handle instance).
	 */
	FTypedElementHandle AcquireHandle() const
	{
		FTypedElementHandle ElementHandle;
		if (IsSet())
		{
			ElementHandle.Private_InitializeAddRef(Id.GetTypeId(), Id.GetElementId(), *DataPtr);
		}
		return ElementHandle;
	}

	/**
	 * Release a copy of the handle that this element represents.
	 * @note This should have come from a call to AcquireHandle.
	 */
	void ReleaseHandle(FTypedElementHandle& InOutElementHandle) const
	{
		checkf(InOutElementHandle.GetId() == Id, TEXT("Element handle ID does not match this owner!"));
		InOutElementHandle.Release();
	}

	FORCEINLINE friend bool operator==(const TTypedElementOwner& InLHS, const TTypedElementOwner& InRHS)
	{
		return InLHS.Id == InRHS.Id;
	}

	FORCEINLINE friend bool operator!=(const TTypedElementOwner& InLHS, const TTypedElementOwner& InRHS)
	{
		return !(InLHS == InRHS);
	}

	friend inline uint32 GetTypeHash(const TTypedElementOwner& InElementOwner)
	{
		return GetTypeHash(InElementOwner.Id);
	}

	FORCEINLINE void Private_InitializeNoRef(const FTypedHandleTypeId InTypeId, const FTypedHandleElementId InElementId, TTypedElementInternalData<ElementDataType>& InData)
	{
		Id.Private_InitializeNoRef(InTypeId, InElementId);
		DataPtr = &InData;
	}

	FORCEINLINE void Private_InitializeAddRef(const FTypedHandleTypeId InTypeId, const FTypedHandleElementId InElementId, TTypedElementInternalData<ElementDataType>& InData)
	{
		Private_InitializeNoRef(InTypeId, InElementId, InData);
		AddRef();
	}

	FORCEINLINE void Private_DestroyNoRef()
	{
		Id.Private_DestroyNoRef();
		DataPtr = nullptr;
	}

	FORCEINLINE void Private_DestroyReleaseRef()
	{
		ReleaseRef();
		Private_DestroyNoRef();
	}

	FORCEINLINE const TTypedElementInternalData<ElementDataType>* Private_GetInternalData() const
	{
		return DataPtr;
	}

private:
	FORCEINLINE void AddRef() const
	{
#if UE_TYPED_ELEMENT_HAS_REFCOUNT
		if (DataPtr)
		{
			DataPtr->AddRef();
		}
#endif	// UE_TYPED_ELEMENT_HAS_REFCOUNT
	}

	FORCEINLINE void ReleaseRef() const
	{
#if UE_TYPED_ELEMENT_HAS_REFCOUNT
		if (DataPtr)
		{
			DataPtr->ReleaseRef();
		}
#endif	// UE_TYPED_ELEMENT_HAS_REFCOUNT
	}

	FTypedElementId Id;
	TTypedElementInternalData<ElementDataType>* DataPtr = nullptr;
};
using FTypedElementOwner = TTypedElementOwner<void>;
