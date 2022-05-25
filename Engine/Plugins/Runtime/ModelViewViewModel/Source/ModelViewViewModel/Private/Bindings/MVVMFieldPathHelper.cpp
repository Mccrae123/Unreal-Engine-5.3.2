// Copyright Epic Games, Inc. All Rights Reserved.

#include "Bindings/MVVMFieldPathHelper.h"

#include "Bindings/MVVMBindingHelper.h"
#include "Misc/MemStack.h"

namespace UE::MVVM::FieldPathHelper
{

namespace Private
{

static const FName NAME_BlueprintGetter = "BlueprintGetter";
static const FName NAME_BlueprintSetter = "BlueprintSetter";


TValueOrError<UStruct*, FString> FindContainer(const FProperty* Property)
{
	const FObjectPropertyBase* ObjectProperty = CastField<const FObjectPropertyBase>(Property);
	const FStructProperty* StructProperty = CastField<const FStructProperty>(Property);

	if (ObjectProperty)
	{
		return MakeValue(ObjectProperty->PropertyClass);
	}
	else if (StructProperty)
	{
		return MakeValue(StructProperty->Struct);
	}
	return MakeError(FString::Printf(TEXT("Only object or struct properties can be used as source paths. '%s' is a '%s'."), *Property->GetName(), *Property->GetClass()->GetName()));
}


TValueOrError<FMVVMConstFieldVariant, FString> TransformWithAccessor(UStruct* CurrentContainer, FMVVMConstFieldVariant CurrentField, bool bForReading)
{
#if WITH_EDITORONLY_DATA
	if (bForReading)
	{
		if (!CurrentField.GetProperty()->HasGetter())
		{
			const FString& BlueprintGetter = CurrentField.GetProperty()->GetMetaData(Private::NAME_BlueprintGetter);
			if (!BlueprintGetter.IsEmpty())
			{
				FMVVMConstFieldVariant NewField = BindingHelper::FindFieldByName(CurrentContainer, FMVVMBindingName(*BlueprintGetter));
				if (NewField.IsFunction())
				{
					CurrentField = NewField;
				}
				else
				{
					return MakeError(FString::Printf(TEXT("The BlueprintGetter '%s' could not be found on object '%s'."), *BlueprintGetter, *CurrentContainer->GetName()));
				}
			}
		}
	}
	else
	{
		if (!CurrentField.GetProperty()->HasSetter())
		{
			const FString& BlueprintSetter = CurrentField.GetProperty()->GetMetaData(Private::NAME_BlueprintSetter);
			if (!BlueprintSetter.IsEmpty())
			{
				FMVVMConstFieldVariant NewField = BindingHelper::FindFieldByName(CurrentContainer, FMVVMBindingName(*BlueprintSetter));
				if (NewField.IsFunction())
				{
					CurrentField = NewField;
				}
				else
				{
					return MakeError(FString::Printf(TEXT("The BlueprintSetter '%s' could not be found on object %s."), *BlueprintSetter, *CurrentContainer->GetName()));
				}
			}
		}
	}
#endif
	return MakeValue(CurrentField);
}

} // namespace


TValueOrError<TArray<FMVVMConstFieldVariant>, FString> GenerateFieldPathList(TSubclassOf<UObject> InFrom, FStringView InFieldPath, bool bForReading)
{
	if (InFrom.Get() == nullptr)
	{
		return MakeError(TEXT("The source class is invalid."));
	}
	if (InFieldPath.IsEmpty())
	{
		return MakeError(TEXT("The FieldPath is empty."));
	}
	if (InFieldPath[InFieldPath.Len() - 1] == TEXT('.'))
	{
		return MakeError(TEXT("The field path cannot end with a '.' character."));
	}

	FMemMark Mark(FMemStack::Get());
	TArray<FMVVMConstFieldVariant, TMemStackAllocator<>> Result;

	UStruct* CurrentContainer = InFrom.Get();

	// Split the string into property or function names
	//ie. myvar.myfunction.myvar
	int32 FoundIndex = INDEX_NONE;
	while (InFieldPath.FindChar(TEXT('.'), FoundIndex))
	{
		FMVVMConstFieldVariant Field = BindingHelper::FindFieldByName(CurrentContainer, FMVVMBindingName(FName(FoundIndex, InFieldPath.GetData())));
		if (Field.IsEmpty())
		{
			return MakeError(FString::Printf(TEXT("The field '%s' does not exist in the struct '%s'."), *FName(FoundIndex, InFieldPath.GetData()).ToString(), CurrentContainer ? *CurrentContainer->GetName() : TEXT("<none>")));
		}
		else if (Field.IsProperty())
		{
			TValueOrError<UStruct*, FString> FoundContainer = Private::FindContainer(Field.GetProperty());
			if (FoundContainer.HasError())
			{
				return MakeError(FoundContainer.StealError());
			}
			CurrentContainer = FoundContainer.GetValue();
		}
		
		if (Field.IsFunction())
		{
			const FProperty* ReturnProperty = BindingHelper::GetReturnProperty(Field.GetFunction());
			TValueOrError<UStruct*, FString> FoundContainer = Private::FindContainer(ReturnProperty);
			if (FoundContainer.HasError())
			{
				return MakeError(FoundContainer.StealError());
			}
			CurrentContainer = FoundContainer.GetValue();
		}

		InFieldPath = InFieldPath.RightChop(FoundIndex + 1);
		Result.Add(Field);
	}

	// The last field can be anything (that is what we are going to bind to)
	if (InFieldPath.Len() > 0)
	{
		FMVVMConstFieldVariant Field = BindingHelper::FindFieldByName(CurrentContainer, FMVVMBindingName(InFieldPath.GetData()));
		if (Field.IsEmpty())
		{
			return MakeError(FString::Printf(TEXT("The field '%s' does not exist in the struct '%s'."), InFieldPath.GetData(), CurrentContainer ? *CurrentContainer->GetName() : TEXT("<none>")));
		}

		Result.Add(Field);
	}

	return GenerateFieldPathList(Result, bForReading);
}


/**
 * Rules for reading:
 *		* Build path using Getter or BlueprintGetter if needed.
 *		* If the FProperty is a FStructProperty and a function was used, then the runtime may use dynamic memory instead of stack memory.
 * Rules for writing:
 *		* Build path using Getter or BlueprintGetter, the last element should use Setter or BlueprintSetter.
 *		* If one of the element in the path has a Setter or BlueprintSetter, then the path needs to stop there and be divided in 3 paths
 *			* ex: PropertyA.PropertyB.PropertyC.PropertyD.PropertyE and PropertyC has a Setter/BlueprintSetter
 *			* 1. To write: Property.Property.SetPropertyC()
 *			* 2. To read: PropertyA.PropertyB.PropertyC or PropertyA.PropertyB.GetPropertyC()
 *			* 3. To continue the reading: PropertyD.PropertyE
 *				PropertyD and PropertyE cannot have Getter/BlueprintGetter if they are FStructProperty
 *		* We can only have one Setter/BlueprintSetter in the path
 */
TValueOrError<TArray<FMVVMConstFieldVariant>, FString> GenerateFieldPathList(const TArrayView<FMVVMConstFieldVariant> InFieldPath, bool bForSourceBinding)
{
	if (InFieldPath.Num() == 0)
	{
		return MakeError(TEXT("The FieldPath is empty."));
	}

	TArray<FMVVMConstFieldVariant> Result;
	Result.Reserve(InFieldPath.Num());
	UStruct* CurrentContainer = InFieldPath[0].GetOwner();

	for (int32 Index = 0; Index < InFieldPath.Num(); ++Index)
	{
		bool bLastField = Index == InFieldPath.Num() - 1;
		FMVVMConstFieldVariant Field = InFieldPath[Index];

		if (Field.IsEmpty())
		{
			return MakeError(FString::Printf(TEXT("The field '%d' does not exist."), Index));
		}

		bool bIsChild = Field.GetOwner()->IsChildOf(CurrentContainer);
		bool bIsDownCast = CurrentContainer->IsChildOf(Field.GetOwner());
		if (CurrentContainer == nullptr || !(bIsChild || bIsDownCast))
		{
			return MakeError(FString::Printf(TEXT("The field '%s' does not exist in the struct '%s'."), *Field.GetName().ToString(), CurrentContainer ? *CurrentContainer->GetName() : TEXT("<none>")));
		}
		
		if (Field.IsProperty())
		{
			TValueOrError<FMVVMConstFieldVariant, FString> TransformedField = Private::TransformWithAccessor(CurrentContainer, Field, (bForSourceBinding || !bLastField));
			if (TransformedField.HasError())
			{
				return MakeError(TransformedField.StealError());
			}
			Field = TransformedField.StealValue();
			check(!Field.IsEmpty());

			if (!bLastField && Field.IsProperty())
			{
				TValueOrError<UStruct*, FString> FoundContainer = Private::FindContainer(Field.GetProperty());
				if (FoundContainer.HasError())
				{
					return MakeError(FoundContainer.StealError());
				}
				CurrentContainer = FoundContainer.GetValue();
			}
		}

		if (!bLastField && Field.IsFunction())
		{
			const FProperty* ReturnProperty = BindingHelper::GetReturnProperty(Field.GetFunction());
			TValueOrError<UStruct*, FString> FoundContainer = Private::FindContainer(ReturnProperty);
			if (FoundContainer.HasError())
			{
				return MakeError(FoundContainer.StealError());
			}
			CurrentContainer = FoundContainer.GetValue();
		}

		Result.Add(Field);
	}

	return MakeValue(MoveTemp(Result));
}


FString ToString(const TArrayView<FMVVMFieldVariant> Fields)
{
	TStringBuilder<512> Builder;
	for (int32 Index = 0; Index < Fields.Num(); ++Index)
	{
		if (Index != 0)
		{
			Builder << TEXT(".");
		}
		Builder << Fields[Index].GetName();
	}
	return Builder.ToString();
}


FString ToString(const TArrayView<FMVVMConstFieldVariant> Fields)
{
	TStringBuilder<512> Builder;
	for (int32 Index = 0; Index < Fields.Num(); ++Index)
	{
		if (Index != 0)
		{
			Builder << TEXT(".");
		}
		Builder << Fields[Index].GetName();
	}
	return Builder.ToString();
}


TValueOrError<UObject*, void> EvaluateObjectProperty(const FFieldContext& InSource)
{
	if (InSource.GetObjectVariant().IsNull())
	{
		return MakeError();
	}

	const bool bIsProperty = InSource.GetFieldVariant().IsProperty();
	const FProperty* GetterType = bIsProperty ? InSource.GetFieldVariant().GetProperty() : BindingHelper::GetReturnProperty(InSource.GetFieldVariant().GetFunction());
	check(GetterType);

	const FObjectPropertyBase* ObjectProperty = CastField<const FObjectPropertyBase>(GetterType);
	if (ObjectProperty == nullptr)
	{
		return MakeError();
	}

	if (bIsProperty)
	{
		return MakeValue(ObjectProperty->GetObjectPropertyValue_InContainer(InSource.GetObjectVariant().GetData()));
	}
	else
	{
		check(InSource.GetObjectVariant().IsUObject());
		check(InSource.GetObjectVariant().GetUObject());

		UFunction* Function = InSource.GetFieldVariant().GetFunction();
		if (InSource.GetObjectVariant().GetUObject()->GetClass()->IsChildOf(Function->GetOuterUClass()))
		{
			return MakeError();
		}

		void* DataPtr = FMemory_Alloca_Aligned(Function->ParmsSize, Function->GetMinAlignment());
		ObjectProperty->InitializeValue(DataPtr);
		InSource.GetObjectVariant().GetUObject()->ProcessEvent(Function, DataPtr);
		UObject* Result = ObjectProperty->GetObjectPropertyValue_InContainer(DataPtr);
		ObjectProperty->DestroyValue(DataPtr);
		return MakeValue(Result);
	}
}

} // namespace
