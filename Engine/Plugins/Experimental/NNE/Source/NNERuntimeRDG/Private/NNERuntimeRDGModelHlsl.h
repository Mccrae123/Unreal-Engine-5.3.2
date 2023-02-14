// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "NNERuntimeRDGModel.h"
#include "NNECoreTypes.h"

class FRDGBuilder;

namespace UE::NNERuntimeRDG::Private::Hlsl
{
struct FOperatorHlsl;

class FModel : public FModelRDG
{
	
public:

	~FModel() = default;

	bool Init(TConstArrayView<uint8> ModelData);

protected:

	virtual int PrepareTensorShapesAndData() override;
	virtual bool AddWeightsToRDGGraph(FRDGBuilder& RDGBuilder) override;
	virtual void AddDispatchOps_RenderThread(FRDGBuilder& GraphBuilder) override;

	bool PrepareWeights();
	bool PrepareConstants();

private:

	TArray<FOperatorHlsl*>	Operators;
	TArray<TRefCountPtr<FRDGPooledBuffer>> WeightsExternalRDGResources;
	TArray<TRefCountPtr<FRDGPooledBuffer>> ConstantsExternalRDGResources;
};

} // namespace UE::NNERuntimeRDG::Private::Hlsl