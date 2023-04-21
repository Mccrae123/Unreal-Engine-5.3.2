// Copyright Epic Games, Inc. All Rights Reserved.

#include "HairStrandsDatas.h"
#include "UObject/ReleaseObjectVersion.h"
#include "UObject/UE5ReleaseStreamObjectVersion.h"
#include "HairAttributes.h"

#if WITH_EDITOR
#include "DerivedDataCache.h"
#include "DerivedDataRequestOwner.h"
#endif

void FHairStrandsInterpolationDatas::SetNum(const uint32 NumCurves)
{
	PointsSimCurvesVertexWeights.SetNum(NumCurves);
	PointsSimCurvesVertexLerp.SetNum(NumCurves);
	PointsSimCurvesVertexIndex.SetNum(NumCurves);
	PointsSimCurvesIndex.SetNum(NumCurves);
}

void FHairStrandsCurves::SetNum(const uint32 NumCurves, uint32 InAttributes)
{
	CurvesOffset.SetNum(NumCurves + 1);
	CurvesCount.SetNum(NumCurves);
	CurvesLength.SetNum(NumCurves);

	// Not initialized to track if the data are available
	if (HasHairAttribute(InAttributes, EHairAttribute::RootUV))		{ CurvesRootUV.SetNum(NumCurves); }
	if (HasHairAttribute(InAttributes, EHairAttribute::StrandID))	{ StrandIDs.SetNum(NumCurves); }
	if (HasHairAttribute(InAttributes, EHairAttribute::ClumpID))	{ ClumpIDs.SetNum(NumCurves); }
	if (HasHairAttribute(InAttributes, EHairAttribute::PrecomputedGuideWeights))
	{
		CurvesClosestGuideIDs.SetNum(NumCurves);
		CurvesClosestGuideWeights.SetNum(NumCurves);
	}
}

void FHairStrandsPoints::SetNum(const uint32 NumPoints, uint32 InAttributes)
{
	PointsPosition.SetNum(NumPoints);
	PointsRadius.SetNum(NumPoints);
	PointsCoordU.SetNum(NumPoints);

	// Not initialized to track if the data are available
	if (HasHairAttribute(InAttributes, EHairAttribute::Color))		{ PointsBaseColor.SetNum(NumPoints); }
	if (HasHairAttribute(InAttributes, EHairAttribute::Roughness))	{ PointsRoughness.SetNum(NumPoints); }
	if (HasHairAttribute(InAttributes, EHairAttribute::AO))			{ PointsAO.SetNum(NumPoints); }
}

void FHairStrandsDatas::Reset()
{
	StrandsCurves.Reset();
	StrandsPoints.Reset();
	HairDensity = 1;
	BoundingBox = FBox(EForceInit::ForceInit);
}

void FHairStrandsInterpolationDatas::Reset()
{
	PointsSimCurvesVertexWeights.Reset();
	PointsSimCurvesVertexLerp.Reset();
	PointsSimCurvesVertexIndex.Reset();
	PointsSimCurvesIndex.Reset();
}

void FHairStrandsClusterCullingData::Reset()
{
	*this = FHairStrandsClusterCullingData();
}

void FHairStrandsCurves::Reset()
{
	CurvesOffset.Reset();
	CurvesCount.Reset();
	CurvesLength.Reset();
	CurvesRootUV.Reset();
	ClumpIDs.Reset();
	CurvesClosestGuideIDs.Reset();
	CurvesClosestGuideWeights.Reset();
}

void FHairStrandsPoints::Reset()
{
	PointsPosition.Reset();
	PointsRadius.Reset();
	PointsCoordU.Reset();
	PointsBaseColor.Reset();
	PointsRoughness.Reset();
	PointsAO.Reset();
}

float GetHairStrandsMaxLength(const FHairStrandsDatas& In)
{
	float MaxLength = 0;
	for (float CurveLength : In.StrandsCurves.CurvesLength)
	{
		MaxLength = FMath::Max(MaxLength, CurveLength);
	}
	return MaxLength;
}

float GetHairStrandsMaxRadius(const FHairStrandsDatas& In)
{
	float MaxRadius = 0;
	for (float PointRadius : In.StrandsPoints.PointsRadius)
	{
		MaxRadius = FMath::Max(MaxRadius, PointRadius);
	}
	return MaxRadius;
}

/////////////////////////////////////////////////////////////////////////////////////////
// Common bulk data

namespace HairStrands
{
#if WITH_EDITOR
	const UE::DerivedData::FValueId HairStrandsValueId = UE::DerivedData::FValueId::FromName("HairStrandsStreamingData");
#endif
}

void FHairStrandsBulkCommon::Write_DDC(UObject* Owner, TArray<UE::DerivedData::FCachePutValueRequest>& Out)
{
#if WITH_EDITORONLY_DATA
	FHairStrandsBulkCommon::FQuery Q;
	Q.Type = FHairStrandsBulkCommon::FQuery::WriteDDC;
	Q.OutWriteDDC = &Out;
	Q.DerivedDataKey = &DerivedDataKey;
	Q.Owner = Owner;
	GetResources(Q);
#endif
}

void FHairStrandsBulkCommon::Read_DDC(TArray<UE::DerivedData::FCacheGetChunkRequest>& Out)
{
#if WITH_EDITORONLY_DATA
	FHairStrandsBulkCommon::FQuery Q;
	Q.Type = FHairStrandsBulkCommon::FQuery::ReadDDC;
	Q.OutReadDDC = &Out;
	Q.DerivedDataKey = &DerivedDataKey;
	GetResources(Q);
#endif
}

void FHairStrandsBulkCommon::Read_IO(FBulkDataBatchRequest& Out)
{
	FBulkDataBatchRequest::FBatchBuilder Batch = Out.NewBatch(GetResourceCount());

	FHairStrandsBulkCommon::FQuery Q;
	Q.Type = FHairStrandsBulkCommon::FQuery::ReadIO;
	Q.OutReadIO = &Batch;
	GetResources(Q);
	Q.OutReadIO->Issue(Out);
}
void FHairStrandsBulkCommon::Write_IO(FArchive& Ar, UObject* Owner)
{
	GetResourceVersion(Ar);

	FHairStrandsBulkCommon::FQuery Q;
	Q.Type = FHairStrandsBulkCommon::FQuery::ReadWriteIO;
	Q.OutWriteIO = &Ar;
	Q.Owner = Owner;
	GetResources(Q);
}

void FHairStrandsBulkCommon::Serialize(FArchive& Ar, UObject* Owner)
{
	SerializeHeader(Ar, Owner);
	SerializeData(Ar, Owner);
}

void FHairStrandsBulkCommon::SerializeData(FArchive& Ar, UObject* Owner)
{
	Write_IO(Ar, Owner);
}

void FHairStrandsBulkCommon::FQuery::Add(FHairBulkContainer& In, const TCHAR* InSuffix) 
{
	check(Type != None);
#if WITH_EDITORONLY_DATA
	if (Type == WriteDDC)
	{
		const int64 DataSizeInByte = In.Data.GetBulkDataSize();
		TArray<uint8> WriteData;
		WriteData.SetNum(DataSizeInByte);
		FMemory::Memcpy(WriteData.GetData(), In.Data.Lock(LOCK_READ_ONLY), DataSizeInByte);
		In.Data.Unlock();

		using namespace UE::DerivedData;
		FCachePutValueRequest& Out = OutWriteDDC->AddDefaulted_GetRef();
		if (Owner) { Out.Name = Owner->GetPathName(); }
		Out.Key 	= ConvertLegacyCacheKey(*DerivedDataKey + InSuffix);
		Out.Value 	= FValue::Compress(MakeSharedBufferFromArray(MoveTemp(WriteData)));
		Out.Policy 	= ECachePolicy::Default;
		Out.UserData= 0;
	}
	else if (Type == ReadDDC)
	{
		check(OutReadDDC);
		using namespace UE::DerivedData;
		FCacheGetChunkRequest& Out = OutReadDDC->AddDefaulted_GetRef();
		Out.Id			= FValueId::Null; // HairStrands::HairStrandsValueId : This is only needed for cache record, not cache value.
		Out.Key			= ConvertLegacyCacheKey(*DerivedDataKey + InSuffix);
		Out.RawOffset	= 0; 			//In.Upload.Offset;
		Out.RawSize		= MAX_uint64; 	//In.Upload.Size;
		Out.RawHash		= FIoHash();
		Out.UserData	= (uint64)&In;
	}
	else 
#endif
	if (Type == ReadIO)
	{
		check(OutReadIO);
		OutReadIO->Read(In.Data);
	}
	else
	{
		check(Type == ReadWriteIO)
		check(OutWriteIO);

		if (OutWriteIO->IsSaving())
		{
			const uint32 BulkFlags = BULKDATA_Force_NOT_InlinePayload;
			In.Data.SetBulkDataFlags(BulkFlags);
		}
		In.Data.Serialize(*OutWriteIO, Owner, 0/*ChunkIndex*/, false /*bAttemptFileMapping*/);
	}
}

/////////////////////////////////////////////////////////////////////////////////////////
// Rest bulk data

void FHairStrandsBulkData::SerializeHeader(FArchive& Ar, UObject* Owner)
{
	GetResourceVersion(Ar);

	Ar << Header.CurveCount;
	Ar << Header.PointCount;
	Ar << Header.MaxLength;
	Ar << Header.MaxRadius;
	Ar << Header.BoundingBox;
	Ar << Header.Flags;
	for (uint8 AttributeIt = 0; AttributeIt < HAIR_CURVE_ATTRIBUTE_COUNT; ++AttributeIt)
	{
		Ar << Header.CurveAttributeOffsets[AttributeIt];
	}
	for (uint8 AttributeIt = 0; AttributeIt < HAIR_POINT_ATTRIBUTE_COUNT; ++AttributeIt)
	{
		Ar << Header.PointAttributeOffsets[AttributeIt];
	}
	Ar << Header.ImportedAttributes;
	Ar << Header.ImportedAttributeFlags;

	Ar << Header.Strides.PositionStride;
	Ar << Header.Strides.CurveStride;
	Ar << Header.Strides.PointToCurveStride;
	Ar << Header.Strides.CurveAttributeChunkStride;
	Ar << Header.Strides.PointAttributeChunkStride;
	Ar << Header.Strides.CurveAttributeChunkElementCount;
	Ar << Header.Strides.PointAttributeChunkElementCount;
}

void FHairStrandsBulkData::GetResourceVersion(FArchive& Ar) const
{
	Ar.UsingCustomVersion(FReleaseObjectVersion::GUID);
	Ar.UsingCustomVersion(FUE5ReleaseStreamObjectVersion::GUID);
}

uint32 FHairStrandsBulkData::GetResourceCount() const
{
	return 5;
}

void FHairStrandsBulkData::GetResources(FHairStrandsBulkCommon::FQuery& Out)
{
	static_assert(sizeof(FHairStrandsPositionFormat::BulkType) == sizeof(FHairStrandsPositionFormat::Type));
	static_assert(sizeof(FHairStrandsAttributeFormat::BulkType) == sizeof(FHairStrandsAttributeFormat::Type));
	static_assert(sizeof(FHairStrandsPointToCurveFormat16::BulkType) == sizeof(FHairStrandsPointToCurveFormat16::Type));
	static_assert(sizeof(FHairStrandsPointToCurveFormat32::BulkType) == sizeof(FHairStrandsPointToCurveFormat32::Type));
	static_assert(sizeof(FHairStrandsRootIndexFormat::BulkType) == sizeof(FHairStrandsRootIndexFormat::Type)); 

	if (!!(Header.Flags & DataFlags_HasData))
	{
		Out.Add(Data.Positions, TEXT("_Positions"));
		Out.Add(Data.CurveAttributes, TEXT("_CurveAttributes"));
		if (Header.Flags & DataFlags_HasPointAttribute)
		{
			Out.Add(Data.PointAttributes, TEXT("_PointAttributes"));
		}
		Out.Add(Data.PointToCurve, TEXT("_PointToCurve"));
		Out.Add(Data.Curves, TEXT("_Curves"));
	}
}

void FHairStrandsBulkData::Reset()
{
	Header.CurveCount = 0;
	Header.PointCount = 0;
	Header.MaxLength = 0;
	Header.MaxRadius = 0;
	Header.BoundingBox = FBox(EForceInit::ForceInit);
	Header.Flags = 0;
	for (uint8 AttributeIt = 0; AttributeIt < HAIR_CURVE_ATTRIBUTE_COUNT; ++AttributeIt)
	{
		Header.CurveAttributeOffsets[AttributeIt] = HAIR_ATTRIBUTE_INVALID_OFFSET;
	}
	for (uint8 AttributeIt = 0; AttributeIt < HAIR_POINT_ATTRIBUTE_COUNT; ++AttributeIt)
	{
		Header.PointAttributeOffsets[AttributeIt] = HAIR_ATTRIBUTE_INVALID_OFFSET;
	}
	// Deallocate memory if needed
	Data.Positions.RemoveBulkData();
	Data.CurveAttributes.RemoveBulkData();
	Data.PointAttributes.RemoveBulkData();
	Data.PointToCurve.RemoveBulkData();
	Data.Curves.RemoveBulkData();

	// Reset the bulk byte buffer to ensure the (serialize) data size is reset to 0
	Data.Positions 		= FHairBulkContainer();
	Data.CurveAttributes= FHairBulkContainer();
	Data.PointAttributes= FHairBulkContainer();
	Data.PointToCurve	= FHairBulkContainer();
	Data.Curves			= FHairBulkContainer();
}


/////////////////////////////////////////////////////////////////////////////////////////
// Interpolation bulk data

void FHairStrandsInterpolationBulkData::Reset()
{
	Header.Flags = 0;
	Header.PointCount = 0;
	Header.SimPointCount = 0;
	
	// Deallocate memory if needed
	Data.Interpolation.RemoveBulkData();
	Data.SimRootPointIndex.RemoveBulkData();

	// Reset the bulk byte buffer to ensure the (serialize) data size is reset to 0
	Data.Interpolation		= FHairBulkContainer();
	Data.SimRootPointIndex	= FHairBulkContainer();
}

void FHairStrandsInterpolationBulkData::SerializeHeader(FArchive& Ar, UObject* Owner)
{
	Ar << Header.Flags;
	Ar << Header.PointCount;
	Ar << Header.SimPointCount;
}

uint32 FHairStrandsInterpolationBulkData::GetResourceCount() const
{
	return (Header.Flags & DataFlags_HasData) ? 2 : 0;
}

void FHairStrandsInterpolationBulkData::GetResources(FHairStrandsBulkCommon::FQuery& Out)
{
	static_assert(sizeof(FHairStrandsRootIndexFormat::BulkType) == sizeof(FHairStrandsRootIndexFormat::Type));

	if (Header.Flags & DataFlags_HasData)
	{
		Out.Add(Data.Interpolation, TEXT("_Interpolation"));
		Out.Add(Data.SimRootPointIndex, TEXT("_SimRootPointIndex"));
	}
}


/////////////////////////////////////////////////////////////////////////////////////////
// Cluster culling bulk data

void FHairStrandsClusterCullingBulkData::Reset()
{
	Header.ClusterCount = 0;
	Header.ClusterLODCount = 0;
	Header.VertexCount = 0;
	Header.VertexLODCount = 0;

	Header.LODVisibility.Empty();
	Header.CPULODScreenSize.Empty();
	Header.LODInfos.Empty();

	Data.ClusterLODInfos.RemoveBulkData();
	Data.VertexToClusterIds.RemoveBulkData();
	Data.ClusterVertexIds.RemoveBulkData();
	Data.PackedClusterInfos.RemoveBulkData();

	// Reset the bulk byte buffer to ensure the (serialize) data size is reset to 0
	Data.ClusterLODInfos 	= FHairBulkContainer();
	Data.VertexToClusterIds = FHairBulkContainer();
	Data.ClusterVertexIds 	= FHairBulkContainer();
	Data.PackedClusterInfos = FHairBulkContainer();
}

void FHairStrandsClusterCullingBulkData::SerializeHeader(FArchive& Ar, UObject* Owner)
{
	Ar << Header.ClusterCount;
	Ar << Header.ClusterLODCount;
	Ar << Header.VertexCount;
	Ar << Header.VertexLODCount;
	Ar << Header.LODVisibility;
	Ar << Header.CPULODScreenSize;
	uint32 LODInfosCount = Header.LODInfos.Num();
	Ar << LODInfosCount;
	if (Ar.IsLoading())
	{
		Header.LODInfos.SetNum(LODInfosCount);
	}
	for (uint32 It = 0; It < LODInfosCount; ++It)
	{
		Ar << Header.LODInfos[It].CurveCount;
		Ar << Header.LODInfos[It].PointCount;
	}
}

uint32 FHairStrandsClusterCullingBulkData::GetResourceCount() const
{
	return 4;
}

bool ValidateHairBulkData();

void FHairStrandsClusterCullingBulkData::GetResources(FHairStrandsBulkCommon::FQuery & Out)
{
	if (Header.ClusterLODCount)
	{
		Out.Add(Data.ClusterLODInfos, TEXT("_ClusterLODInfos"));
	}

	if (Header.VertexCount)
	{
		Out.Add(Data.VertexToClusterIds, TEXT("_VertexToClusterIds"));
	}

	if (Header.VertexLODCount)
	{
		Out.Add(Data.ClusterVertexIds, TEXT("_ClusterVertexIds"));
	}

	if (Header.ClusterCount)
	{
		Out.Add(Data.PackedClusterInfos, TEXT("_PackedClusterInfos"));
	}

	if (ValidateHairBulkData() && (Out.Type == FHairStrandsBulkCommon::FQuery::WriteDDC || Out.Type == FHairStrandsBulkCommon::FQuery::ReadWriteIO))
	{
		Validate(true);
	}
}

void FHairStrandsClusterCullingBulkData::Validate(bool bIsSaving)
{
	if (Header.ClusterCount == 0)
	{
		return;
	}

	const FHairClusterInfo::Packed* Datas = (const FHairClusterInfo::Packed*)Data.PackedClusterInfos.Data.Lock(LOCK_READ_ONLY);
	
	// Simple heuristic to check if the data are valid
	const uint32 MaxCount = FMath::Min(Header.ClusterCount, 128u);
	bool bIsValid = true;
	for (uint32 It = 0; It < MaxCount; ++It)
	{
		bIsValid = bIsValid && Datas[It].LODCount <= 8;
		if (!bIsValid) break;
	}
	if (!bIsValid)
	{
		FString DebugName = Data.ClusterLODInfos.GetDebugName();
		UE_LOG(LogHairStrands, Error, TEXT("[Groom/DDC] Strands - Invalid ClusterCullingBulkData when %s bulk data - %s"), bIsSaving ? TEXT("Saving") : TEXT("Loading"), *DebugName);
	}

	Data.PackedClusterInfos.Data.Unlock();
}
