// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Misc/Attribute.h"
#include "Widgets/InvalidateWidgetReason.h"

#include <type_traits>

class FSlateWidgetClassData;

/**
 * Describes the static information about a Widget's type SlateAttributes.
 **/
class SLATECORE_API FSlateAttributeDescriptor
{
public:
	/**
	 * A EInvalidationWidgetReason Attribute 
	 * It can be explicitly initialize or can be a callback static function or lambda that returns the EInvalidationReason.
	 * The signature of the function takes a const SWidget& as argument.
	 */
	struct FInvalidateWidgetReasonAttribute
	{
		using Arg1Type = const class SWidget&;
		DECLARE_DELEGATE_RetVal_OneParam(EInvalidateWidgetReason, FGetter, Arg1Type);

		FInvalidateWidgetReasonAttribute(const FInvalidateWidgetReasonAttribute&) = default;
		FInvalidateWidgetReasonAttribute(FInvalidateWidgetReasonAttribute&&) = default;
		FInvalidateWidgetReasonAttribute& operator=(const FInvalidateWidgetReasonAttribute&) = default;
		FInvalidateWidgetReasonAttribute& operator=(FInvalidateWidgetReasonAttribute&&) = default;

		/** Default constructor. */
		explicit FInvalidateWidgetReasonAttribute(EInvalidateWidgetReason InReason)
			: Reason(InReason)
			, Getter()
		{
		}

		template<typename... PayloadTypes>
		explicit FInvalidateWidgetReasonAttribute(typename FGetter::template FStaticDelegate<PayloadTypes...>::FFuncPtr InFuncPtr, PayloadTypes&&... InputPayload)
			: Reason(EInvalidateWidgetReason::None)
			, Getter(FGetter::CreateStatic(InFuncPtr, Forward<PayloadTypes>(InputPayload)...))
		{
		}

		template<typename LambdaType, typename... PayloadTypes>
		explicit FInvalidateWidgetReasonAttribute(LambdaType&& InCallable, PayloadTypes&&... InputPayload)
			: Reason(EInvalidateWidgetReason::None)
			, Getter(FGetter::CreateLambda(InCallable, Forward<PayloadTypes>(InputPayload)...))
		{
		}

		bool IsBound() const
		{
			return Getter.IsBound();
		}

		EInvalidateWidgetReason Get(const SWidget& Widget) const
		{
			return IsBound() ? Getter.Execute(Widget) : Reason;
		}

	private:
		EInvalidateWidgetReason Reason;
		FGetter Getter;
	};

public:
	using OffsetType = uint32;

	/** The default sort order that define in which order attributes will be updated. */
	static uint32 DefaultSortOrder(OffsetType Offset) { return Offset * 100; }

	/** */
	struct FAttribute
	{
		FName Name;
		OffsetType Offset = 0;
		FName Prerequisite;
		uint32 SortOrder = 0;
		FInvalidateWidgetReasonAttribute InvalidationReason = FInvalidateWidgetReasonAttribute(EInvalidateWidgetReason::None);
		bool bIsMemberAttribute = false;
		bool bIsPrerequisiteAlsoADependency = false;
		bool bIsADependencyForSomeoneElse = false;
		bool bAffectVisibility = false;
	};

	/** Internal class to initialize the SlateAttributeDescriptor (Add attributes or modify existing attributes). */
	struct SLATECORE_API FInitializer
	{
	private:
		friend FSlateWidgetClassData;
		FInitializer(FSlateAttributeDescriptor& InDescriptor);
		FInitializer(FSlateAttributeDescriptor& InDescriptor, const FSlateAttributeDescriptor& ParentDescriptor);
		FInitializer(const FInitializer&) = delete;
		FInitializer& operator=(const FInitializer&) = delete;

	public:
		~FInitializer();

		struct SLATECORE_API FAttributeEntry
		{
			FAttributeEntry(FSlateAttributeDescriptor& Descriptor, int32 InAttributeIndex);

			/**
			 * Update the attribute after the prerequisite.
			 * The order is guaranteed but other attributes may be updated in between.
			 * No order is guaranteed if the prerequisite or this property is updated manually.
			 */
			FAttributeEntry& UpdatePrerequisite(FName Prerequisite);

			/**
			 * The property only needs to be updated when the dependency changes inside the update loop.
			 * The property can still be set/updated manually.
			 * If the dependency is updated manually, then the property will be updated in the next update loop.
			 * It will implicitly set a prerequisite.
			 */
			FAttributeEntry& UpdateDependency(FName Dependency);

			/**
			 * The attribute affect the visibility of the widget.
			 * We only update the attributes that can change the visibility of the widget when the widget is collapsed.
			 * Attributes that affect visibility must have the Visibility attribute as a Prerequisite or the Visibility attribute must have it as a Prerequisite.
			 */
			FAttributeEntry& AffectVisibility();

		private:
			FSlateAttributeDescriptor& Descriptor;
			int32 AttributeIndex;
		};

		FAttributeEntry AddMemberAttribute(FName AttributeName, OffsetType Offset, const FInvalidateWidgetReasonAttribute& ReasonGetter);
		FAttributeEntry AddMemberAttribute(FName AttributeName, OffsetType Offset, FInvalidateWidgetReasonAttribute&& ReasonGetter);

		/** Change the InvalidationReason of an attribute defined in a base class. */
		void OverrideInvalidationReason(FName AttributeName, const FInvalidateWidgetReasonAttribute& Reason);
		/** Change the InvalidationReason of an attribute defined in a base class. */
		void OverrideInvalidationReason(FName AttributeName, FInvalidateWidgetReasonAttribute&& Reason);

		/** Change the update type of an attribute defined in a base class. */
		void SetAffectVisibility(FName AttributeName, bool bAffectVisibility);


	private:
		FSlateAttributeDescriptor& Descriptor;
	};

	/** @returns the number of Attributes registered. */
	int32 GetAttributeNum() const { return Attributes.Num(); }

	/** @returns the Attribute at the index previously found with IndexOfMemberAttribute */
	const FAttribute& GetAttributeAtIndex(int32 Index) const;

	/** @returns the Attribute with the corresponding name. */
	const FAttribute* FindAttribute(FName AttributeName) const;

	/** @returns the index of a SlateAttribute that have the corresponding memory offset. */
	int32 IndexOfMemberAttribute(OffsetType AttributeOffset) const;

	/** @returns the index of a SlateAttribute that have the corresponding memory offset. */
	int32 IndexOfMemberAttribute(FName AttributeName) const;

	/** @returns the Attribute of a SlateAttribute that have the corresponding memory offset. */
	const FAttribute* FindMemberAttribute(OffsetType AttributeOffset) const;

	/** Iterator over each dependency this attribute is responsible of. */
	template<typename Predicate>
	void ForEachDependentsOn(FAttribute const& Attribute, Predicate Pred) const
	{
		checkf(&Attribute >= Attributes.GetData() && &Attribute <= Attributes.GetData()+Attributes.Num()
			, TEXT("The attribute is not part of this Descriptor."));
		if (Attribute.bIsADependencyForSomeoneElse)
		{
			ForEachDependentsOfImpl(Attribute.Name, (int32)(&Attribute - Attributes.GetData()), Pred);
		}
	}

private:
	FAttribute* FindAttribute(FName AttributeName);

	FInitializer::FAttributeEntry AddMemberAttribute(FName AttributeName, OffsetType Offset, FInvalidateWidgetReasonAttribute ReasonGetter);
	void OverrideInvalidationReason(FName AttributeName, FInvalidateWidgetReasonAttribute ReasonGetter);
	void SetPrerequisite(FAttribute& Attribute, FName Prerequisite, bool bSetAsDependency);
	void SetAffectVisibility(FAttribute& Attribute, bool bUpdate);

	template<typename Predicate>
	void ForEachDependentsOfImpl(FName const& LookForName, int32 Index, Predicate& Pred) const
	{
		++Index;
		for (; Index < Attributes.Num(); ++Index)
		{
			FAttribute const& Other = Attributes[Index];
			if (Other.Prerequisite == LookForName && Other.bIsPrerequisiteAlsoADependency)
			{
				Pred(Index);
				if (Other.bIsADependencyForSomeoneElse)
				{
					ForEachDependentsOfImpl(Other.Name, Index, Pred);
				}
			}
		}
	}

private:
	TArray<FAttribute> Attributes;
};

/**
 * Add a TSlateAttribute to the descriptor.
 * @param _Initializer The FSlateAttributeInitializer from the PrivateRegisterAttributes function.
 * @param _Property The TSlateAttribute property
 * @param _Reason The EInvalidationWidgetReason or a static function/lambda that takes a const SWidget& and that returns the invalidation reason.
 */
#define SLATE_ADD_MEMBER_ATTRIBUTE_DEFINITION_WITH_NAME(_Initializer, _Name, _Property, _Reason) \
		static_assert(decltype(_Property)::IsMemberType, "The SlateProperty is not a TSlateAttribute. Do not use SLATE_ADD_MEMBER_ATTRIBUTE_DEFINITION"); \
		static_assert(!decltype(_Property)::HasDefinedInvalidationReason, "When implementing the SLATE_DECLARE_WIDGET pattern, use TSlateAttribute without the invalidation reason."); \
		static_assert(!std::is_same<decltype(_Reason), EInvalidateWidgetReason>::value || FSlateAttributeBase::IsInvalidateWidgetReasonSupported(_Reason), "The invalidation is not supported by the SlateAttribute."); \
		_Initializer.AddMemberAttribute(_Name, STRUCT_OFFSET(PrivateThisType, _Property), FSlateAttributeDescriptor::FInvalidateWidgetReasonAttribute{_Reason})

#define SLATE_ADD_MEMBER_ATTRIBUTE_DEFINITION(_Initializer, _Property, _Reason) \
	SLATE_ADD_MEMBER_ATTRIBUTE_DEFINITION_WITH_NAME(_Initializer, GET_MEMBER_NAME_CHECKED(PrivateThisType, _Property), _Property, _Reason)

using FSlateAttributeInitializer = FSlateAttributeDescriptor::FInitializer;
