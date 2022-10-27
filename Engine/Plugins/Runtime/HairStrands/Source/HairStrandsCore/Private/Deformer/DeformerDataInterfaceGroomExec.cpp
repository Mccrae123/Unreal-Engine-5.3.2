// Copyright Epic Games, Inc. All Rights Reserved.

#include "DeformerDataInterfaceGroomExec.h"

#include "GroomComponent.h"
#include "ComputeFramework/ShaderParamTypeDefinition.h"
#include "ShaderCompilerCore.h"
#include "ShaderParameterMetadataBuilder.h"
#include "SkeletalRenderPublic.h"

FString UOptimusGroomExecDataInterface::GetDisplayName() const
{
	return TEXT("Execute Groom");
}

FName UOptimusGroomExecDataInterface::GetCategory() const
{
	return CategoryName::ExecutionDataInterfaces;
}

TArray<FOptimusCDIPinDefinition> UOptimusGroomExecDataInterface::GetPinDefinitions() const
{
	TArray<FOptimusCDIPinDefinition> Defs;
	Defs.Add({ "NumThreads", "ReadNumThreads" });
	return Defs;
}


TSubclassOf<UActorComponent> UOptimusGroomExecDataInterface::GetRequiredComponentClass() const
{
	return UGroomComponent::StaticClass();
}


void UOptimusGroomExecDataInterface::GetSupportedInputs(TArray<FShaderFunctionDefinition>& OutFunctions) const
{
	OutFunctions.AddDefaulted_GetRef()
		.SetName(TEXT("ReadNumThreads"))
		.AddReturnType(EShaderFundamentalType::Int, 3);
}


BEGIN_SHADER_PARAMETER_STRUCT(FGroomExecDataInterfaceParameters, )
	SHADER_PARAMETER(FIntVector, NumThreads)
END_SHADER_PARAMETER_STRUCT()

void UOptimusGroomExecDataInterface::GetShaderParameters(TCHAR const* UID, FShaderParametersMetadataBuilder& InOutBuilder, FShaderParametersMetadataAllocations& InOutAllocations) const
{
	InOutBuilder.AddNestedStruct<FGroomExecDataInterfaceParameters>(UID);
}

void UOptimusGroomExecDataInterface::GetShaderHash(FString& InOutKey) const
{
	GetShaderFileHash(TEXT("/Plugin/Runtime/HairStrands/Private/DeformerDataInterfaceGroomExec.ush"), EShaderPlatform::SP_PCD3D_SM5).AppendString(InOutKey);
}

void UOptimusGroomExecDataInterface::GetHLSL(FString& OutHLSL, FString const& InDataInterfaceName) const
{
	TMap<FString, FStringFormatArg> TemplateArgs =
	{
		{ TEXT("DataInterfaceName"), InDataInterfaceName },
	};

	FString TemplateFile;
	LoadShaderSourceFile(TEXT("/Plugin/Runtime/HairStrands/Private/DeformerDataInterfaceGroomExec.ush"), EShaderPlatform::SP_PCD3D_SM5, &TemplateFile, nullptr);
	OutHLSL += FString::Format(*TemplateFile, TemplateArgs);
}

UComputeDataProvider* UOptimusGroomExecDataInterface::CreateDataProvider(TObjectPtr<UObject> InBinding, uint64 InInputMask, uint64 InOutputMask) const
{
	UOptimusGroomExecDataProvider* Provider = NewObject<UOptimusGroomExecDataProvider>();
	Provider->GroomComponent = Cast<UGroomComponent>(InBinding);
	Provider->Domain = Domain;
	return Provider;
}

bool UOptimusGroomExecDataProvider::IsValid() const
{
	return GroomComponent != nullptr && GroomComponent->GetGroupCount() > 0;
}

FComputeDataProviderRenderProxy* UOptimusGroomExecDataProvider::GetRenderProxy()
{
	return new FOptimusGroomExecDataProviderProxy(GroomComponent, Domain);
}

FOptimusGroomExecDataProviderProxy::FOptimusGroomExecDataProviderProxy(UGroomComponent* InGroomComponent, EOptimusGroomExecDomain InDomain)
{
	GroomComponent = InGroomComponent;
	Domain = InDomain;
}

int32 FOptimusGroomExecDataProviderProxy::GetDispatchThreadCount(TArray<FIntVector>& ThreadCounts) const
{
	const int32 NumInvocations = GroomComponent ? GroomComponent->GetGroupCount() : 0;

	ThreadCounts.Reset();
	ThreadCounts.Reserve(NumInvocations);
	for (int32 InvocationIndex = 0; InvocationIndex < NumInvocations; ++InvocationIndex)
	{		
		if (FHairGroupInstance* Instance = GroomComponent->GetGroupInstance(InvocationIndex))
		{
			const int32 NumControlPoints = Instance->Strands.Data->PointCount;
			const int32 NumCurves = Instance->Strands.Data->CurveCount;
			const int32 NumThreads = Domain == EOptimusGroomExecDomain::ControlPoint ? NumControlPoints : NumCurves;
			ThreadCounts.Add(FIntVector(NumThreads, 1, 1));
		}
	}
	return NumInvocations;
}

void FOptimusGroomExecDataProviderProxy::GatherDispatchData(FDispatchSetup const& InDispatchSetup, FCollectedDispatchData& InOutDispatchData)
{
	if (!ensure(InDispatchSetup.ParameterStructSizeForValidation == sizeof(FGroomExecDataInterfaceParameters)))
	{
		return;
	}

	const int32 NumInvocations = GroomComponent ? GroomComponent->GetGroupCount() : 0;
	if (!ensure(NumInvocations == InDispatchSetup.NumInvocations))
	{
		return;
	}

	for (int32 InvocationIndex = 0; InvocationIndex < NumInvocations; ++InvocationIndex)
	{
		FHairGroupInstance* Instance = GroomComponent->GetGroupInstance(InvocationIndex);
		const int32 NumControlPoints = Instance->Strands.Data->PointCount;
		const int32 NumCurves = Instance->Strands.Data->CurveCount;
		const int32 NumThreads = Domain == EOptimusGroomExecDomain::ControlPoint ? NumControlPoints : NumCurves;

		FGroomExecDataInterfaceParameters* Parameters = (FGroomExecDataInterfaceParameters*)(InOutDispatchData.ParameterBuffer + InDispatchSetup.ParameterBufferOffset + InDispatchSetup.ParameterBufferStride * InvocationIndex);
		Parameters->NumThreads = FIntVector(NumThreads, 1, 1);
	}
}
