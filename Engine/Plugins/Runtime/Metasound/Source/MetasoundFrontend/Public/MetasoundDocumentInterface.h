// Copyright Epic Games, Inc. All Rights Reserved.
#pragma once

#include "UObject/Interface.h"
#include "MetasoundFrontendDocument.h"

#include "MetasoundDocumentInterface.generated.h"


// Forward Declarations
struct FMetaSoundFrontendDocumentBuilder;


// UInterface for all MetaSound UClasses that implement a MetaSound document
// as a means for accessing data via code, scripting, execution, or node
// class generation.
UINTERFACE(BlueprintType, meta = (CannotImplementInterfaceInBlueprint))
class METASOUNDFRONTEND_API UMetaSoundDocumentInterface : public UInterface
{
	GENERATED_BODY()
};

class METASOUNDFRONTEND_API IMetaSoundDocumentInterface : public IInterface
{
	GENERATED_BODY()

public:
	// Returns read-only reference to the the MetaSoundFrontendDocument
	// containing all MetaSound runtime & editor data.
	virtual const FMetasoundFrontendDocument& GetDocument() const = 0;
	virtual const UClass& GetBaseMetaSoundUClass() const = 0;

	// Returns the parent class registered with the MetaSound UObject registry.

private:
	virtual FMetasoundFrontendDocument& GetDocument() = 0;

	friend struct FMetaSoundFrontendDocumentBuilder;
};