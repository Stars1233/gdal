/******************************************************************************
 *
 * Project:  BIGGIF Driver
 * Purpose:  Implement GDAL support for reading large GIF files in a
 *           streaming fashion rather than the slurp-into-memory approach
 *           of the normal GIF driver.
 * Author:   Frank Warmerdam, warmerdam@pobox.com
 *
 ******************************************************************************
 * Copyright (c) 2001-2008, Frank Warmerdam <warmerdam@pobox.com>
 * Copyright (c) 2009-2013, Even Rouault <even dot rouault at spatialys.com>
 *
 * SPDX-License-Identifier: MIT
 ****************************************************************************/

#include "cpl_string.h"
#include "gdal_frmts.h"
#include "gdal_pam.h"
#include "gifabstractdataset.h"
#include "gifdrivercore.h"

/************************************************************************/
/* ==================================================================== */
/*                          BIGGIFDataset                               */
/* ==================================================================== */
/************************************************************************/

class BIGGifRasterBand;

class BIGGIFDataset final : public GIFAbstractDataset
{
    friend class BIGGifRasterBand;

    int nLastLineRead;

    GDALDataset *poWorkDS;

    CPLErr ReOpen();

  protected:
    int CloseDependentDatasets() override;

  public:
    BIGGIFDataset();
    ~BIGGIFDataset() override;

    static GDALDataset *Open(GDALOpenInfo *);
};

/************************************************************************/
/* ==================================================================== */
/*                            BIGGifRasterBand                          */
/* ==================================================================== */
/************************************************************************/

class BIGGifRasterBand final : public GIFAbstractRasterBand
{
    friend class BIGGIFDataset;

  public:
    BIGGifRasterBand(BIGGIFDataset *, int);

    CPLErr IReadBlock(int, int, void *) override;
};

/************************************************************************/
/*                          BIGGifRasterBand()                          */
/************************************************************************/

BIGGifRasterBand::BIGGifRasterBand(BIGGIFDataset *poDSIn, int nBackground)
    : GIFAbstractRasterBand(poDSIn, 1, poDSIn->hGifFile->SavedImages,
                            nBackground, TRUE)

{
}

/************************************************************************/
/*                             IReadBlock()                             */
/************************************************************************/

CPLErr BIGGifRasterBand::IReadBlock(CPL_UNUSED int nBlockXOff, int nBlockYOff,
                                    void *pImage)
{
    BIGGIFDataset *poGDS = cpl::down_cast<BIGGIFDataset *>(poDS);

    CPLAssert(nBlockXOff == 0);

    if (panInterlaceMap != nullptr)
        nBlockYOff = panInterlaceMap[nBlockYOff];

    /* -------------------------------------------------------------------- */
    /*      Do we already have this line in the work dataset?               */
    /* -------------------------------------------------------------------- */
    if (poGDS->poWorkDS != nullptr && nBlockYOff <= poGDS->nLastLineRead)
    {
        return poGDS->poWorkDS->RasterIO(GF_Read, 0, nBlockYOff, nBlockXSize, 1,
                                         pImage, nBlockXSize, 1, GDT_Byte, 1,
                                         nullptr, 0, 0, 0, nullptr);
    }

    /* -------------------------------------------------------------------- */
    /*      Do we need to restart from the start of the image?              */
    /* -------------------------------------------------------------------- */
    if (nBlockYOff <= poGDS->nLastLineRead)
    {
        if (poGDS->ReOpen() == CE_Failure)
            return CE_Failure;
    }

    /* -------------------------------------------------------------------- */
    /*      Read till we get our target line.                               */
    /* -------------------------------------------------------------------- */
    CPLErr eErr = CE_None;
    while (poGDS->nLastLineRead < nBlockYOff && eErr == CE_None)
    {
        if (DGifGetLine(poGDS->hGifFile, (GifPixelType *)pImage, nBlockXSize) ==
            GIF_ERROR)
        {
            CPLError(CE_Failure, CPLE_AppDefined,
                     "Failure decoding scanline of GIF file.");
            return CE_Failure;
        }

        poGDS->nLastLineRead++;

        if (poGDS->poWorkDS != nullptr)
        {
            eErr = poGDS->poWorkDS->RasterIO(
                GF_Write, 0, poGDS->nLastLineRead, nBlockXSize, 1, pImage,
                nBlockXSize, 1, GDT_Byte, 1, nullptr, 0, 0, 0, nullptr);
        }
    }

    return eErr;
}

/************************************************************************/
/* ==================================================================== */
/*                             BIGGIFDataset                            */
/* ==================================================================== */
/************************************************************************/

/************************************************************************/
/*                            BIGGIFDataset()                            */
/************************************************************************/

BIGGIFDataset::BIGGIFDataset() : nLastLineRead(-1), poWorkDS(nullptr)
{
}

/************************************************************************/
/*                           ~BIGGIFDataset()                            */
/************************************************************************/

BIGGIFDataset::~BIGGIFDataset()

{
    BIGGIFDataset::FlushCache(true);

    BIGGIFDataset::CloseDependentDatasets();
}

/************************************************************************/
/*                      CloseDependentDatasets()                        */
/************************************************************************/

int BIGGIFDataset::CloseDependentDatasets()
{
    int bHasDroppedRef = GDALPamDataset::CloseDependentDatasets();

    if (poWorkDS != nullptr)
    {
        bHasDroppedRef = TRUE;

        CPLString osTempFilename = poWorkDS->GetDescription();
        GDALDriver *poDrv = poWorkDS->GetDriver();

        GDALClose((GDALDatasetH)poWorkDS);
        poWorkDS = nullptr;

        if (poDrv != nullptr)
            poDrv->Delete(osTempFilename);

        poWorkDS = nullptr;
    }

    return bHasDroppedRef;
}

/************************************************************************/
/*                               ReOpen()                               */
/*                                                                      */
/*      (Re)Open the gif file and process past the first image          */
/*      descriptor.                                                     */
/************************************************************************/

CPLErr BIGGIFDataset::ReOpen()

{
    /* -------------------------------------------------------------------- */
    /*      If the file is already open, close it so we can restart.        */
    /* -------------------------------------------------------------------- */
    if (hGifFile != nullptr)
        GIFAbstractDataset::myDGifCloseFile(hGifFile);

    /* -------------------------------------------------------------------- */
    /*      If we are actually reopening, then we assume that access to     */
    /*      the image data is not strictly once through sequential, and     */
    /*      we will try to create a working database in a temporary         */
    /*      directory to hold the image as we read through it the second    */
    /*      time.                                                           */
    /* -------------------------------------------------------------------- */
    if (hGifFile != nullptr)
    {
        GDALDriver *poGTiffDriver = (GDALDriver *)GDALGetDriverByName("GTiff");

        if (poGTiffDriver != nullptr)
        {
            /* Create as a sparse file to avoid filling up the whole file */
            /* while closing and then destroying this temporary dataset */
            const char *apszOptions[] = {"COMPRESS=LZW", "SPARSE_OK=YES",
                                         nullptr};
            CPLString osTempFilename = CPLGenerateTempFilenameSafe("biggif");

            osTempFilename += ".tif";

            poWorkDS = poGTiffDriver->Create(osTempFilename, nRasterXSize,
                                             nRasterYSize, 1, GDT_Byte,
                                             const_cast<char **>(apszOptions));
        }
    }

    /* -------------------------------------------------------------------- */
    /*      Open                                                            */
    /* -------------------------------------------------------------------- */
    VSIFSeekL(fp, 0, SEEK_SET);

    nLastLineRead = -1;
    hGifFile = GIFAbstractDataset::myDGifOpen(fp, GIFAbstractDataset::ReadFunc);
    if (hGifFile == nullptr)
    {
        CPLError(CE_Failure, CPLE_OpenFailed,
                 "DGifOpen() failed.  Perhaps the gif file is corrupt?\n");

        return CE_Failure;
    }

    /* -------------------------------------------------------------------- */
    /*      Find the first image record.                                    */
    /* -------------------------------------------------------------------- */
    GifRecordType RecordType = FindFirstImage(hGifFile);
    if (RecordType != IMAGE_DESC_RECORD_TYPE)
    {
        GIFAbstractDataset::myDGifCloseFile(hGifFile);
        hGifFile = nullptr;

        CPLError(CE_Failure, CPLE_OpenFailed,
                 "Failed to find image description record in GIF file.");
        return CE_Failure;
    }

    if (DGifGetImageDesc(hGifFile) == GIF_ERROR)
    {
        GIFAbstractDataset::myDGifCloseFile(hGifFile);
        hGifFile = nullptr;

        CPLError(CE_Failure, CPLE_OpenFailed,
                 "Image description reading failed in GIF file.");
        return CE_Failure;
    }

    return CE_None;
}

/************************************************************************/
/*                                Open()                                */
/************************************************************************/

GDALDataset *BIGGIFDataset::Open(GDALOpenInfo *poOpenInfo)

{
    if (!GIFDriverIdentify(poOpenInfo))
        return nullptr;

    if (poOpenInfo->eAccess == GA_Update)
    {
        ReportUpdateNotSupportedByDriver("GIF");
        return nullptr;
    }

    /* -------------------------------------------------------------------- */
    /*      Create a corresponding GDALDataset.                             */
    /* -------------------------------------------------------------------- */
    BIGGIFDataset *poDS = new BIGGIFDataset();

    poDS->fp = poOpenInfo->fpL;
    poOpenInfo->fpL = nullptr;
    poDS->eAccess = GA_ReadOnly;
    if (poDS->ReOpen() == CE_Failure)
    {
        delete poDS;
        return nullptr;
    }

    /* -------------------------------------------------------------------- */
    /*      Capture some information from the file that is of interest.     */
    /* -------------------------------------------------------------------- */

    poDS->nRasterXSize = poDS->hGifFile->SavedImages[0].ImageDesc.Width;
    poDS->nRasterYSize = poDS->hGifFile->SavedImages[0].ImageDesc.Height;
    if (!GDALCheckDatasetDimensions(poDS->nRasterXSize, poDS->nRasterYSize))
    {
        delete poDS;
        return nullptr;
    }

    if (poDS->hGifFile->SavedImages[0].ImageDesc.ColorMap == nullptr &&
        poDS->hGifFile->SColorMap == nullptr)
    {
        CPLDebug("GIF", "Skipping image without color table");
        delete poDS;
        return nullptr;
    }

    /* -------------------------------------------------------------------- */
    /*      Create band information objects.                                */
    /* -------------------------------------------------------------------- */
    poDS->SetBand(1,
                  new BIGGifRasterBand(poDS, poDS->hGifFile->SBackGroundColor));

    /* -------------------------------------------------------------------- */
    /*      Check for georeferencing.                                       */
    /* -------------------------------------------------------------------- */
    poDS->DetectGeoreferencing(poOpenInfo);

    /* -------------------------------------------------------------------- */
    /*      Initialize any PAM information.                                 */
    /* -------------------------------------------------------------------- */
    poDS->SetDescription(poOpenInfo->pszFilename);
    poDS->TryLoadXML(poOpenInfo->GetSiblingFiles());

    /* -------------------------------------------------------------------- */
    /*      Support overviews.                                              */
    /* -------------------------------------------------------------------- */
    poDS->oOvManager.Initialize(poDS, poOpenInfo->pszFilename,
                                poOpenInfo->GetSiblingFiles());

    return poDS;
}

/************************************************************************/
/*                        GDALRegister_BIGGIF()                         */
/************************************************************************/

void GDALRegister_BIGGIF()

{
    if (GDALGetDriverByName(BIGGIF_DRIVER_NAME) != nullptr)
        return;

    GDALDriver *poDriver = new GDALDriver();
    BIGGIFDriverSetCommonMetadata(poDriver);

    poDriver->pfnOpen = BIGGIFDataset::Open;

    GetGDALDriverManager()->RegisterDriver(poDriver);
}
