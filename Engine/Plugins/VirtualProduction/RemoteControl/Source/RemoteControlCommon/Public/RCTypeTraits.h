﻿// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

#include "Math/NumericLimits.h"
#include "Math/UnrealMathUtility.h"
#include "Templates/IsArithmetic.h"
#include "Templates/UnrealTypeTraits.h"
#include "UObject/UnrealType.h"

namespace RemoteControlTypeTraits
{
	namespace Concepts
	{
		/** Concept to check if T has NumericLimits */
		struct CNumerical
		{
			template <typename T>
			auto Requires() -> decltype(
				TAnd<
					TIsSame<typename TNumericLimits<T>::NumericType, T>,
					TNot<TIsSame<T, bool>>>::Value);
		};

		/** Concept to check if PropertyType::TCppType matches ValueType */
		struct CPropertyValuePair
		{
			template <typename PropertyType, typename ValueType>
			auto Requires() -> decltype(TIsDerivedFrom<PropertyType, TProperty<ValueType, PropertyType>>::Value);
		};	
	}

	/** Catch-all for string-like property types */
	template <typename PropertyType, typename Enable = void>
	struct TIsStringLikeProperty
	{
		enum { Value = false };
	};

	template <typename PropertyType>
	struct TIsStringLikeProperty<
			PropertyType,
			typename TEnableIf<
				TOr<
					TIsSame<PropertyType, FStrProperty>,
					TIsSame<PropertyType, FNameProperty>,
					TIsSame<PropertyType, FTextProperty>>::Value>::Type>
	{
		enum
		{
			Value = true
		};
	};

	/** Ensures ValueType is a numeric type. */
	template <typename ValueType>
	using TNumericValueConstraint = TAnd<TModels<RemoteControlTypeTraits::Concepts::CNumerical, ValueType>>;
}

/** Various RemoteControl type traits */
template <typename ValueType, typename Enable = void>
struct TRemoteControlTypeTraits;

/** Various RemoteControl property type traits */
template <typename PropertyType, typename Enable = void>
struct TRemoteControlPropertyTypeTraits;

#pragma region Numeric Types

/** RemoteControlTypeTraits for integers */
template <typename ValueType>
struct TRemoteControlTypeTraits<ValueType,
	typename TEnableIf<
		TAnd<
			TIsArithmetic<ValueType>,
			TNot<TIsFloatingPoint<ValueType>>,
			TModels<RemoteControlTypeTraits::Concepts::CNumerical, ValueType>>::Value, void>::Type>
{
	using Type = ValueType;

	/** Is ValueType supported as a range (protocol input) value? */
	static constexpr bool IsSupportedRangeType() { return true; }

	/** Is ValueType supported as a mapping (property output) value? */
	static constexpr bool IsSupportedMappingType() { return true; }

	/** The default minimum value for newly created range (protocol, input). */
	static constexpr Type DefaultRangeValueMin() { return TNumericLimits<Type>::Min(); }

	/** The default maximum value for newly created range. */
	static constexpr Type DefaultRangeValueMax() { return TNumericLimits<Type>::Max(); }

	/** The default minimum value for newly created mapping. */
	static constexpr Type DefaultMappingValueMin() { return static_cast<Type>(0); }

	/** The default maximum value for newly created mapping. */
	static constexpr Type DefaultMappingValueMax() { return static_cast<Type>(1); }
};

/** RemoteControlTypeTraits for floats */
template <typename ValueType>
struct TRemoteControlTypeTraits<ValueType,
	typename TEnableIf<TIsFloatingPoint<ValueType>::Value, void>::Type>
{
	using Type = ValueType;

	/** Is ValueType supported as a range (protocol input) value? */
	static constexpr bool IsSupportedRangeType() { return true; }

	/** Is ValueType supported as a mapping (property output) value? */
	static constexpr bool IsSupportedMappingType() { return true; }

	static constexpr Type DefaultRangeValueMin() { return static_cast<Type>(0.0f); }

	static constexpr Type DefaultRangeValueMax() { return static_cast<Type>(1.0f); }

	static constexpr Type DefaultMappingValueMin() { return static_cast<Type>(0.0f); }

	static constexpr Type DefaultMappingValueMax() { return static_cast<Type>(1.0f); }
};

/**
 * RemoteControlPropertyTypeTraits for FNumericProperty
 * Currently all numeric types are supported so we can shortcut the above (rather than doing a series of CastField's)
 */
template <>
struct TRemoteControlPropertyTypeTraits<FNumericProperty>
{
	using Type = FNumericProperty;

	/** Is ValueType supported as a range (protocol input) value? */
	static constexpr bool IsSupportedRangeType() { return true; }

	/** Is ValueType supported as a mapping (property output) value? */
	static constexpr bool IsSupportedMappingType() { return true; }
};

/** RemoteControlPropertyTypeTraits for FEnumProperty */
template <>
struct TRemoteControlPropertyTypeTraits<FEnumProperty>
{
	using PropertyType = FEnumProperty;
	using ValueType = uint8;

	/** Is ValueType supported as a range (protocol input) value? */
	static constexpr bool IsSupportedRangeType() { return false; }

	/** Is ValueType supported as a mapping (property output) value? */
	static constexpr bool IsSupportedMappingType() { return true; }
};

#pragma endregion Numeric Types

/** RemoteControlTypeTraits for bool */
template <>
struct TRemoteControlTypeTraits<bool>
{
	using Type = bool;

	/** Is ValueType supported as a range (protocol input) value? */
	static constexpr bool IsSupportedRangeType() { return false; }

	/** Is ValueType supported as a mapping (property output) value? */
	static constexpr bool IsSupportedMappingType() { return true; }

	static constexpr Type DefaultRangeValueMin() { return false; }

	static constexpr Type DefaultRangeValueMax() { return true; }
	
	static constexpr Type DefaultMappingValueMin() { return false; }
	
	static constexpr Type DefaultMappingValueMax() { return true; }
};

/** RemoteControlPropertyTypeTraits for FBoolProperty */
template <>
struct TRemoteControlPropertyTypeTraits<FBoolProperty>
{
	using PropertyType = FBoolProperty;
	using ValueType = bool;

	/** Is ValueType supported as a range (protocol input) value? */
	static constexpr bool IsSupportedRangeType() { return TRemoteControlTypeTraits<bool>::IsSupportedRangeType(); }

	/** Is ValueType supported as a mapping (property output) value? */
	static constexpr bool IsSupportedMappingType() { return TRemoteControlTypeTraits<bool>::IsSupportedMappingType(); }

	/** The default minimum value for newly created range (protocol, input). */
	static constexpr ValueType DefaultRangeValueMin() { return TRemoteControlTypeTraits<bool>::DefaultRangeValueMin(); }

	/** The default maximum value for newly created range. */
	static constexpr ValueType DefaultRangeValueMax() { return TRemoteControlTypeTraits<bool>::DefaultRangeValueMax(); }

	/** The default minimum value for newly created mapping. */
	static constexpr ValueType DefaultMappingValueMin() { return TRemoteControlTypeTraits<bool>::DefaultMappingValueMin(); }

	/** The default maximum value for newly created mapping. */
	static constexpr ValueType DefaultMappingValueMax() { return TRemoteControlTypeTraits<bool>::DefaultMappingValueMax(); }
};

#pragma region String Types

/** RemoteControlTypeTraits for FString */
template <>
struct TRemoteControlTypeTraits<FString>
{
	using Type = FString;
	
	/** Is ValueType supported as a range (protocol input) value? */
	static constexpr bool IsSupportedRangeType() { return false; }

	/** Is ValueType supported as a mapping (property output) value? */
	static constexpr bool IsSupportedMappingType() { return true; }

	static Type DefaultRangeValueMin() { return {}; }

	static Type DefaultRangeValueMax() { return {}; }
	
	static Type DefaultMappingValueMin() { return {}; }
	
	static Type DefaultMappingValueMax() { return {}; }
};

/** RemoteControlPropertyTypeTraits for FStrProperty */
template <>
struct TRemoteControlPropertyTypeTraits<FStrProperty>
{
	using PropertyType = FStrProperty;
	using ValueType = FString;

	/** Is ValueType supported as a range (protocol input) value? */
	static constexpr bool IsSupportedRangeType() { return TRemoteControlTypeTraits<FString>::IsSupportedRangeType(); }

	/** Is ValueType supported as a mapping (property output) value? */
	static constexpr bool IsSupportedMappingType() { return TRemoteControlTypeTraits<FString>::IsSupportedMappingType(); }

	/** The default minimum value for newly created range (protocol, input). */
	static ValueType DefaultRangeValueMin() { return TRemoteControlTypeTraits<FString>::DefaultRangeValueMin(); }

	/** The default maximum value for newly created range. */
	static ValueType DefaultRangeValueMax() { return TRemoteControlTypeTraits<FString>::DefaultRangeValueMax(); }

	/** The default minimum value for newly created mapping. */
	static ValueType DefaultMappingValueMin() { return TRemoteControlTypeTraits<FString>::DefaultMappingValueMin(); }

	/** The default maximum value for newly created mapping. */
	static ValueType DefaultMappingValueMax() { return TRemoteControlTypeTraits<FString>::DefaultMappingValueMax(); }
};

/** RemoteControlTypeTraits for FName */
template <>
struct TRemoteControlTypeTraits<FName>
{
	using Type = FName;

	/** Is ValueType supported as a range (protocol input) value? */
	static constexpr bool IsSupportedRangeType() { return false; }

	/** Is ValueType supported as a mapping (property output) value? */
	static constexpr bool IsSupportedMappingType() { return true; }

	static Type DefaultRangeValueMin() { return {}; }

	static Type DefaultRangeValueMax() { return {}; }
	
	static Type DefaultMappingValueMin() { return {}; }
	
	static Type DefaultMappingValueMax() { return {}; }
};

/** RemoteControlPropertyTypeTraits for FNameProperty */
template <>
struct TRemoteControlPropertyTypeTraits<FNameProperty>
{
	using PropertyType = FNameProperty;
	using ValueType = FName;

	/** Is ValueType supported as a range (protocol input) value? */
	static constexpr bool IsSupportedRangeType() { return TRemoteControlTypeTraits<ValueType>::IsSupportedRangeType(); }

	/** Is ValueType supported as a mapping (property output) value? */
	static constexpr bool IsSupportedMappingType() { return TRemoteControlTypeTraits<ValueType>::IsSupportedMappingType(); }

	/** The default minimum value for newly created range (protocol, input). */
	static ValueType DefaultRangeValueMin() { return TRemoteControlTypeTraits<ValueType>::DefaultRangeValueMin(); }

	/** The default maximum value for newly created range. */
	static ValueType DefaultRangeValueMax() { return TRemoteControlTypeTraits<ValueType>::DefaultRangeValueMax(); }

	/** The default minimum value for newly created mapping. */
	static ValueType DefaultMappingValueMin() { return TRemoteControlTypeTraits<ValueType>::DefaultMappingValueMin(); }

	/** The default maximum value for newly created mapping. */
	static ValueType DefaultMappingValueMax() { return TRemoteControlTypeTraits<ValueType>::DefaultMappingValueMax(); }
};

/** RemoteControlTypeTraits for FText */
template <>
struct TRemoteControlTypeTraits<FText>
{
	using Type = FText;

	/** Is ValueType supported as a range (protocol input) value? */
	static constexpr bool IsSupportedRangeType() { return false; }

	/** Is ValueType supported as a mapping (property output) value? */
	static constexpr bool IsSupportedMappingType() { return true; }

	static Type DefaultRangeValueMin() { return {}; }

	static Type DefaultRangeValueMax() { return {}; }
	
	static Type DefaultMappingValueMin() { return {}; }
	
	static Type DefaultMappingValueMax() { return {}; }
};

/** RemoteControlPropertyTypeTraits for FTextProperty */
template <>
struct TRemoteControlPropertyTypeTraits<FTextProperty>
{
	using PropertyType = FTextProperty;
	using ValueType = FText;

	/** Is ValueType supported as a range (protocol input) value? */
	static constexpr bool IsSupportedRangeType() { return TRemoteControlTypeTraits<ValueType>::IsSupportedRangeType(); }

	/** Is ValueType supported as a mapping (property output) value? */
	static constexpr bool IsSupportedMappingType() { return TRemoteControlTypeTraits<ValueType>::IsSupportedMappingType(); }

	/** The default minimum value for newly created range (protocol, input). */
	static ValueType DefaultRangeValueMin() { return TRemoteControlTypeTraits<ValueType>::DefaultRangeValueMin(); }

	/** The default maximum value for newly created range. */
	static ValueType DefaultRangeValueMax() { return TRemoteControlTypeTraits<ValueType>::DefaultRangeValueMax(); }

	/** The default minimum value for newly created mapping. */
	static ValueType DefaultMappingValueMin() { return TRemoteControlTypeTraits<ValueType>::DefaultMappingValueMin(); }

	/** The default maximum value for newly created mapping. */
	static ValueType DefaultMappingValueMax() { return TRemoteControlTypeTraits<ValueType>::DefaultMappingValueMax(); }
};

#pragma endregion String Types

/** RemoteControlPropertyTypeTraits for FArrayProperty */
template <>
struct TRemoteControlPropertyTypeTraits<FArrayProperty>
{
	using PropertyType = FArrayProperty;

	/** Is ValueType supported as a range (protocol input) value? */
	static constexpr bool IsSupportedRangeType() { return false; }

	/** Is ValueType supported as a mapping (property output) value? */
	static constexpr bool IsSupportedMappingType() { return true; }
};

/** RemoteControlPropertyTypeTraits for FSetProperty */
template <>
struct TRemoteControlPropertyTypeTraits<FSetProperty>
{
	using PropertyType = FSetProperty;

	/** Is ValueType supported as a range (protocol input) value? */
	static constexpr bool IsSupportedRangeType() { return false; }

	/** Is ValueType supported as a mapping (property output) value? */
	static constexpr bool IsSupportedMappingType() { return true; }
};

/** RemoteControlPropertyTypeTraits for FMapProperty */
template <>
struct TRemoteControlPropertyTypeTraits<FMapProperty>
{
	using PropertyType = FMapProperty;

	/** Is ValueType supported as a range (protocol input) value? */
	static constexpr bool IsSupportedRangeType() { return false; }

	/** Is ValueType supported as a mapping (property output) value? */
	static constexpr bool IsSupportedMappingType() { return true; }
};

#pragma region Structs (Built-in)

/** RemoteControlPropertyTypeTraits for FStructProperty */
template <>
struct TRemoteControlPropertyTypeTraits<FStructProperty>
{
	using PropertyType = FNameProperty;

	/** Is ValueType supported as a range (protocol input) value? */
	static constexpr bool IsSupportedRangeType() { return false; }

	/** Is ValueType supported as a mapping (property output) value? */
	static constexpr bool IsSupportedMappingType() { return true; }
};

/** RemoteControlTypeTraits for FVector */
template <>
struct TRemoteControlTypeTraits<FVector>
{
	using Type = FVector;
	
	/** Is ValueType supported as a range (protocol input) value? */
	static constexpr bool IsSupportedRangeType() { return false; }

	/** Is ValueType supported as a mapping (property output) value? */
	static constexpr bool IsSupportedMappingType() { return true; }
	
	static Type DefaultMappingValueMin() { return Type::ZeroVector; }

	static Type DefaultMappingValueMax() { return Type::OneVector; }
};

/** RemoteControlTypeTraits for FVector2D */
template <>
struct TRemoteControlTypeTraits<FVector2D>
{
	using Type = FVector2D;
	
	/** Is ValueType supported as a range (protocol input) value? */
	static constexpr bool IsSupportedRangeType() { return false; }

	/** Is ValueType supported as a mapping (property output) value? */
	static constexpr bool IsSupportedMappingType() { return true; }

	static Type DefaultMappingValueMin() { return Type::ZeroVector; }
	
	static Type DefaultMappingValueMax() { return Type::UnitVector; }
};

/** RemoteControlTypeTraits for FVector4 */
template <>
struct TRemoteControlTypeTraits<FVector4>
{
	using Type = FVector4;
	
	/** Is ValueType supported as a range (protocol input) value? */
	static constexpr bool IsSupportedRangeType() { return false; }

	/** Is ValueType supported as a mapping (property output) value? */
	static constexpr bool IsSupportedMappingType() { return true; }
	
	static Type DefaultMappingValueMin() { return Type(EForceInit::ForceInitToZero); }

	static Type DefaultMappingValueMax() { return Type(1.0f, 1.0f, 1.0f, 1.0f); }
};

/** RemoteControlTypeTraits for FRotator */
template <>
struct TRemoteControlTypeTraits<FRotator>
{
	using Type = FRotator;

	/** Is ValueType supported as a range (protocol input) value? */
	static constexpr bool IsSupportedRangeType() { return false; }

	/** Is ValueType supported as a mapping (property output) value? */
	static constexpr bool IsSupportedMappingType() { return true; }
	
	static Type DefaultMappingValueMin() { return Type::ZeroRotator; }

	static Type DefaultMappingValueMax() { return Type(90.0f, 90.0f, 90.0f); }
};

/** RemoteControlTypeTraits for FQuat */
template <>
struct TRemoteControlTypeTraits<FQuat>
{
	using Type = FQuat;
	
	/** Is ValueType supported as a range (protocol input) value? */
	static constexpr bool IsSupportedRangeType() { return false; }

	/** Is ValueType supported as a mapping (property output) value? */
	static constexpr bool IsSupportedMappingType() { return true; }
	
	static Type DefaultMappingValueMin() { return Type(0.0f, 0.0f, 0.0f, 0.0f); }

	static Type DefaultMappingValueMax() { return Type::Identity; }
};

/** RemoteControlTypeTraits for FTransform */
template <>
struct TRemoteControlTypeTraits<FTransform>
{
	using Type = FTransform;
	
	/** Is ValueType supported as a range (protocol input) value? */
	static constexpr bool IsSupportedRangeType() { return false; }

	/** Is ValueType supported as a mapping (property output) value? */
	static constexpr bool IsSupportedMappingType() { return true; }
	
	static Type DefaultMappingValueMin()
	{
		return Type(
			TRemoteControlTypeTraits<FRotator>::DefaultMappingValueMin(),
			TRemoteControlTypeTraits<FVector>::DefaultMappingValueMin(),
			TRemoteControlTypeTraits<FVector>::DefaultMappingValueMax()); // scale is Max cause it shouldn't be zero
	}

	static Type DefaultMappingValueMax()
	{
		return Type(
			TRemoteControlTypeTraits<FRotator>::DefaultMappingValueMax(),
			TRemoteControlTypeTraits<FVector>::DefaultMappingValueMax(),
			TRemoteControlTypeTraits<FVector>::DefaultMappingValueMax()); // scale is Max cause it shouldn't be zero
	}		
};

/** RemoteControlTypeTraits for FIntPoint */
template <>
struct TRemoteControlTypeTraits<FIntPoint>
{
	using Type = FIntPoint;

	/** Is ValueType supported as a range (protocol input) value? */
	static constexpr bool IsSupportedRangeType() { return false; }

	/** Is ValueType supported as a mapping (property output) value? */
	static constexpr bool IsSupportedMappingType() { return true; }
	
	static Type DefaultMappingValueMin() { return Type::ZeroValue; }

	static Type DefaultMappingValueMax() { return Type(1, 1); }
};

/** RemoteControlTypeTraits for FIntVector */
template <>
struct TRemoteControlTypeTraits<FIntVector>
{
	using Type = FIntVector;

	/** Is ValueType supported as a range (protocol input) value? */
	static constexpr bool IsSupportedRangeType() { return false; }

	/** Is ValueType supported as a mapping (property output) value? */
	static constexpr bool IsSupportedMappingType() { return true; }
	
	static Type DefaultMappingValueMin() { return Type::ZeroValue; }
	
	static Type DefaultMappingValueMax() { return Type(1, 1, 1); }
};

/** RemoteControlTypeTraits for FBox */
template <>
struct TRemoteControlTypeTraits<FBox>
{
	using Type = FBox;
	
	/** Is ValueType supported as a range (protocol input) value? */
	static constexpr bool IsSupportedRangeType() { return false; }

	/** Is ValueType supported as a mapping (property output) value? */
	static constexpr bool IsSupportedMappingType() { return true; }
	
	static Type DefaultMappingValueMin()
	{
		return Type(
			TRemoteControlTypeTraits<FVector>::DefaultMappingValueMin(),
			TRemoteControlTypeTraits<FVector>::DefaultMappingValueMax());
	}

	static Type DefaultMappingValueMax()
	{
		return Type(
			TRemoteControlTypeTraits<FVector>::DefaultMappingValueMax(),
			TRemoteControlTypeTraits<FVector>::DefaultMappingValueMax() * 2.0f);
	}
};

/** RemoteControlTypeTraits for FBox2D */
template <>
struct TRemoteControlTypeTraits<FBox2D>
{
	using Type = FBox2D;
	
	/** Is ValueType supported as a range (protocol input) value? */
	static constexpr bool IsSupportedRangeType() { return false; }

	/** Is ValueType supported as a mapping (property output) value? */
	static constexpr bool IsSupportedMappingType() { return true; }
	
	static Type DefaultMappingValueMin()
	{
		return Type(
			TRemoteControlTypeTraits<FVector2D>::DefaultMappingValueMin(),
			TRemoteControlTypeTraits<FVector2D>::DefaultMappingValueMax());
	}

	static Type DefaultMappingValueMax()
	{
		return Type(
			TRemoteControlTypeTraits<FVector2D>::DefaultMappingValueMax(),
			TRemoteControlTypeTraits<FVector2D>::DefaultMappingValueMax() * 2.0f);
	}
};

/** RemoteControlTypeTraits for FBoxSphereBounds */
template <>
struct TRemoteControlTypeTraits<FBoxSphereBounds>
{
	using Type = FBoxSphereBounds;
	
	/** Is ValueType supported as a range (protocol input) value? */
	static constexpr bool IsSupportedRangeType() { return false; }

	/** Is ValueType supported as a mapping (property output) value? */
	static constexpr bool IsSupportedMappingType() { return true; }
	
	static Type DefaultMappingValueMin() { return Type(TRemoteControlTypeTraits<FBox>::DefaultMappingValueMin()); }

	static Type DefaultMappingValueMax() { return Type(TRemoteControlTypeTraits<FBox>::DefaultMappingValueMax()); }
};

/** RemoteControlTypeTraits for FColor */
template <>
struct TRemoteControlTypeTraits<FColor>
{
	using Type = FColor;
	
	/** Is ValueType supported as a range (protocol input) value? */
	static constexpr bool IsSupportedRangeType() { return false; }

	/** Is ValueType supported as a mapping (property output) value? */
	static constexpr bool IsSupportedMappingType() { return true; }
	
	static Type DefaultMappingValueMin()
	{
		return Type(TRemoteControlTypeTraits<uint8>::DefaultMappingValueMin(),
			TRemoteControlTypeTraits<uint8>::DefaultMappingValueMin(),
			TRemoteControlTypeTraits<uint8>::DefaultMappingValueMin());
	}
		
	static Type DefaultMappingValueMax()
	{
		return Type(TRemoteControlTypeTraits<uint8>::DefaultMappingValueMax(),
			TRemoteControlTypeTraits<uint8>::DefaultMappingValueMax(),
			TRemoteControlTypeTraits<uint8>::DefaultMappingValueMax());
	}
};

/** RemoteControlTypeTraits for FLinearColor */
template <>
struct TRemoteControlTypeTraits<FLinearColor>
{
	using Type = FLinearColor;
	
	/** Is ValueType supported as a range (protocol input) value? */
	static constexpr bool IsSupportedRangeType() { return false; }

	/** Is ValueType supported as a mapping (property output) value? */
	static constexpr bool IsSupportedMappingType() { return true; }
	
	static Type DefaultMappingValueMin()
	{
		return Type(TRemoteControlTypeTraits<float>::DefaultMappingValueMin(),
			TRemoteControlTypeTraits<float>::DefaultMappingValueMin(),
			TRemoteControlTypeTraits<float>::DefaultMappingValueMin());
	}

	static Type DefaultMappingValueMax()
	{
		return Type(TRemoteControlTypeTraits<float>::DefaultMappingValueMax(),
			TRemoteControlTypeTraits<float>::DefaultMappingValueMax(),
			TRemoteControlTypeTraits<float>::DefaultMappingValueMax());
	}
};

#pragma endregion Structs (Built-in)
