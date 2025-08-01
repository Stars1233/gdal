/******************************************************************************
 *
 * Project:  Polarimetric Workstation
 * Purpose:  Radarsat 2 - XML Products (product.xml) driver
 * Author:   Frank Warmerdam, warmerdam@pobox.com
 *
 ******************************************************************************
 * Copyright (c) 2004, Frank Warmerdam <warmerdam@pobox.com>
 * Copyright (c) 2009-2013, Even Rouault <even dot rouault at spatialys.com>
 * Copyright (c) 2020, Defence Research and Development Canada (DRDC) Ottawa Research Centre
 *
 * SPDX-License-Identifier: MIT
 ****************************************************************************/

#include "cpl_minixml.h"
#include "gdal_frmts.h"
#include "gdal_pam.h"
#include "ogr_spatialref.h"

typedef enum eCalibration_t
{
    Sigma0 = 0,
    Gamma,
    Beta0,
    Uncalib,
    None
} eCalibration;

/*** Function to test for valid LUT files ***/
static bool IsValidXMLFile(const char *pszPath, const char *pszLut)
{
    /* Return true for valid xml file, false otherwise */
    char *pszLutFile =
        VSIStrdup(CPLFormFilenameSafe(pszPath, pszLut, nullptr).c_str());

    CPLXMLTreeCloser psLut(CPLParseXMLFile(pszLutFile));

    CPLFree(pszLutFile);

    return psLut.get() != nullptr;
}

// Check that the referenced dataset for each band has the
// correct data type and returns whether a 2 band I+Q dataset should
// be mapped onto a single complex band.
// Returns BANDERROR for error, STRAIGHT for 1:1 mapping, TWOBANDCOMPLEX for 2
// bands -> 1 complex band
typedef enum
{
    BANDERROR,
    STRAIGHT,
    TWOBANDCOMPLEX
} BandMapping;

static BandMapping GetBandFileMapping(GDALDataType eDataType,
                                      GDALDataset *poBandFile)
{

    GDALRasterBand *poBand1 = poBandFile->GetRasterBand(1);
    GDALDataType eBandDataType1 = poBand1->GetRasterDataType();

    // if there is one band and it has the same datatype, the band file gets
    // passed straight through
    if (poBandFile->GetRasterCount() == 1 && eDataType == eBandDataType1)
        return STRAIGHT;

    // if the band file has 2 bands, they should represent I+Q
    // and be a compatible data type
    if (poBandFile->GetRasterCount() == 2 && GDALDataTypeIsComplex(eDataType))
    {
        GDALRasterBand *band2 = poBandFile->GetRasterBand(2);

        if (eBandDataType1 != band2->GetRasterDataType())
            return BANDERROR;  // both bands must be same datatype

        // check compatible types - there are 4 complex types in GDAL
        if ((eDataType == GDT_CInt16 && eBandDataType1 == GDT_Int16) ||
            (eDataType == GDT_CInt32 && eBandDataType1 == GDT_Int32) ||
            (eDataType == GDT_CFloat32 && eBandDataType1 == GDT_Float32) ||
            (eDataType == GDT_CFloat64 && eBandDataType1 == GDT_Float64))
            return TWOBANDCOMPLEX;
    }
    return BANDERROR;  // don't accept any other combinations
}

/************************************************************************/
/* ==================================================================== */
/*                               RS2Dataset                             */
/* ==================================================================== */
/************************************************************************/

class RS2Dataset final : public GDALPamDataset
{
    CPLXMLNode *psProduct;

    int nGCPCount;
    GDAL_GCP *pasGCPList;
    OGRSpatialReference m_oSRS{};
    OGRSpatialReference m_oGCPSRS{};
    char **papszSubDatasets;
    GDALGeoTransform m_gt{};
    bool bHaveGeoTransform;

    char **papszExtraFiles;

    CPL_DISALLOW_COPY_ASSIGN(RS2Dataset)

  protected:
    virtual int CloseDependentDatasets() override;

  public:
    RS2Dataset();
    virtual ~RS2Dataset();

    virtual int GetGCPCount() override;
    const OGRSpatialReference *GetGCPSpatialRef() const override;
    virtual const GDAL_GCP *GetGCPs() override;

    const OGRSpatialReference *GetSpatialRef() const override;
    virtual CPLErr GetGeoTransform(GDALGeoTransform &gt) const override;

    virtual char **GetMetadataDomainList() override;
    virtual char **GetMetadata(const char *pszDomain = "") override;
    virtual char **GetFileList(void) override;

    static GDALDataset *Open(GDALOpenInfo *);
    static int Identify(GDALOpenInfo *);

    CPLXMLNode *GetProduct()
    {
        return psProduct;
    }
};

/************************************************************************/
/* ==================================================================== */
/*                    RS2RasterBand                           */
/* ==================================================================== */
/************************************************************************/

class RS2RasterBand final : public GDALPamRasterBand
{
    GDALDataset *poBandFile = nullptr;

    // 2 bands representing I+Q -> one complex band
    // otherwise poBandFile is passed straight through
    bool bIsTwoBandComplex = false;

    CPL_DISALLOW_COPY_ASSIGN(RS2RasterBand)

  public:
    RS2RasterBand(RS2Dataset *poDSIn, GDALDataType eDataTypeIn,
                  const char *pszPole, GDALDataset *poBandFile,
                  bool bTwoBandComplex = false);
    virtual ~RS2RasterBand();

    virtual CPLErr IReadBlock(int, int, void *) override;

    static GDALDataset *Open(GDALOpenInfo *);
};

/************************************************************************/
/*                            RS2RasterBand                             */
/************************************************************************/

RS2RasterBand::RS2RasterBand(RS2Dataset *poDSIn, GDALDataType eDataTypeIn,
                             const char *pszPole, GDALDataset *poBandFileIn,
                             bool bTwoBandComplex)
    : poBandFile(poBandFileIn)
{
    poDS = poDSIn;

    GDALRasterBand *poSrcBand = poBandFile->GetRasterBand(1);

    poSrcBand->GetBlockSize(&nBlockXSize, &nBlockYSize);

    eDataType = eDataTypeIn;

    bIsTwoBandComplex = bTwoBandComplex;

    if (*pszPole != '\0')
        SetMetadataItem("POLARIMETRIC_INTERP", pszPole);
}

/************************************************************************/
/*                            RSRasterBand()                            */
/************************************************************************/

RS2RasterBand::~RS2RasterBand()

{
    if (poBandFile != nullptr)
        GDALClose(reinterpret_cast<GDALRasterBandH>(poBandFile));
}

/************************************************************************/
/*                             IReadBlock()                             */
/************************************************************************/

CPLErr RS2RasterBand::IReadBlock(int nBlockXOff, int nBlockYOff, void *pImage)

{
    /* -------------------------------------------------------------------- */
    /*      If the last strip is partial, we need to avoid                  */
    /*      over-requesting.  We also need to initialize the extra part     */
    /*      of the block to zero.                                           */
    /* -------------------------------------------------------------------- */
    int nRequestYSize;
    if ((nBlockYOff + 1) * nBlockYSize > nRasterYSize)
    {
        nRequestYSize = nRasterYSize - nBlockYOff * nBlockYSize;
        memset(pImage, 0,
               static_cast<size_t>(GDALGetDataTypeSizeBytes(eDataType)) *
                   nBlockXSize * nBlockYSize);
    }
    else
    {
        nRequestYSize = nBlockYSize;
    }

    /*-------------------------------------------------------------------- */
    /*      If the input imagery is tiled, also need to avoid over-        */
    /*      requesting in the X-direction.                                 */
    /* ------------------------------------------------------------------- */
    int nRequestXSize;
    if ((nBlockXOff + 1) * nBlockXSize > nRasterXSize)
    {
        nRequestXSize = nRasterXSize - nBlockXOff * nBlockXSize;
        memset(pImage, 0,
               static_cast<size_t>(GDALGetDataTypeSizeBytes(eDataType)) *
                   nBlockXSize * nBlockYSize);
    }
    else
    {
        nRequestXSize = nBlockXSize;
    }
    if (eDataType == GDT_CInt16 && poBandFile->GetRasterCount() == 2)
        return poBandFile->RasterIO(
            GF_Read, nBlockXOff * nBlockXSize, nBlockYOff * nBlockYSize,
            nRequestXSize, nRequestYSize, pImage, nRequestXSize, nRequestYSize,
            GDT_Int16, 2, nullptr, 4, nBlockXSize * 4, 2, nullptr);

    /* -------------------------------------------------------------------- */
    /*      File has one sample marked as sample format void, a 32bits.     */
    /* -------------------------------------------------------------------- */
    else if (eDataType == GDT_CInt16 && poBandFile->GetRasterCount() == 1)
    {
        CPLErr eErr = poBandFile->RasterIO(
            GF_Read, nBlockXOff * nBlockXSize, nBlockYOff * nBlockYSize,
            nRequestXSize, nRequestYSize, pImage, nRequestXSize, nRequestYSize,
            GDT_UInt32, 1, nullptr, 4, nBlockXSize * 4, 0, nullptr);

#ifdef CPL_LSB
        /* First, undo the 32bit swap. */
        GDALSwapWords(pImage, 4, nBlockXSize * nBlockYSize, 4);

        /* Then apply 16 bit swap. */
        GDALSwapWords(pImage, 2, nBlockXSize * nBlockYSize * 2, 2);
#endif

        return eErr;
    }

    /* -------------------------------------------------------------------- */
    /*      The 16bit case is straight forward.  The underlying file        */
    /*      looks like a 16bit unsigned data too.                           */
    /* -------------------------------------------------------------------- */
    else if (eDataType == GDT_UInt16)
        return poBandFile->RasterIO(
            GF_Read, nBlockXOff * nBlockXSize, nBlockYOff * nBlockYSize,
            nRequestXSize, nRequestYSize, pImage, nRequestXSize, nRequestYSize,
            GDT_UInt16, 1, nullptr, 2, nBlockXSize * 2, 0, nullptr);
    else if (eDataType == GDT_Byte)
        /* Ticket #2104: Support for ScanSAR products */
        return poBandFile->RasterIO(
            GF_Read, nBlockXOff * nBlockXSize, nBlockYOff * nBlockYSize,
            nRequestXSize, nRequestYSize, pImage, nRequestXSize, nRequestYSize,
            GDT_Byte, 1, nullptr, 1, nBlockXSize, 0, nullptr);

    CPLAssert(false);
    return CE_Failure;
}

/************************************************************************/
/* ==================================================================== */
/*                         RS2CalibRasterBand                           */
/* ==================================================================== */
/************************************************************************/
/* Returns data that has been calibrated to sigma nought, gamma         */
/* or beta nought.                                                      */
/************************************************************************/

class RS2CalibRasterBand final : public GDALPamRasterBand
{
  private:
    // eCalibration m_eCalib;
    GDALDataset *m_poBandDataset;
    GDALDataType m_eType; /* data type of data being ingested */
    float *m_nfTable;
    int m_nTableSize;
    float m_nfOffset;
    char *m_pszLUTFile;

    CPL_DISALLOW_COPY_ASSIGN(RS2CalibRasterBand)

    void ReadLUT();

  public:
    RS2CalibRasterBand(RS2Dataset *poDataset, const char *pszPolarization,
                       GDALDataType eType, GDALDataset *poBandDataset,
                       eCalibration eCalib, const char *pszLUT);
    ~RS2CalibRasterBand();

    CPLErr IReadBlock(int nBlockXOff, int nBlockYOff, void *pImage) override;
};

/************************************************************************/
/*                            ReadLUT()                                 */
/************************************************************************/
/* Read the provided LUT in to m_ndTable                                */
/************************************************************************/
void RS2CalibRasterBand::ReadLUT()
{
    CPLXMLNode *psLUT = CPLParseXMLFile(m_pszLUTFile);

    this->m_nfOffset = static_cast<float>(
        CPLAtof(CPLGetXMLValue(psLUT, "=lut.offset", "0.0")));

    char **papszLUTList = CSLTokenizeString2(
        CPLGetXMLValue(psLUT, "=lut.gains", ""), " ", CSLT_HONOURSTRINGS);

    m_nTableSize = CSLCount(papszLUTList);

    m_nfTable = static_cast<float *>(CPLMalloc(sizeof(float) * m_nTableSize));

    for (int i = 0; i < m_nTableSize; i++)
    {
        m_nfTable[i] = static_cast<float>(CPLAtof(papszLUTList[i]));
    }

    CPLDestroyXMLNode(psLUT);

    CSLDestroy(papszLUTList);
}

/************************************************************************/
/*                        RS2CalibRasterBand()                          */
/************************************************************************/

RS2CalibRasterBand::RS2CalibRasterBand(
    RS2Dataset *poDataset, const char *pszPolarization, GDALDataType eType,
    GDALDataset *poBandDataset, eCalibration /* eCalib */, const char *pszLUT)
    :  // m_eCalib(eCalib),
      m_poBandDataset(poBandDataset), m_eType(eType), m_nfTable(nullptr),
      m_nTableSize(0), m_nfOffset(0), m_pszLUTFile(VSIStrdup(pszLUT))
{
    poDS = poDataset;

    if (*pszPolarization != '\0')
    {
        SetMetadataItem("POLARIMETRIC_INTERP", pszPolarization);
    }

    if (eType == GDT_CInt16)
        eDataType = GDT_CFloat32;
    else
        eDataType = GDT_Float32;

    GDALRasterBand *poRasterBand = poBandDataset->GetRasterBand(1);
    poRasterBand->GetBlockSize(&nBlockXSize, &nBlockYSize);

    ReadLUT();
}

/************************************************************************/
/*                       ~RS2CalibRasterBand()                          */
/************************************************************************/

RS2CalibRasterBand::~RS2CalibRasterBand()
{
    CPLFree(m_nfTable);
    CPLFree(m_pszLUTFile);
    GDALClose(m_poBandDataset);
}

/************************************************************************/
/*                        IReadBlock()                                  */
/************************************************************************/

CPLErr RS2CalibRasterBand::IReadBlock(int nBlockXOff, int nBlockYOff,
                                      void *pImage)
{
    /* -------------------------------------------------------------------- */
    /*      If the last strip is partial, we need to avoid                  */
    /*      over-requesting.  We also need to initialize the extra part     */
    /*      of the block to zero.                                           */
    /* -------------------------------------------------------------------- */
    int nRequestYSize;
    if ((nBlockYOff + 1) * nBlockYSize > nRasterYSize)
    {
        nRequestYSize = nRasterYSize - nBlockYOff * nBlockYSize;
        memset(pImage, 0,
               static_cast<size_t>(GDALGetDataTypeSizeBytes(eDataType)) *
                   nBlockXSize * nBlockYSize);
    }
    else
    {
        nRequestYSize = nBlockYSize;
    }

    CPLErr eErr;
    if (m_eType == GDT_CInt16)
    {
        /* read in complex values */
        GInt16 *pnImageTmp = reinterpret_cast<GInt16 *>(
            CPLMalloc(2 * sizeof(int16_t) * nBlockXSize * nBlockYSize));
        if (m_poBandDataset->GetRasterCount() == 2)
        {
            eErr = m_poBandDataset->RasterIO(
                GF_Read, nBlockXOff * nBlockXSize, nBlockYOff * nBlockYSize,
                nBlockXSize, nRequestYSize, pnImageTmp, nBlockXSize,
                nRequestYSize, GDT_Int16, 2, nullptr, 4, nBlockXSize * 4, 2,
                nullptr);
        }
        else
        {
            eErr = m_poBandDataset->RasterIO(
                GF_Read, nBlockXOff * nBlockXSize, nBlockYOff * nBlockYSize,
                nBlockXSize, nRequestYSize, pnImageTmp, nBlockXSize,
                nRequestYSize, GDT_UInt32, 1, nullptr, 4, nBlockXSize * 4, 0,
                nullptr);

#ifdef CPL_LSB
            /* First, undo the 32bit swap. */
            GDALSwapWords(pImage, 4, nBlockXSize * nBlockYSize, 4);

            /* Then apply 16 bit swap. */
            GDALSwapWords(pImage, 2, nBlockXSize * nBlockYSize * 2, 2);
#endif
        }

        /* calibrate the complex values */
        for (int i = 0; i < nBlockYSize; i++)
        {
            for (int j = 0; j < nBlockXSize; j++)
            {
                /* calculate pixel offset in memory*/
                int nPixOff = (2 * (i * nBlockXSize)) + (j * 2);

                reinterpret_cast<float *>(pImage)[nPixOff] =
                    static_cast<float>(pnImageTmp[nPixOff]) /
                    (m_nfTable[nBlockXOff + j]);
                reinterpret_cast<float *>(pImage)[nPixOff + 1] =
                    static_cast<float>(pnImageTmp[nPixOff + 1]) /
                    (m_nfTable[nBlockXOff + j]);
            }
        }
        CPLFree(pnImageTmp);
    }
    // If the underlying file is NITF CFloat32
    else if (eDataType == GDT_CFloat32 &&
             m_poBandDataset->GetRasterCount() == 1)
    {
        eErr = m_poBandDataset->RasterIO(
            GF_Read, nBlockXOff * nBlockXSize, nBlockYOff * nBlockYSize,
            nBlockXSize, nRequestYSize, pImage, nBlockXSize, nRequestYSize,
            GDT_CFloat32, 1, nullptr, 2 * static_cast<int>(sizeof(float)),
            nBlockXSize * 2 * static_cast<GSpacing>(sizeof(float)), 0, nullptr);

        /* calibrate the complex values */
        for (int i = 0; i < nBlockYSize; i++)
        {
            for (int j = 0; j < nBlockXSize; j++)
            {
                /* calculate pixel offset in memory*/
                const int nPixOff = 2 * (i * nBlockXSize + j);

                reinterpret_cast<float *>(pImage)[nPixOff] /=
                    m_nfTable[nBlockXOff * nBlockXSize + j];
                reinterpret_cast<float *>(pImage)[nPixOff + 1] /=
                    m_nfTable[nBlockXOff * nBlockXSize + j];
            }
        }
    }
    else if (m_eType == GDT_UInt16)
    {
        /* read in detected values */
        GUInt16 *pnImageTmp = reinterpret_cast<GUInt16 *>(
            CPLMalloc(sizeof(uint16_t) * nBlockXSize * nBlockYSize));
        eErr = m_poBandDataset->RasterIO(
            GF_Read, nBlockXOff * nBlockXSize, nBlockYOff * nBlockYSize,
            nBlockXSize, nRequestYSize, pnImageTmp, nBlockXSize, nRequestYSize,
            GDT_UInt16, 1, nullptr, 2, nBlockXSize * 2, 0, nullptr);

        /* iterate over detected values */
        for (int i = 0; i < nBlockYSize; i++)
        {
            for (int j = 0; j < nBlockXSize; j++)
            {
                int nPixOff = (i * nBlockXSize) + j;

                reinterpret_cast<float *>(pImage)[nPixOff] =
                    ((static_cast<float>(pnImageTmp[nPixOff]) *
                      static_cast<float>(pnImageTmp[nPixOff])) +
                     m_nfOffset) /
                    m_nfTable[nBlockXOff + j];
            }
        }
        CPLFree(pnImageTmp);
    } /* Ticket #2104: Support for ScanSAR products */
    else if (m_eType == GDT_Byte)
    {
        GByte *pnImageTmp = reinterpret_cast<GByte *>(
            CPLMalloc(static_cast<size_t>(nBlockXSize) * nBlockYSize));
        eErr = m_poBandDataset->RasterIO(
            GF_Read, nBlockXOff * nBlockXSize, nBlockYOff * nBlockYSize,
            nBlockXSize, nRequestYSize, pnImageTmp, nBlockXSize, nRequestYSize,
            GDT_Byte, 1, nullptr, 1, 1, 0, nullptr);

        /* iterate over detected values */
        for (int i = 0; i < nBlockYSize; i++)
        {
            for (int j = 0; j < nBlockXSize; j++)
            {
                int nPixOff = (i * nBlockXSize) + j;

                reinterpret_cast<float *>(pImage)[nPixOff] =
                    ((pnImageTmp[nPixOff] * pnImageTmp[nPixOff]) + m_nfOffset) /
                    m_nfTable[nBlockXOff + j];
            }
        }
        CPLFree(pnImageTmp);
    }
    else
    {
        CPLAssert(false);
        return CE_Failure;
    }
    return eErr;
}

/************************************************************************/
/* ==================================================================== */
/*                              RS2Dataset                              */
/* ==================================================================== */
/************************************************************************/

/************************************************************************/
/*                             RS2Dataset()                             */
/************************************************************************/

RS2Dataset::RS2Dataset()
    : psProduct(nullptr), nGCPCount(0), pasGCPList(nullptr),
      papszSubDatasets(nullptr), bHaveGeoTransform(FALSE),
      papszExtraFiles(nullptr)
{
    m_oSRS.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
    m_oGCPSRS.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
}

/************************************************************************/
/*                            ~RS2Dataset()                             */
/************************************************************************/

RS2Dataset::~RS2Dataset()

{
    RS2Dataset::FlushCache(true);

    CPLDestroyXMLNode(psProduct);

    if (nGCPCount > 0)
    {
        GDALDeinitGCPs(nGCPCount, pasGCPList);
        CPLFree(pasGCPList);
    }

    RS2Dataset::CloseDependentDatasets();

    CSLDestroy(papszSubDatasets);
    CSLDestroy(papszExtraFiles);
}

/************************************************************************/
/*                      CloseDependentDatasets()                        */
/************************************************************************/

int RS2Dataset::CloseDependentDatasets()
{
    int bHasDroppedRef = GDALPamDataset::CloseDependentDatasets();

    if (nBands != 0)
        bHasDroppedRef = TRUE;

    for (int iBand = 0; iBand < nBands; iBand++)
    {
        delete papoBands[iBand];
    }
    nBands = 0;

    return bHasDroppedRef;
}

/************************************************************************/
/*                            GetFileList()                             */
/************************************************************************/

char **RS2Dataset::GetFileList()

{
    char **papszFileList = GDALPamDataset::GetFileList();

    papszFileList = CSLInsertStrings(papszFileList, -1, papszExtraFiles);

    return papszFileList;
}

/************************************************************************/
/*                             Identify()                               */
/************************************************************************/

int RS2Dataset::Identify(GDALOpenInfo *poOpenInfo)
{
    /* Check for the case where we're trying to read the calibrated data: */
    if (STARTS_WITH_CI(poOpenInfo->pszFilename, "RADARSAT_2_CALIB:"))
    {
        return TRUE;
    }

    /* Check for directory access when there is a product.xml file in the
       directory. */
    if (poOpenInfo->bIsDirectory)
    {
        const CPLString osMDFilename = CPLFormCIFilenameSafe(
            poOpenInfo->pszFilename, "product.xml", nullptr);

        GDALOpenInfo oOpenInfo(osMDFilename.c_str(), GA_ReadOnly);
        return Identify(&oOpenInfo);
    }

    /* otherwise, do our normal stuff */
    if (strlen(poOpenInfo->pszFilename) < 11 ||
        !EQUAL(poOpenInfo->pszFilename + strlen(poOpenInfo->pszFilename) - 11,
               "product.xml"))
        return FALSE;

    if (poOpenInfo->nHeaderBytes < 100)
        return FALSE;

    if (strstr(reinterpret_cast<const char *>(poOpenInfo->pabyHeader),
               "/rs2") == nullptr ||
        strstr(reinterpret_cast<const char *>(poOpenInfo->pabyHeader),
               "<product") == nullptr)
        return FALSE;

    return TRUE;
}

/************************************************************************/
/*                                Open()                                */
/************************************************************************/

GDALDataset *RS2Dataset::Open(GDALOpenInfo *poOpenInfo)

{
    /* -------------------------------------------------------------------- */
    /*      Is this a RADARSAT-2 Product.xml definition?                   */
    /* -------------------------------------------------------------------- */
    if (!RS2Dataset::Identify(poOpenInfo))
    {
        return nullptr;
    }

    /* -------------------------------------------------------------------- */
    /*        Get subdataset information, if relevant */
    /* -------------------------------------------------------------------- */
    const char *pszFilename = poOpenInfo->pszFilename;
    eCalibration eCalib = None;

    if (STARTS_WITH_CI(pszFilename, "RADARSAT_2_CALIB:"))
    {
        pszFilename += 17;

        if (STARTS_WITH_CI(pszFilename, "BETA0"))
            eCalib = Beta0;
        else if (STARTS_WITH_CI(pszFilename, "SIGMA0"))
            eCalib = Sigma0;
        else if (STARTS_WITH_CI(pszFilename, "GAMMA"))
            eCalib = Gamma;
        else if (STARTS_WITH_CI(pszFilename, "UNCALIB"))
            eCalib = Uncalib;
        else
            eCalib = None;

        /* advance the pointer to the actual filename */
        while (*pszFilename != '\0' && *pszFilename != ':')
            pszFilename++;

        if (*pszFilename == ':')
            pszFilename++;

        // need to redo the directory check:
        // the GDALOpenInfo check would have failed because of the calibration
        // string on the filename
        VSIStatBufL sStat;
        if (VSIStatL(pszFilename, &sStat) == 0)
            poOpenInfo->bIsDirectory = VSI_ISDIR(sStat.st_mode);
    }

    CPLString osMDFilename;
    if (poOpenInfo->bIsDirectory)
    {
        osMDFilename =
            CPLFormCIFilenameSafe(pszFilename, "product.xml", nullptr);
    }
    else
        osMDFilename = pszFilename;

    /* -------------------------------------------------------------------- */
    /*      Ingest the Product.xml file.                                    */
    /* -------------------------------------------------------------------- */
    CPLXMLNode *psProduct = CPLParseXMLFile(osMDFilename);
    if (psProduct == nullptr)
        return nullptr;

    /* -------------------------------------------------------------------- */
    /*      Confirm the requested access is supported.                      */
    /* -------------------------------------------------------------------- */
    if (poOpenInfo->eAccess == GA_Update)
    {
        CPLDestroyXMLNode(psProduct);
        ReportUpdateNotSupportedByDriver("RS2");
        return nullptr;
    }

    CPLXMLNode *psImageAttributes =
        CPLGetXMLNode(psProduct, "=product.imageAttributes");
    if (psImageAttributes == nullptr)
    {
        CPLDestroyXMLNode(psProduct);
        CPLError(CE_Failure, CPLE_OpenFailed,
                 "Failed to find <imageAttributes> in document.");
        return nullptr;
    }

    CPLXMLNode *psImageGenerationParameters =
        CPLGetXMLNode(psProduct, "=product.imageGenerationParameters");
    if (psImageGenerationParameters == nullptr)
    {
        CPLDestroyXMLNode(psProduct);
        CPLError(CE_Failure, CPLE_OpenFailed,
                 "Failed to find <imageGenerationParameters> in document.");
        return nullptr;
    }

    /* -------------------------------------------------------------------- */
    /*      Create the dataset.                                             */
    /* -------------------------------------------------------------------- */
    RS2Dataset *poDS = new RS2Dataset();

    poDS->psProduct = psProduct;

    /* -------------------------------------------------------------------- */
    /*      Get overall image information.                                  */
    /* -------------------------------------------------------------------- */
    poDS->nRasterXSize = atoi(CPLGetXMLValue(
        psImageAttributes, "rasterAttributes.numberOfSamplesPerLine", "-1"));
    poDS->nRasterYSize = atoi(CPLGetXMLValue(
        psImageAttributes, "rasterAttributes.numberofLines", "-1"));
    if (poDS->nRasterXSize <= 1 || poDS->nRasterYSize <= 1)
    {
        CPLError(
            CE_Failure, CPLE_OpenFailed,
            "Non-sane raster dimensions provided in product.xml. If this is "
            "a valid RADARSAT-2 scene, please contact your data provider for "
            "a corrected dataset.");
        delete poDS;
        return nullptr;
    }

    /* -------------------------------------------------------------------- */
    /*      Check product type, as to determine if there are LUTs for       */
    /*      calibration purposes.                                           */
    /* -------------------------------------------------------------------- */

    const char *pszProductType =
        CPLGetXMLValue(psImageGenerationParameters,
                       "generalProcessingInformation.productType", "UNK");

    poDS->SetMetadataItem("PRODUCT_TYPE", pszProductType);

    /* the following cases can be assumed to have no LUTs, as per
     * RN-RP-51-2713, but also common sense
     */
    bool bCanCalib = false;
    if (!(STARTS_WITH_CI(pszProductType, "UNK") ||
          STARTS_WITH_CI(pszProductType, "SSG") ||
          STARTS_WITH_CI(pszProductType, "SPG")))
    {
        bCanCalib = true;
    }

    /* -------------------------------------------------------------------- */
    /*      Get dataType (so we can recognise complex data), and the        */
    /*      bitsPerSample.                                                  */
    /* -------------------------------------------------------------------- */
    const char *pszDataType =
        CPLGetXMLValue(psImageAttributes, "rasterAttributes.dataType", "");
    const int nBitsPerSample = atoi(CPLGetXMLValue(
        psImageAttributes, "rasterAttributes.bitsPerSample", ""));

    GDALDataType eDataType;
    if (nBitsPerSample == 16 && EQUAL(pszDataType, "Complex"))
        eDataType = GDT_CInt16;
    else if (nBitsPerSample == 32 &&
             EQUAL(pszDataType,
                   "Complex"))  // NITF datasets can come in this configuration
        eDataType = GDT_CFloat32;
    else if (nBitsPerSample == 16 && STARTS_WITH_CI(pszDataType, "Mag"))
        eDataType = GDT_UInt16;
    else if (nBitsPerSample == 8 && STARTS_WITH_CI(pszDataType, "Mag"))
        eDataType = GDT_Byte;
    else
    {
        delete poDS;
        CPLError(
            CE_Failure, CPLE_AppDefined,
            "dataType=%s, bitsPerSample=%d: not a supported configuration.",
            pszDataType, nBitsPerSample);
        return nullptr;
    }

    /* while we're at it, extract the pixel spacing information */
    const char *pszPixelSpacing = CPLGetXMLValue(
        psImageAttributes, "rasterAttributes.sampledPixelSpacing", "UNK");
    poDS->SetMetadataItem("PIXEL_SPACING", pszPixelSpacing);

    const char *pszLineSpacing = CPLGetXMLValue(
        psImageAttributes, "rasterAttributes.sampledLineSpacing", "UNK");
    poDS->SetMetadataItem("LINE_SPACING", pszLineSpacing);

    /* -------------------------------------------------------------------- */
    /*      Open each of the data files as a complex band.                  */
    /* -------------------------------------------------------------------- */
    CPLString osBeta0LUT;
    CPLString osGammaLUT;
    CPLString osSigma0LUT;

    char *pszPath = CPLStrdup(CPLGetPathSafe(osMDFilename).c_str());
    const int nFLen = static_cast<int>(osMDFilename.size());

    CPLXMLNode *psNode = psImageAttributes->psChild;
    for (; psNode != nullptr; psNode = psNode->psNext)
    {
        if (psNode->eType != CXT_Element ||
            !(EQUAL(psNode->pszValue, "fullResolutionImageData") ||
              EQUAL(psNode->pszValue, "lookupTable")))
            continue;

        if (EQUAL(psNode->pszValue, "lookupTable") && bCanCalib)
        {
            /* Determine which incidence angle correction this is */
            const char *pszLUTType =
                CPLGetXMLValue(psNode, "incidenceAngleCorrection", "");
            const char *pszLUTFile = CPLGetXMLValue(psNode, "", "");
            CPLString osLUTFilePath =
                CPLFormFilenameSafe(pszPath, pszLUTFile, nullptr);

            if (EQUAL(pszLUTType, ""))
                continue;
            else if (EQUAL(pszLUTType, "Beta Nought") &&
                     IsValidXMLFile(pszPath, pszLUTFile))
            {
                poDS->papszExtraFiles =
                    CSLAddString(poDS->papszExtraFiles, osLUTFilePath);

                const size_t nBufLen = nFLen + 27;
                char *pszBuf = static_cast<char *>(CPLMalloc(nBufLen));
                osBeta0LUT = pszLUTFile;
                poDS->SetMetadataItem("BETA_NOUGHT_LUT", pszLUTFile);

                snprintf(pszBuf, nBufLen, "RADARSAT_2_CALIB:BETA0:%s",
                         osMDFilename.c_str());
                poDS->papszSubDatasets = CSLSetNameValue(
                    poDS->papszSubDatasets, "SUBDATASET_3_NAME", pszBuf);
                poDS->papszSubDatasets =
                    CSLSetNameValue(poDS->papszSubDatasets, "SUBDATASET_3_DESC",
                                    "Beta Nought calibrated");
                CPLFree(pszBuf);
            }
            else if (EQUAL(pszLUTType, "Sigma Nought") &&
                     IsValidXMLFile(pszPath, pszLUTFile))
            {
                poDS->papszExtraFiles =
                    CSLAddString(poDS->papszExtraFiles, osLUTFilePath);

                const size_t nBufLen = nFLen + 27;
                char *pszBuf = static_cast<char *>(CPLMalloc(nBufLen));
                osSigma0LUT = pszLUTFile;
                poDS->SetMetadataItem("SIGMA_NOUGHT_LUT", pszLUTFile);

                snprintf(pszBuf, nBufLen, "RADARSAT_2_CALIB:SIGMA0:%s",
                         osMDFilename.c_str());
                poDS->papszSubDatasets = CSLSetNameValue(
                    poDS->papszSubDatasets, "SUBDATASET_2_NAME", pszBuf);
                poDS->papszSubDatasets =
                    CSLSetNameValue(poDS->papszSubDatasets, "SUBDATASET_2_DESC",
                                    "Sigma Nought calibrated");
                CPLFree(pszBuf);
            }
            else if (EQUAL(pszLUTType, "Gamma") &&
                     IsValidXMLFile(pszPath, pszLUTFile))
            {
                poDS->papszExtraFiles =
                    CSLAddString(poDS->papszExtraFiles, osLUTFilePath);

                const size_t nBufLen = nFLen + 27;
                char *pszBuf = static_cast<char *>(CPLMalloc(nBufLen));
                osGammaLUT = pszLUTFile;
                poDS->SetMetadataItem("GAMMA_LUT", pszLUTFile);
                snprintf(pszBuf, nBufLen, "RADARSAT_2_CALIB:GAMMA:%s",
                         osMDFilename.c_str());
                poDS->papszSubDatasets = CSLSetNameValue(
                    poDS->papszSubDatasets, "SUBDATASET_4_NAME", pszBuf);
                poDS->papszSubDatasets =
                    CSLSetNameValue(poDS->papszSubDatasets, "SUBDATASET_4_DESC",
                                    "Gamma calibrated");
                CPLFree(pszBuf);
            }
            continue;
        }

        /* --------------------------------------------------------------------
         */
        /*      Fetch filename. */
        /* --------------------------------------------------------------------
         */
        const char *pszBasename = CPLGetXMLValue(psNode, "", "");
        if (*pszBasename == '\0')
            continue;

        /* --------------------------------------------------------------------
         */
        /*      Form full filename (path of product.xml + basename). */
        /* --------------------------------------------------------------------
         */
        char *pszFullname = CPLStrdup(
            CPLFormFilenameSafe(pszPath, pszBasename, nullptr).c_str());

        /* --------------------------------------------------------------------
         */
        /*      Try and open the file. */
        /* --------------------------------------------------------------------
         */
        GDALDataset *poBandFile =
            GDALDataset::FromHandle(GDALOpen(pszFullname, GA_ReadOnly));
        if (poBandFile == nullptr)
        {
            CPLFree(pszFullname);
            continue;
        }
        if (poBandFile->GetRasterCount() == 0)
        {
            GDALClose(reinterpret_cast<GDALRasterBandH>(poBandFile));
            CPLFree(pszFullname);
            continue;
        }

        /* Some CFloat32 NITF files have nBitsPerSample incorrectly reported */
        /* as 16, and get misinterpreted as CInt16.  Check the underlying NITF
         */
        /* and override if this is the case. */
        if (poBandFile->GetRasterBand(1)->GetRasterDataType() == GDT_CFloat32)
            eDataType = GDT_CFloat32;

        BandMapping b = GetBandFileMapping(eDataType, poBandFile);
        const bool twoBandComplex = b == TWOBANDCOMPLEX;

        poDS->papszExtraFiles =
            CSLAddString(poDS->papszExtraFiles, pszFullname);

        /* --------------------------------------------------------------------
         */
        /*      Create the band. */
        /* --------------------------------------------------------------------
         */
        if (eCalib == None || eCalib == Uncalib)
        {
            RS2RasterBand *poBand = new RS2RasterBand(
                poDS, eDataType, CPLGetXMLValue(psNode, "pole", ""), poBandFile,
                twoBandComplex);

            poDS->SetBand(poDS->GetRasterCount() + 1, poBand);
        }
        else
        {
            const char *pszLUT = nullptr;
            switch (eCalib)
            {
                case Sigma0:
                    pszLUT = osSigma0LUT;
                    break;
                case Beta0:
                    pszLUT = osBeta0LUT;
                    break;
                case Gamma:
                    pszLUT = osGammaLUT;
                    break;
                default:
                    /* we should bomb gracefully... */
                    pszLUT = osSigma0LUT;
            }
            RS2CalibRasterBand *poBand = new RS2CalibRasterBand(
                poDS, CPLGetXMLValue(psNode, "pole", ""), eDataType, poBandFile,
                eCalib, CPLFormFilenameSafe(pszPath, pszLUT, nullptr).c_str());
            poDS->SetBand(poDS->GetRasterCount() + 1, poBand);
        }

        CPLFree(pszFullname);
    }

    if (poDS->papszSubDatasets != nullptr && eCalib == None)
    {
        const size_t nBufLen = nFLen + 28;
        char *pszBuf = static_cast<char *>(CPLMalloc(nBufLen));
        snprintf(pszBuf, nBufLen, "RADARSAT_2_CALIB:UNCALIB:%s",
                 osMDFilename.c_str());
        poDS->papszSubDatasets = CSLSetNameValue(poDS->papszSubDatasets,
                                                 "SUBDATASET_1_NAME", pszBuf);
        poDS->papszSubDatasets =
            CSLSetNameValue(poDS->papszSubDatasets, "SUBDATASET_1_DESC",
                            "Uncalibrated digital numbers");
        CPLFree(pszBuf);
    }
    else if (poDS->papszSubDatasets != nullptr)
    {
        CSLDestroy(poDS->papszSubDatasets);
        poDS->papszSubDatasets = nullptr;
    }

    /* -------------------------------------------------------------------- */
    /*      Set the appropriate MATRIX_REPRESENTATION.                      */
    /* -------------------------------------------------------------------- */

    if (poDS->GetRasterCount() == 4 &&
        (eDataType == GDT_CInt16 || eDataType == GDT_CFloat32))
    {
        poDS->SetMetadataItem("MATRIX_REPRESENTATION", "SCATTERING");
    }

    /* -------------------------------------------------------------------- */
    /*      Collect a few useful metadata items                             */
    /* -------------------------------------------------------------------- */

    CPLXMLNode *psSourceAttrs =
        CPLGetXMLNode(psProduct, "=product.sourceAttributes");
    const char *pszItem = CPLGetXMLValue(psSourceAttrs, "satellite", "");
    poDS->SetMetadataItem("SATELLITE_IDENTIFIER", pszItem);

    pszItem = CPLGetXMLValue(psSourceAttrs, "sensor", "");
    poDS->SetMetadataItem("SENSOR_IDENTIFIER", pszItem);

    if (psSourceAttrs != nullptr)
    {
        /* Get beam mode mnemonic */
        pszItem = CPLGetXMLValue(psSourceAttrs, "beamModeMnemonic", "UNK");
        poDS->SetMetadataItem("BEAM_MODE", pszItem);
        pszItem = CPLGetXMLValue(psSourceAttrs, "rawDataStartTime", "UNK");
        poDS->SetMetadataItem("ACQUISITION_START_TIME", pszItem);

        pszItem =
            CPLGetXMLValue(psSourceAttrs, "inputDatasetFacilityId", "UNK");
        poDS->SetMetadataItem("FACILITY_IDENTIFIER", pszItem);

        pszItem = CPLGetXMLValue(
            psSourceAttrs, "orbitAndAttitude.orbitInformation.passDirection",
            "UNK");
        poDS->SetMetadataItem("ORBIT_DIRECTION", pszItem);
        pszItem = CPLGetXMLValue(
            psSourceAttrs, "orbitAndAttitude.orbitInformation.orbitDataSource",
            "UNK");
        poDS->SetMetadataItem("ORBIT_DATA_SOURCE", pszItem);
        pszItem = CPLGetXMLValue(
            psSourceAttrs, "orbitAndAttitude.orbitInformation.orbitDataFile",
            "UNK");
        poDS->SetMetadataItem("ORBIT_DATA_FILE", pszItem);
    }

    CPLXMLNode *psSarProcessingInformation =
        CPLGetXMLNode(psProduct, "=product.imageGenerationParameters");

    if (psSarProcessingInformation != nullptr)
    {
        /* Get incidence angle information */
        pszItem = CPLGetXMLValue(
            psSarProcessingInformation,
            "sarProcessingInformation.incidenceAngleNearRange", "UNK");
        poDS->SetMetadataItem("NEAR_RANGE_INCIDENCE_ANGLE", pszItem);

        pszItem = CPLGetXMLValue(
            psSarProcessingInformation,
            "sarProcessingInformation.incidenceAngleFarRange", "UNK");
        poDS->SetMetadataItem("FAR_RANGE_INCIDENCE_ANGLE", pszItem);

        pszItem = CPLGetXMLValue(psSarProcessingInformation,
                                 "sarProcessingInformation.slantRangeNearEdge",
                                 "UNK");
        poDS->SetMetadataItem("SLANT_RANGE_NEAR_EDGE", pszItem);

        pszItem = CPLGetXMLValue(
            psSarProcessingInformation,
            "sarProcessingInformation.zeroDopplerTimeFirstLine", "UNK");
        poDS->SetMetadataItem("FIRST_LINE_TIME", pszItem);

        pszItem = CPLGetXMLValue(
            psSarProcessingInformation,
            "sarProcessingInformation.zeroDopplerTimeLastLine", "UNK");
        poDS->SetMetadataItem("LAST_LINE_TIME", pszItem);

        pszItem =
            CPLGetXMLValue(psSarProcessingInformation,
                           "generalProcessingInformation.productType", "UNK");
        poDS->SetMetadataItem("PRODUCT_TYPE", pszItem);

        pszItem = CPLGetXMLValue(
            psSarProcessingInformation,
            "generalProcessingInformation.processingFacility", "UNK");
        poDS->SetMetadataItem("PROCESSING_FACILITY", pszItem);

        pszItem = CPLGetXMLValue(psSarProcessingInformation,
                                 "generalProcessingInformation.processingTime",
                                 "UNK");
        poDS->SetMetadataItem("PROCESSING_TIME", pszItem);
    }

    /*--------------------------------------------------------------------- */
    /*      Collect Map projection/Geotransform information, if present     */
    /* -------------------------------------------------------------------- */
    CPLXMLNode *psMapProjection =
        CPLGetXMLNode(psImageAttributes, "geographicInformation.mapProjection");

    if (psMapProjection != nullptr)
    {
        CPLXMLNode *psPos =
            CPLGetXMLNode(psMapProjection, "positioningInformation");

        pszItem =
            CPLGetXMLValue(psMapProjection, "mapProjectionDescriptor", "UNK");
        poDS->SetMetadataItem("MAP_PROJECTION_DESCRIPTOR", pszItem);

        pszItem =
            CPLGetXMLValue(psMapProjection, "mapProjectionOrientation", "UNK");
        poDS->SetMetadataItem("MAP_PROJECTION_ORIENTATION", pszItem);

        pszItem = CPLGetXMLValue(psMapProjection, "resamplingKernel", "UNK");
        poDS->SetMetadataItem("RESAMPLING_KERNEL", pszItem);

        pszItem = CPLGetXMLValue(psMapProjection, "satelliteHeading", "UNK");
        poDS->SetMetadataItem("SATELLITE_HEADING", pszItem);

        if (psPos != nullptr)
        {
            const double tl_x = CPLStrtod(
                CPLGetXMLValue(psPos, "upperLeftCorner.mapCoordinate.easting",
                               "0.0"),
                nullptr);
            const double tl_y = CPLStrtod(
                CPLGetXMLValue(psPos, "upperLeftCorner.mapCoordinate.northing",
                               "0.0"),
                nullptr);
            const double bl_x = CPLStrtod(
                CPLGetXMLValue(psPos, "lowerLeftCorner.mapCoordinate.easting",
                               "0.0"),
                nullptr);
            const double bl_y = CPLStrtod(
                CPLGetXMLValue(psPos, "lowerLeftCorner.mapCoordinate.northing",
                               "0.0"),
                nullptr);
            const double tr_x = CPLStrtod(
                CPLGetXMLValue(psPos, "upperRightCorner.mapCoordinate.easting",
                               "0.0"),
                nullptr);
            const double tr_y = CPLStrtod(
                CPLGetXMLValue(psPos, "upperRightCorner.mapCoordinate.northing",
                               "0.0"),
                nullptr);
            poDS->m_gt[1] = (tr_x - tl_x) / (poDS->nRasterXSize - 1);
            poDS->m_gt[4] = (tr_y - tl_y) / (poDS->nRasterXSize - 1);
            poDS->m_gt[2] = (bl_x - tl_x) / (poDS->nRasterYSize - 1);
            poDS->m_gt[5] = (bl_y - tl_y) / (poDS->nRasterYSize - 1);
            poDS->m_gt[0] = (tl_x - 0.5 * poDS->m_gt[1] - 0.5 * poDS->m_gt[2]);
            poDS->m_gt[3] = (tl_y - 0.5 * poDS->m_gt[4] - 0.5 * poDS->m_gt[5]);

            /* Use bottom right pixel to test geotransform */
            const double br_x = CPLStrtod(
                CPLGetXMLValue(psPos, "lowerRightCorner.mapCoordinate.easting",
                               "0.0"),
                nullptr);
            const double br_y = CPLStrtod(
                CPLGetXMLValue(psPos, "lowerRightCorner.mapCoordinate.northing",
                               "0.0"),
                nullptr);
            const double testx = poDS->m_gt[0] +
                                 poDS->m_gt[1] * (poDS->nRasterXSize - 0.5) +
                                 poDS->m_gt[2] * (poDS->nRasterYSize - 0.5);
            const double testy = poDS->m_gt[3] +
                                 poDS->m_gt[4] * (poDS->nRasterXSize - 0.5) +
                                 poDS->m_gt[5] * (poDS->nRasterYSize - 0.5);

            /* Give 1/4 pixel numerical error leeway in calculating location
               based on affine transform */
            if ((fabs(testx - br_x) >
                 fabs(0.25 * (poDS->m_gt[1] + poDS->m_gt[2]))) ||
                (fabs(testy - br_y) >
                 fabs(0.25 * (poDS->m_gt[4] + poDS->m_gt[5]))))
            {
                CPLError(CE_Warning, CPLE_AppDefined,
                         "Unexpected error in calculating affine transform: "
                         "corner coordinates inconsistent.");
            }
            else
            {
                poDS->bHaveGeoTransform = TRUE;
            }
        }
    }

    /* -------------------------------------------------------------------- */
    /*      Collect Projection String Information                           */
    /* -------------------------------------------------------------------- */
    CPLXMLNode *psEllipsoid =
        CPLGetXMLNode(psImageAttributes,
                      "geographicInformation.referenceEllipsoidParameters");

    if (psEllipsoid != nullptr)
    {
        OGRSpatialReference oLL, oPrj;
        oLL.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
        oPrj.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);

        const char *pszGeodeticTerrainHeight =
            CPLGetXMLValue(psEllipsoid, "geodeticTerrainHeight", "UNK");
        poDS->SetMetadataItem("GEODETIC_TERRAIN_HEIGHT",
                              pszGeodeticTerrainHeight);

        const char *pszEllipsoidName =
            CPLGetXMLValue(psEllipsoid, "ellipsoidName", "");
        double minor_axis =
            CPLAtof(CPLGetXMLValue(psEllipsoid, "semiMinorAxis", "0.0"));
        double major_axis =
            CPLAtof(CPLGetXMLValue(psEllipsoid, "semiMajorAxis", "0.0"));

        if (EQUAL(pszEllipsoidName, "") || (minor_axis == 0.0) ||
            (major_axis == 0.0))
        {
            CPLError(CE_Warning, CPLE_AppDefined,
                     "Warning- incomplete"
                     " ellipsoid information.  Using wgs-84 parameters.\n");
            oLL.SetWellKnownGeogCS("WGS84");
            oPrj.SetWellKnownGeogCS("WGS84");
        }
        else if (EQUAL(pszEllipsoidName, "WGS84") ||
                 EQUAL(pszEllipsoidName, "WGS 1984"))
        {
            oLL.SetWellKnownGeogCS("WGS84");
            oPrj.SetWellKnownGeogCS("WGS84");
        }
        else
        {
            const double inv_flattening =
                major_axis / (major_axis - minor_axis);
            oLL.SetGeogCS("", "", pszEllipsoidName, major_axis, inv_flattening);
            oPrj.SetGeogCS("", "", pszEllipsoidName, major_axis,
                           inv_flattening);
        }

        if (psMapProjection != nullptr)
        {
            const char *pszProj =
                CPLGetXMLValue(psMapProjection, "mapProjectionDescriptor", "");
            bool bUseProjInfo = false;

            CPLXMLNode *psUtmParams =
                CPLGetXMLNode(psMapProjection, "utmProjectionParameters");

            CPLXMLNode *psNspParams =
                CPLGetXMLNode(psMapProjection, "nspProjectionParameters");

            if ((psUtmParams != nullptr) && poDS->bHaveGeoTransform)
            {
                /* double origEasting, origNorthing; */
                bool bNorth = true;

                const int utmZone =
                    atoi(CPLGetXMLValue(psUtmParams, "utmZone", ""));
                const char *pszHemisphere =
                    CPLGetXMLValue(psUtmParams, "hemisphere", "");
#if 0
                origEasting = CPLStrtod(CPLGetXMLValue(
                    psUtmParams, "mapOriginFalseEasting", "0.0" ), nullptr);
                origNorthing = CPLStrtod(CPLGetXMLValue(
                    psUtmParams, "mapOriginFalseNorthing", "0.0" ), nullptr);
#endif
                if (STARTS_WITH_CI(pszHemisphere, "southern"))
                    bNorth = FALSE;

                if (STARTS_WITH_CI(pszProj, "UTM"))
                {
                    oPrj.SetUTM(utmZone, bNorth);
                    bUseProjInfo = true;
                }
            }
            else if ((psNspParams != nullptr) && poDS->bHaveGeoTransform)
            {
                const double origEasting = CPLStrtod(
                    CPLGetXMLValue(psNspParams, "mapOriginFalseEasting", "0.0"),
                    nullptr);
                const double origNorthing =
                    CPLStrtod(CPLGetXMLValue(psNspParams,
                                             "mapOriginFalseNorthing", "0.0"),
                              nullptr);
                const double copLong = CPLStrtod(
                    CPLGetXMLValue(psNspParams, "centerOfProjectionLongitude",
                                   "0.0"),
                    nullptr);
                const double copLat = CPLStrtod(
                    CPLGetXMLValue(psNspParams, "centerOfProjectionLatitude",
                                   "0.0"),
                    nullptr);
                const double sP1 = CPLStrtod(
                    CPLGetXMLValue(psNspParams, "standardParallels1", "0.0"),
                    nullptr);
                const double sP2 = CPLStrtod(
                    CPLGetXMLValue(psNspParams, "standardParallels2", "0.0"),
                    nullptr);

                if (STARTS_WITH_CI(pszProj, "ARC"))
                {
                    /* Albers Conical Equal Area */
                    oPrj.SetACEA(sP1, sP2, copLat, copLong, origEasting,
                                 origNorthing);
                    bUseProjInfo = true;
                }
                else if (STARTS_WITH_CI(pszProj, "LCC"))
                {
                    /* Lambert Conformal Conic */
                    oPrj.SetLCC(sP1, sP2, copLat, copLong, origEasting,
                                origNorthing);
                    bUseProjInfo = true;
                }
                else if (STARTS_WITH_CI(pszProj, "STPL"))
                {
                    /* State Plate
                       ASSUMPTIONS: "zone" in product.xml matches USGS
                       definition as expected by ogr spatial reference; NAD83
                       zones (versus NAD27) are assumed. */

                    const int nSPZone =
                        atoi(CPLGetXMLValue(psNspParams, "zone", "1"));

                    oPrj.SetStatePlane(nSPZone, TRUE, nullptr, 0.0);
                    bUseProjInfo = true;
                }
            }

            if (bUseProjInfo)
            {
                poDS->m_oSRS = std::move(oPrj);
            }
            else
            {
                CPLError(CE_Warning, CPLE_AppDefined,
                         "Unable to interpret "
                         "projection information; check mapProjection info in "
                         "product.xml!");
            }
        }

        poDS->m_oGCPSRS = std::move(oLL);
    }

    /* -------------------------------------------------------------------- */
    /*      Collect GCPs.                                                   */
    /* -------------------------------------------------------------------- */
    CPLXMLNode *psGeoGrid = CPLGetXMLNode(
        psImageAttributes, "geographicInformation.geolocationGrid");

    if (psGeoGrid != nullptr)
    {
        /* count GCPs */
        poDS->nGCPCount = 0;

        for (psNode = psGeoGrid->psChild; psNode != nullptr;
             psNode = psNode->psNext)
        {
            if (EQUAL(psNode->pszValue, "imageTiePoint"))
                poDS->nGCPCount++;
        }

        poDS->pasGCPList = reinterpret_cast<GDAL_GCP *>(
            CPLCalloc(sizeof(GDAL_GCP), poDS->nGCPCount));

        poDS->nGCPCount = 0;

        for (psNode = psGeoGrid->psChild; psNode != nullptr;
             psNode = psNode->psNext)
        {
            GDAL_GCP *psGCP = poDS->pasGCPList + poDS->nGCPCount;

            if (!EQUAL(psNode->pszValue, "imageTiePoint"))
                continue;

            poDS->nGCPCount++;

            char szID[32];
            snprintf(szID, sizeof(szID), "%d", poDS->nGCPCount);
            psGCP->pszId = CPLStrdup(szID);
            psGCP->pszInfo = CPLStrdup("");
            psGCP->dfGCPPixel =
                CPLAtof(CPLGetXMLValue(psNode, "imageCoordinate.pixel", "0")) +
                0.5;
            psGCP->dfGCPLine =
                CPLAtof(CPLGetXMLValue(psNode, "imageCoordinate.line", "0")) +
                0.5;
            psGCP->dfGCPX = CPLAtof(
                CPLGetXMLValue(psNode, "geodeticCoordinate.longitude", ""));
            psGCP->dfGCPY = CPLAtof(
                CPLGetXMLValue(psNode, "geodeticCoordinate.latitude", ""));
            psGCP->dfGCPZ = CPLAtof(
                CPLGetXMLValue(psNode, "geodeticCoordinate.height", ""));
        }
    }

    CPLFree(pszPath);

    /* -------------------------------------------------------------------- */
    /*      Collect RPC.                                                   */
    /* -------------------------------------------------------------------- */
    CPLXMLNode *psRationalFunctions = CPLGetXMLNode(
        psImageAttributes, "geographicInformation.rationalFunctions");
    if (psRationalFunctions != nullptr)
    {
        char **papszRPC = nullptr;
        static const char *const apszXMLToGDALMapping[] = {
            "biasError",
            "ERR_BIAS",
            "randomError",
            "ERR_RAND",
            //"lineFitQuality", "????",
            //"pixelFitQuality", "????",
            "lineOffset",
            "LINE_OFF",
            "pixelOffset",
            "SAMP_OFF",
            "latitudeOffset",
            "LAT_OFF",
            "longitudeOffset",
            "LONG_OFF",
            "heightOffset",
            "HEIGHT_OFF",
            "lineScale",
            "LINE_SCALE",
            "pixelScale",
            "SAMP_SCALE",
            "latitudeScale",
            "LAT_SCALE",
            "longitudeScale",
            "LONG_SCALE",
            "heightScale",
            "HEIGHT_SCALE",
            "lineNumeratorCoefficients",
            "LINE_NUM_COEFF",
            "lineDenominatorCoefficients",
            "LINE_DEN_COEFF",
            "pixelNumeratorCoefficients",
            "SAMP_NUM_COEFF",
            "pixelDenominatorCoefficients",
            "SAMP_DEN_COEFF",
        };
        for (size_t i = 0; i < CPL_ARRAYSIZE(apszXMLToGDALMapping); i += 2)
        {
            const char *pszValue = CPLGetXMLValue(
                psRationalFunctions, apszXMLToGDALMapping[i], nullptr);
            if (pszValue)
                papszRPC = CSLSetNameValue(
                    papszRPC, apszXMLToGDALMapping[i + 1], pszValue);
        }
        poDS->GDALDataset::SetMetadata(papszRPC, "RPC");
        CSLDestroy(papszRPC);
    }

    /* -------------------------------------------------------------------- */
    /*      Initialize any PAM information.                                 */
    /* -------------------------------------------------------------------- */
    CPLString osDescription;

    switch (eCalib)
    {
        case Sigma0:
            osDescription.Printf("RADARSAT_2_CALIB:SIGMA0:%s",
                                 osMDFilename.c_str());
            break;
        case Beta0:
            osDescription.Printf("RADARSAT_2_CALIB:BETA0:%s",
                                 osMDFilename.c_str());
            break;
        case Gamma:
            osDescription.Printf("RADARSAT_2_CALIB:GAMMA0:%s",
                                 osMDFilename.c_str());
            break;
        case Uncalib:
            osDescription.Printf("RADARSAT_2_CALIB:UNCALIB:%s",
                                 osMDFilename.c_str());
            break;
        default:
            osDescription = osMDFilename;
    }

    if (eCalib != None)
        poDS->papszExtraFiles =
            CSLAddString(poDS->papszExtraFiles, osMDFilename);

    /* -------------------------------------------------------------------- */
    /*      Initialize any PAM information.                                 */
    /* -------------------------------------------------------------------- */
    poDS->SetDescription(osDescription);

    poDS->SetPhysicalFilename(osMDFilename);
    poDS->SetSubdatasetName(osDescription);

    poDS->TryLoadXML();

    /* -------------------------------------------------------------------- */
    /*      Check for overviews.                                            */
    /* -------------------------------------------------------------------- */
    poDS->oOvManager.Initialize(poDS, ":::VIRTUAL:::");

    return poDS;
}

/************************************************************************/
/*                            GetGCPCount()                             */
/************************************************************************/

int RS2Dataset::GetGCPCount()

{
    return nGCPCount;
}

/************************************************************************/
/*                          GetGCPSpatialRef()                          */
/************************************************************************/

const OGRSpatialReference *RS2Dataset::GetGCPSpatialRef() const

{
    return m_oGCPSRS.IsEmpty() ? nullptr : &m_oGCPSRS;
}

/************************************************************************/
/*                               GetGCPs()                              */
/************************************************************************/

const GDAL_GCP *RS2Dataset::GetGCPs()

{
    return pasGCPList;
}

/************************************************************************/
/*                          GetSpatialRef()                             */
/************************************************************************/

const OGRSpatialReference *RS2Dataset::GetSpatialRef() const

{
    return m_oSRS.IsEmpty() ? nullptr : &m_oSRS;
}

/************************************************************************/
/*                          GetGeoTransform()                           */
/************************************************************************/

CPLErr RS2Dataset::GetGeoTransform(GDALGeoTransform &gt) const

{
    gt = m_gt;

    if (bHaveGeoTransform)
        return CE_None;

    return CE_Failure;
}

/************************************************************************/
/*                      GetMetadataDomainList()                         */
/************************************************************************/

char **RS2Dataset::GetMetadataDomainList()
{
    return BuildMetadataDomainList(GDALDataset::GetMetadataDomainList(), TRUE,
                                   "SUBDATASETS", nullptr);
}

/************************************************************************/
/*                            GetMetadata()                             */
/************************************************************************/

char **RS2Dataset::GetMetadata(const char *pszDomain)

{
    if (pszDomain != nullptr && STARTS_WITH_CI(pszDomain, "SUBDATASETS") &&
        papszSubDatasets != nullptr)
        return papszSubDatasets;

    return GDALDataset::GetMetadata(pszDomain);
}

/************************************************************************/
/*                         GDALRegister_RS2()                          */
/************************************************************************/

void GDALRegister_RS2()

{
    if (GDALGetDriverByName("RS2") != nullptr)
        return;

    GDALDriver *poDriver = new GDALDriver();

    poDriver->SetDescription("RS2");
    poDriver->SetMetadataItem(GDAL_DCAP_RASTER, "YES");
    poDriver->SetMetadataItem(GDAL_DMD_LONGNAME, "RadarSat 2 XML Product");
    poDriver->SetMetadataItem(GDAL_DMD_HELPTOPIC, "drivers/raster/rs2.html");
    poDriver->SetMetadataItem(GDAL_DMD_SUBDATASETS, "YES");
    poDriver->SetMetadataItem(GDAL_DCAP_VIRTUALIO, "YES");

    poDriver->pfnOpen = RS2Dataset::Open;
    poDriver->pfnIdentify = RS2Dataset::Identify;

    GetGDALDriverManager()->RegisterDriver(poDriver);
}
