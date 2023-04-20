// Copyright Epic Games, Inc. All Rights Reserved.

#include "NNERuntimeORT.h"
#include "NNERuntimeORTModel.h"
#include "NNERuntimeORTUtils.h"
#include "NNEUtilsModelOptimizer.h"
#include "NNECoreAttributeMap.h"
#include "NNECoreModelData.h"
#include "NNECoreModelOptimizerInterface.h"
#include "NNEProfilingTimer.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(NNERuntimeORT)

FGuid UNNERuntimeORTGpuImpl::GUID = FGuid((int32)'O', (int32)'G', (int32)'P', (int32)'U');
int32 UNNERuntimeORTGpuImpl::Version = 0x00000001;

bool UNNERuntimeORTGpuImpl::CanCreateModelData(FString FileType, TConstArrayView<uint8> FileData) const
{
	return FileType.Compare("onnx", ESearchCase::IgnoreCase) == 0;
}

TArray<uint8> UNNERuntimeORTGpuImpl::CreateModelData(FString FileType, TConstArrayView<uint8> FileData)
{
	if (!CanCreateModelData(FileType, FileData))
	{
		return {};
	}

	TUniquePtr<UE::NNECore::Internal::IModelOptimizer> Optimizer = UE::NNEUtils::Internal::CreateONNXToONNXModelOptimizer();

	FNNEModelRaw InputModel;
	InputModel.Data = FileData;
	InputModel.Format = ENNEInferenceFormat::ONNX;
	FNNEModelRaw OutputModel;
	UE::NNECore::Internal::FOptimizerOptionsMap Options;
	if (!Optimizer->Optimize(InputModel, OutputModel, Options))
	{
		return {};
	}

	int32 GuidSize = sizeof(UNNERuntimeORTGpuImpl::GUID);
	int32 VersionSize = sizeof(UNNERuntimeORTGpuImpl::Version);
	TArray<uint8> Result;
	FMemoryWriter Writer(Result);
	Writer << UNNERuntimeORTGpuImpl::GUID;
	Writer << UNNERuntimeORTGpuImpl::Version;
	Writer.Serialize(OutputModel.Data.GetData(), OutputModel.Data.Num());
	return Result;
}

void UNNERuntimeORTGpuImpl::Init(ENNERuntimeORTGpuProvider InProvider)
{
	check(!ORTEnvironment.IsValid());
	ORTEnvironment = MakeUnique<Ort::Env>();

	Provider = InProvider;
}

FString UNNERuntimeORTGpuImpl::GetRuntimeName() const
{
	switch (Provider)
	{
		case ENNERuntimeORTGpuProvider::Dml:  return TEXT("NNERuntimeORTDml");
		case ENNERuntimeORTGpuProvider::Cuda: return TEXT("NNERuntimeORTCuda");
		default:   return TEXT("NNERuntimeORT_NONE");
	}
}

#if PLATFORM_WINDOWS
bool UNNERuntimeORTGpuImpl::CanCreateModelGPU(TObjectPtr<UNNEModelData> ModelData) const
{
	check(ModelData != nullptr);

	int32 GuidSize = sizeof(UNNERuntimeORTGpuImpl::GUID);
	int32 VersionSize = sizeof(UNNERuntimeORTGpuImpl::Version);
	TConstArrayView<uint8> Data = ModelData->GetModelData(GetRuntimeName());

	if (Data.Num() <= GuidSize + VersionSize)
	{
		return false;
	}

	bool bResult = FGenericPlatformMemory::Memcmp(&(Data[0]), &(UNNERuntimeORTGpuImpl::GUID), GuidSize) == 0;
	bResult &= FGenericPlatformMemory::Memcmp(&(Data[GuidSize]), &(UNNERuntimeORTGpuImpl::Version), VersionSize) == 0;
	return bResult;
}

TUniquePtr<UE::NNECore::IModelGPU> UNNERuntimeORTGpuImpl::CreateModelGPU(TObjectPtr<UNNEModelData> ModelData)
{
	check(ModelData != nullptr);
	check(ORTEnvironment.IsValid());

	if (!CanCreateModelGPU(ModelData))
	{
		return TUniquePtr<UE::NNECore::IModelGPU>();
	}

	const UE::NNERuntimeORT::Private::FRuntimeConf InConf;
	UE::NNERuntimeORT::Private::FModelORT* Model = nullptr;
	TConstArrayView<uint8> Data = ModelData->GetModelData(GetRuntimeName());

	switch (Provider)
	{
		case ENNERuntimeORTGpuProvider::Dml:  
			Model = new UE::NNERuntimeORT::Private::FModelORTDml(ORTEnvironment.Get(), InConf);
			break;
		case ENNERuntimeORTGpuProvider::Cuda: 
			Model = new UE::NNERuntimeORT::Private::FModelORTCuda(ORTEnvironment.Get(), InConf);
			break;
		default:
			UE_LOG(LogNNE, Error, TEXT("Failed to create model for ORT GPU runtime, unsupported provider. Runtime will not be functional."));
			return TUniquePtr<UE::NNECore::IModelGPU>();
	}

	if (!Model->Init(Data))
	{
		delete Model;
		return TUniquePtr<UE::NNECore::IModelGPU>();
	}
	UE::NNECore::IModelGPU* IModel = static_cast<UE::NNECore::IModelGPU*>(Model);
	return TUniquePtr<UE::NNECore::IModelGPU>(IModel);
}

#else // PLATFORM_WINDOWS

bool UNNERuntimeORTGpuImpl::CanCreateModelGPU(TObjectPtr<UNNEModelData> ModelData) const
{
	return false;
}

TUniquePtr<UE::NNECore::IModelGPU> UNNERuntimeORTGpuImpl::CreateModelGPU(TObjectPtr<UNNEModelData> ModelData)
{
	return TUniquePtr<UE::NNECore::IModelGPU>();
}

#endif // PLATFORM_WINDOWS