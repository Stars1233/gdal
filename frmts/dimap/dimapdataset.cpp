/******************************************************************************
 *
 * Project:  SPOT Dimap Driver
 * Purpose:  Implementation of SPOT Dimap driver.
 * Author:   Frank Warmerdam, warmerdam@pobox.com
 *
 * Docs: http://www.spotimage.fr/dimap/spec/documentation/refdoc.htm
 *
 ******************************************************************************
 * Copyright (c) 2007, Frank Warmerdam <warmerdam@pobox.com>
 * Copyright (c) 2008-2013, Even Rouault <even dot rouault at spatialys.com>
 *
 * SPDX-License-Identifier: MIT
 ****************************************************************************/

#include "cpl_minixml.h"
#include "gdal_frmts.h"
#include "gdal_pam.h"
#include "ogr_spatialref.h"
#include "mdreader/reader_pleiades.h"
#include "vrtdataset.h"
#include <map>
#include <algorithm>

/************************************************************************/
/* ==================================================================== */
/*                              DIMAPDataset                            */
/* ==================================================================== */
/************************************************************************/

class DIMAPDataset final : public GDALPamDataset
{
    CPLXMLNode *psProduct{};

    CPLXMLNode *psProductDim{};    // DIMAP2, DIM_<product_id>.XML
    CPLXMLNode *psProductStrip{};  // DIMAP2, STRIP_<product_id>.XML
    CPLString osRPCFilename{};     // DIMAP2, RPC_<product_id>.XML

    VRTDataset *poVRTDS{};

    int nGCPCount{};
    GDAL_GCP *pasGCPList{};

    OGRSpatialReference m_oSRS{};
    OGRSpatialReference m_oGCPSRS{};

    int bHaveGeoTransform{};
    GDALGeoTransform m_gt{};

    CPLString osMDFilename{};
    CPLString osImageDSFilename{};
    CPLString osDIMAPFilename{};
    int nProductVersion = 1;

    char **papszXMLDimapMetadata{};

    CPL_DISALLOW_COPY_ASSIGN(DIMAPDataset)

  protected:
    int CloseDependentDatasets() override;

    int ReadImageInformation();
    int ReadImageInformation2();  // DIMAP 2.

    void SetMetadataFromXML(CPLXMLNode *psProduct,
                            const char *const apszMetadataTranslation[],
                            bool bKeysFromRoot = true);

  public:
    DIMAPDataset();
    ~DIMAPDataset() override;

    const OGRSpatialReference *GetSpatialRef() const override;
    CPLErr GetGeoTransform(GDALGeoTransform &gt) const override;
    int GetGCPCount() override;
    const OGRSpatialReference *GetGCPSpatialRef() const override;
    const GDAL_GCP *GetGCPs() override;
    char **GetMetadataDomainList() override;
    char **GetMetadata(const char *pszDomain) override;
    char **GetFileList() override;

    CPLErr IRasterIO(GDALRWFlag, int, int, int, int, void *, int, int,
                     GDALDataType, int, BANDMAP_TYPE, GSpacing, GSpacing,
                     GSpacing, GDALRasterIOExtraArg *psExtraArg) override;

    static int Identify(GDALOpenInfo *);
    static GDALDataset *Open(GDALOpenInfo *);

    CPLXMLNode *GetProduct()
    {
        return psProduct;
    }
};

/************************************************************************/
/* ==================================================================== */
/*                              DIMAPDataset                            */
/* ==================================================================== */
/************************************************************************/

/************************************************************************/
/*                             DIMAPDataset()                            */
/************************************************************************/

DIMAPDataset::DIMAPDataset()
{
    m_oSRS.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
    m_oGCPSRS.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
}

/************************************************************************/
/*                            ~DIMAPDataset()                           */
/************************************************************************/

DIMAPDataset::~DIMAPDataset()

{
    DIMAPDataset::FlushCache(true);

    CPLDestroyXMLNode(psProduct);

    if (psProductDim != nullptr && psProductDim != psProduct)
        CPLDestroyXMLNode(psProductDim);
    if (psProductStrip != nullptr)
        CPLDestroyXMLNode(psProductStrip);
    if (nGCPCount > 0)
    {
        GDALDeinitGCPs(nGCPCount, pasGCPList);
        CPLFree(pasGCPList);
    }

    CSLDestroy(papszXMLDimapMetadata);

    DIMAPDataset::CloseDependentDatasets();
}

/************************************************************************/
/*                        CloseDependentDatasets()                      */
/************************************************************************/

int DIMAPDataset::CloseDependentDatasets()
{
    int bHasDroppedRef = GDALPamDataset::CloseDependentDatasets();

    if (poVRTDS != nullptr)
    {
        delete poVRTDS;
        poVRTDS = nullptr;
        bHasDroppedRef = TRUE;
    }

    return bHasDroppedRef;
}

/************************************************************************/
/*                      GetMetadataDomainList()                         */
/************************************************************************/

char **DIMAPDataset::GetMetadataDomainList()
{
    return BuildMetadataDomainList(GDALPamDataset::GetMetadataDomainList(),
                                   TRUE, "xml:dimap", nullptr);
}

/************************************************************************/
/*                            GetMetadata()                             */
/*                                                                      */
/*      We implement special support for fetching the full product      */
/*      metadata as xml.                                                */
/************************************************************************/

char **DIMAPDataset::GetMetadata(const char *pszDomain)

{
    if (pszDomain && EQUAL(pszDomain, "xml:dimap"))
    {
        if (papszXMLDimapMetadata == nullptr)
        {
            papszXMLDimapMetadata =
                static_cast<char **>(CPLCalloc(sizeof(char *), 2));
            papszXMLDimapMetadata[0] = CPLSerializeXMLTree(psProduct);
        }
        return papszXMLDimapMetadata;
    }

    return GDALPamDataset::GetMetadata(pszDomain);
}

/************************************************************************/
/*                          GetSpatialRef()                             */
/************************************************************************/

const OGRSpatialReference *DIMAPDataset::GetSpatialRef() const

{
    if (!m_oSRS.IsEmpty())
        return &m_oSRS;

    return GDALPamDataset::GetSpatialRef();
}

/************************************************************************/
/*                          GetGeoTransform()                           */
/************************************************************************/

CPLErr DIMAPDataset::GetGeoTransform(GDALGeoTransform &gt) const

{
    if (bHaveGeoTransform)
    {
        gt = m_gt;
        return CE_None;
    }

    return GDALPamDataset::GetGeoTransform(gt);
}

/************************************************************************/
/*                            GetFileList()                             */
/************************************************************************/

char **DIMAPDataset::GetFileList()

{
    char **papszFileList = GDALPamDataset::GetFileList();
    char **papszImageFiles = poVRTDS->GetFileList();

    papszFileList = CSLInsertStrings(papszFileList, -1, papszImageFiles);

    CSLDestroy(papszImageFiles);

    return papszFileList;
}

/************************************************************************/
/* ==================================================================== */
/*                            DIMAPRasterBand                           */
/* ==================================================================== */
/************************************************************************/

class DIMAPRasterBand final : public GDALPamRasterBand
{
    friend class DIMAPDataset;

    VRTSourcedRasterBand *poVRTBand;

    CPL_DISALLOW_COPY_ASSIGN(DIMAPRasterBand)

  public:
    DIMAPRasterBand(DIMAPDataset *, int, VRTSourcedRasterBand *);

    ~DIMAPRasterBand() override
    {
    }

    CPLErr IReadBlock(int, int, void *) override;
    CPLErr IRasterIO(GDALRWFlag, int, int, int, int, void *, int, int,
                     GDALDataType, GSpacing nPixelSpace, GSpacing nLineSpace,
                     GDALRasterIOExtraArg *psExtraArg) override;
    int GetOverviewCount() override;
    GDALRasterBand *GetOverview(int) override;
    CPLErr ComputeRasterMinMax(int bApproxOK, double adfMinMax[2]) override;
    CPLErr ComputeStatistics(int bApproxOK, double *pdfMin, double *pdfMax,
                             double *pdfMean, double *pdfStdDev,
                             GDALProgressFunc, void *pProgressData) override;

    CPLErr GetHistogram(double dfMin, double dfMax, int nBuckets,
                        GUIntBig *panHistogram, int bIncludeOutOfRange,
                        int bApproxOK, GDALProgressFunc,
                        void *pProgressData) override;
};

/************************************************************************/
/*                          DIMAPRasterBand()                           */
/************************************************************************/

DIMAPRasterBand::DIMAPRasterBand(DIMAPDataset *poDIMAPDS, int nBandIn,
                                 VRTSourcedRasterBand *poVRTBandIn)
    : poVRTBand(poVRTBandIn)
{
    poDS = poDIMAPDS;
    nBand = nBandIn;
    eDataType = poVRTBandIn->GetRasterDataType();

    poVRTBandIn->GetBlockSize(&nBlockXSize, &nBlockYSize);
}

/************************************************************************/
/*                             IReadBlock()                             */
/************************************************************************/

CPLErr DIMAPRasterBand::IReadBlock(int iBlockX, int iBlockY, void *pBuffer)

{
    return poVRTBand->ReadBlock(iBlockX, iBlockY, pBuffer);
}

/************************************************************************/
/*                             IRasterIO()                              */
/************************************************************************/

CPLErr DIMAPRasterBand::IRasterIO(GDALRWFlag eRWFlag, int nXOff, int nYOff,
                                  int nXSize, int nYSize, void *pData,
                                  int nBufXSize, int nBufYSize,
                                  GDALDataType eBufType, GSpacing nPixelSpace,
                                  GSpacing nLineSpace,
                                  GDALRasterIOExtraArg *psExtraArg)

{
    if (GDALPamRasterBand::GetOverviewCount() > 0)
    {
        return GDALPamRasterBand::IRasterIO(
            eRWFlag, nXOff, nYOff, nXSize, nYSize, pData, nBufXSize, nBufYSize,
            eBufType, nPixelSpace, nLineSpace, psExtraArg);
    }

    // If not exist DIMAP overviews, try to use band source overviews.
    return poVRTBand->IRasterIO(eRWFlag, nXOff, nYOff, nXSize, nYSize, pData,
                                nBufXSize, nBufYSize, eBufType, nPixelSpace,
                                nLineSpace, psExtraArg);
}

/************************************************************************/
/*                             IRasterIO()                              */
/************************************************************************/

CPLErr DIMAPDataset::IRasterIO(GDALRWFlag eRWFlag, int nXOff, int nYOff,
                               int nXSize, int nYSize, void *pData,
                               int nBufXSize, int nBufYSize,
                               GDALDataType eBufType, int nBandCount,
                               BANDMAP_TYPE panBandMap, GSpacing nPixelSpace,
                               GSpacing nLineSpace, GSpacing nBandSpace,
                               GDALRasterIOExtraArg *psExtraArg)

{
    if (cpl::down_cast<DIMAPRasterBand *>(papoBands[0])
            ->GDALPamRasterBand::GetOverviewCount() > 0)
    {
        return GDALPamDataset::IRasterIO(eRWFlag, nXOff, nYOff, nXSize, nYSize,
                                         pData, nBufXSize, nBufYSize, eBufType,
                                         nBandCount, panBandMap, nPixelSpace,
                                         nLineSpace, nBandSpace, psExtraArg);
    }

    return poVRTDS->IRasterIO(eRWFlag, nXOff, nYOff, nXSize, nYSize, pData,
                              nBufXSize, nBufYSize, eBufType, nBandCount,
                              panBandMap, nPixelSpace, nLineSpace, nBandSpace,
                              psExtraArg);
}

/************************************************************************/
/*                          GetOverviewCount()                          */
/************************************************************************/

int DIMAPRasterBand::GetOverviewCount()
{
    if (GDALPamRasterBand::GetOverviewCount() > 0)
    {
        return GDALPamRasterBand::GetOverviewCount();
    }
    return poVRTBand->GetOverviewCount();
}

/************************************************************************/
/*                             GetOverview()                            */
/************************************************************************/

GDALRasterBand *DIMAPRasterBand::GetOverview(int iOvr)
{
    if (GDALPamRasterBand::GetOverviewCount() > 0)
    {
        return GDALPamRasterBand::GetOverview(iOvr);
    }
    return poVRTBand->GetOverview(iOvr);
}

/************************************************************************/
/*                         ComputeRasterMinMax()                        */
/************************************************************************/

CPLErr DIMAPRasterBand::ComputeRasterMinMax(int bApproxOK, double adfMinMax[2])
{
    if (GDALPamRasterBand::GetOverviewCount() > 0)
    {
        return GDALPamRasterBand::ComputeRasterMinMax(bApproxOK, adfMinMax);
    }
    return poVRTBand->ComputeRasterMinMax(bApproxOK, adfMinMax);
}

/************************************************************************/
/*                          ComputeStatistics()                         */
/************************************************************************/

CPLErr DIMAPRasterBand::ComputeStatistics(int bApproxOK, double *pdfMin,
                                          double *pdfMax, double *pdfMean,
                                          double *pdfStdDev,
                                          GDALProgressFunc pfnProgress,
                                          void *pProgressData)
{
    if (GDALPamRasterBand::GetOverviewCount() > 0)
    {
        return GDALPamRasterBand::ComputeStatistics(bApproxOK, pdfMin, pdfMax,
                                                    pdfMean, pdfStdDev,
                                                    pfnProgress, pProgressData);
    }
    return poVRTBand->ComputeStatistics(bApproxOK, pdfMin, pdfMax, pdfMean,
                                        pdfStdDev, pfnProgress, pProgressData);
}

/************************************************************************/
/*                            GetHistogram()                            */
/************************************************************************/

CPLErr DIMAPRasterBand::GetHistogram(double dfMin, double dfMax, int nBuckets,
                                     GUIntBig *panHistogram,
                                     int bIncludeOutOfRange, int bApproxOK,
                                     GDALProgressFunc pfnProgress,
                                     void *pProgressData)
{
    if (GDALPamRasterBand::GetOverviewCount() > 0)
    {
        return GDALPamRasterBand::GetHistogram(
            dfMin, dfMax, nBuckets, panHistogram, bIncludeOutOfRange, bApproxOK,
            pfnProgress, pProgressData);
    }
    return poVRTBand->GetHistogram(dfMin, dfMax, nBuckets, panHistogram,
                                   bIncludeOutOfRange, bApproxOK, pfnProgress,
                                   pProgressData);
}

/************************************************************************/
/*                              Identify()                              */
/************************************************************************/

int DIMAPDataset::Identify(GDALOpenInfo *poOpenInfo)

{
    if (STARTS_WITH(poOpenInfo->pszFilename, "DIMAP:"))
        return true;

    if (poOpenInfo->nHeaderBytes >= 100)
    {
        if ((strstr(reinterpret_cast<char *>(poOpenInfo->pabyHeader),
                    "<Dimap_Document") == nullptr) &&
            (strstr(reinterpret_cast<char *>(poOpenInfo->pabyHeader),
                    "<PHR_DIMAP_Document") == nullptr))
            return FALSE;

        return TRUE;
    }
    else if (poOpenInfo->bIsDirectory)
    {
        // DIMAP file.
        CPLString osMDFilename = CPLFormCIFilenameSafe(poOpenInfo->pszFilename,
                                                       "METADATA.DIM", nullptr);

        VSIStatBufL sStat;
        if (VSIStatL(osMDFilename, &sStat) == 0)
        {
            // Make sure this is really a Dimap format.
            GDALOpenInfo oOpenInfo(osMDFilename, GA_ReadOnly, nullptr);
            if (oOpenInfo.nHeaderBytes >= 100)
            {
                if (strstr(reinterpret_cast<char *>(oOpenInfo.pabyHeader),
                           "<Dimap_Document") == nullptr)
                    return FALSE;

                return TRUE;
            }
        }
        else
        {
            // DIMAP 2 file.
            osMDFilename = CPLFormCIFilenameSafe(poOpenInfo->pszFilename,
                                                 "VOL_PHR.XML", nullptr);

            if (VSIStatL(osMDFilename, &sStat) == 0)
                return TRUE;

            // DIMAP VHR2020 file.
            osMDFilename = CPLFormCIFilenameSafe(poOpenInfo->pszFilename,
                                                 "VOL_PNEO.XML", nullptr);

            if (VSIStatL(osMDFilename, &sStat) == 0)
                return TRUE;

            return FALSE;
        }
    }

    return FALSE;
}

/************************************************************************/
/*                                Open()                                */
/************************************************************************/

GDALDataset *DIMAPDataset::Open(GDALOpenInfo *poOpenInfo)

{
    if (!Identify(poOpenInfo))
        return nullptr;

    /* -------------------------------------------------------------------- */
    /*      Confirm the requested access is supported.                      */
    /* -------------------------------------------------------------------- */
    if (poOpenInfo->eAccess == GA_Update)
    {
        ReportUpdateNotSupportedByDriver("DIMAP");
        return nullptr;
    }
    /* -------------------------------------------------------------------- */
    /*      Get the metadata filename.                                      */
    /* -------------------------------------------------------------------- */
    CPLString osFilename;
    CPLString osSelectedSubdataset;

    if (STARTS_WITH(poOpenInfo->pszFilename, "DIMAP:"))
    {
        CPLStringList aosTokens(CSLTokenizeString2(poOpenInfo->pszFilename, ":",
                                                   CSLT_HONOURSTRINGS));
        if (aosTokens.size() != 3)
            return nullptr;

        osFilename = aosTokens[1];
        osSelectedSubdataset = aosTokens[2];
    }
    else
    {
        osFilename = poOpenInfo->pszFilename;
    }

    VSIStatBufL sStat;
    std::string osMDFilename(osFilename);
    if (VSIStatL(osFilename.c_str(), &sStat) == 0 && VSI_ISDIR(sStat.st_mode))
    {
        osMDFilename =
            CPLFormCIFilenameSafe(osFilename, "METADATA.DIM", nullptr);

        /* DIMAP2 */
        if (VSIStatL(osMDFilename.c_str(), &sStat) != 0)
        {
            osMDFilename =
                CPLFormCIFilenameSafe(osFilename, "VOL_PHR.XML", nullptr);
            if (VSIStatL(osMDFilename.c_str(), &sStat) != 0)
            {
                // DIMAP VHR2020 file.
                osMDFilename =
                    CPLFormCIFilenameSafe(osFilename, "VOL_PNEO.XML", nullptr);
            }
        }
    }

    /* -------------------------------------------------------------------- */
    /*      Ingest the xml file.                                            */
    /* -------------------------------------------------------------------- */
    CPLXMLNode *psProduct = CPLParseXMLFile(osMDFilename.c_str());
    if (psProduct == nullptr)
        return nullptr;

    CPLXMLNode *psDoc = CPLGetXMLNode(psProduct, "=Dimap_Document");
    if (!psDoc)
        psDoc = CPLGetXMLNode(psProduct, "=PHR_DIMAP_Document");

    // We check the for the tag Metadata_Identification.METADATA_FORMAT.
    // The metadata will be set to 2.0 for DIMAP2.
    double dfMetadataFormatVersion = CPLAtof(CPLGetXMLValue(
        CPLGetXMLNode(psDoc, "Metadata_Identification.METADATA_FORMAT"),
        "version", "1"));

    const int nProductVersion = dfMetadataFormatVersion >= 2.0 ? 2 : 1;

    std::string osImageDSFilename;
    std::string osDIMAPFilename;
    std::string osRPCFilename;
    CPLXMLNode *psProductDim = nullptr;
    CPLXMLNode *psProductStrip = nullptr;

    CPLStringList aosSubdatasets;

    // Check needed information for the DIMAP format.
    if (nProductVersion == 1)
    {
        CPLXMLNode *psImageAttributes =
            CPLGetXMLNode(psDoc, "Raster_Dimensions");
        if (psImageAttributes == nullptr)
        {
            CPLError(CE_Failure, CPLE_OpenFailed,
                     "Failed to find <Raster_Dimensions> in document.");
            CPLDestroyXMLNode(psProduct);
            return nullptr;
        }
    }
    else  // DIMAP2.
    {
        // Verify if the opened file is not already a product dimap
        if (CPLGetXMLNode(psDoc, "Raster_Data"))
        {
            psProductDim = psProduct;
            osDIMAPFilename = osMDFilename;
        }
        else
        {
            // Verify the presence of the DIMAP product file.
            CPLXMLNode *psDatasetComponents =
                CPLGetXMLNode(psDoc, "Dataset_Content.Dataset_Components");

            if (psDatasetComponents == nullptr)
            {
                CPLError(CE_Failure, CPLE_OpenFailed,
                         "Failed to find <Dataset_Components> in document.");
                CPLDestroyXMLNode(psProduct);
                return nullptr;
            }

            for (CPLXMLNode *psDatasetComponent = psDatasetComponents->psChild;
                 psDatasetComponent != nullptr;
                 psDatasetComponent = psDatasetComponent->psNext)
            {
                const char *pszComponentType =
                    CPLGetXMLValue(psDatasetComponent, "COMPONENT_TYPE", "");
                if (strcmp(pszComponentType, "DIMAP") == 0)
                {
                    // DIMAP product found.
                    const char *pszHref = CPLGetXMLValue(
                        psDatasetComponent, "COMPONENT_PATH.href", "");
                    const CPLString osComponentTitle(CPLGetXMLValue(
                        psDatasetComponent, "COMPONENT_TITLE", ""));
                    const CPLString osComponentTitleLaundered(
                        CPLString(osComponentTitle).replaceAll(' ', '_'));

                    if (strlen(pszHref) > 0 && osDIMAPFilename.empty() &&
                        (osSelectedSubdataset.empty() ||
                         osSelectedSubdataset == osComponentTitleLaundered))
                    {
                        if (poOpenInfo->bIsDirectory)
                        {
                            osDIMAPFilename = CPLFormCIFilenameSafe(
                                poOpenInfo->pszFilename, pszHref, nullptr);
                        }
                        else
                        {
                            CPLString osPath =
                                CPLGetPathSafe(osMDFilename.c_str());
                            osDIMAPFilename =
                                CPLFormFilenameSafe(osPath, pszHref, nullptr);
                        }

                        // Data file might be specified there.
                        const char *pszDataFileHref = CPLGetXMLValue(
                            psDatasetComponent,
                            "Data_Files.Data_File.DATA_FILE_PATH.href", "");

                        if (strlen(pszDataFileHref) > 0)
                        {
                            CPLString osPath =
                                CPLGetPathSafe(osMDFilename.c_str());
                            osImageDSFilename = CPLFormFilenameSafe(
                                osPath, pszDataFileHref, nullptr);
                        }
                    }

                    const int iIdx =
                        static_cast<int>(aosSubdatasets.size() / 2 + 1);
                    aosSubdatasets.SetNameValue(
                        CPLSPrintf("SUBDATASET_%d_NAME", iIdx),
                        CPLSPrintf("DIMAP:\"%s\":%s", poOpenInfo->pszFilename,
                                   osComponentTitleLaundered.c_str()));
                    aosSubdatasets.SetNameValue(
                        CPLSPrintf("SUBDATASET_%d_DESC", iIdx),
                        CPLSPrintf("Component %s", osComponentTitle.c_str()));
                }
            }

            psProductDim = CPLParseXMLFile(osDIMAPFilename.c_str());
            if (psProductDim == nullptr)
            {
                CPLDestroyXMLNode(psProduct);
                return nullptr;
            }
        }

        // We need the {STRIP|RPC}_<product_id>.XML file for a few metadata.
        CPLXMLNode *psDocDim = CPLGetXMLNode(psProductDim, "=Dimap_Document");
        if (!psDocDim)
            psDocDim = CPLGetXMLNode(psProductDim, "=PHR_DIMAP_Document");

        CPLXMLNode *psDatasetSources =
            CPLGetXMLNode(psDocDim, "Dataset_Sources");
        if (psDatasetSources != nullptr)
        {
            CPLString osSTRIPFilename;

            for (CPLXMLNode *psDatasetSource = psDatasetSources->psChild;
                 psDatasetSource != nullptr;
                 psDatasetSource = psDatasetSource->psNext)
            {
                const char *pszSourceType =
                    CPLGetXMLValue(psDatasetSource, "SOURCE_TYPE", "");
                if (strcmp(pszSourceType, "Strip_Source") == 0)
                {
                    const char *pszHref = CPLGetXMLValue(
                        psDatasetSource, "Component.COMPONENT_PATH.href", "");

                    if (strlen(pszHref) > 0)  // STRIP product found.
                    {
                        CPLString osPath =
                            CPLGetPathSafe(osDIMAPFilename.c_str());
                        osSTRIPFilename =
                            CPLFormCIFilenameSafe(osPath, pszHref, nullptr);
                        if (VSIStatL(osSTRIPFilename, &sStat) == 0)
                        {
                            psProductStrip = CPLParseXMLFile(osSTRIPFilename);
                            break;
                        }
                    }
                }
            }
        }

        CPLXMLNode *psDatasetRFMComponents = CPLGetXMLNode(
            psDocDim, "Geoposition.Geoposition_Models.Rational_Function_Model");
        if (psDatasetRFMComponents != nullptr)
        {
            for (CPLXMLNode *psDatasetRFMComponent =
                     psDatasetRFMComponents->psChild;
                 psDatasetRFMComponent != nullptr;
                 psDatasetRFMComponent = psDatasetRFMComponent->psNext)
            {
                const char *pszComponentTitle = CPLGetXMLValue(
                    psDatasetRFMComponent, "COMPONENT_TITLE", "");
                if (strcmp(pszComponentTitle, "RPC Model") == 0)
                {
                    const char *pszHref = CPLGetXMLValue(
                        psDatasetRFMComponent, "COMPONENT_PATH.href", "");

                    if (strlen(pszHref) > 0)  // RPC product found.
                    {
                        CPLString osPath =
                            CPLGetPathSafe(osDIMAPFilename.c_str());
                        osRPCFilename =
                            CPLFormCIFilenameSafe(osPath, pszHref, nullptr);

                        break;
                    }
                }
            }
        }
    }

    /* -------------------------------------------------------------------- */
    /*      Create the dataset.                                             */
    /* -------------------------------------------------------------------- */
    DIMAPDataset *poDS = new DIMAPDataset();

    if (osSelectedSubdataset.empty() && aosSubdatasets.size() > 2)
    {
        poDS->GDALDataset::SetMetadata(aosSubdatasets.List(), "SUBDATASETS");
    }
    poDS->psProduct = psProduct;
    poDS->psProductDim = psProductDim;
    poDS->psProductStrip = psProductStrip;
    poDS->osRPCFilename = std::move(osRPCFilename);
    poDS->nProductVersion = nProductVersion;
    poDS->osMDFilename = std::move(osMDFilename);
    poDS->osImageDSFilename = std::move(osImageDSFilename);
    poDS->osDIMAPFilename = std::move(osDIMAPFilename);

    const int res = (nProductVersion == 2) ? poDS->ReadImageInformation2()
                                           : poDS->ReadImageInformation();

    if (res == FALSE)
    {
        delete poDS;
        return nullptr;
    }

    return poDS;
}

/************************************************************************/
/*               ReadImageInformation() DIMAP Version 1                 */
/************************************************************************/

int DIMAPDataset::ReadImageInformation()
{
    CPLXMLNode *psDoc = CPLGetXMLNode(psProduct, "=Dimap_Document");
    if (!psDoc)
        psDoc = CPLGetXMLNode(psProduct, "=PHR_DIMAP_Document");

    /* -------------------------------------------------------------------- */
    /*      Get overall image information.                                  */
    /* -------------------------------------------------------------------- */

    // TODO: DIMAP 1 probably handle mosaics? Like DIMAP 2?

    /* -------------------------------------------------------------------- */
    /*      Get the name of the underlying file.                            */
    /* -------------------------------------------------------------------- */

    const char *pszHref =
        CPLGetXMLValue(psDoc, "Data_Access.Data_File.DATA_FILE_PATH.href", "");
    CPLString osPath = CPLGetPathSafe(osMDFilename);
    CPLString osImageFilename = CPLFormFilenameSafe(osPath, pszHref, nullptr);

    /* -------------------------------------------------------------------- */
    /*      Try and open the file.                                          */
    /* -------------------------------------------------------------------- */

    auto poImageDS = std::unique_ptr<GDALDataset>(GDALDataset::Open(
        osImageFilename, GDAL_OF_RASTER | GDAL_OF_VERBOSE_ERROR));
    if (poImageDS == nullptr)
    {
        return FALSE;
    }
    nRasterXSize = poImageDS->GetRasterXSize();
    nRasterYSize = poImageDS->GetRasterYSize();

    /* -------------------------------------------------------------------- */
    /*      Create and initialize the corresponding VRT dataset used to     */
    /*      manage the tiled data access.                                   */
    /* -------------------------------------------------------------------- */
    poVRTDS = new VRTDataset(nRasterXSize, nRasterYSize);

    // Don't try to write a VRT file.
    poVRTDS->SetWritable(FALSE);

    for (int iBand = 0; iBand < poImageDS->GetRasterCount(); iBand++)
    {
        poVRTDS->AddBand(
            poImageDS->GetRasterBand(iBand + 1)->GetRasterDataType(), nullptr);

        VRTSourcedRasterBand *poVRTBand =
            reinterpret_cast<VRTSourcedRasterBand *>(
                poVRTDS->GetRasterBand(iBand + 1));

        poVRTBand->AddSimpleSource(osImageFilename, iBand + 1, 0, 0,
                                   nRasterXSize, nRasterYSize, 0, 0,
                                   nRasterXSize, nRasterYSize);
    }

    /* -------------------------------------------------------------------- */
    /*      Create band information objects.                                */
    /* -------------------------------------------------------------------- */
    for (int iBand = 1; iBand <= poVRTDS->GetRasterCount(); iBand++)
    {
        SetBand(iBand, new DIMAPRasterBand(this, iBand,
                                           static_cast<VRTSourcedRasterBand *>(
                                               poVRTDS->GetRasterBand(iBand))));
    }

    /* -------------------------------------------------------------------- */
    /*      Try to collect simple insertion point.                          */
    /* -------------------------------------------------------------------- */
    CPLXMLNode *psGeoLoc =
        CPLGetXMLNode(psDoc, "Geoposition.Geoposition_Insert");

    if (psGeoLoc != nullptr)
    {
        bHaveGeoTransform = TRUE;
        m_gt[0] = CPLAtof(CPLGetXMLValue(psGeoLoc, "ULXMAP", "0"));
        m_gt[1] = CPLAtof(CPLGetXMLValue(psGeoLoc, "XDIM", "0"));
        m_gt[2] = 0.0;
        m_gt[3] = CPLAtof(CPLGetXMLValue(psGeoLoc, "ULYMAP", "0"));
        m_gt[4] = 0.0;
        m_gt[5] = -CPLAtof(CPLGetXMLValue(psGeoLoc, "YDIM", "0"));
    }
    else
    {
        // Try to get geotransform from underlying raster.
        if (poImageDS->GetGeoTransform(m_gt) == CE_None)
            bHaveGeoTransform = TRUE;
    }

    /* -------------------------------------------------------------------- */
    /*      Collect GCPs.                                                   */
    /* -------------------------------------------------------------------- */
    psGeoLoc = CPLGetXMLNode(psDoc, "Geoposition.Geoposition_Points");

    if (psGeoLoc != nullptr)
    {
        // Count gcps.
        nGCPCount = 0;
        for (CPLXMLNode *psNode = psGeoLoc->psChild; psNode != nullptr;
             psNode = psNode->psNext)
        {
            if (EQUAL(psNode->pszValue, "Tie_Point"))
                nGCPCount++;
        }

        pasGCPList =
            static_cast<GDAL_GCP *>(CPLCalloc(sizeof(GDAL_GCP), nGCPCount));

        nGCPCount = 0;

        for (CPLXMLNode *psNode = psGeoLoc->psChild; psNode != nullptr;
             psNode = psNode->psNext)
        {
            GDAL_GCP *psGCP = pasGCPList + nGCPCount;

            if (!EQUAL(psNode->pszValue, "Tie_Point"))
                continue;

            nGCPCount++;

            char szID[32] = {};
            snprintf(szID, sizeof(szID), "%d", nGCPCount);
            psGCP->pszId = CPLStrdup(szID);
            psGCP->pszInfo = CPLStrdup("");
            psGCP->dfGCPPixel =
                CPLAtof(CPLGetXMLValue(psNode, "TIE_POINT_DATA_X", "0")) - 0.5;
            psGCP->dfGCPLine =
                CPLAtof(CPLGetXMLValue(psNode, "TIE_POINT_DATA_Y", "0")) - 0.5;
            psGCP->dfGCPX =
                CPLAtof(CPLGetXMLValue(psNode, "TIE_POINT_CRS_X", ""));
            psGCP->dfGCPY =
                CPLAtof(CPLGetXMLValue(psNode, "TIE_POINT_CRS_Y", ""));
            psGCP->dfGCPZ =
                CPLAtof(CPLGetXMLValue(psNode, "TIE_POINT_CRS_Z", ""));
        }
    }

    /* -------------------------------------------------------------------- */
    /*      Collect the CRS.  For now we look only for EPSG codes.          */
    /* -------------------------------------------------------------------- */
    const char *pszSRS = CPLGetXMLValue(
        psDoc, "Coordinate_Reference_System.Horizontal_CS.HORIZONTAL_CS_CODE",
        nullptr);

    if (pszSRS != nullptr)
    {
        OGRSpatialReference &oSRS = nGCPCount > 0 ? m_oGCPSRS : m_oSRS;
        oSRS.SetFromUserInput(
            pszSRS, OGRSpatialReference::SET_FROM_USER_INPUT_LIMITATIONS_get());
    }
    else
    {
        // Check underlying raster for SRS. We have cases where
        // HORIZONTAL_CS_CODE is empty and the underlying raster
        // is georeferenced (rprinceley).
        const auto poSRS = poImageDS->GetSpatialRef();
        if (poSRS)
        {
            m_oSRS = *poSRS;
        }
    }

    /* -------------------------------------------------------------------- */
    /*      Translate other metadata of interest.                           */
    /* -------------------------------------------------------------------- */
    static const char *const apszMetadataTranslation[] = {
        "Production",
        "",
        "Production.Facility",
        "FACILITY_",
        "Dataset_Sources.Source_Information.Scene_Source",
        "",
        "Data_Processing",
        "",
        "Image_Interpretation.Spectral_Band_Info",
        "SPECTRAL_",
        nullptr,
        nullptr};

    SetMetadataFromXML(psProduct, apszMetadataTranslation);

    /* -------------------------------------------------------------------- */
    /*      Set Band metadata from the <Spectral_Band_Info> content         */
    /* -------------------------------------------------------------------- */

    CPLXMLNode *psImageInterpretationNode =
        CPLGetXMLNode(psDoc, "Image_Interpretation");
    if (psImageInterpretationNode != nullptr)
    {
        CPLXMLNode *psSpectralBandInfoNode = psImageInterpretationNode->psChild;
        while (psSpectralBandInfoNode != nullptr)
        {
            if (psSpectralBandInfoNode->eType == CXT_Element &&
                EQUAL(psSpectralBandInfoNode->pszValue, "Spectral_Band_Info"))
            {
                CPLXMLNode *psTag = psSpectralBandInfoNode->psChild;
                int nBandIndex = 0;
                while (psTag != nullptr)
                {
                    if (psTag->eType == CXT_Element &&
                        psTag->psChild != nullptr &&
                        psTag->psChild->eType == CXT_Text &&
                        psTag->pszValue != nullptr)
                    {
                        if (EQUAL(psTag->pszValue, "BAND_INDEX"))
                        {
                            nBandIndex = atoi(psTag->psChild->pszValue);
                            if (nBandIndex <= 0 ||
                                nBandIndex > poImageDS->GetRasterCount())
                            {
                                CPLError(CE_Warning, CPLE_AppDefined,
                                         "Bad BAND_INDEX value : %s",
                                         psTag->psChild->pszValue);
                                nBandIndex = 0;
                            }
                        }
                        else if (nBandIndex >= 1)
                        {
                            GetRasterBand(nBandIndex)
                                ->SetMetadataItem(psTag->pszValue,
                                                  psTag->psChild->pszValue);
                        }
                    }
                    psTag = psTag->psNext;
                }
            }
            psSpectralBandInfoNode = psSpectralBandInfoNode->psNext;
        }
    }

    /* -------------------------------------------------------------------- */
    /*      Initialize any PAM information.                                 */
    /* -------------------------------------------------------------------- */
    SetDescription(osMDFilename);
    TryLoadXML();

    /* -------------------------------------------------------------------- */
    /*      Check for overviews.                                            */
    /* -------------------------------------------------------------------- */
    oOvManager.Initialize(this, osMDFilename);

    // CID 163546 - poTileDS dereferenced above.
    // coverity[leaked_storage]
    return TRUE;
}

/************************************************************************/
/*               ReadImageInformation() DIMAP Version 2                 */
/************************************************************************/

int DIMAPDataset::ReadImageInformation2()
{
    CPLXMLNode *psDoc = CPLGetXMLNode(psProductDim, "=Dimap_Document");
    if (!psDoc)
        psDoc = CPLGetXMLNode(psProductDim, "=PHR_DIMAP_Document");

    CPLXMLNode *psImageAttributes =
        CPLGetXMLNode(psDoc, "Raster_Data.Raster_Dimensions");
    if (psImageAttributes == nullptr)
    {
        CPLError(CE_Failure, CPLE_OpenFailed,
                 "Failed to find <Raster_Dimensions> in document.");
        return FALSE;
    }

    /* -------------------------------------------------------------------- */
    /*      Get overall image information.                                  */
    /* -------------------------------------------------------------------- */

    /*
        <Raster_Dimensions>
           <NROWS>30</NROWS>
           <NCOLS>20</NCOLS>
           <NBANDS>4</NBANDS>
           <Tile_Set>
              <NTILES>2</NTILES>
              <Regular_Tiling>
                 <NTILES_SIZE nrows="20" ncols="20"/>
                 <NTILES_COUNT ntiles_R="2" ntiles_C="1"/>
                 <OVERLAP_ROW>0</OVERLAP_ROW>
                 <OVERLAP_COL>0</OVERLAP_COL>
              </Regular_Tiling>
           </Tile_Set>
        </Raster_Dimensions>
      */

    const int l_nBands =
        atoi(CPLGetXMLValue(psImageAttributes, "NBANDS", "-1"));
    nRasterXSize = atoi(CPLGetXMLValue(psImageAttributes, "NCOLS", "-1"));
    nRasterYSize = atoi(CPLGetXMLValue(psImageAttributes, "NROWS", "-1"));
    if (nRasterXSize <= 0 || nRasterYSize <= 0)
    {
        CPLError(CE_Failure, CPLE_AppDefined,
                 "Invalid NCOLS(=%d)/NROWS(=%d) value", nRasterXSize,
                 nRasterYSize);
        return FALSE;
    }
    int nTileWidth = atoi(CPLGetXMLValue(
        psImageAttributes, "Tile_Set.Regular_Tiling.NTILES_SIZE.ncols", "-1"));
    int nTileHeight = atoi(CPLGetXMLValue(
        psImageAttributes, "Tile_Set.Regular_Tiling.NTILES_SIZE.nrows", "-1"));
    int nOverlapRow = atoi(CPLGetXMLValue(
        psImageAttributes, "Tile_Set.Regular_Tiling.OVERLAP_ROW", "-1"));
    int nOverlapCol = atoi(CPLGetXMLValue(
        psImageAttributes, "Tile_Set.Regular_Tiling.OVERLAP_COL", "-1"));
    const int nBits =
        atoi(CPLGetXMLValue(psDoc, "Raster_Data.Raster_Encoding.NBITS", "-1"));
    CPLString osDataFormat =
        CPLGetXMLValue(psDoc, "Raster_Data.Data_Access.DATA_FILE_FORMAT", "");
    if (osDataFormat == "image/jp2")
    {
        SetMetadataItem("COMPRESSION", "JPEG2000", "IMAGE_STRUCTURE");
    }

    // For VHR2020: SPECTRAL_PROCESSING = PAN, MS, MS-FS, PMS, PMS-N, PMS-X,
    // PMS-FS
    const CPLString osSpectralProcessing = CPLGetXMLValue(
        psDoc, "Processing_Information.Product_Settings.SPECTRAL_PROCESSING",
        "");
    const bool bTwoDataFilesPerTile =
        osSpectralProcessing == "MS-FS" || osSpectralProcessing == "PMS-FS";

    /* -------------------------------------------------------------------- */
    /*      Get the name of the underlying file.                            */
    /* -------------------------------------------------------------------- */

    CPLXMLNode *psDataFiles =
        CPLGetXMLNode(psDoc, "Raster_Data.Data_Access.Data_Files");

    /*  <Data_Files>
            <Data_File tile_R="1" tile_C="1">
               <DATA_FILE_PATH href="IMG_foo_R1C1.TIF"/>
            </Data_File>
            <Data_File tile_R="2" tile_C="1">
               <DATA_FILE_PATH href="IMG_foo_R2C1.TIF"/>
            </Data_File>
         </Data_Files>
    */

    struct TileIdx
    {
        int nRow;
        int nCol;
        int nPart;  // typically 0.  But for VHR2020 0=RGB, 1=NED

        TileIdx(int nRowIn, int nColIn, int nPartIn = 0)
            : nRow(nRowIn), nCol(nColIn), nPart(nPartIn)
        {
        }

        bool operator<(const TileIdx &other) const
        {
            if (nRow < other.nRow)
                return true;
            if (nRow > other.nRow)
                return false;
            if (nCol < other.nCol)
                return true;
            if (nCol > other.nCol)
                return false;
            return nPart < other.nPart;
        }
    };

    std::map<TileIdx, CPLString> oMapTileIdxToName;
    int nImageDSRow = 1, nImageDSCol = 1;
    if (psDataFiles)
    {
        const CPLString osPath = CPLGetPathSafe(osDIMAPFilename);
        for (int nPart = 0; psDataFiles != nullptr;
             psDataFiles = psDataFiles->psNext, nPart++)
        {
            for (CPLXMLNode *psDataFile = psDataFiles->psChild; psDataFile;
                 psDataFile = psDataFile->psNext)
            {
                if (psDataFile->eType == CXT_Element &&
                    strcmp(psDataFile->pszValue, "Data_File") == 0)
                {
                    const char *pszR =
                        CPLGetXMLValue(psDataFile, "tile_R", nullptr);
                    const char *pszC =
                        CPLGetXMLValue(psDataFile, "tile_C", nullptr);
                    const char *pszHref = CPLGetXMLValue(
                        psDataFile, "DATA_FILE_PATH.href", nullptr);
                    if (pszR && pszC && pszHref)
                    {
                        int nRow = atoi(pszR);
                        int nCol = atoi(pszC);
                        if (nRow < 0 || nCol < 0)
                        {
                            return false;
                        }
                        std::string osTileFilename(
                            CPLFormCIFilenameSafe(osPath, pszHref, nullptr));
                        if ((nRow == 1 && nCol == 1 && nPart == 0) ||
                            osImageDSFilename.empty())
                        {
                            osImageDSFilename = osTileFilename;
                            nImageDSRow = nRow;
                            nImageDSCol = nCol;
                        }
                        oMapTileIdxToName[TileIdx(nRow, nCol, nPart)] =
                            std::move(osTileFilename);
                    }
                }
            }
        }
        if (nOverlapRow > 0 || nOverlapCol > 0)
        {
            CPLError(CE_Warning, CPLE_AppDefined,
                     "Overlap between tiles is not handled currently. "
                     "Only taking into account top left tile");
            oMapTileIdxToName.clear();
            oMapTileIdxToName[TileIdx(1, 1)] = osImageDSFilename;
        }
    }
    else
    {
        oMapTileIdxToName[TileIdx(1, 1)] = osImageDSFilename;
    }

    if (osImageDSFilename.empty())
    {
        CPLError(CE_Failure, CPLE_OpenFailed,
                 "Failed to find <DATA_FILE_PATH> in document.");
        return FALSE;
    }

    /* -------------------------------------------------------------------- */
    /*      Try and open the file.                                          */
    /* -------------------------------------------------------------------- */
    auto poImageDS = std::unique_ptr<GDALDataset>(GDALDataset::Open(
        osImageDSFilename, GDAL_OF_RASTER | GDAL_OF_VERBOSE_ERROR));
    if (poImageDS == nullptr)
    {
        return FALSE;
    }
    if (bTwoDataFilesPerTile)
    {
        if (l_nBands != 6 || poImageDS->GetRasterCount() != 3)
        {
            CPLError(CE_Failure, CPLE_AppDefined, "Inconsistent band count");
            return FALSE;
        }
    }
    else if (poImageDS->GetRasterCount() != l_nBands)
    {
        CPLError(CE_Failure, CPLE_AppDefined, "Inconsistent band count");
        return FALSE;
    }

    if (nTileWidth > 0 && nTileHeight > 0)
    {
        // ok
    }
    else if (oMapTileIdxToName.size() == 1 ||
             (bTwoDataFilesPerTile && oMapTileIdxToName.size() == 2))
    {
        nTileWidth = poImageDS->GetRasterXSize();
        nTileHeight = poImageDS->GetRasterYSize();
    }

    if (!(nTileWidth > 0 && nTileHeight > 0))
    {
        CPLError(CE_Failure, CPLE_AppDefined, "Cannot get tile dimension");
        return FALSE;
    }

    /* -------------------------------------------------------------------- */
    /*      Create and initialize the corresponding VRT dataset used to     */
    /*      manage the tiled data access.                                   */
    /* -------------------------------------------------------------------- */
    poVRTDS = new VRTDataset(nRasterXSize, nRasterYSize);

    // Don't try to write a VRT file.
    poVRTDS->SetWritable(FALSE);

    for (int iBand = 0; iBand < l_nBands; iBand++)
    {
        auto poSrcBandFirstImage = poImageDS->GetRasterBand(
            iBand < poImageDS->GetRasterCount() ? iBand + 1 : 1);
        CPLStringList aosAddBandOptions;
        int nSrcBlockXSize, nSrcBlockYSize;
        poSrcBandFirstImage->GetBlockSize(&nSrcBlockXSize, &nSrcBlockYSize);
        if (oMapTileIdxToName.size() == 1 ||
            ((nTileWidth % nSrcBlockXSize) == 0 &&
             (nTileHeight % nSrcBlockYSize) == 0))
        {
            aosAddBandOptions.SetNameValue("BLOCKXSIZE",
                                           CPLSPrintf("%d", nSrcBlockXSize));
            aosAddBandOptions.SetNameValue("BLOCKYSIZE",
                                           CPLSPrintf("%d", nSrcBlockYSize));
        }
        poVRTDS->AddBand(poSrcBandFirstImage->GetRasterDataType(),
                         aosAddBandOptions.List());

        VRTSourcedRasterBand *poVRTBand =
            reinterpret_cast<VRTSourcedRasterBand *>(
                poVRTDS->GetRasterBand(iBand + 1));
        if (nBits > 0 && nBits != 8 && nBits != 16)
        {
            poVRTBand->SetMetadataItem("NBITS", CPLSPrintf("%d", nBits),
                                       "IMAGE_STRUCTURE");
        }

        for (const auto &oTileIdxNameTuple : oMapTileIdxToName)
        {
            const int nRow = oTileIdxNameTuple.first.nRow;
            const int nCol = oTileIdxNameTuple.first.nCol;
            if (static_cast<int64_t>(nRow - 1) * nTileHeight < nRasterYSize &&
                static_cast<int64_t>(nCol - 1) * nTileWidth < nRasterXSize)
            {
                int nSrcBand;
                if (bTwoDataFilesPerTile)
                {
                    const int nPart = oTileIdxNameTuple.first.nPart;
                    if (nPart == 0 && iBand < 3)
                    {
                        nSrcBand = iBand + 1;
                    }
                    else if (nPart == 1 && iBand >= 3)
                    {
                        nSrcBand = iBand + 1 - 3;
                    }
                    else
                    {
                        continue;
                    }
                }
                else
                {
                    nSrcBand = iBand + 1;
                }

                int nHeight = nTileHeight;
                if (static_cast<int64_t>(nRow) * nTileHeight > nRasterYSize)
                {
                    nHeight = nRasterYSize - (nRow - 1) * nTileHeight;
                }
                int nWidth = nTileWidth;
                if (static_cast<int64_t>(nCol) * nTileWidth > nRasterXSize)
                {
                    nWidth = nRasterXSize - (nCol - 1) * nTileWidth;
                }

                poVRTBand->AddSimpleSource(
                    oTileIdxNameTuple.second, nSrcBand, 0, 0, nWidth, nHeight,
                    (nCol - 1) * nTileWidth, (nRow - 1) * nTileHeight, nWidth,
                    nHeight);
            }
        }
    }

    /* -------------------------------------------------------------------- */
    /*      Expose Overviews if available                                   */
    /* -------------------------------------------------------------------- */
    auto poSrcBandFirstImage = poImageDS->GetRasterBand(1);
    const int nSrcOverviews =
        std::min(30, poSrcBandFirstImage->GetOverviewCount());
    if (nSrcOverviews > 0)
    {
        CPLConfigOptionSetter oSetter("VRT_VIRTUAL_OVERVIEWS", "YES", false);
        std::unique_ptr<int[]> ovrLevels(new int[nSrcOverviews]);
        int iLvl = 1;
        for (int i = 0; i < nSrcOverviews; i++)
        {
            iLvl *= 2;
            ovrLevels[i] = iLvl;
        }
        poVRTDS->IBuildOverviews("average", nSrcOverviews, ovrLevels.get(), 0,
                                 nullptr, nullptr, nullptr, nullptr);
    }

#ifdef DEBUG_VERBOSE
    CPLDebug("DIMAP", "VRT XML: %s", poVRTDS->GetMetadata("xml:VRT")[0]);
#endif

    /* -------------------------------------------------------------------- */
    /*      Create band information objects.                                */
    /* -------------------------------------------------------------------- */
    for (int iBand = 1; iBand <= poVRTDS->GetRasterCount(); iBand++)
    {
        GDALRasterBand *poBand = new DIMAPRasterBand(
            this, iBand,
            static_cast<VRTSourcedRasterBand *>(poVRTDS->GetRasterBand(iBand)));
        if (nBits > 0 && nBits != 8 && nBits != 16)
        {
            poBand->SetMetadataItem("NBITS", CPLSPrintf("%d", nBits),
                                    "IMAGE_STRUCTURE");
        }
        if (bTwoDataFilesPerTile)
        {
            switch (iBand)
            {
                case 1:
                {
                    poBand->SetColorInterpretation(GCI_RedBand);
                    poBand->SetDescription("Red");
                    break;
                }
                case 2:
                {
                    poBand->SetColorInterpretation(GCI_GreenBand);
                    poBand->SetDescription("Green");
                    break;
                }
                case 3:
                {
                    poBand->SetColorInterpretation(GCI_BlueBand);
                    poBand->SetDescription("Blue");
                    break;
                }
                case 4:
                {
                    poBand->SetColorInterpretation(GCI_NIRBand);
                    poBand->SetDescription("NIR");
                    break;
                }
                case 5:
                {
                    poBand->SetColorInterpretation(GCI_RedEdgeBand);
                    poBand->SetDescription("Red Edge");
                    break;
                }
                case 6:
                {
                    poBand->SetColorInterpretation(GCI_CoastalBand);
                    poBand->SetDescription("Deep Blue");
                    break;
                }
                default:
                    break;
            }
        }
        else if (l_nBands == 1 && osSpectralProcessing == "PAN")
        {
            poBand->SetColorInterpretation(GCI_PanBand);
            poBand->SetDescription("Panchromatic");
        }
        SetBand(iBand, poBand);
    }

    /* -------------------------------------------------------------------- */
    /*      Try to collect simple insertion point.                          */
    /* -------------------------------------------------------------------- */
    CPLXMLNode *psGeoLoc =
        CPLGetXMLNode(psDoc, "Geoposition.Geoposition_Insert");

    if (psGeoLoc != nullptr)
    {
        bHaveGeoTransform = TRUE;
        m_gt[0] = CPLAtof(CPLGetXMLValue(psGeoLoc, "ULXMAP", "0"));
        m_gt[1] = CPLAtof(CPLGetXMLValue(psGeoLoc, "XDIM", "0"));
        m_gt[2] = 0.0;
        m_gt[3] = CPLAtof(CPLGetXMLValue(psGeoLoc, "ULYMAP", "0"));
        m_gt[4] = 0.0;
        m_gt[5] = -CPLAtof(CPLGetXMLValue(psGeoLoc, "YDIM", "0"));
    }
    else
    {
        // Try to get geotransform from underlying raster,
        // but make sure it is a real geotransform.
        if (poImageDS->GetGeoTransform(m_gt) == CE_None &&
            !(m_gt[0] <= 1.5 && fabs(m_gt[3]) <= 1.5))
        {
            bHaveGeoTransform = TRUE;
            // fix up the origin if we did not get the geotransform from the
            // top-left tile
            m_gt[0] -= (nImageDSCol - 1) * m_gt[1] * nTileWidth +
                       (nImageDSRow - 1) * m_gt[2] * nTileHeight;
            m_gt[3] -= (nImageDSCol - 1) * m_gt[4] * nTileWidth +
                       (nImageDSRow - 1) * m_gt[5] * nTileHeight;
        }
    }

    /* -------------------------------------------------------------------- */
    /*      Collect the CRS.  For now we look only for EPSG codes.          */
    /* -------------------------------------------------------------------- */
    const char *pszSRS = CPLGetXMLValue(
        psDoc, "Coordinate_Reference_System.Projected_CRS.PROJECTED_CRS_CODE",
        nullptr);
    if (pszSRS == nullptr)
        pszSRS = CPLGetXMLValue(
            psDoc, "Coordinate_Reference_System.Geodetic_CRS.GEODETIC_CRS_CODE",
            nullptr);

    if (pszSRS != nullptr)
    {
        if (bHaveGeoTransform)
        {
            OGRSpatialReference &oSRS = m_oSRS;
            oSRS.SetFromUserInput(
                pszSRS,
                OGRSpatialReference::SET_FROM_USER_INPUT_LIMITATIONS_get());
        }
    }
    else
    {
        // Check underlying raster for SRS. We have cases where
        // HORIZONTAL_CS_CODE is empty and the underlying raster
        // is georeferenced (rprinceley).
        const auto poSRS = poImageDS->GetSpatialRef();
        GDALGeoTransform tmpGT;
        if (poSRS && poImageDS->GetGeoTransform(tmpGT) == CE_None)
        {
            m_oSRS = *poSRS;
        }
    }

    /* -------------------------------------------------------------------- */
    /*      Translate other metadata of interest: DIM_<product_name>.XML    */
    /* -------------------------------------------------------------------- */

    static const char *const apszMetadataTranslationDim[] = {
        "Product_Information.Delivery_Identification",
        "DATASET_",
        "Product_Information.Producer_Information",
        "DATASET_",
        "Dataset_Sources.Source_Identification.Strip_Source",
        "",
        "Processing_Information.Production_Facility",
        "FACILITY_",
        "Processing_Information.Product_Settings",
        "",
        "Processing_Information.Product_Settings.Geometric_Settings",
        "GEOMETRIC_",
        "Processing_Information.Product_Settings.Radiometric_Settings",
        "RADIOMETRIC_",
        "Quality_Assessment.Imaging_Quality_Measurement",
        "CLOUDCOVER_",
        nullptr,
        nullptr};

    SetMetadataFromXML(psProductDim, apszMetadataTranslationDim);

    /* -------------------------------------------------------------------- */
    /*      Translate other metadata of interest: STRIP_<product_name>.XML    */
    /* -------------------------------------------------------------------- */

    static const char *const apszMetadataTranslationStrip[] = {
        "Catalog.Full_Strip.Notations.Cloud_And_Quality_Notation."
        "Data_Strip_Notation",
        "CLOUDCOVER_",
        "Acquisition_Configuration.Platform_Configuration."
        "Ephemeris_Configuration",
        "EPHEMERIS_",
        nullptr,
        nullptr};

    if (psProductStrip != nullptr)
        SetMetadataFromXML(psProductStrip, apszMetadataTranslationStrip);

    if (!osRPCFilename.empty())
    {
        GDALMDReaderPleiades *poReader =
            GDALMDReaderPleiades::CreateReaderForRPC(osRPCFilename);
        char **papszRPC = poReader->LoadRPCXmlFile(psDoc);
        delete poReader;
        if (papszRPC)
            SetMetadata(papszRPC, "RPC");
        CSLDestroy(papszRPC);
    }

    CPLXMLNode *psLocatedUseAreaNode =
        CPLGetXMLNode(psDoc, "Geometric_Data.Use_Area");
    if (psLocatedUseAreaNode != nullptr)
    {
        CPLXMLNode *psLocatedGeometricValuesNode =
            psLocatedUseAreaNode->psChild;
        while (psLocatedGeometricValuesNode != nullptr)
        {
            CPLXMLNode *psLocationType =
                CPLGetXMLNode(psLocatedGeometricValuesNode, "LOCATION_TYPE");
            if (psLocationType == nullptr ||
                psLocationType->psChild == nullptr ||
                !EQUAL(psLocationType->psChild->pszValue, "center"))
            {
                psLocatedGeometricValuesNode =
                    psLocatedGeometricValuesNode->psNext;
                continue;
            }
            static const char *const apszLGVTranslationDim[] = {
                "SATELLITE_ALTITUDE",
                "",
                "Acquisition_Angles",
                "",
                "Solar_Incidences",
                "",
                "Ground_Sample_Distance",
                "",
                nullptr,
                nullptr};

            SetMetadataFromXML(psLocatedGeometricValuesNode,
                               apszLGVTranslationDim, false);
            break;
        }
    }

    /* -------------------------------------------------------------------- */
    /*      Set Band metadata from the <Band_Radiance> and                  */
    /*                                <Band_Spectral_Range> content         */
    /* -------------------------------------------------------------------- */

    CPLXMLNode *psImageInterpretationNode = CPLGetXMLNode(
        psDoc,
        "Radiometric_Data.Radiometric_Calibration.Instrument_Calibration."
        "Band_Measurement_List");
    if (psImageInterpretationNode != nullptr)
    {
        CPLXMLNode *psSpectralBandInfoNode = psImageInterpretationNode->psChild;
        while (psSpectralBandInfoNode != nullptr)
        {
            if (psSpectralBandInfoNode->eType == CXT_Element &&
                (EQUAL(psSpectralBandInfoNode->pszValue, "Band_Radiance") ||
                 EQUAL(psSpectralBandInfoNode->pszValue,
                       "Band_Spectral_Range") ||
                 EQUAL(psSpectralBandInfoNode->pszValue,
                       "Band_Solar_Irradiance")))
            {
                CPLString osName;

                if (EQUAL(psSpectralBandInfoNode->pszValue, "Band_Radiance"))
                    osName = "RADIANCE_";
                else if (EQUAL(psSpectralBandInfoNode->pszValue,
                               "Band_Spectral_Range"))
                    osName = "SPECTRAL_RANGE_";
                else if (EQUAL(psSpectralBandInfoNode->pszValue,
                               "Band_Solar_Irradiance"))
                    osName = "SOLAR_IRRADIANCE_";

                CPLXMLNode *psTag = psSpectralBandInfoNode->psChild;
                int nBandIndex = 0;
                while (psTag != nullptr)
                {
                    if (psTag->eType == CXT_Element &&
                        psTag->psChild != nullptr &&
                        psTag->pszValue != nullptr &&
                        (psTag->psChild->eType == CXT_Text ||
                         EQUAL(psTag->pszValue, "FWHM")))
                    {
                        if (EQUAL(psTag->pszValue, "BAND_ID"))
                        {
                            nBandIndex = 0;
                            if (EQUAL(psTag->psChild->pszValue, "P") ||
                                EQUAL(psTag->psChild->pszValue, "PAN") ||
                                EQUAL(psTag->psChild->pszValue, "B0") ||
                                EQUAL(psTag->psChild->pszValue, "R"))
                                nBandIndex = 1;
                            else if (EQUAL(psTag->psChild->pszValue, "B1") ||
                                     EQUAL(psTag->psChild->pszValue, "G"))
                                nBandIndex = 2;
                            else if (EQUAL(psTag->psChild->pszValue, "B2") ||
                                     EQUAL(psTag->psChild->pszValue, "B"))
                                nBandIndex = 3;
                            else if (EQUAL(psTag->psChild->pszValue, "B3") ||
                                     EQUAL(psTag->psChild->pszValue, "NIR"))
                                nBandIndex = 4;
                            else if (EQUAL(psTag->psChild->pszValue, "RE"))
                                nBandIndex = 5;
                            else if (EQUAL(psTag->psChild->pszValue, "DB"))
                                nBandIndex = 6;

                            if (nBandIndex <= 0 ||
                                nBandIndex > GetRasterCount())
                            {
                                CPLError(CE_Warning, CPLE_AppDefined,
                                         "Bad BAND_ID value : %s",
                                         psTag->psChild->pszValue);
                                nBandIndex = 0;
                            }
                        }
                        else if (nBandIndex >= 1)
                        {
                            CPLString osMDName = osName;
                            osMDName += psTag->pszValue;

                            auto poBand = GetRasterBand(nBandIndex);
                            if (EQUAL(psTag->pszValue, "FWHM"))
                            {
                                if (const char *pszMIN =
                                        CPLGetXMLValue(psTag, "MIN", nullptr))
                                    poBand->SetMetadataItem(
                                        (osMDName + "_MIN").c_str(), pszMIN);
                                if (const char *pszMAX =
                                        CPLGetXMLValue(psTag, "MAX", nullptr))
                                    poBand->SetMetadataItem(
                                        (osMDName + "_MAX").c_str(), pszMAX);
                            }
                            else
                            {
                                poBand->SetMetadataItem(
                                    osMDName, psTag->psChild->pszValue);
                            }
                        }
                    }
                    psTag = psTag->psNext;
                }
            }
            psSpectralBandInfoNode = psSpectralBandInfoNode->psNext;
        }
    }

    // Fill raster band IMAGERY metadata domain from FWHM metadata.
    for (int i = 1; i <= nBands; ++i)
    {
        auto poBand = GetRasterBand(i);
        const char *SPECTRAL_RANGE_MEASURE_UNIT =
            poBand->GetMetadataItem("SPECTRAL_RANGE_MEASURE_UNIT");
        const char *SPECTRAL_RANGE_FWHM_MIN =
            poBand->GetMetadataItem("SPECTRAL_RANGE_FWHM_MIN");
        const char *SPECTRAL_RANGE_FWHM_MAX =
            poBand->GetMetadataItem("SPECTRAL_RANGE_FWHM_MAX");
        if (SPECTRAL_RANGE_MEASURE_UNIT && SPECTRAL_RANGE_FWHM_MIN &&
            SPECTRAL_RANGE_FWHM_MAX &&
            (EQUAL(SPECTRAL_RANGE_MEASURE_UNIT, "nanometer") ||
             EQUAL(SPECTRAL_RANGE_MEASURE_UNIT, "micrometer")))
        {
            const double dfFactorToMicrometer =
                EQUAL(SPECTRAL_RANGE_MEASURE_UNIT, "nanometer") ? 1e-3 : 1.0;
            const double dfMin =
                CPLAtof(SPECTRAL_RANGE_FWHM_MIN) * dfFactorToMicrometer;
            const double dfMax =
                CPLAtof(SPECTRAL_RANGE_FWHM_MAX) * dfFactorToMicrometer;
            poBand->SetMetadataItem("CENTRAL_WAVELENGTH_UM",
                                    CPLSPrintf("%.3f", (dfMin + dfMax) / 2),
                                    "IMAGERY");
            poBand->SetMetadataItem(
                "FWHM_UM", CPLSPrintf("%.3f", dfMax - dfMin), "IMAGERY");
        }
    }

    /* -------------------------------------------------------------------- */
    /*      Initialize any PAM information.                                 */
    /* -------------------------------------------------------------------- */
    SetDescription(osMDFilename);
    TryLoadXML();

    /* -------------------------------------------------------------------- */
    /*      Check for overviews.                                            */
    /* -------------------------------------------------------------------- */
    oOvManager.Initialize(this, osMDFilename);

    return TRUE;
}

/************************************************************************/
/*                          SetMetadataFromXML()                        */
/************************************************************************/

void DIMAPDataset::SetMetadataFromXML(
    CPLXMLNode *psProductIn, const char *const apszMetadataTranslation[],
    bool bKeysFromRoot)
{
    CPLXMLNode *psDoc = psProductIn;
    if (bKeysFromRoot)
    {
        psDoc = CPLGetXMLNode(psProductIn, "=Dimap_Document");
        if (psDoc == nullptr)
        {
            psDoc = CPLGetXMLNode(psProductIn, "=PHR_DIMAP_Document");
        }
    }

    bool bWarnedDiscarding = false;

    for (int iTrItem = 0; apszMetadataTranslation[iTrItem] != nullptr;
         iTrItem += 2)
    {
        CPLXMLNode *psParent =
            CPLGetXMLNode(psDoc, apszMetadataTranslation[iTrItem]);

        if (psParent == nullptr)
            continue;

        // Logic to support directly access a name/value entry
        if (psParent->psChild != nullptr &&
            psParent->psChild->eType == CXT_Text)
        {
            CPLString osName = apszMetadataTranslation[iTrItem + 1];
            osName += apszMetadataTranslation[iTrItem];
            // Limit size to avoid perf issues when inserting
            // in metadata list
            if (osName.size() < 128)
                SetMetadataItem(osName, psParent->psChild->pszValue);
            else if (!bWarnedDiscarding)
            {
                bWarnedDiscarding = true;
                CPLDebug("DIMAP", "Discarding too long metadata item");
            }
            continue;
        }

        // Logic to support a parent element with many name/values.
        CPLXMLNode *psTarget = psParent->psChild;
        for (; psTarget != nullptr && psTarget != psParent;
             psTarget = psTarget->psNext)
        {
            if (psTarget->eType == CXT_Element && psTarget->psChild != nullptr)
            {
                CPLString osName = apszMetadataTranslation[iTrItem + 1];

                if (psTarget->psChild->eType == CXT_Text)
                {
                    osName += psTarget->pszValue;
                    // Limit size to avoid perf issues when inserting
                    // in metadata list
                    if (osName.size() < 128)
                        SetMetadataItem(osName, psTarget->psChild->pszValue);
                    else if (!bWarnedDiscarding)
                    {
                        bWarnedDiscarding = true;
                        CPLDebug("DIMAP", "Discarding too long metadata item");
                    }
                }
                else if (psTarget->psChild->eType == CXT_Attribute)
                {
                    // find the tag value, at the end of the attributes.
                    for (CPLXMLNode *psNode = psTarget->psChild;
                         psNode != nullptr; psNode = psNode->psNext)
                    {
                        if (psNode->eType == CXT_Attribute)
                            continue;
                        else if (psNode->eType == CXT_Text)
                        {
                            osName += psTarget->pszValue;
                            // Limit size to avoid perf issues when inserting
                            // in metadata list
                            if (osName.size() < 128)
                                SetMetadataItem(osName, psNode->pszValue);
                            else if (!bWarnedDiscarding)
                            {
                                bWarnedDiscarding = true;
                                CPLDebug("DIMAP",
                                         "Discarding too long metadata item");
                            }
                        }
                    }
                }
            }
        }
    }
}

/************************************************************************/
/*                            GetGCPCount()                             */
/************************************************************************/

int DIMAPDataset::GetGCPCount()

{
    return nGCPCount;
}

/************************************************************************/
/*                          GetGCPSpatialRef()                          */
/************************************************************************/

const OGRSpatialReference *DIMAPDataset::GetGCPSpatialRef() const

{
    return m_oGCPSRS.IsEmpty() ? nullptr : &m_oGCPSRS;
}

/************************************************************************/
/*                               GetGCPs()                              */
/************************************************************************/

const GDAL_GCP *DIMAPDataset::GetGCPs()

{
    return pasGCPList;
}

/************************************************************************/
/*                         GDALRegister_DIMAP()                         */
/************************************************************************/

void GDALRegister_DIMAP()

{
    if (GDALGetDriverByName("DIMAP") != nullptr)
        return;

    GDALDriver *poDriver = new GDALDriver();

    poDriver->SetDescription("DIMAP");
    poDriver->SetMetadataItem(GDAL_DCAP_RASTER, "YES");
    poDriver->SetMetadataItem(GDAL_DMD_LONGNAME, "SPOT DIMAP");
    poDriver->SetMetadataItem(GDAL_DMD_HELPTOPIC, "drivers/raster/dimap.html");
    poDriver->SetMetadataItem(GDAL_DCAP_VIRTUALIO, "YES");
    poDriver->SetMetadataItem(GDAL_DMD_SUBDATASETS, "YES");

    poDriver->pfnOpen = DIMAPDataset::Open;
    poDriver->pfnIdentify = DIMAPDataset::Identify;

    GetGDALDriverManager()->RegisterDriver(poDriver);
}
