// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "RHI.h"
#include "RHIResources.h"

enum class EDMXPixelBlendingQuality : uint8;

class FTextureResource;
class FTextureRenderTargetResource;
class UTextureRenderTarget2D;
class UMaterialInstanceDynamic;
class UMaterialInterface;
class UTexture;
class UUserWidget;


/**
 * Used in shader permutation for determining number of samples to use in texture blending.
 * If adding to this you must also adjust the public facing option: 'EPixelBlendingQuality' under the runtime module's DMXPixelMappingOutputComponent.h
 */
enum class EDMXPixelShaderBlendingQuality : uint8
{
	Low,
	Medium,
	High,

	MAX
};

namespace UE::DMXPixelMapping
{
	/** Parameters for the Input Texture Renderer */
	struct FDMXPixelMappingInputTextureRenderingParameters
	{
		/** Number of times a texture is downsampled. E.g. when texture size is 512px and is downsampled 3 times, its resulting size is 64px */
		int32 NumDownsamplePasses = 0;

		/** The post process material. If null, no post process material is applied */
		UMaterialInstanceDynamic* PostProcessMID = nullptr;

		/** The input texture parameter name of the post process material */
		FName PostProcessMaterialInputTextureParameterName;

		/** The input texture parameter name of the post process material */
		FName BlurDistanceParameterName;

		/** The blur distance of the post process material */
		float BlurDistance = .2f;

		/**
		 * If true, applies post process material each downsample pass.
		 * If false applies the post process material once after the last downsample pass, or direct if the input is not downsampled.
		 * Only applicable if a post process material is set.
		 */
		bool bApplyPostProcessMaterialEachDownsamplePass = true;

		/** Size of the rendered texture */
		FVector2D OutputSize{ 1.f, 1.f };
	};

}

/**
 * Downsample pixel preview rendering params.
 * Using for pixel rendering setting in preview
 */
struct FDMXPixelMappingDownsamplePixelPreviewParam
{
	/** Position in screen pixels of the top left corner of the quad */
	FVector2D ScreenPixelPosition;

	/** Size in screen pixels of the quad */
	FVector2D ScreenPixelSize;

	/** Downsample pixel position in screen pixels of the quad */
	FIntPoint DownsamplePosition;
};

/**
 * Downsample pixel rendering params
 * Using for pixel rendering in downsample rendering pipeline
 */
struct UE_DEPRECATED(5.2, "Deprecated in favor of FDMXPixelMappingDownsamplePixelParamsV2. To apply color spaces, all color values are now computed at all times.") FDMXPixelMappingDownsamplePixelParam;
struct FDMXPixelMappingDownsamplePixelParam
{
	/** RGBA pixel multiplication */
	FVector4 PixelFactor;

	/** RGBA pixel flag for inversion */
	FIntVector4 InvertPixel;

	/** Position in screen pixels of the top left corner of the quad */
	FIntPoint Position;

	/** Position in texels of the top left corner of the quad's UV's */
	FVector2D UV;

	/** Size in texels of the quad's total UV space */
	FVector2D UVSize;

	/** Size in texels of UV.May match UVSize */
	FVector2D UVCellSize;

	/** The quality of color samples in the pixel shader(number of samples) */
	EDMXPixelBlendingQuality CellBlendingQuality;

	/** Calculates the UV point to sample purely on the UV position/size. Works best for renderers which represent a single pixel */
	bool bStaticCalculateUV;
};

struct FDMXPixelMappingDownsamplePixelParamsV2
{
	/** Position in screen pixels of the top left corner of the quad */
	FIntPoint Position;

	/** Position in texels of the top left corner of the quad's UV's */
	FVector2D UV;

	/** Size in texels of the quad's total UV space */
	FVector2D UVSize;

	/** Size in texels of UV.May match UVSize */
	FVector2D UVCellSize;

	/** The quality of color samples in the pixel shader(number of samples) */
	EDMXPixelBlendingQuality CellBlendingQuality;

	/** Calculates the UV point to sample purely on the UV position/size. Works best for renderers which represent a single pixel */
	bool bStaticCalculateUV;
};

struct FDMXPixelMappingRenderTextureParams
{
	int32 DownsampleTexture = 8;

	int32 NumDownSamplePasses = 1;
	float Distance = 0.2;
	int32 DistanceSteps = 1;
	int32 RadialSteps = 1;
	float RadialOffset = 1;
	int32 KernelPower = 5;
};




/**
 * The public interface of the Pixel Mapping renderer instance interface.
 */
class IDMXPixelMappingRenderer 
	: public TSharedFromThis<IDMXPixelMappingRenderer>
{
public:
	using DownsampleReadCallback = TFunction<void(TArray<FLinearColor>&&, FIntRect)>;

public:
	/** Virtual destructor */
	virtual ~IDMXPixelMappingRenderer() = default;
		
	/**
	 * Blurs input texture onto desination texture
	 *
	 * @param InputTexture						The input texture that is being processed
	 * @param Params							Parameters for post processing.
	 */
	virtual void PostProcessTexture(UTexture* InputTexture, const UE::DMXPixelMapping::FDMXPixelMappingInputTextureRenderingParameters& Params) const = 0;

	/** Gets the post processed texture. May return nullptr while the texture is not rendered yet. */
	virtual UTexture* GetPostProcessedTexture() const = 0;

	/**
	 * Pixelmapping specific, downsample and draw input texture to destination texture.
	 *
	 * @param InputTexture					Rendering resource of input texture
	 * @param DstTexture					Rendering resource of RenderTarget texture
	 * @param InDownsamplePixelPass			Pixels rendering params
	 * @param InCallback					Callback for reading  the pixels from GPU to CPU			
	 */
	 virtual void DownsampleRender(
		const FTextureResource* InputTexture,
		const FTextureResource* DstTexture,
		const FTextureRenderTargetResource* DstTextureTargetResource,
		const TArray<FDMXPixelMappingDownsamplePixelParamsV2>& InDownsamplePixelPass,
		DownsampleReadCallback InCallback
	) const = 0;

	/**
	 * Render material into the RenderTarget2D
	 *
	 * @param InRenderTarget				2D render target texture resource
	 * @param InMaterialInterface			Material to use
	 */
	virtual void RenderMaterial(UTextureRenderTarget2D* InRenderTarget, UMaterialInterface* InMaterialInterface) const = 0;

	/**
	 * Render material into the RenderTarget2D
	 *
	 * @param InRenderTarget				2D render target texture resource
	 * @param InUserWidget					UMG widget to use
	 */
	virtual void RenderWidget(UTextureRenderTarget2D* InRenderTarget, UUserWidget* InUserWidget) const  = 0;

	/**
	 * Rendering input texture to render target
	 *
	 * @param InTextureResource				Input texture resource
	 * @param InRenderTargetTexture			RenderTarget
	 * @param InSize						Rendering size
	 * @param bSRGBSource					If the source texture is sRGB
	 */
	virtual void RenderTextureToRectangle(const FTextureResource* InTextureResource, const FTexture2DRHIRef InRenderTargetTexture, FVector2D InSize, bool bSRGBSource) const = 0;

#if WITH_EDITOR
	/**
	 * Render preview with one or multiple downsampled textures
	 *
	 * @param TextureResource				Rendering resource of RenderTarget texture
	 * @param DownsampleResource			Rendering resource of RenderTarget texture
	 * @param InPixelPreviewParamSet		Pixels rendering params
	 */
	virtual void RenderPreview(const FTextureResource* TextureResource, const FTextureResource* DownsampleResource, TArray<FDMXPixelMappingDownsamplePixelPreviewParam>&& InPixelPreviewParamSet) const = 0;
#endif // WITH_EDITOR

	/**
	* Sets the brigthness of the renderer
	*/
	void SetBrightness(const float InBrightness) { Brightness = InBrightness; }

protected:
	/** Brightness multiplier for the renderer */
	float Brightness = 1.0f;
};
