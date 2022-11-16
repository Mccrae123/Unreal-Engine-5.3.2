﻿// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "InstancedStruct.h"
#include "InstancedStructArray.h"
#include "StructView.h"
#include "WorldConditionBase.h"
#include "WorldConditionTypes.h"
#include "Templates/SubclassOf.h"
#include "WorldConditionQuery.generated.h"

struct FWorldConditionContext;
struct FWorldConditionContextData;
class UWorldConditionSchema;

/**
 * World Condition Query is an expression of World Conditions whose state can be queried.
 * The state of query and individual conditions can be cached, which allows to evaluate the conditions quickly.
 * See FWorldConditionBase for more information about the conditions.
 *
 * The World Condition Query is split it two parts: FWorldConditionQueryDefinition and FWorldConditionQueryState.
 * Definition is the "const" part of the query and state contain runtime caching and runtime state of the condition.
 * This allows the definition to be stored in an asset, and we can allocate just the per instance data when needed.
 *
 * Conditions operate on context data which is defined in a UWorldConditionSchema. The schema describes what kind
 * of structs and objects are available as input for the conditions, and what conditions can be used in specific use case.
 *
 * The state is tightly coupled to the definition. The memory layout of the state is stored in the definition.
 *
 * For convenience there is also FWorldConditionQuery which combines these two in one package.
 */

/**
 * Struct used to store a world condition in editor. Used internally.
 * Note that the Operator and ExpressionDepth are stored here separately from the World Condition to make sure they are not reset if the Condition is empty. 
 */
USTRUCT()
struct WORLDCONDITIONS_API FWorldConditionEditable
{
	GENERATED_BODY()

#if WITH_EDITORONLY_DATA
	FWorldConditionEditable() = default;

	FWorldConditionEditable(const uint8 InExpressionDepth, const EWorldConditionOperator InOperator, const FConstStructView InCondition)
		: ExpressionDepth(InExpressionDepth)
		, Operator(InOperator)
		, Condition(InCondition)
	{
	}
	
	void Reset()
	{
		Operator = EWorldConditionOperator::And;
		ExpressionDepth = 0;
		Condition.Reset();
	}
	
	/** Expression depth controlling the parenthesis of the expression. */
	UPROPERTY(EditAnywhere, Category="Default")
	uint8 ExpressionDepth = 0;

	/** Operator describing how the results of the condition is combined with other conditions. */
	UPROPERTY(EditAnywhere, Category="Default")
	EWorldConditionOperator Operator = EWorldConditionOperator::And;
	
	/** Instance of a world condition. */
	UPROPERTY(EditAnywhere, Category="Default")
	FInstancedStruct Condition;
#endif // WITH_EDITORONLY_DATA
};

/**
 * Definition of a world condition.
 * The mutable state of the world condition is stored in FWorldConditionQueryState.
 * This allows to reuse the definitions and minimize the runtime memory needed to run queries.
 */
USTRUCT()
struct WORLDCONDITIONS_API FWorldConditionQueryDefinition
{
	GENERATED_BODY()

	/** @return true of the definition has conditions and has been initialized. */
	bool IsValid() const;

	/** Initialized the condition from editable data. */
	bool Initialize();

	/** Conditions of the query, populated by Initialize(). */
	UPROPERTY()
	FInstancedStructArray Conditions;

	/** Schema of the definition. */
	UPROPERTY()
	TSubclassOf<UWorldConditionSchema> SchemaClass = nullptr;

#if WITH_EDITORONLY_DATA
	/** Conditions used while editing, converted in to Conditions via Initialize(). */
	UPROPERTY(EditAnywhere, Category="Default", meta=(BaseStruct = "/Script/SmartObjectsModule.WorldCondition"))
	TArray<FWorldConditionEditable> EditableConditions;
#endif // WITH_EDITORONLY_DATA
	
	friend struct FWorldConditionQueryState;
	friend struct FWorldConditionBase;
	friend struct FWorldConditionContext;
};


/**
 * Item used to describe the structure of a world condition query for fast traversal of the expression.
 */
USTRUCT()
struct WORLDCONDITIONS_API FWorldConditionItem
{
	GENERATED_BODY()

	FWorldConditionItem() = default;
	
	explicit FWorldConditionItem(const EWorldConditionOperator InOperator, const uint8 InNextExpressionDepth)
		: Operator(InOperator)
		, NextExpressionDepth(InNextExpressionDepth)
	{
	}

	/** Operator describing how the results of the condition is combined with other conditions. */
	EWorldConditionOperator Operator = EWorldConditionOperator::And;
	
	/** Expression depth controlling the parenthesis of the expression. */
	uint8 NextExpressionDepth = 0;
	
	/** Cached result of the condition. */
	EWorldConditionResult CachedResult = EWorldConditionResult::Invalid;
};

/**
 * Struct used to store the pointer to an UObject based condition state.
 */
USTRUCT()
struct WORLDCONDITIONS_API FWorldConditionStateObject
{
	GENERATED_BODY()

	UPROPERTY()
	TObjectPtr<UObject> Object;
};


/**
 * Runtime state of a world conditions.
 * The structure of the data for the state is defined in a query definition.
 * The definition and conditions are stored in FWorldConditionQueryDefinition.
 * This allows to reuse the definitions and minimize the runtime memory needed to run queries.
 *
 * Note: Any code embedding this struct is responsible for calling AddReferencedObjects().
 */
struct WORLDCONDITIONS_API FWorldConditionQueryState
{
	FWorldConditionQueryState()
		: bHasPerConditionState(false)
	{
	}
	
	~FWorldConditionQueryState();

	/** @return True if the state is property initialized. */
	bool IsValid() const { return Memory != nullptr; }

	/**
	 * Initialized the state for a specific query definition.
	 * @param Owner Owner of any objects created during Init().
	 * @param QueryDefinition definition of the state to initialized.
	 * @return True if initialization succeeded, false if failed (e.g. invalid definition).
	 */
	bool Initialize(const UObject& Owner, const FWorldConditionQueryDefinition& QueryDefinition);

	/**
	 * Frees the allocated data and objects. The definition must be the same as used in init
	 * as it is used to traverse the structure in memory.
	 */
	void Free(const FWorldConditionQueryDefinition& QueryDefinition);

	/** @return Condition item at specific index */
	FWorldConditionItem& GetItem(const int32 Index) const
	{
		check(Memory && Index >= 0 && Index < (int32)NumConditions);
		return *(FWorldConditionItem*)(Memory + Index * sizeof(FWorldConditionItem));
	}

	/** @return Object describing the state of a specified condition. */
	UObject* GetStateObject(const FWorldConditionBase& Condition) const
	{
		check(Condition.StateDataOffset > 0);
		check(Condition.bIsStateObject);
		const FWorldConditionStateObject& StateObject = *reinterpret_cast<FWorldConditionStateObject*>(Memory + Condition.StateDataOffset);
		return StateObject.Object;
	}

	/** @return struct describing the state of a specified condition. */
	FStructView GetStateStruct(const FWorldConditionBase& Condition) const
	{
		check(Condition.StateDataOffset > 0);
		check (!Condition.bIsStateObject);
		const UScriptStruct* ScriptStruct = Cast<UScriptStruct>(Condition.GetRuntimeStateType());
		check(ScriptStruct);
		return FStructView(Cast<UScriptStruct>(Condition.GetRuntimeStateType()), Memory + Condition.StateDataOffset);
	}

	/** @return True if any of the conditions has runtime state. */
	bool HasPerConditionState() const { return bHasPerConditionState; }

	/** @return The number of conditions in the state data. */
	int32 GetNumConditions() const { return NumConditions; }

	/** Adds referenced objects to the collector. */
	void AddReferencedObjects(const FWorldConditionQueryDefinition& QueryDefinition, class FReferenceCollector& Collector) const;

private:
	EWorldConditionResult CachedResult = EWorldConditionResult::Invalid;
	uint8 NumConditions = 0;
	uint8 bHasPerConditionState : 1;
	uint8* Memory = nullptr;

	friend struct FWorldConditionBase;
	friend struct FWorldConditionContext;
};


/**
 * General purpose World Condition Query that combines Query Definition and Query State in one.
 */
USTRUCT()
struct WORLDCONDITIONS_API FWorldConditionQuery
{
	GENERATED_BODY()

	~FWorldConditionQuery();

	/** @return True if the query is activated. */
	bool IsActive() const;

	/**
	 * Activates the world conditions in the query.
	 * @param ContextData ContextData that matches the schema of the query.
	 * @return true of the activation succeeded, or false if failed. Failed queries will return false when IsTrue() is called.
	 */
	bool Activate(const UObject& Owner, const FWorldConditionContextData& ContextData);

	/**
	 * Returns the result of the query. Cached state is returned if it is available,
	 * if update is needed or the query has dynamic context data, IsTrue() is called on the necessary conditions.
	 * @param ContextData ContextData that matches the schema of the query.
	 * @return the value of the query condition expression.
	 */
	bool IsTrue(const FWorldConditionContextData& ContextData) const;
	
	/**
	 * Deactivates the world conditions in the query.
	 * @param ContextData ContextData that matches the schema of the query.
	 */
	void Deactivate(const FWorldConditionContextData& ContextData) const;

	/** @return Schema of the query. */
	const UWorldConditionSchema* GetSchema() const { return QueryDefinition.SchemaClass.GetDefaultObject(); }

	/** Handles object references in the query state. */
	void AddStructReferencedObjects(class FReferenceCollector& Collector) const;

	/**
	 * Initializes a query from array of conditions for testing.
	 * @return true of the query was created and initialized.
	 */
	bool DebugInitialize(const TSubclassOf<UWorldConditionSchema> InSchemaClass, const TConstArrayView<FWorldConditionEditable> InConditions);

protected:
	/** Defines the conditions to run on the query.  */
	UPROPERTY(EditAnywhere, Category="Default");
	FWorldConditionQueryDefinition QueryDefinition;

	/** Runtime state of the query. */
	mutable FWorldConditionQueryState QueryState;

	/** Owner of the query. */
	UPROPERTY()
	TObjectPtr<const UObject> Owner = nullptr;
};

template<>
struct TStructOpsTypeTraits<FWorldConditionQuery> : public TStructOpsTypeTraitsBase2<FWorldConditionQuery>
{
	enum
	{
		WithAddStructReferencedObjects = true,
	};
};
