// Copyright Epic Games, Inc. All Rights Reserved.
#include "HLSLTree/HLSLTree.h"
#include "Misc/StringBuilder.h"
#include "Misc/MemStack.h"
#include "Misc/MemStackUtility.h"
#include "Shader/ShaderTypes.h"
#include "MaterialShared.h" // TODO - split preshader out into its own module
#include "Misc/LargeWorldRenderPosition.h"

static FSHAHash HashString(const TCHAR* String, int32 Length)
{
	FSHAHash Hash;
	FSHA1::HashBuffer(String, Length * sizeof(TCHAR), Hash.Hash);
	return Hash;
}

static FSHAHash HashString(const FStringBuilderBase& StringBuilder)
{
	return HashString(StringBuilder.GetData(), StringBuilder.Len());
}

static FSHAHash HashString(FStringView String)
{
	return HashString(String.GetData(), String.Len());
}

namespace UE
{
namespace HLSLTree
{

EExpressionEvaluation CombineEvaluations(EExpressionEvaluation Lhs, EExpressionEvaluation Rhs)
{
	if (Lhs == EExpressionEvaluation::None)
	{
		// If either is 'None', return the other
		return Rhs;
	}
	else if (Rhs == EExpressionEvaluation::None)
	{
		return Lhs;
	}
	else if (Lhs == EExpressionEvaluation::Constant && Rhs == EExpressionEvaluation::Constant)
	{
		// 2 constants make a constant
		return EExpressionEvaluation::Constant;
	}
	else if (Lhs == EExpressionEvaluation::Shader || Rhs == EExpressionEvaluation::Shader)
	{
		// If either requires shader, shader is required
		return EExpressionEvaluation::Shader;
	}
	// Any combination of constants/preshader can make a preshader
	return EExpressionEvaluation::Preshader;
}

EExpressionDerivative CombineDerivatives(EExpressionDerivative Lhs, EExpressionDerivative Rhs)
{
	if (Lhs == EExpressionDerivative::None)
	{
		return Rhs;
	}
	else if (Rhs == EExpressionDerivative::None)
	{
		return Lhs;
	}
	else if (Lhs == EExpressionDerivative::Invalid || Rhs == EExpressionDerivative::Invalid)
	{
		return EExpressionDerivative::Invalid;
	}
	else if (Lhs == EExpressionDerivative::Zero && Rhs == EExpressionDerivative::Zero)
	{
		return EExpressionDerivative::Zero;
	}
	return EExpressionDerivative::Valid;
}

FErrors::FErrors(FMemStackBase& InAllocator)
	: Allocator(&InAllocator)
{
}

void FErrors::AddError(const FNode* InNode, FStringView InError)
{
	const int32 SizeofString = InError.Len() * sizeof(TCHAR);
	void* Memory = Allocator->Alloc(sizeof(FError) + SizeofString, alignof(FError));
	FError* Error = new(Memory) FError();
	FMemory::Memcpy(Error->Message, InError.GetData(), SizeofString);
	Error->Message[InError.Len()] = 0;
	Error->MessageLength = InError.Len();
	Error->Node = InNode;
	Error->Next = FirstError;
	FirstError = Error;
	NumErrors++;

	ensureMsgf(false, TEXT("%s"), Error->Message);
}

FEmitContext::FEmitContext(FMemStackBase& InAllocator, const Shader::FStructTypeRegistry& InTypeRegistry)
	: Allocator(&InAllocator)
	, TypeRegistry(&InTypeRegistry)
	, Errors(InAllocator)
{
}

FEmitContext::~FEmitContext()
{
}

const TCHAR* FEmitContext::AcquireLocalDeclarationCode()
{
	return MemStack::AllocateStringf(*Allocator, TEXT("Local%d"), NumExpressionLocals++);
}

namespace Private
{
void MoveToScope(FEmitShaderCode* ShaderValue, FScope* Scope)
{
	if (ShaderValue->Scope != Scope)
	{
		FScope* NewScope = FScope::FindSharedParent(ShaderValue->Scope, Scope);
		check(NewScope);

		ShaderValue->Scope = NewScope;
		for (FEmitShaderCode* Dependency : ShaderValue->Dependencies)
		{
			MoveToScope(Dependency, NewScope);
		}
	}
}

void FormatArg_ShaderValue(FEmitShaderCode* ShaderValue, FEmitShaderValueDependencies& OutDependencies, FStringBuilderBase& OutCode)
{
	OutDependencies.Add(ShaderValue);
	OutCode.Append(ShaderValue->Reference);
}

} // namespace Private

FEmitShaderCode* FEmitContext::EmitFormatCodeInternal(const Shader::FType& Type, const TCHAR* Format, bool bInline, const FFormatArgList& ArgList)
{
	check(!Type.IsVoid());

	TStringBuilder<1024> FormattedCode;
	FEmitShaderValueDependencies Dependencies;
	int32 ArgIndex = 0;
	while (true)
	{
		const TCHAR Char = *Format++;
		if (Char == 0)
		{
			break;
		}
		else if (Char == TEXT('%'))
		{
			const FFormatArgVariant& Arg = ArgList[ArgIndex++];
			switch (Arg.Type)
			{
			case EFormatArgType::ShaderValue: Private::FormatArg_ShaderValue(Arg.ShaderValue, Dependencies, FormattedCode); break;
			case EFormatArgType::String: FormattedCode.Append(Arg.String); break;
			case EFormatArgType::Int: FormattedCode.Appendf(TEXT("%d"), Arg.Int); break;
			default:
				checkNoEntry();
				break;
			}
		}
		else
		{
			FormattedCode.Append(Char);
		}
	}
	checkf(ArgIndex == ArgList.Num(), TEXT("%d args were provided, but %d were used"), ArgList.Num(), ArgIndex);

	return EmitCodeInternal(Type, FormattedCode.ToView(), bInline, Dependencies);
}

FEmitShaderCode* FEmitContext::EmitCodeInternal(const Shader::FType& Type, FStringView Code, bool bInline, TArrayView<FEmitShaderCode*> Dependencies)
{
	FScope* CurrentScope = ScopeStack.Last();
	check(IsScopeLive(CurrentScope));

	FEmitShaderCode* ShaderValue = nullptr;

	// Check to see if we've already generated code for an equivalent expression
	const FSHAHash ShaderHash = HashString(Code);
	FEmitShaderCode** const PrevShaderValue = ShaderValueMap.Find(ShaderHash);
	if (PrevShaderValue)
	{
		ShaderValue = *PrevShaderValue;
		check(ShaderValue->Type == Type);
		Private::MoveToScope(ShaderValue, CurrentScope);
	}
	else
	{
		ShaderValue = new(*Allocator) FEmitShaderCode(CurrentScope, Type);
		ShaderValue->Hash = ShaderHash;
		ShaderValue->Dependencies = MemStack::AllocateArrayView(*Allocator, Dependencies);
		if (bInline)
		{
			ShaderValue->Reference = MemStack::AllocateString(*Allocator, Code);
		}
		else
		{
			ShaderValue->Reference = AcquireLocalDeclarationCode();
			ShaderValue->Value = MemStack::AllocateString(*Allocator, Code);
			ShaderValueMap.Add(ShaderHash, ShaderValue);
		}
	}

	return ShaderValue;
}

namespace Private
{
void WriteMaterialUniformAccess(Shader::EValueComponentType ComponentType, uint32 NumComponents, uint32 UniformOffset, FStringBuilderBase& OutResult)
{
	static const TCHAR IndexToMask[] = TEXT("xyzw");
	uint32 RegisterIndex = UniformOffset / 4;
	uint32 RegisterOffset = UniformOffset % 4;
	uint32 NumComponentsToWrite = NumComponents;
	bool bConstructor = false;

	check(ComponentType == Shader::EValueComponentType::Float || ComponentType == Shader::EValueComponentType::Int);
	const bool bIsInt = (ComponentType == Shader::EValueComponentType::Int);

	while (NumComponentsToWrite > 0u)
	{
		const uint32 NumComponentsInRegister = FMath::Min(NumComponentsToWrite, 4u - RegisterOffset);
		if (NumComponentsInRegister < NumComponents && !bConstructor)
		{
			// Uniform will be split across multiple registers, so add the constructor to concat them together
			OutResult.Appendf(TEXT("%s%d("), Shader::GetComponentTypeName(ComponentType), NumComponents);
			bConstructor = true;
		}

		if (bIsInt)
		{
			// PreshaderBuffer is typed as float4, so reinterpret as 'int' if needed
			OutResult.Append(TEXT("asint("));
		}

		OutResult.Appendf(TEXT("Material.PreshaderBuffer[%u]"), RegisterIndex);
		// Can skip writing mask if we're taking all 4 components from the register
		if (NumComponentsInRegister < 4u)
		{
			OutResult.Append(TCHAR('.'));
			for (uint32 i = 0u; i < NumComponentsInRegister; ++i)
			{
				OutResult.Append(IndexToMask[RegisterOffset + i]);
			}
		}

		if (bIsInt)
		{
			OutResult.Append(TEXT(")"));
		}

		NumComponentsToWrite -= NumComponentsInRegister;
		RegisterIndex++;
		RegisterOffset = 0u;
		if (NumComponentsToWrite > 0u)
		{
			OutResult.Append(TEXT(", "));
		}
	}
	if (bConstructor)
	{
		OutResult.Append(TEXT(")"));
	}
}
} // namespace Private

FEmitShaderCode* FEmitContext::EmitPreshaderOrConstant(const FRequestedType& RequestedType, FExpression* Expression)
{
	Shader::FPreshaderData LocalPreshader;
	Expression->EmitValuePreshader(*this, RequestedType, LocalPreshader);

	const Shader::FType Type = RequestedType.GetType();

	FSHA1 Hasher;
	Hasher.Update((uint8*)&Type, sizeof(Type));
	LocalPreshader.AppendHash(Hasher);
	const FSHAHash Hash = Hasher.Finalize();
	FEmitShaderCode* const* PrevShaderValue = PreshaderValueMap.Find(Hash);
	if (PrevShaderValue)
	{
		FScope* CurrentScope = ScopeStack.Last();
		check(IsScopeLive(CurrentScope));

		FEmitShaderCode* ShaderValue = *PrevShaderValue;
		check(ShaderValue->Type == Type);
		Private::MoveToScope(ShaderValue, CurrentScope);
		return ShaderValue;
	}

	Shader::FPreshaderStack Stack;
	const Shader::FPreshaderValue ConstantValue = LocalPreshader.EvaluateConstant(*Material, Stack);

	TStringBuilder<1024> FormattedCode;
	if (Type.IsStruct())
	{
		FormattedCode.Append(TEXT("{ "));
	}

	FMaterialUniformPreshaderHeader* PreshaderHeader = nullptr;
	uint32 CurrentBoolUniformOffset = ~0u;
	uint32 CurrentNumBoolComponents = 32u;

	int32 ComponentIndex = 0;
	for (int32 FieldIndex = 0; FieldIndex < Type.GetNumFlatFields(); ++FieldIndex)
	{
		if (FieldIndex > 0)
		{
			FormattedCode.Append(TEXT(", "));
		}

		const Shader::EValueType FieldType = Type.GetFlatFieldType(FieldIndex);
		const Shader::FValueTypeDescription TypeDesc = Shader::GetValueTypeDescription(FieldType);
		const int32 NumFieldComponents = TypeDesc.NumComponents;
		const EExpressionEvaluation FieldEvaluation = Expression->GetPreparedType().GetFieldData(ComponentIndex, NumFieldComponents).Evaluation;

		if (FieldEvaluation == EExpressionEvaluation::Preshader)
		{
			// Only need to allocate uniform buffer for non-constant components
			// Constant components can have their value inlined into the shader directly
			FUniformExpressionSet& UniformExpressionSet = MaterialCompilationOutput->UniformExpressionSet;
			if (!PreshaderHeader)
			{
				// Allocate a preshader header the first time we hit a non-constant field
				PreshaderHeader = &UniformExpressionSet.UniformPreshaders.AddDefaulted_GetRef();
				PreshaderHeader->FieldIndex = UniformExpressionSet.UniformPreshaderFields.Num();
				PreshaderHeader->NumFields = 0u;
				PreshaderHeader->OpcodeOffset = UniformExpressionSet.UniformPreshaderData.Num();
				Expression->EmitValuePreshader(*this, RequestedType, UniformExpressionSet.UniformPreshaderData);
				PreshaderHeader->OpcodeSize = UniformExpressionSet.UniformPreshaderData.Num() - PreshaderHeader->OpcodeOffset;
			}

			FMaterialUniformPreshaderField& PreshaderField = UniformExpressionSet.UniformPreshaderFields.AddDefaulted_GetRef();
			PreshaderField.ComponentIndex = ComponentIndex;
			PreshaderField.Type = FieldType;
			PreshaderHeader->NumFields++;

			if (TypeDesc.ComponentType == Shader::EValueComponentType::Bool)
			{
				// 'Bool' uniforms are packed into bits
				if (CurrentNumBoolComponents + NumFieldComponents > 32u)
				{
					CurrentBoolUniformOffset = UniformPreshaderOffset++;
					CurrentNumBoolComponents = 0u;
				}

				const uint32 RegisterIndex = CurrentBoolUniformOffset / 4;
				const uint32 RegisterOffset = CurrentBoolUniformOffset % 4;
				FormattedCode.Appendf(TEXT("UnpackUniform_%s(asuint(Material.PreshaderBuffer[%u][%u]), %u)"),
					TypeDesc.Name,
					RegisterIndex,
					RegisterOffset,
					CurrentNumBoolComponents);

				PreshaderField.BufferOffset = CurrentBoolUniformOffset * 32u + CurrentNumBoolComponents;
				CurrentNumBoolComponents += NumFieldComponents;
			}
			else if (TypeDesc.ComponentType == Shader::EValueComponentType::Double)
			{
				// Double uniforms are split into Tile/Offset components to make FLWCScalar/FLWCVectors
				PreshaderField.BufferOffset = UniformPreshaderOffset;

				if (NumFieldComponents > 1)
				{
					FormattedCode.Appendf(TEXT("MakeLWCVector%d("), NumFieldComponents);
				}
				else
				{
					FormattedCode.Append(TEXT("MakeLWCScalar("));
				}

				// Write the tile uniform
				Private::WriteMaterialUniformAccess(Shader::EValueComponentType::Float, NumFieldComponents, UniformPreshaderOffset, FormattedCode);
				UniformPreshaderOffset += NumFieldComponents;
				FormattedCode.Append(TEXT(", "));

				// Write the offset uniform
				Private::WriteMaterialUniformAccess(Shader::EValueComponentType::Float, NumFieldComponents, UniformPreshaderOffset, FormattedCode);
				UniformPreshaderOffset += NumFieldComponents;
				FormattedCode.Append(TEXT(")"));
			}
			else
			{
				// Float/Int uniforms are written directly to the uniform buffer
				const uint32 RegisterOffset = UniformPreshaderOffset % 4;
				if (RegisterOffset + NumFieldComponents > 4u)
				{
					// If this uniform would span multiple registers, align offset to the next register to avoid this
					// TODO - we could keep track of this empty padding space, and pack other smaller uniform types here
					UniformPreshaderOffset = Align(UniformPreshaderOffset, 4u);
				}

				PreshaderField.BufferOffset = UniformPreshaderOffset;
				Private::WriteMaterialUniformAccess(TypeDesc.ComponentType, NumFieldComponents, UniformPreshaderOffset, FormattedCode);
				UniformPreshaderOffset += NumFieldComponents;
			}
		}
		else
		{
			// We allow FieldEvaluation to be 'None', since in that case we still need to fill in a value for the HLSL initializer
			check(FieldEvaluation == EExpressionEvaluation::Constant || FieldEvaluation == EExpressionEvaluation::None);

			// The type generated by the preshader might not match the expected type
			// In the future, with new HLSLTree, preshader could potentially include explicit cast opcodes, and avoid implicit conversions
			Shader::FValue FieldConstantValue(ConstantValue.Type.GetComponentType(ComponentIndex), NumFieldComponents);
			for (int32 i = 0; i < NumFieldComponents; ++i)
			{
				// Allow replicating scalar values
				FieldConstantValue.Component[i] = ConstantValue.Component.Num() == 1 ? ConstantValue.Component[0] : ConstantValue.Component[ComponentIndex + i];
			}

			if (TypeDesc.ComponentType == Shader::EValueComponentType::Double)
			{
				const Shader::FDoubleValue DoubleValue = FieldConstantValue.AsDouble();
				TStringBuilder<256> TileValue;
				TStringBuilder<256> OffsetValue;
				for (int32 Index = 0; Index < NumFieldComponents; ++Index)
				{
					if (Index > 0)
					{
						TileValue.Append(TEXT(", "));
						OffsetValue.Append(TEXT(", "));
					}

					const FLargeWorldRenderScalar Value(DoubleValue[Index]);
					TileValue.Appendf(TEXT("%#.9gf"), Value.GetTile());
					OffsetValue.Appendf(TEXT("%#.9gf"), Value.GetOffset());
				}

				if (NumFieldComponents > 1)
				{
					FormattedCode.Appendf(TEXT("MakeLWCVector%d(float%d(%s), float%d(%s))"), NumFieldComponents, NumFieldComponents, TileValue.ToString(), NumFieldComponents, OffsetValue.ToString());
				}
				else
				{
					FormattedCode.Appendf(TEXT("MakeLWCScalar(%s, %s)"), TileValue.ToString(), OffsetValue.ToString());
				}
			}
			else
			{
				const Shader::FValue CastFieldConstantValue = Shader::Cast(FieldConstantValue, FieldType);
				if (NumFieldComponents > 1)
				{
					FormattedCode.Appendf(TEXT("%s("), TypeDesc.Name);
				}
				for (int32 Index = 0; Index < NumFieldComponents; ++Index)
				{
					if (Index > 0)
					{
						FormattedCode.Append(TEXT(", "));
					}
					CastFieldConstantValue.Component[Index].ToString(TypeDesc.ComponentType, FormattedCode);
				}
				if (NumFieldComponents > 1)
				{
					FormattedCode.Append(TEXT(")"));
				}
			}
		}
		ComponentIndex += NumFieldComponents;
	}
	check(ComponentIndex == Type.GetNumComponents());

	if (Type.IsStruct())
	{
		FormattedCode.Append(TEXT(" }"));
	}

	const bool bInline = !Type.IsStruct(); // struct declarations can't be inline, due to HLSL syntax
	FEmitShaderCode* ShaderValue = EmitCodeInternal(Type, FormattedCode.ToView(), bInline, TArrayView<FEmitShaderCode*>());
	PreshaderValueMap.Add(Hash, ShaderValue);

	return ShaderValue;
}

FEmitShaderCode* FEmitContext::EmitConstantZero(const Shader::FType& Type)
{
	return EmitInlineCode(Type, TEXT("((%)0)"), Type.GetName());
}

namespace Private
{
void EmitShaderValue(FEmitContext& Context, FEmitShaderCode* ShaderValue)
{
	if (ShaderValue->Scope)
	{
		// Emit dependencies first
		for (FEmitShaderCode* Dependency : ShaderValue->Dependencies)
		{
			EmitShaderValue(Context, Dependency);
		}
		// Don't need a declaration for inline values
		if (!ShaderValue->IsInline())
		{
			ShaderValue->Scope->EmitDeclarationf(Context, TEXT("const %s %s = %s;"),
				ShaderValue->Type.GetName(),
				ShaderValue->Reference,
				ShaderValue->Value);
		}
		ShaderValue->Scope = nullptr; // Don't emit again
	}
}
}

void FEmitContext::Finalize()
{
	check(ScopeStack.Num() == 0);
	for (const auto& It : ShaderValueMap)
	{
		Private::EmitShaderValue(*this, It.Value);
	}

	ShaderValueMap.Reset();
	PreshaderValueMap.Reset();
	LocalPHIs.Reset();

	MaterialCompilationOutput->UniformExpressionSet.UniformPreshaderBufferSize = (UniformPreshaderOffset + 3u) / 4u;
}

void FScope::Reset()
{
	State = EScopeState::Uninitialized;
	Declarations = FCodeList();
	Statements = FCodeList();
}

FScope* FScope::FindSharedParent(FScope* Lhs, FScope* Rhs)
{
	FScope* Scope0 = Lhs;
	FScope* Scope1 = Rhs;
	if (Scope1)
	{
		while (Scope0 != Scope1)
		{
			if (Scope0->NestedLevel > Scope1->NestedLevel)
			{
				check(Scope0->ParentScope);
				Scope0 = Scope0->ParentScope;
			}
			else
			{
				check(Scope1->ParentScope);
				Scope1 = Scope1->ParentScope;
			}
		}
	}
	return Scope0;
}

void FExpressionLocalPHI::PrepareValue(FEmitContext& Context, const FRequestedType& RequestedType, FPrepareValueResult& OutResult) const
{
	check(NumValues <= MaxNumPreviousScopes);
	FExpression* ForwardExpression = Values[0];
	bool bForwardExpressionValid = true;

	// There are 2 cases we want to optimize here
	// 1) If the PHI node has the same value in all the previous scopes, we can avoid generating code for the previous scopes, and just use the value directly
	for (int32 i = 1; i < NumValues; ++i)
	{
		FExpression* ScopeExpression = Values[i];
		if (ScopeExpression != ForwardExpression)
		{
			ForwardExpression = nullptr;
			bForwardExpressionValid = false;
			break;
		}
	}

	if (bForwardExpressionValid)
	{
		check(ForwardExpression);
		return OutResult.SetForwardValue(Context, RequestedType, ForwardExpression);
	}

	// 2) PHI has different values in previous scopes, but possible some previous scopes may become dead due to constant folding
	// In this case, we check to see if the value is the same in all live scopes, and forward if possible
	for (int32 i = 0; i < NumValues; ++i)
	{
		// Ignore values in dead scopes
		if (PrepareScope(Context, Scopes[i]))
		{
			FExpression* ScopeExpression = Values[i];
			if (!ForwardExpression)
			{
				ForwardExpression = ScopeExpression;
				bForwardExpressionValid = true;
			}
			else if (ForwardExpression != ScopeExpression)
			{
				bForwardExpressionValid = false;
			}
		}
	}

	if (bForwardExpressionValid)
	{
		check(ForwardExpression);
		return OutResult.SetForwardValue(Context, RequestedType, ForwardExpression);
	}

	FPreparedType TypePerValue[MaxNumPreviousScopes];
	int32 NumValidTypes = 0;
	FPreparedType CurrentType;

	auto UpdateValueTypes = [&]()
	{
		for (int32 i = 0; i < NumValues; ++i)
		{
			if (TypePerValue[i].IsVoid() && PrepareScope(Context, Scopes[i]))
			{
				const FPreparedType& ValueType = PrepareExpressionValue(Context, Values[i], RequestedType);
				if (!ValueType.IsVoid())
				{
					TypePerValue[i] = ValueType;
					CurrentType = MergePreparedTypes(CurrentType, ValueType);
					if (CurrentType.IsVoid())
					{
						return Context.Errors.AddError(this, TEXT("Type mismatch"));
					}
					check(NumValidTypes < NumValues);
					NumValidTypes++;
				}
			}
		}
	};

	// First try to assign all the values we can
	UpdateValueTypes();

	// Assuming we have at least one value with a valid type, we use that to initialize our type
	CurrentType.SetEvaluation(EExpressionEvaluation::Shader); // TODO - No support for preshader flow control
	OutResult.SetType(Context, RequestedType, CurrentType);

	if (NumValidTypes < NumValues)
	{
		// Now try to assign remaining types that failed the first iteration 
		UpdateValueTypes();
		if (NumValidTypes < NumValues)
		{
			return Context.Errors.AddError(this, TEXT("Failed to compute all types for LocalPHI"));
		}
	}
}

void FExpressionLocalPHI::EmitValueShader(FEmitContext& Context, const FRequestedType& RequestedType, FEmitShaderValues& OutResult) const
{
	int32 LocalPHIIndex = Context.LocalPHIs.Find(this);
	if (LocalPHIIndex == INDEX_NONE)
	{
		// This is the first time we've emitted shader code for this PHI
		// First add it to the list, so if this is called recursively, this path will only be taken the first time
		LocalPHIIndex = Context.LocalPHIs.Add(this);

		// Find the outermost scope to declare our local variable
		FScope* DeclarationScope = Context.ScopeStack.Last();
		for (int32 i = 0; i < NumValues; ++i)
		{
			DeclarationScope = FScope::FindSharedParent(DeclarationScope, Scopes[i]);
			if (!DeclarationScope)
			{
				return Context.Errors.AddError(this, TEXT("Invalid LocalPHI"));
			}
		}

		const EExpressionDerivative Derivative = GetPreparedType().GetData().Derivative;
		const FRequestedType LocalType = GetRequestedType();
		const Shader::FType LocalDerivativeType = LocalType.GetType().GetDerivativeType();

		bool bNeedToAddDeclaration = true;
		for (int32 i = 0; i < NumValues; ++i)
		{
			FScope* ValueScope = Scopes[i];
			check(IsScopeLive(ValueScope));

			Context.ScopeStack.Add(ValueScope);
			const FEmitShaderValues ShaderValue = Values[i]->GetValueShader(Context, LocalType);
			Context.ScopeStack.Pop();

			if (ValueScope == DeclarationScope)
			{
				ValueScope->EmitDeclarationf(Context, TEXT("%s LocalPHI%d = %s;"),
					LocalType.GetName(),
					LocalPHIIndex,
					ShaderValue.Code->Reference);
				if (Derivative == EExpressionDerivative::Valid)
				{
					ValueScope->EmitDeclarationf(Context, TEXT("%s LocalPHI%dDdx = %s;"),
						LocalDerivativeType.GetName(),
						LocalPHIIndex,
						ShaderValue.CodeDdx->Reference);
					ValueScope->EmitDeclarationf(Context, TEXT("%s LocalPHI%dDdy = %s;"),
						LocalDerivativeType.GetName(),
						LocalPHIIndex,
						ShaderValue.CodeDdy->Reference);
				}

				bNeedToAddDeclaration = false;
			}
			else
			{
				ValueScope->EmitStatementf(Context, TEXT("LocalPHI%d = %s;"),
					LocalPHIIndex,
					ShaderValue.Code->Reference);
				if (Derivative == EExpressionDerivative::Valid)
				{
					ValueScope->EmitStatementf(Context, TEXT("LocalPHI%dDdx = %s;"),
						LocalPHIIndex,
						ShaderValue.CodeDdx->Reference);
					ValueScope->EmitStatementf(Context, TEXT("LocalPHI%dDdy = %s;"),
						LocalPHIIndex,
						ShaderValue.CodeDdy->Reference);
				}
			}
		}

		if (bNeedToAddDeclaration)
		{
			check(IsScopeLive(DeclarationScope));
			DeclarationScope->EmitDeclarationf(Context, TEXT("%s LocalPHI%d;"),
				LocalType.GetName(),
				LocalPHIIndex);
			if (Derivative == EExpressionDerivative::Valid)
			{
				DeclarationScope->EmitDeclarationf(Context, TEXT("%s LocalPHI%dDdx;"),
					LocalDerivativeType.GetName(),
					LocalPHIIndex);
				DeclarationScope->EmitDeclarationf(Context, TEXT("%s LocalPHI%dDdy;"),
					LocalDerivativeType.GetName(),
					LocalPHIIndex);
			}
		}
	}

	const Shader::FType LocalType = GetType();
	OutResult.Code = Context.EmitInlineCode(LocalType, TEXT("LocalPHI%"), LocalPHIIndex);
	if (GetDerivative(RequestedType) == EExpressionDerivative::Valid)
	{
		const Shader::FType LocalDerivativeType = LocalType.GetDerivativeType();
		OutResult.CodeDdx = Context.EmitInlineCode(LocalDerivativeType, TEXT("LocalPHI%Ddx"), LocalPHIIndex);
		OutResult.CodeDdy = Context.EmitInlineCode(LocalDerivativeType, TEXT("LocalPHI%Ddy"), LocalPHIIndex);
	}
}

void FStatement::Reset()
{
	bEmitShader = false;
}

void FExpression::Reset()
{
	CurrentRequestedType.Reset();
	PrepareValueResult = FPrepareValueResult();
}

const FPreparedType& PrepareExpressionValue(FEmitContext& Context, FExpression* InExpression, const FRequestedType& RequestedType)
{
	static FPreparedType VoidType;
	if (!InExpression)
	{
		return VoidType;
	}

	if (InExpression->bReentryFlag)
	{
		// Valid for this to be called reentrantly
		// Code should ensure that the type is set before the reentrant call, otherwise type will not be valid here
		// LocalPHI nodes rely on this to break loops
		return InExpression->PrepareValueResult.GetPreparedType();
	}

	bool bNeedToUpdateType = false;
	if (InExpression->CurrentRequestedType.RequestedComponents.Num() == 0)
	{
		InExpression->CurrentRequestedType = RequestedType;
		bNeedToUpdateType = !RequestedType.IsVoid();
	}
	else if (InExpression->CurrentRequestedType.GetStructType() != RequestedType.GetStructType())
	{
		Context.Errors.AddError(InExpression, TEXT("Type mismatch"));
		return VoidType;
	}
	else
	{
		const int32 NumComponents = RequestedType.GetNumComponents();
		InExpression->CurrentRequestedType.RequestedComponents.PadToNum(NumComponents, false);
		for (int32 Index = 0; Index < NumComponents; ++Index)
		{
			const EComponentRequest PrevRequest = InExpression->CurrentRequestedType.GetComponentRequest(Index);
			const EComponentRequest Request = RequestedType.GetComponentRequest(Index);
			if ((uint32)Request > (uint32)PrevRequest)
			{
				InExpression->CurrentRequestedType.SetComponentRequest(Index, Request);
				bNeedToUpdateType = true;
			}
		}
	}

	if (bNeedToUpdateType)
	{
		check(!InExpression->CurrentRequestedType.IsVoid());

		InExpression->bReentryFlag = true;
		InExpression->PrepareValue(Context, InExpression->CurrentRequestedType, InExpression->PrepareValueResult);
		InExpression->bReentryFlag = false;

		if (InExpression->PrepareValueResult.GetPreparedType().IsVoid())
		{
			// If we failed to assign a valid type, reset the requested type as well
			// This ensures we'll try to compute a type again the next time we're called
			InExpression->CurrentRequestedType.Reset();
		}
	}

	return InExpression->PrepareValueResult.GetPreparedType();
}

FRequestedType::FRequestedType(int32 NumComponents, EComponentRequest DefaultRequest)
{
	RequestedComponents.Init(IsRequested(DefaultRequest), NumComponents);
	RequestedComponentDerivatives.Init(IsDerivativeRequested(DefaultRequest), NumComponents);
}

FRequestedType::FRequestedType(const Shader::FType& InType, EComponentRequest DefaultRequest)
{
	int32 NumComponents = 0;
	if (InType.IsStruct())
	{
		StructType = InType.StructType;
		NumComponents = InType.StructType->ComponentTypes.Num();
	}
	else
	{
		const Shader::FValueTypeDescription TypeDesc = Shader::GetValueTypeDescription(InType);
		ValueComponentType = TypeDesc.ComponentType;
		NumComponents = TypeDesc.NumComponents;
	}

	RequestedComponents.Init(IsRequested(DefaultRequest), NumComponents);
	RequestedComponentDerivatives.Init(IsDerivativeRequested(DefaultRequest), NumComponents);
}

FRequestedType::FRequestedType(const Shader::EValueType& InType, EComponentRequest DefaultRequest)
{
	const Shader::FValueTypeDescription TypeDesc = Shader::GetValueTypeDescription(InType);
	ValueComponentType = TypeDesc.ComponentType;
	RequestedComponents.Init(IsRequested(DefaultRequest), TypeDesc.NumComponents);
	RequestedComponentDerivatives.Init(IsDerivativeRequested(DefaultRequest), TypeDesc.NumComponents);
}

const Shader::FType FRequestedType::GetType() const
{
	if (IsStruct())
	{
		return StructType;
	}
	return Shader::MakeValueType(ValueComponentType, GetNumComponents());
}

int32 FRequestedType::GetNumComponents() const
{
	if (StructType)
	{
		return StructType->ComponentTypes.Num();
	}
	else
	{
		const int32 MaxComponentIndex = RequestedComponents.FindLast(true);
		if (MaxComponentIndex != INDEX_NONE)
		{
			return MaxComponentIndex + 1;
		}
	}
	return 0;
}

EComponentRequest FRequestedType::GetComponentRequest(int32 Index) const
{
	if (RequestedComponents.IsValidIndex(Index))
	{
		if (RequestedComponentDerivatives.IsValidIndex(Index) && RequestedComponentDerivatives[Index])
		{
			return EComponentRequest::RequestedWithDerivative;
		}
		if (RequestedComponents[Index])
		{
			return EComponentRequest::Requested;
		}
	}
	return EComponentRequest::None;
}

void FRequestedType::SetComponentRequest(int32 Index, EComponentRequest Request)
{
	const bool bRequested = IsRequested(Request);
	const bool bRequestedDerivative = IsDerivativeRequested(Request);
	if (bRequested)
	{
		RequestedComponents.PadToNum(Index + 1, false);
	}
	if (bRequestedDerivative)
	{
		RequestedComponentDerivatives.PadToNum(Index + 1, false);
	}

	if (RequestedComponents.IsValidIndex(Index))
	{
		RequestedComponents[Index] = bRequested;
	}
	if (RequestedComponentDerivatives.IsValidIndex(Index))
	{
		RequestedComponentDerivatives[Index] = bRequestedDerivative;
	}
}

void FRequestedType::SetFieldRequested(const Shader::FStructField* Field, EComponentRequest Request)
{
	const int32 NumComponents = Field->GetNumComponents();
	for (int32 Index = 0; Index < NumComponents; ++Index)
	{
		SetComponentRequest(Field->ComponentIndex + Index, Request);
	}
}

void FRequestedType::SetField(const Shader::FStructField* Field, const FRequestedType& InRequest)
{
	const int32 NumComponents = Field->GetNumComponents();
	for (int32 Index = 0; Index < NumComponents; ++Index)
	{
		SetComponentRequest(Field->ComponentIndex + Index, InRequest.GetComponentRequest(Index));
	}
}

FRequestedType FRequestedType::GetField(const Shader::FStructField* Field) const
{
	FRequestedType Result(Field->Type);
	const int32 NumComponents = Field->GetNumComponents();
	for (int32 Index = 0; Index < NumComponents; ++Index)
	{
		Result.SetComponentRequest(Index, GetComponentRequest(Field->ComponentIndex + Index));
	}
	return Result;
}

FPreparedType::FPreparedType(const Shader::FType& InType)
{
	if (InType.IsStruct())
	{
		StructType = InType.StructType;
	}
	else
	{
		ValueComponentType = Shader::GetValueTypeDescription(InType).ComponentType;
	}
}

int32 FPreparedType::GetNumComponents() const
{
	if (StructType)
	{
		return StructType->ComponentTypes.Num();
	}
	else if (ValueComponentType != Shader::EValueComponentType::Void)
	{
		const int32 MaxComponentIndex = PreparedComponents.FindLastByPredicate([](const FPreparedData& InData) { return InData.IsValid(); });
		if (MaxComponentIndex != INDEX_NONE)
		{
			return MaxComponentIndex + 1;
		}
	}
	return 0;
}

FRequestedType MakeRequestedType(Shader::EValueComponentType ComponentType, const FRequestedType& RequestedComponents)
{
	check(!RequestedComponents.IsStruct());
	FRequestedType Result;
	Result.ValueComponentType = ComponentType;
	Result.RequestedComponents = RequestedComponents.RequestedComponents;
	Result.RequestedComponentDerivatives = RequestedComponents.RequestedComponentDerivatives;
	return Result;
}

bool FPreparedType::IsVoid() const
{
	return GetNumComponents() == 0;
}

Shader::FType FPreparedType::GetType() const
{
	if (IsStruct())
	{
		return StructType;
	}
	return Shader::MakeValueType(ValueComponentType, GetNumComponents());
}

FRequestedType FPreparedType::GetRequestedType() const
{
	const int32 NumComponents = GetNumComponents();
	FRequestedType Result;
	if (NumComponents > 0)
	{
		if (StructType)
		{
			Result.StructType = StructType;
		}
		else
		{
			Result.ValueComponentType = ValueComponentType;
		}
		for (int32 Index = 0; Index < NumComponents; ++Index)
		{
			Result.SetComponentRequest(Index, GetComponentData(Index).GetRequest());
		}
	}
	return Result;
}

FPreparedData FPreparedType::GetData() const
{
	FPreparedData Result;
	for (int32 Index = 0; Index < PreparedComponents.Num(); ++Index)
	{
		const FPreparedData& Component = PreparedComponents[Index];
		if (Component.IsValid())
		{
			Result = CombinePreparedData(Result, Component);
		}
	}
	return Result;
}

FPreparedData FPreparedType::GetData(const FRequestedType& RequestedType) const
{
	FPreparedData Result;
	for (int32 Index = 0; Index < PreparedComponents.Num(); ++Index)
	{
		const FPreparedData& Component = PreparedComponents[Index];
		if (Component.IsValid())
		{
			const EComponentRequest ComponentRequest = RequestedType.GetComponentRequest(Index);
			if (IsRequested(ComponentRequest))
			{
				Result.Evaluation = CombineEvaluations(Result.Evaluation, Component.Evaluation);
				if (IsDerivativeRequested(ComponentRequest))
				{
					Result.Derivative = CombineDerivatives(Result.Derivative, Component.Derivative);
				}
			}
		}
	}
	return Result;
}

FPreparedData FPreparedType::GetFieldData(int32 ComponentIndex, int32 NumComponents) const
{
	FPreparedData Result;
	for (int32 Index = 0; Index < NumComponents; ++Index)
	{
		Result = CombinePreparedData(Result, GetComponentData(ComponentIndex + Index));
	}
	return Result;
}

FPreparedData FPreparedType::GetComponentData(int32 Index) const
{
	FPreparedData Result;
	if (PreparedComponents.IsValidIndex(Index))
	{
		Result = PreparedComponents[Index];
	}
	return Result;
}

void FPreparedType::SetComponentData(int32 Index, const FPreparedData& Data)
{
	if (Data.IsValid() && Index >= PreparedComponents.Num())
	{
		PreparedComponents.AddDefaulted(Index + 1 - PreparedComponents.Num());
	}
	if (PreparedComponents.IsValidIndex(Index))
	{
		PreparedComponents[Index] = Data;
	}
}

void FPreparedType::MergeComponentData(int32 Index, EComponentRequest Request, const FPreparedData& Data)
{
	if (Request != EComponentRequest::None)
	{
		if (Data.IsValid() && Index >= PreparedComponents.Num())
		{
			PreparedComponents.AddDefaulted(Index + 1 - PreparedComponents.Num());
		}
		if (PreparedComponents.IsValidIndex(Index))
		{
			FPreparedData& Component = PreparedComponents[Index];
			Component.Evaluation = CombineEvaluations(Component.Evaluation, Data.Evaluation);
			if (Request == EComponentRequest::RequestedWithDerivative)
			{
				Component.Derivative = CombineDerivatives(Component.Derivative, Data.Derivative);
			}
		}
	}
}

void FPreparedType::SetEvaluation(EExpressionEvaluation Evaluation)
{
	for (int32 Index = 0; Index < PreparedComponents.Num(); ++Index)
	{
		if (PreparedComponents[Index].IsValid())
		{
			PreparedComponents[Index].Evaluation = Evaluation;
		}
	}
}

void FPreparedType::SetField(const Shader::FStructField* Field, const FPreparedType& FieldType)
{
	for (int32 Index = 0; Index < Field->GetNumComponents(); ++Index)
	{
		SetComponentData(Field->ComponentIndex + Index, FieldType.GetComponentData(Index));
	}
}

FPreparedType FPreparedType::GetFieldType(const Shader::FStructField* Field) const
{
	FPreparedType Result(Field->Type);
	for (int32 Index = 0; Index < Field->GetNumComponents(); ++Index)
	{
		Result.SetComponentData(Index, GetComponentData(Field->ComponentIndex + Index));
	}
	return Result;
}

FPreparedType MergePreparedTypes(const FPreparedType& Lhs, const FPreparedType& Rhs)
{
	// If one type is not initialized yet, just use the other type
	if (!Lhs.IsInitialized())
	{
		return Rhs;
	}
	else if (!Rhs.IsInitialized())
	{
		return Lhs;
	}

	int32 NumComponents = 0;
	FPreparedType Result;
	if (Lhs.IsStruct() || Rhs.IsStruct())
	{
		if (Lhs.StructType != Rhs.StructType)
		{
			// Mismatched structs
			return Result;
		}
		Result.StructType = Lhs.StructType;
		NumComponents = Result.StructType->ComponentTypes.Num();
	}
	else
	{
		Result.ValueComponentType = Shader::CombineComponentTypes(Lhs.ValueComponentType, Rhs.ValueComponentType);
		NumComponents = FMath::Max(Lhs.GetNumComponents(), Rhs.GetNumComponents());
	}

	for (int32 Index = 0; Index < NumComponents; ++Index)
	{
		const FPreparedData LhsData = Lhs.GetComponentData(Index);
		const FPreparedData RhsData = Rhs.GetComponentData(Index);
		Result.SetComponentData(Index, CombinePreparedData(LhsData, RhsData));
	}

	return Result;
}

bool FPrepareValueResult::TryMergePreparedType(FEmitContext& Context, const Shader::FStructType* StructType, Shader::EValueComponentType ComponentType)
{
	// If we previously had a forwarded value set, reset that and start over
	if (ForwardValue || !PreparedType.IsInitialized())
	{
		PreparedType.PreparedComponents.Reset();
		PreparedType.ValueComponentType = ComponentType;
		PreparedType.StructType = StructType;
		ForwardValue = nullptr;
		return true;
	}

	if (StructType)
	{
		check(ComponentType == Shader::EValueComponentType::Void);
		if (StructType != PreparedType.StructType)
		{
			Context.Errors.AddError(nullptr, TEXT("Invalid type"));
			return false;
		}
	}
	else
	{
		check(ComponentType != Shader::EValueComponentType::Void);
		PreparedType.ValueComponentType = Shader::CombineComponentTypes(PreparedType.ValueComponentType, ComponentType);
	}

	return true;
}

void FPrepareValueResult::SetType(FEmitContext& Context, const FRequestedType& RequestedType, const FPreparedData& Data, const Shader::FType& Type)
{
	if (TryMergePreparedType(Context, Type.StructType, Shader::GetValueTypeDescription(Type.ValueType).ComponentType))
	{
		if (Data.IsValid())
		{
			const int32 NumComponents = Type.GetNumComponents();
			for (int32 Index = 0; Index < NumComponents; ++Index)
			{
				const EComponentRequest ComponentRequest = RequestedType.GetComponentRequest(Index);
				PreparedType.MergeComponentData(Index, ComponentRequest, Data);
			}
		}
	}
}

void FPrepareValueResult::SetType(FEmitContext& Context, const FRequestedType& RequestedType, const FPreparedType& Type)
{
	if (TryMergePreparedType(Context, Type.StructType, Type.ValueComponentType))
	{
		const int32 NumComponents = RequestedType.GetNumComponents();
		for (int32 Index = 0; Index < NumComponents; ++Index)
		{
			const EComponentRequest ComponentRequest = RequestedType.GetComponentRequest(Index);
			PreparedType.MergeComponentData(Index, ComponentRequest, Type.GetComponentData(Index));
		}
	}
}

void FPrepareValueResult::SetForwardValue(FEmitContext& Context, const FRequestedType& RequestedType, FExpression* InForwardValue)
{
	check(InForwardValue);
	if (InForwardValue != ForwardValue)
	{
		PreparedType = PrepareExpressionValue(Context, InForwardValue, RequestedType);
		ForwardValue = InForwardValue;
	}
}

void FExpression::EmitValueShader(FEmitContext& Context, const FRequestedType& RequestedType, FEmitShaderValues& OutResult) const
{
	check(false);
}

void FExpression::EmitValuePreshader(FEmitContext& Context, const FRequestedType& RequestedType, Shader::FPreshaderData& OutPreshader) const
{
	check(false);
}

FEmitShaderValues FExpression::GetValueShader(FEmitContext& Context, const FRequestedType& RequestedType, const Shader::FType& ResultType)
{
	if (PrepareValueResult.ForwardValue)
	{
		return PrepareValueResult.ForwardValue->GetValueShader(Context, RequestedType, ResultType);
	}

	const FPreparedData Data = PrepareValueResult.PreparedType.GetData(RequestedType);
	check(Data.IsValid());

	FEmitShaderValues Result;
	if (Data.Evaluation == EExpressionEvaluation::Constant || Data.Evaluation == EExpressionEvaluation::Preshader)
	{
		Result.Code = Context.EmitPreshaderOrConstant(RequestedType, this);
		check(Data.Derivative != EExpressionDerivative::Valid); // If a constant has a valid derivative, it should be 'Zero'
		check(!Result.HasDerivatives());
	}
	else
	{
		check(Data.Evaluation == EExpressionEvaluation::Shader);
		EmitValueShader(Context, RequestedType, Result);
		if (Result.HasDerivatives())
		{
			checkf(Data.Derivative == EExpressionDerivative::Valid, TEXT("Expression emitted derivatives, but didn't request them during PrepareValue"));
		}
		else
		{
			checkf(Data.Derivative != EExpressionDerivative::Valid, TEXT("Expression requested derivatives during PrepareValue, but didn't emit them"));
		}
	}

	Result = Context.EmitCast(Result, ResultType);
	if (Data.Derivative == EExpressionDerivative::Zero)
	{
		const Shader::FType DerivativeResultType = ResultType.GetDerivativeType();
		check(!DerivativeResultType.IsVoid());
		check(!Result.HasDerivatives());
		Result.CodeDdx = Context.EmitConstantZero(DerivativeResultType);
		Result.CodeDdy = Result.CodeDdx;
	}

	return Result;
}

FEmitShaderValues FExpression::GetValueShader(FEmitContext& Context, const FRequestedType& RequestedType)
{
	return GetValueShader(Context, RequestedType, RequestedType.GetType());
}

FEmitShaderValues FExpression::GetValueShader(FEmitContext& Context)
{
	return GetValueShader(Context, GetRequestedType());
}

void FExpression::GetValuePreshader(FEmitContext& Context, const FRequestedType& RequestedType, Shader::FPreshaderData& OutPreshader)
{
	if (PrepareValueResult.ForwardValue)
	{
		return PrepareValueResult.ForwardValue->GetValuePreshader(Context, RequestedType, OutPreshader);
	}

	check(!bReentryFlag);
	const EExpressionEvaluation Evaluation = PrepareValueResult.PreparedType.GetData(RequestedType).Evaluation;
	if (Evaluation == EExpressionEvaluation::Preshader)
	{
		bReentryFlag = true;
		EmitValuePreshader(Context, RequestedType, OutPreshader);
		bReentryFlag = false;
	}
	else
	{
		check(Evaluation == EExpressionEvaluation::Constant);
		const Shader::FValue ConstantValue = GetValueConstant(Context, RequestedType);
		OutPreshader.WriteOpcode(Shader::EPreshaderOpcode::Constant).Write(ConstantValue);
	}
}

Shader::FValue FExpression::GetValueConstant(FEmitContext& Context, const FRequestedType& RequestedType)
{
	if (PrepareValueResult.ForwardValue)
	{
		return PrepareValueResult.ForwardValue->GetValueConstant(Context, RequestedType);
	}

	check(!bReentryFlag);
	check(PrepareValueResult.PreparedType.GetData(RequestedType).Evaluation == EExpressionEvaluation::Constant);
	
	Shader::FPreshaderData ConstantPreshader;
	bReentryFlag = true;
	EmitValuePreshader(Context, RequestedType, ConstantPreshader);
	bReentryFlag = false;

	// Evaluate the constant preshader and store its value
	Shader::FPreshaderStack Stack;
	const Shader::FPreshaderValue PreshaderValue = ConstantPreshader.EvaluateConstant(*Context.Material, Stack);
	Shader::FValue Result = PreshaderValue.AsShaderValue(Context.TypeRegistry);

	const Shader::FType RequestedConstantType = RequestedType.GetType();
	if (Result.Type.IsNumeric() && RequestedConstantType.IsNumeric())
	{
		Result = Shader::Cast(Result, RequestedConstantType.ValueType);
	}

	check(Result.Type == RequestedConstantType);
	return Result;
}

bool FScope::HasParentScope(const FScope& InParentScope) const
{
	const FScope* CurrentScope = this;
	while (CurrentScope)
	{
		if (CurrentScope == &InParentScope)
		{
			return true;
		}
		CurrentScope = CurrentScope->ParentScope;
	}
	return false;
}

void FScope::AddPreviousScope(FScope& Scope)
{
	check(NumPreviousScopes < MaxNumPreviousScopes);
	PreviousScope[NumPreviousScopes++] = &Scope;
}

void FScope::InternalEmitCode(FEmitContext& Context, FCodeList& List, ENextScopeFormat ScopeFormat, FScope* Scope, const TCHAR* String, int32 Length)
{
	if (Scope && Scope->ContainedStatement && !Scope->ContainedStatement->bEmitShader)
	{
		Scope->ContainedStatement->bEmitShader = true;
		Context.ScopeStack.Add(Scope);
		Scope->ContainedStatement->EmitShader(Context);
		Context.ScopeStack.Pop();
	}

	const int32 SizeofString = sizeof(TCHAR) * Length;
	void* Memory = Context.Allocator->Alloc(sizeof(FCodeEntry) + SizeofString, alignof(FCodeEntry));
	FCodeEntry* CodeEntry = new(Memory) FCodeEntry();
	FMemory::Memcpy(CodeEntry->String, String, SizeofString);
	CodeEntry->String[Length] = 0;
	CodeEntry->Length = Length;
	CodeEntry->Scope = Scope;
	CodeEntry->ScopeFormat = ScopeFormat;
	CodeEntry->Next = nullptr;

	if (!List.First)
	{
		List.First = CodeEntry;
		List.Last = CodeEntry;
	}
	else
	{
		List.Last->Next = CodeEntry;
		List.Last = CodeEntry;
	}
	List.Num++;
}

bool PrepareScope(FEmitContext& Context, FScope* InScope)
{
	if (InScope && InScope->State == EScopeState::Uninitialized)
	{
		if (!InScope->ParentScope || PrepareScope(Context, InScope->ParentScope))
		{
			if (InScope->OwnerStatement)
			{
				InScope->OwnerStatement->Prepare(Context);
			}
			else
			{
				InScope->State = EScopeState::Live;
			}
		}
		else
		{
			InScope->State = EScopeState::Dead;
		}
	}

	return InScope && InScope->State != EScopeState::Dead;
}

FTree* FTree::Create(FMemStackBase& Allocator)
{
	FTree* Tree = new(Allocator) FTree();
	Tree->Allocator = &Allocator;
	Tree->RootScope = Tree->NewNode<FScope>();
	return Tree;
}

void FTree::Destroy(FTree* Tree)
{
	if (Tree)
	{
		FNode* Node = Tree->Nodes;
		while (Node)
		{
			FNode* Next = Node->NextNode;
			Node->~FNode();
			Node = Next;
		}
		Tree->~FTree();
		FMemory::Memzero(*Tree);
	}
}

//
static void WriteIndent(int32 IndentLevel, FStringBuilderBase& InOutString)
{
	const int32 Offset = InOutString.AddUninitialized(IndentLevel);
	TCHAR* Result = InOutString.GetData() + Offset;
	for (int32 i = 0; i < IndentLevel; ++i)
	{
		*Result++ = TCHAR('\t');
	}
}

void FScope::MarkLive()
{
	if (State == EScopeState::Uninitialized)
	{
		State = EScopeState::Live;
	}
}

void FScope::MarkLiveRecursive()
{
	return MarkLive();

	FScope* Scope = this;
	while (Scope && Scope->State == EScopeState::Uninitialized)
	{
		Scope->State = EScopeState::Live;
		Scope = Scope->ParentScope;
	}
}

void FScope::MarkDead()
{
	// TODO - mark child scopes as dead as well
	State = EScopeState::Dead;
}

void FScope::WriteHLSL(int32 Indent, FStringBuilderBase& OutString) const
{
	{
		const FCodeEntry* CodeDeclaration = Declarations.First;
		while (CodeDeclaration)
		{
			check(!CodeDeclaration->Scope);
			WriteIndent(Indent, OutString);
			OutString.Append(CodeDeclaration->String, CodeDeclaration->Length);
			OutString.Append(TEXT('\n'));
			CodeDeclaration = CodeDeclaration->Next;
		}
	}

	{
		const FCodeEntry* CodeStatement = Statements.First;
		while (CodeStatement)
		{
			if (CodeStatement->Length > 0)
			{
				WriteIndent(Indent, OutString);
				OutString.Append(CodeStatement->String, CodeStatement->Length);
				OutString.Append(TEXT('\n'));
			}
			if (CodeStatement->Scope)
			{
				int32 NextIndent = Indent;
				bool bNeedToCloseScope = false;
				if (CodeStatement->ScopeFormat == ENextScopeFormat::Scoped)
				{
					WriteIndent(Indent, OutString);
					OutString.Append(TEXT("{\n"));
					NextIndent++;
					bNeedToCloseScope = true;
				}

				CodeStatement->Scope->WriteHLSL(NextIndent, OutString);
				if (bNeedToCloseScope)
				{
					WriteIndent(Indent, OutString);
					OutString.Append(TEXT("}\n"));
				}
			}
			CodeStatement = CodeStatement->Next;
		}
	}
}

void FTree::ResetNodes()
{
	FNode* Node = Nodes;
	while (Node)
	{
		FNode* Next = Node->NextNode;
		Node->Reset();
		Node = Next;
	}
}

bool FTree::EmitShader(FEmitContext& Context, FStringBuilderBase& OutCode) const
{
	if (RootScope->ContainedStatement)
	{
		RootScope->ContainedStatement->bEmitShader = true;
		Context.ScopeStack.Add(RootScope);
		RootScope->ContainedStatement->EmitShader(Context);
		Context.ScopeStack.Pop(false);

		if (Context.Errors.Num() > 0)
		{
			return false;
		}
	}

	Context.Finalize();
	RootScope->WriteHLSL(1, OutCode);
	return Context.Errors.Num() == 0;
}

void FTree::RegisterExpression(FExpression* Expression)
{
}

void FTree::RegisterStatement(FScope& Scope, FStatement* Statement)
{
	check(!Scope.ContainedStatement)
	check(!Statement->ParentScope);
	Statement->ParentScope = &Scope;
	Scope.ContainedStatement = Statement;
}

FScope* FTree::NewScope(FScope& Scope)
{
	FScope* NewScope = NewNode<FScope>();
	NewScope->ParentScope = &Scope;
	NewScope->NestedLevel = Scope.NestedLevel + 1;
	NewScope->NumPreviousScopes = 0;
	return NewScope;
}

FScope* FTree::NewOwnedScope(FStatement& Owner)
{
	FScope* NewScope = NewNode<FScope>();
	NewScope->OwnerStatement = &Owner;
	NewScope->ParentScope = Owner.ParentScope;
	NewScope->NestedLevel = NewScope->ParentScope->NestedLevel + 1;
	NewScope->NumPreviousScopes = 0;
	return NewScope;
}

FTextureParameterDeclaration* FTree::NewTextureParameterDeclaration(const FName& Name, const FTextureDescription& DefaultValue)
{
	FTextureParameterDeclaration* Declaration = NewNode<FTextureParameterDeclaration>(Name, DefaultValue);
	return Declaration;
}

} // namespace HLSLTree
} // namespace UE