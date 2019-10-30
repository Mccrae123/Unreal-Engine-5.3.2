// Copyright 1998-2019 Epic Games, Inc. All Rights Reserved.

#include "MovieRenderOverlappedImage.h"

#include "Math/Vector.h"
#include "Math/VectorRegister.h"

#include "MovieRenderPipelineCoreModule.h"
#include "Math/Float16.h"

void FImageOverlappedPlane::Init(int32 InSizeX, int32 InSizeY)
{
	SizeX = InSizeX;
	SizeY = InSizeY;

	// leaves the data as is
	ChannelData.SetNumUninitialized(SizeX * SizeY);
}

void FImageOverlappedPlane::ZeroPlane()
{
	int32 Num = SizeX * SizeY;
	for (int32 Index = 0; Index < Num; Index++)
	{
		ChannelData[Index] = 0.0f;
	}
}

void FImageOverlappedPlane::Reset()
{
	SizeX = 0;
	SizeY = 0;
	ChannelData.Empty();
}

// a subpixel offset of (0.5,0.5) means that the raw data is exactly centered on destination pixels.
void FImageOverlappedPlane::AccumulateSinglePlane(const TArray64<float>& InRawData, const TArray64<float>& InWeightData, int32 InSizeX, int32 InSizeY, int32 InOffsetX, int32 InOffsetY,
								float SubpixelOffsetX, float SubpixelOffsetY)
{
	check(InRawData.Num() == InSizeX * InSizeY);
	check(InWeightData.Num() == InSizeX * InSizeY);

	check(SubpixelOffsetX >= 0.0f);
	check(SubpixelOffsetX <= 1.0f);
	check(SubpixelOffsetY >= 0.0f);
	check(SubpixelOffsetY <= 1.0f);

	// if the subpixel offset is less than 0.5, go back one pixel
	int32 StartX = (SubpixelOffsetX >= 0.5 ? InOffsetX : InOffsetX - 1);
	int32 StartY = (SubpixelOffsetY >= 0.5 ? InOffsetY : InOffsetY - 1);

	float PixelWeight[2][2];
	{
		// make sure that the equal sign is correct, if the subpixel offset is 0.5, the weightX is 0 and it starts on the center pixel
		float WeightX = FMath::Frac(SubpixelOffsetX + 0.5f);
		float WeightY = FMath::Frac(SubpixelOffsetY + 0.5f);

		// row, column
		PixelWeight[0][0] = (1.0f - WeightX) * (1.0f - WeightY);
		PixelWeight[0][1] = (       WeightX) * (1.0f - WeightY);
		PixelWeight[1][0] = (1.0f - WeightX) * (       WeightY);
		PixelWeight[1][1] = (       WeightX) * (       WeightY);
	}

	// Slow, reference version. Maybe optimize later.
	for (int CurrY = 0; CurrY < InSizeY; CurrY++)
	{
		for (int CurrX = 0; CurrX < InSizeX; CurrX++)
		{
			float Val = InRawData[CurrY * InSizeX + CurrX];
			float BaseWeight = InWeightData[CurrY * InSizeX + CurrX];

			for (int OffsetY = 0; OffsetY < 2; OffsetY++)
			{
				for (int OffsetX = 0; OffsetX < 2; OffsetX++)
				{
					int DstY = StartY + CurrY + OffsetY;
					int DstX = StartX + CurrX + OffsetX;

					float Weight = BaseWeight * PixelWeight[OffsetY][OffsetX];

					if (DstX >= 0 && DstY >= 0 &&
						DstX < SizeX && DstY < SizeY)
					{
						ChannelData[DstY * SizeX + DstX] += Weight * Val;
					}
				}
			}
		}
	}

}

void FImageOverlappedAccumulator::InitMemory(int InPlaneSizeX, int InPlaneSizeY, int InNumChannels)
{
	PlaneSizeX = InPlaneSizeX;
	PlaneSizeY = InPlaneSizeY;
	NumChannels = InNumChannels;

	ChannelPlanes.SetNum(NumChannels);

	for (int Channel = 0; Channel < NumChannels; Channel++)
	{
		ChannelPlanes[Channel].Init(PlaneSizeX, PlaneSizeY);
	}

	WeightPlane.Init(PlaneSizeX, PlaneSizeY);
}

void FImageOverlappedAccumulator::ZeroPlanes()
{
	check(ChannelPlanes.Num() == NumChannels);

	for (int Channel = 0; Channel < NumChannels; Channel++)
	{
		ChannelPlanes[Channel].ZeroPlane();
	}

	WeightPlane.ZeroPlane();
}

void FImageOverlappedAccumulator::Reset()
{
	PlaneSizeX = 0;
	PlaneSizeY = 0;
	NumChannels = 0;

	// Let the desctructor clean up
	ChannelPlanes.Empty();
}


void FImageOverlappedAccumulator::GenerateTileWeight(TArray64<float>& Weights, int32 SizeX, int32 SizeY)
{
	Weights.SetNum(SizeX * SizeY);

	// we'll use a simple triangle filter, which goes from 1.0 at the center to 0.0 at 3/4 of the way to the tile edge.

	float ScaleX = 1.0f / (float(SizeX / 2) * .75f);
	float ScaleY = 1.0f / (float(SizeY / 2) * .75f);

	for (int PixY = 0; PixY < SizeY; PixY++)
	{
		for (int PixX = 0; PixX < SizeX; PixX++)
		{
			float Y = float(PixY) + .5f;
			float X = float(PixX) + .5f;

			float DistX = FMath::Abs(SizeX / 2 - X);
			float DistY = FMath::Abs(SizeY / 2 - Y);

			float WeightX = FMath::Clamp(1.0f - DistX * ScaleX, 0.0f, 1.0f);
			float WeightY = FMath::Clamp(1.0f - DistY * ScaleY, 0.0f, 1.0f);

			Weights[PixY * SizeX + PixX] = WeightX * WeightY;
		}
	}
}

void FImageOverlappedAccumulator::AccumulatePixelData(const FImagePixelData& InPixelData, int32 InTileOffsetX, int32 InTileOffsetY, FVector2D InSubpixelOffset)
{
	EImagePixelType Fmt = InPixelData.GetType();

	int32 RawNumChan = InPixelData.GetNumChannels();
	int32 RawBitDepth = InPixelData.GetBitDepth();
	FIntPoint RawSize = InPixelData.GetSize();

	int32 SizeInBytes = 0;
	const void* SrcRawDataPtr = nullptr;

	bool IsFetchOk = InPixelData.GetRawData(SrcRawDataPtr, SizeInBytes);
	check(IsFetchOk); // keeping the if below for now, but this really should always succeed

	if (IsFetchOk)
	{
		// hardcode to 4 channels (RGBA), even if we are only saving fewer channels
		TArray64<float> RawData[4];

		check(NumChannels >= 1);
		check(NumChannels <= 4);

		for (int32 ChanIter = 0; ChanIter < 4; ChanIter++)
		{
			RawData[ChanIter].SetNumUninitialized(RawSize.X * RawSize.Y);
		}

		const double AccumulateBeginTime = FPlatformTime::Seconds();
			
		// for now, only handle rgba8
		if (Fmt == EImagePixelType::Color && RawNumChan == 4 && RawBitDepth == 8)
		{
			const uint8* RawDataPtr = static_cast<const uint8*>(SrcRawDataPtr);

			static bool bIsReferenceUnpack = false;
			if (bIsReferenceUnpack)
			{
				// simple, slow unpack, takes about 10-15ms on a 1080p image
				for (int32 Y = 0; Y < RawSize.Y; Y++)
				{
					for (int32 X = 0; X < RawSize.X; X++)
					{
						int32 ChanReorder[4] = { 2, 1, 0, 3 };
						for (int32 ChanIter = 0; ChanIter < 4; ChanIter++)
						{
							int32 RawValue = RawDataPtr[(Y*RawSize.X + X)*RawNumChan + ChanIter];
							float Value = float(RawValue) / 255.0f;
							int32 Reorder = ChanReorder[ChanIter];
							RawData[Reorder][Y*RawSize.X + X] = Value;
						}
					}
				}
			}
			else
			{
				// slightly optimized, takes about 3-7ms on a 1080p image
				for (int32 Y = 0; Y < RawSize.Y; Y++)
				{
					const uint8* SrcRowDataPtr = &RawDataPtr[Y*RawSize.X*RawNumChan];

					float* DstRowDataR = &RawData[0][Y*RawSize.X];
					float* DstRowDataG = &RawData[1][Y*RawSize.X];
					float* DstRowDataB = &RawData[2][Y*RawSize.X];
					float* DstRowDataA = &RawData[3][Y*RawSize.X];

					VectorRegister ColorScale = MakeVectorRegister(1.0f / 255.0f, 1.0f / 255.0f, 1.0f / 255.0f, 1.0f / 255.0f);

					// simple, one pixel at a time vectorized version, we could do better
					for (int32 X = 0; X < RawSize.X; X++)
					{
						VectorRegister Color = VectorLoadByte4(&SrcRowDataPtr[X*RawNumChan]);
						Color = VectorMultiply(Color, ColorScale); // multiply by 1/255

						const float* RawColorVec = reinterpret_cast<const float *>(&Color);
						DstRowDataR[X] = RawColorVec[2];
						DstRowDataG[X] = RawColorVec[1];
						DstRowDataB[X] = RawColorVec[0];
						DstRowDataA[X] = RawColorVec[3];
					}
				}
			}
		}
		else if (Fmt == EImagePixelType::Float16 && RawNumChan == 4 && RawBitDepth == 16)
		{
			const uint16* RawDataPtr = static_cast<const uint16*>(SrcRawDataPtr);

			// simple, slow unpack version for fp16, could make this faster
			for (int32 Y = 0; Y < RawSize.Y; Y++)
			{
				for (int32 X = 0; X < RawSize.X; X++)
				{
					for (int32 ChanIter = 0; ChanIter < 4; ChanIter++)
					{
						FFloat16 RawValue;
						RawValue.Encoded = RawDataPtr[(Y*RawSize.X + X)*RawNumChan + ChanIter];
						
						// c cast does the conversion from fp16 bits to float
						float Value = float(RawValue);
						RawData[ChanIter][Y*RawSize.X + X] = Value;
					}
				}
			}
		}
		else if (Fmt == EImagePixelType::Float32 && RawNumChan == 4 && RawBitDepth == 32)
		{
			const float* RawDataPtr = static_cast<const float*>(SrcRawDataPtr);

			// reference version for float
			for (int32 Y = 0; Y < RawSize.Y; Y++)
			{
				for (int32 X = 0; X < RawSize.X; X++)
				{
					for (int32 ChanIter = 0; ChanIter < 4; ChanIter++)
					{
						float Value = RawDataPtr[(Y*RawSize.X + X)*RawNumChan + ChanIter];
						RawData[ChanIter][Y*RawSize.X + X] = Value;
					}
				}
			}
		}
		else
		{
			check(0);
		}

		const double AccumulateEndTime = FPlatformTime::Seconds();
		{
			const float ElapsedMs = float((AccumulateEndTime - AccumulateBeginTime)*1000.0f);
			//UE_LOG(LogTemp, Log, TEXT("    [%8.2f] Unpack time."), ElapsedMs);
		}
	
		if (AccumulationGamma != 1.0f)
		{
			// Unfortunately, we don't have an SSE optimized pow function. This function is quite slow (about 30-40ms).
			float Gamma = AccumulationGamma;

			for (int ChanIter = 0; ChanIter < NumChannels; ChanIter++)
			{
				float* DstData = RawData[ChanIter].GetData();
				int32 DstSize = RawSize.X * RawSize.Y;
				for (int32 Iter = 0; Iter < DstSize; Iter++)
				{
					DstData[Iter] = FMath::Pow(DstData[Iter], Gamma);
				}
			}
		} 

		const double GammaEndTime = FPlatformTime::Seconds();
		{
			const float ElapsedMs = float((GammaEndTime - AccumulateEndTime)*1000.0f);
			//UE_LOG(LogTemp, Log, TEXT("        [%8.2f] Gamma time."), ElapsedMs);
		}

		// Calculate weight data for this tile.
		TArray64<float> Weights;
		GenerateTileWeight(Weights, RawSize.X, RawSize.Y);

		for (int32 ChanIter = 0; ChanIter < NumChannels; ChanIter++)
		{
			ChannelPlanes[ChanIter].AccumulateSinglePlane(RawData[ChanIter], Weights, RawSize.X, RawSize.Y, InTileOffsetX, InTileOffsetY, InSubpixelOffset.X, InSubpixelOffset.Y);
		}

		// Accumulate weights as well.
		{
			TArray64<float> VecOne;
			VecOne.SetNum(RawSize.X * RawSize.Y);
			for (int32 Index = 0; Index < VecOne.Num(); Index++)
			{
				VecOne[Index] = 1.0f;
			}

			WeightPlane.AccumulateSinglePlane(VecOne, Weights, RawSize.X, RawSize.Y, InTileOffsetX, InTileOffsetY, InSubpixelOffset.X, InSubpixelOffset.Y);
		}
	}
}

void FImageOverlappedAccumulator::FetchFullImageValue(float Rgba[4], int32 FullX, int32 FullY) const
{
	Rgba[0] = 0.0f;
	Rgba[1] = 0.0f;
	Rgba[2] = 0.0f;
	Rgba[3] = 1.0f;

	float RawWeight = WeightPlane.ChannelData[FullY * PlaneSizeX + FullX];

	float Scale = 1.0f / FMath::Max<float>(RawWeight, 0.0001f);

	for (int64 Chan = 0; Chan < NumChannels; Chan++)
	{
		float Val = ChannelPlanes[Chan].ChannelData[FullY * PlaneSizeX + FullX];

		Rgba[Chan] = Val * Scale;
	}

	if (AccumulationGamma != 1.0f)
	{
		float InvGamma = 1.0f / AccumulationGamma;
		Rgba[0] = FMath::Pow(Rgba[0], InvGamma);
		Rgba[1] = FMath::Pow(Rgba[1], InvGamma);
		Rgba[2] = FMath::Pow(Rgba[2], InvGamma);
		Rgba[3] = FMath::Pow(Rgba[3], InvGamma);
	}
}

void FImageOverlappedAccumulator::FetchFinalPixelDataByte(TArray<FColor> & OutPixelData) const
{
	int32 FullSizeX = PlaneSizeX;
	int32 FullSizeY = PlaneSizeY;
	OutPixelData.SetNumUninitialized(FullSizeX * FullSizeY);

	for (int32 FullY = 0L; FullY < FullSizeY; FullY++)
	{
		for (int32 FullX = 0L; FullX < FullSizeX; FullX++)
		{
			float Rgba[4] = { 0.0f, 0.0f, 0.0f, 1.0f };

			FetchFullImageValue(Rgba, FullX, FullY);
			FColor Color = FColor(Rgba[0]*255.0f,Rgba[1]*255.0f,Rgba[2]*255.0f,Rgba[3]*255.0f);

			// be careful with this index, make sure to use 64bit math, not 32bit
			int64 DstIndex = int64(FullY) * int64(FullSizeX) + int64(FullX);
			OutPixelData[DstIndex] = Color;
		}
	}
}

void FImageOverlappedAccumulator::FetchFinalPixelDataLinearColor(TArray<FLinearColor> & OutPixelData) const
{
	int32 FullSizeX = PlaneSizeX;
	int32 FullSizeY = PlaneSizeY;
	OutPixelData.SetNumUninitialized(FullSizeX * FullSizeY);

	for (int32 FullY = 0L; FullY < FullSizeY; FullY++)
	{
		for (int32 FullX = 0L; FullX < FullSizeX; FullX++)
		{
			float Rgba[4] = { 0.0f, 0.0f, 0.0f, 1.0f };

			FetchFullImageValue(Rgba, FullX, FullY);
			FLinearColor Color = FLinearColor(Rgba[0],Rgba[1],Rgba[2],Rgba[3]);

			// be careful with this index, make sure to use 64bit math, not 32bit
			int64 DstIndex = int64(FullY) * int64(FullSizeX) + int64(FullX);
			OutPixelData[DstIndex] = Color;
		}
	}
}

