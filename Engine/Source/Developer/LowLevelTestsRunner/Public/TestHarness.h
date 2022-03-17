// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#ifndef UE5_ENABLE_TESTHARNESS_ENGINE_SUPPORT
#define UE5_ENABLE_TESTHARNESS_ENGINE_SUPPORT 1
#endif

#if UE5_ENABLE_TESTHARNESS_ENGINE_SUPPORT
#include "CoreTypes.h"
#include "Containers/ContainersFwd.h"
#include "Containers/StringFwd.h"

#if PLATFORM_MAC
// Fix type redefinition error of FVector
#include "HAL/PlatformMath.h"
#endif // PLATFORM_MAC

#include <ostream>

std::ostream& operator<<(std::ostream& Stream, const TCHAR* Value);
std::ostream& operator<<(std::ostream& Stream, const FString& Value);
std::ostream& operator<<(std::ostream& Stream, const FAnsiStringView& Value);
std::ostream& operator<<(std::ostream& Stream, const FWideStringView& Value);
std::ostream& operator<<(std::ostream& Stream, const FUtf8StringView& Value);
std::ostream& operator<<(std::ostream& Stream, const FAnsiStringBuilderBase& Value);
std::ostream& operator<<(std::ostream& Stream, const FWideStringBuilderBase& Value);
std::ostream& operator<<(std::ostream& Stream, const FUtf8StringBuilderBase& Value);

enum class ESPMode : uint8;
template <class ObjectType, ESPMode InMode> class TSharedRef;
template <class ObjectType, ESPMode InMode> class TSharedPtr;

template <typename ObjectType, ESPMode Mode>
std::ostream& operator<<(std::ostream& Stream, const TSharedRef<ObjectType, Mode>& Value)
{
	return Stream << &Value.Get();
}

template <typename ObjectType, ESPMode Mode>
std::ostream& operator<<(std::ostream& Stream, const TSharedPtr<ObjectType, Mode>& Value)
{
	return Stream << Value.Get();
}

template <typename KeyT, typename ValueT>
inline bool operator==(const TMap<KeyT, ValueT>& Left, const TMap<KeyT, ValueT>& Right)
{
	bool bIsEqual = Left.Num() == Right.Num();
	if (bIsEqual)
	{
		for (const auto& Pair : Left)
		{
			const ValueT* RightValue = Right.Find(Pair.Key);
			bIsEqual = bIsEqual && RightValue != nullptr && Pair.Value == *RightValue;
		}
	}
	return bIsEqual;
}

#endif // UE5_ENABLE_TESTHARNESS_ENGINE_SUPPORT

#if defined(THIRD_PARTY_INCLUDES_START) && defined(THIRD_PARTY_INCLUDES_END)
THIRD_PARTY_INCLUDES_START
#endif // defined(THIRD_PARTY_INCLUDES_START) && defined(THIRD_PARTY_INCLUDES_END)

#ifdef _MSC_VER
#pragma pack(push, 8)
#pragma warning(push)
#pragma warning(disable: 4005) // 'identifier': macro redefinition
#pragma warning(disable: 4582) // 'type': constructor is not implicitly called
#pragma warning(disable: 4583) // 'type': destructor is not implicitly called
#endif
#include "catch.hpp"
#ifdef _MSC_VER
#pragma warning(pop)
#pragma pack(pop)
#endif

#if defined(THIRD_PARTY_INCLUDES_START) && defined(THIRD_PARTY_INCLUDES_END)
THIRD_PARTY_INCLUDES_END
#endif // defined(THIRD_PARTY_INCLUDES_START) && defined(THIRD_PARTY_INCLUDES_END)

#if UE5_ENABLE_TESTHARNESS_ENGINE_SUPPORT

// Tell Catch how to print TTuple<KeyType, ValueType>
template <typename... Types> struct TTuple;

template <typename KeyType, typename ValueType>
struct Catch::StringMaker<TTuple<KeyType, ValueType>>
{
	static std::string convert(const TTuple<KeyType, ValueType>& Value)
	{
		return "{ " + StringMaker<KeyType>::convert(Value.Key) + " , " + StringMaker<ValueType>::convert(Value.Value) + " }";
	}
};

#endif // #if UE5_ENABLE_TESTHARNESS_ENGINE_SUPPORT
