// Copyright Epic Games, Inc. All Rights Reserved.

#include "NiagaraDataInterfaceArrayFloat.h"
#include "NiagaraDataInterfaceArrayImpl.h"
#include "NiagaraRenderer.h"

template<>
struct FNDIArrayImplHelper<float> : public FNDIArrayImplHelperBase<float>
{
	typedef float TVMArrayType;
	static constexpr TCHAR const* HLSLValueTypeName = TEXT("float");
	static constexpr TCHAR const* HLSLBufferTypeName = TEXT("float");
	static constexpr EPixelFormat PixelFormat = PF_R32_FLOAT;
	static const FNiagaraTypeDefinition& GetTypeDefinition() { return FNiagaraTypeDefinition::GetFloatDef(); }
	static const float GetDefaultValue() { return 0.0f; }
};

template<>
struct FNDIArrayImplHelper<FVector2f> : public FNDIArrayImplHelperBase<FVector2f>
{
	typedef FVector2f TVMArrayType;
	static constexpr TCHAR const* HLSLValueTypeName = TEXT("float2");
	static constexpr TCHAR const* HLSLBufferTypeName = TEXT("float2");
	static constexpr EPixelFormat PixelFormat = PF_G32R32F;
	static const FNiagaraTypeDefinition& GetTypeDefinition() { return FNiagaraTypeDefinition::GetVec2Def(); }
	static const FVector2f GetDefaultValue() { return FVector2f::ZeroVector; }
};

// LWC_TODO: This is represented as an FVector2f internally (array is converted to floats during PushToRenderThread)
template<>
struct FNDIArrayImplHelper<FVector2d> : public FNDIArrayImplHelperBase<FVector2d>
{
	typedef FVector2f TVMArrayType;
	static constexpr TCHAR const* HLSLValueTypeName = TEXT("float2");
	static constexpr TCHAR const* HLSLBufferTypeName = TEXT("float2");
	static constexpr EPixelFormat PixelFormat = PF_G32R32F;
	static const FNiagaraTypeDefinition& GetTypeDefinition() { return FNiagaraTypeDefinition::GetVec2Def(); }
	static const FVector2f GetDefaultValue() { return FVector2f::ZeroVector; }
	
	static int32 GPUGetTypeStride() { return sizeof(FVector2f); }
	static int32 CPUGetTypeStride() { return sizeof(FVector2f); }

	static void CopyData(void* Dest, const FVector2d* Src, int32 BufferSize)
	{
		FVector2f* DestFloats = (FVector2f*)Dest;
		int32 Num = BufferSize / sizeof(FVector2d);
		for (int32 i = 0; i < Num; i++)
			DestFloats[i] = (FVector2f)Src[i];
	}
};

template<>
struct FNDIArrayImplHelper<FVector3f> : public FNDIArrayImplHelperBase<FVector3f>
{
	typedef FVector3f TVMArrayType;
	static constexpr TCHAR const* HLSLValueTypeName = TEXT("float3");
	static constexpr TCHAR const* HLSLBufferTypeName = TEXT("float");	//-OPT: Current we have no float3 pixel format, when we add one update this to use it
	static constexpr EPixelFormat PixelFormat = PF_R32_FLOAT;
	static const FNiagaraTypeDefinition& GetTypeDefinition() { return FNiagaraTypeDefinition::GetVec3Def(); }
	static const FVector3f GetDefaultValue() { return FVector3f::ZeroVector; }

	static void GPUGetFetchHLSL(FString& OutHLSL, const TCHAR* BufferName)
	{
		OutHLSL.Appendf(TEXT("OutValue.x = %s[ClampedIndex * 3 + 0];"), BufferName);
		OutHLSL.Appendf(TEXT("OutValue.y = %s[ClampedIndex * 3 + 1];"), BufferName);
		OutHLSL.Appendf(TEXT("OutValue.z = %s[ClampedIndex * 3 + 2];"), BufferName);
	}
	static int32 GPUGetTypeStride()
	{
		return sizeof(float);
	}
};

// LWC_TODO: This is represented as an FVector3f internally (array is converted to floats during PushToRenderThread)
template<>
struct FNDIArrayImplHelper<FVector3d> : public FNDIArrayImplHelperBase<FVector3d>
{
	typedef FVector3f TVMArrayType;
	static constexpr TCHAR const* HLSLValueTypeName = TEXT("float3");
	static constexpr TCHAR const* HLSLBufferTypeName = TEXT("float");	//-OPT: Current we have no float3 pixel format, when we add one update this to use it
	static constexpr EPixelFormat PixelFormat = PF_R32_FLOAT;
	static const FNiagaraTypeDefinition& GetTypeDefinition() { return FNiagaraTypeDefinition::GetVec3Def(); }
	static const FVector3f GetDefaultValue() { return FVector3f::ZeroVector; }

	static void GPUGetFetchHLSL(FString& OutHLSL, const TCHAR* BufferName)
	{
		OutHLSL.Appendf(TEXT("OutValue.x = %s[ClampedIndex * 3 + 0];"), BufferName);
		OutHLSL.Appendf(TEXT("OutValue.y = %s[ClampedIndex * 3 + 1];"), BufferName);
		OutHLSL.Appendf(TEXT("OutValue.z = %s[ClampedIndex * 3 + 2];"), BufferName);
	}
	static int32 GPUGetTypeStride()
	{
		return sizeof(float);
	}
	static int32 CPUGetTypeStride()
	{
		return sizeof(FVector3f);
	}

	static void CopyData(void* Dest, const FVector3d* Src, int32 BufferSize)
	{
		FVector3f* DestFloats = (FVector3f*)Dest;
		int32 Num = BufferSize / sizeof(FVector3d);
		for (int32 i = 0; i < Num; i++)
			DestFloats[i] = (FVector3f)Src[i];
	}
};

template<>
struct FNDIArrayImplHelper<FVector4f> : public FNDIArrayImplHelperBase<FVector4f>
{
	typedef FVector4f TVMArrayType;
	static constexpr TCHAR const* HLSLValueTypeName = TEXT("float4");
	static constexpr TCHAR const* HLSLBufferTypeName = TEXT("float4");
	static constexpr EPixelFormat PixelFormat = PF_A32B32G32R32F;
	static const FNiagaraTypeDefinition& GetTypeDefinition() { return FNiagaraTypeDefinition::GetVec4Def(); }
	static const FVector4f GetDefaultValue() { return FVector4f(ForceInitToZero); }
};

// LWC_TODO: This is represented as an FVector4f internally (array is converted to floats during PushToRenderThread)
template<>
struct FNDIArrayImplHelper<FVector4d> : public FNDIArrayImplHelperBase<FVector4d>
{
	typedef FVector4f TVMArrayType;
	static constexpr TCHAR const* HLSLValueTypeName = TEXT("float4");
	static constexpr TCHAR const* HLSLBufferTypeName = TEXT("float4");
	static constexpr EPixelFormat PixelFormat = PF_A32B32G32R32F;
	static const FNiagaraTypeDefinition& GetTypeDefinition() { return FNiagaraTypeDefinition::GetVec4Def(); }
	static const FVector4f GetDefaultValue() { return FVector4f(ForceInitToZero); }

	static int32 GPUGetTypeStride() { return sizeof(FVector4f); }
	static int32 CPUGetTypeStride() { return sizeof(FVector4f); }

	static void CopyData(void* Dest, const FVector4d* Src, int32 BufferSize)
	{
		FVector4f* DestFloats = (FVector4f*)Dest;
		int32 Num = BufferSize / sizeof(FVector4d);
		for (int32 i = 0; i < Num; i++)
			DestFloats[i] = (FVector4f)Src[i];
	}
};

template<>
struct FNDIArrayImplHelper<FLinearColor> : public FNDIArrayImplHelperBase<FLinearColor>
{
	typedef FLinearColor TVMArrayType;
	static constexpr TCHAR const* HLSLValueTypeName = TEXT("float4");
	static constexpr TCHAR const* HLSLBufferTypeName = TEXT("float4");
	static constexpr EPixelFormat PixelFormat = PF_A32B32G32R32F;
	static const FNiagaraTypeDefinition& GetTypeDefinition() { return FNiagaraTypeDefinition::GetColorDef(); }
	static const FLinearColor GetDefaultValue() { return FLinearColor::White; }
};

template<>
struct FNDIArrayImplHelper<FQuat4f> : public FNDIArrayImplHelperBase<FQuat4f>
{
	typedef FQuat4f TVMArrayType;
	static constexpr TCHAR const* HLSLValueTypeName = TEXT("float4");
	static constexpr TCHAR const* HLSLBufferTypeName = TEXT("float4");
	static constexpr EPixelFormat PixelFormat = PF_A32B32G32R32F;
	static const FNiagaraTypeDefinition& GetTypeDefinition() { return FNiagaraTypeDefinition::GetQuatDef(); }
	static const FQuat4f GetDefaultValue() { return FQuat4f::Identity; }
};

// LWC_TODO: This is represented as an FQuat4f internally (array is converted to floats during PushToRenderThread)
template<>
struct FNDIArrayImplHelper<FQuat4d> : public FNDIArrayImplHelperBase<FQuat4d>
{
	typedef FQuat4f TVMArrayType;
	static constexpr TCHAR const* HLSLValueTypeName = TEXT("float4");
	static constexpr TCHAR const* HLSLBufferTypeName = TEXT("float4");
	static constexpr EPixelFormat PixelFormat = PF_A32B32G32R32F;
	static const FNiagaraTypeDefinition& GetTypeDefinition() { return FNiagaraTypeDefinition::GetQuatDef(); }
	static const FQuat4f GetDefaultValue() { return FQuat4f::Identity; }

	static int32 GPUGetTypeStride() { return sizeof(FQuat4f); }
	static int32 CPUGetTypeStride() { return sizeof(FQuat4f); }

	static void CopyData(void* Dest, const FQuat4d* Src, int32 BufferSize)
	{
		FQuat4f* DestFloats = (FQuat4f*)Dest;
		int32 Num = BufferSize / sizeof(FQuat4d);
		for (int32 i = 0; i < Num; i++)
			DestFloats[i] = (FQuat4f)Src[i];
	}
};

UNiagaraDataInterfaceArrayFloat::UNiagaraDataInterfaceArrayFloat(FObjectInitializer const& ObjectInitializer)
	: UNiagaraDataInterfaceArray(ObjectInitializer)
{
	Proxy.Reset(new FNiagaraDataInterfaceProxyArrayImpl());
	Impl.Reset(new FNiagaraDataInterfaceArrayImpl<float, UNiagaraDataInterfaceArrayFloat>(this, FloatData));
}

UNiagaraDataInterfaceArrayFloat2::UNiagaraDataInterfaceArrayFloat2(FObjectInitializer const& ObjectInitializer)
	: UNiagaraDataInterfaceArray(ObjectInitializer)
{
	Proxy.Reset(new FNiagaraDataInterfaceProxyArrayImpl());
	Impl.Reset(new FNiagaraDataInterfaceArrayImpl<FVector2D, UNiagaraDataInterfaceArrayFloat2>(this, FloatData));
}

UNiagaraDataInterfaceArrayFloat3::UNiagaraDataInterfaceArrayFloat3(FObjectInitializer const& ObjectInitializer)
	: UNiagaraDataInterfaceArray(ObjectInitializer)
{
	Proxy.Reset(new FNiagaraDataInterfaceProxyArrayImpl());
	Impl.Reset(new FNiagaraDataInterfaceArrayImpl<FVector, UNiagaraDataInterfaceArrayFloat3>(this, FloatData));
}

UNiagaraDataInterfaceArrayFloat4::UNiagaraDataInterfaceArrayFloat4(FObjectInitializer const& ObjectInitializer)
	: UNiagaraDataInterfaceArray(ObjectInitializer)
{
	Proxy.Reset(new FNiagaraDataInterfaceProxyArrayImpl());
	Impl.Reset(new FNiagaraDataInterfaceArrayImpl<FVector4, UNiagaraDataInterfaceArrayFloat4>(this, FloatData));
}

UNiagaraDataInterfaceArrayColor::UNiagaraDataInterfaceArrayColor(FObjectInitializer const& ObjectInitializer)
	: UNiagaraDataInterfaceArray(ObjectInitializer)
{
	Proxy.Reset(new FNiagaraDataInterfaceProxyArrayImpl());
	Impl.Reset(new FNiagaraDataInterfaceArrayImpl<FLinearColor, UNiagaraDataInterfaceArrayColor>(this, ColorData));
}

UNiagaraDataInterfaceArrayQuat::UNiagaraDataInterfaceArrayQuat(FObjectInitializer const& ObjectInitializer)
	: UNiagaraDataInterfaceArray(ObjectInitializer)
{
	Proxy.Reset(new FNiagaraDataInterfaceProxyArrayImpl());
	Impl.Reset(new FNiagaraDataInterfaceArrayImpl<FQuat, UNiagaraDataInterfaceArrayQuat>(this, QuatData));
}