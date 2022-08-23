﻿// Copyright Epic Games, Inc. All Rights Reserved.

#include "Conditions/StateTreeObjectConditions.h"
#include "StateTreeExecutionContext.h"

#define LOCTEXT_NAMESPACE "StateTreeEditor"


//----------------------------------------------------------------------//
//  FStateTreeCondition_ObjectIsValid
//----------------------------------------------------------------------//

bool FStateTreeObjectIsValidCondition::TestCondition(FStateTreeExecutionContext& Context) const
{
	const InstanceDataType& InstanceData = Context.GetInstanceData<InstanceDataType>(*this);
	const bool bResult = IsValid(InstanceData.Object);
	return bResult ^ bInvert;
}

//----------------------------------------------------------------------//
//  FStateTreeCondition_ObjectEquals
//----------------------------------------------------------------------//

bool FStateTreeObjectEqualsCondition::TestCondition(FStateTreeExecutionContext& Context) const
{
	const InstanceDataType& InstanceData = Context.GetInstanceData<InstanceDataType>(*this);
	const bool bResult = InstanceData.Left == InstanceData.Right;
	return bResult ^ bInvert;
}

//----------------------------------------------------------------------//
//  FStateTreeCondition_ObjectIsChildOfClass
//----------------------------------------------------------------------//

bool FStateTreeObjectIsChildOfClassCondition::TestCondition(FStateTreeExecutionContext& Context) const
{
	const InstanceDataType& InstanceData = Context.GetInstanceData<InstanceDataType>(*this);
	const bool bResult = InstanceData.Object && InstanceData.Class
						&& InstanceData.Object->GetClass()->IsChildOf(InstanceData.Class);
	return bResult ^ bInvert;
}

#undef LOCTEXT_NAMESPACE