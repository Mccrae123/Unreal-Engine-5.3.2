// Copyright Epic Games, Inc. All Rights Reserved.

#include "LegacyONNXTester.h"
#include "NeuralNetworkInferenceQAUtils.h"

#if WITH_EDITOR
#include "ModelProtoFileReader.h"
#endif //WITH_EDITOR



/* FLegacyONNXTester public functions
 *****************************************************************************/

void FLegacyONNXTester::ONNXReadNetworkTest(const FString& InONNXModelFileName)
{
	UE_LOG(LogNeuralNetworkInferenceQA, Display, TEXT("-------------------- Read ONNX Network And Test"));

#if WITH_EDITOR
	FModelProto ModelProto;
	FModelProtoFileReader::ReadModelProtoFromFile(ModelProto, InONNXModelFileName);
	UE_LOG(LogNeuralNetworkInferenceQA, Display, TEXT("ModelProto:\n%s"), *ModelProto.ToString());
	ensureMsgf(ModelProto.IsLoaded(), TEXT("FLegacyONNXTester::ONNXReadNetworkTest() failed, FModelProto could not be read from InONNXModelFileName: %s."), *InONNXModelFileName);
#else //WITH_EDITOR
	UE_LOG(LogNeuralNetworkInferenceQA, Display, TEXT("ONNXReadNetworkTest test skipped (only in Editor)."));
#endif //WITH_EDITOR
}
