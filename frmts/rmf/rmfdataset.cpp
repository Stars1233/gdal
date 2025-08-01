/******************************************************************************
 *
 * Project:  Raster Matrix Format
 * Purpose:  Read/write raster files used in GIS "Integratsia"
 *           (also known as "Panorama" GIS).
 * Author:   Andrey Kiselev, dron@ak4719.spb.edu
 *
 ******************************************************************************
 * Copyright (c) 2005, Andrey Kiselev <dron@ak4719.spb.edu>
 * Copyright (c) 2007-2012, Even Rouault <even dot rouault at spatialys.com>
 * Copyright (c) 2023, NextGIS <info@nextgis.com>
 *
 * SPDX-License-Identifier: MIT
 ****************************************************************************/
#include <algorithm>
#include <array>
#include <limits>

#include "cpl_string.h"
#include "gdal_frmts.h"
#include "ogr_spatialref.h"

#include "rmfdataset.h"

#include "cpl_safemaths.hpp"

constexpr int RMF_DEFAULT_BLOCKXSIZE = 256;
constexpr int RMF_DEFAULT_BLOCKYSIZE = 256;

static const char RMF_SigRSW[] = {'R', 'S', 'W', '\0'};
static const char RMF_SigRSW_BE[] = {'\0', 'W', 'S', 'R'};
static const char RMF_SigMTW[] = {'M', 'T', 'W', '\0'};

static const char RMF_UnitsEmpty[] = "";
static const char RMF_UnitsM[] = "m";
static const char RMF_UnitsCM[] = "cm";
static const char RMF_UnitsDM[] = "dm";
static const char RMF_UnitsMM[] = "mm";

constexpr double RMF_DEFAULT_SCALE = 10000.0;
constexpr double RMF_DEFAULT_RESOLUTION = 100.0;

constexpr const char *MD_VERSION_KEY = "VERSION";
constexpr const char *MD_NAME_KEY = "NAME";
constexpr const char *MD_SCALE_KEY = "SCALE";
constexpr const char *MD_FRAME_KEY = "FRAME";

constexpr const char *MD_MATH_BASE_MAP_TYPE_KEY = "MATH_BASE.Map type";
constexpr const char *MD_MATH_BASE_PROJECTION_KEY = "MATH_BASE.Projection";

constexpr int nMaxFramePointCount = 2048;
constexpr GInt32 nPolygonType =
    2147385342;  // 2147385342 magic number for polygon

/* -------------------------------------------------------------------- */
/*  Note: Due to the fact that in the early versions of RMF             */
/*  format the field of the iEPSGCode was marked as a 'reserved',       */
/*  in the header on its place in many cases garbage values were written.*/
/*  Most of them can be weeded out by the minimum EPSG code value.      */
/*                                                                      */
/*  see: Surveying and Positioning Guidance Note Number 7, part 1       */
/*       Using the EPSG Geodetic Parameter Dataset p. 22                */
/*       http://www.epsg.org/Portals/0/373-07-1.pdf                     */
/* -------------------------------------------------------------------- */
constexpr GInt32 RMF_EPSG_MIN_CODE = 1024;

static char *RMFUnitTypeToStr(GUInt32 iElevationUnit)
{
    switch (iElevationUnit)
    {
        case 0:
            return CPLStrdup(RMF_UnitsM);
        case 1:
            return CPLStrdup(RMF_UnitsDM);
        case 2:
            return CPLStrdup(RMF_UnitsCM);
        case 3:
            return CPLStrdup(RMF_UnitsMM);
        default:
            return CPLStrdup(RMF_UnitsEmpty);
    }
}

static GUInt32 RMFStrToUnitType(const char *pszUnit, int *pbSuccess = nullptr)
{
    if (pbSuccess != nullptr)
    {
        *pbSuccess = TRUE;
    }
    if (EQUAL(pszUnit, RMF_UnitsM))
        return 0;
    else if (EQUAL(pszUnit, RMF_UnitsDM))
        return 1;
    else if (EQUAL(pszUnit, RMF_UnitsCM))
        return 2;
    else if (EQUAL(pszUnit, RMF_UnitsMM))
        return 3;
    else
    {
        // There is no 'invalid unit' in RMF format. So meter is default...
        if (pbSuccess != nullptr)
        {
            *pbSuccess = FALSE;
        }
        return 0;
    }
}

/************************************************************************/
/* ==================================================================== */
/*                            RMFRasterBand                             */
/* ==================================================================== */
/************************************************************************/

/************************************************************************/
/*                           RMFRasterBand()                            */
/************************************************************************/

RMFRasterBand::RMFRasterBand(RMFDataset *poDSIn, int nBandIn,
                             GDALDataType eType)
    : nLastTileWidth(poDSIn->GetRasterXSize() % poDSIn->sHeader.nTileWidth),
      nLastTileHeight(poDSIn->GetRasterYSize() % poDSIn->sHeader.nTileHeight),
      nDataSize(GDALGetDataTypeSizeBytes(eType))
{
    poDS = poDSIn;
    nBand = nBandIn;

    eDataType = eType;
    nBlockXSize = poDSIn->sHeader.nTileWidth;
    nBlockYSize = poDSIn->sHeader.nTileHeight;
    nBlockSize = nBlockXSize * nBlockYSize;
    nBlockBytes = nBlockSize * nDataSize;

#ifdef DEBUG
    CPLDebug("RMF",
             "Band %d: tile width is %d, tile height is %d, "
             " last tile width %u, last tile height %u, "
             "bytes per pixel is %d, data type size is %d",
             nBand, nBlockXSize, nBlockYSize, nLastTileWidth, nLastTileHeight,
             poDSIn->sHeader.nBitDepth / 8, nDataSize);
#endif
}

/************************************************************************/
/*                           ~RMFRasterBand()                           */
/************************************************************************/

RMFRasterBand::~RMFRasterBand()
{
}

/************************************************************************/
/*                             IReadBlock()                             */
/************************************************************************/

CPLErr RMFRasterBand::IReadBlock(int nBlockXOff, int nBlockYOff, void *pImage)
{
    RMFDataset *poGDS = cpl::down_cast<RMFDataset *>(poDS);

    CPLAssert(poGDS != nullptr && nBlockXOff >= 0 && nBlockYOff >= 0 &&
              pImage != nullptr);

    memset(pImage, 0, nBlockBytes);

    GUInt32 nRawXSize = nBlockXSize;
    GUInt32 nRawYSize = nBlockYSize;

    if (nLastTileWidth &&
        static_cast<GUInt32>(nBlockXOff) == poGDS->nXTiles - 1)
        nRawXSize = nLastTileWidth;

    if (nLastTileHeight &&
        static_cast<GUInt32>(nBlockYOff) == poGDS->nYTiles - 1)
        nRawYSize = nLastTileHeight;

    GUInt32 nRawBytes = nRawXSize * nRawYSize * poGDS->sHeader.nBitDepth / 8;

    // Direct read optimization
    if (poGDS->nBands == 1 && poGDS->sHeader.nBitDepth >= 8 &&
        nRawXSize == static_cast<GUInt32>(nBlockXSize) &&
        nRawYSize == static_cast<GUInt32>(nBlockYSize))
    {
        bool bNullTile = false;
        if (CE_None != poGDS->ReadTile(nBlockXOff, nBlockYOff,
                                       reinterpret_cast<GByte *>(pImage),
                                       nRawBytes, nRawXSize, nRawYSize,
                                       bNullTile))
        {
            CPLError(CE_Failure, CPLE_FileIO,
                     "Failed to read tile xOff %d yOff %d", nBlockXOff,
                     nBlockYOff);
            return CE_Failure;
        }
        if (bNullTile)
        {
            const int nChunkSize =
                std::max(1, GDALGetDataTypeSizeBytes(eDataType));
            const GPtrDiff_t nWords =
                static_cast<GPtrDiff_t>(nBlockXSize) * nBlockYSize;
            GDALCopyWords64(&poGDS->sHeader.dfNoData, GDT_Float64, 0, pImage,
                            eDataType, nChunkSize, nWords);
        }
        return CE_None;
    }
#ifdef DEBUG
    CPLDebug("RMF", "IReadBlock nBand %d, RawSize [%d, %d], Bits %u", nBand,
             nRawXSize, nRawYSize, poGDS->sHeader.nBitDepth);
#endif  // DEBUG
    if (poGDS->pabyCurrentTile == nullptr ||
        poGDS->nCurrentTileXOff != nBlockXOff ||
        poGDS->nCurrentTileYOff != nBlockYOff ||
        poGDS->nCurrentTileBytes != nRawBytes)
    {
        if (poGDS->pabyCurrentTile == nullptr)
        {
            GUInt32 nMaxTileBytes = poGDS->sHeader.nTileWidth *
                                    poGDS->sHeader.nTileHeight *
                                    poGDS->sHeader.nBitDepth / 8;
            poGDS->pabyCurrentTile = reinterpret_cast<GByte *>(
                VSIMalloc(std::max(1U, nMaxTileBytes)));
            if (!poGDS->pabyCurrentTile)
            {
                CPLError(CE_Failure, CPLE_OutOfMemory,
                         "Can't allocate tile block of size %lu.\n%s",
                         static_cast<unsigned long>(nMaxTileBytes),
                         VSIStrerror(errno));
                poGDS->nCurrentTileBytes = 0;
                return CE_Failure;
            }
        }

        poGDS->nCurrentTileXOff = nBlockXOff;
        poGDS->nCurrentTileYOff = nBlockYOff;
        poGDS->nCurrentTileBytes = nRawBytes;

        if (CE_None != poGDS->ReadTile(nBlockXOff, nBlockYOff,
                                       poGDS->pabyCurrentTile, nRawBytes,
                                       nRawXSize, nRawYSize,
                                       poGDS->bCurrentTileIsNull))
        {
            CPLError(CE_Failure, CPLE_FileIO,
                     "Failed to read tile xOff %d yOff %d", nBlockXOff,
                     nBlockYOff);
            poGDS->nCurrentTileBytes = 0;
            return CE_Failure;
        }
    }

    /* -------------------------------------------------------------------- */
    /*  Deinterleave pixels from input buffer.                              */
    /* -------------------------------------------------------------------- */

    if (poGDS->bCurrentTileIsNull)
    {
        const int nChunkSize = std::max(1, GDALGetDataTypeSizeBytes(eDataType));
        const GPtrDiff_t nWords =
            static_cast<GPtrDiff_t>(nBlockXSize) * nBlockYSize;
        GDALCopyWords64(&poGDS->sHeader.dfNoData, GDT_Float64, 0, pImage,
                        eDataType, nChunkSize, nWords);
        return CE_None;
    }
    else if ((poGDS->eRMFType == RMFT_RSW &&
              (poGDS->sHeader.nBitDepth == 8 ||
               poGDS->sHeader.nBitDepth == 24 ||
               poGDS->sHeader.nBitDepth == 32)) ||
             (poGDS->eRMFType == RMFT_MTW))
    {
        const size_t nTilePixelSize = poGDS->sHeader.nBitDepth / 8;
        const size_t nTileLineSize = nTilePixelSize * nRawXSize;
        const size_t nBlockLineSize =
            static_cast<size_t>(nDataSize) * nBlockXSize;
        int iDstBand = (poGDS->nBands - nBand);
        for (GUInt32 iLine = 0; iLine != nRawYSize; ++iLine)
        {
            GByte *pabySrc;
            GByte *pabyDst;
            pabySrc = poGDS->pabyCurrentTile + iLine * nTileLineSize +
                      iDstBand * nDataSize;
            pabyDst =
                reinterpret_cast<GByte *>(pImage) + iLine * nBlockLineSize;
            GDALCopyWords(pabySrc, eDataType, static_cast<int>(nTilePixelSize),
                          pabyDst, eDataType, static_cast<int>(nDataSize),
                          nRawXSize);
        }
        return CE_None;
    }
    else if (poGDS->eRMFType == RMFT_RSW && poGDS->sHeader.nBitDepth == 16 &&
             poGDS->nBands == 3)
    {
        const size_t nTilePixelBits = poGDS->sHeader.nBitDepth;
        const size_t nTileLineSize = nTilePixelBits * nRawXSize / 8;
        const size_t nBlockLineSize =
            static_cast<size_t>(nDataSize) * nBlockXSize;

        for (GUInt32 iLine = 0; iLine != nRawYSize; ++iLine)
        {
            GUInt16 *pabySrc;
            GByte *pabyDst;
            pabySrc = reinterpret_cast<GUInt16 *>(poGDS->pabyCurrentTile +
                                                  iLine * nTileLineSize);
            pabyDst =
                reinterpret_cast<GByte *>(pImage) + iLine * nBlockLineSize;

            for (GUInt32 i = 0; i < nRawXSize; i++)
            {
                switch (nBand)
                {
                    case 1:
                        pabyDst[i] =
                            static_cast<GByte>((pabySrc[i] & 0x7c00) >> 7);
                        break;
                    case 2:
                        pabyDst[i] =
                            static_cast<GByte>((pabySrc[i] & 0x03e0) >> 2);
                        break;
                    case 3:
                        pabyDst[i] =
                            static_cast<GByte>((pabySrc[i] & 0x1F) << 3);
                        break;
                    default:
                        break;
                }
            }
        }
        return CE_None;
    }
    else if (poGDS->eRMFType == RMFT_RSW && poGDS->nBands == 1 &&
             poGDS->sHeader.nBitDepth == 4)
    {
        if (poGDS->nCurrentTileBytes != (nBlockSize + 1) / 2)
        {
            CPLError(CE_Failure, CPLE_AppDefined,
                     "Tile has %d bytes, %d were expected",
                     poGDS->nCurrentTileBytes, (nBlockSize + 1) / 2);
            return CE_Failure;
        }

        const size_t nTilePixelBits = poGDS->sHeader.nBitDepth;
        const size_t nTileLineSize = nTilePixelBits * nRawXSize / 8;
        const size_t nBlockLineSize =
            static_cast<size_t>(nDataSize) * nBlockXSize;

        for (GUInt32 iLine = 0; iLine != nRawYSize; ++iLine)
        {
            GByte *pabySrc;
            GByte *pabyDst;
            pabySrc = poGDS->pabyCurrentTile + iLine * nTileLineSize;
            pabyDst =
                reinterpret_cast<GByte *>(pImage) + iLine * nBlockLineSize;
            for (GUInt32 i = 0; i < nRawXSize; ++i)
            {
                if (i & 0x01)
                    pabyDst[i] = (*pabySrc++ & 0xF0) >> 4;
                else
                    pabyDst[i] = *pabySrc & 0x0F;
            }
        }
        return CE_None;
    }
    else if (poGDS->eRMFType == RMFT_RSW && poGDS->nBands == 1 &&
             poGDS->sHeader.nBitDepth == 1)
    {
        if (poGDS->nCurrentTileBytes != (nBlockSize + 7) / 8)
        {
            CPLError(CE_Failure, CPLE_AppDefined,
                     "Tile has %d bytes, %d were expected",
                     poGDS->nCurrentTileBytes, (nBlockSize + 7) / 8);
            return CE_Failure;
        }

        const size_t nTilePixelBits = poGDS->sHeader.nBitDepth;
        const size_t nTileLineSize = nTilePixelBits * nRawXSize / 8;
        const size_t nBlockLineSize =
            static_cast<size_t>(nDataSize) * nBlockXSize;

        for (GUInt32 iLine = 0; iLine != nRawYSize; ++iLine)
        {
            GByte *pabySrc;
            GByte *pabyDst;
            pabySrc = poGDS->pabyCurrentTile + iLine * nTileLineSize;
            pabyDst =
                reinterpret_cast<GByte *>(pImage) + iLine * nBlockLineSize;

            for (GUInt32 i = 0; i < nRawXSize; ++i)
            {
                switch (i & 0x7)
                {
                    case 0:
                        pabyDst[i] = (*pabySrc & 0x80) >> 7;
                        break;
                    case 1:
                        pabyDst[i] = (*pabySrc & 0x40) >> 6;
                        break;
                    case 2:
                        pabyDst[i] = (*pabySrc & 0x20) >> 5;
                        break;
                    case 3:
                        pabyDst[i] = (*pabySrc & 0x10) >> 4;
                        break;
                    case 4:
                        pabyDst[i] = (*pabySrc & 0x08) >> 3;
                        break;
                    case 5:
                        pabyDst[i] = (*pabySrc & 0x04) >> 2;
                        break;
                    case 6:
                        pabyDst[i] = (*pabySrc & 0x02) >> 1;
                        break;
                    case 7:
                        pabyDst[i] = *pabySrc++ & 0x01;
                        break;
                    default:
                        break;
                }
            }
        }
        return CE_None;
    }

    CPLError(CE_Failure, CPLE_AppDefined,
             "Invalid block data type. BitDepth %d, nBands %d",
             static_cast<int>(poGDS->sHeader.nBitDepth), poGDS->nBands);

    return CE_Failure;
}

/************************************************************************/
/*                            IWriteBlock()                             */
/************************************************************************/

CPLErr RMFRasterBand::IWriteBlock(int nBlockXOff, int nBlockYOff, void *pImage)
{
    CPLAssert(poDS != nullptr && nBlockXOff >= 0 && nBlockYOff >= 0 &&
              pImage != nullptr);

    RMFDataset *poGDS = cpl::down_cast<RMFDataset *>(poDS);

    // First drop current tile read by IReadBlock
    poGDS->nCurrentTileBytes = 0;

    GUInt32 nRawXSize = nBlockXSize;
    GUInt32 nRawYSize = nBlockYSize;

    if (nLastTileWidth &&
        static_cast<GUInt32>(nBlockXOff) == poGDS->nXTiles - 1)
        nRawXSize = nLastTileWidth;

    if (nLastTileHeight &&
        static_cast<GUInt32>(nBlockYOff) == poGDS->nYTiles - 1)
        nRawYSize = nLastTileHeight;

    const size_t nTilePixelSize =
        static_cast<size_t>(nDataSize) * poGDS->nBands;
    const size_t nTileLineSize = nTilePixelSize * nRawXSize;
    const size_t nTileSize = nTileLineSize * nRawYSize;
    const size_t nBlockLineSize = static_cast<size_t>(nDataSize) * nBlockXSize;

#ifdef DEBUG
    CPLDebug(
        "RMF",
        "IWriteBlock BlockSize [%d, %d], RawSize [%d, %d], size %d, nBand %d",
        nBlockXSize, nBlockYSize, nRawXSize, nRawYSize,
        static_cast<int>(nTileSize), nBand);
#endif  // DEBUG

    if (poGDS->nBands == 1 && nRawXSize == static_cast<GUInt32>(nBlockXSize) &&
        nRawYSize == static_cast<GUInt32>(nBlockYSize))
    {  // Immediate write
        return poGDS->WriteTile(
            nBlockXOff, nBlockYOff, reinterpret_cast<GByte *>(pImage),
            static_cast<size_t>(nRawXSize) * nRawYSize * nDataSize, nRawXSize,
            nRawYSize);
    }
    else
    {  // Try to construct full tile in memory and write later
        const GUInt32 nTile = nBlockYOff * poGDS->nXTiles + nBlockXOff;

        // Find tile
        auto poTile(poGDS->oUnfinishedTiles.find(nTile));
        if (poTile == poGDS->oUnfinishedTiles.end())
        {
            RMFTileData oTile;
            oTile.oData.resize(nTileSize);
            // If not found, but exist on disk than read it
            if (poGDS->paiTiles[2 * nTile + 1])
            {
                CPLErr eRes;
                bool bNullTile = false;
                eRes =
                    poGDS->ReadTile(nBlockXOff, nBlockYOff, oTile.oData.data(),
                                    nTileSize, nRawXSize, nRawYSize, bNullTile);
                if (eRes != CE_None)
                {
                    CPLError(CE_Failure, CPLE_FileIO,
                             "Can't read block with offset [%d, %d]",
                             nBlockXOff, nBlockYOff);
                    return eRes;
                }
            }
            poTile = poGDS->oUnfinishedTiles.insert(
                poGDS->oUnfinishedTiles.end(), std::make_pair(nTile, oTile));
        }

        GByte *pabyTileData = poTile->second.oData.data();

        // Copy new data to a tile
        int iDstBand = (poGDS->nBands - nBand);
        for (GUInt32 iLine = 0; iLine != nRawYSize; ++iLine)
        {
            const GByte *pabySrc;
            GByte *pabyDst;
            pabySrc = reinterpret_cast<const GByte *>(pImage) +
                      iLine * nBlockLineSize;
            pabyDst =
                pabyTileData + iLine * nTileLineSize + iDstBand * nDataSize;
            GDALCopyWords(pabySrc, eDataType, static_cast<int>(nDataSize),
                          pabyDst, eDataType, static_cast<int>(nTilePixelSize),
                          nRawXSize);
        }
        ++poTile->second.nBandsWritten;

        // Write to disk if tile is finished
        if (poTile->second.nBandsWritten == poGDS->nBands)
        {
            poGDS->WriteTile(nBlockXOff, nBlockYOff, pabyTileData, nTileSize,
                             nRawXSize, nRawYSize);
            poGDS->oUnfinishedTiles.erase(poTile);
        }
#ifdef DEBUG
        CPLDebug("RMF", "poGDS->oUnfinishedTiles.size() %d",
                 static_cast<int>(poGDS->oUnfinishedTiles.size()));
#endif  // DEBUG
    }

    return CE_None;
}

/************************************************************************/
/*                          GetNoDataValue()                            */
/************************************************************************/

double RMFRasterBand::GetNoDataValue(int *pbSuccess)

{
    RMFDataset *poGDS = cpl::down_cast<RMFDataset *>(poDS);

    if (pbSuccess)
        *pbSuccess = TRUE;

    return poGDS->sHeader.dfNoData;
}

CPLErr RMFRasterBand::SetNoDataValue(double dfNoData)
{
    RMFDataset *poGDS = cpl::down_cast<RMFDataset *>(poDS);

    poGDS->sHeader.dfNoData = dfNoData;
    poGDS->bHeaderDirty = true;

    return CE_None;
}

/************************************************************************/
/*                            GetUnitType()                             */
/************************************************************************/

const char *RMFRasterBand::GetUnitType()

{
    RMFDataset *poGDS = cpl::down_cast<RMFDataset *>(poDS);

    return poGDS->pszUnitType;
}

/************************************************************************/
/*                            SetUnitType()                             */
/************************************************************************/

CPLErr RMFRasterBand::SetUnitType(const char *pszNewValue)

{
    RMFDataset *poGDS = cpl::down_cast<RMFDataset *>(poDS);
    int bSuccess = FALSE;
    int iNewUnit = RMFStrToUnitType(pszNewValue, &bSuccess);

    if (bSuccess)
    {
        CPLFree(poGDS->pszUnitType);
        poGDS->pszUnitType = CPLStrdup(pszNewValue);
        poGDS->sHeader.iElevationUnit = iNewUnit;
        poGDS->bHeaderDirty = true;
        return CE_None;
    }
    else
    {
        CPLError(CE_Warning, CPLE_NotSupported,
                 "RMF driver does not support '%s' elevation units. "
                 "Possible values are: m, dm, cm, mm.",
                 pszNewValue);
        return CE_Failure;
    }
}

/************************************************************************/
/*                           GetColorTable()                            */
/************************************************************************/

GDALColorTable *RMFRasterBand::GetColorTable()
{
    RMFDataset *poGDS = cpl::down_cast<RMFDataset *>(poDS);

    return poGDS->poColorTable;
}

/************************************************************************/
/*                           SetColorTable()                            */
/************************************************************************/

CPLErr RMFRasterBand::SetColorTable(GDALColorTable *poColorTable)
{
    RMFDataset *poGDS = cpl::down_cast<RMFDataset *>(poDS);

    if (poColorTable)
    {
        if (poGDS->eRMFType == RMFT_RSW && poGDS->nBands == 1)
        {
            if (!poGDS->pabyColorTable)
                return CE_Failure;

            GDALColorEntry oEntry;
            for (GUInt32 i = 0; i < poGDS->nColorTableSize; i++)
            {
                poColorTable->GetColorEntryAsRGB(i, &oEntry);
                // Red
                poGDS->pabyColorTable[i * 4 + 0] =
                    static_cast<GByte>(oEntry.c1);
                // Green
                poGDS->pabyColorTable[i * 4 + 1] =
                    static_cast<GByte>(oEntry.c2);
                // Blue
                poGDS->pabyColorTable[i * 4 + 2] =
                    static_cast<GByte>(oEntry.c3);
                poGDS->pabyColorTable[i * 4 + 3] = 0;
            }

            poGDS->bHeaderDirty = true;
        }
        return CE_None;
    }

    return CE_Failure;
}

int RMFRasterBand::GetOverviewCount()
{
    RMFDataset *poGDS = cpl::down_cast<RMFDataset *>(poDS);
    if (poGDS->poOvrDatasets.empty())
        return GDALRasterBand::GetOverviewCount();
    else
        return static_cast<int>(poGDS->poOvrDatasets.size());
}

GDALRasterBand *RMFRasterBand::GetOverview(int i)
{
    RMFDataset *poGDS = cpl::down_cast<RMFDataset *>(poDS);
    size_t n = static_cast<size_t>(i);
    if (poGDS->poOvrDatasets.empty())
        return GDALRasterBand::GetOverview(i);
    else
        return poGDS->poOvrDatasets[n]->GetRasterBand(nBand);
}

CPLErr RMFRasterBand::IRasterIO(GDALRWFlag eRWFlag, int nXOff, int nYOff,
                                int nXSize, int nYSize, void *pData,
                                int nBufXSize, int nBufYSize,
                                GDALDataType eType, GSpacing nPixelSpace,
                                GSpacing nLineSpace,
                                GDALRasterIOExtraArg *psExtraArg)
{
    RMFDataset *poGDS = cpl::down_cast<RMFDataset *>(poDS);

    if (eRWFlag == GF_Read && poGDS->poCompressData != nullptr &&
        poGDS->poCompressData->oThreadPool.GetThreadCount() > 0)
    {
        poGDS->poCompressData->oThreadPool.WaitCompletion();
    }

    return GDALRasterBand::IRasterIO(eRWFlag, nXOff, nYOff, nXSize, nYSize,
                                     pData, nBufXSize, nBufYSize, eType,
                                     nPixelSpace, nLineSpace, psExtraArg);
}

/************************************************************************/
/*                       GetColorInterpretation()                       */
/************************************************************************/

GDALColorInterp RMFRasterBand::GetColorInterpretation()
{
    RMFDataset *poGDS = cpl::down_cast<RMFDataset *>(poDS);

    if (poGDS->nBands == 3)
    {
        if (nBand == 1)
            return GCI_RedBand;
        else if (nBand == 2)
            return GCI_GreenBand;
        else if (nBand == 3)
            return GCI_BlueBand;

        return GCI_Undefined;
    }

    if (poGDS->eRMFType == RMFT_RSW)
        return GCI_PaletteIndex;

    return GCI_Undefined;
}

/************************************************************************/
/* ==================================================================== */
/*                              RMFDataset                              */
/* ==================================================================== */
/************************************************************************/

/************************************************************************/
/*                           RMFDataset()                               */
/************************************************************************/

RMFDataset::RMFDataset() : pszUnitType(CPLStrdup(RMF_UnitsEmpty))
{
    m_oSRS.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
    nBands = 0;
    memset(&sHeader, 0, sizeof(sHeader));
    memset(&sExtHeader, 0, sizeof(sExtHeader));
}

/************************************************************************/
/*                            ~RMFDataset()                             */
/************************************************************************/

RMFDataset::~RMFDataset()
{
    RMFDataset::FlushCache(true);
    for (size_t n = 0; n != poOvrDatasets.size(); ++n)
    {
        poOvrDatasets[n]->RMFDataset::FlushCache(true);
    }

    VSIFree(paiTiles);
    VSIFree(pabyDecompressBuffer);
    VSIFree(pabyCurrentTile);
    CPLFree(pszUnitType);
    CPLFree(pabyColorTable);
    if (poColorTable != nullptr)
        delete poColorTable;

    for (size_t n = 0; n != poOvrDatasets.size(); ++n)
    {
        GDALClose(poOvrDatasets[n]);
    }

    if (fp != nullptr && poParentDS == nullptr)
    {
        VSIFCloseL(fp);
    }
}

/************************************************************************/
/*                          GetGeoTransform()                           */
/************************************************************************/

CPLErr RMFDataset::GetGeoTransform(GDALGeoTransform &gt) const
{
    gt = m_gt;

    if (sHeader.iGeorefFlag)
        return CE_None;

    return CE_Failure;
}

/************************************************************************/
/*                          SetGeoTransform()                           */
/************************************************************************/

CPLErr RMFDataset::SetGeoTransform(const GDALGeoTransform &gt)
{
    m_gt = gt;
    sHeader.dfPixelSize = m_gt[1];
    if (sHeader.dfPixelSize != 0.0)
        sHeader.dfResolution = sHeader.dfScale / sHeader.dfPixelSize;
    sHeader.dfLLX = m_gt[0];
    sHeader.dfLLY = m_gt[3] - nRasterYSize * sHeader.dfPixelSize;
    sHeader.iGeorefFlag = 1;

    bHeaderDirty = true;

    return CE_None;
}

/************************************************************************/
/*                          GetSpatialRef()                             */
/************************************************************************/

const OGRSpatialReference *RMFDataset::GetSpatialRef() const

{
    return m_oSRS.IsEmpty() ? nullptr : &m_oSRS;
}

/************************************************************************/
/*                           SetSpatialRef()                            */
/************************************************************************/

CPLErr RMFDataset::SetSpatialRef(const OGRSpatialReference *poSRS)

{
    m_oSRS.Clear();
    if (poSRS)
        m_oSRS = *poSRS;

    bHeaderDirty = true;

    return CE_None;
}

/************************************************************************/
/*                           WriteHeader()                              */
/************************************************************************/

CPLErr RMFDataset::WriteHeader()
{
    /* -------------------------------------------------------------------- */
    /*  Setup projection.                                                   */
    /* -------------------------------------------------------------------- */
    if (!m_oSRS.IsEmpty())
    {
        long iProjection = 0;
        long iDatum = 0;
        long iEllips = 0;
        long iZone = 0;
        int iVertCS = 0;
        double adfPrjParams[7] = {};

        m_oSRS.exportToPanorama(&iProjection, &iDatum, &iEllips, &iZone,
                                adfPrjParams);
        m_oSRS.exportVertCSToPanorama(&iVertCS);
        sHeader.iProjection = static_cast<GInt32>(iProjection);
        sHeader.dfStdP1 = adfPrjParams[0];
        sHeader.dfStdP2 = adfPrjParams[1];
        sHeader.dfCenterLat = adfPrjParams[2];
        sHeader.dfCenterLong = adfPrjParams[3];
        if (m_oSRS.GetAuthorityName(nullptr) != nullptr &&
            m_oSRS.GetAuthorityCode(nullptr) != nullptr &&
            EQUAL(m_oSRS.GetAuthorityName(nullptr), "EPSG"))
        {
            sHeader.iEPSGCode = atoi(m_oSRS.GetAuthorityCode(nullptr));
        }

        sExtHeader.nEllipsoid = static_cast<GInt32>(iEllips);
        sExtHeader.nDatum = static_cast<GInt32>(iDatum);
        sExtHeader.nZone = static_cast<GInt32>(iZone);
        sExtHeader.nVertDatum = static_cast<GInt32>(iVertCS);

        // Set map type
        auto pszMapType = GetMetadataItem(MD_MATH_BASE_MAP_TYPE_KEY);
        if (pszMapType != nullptr)
        {
            sHeader.iMapType = static_cast<GInt32>(atoi(pszMapType));
        }
    }

#define RMF_WRITE_LONG(ptr, value, offset)                                     \
    do                                                                         \
    {                                                                          \
        GInt32 iLong = CPL_LSBWORD32(value);                                   \
        memcpy((ptr) + (offset), &iLong, 4);                                   \
    } while (false);

#define RMF_WRITE_ULONG(ptr, value, offset)                                    \
    do                                                                         \
    {                                                                          \
        GUInt32 iULong = CPL_LSBWORD32(value);                                 \
        memcpy((ptr) + (offset), &iULong, 4);                                  \
    } while (false);

#define RMF_WRITE_DOUBLE(ptr, value, offset)                                   \
    do                                                                         \
    {                                                                          \
        double dfDouble = (value);                                             \
        CPL_LSBPTR64(&dfDouble);                                               \
        memcpy((ptr) + (offset), &dfDouble, 8);                                \
    } while (false);

    // Frame if present
    std::vector<RSWFrameCoord> astFrameCoords;
    auto pszFrameWKT = GetMetadataItem(MD_FRAME_KEY);
    if (pszFrameWKT != nullptr)
    {
        CPLDebug("RMF", "Write to header frame: %s", pszFrameWKT);
        OGRGeometry *poFrameGeom = nullptr;
        if (OGRGeometryFactory::createFromWkt(pszFrameWKT, nullptr,
                                              &poFrameGeom) == OGRERR_NONE)
        {
            if (poFrameGeom->getGeometryType() == wkbPolygon)
            {
                GDALGeoTransform reverseGT;
                if (m_gt.GetInverse(reverseGT))
                {
                    OGRPolygon *poFramePoly = poFrameGeom->toPolygon();
                    if (!poFramePoly->IsEmpty())
                    {
                        OGRLinearRing *poFrameRing =
                            poFramePoly->getExteriorRing();
                        for (int i = 0; i < poFrameRing->getNumPoints(); i++)
                        {
                            int nX =
                                int(reverseGT[0] +
                                    poFrameRing->getX(i) * reverseGT[1] - 0.5);
                            int nY =
                                int(reverseGT[3] +
                                    poFrameRing->getY(i) * reverseGT[5] - 0.5);

                            CPLDebug("RMF", "X: %d, Y: %d", nX, nY);

                            astFrameCoords.push_back({nX, nY});
                        }
                    }

                    if (astFrameCoords.empty() ||
                        astFrameCoords.size() > nMaxFramePointCount)
                    {
                        // CPLError(CE_Warning, CPLE_AppDefined, "Invalid frame WKT: %s", pszFrameWKT);
                        CPLDebug("RMF", "Write to header frame failed: no "
                                        "points or too many");
                        astFrameCoords.clear();
                    }
                    else
                    {
                        sHeader.nROISize = static_cast<GUInt32>(
                            sizeof(RSWFrame) +
                            sizeof(RSWFrameCoord) *
                                astFrameCoords
                                    .size());  // Set real size and real point count
                        sHeader.iFrameFlag = 0;
                    }
                }
                else
                {
                    CPLDebug("RMF", "Write to header frame failed: "
                                    "GDALInvGeoTransform == FALSE");
                }
            }
            OGRGeometryFactory::destroyGeometry(poFrameGeom);
        }
        else
        {
            CPLDebug("RMF", "Write to header frame failed: "
                            "OGRGeometryFactory::createFromWkt error");
        }
    }

    vsi_l_offset iCurrentFileSize(GetLastOffset());
    sHeader.nFileSize0 = GetRMFOffset(iCurrentFileSize, &iCurrentFileSize);
    sHeader.nSize = sHeader.nFileSize0 - GetRMFOffset(nHeaderOffset, nullptr);
    /* -------------------------------------------------------------------- */
    /*  Write out the main header.                                          */
    /* -------------------------------------------------------------------- */
    {
        GByte abyHeader[RMF_HEADER_SIZE] = {};

        memcpy(abyHeader, sHeader.bySignature, RMF_SIGNATURE_SIZE);
        RMF_WRITE_ULONG(abyHeader, sHeader.iVersion, 4);
        RMF_WRITE_ULONG(abyHeader, sHeader.nSize, 8);
        RMF_WRITE_ULONG(abyHeader, sHeader.nOvrOffset, 12);
        RMF_WRITE_ULONG(abyHeader, sHeader.iUserID, 16);
        memcpy(abyHeader + 20, sHeader.byName, RMF_NAME_SIZE);
        RMF_WRITE_ULONG(abyHeader, sHeader.nBitDepth, 52);
        RMF_WRITE_ULONG(abyHeader, sHeader.nHeight, 56);
        RMF_WRITE_ULONG(abyHeader, sHeader.nWidth, 60);
        RMF_WRITE_ULONG(abyHeader, sHeader.nXTiles, 64);
        RMF_WRITE_ULONG(abyHeader, sHeader.nYTiles, 68);
        RMF_WRITE_ULONG(abyHeader, sHeader.nTileHeight, 72);
        RMF_WRITE_ULONG(abyHeader, sHeader.nTileWidth, 76);
        RMF_WRITE_ULONG(abyHeader, sHeader.nLastTileHeight, 80);
        RMF_WRITE_ULONG(abyHeader, sHeader.nLastTileWidth, 84);
        RMF_WRITE_ULONG(abyHeader, sHeader.nROIOffset, 88);
        RMF_WRITE_ULONG(abyHeader, sHeader.nROISize, 92);
        RMF_WRITE_ULONG(abyHeader, sHeader.nClrTblOffset, 96);
        RMF_WRITE_ULONG(abyHeader, sHeader.nClrTblSize, 100);
        RMF_WRITE_ULONG(abyHeader, sHeader.nTileTblOffset, 104);
        RMF_WRITE_ULONG(abyHeader, sHeader.nTileTblSize, 108);
        RMF_WRITE_LONG(abyHeader, sHeader.iMapType, 124);
        RMF_WRITE_LONG(abyHeader, sHeader.iProjection, 128);
        RMF_WRITE_LONG(abyHeader, sHeader.iEPSGCode, 132);
        RMF_WRITE_DOUBLE(abyHeader, sHeader.dfScale, 136);
        RMF_WRITE_DOUBLE(abyHeader, sHeader.dfResolution, 144);
        RMF_WRITE_DOUBLE(abyHeader, sHeader.dfPixelSize, 152);
        RMF_WRITE_DOUBLE(abyHeader, sHeader.dfLLY, 160);
        RMF_WRITE_DOUBLE(abyHeader, sHeader.dfLLX, 168);
        RMF_WRITE_DOUBLE(abyHeader, sHeader.dfStdP1, 176);
        RMF_WRITE_DOUBLE(abyHeader, sHeader.dfStdP2, 184);
        RMF_WRITE_DOUBLE(abyHeader, sHeader.dfCenterLong, 192);
        RMF_WRITE_DOUBLE(abyHeader, sHeader.dfCenterLat, 200);
        *(abyHeader + 208) = sHeader.iCompression;
        *(abyHeader + 209) = sHeader.iMaskType;
        *(abyHeader + 210) = sHeader.iMaskStep;
        *(abyHeader + 211) = sHeader.iFrameFlag;
        RMF_WRITE_ULONG(abyHeader, sHeader.nFlagsTblOffset, 212);
        RMF_WRITE_ULONG(abyHeader, sHeader.nFlagsTblSize, 216);
        RMF_WRITE_ULONG(abyHeader, sHeader.nFileSize0, 220);
        RMF_WRITE_ULONG(abyHeader, sHeader.nFileSize1, 224);
        *(abyHeader + 228) = sHeader.iUnknown;
        *(abyHeader + 244) = sHeader.iGeorefFlag;
        *(abyHeader + 245) = sHeader.iInverse;
        *(abyHeader + 246) = sHeader.iJpegQuality;
        memcpy(abyHeader + 248, sHeader.abyInvisibleColors,
               sizeof(sHeader.abyInvisibleColors));
        RMF_WRITE_DOUBLE(abyHeader, sHeader.adfElevMinMax[0], 280);
        RMF_WRITE_DOUBLE(abyHeader, sHeader.adfElevMinMax[1], 288);
        RMF_WRITE_DOUBLE(abyHeader, sHeader.dfNoData, 296);
        RMF_WRITE_ULONG(abyHeader, sHeader.iElevationUnit, 304);
        *(abyHeader + 308) = sHeader.iElevationType;
        RMF_WRITE_ULONG(abyHeader, sHeader.nExtHdrOffset, 312);
        RMF_WRITE_ULONG(abyHeader, sHeader.nExtHdrSize, 316);

        VSIFSeekL(fp, nHeaderOffset, SEEK_SET);
        VSIFWriteL(abyHeader, 1, sizeof(abyHeader), fp);
    }

    /* -------------------------------------------------------------------- */
    /*  Write out the extended header.                                      */
    /* -------------------------------------------------------------------- */

    if (sHeader.nExtHdrOffset && sHeader.nExtHdrSize >= RMF_MIN_EXT_HEADER_SIZE)
    {
        if (sHeader.nExtHdrSize > RMF_MAX_EXT_HEADER_SIZE)
        {
            CPLError(CE_Failure, CPLE_FileIO, "RMF File malformed");
            return CE_Failure;
        }
        GByte *pabyExtHeader =
            static_cast<GByte *>(CPLCalloc(sHeader.nExtHdrSize, 1));

        RMF_WRITE_LONG(pabyExtHeader, sExtHeader.nEllipsoid, 24);
        RMF_WRITE_LONG(pabyExtHeader, sExtHeader.nVertDatum, 28);
        RMF_WRITE_LONG(pabyExtHeader, sExtHeader.nDatum, 32);
        RMF_WRITE_LONG(pabyExtHeader, sExtHeader.nZone, 36);

        VSIFSeekL(fp, GetFileOffset(sHeader.nExtHdrOffset), SEEK_SET);
        VSIFWriteL(pabyExtHeader, 1, sHeader.nExtHdrSize, fp);

        CPLFree(pabyExtHeader);
    }

    /* -------------------------------------------------------------------- */
    /*  Write out the color table.                                          */
    /* -------------------------------------------------------------------- */

    if (sHeader.nClrTblOffset && sHeader.nClrTblSize)
    {
        VSIFSeekL(fp, GetFileOffset(sHeader.nClrTblOffset), SEEK_SET);
        VSIFWriteL(pabyColorTable, 1, sHeader.nClrTblSize, fp);
    }

    if (sHeader.nROIOffset && sHeader.nROISize)
    {
        GByte *pabyROI = static_cast<GByte *>(CPLCalloc(sHeader.nROISize, 1));
        memset(pabyROI, 0, sHeader.nROISize);

        auto nPointCount = astFrameCoords.size();
        size_t offset = 0;
        RMF_WRITE_LONG(pabyROI, nPolygonType, offset);
        offset += 4;
        RMF_WRITE_LONG(pabyROI, static_cast<GInt32>((4 + nPointCount * 2) * 4),
                       offset);
        offset += 4;
        RMF_WRITE_LONG(pabyROI, 0, offset);
        offset += 4;
        RMF_WRITE_LONG(pabyROI, static_cast<GInt32>(32768 * nPointCount * 2),
                       offset);
        offset += 4;

        // Write points
        for (size_t i = 0; i < nPointCount; i++)
        {
            RMF_WRITE_LONG(pabyROI, astFrameCoords[i].nX, offset);
            offset += 4;
            RMF_WRITE_LONG(pabyROI, astFrameCoords[i].nY, offset);
            offset += 4;
        }

        VSIFSeekL(fp, GetFileOffset(sHeader.nROIOffset), SEEK_SET);
        VSIFWriteL(pabyROI, 1, sHeader.nROISize, fp);

        CPLFree(pabyROI);
    }

    if (sHeader.nFlagsTblOffset && sHeader.nFlagsTblSize)
    {
        GByte *pabyFlagsTbl =
            static_cast<GByte *>(CPLCalloc(sHeader.nFlagsTblSize, 1));

        if (sHeader.iFrameFlag == 0)
        {
            // TODO: Add more strictly check for flag value
            memset(
                pabyFlagsTbl, 2,
                sHeader
                    .nFlagsTblSize);  // Mark all blocks as intersected with ROI. 0 - complete outside, 1 - complete inside.
        }
        else
        {
            memset(pabyFlagsTbl, 0, sHeader.nFlagsTblSize);
        }

        VSIFSeekL(fp, GetFileOffset(sHeader.nFlagsTblOffset), SEEK_SET);
        VSIFWriteL(pabyFlagsTbl, 1, sHeader.nFlagsTblSize, fp);

        CPLFree(pabyFlagsTbl);
    }

#undef RMF_WRITE_DOUBLE
#undef RMF_WRITE_ULONG
#undef RMF_WRITE_LONG

    /* -------------------------------------------------------------------- */
    /*  Write out the block table, swap if needed.                          */
    /* -------------------------------------------------------------------- */

    VSIFSeekL(fp, GetFileOffset(sHeader.nTileTblOffset), SEEK_SET);

#ifdef CPL_MSB
    GUInt32 *paiTilesSwapped =
        static_cast<GUInt32 *>(CPLMalloc(sHeader.nTileTblSize));
    if (!paiTilesSwapped)
        return CE_Failure;

    memcpy(paiTilesSwapped, paiTiles, sHeader.nTileTblSize);
    for (GUInt32 i = 0; i < sHeader.nTileTblSize / sizeof(GUInt32); i++)
        CPL_SWAP32PTR(paiTilesSwapped + i);
    VSIFWriteL(paiTilesSwapped, 1, sHeader.nTileTblSize, fp);

    CPLFree(paiTilesSwapped);
#else
    VSIFWriteL(paiTiles, 1, sHeader.nTileTblSize, fp);
#endif

    bHeaderDirty = false;

    return CE_None;
}

/************************************************************************/
/*                             FlushCache()                             */
/************************************************************************/

CPLErr RMFDataset::FlushCache(bool bAtClosing)

{
    CPLErr eErr = GDALDataset::FlushCache(bAtClosing);

    if (poCompressData != nullptr &&
        poCompressData->oThreadPool.GetThreadCount() > 0)
    {
        poCompressData->oThreadPool.WaitCompletion();
    }

    if (bAtClosing && eRMFType == RMFT_MTW && eAccess == GA_Update)
    {
        GDALRasterBand *poBand = GetRasterBand(1);

        if (poBand)
        {
            // ComputeRasterMinMax can setup error in case of dataset full
            // from NoData values, but it  makes no sense here.
            CPLErrorStateBackuper oErrorStateBackuper(CPLQuietErrorHandler);
            poBand->ComputeRasterMinMax(FALSE, sHeader.adfElevMinMax);
            bHeaderDirty = true;
        }
    }
    if (bHeaderDirty && WriteHeader() != CE_None)
        eErr = CE_Failure;
    return eErr;
}

/************************************************************************/
/*                              Identify()                              */
/************************************************************************/

int RMFDataset::Identify(GDALOpenInfo *poOpenInfo)

{
    if (poOpenInfo->pabyHeader == nullptr)
        return FALSE;

    if (memcmp(poOpenInfo->pabyHeader, RMF_SigRSW, sizeof(RMF_SigRSW)) != 0 &&
        memcmp(poOpenInfo->pabyHeader, RMF_SigRSW_BE, sizeof(RMF_SigRSW_BE)) !=
            0 &&
        memcmp(poOpenInfo->pabyHeader, RMF_SigMTW, sizeof(RMF_SigMTW)) != 0)
        return FALSE;

    return TRUE;
}

/************************************************************************/
/*                                Open()                                */
/************************************************************************/

GDALDataset *RMFDataset::Open(GDALOpenInfo *poOpenInfo)
{
    auto poDS = Open(poOpenInfo, nullptr, 0);
    if (poDS == nullptr)
    {
        return nullptr;
    }

    RMFDataset *poCurrentLayer = poDS;
    RMFDataset *poParent = poCurrentLayer;
    const int nMaxPossibleOvCount = 64;

    for (int iOv = 0; iOv < nMaxPossibleOvCount && poCurrentLayer != nullptr;
         ++iOv)
    {
        poCurrentLayer = poCurrentLayer->OpenOverview(poParent, poOpenInfo);
        if (poCurrentLayer == nullptr)
            break;
        poParent->poOvrDatasets.push_back(poCurrentLayer);
    }

    return poDS;
}

RMFDataset *RMFDataset::Open(GDALOpenInfo *poOpenInfo, RMFDataset *poParentDS,
                             vsi_l_offset nNextHeaderOffset)
{
    if (!Identify(poOpenInfo) ||
        (poParentDS == nullptr && poOpenInfo->fpL == nullptr))
        return nullptr;

    /* -------------------------------------------------------------------- */
    /*  Create a corresponding GDALDataset.                                 */
    /* -------------------------------------------------------------------- */
    RMFDataset *poDS = new RMFDataset();

    if (poParentDS == nullptr)
    {
        poDS->fp = poOpenInfo->fpL;
        poOpenInfo->fpL = nullptr;
        poDS->nHeaderOffset = 0;
        poDS->poParentDS = nullptr;
    }
    else
    {
        poDS->fp = poParentDS->fp;
        poDS->poParentDS = poParentDS;
        poDS->nHeaderOffset = nNextHeaderOffset;
    }
    poDS->eAccess = poOpenInfo->eAccess;

#define RMF_READ_SHORT(ptr, value, offset)                                     \
    do                                                                         \
    {                                                                          \
        memcpy(&(value), reinterpret_cast<GInt16 *>((ptr) + (offset)),         \
               sizeof(GInt16));                                                \
        if (poDS->bBigEndian)                                                  \
        {                                                                      \
            CPL_MSBPTR16(&(value));                                            \
        }                                                                      \
        else                                                                   \
        {                                                                      \
            CPL_LSBPTR16(&(value));                                            \
        }                                                                      \
    } while (false);

#define RMF_READ_ULONG(ptr, value, offset)                                     \
    do                                                                         \
    {                                                                          \
        memcpy(&(value), reinterpret_cast<GUInt32 *>((ptr) + (offset)),        \
               sizeof(GUInt32));                                               \
        if (poDS->bBigEndian)                                                  \
        {                                                                      \
            CPL_MSBPTR32(&(value));                                            \
        }                                                                      \
        else                                                                   \
        {                                                                      \
            CPL_LSBPTR32(&(value));                                            \
        }                                                                      \
    } while (false);

#define RMF_READ_LONG(ptr, value, offset) RMF_READ_ULONG(ptr, value, offset)

#define RMF_READ_DOUBLE(ptr, value, offset)                                    \
    do                                                                         \
    {                                                                          \
        memcpy(&(value), reinterpret_cast<double *>((ptr) + (offset)),         \
               sizeof(double));                                                \
        if (poDS->bBigEndian)                                                  \
        {                                                                      \
            CPL_MSBPTR64(&(value));                                            \
        }                                                                      \
        else                                                                   \
        {                                                                      \
            CPL_LSBPTR64(&(value));                                            \
        }                                                                      \
    } while (false);

    /* -------------------------------------------------------------------- */
    /*  Read the main header.                                               */
    /* -------------------------------------------------------------------- */

    {
        GByte abyHeader[RMF_HEADER_SIZE] = {};

        VSIFSeekL(poDS->fp, nNextHeaderOffset, SEEK_SET);
        if (VSIFReadL(abyHeader, 1, sizeof(abyHeader), poDS->fp) !=
            sizeof(abyHeader))
        {
            delete poDS;
            return nullptr;
        }

        if (memcmp(abyHeader, RMF_SigMTW, sizeof(RMF_SigMTW)) == 0)
        {
            poDS->eRMFType = RMFT_MTW;
        }
        else if (memcmp(abyHeader, RMF_SigRSW_BE, sizeof(RMF_SigRSW_BE)) == 0)
        {
            poDS->eRMFType = RMFT_RSW;
            poDS->bBigEndian = true;
        }
        else
        {
            poDS->eRMFType = RMFT_RSW;
        }

        memcpy(poDS->sHeader.bySignature, abyHeader, RMF_SIGNATURE_SIZE);
        RMF_READ_ULONG(abyHeader, poDS->sHeader.iVersion, 4);
        RMF_READ_ULONG(abyHeader, poDS->sHeader.nSize, 8);
        RMF_READ_ULONG(abyHeader, poDS->sHeader.nOvrOffset, 12);
        RMF_READ_ULONG(abyHeader, poDS->sHeader.iUserID, 16);
        memcpy(poDS->sHeader.byName, abyHeader + 20,
               sizeof(poDS->sHeader.byName));
        poDS->sHeader.byName[sizeof(poDS->sHeader.byName) - 1] = '\0';
        RMF_READ_ULONG(abyHeader, poDS->sHeader.nBitDepth, 52);
        RMF_READ_ULONG(abyHeader, poDS->sHeader.nHeight, 56);
        RMF_READ_ULONG(abyHeader, poDS->sHeader.nWidth, 60);
        RMF_READ_ULONG(abyHeader, poDS->sHeader.nXTiles, 64);
        RMF_READ_ULONG(abyHeader, poDS->sHeader.nYTiles, 68);
        RMF_READ_ULONG(abyHeader, poDS->sHeader.nTileHeight, 72);
        RMF_READ_ULONG(abyHeader, poDS->sHeader.nTileWidth, 76);
        RMF_READ_ULONG(abyHeader, poDS->sHeader.nLastTileHeight, 80);
        RMF_READ_ULONG(abyHeader, poDS->sHeader.nLastTileWidth, 84);
        RMF_READ_ULONG(abyHeader, poDS->sHeader.nROIOffset, 88);
        RMF_READ_ULONG(abyHeader, poDS->sHeader.nROISize, 92);
        RMF_READ_ULONG(abyHeader, poDS->sHeader.nClrTblOffset, 96);
        RMF_READ_ULONG(abyHeader, poDS->sHeader.nClrTblSize, 100);
        RMF_READ_ULONG(abyHeader, poDS->sHeader.nTileTblOffset, 104);
        RMF_READ_ULONG(abyHeader, poDS->sHeader.nTileTblSize, 108);
        RMF_READ_LONG(abyHeader, poDS->sHeader.iMapType, 124);
        RMF_READ_LONG(abyHeader, poDS->sHeader.iProjection, 128);
        RMF_READ_LONG(abyHeader, poDS->sHeader.iEPSGCode, 132);
        RMF_READ_DOUBLE(abyHeader, poDS->sHeader.dfScale, 136);
        RMF_READ_DOUBLE(abyHeader, poDS->sHeader.dfResolution, 144);
        RMF_READ_DOUBLE(abyHeader, poDS->sHeader.dfPixelSize, 152);
        RMF_READ_DOUBLE(abyHeader, poDS->sHeader.dfLLY, 160);
        RMF_READ_DOUBLE(abyHeader, poDS->sHeader.dfLLX, 168);
        RMF_READ_DOUBLE(abyHeader, poDS->sHeader.dfStdP1, 176);
        RMF_READ_DOUBLE(abyHeader, poDS->sHeader.dfStdP2, 184);
        RMF_READ_DOUBLE(abyHeader, poDS->sHeader.dfCenterLong, 192);
        RMF_READ_DOUBLE(abyHeader, poDS->sHeader.dfCenterLat, 200);
        poDS->sHeader.iCompression = *(abyHeader + 208);
        poDS->sHeader.iMaskType = *(abyHeader + 209);
        poDS->sHeader.iMaskStep = *(abyHeader + 210);
        poDS->sHeader.iFrameFlag = *(abyHeader + 211);
        RMF_READ_ULONG(abyHeader, poDS->sHeader.nFlagsTblOffset, 212);
        RMF_READ_ULONG(abyHeader, poDS->sHeader.nFlagsTblSize, 216);
        RMF_READ_ULONG(abyHeader, poDS->sHeader.nFileSize0, 220);
        RMF_READ_ULONG(abyHeader, poDS->sHeader.nFileSize1, 224);
        poDS->sHeader.iUnknown = *(abyHeader + 228);
        poDS->sHeader.iGeorefFlag = *(abyHeader + 244);
        poDS->sHeader.iInverse = *(abyHeader + 245);
        poDS->sHeader.iJpegQuality = *(abyHeader + 246);
        memcpy(poDS->sHeader.abyInvisibleColors, abyHeader + 248,
               sizeof(poDS->sHeader.abyInvisibleColors));
        RMF_READ_DOUBLE(abyHeader, poDS->sHeader.adfElevMinMax[0], 280);
        RMF_READ_DOUBLE(abyHeader, poDS->sHeader.adfElevMinMax[1], 288);
        RMF_READ_DOUBLE(abyHeader, poDS->sHeader.dfNoData, 296);

        RMF_READ_ULONG(abyHeader, poDS->sHeader.iElevationUnit, 304);
        poDS->sHeader.iElevationType = *(abyHeader + 308);
        RMF_READ_ULONG(abyHeader, poDS->sHeader.nExtHdrOffset, 312);
        RMF_READ_ULONG(abyHeader, poDS->sHeader.nExtHdrSize, 316);
        poDS->SetMetadataItem(MD_SCALE_KEY,
                              CPLSPrintf("1 : %u", int(poDS->sHeader.dfScale)));
        poDS->SetMetadataItem(MD_NAME_KEY,
                              CPLSPrintf("%s", poDS->sHeader.byName));
        poDS->SetMetadataItem(MD_VERSION_KEY,
                              CPLSPrintf("%d", poDS->sHeader.iVersion));
        poDS->SetMetadataItem(MD_MATH_BASE_MAP_TYPE_KEY,
                              CPLSPrintf("%d", poDS->sHeader.iMapType));
        poDS->SetMetadataItem(MD_MATH_BASE_PROJECTION_KEY,
                              CPLSPrintf("%d", poDS->sHeader.iProjection));
    }

    if (poDS->sHeader.nTileTblSize % (sizeof(GUInt32) * 2))
    {
        CPLError(CE_Warning, CPLE_IllegalArg, "Invalid tile table size.");
        delete poDS;
        return nullptr;
    }

    bool bInvalidTileSize;
    try
    {
        uint64_t nMaxTileBits =
            (CPLSM(static_cast<uint64_t>(2)) *
             CPLSM(static_cast<uint64_t>(poDS->sHeader.nTileWidth)) *
             CPLSM(static_cast<uint64_t>(poDS->sHeader.nTileHeight)) *
             CPLSM(static_cast<uint64_t>(poDS->sHeader.nBitDepth)))
                .v();
        bInvalidTileSize =
            (nMaxTileBits >
             static_cast<uint64_t>(std::numeric_limits<GUInt32>::max()));
    }
    catch (...)
    {
        bInvalidTileSize = true;
    }
    if (bInvalidTileSize)
    {
        CPLError(CE_Warning, CPLE_IllegalArg,
                 "Invalid tile size. Width %lu, height %lu, bit depth %lu.",
                 static_cast<unsigned long>(poDS->sHeader.nTileWidth),
                 static_cast<unsigned long>(poDS->sHeader.nTileHeight),
                 static_cast<unsigned long>(poDS->sHeader.nBitDepth));
        delete poDS;
        return nullptr;
    }

    if (poDS->sHeader.nLastTileWidth > poDS->sHeader.nTileWidth ||
        poDS->sHeader.nLastTileHeight > poDS->sHeader.nTileHeight)
    {
        CPLError(CE_Warning, CPLE_IllegalArg,
                 "Invalid last tile size %lu x %lu. "
                 "It can't be greater than %lu x %lu.",
                 static_cast<unsigned long>(poDS->sHeader.nLastTileWidth),
                 static_cast<unsigned long>(poDS->sHeader.nLastTileHeight),
                 static_cast<unsigned long>(poDS->sHeader.nTileWidth),
                 static_cast<unsigned long>(poDS->sHeader.nTileHeight));
        delete poDS;
        return nullptr;
    }

    if (poParentDS != nullptr)
    {
        if (0 != memcmp(poDS->sHeader.bySignature,
                        poParentDS->sHeader.bySignature, RMF_SIGNATURE_SIZE))
        {
            CPLError(CE_Warning, CPLE_IllegalArg,
                     "Invalid subheader signature.");
            delete poDS;
            return nullptr;
        }
    }

    /* -------------------------------------------------------------------- */
    /*  Read the extended header.                                           */
    /* -------------------------------------------------------------------- */

    if (poDS->sHeader.nExtHdrOffset &&
        poDS->sHeader.nExtHdrSize >= RMF_MIN_EXT_HEADER_SIZE)
    {
        if (poDS->sHeader.nExtHdrSize > RMF_MAX_EXT_HEADER_SIZE)
        {
            CPLError(CE_Failure, CPLE_FileIO, "RMF File malformed");
            delete poDS;
            return nullptr;
        }
        GByte *pabyExtHeader =
            static_cast<GByte *>(CPLCalloc(poDS->sHeader.nExtHdrSize, 1));
        if (pabyExtHeader == nullptr)
        {
            delete poDS;
            return nullptr;
        }

        VSIFSeekL(poDS->fp, poDS->GetFileOffset(poDS->sHeader.nExtHdrOffset),
                  SEEK_SET);
        VSIFReadL(pabyExtHeader, 1, poDS->sHeader.nExtHdrSize, poDS->fp);

        RMF_READ_LONG(pabyExtHeader, poDS->sExtHeader.nEllipsoid, 24);
        RMF_READ_LONG(pabyExtHeader, poDS->sExtHeader.nVertDatum, 28);
        RMF_READ_LONG(pabyExtHeader, poDS->sExtHeader.nDatum, 32);
        RMF_READ_LONG(pabyExtHeader, poDS->sExtHeader.nZone, 36);

        CPLFree(pabyExtHeader);
    }

    CPLDebug("RMF", "Version %d", poDS->sHeader.iVersion);

    constexpr GUInt32 ROI_MAX_SIZE_TO_AVOID_EXCESSIVE_RAM_USAGE =
        10 * 1024 * 1024;
#ifdef DEBUG

    CPLDebug("RMF",
             "%s image has width %d, height %d, bit depth %d, "
             "compression scheme %d, %s, nodata %f",
             (poDS->eRMFType == RMFT_MTW) ? "MTW" : "RSW", poDS->sHeader.nWidth,
             poDS->sHeader.nHeight, poDS->sHeader.nBitDepth,
             poDS->sHeader.iCompression,
             poDS->bBigEndian ? "big endian" : "little endian",
             poDS->sHeader.dfNoData);
    CPLDebug("RMF",
             "Size %d, offset to overview %#lx, user ID %d, "
             "ROI offset %#lx, ROI size %d",
             poDS->sHeader.nSize,
             static_cast<unsigned long>(poDS->sHeader.nOvrOffset),
             poDS->sHeader.iUserID,
             static_cast<unsigned long>(poDS->sHeader.nROIOffset),
             poDS->sHeader.nROISize);
    CPLDebug("RMF", "Map type %d, projection %d, scale %f, resolution %f, ",
             poDS->sHeader.iMapType, poDS->sHeader.iProjection,
             poDS->sHeader.dfScale, poDS->sHeader.dfResolution);
    CPLDebug("RMF", "EPSG %d ", poDS->sHeader.iEPSGCode);
    CPLDebug("RMF", "Georeferencing: pixel size %f, LLX %f, LLY %f",
             poDS->sHeader.dfPixelSize, poDS->sHeader.dfLLX,
             poDS->sHeader.dfLLY);

    if (poDS->sHeader.nROIOffset &&
        poDS->sHeader.nROISize >= sizeof(RSWFrame) &&
        poDS->sHeader.nROISize <= ROI_MAX_SIZE_TO_AVOID_EXCESSIVE_RAM_USAGE)
    {
        GByte *pabyROI = reinterpret_cast<GByte *>(
            VSI_MALLOC_VERBOSE(poDS->sHeader.nROISize));
        if (pabyROI == nullptr)
        {
            delete poDS;
            return nullptr;
        }

        VSIFSeekL(poDS->fp, poDS->GetFileOffset(poDS->sHeader.nROIOffset),
                  SEEK_SET);
        if (VSIFReadL(pabyROI, poDS->sHeader.nROISize, 1, poDS->fp) != 1)
        {
            CPLError(CE_Failure, CPLE_FileIO, "Cannot read ROI");
            CPLFree(pabyROI);
            delete poDS;
            return nullptr;
        }

        GInt32 nValue;

        CPLDebug("RMF", "ROI coordinates:");
        /* coverity[tainted_data] */
        for (GUInt32 i = 0; i + sizeof(nValue) <= poDS->sHeader.nROISize;
             i += sizeof(nValue))
        {
            RMF_READ_LONG(pabyROI, nValue, i);
            CPLDebug("RMF", "%d", nValue);
        }

        CPLFree(pabyROI);
    }
#endif
    if (poDS->sHeader.nWidth >= INT_MAX || poDS->sHeader.nHeight >= INT_MAX ||
        !GDALCheckDatasetDimensions(poDS->sHeader.nWidth,
                                    poDS->sHeader.nHeight))
    {
        delete poDS;
        return nullptr;
    }

    /* -------------------------------------------------------------------- */
    /*  Read array of blocks offsets/sizes.                                 */
    /* -------------------------------------------------------------------- */

    // To avoid useless excessive memory allocation
    if (poDS->sHeader.nTileTblSize > 1000000)
    {
        VSIFSeekL(poDS->fp, 0, SEEK_END);
        vsi_l_offset nFileSize = VSIFTellL(poDS->fp);
        if (nFileSize < poDS->sHeader.nTileTblSize)
        {
            delete poDS;
            return nullptr;
        }
    }

    if (VSIFSeekL(poDS->fp, poDS->GetFileOffset(poDS->sHeader.nTileTblOffset),
                  SEEK_SET) < 0)
    {
        delete poDS;
        return nullptr;
    }

    poDS->paiTiles =
        reinterpret_cast<GUInt32 *>(VSIMalloc(poDS->sHeader.nTileTblSize));
    if (!poDS->paiTiles)
    {
        delete poDS;
        return nullptr;
    }

    if (VSIFReadL(poDS->paiTiles, 1, poDS->sHeader.nTileTblSize, poDS->fp) <
        poDS->sHeader.nTileTblSize)
    {
        CPLDebug("RMF", "Can't read tiles offsets/sizes table.");
        delete poDS;
        return nullptr;
    }

#ifdef CPL_MSB
    if (!poDS->bBigEndian)
    {
        for (GUInt32 i = 0; i < poDS->sHeader.nTileTblSize / sizeof(GUInt32);
             i++)
            CPL_SWAP32PTR(poDS->paiTiles + i);
    }
#else
    if (poDS->bBigEndian)
    {
        for (GUInt32 i = 0; i < poDS->sHeader.nTileTblSize / sizeof(GUInt32);
             i++)
            CPL_SWAP32PTR(poDS->paiTiles + i);
    }
#endif

#ifdef DEBUG
    CPLDebug("RMF", "List of block offsets/sizes:");

    for (GUInt32 i = 0; i < poDS->sHeader.nTileTblSize / sizeof(GUInt32);
         i += 2)
    {
        CPLDebug("RMF", "    %u / %u", poDS->paiTiles[i],
                 poDS->paiTiles[i + 1]);
    }
#endif

    /* -------------------------------------------------------------------- */
    /*  Set up essential image parameters.                                  */
    /* -------------------------------------------------------------------- */
    GDALDataType eType = GDT_Byte;

    poDS->nRasterXSize = poDS->sHeader.nWidth;
    poDS->nRasterYSize = poDS->sHeader.nHeight;

    if (poDS->eRMFType == RMFT_RSW)
    {
        switch (poDS->sHeader.nBitDepth)
        {
            case 32:
            case 24:
            case 16:
                poDS->nBands = 3;
                break;
            case 1:
            case 4:
            case 8:
                if (poParentDS != nullptr &&
                    poParentDS->poColorTable != nullptr)
                {
                    poDS->poColorTable = poParentDS->poColorTable->Clone();
                }
                else
                {
                    // Allocate memory for colour table and read it
                    poDS->nColorTableSize = 1 << poDS->sHeader.nBitDepth;
                    GUInt32 nExpectedColorTableBytes =
                        poDS->nColorTableSize * 4;
                    if (nExpectedColorTableBytes > poDS->sHeader.nClrTblSize)
                    {
                        // We could probably test for strict equality in
                        // the above test ???
                        CPLDebug("RMF",
                                 "Wrong color table size. "
                                 "Expected %u, got %u.",
                                 nExpectedColorTableBytes,
                                 poDS->sHeader.nClrTblSize);
                        delete poDS;
                        return nullptr;
                    }
                    poDS->pabyColorTable = reinterpret_cast<GByte *>(
                        VSIMalloc(nExpectedColorTableBytes));
                    if (poDS->pabyColorTable == nullptr)
                    {
                        CPLDebug("RMF", "Can't allocate color table.");
                        delete poDS;
                        return nullptr;
                    }
                    if (VSIFSeekL(
                            poDS->fp,
                            poDS->GetFileOffset(poDS->sHeader.nClrTblOffset),
                            SEEK_SET) < 0)
                    {
                        CPLDebug("RMF", "Can't seek to color table location.");
                        delete poDS;
                        return nullptr;
                    }
                    if (VSIFReadL(poDS->pabyColorTable, 1,
                                  nExpectedColorTableBytes,
                                  poDS->fp) < nExpectedColorTableBytes)
                    {
                        CPLDebug("RMF", "Can't read color table.");
                        delete poDS;
                        return nullptr;
                    }

                    poDS->poColorTable = new GDALColorTable();
                    for (GUInt32 i = 0; i < poDS->nColorTableSize; i++)
                    {
                        const GDALColorEntry oEntry = {
                            poDS->pabyColorTable[i * 4],      // Red
                            poDS->pabyColorTable[i * 4 + 1],  // Green
                            poDS->pabyColorTable[i * 4 + 2],  // Blue
                            255                               // Alpha
                        };

                        poDS->poColorTable->SetColorEntry(i, &oEntry);
                    }
                }
                poDS->nBands = 1;
                break;
            default:
                CPLError(CE_Warning, CPLE_IllegalArg,
                         "Invalid RSW bit depth %lu.",
                         static_cast<unsigned long>(poDS->sHeader.nBitDepth));
                delete poDS;
                return nullptr;
        }
        eType = GDT_Byte;
    }
    else
    {
        poDS->nBands = 1;
        if (poDS->sHeader.nBitDepth == 8)
        {
            eType = GDT_Byte;
        }
        else if (poDS->sHeader.nBitDepth == 16)
        {
            eType = GDT_Int16;
        }
        else if (poDS->sHeader.nBitDepth == 32)
        {
            eType = GDT_Int32;
        }
        else if (poDS->sHeader.nBitDepth == 64)
        {
            eType = GDT_Float64;
        }
        else
        {
            CPLError(CE_Warning, CPLE_IllegalArg, "Invalid MTW bit depth %lu.",
                     static_cast<unsigned long>(poDS->sHeader.nBitDepth));
            delete poDS;
            return nullptr;
        }
    }

    if (poDS->sHeader.nTileWidth == 0 || poDS->sHeader.nTileWidth > INT_MAX ||
        poDS->sHeader.nTileHeight == 0 || poDS->sHeader.nTileHeight > INT_MAX)
    {
        CPLDebug("RMF", "Invalid tile dimension : %u x %u",
                 poDS->sHeader.nTileWidth, poDS->sHeader.nTileHeight);
        delete poDS;
        return nullptr;
    }

    const int nDataSize = GDALGetDataTypeSizeBytes(eType);
    const int nBlockXSize = static_cast<int>(poDS->sHeader.nTileWidth);
    const int nBlockYSize = static_cast<int>(poDS->sHeader.nTileHeight);
    if (nDataSize == 0 || nBlockXSize > INT_MAX / nBlockYSize ||
        nBlockYSize > INT_MAX / nDataSize ||
        nBlockXSize > INT_MAX / (nBlockYSize * nDataSize))
    {
        CPLDebug("RMF", "Too big raster / tile dimension");
        delete poDS;
        return nullptr;
    }

    poDS->nXTiles = DIV_ROUND_UP(poDS->nRasterXSize, nBlockXSize);
    poDS->nYTiles = DIV_ROUND_UP(poDS->nRasterYSize, nBlockYSize);

#ifdef DEBUG
    CPLDebug("RMF", "Image is %d tiles wide, %d tiles long", poDS->nXTiles,
             poDS->nYTiles);
#endif

    /* -------------------------------------------------------------------- */
    /*  Choose compression scheme.                                          */
    /* -------------------------------------------------------------------- */
    if (CE_None != poDS->SetupCompression(eType, poOpenInfo->pszFilename))
    {
        delete poDS;
        return nullptr;
    }

    if (poOpenInfo->eAccess == GA_Update)
    {
        if (poParentDS == nullptr)
        {
            if (CE_None !=
                poDS->InitCompressorData(poOpenInfo->papszOpenOptions))
            {
                delete poDS;
                return nullptr;
            }
        }
        else
        {
            poDS->poCompressData = poParentDS->poCompressData;
        }
    }
    /* -------------------------------------------------------------------- */
    /*  Create band information objects.                                    */
    /* -------------------------------------------------------------------- */
    for (int iBand = 1; iBand <= poDS->nBands; iBand++)
        poDS->SetBand(iBand, new RMFRasterBand(poDS, iBand, eType));

    poDS->SetupNBits();

    if (poDS->nBands > 1)
    {
        poDS->SetMetadataItem("INTERLEAVE", "PIXEL", "IMAGE_STRUCTURE");
    }
    /* -------------------------------------------------------------------- */
    /*  Set up projection.                                                  */
    /*                                                                      */
    /*  XXX: If projection value is not specified, but image still have     */
    /*  georeferencing information, assume Gauss-Kruger projection.         */
    /* -------------------------------------------------------------------- */
    if (poDS->sHeader.iEPSGCode > RMF_EPSG_MIN_CODE ||
        poDS->sHeader.iProjection > 0 ||
        (poDS->sHeader.dfPixelSize != 0.0 && poDS->sHeader.dfLLX != 0.0 &&
         poDS->sHeader.dfLLY != 0.0))
    {
        GInt32 nProj =
            (poDS->sHeader.iProjection) ? poDS->sHeader.iProjection : 1;
        double padfPrjParams[8] = {poDS->sHeader.dfStdP1,
                                   poDS->sHeader.dfStdP2,
                                   poDS->sHeader.dfCenterLat,
                                   poDS->sHeader.dfCenterLong,
                                   1.0,
                                   0.0,
                                   0.0,
                                   0.0};

        // XXX: Compute zone number for Gauss-Kruger (Transverse Mercator)
        // projection if it is not specified.
        if (nProj == 1L && poDS->sHeader.dfCenterLong == 0.0)
        {
            if (poDS->sExtHeader.nZone == 0)
            {
                double centerXCoord =
                    poDS->sHeader.dfLLX +
                    (poDS->nRasterXSize * poDS->sHeader.dfPixelSize / 2.0);
                padfPrjParams[7] = floor((centerXCoord - 500000.0) / 1000000.0);
            }
            else
            {
                padfPrjParams[7] = poDS->sExtHeader.nZone;
            }
        }

        OGRErr res = OGRERR_FAILURE;
        if (nProj >= 0 &&
            (poDS->sExtHeader.nDatum >= 0 || poDS->sExtHeader.nEllipsoid >= 0))
        {
            res = poDS->m_oSRS.importFromPanorama(
                nProj, poDS->sExtHeader.nDatum, poDS->sExtHeader.nEllipsoid,
                padfPrjParams);
        }

        if (poDS->sHeader.iEPSGCode > RMF_EPSG_MIN_CODE &&
            (OGRERR_NONE != res || poDS->m_oSRS.IsLocal()))
        {
            res = poDS->m_oSRS.importFromEPSG(poDS->sHeader.iEPSGCode);
        }

        const char *pszSetVertCS =
            CSLFetchNameValueDef(poOpenInfo->papszOpenOptions, "RMF_SET_VERTCS",
                                 CPLGetConfigOption("RMF_SET_VERTCS", "NO"));
        if (CPLTestBool(pszSetVertCS) && res == OGRERR_NONE &&
            poDS->sExtHeader.nVertDatum > 0)
        {
            poDS->m_oSRS.importVertCSFromPanorama(poDS->sExtHeader.nVertDatum);
        }
    }

    /* -------------------------------------------------------------------- */
    /*  Set up georeferencing.                                              */
    /* -------------------------------------------------------------------- */
    if ((poDS->eRMFType == RMFT_RSW && poDS->sHeader.iGeorefFlag) ||
        (poDS->eRMFType == RMFT_MTW && poDS->sHeader.dfPixelSize != 0.0))
    {
        poDS->m_gt[0] = poDS->sHeader.dfLLX;
        poDS->m_gt[3] = poDS->sHeader.dfLLY +
                        poDS->nRasterYSize * poDS->sHeader.dfPixelSize;
        poDS->m_gt[1] = poDS->sHeader.dfPixelSize;
        poDS->m_gt[5] = -poDS->sHeader.dfPixelSize;
        poDS->m_gt[2] = 0.0;
        poDS->m_gt[4] = 0.0;
    }

    /* -------------------------------------------------------------------- */
    /*  Set units.                                                          */
    /* -------------------------------------------------------------------- */

    if (poDS->eRMFType == RMFT_MTW)
    {
        CPLFree(poDS->pszUnitType);
        poDS->pszUnitType = RMFUnitTypeToStr(poDS->sHeader.iElevationUnit);
    }

    /* -------------------------------------------------------------------- */
    /*  Report some other dataset related information.                      */
    /* -------------------------------------------------------------------- */

    if (poDS->eRMFType == RMFT_MTW)
    {
        char szTemp[256] = {};

        snprintf(szTemp, sizeof(szTemp), "%g", poDS->sHeader.adfElevMinMax[0]);
        poDS->SetMetadataItem("ELEVATION_MINIMUM", szTemp);

        snprintf(szTemp, sizeof(szTemp), "%g", poDS->sHeader.adfElevMinMax[1]);
        poDS->SetMetadataItem("ELEVATION_MAXIMUM", szTemp);

        poDS->SetMetadataItem("ELEVATION_UNITS", poDS->pszUnitType);

        snprintf(szTemp, sizeof(szTemp), "%d", poDS->sHeader.iElevationType);
        poDS->SetMetadataItem("ELEVATION_TYPE", szTemp);
    }

    /* -------------------------------------------------------------------- */
    /*      Check for overviews.                                            */
    /* -------------------------------------------------------------------- */
    if (nNextHeaderOffset == 0 && poParentDS == nullptr)
    {
        poDS->oOvManager.Initialize(poDS, poOpenInfo->pszFilename);
    }

    /* Set frame */
    if (poDS->sHeader.nROIOffset &&
        poDS->sHeader.nROISize >= sizeof(RSWFrame) &&
        poDS->sHeader.nROISize <= ROI_MAX_SIZE_TO_AVOID_EXCESSIVE_RAM_USAGE)
    {
        GByte *pabyROI = reinterpret_cast<GByte *>(
            VSI_MALLOC_VERBOSE(poDS->sHeader.nROISize));
        if (pabyROI == nullptr)
        {
            delete poDS;
            return nullptr;
        }

        VSIFSeekL(poDS->fp, poDS->GetFileOffset(poDS->sHeader.nROIOffset),
                  SEEK_SET);
        if (VSIFReadL(pabyROI, poDS->sHeader.nROISize, 1, poDS->fp) != 1)
        {
            CPLError(CE_Failure, CPLE_FileIO, "Cannot read ROI");
            CPLFree(pabyROI);
            delete poDS;
            return nullptr;
        }

        GInt32 nFrameType;
        RMF_READ_LONG(pabyROI, nFrameType, 0);
        if (nFrameType == nPolygonType)
        {
            CPLString osWKT = "POLYGON((";
            bool bFirst = true;

            CPLDebug("RMF", "ROI coordinates:");
            /* coverity[tainted_data] */
            for (GUInt32 i = sizeof(RSWFrame);
                 i + sizeof(RSWFrameCoord) <= poDS->sHeader.nROISize;
                 i += sizeof(RSWFrameCoord))
            {
                GInt32 nX, nY;
                RMF_READ_LONG(pabyROI, nX, i);
                RMF_READ_LONG(pabyROI, nY, i + 4);

                CPLDebug("RMF", "X: %d, Y: %d", nX, nY);

                double dfX =
                    poDS->m_gt[0] + nX * poDS->m_gt[1] + nY * poDS->m_gt[2];
                double dfY =
                    poDS->m_gt[3] + nX * poDS->m_gt[4] + nY * poDS->m_gt[5];

                if (bFirst)
                {
                    osWKT += CPLSPrintf("%f %f", dfX, dfY);
                    bFirst = false;
                }
                else
                {
                    osWKT += CPLSPrintf(", %f %f", dfX, dfY);
                }
            }
            osWKT += "))";
            CPLDebug("RMF", "Frame WKT: %s", osWKT.c_str());
            poDS->SetMetadataItem(MD_FRAME_KEY, osWKT);
        }
        CPLFree(pabyROI);
    }

#undef RMF_READ_DOUBLE
#undef RMF_READ_LONG
#undef RMF_READ_ULONG

    if (poDS->sHeader.nFlagsTblOffset && poDS->sHeader.nFlagsTblSize)
    {
        VSIFSeekL(poDS->fp, poDS->GetFileOffset(poDS->sHeader.nFlagsTblOffset),
                  SEEK_SET);
        CPLDebug("RMF", "Blocks flags:");
        /* coverity[tainted_data] */
        for (GUInt32 i = 0; i < poDS->sHeader.nFlagsTblSize; i += sizeof(GByte))
        {
            GByte nValue;
            if (VSIFReadL(&nValue, 1, sizeof(nValue), poDS->fp) !=
                sizeof(nValue))
            {
                CPLDebug("RMF", "Cannot read Block flag at index %u", i);
                break;
            }
            CPLDebug("RMF", "Block %u -- flag %d", i, nValue);
        }
    }
    return poDS;
}

/************************************************************************/
/*                               Create()                               */
/************************************************************************/
GDALDataset *RMFDataset::Create(const char *pszFilename, int nXSize, int nYSize,
                                int nBandsIn, GDALDataType eType,
                                char **papszParamList)
{
    return Create(pszFilename, nXSize, nYSize, nBandsIn, eType, papszParamList,
                  nullptr, 1.0);
}

GDALDataset *RMFDataset::Create(const char *pszFilename, int nXSize, int nYSize,
                                int nBandsIn, GDALDataType eType,
                                char **papszParamList, RMFDataset *poParentDS,
                                double dfOvFactor)

{
    if (nBandsIn != 1 && nBandsIn != 3)
    {
        CPLError(CE_Failure, CPLE_NotSupported,
                 "RMF driver doesn't support %d bands. Must be 1 or 3.",
                 nBandsIn);

        return nullptr;
    }

    if (nBandsIn == 1 && eType != GDT_Byte && eType != GDT_Int16 &&
        eType != GDT_Int32 && eType != GDT_Float64)
    {
        CPLError(
            CE_Failure, CPLE_AppDefined,
            "Attempt to create RMF dataset with an illegal data type (%s), "
            "only Byte, Int16, Int32 and Float64 types supported "
            "by the format for single-band images.",
            GDALGetDataTypeName(eType));

        return nullptr;
    }

    if (nBandsIn == 3 && eType != GDT_Byte)
    {
        CPLError(
            CE_Failure, CPLE_AppDefined,
            "Attempt to create RMF dataset with an illegal data type (%s), "
            "only Byte type supported by the format for three-band images.",
            GDALGetDataTypeName(eType));

        return nullptr;
    }

    /* -------------------------------------------------------------------- */
    /*  Create the dataset.                                                 */
    /* -------------------------------------------------------------------- */
    RMFDataset *poDS = new RMFDataset();

    GUInt32 nBlockXSize =
        (nXSize < RMF_DEFAULT_BLOCKXSIZE) ? nXSize : RMF_DEFAULT_BLOCKXSIZE;
    GUInt32 nBlockYSize =
        (nYSize < RMF_DEFAULT_BLOCKYSIZE) ? nYSize : RMF_DEFAULT_BLOCKYSIZE;
    double dfScale;
    double dfResolution;
    double dfPixelSize;
    if (poParentDS == nullptr)
    {
        poDS->fp = VSIFOpenL(pszFilename, "w+b");
        if (poDS->fp == nullptr)
        {
            CPLError(CE_Failure, CPLE_OpenFailed, "Unable to create file %s.",
                     pszFilename);
            delete poDS;
            return nullptr;
        }

        const char *pszScaleValue =
            CSLFetchNameValue(papszParamList, MD_SCALE_KEY);
        if (pszScaleValue != nullptr && CPLStrnlen(pszScaleValue, 10) > 4)
        {
            dfScale = atof(pszScaleValue + 4);
        }
        else
        {
            dfScale = RMF_DEFAULT_SCALE;
        }
        dfResolution = RMF_DEFAULT_RESOLUTION;
        dfPixelSize = 1;

        if (CPLFetchBool(papszParamList, "MTW", false))
            poDS->eRMFType = RMFT_MTW;
        else
            poDS->eRMFType = RMFT_RSW;

        GUInt32 iVersion = RMF_VERSION;
        const char *pszRMFHUGE = CSLFetchNameValue(papszParamList, "RMFHUGE");

        if (pszRMFHUGE == nullptr)
            pszRMFHUGE = "NO";  // Keep old behavior by default

        if (EQUAL(pszRMFHUGE, "NO"))
        {
            iVersion = RMF_VERSION;
        }
        else if (EQUAL(pszRMFHUGE, "YES"))
        {
            iVersion = RMF_VERSION_HUGE;
        }
        else if (EQUAL(pszRMFHUGE, "IF_SAFER"))
        {
            const double dfImageSize =
                static_cast<double>(nXSize) * static_cast<double>(nYSize) *
                static_cast<double>(nBandsIn) *
                static_cast<double>(GDALGetDataTypeSizeBytes(eType));
            if (dfImageSize > 3.0 * 1024.0 * 1024.0 * 1024.0)
            {
                iVersion = RMF_VERSION_HUGE;
            }
            else
            {
                iVersion = RMF_VERSION;
            }
        }

        const char *pszValue = CSLFetchNameValue(papszParamList, "BLOCKXSIZE");
        if (pszValue != nullptr)
            nBlockXSize = atoi(pszValue);
        if (static_cast<int>(nBlockXSize) <= 0)
            nBlockXSize = RMF_DEFAULT_BLOCKXSIZE;

        pszValue = CSLFetchNameValue(papszParamList, "BLOCKYSIZE");
        if (pszValue != nullptr)
            nBlockYSize = atoi(pszValue);
        if (static_cast<int>(nBlockYSize) <= 0)
            nBlockYSize = RMF_DEFAULT_BLOCKXSIZE;

        if (poDS->eRMFType == RMFT_MTW)
            memcpy(poDS->sHeader.bySignature, RMF_SigMTW, RMF_SIGNATURE_SIZE);
        else
            memcpy(poDS->sHeader.bySignature, RMF_SigRSW, RMF_SIGNATURE_SIZE);
        poDS->sHeader.iVersion = iVersion;
        poDS->sHeader.nOvrOffset = 0x00;
    }
    else
    {
        poDS->fp = poParentDS->fp;
        memcpy(poDS->sHeader.bySignature, poParentDS->sHeader.bySignature,
               RMF_SIGNATURE_SIZE);
        poDS->sHeader.iVersion = poParentDS->sHeader.iVersion;
        poDS->eRMFType = poParentDS->eRMFType;
        nBlockXSize = poParentDS->sHeader.nTileWidth;
        nBlockYSize = poParentDS->sHeader.nTileHeight;
        dfScale = poParentDS->sHeader.dfScale;
        dfResolution = poParentDS->sHeader.dfResolution / dfOvFactor;
        dfPixelSize = poParentDS->sHeader.dfPixelSize * dfOvFactor;

        poDS->nHeaderOffset = poParentDS->GetLastOffset();
        poParentDS->sHeader.nOvrOffset =
            poDS->GetRMFOffset(poDS->nHeaderOffset, &poDS->nHeaderOffset);
        poParentDS->bHeaderDirty = true;
        VSIFSeekL(poDS->fp, poDS->nHeaderOffset, SEEK_SET);
        poDS->poParentDS = poParentDS;
        CPLDebug("RMF",
                 "Create overview subfile at " CPL_FRMT_GUIB
                 " with size %dx%d, parent overview offset %d",
                 poDS->nHeaderOffset, nXSize, nYSize,
                 poParentDS->sHeader.nOvrOffset);
    }
    /* -------------------------------------------------------------------- */
    /*  Fill the RMFHeader                                                  */
    /* -------------------------------------------------------------------- */
    CPLDebug("RMF", "Version %d", poDS->sHeader.iVersion);

    poDS->sHeader.iUserID = 0x00;
    memset(poDS->sHeader.byName, 0, sizeof(poDS->sHeader.byName));
    poDS->sHeader.nBitDepth = GDALGetDataTypeSizeBits(eType) * nBandsIn;
    poDS->sHeader.nHeight = nYSize;
    poDS->sHeader.nWidth = nXSize;
    poDS->sHeader.nTileWidth = nBlockXSize;
    poDS->sHeader.nTileHeight = nBlockYSize;

    poDS->nXTiles = poDS->sHeader.nXTiles =
        DIV_ROUND_UP(nXSize, poDS->sHeader.nTileWidth);
    poDS->nYTiles = poDS->sHeader.nYTiles =
        DIV_ROUND_UP(nYSize, poDS->sHeader.nTileHeight);
    poDS->sHeader.nLastTileHeight = nYSize % poDS->sHeader.nTileHeight;
    if (!poDS->sHeader.nLastTileHeight)
        poDS->sHeader.nLastTileHeight = poDS->sHeader.nTileHeight;
    poDS->sHeader.nLastTileWidth = nXSize % poDS->sHeader.nTileWidth;
    if (!poDS->sHeader.nLastTileWidth)
        poDS->sHeader.nLastTileWidth = poDS->sHeader.nTileWidth;

    // poDS->sHeader.nROIOffset = 0x00;
    // poDS->sHeader.nROISize = 0x00;

    vsi_l_offset nCurPtr = poDS->nHeaderOffset + RMF_HEADER_SIZE;

    // Extended header
    poDS->sHeader.nExtHdrOffset = poDS->GetRMFOffset(nCurPtr, &nCurPtr);
    poDS->sHeader.nExtHdrSize = RMF_EXT_HEADER_SIZE;
    nCurPtr += poDS->sHeader.nExtHdrSize;

    // Color table
    if (poDS->eRMFType == RMFT_RSW && nBandsIn == 1)
    {
        if (poDS->sHeader.nBitDepth > 8)
        {
            CPLError(CE_Failure, CPLE_AppDefined,
                     "Cannot create color table of RSW with nBitDepth = %d. "
                     "Retry with MTW ?",
                     poDS->sHeader.nBitDepth);
            delete poDS;
            return nullptr;
        }

        poDS->sHeader.nClrTblOffset = poDS->GetRMFOffset(nCurPtr, &nCurPtr);
        poDS->nColorTableSize = 1 << poDS->sHeader.nBitDepth;
        poDS->sHeader.nClrTblSize = poDS->nColorTableSize * 4;
        poDS->pabyColorTable =
            static_cast<GByte *>(VSI_MALLOC_VERBOSE(poDS->sHeader.nClrTblSize));
        if (poDS->pabyColorTable == nullptr)
        {
            delete poDS;
            return nullptr;
        }
        for (GUInt32 i = 0; i < poDS->nColorTableSize; i++)
        {
            poDS->pabyColorTable[i * 4 + 0] = static_cast<GByte>(i);
            poDS->pabyColorTable[i * 4 + 1] = static_cast<GByte>(i);
            poDS->pabyColorTable[i * 4 + 2] = static_cast<GByte>(i);
            poDS->pabyColorTable[i * 4 + 3] = 0;
        }
        nCurPtr += poDS->sHeader.nClrTblSize;
    }
    else
    {
        poDS->sHeader.nClrTblOffset = 0x00;
        poDS->sHeader.nClrTblSize = 0x00;
    }

    // Add room for ROI (frame)
    poDS->sHeader.nROIOffset = poDS->GetRMFOffset(nCurPtr, &nCurPtr);
    poDS->sHeader.nROISize = 0x00;
    nCurPtr +=
        sizeof(RSWFrame) +
        sizeof(RSWFrameCoord) *
            nMaxFramePointCount;  // Allocate nMaxFramePointCount coordinates for frame

    // Add blocks flags
    poDS->sHeader.nFlagsTblOffset = poDS->GetRMFOffset(nCurPtr, &nCurPtr);
    poDS->sHeader.nFlagsTblSize =
        sizeof(GByte) * poDS->sHeader.nXTiles * poDS->sHeader.nYTiles;
    nCurPtr += poDS->sHeader.nFlagsTblSize;

    // Blocks table
    poDS->sHeader.nTileTblOffset = poDS->GetRMFOffset(nCurPtr, &nCurPtr);
    poDS->sHeader.nTileTblSize =
        2 * sizeof(GUInt32) * poDS->sHeader.nXTiles * poDS->sHeader.nYTiles;
    poDS->paiTiles =
        static_cast<GUInt32 *>(CPLCalloc(poDS->sHeader.nTileTblSize, 1));
    // nCurPtr += poDS->sHeader.nTileTblSize;
    const GUInt32 nTileSize = poDS->sHeader.nTileWidth *
                              poDS->sHeader.nTileHeight *
                              GDALGetDataTypeSizeBytes(eType);
    poDS->sHeader.nSize =
        poDS->paiTiles[poDS->sHeader.nTileTblSize / 4 - 2] + nTileSize;

    // Elevation units
    poDS->sHeader.iElevationUnit = RMFStrToUnitType(poDS->pszUnitType);

    poDS->sHeader.iMapType = -1;
    poDS->sHeader.iProjection = -1;
    poDS->sHeader.iEPSGCode = -1;
    poDS->sHeader.dfScale = dfScale;
    poDS->sHeader.dfResolution = dfResolution;
    poDS->sHeader.dfPixelSize = dfPixelSize;
    poDS->sHeader.iMaskType = 0;
    poDS->sHeader.iMaskStep = 0;
    poDS->sHeader.iFrameFlag = 1;  // 1 - Frame not using
    // poDS->sHeader.nFlagsTblOffset = 0x00;
    // poDS->sHeader.nFlagsTblSize = 0x00;
    poDS->sHeader.nFileSize0 = 0x00;
    poDS->sHeader.nFileSize1 = 0x00;
    poDS->sHeader.iUnknown = 0;
    poDS->sHeader.iGeorefFlag = 0;
    poDS->sHeader.iInverse = 0;
    poDS->sHeader.iJpegQuality = 0;
    memset(poDS->sHeader.abyInvisibleColors, 0,
           sizeof(poDS->sHeader.abyInvisibleColors));
    poDS->sHeader.iElevationType = 0;

    poDS->nRasterXSize = nXSize;
    poDS->nRasterYSize = nYSize;
    poDS->eAccess = GA_Update;
    poDS->nBands = nBandsIn;

    if (poParentDS == nullptr)
    {
        poDS->sHeader.adfElevMinMax[0] = 0.0;
        poDS->sHeader.adfElevMinMax[1] = 0.0;
        poDS->sHeader.dfNoData = 0.0;
        poDS->sHeader.iCompression =
            GetCompressionType(CSLFetchNameValue(papszParamList, "COMPRESS"));
        if (CE_None != poDS->InitCompressorData(papszParamList))
        {
            delete poDS;
            return nullptr;
        }

        if (poDS->sHeader.iCompression == RMF_COMPRESSION_JPEG)
        {
            const char *pszJpegQuality =
                CSLFetchNameValue(papszParamList, "JPEG_QUALITY");
            if (pszJpegQuality == nullptr)
            {
                poDS->sHeader.iJpegQuality = 75;
            }
            else
            {
                int iJpegQuality = atoi(pszJpegQuality);
                if (iJpegQuality < 10 || iJpegQuality > 100)
                {
                    CPLError(CE_Failure, CPLE_IllegalArg,
                             "JPEG_QUALITY=%s is not a legal value in the "
                             "range 10-100.\n"
                             "Defaulting to 75",
                             pszJpegQuality);
                    iJpegQuality = 75;
                }
                poDS->sHeader.iJpegQuality = static_cast<GByte>(iJpegQuality);
            }
        }

        if (CE_None != poDS->SetupCompression(eType, pszFilename))
        {
            delete poDS;
            return nullptr;
        }
    }
    else
    {
        poDS->sHeader.adfElevMinMax[0] = poParentDS->sHeader.adfElevMinMax[0];
        poDS->sHeader.adfElevMinMax[1] = poParentDS->sHeader.adfElevMinMax[1];
        poDS->sHeader.dfNoData = poParentDS->sHeader.dfNoData;
        poDS->sHeader.iCompression = poParentDS->sHeader.iCompression;
        poDS->sHeader.iJpegQuality = poParentDS->sHeader.iJpegQuality;
        poDS->Decompress = poParentDS->Decompress;
        poDS->Compress = poParentDS->Compress;
        poDS->poCompressData = poParentDS->poCompressData;
    }

    if (nBandsIn > 1)
    {
        poDS->SetMetadataItem("INTERLEAVE", "PIXEL", "IMAGE_STRUCTURE");
    }

    poDS->WriteHeader();

    /* -------------------------------------------------------------------- */
    /*      Create band information objects.                                */
    /* -------------------------------------------------------------------- */
    for (int iBand = 1; iBand <= poDS->nBands; iBand++)
        poDS->SetBand(iBand, new RMFRasterBand(poDS, iBand, eType));

    poDS->SetupNBits();

    return GDALDataset::FromHandle(poDS);
}

// GIS Panorama 11 was introduced new format for huge files (greater than 3 Gb)
vsi_l_offset RMFDataset::GetFileOffset(GUInt32 iRMFOffset) const
{
    if (sHeader.iVersion >= RMF_VERSION_HUGE)
    {
        return (static_cast<vsi_l_offset>(iRMFOffset)) * RMF_HUGE_OFFSET_FACTOR;
    }

    return static_cast<vsi_l_offset>(iRMFOffset);
}

GUInt32 RMFDataset::GetRMFOffset(vsi_l_offset nFileOffset,
                                 vsi_l_offset *pnNewFileOffset) const
{
    if (sHeader.iVersion >= RMF_VERSION_HUGE)
    {
        // Round offset to next RMF_HUGE_OFFSET_FACTOR
        const GUInt32 iRMFOffset =
            static_cast<GUInt32>((nFileOffset + (RMF_HUGE_OFFSET_FACTOR - 1)) /
                                 RMF_HUGE_OFFSET_FACTOR);
        if (pnNewFileOffset != nullptr)
        {
            *pnNewFileOffset = GetFileOffset(iRMFOffset);
        }
        return iRMFOffset;
    }

    if (pnNewFileOffset != nullptr)
    {
        *pnNewFileOffset = nFileOffset;
    }
    return static_cast<GUInt32>(nFileOffset);
}

RMFDataset *RMFDataset::OpenOverview(RMFDataset *poParent,
                                     GDALOpenInfo *poOpenInfo)
{
    if (sHeader.nOvrOffset == 0)
    {
        return nullptr;
    }

    if (poParent == nullptr)
    {
        return nullptr;
    }

    vsi_l_offset nSubOffset = GetFileOffset(sHeader.nOvrOffset);

    CPLDebug("RMF",
             "Try to open overview subfile at " CPL_FRMT_GUIB " for '%s'",
             nSubOffset, poOpenInfo->pszFilename);

    if (!poParent->poOvrDatasets.empty())
    {
        if (poParent->GetFileOffset(poParent->sHeader.nOvrOffset) == nSubOffset)
        {
            CPLError(CE_Warning, CPLE_IllegalArg,
                     "Recursive subdataset list is detected. "
                     "Overview open failed.");
            return nullptr;
        }

        for (size_t n = 0; n != poParent->poOvrDatasets.size() - 1; ++n)
        {
            RMFDataset *poOvr(poParent->poOvrDatasets[n]);

            if (poOvr == nullptr)
                continue;
            if (poOvr->GetFileOffset(poOvr->sHeader.nOvrOffset) == nSubOffset)
            {
                CPLError(CE_Warning, CPLE_IllegalArg,
                         "Recursive subdataset list is detected. "
                         "Overview open failed.");
                return nullptr;
            }
        }
    }

    size_t nHeaderSize(RMF_HEADER_SIZE);
    GByte *pabyNewHeader;
    pabyNewHeader = static_cast<GByte *>(
        CPLRealloc(poOpenInfo->pabyHeader, nHeaderSize + 1));
    if (pabyNewHeader == nullptr)
    {
        CPLError(CE_Warning, CPLE_OutOfMemory,
                 "Can't allocate buffer for overview header");
        return nullptr;
    }

    poOpenInfo->pabyHeader = pabyNewHeader;
    memset(poOpenInfo->pabyHeader, 0, nHeaderSize + 1);
    VSIFSeekL(fp, nSubOffset, SEEK_SET);
    poOpenInfo->nHeaderBytes =
        static_cast<int>(VSIFReadL(poOpenInfo->pabyHeader, 1, nHeaderSize, fp));

    return Open(poOpenInfo, poParent, nSubOffset);
}

CPLErr RMFDataset::IBuildOverviews(const char *pszResampling, int nOverviews,
                                   const int *panOverviewList, int nBandsIn,
                                   const int *panBandList,
                                   GDALProgressFunc pfnProgress,
                                   void *pProgressData,
                                   CSLConstList papszOptions)
{
    bool bUseGenericHandling = false;

    if (GetAccess() != GA_Update)
    {
        CPLDebug("RMF", "File open for read-only accessing, "
                        "creating overviews externally.");

        bUseGenericHandling = true;
    }

    if (bUseGenericHandling)
    {
        if (!poOvrDatasets.empty())
        {
            CPLError(CE_Failure, CPLE_NotSupported,
                     "Cannot add external overviews when there are already "
                     "internal overviews");
            return CE_Failure;
        }

        return GDALDataset::IBuildOverviews(
            pszResampling, nOverviews, panOverviewList, nBandsIn, panBandList,
            pfnProgress, pProgressData, papszOptions);
    }

    if (nBandsIn != GetRasterCount())
    {
        CPLError(CE_Failure, CPLE_NotSupported,
                 "Generation of overviews in RMF is only "
                 "supported when operating on all bands.  "
                 "Operation failed.");
        return CE_Failure;
    }

    if (nOverviews == 0)
    {
        if (poOvrDatasets.empty())
        {
            return GDALDataset::IBuildOverviews(
                pszResampling, nOverviews, panOverviewList, nBandsIn,
                panBandList, pfnProgress, pProgressData, papszOptions);
        }
        return CleanOverviews();
    }

    // First destroy old overviews
    if (CE_None != CleanOverviews())
    {
        return CE_Failure;
    }

    CPLDebug("RMF", "Build overviews on dataset %d x %d size", GetRasterXSize(),
             GetRasterYSize());

    GDALDataType eMainType = GetRasterBand(1)->GetRasterDataType();
    RMFDataset *poParent = this;
    double prevOvLevel = 1.0;
    for (int n = 0; n != nOverviews; ++n)
    {
        int nOvLevel = panOverviewList[n];
        const int nOXSize = DIV_ROUND_UP(GetRasterXSize(), nOvLevel);
        const int nOYSize = DIV_ROUND_UP(GetRasterYSize(), nOvLevel);
        CPLDebug("RMF", "\tCreate overview #%d size %d x %d", nOvLevel, nOXSize,
                 nOYSize);

        RMFDataset *poOvrDataset;
        poOvrDataset = static_cast<RMFDataset *>(RMFDataset::Create(
            nullptr, nOXSize, nOYSize, GetRasterCount(), eMainType, nullptr,
            poParent, nOvLevel / prevOvLevel));

        if (poOvrDataset == nullptr)
        {
            CPLError(CE_Failure, CPLE_AppDefined,
                     "Can't create overview dataset #%d size %d x %d", nOvLevel,
                     nOXSize, nOYSize);
            return CE_Failure;
        }

        prevOvLevel = nOvLevel;
        poParent = poOvrDataset;
        poOvrDatasets.push_back(poOvrDataset);
    }

    GDALRasterBand ***papapoOverviewBands =
        static_cast<GDALRasterBand ***>(CPLCalloc(sizeof(void *), nBandsIn));
    GDALRasterBand **papoBandList =
        static_cast<GDALRasterBand **>(CPLCalloc(sizeof(void *), nBandsIn));

    for (int iBand = 0; iBand < nBandsIn; ++iBand)
    {
        GDALRasterBand *poBand = GetRasterBand(panBandList[iBand]);

        papoBandList[iBand] = poBand;
        papapoOverviewBands[iBand] = static_cast<GDALRasterBand **>(
            CPLCalloc(sizeof(void *), poBand->GetOverviewCount()));

        for (int i = 0; i < nOverviews; ++i)
        {
            papapoOverviewBands[iBand][i] = poBand->GetOverview(i);
        }
    }
#ifdef DEBUG
    for (int iBand = 0; iBand < nBandsIn; ++iBand)
    {
        CPLDebug("RMF", "Try to create overview for #%d size %d x %d",
                 iBand + 1, papoBandList[iBand]->GetXSize(),
                 papoBandList[iBand]->GetYSize());
        for (int i = 0; i < nOverviews; ++i)
        {
            CPLDebug("RMF", "\t%d x %d",
                     papapoOverviewBands[iBand][i]->GetXSize(),
                     papapoOverviewBands[iBand][i]->GetYSize());
        }
    }
#endif  // DEBUG
    CPLErr res;
    res = GDALRegenerateOverviewsMultiBand(
        nBandsIn, papoBandList, nOverviews, papapoOverviewBands, pszResampling,
        pfnProgress, pProgressData, papszOptions);

    for (int iBand = 0; iBand < nBandsIn; ++iBand)
    {
        CPLFree(papapoOverviewBands[iBand]);
    }

    CPLFree(papapoOverviewBands);
    CPLFree(papoBandList);

    return res;
}

CPLErr RMFDataset::IRasterIO(GDALRWFlag eRWFlag, int nXOff, int nYOff,
                             int nXSize, int nYSize, void *pData, int nBufXSize,
                             int nBufYSize, GDALDataType eBufType,
                             int nBandCount, BANDMAP_TYPE panBandMap,
                             GSpacing nPixelSpace, GSpacing nLineSpace,
                             GSpacing nBandSpace,
                             GDALRasterIOExtraArg *psExtraArg)
{
#ifdef DEBUG
    CPLDebug("RMF", "Dataset %p, %s %d %d %d %d, %d %d", this,
             (eRWFlag == GF_Read ? "Read" : "Write"), nXOff, nYOff, nXSize,
             nYSize, nBufXSize, nBufYSize);
#endif  // DEBUG
    if (eRWFlag == GF_Read && poCompressData != nullptr &&
        poCompressData->oThreadPool.GetThreadCount() > 0)
    {
        poCompressData->oThreadPool.WaitCompletion();
    }

    return GDALDataset::IRasterIO(eRWFlag, nXOff, nYOff, nXSize, nYSize, pData,
                                  nBufXSize, nBufYSize, eBufType, nBandCount,
                                  panBandMap, nPixelSpace, nLineSpace,
                                  nBandSpace, psExtraArg);
}

vsi_l_offset RMFDataset::GetLastOffset() const
{
    vsi_l_offset nLastTileOff = 0;
    GUInt32 nTiles(sHeader.nTileTblSize / sizeof(GUInt32));

    for (GUInt32 n = 0; n < nTiles; n += 2)
    {
        vsi_l_offset nTileOffset = GetFileOffset(paiTiles[n]);
        GUInt32 nTileBytes = paiTiles[n + 1];
        nLastTileOff = std::max(nLastTileOff, nTileOffset + nTileBytes);
    }

    nLastTileOff = std::max(nLastTileOff, GetFileOffset(sHeader.nROIOffset) +
                                              sHeader.nROISize);
    nLastTileOff = std::max(nLastTileOff, GetFileOffset(sHeader.nClrTblOffset) +
                                              sHeader.nClrTblSize);
    nLastTileOff =
        std::max(nLastTileOff,
                 GetFileOffset(sHeader.nTileTblOffset) + sHeader.nTileTblSize);
    nLastTileOff =
        std::max(nLastTileOff, GetFileOffset(sHeader.nFlagsTblOffset) +
                                   sHeader.nFlagsTblSize);
    nLastTileOff = std::max(nLastTileOff, GetFileOffset(sHeader.nExtHdrOffset) +
                                              sHeader.nExtHdrSize);
    return nLastTileOff;
}

CPLErr RMFDataset::CleanOverviews()
{
    if (sHeader.nOvrOffset == 0)
    {
        return CE_None;
    }

    if (GetAccess() != GA_Update)
    {
        CPLError(CE_Failure, CPLE_NotSupported,
                 "File open for read-only accessing, "
                 "overviews cleanup failed.");
        return CE_Failure;
    }

    if (poParentDS != nullptr)
    {
        CPLError(CE_Failure, CPLE_NotSupported,
                 "Overviews cleanup for non-root dataset is not possible.");
        return CE_Failure;
    }

    for (size_t n = 0; n != poOvrDatasets.size(); ++n)
    {
        GDALClose(poOvrDatasets[n]);
    }
    poOvrDatasets.clear();

    vsi_l_offset nLastTileOff = GetLastOffset();

    if (0 != VSIFSeekL(fp, 0, SEEK_END))
    {
        CPLError(CE_Failure, CPLE_FileIO,
                 "Failed to seek to end of file, "
                 "overviews cleanup failed.");
    }

    vsi_l_offset nFileSize = VSIFTellL(fp);
    if (nFileSize < nLastTileOff)
    {
        CPLError(CE_Failure, CPLE_FileIO,
                 "Invalid file offset, "
                 "overviews cleanup failed.");
        return CE_Failure;
    }

    CPLDebug("RMF", "Truncate to " CPL_FRMT_GUIB, nLastTileOff);
    CPLDebug("RMF", "File size:  " CPL_FRMT_GUIB, nFileSize);

    if (0 != VSIFTruncateL(fp, nLastTileOff))
    {
        CPLError(CE_Failure, CPLE_FileIO,
                 "Failed to truncate file, "
                 "overviews cleanup failed.");
        return CE_Failure;
    }

    sHeader.nOvrOffset = 0;
    bHeaderDirty = true;

    return CE_None;
}

/************************************************************************/
/*                         GetCompressionType()                         */
/************************************************************************/

GByte RMFDataset::GetCompressionType(const char *pszCompressName)
{
    if (pszCompressName == nullptr || EQUAL(pszCompressName, "NONE"))
    {
        return RMF_COMPRESSION_NONE;
    }
    else if (EQUAL(pszCompressName, "LZW"))
    {
        return RMF_COMPRESSION_LZW;
    }
    else if (EQUAL(pszCompressName, "JPEG"))
    {
        return RMF_COMPRESSION_JPEG;
    }
    else if (EQUAL(pszCompressName, "RMF_DEM"))
    {
        return RMF_COMPRESSION_DEM;
    }

    CPLError(CE_Failure, CPLE_AppDefined,
             "RMF: Unknown compression scheme <%s>.\n"
             "Defaults to NONE compression.",
             pszCompressName);
    return RMF_COMPRESSION_NONE;
}

/************************************************************************/
/*                        SetupCompression()                            */
/************************************************************************/

int RMFDataset::SetupCompression(GDALDataType eType, const char *pszFilename)
{
    /* -------------------------------------------------------------------- */
    /*  XXX: The DEM compression method seems to be only applicable         */
    /*  to Int32 data.                                                      */
    /* -------------------------------------------------------------------- */
    if (sHeader.iCompression == RMF_COMPRESSION_NONE)
    {
        Decompress = nullptr;
        Compress = nullptr;
    }
    else if (sHeader.iCompression == RMF_COMPRESSION_LZW)
    {
        Decompress = &LZWDecompress;
        Compress = &LZWCompress;
        SetMetadataItem("COMPRESSION", "LZW", "IMAGE_STRUCTURE");
    }
    else if (sHeader.iCompression == RMF_COMPRESSION_JPEG)
    {
        if (eType != GDT_Byte || nBands != RMF_JPEG_BAND_COUNT ||
            sHeader.nBitDepth != 24)
        {
            CPLError(CE_Failure, CPLE_AppDefined,
                     "RMF support only 24 bpp JPEG compressed files.");
            return CE_Failure;
        }
#ifdef HAVE_LIBJPEG
        CPLString oBuf;
        oBuf.Printf("%d", sHeader.iJpegQuality);
        Decompress = &JPEGDecompress;
        Compress = &JPEGCompress;
        SetMetadataItem("JPEG_QUALITY", oBuf.c_str(), "IMAGE_STRUCTURE");
        SetMetadataItem("COMPRESSION", "JPEG", "IMAGE_STRUCTURE");
#else   // HAVE_LIBJPEG
        CPLError(CE_Failure, CPLE_AppDefined,
                 "JPEG codec is needed to open <%s>.\n"
                 "Please rebuild GDAL with libjpeg support.",
                 pszFilename);
        return CE_Failure;
#endif  // HAVE_LIBJPEG
    }
    else if (sHeader.iCompression == RMF_COMPRESSION_DEM &&
             eType == GDT_Int32 && nBands == RMF_DEM_BAND_COUNT)
    {
        Decompress = &DEMDecompress;
        Compress = &DEMCompress;
        SetMetadataItem("COMPRESSION", "RMF_DEM", "IMAGE_STRUCTURE");
    }
    else
    {
        CPLError(CE_Failure, CPLE_AppDefined,
                 "Unknown compression #%d at file <%s>.", sHeader.iCompression,
                 pszFilename);
        return CE_Failure;
    }

    return CE_None;
}

void RMFDataset::WriteTileJobFunc(void *pData)
{
    RMFCompressionJob *psJob = static_cast<RMFCompressionJob *>(pData);
    RMFDataset *poDS = psJob->poDS;

    GByte *pabyTileData;
    size_t nTileSize;

    if (poDS->Compress)
    {
        // RMF doesn't store compressed tiles with size greater than 80% of
        // uncompressed size
        GUInt32 nMaxCompressedTileSize =
            static_cast<GUInt32>((psJob->nUncompressedBytes * 8) / 10);
        size_t nCompressedBytes =
            poDS->Compress(psJob->pabyUncompressedData,
                           static_cast<GUInt32>(psJob->nUncompressedBytes),
                           psJob->pabyCompressedData, nMaxCompressedTileSize,
                           psJob->nXSize, psJob->nYSize, poDS);
        if (nCompressedBytes == 0)
        {
            pabyTileData = psJob->pabyUncompressedData;
            nTileSize = psJob->nUncompressedBytes;
        }
        else
        {
            pabyTileData = psJob->pabyCompressedData;
            nTileSize = nCompressedBytes;
        }
    }
    else
    {
        pabyTileData = psJob->pabyUncompressedData;
        nTileSize = psJob->nUncompressedBytes;
    }

    {
        CPLMutexHolder oHolder(poDS->poCompressData->hWriteTileMutex);
        psJob->eResult = poDS->WriteRawTile(
            psJob->nBlockXOff, psJob->nBlockYOff, pabyTileData, nTileSize);
    }
    if (poDS->poCompressData->oThreadPool.GetThreadCount() > 0)
    {
        CPLMutexHolder oHolder(poDS->poCompressData->hReadyJobMutex);
        poDS->poCompressData->asReadyJobs.push_back(psJob);
    }
}

CPLErr RMFDataset::InitCompressorData(char **papszParamList)
{
    const char *pszNumThreads =
        CSLFetchNameValue(papszParamList, "NUM_THREADS");
    if (pszNumThreads == nullptr)
        pszNumThreads = CPLGetConfigOption("GDAL_NUM_THREADS", nullptr);

    int nThreads = 0;
    if (pszNumThreads != nullptr)
    {
        nThreads = EQUAL(pszNumThreads, "ALL_CPUS") ? CPLGetNumCPUs()
                                                    : atoi(pszNumThreads);
    }

    if (nThreads < 0)
    {
        nThreads = 0;
    }
    if (nThreads > 1024)
    {
        nThreads = 1024;
    }

    poCompressData = std::make_shared<RMFCompressData>();
    if (nThreads > 0)
    {
        if (!poCompressData->oThreadPool.Setup(nThreads, nullptr, nullptr))
        {
            CPLError(CE_Failure, CPLE_AppDefined,
                     "Can't setup %d compressor threads", nThreads);
            return CE_Failure;
        }
    }

    poCompressData->asJobs.resize(nThreads + 1);

    size_t nMaxTileBytes =
        sHeader.nTileWidth * sHeader.nTileHeight * sHeader.nBitDepth / 8;
    size_t nCompressBufferSize =
        2 * nMaxTileBytes * poCompressData->asJobs.size();
    poCompressData->pabyBuffers =
        static_cast<GByte *>(VSIMalloc(nCompressBufferSize));

    CPLDebug("RMF", "Setup %d compressor threads and allocate %lu bytes buffer",
             nThreads, static_cast<unsigned long>(nCompressBufferSize));
    if (poCompressData->pabyBuffers == nullptr)
    {
        CPLError(CE_Failure, CPLE_OutOfMemory,
                 "Can't allocate compress buffer of size %lu.",
                 static_cast<unsigned long>(nCompressBufferSize));
        return CE_Failure;
    }

    for (size_t i = 0; i != poCompressData->asJobs.size(); ++i)
    {
        RMFCompressionJob &sJob(poCompressData->asJobs[i]);
        sJob.pabyCompressedData =
            poCompressData->pabyBuffers + 2 * i * nMaxTileBytes;
        sJob.pabyUncompressedData = sJob.pabyCompressedData + nMaxTileBytes;
        poCompressData->asReadyJobs.push_back(&sJob);
    }

    if (nThreads > 0)
    {
        poCompressData->hReadyJobMutex = CPLCreateMutex();
        CPLReleaseMutex(poCompressData->hReadyJobMutex);
        poCompressData->hWriteTileMutex = CPLCreateMutex();
        CPLReleaseMutex(poCompressData->hWriteTileMutex);
    }

    return CE_None;
}

CPLErr RMFDataset::WriteTile(int nBlockXOff, int nBlockYOff, GByte *pabyData,
                             size_t nBytes, GUInt32 nRawXSize,
                             GUInt32 nRawYSize)
{
    RMFCompressionJob *poJob = nullptr;
    if (poCompressData == nullptr)
    {
        CPLError(CE_Failure, CPLE_AppDefined, "RMF: Compress data is null");
        return CE_Failure;
    }

    if (poCompressData->oThreadPool.GetThreadCount() > 0)
    {
        size_t nJobs(poCompressData->asJobs.size());

        poCompressData->oThreadPool.WaitCompletion(static_cast<int>(nJobs - 1));

        CPLMutexHolder oHolder(poCompressData->hReadyJobMutex);
        CPLAssert(!poCompressData->asReadyJobs.empty());
        poJob = poCompressData->asReadyJobs.front();
        poCompressData->asReadyJobs.pop_front();
    }
    else
    {
        poJob = poCompressData->asReadyJobs.front();
    }

    if (poJob->eResult != CE_None)
    {
        // One of the previous jobs is not done.
        // Detailed debug message is already emitted from WriteRawTile
        return poJob->eResult;
    }
    poJob->poDS = this;
    poJob->eResult = CE_Failure;
    poJob->nBlockXOff = nBlockXOff;
    poJob->nBlockYOff = nBlockYOff;
    poJob->nUncompressedBytes = nBytes;
    poJob->nXSize = nRawXSize;
    poJob->nYSize = nRawYSize;

    memcpy(poJob->pabyUncompressedData, pabyData, nBytes);

    if (poCompressData->oThreadPool.GetThreadCount() > 0)
    {
        if (!poCompressData->oThreadPool.SubmitJob(WriteTileJobFunc, poJob))
        {
            CPLError(CE_Failure, CPLE_NotSupported,
                     "Can't submit job to thread pool.");
            return CE_Failure;
        }
    }
    else
    {
        WriteTileJobFunc(poJob);
        if (poJob->eResult != CE_None)
        {
            return poJob->eResult;
        }
    }

    return CE_None;
}

CPLErr RMFDataset::WriteRawTile(int nBlockXOff, int nBlockYOff, GByte *pabyData,
                                size_t nTileBytes)
{
    CPLAssert(nBlockXOff >= 0 && nBlockYOff >= 0 && pabyData != nullptr &&
              nTileBytes > 0);

    const GUInt32 nTile = nBlockYOff * nXTiles + nBlockXOff;

    vsi_l_offset nTileOffset = GetFileOffset(paiTiles[2 * nTile]);
    size_t nTileSize = static_cast<size_t>(paiTiles[2 * nTile + 1]);

    if (nTileOffset && nTileSize <= nTileBytes)
    {
        if (VSIFSeekL(fp, nTileOffset, SEEK_SET) < 0)
        {
            CPLError(
                CE_Failure, CPLE_FileIO,
                "Can't seek to offset %ld in output file to write data.\n%s",
                static_cast<long>(nTileOffset), VSIStrerror(errno));
            return CE_Failure;
        }
    }
    else
    {
        if (VSIFSeekL(fp, 0, SEEK_END) < 0)
        {
            CPLError(
                CE_Failure, CPLE_FileIO,
                "Can't seek to offset %ld in output file to write data.\n%s",
                static_cast<long>(nTileOffset), VSIStrerror(errno));
            return CE_Failure;
        }
        nTileOffset = VSIFTellL(fp);
        vsi_l_offset nNewTileOffset = 0;
        paiTiles[2 * nTile] = GetRMFOffset(nTileOffset, &nNewTileOffset);

        if (nTileOffset != nNewTileOffset)
        {
            if (VSIFSeekL(fp, nNewTileOffset, SEEK_SET) < 0)
            {
                CPLError(CE_Failure, CPLE_FileIO,
                         "Can't seek to offset %ld in output file to "
                         "write data.\n%s",
                         static_cast<long>(nNewTileOffset), VSIStrerror(errno));
                return CE_Failure;
            }
        }
        bHeaderDirty = true;
    }

#ifdef CPL_MSB
    // Compressed tiles are already with proper byte order
    if (eRMFType == RMFT_MTW && sHeader.iCompression == RMF_COMPRESSION_NONE)
    {
        // Byte swap can be done in place
        if (sHeader.nBitDepth == 16)
        {
            for (size_t i = 0; i < nTileBytes; i += 2)
                CPL_SWAP16PTR(pabyData + i);
        }
        else if (sHeader.nBitDepth == 32)
        {
            for (size_t i = 0; i < nTileBytes; i += 4)
                CPL_SWAP32PTR(pabyData + i);
        }
        else if (sHeader.nBitDepth == 64)
        {
            for (size_t i = 0; i < nTileBytes; i += 8)
                CPL_SWAPDOUBLE(pabyData + i);
        }
    }
#endif

    bool bOk = (VSIFWriteL(pabyData, 1, nTileBytes, fp) == nTileBytes);

    if (!bOk)
    {
        CPLError(CE_Failure, CPLE_FileIO,
                 "Can't write tile with X offset %d and Y offset %d.\n%s",
                 nBlockXOff, nBlockYOff, VSIStrerror(errno));
        return CE_Failure;
    }

    paiTiles[2 * nTile + 1] = static_cast<GUInt32>(nTileBytes);
    bHeaderDirty = true;

    return CE_None;
}

CPLErr RMFDataset::ReadTile(int nBlockXOff, int nBlockYOff, GByte *pabyData,
                            size_t nRawBytes, GUInt32 nRawXSize,
                            GUInt32 nRawYSize, bool &bNullTile)
{
    bNullTile = false;

    const GUInt32 nTile = nBlockYOff * nXTiles + nBlockXOff;
    if (2 * nTile + 1 >= sHeader.nTileTblSize / sizeof(GUInt32))
    {
        return CE_Failure;
    }
    vsi_l_offset nTileOffset = GetFileOffset(paiTiles[2 * nTile]);
    GUInt32 nTileBytes = paiTiles[2 * nTile + 1];
    // RMF doesn't store compressed tiles with size greater than 80% of
    // uncompressed size. But just in case, select twice as many.
    GUInt32 nMaxTileBytes =
        2 * sHeader.nTileWidth * sHeader.nTileHeight * sHeader.nBitDepth / 8;

    if (nTileBytes >= nMaxTileBytes)
    {
        CPLError(CE_Failure, CPLE_AppDefined,
                 "Invalid tile size %lu at offset %ld. Must be less than %lu",
                 static_cast<unsigned long>(nTileBytes),
                 static_cast<long>(nTileOffset),
                 static_cast<unsigned long>(nMaxTileBytes));
        return CE_Failure;
    }

    if (nTileOffset == 0)
    {
        bNullTile = true;
        return CE_None;
    }

#ifdef DEBUG
    CPLDebug("RMF", "Read RawSize [%d, %d], nTileBytes %d, nRawBytes %d",
             nRawXSize, nRawYSize, static_cast<int>(nTileBytes),
             static_cast<int>(nRawBytes));
#endif  // DEBUG

    if (VSIFSeekL(fp, nTileOffset, SEEK_SET) < 0)
    {
        // XXX: We will not report error here, because file just may be
        // in update state and data for this block will be available later
        if (eAccess == GA_Update)
            return CE_None;

        CPLError(CE_Failure, CPLE_FileIO,
                 "Can't seek to offset %ld in input file to read data.\n%s",
                 static_cast<long>(nTileOffset), VSIStrerror(errno));
        return CE_Failure;
    }

    if (Decompress == nullptr || nTileBytes == nRawBytes)
    {
        if (nTileBytes != nRawBytes)
        {
            CPLError(CE_Failure, CPLE_AppDefined,
                     "RMF: Invalid tile size %lu, expected %lu",
                     static_cast<unsigned long>(nTileBytes),
                     static_cast<unsigned long>(nRawBytes));
            return CE_Failure;
        }

        if (VSIFReadL(pabyData, 1, nRawBytes, fp) < nRawBytes)
        {
            CPLError(CE_Failure, CPLE_FileIO,
                     "RMF: Can't read at offset %lu from input file.\n%s",
                     static_cast<unsigned long>(nTileOffset),
                     VSIStrerror(errno));
            return CE_Failure;
        }

#ifdef CPL_MSB
        if (eRMFType == RMFT_MTW)
        {
            if (sHeader.nBitDepth == 16)
            {
                for (GUInt32 i = 0; i < nRawBytes; i += 2)
                    CPL_SWAP16PTR(pabyData + i);
            }
            else if (sHeader.nBitDepth == 32)
            {
                for (GUInt32 i = 0; i < nRawBytes; i += 4)
                    CPL_SWAP32PTR(pabyData + i);
            }
            else if (sHeader.nBitDepth == 64)
            {
                for (GUInt32 i = 0; i < nRawBytes; i += 8)
                    CPL_SWAPDOUBLE(pabyData + i);
            }
        }
#endif
        return CE_None;
    }

    if (pabyDecompressBuffer == nullptr)
    {
        pabyDecompressBuffer =
            static_cast<GByte *>(VSIMalloc(std::max(1U, nMaxTileBytes)));
        if (!pabyDecompressBuffer)
        {
            CPLError(CE_Failure, CPLE_OutOfMemory,
                     "Can't allocate decompress buffer of size %lu.\n%s",
                     static_cast<unsigned long>(nMaxTileBytes),
                     VSIStrerror(errno));
            return CE_Failure;
        }
    }

    if (VSIFReadL(pabyDecompressBuffer, 1, nTileBytes, fp) < nTileBytes)
    {
        CPLError(CE_Failure, CPLE_FileIO,
                 "RMF: Can't read at offset %lu from input file.\n%s",
                 static_cast<unsigned long>(nTileOffset), VSIStrerror(errno));
        return CE_Failure;
    }

    size_t nDecompressedSize =
        Decompress(pabyDecompressBuffer, nTileBytes, pabyData,
                   static_cast<GUInt32>(nRawBytes), nRawXSize, nRawYSize);

    if (nDecompressedSize != static_cast<size_t>(nRawBytes))
    {
        CPLError(CE_Failure, CPLE_FileIO,
                 "Can't decompress tile xOff %d yOff %d. "
                 "Raw tile size is %lu but decompressed is %lu. "
                 "Compressed tile size is %lu",
                 nBlockXOff, nBlockYOff, static_cast<unsigned long>(nRawBytes),
                 static_cast<unsigned long>(nDecompressedSize),
                 static_cast<unsigned long>(nTileBytes));
        return CE_Failure;
    }
    // We don't need to swap bytes here,
    // because decompressed data is in proper byte order
    return CE_None;
}

void RMFDataset::SetupNBits()
{
    int nBitDepth = 0;
    if (sHeader.nBitDepth < 8 && nBands == 1)
    {
        nBitDepth = static_cast<int>(sHeader.nBitDepth);
    }
    else if (sHeader.nBitDepth == 16 && nBands == 3 && eRMFType == RMFT_RSW)
    {
        nBitDepth = 5;
    }

    if (nBitDepth > 0)
    {
        char szNBits[32] = {};
        snprintf(szNBits, sizeof(szNBits), "%d", nBitDepth);
        for (int iBand = 1; iBand <= nBands; iBand++)
        {
            GetRasterBand(iBand)->SetMetadataItem("NBITS", szNBits,
                                                  "IMAGE_STRUCTURE");
        }
    }
}

/************************************************************************/
/*                        GDALRegister_RMF()                            */
/************************************************************************/

void GDALRegister_RMF()

{
    if (GDALGetDriverByName("RMF") != nullptr)
        return;

    GDALDriver *poDriver = new GDALDriver();

    poDriver->SetDescription("RMF");
    poDriver->SetMetadataItem(GDAL_DCAP_RASTER, "YES");
    poDriver->SetMetadataItem(GDAL_DMD_LONGNAME, "Raster Matrix Format");
    poDriver->SetMetadataItem(GDAL_DMD_HELPTOPIC, "drivers/raster/rmf.html");
    poDriver->SetMetadataItem(GDAL_DMD_EXTENSION, "rsw");
    poDriver->SetMetadataItem(GDAL_DMD_CREATIONDATATYPES,
                              "Byte Int16 Int32 Float64");
    poDriver->SetMetadataItem(
        GDAL_DMD_CREATIONOPTIONLIST,
        "<CreationOptionList>"
        "   <Option name='MTW' type='boolean' description='Create MTW DEM "
        "matrix'/>"
        "   <Option name='BLOCKXSIZE' type='int' description='Tile Width'/>"
        "   <Option name='BLOCKYSIZE' type='int' description='Tile Height'/>"
        "   <Option name='RMFHUGE' type='string-select' description='Creation "
        "of huge RMF file (Supported by GIS Panorama since v11)'>"
        "     <Value>NO</Value>"
        "     <Value>YES</Value>"
        "     <Value>IF_SAFER</Value>"
        "   </Option>"
        "   <Option name='COMPRESS' type='string-select' default='NONE'>"
        "     <Value>NONE</Value>"
        "     <Value>LZW</Value>"
        "     <Value>JPEG</Value>"
        "     <Value>RMF_DEM</Value>"
        "   </Option>"
        "   <Option name='JPEG_QUALITY' type='int' description='JPEG quality "
        "1-100' default='75'/>"
        "   <Option name='NUM_THREADS' type='string' description='Number of "
        "worker threads for compression. Can be set to ALL_CPUS' default='1'/>"
        "</CreationOptionList>");
    poDriver->SetMetadataItem(GDAL_DCAP_VIRTUALIO, "YES");

    poDriver->pfnIdentify = RMFDataset::Identify;
    poDriver->pfnOpen = RMFDataset::Open;
    poDriver->pfnCreate = RMFDataset::Create;
    poDriver->SetMetadataItem(
        GDAL_DMD_OPENOPTIONLIST,
        "<OpenOptionList>"
        "  <Option name='RMF_SET_VERTCS' type='string' description='Layers "
        "spatial reference will include vertical coordinate system description "
        "if exist' default='NO'/>"
        "</OpenOptionList>");

    GetGDALDriverManager()->RegisterDriver(poDriver);
}

/************************************************************************/
/*                            RMFCompressData                           */
/************************************************************************/

RMFCompressData::RMFCompressData() : pabyBuffers(nullptr)
{
}

RMFCompressData::~RMFCompressData()
{
    if (pabyBuffers != nullptr)
    {
        VSIFree(pabyBuffers);
    }

    if (hWriteTileMutex != nullptr)
    {
        CPLDestroyMutex(hWriteTileMutex);
    }

    if (hReadyJobMutex != nullptr)
    {
        CPLDestroyMutex(hReadyJobMutex);
    }
}

GDALSuggestedBlockAccessPattern
RMFRasterBand::GetSuggestedBlockAccessPattern() const
{
    return GSBAP_RANDOM;
}

CPLErr RMFDataset::SetMetadataItem(const char *pszName, const char *pszValue,
                                   const char *pszDomain)
{
    if (GetAccess() == GA_Update)
    {
        CPLDebug("RMF", "SetMetadataItem: %s=%s", pszName, pszValue);
        if (EQUAL(pszName, MD_NAME_KEY))
        {
            memcpy(sHeader.byName, pszValue,
                   CPLStrnlen(pszValue, RMF_NAME_SIZE));
            bHeaderDirty = true;
        }
        else if (EQUAL(pszName, MD_SCALE_KEY) && CPLStrnlen(pszValue, 10) > 4)
        {
            sHeader.dfScale = atof(pszValue + 4);
            sHeader.dfResolution = sHeader.dfScale / sHeader.dfPixelSize;
            bHeaderDirty = true;
        }
        else if (EQUAL(pszName, MD_FRAME_KEY))
        {
            bHeaderDirty = true;
        }
    }
    return GDALDataset::SetMetadataItem(pszName, pszValue, pszDomain);
}

CPLErr RMFDataset::SetMetadata(char **papszMetadata, const char *pszDomain)
{
    if (GetAccess() == GA_Update)
    {
        auto pszName = CSLFetchNameValue(papszMetadata, MD_NAME_KEY);
        if (pszName != nullptr)
        {
            memcpy(sHeader.byName, pszName, CPLStrnlen(pszName, RMF_NAME_SIZE));
            bHeaderDirty = true;

            CPLDebug("RMF", "SetMetadata: %s", pszName);
        }
        auto pszScale = CSLFetchNameValue(papszMetadata, MD_SCALE_KEY);
        if (pszScale != nullptr && CPLStrnlen(pszScale, 10) > 4)
        {
            sHeader.dfScale = atof(pszScale + 4);
            sHeader.dfResolution = sHeader.dfScale / sHeader.dfPixelSize;
            bHeaderDirty = true;

            CPLDebug("RMF", "SetMetadata: %s", pszScale);
        }
        auto pszFrame = CSLFetchNameValue(papszMetadata, MD_FRAME_KEY);
        if (pszFrame != nullptr)
        {
            bHeaderDirty = true;

            CPLDebug("RMF", "SetMetadata: %s", pszFrame);
        }
    }
    return GDALDataset::SetMetadata(papszMetadata, pszDomain);
}
