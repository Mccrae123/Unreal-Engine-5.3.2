// Copyright Epic Games, Inc. All Rights Reserved.

#include "Scope.h"
#include "UnrealHeaderTool.h"
#include "UObject/ErrorException.h"
#include "ParserHelper.h"
#include "UnrealTypeDefinitionInfo.h"
#include "ClassMaps.h"

FScope::FScope(FScope* InParent)
	: Parent(InParent)
{ }

FScope::FScope()
	: Parent(nullptr)
{

}

void FScope::AddType(FUnrealFieldDefinitionInfo& Type)
{
	TypeMap.Add(Type.GetField()->GetFName(), &Type);
}

/**
 * Dispatch type to one of three arrays Enums, Structs and DelegateFunctions.
 *
 * @param Type Input type.
 * @param Enums (Output parameter) Array to fill with enums.
 * @param Structs (Output parameter) Array to fill with structs.
 * @param DelegateFunctions (Output parameter) Array to fill with delegate functions.
 */
void DispatchType(FUnrealFieldDefinitionInfo& FieldDef, TArray<FUnrealEnumDefinitionInfo*>& Enums, TArray<FUnrealScriptStructDefinitionInfo*>& Structs, TArray<FUnrealFunctionDefinitionInfo*>& DelegateFunctions)
{
	UField* Type = FieldDef.GetField();
	UClass* TypeClass = Type->GetClass();

	if (TypeClass == UClass::StaticClass() || TypeClass == UStruct::StaticClass())
	{
		// Inner scopes.
		FScope::GetTypeScope((UStruct*)Type)->SplitTypesIntoArrays(Enums, Structs, DelegateFunctions);
	}
	else if (TypeClass == UEnum::StaticClass())
	{
		Enums.Add(&UHTCastChecked<FUnrealEnumDefinitionInfo>(FieldDef));
	}
	else if (TypeClass == UScriptStruct::StaticClass())
	{
		Structs.Add(&UHTCastChecked<FUnrealScriptStructDefinitionInfo>(FieldDef));
	}
	else if (TypeClass == UDelegateFunction::StaticClass() || TypeClass == USparseDelegateFunction::StaticClass())
	{
		bool bAdded = false;
		UDelegateFunction* Function = (UDelegateFunction*)Type;

		if (Function->GetSuperFunction() == NULL)
		{
			DelegateFunctions.Add(&UHTCastChecked<FUnrealFunctionDefinitionInfo>(FieldDef));
			bAdded = true;
		}

		check(bAdded);
	}
}

void FScope::SplitTypesIntoArrays(TArray<FUnrealEnumDefinitionInfo*>& Enums, TArray<FUnrealScriptStructDefinitionInfo*>& Structs, TArray<FUnrealFunctionDefinitionInfo*>& DelegateFunctions)
{
	for (TPair<FName, FUnrealFieldDefinitionInfo*>& TypePair : TypeMap)
	{
		DispatchType(*TypePair.Value, Enums, Structs, DelegateFunctions);
	}
}

TSharedRef<FScope> FScope::GetTypeScope(UStruct* Type)
{
	TSharedRef<FUnrealTypeDefinitionInfo>* TypeDef = GTypeDefinitionInfoMap.Find(Type);
	if (TypeDef == nullptr)
	{
		FError::Throwf(TEXT("Couldn't find scope for the type %s."), *Type->GetName());
	}

	return (*TypeDef)->GetScope();
}

FUnrealFieldDefinitionInfo* FScope::FindTypeByName(FName Name)
{
	if (!Name.IsNone())
	{
		TDeepScopeTypeIterator<FUnrealFieldDefinitionInfo, false> TypeIterator(this);

		while (TypeIterator.MoveNext())
		{
			FUnrealFieldDefinitionInfo* Type = *TypeIterator;
			if (Type->GetField()->GetFName() == Name)
			{
				return Type;
			}
		}
	}

	return nullptr;
}

const FUnrealFieldDefinitionInfo* FScope::FindTypeByName(FName Name) const
{
	if (!Name.IsNone())
	{
		TScopeTypeIterator<FUnrealFieldDefinitionInfo, true> TypeIterator = GetTypeIterator();

		while (TypeIterator.MoveNext())
		{
			FUnrealFieldDefinitionInfo* Type = *TypeIterator;
			if (Type->GetField()->GetFName() == Name)
			{
				return Type;
			}
		}
	}

	return nullptr;
}

bool FScope::IsFileScope() const
{
	return Parent == nullptr;
}

bool FScope::ContainsTypes() const
{
	return TypeMap.Num() > 0;
}

FFileScope* FScope::GetFileScope()
{
	FScope* CurrentScope = this;
	while (!CurrentScope->IsFileScope())
	{
		CurrentScope = const_cast<FScope*>(CurrentScope->GetParent());
	}

	return CurrentScope->AsFileScope();
}

FFileScope::FFileScope(FName InName, FUnrealSourceFile* InSourceFile)
	: SourceFile(InSourceFile), Name(InName)
{ }

void FFileScope::IncludeScope(FFileScope* IncludedScope)
{
	IncludedScopes.Add(IncludedScope);
}

FUnrealSourceFile* FFileScope::GetSourceFile() const
{
	return SourceFile;
}

FName FFileScope::GetName() const
{
	return Name;
}

FName FStructScope::GetName() const
{
	return Struct->GetFName();
}
