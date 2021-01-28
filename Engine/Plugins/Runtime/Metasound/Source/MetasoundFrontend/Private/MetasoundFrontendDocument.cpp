// Copyright Epic Games, Inc. All Rights Reserved.

#include "MetasoundFrontendDocument.h"

#include "MetasoundFrontend.h"
#include "MetasoundFrontendRegistries.h"

namespace Metasound
{
	const FGuid FrontendInvalidID = FGuid();
}

void FMetasoundFrontendLiteral::Set(bool InValue)
{
	Clear();
	AsBool = InValue;
	Type = EMetasoundFrontendLiteralType::Bool;
}

void FMetasoundFrontendLiteral::Set(int32 InValue)
{
	Clear();
	AsInteger = InValue;
	Type = EMetasoundFrontendLiteralType::Integer;
}

void FMetasoundFrontendLiteral::Set(float InValue)
{
	Clear();
	AsFloat = InValue;
	Type = EMetasoundFrontendLiteralType::Float;
}

void FMetasoundFrontendLiteral::Set(const FString& InValue)
{
	Clear();
	AsString = InValue;
	Type = EMetasoundFrontendLiteralType::String;
}

void FMetasoundFrontendLiteral::Set(UObject* InValue)
{
	Clear();
	AsUObject = InValue;
	Type = EMetasoundFrontendLiteralType::UObject;
}

void FMetasoundFrontendLiteral::Set(const TArray<UObject*>& InValue)
{
	Clear();
	AsUObjectArray = InValue;
	Type = EMetasoundFrontendLiteralType::UObjectArray;
}

void FMetasoundFrontendLiteral::Clear()
{
	Type = EMetasoundFrontendLiteralType::None;

	AsBool = false;
	AsInteger = 0;
	AsFloat = 0.0f;
	AsString.Empty();
	AsUObject = nullptr;
	AsUObjectArray.Empty();
}

FMetasoundFrontendNodeInterface::FMetasoundFrontendNodeInterface(const FMetasoundFrontendClassInterface& InClassInterface)
{
	for (const FMetasoundFrontendClassInput& Input : InClassInterface.Inputs)
	{
		Inputs.Add(Input);
	}

	for (const FMetasoundFrontendClassOutput& Output : InClassInterface.Outputs)
	{
		Outputs.Add(Output);
	}

	for (const FMetasoundFrontendClassEnvironmentVariable& EnvVar : InClassInterface.Environment)
	{
		FMetasoundFrontendVertex EnvVertex;
		EnvVertex.Name = EnvVar.Name;
		EnvVertex.TypeName = EnvVar.TypeName;

		Environment.Add(MoveTemp(EnvVertex));
	}
}

FMetasoundFrontendNode::FMetasoundFrontendNode(const FMetasoundFrontendClass& InClass)
: ClassID(InClass.ID)
, Name(InClass.Metadata.ClassName.Name.ToString())
, Interface(InClass.Interface)
{

}

bool FMetasoundFrontendVertex::IsFunctionalEquivalent(const FMetasoundFrontendVertex& InLHS, const FMetasoundFrontendVertex& InRHS)
{
	return (InLHS.Name == InRHS.Name) && (InLHS.TypeName == InRHS.TypeName) && (InLHS.PointIDs.Num() == InRHS.PointIDs.Num());
}

bool FMetasoundFrontendVertexBehavior::IsFunctionalEquivalent(const FMetasoundFrontendVertexBehavior& InLHS, const FMetasoundFrontendVertexBehavior& InRHS)
{
	return (InLHS.Type == InRHS.Type) && (InLHS.ArrayMin == InRHS.ArrayMin) && (InLHS.ArrayMax == InRHS.ArrayMax);
}

bool FMetasoundFrontendClassVertex::IsFunctionalEquivalent(const FMetasoundFrontendClassVertex& InLHS, const FMetasoundFrontendClassVertex& InRHS) 
{
	return FMetasoundFrontendVertex::IsFunctionalEquivalent(InLHS, InRHS) && FMetasoundFrontendVertexBehavior::IsFunctionalEquivalent(InLHS.Behavior, InRHS.Behavior);
}

FMetasoundFrontendClassName::FMetasoundFrontendClassName(const FName& InNamespace, const FName& InName, const FName& InVariant)
: Namespace(InNamespace)
, Name(InName)
, Variant(InVariant)
{
}

FMetasoundFrontendClassName::FMetasoundFrontendClassName(const Metasound::FNodeClassName& InName)
: FMetasoundFrontendClassName(InName.GetNamespace(), InName.GetName(), InName.GetVariant())
{
}

FName FMetasoundFrontendClassName::GetScopedName() const
{
	return Metasound::FNodeClassName::FormatScopedName(Namespace, Name);
}

FName FMetasoundFrontendClassName::GetFullName() const
{
	return Metasound::FNodeClassName::FormatFullName(Namespace, Name, Variant);
}

FString FMetasoundFrontendClassName::ToString() const
{
	return GetFullName().ToString();
}

bool operator==(const FMetasoundFrontendClassName& InLHS, const FMetasoundFrontendClassName& InRHS)
{
	return (InLHS.Namespace == InRHS.Namespace) && (InLHS.Name == InRHS.Name) && (InLHS.Variant == InRHS.Variant);
}

FMetasoundFrontendClassMetadata::FMetasoundFrontendClassMetadata(const Metasound::FNodeClassMetadata& InNodeClassMetadata)
: ClassName(InNodeClassMetadata.ClassName)
, Version{InNodeClassMetadata.MajorVersion, InNodeClassMetadata.MinorVersion}
, Type(EMetasoundFrontendClassType::External)
, DisplayName(InNodeClassMetadata.DisplayName)
, Description(InNodeClassMetadata.Description)
, PromptIfMissing(InNodeClassMetadata.PromptIfMissing)
, Author(InNodeClassMetadata.Author)
, Keywords(InNodeClassMetadata.Keywords)
, CategoryHierarchy(InNodeClassMetadata.CategoryHierarchy)
{
}

FMetasoundFrontendClassInput::FMetasoundFrontendClassInput(const FMetasoundFrontendClassVertex& InOther)
:	FMetasoundFrontendClassVertex(InOther)
{
	EMetasoundFrontendLiteralType DefaultType = Metasound::Frontend::GetMetasoundLiteralType(FMetasoundFrontendRegistryContainer::Get()->GetDesiredLiteralTypeForDataType(InOther.TypeName));

	for (const FGuid& PointID : PointIDs)
	{
		FMetasoundFrontendVertexLiteral Default;
		Default.PointID = PointID;
		Default.Value.Type = DefaultType;
		Defaults.Add(Default);
	}
}

FMetasoundFrontendGraphClass::FMetasoundFrontendGraphClass()
{
	Metadata.Type = EMetasoundFrontendClassType::Graph;
}

FMetasoundFrontendDocument::FMetasoundFrontendDocument()
{
	RootGraph.ID = FGuid::NewGuid();
	RootGraph.Metadata.Type = EMetasoundFrontendClassType::Graph;
}

