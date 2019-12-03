// Copyright 1998-2019 Epic Games, Inc. All Rights Reserved.

#include "HAL/UnrealMemory.h"
#include "Templates/Decay.h"
#include "Templates/UnrealTypeTraits.h"
#include "Traits/IntType.h"

namespace StringViewPrivate
{
	// These exported functions are implemented outside of the class to avoid the complexity
	// of correctly exporting functions using many toolchains from a type that uses the CRTP.

	template<class ViewType>
	CORE_API int32 Compare(const ViewType& Lhs, const ViewType& Rhs, ESearchCase::Type SearchCase);

	template<class ViewType>
	CORE_API bool FindChar(const ViewType& InView, typename ViewType::ElementType InChar, typename ViewType::SizeType& OutIndex);

	template<class ViewType>
	CORE_API bool FindLastChar(const ViewType& InView, typename ViewType::ElementType InChar, typename ViewType::SizeType& OutIndex);
}

template <typename CharType, typename ViewType>
inline const CharType& TStringViewImpl<CharType, ViewType>::operator[](SizeType Index) const
{
	checkf(Index >= 0 && Index < Size, TEXT("Index out of bounds on StringView: index %i on a view with a length of %i"), Index, Size);
	return DataPtr[Index];
}

template <typename CharType, typename ViewType>
inline typename TStringViewImpl<CharType, ViewType>::SizeType TStringViewImpl<CharType, ViewType>::CopyString(CharType* Dest, SizeType CharCount, SizeType Position) const
{
	const  SizeType CopyCount = FMath::Min(Size - Position, CharCount);
	FMemory::Memcpy(Dest, DataPtr + Position, CopyCount);
	return CopyCount;
}

template <typename CharType, typename ViewType>
inline ViewType TStringViewImpl<CharType, ViewType>::Left(SizeType CharCount) const
{
	return ViewType(DataPtr, FMath::Clamp(CharCount, 0, Size));
}

template <typename CharType, typename ViewType>
inline ViewType TStringViewImpl<CharType, ViewType>::LeftChop(SizeType CharCount) const
{
	return ViewType(DataPtr, FMath::Clamp(Size - CharCount, 0, Size));
}

template <typename CharType, typename ViewType>
inline ViewType TStringViewImpl<CharType, ViewType>::Right(SizeType CharCount) const
{
	const SizeType OutLen = FMath::Clamp(CharCount, 0, Size);
	return ViewType(DataPtr + Size - OutLen, OutLen);
}

template <typename CharType, typename ViewType>
inline ViewType TStringViewImpl<CharType, ViewType>::RightChop(SizeType CharCount) const
{
	const SizeType OutLen = FMath::Clamp(Size - CharCount, 0, Size);
	return ViewType(DataPtr + Size - OutLen, OutLen);
}

template <typename CharType, typename ViewType>
inline ViewType TStringViewImpl<CharType, ViewType>::Mid(SizeType Position, SizeType CharCount) const
{
	check(CharCount >= 0);
	using USizeType = TUnsignedIntType_T<sizeof(SizeType)>;
	Position = FMath::Clamp<USizeType>(Position, 0, Size);
	CharCount = FMath::Clamp<USizeType>(CharCount, 0, Size - Position);
	return ViewType(DataPtr + Position, CharCount);
}

template <typename CharType, typename ViewType>
inline bool TStringViewImpl<CharType, ViewType>::Equals(const ViewType& Other, ESearchCase::Type SearchCase) const
{
	return Size == Other.Size && Compare(Other, SearchCase) == 0;
}

template <typename CharType, typename ViewType>
inline int32 TStringViewImpl<CharType, ViewType>::Compare(const ViewType& Other, ESearchCase::Type SearchCase) const
{
	return StringViewPrivate::Compare(static_cast<const ViewType&>(*this), Other, SearchCase);
}

template <typename CharType, typename ViewType>
inline bool TStringViewImpl<CharType, ViewType>::StartsWith(const ViewType& Prefix, ESearchCase::Type SearchCase) const
{
	return Prefix.Equals(Left(Prefix.Len()), SearchCase);
}

template <typename CharType, typename ViewType>
inline bool TStringViewImpl<CharType, ViewType>::EndsWith(const ViewType& Suffix, ESearchCase::Type SearchCase) const
{
	return Suffix.Equals(Right(Suffix.Len()), SearchCase);
}

template <typename CharType, typename ViewType>
inline bool TStringViewImpl<CharType, ViewType>::FindChar(CharType InChar, SizeType& OutIndex) const
{
	return StringViewPrivate::FindChar(static_cast<const ViewType&>(*this), InChar, OutIndex);
}

template <typename CharType, typename ViewType>
inline bool TStringViewImpl<CharType, ViewType>::FindLastChar(CharType InChar, SizeType& OutIndex) const
{
	return StringViewPrivate::FindLastChar(static_cast<const ViewType&>(*this), InChar, OutIndex);
}

// Case-insensitive comparison operators

template <typename CharType, typename ViewType, typename RangeType,
	typename = typename TStringViewImpl<CharType, ViewType>::template TEnableIfCompatibleRangeType<RangeType>>
inline bool operator==(const TStringViewImpl<CharType, ViewType>& Lhs, RangeType&& Rhs)
{
	return Lhs.Equals(ViewType(Forward<RangeType>(Rhs)), ESearchCase::IgnoreCase);
}

template <typename CharType, typename ViewType, typename RangeType,
	typename = typename TEnableIf<TNot<TIsDerivedFrom<typename TDecay<RangeType>::Type, TStringViewImpl<CharType, ViewType>>>::Value>::Type>
inline auto operator==(RangeType&& Lhs, const TStringViewImpl<CharType, ViewType>& Rhs) -> decltype(Rhs == Forward<RangeType>(Lhs))
{
	return Rhs == Forward<RangeType>(Lhs);
}

template <typename CharType, typename ViewType, typename RangeType>
inline auto operator!=(const TStringViewImpl<CharType, ViewType>& Lhs, RangeType&& Rhs) -> decltype(!(Lhs == Forward<RangeType>(Rhs)))
{
	return !(Lhs == Forward<RangeType>(Rhs));
}

template <typename CharType, typename ViewType, typename RangeType,
	typename = typename TEnableIf<TNot<TIsDerivedFrom<typename TDecay<RangeType>::Type, TStringViewImpl<CharType, ViewType>>>::Value>::Type>
inline auto operator!=(RangeType&& Lhs, const TStringViewImpl<CharType, ViewType>& Rhs) -> decltype(!(Rhs == Forward<RangeType>(Lhs)))
{
	return !(Rhs == Forward<RangeType>(Lhs));
}

// Case-insensitive C-string comparison operators

template <typename CharType, typename ViewType>
inline bool operator==(const TStringViewImpl<CharType, ViewType>& Lhs, const CharType* Rhs)
{
	return TCString<CharType>::Strnicmp(Lhs.GetData(), Rhs, Lhs.Len()) == 0 && !Rhs[Lhs.Len()];
}

template <typename CharType, typename ViewType>
inline bool operator==(const CharType* Lhs, const TStringViewImpl<CharType, ViewType>& Rhs)
{
	return Rhs == Lhs;
}

template <typename CharType, typename ViewType>
inline bool operator!=(const TStringViewImpl<CharType, ViewType>& Lhs, const CharType* Rhs)
{
	return !(Lhs == Rhs);
}

template <typename CharType, typename ViewType>
inline bool operator!=(const CharType* Lhs, const TStringViewImpl<CharType, ViewType>& Rhs)
{
	return !(Lhs == Rhs);
}
