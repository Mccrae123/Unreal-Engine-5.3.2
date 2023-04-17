// Copyright Epic Games, Inc. All Rights Reserved.
#include "RandomizeColumn.h"
#include "ChooserPropertyAccess.h"

bool FRandomizeContextProperty::GetValue(const UObject* ContextObject, const FChooserRandomizationContext*& OutResult) const
{
	
	UStruct* StructType = ContextObject->GetClass();
	const void* Container = ContextObject;
	
	if (UE::Chooser::ResolvePropertyChain(Container, StructType, Binding.PropertyBindingChain))
	{
		if (FStructProperty* Property = FindFProperty<FStructProperty>(StructType, Binding.PropertyBindingChain.Last()))
		{
			OutResult = Property->ContainerPtrToValuePtr<FChooserRandomizationContext>(Container);
			return true;
		}
	}

	return false;
}

FRandomizeColumn::FRandomizeColumn()
{
	InputValue.InitializeAs(FRandomizeContextProperty::StaticStruct());
}

void FRandomizeColumn::Filter(FChooserDebuggingInfo& DebuggingInfo, const UObject* ContextObject, const TArray<uint32>& IndexListIn, TArray<uint32>& IndexListOut) const
{
	int Count = IndexListIn.Num();
	int Selection = 0;

	const FChooserRandomizationContext* RandomizationContext = nullptr;
	if (ContextObject && InputValue.IsValid())
	{
		InputValue.Get<FChooserParameterRandomizeBase>().GetValue(ContextObject,RandomizationContext);
	}
	
	if (Count > 1)
	{
		int LastSelectedIndex = -1;
		if (RandomizationContext)
		{
			if (const FRandomizationState* State = RandomizationContext->StateMap.Find(this))
			{
				LastSelectedIndex = State->LastSelectedRow;
			}
		}
		
		// compute the sum of all weights/probabilities
		float TotalWeight = 0;
		for (uint32 Index : IndexListIn)
		{
			float RowWeight = 1.0f;
			if (RowValues.Num() > static_cast<int32>(Index))
			{
				RowWeight = RowValues[Index];
			}
			
			if (Index == LastSelectedIndex)
			{
				RowWeight *= RepeatProbabilityMultiplier;
			}
			
			TotalWeight += RowWeight;
		}

		// pick a random float from 0-total weight
		float RandomNumber = FMath::FRandRange(0.0f, TotalWeight);
		float Weight = 0;

		// add up the weights again, and select the index where our sum clears the random float
		for (; Selection < Count - 1; Selection++)
		{
			int Index = static_cast<int32>(IndexListIn[Selection]);
			float RowWeight = 1.0f;
			
			if (RowValues.Num() > Index)
			{
				RowWeight = RowValues[Index];
			}
			
			if (Index == LastSelectedIndex)
			{
				RowWeight *= RepeatProbabilityMultiplier;
			}
			
			Weight += RowWeight;
			if (Weight > RandomNumber)
			{
				break;
			}
		}
	}

	if (Selection < Count)
	{
		IndexListOut.Add(IndexListIn[Selection]);
	}
}

void FRandomizeColumn::SetOutputs(FChooserDebuggingInfo& DebugInfo, UObject* ContextObject, int RowIndex) const
{
	if (ContextObject && InputValue.IsValid())
	{
		const FChooserRandomizationContext* ConstRandomizationContext = nullptr;
		InputValue.Get<FChooserParameterRandomizeBase>().GetValue(ContextObject,ConstRandomizationContext);
		if(ConstRandomizationContext)
		{
			FChooserRandomizationContext* RandomizationContext = const_cast<FChooserRandomizationContext*>(ConstRandomizationContext);
			FRandomizationState& State = RandomizationContext->StateMap.FindOrAdd(this, FRandomizationState());
			State.LastSelectedRow = RowIndex;
		}
	}
}
