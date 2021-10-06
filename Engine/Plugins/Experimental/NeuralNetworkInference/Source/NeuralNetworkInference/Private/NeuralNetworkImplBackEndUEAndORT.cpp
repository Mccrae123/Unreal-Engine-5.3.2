// Copyright Epic Games, Inc. All Rights Reserved.

#include "NeuralNetworkImplBackEndUEAndORT.h"
#include "NeuralNetworkInferenceUtils.h"
#include "NeuralNetworkInferenceUtilsGPU.h"
#if WITH_EDITOR
#include "Misc/MessageDialog.h"
#endif //WITH_EDITOR
#include "RedirectCoutAndCerrToUeLog.h"

#if defined(WITH_UE_AND_ORT_SUPPORT) && defined(PLATFORM_WIN64)
	#include "HAL/CriticalSection.h"
	#include "RHI.h"
	#include "DynamicRHI.h"
	
	// Disable NOMINMAX & WIN32_LEAN_AND_MEAN defines to avoid compiler warnings
	#pragma push_macro("NOMINMAX")
	#pragma push_macro("WIN32_LEAN_AND_MEAN")
	#undef NOMINMAX
	#undef WIN32_LEAN_AND_MEAN
	#include "D3D12RHIPrivate.h"
	#pragma pop_macro("WIN32_LEAN_AND_MEAN")
	#pragma pop_macro("NOMINMAX")

#endif

//#define WITH_NNI_CPU_NOT_RECOMMENDED // Only for debugging purposes

NNI_THIRD_PARTY_INCLUDES_START
#undef check
#undef TEXT
#ifdef WITH_UE_AND_ORT_SUPPORT
	#ifdef PLATFORM_WIN64
	#include "core/providers/dml/dml_provider_factory.h"
	#endif
	#ifdef WITH_NNI_CPU_NOT_RECOMMENDED
	#include "core/providers/nni_cpu/nni_cpu_provider_factory.h"
	#endif //WITH_NNI_CPU_NOT_RECOMMENDED
#endif //WITH_UE_AND_ORT_SUPPORT
NNI_THIRD_PARTY_INCLUDES_END



#if defined(WITH_UE_AND_ORT_SUPPORT) && defined(PLATFORM_WIN64)

/* FPrivateImplBackEndUEAndORT auxiliary class
 *****************************************************************************/
class FPrivateImplBackEndUEAndORT
{
public:
	static IDMLDevice* GetDMLDeviceThreadSafe(ID3D12Device* Device);

private:
	/**
	 * Helper class that maintains a list of created DML Devices for given ID3D12Device
	 */
	class FDMLDeviceList
	{
	public:
		IDMLDevice* GetDMLDevice(ID3D12Device* Device);

	private:
		IDMLDevice* Add(ID3D12Device* Device);

		struct DMLDeviceEntry
		{
			ID3D12Device* Device;
			IDMLDevice* DmlDevice;
		};

		TArray<DMLDeviceEntry>		Entries;
	};
};

IDMLDevice* FPrivateImplBackEndUEAndORT::GetDMLDeviceThreadSafe(ID3D12Device* Device)
{
	static FCriticalSection CriticalSection; /* Critical section to protect GetDMLDeviceThreadSafe from being called simultaneously from multiple threads. */
	static FDMLDeviceList DMLDeviceList;
	FScopeLock Lock(&CriticalSection);
	return DMLDeviceList.GetDMLDevice(Device);
}

IDMLDevice* FPrivateImplBackEndUEAndORT::FDMLDeviceList::GetDMLDevice(ID3D12Device* Device)
{
	for (size_t c = 0; c < Entries.Num(); ++c)
	{
		if (Entries[c].Device == Device)
		{
			return Entries[c].DmlDevice;
		}
	}

	return Add(Device);
}

IDMLDevice* FPrivateImplBackEndUEAndORT::FDMLDeviceList::Add(ID3D12Device* Device)
{
	// Create new DML Device
	IDMLDevice* DmlDevice = nullptr;

	DML_CREATE_DEVICE_FLAGS DmlCreateFlags = DML_CREATE_DEVICE_FLAG_NONE;

#if !UE_BUILD_SHIPPING
	if (D3D12RHI_ShouldCreateWithD3DDebug()
		|| FParse::Param(FCommandLine::Get(), TEXT("d3d12gpuvalidation")) || FParse::Param(FCommandLine::Get(), TEXT("gpuvalidation")))
	{
		DmlCreateFlags |= DML_CREATE_DEVICE_FLAG_DEBUG;
	}
#endif

	HRESULT res = DMLCreateDevice1(Device, DmlCreateFlags, DML_FEATURE_LEVEL_2_0, IID_PPV_ARGS(&DmlDevice));

	// Handle the case if Graphics Debug Tools are not installed
	if (res == DXGI_ERROR_SDK_COMPONENT_MISSING)
	{
		DmlCreateFlags &= ~DML_CREATE_DEVICE_FLAG_DEBUG;

		res = DMLCreateDevice1(Device, DmlCreateFlags, DML_FEATURE_LEVEL_2_0, IID_PPV_ARGS(&DmlDevice));
	}

	if (!DmlDevice)
	{
		UE_LOG(LogNeuralNetworkInference, Warning, TEXT("FDMLDeviceList::Add(): Failed to create DML device, res=%0x."), res);
		return nullptr;
	}

	Entries.Push(DMLDeviceEntry{ Device, DmlDevice });

	return DmlDevice;
}

#endif



/* UNeuralNetwork public functions
 *****************************************************************************/

void UNeuralNetwork::FImplBackEndUEAndORT::WarnAndSetDeviceToCPUIfDX12NotEnabled(ENeuralDeviceType& InOutDeviceType)
{
	if (InOutDeviceType != ENeuralDeviceType::CPU)
	{
		if (!IsGPUConfigCompatible())
		{
			InOutDeviceType = ENeuralDeviceType::CPU;

			const FString RHIName = GDynamicRHI->GetName();
			const FString ErrorMessage = TEXT("On Windows, only DirectX 12 rendering (\"D3D12\") is compatible with the UEAndORT back end of NeuralNetworkInference (NNI). Instead, \"") + RHIName
				+ TEXT("\" was used. You have the following options:\n\n"
					"\t1. (Recommended) Switch Unreal Engine to DX12. In order to do that:\n"
					"\t\t - Go to \"Project Settings\", \"Platforms\", \"Windows\", \"Default RHI\".\n"
					"\t\t - Select \"DirectX 12\".\n"
					"\t\t - Restart Unreal Engine.\n"
					"\t2. Alternatively, switch the network to CPU with UNeuralNetwork::SetDeviceType().\n"
					"\t3. (Not recommended) You could also switch the network to UEOnly with UNeuralNetwork::SetBackEnd().\n\n"
					"Network set to CPU provisionally.");
			UE_LOG(LogNeuralNetworkInference, Warning, TEXT("FImplBackEndUEAndORT::WarnAndSetDeviceToCPUIfDX12NotEnabled(): %s"), *ErrorMessage);
#if WITH_EDITOR
			FMessageDialog::Open(EAppMsgType::Ok, FText::FromString(ErrorMessage));
#endif //WITH_EDITOR
		}
	}
}

bool UNeuralNetwork::FImplBackEndUEAndORT::IsGPUConfigCompatible()
{
#ifdef WITH_UE_AND_ORT_SUPPORT
#ifdef PLATFORM_WIN64
	// Return whether it is DX12
	const FString RHIName = GDynamicRHI->GetName();
	return (RHIName == TEXT("D3D12"));
#endif //PLATFORM_WIN64
#endif //WITH_UE_AND_ORT_SUPPORT

	// If not Windows and/or if WITH_UE_AND_ORT_SUPPORT not defined, then this should return true because GPU will always work
	return true;
}

//bool UNeuralNetwork::FImplBackEndUEAndORT::Load(TSharedPtr<FImplBackEndUEAndORT>& InOutImplBackEndUEAndORT, TArray<bool>& OutAreInputTensorSizesVariable, const TArray<uint8>& InModelReadFromFileInBytes, const FString& InModelFullFilePath, const ENeuralDeviceType InDeviceType)
//{
//	// Initialize and configure InOutImplBackEndUEAndORT
//	const FRedirectCoutAndCerrToUeLog RedirectCoutAndCerrToUeLog;
//	if (!InitializedAndConfigureMembers(InOutImplBackEndUEAndORT, InModelFullFilePath, InDeviceType))
//	{
//		UE_LOG(LogNeuralNetworkInference, Warning, TEXT("FImplBackEndUEAndORT::Load(): InitializedAndConfigureMembers failed."));
//		return false;
//	}
//
//	// Note: This code uses the changes in the ONNX Runtime API, which are not needed for desktop platforms (where ONNX/ProtoBuf is supported)
//	// Fill InModelReadFromFileInBytes from ONNX/ORT file
//	const FString FileExtension = FPaths::GetExtension(InModelFullFilePath, /*bIncludeDot*/ false);
//	const char* const FilePathCharPtr = TCHAR_TO_ANSI(*InModelFullFilePath);
//	// If ONNX file, turn into ORT format first
//	if (FileExtension.Equals(TEXT("onnx"), ESearchCase::IgnoreCase))
//	{
//		FString ONNXPathPart, ONNXFilenamePart, ONNXExtensionPart;
//		FPaths::Split(InModelFullFilePath, ONNXPathPart, ONNXFilenamePart, ONNXExtensionPart);
//		const FString OutputORTOptimizedModelPath = ONNXPathPart / ONNXFilenamePart + TEXT(".ort");
//#ifdef _WIN32
//		InOutImplBackEndUEAndORT->SessionOptions->SetOptimizedModelFilePath(*OutputORTOptimizedModelPath);
//#else
//		InOutImplBackEndUEAndORT->SessionOptions->SetOptimizedModelFilePath(TCHAR_TO_ANSI(*OutputORTOptimizedModelPath));
//#endif //_WIN32
//		// ONNX --> ORT conversion
//		// This session is just temporarily opened so the ORT model file can be generated
//		InOutImplBackEndUEAndORT->Session = MakeUnique<Ort::Session>(*InOutImplBackEndUEAndORT->Environment,
//#ifdef _WIN32
//			*InModelFullFilePath,
//#else
//			FilePathCharPtr,
//#endif
//			*InOutImplBackEndUEAndORT->SessionOptions);
//	
//		// Read model from OutputORTOptimizedModelPath
//		return Load(OutputORTOptimizedModelPath);
//	}
//	// Create session (it should be ORT file at this point), and read InModelReadFromFileInBytes if not empty
//	else if (FileExtension.Equals(TEXT("ort"), ESearchCase::IgnoreCase))
//	{
//		// Read model from InModelFullFilePath
//		std::vector<uint8_t> OutputModelReadFromFileInBytesVector;
//		InOutImplBackEndUEAndORT->Session = MakeUnique<Ort::Session>(*InOutImplBackEndUEAndORT->Environment,
//#ifdef _WIN32
//			*InModelFullFilePath,
//#else
//			FilePathCharPtr,
//#endif
//			*InOutImplBackEndUEAndORT->SessionOptions, &OutputModelReadFromFileInBytesVector);
//	
//		// Fill InModelReadFromFileInBytes
//		const int32 ArraySize = OutputModelReadFromFileInBytesVector.size();
//		if (ArraySize > 0)
//		{
//			InModelReadFromFileInBytes.SetNumUninitialized(ArraySize);
//			FMemory::Memcpy(InModelReadFromFileInBytes.GetData(), &OutputModelReadFromFileInBytesVector[0], ArraySize);
//		}
//	
//		return Load();
//	}
//	// Else
//	UE_LOG(LogNeuralNetworkInference, Warning, TEXT("FImplBackEndUEAndORT::Load(): Unknown file format \"%s\" used."), *FileExtension);
//	return false;
//}

bool UNeuralNetwork::FImplBackEndUEAndORT::Load(TSharedPtr<FImplBackEndUEAndORT>& InOutImplBackEndUEAndORT, TArray<bool>& OutAreInputTensorSizesVariable, const TArray<uint8>& InModelReadFromFileInBytes, const FString& InModelFullFilePath, const ENeuralDeviceType InDeviceType, const ENeuralDeviceType InInputDeviceType, const ENeuralDeviceType InOutputDeviceType)
{
#ifdef WITH_UE_AND_ORT_SUPPORT
#if WITH_EDITOR
	try
#endif //WITH_EDITOR
	{
		const FRedirectCoutAndCerrToUeLog RedirectCoutAndCerrToUeLog;

		// Initialize and configure InOutImplBackEndUEAndORT
		if (!UNeuralNetwork::FImplBackEndUEAndORT::InitializedAndConfigureMembers(InOutImplBackEndUEAndORT, InModelFullFilePath, InDeviceType))
		{
			UE_LOG(LogNeuralNetworkInference, Warning, TEXT("FImplBackEndUEAndORT::Load(): InitializedAndConfigureMembers failed."));
			return false;
		}

		// Create session from model saved in InModelReadFromFileInBytes (if not empty)
		if (InModelReadFromFileInBytes.Num() > 0)
		{
			// Read model from ModelReadFromFileInBytesVector
			InOutImplBackEndUEAndORT->Session = MakeUnique<Ort::Session>(*InOutImplBackEndUEAndORT->Environment, InModelReadFromFileInBytes.GetData(), InModelReadFromFileInBytes.Num(), *InOutImplBackEndUEAndORT->SessionOptions);


#ifdef PLATFORM_WIN64
			// Check if resource allocator is properly initialized
			if (InOutImplBackEndUEAndORT->DmlGPUAllocator.IsValid() && !InOutImplBackEndUEAndORT->DmlGPUAllocator->IsValid())
			{
				UE_LOG(LogNeuralNetworkInference, Warning, TEXT("FImplBackEndUEAndORT::Load() DirectML GPU resource allocator has failed to initialize."));
				return false;
			}
#endif
		}
		// Else
		else
		{
			UE_LOG(LogNeuralNetworkInference, Warning, TEXT("FImplBackEndUEAndORT::Load(): InModelReadFromFileInBytes was empty."));
			return false;
		}
		
		// Sanity check if device type is CPU and to make sure that input and/or output is also on the CPU
		ENeuralDeviceType	InputDeviceType = InInputDeviceType;
		ENeuralDeviceType	OutputDeviceType = InOutputDeviceType;

		if (InDeviceType == ENeuralDeviceType::CPU && (InInputDeviceType == ENeuralDeviceType::GPU || InOutputDeviceType == ENeuralDeviceType::GPU))
		{
			UE_LOG(LogNeuralNetworkInference, Warning, TEXT("FImplBackEndUEAndORT::Load(): DeviceType is CPU but Input and/or Output is set to GPU, setting all to CPU."));
			InputDeviceType = ENeuralDeviceType::CPU;
			OutputDeviceType = ENeuralDeviceType::CPU;
		}

		if (!InOutImplBackEndUEAndORT->ConfigureTensors(InOutImplBackEndUEAndORT->InputTensors, &OutAreInputTensorSizesVariable, InputDeviceType, OutputDeviceType))
		{
			UE_LOG(LogNeuralNetworkInference, Warning, TEXT("FImplBackEndUEAndORT::Load(): Failed to configure input tensors."));
			return false;
		}

		if (!InOutImplBackEndUEAndORT->ConfigureTensors(InOutImplBackEndUEAndORT->OutputTensors, nullptr, InputDeviceType, OutputDeviceType))
		{
			UE_LOG(LogNeuralNetworkInference, Warning, TEXT("FImplBackEndUEAndORT::Load(): Failed to configure output tensors."));
			return false;
		}

		return true;
	}
#if WITH_EDITOR
	catch (const std::exception& Exception)
	{
		UE_LOG(LogNeuralNetworkInference, Error, TEXT("%s"), UTF8_TO_TCHAR(Exception.what()));
		return false;
	}
#endif //WITH_EDITOR

#else //WITH_UE_AND_ORT_SUPPORT
	UE_LOG(LogNeuralNetworkInference, Warning, TEXT("FImplBackEndUEAndORT::Load(): Platform or Operating System not suported yet for UEAndORT BackEnd. Set BackEnd to ENeuralBackEnd::Auto (recommended) or ENeuralBackEnd::UEOnly for this platform."));
	return false;
#endif //WITH_UE_AND_ORT_SUPPORT
}

#ifdef WITH_UE_AND_ORT_SUPPORT
void UNeuralNetwork::FImplBackEndUEAndORT::ClearResources()
{
#ifdef PLATFORM_WIN64
	if (DmlGPUAllocator.IsValid() && DmlGPUAllocator->IsValid())
	{ 
		const int32 Num = DmlGPUResources.Num();
		for (int32 Index = 0; Index < Num; ++Index)
		{
			DmlGPUAllocator->FreeGPUAllocation(DmlGPUResources[Index]);
		}

		DmlGPUResources.Reset(0);
		DmlGPUAllocator.Reset(nullptr);
	}
#endif //PLATFORM_WIN64
}
#endif //WITH_UE_AND_ORT_SUPPORT

void UNeuralNetwork::FImplBackEndUEAndORT::Run(const ENeuralNetworkSynchronousMode InSynchronousMode, const ENeuralDeviceType InInputDeviceType, const ENeuralDeviceType InOutputDeviceType)
{
#ifdef WITH_UE_AND_ORT_SUPPORT
#if WITH_EDITOR
	try
#endif //WITH_EDITOR
	{
		const FRedirectCoutAndCerrToUeLog RedirectCoutAndCerrToUeLog;

		// @todo: Temporarily disabled until we connect GPU input/output between UE and ORT
		if (InInputDeviceType == ENeuralDeviceType::GPU)
		{
			UE_LOG(LogNeuralNetworkInference, Warning, TEXT("FImplBackEndUEAndORT::Run(): InputDeviceType must be set to CPU for now."));
			return;
		}

		// Run UNeuralNetwork
		if (InSynchronousMode == ENeuralNetworkSynchronousMode::Synchronous)
		{
			Session->Run(Ort::RunOptions{ nullptr },
				InputTensorNames.GetData(), &InputOrtTensors[0], InputTensorNames.Num(),
				OutputTensorNames.GetData(), &OutputOrtTensors[0], OutputTensorNames.Num());
		}
		else if (InSynchronousMode == ENeuralNetworkSynchronousMode::Asynchronous)
		{
			UE_LOG(LogNeuralNetworkInference, Warning, TEXT("FImplBackEndUEAndORT::Run(): SynchronousMode = %d not implemented yet. Use SynchronousMode = Synchronous."), (int32)InSynchronousMode);
		}
		else
		{
			UE_LOG(LogNeuralNetworkInference, Warning, TEXT("FImplBackEndUEAndORT::Run(): Unknown SynchronousMode = %d."), (int32)InSynchronousMode);
		}
	}
#if WITH_EDITOR
	catch (const std::exception& Exception)
	{
		UE_LOG(LogNeuralNetworkInference, Error, TEXT("%s"), UTF8_TO_TCHAR(Exception.what()));
	}
#endif //WITH_EDITOR

#else //WITH_UE_AND_ORT_SUPPORT
	UE_LOG(LogNeuralNetworkInference, Warning, TEXT("FImplBackEndUEAndORT::Run(): Platform or Operating System not suported yet for UEAndORT BackEnd. Set BackEnd to ENeuralBackEnd::Auto or ENeuralBackEnd::UEOnly for this platform."));
#endif //WITH_UE_AND_ORT_SUPPORT
}



/* UNeuralNetwork private functions
 *****************************************************************************/

#ifdef WITH_UE_AND_ORT_SUPPORT

bool UNeuralNetwork::FImplBackEndUEAndORT::InitializedAndConfigureMembers(TSharedPtr<FImplBackEndUEAndORT>& InOutImplBackEndUEAndORT, const FString& InModelFullFilePath, const ENeuralDeviceType InDeviceType)
{
	// Initialize InOutImplBackEndUEAndORT
	if (!InOutImplBackEndUEAndORT.IsValid())
	{
		InOutImplBackEndUEAndORT = MakeShared<FImplBackEndUEAndORT>();

		// Set up ORT and create an environment
		Ort::InitApi();
		const char* const ModelFullFilePathCharPtr = TCHAR_TO_ANSI(*InModelFullFilePath);
		InOutImplBackEndUEAndORT->Environment = MakeUnique<Ort::Env>(ORT_LOGGING_LEVEL_WARNING, ModelFullFilePathCharPtr); // Any unique string would work, it does not need to be the file path

		InOutImplBackEndUEAndORT->Allocator = MakeUnique<Ort::AllocatorWithDefaultOptions>();

		InOutImplBackEndUEAndORT->AllocatorInfo = MakeUnique<Ort::MemoryInfo>(Ort::MemoryInfo::CreateCpu(OrtDeviceAllocator, OrtMemTypeCPU));
	}

	InOutImplBackEndUEAndORT->ClearResources();

	// Configure InOutImplBackEndUEAndORT
	if (!InOutImplBackEndUEAndORT->ConfigureMembers(InDeviceType))
	{
		UE_LOG(LogNeuralNetworkInference, Warning, TEXT("FImplBackEndUEAndORT::InitializedAndConfigureMembers(): ConfigureMembers failed."));
		return false;
	}

	return true;
}

bool UNeuralNetwork::FImplBackEndUEAndORT::ConfigureMembers(const ENeuralDeviceType InDeviceType)
{
	// Configure Session
	SessionOptions = MakeUnique<Ort::SessionOptions>();

	// Configure number threads
	SessionOptions->SetIntraOpNumThreads(2);
	// Uncomment if you want to change the priority of the threads, by default is TPri_Normal
	//SessionOptions->SetPriorityOpThreads(EThreadPriority::TPri_Normal);

	// Configure Provider
	// GPU
	if (InDeviceType == ENeuralDeviceType::GPU)
	{
#ifdef PLATFORM_WIN64
		// To create a DirectML device we need to check that we're using DX12 first
		if (!IsGPUConfigCompatible())
		{
			UE_LOG(LogNeuralNetworkInference, Warning, TEXT("FImplBackEndUEAndORT::ConfigureMembers(): UEAndORT back end for GPU needs DX12 enabled."));
			return false;
		}

		// Get adapter's D3D12 device that we would like to share with DirectML execution provider
		// NOTE: For now we're only using first device that has Dadapter 0 and device 0
		FD3D12DynamicRHI* Rhi = static_cast<FD3D12DynamicRHI*>(GDynamicRHI);

		if (Rhi->GetNumAdapters() > 1 || Rhi->GetAdapter(0).GetDesc().NumDeviceNodes > 1)
		{
			UE_LOG(LogNeuralNetworkInference, Warning, TEXT("FImplBackEndUEAndORT::ConfigureMembers(): There are multiple (%d) adapters and/or multiple (%d) devices, using device at index 0."),
				Rhi->GetNumAdapters(), Rhi->GetAdapter(0).GetDesc().NumDeviceNodes);
			return false;
		}

		ID3D12Device* NativeDevice = Rhi->GetAdapter(0).GetD3DDevice();

		// Make sure that we have one DMLDevice per D3D12 device
		IDMLDevice* DmlDevice = FPrivateImplBackEndUEAndORT::GetDMLDeviceThreadSafe(NativeDevice);

		if (!DmlDevice)
		{
			UE_LOG(LogNeuralNetworkInference, Warning, TEXT("FImplBackEndUEAndORT::ConfigureMembers(): Invalid DML device found."));
			return false;
		}

		// Get a ID3D12CommandQueue as well
		// TODO: Should we create our own queue?
		ID3D12CommandQueue* NativeCmdQ = Rhi->RHIGetD3DCommandQueue();

		// ORT GPU (Direct ML)
		SessionOptions->SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL); // ORT_ENABLE_ALL, ORT_ENABLE_EXTENDED, ORT_ENABLE_BASIC, ORT_DISABLE_ALL

		DmlGPUAllocator = MakeUnique<Ort::DMLGPUResourceAllocator>();

		// Set DirectML execution provider options
		DmlProviderOptions.Reset(new OrtDMLProviderOptions());
		DmlProviderOptions->dml_device = DmlDevice;
		DmlProviderOptions->cmd_queue = NativeCmdQ;
		DmlProviderOptions->resource_allocator = DmlGPUAllocator->GetAllocatorAddressOf();

		if (OrtSessionOptionsAppendExecutionProviderWithOptions_DML(*SessionOptions, DmlProviderOptions.Get())) // OrtSessionOptionsAppendExecutionProvider_DML(*SessionOptions, 0) without sharing
		{
			UE_LOG(LogNeuralNetworkInference, Warning, TEXT("FImplBackEndUEAndORT::ConfigureMembers(): Some error occurred when using OrtSessionOptionsAppendExecutionProviderEx_DML()."));
			return false;
		}
		return true; // @todo: Remove this line when NNI_HLSL is working
#else
		UE_LOG(LogNeuralNetworkInference, Warning, TEXT("FImplBackEndUEAndORT::ConfigureMembers(): GPU mode only supported in Windows for now. Please, switch to CPU or to Windows."));

		//SessionOptions->SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_DISABLE_ALL); // ORT_ENABLE_ALL, ORT_ENABLE_EXTENDED, ORT_ENABLE_BASIC, ORT_DISABLE_ALL
		//if (OrtSessionOptionsAppendExecutionProvider_NNI_HLSL(*SessionOptions, 0))
		//{
		//	UE_LOG(LogNeuralNetworkInference, Warning, TEXT("FImplBackEndUEAndORT::ConfigureMembers(): Some error occurred."));
		//	return false;
		//}
#endif //PLATFORM_WIN64
	}
	// CPU
	//else // @todo: Uncomment this line when NNI_HLSL is working
	{
#ifdef WITH_NNI_CPU_NOT_RECOMMENDED
		// NNI CPU (Deprecated)
		SessionOptions->SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_DISABLE_ALL); // ORT_ENABLE_ALL, ORT_ENABLE_EXTENDED, ORT_ENABLE_BASIC, ORT_DISABLE_ALL
		if (OrtSessionOptionsAppendExecutionProvider_NNI_CPU(*SessionOptions))
		{
			UE_LOG(LogNeuralNetworkInference, Warning, TEXT("FImplBackEndUEAndORT::ConfigureMembers(): OrtSessionOptionsAppendExecutionProvider_NNI_CPU failed."));
			return false;
		}
#else
		// ORT CPU
		SessionOptions->SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL); // ORT_ENABLE_ALL, ORT_ENABLE_EXTENDED, ORT_ENABLE_BASIC, ORT_DISABLE_ALL
#endif //ORT_CPU
	}

	return true;
}

bool UNeuralNetwork::FImplBackEndUEAndORT::ConfigureTensors(TArray<FNeuralTensor>& OutTensors, TArray<bool>* OutAreInputTensorSizesVariable, const ENeuralDeviceType InInputDeviceType, const ENeuralDeviceType InOutputDeviceType)
{
	const bool bIsInput = (OutAreInputTensorSizesVariable != nullptr);
	TArray<const char*> TensorNames;
	TArray<ENeuralDataType> TensorDataTypes;
	TArray<TArray<int64>> TensorSizes;
	TArray<ENeuralTensorTypeGPU> TensorGPUTypes;

	const uint32 NumberTensors = bIsInput ? Session->GetInputCount() : Session->GetOutputCount();
	if (OutAreInputTensorSizesVariable)
	{
		OutAreInputTensorSizesVariable->SetNum(NumberTensors);
	}
	for (uint32 TensorIndex = 0; TensorIndex < NumberTensors; ++TensorIndex)
	{
		// Get node name
		{
			const char* TensorName = bIsInput ? Session->GetInputName(TensorIndex, *Allocator) : Session->GetOutputName(TensorIndex, *Allocator);
			TensorNames.Emplace(TensorName);
		}

		// Get node type
		Ort::TypeInfo CurrentTypeInfo = bIsInput ? Session->GetInputTypeInfo(TensorIndex) : Session->GetOutputTypeInfo(TensorIndex);

		Ort::TensorTypeAndShapeInfo CurrentTensorInfo = CurrentTypeInfo.GetTensorTypeAndShapeInfo();

		ENeuralDataType TensorDataType;
		{
			const ONNXTensorElementDataType ONNXTensorElementDataTypeEnum = CurrentTensorInfo.GetElementType();
			if (ONNXTensorElementDataTypeEnum == ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT)
			{
				TensorDataType = ENeuralDataType::Float;
			}
			//else if (ONNXTensorElementDataTypeEnum == ONNX_TENSOR_ELEMENT_DATA_TYPE_INT32)
			//{
			//	TensorDataType = ENeuralDataType::Int32;
			//}
			//else if (ONNXTensorElementDataTypeEnum == ONNX_TENSOR_ELEMENT_DATA_TYPE_INT64)
			//{
			//	TensorDataType = ENeuralDataType::Int64;
			//}
			//else if (ONNXTensorElementDataTypeEnum == ONNX_TENSOR_ELEMENT_DATA_TYPE_UINT32)
			//{
			//	TensorDataType = ENeuralDataType::UInt32;
			//}
			//else if (ONNXTensorElementDataTypeEnum == ONNX_TENSOR_ELEMENT_DATA_TYPE_UINT64)
			//{
			//	TensorDataType = ENeuralDataType::UInt64;
			//}
			else
			{
				TensorDataType = ENeuralDataType::None;
				UE_LOG(LogNeuralNetworkInference, Warning, TEXT("FImplBackEndUEAndORT::ConfigureTensors(): ONNXTensorElementDataTypeEnum = %d not implemented yet."), (int32)ONNXTensorElementDataTypeEnum);
				return false;
			}
		}
		TensorDataTypes.Push(TensorDataType);

		// Get input shapes/dims
		TArray<int64> CurrentTensorSizes;
		{
			for (const int64_t CurrentTensorSize : CurrentTensorInfo.GetShape())
			{
				if (OutAreInputTensorSizesVariable)
				{
					(*OutAreInputTensorSizesVariable)[TensorIndex] |= (CurrentTensorSize < 0);
				}
				// Negative (variable) dimensions not implemented yet
				if (CurrentTensorSize < 0)
				{
					CurrentTensorSizes.Push(1);
					UE_LOG(LogNeuralNetworkInference, Display,
						TEXT("Negative (i.e., variable) dimensions not allowed yet, hard-coded to 1. Let us know if you really need variable dimensions."
							" Keep in mind that fixed sizes might allow additional optimizations and speedup of the network during Run()."));
				}
				else
				{
					CurrentTensorSizes.Push(CurrentTensorSize);
				}
			}
		}
		TensorSizes.Push(CurrentTensorSizes);

		// @todo: Should caller specify tensor GPU type?
		// Input tensor GPU type is set to Generic
		// Output tensor GPU type is set to Output (i.e. data should not be copied from CPU)
		ENeuralTensorTypeGPU	TensorGPUType;

		if (bIsInput)
		{
			TensorGPUType = ENeuralTensorTypeGPU::Generic;
		}
		else
		{
			TensorGPUType = InOutputDeviceType == ENeuralDeviceType::GPU ? ENeuralTensorTypeGPU::Output : ENeuralTensorTypeGPU::Generic;
		}

		TensorGPUTypes.Push(TensorGPUType);

		CurrentTypeInfo.release();
	}

	return SetTensorsFromNetwork(OutTensors, TensorNames, TensorDataTypes, TensorSizes, TensorGPUTypes, bIsInput);
}

bool UNeuralNetwork::FImplBackEndUEAndORT::SetTensorsFromNetwork(TArray<FNeuralTensor>& OutTensors, TArray<const char*>& InTensorNames, TArray<ENeuralDataType>& InTensorDataTypes,
	TArray<TArray<int64>>& InSizes, TArray<ENeuralTensorTypeGPU>& InTensorGPUTypes, const bool bIsInput)
{
	const int32 TensorNumber = InTensorNames.Num();
	if (InTensorDataTypes.Num() != TensorNumber || InSizes.Num() != TensorNumber)
	{
		UE_LOG(LogNeuralNetworkInference, Warning, TEXT("FImplBackEndUEAndORT::SetTensorsFromNetwork(): InTensorNames.Num() == InTensorDataTypes.Num() == InSizes.Num() failed, %d vs. %d vs. %d."),
			InTensorNames.Num(), InTensorDataTypes.Num(), InSizes.Num());
		return false;
	}

	// Swap variables
	TArray<const char*>& TensorNames = (bIsInput ? InputTensorNames : OutputTensorNames);
	Swap(TensorNames, InTensorNames);

	// Note: Switching from/to CPU to/from GPU would cause the FNeuralTensors to be re-initialized. We need to avoid that. For that, we will only re-allocate the tensors...
	// - If bAreTensorsAlreadyCreatedWithRightNames == false, meaning tensors had not been created until now for this network.
	// - And if the existing tensors have the right size, given that SetNumUninitialized() only re-allocates them if their size has changed.

	// Fill bAreTensorsAlreadyCreatedWithRightNames - Check if tensors already created with the right names
	bool bAreTensorsAlreadyCreatedWithRightNames = (OutTensors.Num() == TensorNames.Num());
	if (bAreTensorsAlreadyCreatedWithRightNames)
	{
		for (int32 TensorIndex = 0; TensorIndex < TensorNumber; ++TensorIndex)
		{
			if (OutTensors[TensorIndex].GetName() != ANSI_TO_TCHAR(TensorNames[TensorIndex]))
			{
				bAreTensorsAlreadyCreatedWithRightNames = false;
				break;
			}
		}
	}

	// Assign name to each input/output tensor
	if (!bAreTensorsAlreadyCreatedWithRightNames)
	{
		OutTensors.Empty();
		for (int32 TensorIndex = 0; TensorIndex < TensorNumber; ++TensorIndex)
		{
			const char* TensorName = TensorNames[TensorIndex];
			OutTensors.Emplace(FNeuralTensor(ANSI_TO_TCHAR(TensorName), InTensorGPUTypes[TensorIndex]));
		}
	}
	ensureMsgf(OutTensors.Num() == TensorNumber, TEXT("OutTensors.Num() == TensorNumber failed, %d != %d."), OutTensors.Num(), TensorNumber);

	// Config each TensorIndex
	TArray<Ort::Value>& OrtTensors = (bIsInput ? InputOrtTensors : OutputOrtTensors);
	for (int32 TensorIndex = 0; TensorIndex < TensorNumber; ++TensorIndex)
	{
		if (OrtTensors.Num() <= TensorIndex)
		{
			OrtTensors.Emplace(Ort::Value(nullptr));
		}

#ifdef PLATFORM_WIN64
		if (InTensorGPUTypes[TensorIndex] == ENeuralTensorTypeGPU::Generic)
		{
			// Pre-allocate TArray (if size is different)
			OutTensors[TensorIndex].SetNumUninitialized(InSizes[TensorIndex], InTensorDataTypes[TensorIndex]);
			// Link tensor with ORT blob
			LinkTensorToONNXRuntime(OutTensors, OrtTensors, *AllocatorInfo, TensorIndex);
		}
		else if (InTensorGPUTypes[TensorIndex] == ENeuralTensorTypeGPU::Output)
		{
			// @todo: should we remove this? It's currently used to read memory from GPU to CPU
			OutTensors[TensorIndex].SetNumUninitialized(InSizes[TensorIndex], InTensorDataTypes[TensorIndex]);

			OutTensors[TensorIndex].SetEnableGPU(true);

			// @todo: This requires SetNumUnitialized() to be run, otherwise Size and Volume will be set to 0
			void* D3DResource = nullptr;

			if (!OutTensors[TensorIndex].InitPooledBuffer(&D3DResource))
			{
				UE_LOG(LogNeuralNetworkInference, Warning, TEXT("FImplBackEndUEAndORT::SetTensorsFromNetwork(): Failed to initialize pooled buffer"));
				return false;
			}

			// Link tensor with ORT blob
			if (!LinkTensorResourceToONNXRuntime(OutTensors[TensorIndex], OrtTensors[TensorIndex], D3DResource))
			{
				UE_LOG(LogNeuralNetworkInference, Warning, TEXT("FImplBackEndUEAndORT::SetTensorsFromNetwork(): Failed to link GPU resource to ONNX runtime"));
				return false;
			}
		}
#else
		// Pre-allocate TArray (if size is different)
		OutTensors[TensorIndex].SetNumUninitialized(InSizes[TensorIndex], InTensorDataTypes[TensorIndex]);
		// Link tensor with ORT blob
		LinkTensorToONNXRuntime(OutTensors, OrtTensors, *AllocatorInfo, TensorIndex);
#endif
	}

	return true;
}

void UNeuralNetwork::FImplBackEndUEAndORT::LinkTensorToONNXRuntime(TArray<FNeuralTensor>& InOutTensors, TArray<Ort::Value>& InOutOrtTensors, Ort::MemoryInfo& InOutAllocatorInfo, const int32 InTensorIndex)
{
	const TArray<int64>& Sizes = InOutTensors[InTensorIndex].GetSizes();
	if (Sizes.Num() > 0 && InOutTensors[InTensorIndex].Num() > 0)
	{
		FNeuralTensor& Tensor = InOutTensors[InTensorIndex];
		const int64 Volume = Tensor.Num();
		const int32 ArrayDimensions = Sizes.Num();

		const ENeuralDataType NeuralDataType = Tensor.GetDataType();
		if (NeuralDataType == ENeuralDataType::Float)
		{
#ifdef _WIN32
			const TArray<int64_t>& SizesInt64t = Sizes;
#else
			checkf(sizeof(int64) == sizeof(int64_t), TEXT("int64 and int64_t should both have the same size."));
			TArray<int64_t> SizesInt64t;
			SizesInt64t.SetNumUninitialized(ArrayDimensions);
			FMemory::Memcpy(SizesInt64t.GetData(), (int64_t*)Sizes.GetData(), sizeof(int64_t) * ArrayDimensions);
#endif //_WIN32
			InOutOrtTensors[InTensorIndex] = Ort::Value::CreateTensor<float>(InOutAllocatorInfo, Tensor.GetDataCasted<float>(), Volume, SizesInt64t.GetData(), ArrayDimensions);
		}
		//else if (NeuralDataType == ENeuralDataType::Double)
		//{
		//	InOutOrtTensors[InTensorIndex] = Ort::Value::CreateTensor<double>(InOutAllocatorInfo, Tensor.GetDataCasted<double>(), Volume, Sizes.GetData(), ArrayDimensions);
		//}
		else
		{
			UE_LOG(LogNeuralNetworkInference, Warning, TEXT("FImplBackEndUEAndORT::LinkTensorToONNXRuntime(): Not implemented (yet) for ENeuralDataType = %d."), (int32)NeuralDataType);
		}
	}
}

#ifdef PLATFORM_WIN64

bool UNeuralNetwork::FImplBackEndUEAndORT::LinkTensorResourceToONNXRuntime(FNeuralTensor& InOutTensor, Ort::Value& InOutOrtTensor, void* D3DResource)
{
	if (!DmlGPUAllocator.IsValid() || !DmlGPUAllocator->IsValid())
	{
		UE_LOG(LogNeuralNetworkInference, Warning, TEXT("FImplBackEndUEAndORT::LinkTensorResourceToONNXRuntime(): DmlGPUAllocator is not valid"));
		return false;
	}

	void* DmlGPUAllocation = DmlGPUAllocator->GPUAllocationFromD3DResource(D3DResource);
	if (!DmlGPUAllocation)
	{
		UE_LOG(LogNeuralNetworkInference, Warning, TEXT("FImplBackEndUEAndORT::LinkTensorResourceToONNXRuntime(): DmlGPUAllocation is NULL"));
		return false;
	}

	DmlGPUResources.Emplace(DmlGPUAllocation);

	const TArray<int64>& Sizes = InOutTensor.GetSizes();
	if (Sizes.Num() > 0 && InOutTensor.Num() > 0)
	{
		const int32 ArrayDimensions = Sizes.Num();

		const ENeuralDataType NeuralDataType = InOutTensor.GetDataType();
		if (NeuralDataType == ENeuralDataType::Float)
		{
#ifdef _WIN32
			const TArray<int64_t>& SizesInt64t = Sizes;
#else
			checkf(sizeof(int64) == sizeof(int64_t), TEXT("int64 and int64_t should both have the same size."));
			TArray<int64_t> SizesInt64t;
			SizesInt64t.SetNumUninitialized(ArrayDimensions);
			FMemory::Memcpy(SizesInt64t.GetData(), (int64_t*)Sizes.GetData(), sizeof(int64_t) * ArrayDimensions);
#endif //_WIN32
			InOutOrtTensor = Ort::Value::CreateTensor(DmlGPUAllocator->GetProviderMemoryInfo(), DmlGPUAllocation, InOutTensor.NumInBytes(), SizesInt64t.GetData(), ArrayDimensions, ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT);
		}
		//else if (NeuralDataType == ENeuralDataType::Double)
		//{
		//	InOutOrtTensor = Ort::Value::CreateTensor(DmlGPUAllocator->GetProviderMemoryInfo(), DmlGPUAllocation, InOutTensor.NumInBytes(), SizesInt64t.GetData(), ArrayDimensions, ONNX_TENSOR_ELEMENT_DATA_TYPE_DOUBLE);
		//}
		else
		{
			UE_LOG(LogNeuralNetworkInference, Warning, TEXT("FImplBackEndUEAndORT::LinkTensorToONNXRuntime(): Not implemented (yet) for ENeuralDataType = %d."), (int32)NeuralDataType);
			return false;
		}
	}

	return true;
}

#endif

#endif //WITH_UE_AND_ORT_SUPPORT
