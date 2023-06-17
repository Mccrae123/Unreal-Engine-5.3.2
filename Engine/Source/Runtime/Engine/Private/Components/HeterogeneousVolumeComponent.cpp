// Copyright Epic Games, Inc. All Rights Reserved.

#include "Components/HeterogeneousVolumeComponent.h"

#include "Components/BillboardComponent.h"
#include "Engine/Texture2D.h"
#include "HeterogeneousVolumeInterface.h"
#include "LocalVertexFactory.h"
#include "MaterialDomain.h"
#include "Materials/Material.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/MaterialInterface.h"
#include "PrimitiveViewRelevance.h"
#include "PrimitiveSceneProxy.h"
#include "SceneInterface.h"
#include "SceneManagement.h"
#include "SceneView.h"
#include "SparseVolumeTexture/SparseVolumeTexture.h"
#include "StaticMeshResources.h"
#include "UObject/ConstructorHelpers.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(HeterogeneousVolumeComponent)

#define LOCTEXT_NAMESPACE "HeterogeneousVolumeComponent"

class FHeterogeneousVolumeSceneProxy : public FPrimitiveSceneProxy
{
public:
	FHeterogeneousVolumeSceneProxy(UHeterogeneousVolumeComponent* InComponent);
	virtual ~FHeterogeneousVolumeSceneProxy();

	//~ Begin FPrimitiveSceneProxy Interface.
	virtual FPrimitiveViewRelevance GetViewRelevance(const FSceneView* View) const override;

	virtual void GetDynamicMeshElements(
		const TArray<const FSceneView*>& Views,
		const FSceneViewFamily& ViewFamily,
		uint32 VisibilityMap,
		FMeshElementCollector& Collector) const override;

	virtual SIZE_T GetTypeHash() const override
	{
		static size_t UniquePointer;
		return reinterpret_cast<size_t>(&UniquePointer);
	}

	virtual uint32 GetMemoryFootprint(void) const override { return sizeof(*this) + GetAllocatedSize(); }
	uint32 GetAllocatedSize(void) const { return FPrimitiveSceneProxy::GetAllocatedSize(); }
	//~ End FPrimitiveSceneProxy Interface.

private:
	UMaterialInterface* MaterialInterface;
	FLocalVertexFactory VertexFactory;
	FStaticMeshVertexBuffers StaticMeshVertexBuffers;
	FHeterogeneousVolumeData HeterogeneousVolumeData;
};

/*=============================================================================
	FHeterogeneousVolumeSceneProxy implementation.
=============================================================================*/

FHeterogeneousVolumeSceneProxy::FHeterogeneousVolumeSceneProxy(UHeterogeneousVolumeComponent* InComponent)
	: FPrimitiveSceneProxy(InComponent)
	, MaterialInterface(InComponent->GetMaterial(0))
	, VertexFactory(GetScene().GetFeatureLevel(), "FHeterogeneousVolumeSceneProxy")
	, HeterogeneousVolumeData(this)
{
	bIsHeterogeneousVolume = true;

	HeterogeneousVolumeData.VoxelResolution = InComponent->VolumeResolution;

	// Infer minimum voxel size from bounds and resolution
	FVector VoxelSize = 2.0 * InComponent->Bounds.BoxExtent;
	VoxelSize.X /= InComponent->VolumeResolution.X;
	VoxelSize.Y /= InComponent->VolumeResolution.Y;
	VoxelSize.Z /= InComponent->VolumeResolution.Z;
	HeterogeneousVolumeData.MinimumVoxelSize = FMath::Max(VoxelSize.GetMin(), 0.001);

	if (InComponent->bAnimate && InComponent->MaterialInstanceDynamic)
	{
		MaterialInterface = InComponent->MaterialInstanceDynamic;
	}

	HeterogeneousVolumeData.LightingDownsampleFactor = InComponent->LightingDownsampleFactor;

	// Initialize vertex buffer data for a quad
	StaticMeshVertexBuffers.PositionVertexBuffer.Init(4);
	StaticMeshVertexBuffers.StaticMeshVertexBuffer.Init(4, 1);
	StaticMeshVertexBuffers.ColorVertexBuffer.Init(4);

	for (uint32 VertexIndex = 0; VertexIndex < 4; ++VertexIndex)
	{
		StaticMeshVertexBuffers.ColorVertexBuffer.VertexColor(VertexIndex) = FColor::White;
	}

	StaticMeshVertexBuffers.PositionVertexBuffer.VertexPosition(0) = FVector3f(-1.0, -1.0, -1.0);
	StaticMeshVertexBuffers.PositionVertexBuffer.VertexPosition(1) = FVector3f(-1.0, 1.0, -1.0);
	StaticMeshVertexBuffers.PositionVertexBuffer.VertexPosition(2) = FVector3f(1.0, -1.0, -1.0);
	StaticMeshVertexBuffers.PositionVertexBuffer.VertexPosition(3) = FVector3f(1.0, 1.0, -1.0);

	StaticMeshVertexBuffers.StaticMeshVertexBuffer.SetVertexUV(0, 0, FVector2f(0, 0));
	StaticMeshVertexBuffers.StaticMeshVertexBuffer.SetVertexUV(1, 0, FVector2f(0, 1));
	StaticMeshVertexBuffers.StaticMeshVertexBuffer.SetVertexUV(2, 0, FVector2f(1, 0));
	StaticMeshVertexBuffers.StaticMeshVertexBuffer.SetVertexUV(3, 0, FVector2f(1, 1));

	FHeterogeneousVolumeSceneProxy* Self = this;
	ENQUEUE_RENDER_COMMAND(FHeterogeneousVolumeSceneProxyInit)(
		[Self](FRHICommandListImmediate& RHICmdList)
		{
			Self->StaticMeshVertexBuffers.PositionVertexBuffer.InitResource(RHICmdList);
			Self->StaticMeshVertexBuffers.StaticMeshVertexBuffer.InitResource(RHICmdList);
			Self->StaticMeshVertexBuffers.ColorVertexBuffer.InitResource(RHICmdList);

			FLocalVertexFactory::FDataType Data;
			Self->StaticMeshVertexBuffers.PositionVertexBuffer.BindPositionVertexBuffer(&Self->VertexFactory, Data);
			Self->StaticMeshVertexBuffers.StaticMeshVertexBuffer.BindTangentVertexBuffer(&Self->VertexFactory, Data);
			Self->StaticMeshVertexBuffers.StaticMeshVertexBuffer.BindPackedTexCoordVertexBuffer(&Self->VertexFactory, Data);
			Self->StaticMeshVertexBuffers.StaticMeshVertexBuffer.BindLightMapVertexBuffer(&Self->VertexFactory, Data, 0);
			Self->StaticMeshVertexBuffers.ColorVertexBuffer.BindColorVertexBuffer(&Self->VertexFactory, Data);
			Self->VertexFactory.SetData(Data);

			Self->VertexFactory.InitResource(RHICmdList);
		}
	);
}

/** Virtual destructor. */
FHeterogeneousVolumeSceneProxy::~FHeterogeneousVolumeSceneProxy()
{
	VertexFactory.ReleaseResource();
	StaticMeshVertexBuffers.PositionVertexBuffer.ReleaseResource();
	StaticMeshVertexBuffers.StaticMeshVertexBuffer.ReleaseResource();
	StaticMeshVertexBuffers.ColorVertexBuffer.ReleaseResource();
}

FPrimitiveViewRelevance FHeterogeneousVolumeSceneProxy::GetViewRelevance(const FSceneView* View) const
{
	FPrimitiveViewRelevance Result;

	if (MaterialInterface)
	{
		FMaterialRelevance MaterialRelevance = MaterialInterface->GetRelevance_Concurrent(View->GetFeatureLevel());
		MaterialRelevance.SetPrimitiveViewRelevance(Result);
	}

	Result.bDrawRelevance = IsShown(View);
	Result.bOpaque = false;
	Result.bStaticRelevance = false;
	Result.bDynamicRelevance = true;
	Result.bShadowRelevance = IsShadowCast(View);
	Result.bRenderInMainPass = ShouldRenderInMainPass();

	return Result;
}

void FHeterogeneousVolumeSceneProxy::GetDynamicMeshElements(
	const TArray<const FSceneView*>& Views,
	const FSceneViewFamily& ViewFamily,
	uint32 VisibilityMap,
	FMeshElementCollector& Collector) const
{
	check(IsInRenderingThread());

	// Create a dummy MeshBatch to make the system happy..
	if (MaterialInterface)
	{
		// Set up MeshBatch
		FMeshBatch& Mesh = Collector.AllocateMesh();

		Mesh.VertexFactory = &VertexFactory;
		Mesh.MaterialRenderProxy = MaterialInterface->GetRenderProxy();
		Mesh.LCI = NULL;
		Mesh.ReverseCulling = IsLocalToWorldDeterminantNegative() ? true : false;
		Mesh.CastShadow = false;
		//Mesh.DepthPriorityGroup = (ESceneDepthPriorityGroup)GetDepthPriorityGroup(View);
		Mesh.Type = PT_TriangleStrip;
		Mesh.bDisableBackfaceCulling = true;

		// Set up the FMeshBatchElement.
		FMeshBatchElement& BatchElement = Mesh.Elements[0];
		BatchElement.IndexBuffer = NULL;
		BatchElement.FirstIndex = 0;
		BatchElement.MinVertexIndex = 0;
		BatchElement.MaxVertexIndex = 3;
		BatchElement.NumPrimitives = 2;
		BatchElement.BaseVertexIndex = 0;

		//FHeterogeneousVolumeData* HeterogeneousVolumeData = &Collector.AllocateOneFrameResource<FHeterogeneousVolumeData>(this);
		BatchElement.UserData = &HeterogeneousVolumeData;

		Mesh.bCanApplyViewModeOverrides = true;
		Mesh.bUseWireframeSelectionColoring = IsSelected();

		Collector.AddMesh(0, Mesh);
	}
}

/*=============================================================================
	HeterogeneousVolumeComponent implementation.
=============================================================================*/

UHeterogeneousVolumeComponent::UHeterogeneousVolumeComponent(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	PrimaryComponentTick.bCanEverTick = true;
	// What is this?
	PrimaryComponentTick.TickGroup = TG_DuringPhysics;

#if WITH_EDITORONLY_DATA
	bTickInEditor = true;
#endif // WITH_EDITORONLY_DATA

	MaterialInstanceDynamic = nullptr;
	VolumeResolution = FIntVector(128);
	Framerate = 24.0f;
	bAnimate = false;
	LightingDownsampleFactor = 1.0f;
	Time = 0.0f;
}

FPrimitiveSceneProxy* UHeterogeneousVolumeComponent::CreateSceneProxy()
{
	return new FHeterogeneousVolumeSceneProxy(this);
}

FBoxSphereBounds UHeterogeneousVolumeComponent::CalcBounds(const FTransform& LocalToWorld) const
{
	FBoxSphereBounds NewBounds;
	NewBounds.Origin = FVector::ZeroVector;
	NewBounds.BoxExtent = FVector(50, 50, 50);
	NewBounds.SphereRadius = NewBounds.BoxExtent.Length();

	return NewBounds.TransformBy(LocalToWorld);
}

void UHeterogeneousVolumeComponent::PostLoad()
{
	Super::PostLoad();

	UMaterialInterface* MaterialInterface = GetMaterial(0);
	if (MaterialInterface)
	{
		const UMaterial* Material = MaterialInterface->GetMaterial();
		if (Material && Material->MaterialDomain == EMaterialDomain::MD_Volume)
		{
			Material->GetRenderProxy();
			MaterialInterface->CheckMaterialUsage(MATUSAGE_HeterogeneousVolumes);
		}

		MaterialInstanceDynamic = UMaterialInstanceDynamic::Create(MaterialInterface, nullptr);
	}
}

#if WITH_EDITOR
void UHeterogeneousVolumeComponent::PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent)
{
	Super::PostEditChangeProperty(PropertyChangedEvent);

	FName PropertyName;
	if (PropertyChangedEvent.Property)
	{
		PropertyName = PropertyChangedEvent.Property->GetFName();
	}

	if (PropertyName == GET_MEMBER_NAME_CHECKED(UHeterogeneousVolumeComponent, OverrideMaterials))
	{
		UMaterialInterface* MaterialInterface = GetMaterial(0);
		if (MaterialInterface)
		{
			const UMaterial* Material = MaterialInterface->GetMaterial();
			if (Material && Material->MaterialDomain == EMaterialDomain::MD_Volume)
			{
				Material->GetRenderProxy();
				MaterialInterface->CheckMaterialUsage(MATUSAGE_HeterogeneousVolumes);
			}

			MaterialInstanceDynamic = UMaterialInstanceDynamic::Create(MaterialInterface, nullptr);
		}
	}
}
#endif // WITH_EDITOR

void UHeterogeneousVolumeComponent::GetUsedMaterials(TArray<UMaterialInterface*>& OutMaterials, bool bGetDebugMaterials) const
{
	UMeshComponent::GetUsedMaterials(OutMaterials, bGetDebugMaterials);
	
	if (MaterialInstanceDynamic)
	{
		OutMaterials.Add(MaterialInstanceDynamic);
	}
}

void UHeterogeneousVolumeComponent::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);

	uint32 MaterialIndex = 0;
	UMaterialInterface* Material = UMeshComponent::GetMaterial(MaterialIndex);
	if (Material)
	{
		// Get all SVT params in the material
		TArray<FMaterialParameterInfo> SVTParameterInfo;
		TArray<FGuid> SVTParameterIds;
		Material->GetAllSparseVolumeTextureParameterInfo(SVTParameterInfo, SVTParameterIds);

		if (!SVTParameterInfo.IsEmpty())
		{
			USparseVolumeTexture* DefaultSVT = nullptr;
			bool bValid = Material->GetSparseVolumeTextureParameterDefaultValue(SVTParameterInfo[MaterialIndex], DefaultSVT);
			if (DefaultSVT)
			{
				const int32 FrameCount = DefaultSVT->GetNumFrames();
				const float FrameIndexF = bAnimate ? FMath::Fmod(Time * Framerate, (float)FrameCount) : 0;
				const int32 MipLevel = 0;
				bool bIsBlocking = false;
				USparseVolumeTextureFrame* SVTFrame = USparseVolumeTextureFrame::GetFrameAndIssueStreamingRequest(DefaultSVT, FrameIndexF, MipLevel, bIsBlocking);
				VolumeResolution = SVTFrame ? SVTFrame->GetVolumeResolution() : FIntVector(1);

				if (bAnimate && MaterialInstanceDynamic)
				{
					MaterialInstanceDynamic->SetSparseVolumeTextureParameterValue(SVTParameterInfo[MaterialIndex].Name, SVTFrame);
				}
			}
		}
	}

	Time += DeltaTime;
}

AHeterogeneousVolume::AHeterogeneousVolume(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	HeterogeneousVolumeComponent = CreateDefaultSubobject<UHeterogeneousVolumeComponent>(TEXT("HeterogeneousVolumeComponent"));
	RootComponent = HeterogeneousVolumeComponent;

#if WITH_EDITORONLY_DATA

	if (!IsRunningCommandlet())
	{
		// Structure to hold one-time initialization
		struct FConstructorStatics
		{
			FName ID_HeterogeneousVolume;
			FText NAME_HeterogeneousVolume;
			FConstructorStatics()
				: ID_HeterogeneousVolume(TEXT("Fog"))
				, NAME_HeterogeneousVolume(NSLOCTEXT("SpriteCategory", "Fog", "Fog"))
			{
			}
		};
		static FConstructorStatics ConstructorStatics;

		if (GetSpriteComponent())
		{
			GetSpriteComponent()->SetRelativeScale3D(FVector(0.5f, 0.5f, 0.5f));
			GetSpriteComponent()->SpriteInfo.Category = ConstructorStatics.ID_HeterogeneousVolume;
			GetSpriteComponent()->SpriteInfo.DisplayName = ConstructorStatics.NAME_HeterogeneousVolume;
			GetSpriteComponent()->SetupAttachment(HeterogeneousVolumeComponent);
		}
	}
#endif // WITH_EDITORONLY_DATA

	PrimaryActorTick.bCanEverTick = true;
	SetHidden(false);
}

#undef LOCTEXT_NAMESPACE