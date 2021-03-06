/**
 * FreeRDP: A Remote Desktop Protocol Implementation
 * ClearCodec Bitmap Compression
 *
 * Copyright 2014 Marc-Andre Moreau <marcandre.moreau@gmail.com>
 * Copyright 2016 Armin Novak <armin.novak@thincast.com>
 * Copyright 2016 Thincast Technologies GmbH
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <winpr/crt.h>
#include <winpr/print.h>
#include <winpr/bitstream.h>

#include <freerdp/codec/color.h>
#include <freerdp/codec/clear.h>
#include <freerdp/log.h>

#define TAG FREERDP_TAG("codec.clear")

static const UINT32 CLEAR_LOG2_FLOOR[256] =
{
	0, 0, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 3, 3, 3, 3,
	4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4,
	5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5,
	5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5,
	6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6,
	6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6,
	6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6,
	6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6,
	7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7,
	7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7,
	7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7,
	7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7,
	7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7,
	7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7,
	7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7,
	7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7
};

static const BYTE CLEAR_8BIT_MASKS[9] =
{
	0x00, 0x01, 0x03, 0x07, 0x0F, 0x1F, 0x3F, 0x7F, 0xFF
};

static BOOL convert_color(BYTE* dst, UINT32 nDstStep, UINT32 DstFormat,
                          UINT32 nXDst, UINT32 nYDst, UINT32 nWidth, UINT32 nHeight,
                          const BYTE* src, UINT32 nSrcStep, UINT32 SrcFormat,
                          UINT32 nDstWidth, UINT32 nDstHeight, const gdiPalette* palette)
{
	UINT32 x, y;

	if (nWidth + nXDst > nDstWidth)
		nWidth = nDstWidth - nXDst;

	if (nHeight + nYDst > nDstHeight)
		nHeight = nDstHeight - nYDst;

	for (y = 0; y < nHeight; y++)
	{
		const BYTE* pSrcLine = &src[y * nSrcStep];
		BYTE* pDstLine = &dst[(nYDst + y) * nDstStep];

		for (x = 0; x < nWidth; x++)
		{
			const BYTE* pSrcPixel =
			    &pSrcLine[x * GetBytesPerPixel(SrcFormat)];
			BYTE* pDstPixel =
			    &pDstLine[(nXDst + x) * GetBytesPerPixel(DstFormat)];
			UINT32 color = ReadColor(pSrcPixel, SrcFormat);
			color = ConvertColor(color, SrcFormat,
			                     DstFormat, palette);

			if (!WriteColor(pDstPixel, DstFormat, color))
				return FALSE;
		}
	}

	return TRUE;
}

static BOOL clear_decompress_nscodec(NSC_CONTEXT* nsc, UINT32 width,
                                     UINT32 height,
                                     wStream* s, UINT32 bitmapDataByteCount,
                                     BYTE* pDstData, UINT32 DstFormat, UINT32 nDstStep,
                                     UINT32 nXDstRel, UINT32 nYDstRel)
{
	BOOL rc;

	if (Stream_GetRemainingLength(s) < bitmapDataByteCount)
	{
		WLog_ERR(TAG, "stream short %lu [%lu expected]", Stream_GetRemainingLength(s),
		         bitmapDataByteCount);
		return FALSE;
	}

	rc = nsc_process_message(nsc, 32, width, height, Stream_Pointer(s),
	                         bitmapDataByteCount, pDstData, DstFormat,
	                         nDstStep, nXDstRel, nYDstRel, width, height, FREERDP_FLIP_NONE);
	Stream_Seek(s, bitmapDataByteCount);
	return rc;
}

static BOOL clear_decompress_subcode_rlex(wStream* s,
        UINT32 bitmapDataByteCount,
        UINT32 width, UINT32 height,
        BYTE* pDstData, UINT32 DstFormat, UINT32 nDstStep,
        UINT32 nXDstRel, UINT32 nYDstRel, UINT32 nDstWidth, UINT32 nDstHeight)
{
	UINT32 x = 0, y = 0;
	UINT32 i;
	UINT32 SrcFormat = PIXEL_FORMAT_BGR24;
	UINT32 pixelCount;
	UINT32 nSrcStep;
	UINT32 bitmapDataOffset;
	UINT32 pixelIndex;
	UINT32 numBits;
	BYTE startIndex;
	BYTE stopIndex;
	BYTE suiteIndex;
	BYTE suiteDepth;
	BYTE paletteCount;
	UINT32 palette[128] = { 0 };

	if (Stream_GetRemainingLength(s) < bitmapDataByteCount)
	{
		WLog_ERR(TAG, "stream short %lu [%lu expected]", Stream_GetRemainingLength(s),
		         bitmapDataByteCount);
		return FALSE;
	}

	Stream_Read_UINT8(s, paletteCount);
	bitmapDataOffset = 1 + (paletteCount * 3);

	if (paletteCount > 127)
	{
		WLog_ERR(TAG, "paletteCount %lu", paletteCount);
		return FALSE;
	}

	for (i = 0; i < paletteCount; i++)
	{
		BYTE r, g, b;
		UINT32 color;
		Stream_Read_UINT8(s, b);
		Stream_Read_UINT8(s, g);
		Stream_Read_UINT8(s, r);
		color = GetColor(SrcFormat, r, g, b, 0xFF);
		palette[i] = ConvertColor(color, SrcFormat, DstFormat, NULL);
	}

	pixelIndex = 0;
	pixelCount = width * height;
	numBits = CLEAR_LOG2_FLOOR[paletteCount - 1] + 1;

	while (bitmapDataOffset < bitmapDataByteCount)
	{
		UINT32 tmp;
		UINT32 color;
		UINT32 runLengthFactor;

		if (Stream_GetRemainingLength(s) < 2)
		{
			WLog_ERR(TAG, "stream short %lu [%lu expected]", Stream_GetRemainingLength(s),
			         2);
			return FALSE;
		}

		Stream_Read_UINT8(s, tmp);
		Stream_Read_UINT8(s, runLengthFactor);
		bitmapDataOffset += 2;
		suiteDepth = (tmp >> numBits) & CLEAR_8BIT_MASKS[(8 - numBits)];
		stopIndex = tmp & CLEAR_8BIT_MASKS[numBits];
		startIndex = stopIndex - suiteDepth;

		if (runLengthFactor >= 0xFF)
		{
			if (Stream_GetRemainingLength(s) < 2)
			{
				WLog_ERR(TAG, "stream short %lu [%lu expected]", Stream_GetRemainingLength(s),
				         2);
				return FALSE;
			}

			Stream_Read_UINT16(s, runLengthFactor);
			bitmapDataOffset += 2;

			if (runLengthFactor >= 0xFFFF)
			{
				if (Stream_GetRemainingLength(s) < 4)
				{
					WLog_ERR(TAG, "stream short %lu [%lu expected]", Stream_GetRemainingLength(s),
					         4);
					return FALSE;
				}

				Stream_Read_UINT32(s, runLengthFactor);
				bitmapDataOffset += 4;
			}
		}

		if (startIndex >= paletteCount)
		{
			WLog_ERR(TAG, "startIndex %lu > paletteCount %lu]", startIndex, paletteCount);
			return FALSE;
		}

		if (stopIndex >= paletteCount)
		{
			WLog_ERR(TAG, "stopIndex %lu > paletteCount %lu]", stopIndex, paletteCount);
			return FALSE;
		}

		suiteIndex = startIndex;

		if (suiteIndex > 127)
		{
			WLog_ERR(TAG, "suiteIndex %lu > %lu]", suiteIndex, 127);
			return FALSE;
		}

		color = palette[suiteIndex];

		if ((pixelIndex + runLengthFactor) > pixelCount)
		{
			WLog_ERR(TAG, "pixelIndex %lu + runLengthFactor %lu > pixelCount %lu",
			         pixelIndex, runLengthFactor,
			         pixelCount);
			return FALSE;
		}

		for (i = 0; i < runLengthFactor; i++)
		{
			BYTE* pTmpData = &pDstData[(nXDstRel + x) * GetBytesPerPixel(DstFormat) +
			                           (nYDstRel + y) * nDstStep];

			if ((nXDstRel + x < nDstWidth) && (nYDstRel + y < nDstHeight))
				WriteColor(pTmpData, DstFormat, color);

			if (++x >= width)
			{
				y++;
				x = 0;
			}
		}

		pixelIndex += runLengthFactor;

		if ((pixelIndex + (suiteDepth + 1)) > pixelCount)
		{
			WLog_ERR(TAG, "pixelIndex %lu + suiteDepth %lu + 1 > pixelCount %lu",
			         pixelIndex, suiteDepth,
			         pixelCount);
			return FALSE;
		}

		for (i = 0; i <= suiteDepth; i++)
		{
			BYTE* pTmpData = &pDstData[(nXDstRel + x) * GetBytesPerPixel(DstFormat) +
			                           (nYDstRel + y) * nDstStep];
			UINT32 color = palette[suiteIndex];

			if (suiteIndex > 127)
			{
				WLog_ERR(TAG, "suiteIndex %lu > %lu", suiteIndex, 127);
				return FALSE;
			}

			suiteIndex++;

			if ((nXDstRel + x < nDstWidth) && (nYDstRel + y < nDstHeight))
				WriteColor(pTmpData, DstFormat, color);

			if (++x >= width)
			{
				y++;
				x = 0;
			}
		}

		pixelIndex += (suiteDepth + 1);
	}

	nSrcStep = width * GetBytesPerPixel(DstFormat);

	if (pixelIndex != pixelCount)
	{
		WLog_ERR(TAG, "pixelIndex %lu != pixelCount %lu", pixelIndex, pixelCount);
		return FALSE;
	}

	return TRUE;
}

static BOOL clear_decompress_residual_data(CLEAR_CONTEXT* clear,
        wStream* s,
        UINT32 residualByteCount,
        UINT32 nWidth, UINT32 nHeight,
        BYTE* pDstData, UINT32 DstFormat, UINT32 nDstStep,
        UINT32 nXDst, UINT32 nYDst,
        UINT32 nDstWidth, UINT32 nDstHeight,
        const gdiPalette* palette)
{
	UINT32 i;
	UINT32 nSrcStep;
	UINT32 suboffset;
	BYTE* dstBuffer;
	UINT32 pixelIndex;
	UINT32 pixelCount;

	if (Stream_GetRemainingLength(s) < residualByteCount)
	{
		WLog_ERR(TAG, "stream short %lu [%lu expected]", Stream_GetRemainingLength(s),
		         residualByteCount);
		return FALSE;
	}

	suboffset = 0;
	pixelIndex = 0;
	pixelCount = nWidth * nHeight;
	dstBuffer = clear->TempBuffer;

	while (suboffset < residualByteCount)
	{
		BYTE r, g, b;
		UINT32 runLengthFactor;
		UINT32 color;

		if (Stream_GetRemainingLength(s) < 4)
		{
			WLog_ERR(TAG, "stream short %lu [%lu expected]", Stream_GetRemainingLength(s),
			         4);
			return FALSE;
		}

		Stream_Read_UINT8(s, b);
		Stream_Read_UINT8(s, g);
		Stream_Read_UINT8(s, r);
		Stream_Read_UINT8(s, runLengthFactor);
		suboffset += 4;
		color = GetColor(clear->format, r, g, b, 0xFF);

		if (runLengthFactor >= 0xFF)
		{
			if (Stream_GetRemainingLength(s) < 2)
			{
				WLog_ERR(TAG, "stream short %lu [%lu expected]", Stream_GetRemainingLength(s),
				         2);
				return FALSE;
			}

			Stream_Read_UINT16(s, runLengthFactor);
			suboffset += 2;

			if (runLengthFactor >= 0xFFFF)
			{
				if (Stream_GetRemainingLength(s) < 4)
				{
					WLog_ERR(TAG, "stream short %lu [%lu expected]", Stream_GetRemainingLength(s),
					         4);
					return FALSE;
				}

				Stream_Read_UINT32(s, runLengthFactor);
				suboffset += 4;
			}
		}

		if ((pixelIndex + runLengthFactor) > pixelCount)
		{
			WLog_ERR(TAG, "pixelIndex %lu + runLengthFactor %lu > pixelCount %lu",
			         pixelIndex, runLengthFactor,
			         pixelCount);
			return FALSE;
		}

		for (i = 0; i < runLengthFactor; i++)
		{
			WriteColor(dstBuffer, clear->format, color);
			dstBuffer += GetBytesPerPixel(clear->format);
		}

		pixelIndex += runLengthFactor;
	}

	nSrcStep = nWidth * GetBytesPerPixel(clear->format);

	if (pixelIndex != pixelCount)
	{
		WLog_ERR(TAG, "pixelIndex %lu != pixelCount %lu", pixelIndex, pixelCount);
		return FALSE;
	}

	return convert_color(pDstData, nDstStep, DstFormat,
	                     nXDst, nYDst, nWidth, nHeight,
	                     clear->TempBuffer, nSrcStep, clear->format,
	                     nDstWidth, nDstHeight, palette);
}

static BOOL clear_decompress_subcodecs_data(CLEAR_CONTEXT* clear, wStream* s,
        UINT32 subcodecByteCount, UINT32 nWidth, UINT32 nHeight,
        BYTE* pDstData, UINT32 DstFormat, UINT32 nDstStep,
        UINT32 nXDst, UINT32 nYDst, UINT32 nDstWidth, UINT32 nDstHeight,
        const gdiPalette* palette)
{
	UINT16 xStart;
	UINT16 yStart;
	UINT16 width;
	UINT16 height;
	UINT32 bitmapDataByteCount;
	BYTE subcodecId;
	UINT32 suboffset;

	if (Stream_GetRemainingLength(s) < subcodecByteCount)
	{
		WLog_ERR(TAG, "stream short %lu [%lu expected]", Stream_GetRemainingLength(s),
		         subcodecByteCount);
		return FALSE;
	}

	suboffset = 0;

	while (suboffset < subcodecByteCount)
	{
		UINT32 nXDstRel;
		UINT32 nYDstRel;

		if (Stream_GetRemainingLength(s) < 13)
		{
			WLog_ERR(TAG, "stream short %lu [%lu expected]", Stream_GetRemainingLength(s),
			         13);
			return FALSE;
		}

		Stream_Read_UINT16(s, xStart);
		Stream_Read_UINT16(s, yStart);
		Stream_Read_UINT16(s, width);
		Stream_Read_UINT16(s, height);
		Stream_Read_UINT32(s, bitmapDataByteCount);
		Stream_Read_UINT8(s, subcodecId);
		suboffset += 13;

		if (Stream_GetRemainingLength(s) < bitmapDataByteCount)
		{
			WLog_ERR(TAG, "stream short %lu [%lu expected]", Stream_GetRemainingLength(s),
			         bitmapDataByteCount);
			return FALSE;
		}

		nXDstRel = nXDst + xStart;
		nYDstRel = nYDst + yStart;

		if (width > nWidth)
		{
			WLog_ERR(TAG, "width %lu > nWidth %lu", width, nWidth);
			return FALSE;
		}

		if (height > nHeight)
		{
			WLog_ERR(TAG, "height %lu > nHeight %lu", height, nHeight);
			return FALSE;
		}

		if ((width * height * GetBytesPerPixel(clear->format)) >
		    clear->TempSize)
		{
			clear->TempSize = (width * height * GetBytesPerPixel(clear->format));
			clear->TempBuffer = (BYTE*) realloc(clear->TempBuffer, clear->TempSize);

			if (!clear->TempBuffer)
			{
				WLog_ERR(TAG, "clear->TempBuffer realloc failed for %lu bytes",
				         clear->TempSize);
				return FALSE;
			}
		}

		switch (subcodecId)
		{
			case 0: /* Uncompressed */
				{
					UINT32 nSrcStep = width * GetBytesPerPixel(PIXEL_FORMAT_BGR24);
					UINT32 nSrcSize = nSrcStep * height;

					if (bitmapDataByteCount != nSrcSize)
					{
						WLog_ERR(TAG, "bitmapDataByteCount %lu != nSrcSize %lu", bitmapDataByteCount,
						         nSrcSize);
						return FALSE;
					}

					if (!convert_color(pDstData, nDstStep, DstFormat,
					                   nXDstRel, nYDstRel, width, height,
					                   Stream_Pointer(s), nSrcStep,
					                   PIXEL_FORMAT_BGR24,
					                   nDstWidth, nDstHeight, palette))
						return FALSE;

					Stream_Seek(s, bitmapDataByteCount);
				}
				break;

			case 1: /* NSCodec */
				if (!clear_decompress_nscodec(clear->nsc, width, height,
				                              s, bitmapDataByteCount,
				                              pDstData, DstFormat, nDstStep,
				                              nXDstRel, nYDstRel))
					return FALSE;

				break;

			case 2: /* CLEARCODEC_SUBCODEC_RLEX */
				if (!clear_decompress_subcode_rlex(s,
				                                   bitmapDataByteCount,
				                                   width, height,
				                                   pDstData, DstFormat, nDstStep,
				                                   nXDstRel, nYDstRel,
				                                   nDstWidth, nDstHeight))
					return FALSE;

				break;

			default:
				WLog_ERR(TAG, "Unknown subcodec ID %lu", subcodecId);
				return FALSE;
		}

		suboffset += bitmapDataByteCount;
	}

	return TRUE;
}

static BOOL resize_vbar_entry(CLEAR_CONTEXT* clear, CLEAR_VBAR_ENTRY* vBarEntry)
{
	if (vBarEntry->count > vBarEntry->size)
	{
		const UINT32 bpp = GetBytesPerPixel(clear->format);
		const UINT32 oldPos = vBarEntry->size * bpp;
		const UINT32 diffSize = (vBarEntry->count - vBarEntry->size) * bpp;
		BYTE* tmp;
		vBarEntry->size = vBarEntry->count;
		tmp = (BYTE*) realloc(vBarEntry->pixels,
		                      vBarEntry->count * bpp);

		if (!tmp)
		{
			WLog_ERR(TAG, "vBarEntry->pixels realloc %lu failed", vBarEntry->count * bpp);
			return FALSE;
		}

		memset(&tmp[oldPos], 0, diffSize);
		vBarEntry->pixels = tmp;
	}

	if (!vBarEntry->pixels && vBarEntry->size)
	{
		WLog_ERR(TAG, "!vBarEntry->pixels %lu && vBarEntry->size %lu",
		         vBarEntry->pixels, vBarEntry->size);
		return FALSE;
	}

	return TRUE;
}

static BOOL clear_decompress_bands_data(CLEAR_CONTEXT* clear,
                                        wStream* s, UINT32 bandsByteCount,
                                        UINT32 nWidth, UINT32 nHeight,
                                        BYTE* pDstData, UINT32 DstFormat, UINT32 nDstStep,
                                        UINT32 nXDst, UINT32 nYDst)
{
	UINT32 i, y;
	UINT32 count;
	UINT32 suboffset;
	UINT32 nXDstRel;
	UINT32 nYDstRel;

	if (Stream_GetRemainingLength(s) < bandsByteCount)
	{
		WLog_ERR(TAG, "stream short %lu [%lu expected]", Stream_GetRemainingLength(s),
		         11);
		return FALSE;
	}

	suboffset = 0;

	while (suboffset < bandsByteCount)
	{
		BYTE r, g, b;
		UINT16 xStart;
		UINT16 xEnd;
		UINT16 yStart;
		UINT16 yEnd;
		UINT32 colorBkg;
		UINT16 vBarHeader;
		UINT16 vBarIndex;
		UINT16 vBarYOn;
		UINT16 vBarYOff;
		UINT32 vBarCount;
		UINT32 vBarPixelCount;
		UINT32 vBarShortPixelCount;

		if (Stream_GetRemainingLength(s) < 11)
		{
			WLog_ERR(TAG, "stream short %lu [%lu expected]", Stream_GetRemainingLength(s),
			         11);
			return FALSE;
		}

		Stream_Read_UINT16(s, xStart);
		Stream_Read_UINT16(s, xEnd);
		Stream_Read_UINT16(s, yStart);
		Stream_Read_UINT16(s, yEnd);
		Stream_Read_UINT8(s, b);
		Stream_Read_UINT8(s, g);
		Stream_Read_UINT8(s, r);
		suboffset += 11;
		colorBkg = GetColor(clear->format, r, g, b, 0xFF);

		if (xEnd < xStart)
		{
			WLog_ERR(TAG, "xEnd %lu < xStart %lu", xEnd, xStart);
			return FALSE;
		}

		if (yEnd < yStart)
		{
			WLog_ERR(TAG, "yEnd %lu < yStart %lu", yEnd, yStart);
			return FALSE;
		}

		vBarCount = (xEnd - xStart) + 1;

		for (i = 0; i < vBarCount; i++)
		{
			UINT32 vBarHeight;
			CLEAR_VBAR_ENTRY* vBarEntry = NULL;
			CLEAR_VBAR_ENTRY* vBarShortEntry;
			BOOL vBarUpdate = FALSE;
			const BYTE* pSrcPixel;

			if (Stream_GetRemainingLength(s) < 2)
			{
				WLog_ERR(TAG, "stream short %lu [%lu expected]", Stream_GetRemainingLength(s),
				         2);
				return FALSE;
			}

			Stream_Read_UINT16(s, vBarHeader);
			suboffset += 2;
			vBarHeight = (yEnd - yStart + 1);

			if (vBarHeight > 52)
			{
				WLog_ERR(TAG, "vBarHeight > 52", vBarHeight);
				return FALSE;
			}

			if ((vBarHeader & 0xC000) == 0x4000) /* SHORT_VBAR_CACHE_HIT */
			{
				vBarIndex = (vBarHeader & 0x3FFF);
				vBarShortEntry = &(clear->ShortVBarStorage[vBarIndex]);

				if (!vBarShortEntry)
				{
					WLog_ERR(TAG, "missing vBarShortEntry %lu", vBarIndex);
					return FALSE;
				}

				if (Stream_GetRemainingLength(s) < 1)
				{
					WLog_ERR(TAG, "stream short %lu [%lu expected]", Stream_GetRemainingLength(s),
					         1);
					return FALSE;
				}

				Stream_Read_UINT8(s, vBarYOn);
				suboffset += 1;
				vBarShortPixelCount = vBarShortEntry->count;
				vBarUpdate = TRUE;
			}
			else if ((vBarHeader & 0xC000) == 0x0000) /* SHORT_VBAR_CACHE_MISS */
			{
				vBarYOn = (vBarHeader & 0xFF);
				vBarYOff = ((vBarHeader >> 8) & 0x3F);

				if (vBarYOff < vBarYOn)
				{
					WLog_ERR(TAG, "vBarYOff %lu < vBarYOn %lu", vBarYOff, vBarYOn);
					return FALSE;
				}

				vBarShortPixelCount = (vBarYOff - vBarYOn);

				if (vBarShortPixelCount > 52)
				{
					WLog_ERR(TAG, "vBarShortPixelCount %lu > 52", vBarShortPixelCount);
					return FALSE;
				}

				if (Stream_GetRemainingLength(s) < (vBarShortPixelCount * 3))
				{
					WLog_ERR(TAG, "stream short %lu [%lu expected]", Stream_GetRemainingLength(s),
					         (vBarShortPixelCount * 3));
					return FALSE;
				}

				if (clear->ShortVBarStorageCursor >= CLEARCODEC_VBAR_SHORT_SIZE)
				{
					WLog_ERR(TAG,
					         "clear->ShortVBarStorageCursor %lu >= CLEARCODEC_VBAR_SHORT_SIZE %lu",
					         clear->ShortVBarStorageCursor, CLEARCODEC_VBAR_SHORT_SIZE);
					return FALSE;
				}

				vBarShortEntry = &(clear->ShortVBarStorage[clear->ShortVBarStorageCursor]);
				vBarShortEntry->count = vBarShortPixelCount;

				if (!resize_vbar_entry(clear, vBarShortEntry))
					return FALSE;

				for (y = 0; y < vBarShortPixelCount; y++)
				{
					BYTE r, g, b;
					BYTE* dstBuffer =
					    &vBarShortEntry->pixels[y * GetBytesPerPixel(clear->format)];
					UINT32 color;
					Stream_Read_UINT8(s, b);
					Stream_Read_UINT8(s, g);
					Stream_Read_UINT8(s, r);
					color = GetColor(clear->format, r, g, b, 0xFF);

					if (!WriteColor(dstBuffer, clear->format, color))
						return FALSE;
				}

				suboffset += (vBarShortPixelCount * 3);
				clear->ShortVBarStorageCursor =
				    (clear->ShortVBarStorageCursor + 1) % CLEARCODEC_VBAR_SHORT_SIZE;
				vBarUpdate = TRUE;
			}
			else if ((vBarHeader & 0x8000) == 0x8000) /* VBAR_CACHE_HIT */
			{
				vBarIndex = (vBarHeader & 0x7FFF);
				vBarEntry = &(clear->VBarStorage[vBarIndex]);
			}
			else
			{
				WLog_ERR(TAG, "invalid vBarHeader %08lX", vBarHeader);
				return FALSE; /* invalid vBarHeader */
			}

			if (vBarUpdate)
			{
				UINT32 x;
				BYTE* pSrcPixel;
				BYTE* dstBuffer;

				if (clear->VBarStorageCursor >= CLEARCODEC_VBAR_SIZE)
				{
					WLog_ERR(TAG, "clear->VBarStorageCursor %lu >= CLEARCODEC_VBAR_SIZE %lu",
					         clear->VBarStorageCursor,
					         CLEARCODEC_VBAR_SIZE);
					return FALSE;
				}

				vBarEntry = &(clear->VBarStorage[clear->VBarStorageCursor]);
				vBarPixelCount = vBarHeight;
				vBarEntry->count = vBarPixelCount;

				if (!resize_vbar_entry(clear, vBarEntry))
					return FALSE;

				dstBuffer = vBarEntry->pixels;
				/* if (y < vBarYOn), use colorBkg */
				y = 0;
				count = vBarYOn;

				if ((y + count) > vBarPixelCount)
					count = (vBarPixelCount > y) ? (vBarPixelCount - y) : 0;

				while (count--)
				{
					WriteColor(dstBuffer, clear->format, colorBkg);
					dstBuffer += GetBytesPerPixel(clear->format);
				}

				/*
				 * if ((y >= vBarYOn) && (y < (vBarYOn + vBarShortPixelCount))),
				 * use vBarShortPixels at index (y - shortVBarYOn)
				 */
				y = vBarYOn;
				count = vBarShortPixelCount;

				if ((y + count) > vBarPixelCount)
					count = (vBarPixelCount > y) ? (vBarPixelCount - y) : 0;

				pSrcPixel = &vBarShortEntry->pixels[(y - vBarYOn) * GetBytesPerPixel(
				                                        clear->format)];

				for (x = 0; x < count; x++)
				{
					UINT32 color;
					color =	ReadColor(&vBarShortEntry->pixels[x * GetBytesPerPixel(clear->format)],
					                  clear->format);

					if (!WriteColor(dstBuffer, clear->format, color))
						return FALSE;

					dstBuffer += GetBytesPerPixel(clear->format);
				}

				/* if (y >= (vBarYOn + vBarShortPixelCount)), use colorBkg */
				y = vBarYOn + vBarShortPixelCount;
				count = (vBarPixelCount > y) ? (vBarPixelCount - y) : 0;

				while (count--)
				{
					if (!WriteColor(dstBuffer, clear->format, colorBkg))
						return FALSE;

					dstBuffer += GetBytesPerPixel(clear->format);
				}

				vBarEntry->count = vBarPixelCount;
				clear->VBarStorageCursor = (clear->VBarStorageCursor + 1) %
				                           CLEARCODEC_VBAR_SIZE;
			}

			if (vBarEntry->count != vBarHeight)
			{
				WLog_ERR(TAG, "vBarEntry->count %lu != vBarHeight %lu", vBarEntry->count,
				         vBarHeight);
				return FALSE;
			}

			nXDstRel = nXDst + xStart;
			nYDstRel = nYDst + yStart;
			pSrcPixel = vBarEntry->pixels;

			if (i < nWidth)
			{
				count = vBarEntry->count;

				if (count > nHeight)
					count = nHeight;

				for (y = 0; y < count; y++)
				{
					BYTE* pDstPixel8 = &pDstData[((nYDstRel + y) * nDstStep) +
					                             ((nXDstRel + i) * GetBytesPerPixel(DstFormat))];
					UINT32 color = ReadColor(pSrcPixel, clear->format);
					color = ConvertColor(color, clear->format,
					                     DstFormat, NULL);

					if (!WriteColor(pDstPixel8, DstFormat, color))
						return FALSE;

					pSrcPixel += GetBytesPerPixel(clear->format);
				}
			}
		}
	}

	return TRUE;
}

static BOOL clear_decompress_glyph_data(CLEAR_CONTEXT* clear,
                                        wStream* s, UINT32 glyphFlags,
                                        UINT32 nWidth, UINT32 nHeight,
                                        BYTE* pDstData, UINT32 DstFormat, UINT32 nDstStep,
                                        UINT32 nXDst, UINT32 nYDst,
                                        UINT32 nDstWidth, UINT32 nDstHeight,
                                        const gdiPalette* palette)
{
	UINT16 glyphIndex = 0;

	if ((glyphFlags & CLEARCODEC_FLAG_GLYPH_HIT) &&
	    !(glyphFlags & CLEARCODEC_FLAG_GLYPH_INDEX))
	{
		WLog_ERR(TAG, "Invalid glyph flags %08X", glyphFlags);
		return FALSE;
	}

	if ((glyphFlags & CLEARCODEC_FLAG_GLYPH_INDEX) == 0)
		return TRUE;

	if ((nWidth * nHeight) > (1024 * 1024))
	{
		WLog_ERR(TAG, "glyph too large: %ux%u", nWidth, nHeight);
		return FALSE;
	}

	if (Stream_GetRemainingLength(s) < 2)
	{
		WLog_ERR(TAG, "stream short %lu [%lu expected]", Stream_GetRemainingLength(s),
		         2);
		return FALSE;
	}

	Stream_Read_UINT16(s, glyphIndex);

	if (glyphIndex >= 4000)
	{
		WLog_ERR(TAG, "Invalid glyphIndex %u", glyphIndex);
		return FALSE;
	}

	if (glyphFlags & CLEARCODEC_FLAG_GLYPH_HIT)
	{
		UINT32 nSrcStep;
		CLEAR_GLYPH_ENTRY* glyphEntry = &(clear->GlyphCache[glyphIndex]);
		BYTE* glyphData;

		if (!glyphEntry)
		{
			WLog_ERR(TAG, "clear->GlyphCache[%lu]=NULL", glyphIndex);
			return FALSE;
		}

		glyphData = (BYTE*) glyphEntry->pixels;

		if (!glyphData)
		{
			WLog_ERR(TAG, "clear->GlyphCache[%lu]->pixels=NULL", glyphIndex);
			return FALSE;
		}

		if ((nWidth * nHeight) > glyphEntry->count)
		{
			WLog_ERR(TAG, "(nWidth %lu * nHeight %lu) > glyphEntry->count %lu", nWidth, nHeight,
			         glyphEntry->count);
			return FALSE;
		}

		nSrcStep = nWidth * GetBytesPerPixel(clear->format);
		return convert_color(pDstData, nDstStep, DstFormat,
		                     nXDst, nYDst, nWidth, nHeight,
		                     glyphData, nSrcStep, clear->format,
		                     nDstWidth, nDstHeight, palette);
	}

	if (glyphFlags & CLEARCODEC_FLAG_GLYPH_INDEX)
	{
		BYTE* glyphData;
		UINT32 nSrcStep;
		CLEAR_GLYPH_ENTRY* glyphEntry = &(clear->GlyphCache[glyphIndex]);
		glyphEntry->count = nWidth * nHeight;

		if (glyphEntry->count > glyphEntry->size)
		{
			UINT32* tmp;
			glyphEntry->size = glyphEntry->count;
			tmp = (UINT32*) realloc(glyphEntry->pixels, glyphEntry->size * 4);

			if (!tmp)
			{
				WLog_ERR(TAG, "glyphEntry->pixels realloc %lu failed!", glyphEntry->size * 4);
				return FALSE;
			}

			glyphEntry->pixels = tmp;
		}

		if (!glyphEntry->pixels)
		{
			WLog_ERR(TAG, "glyphEntry->pixels=NULL");
			return FALSE;
		}

		glyphData = (BYTE*) glyphEntry->pixels;
		nSrcStep = nWidth * GetBytesPerPixel(clear->format);

		if (!convert_color(pDstData, nDstStep, DstFormat,
		                   nXDst, nYDst, nWidth, nHeight,
		                   glyphData, nSrcStep, clear->format,
		                   nDstWidth, nDstHeight, palette))
			return FALSE;
	}

	return TRUE;
}

INT32 clear_decompress(CLEAR_CONTEXT* clear, const BYTE* pSrcData,
                       UINT32 SrcSize, UINT32 nWidth, UINT32 nHeight,
                       BYTE* pDstData, UINT32 DstFormat, UINT32 nDstStep,
                       UINT32 nXDst, UINT32 nYDst, UINT32 nDstWidth,
                       UINT32 nDstHeight, const gdiPalette* palette)
{
	INT32 rc = -1;
	BYTE seqNumber;
	BYTE glyphFlags;
	UINT32 residualByteCount;
	UINT32 bandsByteCount;
	UINT32 subcodecByteCount;
	wStream* s;

	if (!pDstData)
		return -1002;

	if ((nDstWidth == 0) || (nDstHeight == 0))
		return -1022;

	if ((nWidth > 0xFFFF) || (nHeight > 0xFFFF))
		return -1004;

	s = Stream_New((BYTE*)pSrcData, SrcSize);

	if (!s)
		return -2005;

	Stream_SetLength(s, SrcSize);

	if (Stream_GetRemainingLength(s) < 2)
	{
		WLog_ERR(TAG, "stream short %lu [%lu expected]", Stream_GetRemainingLength(s), 2);
		goto fail;
	}

	Stream_Read_UINT8(s, glyphFlags);
	Stream_Read_UINT8(s, seqNumber);

	if (!clear->seqNumber && seqNumber)
		clear->seqNumber = seqNumber;

	if (seqNumber != clear->seqNumber)
	{
		WLog_ERR(TAG, "Sequence number unexpected %u - %u",
		         seqNumber, clear->seqNumber);
		WLog_ERR(TAG, "seqNumber %lu != clear->seqNumber %lu", seqNumber, clear->seqNumber);
		goto fail;
	}

	clear->seqNumber = (seqNumber + 1) % 256;

	if (glyphFlags & CLEARCODEC_FLAG_CACHE_RESET)
	{
		clear->VBarStorageCursor = 0;
		clear->ShortVBarStorageCursor = 0;
	}

	if (!clear_decompress_glyph_data(clear, s, glyphFlags, nWidth,
	                                 nHeight, pDstData, DstFormat,
	                                 nDstStep, nXDst, nYDst,
	                                 nDstWidth, nDstHeight, palette))
	{
		WLog_ERR(TAG, "clear_decompress_glyph_data failed!");
		goto fail;
	}

	/* Read composition payload header parameters */
	if (Stream_GetRemainingLength(s) < 12)
	{
		const UINT32 mask = (CLEARCODEC_FLAG_GLYPH_HIT | CLEARCODEC_FLAG_GLYPH_INDEX);

		if ((glyphFlags & mask) == mask)
			goto finish;

		WLog_ERR(TAG, "stream short %lu [%lu expected]", Stream_GetRemainingLength(s), 12);
		goto fail;
	}

	Stream_Read_UINT32(s, residualByteCount);
	Stream_Read_UINT32(s, bandsByteCount);
	Stream_Read_UINT32(s, subcodecByteCount);

	if (residualByteCount > 0)
	{
		if (!clear_decompress_residual_data(clear, s, residualByteCount, nWidth,
		                                    nHeight, pDstData, DstFormat, nDstStep, nXDst, nYDst,
		                                    nDstWidth, nDstHeight, palette))
		{
			WLog_ERR(TAG, "clear_decompress_residual_data failed!");
			goto fail;
		}
	}

	if (bandsByteCount > 0)
	{
		if (!clear_decompress_bands_data(clear, s, bandsByteCount, nWidth, nHeight,
		                                 pDstData, DstFormat, nDstStep, nXDst, nYDst))
		{
			WLog_ERR(TAG, "clear_decompress_bands_data failed!");
			goto fail;
		}
	}

	if (subcodecByteCount > 0)
	{
		if (!clear_decompress_subcodecs_data(clear, s, subcodecByteCount, nWidth,
		                                     nHeight, pDstData, DstFormat,
		                                     nDstStep, nXDst, nYDst,
		                                     nDstWidth, nDstHeight, palette))
		{
			WLog_ERR(TAG, "clear_decompress_subcodecs_data failed!");
			goto fail;
		}
	}

finish:
	rc = 0;
fail:
	Stream_Free(s, FALSE);
	return rc;
}

int clear_compress(CLEAR_CONTEXT* clear, BYTE* pSrcData, UINT32 SrcSize,
                   BYTE** ppDstData, UINT32* pDstSize)
{
	WLog_ERR(TAG, "TODO: %s not implemented!");
	return 1;
}
BOOL clear_context_reset(CLEAR_CONTEXT* clear)
{
	if (!clear)
		return FALSE;

	clear->seqNumber = 0;
	clear->VBarStorageCursor = 0;
	clear->ShortVBarStorageCursor = 0;
	return TRUE;
}
CLEAR_CONTEXT* clear_context_new(BOOL Compressor)
{
	CLEAR_CONTEXT* clear;
	clear = (CLEAR_CONTEXT*) calloc(1, sizeof(CLEAR_CONTEXT));

	if (!clear)
		return NULL;

	clear->Compressor = Compressor;
	clear->nsc = nsc_context_new();
	clear->format = PIXEL_FORMAT_BGRX32;

	if (!clear->nsc)
		goto error_nsc;

	nsc_context_set_pixel_format(clear->nsc, PIXEL_FORMAT_RGB24);
	clear->TempSize = 512 * 512 * 4;
	clear->TempBuffer = (BYTE*) malloc(clear->TempSize);

	if (!clear->TempBuffer)
		goto error_nsc;

	if (!clear_context_reset(clear))
		goto error_nsc;

	return clear;
error_nsc:
	clear_context_free(clear);
	return NULL;
}
void clear_context_free(CLEAR_CONTEXT* clear)
{
	int i;

	if (!clear)
		return;

	nsc_context_free(clear->nsc);
	free(clear->TempBuffer);

	for (i = 0; i < 4000; i++)
		free(clear->GlyphCache[i].pixels);

	for (i = 0; i < 32768; i++)
		free(clear->VBarStorage[i].pixels);

	for (i = 0; i < 16384; i++)
		free(clear->ShortVBarStorage[i].pixels);

	free(clear);
}
