// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "NiagaraDataInterface.h"
#include "NiagaraCommon.h"
#include "NiagaraEmitterInstance.h"
#include "NiagaraDataInterfaceParticleRead.generated.h"

struct FNDIParticleRead_InstanceData
{
	FNiagaraSystemInstance* SystemInstance;
	FNiagaraEmitterInstance* EmitterInstance;
};

UCLASS(EditInlineNew, Category = "ParticleRead", meta = (DisplayName = "Particle Attribute Reader"))
class NIAGARA_API UNiagaraDataInterfaceParticleRead : public UNiagaraDataInterface
{
	GENERATED_UCLASS_BODY()
public:
	UPROPERTY(EditAnywhere, Category = "ParticleRead")
	FString EmitterName;

	//UObject Interface
	virtual void PostInitProperties()override;
	//UObject Interface End

	//UNiagaraDataInterface Interface
	virtual bool InitPerInstanceData(void* PerInstanceData, FNiagaraSystemInstance* SystemInstance) override;
	virtual int32 PerInstanceDataSize() const { return sizeof(FNDIParticleRead_InstanceData); }
	virtual void GetFunctions(TArray<FNiagaraFunctionSignature>& OutFunctions) override;
	virtual void GetVMExternalFunction(const FVMExternalFunctionBindingInfo& BindingInfo, void* InstanceData, FVMExternalFunction& OutFunc) override;
	virtual bool CanExecuteOnTarget(ENiagaraSimTarget Target) const override { return true; }
	virtual bool Equals(const UNiagaraDataInterface* Other) const override;
	virtual void GetParameterDefinitionHLSL(const FNiagaraDataInterfaceGPUParamInfo& ParamInfo, FString& OutHLSL) override;
	virtual bool GetFunctionHLSL(const FNiagaraDataInterfaceGPUParamInfo& ParamInfo, const FNiagaraDataInterfaceGeneratedFunction& FunctionInfo, int FunctionInstanceIndex, FString& OutHLSL) override;
	virtual FNiagaraDataInterfaceParametersCS* ConstructComputeParameters() const override;
	virtual void ProvidePerInstanceDataForRenderThread(void* DataForRenderThread, void* PerInstanceData, const FNiagaraSystemInstanceID& SystemInstance) override;
	//UNiagaraDataInterface Interface End

	void GetNumSpawnedParticles(FVectorVMContext& Context);
	void GetSpawnedIDAtIndex(FVectorVMContext& Context);
	void ReadFloat(FVectorVMContext& Context, FName AttributeToRead);
	void ReadVector2(FVectorVMContext& Context, FName AttributeToRead);
	void ReadVector3(FVectorVMContext& Context, FName AttributeToRead);
	void ReadVector4(FVectorVMContext& Context, FName AttributeToRead);
	void ReadInt(FVectorVMContext& Context, FName AttributeToRead);
	void ReadBool(FVectorVMContext& Context, FName AttributeToRead);
	void ReadColor(FVectorVMContext& Context, FName AttributeToRead);
	void ReadQuat(FVectorVMContext& Context, FName AttributeToRead);

protected:
	virtual bool CopyToInternal(UNiagaraDataInterface* Destination) const override;

private:
	template<typename T>
	T RetrieveValueWithCheck(FNiagaraEmitterInstance* EmitterInstance, const FNiagaraTypeDefinition& Type, const FName& Attr, const FNiagaraID& ParticleID, bool& bValid);
};