// Copyright Epic Games, Inc. All Rights Reserved.

#include "Animation/AnimNodeFunctionRef.h"
#include "Animation/AnimNodeBase.h"
#include "Animation/AnimExecutionContext.h"
#include "Animation/AnimNodeReference.h"
#include "Animation/AnimSubsystem_NodeRelevancy.h"
#include "Animation/AnimInstance.h"

void FAnimNodeFunctionRef::Initialize(const UClass* InClass)
{
	if(FunctionName != NAME_None)
	{
		Function = InClass->FindFunctionByName(FunctionName);
	}
}

void FAnimNodeFunctionRef::Call(UObject* InObject, void* InParameters) const
{
	if(IsValid())
	{
		InObject->ProcessEvent(Function, InParameters);
	}
}

namespace UE
{
namespace Anim
{

template<typename WrapperType, typename ContextType>
static void CallFunctionHelper(const FAnimNodeFunctionRef& InFunction, ContextType InContext, FAnimNode_Base& InNode)
{
	if(InFunction.IsValid())
	{
		UAnimInstance* AnimInstance = CastChecked<UAnimInstance>(InContext.GetAnimInstanceObject());
		
		TSharedRef<FAnimExecutionContext::FData> ContextData = MakeShared<FAnimExecutionContext::FData>(InContext);
			
		struct FAnimNodeFunctionParams
		{
			WrapperType ExecutionContext;
			FAnimNodeReference NodeReference;
		};
			
		FAnimNodeFunctionParams Params = { WrapperType(ContextData), FAnimNodeReference(AnimInstance, InNode) };
			
		InFunction.Call(AnimInstance, &Params);
	}
}

void FNodeFunctionCaller::InitialUpdate(const FAnimationUpdateContext& InContext, FAnimNode_Base& InNode)
{
	if(InNode.NodeData != nullptr)
	{
		const FAnimNodeFunctionRef& Function = InNode.GetInitialUpdateFunction();
		if(Function.IsValid())
		{
			FAnimSubsystemInstance_NodeRelevancy& RelevancySubsystem = CastChecked<UAnimInstance>(InContext.GetAnimInstanceObject())->GetSubsystem<FAnimSubsystemInstance_NodeRelevancy>();
			const EAnimNodeInitializationStatus Status = RelevancySubsystem.UpdateNodeInitializationStatus(InContext, InNode);
			if(Status == EAnimNodeInitializationStatus::InitialUpdate)
			{
				CallFunctionHelper<FAnimUpdateContext>(Function, InContext, InNode);
			}
		}
	}
}
	
void FNodeFunctionCaller::BecomeRelevant(const FAnimationUpdateContext& InContext, FAnimNode_Base& InNode)
{
	if(InNode.NodeData != nullptr)
	{
		const FAnimNodeFunctionRef& Function = InNode.GetBecomeRelevantFunction();
		if(Function.IsValid())
		{
			FAnimSubsystemInstance_NodeRelevancy& RelevancySubsystem = CastChecked<UAnimInstance>(InContext.GetAnimInstanceObject())->GetSubsystem<FAnimSubsystemInstance_NodeRelevancy>();
			FAnimNodeRelevancyStatus Status = RelevancySubsystem.UpdateNodeRelevancy(InContext, InNode);
			if(Status.HasJustBecomeRelevant())
			{
				CallFunctionHelper<FAnimUpdateContext>(Function, InContext, InNode);
			}
		}
	}
}
	
void FNodeFunctionCaller::Update(const FAnimationUpdateContext& InContext, FAnimNode_Base& InNode)
{
	if(InNode.NodeData != nullptr)
	{
		CallFunctionHelper<FAnimUpdateContext>(InNode.GetUpdateFunction(), InContext, InNode);
	}
}

}}