// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "MuR/ImagePrivate.h"
#include "MuR/MutableTrace.h"
#include "Async/ParallelFor.h"


namespace mu
{

    //---------------------------------------------------------------------------------------------
    //! Point-filter image resize
    //! TODO: Optimise
    //---------------------------------------------------------------------------------------------
    inline Ptr<Image> ImageResize( const Image* pBase, FImageSize destSize )
    {
		MUTABLE_CPUPROFILER_SCOPE(ImageResizePoint)

		check( !(pBase->m_flags&Image::IF_CANNOT_BE_SCALED) );

        FImageSize baseSize = FImageSize( pBase->GetSizeX(), pBase->GetSizeY() );

        ImagePtr pDest = new Image( destSize[0], destSize[1], 1, pBase->GetFormat() );

        switch ( pBase->GetFormat() )
        {

        case EImageFormat::IF_L_UBYTE:
        {
            const uint8_t* pBaseBuf = pBase->GetData();
            uint8_t* pDestBuf = pDest->GetData();

			// Simple nearest pixel implementation
			uint32 dx_16 = (uint32(baseSize[0]) << 16) / destSize[0];
			uint32 dy_16 = (uint32(baseSize[1]) << 16) / destSize[1];
			uint32 sy_16 = 0;
			for (uint16 y = 0; y < destSize[1]; ++y)
			{
				uint32 sy = sy_16 >> 16;
				const uint8_t* pRowBuf = pBase->GetData() + baseSize[0] * sy;

				uint32 sx_16 = 0;
				for (uint16 x = 0; x < destSize[0]; ++x)
				{
					uint32 sx = sx_16 >> 16;
					*pDestBuf = pRowBuf[sx];
					pDestBuf++;

					sx_16 += dx_16;
				}

				sy_16 += dy_16;
			}

            break;
        }

        case EImageFormat::IF_RGB_UBYTE:
        {
            const uint8_t* pBaseBuf = pBase->GetData();
            uint8_t* pDestBuf = pDest->GetData();

            // Simple nearest pixel implementation
            for ( int y=0; y<destSize[1]; ++y )
            {
                int sy = (y*baseSize[1])/destSize[1];

                for ( int x=0; x<destSize[0]; ++x )
                {
                    int sx = (x*baseSize[0])/destSize[0];
                    pDestBuf[0] = pBaseBuf[(sy*baseSize[0]+sx)*3+0];
                    pDestBuf[1] = pBaseBuf[(sy*baseSize[0]+sx)*3+1];
                    pDestBuf[2] = pBaseBuf[(sy*baseSize[0]+sx)*3+2];
                    pDestBuf+=3;
                }
            }

            break;
        }

        case EImageFormat::IF_BGRA_UBYTE:
        case EImageFormat::IF_RGBA_UBYTE:
        {
            const uint8_t* pBaseBuf = pBase->GetData();
            uint8_t* pDestBuf = pDest->GetData();

            // Simple nearest pixel implementation
            for ( int y=0; y<destSize[1]; ++y )
            {
                int sy = (y*baseSize[1])/destSize[1];

                for ( int x=0; x<destSize[0]; ++x )
                {
                    int sx = (x*baseSize[0])/destSize[0];
                    pDestBuf[0] = pBaseBuf[(sy*baseSize[0]+sx)*4+0];
                    pDestBuf[1] = pBaseBuf[(sy*baseSize[0]+sx)*4+1];
                    pDestBuf[2] = pBaseBuf[(sy*baseSize[0]+sx)*4+2];
                    pDestBuf[3] = pBaseBuf[(sy*baseSize[0]+sx)*4+3];
                    pDestBuf+=4;
                }
            }

            break;
        }

        default:
            // Case not implemented
            check( false );
            //mu::Halt();
        }

        return pDest;
    }



    //---------------------------------------------------------------------------------------------
    //!
    //---------------------------------------------------------------------------------------------
    template< int NC >
    void ImageMagnifyX( Image* pDest, const Image* pBase )
    {
        int baseSizeX = pBase->GetSizeX();
        int destSizeX = pDest->GetSizeX();
        int sizeY = pBase->GetSizeY();

        uint32_t dx_16 = (uint32_t(baseSizeX)<<16) / destSizeX;

        // Linear filtering
        //for ( int y=0; y<sizeY; ++y )
		const auto& ProcessLine = [
			pDest, pBase, dx_16, baseSizeX, destSizeX
		] (uint32 y)
		{
			uint32_t px_16 = 0;
			const uint8_t* pBaseBuf = pBase->GetData() + y * baseSizeX * NC;
			uint8_t* pDestBuf = pDest->GetData() + y * destSizeX * NC;

			for (int x = 0; x < destSizeX; ++x)
			{
				uint32_t px = px_16 >> 16;
				uint32_t epx_16 = px_16 + dx_16;

				if ((px_16 & 0xffff0000) == ((epx_16 - 1) & 0xffff0000))
				{
					// One fraction
					for (int c = 0; c < NC; ++c)
					{
						pDestBuf[c] += pBaseBuf[px * NC + c];
					}
				}
				else
				{
					// Two fractions
					uint32_t frac1 = (px_16 & 0xffff);
					uint32_t frac0 = 0x10000 - frac1;

					for (int c = 0; c < NC; ++c)
					{
						pDestBuf[c] = uint8_t( (pBaseBuf[px * NC + c] * frac0 + pBaseBuf[(px + 1) * NC + c] * frac1) >> 16 );
					}

					++px;
				}

				px_16 = epx_16;
				pDestBuf += NC;
			}
		};

		ParallelFor(sizeY, ProcessLine);
    }


    inline void ImageMagnifyX( Image* pDest, const Image* pBase )
    {
		MUTABLE_CPUPROFILER_SCOPE(ImageMagnifyX)

        check( pDest->GetSizeY() == pBase->GetSizeY() );
        check( pDest->GetSizeX() > pBase->GetSizeX() );

        switch ( pBase->GetFormat() )
        {

        case EImageFormat::IF_L_UBYTE:
        {
            ImageMagnifyX<1>( pDest, pBase );
            break;
        }

        case EImageFormat::IF_RGB_UBYTE:
        {
            ImageMagnifyX<3>( pDest, pBase );
            break;
        }

        case EImageFormat::IF_BGRA_UBYTE:
        case EImageFormat::IF_RGBA_UBYTE:
        {
            ImageMagnifyX<4>( pDest, pBase );
            break;
        }

        default:
            // Case not implemented
            check( false );
        }
    }


    //---------------------------------------------------------------------------------------------
    //! General image minimisation
    //---------------------------------------------------------------------------------------------
    template< int NC >
    void ImageMinifyX( Image* pDest, const Image* pBase )
    {
        int baseSizeX = pBase->GetSizeX();
        int destSizeX = pDest->GetSizeX();
        int sizeY = pBase->GetSizeY();

        uint32_t dx_16 = (uint32_t(baseSizeX)<<16) / destSizeX;

        // Linear filtering
        //for ( int y=0; y<sizeY; ++y )
		const auto& ProcessLine = [
			pDest, pBase, dx_16, baseSizeX, destSizeX
		] (uint32 y)
		{
			const uint8_t* pBaseBuf = pBase->GetData() + y * baseSizeX * NC;
			uint8_t* pDestBuf = pDest->GetData() + y * destSizeX * NC;

			uint32_t px_16 = 0;
			for (int x = 0; x < destSizeX; ++x)
			{
				uint32_t r_16[NC];
				for (int c = 0; c < NC; ++c)
				{
					r_16[c] = 0;
				}

				uint32_t epx_16 = px_16 + dx_16;
				uint32_t px = px_16 >> 16;
				uint32_t epx = epx_16 >> 16;

				// First fraction
				uint32_t frac0 = px_16 & 0xffff;
				if (frac0)
				{
					for (int c = 0; c < NC; ++c)
					{
						r_16[c] += (0x10000 - frac0) * pBaseBuf[px * NC + c];
					}

					++px;
				}

				// Whole pixels
				while (px < epx)
				{
					for (int c = 0; c < NC; ++c)
					{
						r_16[c] += uint32_t(pBaseBuf[px * NC + c]) << 16;
					}

					++px;
				}

				// Second fraction
				uint32_t frac1 = epx_16 & 0xffff;
				if (frac1)
				{
					for (int c = 0; c < NC; ++c)
					{
						r_16[c] += frac1 * pBaseBuf[px * NC + c];
					}
				}

				for (int c = 0; c < NC; ++c)
				{
					pDestBuf[c] = (uint8_t)(r_16[c] / dx_16);
				}

				px_16 = epx_16;
				pDestBuf += NC;
			}
		};

		ParallelFor(sizeY, ProcessLine);
    }


    //---------------------------------------------------------------------------------------------
    //! Optimised for whole factors
    //---------------------------------------------------------------------------------------------
    template< int NC, int FACTOR >
    void ImageMinifyX_Exact( Image* pDest, const Image* pBase )
    {
		int baseSizeX = pBase->GetSizeX();
        int destSizeX = pDest->GetSizeX();
        int sizeY = pBase->GetSizeY();

        const uint8_t* pBaseBuf = pBase->GetData();
        uint8_t* pDestBuf = pDest->GetData();

        // Linear filtering
		const auto& ProcessLine = [
			pDest, pBase, baseSizeX, destSizeX
		] (uint32 y)
		{
			const uint8_t* pBaseBuf = pBase->GetData() + y * baseSizeX * NC;
			uint8_t* pDestBuf = pDest->GetData() + y * destSizeX * NC;

			uint32_t r[NC];
			for (int x = 0; x < destSizeX; ++x)
			{
				for (int c = 0; c < NC; ++c)
				{
					r[c] = 0;
					for (int a = 0; a < FACTOR; ++a)
					{
						r[c] += pBaseBuf[a * NC + c];
					}
				}

				for (int c = 0; c < NC; ++c)
				{
					pDestBuf[c] = (uint8_t)(r[c] / FACTOR);
				}

				pDestBuf += NC;
				pBaseBuf += NC * FACTOR;
			}
		};

		ParallelFor(sizeY, ProcessLine);
    }

	inline uint32 AverageChannel(uint32 a, uint32 b)
	{
		uint32 r = (a + b) >> 1;
		return r;
	}

	template<>
	inline void ImageMinifyX_Exact<4,2>(Image* pDest, const Image* pBase)
	{
		int baseSizeX = pBase->GetSizeX();
		int destSizeX = pDest->GetSizeX();
		int sizeY = pBase->GetSizeY();

		const uint8* pBaseBuf = pBase->GetData();
		uint8* pDestBuf = pDest->GetData();

		int32 TotalBasePixels = baseSizeX * sizeY;
		constexpr int32 BasePixelsPerBatch = 4096 * 2;
		int32 NumBatches = FMath::DivideAndRoundUp(TotalBasePixels, BasePixelsPerBatch);

		// Linear filtering
		const auto& ProcessBatchUnaligned = 
			[ pDestBuf, pBaseBuf, baseSizeX, destSizeX, BasePixelsPerBatch, TotalBasePixels ]
		(int32 BatchIndex)
		{
			const uint8* pBatchBaseBuf = pBaseBuf + BatchIndex * BasePixelsPerBatch * 4;
			uint8* pBatchDestBuf = pDestBuf + BatchIndex * BasePixelsPerBatch * 4/2;

			int32 NumBasePixels = FMath::Min(BasePixelsPerBatch, TotalBasePixels-BatchIndex* BasePixelsPerBatch );

			uint16 r[4];
			for (int x = 0; x < NumBasePixels; x+=2)
			{
				r[0] = pBatchBaseBuf[0] + pBatchBaseBuf[0 + 4];
				r[1] = pBatchBaseBuf[1] + pBatchBaseBuf[1 + 4];
				r[2] = pBatchBaseBuf[2] + pBatchBaseBuf[2 + 4];
				r[3] = pBatchBaseBuf[3] + pBatchBaseBuf[3 + 4];

				pBatchDestBuf[0] = (uint8)(r[0] >> 1);
				pBatchDestBuf[1] = (uint8)(r[1] >> 1);
				pBatchDestBuf[2] = (uint8)(r[2] >> 1);
				pBatchDestBuf[3] = (uint8)(r[3] >> 1);

				pBatchBaseBuf += 4 * 2;
				pBatchDestBuf += 4;
			}
		};

		const auto& ProcessBatchAligned =
			[pDestBuf, pBaseBuf, baseSizeX, destSizeX, BasePixelsPerBatch, TotalBasePixels]
		(int32 BatchIndex)
		{
			const uint32* pBatchBaseBuf = reinterpret_cast<const uint32*>(pBaseBuf) + BatchIndex * BasePixelsPerBatch;
			uint32* pBatchDestBuf = reinterpret_cast<uint32*>(pDestBuf) + BatchIndex * (BasePixelsPerBatch>>1);

			int32 NumBasePixels = FMath::Min(BasePixelsPerBatch, TotalBasePixels - BatchIndex * BasePixelsPerBatch);

			for (int p=0; p<NumBasePixels;++p)
			{
				uint32 FullSource0 = pBatchBaseBuf[p*2+0];
				uint32 FullSource1 = pBatchBaseBuf[p*2+1];

				uint32 FullResult = 0;

				FullResult |= AverageChannel((FullSource0 >>  0) & 0xff, (FullSource1 >>  0) & 0xff)  <<  0;
				FullResult |= AverageChannel((FullSource0 >>  8) & 0xff, (FullSource1 >>  8) & 0xff)  <<  8;
				FullResult |= AverageChannel((FullSource0 >> 16) & 0xff, (FullSource1 >> 16) & 0xff)  << 16;
				FullResult |= AverageChannel((FullSource0 >> 24) & 0xff, (FullSource1 >> 24) & 0xff)  << 24;

				pBatchDestBuf[p] = FullResult;
			}
		};

//		if ( (SIZE_T(pBaseBuf) & 0x3) || (SIZE_T(pDestBuf) & 0x3) )
		{
			ParallelFor(NumBatches, ProcessBatchUnaligned);
		}
		/*else
		{
			ParallelFor(NumBatches, ProcessBatchAligned);
		}*/
	}


    //---------------------------------------------------------------------------------------------
    //! Image minify X version hub.
    //---------------------------------------------------------------------------------------------
    inline void ImageMinifyX( Image* pDest, const Image* pBase )
    {
		MUTABLE_CPUPROFILER_SCOPE(ImageMinifyX)

        check( pDest->GetSizeY() == pBase->GetSizeY() );
        check( pDest->GetSizeX() < pBase->GetSizeX() );

        switch ( pBase->GetFormat() )
        {

        case EImageFormat::IF_L_UBYTE:
        {
            if ( 2*pDest->GetSizeX()==pBase->GetSizeX() )
            {
                // Optimised case
                ImageMinifyX_Exact<1,2>( pDest, pBase );
            }
            else
            {
                // Generic case
                ImageMinifyX<1>( pDest, pBase );
            }
            break;
        }

        case EImageFormat::IF_RGB_UBYTE:
        {
            if ( 2*pDest->GetSizeX()==pBase->GetSizeX() )
            {
                // Optimised case
                ImageMinifyX_Exact<3,2>( pDest, pBase );
            }
            else
            {
                // Generic case
                ImageMinifyX<3>( pDest, pBase );
            }
            break;
        }

        case EImageFormat::IF_BGRA_UBYTE:
        case EImageFormat::IF_RGBA_UBYTE:
        {
            if ( 2*pDest->GetSizeX()==pBase->GetSizeX() )
            {
                // Optimised case
                ImageMinifyX_Exact<4,2>( pDest, pBase );
            }
            else if (4 * pDest->GetSizeX() == pBase->GetSizeX())
			{
				// Optimised case
				ImageMinifyX_Exact<4, 4>(pDest, pBase);
			}
			else
            {
                // Generic case
                ImageMinifyX<4>( pDest, pBase );
            }
            break;
        }

        default:
            // Case not implemented
            check( false );
        }

    }


    //---------------------------------------------------------------------------------------------
    //!
    //---------------------------------------------------------------------------------------------
    template< int NC >
    void ImageMagnifyY( Image* pDest, const Image* pBase )
    {
		if (!pBase || !pDest || 
			!pBase->GetSizeX() || !pBase->GetSizeY() || !pDest->GetSizeX() || !pDest->GetSizeY())
		{
			return;
		}
			
		int baseSizeY = pBase->GetSizeY();
        int destSizeY = pDest->GetSizeY();
        int sizeX = pBase->GetSizeX();

        size_t rowSize = sizeX * NC;

        // Common case, optimised.
        if (destSizeY==baseSizeY*2)
        {
			//for (int y = 0; y < baseSizeY; ++y)
			const auto& ProcessLine = [
				pDest, pBase, rowSize
			] (uint32 y)
            {
				uint8_t* pDestBuf = pDest->GetData()+ 2*y*rowSize;
				const uint8_t* pBaseBuf = pBase->GetData() + y * rowSize;

                memcpy( pDestBuf, pBaseBuf, rowSize );
                pDestBuf += rowSize;

                memcpy( pDestBuf, pBaseBuf, rowSize );
            };

			ParallelFor(baseSizeY, ProcessLine);
        }
        else
        {
            uint32_t dy_16 = ( uint32_t( baseSizeY ) << 16 ) / destSizeY;

            // Linear filtering
            // \todo: optimise: swap loops, etc.
            //for ( int x=0; x<sizeX; ++x )
			const auto& ProcessColumn = [
				pDest, pBase, sizeX, destSizeY, dy_16
			] (uint32 x)
            {
                uint32_t py_16 = 0;
                uint8_t* pDestBuf = pDest->GetData()+x*NC;
				const uint8_t* pBaseBuf = pBase->GetData();

                for ( int y=0; y<destSizeY; ++y )
                {
                    uint32_t py = py_16 >> 16;
                    uint32_t epy_16 = py_16+dy_16;

                    if ( (py_16 & 0xffff0000) == ((epy_16-1) & 0xffff0000) )
                    {
                        // One fraction
                        for ( int c=0; c<NC; ++c )
                        {
                            pDestBuf[c] += pBaseBuf[(py*sizeX+x)*NC+c];
                        }
                    }
                    else
                    {
                        // Two fractions
                        uint32_t frac1 = (py_16 & 0xffff);
                        uint32_t frac0 = 0x10000 - frac1;

                        for ( int c=0; c<NC; ++c )
                        {
                            pDestBuf[c] = (uint8_t)( ( pBaseBuf[(py*sizeX+x)*NC+c] * frac0 +
                                            pBaseBuf[((py+1)*sizeX+x)*NC+c] * frac1
                                            ) >> 16 );
                        }

                        ++py;
                    }

                    py_16 = epy_16;
                    pDestBuf+=sizeX*NC;
                }
            };

			ParallelFor(sizeX, ProcessColumn);
        }
    }


    inline void ImageMagnifyY( Image* pDest, const Image* pBase )
    {
        check( pDest->GetSizeY() > pBase->GetSizeY() );
        check( pDest->GetSizeX() == pBase->GetSizeX() );

		MUTABLE_CPUPROFILER_SCOPE(ImageMagnifyY)

        switch ( pBase->GetFormat() )
        {

        case EImageFormat::IF_L_UBYTE:
        {
            ImageMagnifyY<1>( pDest, pBase );
            break;
        }

        case EImageFormat::IF_RGB_UBYTE:
        {
            ImageMagnifyY<3>( pDest, pBase );
            break;
        }

        case EImageFormat::IF_RGBA_UBYTE:
        case EImageFormat::IF_BGRA_UBYTE:
        {
            ImageMagnifyY<4>( pDest, pBase );
            break;
        }

        default:
            // Case not implemented
            check( false );
        }
    }


    //---------------------------------------------------------------------------------------------
    //!
    //---------------------------------------------------------------------------------------------
    template< int NC >
    void ImageMinifyY( Image* pDest, const Image* pBase )
    {
        int baseSizeY = pBase->GetSizeY();
        int destSizeY = pDest->GetSizeY();
        int sizeX = pBase->GetSizeX();

        uint32 dy_16 = (uint32(baseSizeY)<<16) / destSizeY;

        const uint8* pBaseBuf = pBase->GetData();

        // Linear filtering
		//for (int x = 0; x < sizeX; ++x)
		const auto& ProcessColumn = [
			pDest, pBaseBuf, sizeX, destSizeY, dy_16
		] (uint32 x) 
		{
			uint8* pDestBuf = pDest->GetData() + x * NC;
			uint32 py_16 = 0;
			for (int y = 0; y < destSizeY; ++y)
			{
				uint32 r_16[NC];
				for (int c = 0; c < NC; ++c)
				{
					r_16[c] = 0;
				}

				uint32 epy_16 = py_16 + dy_16;
				uint32 py = py_16 >> 16;
				uint32 epy = epy_16 >> 16;

				// First fraction
				uint32 frac0 = py_16 & 0xffff;
				if (frac0)
				{
					for (int c = 0; c < NC; ++c)
					{
						r_16[c] += (0x10000 - frac0) * pBaseBuf[(py * sizeX + x) * NC + c];
					}

					++py;
				}

				// Whole pixels
				while (py < epy)
				{
					for (int c = 0; c < NC; ++c)
					{
						r_16[c] += uint32(pBaseBuf[(py * sizeX + x) * NC + c]) << 16;
					}

					++py;
				}

				// Second fraction
				uint32 frac1 = epy_16 & 0xffff;
				if (frac1)
				{
					for (int c = 0; c < NC; ++c)
					{
						r_16[c] += frac1 * pBaseBuf[(py * sizeX + x) * NC + c];
					}
				}

				for (int c = 0; c < NC; ++c)
				{
					pDestBuf[c] = (uint8)(r_16[c] / dy_16);
				}

				py_16 = epy_16;
				pDestBuf += sizeX * NC;
			}
		};

		ParallelFor(sizeX, ProcessColumn);

    }


    //---------------------------------------------------------------------------------------------
    //!
    //---------------------------------------------------------------------------------------------
    template< int NC, int FACTOR >
    void ImageMinifyY_Exact( Image* pDest, const Image* pBase )
    {
        int destSizeY = pDest->GetSizeY();
        int sizeX = pBase->GetSizeX();


        // Linear filtering
        //for ( int y=0; y<destSizeY; ++y )
		const auto ProcessRow = [
			pDest, pBase, sizeX, destSizeY
		] (uint32 y)
		{
			uint8_t* pDestBuf = pDest->GetData() + y*NC*sizeX;
			const uint8_t* pBaseBuf = pBase->GetData() + y * FACTOR * sizeX * NC;

			for ( int x=0; x<sizeX; ++x )
            {
                uint32_t r[NC];
                for ( int c=0; c<NC; ++c)
                {
                    r[c] = 0;
                }

                // Whole pixels
                for ( int f=0; f<FACTOR; ++f )
                {
                    for ( int c=0; c<NC; ++c)
                    {
                        r[c] += pBaseBuf[ sizeX*NC*f + x*NC + c ];
                    }
                }

                for ( int c=0; c<NC; ++c)
                {
                    pDestBuf[c] = (uint8_t)(r[c]/FACTOR);
                }

                pDestBuf += NC;
            }
            pBaseBuf += FACTOR*sizeX*NC;
        };

		ParallelFor(destSizeY, ProcessRow);
    }


    //---------------------------------------------------------------------------------------------
    //! Image minify Y version hub.
    //---------------------------------------------------------------------------------------------
    inline void ImageMinifyY( Image* pDest, const Image* pBase )
    {
        check( pDest->GetSizeY() < pBase->GetSizeY() );
        check( pDest->GetSizeX() == pBase->GetSizeX() );

		MUTABLE_CPUPROFILER_SCOPE(ImageMinifyY)

        switch ( pBase->GetFormat() )
        {

        case EImageFormat::IF_L_UBYTE:
        {
            if ( 2*pDest->GetSizeY()==pBase->GetSizeY() )
            {
                // Optimised case
                ImageMinifyY_Exact<1,2>( pDest, pBase );
            }
            else
            {
                // Generic case
                ImageMinifyY<1>( pDest, pBase );
            }
            break;
        }

        case EImageFormat::IF_RGB_UBYTE:
        {
            if ( 2*pDest->GetSizeY()==pBase->GetSizeY() )
            {
                // Optimised case
                ImageMinifyY_Exact<3,2>( pDest, pBase );
            }
            else
            {
                // Generic case
                ImageMinifyY<3>( pDest, pBase );
            }
            break;
        }

        case EImageFormat::IF_RGBA_UBYTE:
        case EImageFormat::IF_BGRA_UBYTE:
        {
            if ( 2*pDest->GetSizeY()==pBase->GetSizeY() )
            {
                // Optimised case
                ImageMinifyY_Exact<4,2>( pDest, pBase );
            }
            else
            {
                // Generic case
                ImageMinifyY<4>( pDest, pBase );
            }
            break;
        }

        default:
            // Case not implemented
            check( false );
            //mu::Halt();
        }

    }


	/** Bilinear filter image resize. */
	extern Ptr<Image> ImageResizeLinear(int32 imageCompressionQuality, const Image* pBasePtr, FImageSize destSize);
	extern void ImageResizeLinear(Image* pDest, int32 imageCompressionQuality, const Image* pBasePtr);

}
