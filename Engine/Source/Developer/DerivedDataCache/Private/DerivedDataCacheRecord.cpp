// Copyright Epic Games, Inc. All Rights Reserved.

#include "DerivedDataCacheRecord.h"

#include "Containers/UnrealString.h"
#include "DerivedDataCacheKey.h"
#include "Misc/StringBuilder.h"
#include "Serialization/CompactBinary.h"
#include "UObject/NameTypes.h"

namespace UE::DerivedData::Private
{

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

class FCacheRecordBuilderInternal final : public ICacheRecordBuilderInternal
{
public:
	explicit FCacheRecordBuilderInternal(const FCacheKey& Key);
	~FCacheRecordBuilderInternal() final = default;

	void SetMeta(FCbObject&& Meta) final;

	FPayloadId SetValue(const FSharedBuffer& Buffer, const FPayloadId& Id) final;
	FPayloadId SetValue(FPayload&& Payload) final;

	FPayloadId AddAttachment(const FSharedBuffer& Buffer, const FPayloadId& Id) final;
	FPayloadId AddAttachment(FPayload&& Payload) final;

	FCacheRecord Build() final;
	FRequest BuildAsync(FOnCacheRecordComplete&& OnComplete, EPriority Priority) final;

	FCacheKey Key;
	FCbObject Meta;
	FPayload Value;
	TArray<FPayload> Attachments;
};

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

class FCacheRecordInternal final : public ICacheRecordInternal
{
public:
	FCacheRecordInternal() = default;
	explicit FCacheRecordInternal(FCacheRecordBuilderInternal&& RecordBuilder);

	~FCacheRecordInternal() final = default;

	FCacheRecord Clone() const final;

	const FCacheKey& GetKey() const final;
	const FCbObject& GetMeta() const final;

	FSharedBuffer GetValue() const final;
	const FPayload& GetValuePayload() const final;

	FSharedBuffer GetAttachment(const FPayloadId& Id) const final;
	const FPayload& GetAttachmentPayload(const FPayloadId& Id) const final;
	TConstArrayView<FPayload> GetAttachmentPayloads() const final;

	FCacheKey Key;
	FCbObject Meta;
	FPayload Value;
	TArray<FPayload> Attachments;
	mutable FSharedBuffer ValueCache;
	mutable TArray<FSharedBuffer> AttachmentsCache;
};

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

static const FPayload& GetEmptyCachePayload()
{
	static const FPayload Empty;
	return Empty;
}

static FPayloadId GetOrCreatePayloadId(const FPayloadId& Id, const FIoHash& RawHash)
{
	return Id.IsValid() ? Id : FPayloadId::FromHash(RawHash);
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

FCacheRecordInternal::FCacheRecordInternal(FCacheRecordBuilderInternal&& RecordBuilder)
	: Key(RecordBuilder.Key)
	, Meta(MoveTemp(RecordBuilder.Meta))
	, Value(MoveTemp(RecordBuilder.Value))
	, Attachments(MoveTemp(RecordBuilder.Attachments))
{
}

FCacheRecord FCacheRecordInternal::Clone() const
{
	return CreateCacheRecord(new FCacheRecordInternal(*this));
}

const FCacheKey& FCacheRecordInternal::GetKey() const
{
	return Key;
}

const FCbObject& FCacheRecordInternal::GetMeta() const
{
	return Meta;
}

FSharedBuffer FCacheRecordInternal::GetValue() const
{
	if (ValueCache.IsNull() && Value.IsValid())
	{
		ValueCache = Value.GetData().Decompress();
	}
	return ValueCache;
}

const FPayload& FCacheRecordInternal::GetValuePayload() const
{
	return Value;
}

FSharedBuffer FCacheRecordInternal::GetAttachment(const FPayloadId& Id) const
{
	const int32 Index = Algo::LowerBound(Attachments, Id, FPayloadLessById());
	if (Attachments.IsValidIndex(Index) && FPayloadEqualById()(Attachments[Index], Id))
	{
		if (AttachmentsCache.IsEmpty())
		{
			AttachmentsCache.SetNum(Attachments.Num());
		}
		FSharedBuffer& DataCache = AttachmentsCache[Index];
		if (!DataCache)
		{
			DataCache = Attachments[Index].GetData().Decompress();
		}
		return DataCache;
	}
	return FSharedBuffer();
}

const FPayload& FCacheRecordInternal::GetAttachmentPayload(const FPayloadId& Id) const
{
	const int32 Index = Algo::LowerBound(Attachments, Id, FPayloadLessById());
	if (Attachments.IsValidIndex(Index) && FPayloadEqualById()(Attachments[Index], Id))
	{
		return Attachments[Index];
	}
	return GetEmptyCachePayload();
}

TConstArrayView<FPayload> FCacheRecordInternal::GetAttachmentPayloads() const
{
	return Attachments;
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

FCacheRecordBuilderInternal::FCacheRecordBuilderInternal(const FCacheKey& InKey)
	: Key(InKey)
{
}

void FCacheRecordBuilderInternal::SetMeta(FCbObject&& InMeta)
{
	Meta = MoveTemp(InMeta);
	Meta.MakeOwned();
}

FPayloadId FCacheRecordBuilderInternal::SetValue(const FSharedBuffer& Buffer, const FPayloadId& Id)
{
	FCompressedBuffer CompressedBuffer = FCompressedBuffer::Compress(NAME_Default, Buffer);
	const FPayloadId ValueId = GetOrCreatePayloadId(Id, FIoHash(CompressedBuffer.GetRawHash()));
	return SetValue(FPayload(ValueId, MoveTemp(CompressedBuffer)));
}

FPayloadId FCacheRecordBuilderInternal::SetValue(FPayload&& Payload)
{
	checkf(Payload, TEXT("Failed to set value on %s because the payload is null."), *WriteToString<96>(Key));
	checkf(Value.IsNull(),
		TEXT("Cache: Failed to set value on %s with ID %s because it has an existing value with ID %s."),
		*WriteToString<96>(Key), *WriteToString<32>(Payload.GetId()), *WriteToString<32>(Value.GetId()));
	Value = MoveTemp(Payload);
	return Value.GetId();
}

FPayloadId FCacheRecordBuilderInternal::AddAttachment(const FSharedBuffer& Buffer, const FPayloadId& Id)
{
	FCompressedBuffer CompressedBuffer = FCompressedBuffer::Compress(NAME_Default, Buffer);
	const FPayloadId AttachmentId = GetOrCreatePayloadId(Id, FIoHash(CompressedBuffer.GetRawHash()));
	return AddAttachment(FPayload(AttachmentId, MoveTemp(CompressedBuffer)));
}

FPayloadId FCacheRecordBuilderInternal::AddAttachment(FPayload&& Payload)
{
	checkf(Payload, TEXT("Failed to add attachment on %s because the payload is null."), *WriteToString<96>(Key));
	const int32 Index = Algo::LowerBound(Attachments, Payload, FPayloadLessById());
	checkf(!Attachments.IsValidIndex(Index) || !FPayloadEqualById()(Attachments[Index], Payload),
		TEXT("Failed to add attachment on %s with ID %s because it has an existing attachment with that ID."),
		*WriteToString<96>(Key), *WriteToString<32>(Payload.GetId()));
	Attachments.Insert(MoveTemp(Payload), Index);
	return Attachments[Index].GetId();
}

FCacheRecord FCacheRecordBuilderInternal::Build()
{
	return CreateCacheRecord(new FCacheRecordInternal(MoveTemp(*this)));
}

FRequest FCacheRecordBuilderInternal::BuildAsync(FOnCacheRecordComplete&& OnComplete, EPriority Priority)
{
	OnComplete(Build());
	return FRequest();
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

FCacheRecord CreateCacheRecord(ICacheRecordInternal* Record)
{
	return FCacheRecord(Record);
}

FCacheRecordBuilder CreateCacheRecordBuilder(ICacheRecordBuilderInternal* RecordBuilder)
{
	return FCacheRecordBuilder(RecordBuilder);
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

FCacheRecordBuilder CreateCacheRecordBuilder(const FCacheKey& Key)
{
	return CreateCacheRecordBuilder(new FCacheRecordBuilderInternal(Key));
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

} // UE::DerivedData::Private
