// Copyright Epic Games, Inc. All Rights Reserved.

#include "OptimusDataInterfaceGraph.h"

#include "Components/SkinnedMeshComponent.h"
#include "ComputeFramework/ShaderParameterMetadataAllocation.h"
#include "ComputeFramework/ShaderParameterMetadataBuilder.h"
#include "ComputeFramework/ShaderParamTypeDefinition.h"
#include "OptimusHelpers.h"
#include "OptimusDeformerInstance.h"
#include "OptimusVariableDescription.h"

void UOptimusGraphDataInterface::Init(TArray<FOptimusGraphVariableDescription> const& InVariables)
{
	Variables = InVariables;

	FShaderParametersMetadataBuilder Builder;
	for (FOptimusGraphVariableDescription const& Variable : Variables)
	{
		Optimus::AddParamForType(Builder, *Variable.Name, Variable.ValueType);
	}
	TSharedPtr<FShaderParametersMetadata> ShaderParameterMetadata(Builder.Build(FShaderParametersMetadata::EUseCase::ShaderParameterStruct, TEXT("UGraphDataInterface")));

	TArray<FShaderParametersMetadata::FMember> const& Members = ShaderParameterMetadata->GetMembers();
	for (int32 VariableIndex = 0; VariableIndex < Variables.Num(); ++VariableIndex)
	{
		check(Variables[VariableIndex].Name == Members[VariableIndex].GetName());
		Variables[VariableIndex].Offset = Members[VariableIndex].GetOffset();
	}

	ParameterBufferSize = ShaderParameterMetadata->GetSize();
}

void UOptimusGraphDataInterface::GetSupportedInputs(TArray<FShaderFunctionDefinition>& OutFunctions) const
{
	OutFunctions.Reserve(OutFunctions.Num() + Variables.Num());
	for (FOptimusGraphVariableDescription const& Variable : Variables)
	{
		OutFunctions.AddDefaulted_GetRef()
			.SetName(FString::Printf(TEXT("Read%s"), *Variable.Name))
			.AddReturnType(Variable.ValueType);
	}
}

void UOptimusGraphDataInterface::GetShaderParameters(TCHAR const* UID, FShaderParametersMetadataBuilder& InOutBuilder, FShaderParametersMetadataAllocations& InOutAllocations) const
{
	// Build metadata nested structure containing all variables.
	FShaderParametersMetadataBuilder Builder;
	for (FOptimusGraphVariableDescription const& Variable : Variables)
	{
		Optimus::AddParamForType(Builder, *Variable.Name, Variable.ValueType);
	}

	FShaderParametersMetadata* ShaderParameterMetadata = Builder.Build(FShaderParametersMetadata::EUseCase::ShaderParameterStruct, TEXT("UGraphDataInterface"));
	// Add the metadata to InOutAllocations so that it is released when we are done.
	InOutAllocations.ShaderParameterMetadatas.Add(ShaderParameterMetadata);

	// Add the generated nested struct to our builder.
	InOutBuilder.AddNestedStruct(UID, ShaderParameterMetadata);
}

void UOptimusGraphDataInterface::GetHLSL(FString& OutHLSL) const
{
	// Need include for DI_LOCAL macro expansion.
	OutHLSL += TEXT("#include \"/Plugin/ComputeFramework/Private/ComputeKernelCommon.ush\"\n");
	// Add uniforms.
	for (FOptimusGraphVariableDescription const& Variable : Variables)
	{
		OutHLSL += FString::Printf(TEXT("%s DI_LOCAL(%s);\n"), *Variable.ValueType->ToString(), *Variable.Name);
	}
	// Add function getters.
	for (FOptimusGraphVariableDescription const& Variable : Variables)
	{
		OutHLSL += FString::Printf(TEXT("DI_IMPL_READ(Read%s, %s, )\n{\n\treturn DI_LOCAL(%s);\n}\n"), *Variable.Name, *Variable.ValueType->ToString(), *Variable.Name);
	}
}

void UOptimusGraphDataInterface::GetSourceTypes(TArray<UClass*>& OutSourceTypes) const
{
	OutSourceTypes.Add(USkinnedMeshComponent::StaticClass());
}

UComputeDataProvider* UOptimusGraphDataInterface::CreateDataProvider(TArrayView< TObjectPtr<UObject> > InSourceObjects, uint64 InInputMask, uint64 InOutputMask) const
{
	UOptimusGraphDataProvider* Provider = NewObject<UOptimusGraphDataProvider>();

	if (InSourceObjects.Num() == 1)
	{
		Provider->SkinnedMeshComponent = Cast<USkinnedMeshComponent>(InSourceObjects[0]);
		Provider->Variables = Variables;
		Provider->ParameterBufferSize = ParameterBufferSize;
	}

	return Provider;
}


void UOptimusGraphDataProvider::SetConstant(FString const& InVariableName, TArray<uint8> const& InValue)
{
	for (int32 VariableIndex = 0; VariableIndex < Variables.Num(); ++VariableIndex)
	{
		if (Variables[VariableIndex].Name == InVariableName)
		{
			if (ensure(Variables[VariableIndex].Value.Num() == InValue.Num()))
			{
				Variables[VariableIndex].Value = InValue;
				break;
			}
		}
	}
}

FComputeDataProviderRenderProxy* UOptimusGraphDataProvider::GetRenderProxy()
{
	UOptimusDeformerInstance* DeformerInstance = Cast<UOptimusDeformerInstance>(SkinnedMeshComponent->MeshDeformerInstance);

	return new FOptimusGraphDataProviderProxy(DeformerInstance, Variables, ParameterBufferSize);
}


FOptimusGraphDataProviderProxy::FOptimusGraphDataProviderProxy(UOptimusDeformerInstance const* DeformerInstance, TArray<FOptimusGraphVariableDescription> const& Variables, int32 ParameterBufferSize)
{
	// Get all variables from deformer instance and fill buffer.
	ParameterData.AddZeroed(ParameterBufferSize);

	if (DeformerInstance == nullptr)
	{
		return;
	}

	TArray<UOptimusVariableDescription*> const& VariableValues = DeformerInstance->GetVariables();
	for (FOptimusGraphVariableDescription const& Variable : Variables)
	{
		if (Variable.Value.Num())
		{
			// Use the constant value.
			FMemory::Memcpy(&ParameterData[Variable.Offset], Variable.Value.GetData(), Variable.Value.Num());
		}
		else
		{
			// Find value from variables on the deformer instance.
			// todo[CF]: Use a map for more efficient look up? Or something even faster like having a fixed location per variable?
			for (UOptimusVariableDescription const* VariableValue : VariableValues)
			{
				if (VariableValue != nullptr)
				{
					if (Variable.ValueType == VariableValue->DataType->ShaderValueType && Variable.Name == VariableValue->VariableName.GetPlainNameString())
					{
						TArrayView<uint8> ParameterEntry(&ParameterData[Variable.Offset], VariableValue->DataType->ShaderValueSize);
						VariableValue->DataType->ConvertPropertyValueToShader(VariableValue->ValueData, ParameterEntry);
						break;
					}
				}
			}
		}
	}
}

void FOptimusGraphDataProviderProxy::GatherDispatchData(FDispatchSetup const& InDispatchSetup, FCollectedDispatchData& InOutDispatchData)
{
	if (ParameterData.Num() == 0)
	{
		// todo[CF]: Why can we end up here? Remove this condition if possible.
		return;
	}

	if (!ensure(ParameterData.Num() == InDispatchSetup.ParameterStructSizeForValidation))
	{
		return;
	}

	for (int32 InvocationIndex = 0; InvocationIndex < InDispatchSetup.NumInvocations; ++InvocationIndex)
	{
		void* ParameterBuffer = (void*)(InOutDispatchData.ParameterBuffer + InDispatchSetup.ParameterBufferOffset + InDispatchSetup.ParameterBufferStride * InvocationIndex);
		FMemory::Memcpy(ParameterBuffer, ParameterData.GetData(), ParameterData.Num());
	}
}
