// Copyright Epic Games, Inc. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "Engine/EngineTypes.h"
#include "Containers/ArrayView.h"
#include "Containers/StringView.h"
#include "Misc/StringBuilder.h"
#include "Misc/MemStack.h"
#include "Hash/xxhash.h"
#include "RHIDefinitions.h"
#include "HLSLTree/HLSLTreeTypes.h"

class FMaterial;
class FMaterialCompilationOutput;
struct FStaticParameterSet;

namespace UE
{

namespace Shader
{
class FPreshaderData;
} // namespace Shader

namespace HLSLTree
{

class FErrorHandlerInterface;
class FNode;
class FScope;
class FExpression;
class FFunction;
class FRequestedType;
class FPreparedType;
class FEmitScope;
class FEmitShaderExpression;
class FEmitShaderStatement;

struct FEmitPreshaderScope;

struct FEmitShaderScopeEntry
{
	FEmitShaderScopeEntry() = default;
	FEmitShaderScopeEntry(FEmitScope* InScope, int32 InIndent, FStringBuilderBase& InCode) : Scope(InScope), Code(&InCode), Indent(InIndent) {}

	FEmitScope* Scope = nullptr;
	FStringBuilderBase* Code = nullptr;
	int32 Indent = 0;
};
using FEmitShaderScopeStack = TArray<FEmitShaderScopeEntry, TInlineAllocator<16>>;

class FEmitShaderNode
{
public:
	virtual void EmitShaderCode(FEmitShaderScopeStack& Stack, int32 Indent, FStringBuilderBase& OutString) = 0;
	virtual FEmitShaderExpression* AsExpression() { return nullptr; }
	virtual FEmitShaderStatement* AsStatement() { return nullptr; }

	FEmitShaderNode(FEmitScope& InScope, TArrayView<FEmitShaderNode*> InDependencies);

	FEmitScope* Scope = nullptr;
	FEmitShaderNode* NextScopedNode = nullptr;
	TArrayView<FEmitShaderNode*> Dependencies;
};
using FEmitShaderDependencies = TArray<FEmitShaderNode*, TInlineAllocator<8>>;

class FEmitShaderExpression final : public FEmitShaderNode
{
public:
	FEmitShaderExpression(FEmitScope& InScope, TArrayView<FEmitShaderNode*> InDependencies, const Shader::FType& InType, FXxHash64 InHash)
		: FEmitShaderNode(InScope, InDependencies)
		, Type(InType)
		, Hash(InHash)
	{}

	virtual void EmitShaderCode(FEmitShaderScopeStack& Stack, int32 Indent, FStringBuilderBase& OutString) override;
	virtual FEmitShaderExpression* AsExpression() override { return this; }
	inline bool IsInline() const { return Value == nullptr; }

	const TCHAR* Reference = nullptr;
	const TCHAR* Value = nullptr;
	Shader::FType Type;
	FXxHash64 Hash;
};

enum class EEmitScopeFormat : uint8
{
	None,
	Unscoped,
	Scoped,
};

class FEmitShaderStatement final : public FEmitShaderNode
{
public:
	FEmitShaderStatement(FEmitScope& InScope, TArrayView<FEmitShaderNode*> InDependencies)
		: FEmitShaderNode(InScope, InDependencies)
	{}

	virtual void EmitShaderCode(FEmitShaderScopeStack& Stack, int32 Indent, FStringBuilderBase& OutString) override;
	virtual FEmitShaderStatement* AsStatement() override { return this; }

	FEmitScope* NestedScopes[2] = { nullptr };
	FStringView Code[2];
	EEmitScopeFormat ScopeFormat;
};

enum class EFormatArgType : uint8
{
	Void,
	ShaderValue,
	String,
	Int,
};

struct FFormatArgVariant
{
	FFormatArgVariant() {}
	FFormatArgVariant(FEmitShaderExpression* InValue) : Type(EFormatArgType::ShaderValue), ShaderValue(InValue) { check(InValue); }
	FFormatArgVariant(const TCHAR* InValue) : Type(EFormatArgType::String), String(InValue) { check(InValue); }
	FFormatArgVariant(int32 InValue) : Type(EFormatArgType::Int), Int(InValue) {}

	EFormatArgType Type = EFormatArgType::Void;
	union
	{
		FEmitShaderExpression* ShaderValue;
		const TCHAR* String;
		int32 Int;
	};
};

using FFormatArgList = TArray<FFormatArgVariant, TInlineAllocator<8>>;

namespace Private
{
inline void BuildFormatArgList(FFormatArgList&) {}

template<typename Type, typename... Types>
inline void BuildFormatArgList(FFormatArgList& OutList, Type Arg, Types... Args)
{
	OutList.Add(Arg);
	BuildFormatArgList(OutList, Forward<Types>(Args)...);
}

void InternalFormatStrings(FStringBuilderBase* OutString0,
	FStringBuilderBase* OutString1,
	FEmitShaderDependencies& OutDependencies,
	FStringView Format0,
	FStringView Format1,
	const FFormatArgList& ArgList);
} // namespace Private

template<typename FormatType, typename... Types>
void FormatString(FStringBuilderBase& OutString, FEmitShaderDependencies& OutDependencies, const FormatType& Format, Types... Args)
{
	FFormatArgList ArgList;
	Private::BuildFormatArgList(ArgList, Forward<Types>(Args)...);
	Private::InternalFormatStrings(&OutString, nullptr, OutDependencies, Format, FStringView(), ArgList);
}

template<typename FormatType0, typename FormatType1, typename... Types>
void FormatStrings(FStringBuilderBase& OutString0, FStringBuilderBase& OutString1, FEmitShaderDependencies& OutDependencies, const FormatType0& Format0, const FormatType1& Format1, Types... Args)
{
	FFormatArgList ArgList;
	Private::BuildFormatArgList(ArgList, Forward<Types>(Args)...);
	Private::InternalFormatStrings(&OutString0, &OutString1, OutDependencies, Format0, Format1, ArgList);
}

enum class EEmitScopeState : uint8
{
	Uninitialized,
	Initializing,
	Live,
	Dead,
};

class FEmitScope
{
public:
	static FEmitScope* FindSharedParent(FEmitScope* Lhs, FEmitScope* Rhs);
	bool HasParent(const FEmitScope* InParent) const;
	bool IsLoop() const;
	FEmitScope* FindLoop();

	void EmitShaderCode(FEmitShaderScopeStack& Stack);

	FEmitScope* ParentScope = nullptr;
	FStatement* OwnerStatement = nullptr;
	FStatement* ContainedStatement = nullptr;
	FEmitShaderNode* FirstNode = nullptr;
	int32 NestedLevel = 0;
	EEmitScopeState State = EEmitScopeState::Uninitialized;
	EExpressionEvaluation Evaluation = EExpressionEvaluation::None;
};

struct FPreshaderLocalPHIScope
{
	FPreshaderLocalPHIScope(const FExpression* InExpression, int32 InValueStackPosition) : ExpressionLocalPHI(InExpression), ValueStackPosition(InValueStackPosition) {}

	const FExpression* ExpressionLocalPHI;
	int32 ValueStackPosition;
};

/** Tracks shared state while emitting HLSL code */
class FEmitContext
{
public:
	explicit FEmitContext(FMemStackBase& InAllocator, FErrorHandlerInterface& InErrors, const Shader::FStructTypeRegistry& InTypeRegistry);
	~FEmitContext();

	FPreparedType PrepareExpression(FExpression* InExpression, FEmitScope& Scope, const FRequestedType& RequestedType);

	FEmitScope* InternalPrepareScope(FScope* Scope, FScope* ParentScope);
	FEmitScope* PrepareScope(FScope* Scope);
	FEmitScope* PrepareScopeWithParent(FScope* Scope, FScope* ParentScope);
	void MarkScopeEvaluation(FEmitScope& EmitParentScope, FScope* Scope, EExpressionEvaluation Evaluation);
	void MarkScopeDead(FEmitScope& EmitParentScope, FScope* Scope);

	void EmitPreshaderScope(const FScope* Scope, const FRequestedType& RequestedType, TArrayView<const FEmitPreshaderScope> PreshaderScopes, Shader::FPreshaderData& OutPreshader);
	void EmitPreshaderScope(FEmitScope& EmitScope, const FRequestedType& RequestedType, TArrayView<const FEmitPreshaderScope> PreshaderScopes, Shader::FPreshaderData& OutPreshader);

	FEmitScope* AcquireEmitScopeWithParent(const FScope* Scope, FEmitScope* EmitParentScope);
	FEmitScope* AcquireEmitScope(const FScope* Scope);
	FEmitScope* FindEmitScope(const FScope* Scope);
	FEmitScope* InternalEmitScope(const FScope* Scope);

	template<typename T>
	static TArrayView<FEmitShaderNode*> MakeDependencies(T*& Dependency)
	{
		return Dependency ? TArrayView<FEmitShaderNode*>(&Dependency, 1) : TArrayView<FEmitShaderNode*>();
	}

	void Finalize();

	FEmitShaderExpression* InternalEmitExpression(FEmitScope& Scope, TArrayView<FEmitShaderNode*> Dependencies, bool bInline, const Shader::FType& Type, FStringView Code);

	template<typename FormatType, typename... Types>
	FEmitShaderExpression* EmitExpressionWithDependencies(FEmitScope& Scope, TArrayView<FEmitShaderNode*> Dependencies, const Shader::FType& Type, const FormatType& Format, Types... Args)
	{
		TStringBuilder<2048> String;
		FEmitShaderDependencies LocalDependencies(Dependencies);
		FormatString(String, LocalDependencies, Format, Forward<Types>(Args)...);
		return InternalEmitExpression(Scope, LocalDependencies, false, Type, String.ToView());
	}

	template<typename FormatType, typename... Types>
	FEmitShaderExpression* EmitInlineExpressionWithDependencies(FEmitScope& Scope, TArrayView<FEmitShaderNode*> Dependencies, const Shader::FType& Type, const FormatType& Format, Types... Args)
	{
		TStringBuilder<2048> String;
		FEmitShaderDependencies LocalDependencies(Dependencies);
		FormatString(String, LocalDependencies, Format, Forward<Types>(Args)...);
		return InternalEmitExpression(Scope, LocalDependencies, true, Type, String.ToView());
	}

	template<typename FormatType, typename... Types>
	FEmitShaderExpression* EmitInlineExpressionWithDependency(FEmitScope& Scope, FEmitShaderNode* Dependency, const Shader::FType& Type, const FormatType& Format, Types... Args)
	{
		return EmitInlineExpressionWithDependencies(Scope, MakeDependencies(Dependency), Type, Format, Forward<Types>(Args)...);
	}

	template<typename FormatType, typename... Types>
	FEmitShaderExpression* EmitExpression(FEmitScope& Scope, const Shader::FType& Type, const FormatType& Format, Types... Args)
	{
		return EmitExpressionWithDependencies(Scope, TArrayView<FEmitShaderNode*>(), Type, Format, Forward<Types>(Args)...);
	}

	template<typename FormatType, typename... Types>
	FEmitShaderExpression* EmitInlineExpression(FEmitScope& Scope, const Shader::FType& Type, const FormatType& Format, Types... Args)
	{
		return EmitInlineExpressionWithDependencies(Scope, TArrayView<FEmitShaderNode*>(), Type, Format, Forward<Types>(Args)...);
	}

	FEmitShaderStatement* InternalEmitStatement(FEmitScope& Scope, TArrayView<FEmitShaderNode*> Dependencies, EEmitScopeFormat ScopeFormat, FEmitScope* NestedScope0, FEmitScope* NestedScope1, FStringView Code0, FStringView Code1);

	template<typename FormatType0, typename FormatType1, typename... Types>
	FEmitShaderStatement* EmitFormatStatementInternal(FEmitScope& Scope,
		TArrayView<FEmitShaderNode*> Dependencies,
		EEmitScopeFormat ScopeFormat,
		FEmitScope* NestedScope0,
		FEmitScope* NestedScope1,
		const FormatType0& Format0,
		const FormatType1& Format1,
		Types... Args)
	{
		TStringBuilder<1024> String0;
		TStringBuilder<1024> String1;
		FEmitShaderDependencies LocalDependencies(Dependencies);
		FormatStrings(String0, String1, LocalDependencies, Format0, Format1, Forward<Types>(Args)...);
		return InternalEmitStatement(Scope, LocalDependencies, ScopeFormat, NestedScope0, NestedScope1, String0.ToView(), String1.ToView());
	}

	template<typename FormatType, typename... Types>
	FEmitShaderStatement* EmitStatementWithDependencies(FEmitScope& Scope, TArrayView<FEmitShaderNode*> Dependencies, const FormatType& Format, Types... Args)
	{
		TStringBuilder<1024> String;
		FEmitShaderDependencies LocalDependencies(Dependencies);
		FormatString(String, LocalDependencies, Format, Forward<Types>(Args)...);
		return InternalEmitStatement(Scope, LocalDependencies, EEmitScopeFormat::None, nullptr, nullptr, String.ToView(), FStringView());
	}

	template<typename FormatType, typename... Types>
	FEmitShaderStatement* EmitStatementWithDependency(FEmitScope& Scope, FEmitShaderNode* Dependency, const FormatType& Format, Types... Args)
	{
		return EmitStatementWithDependencies(Scope, MakeDependencies(Dependency), Format, Forward<Types>(Args)...);
	}

	template<typename FormatType, typename... Types>
	FEmitShaderStatement* EmitStatement(FEmitScope& Scope, const FormatType& Format, Types... Args)
	{
		return EmitStatementWithDependencies(Scope, TArrayView<FEmitShaderNode*>(), Format, Forward<Types>(Args)...);
	}

	FEmitShaderStatement* EmitNextScopeWithDependency(FEmitScope& Scope, FEmitShaderNode* Dependency, FScope* NextScope)
	{
		FEmitScope* EmitScope = InternalEmitScope(NextScope);
		if (EmitScope)
		{
			return InternalEmitStatement(Scope, MakeDependencies(Dependency), EEmitScopeFormat::Unscoped, EmitScope, nullptr, FStringView(), FStringView());
		}
		return nullptr;
	}

	FEmitShaderStatement* EmitNextScope(FEmitScope& Scope, FScope* NextScope)
	{
		return EmitNextScopeWithDependency(Scope, nullptr, NextScope);
	}

	template<typename FormatType, typename... Types>
	FEmitShaderStatement* EmitNestedScope(FEmitScope& Scope, FScope* NestedScope, const FormatType& Format, Types... Args)
	{
		FEmitScope* EmitScope = InternalEmitScope(NestedScope);
		if (EmitScope)
		{
			TStringBuilder<1024> String;
			FEmitShaderDependencies LocalDependencies;
			FormatString(String, LocalDependencies, Format, Forward<Types>(Args)...);
			return InternalEmitStatement(Scope, LocalDependencies, EEmitScopeFormat::Scoped, EmitScope, nullptr, String.ToView(), FStringView());
		}
		return nullptr;
	}

	template<typename FormatType0, typename FormatType1, typename... Types>
	FEmitShaderStatement* EmitNestedScopes(FEmitScope& Scope, FScope* NestedScope0, FScope* NestedScope1, const FormatType0& Format0, const FormatType1& Format1, Types... Args)
	{
		FEmitScope* EmitScope0 = InternalEmitScope(NestedScope0);
		FEmitScope* EmitScope1 = InternalEmitScope(NestedScope1);
		if (EmitScope1)
		{
			TStringBuilder<1024> String0;
			TStringBuilder<1024> String1;
			FEmitShaderDependencies LocalDependencies;
			FormatStrings(String0, String1, LocalDependencies, Format0, Format1, Forward<Types>(Args)...);
			return InternalEmitStatement(Scope, LocalDependencies, EEmitScopeFormat::Scoped, EmitScope0, EmitScope1, String0.ToView(), String1.ToView());
		}
		else if (EmitScope0)
		{
			TStringBuilder<1024> String;
			FEmitShaderDependencies LocalDependencies;
			FormatString(String, LocalDependencies, Format0, Forward<Types>(Args)...);
			return InternalEmitStatement(Scope, LocalDependencies, EEmitScopeFormat::Scoped, EmitScope0, nullptr, String.ToView(), FStringView());
		}

		return nullptr;
	}

	FEmitShaderExpression* EmitPreshaderOrConstant(FEmitScope& Scope, const FRequestedType& RequestedType, FExpression* Expression);
	FEmitShaderExpression* EmitConstantZero(FEmitScope& Scope, const Shader::FType& Type);
	FEmitShaderExpression* EmitCast(FEmitScope& Scope, FEmitShaderExpression* ShaderValue, const Shader::FType& DestType);

	FMemStackBase* Allocator = nullptr;
	FErrorHandlerInterface* Errors = nullptr;
	const Shader::FStructTypeRegistry* TypeRegistry = nullptr;
	EShaderFrequency ShaderFrequency = SF_Pixel;

	TArray<FEmitShaderNode*> EmitNodes;
	TMap<const FScope*, FEmitScope*> EmitScopeMap;
	TMap<const FExpression*, FEmitScope*> PrepareLocalPHIMap;
	TMap<const FExpression*, FEmitShaderExpression*> EmitLocalPHIMap;
	TMap<FXxHash64, FEmitShaderExpression*> EmitExpressionMap;
	TMap<FXxHash64, FEmitShaderExpression*> EmitPreshaderMap;
	TMap<const FFunction*, FEmitShaderNode*> EmitFunctionMap;
	TArray<struct FPreshaderLoopScope*> PreshaderLoopScopes;
	TArray<const FPreshaderLocalPHIScope*> PreshaderLocalPHIScopes;
	int32 PreshaderStackPosition = 0;

	// TODO - remove preshader material dependency
	const FMaterial* Material = nullptr;
	const FStaticParameterSet* StaticParameters = nullptr;
	FMaterialCompilationOutput* MaterialCompilationOutput = nullptr;
	TMap<Shader::FValue, uint32> DefaultUniformValues;
	uint32 UniformPreshaderOffset = 0u;
	uint32 CurrentBoolUniformOffset = ~0u;
	uint32 CurrentNumBoolComponents = 32u;
	bool bReadMaterialNormal = false;
	uint32 TexCoordMask[SF_NumFrequencies] = { 0u };

	int32 NumExpressionLocals = 0;
	int32 NumExpressionLocalPHIs = 0;
};

} // namespace HLSLTree
} // namespace UE
