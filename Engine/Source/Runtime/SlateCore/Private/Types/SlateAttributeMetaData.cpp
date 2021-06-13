// Copyright Epic Games, Inc. All Rights Reserved.

#include "Types/SlateAttributeMetaData.h"

#include "Algo/BinarySearch.h"
#include "Layout/Children.h"
#include "Types/ReflectionMetadata.h"
#include "Widgets/SWidget.h"

#include <limits>


const FSlateAttributeMetaData::FGetterItem::FAttributeIndex FSlateAttributeMetaData::FGetterItem::InvalidAttributeIndex = std::numeric_limits<FSlateAttributeMetaData::FGetterItem::FAttributeIndex>::max();


namespace Private
{
	FSlateAttributeDescriptor::OffsetType FindOffet(const SWidget& OwningWidget, const FSlateAttributeBase& Attribute)
	{
		UPTRINT Offset = (UPTRINT)(&Attribute) - (UPTRINT)(&OwningWidget);
		ensure(Offset <= std::numeric_limits<FSlateAttributeDescriptor::OffsetType>::max());
		return (FSlateAttributeDescriptor::OffsetType)(Offset);
	}
}


FSlateAttributeMetaData* FSlateAttributeMetaData::FindMetaData(const SWidget& OwningWidget)
{
	if (OwningWidget.HasRegisteredSlateAttribute())
	{
		check(OwningWidget.MetaData.Num() > 0);
		const TSharedRef<ISlateMetaData>& SlateMetaData = OwningWidget.MetaData[0];
		check(SlateMetaData->IsOfType<FSlateAttributeMetaData>());
		return &(static_cast<FSlateAttributeMetaData&>(SlateMetaData.Get()));
	}
#if WITH_SLATE_DEBUGGING
	else if (OwningWidget.MetaData.Num() > 0)
	{
		const TSharedRef<ISlateMetaData>& SlateMetaData = OwningWidget.MetaData[0];
		if (SlateMetaData->IsOfType<FSlateAttributeMetaData>())
		{
			ensureMsgf(false, TEXT("bHasRegisteredSlateAttribute should be set on the SWidget '%s'"), *FReflectionMetaData::GetWidgetDebugInfo(OwningWidget));
			return &(static_cast<FSlateAttributeMetaData&>(SlateMetaData.Get()));
		}
	}
#endif
	return nullptr;
}


void FSlateAttributeMetaData::RegisterAttribute(SWidget& OwningWidget, FSlateAttributeBase& Attribute, ESlateAttributeType AttributeType, TUniquePtr<ISlateAttributeGetter>&& Wrapper)
{
	if (FSlateAttributeMetaData* AttributeMetaData = FSlateAttributeMetaData::FindMetaData(OwningWidget))
	{
		AttributeMetaData->RegisterAttributeImpl(OwningWidget, Attribute, AttributeType, MoveTemp(Wrapper));
	}
	else
	{
		TSharedRef<FSlateAttributeMetaData> NewAttributeMetaData = MakeShared<FSlateAttributeMetaData>();
		NewAttributeMetaData->RegisterAttributeImpl(OwningWidget, Attribute, AttributeType, MoveTemp(Wrapper));
		OwningWidget.bHasRegisteredSlateAttribute = true;
		OwningWidget.MetaData.Insert(NewAttributeMetaData, 0);
		if (OwningWidget.IsConstructed() && OwningWidget.IsAttributesUpdatesEnabled())
		{
			OwningWidget.Invalidate(EInvalidateWidgetReason::AttributeRegistration);
		}
	}
}


void FSlateAttributeMetaData::RegisterAttributeImpl(SWidget& OwningWidget, FSlateAttributeBase& Attribute, ESlateAttributeType AttributeType, TUniquePtr<ISlateAttributeGetter>&& Getter)
{
	const int32 FoundIndex = IndexOfAttribute(Attribute);
	if (FoundIndex != INDEX_NONE)
	{
		Attributes[FoundIndex].Getter = MoveTemp(Getter);
		Attributes[FoundIndex].bUpdatedOnce = false;
	}
	else
	{
		if (AttributeType == ESlateAttributeType::Member)
		{
			// MemberAttribute are optional for now but will be needed in the future
			const FSlateAttributeDescriptor::OffsetType Offset = Private::FindOffet(OwningWidget, Attribute);
			const FSlateAttributeDescriptor& Descriptor = OwningWidget.GetWidgetClass().GetAttributeDescriptor();
			const int32 FoundMemberAttributeIndex = Descriptor.IndexOfMemberAttribute(Offset);

			if (FoundMemberAttributeIndex != INDEX_NONE)
			{
				FSlateAttributeDescriptor::FAttribute const& FoundAttribute = Descriptor.GetAttributeAtIndex(FoundMemberAttributeIndex);
				check(FoundMemberAttributeIndex < std::numeric_limits<FGetterItem::FAttributeIndex>::max());

				const int32 InsertLocation = Algo::LowerBoundBy(Attributes, FoundAttribute.SortOrder, [](const FGetterItem& Item) { return Item.SortOrder; }, TLess<>());
				FGetterItem& GetterItem = Attributes.Insert_GetRef({ &Attribute, FoundAttribute.SortOrder, MoveTemp(Getter), (FGetterItem::FAttributeIndex)FoundMemberAttributeIndex }, InsertLocation);
				GetterItem.AttributeType = ESlateAttributeType::Member;

				// Do I have dependency or am I a dependency
				if (!FoundAttribute.Prerequisite.IsNone() && FoundAttribute.bIsPrerequisiteAlsoADependency) 
				{
					// I can only be updated if the prerequisite is updated
					const int32 FoundDependencyAttributeIndex = Descriptor.IndexOfMemberAttribute(FoundAttribute.Prerequisite);
					if (FoundDependencyAttributeIndex != INDEX_NONE)
					{
						check(FoundDependencyAttributeIndex < std::numeric_limits<FGetterItem::FAttributeIndex>::max());
						GetterItem.CachedAttributeDependencyIndex = (FGetterItem::FAttributeIndex)FoundDependencyAttributeIndex;
					}
				}
				GetterItem.bIsADependencyForSomeoneElse = FoundAttribute.bIsADependencyForSomeoneElse;
				GetterItem.bAffectVisibility = FoundAttribute.bAffectVisibility;
				if (GetterItem.bAffectVisibility)
				{
					++AffectVisibilityCounter;
				}
			}
			else
			{
				const uint32 SortOrder = FSlateAttributeDescriptor::DefaultSortOrder(Offset);

				const  int32 InsertLocation = Algo::LowerBoundBy(Attributes, SortOrder, [](const FGetterItem& Item) { return Item.SortOrder; }, TLess<>());
				FGetterItem& GetterItem = Attributes.Insert_GetRef({&Attribute, SortOrder, MoveTemp(Getter)}, InsertLocation);
				GetterItem.AttributeType = ESlateAttributeType::Member;
			}
		}
		else if (AttributeType == ESlateAttributeType::Managed)
		{
			const uint32 ManagedSortOrder = std::numeric_limits<uint32>::max();
			FGetterItem& GetterItem = Attributes.Emplace_GetRef(&Attribute, ManagedSortOrder, MoveTemp(Getter));
			GetterItem.AttributeType = ESlateAttributeType::Managed;
		}
		else
		{
			ensureMsgf(false, TEXT("The SlateAttributeType is not supported"));
		}
	}
}


bool FSlateAttributeMetaData::UnregisterAttribute(SWidget& OwningWidget, const FSlateAttributeBase& Attribute)
{
	if (FSlateAttributeMetaData* AttributeMetaData = FSlateAttributeMetaData::FindMetaData(OwningWidget))
	{
		const bool bResult = AttributeMetaData->UnregisterAttributeImpl(Attribute);
		if (AttributeMetaData->Attributes.Num() == 0)
		{
			check(bResult); // if the num is 0 then we should have remove an item.
			OwningWidget.bHasRegisteredSlateAttribute = false;
			OwningWidget.MetaData.RemoveAtSwap(0);
			if (OwningWidget.IsConstructed() && OwningWidget.IsAttributesUpdatesEnabled())
			{
				OwningWidget.Invalidate(EInvalidateWidgetReason::AttributeRegistration);
			}
		}
		return bResult;
	}
	return false;
}


bool FSlateAttributeMetaData::UnregisterAttributeImpl(const FSlateAttributeBase& Attribute)
{
	const int32 FoundIndex = IndexOfAttribute(Attribute);
	if (FoundIndex != INDEX_NONE)
	{
		if (Attributes[FoundIndex].bAffectVisibility)
		{
			check(AffectVisibilityCounter > 0);
			--AffectVisibilityCounter;
		}
		Attributes.RemoveAt(FoundIndex); // keep the order valid
		return true;
	}
	return false;
}


TArray<FName> FSlateAttributeMetaData::GetAttributeNames(const SWidget& OwningWidget)
{
	TArray<FName> Names;
	if (FSlateAttributeMetaData* AttributeMetaData = FSlateAttributeMetaData::FindMetaData(OwningWidget))
	{
		Names.Reserve(AttributeMetaData->Attributes.Num());
		for (const FGetterItem& Getter : AttributeMetaData->Attributes)
		{
			const FName Name = Getter.GetAttributeName(OwningWidget);
			if (Name.IsValid())
			{
				Names.Add(Name);
			}
		}
	}
	return Names;
}


FSlateAttributeMetaData::FGetterItem::FInvalidationDetail FSlateAttributeMetaData::FGetterItem::GetInvalidationDetail(const SWidget& OwningWidget, EInvalidateWidgetReason Reason) const
{
	if (CachedAttributeDescriptorIndex != FGetterItem::InvalidAttributeIndex)
	{
		const FSlateAttributeDescriptor::FAttribute& DescriptorAttribute = OwningWidget.GetWidgetClass().GetAttributeDescriptor().GetAttributeAtIndex(CachedAttributeDescriptorIndex);
		return FInvalidationDetail{&DescriptorAttribute.OnValueChanged, DescriptorAttribute.InvalidationReason.Get(OwningWidget)};
	}
	return FInvalidationDetail{nullptr, Reason};
}


FName FSlateAttributeMetaData::FGetterItem::GetAttributeName(const SWidget& OwningWidget) const
{
	if (CachedAttributeDescriptorIndex != FGetterItem::InvalidAttributeIndex)
	{
		const FSlateAttributeDescriptor::FAttribute& DescriptorAttribute = OwningWidget.GetWidgetClass().GetAttributeDescriptor().GetAttributeAtIndex(CachedAttributeDescriptorIndex);
		return DescriptorAttribute.Name;
	}
	return FName();
}


void FSlateAttributeMetaData::InvalidateWidget(SWidget& OwningWidget, const FSlateAttributeBase& Attribute, ESlateAttributeType AttributeType, EInvalidateWidgetReason Reason)
{
	// The widget is in the construction phase or is building in the WidgetList.
	//It's already invalidated... no need to keep invalidating it.
	//N.B. no needs to set the bUpatedManually in this case because
	//	1. they are in construction, so they will all be called anyway
	//	2. they are in WidgetList, so the SlateAttribute.Set will not be called
	if (!OwningWidget.IsConstructed())
	{
		return;
	}

	const FSlateAttributeDescriptor::FAttributeValueChangedDelegate* OnValueChangedCallback = nullptr;

	if (FSlateAttributeMetaData* AttributeMetaData = FSlateAttributeMetaData::FindMetaData(OwningWidget))
	{
		const int32 FoundIndex = AttributeMetaData->IndexOfAttribute(Attribute);
		if (FoundIndex != INDEX_NONE)
		{
			FGetterItem& GetterItem = AttributeMetaData->Attributes[FoundIndex];
			{
				const FGetterItem::FInvalidationDetail Detail = GetterItem.GetInvalidationDetail(OwningWidget, Reason);
				OnValueChangedCallback = Detail.Get<0>();
				Reason = Detail.Get<1>() | AttributeMetaData->CachedInvalidationReason;
				AttributeMetaData->CachedInvalidationReason = EInvalidateWidgetReason::None;
			}

			// The dependency attribute need to be updated in the update loop (note that it may not be registered yet)
			if (GetterItem.bIsADependencyForSomeoneElse)
			{
				GetterItem.bUpdatedManually = true;
				AttributeMetaData->SetNeedToResetFlag(FoundIndex);
			}
		}
		// Not registered/bound but may be defined in the Descriptor
		else if (AttributeType == ESlateAttributeType::Member)
		{
			FSlateAttributeDescriptor const& AttributeDescriptor = OwningWidget.GetWidgetClass().GetAttributeDescriptor();
			const FSlateAttributeDescriptor::OffsetType Offset = Private::FindOffet(OwningWidget, Attribute);
			if (FSlateAttributeDescriptor::FAttribute const* FoundAttribute = AttributeDescriptor.FindMemberAttribute(Offset))
			{
				OnValueChangedCallback = &FoundAttribute->OnValueChanged;
				Reason = FoundAttribute->InvalidationReason.Get(OwningWidget) | AttributeMetaData->CachedInvalidationReason;
				AttributeMetaData->CachedInvalidationReason = EInvalidateWidgetReason::None;

				if (FoundAttribute->bIsADependencyForSomeoneElse)
				{
					// Find if that dependency is registered, if not it is ok because every attribute is updated at least once
					// Set UpdatedOnce to false to force a new update.
					AttributeDescriptor.ForEachDependentsOn(*FoundAttribute, [AttributeMetaData](int32 DependencyIndex)
					{
						FGetterItem* FoundOther = AttributeMetaData->Attributes.FindByPredicate([DependencyIndex](FGetterItem const& Other)
						{
							check(DependencyIndex != INDEX_NONE);
							check(DependencyIndex < std::numeric_limits<FGetterItem::FAttributeIndex>::max());
							return Other.CachedAttributeDescriptorIndex == (FGetterItem::FAttributeIndex)DependencyIndex;
						});
						if (FoundOther)
						{
							FoundOther->bUpdatedOnce = false;
						}
					});
				}
			}
		}
	}
	else if (AttributeType == ESlateAttributeType::Member)
	{
		const FSlateAttributeDescriptor::OffsetType Offset = Private::FindOffet(OwningWidget, Attribute);
		if (FSlateAttributeDescriptor::FAttribute const* FoundAttribute = OwningWidget.GetWidgetClass().GetAttributeDescriptor().FindMemberAttribute(Offset))
		{
			Reason = FoundAttribute->InvalidationReason.Get(OwningWidget);
			OnValueChangedCallback = &FoundAttribute->OnValueChanged;
		}
	}

#if WITH_SLATE_DEBUGGING
	ensureAlwaysMsgf(FSlateAttributeBase::IsInvalidateWidgetReasonSupported(Reason), TEXT("%s is not an EInvalidateWidgetReason supported by SlateAttribute."), *LexToString(Reason));
#endif

	OwningWidget.Invalidate(Reason);
	if (OnValueChangedCallback)
	{
		OnValueChangedCallback->ExecuteIfBound(OwningWidget);
	}
}


void FSlateAttributeMetaData::UpdateAllAttributes(SWidget& OwningWidget, EInvalidationPermission InvalidationStyle)
{
	if (FSlateAttributeMetaData* AttributeMetaData = FSlateAttributeMetaData::FindMetaData(OwningWidget))
	{
		AttributeMetaData->UpdateAttributesImpl(OwningWidget, InvalidationStyle, 0, AttributeMetaData->Attributes.Num());
		if (AttributeMetaData->ResetFlag != EResetFlags::None)
		{
			for (FGetterItem& GetterItem : AttributeMetaData->Attributes)
			{
				GetterItem.bUpdatedManually = false;
				GetterItem.bUpdatedThisFrame = false;
			}
			AttributeMetaData->ResetFlag = EResetFlags::None;
		}
	}
}


void FSlateAttributeMetaData::UpdateOnlyVisibilityAttributes(SWidget& OwningWidget, EInvalidationPermission InvalidationStyle)
{
	if (FSlateAttributeMetaData* AttributeMetaData = FSlateAttributeMetaData::FindMetaData(OwningWidget))
	{
		if (AttributeMetaData->AffectVisibilityCounter > 0)
		{
			const int32 StartIndex = 0;
			const int32 EndIndex = AttributeMetaData->AffectVisibilityCounter;
			AttributeMetaData->UpdateAttributesImpl(OwningWidget, InvalidationStyle, StartIndex, EndIndex);
			if (EnumHasAllFlags(AttributeMetaData->ResetFlag, EResetFlags::NeedToReset_OnlyVisibility))
			{
				for (int32 Index = StartIndex; Index < EndIndex; ++Index)
				{
					FGetterItem& GetterItem = AttributeMetaData->Attributes[Index];
					GetterItem.bUpdatedManually = false;
					GetterItem.bUpdatedThisFrame = false;
				}
				EnumRemoveFlags(AttributeMetaData->ResetFlag, EResetFlags::NeedToReset_OnlyVisibility);
			}
		}
	}
}


void FSlateAttributeMetaData::UpdateExceptVisibilityAttributes(SWidget& OwningWidget, EInvalidationPermission InvalidationStyle)
{
	if (FSlateAttributeMetaData* AttributeMetaData = FSlateAttributeMetaData::FindMetaData(OwningWidget))
	{
		if (AttributeMetaData->AffectVisibilityCounter < AttributeMetaData->Attributes.Num())
		{
			const int32 StartIndex = AttributeMetaData->AffectVisibilityCounter;
			const int32 EndIndex = AttributeMetaData->Attributes.Num();
			AttributeMetaData->UpdateAttributesImpl(OwningWidget, InvalidationStyle, StartIndex, EndIndex);
			if (EnumHasAllFlags(AttributeMetaData->ResetFlag, EResetFlags::NeedToReset_ExceptVisibility))
			{
				for (int32 Index = StartIndex; Index < EndIndex; ++Index)
				{
					FGetterItem& GetterItem = AttributeMetaData->Attributes[Index];
					GetterItem.bUpdatedManually = false;
					GetterItem.bUpdatedThisFrame = false;
				}
				EnumRemoveFlags(AttributeMetaData->ResetFlag, EResetFlags::NeedToReset_ExceptVisibility);
			}
		}
	}
}


void FSlateAttributeMetaData::UpdateChildrenOnlyVisibilityAttributes(SWidget& OwningWidget, EInvalidationPermission InvalidationStyle, bool bRecursive)
{
	OwningWidget.GetChildren()->ForEachWidget([InvalidationStyle, bRecursive](SWidget& Child)
		{
			UpdateOnlyVisibilityAttributes(Child, InvalidationStyle);
			if (bRecursive)
			{
				UpdateChildrenOnlyVisibilityAttributes(Child, InvalidationStyle, bRecursive);
			}
		});
}


void FSlateAttributeMetaData::UpdateAttributesImpl(SWidget& OwningWidget, EInvalidationPermission InvalidationStyle, int32 StartIndex, int32 IndexNum)
{
	bool bInvalidateIfNeeded = (InvalidationStyle == EInvalidationPermission::AllowInvalidation) || (InvalidationStyle == EInvalidationPermission::AllowInvalidationIfConstructed && OwningWidget.IsConstructed());
	bool bAllowInvalidation = bInvalidateIfNeeded || InvalidationStyle == EInvalidationPermission::DelayInvalidation;
	EInvalidateWidgetReason InvalidationReason = EInvalidateWidgetReason::None;
	for (int32 Index = StartIndex; Index < IndexNum; ++Index)
	{
		FGetterItem& GetterItem = Attributes[Index];

		// Update every attribute at least once.
		//Check if it has a dependency and if it was updated this frame (it could be from an UpdateNow)
		if (GetterItem.CachedAttributeDependencyIndex != FGetterItem::InvalidAttributeIndex && GetterItem.bUpdatedOnce)
		{
			// Note that the dependency is maybe not registered and the attribute may have been invalidated manually

			// Because the list is sorted, the dependency needs to be before this element.
			bool bShouldUpdate = false;
			bool bFound = false;
			for (int32 OtherIndex = Index-1; OtherIndex >= 0; --OtherIndex)
			{
				const FGetterItem& OtherGetterItem = Attributes[OtherIndex];
				if (OtherGetterItem.CachedAttributeDescriptorIndex == GetterItem.CachedAttributeDependencyIndex)
				{
					bFound = true;
					bShouldUpdate = OtherGetterItem.bUpdatedThisFrame || OtherGetterItem.bUpdatedManually;
					break;
				}
			}

			if (!bShouldUpdate)
			{
				continue;
			}
		}

		ISlateAttributeGetter::FUpdateAttributeResult Result = GetterItem.Getter->UpdateAttribute(OwningWidget);
		GetterItem.bUpdatedOnce = true;
		GetterItem.bUpdatedThisFrame = Result.bInvalidationRequested;
		if (Result.bInvalidationRequested && bAllowInvalidation)
		{
			SetNeedToResetFlag(Index);
			const FGetterItem::FInvalidationDetail Detail = GetterItem.GetInvalidationDetail(OwningWidget, Result.InvalidationReason);
			if (Detail.Get<0>())
			{
				Detail.Get<0>()->ExecuteIfBound(OwningWidget);
			}
			InvalidationReason |= Detail.Get<1>();
		}
	}

	if (bInvalidateIfNeeded)
	{
#if WITH_SLATE_DEBUGGING
		ensureAlwaysMsgf(FSlateAttributeBase::IsInvalidateWidgetReasonSupported(InvalidationReason | CachedInvalidationReason), TEXT("%s is not an EInvalidateWidgetReason supported by SlateAttribute."), *LexToString(InvalidationReason | CachedInvalidationReason));
#endif
		OwningWidget.Invalidate(InvalidationReason | CachedInvalidationReason);
		CachedInvalidationReason = EInvalidateWidgetReason::None;
	}
	else if (InvalidationStyle == EInvalidationPermission::DelayInvalidation)
	{
		CachedInvalidationReason |= InvalidationReason;
	}
	else if (InvalidationStyle == EInvalidationPermission::DenyAndClearDelayedInvalidation)
	{
		CachedInvalidationReason = EInvalidateWidgetReason::None;
	}
}


void FSlateAttributeMetaData::UpdateAttribute(SWidget& OwningWidget, FSlateAttributeBase& Attribute)
{
	if (FSlateAttributeMetaData* AttributeMetaData = FSlateAttributeMetaData::FindMetaData(OwningWidget))
	{
		const int32 FoundIndex = AttributeMetaData->IndexOfAttribute(Attribute);
		if (FoundIndex != INDEX_NONE)
		{
			FGetterItem& GetterItem = AttributeMetaData->Attributes[FoundIndex];
			GetterItem.bUpdatedOnce = true;
			check(GetterItem.Getter.Get());
			ISlateAttributeGetter::FUpdateAttributeResult Result = GetterItem.Getter->UpdateAttribute(OwningWidget);
			if (Result.bInvalidationRequested)
			{
				if (OwningWidget.IsConstructed())
				{
					const FGetterItem::FInvalidationDetail Detail = GetterItem.GetInvalidationDetail(OwningWidget, Result.InvalidationReason);
					EInvalidateWidgetReason Reason = Detail.Get<1>() | AttributeMetaData->CachedInvalidationReason;
#if WITH_SLATE_DEBUGGING
					ensureAlwaysMsgf(FSlateAttributeBase::IsInvalidateWidgetReasonSupported(Reason), TEXT("%s is not an EInvalidateWidgetReason supported by SlateAttribute."), *LexToString(Reason));
#endif
					OwningWidget.Invalidate(Reason);
					if (Detail.Get<0>())
					{
						Detail.Get<0>()->ExecuteIfBound(OwningWidget);
					}
					AttributeMetaData->CachedInvalidationReason = EInvalidateWidgetReason::None;
				}

				// The dependency attribute need to be updated in the update loop (note that it may not be registered yet)
				if (GetterItem.bIsADependencyForSomeoneElse)
				{
					GetterItem.bUpdatedManually = true;
					AttributeMetaData->SetNeedToResetFlag(FoundIndex);
				}
			}
		}
	}
}


bool FSlateAttributeMetaData::IsAttributeBound(const SWidget& OwningWidget, const FSlateAttributeBase& Attribute)
{
	if (FSlateAttributeMetaData* AttributeMetaData = FSlateAttributeMetaData::FindMetaData(OwningWidget))
	{
		return AttributeMetaData->IndexOfAttribute(Attribute) != INDEX_NONE;
	}
	return false;
}


SlateAttributePrivate::ISlateAttributeGetter* FSlateAttributeMetaData::GetAttributeGetter(const SWidget& OwningWidget, const FSlateAttributeBase& Attribute)
{
	if (FSlateAttributeMetaData* AttributeMetaData = FSlateAttributeMetaData::FindMetaData(OwningWidget))
	{
		const int32 FoundIndex = AttributeMetaData->IndexOfAttribute(Attribute);
		if (FoundIndex != INDEX_NONE)
		{
			return AttributeMetaData->Attributes[FoundIndex].Getter.Get();
		}
	}
	return nullptr;
}


FDelegateHandle FSlateAttributeMetaData::GetAttributeGetterHandle(const SWidget& OwningWidget, const FSlateAttributeBase& Attribute)
{
	if (FSlateAttributeMetaData* AttributeMetaData = FSlateAttributeMetaData::FindMetaData(OwningWidget))
	{
		const int32 FoundIndex = AttributeMetaData->IndexOfAttribute(Attribute);
		if (FoundIndex != INDEX_NONE)
		{
			return AttributeMetaData->Attributes[FoundIndex].Getter->GetDelegateHandle();
		}
	}
	return FDelegateHandle();
}


void FSlateAttributeMetaData::MoveAttribute(const SWidget& OwningWidget, FSlateAttributeBase& NewAttribute, ESlateAttributeType AttributeType, const FSlateAttributeBase* PreviousAttribute)
{
	checkf(AttributeType == ESlateAttributeType::Managed, TEXT("TSlateAttribute cannot be moved. This should be already prevented in SlateAttribute.h"));
	if (FSlateAttributeMetaData* AttributeMetaData = FSlateAttributeMetaData::FindMetaData(OwningWidget))
	{
		const int32 FoundIndex = AttributeMetaData->Attributes.IndexOfByPredicate([PreviousAttribute](const FGetterItem& Item) { return Item.Attribute == PreviousAttribute; });
		if (FoundIndex != INDEX_NONE)
		{
			AttributeMetaData->Attributes[FoundIndex].Attribute = &NewAttribute;
			AttributeMetaData->Attributes[FoundIndex].Getter->SetAttribute(NewAttribute);
			//Attributes.Sort(); // Managed are always at the end and there order is not realiable.
		}
	}
}
