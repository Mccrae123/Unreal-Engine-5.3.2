// Copyright Epic Games, Inc. All Rights Reserved.

#include "ModelUnitTester.h"
#include "NeuralNetworkInferenceQATimer.h"
#include "NeuralNetworkInferenceQAUtils.h"
#include "Misc/Paths.h"



/* FModelUnitTester static public functions
 *****************************************************************************/

bool FModelUnitTester::GlobalTest(const FString& InProjectContentDir, const FString& InModelZooRelativeDirectory)
{
	// Model names, input values, and number of repetitions for profiling
#ifdef WITH_UE_AND_ORT_SUPPORT
	const TArray<FString> ModelNames({ TEXT("MLRigDeformer"), TEXT("cloth_network"), TEXT("HS"), TEXT("RL") });
	const TArray<float> InputArrayValues({ 1.f, 0.f, -1.f, 100.f, -100.f, 0.5f, -0.5f }); // This one can be shorter than CPU/GPUGroundTruths
	const TArray<int32> CPURepetitions({ 1000, 1000,  50, 1000 });
	const TArray<int32> GPURepetitions({ 1000, 1000, 100, 1000 });
#else
	UE_LOG(LogNeuralNetworkInferenceQA, Display, TEXT("--------------- Some FModelUnitTester tests skipped (only if WITH_UE_AND_ORT_SUPPORT)."));
	const TArray<FString> ModelNames({ TEXT("MLRigDeformer"), TEXT("cloth_network") });
	const TArray<float> InputArrayValues({ 1.f, 0.f });
	const TArray<int32> CPURepetitions({ 100, 0 });
	const TArray<int32> GPURepetitions({ 100, 100 });
UE_LOG(LogNeuralNetworkInferenceQA, Display, TEXT("--------------- FModelUnitTester test skipped (only if WITH_UE_AND_ORT_SUPPORT)."));
return true;
#endif //WITH_UE_AND_ORT_SUPPORT

	// Ground truths
	const TArray<TArray<double>> CPUGroundTruths({ {3.728547, 0.008774, 4.595651, 212.193216, 742.434561, 4.250668, 4.717748}, {0.042571, 0.023693, 0.015783, 13.100505, 8.050994, 0.028807, 0.016387},
		{138.372906, 126.753839, 127.287254, 130.316062, 127.303424, 124.800896, 126.546051}, {0.488662, 0.472437, 0.478862, 0.522685, 0.038322, 0.480848, 0.483821}});
	const TArray<TArray<double>> GPUGroundTruths({ {3.728547, 0.008774, 4.595651, 212.193208, 742.434578, 4.250668, 4.717748}, {0.042571, 0.023693, 0.015783, 13.100504, 8.050994, 0.028807, 0.016387},
		{138.373184, 126.754100, 127.287398, 130.316194, 127.303495, 124.801134, 126.5462530}, {0.488662, 0.472437, 0.478862, 0.522685, 0.038322, 0.480848, 0.483821}});

	// Run tests
	return ModelLoadAccuracyAndSpeedTests(InProjectContentDir, InModelZooRelativeDirectory, ModelNames, InputArrayValues, CPUGroundTruths, GPUGroundTruths, CPURepetitions, GPURepetitions);
}



/* FModelUnitTester static private functions
 *****************************************************************************/

bool FModelUnitTester::ModelLoadAccuracyAndSpeedTests(const FString& InProjectContentDir, const FString& InModelZooRelativeDirectory, const TArray<FString>& InModelNames,
	const TArray<float>& InInputArrayValues, const TArray<TArray<double>>& InCPUGroundTruths, const TArray<TArray<double>>& InGPUGroundTruths, const TArray<int32>& InCPURepetitions,
	const TArray<int32>& InGPURepetitions)
{
	bool bDidGlobalTestPassed = true;

	const FString ModelZooDirectory = InProjectContentDir / InModelZooRelativeDirectory;

	// Test OTXT/UAsset accuracy
	for (int32 ModelIndex = 0; ModelIndex < InModelNames.Num(); ++ModelIndex)
	{
		const FString& ModelName = InModelNames[ModelIndex];
		const TArray<double>& CPUGroundTruths = InCPUGroundTruths[ModelIndex];
		const TArray<double>& GPUGroundTruths = InGPUGroundTruths[ModelIndex];

		UE_LOG(LogNeuralNetworkInferenceQA, Display, TEXT("--------------- %s - Network Uasset Load and Run"), *ModelName);
		const FString UAssetModelFilePath = GetUAssetModelFilePath(ModelName, InModelZooRelativeDirectory);
		UNeuralNetwork* Network = NetworkUassetLoadTest(UAssetModelFilePath);
		if (!Network)
		{
			ensureMsgf(false, TEXT("UNeuralNetwork could not be loaded from UAssetModelFilePath %s."), *UAssetModelFilePath);
			return false;
		}
		// Input debugging
		UE_LOG(LogNeuralNetworkInferenceQA, Display, TEXT("--------------- %s - Input/Output"), *ModelName);
		for (int32 TensorIndex = 0; TensorIndex < Network->GetInputTensorNumber(); ++TensorIndex)
		{
			UE_LOG(LogNeuralNetworkInferenceQA, Display, TEXT("InputTensor[%d] = %s."), TensorIndex, *Network->GetInputTensor(TensorIndex).GetName());
		}
		// Output debugging
		for (int32 TensorIndex = 0; TensorIndex < Network->GetOutputTensorNumber(); ++TensorIndex)
		{
			UE_LOG(LogNeuralNetworkInferenceQA, Display, TEXT("OutputTensor[%d] = %s."), TensorIndex, *Network->GetOutputTensor(TensorIndex).GetName());
		}
		bDidGlobalTestPassed &= ModelAccuracyTest(Network, InInputArrayValues, CPUGroundTruths, GPUGroundTruths);

		UE_LOG(LogNeuralNetworkInferenceQA, Display, TEXT("---------------------------------------------------------------------------------------------------------------------------------"));

		UE_LOG(LogNeuralNetworkInferenceQA, Display, TEXT("--------------- %s - Network ONNX/ORT Load and Run"), *ModelName);
#if WITH_EDITOR
		for (int32 ONNXOrORTIndex = 0; ONNXOrORTIndex < 2; ++ONNXOrORTIndex)
		{
			const FString ModelFilePath = (ONNXOrORTIndex % 2 == 0 ? GetONNXModelFilePath(ModelZooDirectory, ModelName) : GetORTModelFilePath(ModelZooDirectory, ModelName));
			const FString ModelType = (ONNXOrORTIndex % 2 == 0 ? TEXT("ONNX") : TEXT("ORT"));
if (ONNXOrORTIndex == 1)
{
	break;
}
			UE_LOG(LogNeuralNetworkInferenceQA, Display, TEXT("-------------------- %s - Network %s Load and Run - %s"), *ModelName, *ModelType, *ModelFilePath);
			bDidGlobalTestPassed &= ModelAccuracyTest(NetworkONNXOrORTLoadTest(ModelFilePath), InInputArrayValues, CPUGroundTruths, GPUGroundTruths);
		}
#else //WITH_EDITOR
	UE_LOG(LogNeuralNetworkInferenceQA, Display, TEXT("-------------------- Skipped (only in WITH_EDITOR enabled)."));
#endif //WITH_EDITOR
	}

	// Profile speed
	for (int32 ModelIndex = 0; ModelIndex < InModelNames.Num(); ++ModelIndex)
	{
		const FString& ModelName = InModelNames[ModelIndex];
		UE_LOG(LogNeuralNetworkInferenceQA, Display, TEXT("--------------- %s - Network UAsset Speed Profiling"), *ModelName);
		const FString UAssetModelFilePath = GetUAssetModelFilePath(ModelName, InModelZooRelativeDirectory);
		bDidGlobalTestPassed &= ModelSpeedTest(UAssetModelFilePath, InCPURepetitions[ModelIndex], InGPURepetitions[ModelIndex]);
	}

	return true;
}

FString FModelUnitTester::GetONNXModelFilePath(const FString& ModelZooDirectory, const FString& InModelName)
{
	return /*FPaths::ConvertRelativePathToFull*/(ModelZooDirectory / InModelName + TEXT("/") + InModelName + TEXT(".onnx")); // E.g., ModelZooDirectory / TEXT("ExampleNetworkReadable/ExampleNetworkReadable.onnx")
}

FString FModelUnitTester::GetORTModelFilePath(const FString& ModelZooDirectory, const FString& InModelName)
{
	return /*FPaths::ConvertRelativePathToFull*/(ModelZooDirectory / InModelName + TEXT("/") + InModelName + TEXT(".ort")); // E.g., ModelZooDirectory / TEXT("ExampleNetworkReadable/ExampleNetworkReadable.ort")
}

FString FModelUnitTester::GetUAssetModelFilePath(const FString& InModelName, const FString& InModelZooRelativeDirectory)
{
	return InModelName + TEXT("'/Game/") + InModelZooRelativeDirectory / InModelName + TEXT("/") + InModelName + TEXT(".") + InModelName + TEXT("'"); // E.g., '/Game/[MODEL_ZOO_DIR]/ExampleNetworkReadable/ExampleNetworkReadable.ExampleNetworkReadable'
}

UNeuralNetwork* FModelUnitTester::NetworkUassetLoadTest(const FString& InUAssetPath)
{
	UNeuralNetwork* Network = LoadObject<UNeuralNetwork>((UObject*)GetTransientPackage(), *InUAssetPath);
	if (!Network)
	{
		ensureMsgf(false, TEXT("UNeuralNetwork is a nullptr. Path: \"%s\"."), *InUAssetPath);
		return nullptr;
	}
	if (!Network->IsLoaded())
	{
		ensureMsgf(false, TEXT("UNeuralNetwork could not be loaded from uasset disk location. Path: \"%s\"."), *InUAssetPath);
		return nullptr;
	}
	return Network;
}

UNeuralNetwork* FModelUnitTester::NetworkONNXOrORTLoadTest(const FString& InModelFilePath)
{
	// Load network architecture and weights from file
	UNeuralNetwork* Network = NewObject<UNeuralNetwork>((UObject*)GetTransientPackage(), UNeuralNetwork::StaticClass());
	if (!Network)
	{
		ensureMsgf(false, TEXT("UNeuralNetwork is a nullptr. Path: \"%s\"."), *InModelFilePath);
		return nullptr;
	}
	if (!Network->Load(InModelFilePath))
	{
		ensureMsgf(false, TEXT("UNeuralNetwork could not be loaded from ONNX file disk location. Path: \"%s\"."), *InModelFilePath);
		return nullptr;
	}
	return Network;
}

bool FModelUnitTester::ModelAccuracyTest(UNeuralNetwork* InOutNetwork, const TArray<float>& InInputArrayValues, const TArray<double>& InCPUGroundTruths, const TArray<double>& InGPUGroundTruths)
{
	// Sanity check
	if (!InOutNetwork)
	{
		return false;
	}
	// Find NetworkSize
	const int64 NetworkSize = InOutNetwork->GetInputTensor().Num();
	// Initialize input data
	TArray<TArray<float>> InputArrays;
	{
		InputArrays.Reserve(InInputArrayValues.Num());
		for (const float InputArrayValue : InInputArrayValues)
		{
			InputArrays.Emplace(TArray<float>({}));
			InputArrays.Last().Init(InputArrayValue, NetworkSize);
		}
		ensureMsgf(InputArrays.Num() <= InCPUGroundTruths.Num() && InputArrays.Num() <= InGPUGroundTruths.Num(),
			TEXT("InputArrays.Num() <= InCPUGroundTruths.Num() && InputArrays.Num() <= InGPUGroundTruths.Num() failed: %d vs. %d vs. %d."),
			InputArrays.Num(), InCPUGroundTruths.Num(), InGPUGroundTruths.Num());
	}
	// Run each input with CPU/GPU and compare with each other and with the ground truth
	bool bDidGlobalTestPassed = true;
	for (int32 Index = 0; Index < InputArrays.Num(); ++Index)
	{
		TArray<float>& InputArray = InputArrays[Index];
		const double CPUGroundTruth = InCPUGroundTruths[Index];
		const double GPUGroundTruth = InGPUGroundTruths[Index];
		InOutNetwork->SetInputFromArrayCopy(InputArray);
		// CPU
		InOutNetwork->SetDeviceType(ENeuralDeviceType::CPU);
		InOutNetwork->Run();
		TArray<float> OutputArrayCPU = InOutNetwork->GetOutputTensor().GetArrayCopy<float>();
		// GPU
		InOutNetwork->SetDeviceType(ENeuralDeviceType::GPU);
		InOutNetwork->Run();
		TArray<float> OutputArrayGPU = InOutNetwork->GetOutputTensor().GetArrayCopy<float>();
		// Final verbose
		const double CPUAvgL1Norm = GetAveragedL1Norm(OutputArrayCPU);
		const double GPUAvgL1Norm = GetAveragedL1Norm(OutputArrayGPU);
		const double RelativeCoefficient = 1. / FMath::Max(1., FMath::Min(CPUAvgL1Norm, GPUAvgL1Norm)); // Max(1, X) to avoid 0s
		const double CPUGPUAvgL1NormDiff = GetAveragedL1NormDiff(OutputArrayCPU, OutputArrayGPU) * RelativeCoefficient * 1e3;
		const double FastCPUGPUAvgL1NormDiff = FMath::Abs((CPUAvgL1Norm - GPUAvgL1Norm)) * RelativeCoefficient * 1e6;
		const double FastCPUAvgL1NormDiff = FMath::Abs(CPUAvgL1Norm - CPUGroundTruth) / FMath::Max(1., FMath::Min(CPUAvgL1Norm, CPUGroundTruth)) * 1e7;
		const double FastGPUAvgL1NormDiff = FMath::Abs(GPUAvgL1Norm - GPUGroundTruth) / FMath::Max(1., FMath::Min(GPUAvgL1Norm, GPUGroundTruth)) * 1e7;
		UE_LOG(LogNeuralNetworkInferenceQA, Display, TEXT("InputNorm = %f, OutputNormCPU = %f, OutputNormGPU = %f, OutputNormGT = %f, CPUAvgL1Norm = %f, GPUAvgL1Norm = %f,"
			"\tCPUGPUAvgL1NormDiff = %fe-3, FastCPUGPUAvgL1NormDiff = %fe-6, FastCPUAvgL1NormDiff = %fe-7, FastGPUAvgL1NormDiff = %fe-7 (1e-7 is roughly the precision for float)."),
			GetAveragedL1Norm(InputArray), CPUAvgL1Norm, GPUAvgL1Norm, CPUGroundTruth, CPUAvgL1Norm, GPUAvgL1Norm, CPUGPUAvgL1NormDiff, FastCPUGPUAvgL1NormDiff, FastCPUAvgL1NormDiff, FastGPUAvgL1NormDiff);
		const TArray<int64>& InputSizes = InOutNetwork->GetInputTensor().GetSizes();
		const TArray<int64>& OutputSizes = InOutNetwork->GetOutputTensor().GetSizes();
		const int64 MaxNumberElementsToDisplay = 100;
		// Check if test failed and (if so) display information
		bool bDidSomeTestFailed = (!FMath::IsFinite(FastCPUGPUAvgL1NormDiff) || FastCPUGPUAvgL1NormDiff > 5);
		bDidSomeTestFailed |= (!FMath::IsFinite(CPUGPUAvgL1NormDiff) || CPUGPUAvgL1NormDiff > 1);
		bDidSomeTestFailed |= (!FMath::IsFinite(FastCPUAvgL1NormDiff) || FastCPUAvgL1NormDiff > 30);
		bDidSomeTestFailed |= (!FMath::IsFinite(FastGPUAvgL1NormDiff) || FastGPUAvgL1NormDiff > 30);
		if (bDidSomeTestFailed)
		{
			UE_LOG(LogNeuralNetworkInferenceQA, Display, TEXT("FastCPUGPUAvgL1NormDiff (%fe-6) < 5e-6 might have failed."), FastCPUGPUAvgL1NormDiff);
			UE_LOG(LogNeuralNetworkInferenceQA, Display, TEXT("CPUGPUAvgL1NormDiff (%fe-7) < 1e-7 might have failed."), CPUGPUAvgL1NormDiff);
			UE_LOG(LogNeuralNetworkInferenceQA, Display, TEXT("FastCPUAvgL1NormDiff (%fe-7) < 30e-7 might have failed (~30 times the float precision).\nOutputArrayCPU = %s."), FastCPUAvgL1NormDiff, *FNeuralTensor(OutputArrayCPU, OutputSizes).ToString(MaxNumberElementsToDisplay));
			UE_LOG(LogNeuralNetworkInferenceQA, Display, TEXT("FastGPUAvgL1NormDiff (%fe-7) < 30e-7 might have failed (~30 times the float precision).\nOutputArrayGPU = %s."), FastGPUAvgL1NormDiff, *FNeuralTensor(OutputArrayGPU, OutputSizes).ToString(MaxNumberElementsToDisplay));
			UE_LOG(LogNeuralNetworkInferenceQA, Display, TEXT("Input = %s"),
				*FNeuralTensor(InOutNetwork->GetInputTensor().GetArrayCopy<float>(), InputSizes).ToString(MaxNumberElementsToDisplay));
			UE_LOG(LogNeuralNetworkInferenceQA, Display, TEXT("OutputArrayCPU = %s"), *FNeuralTensor(OutputArrayCPU, OutputSizes).ToString(MaxNumberElementsToDisplay));
			UE_LOG(LogNeuralNetworkInferenceQA, Display, TEXT("OutputArrayGPU = %s"), *FNeuralTensor(OutputArrayGPU, OutputSizes).ToString(MaxNumberElementsToDisplay));
			ensureMsgf(false, TEXT("At least 1 of the 4 CPU/GPU tests failed."));
			return false;
		}
		bDidGlobalTestPassed &= !bDidSomeTestFailed;
	}
	// Test successful
	return bDidGlobalTestPassed;
}

#define MODEL_UNIT_TESTER_SPEED_TEST(OutCopyTimeInMilliSeconds, OutNetworkTimeInMilliSeconds, InRepetitions, bInIsGPU) \
	/* Input/output copy speed */ \
	Timer.Tic(); \
	for (int32 TimerIndex = 0; TimerIndex < InRepetitions; ++TimerIndex) \
	{ \
		InOutNetwork->SetInputFromArrayCopy(InputArray); \
		OutputArrayGPU = InOutNetwork->GetOutputTensor().GetArrayCopy<float>(); \
	} \
	const float OutCopyTimeInMilliSeconds = Timer.Toc() / InRepetitions; \
	/* Forward() speed */ \
	if (InRepetitions > 1) \
	{ \
		for (int32 TimerIndex = 0; TimerIndex < 5; ++TimerIndex) \
		{ \
			InOutNetwork->Run(); \
		} \
	} \
	Timer.Tic(); \
	if (InRepetitions > 0) \
	{ \
		for (int32 TimerIndex = 0; TimerIndex < InRepetitions; ++TimerIndex) \
		{ \
			InOutNetwork->SetInputFromArrayCopy(InputArray); \
			InOutNetwork->Run(); \
			OutputArrayGPU = InOutNetwork->GetOutputTensor().GetArrayCopy<float>(); \
		} \
	} \
	const float OutNetworkTimeInMilliSeconds = Timer.Toc() / InRepetitions - OutCopyTimeInMilliSeconds

bool FModelUnitTester::ModelSpeedTest(const FString& InUAssetPath, const int32 InCPURepetitions, const int32 InGPURepetitions)
{
	// Load Network
	UNeuralNetwork* InOutNetwork = NetworkUassetLoadTest(InUAssetPath);
	// Sanity check
	if (!InOutNetwork)
	{
		ensureMsgf(false, TEXT("InOutNetwork was a nullptr."));
		return false;
	}
	// Needed variables
	const int64 NetworkSize = InOutNetwork->GetInputTensor().Num();
	TArray<float> InputArray;
	InputArray.Init(1.f, NetworkSize);
	TArray<float> OutputArrayGPU;
	// Speed profiling
	FNeuralNetworkInferenceQATimer Timer;
	// CPU
	InOutNetwork->SetDeviceType(ENeuralDeviceType::CPU);
	MODEL_UNIT_TESTER_SPEED_TEST(CPUCopyTimer1, CPUNetworkTimer1, 1, /*bIsGPU*/false);
	InOutNetwork->SetDeviceType(ENeuralDeviceType::CPU);
	MODEL_UNIT_TESTER_SPEED_TEST(CPUCopyTimer, CPUNetworkTimer, InCPURepetitions, /*bIsGPU*/false);
	// GPU
	InOutNetwork->SetDeviceType(ENeuralDeviceType::GPU);
	MODEL_UNIT_TESTER_SPEED_TEST(GPUCopyTimer1, GPUNetworkTimer1, 1, /*bIsGPU*/true);
	InOutNetwork->SetDeviceType(ENeuralDeviceType::GPU);
	MODEL_UNIT_TESTER_SPEED_TEST(GPUCopyTimer, GPUNetworkTimer, InGPURepetitions, /*bIsGPU*/true);
	// Display speed times
	UE_LOG(LogNeuralNetworkInferenceQA, Display,
		TEXT("Forward pass speed profiling (TensorCopy and Forward()): CPUx1 = %f+%f msec, CPUx%d = %f+%f msec, GPUx1 = %f+%f msec, GPUx%d = %f+%f msec."),
		CPUCopyTimer1, CPUNetworkTimer1, InCPURepetitions, CPUCopyTimer, CPUNetworkTimer,
		GPUCopyTimer1, GPUNetworkTimer1, InGPURepetitions, GPUCopyTimer, GPUNetworkTimer);
	// Test successful
	return true;
}

double FModelUnitTester::GetAveragedL1Norm(const TArray<float>& InArray)
{
	double AveragedL1Norm = 0.;
	for (const float Value : InArray)
	{
		AveragedL1Norm += FMath::Abs(Value);
	}
	AveragedL1Norm /= InArray.Num();
	return AveragedL1Norm;
}

double FModelUnitTester::GetAveragedL1NormDiff(const TArray<float>& InArray1, const TArray<float>& InArray2)
{
	// Sanity check
	if (InArray1.Num() != InArray2.Num())
	{
		ensureMsgf(false, TEXT("InArray1.Num() == InArray2.Num() failed: %d != %d."), InArray1.Num(), InArray2.Num());
		return -1.;
	}
	// Averaged L1 norm
	double AveragedL1NormDiff = 0.;
	for (int32 Index = 0; Index < InArray1.Num(); ++Index)
	{
		AveragedL1NormDiff += FMath::Abs(InArray1[Index] - InArray2[Index]);
	}
	AveragedL1NormDiff /= InArray1.Num();
	return AveragedL1NormDiff;
}
