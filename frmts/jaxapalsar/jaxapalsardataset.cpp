/******************************************************************************
 *
 * Project:  PALSAR JAXA imagery reader
 * Purpose:  Support for PALSAR L1.1/1.5 imagery and appropriate metadata from
 *           JAXA and JAXA-supported ground stations (ASF, ESA, etc.). This
 *           driver does not support ERSDAC products.
 * Author:   Philippe Vachon <philippe@cowpig.ca>
 *
 ******************************************************************************
 * Copyright (c) 2007, Philippe P. Vachon <philippe@cowpig.ca>
 * Copyright (c) 2008-2011, Even Rouault <even dot rouault at spatialys.com>
 *
 * SPDX-License-Identifier: MIT
 ****************************************************************************/

#include "gdal_frmts.h"
#include "gdal_pam.h"

#if defined(_WIN32)
#define SEP_STRING "\\"
#else
#define SEP_STRING "/"
#endif

/* read binary fields */
#ifdef CPL_LSB
#define READ_WORD(f, x)                                                        \
    do                                                                         \
    {                                                                          \
        VSIFReadL(&(x), 4, 1, (f));                                            \
        (x) = CPL_SWAP32((x));                                                 \
    } while (false);
#define READ_SHORT(f, x)                                                       \
    do                                                                         \
    {                                                                          \
        VSIFReadL(&(x), 2, 1, (f));                                            \
        (x) = CPL_SWAP16((x));                                                 \
    } while (false);
#else
#define READ_WORD(f, x)                                                        \
    do                                                                         \
    {                                                                          \
        VSIFReadL(&(x), 4, 1, (f));                                            \
    } while (false);
#define READ_SHORT(f, x)                                                       \
    do                                                                         \
    {                                                                          \
        VSIFReadL(&(x), 2, 1, (f));                                            \
    } while (false);
#endif /* def CPL_LSB */
#define READ_BYTE(f, x)                                                        \
    do                                                                         \
    {                                                                          \
        VSIFReadL(&(x), 1, 1, (f));                                            \
    } while (false);

/* read floating point value stored as ASCII */
#define READ_CHAR_FLOAT(n, l, f)                                               \
    do                                                                         \
    {                                                                          \
        char psBuf[(l + 1)];                                                   \
        psBuf[(l)] = '\0';                                                     \
        VSIFReadL(&psBuf, (l), 1, (f));                                        \
        (n) = CPLAtof(psBuf);                                                  \
    } while (false);

/* read numbers stored as ASCII */
#define READ_CHAR_VAL(x, n, f)                                                 \
    do                                                                         \
    {                                                                          \
        char psBuf[(n + 1)];                                                   \
        psBuf[(n)] = '\0';                                                     \
        VSIFReadL(&psBuf, (n), 1, (f));                                        \
        (x) = atoi(psBuf);                                                     \
    } while (false);

/* read string fields
 * note: string must be size of field to be extracted + 1
 */
#define READ_STRING(s, n, f)                                                   \
    do                                                                         \
    {                                                                          \
        VSIFReadL(&(s), 1, (n), (f));                                          \
        (s)[(n)] = '\0';                                                       \
    } while (false);

/*************************************************************************/
/* a few key offsets in the volume directory file */
#define VOL_DESC_RECORD_LENGTH 360
#define FILE_PTR_RECORD_LENGTH 360
#define NUM_RECORDS_OFFSET 160

/* a few key offsets and values within the File Pointer record */
#define REF_FILE_CLASS_CODE_OFFSET 66
#define REF_FILE_CLASS_CODE_LENGTH 4
#define FILE_NAME_OFFSET 310

/* some image option descriptor records */
#define BITS_PER_SAMPLE_OFFSET 216
#define BITS_PER_SAMPLE_LENGTH 4
#define SAMPLES_PER_GROUP_OFFSET 220
#define SAMPLES_PER_GROUP_LENGTH 4
#define NUMBER_LINES_OFFSET 236
#define NUMBER_LINES_LENGTH 8
#define SAR_DATA_RECORD_LENGTH_OFFSET 186
#define SAR_DATA_RECORD_LENGTH_LENGTH 6

#define IMAGE_OPT_DESC_LENGTH 720

#define SIG_DAT_REC_OFFSET 412
#define PROC_DAT_REC_OFFSET 192

/* metadata to be extracted from the leader file */
#define LEADER_FILE_DESCRIPTOR_LENGTH 720
#define DATA_SET_SUMMARY_LENGTH 4096

/* relative to end of leader file descriptor */
#define EFFECTIVE_LOOKS_AZIMUTH_OFFSET 1174 /* floating point text */
#define EFFECTIVE_LOOKS_AZIMUTH_LENGTH 16

/* relative to leader file descriptor + dataset summary length */
#define PIXEL_SPACING_OFFSET 92
#define LINE_SPACING_OFFSET 108
#define ALPHANUMERIC_PROJECTION_NAME_OFFSET 412
#define TOP_LEFT_LAT_OFFSET 1072
#define TOP_LEFT_LON_OFFSET 1088
#define TOP_RIGHT_LAT_OFFSET 1104
#define TOP_RIGHT_LON_OFFSET 1120
#define BOTTOM_RIGHT_LAT_OFFSET 1136
#define BOTTOM_RIGHT_LON_OFFSET 1152
#define BOTTOM_LEFT_LAT_OFFSET 1168
#define BOTTOM_LEFT_LON_OFFSET 1184

namespace gdal::PSALSARJaxa
{
/* a few useful enums */
enum eFileType
{
    level_11 = 0,
    level_15,
    level_10,
    level_unknown = 999,
};

enum ePolarization
{
    hh = 0,
    hv,
    vh,
    vv
};
}  // namespace gdal::PSALSARJaxa

using namespace gdal::PSALSARJaxa;

/************************************************************************/
/* ==================================================================== */
/*                        PALSARJaxaDataset                             */
/* ==================================================================== */
/************************************************************************/

class PALSARJaxaRasterBand;

class PALSARJaxaDataset final : public GDALPamDataset
{
    friend class PALSARJaxaRasterBand;

  private:
    GDAL_GCP *pasGCPList;
    int nGCPCount;
    eFileType nFileType;

  public:
    PALSARJaxaDataset();
    ~PALSARJaxaDataset();

    int GetGCPCount() override;
    const GDAL_GCP *GetGCPs() override;

    static GDALDataset *Open(GDALOpenInfo *poOpenInfo);
    static int Identify(GDALOpenInfo *poOpenInfo);
    static void ReadMetadata(PALSARJaxaDataset *poDS, VSILFILE *fp);
};

PALSARJaxaDataset::PALSARJaxaDataset()
    : pasGCPList(nullptr), nGCPCount(0), nFileType(level_unknown)
{
}

PALSARJaxaDataset::~PALSARJaxaDataset()
{
    if (nGCPCount > 0)
    {
        GDALDeinitGCPs(nGCPCount, pasGCPList);
        CPLFree(pasGCPList);
    }
}

/************************************************************************/
/* ==================================================================== */
/*                        PALSARJaxaRasterBand                          */
/* ==================================================================== */
/************************************************************************/

class PALSARJaxaRasterBand final : public GDALRasterBand
{
    VSILFILE *fp;
    ePolarization nPolarization;
    eFileType nFileType;
    int nBitsPerSample;
    int nSamplesPerGroup;
    int nRecordSize;

  public:
    PALSARJaxaRasterBand(PALSARJaxaDataset *poDS, int nBand, VSILFILE *fp);
    ~PALSARJaxaRasterBand();

    CPLErr IReadBlock(int nBlockXOff, int nBlockYOff, void *pImage) override;
};

/************************************************************************/
/*                         PALSARJaxaRasterBand()                       */
/************************************************************************/

PALSARJaxaRasterBand::PALSARJaxaRasterBand(PALSARJaxaDataset *poDSIn,
                                           int nBandIn, VSILFILE *fpIn)
    : fp(fpIn), nPolarization(hh), nBitsPerSample(0), nSamplesPerGroup(0),
      nRecordSize(0)
{
    poDS = poDSIn;
    nBand = nBandIn;

    /* Read image options record to determine the type of data */
    VSIFSeekL(fp, BITS_PER_SAMPLE_OFFSET, SEEK_SET);
    READ_CHAR_VAL(nBitsPerSample, BITS_PER_SAMPLE_LENGTH, fp);
    READ_CHAR_VAL(nSamplesPerGroup, SAMPLES_PER_GROUP_LENGTH, fp);

    if (nBitsPerSample == 32 && nSamplesPerGroup == 2)
    {
        eDataType = GDT_CFloat32;
        nFileType = level_11;
    }
    else if (nBitsPerSample == 8 && nSamplesPerGroup == 2)
    {
        eDataType = GDT_CInt16; /* shuold be 2 x signed byte */
        nFileType = level_10;
    }
    else
    {
        eDataType = GDT_UInt16;
        nFileType = level_15;
    }

    poDSIn->nFileType = nFileType;

    /* Read number of range/azimuth lines */
    VSIFSeekL(fp, NUMBER_LINES_OFFSET, SEEK_SET);
    READ_CHAR_VAL(nRasterYSize, NUMBER_LINES_LENGTH, fp);
    VSIFSeekL(fp, SAR_DATA_RECORD_LENGTH_OFFSET, SEEK_SET);
    READ_CHAR_VAL(nRecordSize, SAR_DATA_RECORD_LENGTH_LENGTH, fp);
    const int nDenom = ((nBitsPerSample / 8) * nSamplesPerGroup);
    if (nDenom != 0)
        nRasterXSize =
            (nRecordSize - (nFileType != level_15 ? SIG_DAT_REC_OFFSET
                                                  : PROC_DAT_REC_OFFSET)) /
            nDenom;

    poDSIn->nRasterXSize = nRasterXSize;
    poDSIn->nRasterYSize = nRasterYSize;

    /* Polarization */
    switch (nBand)
    {
        case 0:
            nPolarization = hh;
            SetMetadataItem("POLARIMETRIC_INTERP", "HH");
            break;
        case 1:
            nPolarization = hv;
            SetMetadataItem("POLARIMETRIC_INTERP", "HV");
            break;
        case 2:
            nPolarization = vh;
            SetMetadataItem("POLARIMETRIC_INTERP", "VH");
            break;
        case 3:
            nPolarization = vv;
            SetMetadataItem("POLARIMETRIC_INTERP", "VV");
            break;
            // TODO: What about the default?
    }

    /* size of block we can read */
    nBlockXSize = nRasterXSize;
    nBlockYSize = 1;

    /* set the file pointer to the first SAR data record */
    VSIFSeekL(fp, IMAGE_OPT_DESC_LENGTH, SEEK_SET);
}

/************************************************************************/
/*                        ~PALSARJaxaRasterBand()                       */
/************************************************************************/

PALSARJaxaRasterBand::~PALSARJaxaRasterBand()
{
    if (fp)
        VSIFCloseL(fp);
}

/************************************************************************/
/*                             IReadBlock()                             */
/************************************************************************/

CPLErr PALSARJaxaRasterBand::IReadBlock(CPL_UNUSED int nBlockXOff,
                                        int nBlockYOff, void *pImage)
{
    int nNumBytes = 0;
    if (nFileType == level_11)
    {
        nNumBytes = 8;
    }
    else
    {
        nNumBytes = 2;
    }

    int nOffset =
        IMAGE_OPT_DESC_LENGTH + ((nBlockYOff - 1) * nRecordSize) +
        (nFileType == level_11 ? SIG_DAT_REC_OFFSET : PROC_DAT_REC_OFFSET);

    VSIFSeekL(fp, nOffset, SEEK_SET);
    VSIFReadL(pImage, nNumBytes, nRasterXSize, fp);

#ifdef CPL_LSB
    if (nFileType == level_11)
        GDALSwapWords(pImage, 4, nBlockXSize * 2, 4);
    else
        GDALSwapWords(pImage, 2, nBlockXSize, 2);
#endif

    return CE_None;
}

/************************************************************************/
/* ==================================================================== */
/*                      PALSARJaxaDataset                               */
/* ==================================================================== */
/************************************************************************/

/************************************************************************/
/*                          ReadMetadata()                              */
/************************************************************************/

int PALSARJaxaDataset::GetGCPCount()
{
    return nGCPCount;
}

/************************************************************************/
/*                             GetGCPs()                                */
/************************************************************************/

const GDAL_GCP *PALSARJaxaDataset::GetGCPs()
{
    return pasGCPList;
}

/************************************************************************/
/*                            ReadMetadata()                            */
/************************************************************************/

void PALSARJaxaDataset::ReadMetadata(PALSARJaxaDataset *poDS, VSILFILE *fp)
{
    /* seek to the end of the leader file descriptor */
    VSIFSeekL(fp, LEADER_FILE_DESCRIPTOR_LENGTH, SEEK_SET);
    if (poDS->nFileType == level_10)
    {
        poDS->SetMetadataItem("PRODUCT_LEVEL", "1.0");
        poDS->SetMetadataItem("AZIMUTH_LOOKS", "1.0");
    }
    else if (poDS->nFileType == level_11)
    {
        poDS->SetMetadataItem("PRODUCT_LEVEL", "1.1");
        poDS->SetMetadataItem("AZIMUTH_LOOKS", "1.0");
    }
    else
    {
        poDS->SetMetadataItem("PRODUCT_LEVEL", "1.5");
        /* extract equivalent number of looks */
        VSIFSeekL(
            fp, LEADER_FILE_DESCRIPTOR_LENGTH + EFFECTIVE_LOOKS_AZIMUTH_OFFSET,
            SEEK_SET);
        char szENL[17];
        double dfENL;
        READ_CHAR_FLOAT(dfENL, 16, fp);
        snprintf(szENL, sizeof(szENL), "%-16.1f", dfENL);
        poDS->SetMetadataItem("AZIMUTH_LOOKS", szENL);

        /* extract pixel spacings */
        VSIFSeekL(fp,
                  LEADER_FILE_DESCRIPTOR_LENGTH + DATA_SET_SUMMARY_LENGTH +
                      PIXEL_SPACING_OFFSET,
                  SEEK_SET);
        double dfPixelSpacing;
        double dfLineSpacing;
        char szPixelSpacing[33];
        char szLineSpacing[33];
        READ_CHAR_FLOAT(dfPixelSpacing, 16, fp);
        READ_CHAR_FLOAT(dfLineSpacing, 16, fp);
        snprintf(szPixelSpacing, sizeof(szPixelSpacing), "%-32.1f",
                 dfPixelSpacing);
        snprintf(szLineSpacing, sizeof(szLineSpacing), "%-32.1f",
                 dfLineSpacing);
        poDS->SetMetadataItem("PIXEL_SPACING", szPixelSpacing);
        poDS->SetMetadataItem("LINE_SPACING", szPixelSpacing);

        /* Alphanumeric projection name */
        VSIFSeekL(fp,
                  LEADER_FILE_DESCRIPTOR_LENGTH + DATA_SET_SUMMARY_LENGTH +
                      ALPHANUMERIC_PROJECTION_NAME_OFFSET,
                  SEEK_SET);
        char szProjName[33];
        READ_STRING(szProjName, 32, fp);
        poDS->SetMetadataItem("PROJECTION_NAME", szProjName);

        /* Extract corner GCPs */
        poDS->nGCPCount = 4;
        poDS->pasGCPList =
            (GDAL_GCP *)CPLCalloc(sizeof(GDAL_GCP), poDS->nGCPCount);
        GDALInitGCPs(poDS->nGCPCount, poDS->pasGCPList);

        /* setup the GCPs */
        int i;
        for (i = 0; i < poDS->nGCPCount; i++)
        {
            char szID[30];
            snprintf(szID, sizeof(szID), "%d", i + 1);
            CPLFree(poDS->pasGCPList[i].pszId);
            poDS->pasGCPList[i].pszId = CPLStrdup(szID);
            poDS->pasGCPList[i].dfGCPZ = 0.0;
        }

        double dfTemp = 0.0;
        /* seek to start of GCPs */
        VSIFSeekL(fp,
                  LEADER_FILE_DESCRIPTOR_LENGTH + DATA_SET_SUMMARY_LENGTH +
                      TOP_LEFT_LAT_OFFSET,
                  SEEK_SET);

        /* top-left GCP */
        READ_CHAR_FLOAT(dfTemp, 16, fp);
        poDS->pasGCPList[0].dfGCPY = dfTemp;
        READ_CHAR_FLOAT(dfTemp, 16, fp);
        poDS->pasGCPList[0].dfGCPX = dfTemp;
        poDS->pasGCPList[0].dfGCPLine = 0.5;
        poDS->pasGCPList[0].dfGCPPixel = 0.5;

        /* top right GCP */
        READ_CHAR_FLOAT(dfTemp, 16, fp);
        poDS->pasGCPList[1].dfGCPY = dfTemp;
        READ_CHAR_FLOAT(dfTemp, 16, fp);
        poDS->pasGCPList[1].dfGCPX = dfTemp;
        poDS->pasGCPList[1].dfGCPLine = 0.5;
        poDS->pasGCPList[1].dfGCPPixel = poDS->nRasterYSize - 0.5;

        /* bottom right GCP */
        READ_CHAR_FLOAT(dfTemp, 16, fp);
        poDS->pasGCPList[2].dfGCPY = dfTemp;
        READ_CHAR_FLOAT(dfTemp, 16, fp);
        poDS->pasGCPList[2].dfGCPX = dfTemp;
        poDS->pasGCPList[2].dfGCPLine = poDS->nRasterYSize - 0.5;
        poDS->pasGCPList[2].dfGCPPixel = poDS->nRasterYSize - 0.5;

        /* bottom left GCP */
        READ_CHAR_FLOAT(dfTemp, 16, fp);
        poDS->pasGCPList[3].dfGCPY = dfTemp;
        READ_CHAR_FLOAT(dfTemp, 16, fp);
        poDS->pasGCPList[3].dfGCPX = dfTemp;
        poDS->pasGCPList[3].dfGCPLine = poDS->nRasterYSize - 0.5;
        poDS->pasGCPList[3].dfGCPPixel = 0.5;
    }

    /* some generic metadata items */
    poDS->SetMetadataItem("SENSOR_BAND", "L"); /* PALSAR is L-band */
    poDS->SetMetadataItem("RANGE_LOOKS", "1.0");

    /* Check if this is a PolSAR dataset */
    if (poDS->GetRasterCount() == 4)
    {
        /* PALSAR data is only available from JAXA in Scattering Matrix form */
        poDS->SetMetadataItem("MATRIX_REPRESENTATION", "SCATTERING");
    }
}

/************************************************************************/
/*                              Identify()                              */
/************************************************************************/

static void ReadWord(VSILFILE *fp, int *pVal)
{
    READ_WORD(fp, *pVal);
}

static void ReadByte(VSILFILE *fp, int *pVal)
{
    READ_BYTE(fp, *pVal);
}

int PALSARJaxaDataset::Identify(GDALOpenInfo *poOpenInfo)
{
    if (poOpenInfo->nHeaderBytes < 360 || poOpenInfo->fpL == nullptr)
        return 0;

    /* First, check that this is a PALSAR image indeed */
    if (!STARTS_WITH_CI((char *)(poOpenInfo->pabyHeader + 60), "AL"))
    {
        return 0;
    }
    const std::string osBasename = CPLGetBasenameSafe(poOpenInfo->pszFilename);
    if (osBasename.size() < 9 ||
        !STARTS_WITH_CI(osBasename.c_str() + 4, "ALPSR"))
    {
        return 0;
    }

    /* Check that this is a volume directory file */
    int nRecordSeq = 0;
    int nRecordSubtype = 0;
    int nRecordType = 0;
    int nSecondSubtype = 0;
    int nThirdSubtype = 0;
    int nLengthRecord = 0;

    VSIFSeekL(poOpenInfo->fpL, 0, SEEK_SET);

    ReadWord(poOpenInfo->fpL, &nRecordSeq);
    ReadByte(poOpenInfo->fpL, &nRecordSubtype);
    ReadByte(poOpenInfo->fpL, &nRecordType);
    ReadByte(poOpenInfo->fpL, &nSecondSubtype);
    ReadByte(poOpenInfo->fpL, &nThirdSubtype);
    ReadWord(poOpenInfo->fpL, &nLengthRecord);

    VSIFSeekL(poOpenInfo->fpL, 0, SEEK_SET);

    /* Check that we have the right record */
    if (nRecordSeq == 1 && nRecordSubtype == 192 && nRecordType == 192 &&
        nSecondSubtype == 18 && nThirdSubtype == 18 && nLengthRecord == 360)
    {
        return 1;
    }

    return 0;
}

/************************************************************************/
/*                                Open()                                */
/************************************************************************/
GDALDataset *PALSARJaxaDataset::Open(GDALOpenInfo *poOpenInfo)
{
    /* Check that this actually is a JAXA PALSAR product */
    if (!PALSARJaxaDataset::Identify(poOpenInfo))
        return nullptr;

    /* -------------------------------------------------------------------- */
    /*      Confirm the requested access is supported.                      */
    /* -------------------------------------------------------------------- */
    if (poOpenInfo->eAccess == GA_Update)
    {
        ReportUpdateNotSupportedByDriver("JAXAPALSAR");
        return nullptr;
    }

    PALSARJaxaDataset *poDS = new PALSARJaxaDataset();

    /* Get the suffix of the filename, we'll need this */
    char *pszSuffix =
        VSIStrdup((char *)(CPLGetFilename(poOpenInfo->pszFilename) + 3));

    /* Try to read each of the polarizations */
    const size_t nImgFileLen =
        CPLGetDirnameSafe(poOpenInfo->pszFilename).size() + strlen(pszSuffix) +
        8;
    char *pszImgFile = (char *)CPLMalloc(nImgFileLen);

    int nBandNum = 1;

    /* HH */
    snprintf(pszImgFile, nImgFileLen, "%s%sIMG-HH%s",
             CPLGetDirnameSafe(poOpenInfo->pszFilename).c_str(), SEP_STRING,
             pszSuffix);
    VSILFILE *fpHH = VSIFOpenL(pszImgFile, "rb");
    if (fpHH != nullptr)
    {
        poDS->SetBand(nBandNum, new PALSARJaxaRasterBand(poDS, 0, fpHH));
        nBandNum++;
    }

    /* HV */
    snprintf(pszImgFile, nImgFileLen, "%s%sIMG-HV%s",
             CPLGetDirnameSafe(poOpenInfo->pszFilename).c_str(), SEP_STRING,
             pszSuffix);
    VSILFILE *fpHV = VSIFOpenL(pszImgFile, "rb");
    if (fpHV != nullptr)
    {
        poDS->SetBand(nBandNum, new PALSARJaxaRasterBand(poDS, 1, fpHV));
        nBandNum++;
    }

    /* VH */
    snprintf(pszImgFile, nImgFileLen, "%s%sIMG-VH%s",
             CPLGetDirnameSafe(poOpenInfo->pszFilename).c_str(), SEP_STRING,
             pszSuffix);
    VSILFILE *fpVH = VSIFOpenL(pszImgFile, "rb");
    if (fpVH != nullptr)
    {
        poDS->SetBand(nBandNum, new PALSARJaxaRasterBand(poDS, 2, fpVH));
        nBandNum++;
    }

    /* VV */
    snprintf(pszImgFile, nImgFileLen, "%s%sIMG-VV%s",
             CPLGetDirnameSafe(poOpenInfo->pszFilename).c_str(), SEP_STRING,
             pszSuffix);
    VSILFILE *fpVV = VSIFOpenL(pszImgFile, "rb");
    if (fpVV != nullptr)
    {
        poDS->SetBand(nBandNum, new PALSARJaxaRasterBand(poDS, 3, fpVV));
        /* nBandNum++; */
    }

    VSIFree(pszImgFile);

    /* did we get at least one band? */
    if (fpVV == nullptr && fpVH == nullptr && fpHV == nullptr &&
        fpHH == nullptr)
    {
        CPLError(
            CE_Failure, CPLE_AppDefined,
            "Unable to find any image data. Aborting opening as PALSAR image.");
        delete poDS;
        VSIFree(pszSuffix);
        return nullptr;
    }

    /* Level 1.0 products are not supported */
    if (poDS->nFileType == level_10)
    {
        CPLError(CE_Failure, CPLE_AppDefined,
                 "ALOS PALSAR Level 1.0 products are not supported. Aborting "
                 "opening as PALSAR image.");
        delete poDS;
        VSIFree(pszSuffix);
        return nullptr;
    }

    /* read metadata from Leader file. */
    const size_t nLeaderFilenameLen =
        strlen(CPLGetDirnameSafe(poOpenInfo->pszFilename).c_str()) +
        strlen(pszSuffix) + 5;
    char *pszLeaderFilename = (char *)CPLMalloc(nLeaderFilenameLen);
    snprintf(pszLeaderFilename, nLeaderFilenameLen, "%s%sLED%s",
             CPLGetDirnameSafe(poOpenInfo->pszFilename).c_str(), SEP_STRING,
             pszSuffix);

    VSILFILE *fpLeader = VSIFOpenL(pszLeaderFilename, "rb");
    /* check if the leader is actually present */
    if (fpLeader != nullptr)
    {
        ReadMetadata(poDS, fpLeader);
        VSIFCloseL(fpLeader);
    }

    VSIFree(pszLeaderFilename);

    VSIFree(pszSuffix);

    /* -------------------------------------------------------------------- */
    /*      Initialize any PAM information.                                 */
    /* -------------------------------------------------------------------- */
    poDS->SetDescription(poOpenInfo->pszFilename);
    poDS->TryLoadXML();

    /* -------------------------------------------------------------------- */
    /*      Check for overviews.                                            */
    /* -------------------------------------------------------------------- */
    poDS->oOvManager.Initialize(poDS, poOpenInfo->pszFilename);

    return poDS;
}

/************************************************************************/
/*                      GDALRegister_PALSARJaxa()                       */
/************************************************************************/

void GDALRegister_PALSARJaxa()

{
    if (GDALGetDriverByName("JAXAPALSAR") != nullptr)
        return;

    GDALDriver *poDriver = new GDALDriver();

    poDriver->SetDescription("JAXAPALSAR");
    poDriver->SetMetadataItem(GDAL_DCAP_RASTER, "YES");
    poDriver->SetMetadataItem(GDAL_DMD_LONGNAME,
                              "JAXA PALSAR Product Reader (Level 1.1/1.5)");
    poDriver->SetMetadataItem(GDAL_DMD_HELPTOPIC, "drivers/raster/palsar.html");

    poDriver->pfnOpen = PALSARJaxaDataset::Open;
    poDriver->pfnIdentify = PALSARJaxaDataset::Identify;
    poDriver->SetMetadataItem(GDAL_DCAP_VIRTUALIO, "YES");

    GetGDALDriverManager()->RegisterDriver(poDriver);
}
