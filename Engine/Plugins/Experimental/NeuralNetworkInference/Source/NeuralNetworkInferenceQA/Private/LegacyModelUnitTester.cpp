// Copyright Epic Games, Inc. All Rights Reserved.

#include "LegacyModelUnitTester.h"
#include "ModelProtoFileReaderTester.h"
#include "NeuralNetworkInferenceTimer.h"
#include "NeuralNetworkInferenceQAUtils.h"
#include "Misc/Paths.h"



/* FLegacyModelUnitTester static public functions
 *****************************************************************************/

bool FLegacyModelUnitTester::GlobalTest(const FString& InProjectContentDir, const FString& InModelZooRelativeDirectory)
{
	// Model load, accuracy, and speed test
	const TArray<FString> ModelNames({ TEXT("MLRigDeformer")/*, TEXT("cloth_network")*/ });
	const TArray<float> InputArrayValues({ 1.f, 0.f, -1.f, 100.f/*, -100.f, 0.5f, -0.5f*/ }); // This one can be shorter than CPU/GPUGroundTruths
	const TArray<TArray<double>> CPUGroundTruths({ {3.728547, 0.008774, 4.595651, 212.193216, 742.434561, 4.250668, 4.717748}, {0.042571, 0.023693, 0.015783, 13.100505, 8.050994, 0.028807, 0.016387} });
	const TArray<TArray<double>> GPUGroundTruths({ {3.728547, 0.008774, 4.595651, 212.193208, 742.434578, 4.250668, 4.717748}, {0.042571, 0.023693, 0.015783, 13.100504, 8.050994, 0.028807, 0.016387} });
	const TArray<int32> CPURepetitions({ 10, 0 }); // Used to be 100
	const TArray<int32> GPURepetitions({ 10, 10 }); // Used to be 100
	return ModelLoadAccuracyAndSpeedTests(InProjectContentDir, InModelZooRelativeDirectory, ModelNames, InputArrayValues, CPUGroundTruths, GPUGroundTruths, CPURepetitions, GPURepetitions);
}



/* FLegacyModelUnitTester static private functions
 *****************************************************************************/

bool FLegacyModelUnitTester::ModelLoadAccuracyAndSpeedTests(const FString& InProjectContentDir, const FString& InModelZooRelativeDirectory, const TArray<FString>& InModelNames,
	const TArray<float>& InInputArrayValues, const TArray<TArray<double>>& InCPUGroundTruths, const TArray<TArray<double>>& InGPUGroundTruths, const TArray<int32>& InCPURepetitions,
	const TArray<int32>& InGPURepetitions)
{
	bool bDidGlobalTestPassed = true;

	const FString ModelZooDirectory = InProjectContentDir / InModelZooRelativeDirectory;

	// Test ONNX Reader
	for (const FString& ModelName : InModelNames)
	{
		UE_LOG(LogNeuralNetworkInferenceQA, Display, TEXT("--------------- %s - OTXT IO"), *ModelName);
		const FString OtxtModelFilePath = GetOtxtModelFilePath(ModelZooDirectory, ModelName);
		bDidGlobalTestPassed &= FModelProtoFileReaderTester::Test(OtxtModelFilePath);
		UE_LOG(LogNeuralNetworkInferenceQA, Display, TEXT("--------------- %s - ONNX IO"), *ModelName);
		const FString ONNXModelFilePath = GetONNXModelFilePath(ModelZooDirectory, ModelName);
		bDidGlobalTestPassed &= FModelProtoFileReaderTester::Test(ONNXModelFilePath);
	}
	UE_LOG(LogNeuralNetworkInferenceQA, Display, TEXT("---------------------------------------------------------------------------------------------------------------------------------"));

	// Test ONNX/UAsset accuracy
	for (int32 ModelIndex = 0; ModelIndex < InModelNames.Num(); ++ModelIndex)
	{
		const FString& ModelName = InModelNames[ModelIndex];
		const TArray<double>& CPUGroundTruths = InCPUGroundTruths[ModelIndex];
		const TArray<double>& GPUGroundTruths = InGPUGroundTruths[ModelIndex];

		const FString ONNXModelFilePath = GetONNXModelFilePath(ModelZooDirectory, ModelName);
		UE_LOG(LogNeuralNetworkInferenceQA, Display, TEXT("--------------- %s - Legacy Network ONNX Load and Run - %s"), *ModelName, *ONNXModelFilePath);
		bDidGlobalTestPassed &= ModelAccuracyTest(NetworkONNXLoadTest(ONNXModelFilePath), InInputArrayValues, CPUGroundTruths, GPUGroundTruths);

		UE_LOG(LogNeuralNetworkInferenceQA, Display, TEXT("--------------- %s - Legacy Network Uasset Load and Run"), *ModelName);
		const FString UAssetModelFilePath = GetUAssetModelFilePath(ModelName, InModelZooRelativeDirectory);
		bDidGlobalTestPassed &= ModelAccuracyTest(NetworkUassetLoadTest(UAssetModelFilePath), InInputArrayValues, CPUGroundTruths, GPUGroundTruths);

		UE_LOG(LogNeuralNetworkInferenceQA, Display, TEXT("---------------------------------------------------------------------------------------------------------------------------------"));
	}

	// Profile speed
	for (int32 ModelIndex = 0; ModelIndex < InModelNames.Num(); ++ModelIndex)
	{
		const FString& ModelName = InModelNames[ModelIndex];
		UE_LOG(LogNeuralNetworkInferenceQA, Display, TEXT("--------------- %s - Legacy Network UAsset Speed Profiling"), *ModelName);
		const FString UAssetModelFilePath = GetUAssetModelFilePath(ModelName, InModelZooRelativeDirectory);
		bDidGlobalTestPassed &= ModelSpeedTest(UAssetModelFilePath, InCPURepetitions[ModelIndex], InGPURepetitions[ModelIndex]);
	}

	return true;
}

FString FLegacyModelUnitTester::GetONNXModelFilePath(const FString& ModelZooDirectory, const FString& InModelName)
{
	return FPaths::ConvertRelativePathToFull(ModelZooDirectory + InModelName + TEXT("/") + InModelName + TEXT(".onnx")); // E.g., ModelZooDirectory / TEXT("ExampleNetworkReadable/ExampleNetworkReadable.onnx")
}

FString FLegacyModelUnitTester::GetOtxtModelFilePath(const FString& ModelZooDirectory, const FString& InModelName)
{
	return FPaths::ConvertRelativePathToFull(ModelZooDirectory + InModelName + TEXT("/") + InModelName + TEXT(".otxt")); // E.g., ModelZooDirectory / TEXT("ExampleNetworkReadable/ExampleNetworkReadable.otxt")
}

FString FLegacyModelUnitTester::GetUAssetModelFilePath(const FString& InModelName, const FString& InModelZooRelativeDirectory)
{
	return InModelName + TEXT("'/Game/") + InModelZooRelativeDirectory / InModelName + TEXT("/") + InModelName + TEXT("Legacy.") + InModelName + TEXT("Legacy'"); // E.g., '/Game/[MODEL_ZOO_DIR]/ExampleNetworkReadable/ExampleNetworkReadable.ExampleNetworkReadable'
}

UNeuralNetworkLegacy* FLegacyModelUnitTester::NetworkUassetLoadTest(const FString& InUAssetPath)
{
	UNeuralNetworkLegacy* Network = LoadObject<UNeuralNetworkLegacy>((UObject*)GetTransientPackage(), *InUAssetPath);
	if (!Network)
	{
		ensureMsgf(false, TEXT("UNeuralNetworkLegacy is a nullptr. Path: \"%s\"."), *InUAssetPath);
		return nullptr;
	}
	if (!Network->IsLoaded())
	{
		ensureMsgf(false, TEXT("UNeuralNetworkLegacy could not be loaded from uasset disk location. Path: \"%s\"."), *InUAssetPath);
		return nullptr;
	}
	UE_LOG(LogNeuralNetworkInferenceQA, Display, TEXT("--------------- %d layers loaded"), Network->GetTensors().Num());
	return Network;
}

UNeuralNetworkLegacy* FLegacyModelUnitTester::NetworkONNXLoadTest(const FString& InONNXFilePath)
{
#if WITH_EDITOR
	// Load network architecture and weights from file
	UNeuralNetworkLegacy* Network = NewObject<UNeuralNetworkLegacy>((UObject*)GetTransientPackage(), UNeuralNetworkLegacy::StaticClass());
	if (!Network)
	{
		ensureMsgf(false, TEXT("UNeuralNetworkLegacy is a nullptr. Path: \"%s\"."), *InONNXFilePath);
		return nullptr;
	}
	if (!Network->Load(InONNXFilePath))
	{
		ensureMsgf(false, TEXT("UNeuralNetworkLegacy could not be loaded from ONNX file disk location. Path: \"%s\"."), *InONNXFilePath);
		return nullptr;
	}
	return Network;
#else //WITH_EDITOR
	UE_LOG(LogNeuralNetworkInferenceQA, Display, TEXT("--------------- NetworkONNXLoadTest test skipped (only in Editor)."));
	return nullptr;
#endif //WITH_EDITOR
}

bool FLegacyModelUnitTester::ModelAccuracyTest(UNeuralNetworkLegacy* InOutNetwork, const TArray<float>& InInputArrayValues, const TArray<double>& InCPUGroundTruths, const TArray<double>& InGPUGroundTruths)
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
	for (int32 Index = 0; Index < InputArrays.Num(); ++Index)
	{
		TArray<float>& InputArray = InputArrays[Index];
		const double CPUGroundTruth = InCPUGroundTruths[Index];
		const double GPUGroundTruth = InGPUGroundTruths[Index];
		InOutNetwork->SetInputFromArrayCopy(InputArray);
		// CPU
		InOutNetwork->SetDeviceType(ENeuralDeviceType::CPU);
		InOutNetwork->Run();
		const TArray<float> OutputArrayCPU = InOutNetwork->GetOutputTensor().GetArrayCopy<float>();
		// GPU
		InOutNetwork->SetDeviceType(ENeuralDeviceType::GPU);
		InOutNetwork->Run();
		const TArray<float> OutputArrayGPU = InOutNetwork->GetOutputTensor().GetArrayCopy<float>();
		// Final verbose
		const double CPUAvgL1Norm = GetAveragedL1Norm(OutputArrayCPU);
		const double GPUAvgL1Norm = GetAveragedL1Norm(OutputArrayGPU);
		const double RelativeCoefficient = 1. / FMath::Max(1., FMath::Min(CPUAvgL1Norm, GPUAvgL1Norm)); // Max(1, X) to avoid 0s
		const double CPUGPUAvgL1NormDiff = GetAveragedL1NormDiff(OutputArrayCPU, OutputArrayGPU) * RelativeCoefficient * 1e3;
		const double FastCPUGPUAvgL1NormDiff = FMath::Abs((CPUAvgL1Norm - GPUAvgL1Norm)) * RelativeCoefficient * 1e7;
		const double FastCPUAvgL1NormDiff = FMath::Abs(CPUAvgL1Norm - CPUGroundTruth) / FMath::Max(1., FMath::Min(CPUAvgL1Norm, CPUGroundTruth)) * 1e7;
		const double FastGPUAvgL1NormDiff = FMath::Abs(GPUAvgL1Norm - GPUGroundTruth) / FMath::Max(1., FMath::Min(GPUAvgL1Norm, GPUGroundTruth)) * 1e7;
		UE_LOG(LogNeuralNetworkInferenceQA, Display, TEXT("InputNorm = %f, CPUAvgL1Norm = %f, GPUAvgL1Norm = %f,"
			"\tCPUGPUAvgL1NormDiff = %fe-3, FastCPUGPUAvgL1NormDiff = %fe-7, FastCPUAvgL1NormDiff = %fe-7, FastGPUAvgL1NormDiff = %fe-7 (1e-7 is roughly the precision for float)."),
			GetAveragedL1Norm(InputArray), CPUAvgL1Norm, GPUAvgL1Norm, CPUGPUAvgL1NormDiff, FastCPUGPUAvgL1NormDiff, FastCPUAvgL1NormDiff, FastGPUAvgL1NormDiff);
		const TArray<int64>& OutputSizes = InOutNetwork->GetOutputTensor().GetSizes();
		const int64 MaxNumberElementsToDisplay = 100;
		if (!FMath::IsFinite(FastCPUGPUAvgL1NormDiff) || FastCPUGPUAvgL1NormDiff > 5)
		{
			UE_LOG(LogNeuralNetworkInferenceQA, Display, TEXT("FastCPUGPUAvgL1NormDiff (%fe-7) < 5e-7 failed (~5 times the float precision)."), FastCPUGPUAvgL1NormDiff);
			UE_LOG(LogNeuralNetworkInferenceQA, Display, TEXT("Input = %s"), *InOutNetwork->GetInputTensor().ToString(MaxNumberElementsToDisplay));
			UE_LOG(LogNeuralNetworkInferenceQA, Display, TEXT("OutputArrayCPU = %s"), *FNeuralTensor(OutputArrayCPU, OutputSizes).ToString(MaxNumberElementsToDisplay));
			UE_LOG(LogNeuralNetworkInferenceQA, Display, TEXT("OutputArrayGPU = %s"), *FNeuralTensor(OutputArrayGPU, OutputSizes).ToString(MaxNumberElementsToDisplay));
			ensureMsgf(false, TEXT("FastCPUGPUAvgL1NormDiff < 5e-7 failed."));
		}
		if (!FMath::IsFinite(CPUGPUAvgL1NormDiff) || CPUGPUAvgL1NormDiff > 1)
		{
			UE_LOG(LogNeuralNetworkInferenceQA, Display, TEXT("CPUGPUAvgL1NormDiff (%fe-7) < 1e-7 failed."), CPUGPUAvgL1NormDiff);
			UE_LOG(LogNeuralNetworkInferenceQA, Display, TEXT("Input = %s"), *InOutNetwork->GetInputTensor().ToString(MaxNumberElementsToDisplay));
			UE_LOG(LogNeuralNetworkInferenceQA, Display, TEXT("OutputArrayCPU = %s"), *FNeuralTensor(OutputArrayCPU, OutputSizes).ToString(MaxNumberElementsToDisplay));
			UE_LOG(LogNeuralNetworkInferenceQA, Display, TEXT("OutputArrayGPU = %s"), *FNeuralTensor(OutputArrayGPU, OutputSizes).ToString(MaxNumberElementsToDisplay));
			ensureMsgf(false, TEXT("CPUGPUAvgL1NormDiff < 1e-7 failed."));
		}
		ensureMsgf(FMath::IsFinite(FastCPUAvgL1NormDiff) && FastCPUAvgL1NormDiff < 5, TEXT("FastCPUAvgL1NormDiff (%fe-7) < 5e-7 failed (~5 times the float precision).\nOutputArrayCPU = %s"),
			FastCPUAvgL1NormDiff, *FNeuralTensor(OutputArrayCPU, OutputSizes).ToString(MaxNumberElementsToDisplay));
		ensureMsgf(FMath::IsFinite(FastGPUAvgL1NormDiff) && FastGPUAvgL1NormDiff < 5, TEXT("FastGPUAvgL1NormDiff (%fe-7) < 5e-7 failed (~5 times the float precision).\nOutputArrayGPU = %s"),
			FastGPUAvgL1NormDiff, *FNeuralTensor(OutputArrayGPU, OutputSizes).ToString(MaxNumberElementsToDisplay));
	}
	// Test successful
	return true;
}

#define LEGACY_MODEL_UNIT_TESTER_SPEED_TEST(OutCopyTimeInMilliSeconds, OutNetworkTimeInMilliSeconds, OutEmptyNetworkTimeInMilliSeconds, InRepetitions, bInIsGPU) \
	/* Input/output copy speed */ \
	Timer.Tic(); \
	for (int32 TimerIndex = 0; TimerIndex < InRepetitions; ++TimerIndex) \
	{ \
		InOutNetwork->SetInputFromArrayCopy(InputArray); \
		OutputArrayGPU = InOutNetwork->GetOutputTensor().GetArrayCopy<float>(); \
	} \
	const float OutCopyTimeInMilliSeconds = Timer.Toc() / InRepetitions; \
	/* Empty Forward() speed */ \
	if (bInIsGPU) \
	{ \
		InOutNetwork->Run(); \
		Timer.Tic(); \
		for (int32 TimerIndex = 0; TimerIndex < InRepetitions; ++TimerIndex) \
		{ \
			InOutNetwork->SetInputFromArrayCopy(InputArray); \
			InOutNetwork->Run(/*GPUSynchronousMode*/ENeuralNetworkSynchronousMode::Synchronous, /*InInputDeviceType*/ENeuralDeviceType::CPU, /*InOutputDeviceType*/ENeuralDeviceType::CPU, /*bRunGPUEmptyOnlyForProfiling*/true); \
			OutputArrayGPU = InOutNetwork->GetOutputTensor().GetArrayCopy<float>(); \
		} \
	} \
	const float OutEmptyNetworkTimeInMilliSeconds = Timer.Toc() / InRepetitions - OutCopyTimeInMilliSeconds; \
	/* Forward() speed */ \
	if (InRepetitions > 1) \
	{ \
		InOutNetwork->Run(); \
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
	const float OutNetworkTimeInMilliSeconds = Timer.Toc() / InRepetitions - OutCopyTimeInMilliSeconds - (bInIsGPU ? OutEmptyNetworkTimeInMilliSeconds : 0)

bool FLegacyModelUnitTester::ModelSpeedTest(const FString& InUAssetPath, const int32 InCPURepetitions, const int32 InGPURepetitions)
{
	// Load Network
	UNeuralNetworkLegacy* InOutNetwork = NetworkUassetLoadTest(InUAssetPath);
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
	FNeuralNetworkInferenceTimer Timer;
	// CPU
	InOutNetwork->SetDeviceType(ENeuralDeviceType::CPU);
	LEGACY_MODEL_UNIT_TESTER_SPEED_TEST(CPUCopyTimer1, CPUNetworkTimer1, DummyCPU1, 1, /*bIsGPU*/false);
	InOutNetwork->SetDeviceType(ENeuralDeviceType::CPU);
	LEGACY_MODEL_UNIT_TESTER_SPEED_TEST(CPUCopyTimer, CPUNetworkTimer, DummyCPU, InCPURepetitions, /*bIsGPU*/false);
	// GPU
	InOutNetwork->SetDeviceType(ENeuralDeviceType::GPU);
	LEGACY_MODEL_UNIT_TESTER_SPEED_TEST(GPUCopyTimer1, GPUNetworkTimer1, GPUEmptyNetworkTimer1, 1, /*bIsGPU*/true);
	InOutNetwork->SetDeviceType(ENeuralDeviceType::GPU);
	LEGACY_MODEL_UNIT_TESTER_SPEED_TEST(GPUCopyTimer, GPUNetworkTimer, GPUEmptyNetworkTimer, InGPURepetitions, /*bIsGPU*/true);
	// Display speed times
	UE_LOG(LogNeuralNetworkInferenceQA, Display,
		TEXT("Forward pass speed profiling (TensorCopy, optionally EmptyForward(), and Forward()): CPUx1 = %f+%f msec, CPUx%d = %f+%f msec, GPUx1 = %f+%f+%f msec, GPUx%d = %f+%f+%f msec."),
		CPUCopyTimer1,                        CPUNetworkTimer1, InCPURepetitions, CPUCopyTimer,                       CPUNetworkTimer,
		GPUCopyTimer1, GPUEmptyNetworkTimer1, GPUNetworkTimer1, InGPURepetitions, GPUCopyTimer, GPUEmptyNetworkTimer, GPUNetworkTimer);
	// Test successful
	return true;
}

double FLegacyModelUnitTester::GetAveragedL1Norm(const TArray<float>& InArray)
{
	double AveragedL1Norm = 0.;
	for (const float Value : InArray)
	{
		AveragedL1Norm += FMath::Abs(Value);
	}
	AveragedL1Norm /= InArray.Num();
	return AveragedL1Norm;
}

double FLegacyModelUnitTester::GetAveragedL1NormDiff(const TArray<float>& InArray1, const TArray<float>& InArray2)
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
