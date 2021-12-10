// Copyright Epic Games, Inc. All Rights Reserved.
#pragma once

#include "Misc/EnumClassFlags.h"
#include "Containers/BitArray.h"
#include "Shader/ShaderTypes.h"

class UTexture;

namespace UE
{
namespace HLSLTree
{

class FNode;
class FStructType;
class FExpression;
class FTextureParameterDeclaration;
class FStatement;
class FScope;
class FTree;

/**
 * Describes how a given expression needs to be evaluated */
enum class EExpressionEvaluation : uint8
{
	/** Invalid/uninitialized */
	None,

	/** The expression outputs HLSL code (via FExpressionEmitResult::Writer) */
	Shader,

	/** The expression outputs preshader code evaluated at runtime (via FExpressionEmitResult::Preshader) */
	Preshader,

	/** The expression outputs constant preshader code evaluated at compile time (via FExpressionEmitResult::Preshader) */
	Constant,
};

EExpressionEvaluation CombineEvaluations(EExpressionEvaluation Lhs, EExpressionEvaluation Rhs);

enum class EExpressionDerivative : uint8
{
	/** Uninitialized/unknown */
	None,

	/** Derivative is not valid */
	Invalid,

	/** Derivative is known to be 0 */
	Zero,

	/** Derivative is valid */
	Valid,
};

EExpressionDerivative CombineDerivatives(EExpressionDerivative Lhs, EExpressionDerivative Rhs);

struct FTextureDescription
{
	FTextureDescription()
		: Texture(nullptr), SamplerType(SAMPLERTYPE_Color)
	{}

	FTextureDescription(UTexture* InTexture, EMaterialSamplerType InSamplerType)
		: Texture(InTexture), SamplerType(InSamplerType)
	{}

	UTexture* Texture;
	EMaterialSamplerType SamplerType;
};
inline bool operator==(const FTextureDescription& Lhs, const FTextureDescription& Rhs)
{
	return Lhs.Texture == Rhs.Texture && Lhs.SamplerType == Rhs.SamplerType;
}
inline bool operator!=(const FTextureDescription& Lhs, const FTextureDescription& Rhs)
{
	return !operator==(Lhs, Rhs);
}
inline uint32 GetTypeHash(const FTextureDescription& Ref)
{
	return HashCombine(GetTypeHash(Ref.Texture), GetTypeHash(Ref.SamplerType));
}

} // namespace HLSLTree
} // namespace UE
