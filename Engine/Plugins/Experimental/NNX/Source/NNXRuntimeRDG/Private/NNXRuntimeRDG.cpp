// Copyright Epic Games, Inc. All Rights Reserved.

#include "NNXRuntimeRDG.h"
#include "NNXInferenceModel.h"
#include "NNXRuntimeFormat.h"

#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"

#include "Serialization/MemoryReader.h"

namespace NNX
{

//
//
//
bool AlwaysValidValidationFunction(
	const UE::NNECore::FAttributeMap& AttributeMap, 
	TConstArrayView<EMLTensorDataType> InputTensorTypes,
	TConstArrayView<const FSymbolicTensorShape> InputShapes)
{
	return true;
}


//
//
//
FInputValidator::FInputValidator() : 
	NumRequiredInput(0), NumOptionalInput(0)
{
	TemplateTypes.SetNum(1);
}

bool FInputValidator::Validate(TConstArrayView<EMLTensorDataType> InputTypes)
{
	check(InputTemplateIndices.Num() == NumRequiredInput + NumOptionalInput);

	bool bAreInputValid = true;
	int32 NumInputsToValidate = FMath::Min(InputTemplateIndices.Num(), InputTypes.Num());
	
	if (InputTypes.Num() < NumRequiredInput)
	{
		UE_LOG(LogNNX, Warning, TEXT("Required '%d' inputs but found '%d'."), NumRequiredInput, InputTypes.Num());
		bAreInputValid = false;
	}
	if (InputTypes.Num() > NumRequiredInput+NumOptionalInput)
	{
		UE_LOG(LogNNX, Warning, TEXT("Got a total of '%d' inputs but should have '%d' maximum."), InputTypes.Num(), NumRequiredInput + NumOptionalInput);
		bAreInputValid = false;
	}
	
	for (int32 Idx = 0; Idx < NumInputsToValidate; ++Idx)
	{
		const int32 TemplateIdx = InputTemplateIndices[Idx];
		
		check(TemplateIdx < TemplateTypes.Num());
		if (INDEX_NONE == TemplateTypes[TemplateIdx].Find(InputTypes[Idx]))
		{
			UE_LOG(LogNNX, Warning, TEXT("Input '%d' from template idx '%d' is of type '%d' is not supported."), Idx, TemplateIdx, (int)InputTypes[Idx]);
			bAreInputValid = false;
		}
	}
	return bAreInputValid;
}
void FInputValidator::SetTemplateCount(int TemplateCount)
{
	TemplateTypes.SetNum(TemplateCount);
}
void FInputValidator::AddSupportedType(EMLTensorDataType Type, int TemplateIdx)
{
	check(TemplateTypes.Num() > TemplateIdx);
	TemplateTypes[TemplateIdx].Add(Type);
}
void FInputValidator::AddOptional(int32 TemplateIdx)
{
	InputTemplateIndices.Add(TemplateIdx);
	++NumOptionalInput;
}

void FInputValidator::AddRequired(int32 TemplateIdx)
{
	checkf(NumOptionalInput==0, TEXT("All required attribute should be declared before the optional ones as they are referenced by indices"));
	InputTemplateIndices.Add(TemplateIdx);
	++NumRequiredInput;
}


//
//
//
void FAttributeValidator::AddOptional(const FString& Name, ENNEAttributeDataType Type)
{
	checkf(nullptr == OptionalAttributes.FindByPredicate([Name](const FEntry& Other) { return Other.Name == Name; }), TEXT("Attribute name should be unique"));
	checkf(nullptr == RequiredAttributes.FindByPredicate([Name](const FEntry& Other) { return Other.Name == Name; }), TEXT("Attribute name should be unique"));
	OptionalAttributes.Emplace(Name, Type);
}

void FAttributeValidator::AddRequired(const FString& Name, ENNEAttributeDataType Type)
{
	checkf(nullptr == OptionalAttributes.FindByPredicate([Name](const FEntry& Other) { return Other.Name == Name; }), TEXT("Attribute name should be unique"));
	checkf(nullptr == RequiredAttributes.FindByPredicate([Name](const FEntry& Other) { return Other.Name == Name; }), TEXT("Attribute name should be unique"));
	RequiredAttributes.Emplace(Name, Type);
}

bool FAttributeValidator::Validate(const UE::NNECore::FAttributeMap& AttributesToValidate)
{
	bool bAreAttributesValid = true;

	//Verify all required attribute are matching specifications
	for (int32 Idx = 0; Idx < RequiredAttributes.Num(); ++Idx)
	{
		const FNNEAttributeValue* FoundAttribute = AttributesToValidate.GetAttributeValue(RequiredAttributes[Idx].Name);
		
		if (FoundAttribute == nullptr)
		{
			bAreAttributesValid = false;
			UE_LOG(LogNNX, Warning, TEXT("Required attribute '%s' not found."),
				*RequiredAttributes[Idx].Name);
		}
		else if (RequiredAttributes[Idx].Type != FoundAttribute->GetType())
		{
			bAreAttributesValid = false;
			UE_LOG(LogNNX, Warning, TEXT("Required attribute '%s' type '%d' does not match expected type '%d'."),
				*RequiredAttributes[Idx].Name,
				(int)FoundAttribute->GetType(),
				(int)RequiredAttributes[Idx].Type);
		}
	}

	//Verify all optional attribute are matching specifications
	for (int32 Idx = 0; Idx < OptionalAttributes.Num(); ++Idx)
	{
		const FNNEAttributeValue* FoundAttribute = AttributesToValidate.GetAttributeValue(OptionalAttributes[Idx].Name);
		
		if ((FoundAttribute != nullptr) && (OptionalAttributes[Idx].Type != FoundAttribute->GetType()))
		{
			bAreAttributesValid = false;
			UE_LOG(LogNNX, Warning, TEXT("Optional attribute '%s' type '%d' does not match expected type '%d'."),
				*OptionalAttributes[Idx].Name,
				(int)FoundAttribute->GetType(),
				(int)OptionalAttributes[Idx].Type);
		}
	}

	//Verify all attributes are either required or optional, otherwise they are unsupported
	for (int32 Idx = 0; Idx < AttributesToValidate.Num(); ++Idx)
	{
		const FString& Name = AttributesToValidate.GetName(Idx);
		const FEntry* OptionalAttribute = OptionalAttributes.FindByPredicate([Name](const FEntry& Other) { return Other.Name == Name; });
		const FEntry* RequiredAttribute = RequiredAttributes.FindByPredicate([Name](const FEntry& Other) { return Other.Name == Name; });
		
		if (OptionalAttribute == nullptr && RequiredAttribute == nullptr)
		{
			bAreAttributesValid = false;
			UE_LOG(LogNNX, Warning, TEXT("Found unsupported attribute '%s'."), *Name);
		}
	}

	return bAreAttributesValid;
}
	
//
//
//
FMLInferenceModelRDG::FMLInferenceModelRDG()
	: FMLInferenceModel(EMLInferenceModelType::RDG)
	, bUseManualTransitions(false)
{
	Readback.RHI = new FRHIGPUBufferReadback("FMLTensorReadback");
}

FMLInferenceModelRDG::~FMLInferenceModelRDG()
{
	delete Readback.RHI;
}

//
//
//
bool FMLInferenceModelRDG::LoadModel(const FNNIModelRaw& InModel, FMLRuntimeFormat& Format)
{
	ENNXInferenceFormat FormatType = InModel.Format;

	if (FormatType != ENNXInferenceFormat::NNXRT)
	{
		UE_LOG(LogNNX, Warning, TEXT("Unsupported format type for NNX inference model"));
		return false;
	}

	FMemoryReader Reader(InModel.Data);

	FMLRuntimeFormat::StaticStruct()->SerializeBin(Reader, &Format);

	// Add tensors
	for (int32 Idx = 0; Idx < Format.Tensors.Num(); ++Idx)
	{
		const FMLFormatTensorDesc& FormatTensorDesc = Format.Tensors[Idx];

		// When handling dynamic input shape FMLTensorDesc should then contain a FSymbolicTensorShape
        // while actual inference work on FConcreteTensorShape resolved by shape inference.
		FSymbolicTensorShape SymbolicShape = FSymbolicTensorShape::Make(FormatTensorDesc.Shape);
		check(SymbolicShape.IsConcrete());
		FConcreteTensorShape ConcreteShape = FConcreteTensorShape::Make(SymbolicShape);
		
		FMLTensorDesc Tensor = FMLTensorDesc::Make(FormatTensorDesc.Name, ConcreteShape, FormatTensorDesc.DataType);
		
		Tensor.DataSize = Tensor.GetElemByteSize() * Tensor.Volume();

		if (FormatTensorDesc.Type == EMLFormatTensorType::Input)
		{
			InputTensors.Add(Tensor);
			AllTensors.Add(Tensor);
		}
		else if (FormatTensorDesc.Type == EMLFormatTensorType::Output)
		{
			OutputTensors.Add(Tensor);
			AllTensors.Add(Tensor);
		}
		else if (FormatTensorDesc.Type == EMLFormatTensorType::Intermediate)
		{
			AllTensors.Add(Tensor);
		}
		checkf(FormatTensorDesc.Type != EMLFormatTensorType::None, TEXT("Unsupported tensor type None"));
	}

	return true;
}

/**
 * Run the inference model (synchronous version)
 */
int FMLInferenceModelRDG::Run(TArrayView<const FMLTensorBinding> InInputBindings, TArrayView<const FMLTensorBinding> OutOutputBindings)
{
	FEvent* Signal = FGenericPlatformProcess::GetSynchEventFromPool(false);
	int		Res = 0;

	ENQUEUE_RENDER_COMMAND(FMLInferenceModel_Run)
	(
		[&Signal, &Res, this, InInputBindings, OutOutputBindings](FRHICommandListImmediate& RHICmdList)
		{
			TOptional<ERHIPipeline>		Pipeline = RHICmdList.GetPipeline();

			if (Pipeline == ERHIPipeline::None)
			{
				RHICmdList.SwitchPipeline(ERHIPipeline::Graphics);
			}

			FRDGBuilder	GraphBuilder(RHICmdList);

			Res = EnqueueRDG(GraphBuilder, InInputBindings, OutOutputBindings);
			if (Res == 0)
			{
				GraphBuilder.Execute();

				// FIXME: Using BlockUntilGPUIdle() prevents hang on Linux
				RHICmdList.BlockUntilGPUIdle();
				//RHICmdList.SubmitCommandsHint();

				// Wait for readback
				while (!Readback.RHI->IsReady())
				{
					FPlatformProcess::Sleep(0.001f);
				}

				// Process readback
				{
					const void* BuffData = Readback.RHI->Lock(Readback.Size);
					check(BuffData);
					FMemory::Memcpy(Readback.CpuMemory, BuffData, Readback.Size);
					Readback.RHI->Unlock();
				}
			}
			
			Signal->Trigger();
		}
	);

	// We need to wait for render thread to finish
	Signal->Wait();

	FGenericPlatformProcess::ReturnSynchEventToPool(Signal);

	return Res;
}

/**
 * Enqueue operators to RDG, the caller will run the GraphBuilder.Execute()
 */
int FMLInferenceModelRDG::EnqueueRDG(FRDGBuilder& GraphBuilder, TArrayView<const FMLTensorBinding> InInputBindings, TArrayView<const FMLTensorBinding> InOutputBindings)
{
	check(IsInRenderingThread());

	int Res;

	// Process input tensors, and if required, allocate RDG buffers
	FMLTensorBindingArray	RDGInputBindings;
	FMLIntArray				RDGUploadIndices;

	Res = SetTensors(GraphBuilder, RDGInputBindings, RDGUploadIndices, InInputBindings, InputTensors);
	if (Res != 0)
	{
		UE_LOG(LogNNX, Warning, TEXT("Invalid input tensor binding type for tensor index:%d"), Res);
		return -1;
	}

	// Process output tensors, and if required, allocate RDG buffers
	FMLTensorBindingArray	RDGOutputBindings;
	FMLIntArray				RDGReadbackIndices;

	Res = SetTensors(GraphBuilder, RDGOutputBindings, RDGReadbackIndices, InOutputBindings, OutputTensors);
	if (Res != 0)
	{
		UE_LOG(LogNNX, Warning, TEXT("Invalid output tensor binding type for tensor index:%d"), Res);
		return -1;
	}

	// If required, upload input tensors to GPU
	if (!RDGUploadIndices.IsEmpty())
	{
		AddTensorUploads_RenderThread(GraphBuilder, RDGUploadIndices, RDGInputBindings, InInputBindings);
	}

	// We can now dispatch operators
	AddDispatchOps_RenderThread(GraphBuilder, RDGInputBindings, RDGOutputBindings);

	// If required, readback the output tensors to CPU
	if (!RDGReadbackIndices.IsEmpty())
	{
		AddTensorReadbacks_RenderThread(GraphBuilder, RDGReadbackIndices, RDGOutputBindings, InOutputBindings);		
	}

	return 0;
}

/** 
 * Process tensor bindings and check if we need to create RDG Buffer for CPU binding 
 * Returns 0 on success, or index of a tensor binding if the tensor binding type is not supported.
 */
int FMLInferenceModelRDG::SetTensors(FRDGBuilder& GraphBuilder, FMLTensorBindingArray& OutBindings, FMLIntArray& OutIndices, TArrayView<const FMLTensorBinding> InBindings, TArrayView<const FMLTensorDesc> InTensors)
{
	for (int32 Idx = 0; Idx < InBindings.Num(); ++Idx)
	{
		const FMLTensorBinding& Binding = InBindings[Idx];
		const FMLTensorDesc& TensorDesc = InTensors[Idx];

		if (Binding.BindingType == EMLTensorBindingDataType::CPUMemory)
		{
			// FIXME: CreateStructuredDesc() creates a crash on VulkanRHI
			//FRDGBufferDesc Desc = FRDGBufferDesc::CreateStructuredDesc(TensorDesc.GetElemByteSize(), TensorDesc.Num());
			FRDGBufferDesc Desc = FRDGBufferDesc::CreateBufferDesc(TensorDesc.GetElemByteSize(), TensorDesc.Num());

			// FIXME: We should use BUF_SourceCopy for only output buffers (GPU readback)
			Desc.Usage = EBufferUsageFlags(Desc.Usage | BUF_SourceCopy);

			FRDGBufferRef TensorBuffer = GraphBuilder.CreateBuffer(Desc, *TensorDesc.Name, ERDGBufferFlags::None);
			
			OutBindings.Emplace(FMLTensorBinding::FromRDG(TensorBuffer, InTensors[Idx].DataSize));
			OutIndices.Add(Idx);
		}
		else if (Binding.BindingType == EMLTensorBindingDataType::RDGBuffer)
		{
			OutBindings.Add(Binding);
		}
		else
		{
			// Unsupported tensor binding type
			return Idx;
		}
	}

	return 0;
}

//
//
//
void FMLInferenceModelRDG::AddTensorUploads_RenderThread(FRDGBuilder& GraphBuilder, TArrayView<const int32> InUploadIndices, TArrayView<FMLTensorBinding> InRDGBindings, TArrayView<const FMLTensorBinding> InBindings)
{
	for (int32 Idx = 0; Idx < InUploadIndices.Num(); ++Idx)
	{
		const int32				TensorIdx = InUploadIndices[Idx];
		const FMLTensorBinding& RDGBinding = InRDGBindings[TensorIdx];
		const FMLTensorBinding& InBinding = InBindings[TensorIdx];
		const FMLTensorDesc&	TensorDesc = InputTensors[TensorIdx];

		GraphBuilder.QueueBufferUpload(RDGBinding.Buffer, InBinding.CpuMemory, TensorDesc.DataSize, ERDGInitialDataFlags::NoCopy);
	}
}

//
//
//
void FMLInferenceModelRDG::AddTensorReadbacks_RenderThread(FRDGBuilder& GraphBuilder, TArrayView<const int32> InReadbackIndices, TArrayView<const FMLTensorBinding> InRDGBindings, TArrayView<const FMLTensorBinding> InBindings)
{	
	for (int32 Idx = 0; Idx < InReadbackIndices.Num(); ++Idx)
	{
		const int32				TensorIdx = InReadbackIndices[Idx];
		const FMLTensorBinding& RDGBinding = InRDGBindings[TensorIdx];
		const FMLTensorBinding& Binding = InBindings[TensorIdx];
		const FMLTensorDesc&	TensorDesc = OutputTensors[TensorIdx];

		FMLTensorReadbackParameters* TensorReadbackParams = GraphBuilder.AllocParameters<FMLTensorReadbackParameters>();

		TensorReadbackParams->Buffer = RDGBinding.Buffer;

		GraphBuilder.AddPass(
			RDG_EVENT_NAME("FMLInferenceModelAddTensorReadback:%s", *TensorDesc.Name),
			TensorReadbackParams,
			ERDGPassFlags::Readback | ERDGPassFlags::NeverCull,
			[this, Binding, TensorDesc, TensorReadbackParams](FRHICommandListImmediate& RHICmdList)
			{
				FRHIBuffer* OutputBuffer = TensorReadbackParams->Buffer->GetRHI();

				// TODO: FIXME: We need to transition the resources for DirectML
				if (bUseManualTransitions)
				{
					FRHITransitionInfo Transitions[] =
					{
						FRHITransitionInfo(OutputBuffer, ERHIAccess::UAVCompute, ERHIAccess::CopySrc)
					};

					RHICmdList.Transition(MakeArrayView(Transitions, UE_ARRAY_COUNT(Transitions)));
					RHICmdList.SubmitCommandsHint();
				}

				Readback.RHI->EnqueueCopy(RHICmdList, OutputBuffer, TensorDesc.DataSize);
				Readback.CpuMemory = Binding.CpuMemory;
				Readback.Offset = 0;
				Readback.Size = TensorDesc.DataSize;
			}
		);
	}
}


//
//
//
//void FMLInferenceModelRDG::AddTensorUpload_RenderThread(FRDGBuilder& GraphBuilder, FRDGBufferRef TensorBuffer, const FMLTensorBinding& InTensorBinding, const FMLTensorDesc& TensorDesc)
//{
//	FMLTensorUploadParameters* TensorUploadParams = GraphBuilder.AllocParameters<FMLTensorUploadParameters>();
//
//	TensorUploadParams->Buffer = TensorBuffer;
//
//	GraphBuilder.AddPass(
//		RDG_EVENT_NAME("NNXDmlTensorUpload"),
//		TensorUploadParams,
//		ERDGPassFlags::Copy | ERDGPassFlags::NeverCull,
//		[InTensorBinding, TensorDesc, TensorUploadParams](FRHICommandListImmediate& RHICmdList)
//		{
//			// Copy input					
//			void* BuffData = RHICmdList.LockBuffer(TensorUploadParams->Buffer->GetRHI(), 0, TensorDesc.DataSize, RLM_WriteOnly);
//			FMemory::Memcpy(BuffData, InTensorBinding.CpuMemory, TensorDesc.DataSize);
//			RHICmdList.UnlockBuffer(TensorUploadParams->Buffer->GetRHI());
//		}
//	);
//}

//
//
//
//void FMLInferenceModelRDG::AddTensorReadback_RenderThread(FRDGBuilder& GraphBuilder, const FMLTensorBinding& InTensorBinding, const FMLTensorDesc& TensorDesc)
//{	
//	FMLTensorReadbackParameters* TensorReadbackParams = GraphBuilder.AllocParameters<FMLTensorReadbackParameters>();
//
//	TensorReadbackParams->Buffer = TensorBuffer;
//
//	GraphBuilder.AddPass(
//		RDG_EVENT_NAME("NNXDmlTensorReadback"),
//		TensorReadbackParams,
//		ERDGPassFlags::Copy | ERDGPassFlags::NeverCull,
//		[InTensorBinding, TensorDesc, TensorReadbackParams](FRHICommandListImmediate& RHICmdList)
//		{
//			// Copy input					
//			void* BuffData = RHICmdList.LockBuffer(TensorReadbackParams->Buffer->GetRHI(), 0, TensorDesc.DataSize, RLM_WriteOnly);
//			FMemory::Memcpy(BuffData, InTensorBinding.CpuMemory, TensorDesc.DataSize);
//			RHICmdList.UnlockBuffer(TensorReadbackParams->Buffer->GetRHI());
//		}
//	);
//}

} // NNX
