// Copyright Epic Games, Inc. All Rights Reserved.

#include "DMXFixtureActorMatrix.h"

#include "Rendering/Texture2DResource.h"
#include "Components/StaticMeshComponent.h"

void UpdateMatrixTexture(uint8* MatrixData, UTexture2D* DynamicTexture, int32 MipIndex, uint32 NumRegions, FUpdateTextureRegion2D Region, uint32 SrcPitch, uint32 SrcBpp)//, TFunction<void(uint8* SrcData)> DataCleanupFunc = [](uint8*) {})
{
	if (DynamicTexture->Resource)
	{
		ENQUEUE_RENDER_COMMAND(UpdateTextureRegionsData)(
			[=](FRHICommandListImmediate& RHICmdList)
			{
				FTexture2DResource* Resource = (FTexture2DResource*)DynamicTexture->Resource;
				RHIUpdateTexture2D(
					Resource->GetTexture2DRHI(),
					MipIndex,
					Region,
					SrcPitch,
					MatrixData
					+ Region.SrcY * SrcPitch
					+ Region.SrcX * SrcBpp
				);

				//DataCleanupFunc();
			});

	}
}

ADMXFixtureActorMatrix::ADMXFixtureActorMatrix()
{
	MatrixHead = CreateDefaultSubobject<UProceduralMeshComponent>(TEXT("MatrixHead"));
	MatrixHead->SetupAttachment(Head);

	SpotLight->SetInnerConeAngle(65);
	SpotLight->SetOuterConeAngle(80);

	MatrixHeight = 100;
	MatrixWidth = 100;
	MatrixDepth = 10;

	NbrTextureRows = 1;
	XCells = 1;
	YCells = 1;
	MatrixDataSize = 0;

	MatrixData = nullptr;
	TextureRegion = nullptr;
}

ADMXFixtureActorMatrix::~ADMXFixtureActorMatrix()
{
	if (TextureRegion)
		delete TextureRegion;

	if (MatrixData)
		delete MatrixData;
}

#if WITH_EDITOR
// Support in-editor and PIE
void ADMXFixtureActorMatrix::PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent)
{
	Super::PostEditChangeProperty(PropertyChangedEvent);
	FeedFixtureData();
}
#endif

void ADMXFixtureActorMatrix::InitializeMatrixFixture()
{
	GetComponents<UStaticMeshComponent>(StaticMeshComponents);

	// Create dynamic materials
	DynamicMaterialLens = UMaterialInstanceDynamic::Create(LensMaterialInstance, NULL);
	DynamicMaterialBeam = UMaterialInstanceDynamic::Create(BeamMaterialInstance, NULL);
	DynamicMaterialSpotLight = UMaterialInstanceDynamic::Create(SpotLightMaterialInstance, NULL);
	DynamicMaterialPointLight = UMaterialInstanceDynamic::Create(PointLightMaterialInstance, NULL);

	float Quality = 1.0f;
	switch (QualityLevel)
	{
		case(EDMXFixtureQualityLevel::LowQuality): Quality = 0.25f; break;
		case(EDMXFixtureQualityLevel::MediumQuality): Quality = 0.5f; break;
		case(EDMXFixtureQualityLevel::HighQuality): Quality = 1.0f; break;
		case(EDMXFixtureQualityLevel::UltraQuality): Quality = 2.0f; break;
		default: Quality = 1.0f;
	}

	// Get matrix properties
	FDMXFixtureMatrix MatrixProperties;
	UDMXSubsystem* DMXSubsystem = UDMXSubsystem::GetDMXSubsystem_Pure();
	UDMXEntityFixturePatch* FixturePatch = DMX->GetFixturePatch();
	DMXSubsystem->GetMatrixProperties(FixturePatch, MatrixProperties);
	XCells = MatrixProperties.XCells;
	YCells = MatrixProperties.YCells;

	// Limit cells [1-64]
	XCells = FMath::Max(XCells, 1);
	YCells = FMath::Max(YCells, 1);
	XCells = FMath::Min(XCells, 64);
	YCells = FMath::Min(YCells, 64);

	int NbrCells = XCells * YCells;

	// Create array to hold data in bgra order
	NbrTextureRows = 2;	// using 2 rows to store dmx data
	MatrixDataSize = NbrCells * 4 * NbrTextureRows;
	MatrixData = new uint8[MatrixDataSize];
	for (int i = 0; i < MatrixDataSize; i++)
	{
		MatrixData[i] = 128;
	}

	// Generate runtime procedural mesh
	GenerateMatrixMesh();

	// Create transient texture at runtime (DynamicTexture)
	int TextureWidth = NbrCells;
	int TextureHeight = NbrTextureRows;
	MatrixDataTexture = UTexture2D::CreateTransient(TextureWidth, TextureHeight, EPixelFormat::PF_B8G8R8A8);
	MatrixDataTexture->SRGB = 0;
	MatrixDataTexture->bNoTiling = true;
	MatrixDataTexture->Filter = TextureFilter::TF_Nearest; //pixelated
	MatrixDataTexture->AddressX = TextureAddress::TA_Clamp;
	MatrixDataTexture->AddressY = TextureAddress::TA_Clamp;
	MatrixDataTexture->CompressionSettings = TextureCompressionSettings::TC_VectorDisplacementmap;
	MatrixDataTexture->UpdateResource(); //to initialize resource
	TextureRegion = new FUpdateTextureRegion2D(0, 0, 0, 0, TextureWidth, TextureHeight);

	// Push fixture data into materials and lights
	FeedFixtureData();

	// Assign dynamic materials to lights
	SpotLight->SetMaterial(0, DynamicMaterialSpotLight);
	PointLight->SetMaterial(0, DynamicMaterialPointLight);

	// feed matrix properties to lens material
	if (DynamicMaterialLens)
	{
		DynamicMaterialLens->SetScalarParameterValue("XCells", XCells);
		DynamicMaterialLens->SetScalarParameterValue("YCells", YCells);
		DynamicMaterialLens->SetScalarParameterValue("CellWidth", MatrixWidth / XCells);
		DynamicMaterialLens->SetScalarParameterValue("CellHeight", MatrixHeight / YCells);
		DynamicMaterialLens->SetTextureParameterValue("MatrixData", MatrixDataTexture);
		MatrixHead->SetMaterial(0, DynamicMaterialLens);
	}

	// feed matrix properties to beam material
	if (DynamicMaterialBeam)
	{
		int NbrSamples = FMath::CeilToInt(Quality * 4);
		DynamicMaterialBeam->SetScalarParameterValue("NbrSamples", NbrSamples);
		DynamicMaterialBeam->SetScalarParameterValue("XCells", XCells);
		DynamicMaterialBeam->SetScalarParameterValue("YCells", YCells);
		DynamicMaterialBeam->SetScalarParameterValue("CellWidth", MatrixWidth / XCells);
		DynamicMaterialBeam->SetScalarParameterValue("CellHeight", MatrixHeight / YCells);
		DynamicMaterialBeam->SetTextureParameterValue("MatrixData", MatrixDataTexture);
		MatrixHead->SetMaterial(1, DynamicMaterialBeam);
	}

	// Initialize components
	TInlineComponentArray<UDMXFixtureComponent*> DMXComponents;
	GetComponents<UDMXFixtureComponent>(DMXComponents);
	for (auto& DMXComponent : DMXComponents)
	{
		DMXComponent->Initialize();
	}

	HasBeenInitialized = true;

	// Set initial fixture state using current DMX data
	TArray<FDMXCell> Cells;
	DMXSubsystem->GetAllMatrixCells(FixturePatch, Cells);
	PushFixtureMatrixCellData(Cells);
}

// DMX Data is packed based on this convention
// texture row index 0: RGBColor / Dimmer (4 channels total)
// texture row index 1: Pan / Tilt  (2 channels total)
void ADMXFixtureActorMatrix::UpdateMatrixData(int RowIndex, int CellIndex, int ChannelIndex, uint8 Value)
{
	int index = (RowIndex * XCells * YCells * 4) + (CellIndex * 4) + ChannelIndex;
	if (index < MatrixDataSize)
	{
		MatrixData[index] = Value;
	}
}

// NB: Matrix data and effects are hardcoded for now - We could expose that to BP later
// Cells should always come in following [top-left to bottom-right] convention
void ADMXFixtureActorMatrix::PushFixtureMatrixCellData(TArray<FDMXCell> Cells)
{
	if (HasBeenInitialized)
	{
		// Get current components (supports PIE)
		TInlineComponentArray<UDMXFixtureComponent*> DMXComponents;
		GetComponents<UDMXFixtureComponent>(DMXComponents);

		// get subsystem
		UDMXSubsystem* DMXSubsystem = UDMXSubsystem::GetDMXSubsystem_Pure();

		// get fixture patch
		UDMXEntityFixturePatch* FixturePatch = DMX->GetFixturePatch();

		for (int CurrentCellIndex = 0; CurrentCellIndex < Cells.Num(); CurrentCellIndex++)
		{
			TMap<FDMXAttributeName, int32> PixelAttributesMap;
			FDMXCell Pixel = Cells[CurrentCellIndex];
			DMXSubsystem->GetMatrixCellValue(FixturePatch, Pixel.Coordinate, PixelAttributesMap);

			for (auto& DMXComponent : DMXComponents)
			{
				if (DMXComponent->IsEnabled && DMXComponent->UsingMatrixData)
				{
					// set current cell reference
					DMXComponent->SetCurrentCell(CurrentCellIndex);

					// Color component
					UDMXFixtureComponentColor* ColorComponent = Cast<UDMXFixtureComponentColor>(DMXComponent);
					if(ColorComponent)
					{
						int* d1 = PixelAttributesMap.Find(ColorComponent->ChannelName1.Name);
						int* d2 = PixelAttributesMap.Find(ColorComponent->ChannelName2.Name);
						int* d3 = PixelAttributesMap.Find(ColorComponent->ChannelName3.Name);
						int* d4 = PixelAttributesMap.Find(ColorComponent->ChannelName4.Name);

						// 255 if channel not found
						int r = (d1) ? *d1 : ColorComponent->BitResolution;
						int g = (d2) ? *d2 : ColorComponent->BitResolution;
						int b = (d3) ? *d3 : ColorComponent->BitResolution;
						int a = (d4) ? *d4 : ColorComponent->BitResolution;

						FLinearColor NewTargetColor = ColorComponent->RemapColor(r, g, b, a);
						if (ColorComponent->IsColorValid(NewTargetColor))
						{
							ColorComponent->SetTargetColor(NewTargetColor);

							// pack data in Matrix structure
							uint8 Red = FMath::FloorToInt(NewTargetColor.R * 255);
							uint8 Green = FMath::FloorToInt(NewTargetColor.G * 255);
							uint8 Blue = FMath::FloorToInt(NewTargetColor.B * 255);
							uint8 Dimmer = FMath::FloorToInt(NewTargetColor.A * 255);
							UpdateMatrixData(0, CurrentCellIndex, 0, Blue);
							UpdateMatrixData(0, CurrentCellIndex, 1, Green);
							UpdateMatrixData(0, CurrentCellIndex, 2, Red);
							UpdateMatrixData(0, CurrentCellIndex, 3, Dimmer);
						}
					}

					// Single channel component - hardcoded for now
					UDMXFixtureComponentSingle* SingleComponent = Cast<UDMXFixtureComponentSingle>(DMXComponent);
					if(SingleComponent)
					{
						int* d1 = PixelAttributesMap.Find(SingleComponent->DMXChannel.Name.Name);
						int ChannelIndex = 0;
						if (d1)
						{
							if (SingleComponent->DMXChannel.Name.Name == FName("Dimmer"))
							{
								float TargetValue = SingleComponent->RemapValue(*d1);
								if (SingleComponent->IsTargetValid(TargetValue))
								{
									uint8 Dimmer = FMath::FloorToInt(TargetValue * 255);
									UpdateMatrixData(0, CurrentCellIndex, 3, Dimmer);
								}
							}

							if (SingleComponent->DMXChannel.Name.Name == FName("Pan"))
							{
								float TargetValue = float(*d1) / SingleComponent->DMXChannel.BitResolution;
								if (SingleComponent->IsTargetValid(TargetValue))
								{
									uint8 Pan = FMath::FloorToInt(TargetValue * 255);
									UpdateMatrixData(1, CurrentCellIndex, 0, Pan);
								}
							}

							if (SingleComponent->DMXChannel.Name.Name == FName("Tilt"))
							{
								float TargetValue = float(*d1) / SingleComponent->DMXChannel.BitResolution;
								if (SingleComponent->IsTargetValid(TargetValue))
								{
									uint8 Tilt = FMath::FloorToInt(TargetValue * 255);
									UpdateMatrixData(1, CurrentCellIndex, 1, Tilt);
								}
							}
						}
					}
				}
			}
		}

		// push matrix data in dynamic texture
		UpdateDynamicTexture();

		// set light color
		FLinearColor MatrixAverageColor = GetMatrixAverageColor();
		SpotLight->SetLightColor(MatrixAverageColor, false);
		SpotLight->SetIntensity(LightIntensityMax * MatrixAverageColor.A);
	}
}


FLinearColor ADMXFixtureActorMatrix::GetMatrixAverageColor()
{
	FLinearColor AverageColor(0,0,0,0);
	int NbrCells = XCells * YCells;
	for (int i = 0; i < NbrCells; i++)
	{
		AverageColor.B += MatrixData[i*4] / 255.0f;
		AverageColor.G += MatrixData[(i*4) + 1] / 255.0f;
		AverageColor.R += MatrixData[(i*4) + 2] / 255.0f;
		AverageColor.A += MatrixData[(i*4) + 3] / 255.0f;
	}
	AverageColor = AverageColor / NbrCells;
	return AverageColor;
}

void ADMXFixtureActorMatrix::UpdateDynamicTexture()
{
	if (MatrixDataTexture)
	{
		int NbrCells = XCells * YCells;
		UpdateMatrixTexture(MatrixData, MatrixDataTexture, 0, 1, *TextureRegion, NbrCells * 4, 4);
	}
}

/*
void ADMXFixtureActorMatrix::PostLoad()
{
	Super::PostLoad();
	//GenerateMatrixMesh();
}
*/

void ADMXFixtureActorMatrix::GenerateMatrixMesh()
{
	MatrixHead->ClearAllMeshSections();
	GenerateMatrixCells();
	GenerateMatrixBeam();
	MatrixHead->SetRelativeLocation(FVector(MatrixWidth*-0.5f, MatrixHeight * -0.5f, 10));
}


void ADMXFixtureActorMatrix::GenerateEditorMatrixMesh()
{
	if (DMX && !GWorld->HasBegunPlay())
	{
		FDMXFixtureMatrix MatrixProperties;
		UDMXEntityFixturePatch* FixturePatch = DMX->GetFixturePatch();
		UDMXSubsystem* DMXSubsystem = UDMXSubsystem::GetDMXSubsystem_Pure();
		DMXSubsystem->GetMatrixProperties(FixturePatch, MatrixProperties);
		XCells = MatrixProperties.XCells;
		YCells = MatrixProperties.YCells;

		// Limit cells [1-64]
		XCells = FMath::Max(XCells, 1);
		YCells = FMath::Max(YCells, 1);
		XCells = FMath::Min(XCells, 64);
		YCells = FMath::Min(YCells, 64);

		MatrixHead->ClearAllMeshSections();
		GenerateMatrixCells();
		GenerateMatrixBeam();
		MatrixHead->SetRelativeLocation(FVector(MatrixWidth * -0.5f, MatrixHeight * -0.5f, 10));

		// Assign MIC
		MatrixHead->SetMaterial(0, LensMaterialInstance);
		MatrixHead->SetMaterial(1, BeamMaterialInstance);
	}
}

void ADMXFixtureActorMatrix::GenerateMatrixCells()
{
	// Reset arrays
	Vertices.Reset();
	Triangles.Reset();
	Normals.Reset();
	Tangents.Reset();
	UV0.Reset();
	UV1.Reset();
	UV2.Reset();
	Colors.Reset();
	QuadIndexCount = 0;

	// Quad 3d Positions
	FVector TopLeftPosition(0, 1, 0);
	FVector BottomLeftPosition(0, 0, 0);
	FVector BottomRightPosition(1, 0, 0);
	FVector TopRightPosition(1, 1, 0);

	// Quad 2d UVs
	FVector2D TopLeftUV(0, 1);
	FVector2D BottomLeftUV(0, 0);
	FVector2D BottomRightUV(1, 0);
	FVector2D TopRightUV(1, 1);

	FProcMeshTangent Tangent = FProcMeshTangent(1.0f, 0.0f, 0.0f);

	// Normal
	FVector Normal = FVector::CrossProduct(TopLeftPosition-BottomRightPosition, TopLeftPosition-TopRightPosition).GetSafeNormal();

	// Quads following [topLeft -> bottomRight] convention
	float QuadWidth = MatrixWidth / XCells;
	float QuadHeight = MatrixHeight / YCells;
	float RowOffset = 0;
	float ColumnOffset = 0;
	for (int RowIndex = 0; RowIndex < YCells; RowIndex++)
	{
		RowOffset = RowIndex * QuadHeight;
		for (int ColumnIndex = 0; ColumnIndex < XCells; ColumnIndex++)
		{
			ColumnOffset = ColumnIndex * QuadWidth;

			FVector P1 = TopLeftPosition;
			P1.X = (P1.X*QuadWidth) + ColumnOffset;
			P1.Y = (P1.Y*QuadHeight) + RowOffset;

			FVector P2 = BottomLeftPosition;
			P2.X = (P2.X * QuadWidth) + ColumnOffset;
			P2.Y = (P2.Y * QuadHeight) + RowOffset;

			FVector P3 = BottomRightPosition;
			P3.X = (P3.X * QuadWidth) + ColumnOffset;
			P3.Y = (P3.Y * QuadHeight) + RowOffset;

			FVector P4= TopRightPosition;
			P4.X = (P4.X * QuadWidth) + ColumnOffset;
			P4.Y = (P4.Y * QuadHeight) + RowOffset;

			Vertices.Add(P1);
			Vertices.Add(P2);
			Vertices.Add(P3);
			Vertices.Add(P4);

			int IndexOffset = QuadIndexCount * 4;
			Triangles.Add(0 + IndexOffset);
			Triangles.Add(2 + IndexOffset);
			Triangles.Add(1 + IndexOffset);
			Triangles.Add(0 + IndexOffset);
			Triangles.Add(3 + IndexOffset);
			Triangles.Add(2 + IndexOffset);

			for (int i = 0; i < 4; i++)
			{
				Normals.Add(Normal);
				Tangents.Add(Tangent);
				Colors.Add(FColor(255, 255, 255));
			}

			// UVs to sample lens mask
			UV0.Add(TopLeftUV);
			UV0.Add(BottomLeftUV);
			UV0.Add(BottomRightUV);
			UV0.Add(TopRightUV);

			// UVs to sample first row, targetting middle of pixel
			//float UOffset = QuadIndexCount / float(NbrCells);
			//float UHalfOffset = (1.0f/NbrCells) * 0.5f;
			//UV1.Add(FVector2D(UOffset + UHalfOffset, 0.5));
			//UV1.Add(FVector2D(UOffset + UHalfOffset, 0.5));
			//UV1.Add(FVector2D(UOffset + UHalfOffset, 0.5));
			//UV1.Add(FVector2D(UOffset + UHalfOffset, 0.5));

			// Pack QuadIndexCount value into two 8bits
			// decoding: HighByte*256 + LowByte
			uint8 HighByte = FMath::Floor(QuadIndexCount / 256.0f);
			uint8 LowByte = QuadIndexCount % 256;
			UV1.Add(FVector2D(HighByte, LowByte));
			UV1.Add(FVector2D(HighByte, LowByte));
			UV1.Add(FVector2D(HighByte, LowByte));
			UV1.Add(FVector2D(HighByte, LowByte));

			// UVs to specify if vertex is part of "lens=1" or "chassis=0"
			UV2.Add(FVector2D(1, 1));
			UV2.Add(FVector2D(1, 1));
			UV2.Add(FVector2D(1, 1));
			UV2.Add(FVector2D(1, 1));

			QuadIndexCount++;
		}
	}

	// Create Matrix Chassis
	FVector MatrixTopLeftPosition(0, 0, 0);
	FVector MatrixBottomLeftPosition(0, QuadHeight*YCells, 0);
	FVector MatrixBottomRightPosition(MatrixWidth, QuadHeight*YCells, 0);
	FVector MatrixTopRightPosition(MatrixWidth, 0, 0);
	GenerateMatrixChassis(MatrixTopLeftPosition, MatrixBottomLeftPosition, MatrixBottomRightPosition, MatrixTopRightPosition);

	// Create mesh section 0
	MatrixHead->CreateMeshSection(0, Vertices, Triangles, Normals, UV0, UV1, UV2, UV0, Colors, Tangents, false);
}


void ADMXFixtureActorMatrix::GenerateMatrixBeam()
{
	// Reset arrays
	Vertices.Reset();
	Triangles.Reset();
	Normals.Reset();
	Tangents.Reset();
	UV0.Reset();
	UV1.Reset();
	UV2.Reset();
	Colors.Reset();
	
	float Quality = 1.0f;
	switch (QualityLevel)
	{
		case(EDMXFixtureQualityLevel::LowQuality): Quality = 0.25f; break;
		case(EDMXFixtureQualityLevel::MediumQuality): Quality = 0.5f; break;
		case(EDMXFixtureQualityLevel::HighQuality): Quality = 1.0f; break;
		case(EDMXFixtureQualityLevel::UltraQuality): Quality = 2.0f; break;
		default: Quality = 1.0f;
	}

	int NbrSamples = FMath::CeilToInt(Quality * 4);

	// Quad 3d directions from center position
	FVector TopLeftDirection(-1, 1, 0);
	FVector BottomLeftDirection(-1,-1, 0);
	FVector BottomRightDirection(1, -1, 0);
	FVector TopRightDirection(1, 1, 0);

	// Quad 2d UVs
	FVector2D TopLeftUV(0, 1);
	FVector2D BottomLeftUV(0, 0);
	FVector2D BottomRightUV(1, 0);
	FVector2D TopRightUV(1, 1);

	// Tangent and normal
	FProcMeshTangent Tangent = FProcMeshTangent(1.0f, 0.0f, 0.0f);
	FVector Normal = FVector(0, 0, 1);

	// Build quads
	float MaxDistance = MatrixWidth * MatrixHeight * 0.01f;
	MaxDistance = FMath::Min(MaxDistance, 50.0f);

	float QuadDistance = MaxDistance / NbrSamples;
	float QuadWidth = MatrixWidth / XCells;
	float QuadHeight = MatrixHeight / YCells;
	float StartX = QuadWidth * 0.5f;
	float StartY = QuadHeight * 0.5f;
	float RowOffset = 0;
	float ColumnOffset = 0;
	float QuadScaleIncrement = 1.5f / NbrSamples;
	float QuadScale = 1.0f;
	FVector QuadSize(QuadWidth*0.5f, QuadHeight*0.5f, 0);

	int QuadCount = 0;
	for (int SampleIndex = 0; SampleIndex < NbrSamples; SampleIndex++)
	{
		int QuadIndex = 0;
		QuadScale += QuadScaleIncrement;
		for (int RowIndex = 0; RowIndex < YCells; RowIndex++)
		{
			RowOffset = RowIndex * QuadHeight;
			for (int ColumnIndex = 0; ColumnIndex < XCells; ColumnIndex++)
			{
				ColumnOffset = ColumnIndex * QuadWidth;

				// Pack QuadIndex value into two 8bits
				// decoding: HighByte*256 + LowByte
				uint8 HighByte = FMath::Floor(QuadIndex / 256.0f);
				uint8 LowByte = QuadIndex % 256;

				// Positions
				FVector CenterPosition(StartX + ColumnOffset, StartY + RowOffset, 1.0f + (QuadDistance * SampleIndex));
				FVector P1 = CenterPosition + (TopLeftDirection * QuadSize * QuadScale);
				FVector P2 = CenterPosition + (BottomLeftDirection * QuadSize * QuadScale);
				FVector P3 = CenterPosition + (BottomRightDirection * QuadSize * QuadScale);
				FVector P4 = CenterPosition + (TopRightDirection * QuadSize * QuadScale);

				Vertices.Add(P1);
				Vertices.Add(P2);
				Vertices.Add(P3);
				Vertices.Add(P4);

				// Triangles
				int IndexOffset = QuadCount * 4;
				Triangles.Add(0 + IndexOffset);
				Triangles.Add(2 + IndexOffset);
				Triangles.Add(1 + IndexOffset);
				Triangles.Add(0 + IndexOffset);
				Triangles.Add(3 + IndexOffset);
				Triangles.Add(2 + IndexOffset);

				for (int i = 0; i < 4; i++)
				{
					Normals.Add(Normal);
					Tangents.Add(Tangent);
					Colors.Add(FColor(255, 255, 255));
					UV1.Add(FVector2D(HighByte, LowByte));
					UV2.Add(FVector2D(1, 1));
				}

				// UVs to sample lens mask
				UV0.Add(TopLeftUV);
				UV0.Add(BottomLeftUV);
				UV0.Add(BottomRightUV);
				UV0.Add(TopRightUV);

				QuadIndex++;
				QuadCount++;
			}
		}
	}

	// Create mesh section 1
	MatrixHead->CreateMeshSection(1, Vertices, Triangles, Normals, UV0, UV1, UV2, UV0, Colors, Tangents, false);
}

void ADMXFixtureActorMatrix::GenerateMatrixChassis(FVector TL, FVector BL, FVector BR, FVector TR)
{
	// Create 5 faces to close matrix box
	FVector Depth(0, 0, MatrixDepth);
	FProcMeshTangent Tangent = FProcMeshTangent(1.0f, 0.0f, 0.0f);
	
	// bottom face
	AddQuad(TL-Depth, BL-Depth, BR-Depth, TR-Depth, Tangent);

	// side 1
	FVector P1 = BL;
	FVector P2 = BL - Depth;
	FVector P3 = BR - Depth;
	FVector P4 = BR;
	AddQuad(P1, P4, P3, P2, Tangent);

	// side 2
	P1 = TL;
	P2 = TL - Depth;
	P3 = BL - Depth;
	P4 = BL;
	AddQuad(P1, P4, P3, P2, Tangent);

	// side 3
	P1 = TR;
	P2 = TR - Depth;
	P3 = TL - Depth;
	P4 = TL;
	AddQuad(P1, P4, P3, P2, Tangent);

	// side 4
	P1 = BR;
	P2 = BR - Depth;
	P3 = TR - Depth;
	P4 = TR;
	AddQuad(P1, P4, P3, P2, Tangent);
}

void ADMXFixtureActorMatrix::AddQuad(FVector TL, FVector BL, FVector BR, FVector TR, FProcMeshTangent Tangent)
{
	Vertices.Add(TL);
	Vertices.Add(BL);
	Vertices.Add(BR);
	Vertices.Add(TR);

	int IndexOffset = QuadIndexCount * 4;
	Triangles.Add(0 + IndexOffset);
	Triangles.Add(2 + IndexOffset);
	Triangles.Add(1 + IndexOffset);
	Triangles.Add(0 + IndexOffset);
	Triangles.Add(3 + IndexOffset);
	Triangles.Add(2 + IndexOffset);

	FVector Normal = FVector::CrossProduct(TL-BR, TL-TR).GetSafeNormal();
	for (int i = 0; i < 4; i++)
	{
		Normals.Add(Normal);
		Tangents.Add(Tangent);
		Colors.Add(FColor(255, 255, 255));
		UV0.Add(FVector2D(0, 0));
		UV1.Add(FVector2D(0, 0));
		UV2.Add(FVector2D(0, 0)); // "chassis=0"
	}

	QuadIndexCount++;
}
