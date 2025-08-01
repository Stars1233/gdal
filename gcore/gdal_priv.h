/******************************************************************************
 *
 * Name:     gdal_priv.h
 * Project:  GDAL Core
 * Purpose:  GDAL Core C++/Private declarations.
 * Author:   Frank Warmerdam, warmerdam@pobox.com
 *
 ******************************************************************************
 * Copyright (c) 1998, Frank Warmerdam
 * Copyright (c) 2007-2014, Even Rouault <even dot rouault at spatialys.com>
 *
 * SPDX-License-Identifier: MIT
 ****************************************************************************/

#ifndef GDAL_PRIV_H_INCLUDED
#define GDAL_PRIV_H_INCLUDED

/**
 * \file gdal_priv.h
 *
 * C++ GDAL entry points.
 */

/* -------------------------------------------------------------------- */
/*      Predeclare various classes before pulling in gdal.h, the        */
/*      public declarations.                                            */
/* -------------------------------------------------------------------- */
class GDALMajorObject;
class GDALDataset;
class GDALRasterBand;
class GDALDriver;
class GDALRasterAttributeTable;
class GDALProxyDataset;
class GDALProxyRasterBand;
class GDALAsyncReader;
class GDALRelationship;
class GDALAlgorithm;

/* -------------------------------------------------------------------- */
/*      Pull in the public declarations.  This gets the C apis, and     */
/*      also various constants.  However, we will still get to          */
/*      provide the real class definitions for the GDAL classes.        */
/* -------------------------------------------------------------------- */

#include "gdal.h"
#include "gdal_frmts.h"
#include "gdalsubdatasetinfo.h"
#include "cpl_vsi.h"
#include "cpl_conv.h"
#include "cpl_string.h"
#include "cpl_minixml.h"
#include "cpl_multiproc.h"
#include "cpl_atomic_ops.h"

#include <stdarg.h>

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdint>
#include <iterator>
#include <limits>
#include <map>
#include <memory>
#include <set>
#if __cplusplus >= 202002L
#include <span>
#endif
#include <type_traits>
#include <utility>
#include <vector>

#include "ogr_core.h"
#include "ogr_feature.h"

//! @cond Doxygen_Suppress
#define GMO_VALID 0x0001
#define GMO_IGNORE_UNIMPLEMENTED 0x0002
#define GMO_SUPPORT_MD 0x0004
#define GMO_SUPPORT_MDMD 0x0008
#define GMO_MD_DIRTY 0x0010
#define GMO_PAM_CLASS 0x0020

//! @endcond

/************************************************************************/
/*                       GDALMultiDomainMetadata                        */
/************************************************************************/

//! @cond Doxygen_Suppress
class CPL_DLL GDALMultiDomainMetadata
{
  private:
    CPLStringList aosDomainList{};

    struct Comparator
    {
        bool operator()(const char *a, const char *b) const
        {
            return STRCASECMP(a, b) < 0;
        }
    };

    std::map<const char *, CPLStringList, Comparator> oMetadata{};

  public:
    GDALMultiDomainMetadata();

    /** Copy constructor */
    GDALMultiDomainMetadata(const GDALMultiDomainMetadata &) = default;

    /** Copy assignment operator */
    GDALMultiDomainMetadata &
    operator=(const GDALMultiDomainMetadata &) = default;

    /** Move constructor */
    GDALMultiDomainMetadata(GDALMultiDomainMetadata &&) = default;

    /** Move assignment operator */
    GDALMultiDomainMetadata &operator=(GDALMultiDomainMetadata &&) = default;

    ~GDALMultiDomainMetadata();

    int XMLInit(const CPLXMLNode *psMetadata, int bMerge);
    CPLXMLNode *Serialize() const;

    CSLConstList GetDomainList() const
    {
        return aosDomainList.List();
    }

    char **GetMetadata(const char *pszDomain = "");
    CPLErr SetMetadata(CSLConstList papszMetadata, const char *pszDomain = "");
    const char *GetMetadataItem(const char *pszName,
                                const char *pszDomain = "");
    CPLErr SetMetadataItem(const char *pszName, const char *pszValue,
                           const char *pszDomain = "");

    void Clear();

    inline void clear()
    {
        Clear();
    }
};

//! @endcond

/* ******************************************************************** */
/*                           GDALMajorObject                            */
/*                                                                      */
/*      Base class providing metadata, description and other            */
/*      services shared by major objects.                               */
/* ******************************************************************** */

/** Object with metadata. */
class CPL_DLL GDALMajorObject
{
  protected:
    //! @cond Doxygen_Suppress
    int nFlags;  // GMO_* flags.
    CPLString sDescription{};
    GDALMultiDomainMetadata oMDMD{};

    //! @endcond

    char **BuildMetadataDomainList(char **papszList, int bCheckNonEmpty,
                                   ...) CPL_NULL_TERMINATED;

    /** Copy constructor */
    GDALMajorObject(const GDALMajorObject &) = default;

    /** Copy assignment operator */
    GDALMajorObject &operator=(const GDALMajorObject &) = default;

    /** Move constructor */
    GDALMajorObject(GDALMajorObject &&) = default;

    /** Move assignment operator */
    GDALMajorObject &operator=(GDALMajorObject &&) = default;

  public:
    GDALMajorObject();
    virtual ~GDALMajorObject();

    int GetMOFlags() const;
    void SetMOFlags(int nFlagsIn);

    virtual const char *GetDescription() const;
    virtual void SetDescription(const char *);

    virtual char **GetMetadataDomainList();

    virtual char **GetMetadata(const char *pszDomain = "");
    virtual CPLErr SetMetadata(char **papszMetadata,
                               const char *pszDomain = "");
    virtual const char *GetMetadataItem(const char *pszName,
                                        const char *pszDomain = "");
    virtual CPLErr SetMetadataItem(const char *pszName, const char *pszValue,
                                   const char *pszDomain = "");

    /** Convert a GDALMajorObject* to a GDALMajorObjectH.
     * @since GDAL 2.3
     */
    static inline GDALMajorObjectH ToHandle(GDALMajorObject *poMajorObject)
    {
        return static_cast<GDALMajorObjectH>(poMajorObject);
    }

    /** Convert a GDALMajorObjectH to a GDALMajorObject*.
     * @since GDAL 2.3
     */
    static inline GDALMajorObject *FromHandle(GDALMajorObjectH hMajorObject)
    {
        return static_cast<GDALMajorObject *>(hMajorObject);
    }
};

/* ******************************************************************** */
/*                         GDALDefaultOverviews                         */
/* ******************************************************************** */

//! @cond Doxygen_Suppress
class GDALOpenInfo;

class CPL_DLL GDALDefaultOverviews
{
    friend class GDALDataset;

    GDALDataset *poDS;
    GDALDataset *poODS;

    CPLString osOvrFilename{};

    bool bOvrIsAux;

    bool bCheckedForMask;
    bool bOwnMaskDS;
    GDALDataset *poMaskDS;

    // For "overview datasets" we record base level info so we can
    // find our way back to get overview masks.
    GDALDataset *poBaseDS;

    // Stuff for deferred initialize/overviewscans.
    bool bCheckedForOverviews;
    void OverviewScan();
    char *pszInitName;
    bool bInitNameIsOVR;
    char **papszInitSiblingFiles;

  public:
    GDALDefaultOverviews();
    ~GDALDefaultOverviews();

    void Initialize(GDALDataset *poDSIn, const char *pszName = nullptr,
                    CSLConstList papszSiblingFiles = nullptr,
                    bool bNameIsOVR = false);

    void Initialize(GDALDataset *poDSIn, GDALOpenInfo *poOpenInfo,
                    const char *pszName = nullptr,
                    bool bTransferSiblingFilesIfLoaded = true);

    void TransferSiblingFiles(char **papszSiblingFiles);

    int IsInitialized();

    int CloseDependentDatasets();

    // Overview Related

    int GetOverviewCount(int nBand);
    GDALRasterBand *GetOverview(int nBand, int iOverview);

    CPLErr BuildOverviews(const char *pszBasename, const char *pszResampling,
                          int nOverviews, const int *panOverviewList,
                          int nBands, const int *panBandList,
                          GDALProgressFunc pfnProgress, void *pProgressData,
                          CSLConstList papszOptions);

    CPLErr BuildOverviewsSubDataset(const char *pszPhysicalFile,
                                    const char *pszResampling, int nOverviews,
                                    const int *panOverviewList, int nBands,
                                    const int *panBandList,
                                    GDALProgressFunc pfnProgress,
                                    void *pProgressData,
                                    CSLConstList papszOptions);

    CPLErr BuildOverviewsMask(const char *pszResampling, int nOverviews,
                              const int *panOverviewList,
                              GDALProgressFunc pfnProgress, void *pProgressData,
                              CSLConstList papszOptions);

    static bool CheckSrcOverviewsConsistencyWithBase(
        GDALDataset *poFullResDS,
        const std::vector<GDALDataset *> &apoSrcOvrDS);

    CPLErr AddOverviews(const char *pszBasename,
                        const std::vector<GDALDataset *> &apoSrcOvrDS,
                        GDALProgressFunc pfnProgress, void *pProgressData,
                        CSLConstList papszOptions);

    CPLErr CleanOverviews();

    // Mask Related

    CPLErr CreateMaskBand(int nFlags, int nBand = -1);
    GDALRasterBand *GetMaskBand(int nBand);
    int GetMaskFlags(int nBand);

    int HaveMaskFile(char **papszSiblings = nullptr,
                     const char *pszBasename = nullptr);

    char **GetSiblingFiles()
    {
        return papszInitSiblingFiles;
    }

  private:
    CPL_DISALLOW_COPY_ASSIGN(GDALDefaultOverviews)

    CPLErr CreateOrOpenOverviewFile(const char *pszBasename,
                                    CSLConstList papszOptions);
};

//! @endcond

/* ******************************************************************** */
/*                             GDALOpenInfo                             */
/* ******************************************************************** */

/** Class for dataset open functions. */
class CPL_DLL GDALOpenInfo
{
    bool bHasGotSiblingFiles = false;
    char **papszSiblingFiles = nullptr;
    int nHeaderBytesTried = 0;

  public:
    GDALOpenInfo(const char *pszFile, int nOpenFlagsIn,
                 const char *const *papszSiblingFiles = nullptr);
    ~GDALOpenInfo(void);

    /** Filename */
    char *pszFilename = nullptr;

    /** Result of CPLGetExtension(pszFilename); */
    std::string osExtension{};

    /** Open options */
    char **papszOpenOptions = nullptr;

    /** Access flag */
    GDALAccess eAccess = GA_ReadOnly;
    /** Open flags */
    int nOpenFlags = 0;

    /** Whether stat()'ing the file was successful */
    bool bStatOK = false;
    /** Whether the file is a directory */
    bool bIsDirectory = false;

    /** Pointer to the file */
    VSILFILE *fpL = nullptr;

    /** Number of bytes in pabyHeader */
    int nHeaderBytes = 0;
    /** Buffer with first bytes of the file */
    GByte *pabyHeader = nullptr;

    /** Allowed drivers (NULL for all) */
    const char *const *papszAllowedDrivers = nullptr;

    int TryToIngest(int nBytes);
    char **GetSiblingFiles();
    char **StealSiblingFiles();
    bool AreSiblingFilesLoaded() const;

    bool IsSingleAllowedDriver(const char *pszDriverName) const;

    /** Return whether the extension of the file is equal to pszExt, using
     * case-insensitive comparison.
     * @since 3.11 */
    inline bool IsExtensionEqualToCI(const char *pszExt) const
    {
        return EQUAL(osExtension.c_str(), pszExt);
    }

  private:
    CPL_DISALLOW_COPY_ASSIGN(GDALOpenInfo)
};

/* ******************************************************************** */
/*                             gdal::GCP                                */
/* ******************************************************************** */

namespace gdal
{
/** C++ wrapper over the C GDAL_GCP structure.
 *
 * It has the same binary layout, and thus a gdal::GCP pointer can be cast as a
 * GDAL_GCP pointer.
 *
 * @since 3.9
 */
class CPL_DLL GCP
{
  public:
    explicit GCP(const char *pszId = "", const char *pszInfo = "",
                 double dfPixel = 0, double dfLine = 0, double dfX = 0,
                 double dfY = 0, double dfZ = 0);
    ~GCP();
    GCP(const GCP &);
    explicit GCP(const GDAL_GCP &other);
    GCP &operator=(const GCP &);
    GCP(GCP &&);
    GCP &operator=(GCP &&);

    /** Returns the "id" member. */
    inline const char *Id() const
    {
        return gcp.pszId;
    }

    void SetId(const char *pszId);

    /** Returns the "info" member. */
    inline const char *Info() const
    {
        return gcp.pszInfo;
    }

    void SetInfo(const char *pszInfo);

    /** Returns the "pixel" member. */
    inline double Pixel() const
    {
        return gcp.dfGCPPixel;
    }

    /** Returns a reference to the "pixel" member. */
    inline double &Pixel()
    {
        return gcp.dfGCPPixel;
    }

    /** Returns the "line" member. */
    inline double Line() const
    {
        return gcp.dfGCPLine;
    }

    /** Returns a reference to the "line" member. */
    inline double &Line()
    {
        return gcp.dfGCPLine;
    }

    /** Returns the "X" member. */
    inline double X() const
    {
        return gcp.dfGCPX;
    }

    /** Returns a reference to the "X" member. */
    inline double &X()
    {
        return gcp.dfGCPX;
    }

    /** Returns the "Y" member. */
    inline double Y() const
    {
        return gcp.dfGCPY;
    }

    /** Returns a reference to the "Y" member. */
    inline double &Y()
    {
        return gcp.dfGCPY;
    }

    /** Returns the "Z" member. */
    inline double Z() const
    {
        return gcp.dfGCPZ;
    }

    /** Returns a reference to the "Z" member. */
    inline double &Z()
    {
        return gcp.dfGCPZ;
    }

    /** Casts as a C GDAL_GCP pointer */
    inline const GDAL_GCP *c_ptr() const
    {
        return &gcp;
    }

    static const GDAL_GCP *c_ptr(const std::vector<GCP> &asGCPs);

    static std::vector<GCP> fromC(const GDAL_GCP *pasGCPList, int nGCPCount);

  private:
    GDAL_GCP gcp;
};

} /* namespace gdal */

/* ******************************************************************** */
/*                             GDALGeoTransform                         */
/* ******************************************************************** */

/** Class that encapsulates a geotransform matrix.
 *
 * It contains 6 coefficients expressing an affine transformation from
 * (column, line) raster space to (X, Y) georeferenced space, such that
 *
 * \code{.c}
 *  X = xorig + column * xscale + line * xrot;
 *  Y = yorig + column * yrot   + line * yscale;
 * \endcode
 *
 * The default value is the identity transformation.
 *
 * @since 3.12
 */
class GDALGeoTransform
{
  public:
    // NOTE to GDAL developers: do not reorder those coefficients!

    /** X value of the origin of the raster */
    double xorig = 0;

    /** X scale factor */
    double xscale = 1;

    /** X rotation factor */
    double xrot = 0;

    /** Y value of the origin of the raster */
    double yorig = 0;

    /** Y rotation factor */
    double yrot = 0;

    /** Y scale factor */
    double yscale = 1;

    /** Default constructor for an identity geotransformation matrix. */
    inline GDALGeoTransform() = default;

    /** Constructor from a array of 6 double */
    inline explicit GDALGeoTransform(const double coeffs[6])
    {
        static_assert(sizeof(GDALGeoTransform) == 6 * sizeof(double),
                      "Wrong size for GDALGeoTransform");
        xorig = coeffs[0];
        xscale = coeffs[1];
        xrot = coeffs[2];
        yorig = coeffs[3];
        yrot = coeffs[4];
        yscale = coeffs[5];
    }

    /** Constructor from 6 double values */
    inline GDALGeoTransform(double xorigIn, double xscaleIn, double xrotIn,
                            double yorigIn, double yrotIn, double yscaleIn)
    {
        xorig = xorigIn;
        xscale = xscaleIn;
        xrot = xrotIn;
        yorig = yorigIn;
        yrot = yrotIn;
        yscale = yscaleIn;
    }

    /** Element accessor. idx must be in [0,5] range */
    template <typename T> inline double operator[](T idx) const
    {
        return *(&xorig + idx);
    }

    /** Element accessor. idx must be in [0,5] range */
    template <typename T> inline double &operator[](T idx)
    {
        return *(&xorig + idx);
    }

    /** Equality test operator */
    inline bool operator==(const GDALGeoTransform &other) const
    {
        return xorig == other.xorig && xscale == other.xscale &&
               xrot == other.xrot && yorig == other.yorig &&
               yrot == other.yrot && yscale == other.yscale;
    }

    /** Inequality test operator */
    inline bool operator!=(const GDALGeoTransform &other) const
    {
        return !(operator==(other));
    }

    /** Cast to const double* */
    inline const double *data() const
    {
        return &xorig;
    }

    /** Cast to double* */
    inline double *data()
    {
        return &xorig;
    }

    /**
     * Apply GeoTransform to x/y coordinate.
     *
     * Applies the following computation, converting a (pixel, line) coordinate
     * into a georeferenced (geo_x, geo_y) location.
     * \code{.c}
     *  *pdfGeoX = padfGeoTransform[0] + dfPixel * padfGeoTransform[1]
     *                                 + dfLine  * padfGeoTransform[2];
     *  *pdfGeoY = padfGeoTransform[3] + dfPixel * padfGeoTransform[4]
     *                                 + dfLine  * padfGeoTransform[5];
     * \endcode
     *
     * @param dfPixel Input pixel position.
     * @param dfLine Input line position.
     * @param pdfGeoX output location where geo_x (easting/longitude)
     * location is placed.
     * @param pdfGeoY output location where geo_y (northing/latitude)
     * location is placed.
     */

    inline void Apply(double dfPixel, double dfLine, double *pdfGeoX,
                      double *pdfGeoY) const
    {
        GDALApplyGeoTransform(data(), dfPixel, dfLine, pdfGeoX, pdfGeoY);
    }

    /**
     * Invert Geotransform.
     *
     * This function will invert a standard 3x2 set of GeoTransform coefficients.
     * This converts the equation from being pixel to geo to being geo to pixel.
     *
     * @param[out] inverse Output geotransform
     *
     * @return true on success or false if the equation is uninvertable.
     */
    inline bool GetInverse(GDALGeoTransform &inverse) const
    {
        return GDALInvGeoTransform(data(), inverse.data()) == TRUE;
    }

    /** Rescale a geotransform by multiplying its scale and rotation terms by
     * the provided ratios.
     *
     * This is typically used to compute the geotransform matrix of an overview
     * dataset from the full resolution dataset, where the ratios are the size
     * of the full resolution dataset divided by the size of the overview.
     */
    inline void Rescale(double dfXRatio, double dfYRatio)
    {
        xscale *= dfXRatio;
        xrot *= dfYRatio;
        yrot *= dfXRatio;
        yscale *= dfYRatio;
    }
};

/* ******************************************************************** */
/*                             GDALDataset                              */
/* ******************************************************************** */

class OGRLayer;
class OGRGeometry;
class OGRSpatialReference;
class OGRStyleTable;
class swq_select;
class swq_select_parse_options;
class GDALGroup;

//! @cond Doxygen_Suppress
typedef struct GDALSQLParseInfo GDALSQLParseInfo;
//! @endcond

//! @cond Doxygen_Suppress
#ifdef GDAL_COMPILATION
#define OPTIONAL_OUTSIDE_GDAL(val)
#else
#define OPTIONAL_OUTSIDE_GDAL(val) = val
#endif
//! @endcond

//! @cond Doxygen_Suppress
// This macro can be defined to check that GDALDataset::IRasterIO()
// implementations do not alter the passed panBandList. It is not defined
// by default (and should not!), hence int* is used.
#if defined(GDAL_BANDMAP_TYPE_CONST_SAFE)
#define BANDMAP_TYPE const int *
#else
#define BANDMAP_TYPE int *
#endif
//! @endcond

/** A set of associated raster bands, usually from one file. */
class CPL_DLL GDALDataset : public GDALMajorObject
{
    friend GDALDatasetH CPL_STDCALL
    GDALOpenEx(const char *pszFilename, unsigned int nOpenFlags,
               const char *const *papszAllowedDrivers,
               const char *const *papszOpenOptions,
               const char *const *papszSiblingFiles);
    friend CPLErr CPL_STDCALL GDALClose(GDALDatasetH hDS);

    friend class GDALDriver;
    friend class GDALDefaultOverviews;
    friend class GDALProxyDataset;
    friend class GDALDriverManager;

    CPL_INTERNAL void AddToDatasetOpenList();

    CPL_INTERNAL void UnregisterFromSharedDataset();

    CPL_INTERNAL static void ReportErrorV(const char *pszDSName,
                                          CPLErr eErrClass, CPLErrorNum err_no,
                                          const char *fmt, va_list args);

  protected:
    //! @cond Doxygen_Suppress
    GDALDriver *poDriver = nullptr;
    GDALAccess eAccess = GA_ReadOnly;

    // Stored raster information.
    int nRasterXSize = 512;
    int nRasterYSize = 512;
    int nBands = 0;
    GDALRasterBand **papoBands = nullptr;

    static constexpr int OPEN_FLAGS_CLOSED = -1;
    int nOpenFlags =
        0;  // set to OPEN_FLAGS_CLOSED after Close() has been called

    int nRefCount = 1;
    bool bForceCachedIO = false;
    bool bShared = false;
    bool bIsInternal = true;
    bool bSuppressOnClose = false;

    mutable std::map<std::string, std::unique_ptr<OGRFieldDomain>>
        m_oMapFieldDomains{};

    GDALDataset(void);
    explicit GDALDataset(int bForceCachedIO);

    void RasterInitialize(int, int);
    void SetBand(int nNewBand, GDALRasterBand *poBand);
    void SetBand(int nNewBand, std::unique_ptr<GDALRasterBand> poBand);

    GDALDefaultOverviews oOvManager{};

    virtual CPLErr IBuildOverviews(const char *pszResampling, int nOverviews,
                                   const int *panOverviewList, int nListBands,
                                   const int *panBandList,
                                   GDALProgressFunc pfnProgress,
                                   void *pProgressData,
                                   CSLConstList papszOptions);

    virtual CPLErr
    IRasterIO(GDALRWFlag eRWFlag, int nXOff, int nYOff, int nXSize, int nYSize,
              void *pData, int nBufXSize, int nBufYSize, GDALDataType eBufType,
              int nBandCount, BANDMAP_TYPE panBandMap, GSpacing nPixelSpace,
              GSpacing nLineSpace, GSpacing nBandSpace,
              GDALRasterIOExtraArg *psExtraArg) CPL_WARN_UNUSED_RESULT;

    /* This method should only be be overloaded by GDALProxyDataset */
    virtual CPLErr
    BlockBasedRasterIO(GDALRWFlag eRWFlag, int nXOff, int nYOff, int nXSize,
                       int nYSize, void *pData, int nBufXSize, int nBufYSize,
                       GDALDataType eBufType, int nBandCount,
                       const int *panBandMap, GSpacing nPixelSpace,
                       GSpacing nLineSpace, GSpacing nBandSpace,
                       GDALRasterIOExtraArg *psExtraArg) CPL_WARN_UNUSED_RESULT;
    CPLErr BlockBasedFlushCache(bool bAtClosing);

    CPLErr
    BandBasedRasterIO(GDALRWFlag eRWFlag, int nXOff, int nYOff, int nXSize,
                      int nYSize, void *pData, int nBufXSize, int nBufYSize,
                      GDALDataType eBufType, int nBandCount,
                      const int *panBandMap, GSpacing nPixelSpace,
                      GSpacing nLineSpace, GSpacing nBandSpace,
                      GDALRasterIOExtraArg *psExtraArg) CPL_WARN_UNUSED_RESULT;

    CPLErr
    RasterIOResampled(GDALRWFlag eRWFlag, int nXOff, int nYOff, int nXSize,
                      int nYSize, void *pData, int nBufXSize, int nBufYSize,
                      GDALDataType eBufType, int nBandCount,
                      const int *panBandMap, GSpacing nPixelSpace,
                      GSpacing nLineSpace, GSpacing nBandSpace,
                      GDALRasterIOExtraArg *psExtraArg) CPL_WARN_UNUSED_RESULT;

    CPLErr ValidateRasterIOOrAdviseReadParameters(
        const char *pszCallingFunc, int *pbStopProcessingOnCENone, int nXOff,
        int nYOff, int nXSize, int nYSize, int nBufXSize, int nBufYSize,
        int nBandCount, const int *panBandMap);

    CPLErr TryOverviewRasterIO(GDALRWFlag eRWFlag, int nXOff, int nYOff,
                               int nXSize, int nYSize, void *pData,
                               int nBufXSize, int nBufYSize,
                               GDALDataType eBufType, int nBandCount,
                               const int *panBandMap, GSpacing nPixelSpace,
                               GSpacing nLineSpace, GSpacing nBandSpace,
                               GDALRasterIOExtraArg *psExtraArg, int *pbTried);

    void ShareLockWithParentDataset(GDALDataset *poParentDataset);

    bool m_bCanBeReopened = false;

    virtual bool CanBeCloned(int nScopeFlags, bool bCanShareState) const;

    friend class GDALThreadSafeDataset;
    friend class MEMDataset;
    virtual std::unique_ptr<GDALDataset> Clone(int nScopeFlags,
                                               bool bCanShareState) const;

    //! @endcond

    void CleanupPostFileClosing();

    virtual int CloseDependentDatasets();
    //! @cond Doxygen_Suppress
    int ValidateLayerCreationOptions(const char *const *papszLCO);

    char **papszOpenOptions = nullptr;

    friend class GDALRasterBand;

    // The below methods related to read write mutex are fragile logic, and
    // should not be used by out-of-tree code if possible.
    int EnterReadWrite(GDALRWFlag eRWFlag);
    void LeaveReadWrite();
    void InitRWLock();

    void TemporarilyDropReadWriteLock();
    void ReacquireReadWriteLock();

    void DisableReadWriteMutex();

    int AcquireMutex();
    void ReleaseMutex();

    bool IsAllBands(int nBandCount, const int *panBandList) const;
    //! @endcond

  public:
    ~GDALDataset() override;

    virtual CPLErr Close();

    int GetRasterXSize() const;
    int GetRasterYSize() const;
    int GetRasterCount() const;
    GDALRasterBand *GetRasterBand(int);
    const GDALRasterBand *GetRasterBand(int) const;

    /**
     * @brief SetQueryLoggerFunc
     * @param pfnQueryLoggerFuncIn query logger function callback
     * @param poQueryLoggerArgIn arguments passed to the query logger function
     * @return true on success
     */
    virtual bool SetQueryLoggerFunc(GDALQueryLoggerFunc pfnQueryLoggerFuncIn,
                                    void *poQueryLoggerArgIn);

    /** Class returned by GetBands() that act as a container for raster bands.
     */
    class CPL_DLL Bands
    {
      private:
        friend class GDALDataset;
        GDALDataset *m_poSelf;

        CPL_INTERNAL explicit Bands(GDALDataset *poSelf) : m_poSelf(poSelf)
        {
        }

        class CPL_DLL Iterator
        {
            struct Private;
            std::unique_ptr<Private> m_poPrivate;

          public:
            Iterator(GDALDataset *poDS, bool bStart);
            Iterator(const Iterator &oOther);  // declared but not defined.
                                               // Needed for gcc 5.4 at least
            Iterator(Iterator &&oOther) noexcept;  // declared but not defined.
                // Needed for gcc 5.4 at least
            ~Iterator();
            GDALRasterBand *operator*();
            Iterator &operator++();
            bool operator!=(const Iterator &it) const;
        };

      public:
        const Iterator begin() const;

        const Iterator end() const;

        size_t size() const;

        GDALRasterBand *operator[](int iBand);
        GDALRasterBand *operator[](size_t iBand);
    };

    Bands GetBands();

    virtual CPLErr FlushCache(bool bAtClosing = false);
    virtual CPLErr DropCache();

    virtual GIntBig GetEstimatedRAMUsage();

    virtual const OGRSpatialReference *GetSpatialRef() const;
    virtual CPLErr SetSpatialRef(const OGRSpatialReference *poSRS);

    // Compatibility layer
    const char *GetProjectionRef(void) const;
    CPLErr SetProjection(const char *pszProjection);

    virtual CPLErr GetGeoTransform(GDALGeoTransform &gt) const;
    virtual CPLErr SetGeoTransform(const GDALGeoTransform &gt);

    CPLErr GetGeoTransform(double *padfGeoTransform) const
#if defined(GDAL_COMPILATION) && !defined(DOXYGEN_XML)
        CPL_WARN_DEPRECATED("Use GetGeoTransform(GDALGeoTransform&) instead")
#endif
            ;

    CPLErr SetGeoTransform(const double *padfGeoTransform)
#if defined(GDAL_COMPILATION) && !defined(DOXYGEN_XML)
        CPL_WARN_DEPRECATED(
            "Use SetGeoTransform(const GDALGeoTransform&) instead")
#endif
            ;

    virtual CPLErr GetExtent(OGREnvelope *psExtent,
                             const OGRSpatialReference *poCRS = nullptr) const;
    virtual CPLErr GetExtentWGS84LongLat(OGREnvelope *psExtent) const;

    CPLErr GeolocationToPixelLine(
        double dfGeolocX, double dfGeolocY, const OGRSpatialReference *poSRS,
        double *pdfPixel, double *pdfLine,
        CSLConstList papszTransformerOptions = nullptr) const;

    virtual CPLErr AddBand(GDALDataType eType, char **papszOptions = nullptr);

    virtual void *GetInternalHandle(const char *pszHandleName);
    virtual GDALDriver *GetDriver(void);
    virtual char **GetFileList(void);

    virtual const char *GetDriverName();

    virtual const OGRSpatialReference *GetGCPSpatialRef() const;
    virtual int GetGCPCount();
    virtual const GDAL_GCP *GetGCPs();
    virtual CPLErr SetGCPs(int nGCPCount, const GDAL_GCP *pasGCPList,
                           const OGRSpatialReference *poGCP_SRS);

    // Compatibility layer
    const char *GetGCPProjection();
    CPLErr SetGCPs(int nGCPCount, const GDAL_GCP *pasGCPList,
                   const char *pszGCPProjection);

    virtual CPLErr AdviseRead(int nXOff, int nYOff, int nXSize, int nYSize,
                              int nBufXSize, int nBufYSize, GDALDataType eDT,
                              int nBandCount, int *panBandList,
                              char **papszOptions);

    virtual CPLErr CreateMaskBand(int nFlagsIn);

    virtual GDALAsyncReader *
    BeginAsyncReader(int nXOff, int nYOff, int nXSize, int nYSize, void *pBuf,
                     int nBufXSize, int nBufYSize, GDALDataType eBufType,
                     int nBandCount, int *panBandMap, int nPixelSpace,
                     int nLineSpace, int nBandSpace, char **papszOptions);
    virtual void EndAsyncReader(GDALAsyncReader *poARIO);

    //! @cond Doxygen_Suppress
    struct RawBinaryLayout
    {
        enum class Interleaving
        {
            UNKNOWN,
            BIP,
            BIL,
            BSQ
        };
        std::string osRawFilename{};
        Interleaving eInterleaving = Interleaving::UNKNOWN;
        GDALDataType eDataType = GDT_Unknown;
        bool bLittleEndianOrder = false;

        vsi_l_offset nImageOffset = 0;
        GIntBig nPixelOffset = 0;
        GIntBig nLineOffset = 0;
        GIntBig nBandOffset = 0;
    };

    virtual bool GetRawBinaryLayout(RawBinaryLayout &);
    //! @endcond

#ifndef DOXYGEN_SKIP
    CPLErr RasterIO(GDALRWFlag eRWFlag, int nXOff, int nYOff, int nXSize,
                    int nYSize, void *pData, int nBufXSize, int nBufYSize,
                    GDALDataType eBufType, int nBandCount,
                    const int *panBandMap, GSpacing nPixelSpace,
                    GSpacing nLineSpace, GSpacing nBandSpace,
                    GDALRasterIOExtraArg *psExtraArg
                        OPTIONAL_OUTSIDE_GDAL(nullptr)) CPL_WARN_UNUSED_RESULT;
#else
    CPLErr RasterIO(GDALRWFlag eRWFlag, int nXOff, int nYOff, int nXSize,
                    int nYSize, void *pData, int nBufXSize, int nBufYSize,
                    GDALDataType eBufType, int nBandCount,
                    const int *panBandMap, GSpacing nPixelSpace,
                    GSpacing nLineSpace, GSpacing nBandSpace,
                    GDALRasterIOExtraArg *psExtraArg) CPL_WARN_UNUSED_RESULT;
#endif

    virtual CPLStringList GetCompressionFormats(int nXOff, int nYOff,
                                                int nXSize, int nYSize,
                                                int nBandCount,
                                                const int *panBandList);
    virtual CPLErr ReadCompressedData(const char *pszFormat, int nXOff,
                                      int nYOff, int nXSize, int nYSize,
                                      int nBands, const int *panBandList,
                                      void **ppBuffer, size_t *pnBufferSize,
                                      char **ppszDetailedFormat);

    int Reference();
    int Dereference();
    int ReleaseRef();

    /** Return access mode.
     * @return access mode.
     */
    GDALAccess GetAccess() const
    {
        return eAccess;
    }

    int GetShared() const;
    void MarkAsShared();

    void MarkSuppressOnClose();
    void UnMarkSuppressOnClose();

    /** Return MarkSuppressOnClose flag.
    * @return MarkSuppressOnClose flag.
    */
    bool IsMarkedSuppressOnClose() const
    {
        return bSuppressOnClose;
    }

    /** Return open options.
     * @return open options.
     */
    char **GetOpenOptions()
    {
        return papszOpenOptions;
    }

    bool IsThreadSafe(int nScopeFlags) const;

#ifndef DOXYGEN_SKIP
    /** Return open options.
     * @return open options.
     */
    CSLConstList GetOpenOptions() const
    {
        return papszOpenOptions;
    }
#endif

    static GDALDataset **GetOpenDatasets(int *pnDatasetCount);

#ifndef DOXYGEN_SKIP
    CPLErr
    BuildOverviews(const char *pszResampling, int nOverviews,
                   const int *panOverviewList, int nListBands,
                   const int *panBandList, GDALProgressFunc pfnProgress,
                   void *pProgressData,
                   CSLConstList papszOptions OPTIONAL_OUTSIDE_GDAL(nullptr));
#else
    CPLErr BuildOverviews(const char *pszResampling, int nOverviews,
                          const int *panOverviewList, int nListBands,
                          const int *panBandList, GDALProgressFunc pfnProgress,
                          void *pProgressData, CSLConstList papszOptions);
#endif

    virtual CPLErr AddOverviews(const std::vector<GDALDataset *> &apoSrcOvrDS,
                                GDALProgressFunc pfnProgress,
                                void *pProgressData, CSLConstList papszOptions);

#ifndef DOXYGEN_XML
    void ReportError(CPLErr eErrClass, CPLErrorNum err_no, const char *fmt,
                     ...) const CPL_PRINT_FUNC_FORMAT(4, 5);

    static void ReportError(const char *pszDSName, CPLErr eErrClass,
                            CPLErrorNum err_no, const char *fmt, ...)
        CPL_PRINT_FUNC_FORMAT(4, 5);
#endif

    char **GetMetadata(const char *pszDomain = "") override;

// Only defined when Doxygen enabled
#ifdef DOXYGEN_SKIP
    CPLErr SetMetadata(char **papszMetadata, const char *pszDomain) override;
    CPLErr SetMetadataItem(const char *pszName, const char *pszValue,
                           const char *pszDomain) override;
#endif

    char **GetMetadataDomainList() override;

    virtual void ClearStatistics();

    /** Convert a GDALDataset* to a GDALDatasetH.
     * @since GDAL 2.3
     */
    static inline GDALDatasetH ToHandle(GDALDataset *poDS)
    {
        return static_cast<GDALDatasetH>(poDS);
    }

    /** Convert a GDALDatasetH to a GDALDataset*.
     * @since GDAL 2.3
     */
    static inline GDALDataset *FromHandle(GDALDatasetH hDS)
    {
        return static_cast<GDALDataset *>(hDS);
    }

    /** @see GDALOpenEx().
     * @since GDAL 2.3
     */
    static GDALDataset *Open(const char *pszFilename,
                             unsigned int nOpenFlags = 0,
                             const char *const *papszAllowedDrivers = nullptr,
                             const char *const *papszOpenOptions = nullptr,
                             const char *const *papszSiblingFiles = nullptr)
    {
        return FromHandle(GDALOpenEx(pszFilename, nOpenFlags,
                                     papszAllowedDrivers, papszOpenOptions,
                                     papszSiblingFiles));
    }

    /** Object returned by GetFeatures() iterators */
    struct FeatureLayerPair
    {
        /** Unique pointer to a OGRFeature. */
        OGRFeatureUniquePtr feature{};

        /** Layer to which the feature belongs to. */
        OGRLayer *layer = nullptr;
    };

    //! @cond Doxygen_Suppress
    // SetEnableOverviews() only to be used by GDALOverviewDataset
    void SetEnableOverviews(bool bEnable);

    // Only to be used by driver's GetOverviewCount() method.
    bool AreOverviewsEnabled() const;

    static void ReportUpdateNotSupportedByDriver(const char *pszDriverName);
    //! @endcond

  private:
    class Private;
    Private *m_poPrivate;

    CPL_INTERNAL OGRLayer *BuildLayerFromSelectInfo(
        swq_select *psSelectInfo, OGRGeometry *poSpatialFilter,
        const char *pszDialect, swq_select_parse_options *poSelectParseOptions);
    CPLStringList oDerivedMetadataList{};

  public:
    virtual int GetLayerCount();
    virtual OGRLayer *GetLayer(int iLayer);

    virtual bool IsLayerPrivate(int iLayer) const;

    /** Class returned by GetLayers() that acts as a range of layers.
     * @since GDAL 2.3
     */
    class CPL_DLL Layers
    {
      private:
        friend class GDALDataset;
        GDALDataset *m_poSelf;

        CPL_INTERNAL explicit Layers(GDALDataset *poSelf) : m_poSelf(poSelf)
        {
        }

      public:
        /** Layer iterator.
         * @since GDAL 2.3
         */
        class CPL_DLL Iterator
        {
            struct Private;
            std::unique_ptr<Private> m_poPrivate;

          public:
            using value_type = OGRLayer *; /**< value_type */
            using reference = OGRLayer *;  /**< reference */
            using difference_type = void;  /**< difference_type */
            using pointer = void;          /**< pointer */
            using iterator_category =
                std::input_iterator_tag; /**< iterator_category */

            Iterator(); /**< Default constructor */
            Iterator(GDALDataset *poDS, bool bStart); /**< Constructor */
            Iterator(const Iterator &oOther);         /**< Copy constructor */
            Iterator(Iterator &&oOther) noexcept;     /**< Move constructor */
            ~Iterator();                              /**< Destructor */

            Iterator &
            operator=(const Iterator &oOther); /**< Assignment operator */
            Iterator &operator=(
                Iterator &&oOther) noexcept; /**< Move assignment operator */

            OGRLayer *operator*() const; /**< Dereference operator */
            Iterator &operator++();      /**< Pre-increment operator */
            Iterator operator++(int);    /**< Post-increment operator */
            bool operator!=(const Iterator &it)
                const; /**< Difference comparison operator */
        };

        Iterator begin() const;
        Iterator end() const;

        size_t size() const;

        OGRLayer *operator[](int iLayer);
        OGRLayer *operator[](size_t iLayer);
        OGRLayer *operator[](const char *pszLayername);
    };

    Layers GetLayers();

    virtual OGRLayer *GetLayerByName(const char *);

    int GetLayerIndex(const char *pszName);

    virtual OGRErr DeleteLayer(int iLayer);

    virtual void ResetReading();
    virtual OGRFeature *GetNextFeature(OGRLayer **ppoBelongingLayer,
                                       double *pdfProgressPct,
                                       GDALProgressFunc pfnProgress,
                                       void *pProgressData);

    /** Class returned by GetFeatures() that act as a container for vector
     * features. */
    class CPL_DLL Features
    {
      private:
        friend class GDALDataset;
        GDALDataset *m_poSelf;

        CPL_INTERNAL explicit Features(GDALDataset *poSelf) : m_poSelf(poSelf)
        {
        }

        class CPL_DLL Iterator
        {
            struct Private;
            std::unique_ptr<Private> m_poPrivate;

          public:
            Iterator(GDALDataset *poDS, bool bStart);
            Iterator(const Iterator &oOther);  // declared but not defined.
                                               // Needed for gcc 5.4 at least
            Iterator(Iterator &&oOther) noexcept;  // declared but not defined.
                // Needed for gcc 5.4 at least
            ~Iterator();
            const FeatureLayerPair &operator*() const;
            Iterator &operator++();
            bool operator!=(const Iterator &it) const;
        };

      public:
        const Iterator begin() const;

        const Iterator end() const;
    };

    Features GetFeatures();

    virtual int TestCapability(const char *);

    virtual std::vector<std::string>
    GetFieldDomainNames(CSLConstList papszOptions = nullptr) const;

    virtual const OGRFieldDomain *GetFieldDomain(const std::string &name) const;

    virtual bool AddFieldDomain(std::unique_ptr<OGRFieldDomain> &&domain,
                                std::string &failureReason);

    virtual bool DeleteFieldDomain(const std::string &name,
                                   std::string &failureReason);

    virtual bool UpdateFieldDomain(std::unique_ptr<OGRFieldDomain> &&domain,
                                   std::string &failureReason);

    virtual std::vector<std::string>
    GetRelationshipNames(CSLConstList papszOptions = nullptr) const;

    virtual const GDALRelationship *
    GetRelationship(const std::string &name) const;

    virtual bool
    AddRelationship(std::unique_ptr<GDALRelationship> &&relationship,
                    std::string &failureReason);

    virtual bool DeleteRelationship(const std::string &name,
                                    std::string &failureReason);

    virtual bool
    UpdateRelationship(std::unique_ptr<GDALRelationship> &&relationship,
                       std::string &failureReason);

    //! @cond Doxygen_Suppress
    OGRLayer *CreateLayer(const char *pszName);

    OGRLayer *CreateLayer(const char *pszName, std::nullptr_t);
    //! @endcond

    OGRLayer *CreateLayer(const char *pszName,
                          const OGRSpatialReference *poSpatialRef,
                          OGRwkbGeometryType eGType = wkbUnknown,
                          CSLConstList papszOptions = nullptr);

    OGRLayer *CreateLayer(const char *pszName,
                          const OGRGeomFieldDefn *poGeomFieldDefn,
                          CSLConstList papszOptions = nullptr);

    virtual OGRLayer *CopyLayer(OGRLayer *poSrcLayer, const char *pszNewName,
                                char **papszOptions = nullptr);

    virtual OGRStyleTable *GetStyleTable();
    virtual void SetStyleTableDirectly(OGRStyleTable *poStyleTable);

    virtual void SetStyleTable(OGRStyleTable *poStyleTable);

    virtual OGRLayer *ExecuteSQL(const char *pszStatement,
                                 OGRGeometry *poSpatialFilter,
                                 const char *pszDialect);
    virtual void ReleaseResultSet(OGRLayer *poResultsSet);
    virtual OGRErr AbortSQL();

    int GetRefCount() const;
    int GetSummaryRefCount() const;
    OGRErr Release();

    virtual OGRErr StartTransaction(int bForce = FALSE);
    virtual OGRErr CommitTransaction();
    virtual OGRErr RollbackTransaction();

    virtual std::shared_ptr<GDALGroup> GetRootGroup() const;

    static std::string BuildFilename(const char *pszFilename,
                                     const char *pszReferencePath,
                                     bool bRelativeToReferencePath);

    //! @cond Doxygen_Suppress
    static int IsGenericSQLDialect(const char *pszDialect);

    // Semi-public methods. Only to be used by in-tree drivers.
    GDALSQLParseInfo *
    BuildParseInfo(swq_select *psSelectInfo,
                   swq_select_parse_options *poSelectParseOptions);
    static void DestroyParseInfo(GDALSQLParseInfo *psParseInfo);
    OGRLayer *ExecuteSQL(const char *pszStatement, OGRGeometry *poSpatialFilter,
                         const char *pszDialect,
                         swq_select_parse_options *poSelectParseOptions);

    static constexpr const char *const apszSpecialSubDatasetSyntax[] = {
        "NITF_IM:{ANY}:{FILENAME}", "PDF:{ANY}:{FILENAME}",
        "RASTERLITE:{FILENAME},{ANY}", "TILEDB:\"{FILENAME}\":{ANY}",
        "TILEDB:{FILENAME}:{ANY}"};

    //! @endcond

  protected:
    virtual OGRLayer *ICreateLayer(const char *pszName,
                                   const OGRGeomFieldDefn *poGeomFieldDefn,
                                   CSLConstList papszOptions);

    //! @cond Doxygen_Suppress
    OGRErr ProcessSQLCreateIndex(const char *);
    OGRErr ProcessSQLDropIndex(const char *);
    OGRErr ProcessSQLDropTable(const char *);
    OGRErr ProcessSQLAlterTableAddColumn(const char *);
    OGRErr ProcessSQLAlterTableDropColumn(const char *);
    OGRErr ProcessSQLAlterTableAlterColumn(const char *);
    OGRErr ProcessSQLAlterTableRenameColumn(const char *);

    OGRStyleTable *m_poStyleTable = nullptr;

    friend class GDALProxyPoolDataset;
    //! @endcond

  private:
    CPL_DISALLOW_COPY_ASSIGN(GDALDataset)
};

//! @cond Doxygen_Suppress
struct CPL_DLL GDALDatasetUniquePtrDeleter
{
    void operator()(GDALDataset *poDataset) const
    {
        GDALClose(poDataset);
    }
};

//! @endcond

//! @cond Doxygen_Suppress
struct CPL_DLL GDALDatasetUniquePtrReleaser
{
    void operator()(GDALDataset *poDataset) const
    {
        if (poDataset)
            poDataset->Release();
    }
};

//! @endcond

/** Unique pointer type for GDALDataset.
 * Appropriate for use on datasets open in non-shared mode and onto which
 * reference counter has not been manually modified.
 * @since GDAL 2.3
 */
using GDALDatasetUniquePtr =
    std::unique_ptr<GDALDataset, GDALDatasetUniquePtrDeleter>;

/* ******************************************************************** */
/*                           GDALRasterBlock                            */
/* ******************************************************************** */

/** A single raster block in the block cache.
 *
 * And the global block manager that manages a least-recently-used list of
 * blocks from various datasets/bands */
class CPL_DLL GDALRasterBlock
{
    friend class GDALAbstractBandBlockCache;

    GDALDataType eType;

    bool bDirty;
    volatile int nLockCount;

    int nXOff;
    int nYOff;

    int nXSize;
    int nYSize;

    void *pData;

    GDALRasterBand *poBand;

    GDALRasterBlock *poNext;
    GDALRasterBlock *poPrevious;

    bool bMustDetach;

    CPL_INTERNAL void Detach_unlocked(void);
    CPL_INTERNAL void Touch_unlocked(void);

    CPL_INTERNAL void RecycleFor(int nXOffIn, int nYOffIn);

  public:
    GDALRasterBlock(GDALRasterBand *, int, int);
    GDALRasterBlock(int nXOffIn, int nYOffIn); /* only for lookup purpose */
    virtual ~GDALRasterBlock();

    CPLErr Internalize(void);
    void Touch(void);
    void MarkDirty(void);
    void MarkClean(void);

    /** Increment the lock count */
    int AddLock(void)
    {
        return CPLAtomicInc(&nLockCount);
    }

    /** Decrement the lock count */
    int DropLock(void)
    {
        return CPLAtomicDec(&nLockCount);
    }

    void Detach();

    CPLErr Write();

    /** Return the data type
     * @return data type
     */
    GDALDataType GetDataType() const
    {
        return eType;
    }

    /** Return the x offset of the top-left corner of the block
     * @return x offset
     */
    int GetXOff() const
    {
        return nXOff;
    }

    /** Return the y offset of the top-left corner of the block
     * @return y offset
     */
    int GetYOff() const
    {
        return nYOff;
    }

    /** Return the width of the block
     * @return width
     */
    int GetXSize() const
    {
        return nXSize;
    }

    /** Return the height of the block
     * @return height
     */
    int GetYSize() const
    {
        return nYSize;
    }

    /** Return the dirty flag
     * @return dirty flag
     */
    int GetDirty() const
    {
        return bDirty;
    }

    /** Return the data buffer
     * @return data buffer
     */
    void *GetDataRef(void)
    {
        return pData;
    }

    /** Return the block size in bytes
     * @return block size.
     */
    GPtrDiff_t GetBlockSize() const
    {
        return static_cast<GPtrDiff_t>(nXSize) * nYSize *
               GDALGetDataTypeSizeBytes(eType);
    }

    int TakeLock();
    int DropLockForRemovalFromStorage();

    /// @brief Accessor to source GDALRasterBand object.
    /// @return source raster band of the raster block.
    GDALRasterBand *GetBand()
    {
        return poBand;
    }

    static void FlushDirtyBlocks();
    static int FlushCacheBlock(int bDirtyBlocksOnly = FALSE);
    static void Verify();

    static void EnterDisableDirtyBlockFlush();
    static void LeaveDisableDirtyBlockFlush();

#ifdef notdef
    static void CheckNonOrphanedBlocks(GDALRasterBand *poBand);
    void DumpBlock();
    static void DumpAll();
#endif

    /* Should only be called by GDALDestroyDriverManager() */
    //! @cond Doxygen_Suppress
    CPL_INTERNAL static void DestroyRBMutex();
    //! @endcond

  private:
    CPL_DISALLOW_COPY_ASSIGN(GDALRasterBlock)
};

/* ******************************************************************** */
/*                             GDALColorTable                           */
/* ******************************************************************** */

/** A color table / palette. */

class CPL_DLL GDALColorTable
{
    GDALPaletteInterp eInterp;

    std::vector<GDALColorEntry> aoEntries{};

  public:
    explicit GDALColorTable(GDALPaletteInterp = GPI_RGB);

    /** Copy constructor */
    GDALColorTable(const GDALColorTable &) = default;

    /** Copy assignment operator */
    GDALColorTable &operator=(const GDALColorTable &) = default;

    /** Move constructor */
    GDALColorTable(GDALColorTable &&) = default;

    /** Move assignment operator */
    GDALColorTable &operator=(GDALColorTable &&) = default;

    ~GDALColorTable();

    GDALColorTable *Clone() const;
    int IsSame(const GDALColorTable *poOtherCT) const;

    GDALPaletteInterp GetPaletteInterpretation() const;

    int GetColorEntryCount() const;
    const GDALColorEntry *GetColorEntry(int i) const;
    int GetColorEntryAsRGB(int i, GDALColorEntry *poEntry) const;
    void SetColorEntry(int i, const GDALColorEntry *poEntry);
    int CreateColorRamp(int nStartIndex, const GDALColorEntry *psStartColor,
                        int nEndIndex, const GDALColorEntry *psEndColor);
    bool IsIdentity() const;

    static std::unique_ptr<GDALColorTable>
    LoadFromFile(const char *pszFilename);

    /** Convert a GDALColorTable* to a GDALRasterBandH.
     * @since GDAL 2.3
     */
    static inline GDALColorTableH ToHandle(GDALColorTable *poCT)
    {
        return static_cast<GDALColorTableH>(poCT);
    }

    /** Convert a GDALColorTableH to a GDALColorTable*.
     * @since GDAL 2.3
     */
    static inline GDALColorTable *FromHandle(GDALColorTableH hCT)
    {
        return static_cast<GDALColorTable *>(hCT);
    }
};

/* ******************************************************************** */
/*                       GDALAbstractBandBlockCache                     */
/* ******************************************************************** */

//! @cond Doxygen_Suppress

//! This manages how a raster band store its cached block.
// only used by GDALRasterBand implementation.

class GDALAbstractBandBlockCache
{
    // List of blocks that can be freed or recycled, and its lock
    CPLLock *hSpinLock = nullptr;
    GDALRasterBlock *psListBlocksToFree = nullptr;

    // Band keep alive counter, and its lock & condition
    CPLCond *hCond = nullptr;
    CPLMutex *hCondMutex = nullptr;
    volatile int nKeepAliveCounter = 0;

    volatile int m_nDirtyBlocks = 0;

    CPL_DISALLOW_COPY_ASSIGN(GDALAbstractBandBlockCache)

  protected:
    GDALRasterBand *poBand;

    int m_nInitialDirtyBlocksInFlushCache = 0;
    int m_nLastTick = -1;
    size_t m_nWriteDirtyBlocksDisabled = 0;

    void FreeDanglingBlocks();
    void UnreferenceBlockBase();

    void StartDirtyBlockFlushingLog();
    void UpdateDirtyBlockFlushingLog();
    void EndDirtyBlockFlushingLog();

  public:
    explicit GDALAbstractBandBlockCache(GDALRasterBand *poBand);
    virtual ~GDALAbstractBandBlockCache();

    GDALRasterBlock *CreateBlock(int nXBlockOff, int nYBlockOff);
    void AddBlockToFreeList(GDALRasterBlock *poBlock);
    void IncDirtyBlocks(int nInc);
    void WaitCompletionPendingTasks();

    void EnableDirtyBlockWriting()
    {
        --m_nWriteDirtyBlocksDisabled;
    }

    void DisableDirtyBlockWriting()
    {
        ++m_nWriteDirtyBlocksDisabled;
    }

    bool HasDirtyBlocks() const
    {
        return m_nDirtyBlocks > 0;
    }

    virtual bool Init() = 0;
    virtual bool IsInitOK() = 0;
    virtual CPLErr FlushCache() = 0;
    virtual CPLErr AdoptBlock(GDALRasterBlock *poBlock) = 0;
    virtual GDALRasterBlock *TryGetLockedBlockRef(int nXBlockOff,
                                                  int nYBlockYOff) = 0;
    virtual CPLErr UnreferenceBlock(GDALRasterBlock *poBlock) = 0;
    virtual CPLErr FlushBlock(int nXBlockOff, int nYBlockOff,
                              int bWriteDirtyBlock) = 0;
};

GDALAbstractBandBlockCache *
GDALArrayBandBlockCacheCreate(GDALRasterBand *poBand);
GDALAbstractBandBlockCache *
GDALHashSetBandBlockCacheCreate(GDALRasterBand *poBand);

//! @endcond

/* ******************************************************************** */
/*                            GDALRasterBand                            */
/* ******************************************************************** */

class GDALMDArray;
class GDALDoublePointsCache;

/** Range of values found in a mask band */
typedef enum
{
    GMVR_UNKNOWN, /*! Unknown (can also be used for any values between 0 and 255
                     for a Byte band) */
    GMVR_0_AND_1_ONLY,   /*! Only 0 and 1 */
    GMVR_0_AND_255_ONLY, /*! Only 0 and 255 */
} GDALMaskValueRange;

/** Suggested/most efficient access pattern to blocks. */
typedef int GDALSuggestedBlockAccessPattern;

/** Unknown, or no particular read order is suggested. */
constexpr GDALSuggestedBlockAccessPattern GSBAP_UNKNOWN = 0;

/** Random access to blocks is efficient. */
constexpr GDALSuggestedBlockAccessPattern GSBAP_RANDOM = 1;

/** Reading by strips from top to bottom is the most efficient. */
constexpr GDALSuggestedBlockAccessPattern GSBAP_TOP_TO_BOTTOM = 2;

/** Reading by strips from bottom to top is the most efficient. */
constexpr GDALSuggestedBlockAccessPattern GSBAP_BOTTOM_TO_TOP = 3;

/** Reading the largest chunk from the raster is the most efficient (can be
 * combined with above values). */
constexpr GDALSuggestedBlockAccessPattern GSBAP_LARGEST_CHUNK_POSSIBLE = 0x100;

class GDALComputedRasterBand;

/** A rectangular subset of pixels within a raster */
class GDALRasterWindow
{
  public:
    /** left offset of the window */
    int nXOff;

    /** top offset of the window */
    int nYOff;

    /** window width */
    int nXSize;

    /** window height */
    int nYSize;
};

/** A single raster band (or channel). */

class CPL_DLL GDALRasterBand : public GDALMajorObject
{
  private:
    friend class GDALArrayBandBlockCache;
    friend class GDALHashSetBandBlockCache;
    friend class GDALRasterBlock;
    friend class GDALDataset;

    CPLErr eFlushBlockErr = CE_None;
    GDALAbstractBandBlockCache *poBandBlockCache = nullptr;

    CPL_INTERNAL void SetFlushBlockErr(CPLErr eErr);
    CPL_INTERNAL CPLErr UnreferenceBlock(GDALRasterBlock *poBlock);
    CPL_INTERNAL void IncDirtyBlocks(int nInc);

    CPL_INTERNAL CPLErr RasterIOInternal(
        GDALRWFlag eRWFlag, int nXOff, int nYOff, int nXSize, int nYSize,
        void *pData, int nBufXSize, int nBufYSize, GDALDataType eBufType,
        GSpacing nPixelSpace, GSpacing nLineSpace,
        GDALRasterIOExtraArg *psExtraArg) CPL_WARN_UNUSED_RESULT;

  protected:
    //! @cond Doxygen_Suppress
    GDALDataset *poDS = nullptr;
    int nBand = 0; /* 1 based */

    int nRasterXSize = 0;
    int nRasterYSize = 0;

    GDALDataType eDataType = GDT_Byte;
    GDALAccess eAccess = GA_ReadOnly;

    /* stuff related to blocking, and raster cache */
    int nBlockXSize = -1;
    int nBlockYSize = -1;
    int nBlocksPerRow = 0;
    int nBlocksPerColumn = 0;

    int nBlockReads = 0;
    int bForceCachedIO = 0;

    friend class GDALComputedRasterBand;
    friend class GDALComputedDataset;

    class GDALRasterBandOwnedOrNot
    {
      public:
        GDALRasterBandOwnedOrNot() = default;

        GDALRasterBandOwnedOrNot(GDALRasterBandOwnedOrNot &&) = default;

        void reset()
        {
            m_poBandOwned.reset();
            m_poBandRef = nullptr;
        }

        void resetNotOwned(GDALRasterBand *poBand)
        {
            m_poBandOwned.reset();
            m_poBandRef = poBand;
        }

        void reset(std::unique_ptr<GDALRasterBand> poBand)
        {
            m_poBandOwned = std::move(poBand);
            m_poBandRef = nullptr;
        }

        const GDALRasterBand *get() const
        {
            return static_cast<const GDALRasterBand *>(*this);
        }

        GDALRasterBand *get()
        {
            return static_cast<GDALRasterBand *>(*this);
        }

        bool IsOwned() const
        {
            return m_poBandOwned != nullptr;
        }

        operator const GDALRasterBand *() const
        {
            return m_poBandOwned ? m_poBandOwned.get() : m_poBandRef;
        }

        operator GDALRasterBand *()
        {
            return m_poBandOwned ? m_poBandOwned.get() : m_poBandRef;
        }

      private:
        CPL_DISALLOW_COPY_ASSIGN(GDALRasterBandOwnedOrNot)
        std::unique_ptr<GDALRasterBand> m_poBandOwned{};
        GDALRasterBand *m_poBandRef = nullptr;
    };

    GDALRasterBandOwnedOrNot poMask{};
    bool m_bEnablePixelTypeSignedByteWarning =
        true;  // Remove me in GDAL 4.0. See GetMetadataItem() implementation
    int nMaskFlags = 0;

    void InvalidateMaskBand();

    friend class GDALProxyRasterBand;
    friend class GDALDefaultOverviews;

    CPLErr
    RasterIOResampled(GDALRWFlag eRWFlag, int nXOff, int nYOff, int nXSize,
                      int nYSize, void *pData, int nBufXSize, int nBufYSize,
                      GDALDataType eBufType, GSpacing nPixelSpace,
                      GSpacing nLineSpace,
                      GDALRasterIOExtraArg *psExtraArg) CPL_WARN_UNUSED_RESULT;

    int EnterReadWrite(GDALRWFlag eRWFlag);
    void LeaveReadWrite();
    void InitRWLock();
    void SetValidPercent(GUIntBig nSampleCount, GUIntBig nValidCount);

    mutable GDALDoublePointsCache *m_poPointsCache = nullptr;

    //! @endcond

  protected:
    virtual CPLErr IReadBlock(int nBlockXOff, int nBlockYOff, void *pData) = 0;
    virtual CPLErr IWriteBlock(int nBlockXOff, int nBlockYOff, void *pData);

    virtual CPLErr
    IRasterIO(GDALRWFlag eRWFlag, int nXOff, int nYOff, int nXSize, int nYSize,
              void *pData, int nBufXSize, int nBufYSize, GDALDataType eBufType,
              GSpacing nPixelSpace, GSpacing nLineSpace,
              GDALRasterIOExtraArg *psExtraArg) CPL_WARN_UNUSED_RESULT;

    virtual int IGetDataCoverageStatus(int nXOff, int nYOff, int nXSize,
                                       int nYSize, int nMaskFlagStop,
                                       double *pdfDataPct);

    virtual bool
    EmitErrorMessageIfWriteNotSupported(const char *pszCaller) const;

    //! @cond Doxygen_Suppress
    CPLErr
    OverviewRasterIO(GDALRWFlag eRWFlag, int nXOff, int nYOff, int nXSize,
                     int nYSize, void *pData, int nBufXSize, int nBufYSize,
                     GDALDataType eBufType, GSpacing nPixelSpace,
                     GSpacing nLineSpace,
                     GDALRasterIOExtraArg *psExtraArg) CPL_WARN_UNUSED_RESULT;

    CPLErr TryOverviewRasterIO(GDALRWFlag eRWFlag, int nXOff, int nYOff,
                               int nXSize, int nYSize, void *pData,
                               int nBufXSize, int nBufYSize,
                               GDALDataType eBufType, GSpacing nPixelSpace,
                               GSpacing nLineSpace,
                               GDALRasterIOExtraArg *psExtraArg, int *pbTried);

    CPLErr SplitRasterIO(GDALRWFlag eRWFlag, int nXOff, int nYOff, int nXSize,
                         int nYSize, void *pData, int nBufXSize, int nBufYSize,
                         GDALDataType eBufType, GSpacing nPixelSpace,
                         GSpacing nLineSpace, GDALRasterIOExtraArg *psExtraArg)
        CPL_WARN_UNUSED_RESULT;

    int InitBlockInfo();

    void AddBlockToFreeList(GDALRasterBlock *);

    bool HasBlockCache() const
    {
        return poBandBlockCache != nullptr;
    }

    bool HasDirtyBlocks() const
    {
        return poBandBlockCache && poBandBlockCache->HasDirtyBlocks();
    }

    //! @endcond

  public:
    GDALRasterBand();
    explicit GDALRasterBand(int bForceCachedIO);

    //! @cond Doxygen_Suppress
    GDALRasterBand(GDALRasterBand &&) = default;
    //! @endcond

    ~GDALRasterBand() override;

    int GetXSize() const;
    int GetYSize() const;
    int GetBand() const;
    GDALDataset *GetDataset() const;

    GDALDataType GetRasterDataType(void) const;
    void GetBlockSize(int *pnXSize, int *pnYSize) const;
    CPLErr GetActualBlockSize(int nXBlockOff, int nYBlockOff, int *pnXValid,
                              int *pnYValid) const;

    virtual GDALSuggestedBlockAccessPattern
    GetSuggestedBlockAccessPattern() const;

    GDALAccess GetAccess();

#ifndef DOXYGEN_SKIP
    CPLErr RasterIO(GDALRWFlag eRWFlag, int nXOff, int nYOff, int nXSize,
                    int nYSize, void *pData, int nBufXSize, int nBufYSize,
                    GDALDataType eBufType, GSpacing nPixelSpace,
                    GSpacing nLineSpace,
                    GDALRasterIOExtraArg *psExtraArg
                        OPTIONAL_OUTSIDE_GDAL(nullptr)) CPL_WARN_UNUSED_RESULT;
#else
    CPLErr RasterIO(GDALRWFlag eRWFlag, int nXOff, int nYOff, int nXSize,
                    int nYSize, void *pData, int nBufXSize, int nBufYSize,
                    GDALDataType eBufType, GSpacing nPixelSpace,
                    GSpacing nLineSpace,
                    GDALRasterIOExtraArg *psExtraArg) CPL_WARN_UNUSED_RESULT;
#endif

    template <class T>
    CPLErr ReadRaster(T *pData, size_t nArrayEltCount = 0, double dfXOff = 0,
                      double dfYOff = 0, double dfXSize = 0, double dfYSize = 0,
                      size_t nBufXSize = 0, size_t nBufYSize = 0,
                      GDALRIOResampleAlg eResampleAlg = GRIORA_NearestNeighbour,
                      GDALProgressFunc pfnProgress = nullptr,
                      void *pProgressData = nullptr) const;

    template <class T>
    CPLErr ReadRaster(std::vector<T> &vData, double dfXOff = 0,
                      double dfYOff = 0, double dfXSize = 0, double dfYSize = 0,
                      size_t nBufXSize = 0, size_t nBufYSize = 0,
                      GDALRIOResampleAlg eResampleAlg = GRIORA_NearestNeighbour,
                      GDALProgressFunc pfnProgress = nullptr,
                      void *pProgressData = nullptr) const;

#if __cplusplus >= 202002L
    //! @cond Doxygen_Suppress
    template <class T>
    inline CPLErr
    ReadRaster(std::span<T> pData, double dfXOff = 0, double dfYOff = 0,
               double dfXSize = 0, double dfYSize = 0, size_t nBufXSize = 0,
               size_t nBufYSize = 0,
               GDALRIOResampleAlg eResampleAlg = GRIORA_NearestNeighbour,
               GDALProgressFunc pfnProgress = nullptr,
               void *pProgressData = nullptr) const
    {
        return ReadRaster(pData.data(), pData.size(), dfXOff, dfYOff, dfXSize,
                          dfYSize, nBufXSize, nBufYSize, eResampleAlg,
                          pfnProgress, pProgressData);
    }

    //! @endcond
#endif

    GDALComputedRasterBand
    operator+(const GDALRasterBand &other) const CPL_WARN_UNUSED_RESULT;
    GDALComputedRasterBand operator+(double cst) const CPL_WARN_UNUSED_RESULT;
    friend GDALComputedRasterBand CPL_DLL
    operator+(double cst, const GDALRasterBand &other) CPL_WARN_UNUSED_RESULT;

    GDALComputedRasterBand
    operator-(const GDALRasterBand &other) const CPL_WARN_UNUSED_RESULT;
    GDALComputedRasterBand operator-(double cst) const CPL_WARN_UNUSED_RESULT;
    friend GDALComputedRasterBand CPL_DLL
    operator-(double cst, const GDALRasterBand &other) CPL_WARN_UNUSED_RESULT;

    GDALComputedRasterBand
    operator*(const GDALRasterBand &other) const CPL_WARN_UNUSED_RESULT;
    GDALComputedRasterBand operator*(double cst) const CPL_WARN_UNUSED_RESULT;
    friend GDALComputedRasterBand CPL_DLL
    operator*(double cst, const GDALRasterBand &other) CPL_WARN_UNUSED_RESULT;

    GDALComputedRasterBand
    operator/(const GDALRasterBand &other) const CPL_WARN_UNUSED_RESULT;
    GDALComputedRasterBand operator/(double cst) const CPL_WARN_UNUSED_RESULT;
    friend GDALComputedRasterBand CPL_DLL
    operator/(double cst, const GDALRasterBand &other) CPL_WARN_UNUSED_RESULT;

    GDALComputedRasterBand
    operator>(const GDALRasterBand &other) const CPL_WARN_UNUSED_RESULT;
    GDALComputedRasterBand operator>(double cst) const CPL_WARN_UNUSED_RESULT;
    friend GDALComputedRasterBand CPL_DLL
    operator>(double cst, const GDALRasterBand &other) CPL_WARN_UNUSED_RESULT;

    GDALComputedRasterBand
    operator>=(const GDALRasterBand &other) const CPL_WARN_UNUSED_RESULT;
    GDALComputedRasterBand operator>=(double cst) const CPL_WARN_UNUSED_RESULT;
    friend GDALComputedRasterBand CPL_DLL
    operator>=(double cst, const GDALRasterBand &other) CPL_WARN_UNUSED_RESULT;

    GDALComputedRasterBand
    operator<(const GDALRasterBand &other) const CPL_WARN_UNUSED_RESULT;
    GDALComputedRasterBand operator<(double cst) const CPL_WARN_UNUSED_RESULT;
    friend GDALComputedRasterBand CPL_DLL
    operator<(double cst, const GDALRasterBand &other) CPL_WARN_UNUSED_RESULT;

    GDALComputedRasterBand
    operator<=(const GDALRasterBand &other) const CPL_WARN_UNUSED_RESULT;
    GDALComputedRasterBand operator<=(double cst) const CPL_WARN_UNUSED_RESULT;
    friend GDALComputedRasterBand CPL_DLL
    operator<=(double cst, const GDALRasterBand &other) CPL_WARN_UNUSED_RESULT;

    GDALComputedRasterBand
    operator==(const GDALRasterBand &other) const CPL_WARN_UNUSED_RESULT;
    GDALComputedRasterBand operator==(double cst) const CPL_WARN_UNUSED_RESULT;
    friend GDALComputedRasterBand CPL_DLL
    operator==(double cst, const GDALRasterBand &other) CPL_WARN_UNUSED_RESULT;

    GDALComputedRasterBand
    operator!=(const GDALRasterBand &other) const CPL_WARN_UNUSED_RESULT;
    GDALComputedRasterBand operator!=(double cst) const CPL_WARN_UNUSED_RESULT;
    friend GDALComputedRasterBand CPL_DLL
    operator!=(double cst, const GDALRasterBand &other) CPL_WARN_UNUSED_RESULT;

#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Weffc++"
#endif

    GDALComputedRasterBand
    operator&&(const GDALRasterBand &other) const CPL_WARN_UNUSED_RESULT;
    GDALComputedRasterBand operator&&(bool cst) const CPL_WARN_UNUSED_RESULT;
    friend GDALComputedRasterBand CPL_DLL
    operator&&(bool cst, const GDALRasterBand &other) CPL_WARN_UNUSED_RESULT;

    GDALComputedRasterBand
    operator||(const GDALRasterBand &other) const CPL_WARN_UNUSED_RESULT;
    GDALComputedRasterBand operator||(bool cst) const CPL_WARN_UNUSED_RESULT;
    friend GDALComputedRasterBand CPL_DLL
    operator||(bool cst, const GDALRasterBand &other) CPL_WARN_UNUSED_RESULT;

#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

    GDALComputedRasterBand operator!() const CPL_WARN_UNUSED_RESULT;

    GDALComputedRasterBand operator-() const CPL_WARN_UNUSED_RESULT;

    GDALComputedRasterBand AsType(GDALDataType) const CPL_WARN_UNUSED_RESULT;

    CPLErr ReadBlock(int nXBlockOff, int nYBlockOff,
                     void *pImage) CPL_WARN_UNUSED_RESULT;

    CPLErr WriteBlock(int nXBlockOff, int nYBlockOff,
                      void *pImage) CPL_WARN_UNUSED_RESULT;

    // This method should only be overloaded by GDALProxyRasterBand
    virtual GDALRasterBlock *
    GetLockedBlockRef(int nXBlockOff, int nYBlockOff,
                      int bJustInitialize = FALSE) CPL_WARN_UNUSED_RESULT;

    // This method should only be overloaded by GDALProxyRasterBand
    virtual GDALRasterBlock *
    TryGetLockedBlockRef(int nXBlockOff,
                         int nYBlockYOff) CPL_WARN_UNUSED_RESULT;

    // This method should only be overloaded by GDALProxyRasterBand
    virtual CPLErr FlushBlock(int nXBlockOff, int nYBlockOff,
                              int bWriteDirtyBlock = TRUE);

    unsigned char *
    GetIndexColorTranslationTo(/* const */ GDALRasterBand *poReferenceBand,
                               unsigned char *pTranslationTable = nullptr,
                               int *pApproximateMatching = nullptr);

    // New OpengIS CV_SampleDimension stuff.

    virtual CPLErr FlushCache(bool bAtClosing = false);
    virtual CPLErr DropCache();
    virtual char **GetCategoryNames();
    virtual double GetNoDataValue(int *pbSuccess = nullptr);
    virtual int64_t GetNoDataValueAsInt64(int *pbSuccess = nullptr);
    virtual uint64_t GetNoDataValueAsUInt64(int *pbSuccess = nullptr);
    virtual double GetMinimum(int *pbSuccess = nullptr);
    virtual double GetMaximum(int *pbSuccess = nullptr);
    virtual double GetOffset(int *pbSuccess = nullptr);
    virtual double GetScale(int *pbSuccess = nullptr);
    virtual const char *GetUnitType();
    virtual GDALColorInterp GetColorInterpretation();
    virtual GDALColorTable *GetColorTable();
    virtual CPLErr Fill(double dfRealValue, double dfImaginaryValue = 0);

    virtual CPLErr SetCategoryNames(char **papszNames);
    virtual CPLErr SetNoDataValue(double dfNoData);
    virtual CPLErr SetNoDataValueAsInt64(int64_t nNoData);
    virtual CPLErr SetNoDataValueAsUInt64(uint64_t nNoData);
    CPLErr SetNoDataValueAsString(const char *pszNoData,
                                  bool *pbCannotBeExactlyRepresented = nullptr);
    virtual CPLErr DeleteNoDataValue();
    virtual CPLErr SetColorTable(GDALColorTable *poCT);
    virtual CPLErr SetColorInterpretation(GDALColorInterp eColorInterp);
    virtual CPLErr SetOffset(double dfNewOffset);
    virtual CPLErr SetScale(double dfNewScale);
    virtual CPLErr SetUnitType(const char *pszNewValue);

    virtual CPLErr GetStatistics(int bApproxOK, int bForce, double *pdfMin,
                                 double *pdfMax, double *pdfMean,
                                 double *padfStdDev);
    virtual CPLErr ComputeStatistics(int bApproxOK, double *pdfMin,
                                     double *pdfMax, double *pdfMean,
                                     double *pdfStdDev, GDALProgressFunc,
                                     void *pProgressData);
    virtual CPLErr SetStatistics(double dfMin, double dfMax, double dfMean,
                                 double dfStdDev);
    virtual CPLErr ComputeRasterMinMax(int bApproxOK, double *adfMinMax);
    virtual CPLErr ComputeRasterMinMaxLocation(double *pdfMin, double *pdfMax,
                                               int *pnMinX, int *pnMinY,
                                               int *pnMaxX, int *pnMaxY);

// Only defined when Doxygen enabled
#ifdef DOXYGEN_SKIP
    CPLErr SetMetadata(char **papszMetadata, const char *pszDomain) override;
    CPLErr SetMetadataItem(const char *pszName, const char *pszValue,
                           const char *pszDomain) override;
#endif
    virtual const char *GetMetadataItem(const char *pszName,
                                        const char *pszDomain = "") override;

    virtual int HasArbitraryOverviews();
    virtual int GetOverviewCount();
    virtual GDALRasterBand *GetOverview(int i);
    virtual GDALRasterBand *GetRasterSampleOverview(GUIntBig);
    virtual CPLErr BuildOverviews(const char *pszResampling, int nOverviews,
                                  const int *panOverviewList,
                                  GDALProgressFunc pfnProgress,
                                  void *pProgressData,
                                  CSLConstList papszOptions);

    virtual CPLErr AdviseRead(int nXOff, int nYOff, int nXSize, int nYSize,
                              int nBufXSize, int nBufYSize,
                              GDALDataType eBufType, char **papszOptions);

    virtual CPLErr GetHistogram(double dfMin, double dfMax, int nBuckets,
                                GUIntBig *panHistogram, int bIncludeOutOfRange,
                                int bApproxOK, GDALProgressFunc,
                                void *pProgressData);

    virtual CPLErr GetDefaultHistogram(double *pdfMin, double *pdfMax,
                                       int *pnBuckets, GUIntBig **ppanHistogram,
                                       int bForce, GDALProgressFunc,
                                       void *pProgressData);
    virtual CPLErr SetDefaultHistogram(double dfMin, double dfMax, int nBuckets,
                                       GUIntBig *panHistogram);

    virtual GDALRasterAttributeTable *GetDefaultRAT();
    virtual CPLErr SetDefaultRAT(const GDALRasterAttributeTable *poRAT);

    virtual GDALRasterBand *GetMaskBand();
    virtual int GetMaskFlags();
    virtual CPLErr CreateMaskBand(int nFlagsIn);
    virtual bool IsMaskBand() const;
    virtual GDALMaskValueRange GetMaskValueRange() const;

    virtual CPLVirtualMem *
    GetVirtualMemAuto(GDALRWFlag eRWFlag, int *pnPixelSpace,
                      GIntBig *pnLineSpace,
                      char **papszOptions) CPL_WARN_UNUSED_RESULT;

    int GetDataCoverageStatus(int nXOff, int nYOff, int nXSize, int nYSize,
                              int nMaskFlagStop = 0,
                              double *pdfDataPct = nullptr);

    std::shared_ptr<GDALMDArray> AsMDArray() const;

    CPLErr InterpolateAtGeolocation(
        double dfGeolocX, double dfGeolocY, const OGRSpatialReference *poSRS,
        GDALRIOResampleAlg eInterpolation, double *pdfRealValue,
        double *pdfImagValue = nullptr,
        CSLConstList papszTransformerOptions = nullptr) const;

    virtual CPLErr InterpolateAtPoint(double dfPixel, double dfLine,
                                      GDALRIOResampleAlg eInterpolation,
                                      double *pdfRealValue,
                                      double *pdfImagValue = nullptr) const;

    //! @cond Doxygen_Suppress
    class CPL_DLL WindowIterator
    {
      public:
        using iterator_category = std::input_iterator_tag;
        using difference_type = std::ptrdiff_t;

        using value_type = GDALRasterWindow;
        using pointer = value_type *;
        using reference = value_type &;

        WindowIterator(int nRasterXSize, int nRasterYSize, int nBlockXSize,
                       int nBlockYSize, int nRow, int nCol);

        bool operator==(const WindowIterator &other) const;

        bool operator!=(const WindowIterator &other) const;

        value_type operator*() const;

        WindowIterator &operator++();

      private:
        const int m_nRasterXSize;
        const int m_nRasterYSize;
        const int m_nBlockXSize;
        const int m_nBlockYSize;
        int m_row;
        int m_col;
    };

    class CPL_DLL WindowIteratorWrapper
    {
      public:
        explicit WindowIteratorWrapper(const GDALRasterBand &band);

        WindowIterator begin() const;

        WindowIterator end() const;

      private:
        const int m_nRasterXSize;
        const int m_nRasterYSize;
        int m_nBlockXSize;
        int m_nBlockYSize;
    };

    //! @endcond

    WindowIteratorWrapper IterateWindows() const;

#ifndef DOXYGEN_XML
    void ReportError(CPLErr eErrClass, CPLErrorNum err_no, const char *fmt,
                     ...) const CPL_PRINT_FUNC_FORMAT(4, 5);
#endif

    //! @cond Doxygen_Suppress
    static void ThrowIfNotSameDimensions(const GDALRasterBand &first,
                                         const GDALRasterBand &second);

    //! @endcond

    /** Convert a GDALRasterBand* to a GDALRasterBandH.
     * @since GDAL 2.3
     */
    static inline GDALRasterBandH ToHandle(GDALRasterBand *poBand)
    {
        return static_cast<GDALRasterBandH>(poBand);
    }

    /** Convert a GDALRasterBandH to a GDALRasterBand*.
     * @since GDAL 2.3
     */
    static inline GDALRasterBand *FromHandle(GDALRasterBandH hBand)
    {
        return static_cast<GDALRasterBand *>(hBand);
    }

    //! @cond Doxygen_Suppress
    // Remove me in GDAL 4.0. See GetMetadataItem() implementation
    // Internal use in GDAL only !
    virtual void EnablePixelTypeSignedByteWarning(bool b)
#ifndef GDAL_COMPILATION
        CPL_WARN_DEPRECATED("Do not use that method outside of GDAL!")
#endif
            ;

    //! @endcond

  private:
    CPL_DISALLOW_COPY_ASSIGN(GDALRasterBand)
};

//! @cond Doxygen_Suppress
#define GDAL_EXTERN_TEMPLATE_READ_RASTER(T)                                    \
    extern template CPLErr GDALRasterBand::ReadRaster<T>(                      \
        T * pData, size_t nArrayEltCount, double dfXOff, double dfYOff,        \
        double dfXSize, double dfYSize, size_t nBufXSize, size_t nBufYSize,    \
        GDALRIOResampleAlg eResampleAlg, GDALProgressFunc pfnProgress,         \
        void *pProgressData) const;

GDAL_EXTERN_TEMPLATE_READ_RASTER(uint8_t)
GDAL_EXTERN_TEMPLATE_READ_RASTER(int8_t)
GDAL_EXTERN_TEMPLATE_READ_RASTER(uint16_t)
GDAL_EXTERN_TEMPLATE_READ_RASTER(int16_t)
GDAL_EXTERN_TEMPLATE_READ_RASTER(uint32_t)
GDAL_EXTERN_TEMPLATE_READ_RASTER(int32_t)
GDAL_EXTERN_TEMPLATE_READ_RASTER(uint64_t)
GDAL_EXTERN_TEMPLATE_READ_RASTER(int64_t)
#ifdef CPL_FLOAT_H_INCLUDED
GDAL_EXTERN_TEMPLATE_READ_RASTER(GFloat16)
#endif
GDAL_EXTERN_TEMPLATE_READ_RASTER(float)
GDAL_EXTERN_TEMPLATE_READ_RASTER(double)
// Not allowed by C++ standard
// GDAL_EXTERN_TEMPLATE_READ_RASTER(std::complex<int16_t>)
// GDAL_EXTERN_TEMPLATE_READ_RASTER(std::complex<int32_t>)
GDAL_EXTERN_TEMPLATE_READ_RASTER(std::complex<float>)
GDAL_EXTERN_TEMPLATE_READ_RASTER(std::complex<double>)

#define GDAL_EXTERN_TEMPLATE_READ_RASTER_VECTOR(T)                             \
    extern template CPLErr GDALRasterBand::ReadRaster<T>(                      \
        std::vector<T> & vData, double dfXOff, double dfYOff, double dfXSize,  \
        double dfYSize, size_t nBufXSize, size_t nBufYSize,                    \
        GDALRIOResampleAlg eResampleAlg, GDALProgressFunc pfnProgress,         \
        void *pProgressData) const;

GDAL_EXTERN_TEMPLATE_READ_RASTER_VECTOR(uint8_t)
GDAL_EXTERN_TEMPLATE_READ_RASTER_VECTOR(int8_t)
GDAL_EXTERN_TEMPLATE_READ_RASTER_VECTOR(uint16_t)
GDAL_EXTERN_TEMPLATE_READ_RASTER_VECTOR(int16_t)
GDAL_EXTERN_TEMPLATE_READ_RASTER_VECTOR(uint32_t)
GDAL_EXTERN_TEMPLATE_READ_RASTER_VECTOR(int32_t)
GDAL_EXTERN_TEMPLATE_READ_RASTER_VECTOR(uint64_t)
GDAL_EXTERN_TEMPLATE_READ_RASTER_VECTOR(int64_t)
#ifdef CPL_FLOAT_H_INCLUDED
GDAL_EXTERN_TEMPLATE_READ_RASTER_VECTOR(GFloat16)
#endif
GDAL_EXTERN_TEMPLATE_READ_RASTER_VECTOR(float)
GDAL_EXTERN_TEMPLATE_READ_RASTER_VECTOR(double)
// Not allowed by C++ standard
// GDAL_EXTERN_TEMPLATE_READ_RASTER_VECTOR(std::complex<int16_t>)
// GDAL_EXTERN_TEMPLATE_READ_RASTER_VECTOR(std::complex<int32_t>)
GDAL_EXTERN_TEMPLATE_READ_RASTER_VECTOR(std::complex<float>)
GDAL_EXTERN_TEMPLATE_READ_RASTER_VECTOR(std::complex<double>)

//! @endcond

/* ******************************************************************** */
/*                       GDALComputedRasterBand                         */
/* ******************************************************************** */

/** Class represented the result of an operation on one or two input bands.
 *
 * Such class is instantiated only by operators on GDALRasterBand.
 * The resulting band is lazy evaluated.
 *
 * @since 3.12
 */
class CPL_DLL GDALComputedRasterBand final : public GDALRasterBand
{
  public:
    /** Destructor */
    ~GDALComputedRasterBand() override;

    //! @cond Doxygen_Suppress
    enum class Operation
    {
        OP_ADD,
        OP_SUBTRACT,
        OP_MULTIPLY,
        OP_DIVIDE,
        OP_MIN,
        OP_MAX,
        OP_MEAN,
        OP_GT,
        OP_GE,
        OP_LT,
        OP_LE,
        OP_EQ,
        OP_NE,
        OP_LOGICAL_AND,
        OP_LOGICAL_OR,
        OP_CAST,
        OP_TERNARY,
        OP_ABS,
        OP_SQRT,
        OP_LOG,
        OP_LOG10,
        OP_POW,
    };

    GDALComputedRasterBand(
        Operation op, const std::vector<const GDALRasterBand *> &bands,
        double constant = std::numeric_limits<double>::quiet_NaN());
    GDALComputedRasterBand(Operation op, const GDALRasterBand &band);
    GDALComputedRasterBand(Operation op, double constant,
                           const GDALRasterBand &band);
    GDALComputedRasterBand(Operation op, const GDALRasterBand &band,
                           double constant);
    GDALComputedRasterBand(Operation op, const GDALRasterBand &band,
                           GDALDataType dt);

    // Semi-public for gdal::min(), gdal::max()
    GDALComputedRasterBand(Operation op, const GDALRasterBand &firstBand,
                           const GDALRasterBand &secondBand);

    GDALComputedRasterBand(GDALComputedRasterBand &&) = default;

    //! @endcond

    double GetNoDataValue(int *pbSuccess = nullptr) override;

    /** Convert a GDALComputedRasterBand* to a GDALComputedRasterBandH.
     */
    static inline GDALComputedRasterBandH
    ToHandle(GDALComputedRasterBand *poBand)
    {
        return static_cast<GDALComputedRasterBandH>(poBand);
    }

    /** Convert a GDALComputedRasterBandH to a GDALComputedRasterBand*.
     */
    static inline GDALComputedRasterBand *
    FromHandle(GDALComputedRasterBandH hBand)
    {
        return static_cast<GDALComputedRasterBand *>(hBand);
    }

  protected:
    friend class GDALRasterBand;

    CPLErr IReadBlock(int, int, void *) override;

    CPLErr IRasterIO(GDALRWFlag eRWFlag, int nXOff, int nYOff, int nXSize,
                     int nYSize, void *pData, int nBufXSize, int nBufYSize,
                     GDALDataType eBufType, GSpacing nPixelSpace,
                     GSpacing nLineSpace,
                     GDALRasterIOExtraArg *psExtraArg) override;

  private:
    friend class GDALComputedDataset;
    std::unique_ptr<GDALDataset, GDALDatasetUniquePtrReleaser> m_poOwningDS{};
    bool m_bHasNoData{false};
    double m_dfNoDataValue{0};

    GDALComputedRasterBand(const GDALComputedRasterBand &, bool);
    GDALComputedRasterBand(const GDALComputedRasterBand &) = delete;
    GDALComputedRasterBand &operator=(const GDALComputedRasterBand &) = delete;
    GDALComputedRasterBand &operator=(GDALComputedRasterBand &&) = delete;
};

namespace gdal
{
using std::abs;
GDALComputedRasterBand CPL_DLL abs(const GDALRasterBand &band);

using std::fabs;
GDALComputedRasterBand CPL_DLL fabs(const GDALRasterBand &band);

using std::sqrt;
GDALComputedRasterBand CPL_DLL sqrt(const GDALRasterBand &band);

using std::log;
GDALComputedRasterBand CPL_DLL log(const GDALRasterBand &band);

using std::log10;
GDALComputedRasterBand CPL_DLL log10(const GDALRasterBand &band);

using std::pow;
GDALComputedRasterBand CPL_DLL pow(const GDALRasterBand &band, double constant);
#ifndef DOXYGEN_SKIP
GDALComputedRasterBand CPL_DLL pow(double constant, const GDALRasterBand &band);
GDALComputedRasterBand CPL_DLL pow(const GDALRasterBand &band1,
                                   const GDALRasterBand &band2);
#endif

GDALComputedRasterBand CPL_DLL IfThenElse(const GDALRasterBand &condBand,
                                          const GDALRasterBand &thenBand,
                                          const GDALRasterBand &elseBand);

//! @cond Doxygen_Suppress

GDALComputedRasterBand CPL_DLL IfThenElse(const GDALRasterBand &condBand,
                                          double thenValue,
                                          const GDALRasterBand &elseBand);

GDALComputedRasterBand CPL_DLL IfThenElse(const GDALRasterBand &condBand,
                                          const GDALRasterBand &thenBand,
                                          double elseValue);

GDALComputedRasterBand CPL_DLL IfThenElse(const GDALRasterBand &condBand,
                                          double thenValue, double elseValue);

//! @endcond

using std::max;
using std::min;

GDALComputedRasterBand CPL_DLL min(const GDALRasterBand &first,
                                   const GDALRasterBand &second);

//! @cond Doxygen_Suppress

namespace detail
{

template <typename U, typename Enable> struct minDealFirstArg;

template <typename U>
struct minDealFirstArg<
    U, typename std::enable_if<std::is_arithmetic<U>::value>::type>
{
    inline static void process(std::vector<const GDALRasterBand *> &,
                               double &constant, const U &first)
    {
        if (std::isnan(constant) || static_cast<double>(first) < constant)
            constant = static_cast<double>(first);
    }
};

template <typename U>
struct minDealFirstArg<
    U, typename std::enable_if<!std::is_arithmetic<U>::value>::type>
{
    inline static void process(std::vector<const GDALRasterBand *> &bands,
                               double &, const U &first)
    {
        if (!bands.empty())
            GDALRasterBand::ThrowIfNotSameDimensions(first, *(bands.front()));
        bands.push_back(&first);
    }
};

inline static GDALComputedRasterBand
minInternal(std::vector<const GDALRasterBand *> &bands, double constant)
{
    return GDALComputedRasterBand(GDALComputedRasterBand::Operation::OP_MIN,
                                  bands, constant);
}

template <typename U, typename... V>
GDALComputedRasterBand minInternal(std::vector<const GDALRasterBand *> &bands,
                                   double constant, const U &first, V &&...rest)
{
    minDealFirstArg<U, void>::process(bands, constant, first);
    return minInternal(bands, constant, std::forward<V>(rest)...);
}

}  // namespace detail

template <typename U, typename... V>
inline GDALComputedRasterBand min(const U &first, V &&...rest)
{
    std::vector<const GDALRasterBand *> bands;
    return detail::minInternal(bands, std::numeric_limits<double>::quiet_NaN(),
                               first, std::forward<V>(rest)...);
}

//! @endcond

GDALComputedRasterBand CPL_DLL max(const GDALRasterBand &first,
                                   const GDALRasterBand &second);

//! @cond Doxygen_Suppress

namespace detail
{

template <typename U, typename Enable> struct maxDealFirstArg;

template <typename U>
struct maxDealFirstArg<
    U, typename std::enable_if<std::is_arithmetic<U>::value>::type>
{
    inline static void process(std::vector<const GDALRasterBand *> &,
                               double &constant, const U &first)
    {
        if (std::isnan(constant) || static_cast<double>(first) > constant)
            constant = static_cast<double>(first);
    }
};

template <typename U>
struct maxDealFirstArg<
    U, typename std::enable_if<!std::is_arithmetic<U>::value>::type>
{
    inline static void process(std::vector<const GDALRasterBand *> &bands,
                               double &, const U &first)
    {
        if (!bands.empty())
            GDALRasterBand::ThrowIfNotSameDimensions(first, *(bands.front()));
        bands.push_back(&first);
    }
};

inline static GDALComputedRasterBand
maxInternal(std::vector<const GDALRasterBand *> &bands, double constant)
{
    return GDALComputedRasterBand(GDALComputedRasterBand::Operation::OP_MAX,
                                  bands, constant);
}

template <typename U, typename... V>
GDALComputedRasterBand maxInternal(std::vector<const GDALRasterBand *> &bands,
                                   double constant, const U &first, V &&...rest)
{
    maxDealFirstArg<U, void>::process(bands, constant, first);
    return maxInternal(bands, constant, std::forward<V>(rest)...);
}

}  // namespace detail

template <typename U, typename... V>
inline GDALComputedRasterBand max(const U &first, V &&...rest)
{
    std::vector<const GDALRasterBand *> bands;
    return detail::maxInternal(bands, std::numeric_limits<double>::quiet_NaN(),
                               first, std::forward<V>(rest)...);
}

//! @endcond

GDALComputedRasterBand CPL_DLL mean(const GDALRasterBand &first,
                                    const GDALRasterBand &second);

//! @cond Doxygen_Suppress
inline GDALComputedRasterBand
meanInternal(std::vector<const GDALRasterBand *> &bands)
{
    return GDALComputedRasterBand(GDALComputedRasterBand::Operation::OP_MEAN,
                                  bands);
}

template <typename U, typename... V>
inline GDALComputedRasterBand
meanInternal(std::vector<const GDALRasterBand *> &bands, const U &first,
             V &&...rest)
{
    if (!bands.empty())
        GDALRasterBand::ThrowIfNotSameDimensions(first, *(bands.front()));
    bands.push_back(&first);
    return meanInternal(bands, std::forward<V>(rest)...);
}

template <typename... Args> inline GDALComputedRasterBand mean(Args &&...args)
{
    std::vector<const GDALRasterBand *> bands;
    return meanInternal(bands, std::forward<Args>(args)...);
}

//! @endcond

}  // namespace gdal

//! @cond Doxygen_Suppress
/* ******************************************************************** */
/*                         GDALAllValidMaskBand                         */
/* ******************************************************************** */

class CPL_DLL GDALAllValidMaskBand : public GDALRasterBand
{
  protected:
    CPLErr IReadBlock(int, int, void *) override;

    CPLErr IRasterIO(GDALRWFlag eRWFlag, int nXOff, int nYOff, int nXSize,
                     int nYSize, void *pData, int nBufXSize, int nBufYSize,
                     GDALDataType eBufType, GSpacing nPixelSpace,
                     GSpacing nLineSpace,
                     GDALRasterIOExtraArg *psExtraArg) override;

    bool
    EmitErrorMessageIfWriteNotSupported(const char *pszCaller) const override;

    CPL_DISALLOW_COPY_ASSIGN(GDALAllValidMaskBand)

  public:
    explicit GDALAllValidMaskBand(GDALRasterBand *);
    ~GDALAllValidMaskBand() override;

    GDALRasterBand *GetMaskBand() override;
    int GetMaskFlags() override;

    bool IsMaskBand() const override
    {
        return true;
    }

    GDALMaskValueRange GetMaskValueRange() const override
    {
        return GMVR_0_AND_255_ONLY;
    }

    CPLErr ComputeStatistics(int bApproxOK, double *pdfMin, double *pdfMax,
                             double *pdfMean, double *pdfStdDev,
                             GDALProgressFunc, void *pProgressData) override;
};

/* ******************************************************************** */
/*                         GDALNoDataMaskBand                           */
/* ******************************************************************** */

class CPL_DLL GDALNoDataMaskBand : public GDALRasterBand
{
    friend class GDALRasterBand;
    double m_dfNoDataValue = 0;
    int64_t m_nNoDataValueInt64 = 0;
    uint64_t m_nNoDataValueUInt64 = 0;
    GDALRasterBand *m_poParent = nullptr;

    CPL_DISALLOW_COPY_ASSIGN(GDALNoDataMaskBand)

  protected:
    CPLErr IReadBlock(int, int, void *) override;
    CPLErr IRasterIO(GDALRWFlag, int, int, int, int, void *, int, int,
                     GDALDataType, GSpacing, GSpacing,
                     GDALRasterIOExtraArg *psExtraArg) override;

    bool
    EmitErrorMessageIfWriteNotSupported(const char *pszCaller) const override;

  public:
    explicit GDALNoDataMaskBand(GDALRasterBand *);
    explicit GDALNoDataMaskBand(GDALRasterBand *, double dfNoDataValue);
    ~GDALNoDataMaskBand() override;

    bool IsMaskBand() const override
    {
        return true;
    }

    GDALMaskValueRange GetMaskValueRange() const override
    {
        return GMVR_0_AND_255_ONLY;
    }

    static bool IsNoDataInRange(double dfNoDataValue, GDALDataType eDataType);
};

/* ******************************************************************** */
/*                  GDALNoDataValuesMaskBand                            */
/* ******************************************************************** */

class CPL_DLL GDALNoDataValuesMaskBand : public GDALRasterBand
{
    double *padfNodataValues;

    CPL_DISALLOW_COPY_ASSIGN(GDALNoDataValuesMaskBand)

  protected:
    CPLErr IReadBlock(int, int, void *) override;

    bool
    EmitErrorMessageIfWriteNotSupported(const char *pszCaller) const override;

  public:
    explicit GDALNoDataValuesMaskBand(GDALDataset *);
    ~GDALNoDataValuesMaskBand() override;

    bool IsMaskBand() const override
    {
        return true;
    }

    GDALMaskValueRange GetMaskValueRange() const override
    {
        return GMVR_0_AND_255_ONLY;
    }
};

/* ******************************************************************** */
/*                         GDALRescaledAlphaBand                        */
/* ******************************************************************** */

class GDALRescaledAlphaBand : public GDALRasterBand
{
    GDALRasterBand *poParent;
    void *pTemp;

    CPL_DISALLOW_COPY_ASSIGN(GDALRescaledAlphaBand)

  protected:
    CPLErr IReadBlock(int, int, void *) override;
    CPLErr IRasterIO(GDALRWFlag, int, int, int, int, void *, int, int,
                     GDALDataType, GSpacing, GSpacing,
                     GDALRasterIOExtraArg *psExtraArg) override;

    bool
    EmitErrorMessageIfWriteNotSupported(const char *pszCaller) const override;

  public:
    explicit GDALRescaledAlphaBand(GDALRasterBand *);
    ~GDALRescaledAlphaBand() override;

    bool IsMaskBand() const override
    {
        return true;
    }
};

//! @endcond

/* ******************************************************************** */
/*                          GDALIdentifyEnum                            */
/* ******************************************************************** */

/**
 * Enumeration used by GDALDriver::pfnIdentify().
 *
 * @since GDAL 2.1
 */
typedef enum
{
    /** Identify could not determine if the file is recognized or not by the
       probed driver. */
    GDAL_IDENTIFY_UNKNOWN = -1,
    /** Identify determined the file is not recognized by the probed driver. */
    GDAL_IDENTIFY_FALSE = 0,
    /** Identify determined the file is recognized by the probed driver. */
    GDAL_IDENTIFY_TRUE = 1
} GDALIdentifyEnum;

/* ******************************************************************** */
/*                              GDALDriver                              */
/* ******************************************************************** */

/**
 * \brief Format specific driver.
 *
 * An instance of this class is created for each supported format, and
 * manages information about the format.
 *
 * This roughly corresponds to a file format, though some
 * drivers may be gateways to many formats through a secondary
 * multi-library.
 */

class CPL_DLL GDALDriver : public GDALMajorObject
{
  public:
    GDALDriver();
    ~GDALDriver() override;

    CPLErr SetMetadataItem(const char *pszName, const char *pszValue,
                           const char *pszDomain = "") override;

    /* -------------------------------------------------------------------- */
    /*      Public C++ methods.                                             */
    /* -------------------------------------------------------------------- */
    GDALDataset *Create(const char *pszName, int nXSize, int nYSize, int nBands,
                        GDALDataType eType,
                        CSLConstList papszOptions) CPL_WARN_UNUSED_RESULT;

    GDALDataset *
    CreateMultiDimensional(const char *pszName,
                           CSLConstList papszRootGroupOptions,
                           CSLConstList papszOptions) CPL_WARN_UNUSED_RESULT;

    CPLErr Delete(const char *pszName);
    CPLErr Rename(const char *pszNewName, const char *pszOldName);
    CPLErr CopyFiles(const char *pszNewName, const char *pszOldName);

    GDALDataset *CreateCopy(const char *, GDALDataset *, int,
                            CSLConstList papszOptions,
                            GDALProgressFunc pfnProgress,
                            void *pProgressData) CPL_WARN_UNUSED_RESULT;

    bool CanVectorTranslateFrom(const char *pszDestName,
                                GDALDataset *poSourceDS,
                                CSLConstList papszVectorTranslateArguments,
                                char ***ppapszFailureReasons);

    /**
     * \brief Returns TRUE if the given open option is supported by the driver.
     * @param pszOpenOptionName name of the open option to be checked
     * @return TRUE if the driver supports the open option
     * @since GDAL 3.11
     */
    bool HasOpenOption(const char *pszOpenOptionName) const;

    GDALDataset *
    VectorTranslateFrom(const char *pszDestName, GDALDataset *poSourceDS,
                        CSLConstList papszVectorTranslateArguments,
                        GDALProgressFunc pfnProgress,
                        void *pProgressData) CPL_WARN_UNUSED_RESULT;

    /* -------------------------------------------------------------------- */
    /*      The following are semiprivate, not intended to be accessed      */
    /*      by anyone but the formats instantiating and populating the      */
    /*      drivers.                                                        */
    /* -------------------------------------------------------------------- */
    //! @cond Doxygen_Suppress

    // Not aimed at being used outside of GDAL. Use GDALDataset::Open() instead
    GDALDataset *Open(GDALOpenInfo *poOpenInfo, bool bSetOpenOptions);

    typedef GDALDataset *(*OpenCallback)(GDALOpenInfo *);

    OpenCallback pfnOpen = nullptr;

    virtual OpenCallback GetOpenCallback()
    {
        return pfnOpen;
    }

    typedef GDALDataset *(*CreateCallback)(const char *pszName, int nXSize,
                                           int nYSize, int nBands,
                                           GDALDataType eType,
                                           char **papszOptions);

    CreateCallback pfnCreate = nullptr;

    virtual CreateCallback GetCreateCallback()
    {
        return pfnCreate;
    }

    GDALDataset *(*pfnCreateEx)(GDALDriver *, const char *pszName, int nXSize,
                                int nYSize, int nBands, GDALDataType eType,
                                char **papszOptions) = nullptr;

    typedef GDALDataset *(*CreateMultiDimensionalCallback)(
        const char *pszName, CSLConstList papszRootGroupOptions,
        CSLConstList papszOptions);

    CreateMultiDimensionalCallback pfnCreateMultiDimensional = nullptr;

    virtual CreateMultiDimensionalCallback GetCreateMultiDimensionalCallback()
    {
        return pfnCreateMultiDimensional;
    }

    typedef CPLErr (*DeleteCallback)(const char *pszName);
    DeleteCallback pfnDelete = nullptr;

    virtual DeleteCallback GetDeleteCallback()
    {
        return pfnDelete;
    }

    typedef GDALDataset *(*CreateCopyCallback)(const char *, GDALDataset *, int,
                                               char **,
                                               GDALProgressFunc pfnProgress,
                                               void *pProgressData);

    CreateCopyCallback pfnCreateCopy = nullptr;

    virtual CreateCopyCallback GetCreateCopyCallback()
    {
        return pfnCreateCopy;
    }

    void *pDriverData = nullptr;

    void (*pfnUnloadDriver)(GDALDriver *) = nullptr;

    /** Identify() if the file is recognized or not by the driver.

       Return GDAL_IDENTIFY_TRUE (1) if the passed file is certainly recognized
       by the driver. Return GDAL_IDENTIFY_FALSE (0) if the passed file is
       certainly NOT recognized by the driver. Return GDAL_IDENTIFY_UNKNOWN (-1)
       if the passed file may be or may not be recognized by the driver, and
       that a potentially costly test must be done with pfnOpen.
    */
    int (*pfnIdentify)(GDALOpenInfo *) = nullptr;
    int (*pfnIdentifyEx)(GDALDriver *, GDALOpenInfo *) = nullptr;

    typedef CPLErr (*RenameCallback)(const char *pszNewName,
                                     const char *pszOldName);
    RenameCallback pfnRename = nullptr;

    virtual RenameCallback GetRenameCallback()
    {
        return pfnRename;
    }

    typedef CPLErr (*CopyFilesCallback)(const char *pszNewName,
                                        const char *pszOldName);
    CopyFilesCallback pfnCopyFiles = nullptr;

    virtual CopyFilesCallback GetCopyFilesCallback()
    {
        return pfnCopyFiles;
    }

    // Used for legacy OGR drivers, and Python drivers
    GDALDataset *(*pfnOpenWithDriverArg)(GDALDriver *,
                                         GDALOpenInfo *) = nullptr;

    /* For legacy OGR drivers */
    GDALDataset *(*pfnCreateVectorOnly)(GDALDriver *, const char *pszName,
                                        char **papszOptions) = nullptr;
    CPLErr (*pfnDeleteDataSource)(GDALDriver *, const char *pszName) = nullptr;

    /** Whether pfnVectorTranslateFrom() can be run given the source dataset
     * and the non-positional arguments of GDALVectorTranslate() stored
     * in papszVectorTranslateArguments.
     */
    bool (*pfnCanVectorTranslateFrom)(
        const char *pszDestName, GDALDataset *poSourceDS,
        CSLConstList papszVectorTranslateArguments,
        char ***ppapszFailureReasons) = nullptr;

    /** Creates a copy from the specified source dataset, using the
     * non-positional arguments of GDALVectorTranslate() stored
     * in papszVectorTranslateArguments.
     */
    GDALDataset *(*pfnVectorTranslateFrom)(
        const char *pszDestName, GDALDataset *poSourceDS,
        CSLConstList papszVectorTranslateArguments,
        GDALProgressFunc pfnProgress, void *pProgressData) = nullptr;

    /**
     * Returns a (possibly null) pointer to the Subdataset informational function
     * from the subdataset file name.
     */
    GDALSubdatasetInfo *(*pfnGetSubdatasetInfoFunc)(const char *pszFileName) =
        nullptr;

    typedef GDALAlgorithm *(*InstantiateAlgorithmCallback)(
        const std::vector<std::string> &aosPath);
    InstantiateAlgorithmCallback pfnInstantiateAlgorithm = nullptr;

    virtual InstantiateAlgorithmCallback GetInstantiateAlgorithmCallback()
    {
        return pfnInstantiateAlgorithm;
    }

    /** Instantiate an algorithm by its full path (omitting leading "gdal").
     * For example {"driver", "pdf", "list-layers"}
     */
    GDALAlgorithm *
    InstantiateAlgorithm(const std::vector<std::string> &aosPath);

    /** Declare an algorithm by its full path (omitting leading "gdal").
     * For example {"driver", "pdf", "list-layers"}
     */
    void DeclareAlgorithm(const std::vector<std::string> &aosPath);

    //! @endcond

    /* -------------------------------------------------------------------- */
    /*      Helper methods.                                                 */
    /* -------------------------------------------------------------------- */
    //! @cond Doxygen_Suppress
    GDALDataset *DefaultCreateCopy(const char *, GDALDataset *, int,
                                   CSLConstList papszOptions,
                                   GDALProgressFunc pfnProgress,
                                   void *pProgressData) CPL_WARN_UNUSED_RESULT;

    static CPLErr DefaultCreateCopyMultiDimensional(
        GDALDataset *poSrcDS, GDALDataset *poDstDS, bool bStrict,
        CSLConstList /*papszOptions*/, GDALProgressFunc pfnProgress,
        void *pProgressData);

    static CPLErr DefaultCopyMasks(GDALDataset *poSrcDS, GDALDataset *poDstDS,
                                   int bStrict);
    static CPLErr DefaultCopyMasks(GDALDataset *poSrcDS, GDALDataset *poDstDS,
                                   int bStrict, CSLConstList papszOptions,
                                   GDALProgressFunc pfnProgress,
                                   void *pProgressData);

    CPLErr QuietDeleteForCreateCopy(const char *pszFilename,
                                    GDALDataset *poSrcDS);

    //! @endcond
    static CPLErr QuietDelete(const char *pszName,
                              CSLConstList papszAllowedDrivers = nullptr);

    //! @cond Doxygen_Suppress
    static CPLErr DefaultRename(const char *pszNewName, const char *pszOldName);
    static CPLErr DefaultCopyFiles(const char *pszNewName,
                                   const char *pszOldName);
    static void DefaultCopyMetadata(GDALDataset *poSrcDS, GDALDataset *poDstDS,
                                    CSLConstList papszOptions,
                                    CSLConstList papszExcludedDomains);

    //! @endcond

    /** Convert a GDALDriver* to a GDALDriverH.
     * @since GDAL 2.3
     */
    static inline GDALDriverH ToHandle(GDALDriver *poDriver)
    {
        return static_cast<GDALDriverH>(poDriver);
    }

    /** Convert a GDALDriverH to a GDALDriver*.
     * @since GDAL 2.3
     */
    static inline GDALDriver *FromHandle(GDALDriverH hDriver)
    {
        return static_cast<GDALDriver *>(hDriver);
    }

  private:
    CPL_DISALLOW_COPY_ASSIGN(GDALDriver)
};

/************************************************************************/
/*                       GDALPluginDriverProxy                          */
/************************************************************************/

// clang-format off
/** Proxy for a plugin driver.
 *
 * Such proxy must be registered with
 * GDALDriverManager::DeclareDeferredPluginDriver().
 *
 * If the real driver defines any of the following metadata items, the
 * proxy driver should also define them with the same value:
 * <ul>
 * <li>GDAL_DMD_LONGNAME</li>
 * <li>GDAL_DMD_EXTENSIONS</li>
 * <li>GDAL_DMD_EXTENSION</li>
 * <li>GDAL_DMD_OPENOPTIONLIST</li>
 * <li>GDAL_DMD_SUBDATASETS</li>
 * <li>GDAL_DMD_CONNECTION_PREFIX</li>
 * <li>GDAL_DCAP_RASTER</li>
 * <li>GDAL_DCAP_MULTIDIM_RASTER</li>
 * <li>GDAL_DCAP_VECTOR</li>
 * <li>GDAL_DCAP_GNM</li>
 * <li>GDAL_DCAP_MULTIPLE_VECTOR_LAYERS</li>
 * <li>GDAL_DCAP_NONSPATIAL</li>
 * <li>GDAL_DCAP_VECTOR_TRANSLATE_FROM</li>
 * </ul>
 *
 * The pfnIdentify and pfnGetSubdatasetInfoFunc callbacks, if they are
 * defined in the real driver, should also be set on the proxy driver.
 *
 * Furthermore, the following metadata items must be defined if the real
 * driver sets the corresponding callback:
 * <ul>
 * <li>GDAL_DCAP_OPEN: must be set to YES if the real driver defines pfnOpen</li>
 * <li>GDAL_DCAP_CREATE: must be set to YES if the real driver defines pfnCreate</li>
 * <li>GDAL_DCAP_CREATE_MULTIDIMENSIONAL: must be set to YES if the real driver defines pfnCreateMultiDimensional</li>
 * <li>GDAL_DCAP_CREATECOPY: must be set to YES if the real driver defines pfnCreateCopy</li>
 * </ul>
 *
 * @since 3.9
 */
// clang-format on

class GDALPluginDriverProxy : public GDALDriver
{
    const std::string m_osPluginFileName;
    std::string m_osPluginFullPath{};
    std::unique_ptr<GDALDriver> m_poRealDriver{};
    std::set<std::string> m_oSetMetadataItems{};

    GDALDriver *GetRealDriver();

    CPL_DISALLOW_COPY_ASSIGN(GDALPluginDriverProxy)

  protected:
    friend class GDALDriverManager;

    //! @cond Doxygen_Suppress
    void SetPluginFullPath(const std::string &osFullPath)
    {
        m_osPluginFullPath = osFullPath;
    }

    //! @endcond

  public:
    explicit GDALPluginDriverProxy(const std::string &osPluginFileName);

    /** Return the plugin file name (not a full path) */
    const std::string &GetPluginFileName() const
    {
        return m_osPluginFileName;
    }

    //! @cond Doxygen_Suppress
    OpenCallback GetOpenCallback() override;

    CreateCallback GetCreateCallback() override;

    CreateMultiDimensionalCallback GetCreateMultiDimensionalCallback() override;

    CreateCopyCallback GetCreateCopyCallback() override;

    DeleteCallback GetDeleteCallback() override;

    RenameCallback GetRenameCallback() override;

    CopyFilesCallback GetCopyFilesCallback() override;

    InstantiateAlgorithmCallback GetInstantiateAlgorithmCallback() override;
    //! @endcond

    CPLErr SetMetadataItem(const char *pszName, const char *pszValue,
                           const char *pszDomain = "") override;

    char **GetMetadata(const char *pszDomain) override;

    const char *GetMetadataItem(const char *pszName,
                                const char *pszDomain = "") override;
};

/* ******************************************************************** */
/*                          GDALDriverManager                           */
/* ******************************************************************** */

/**
 * Class for managing the registration of file format drivers.
 *
 * Use GetGDALDriverManager() to fetch the global singleton instance of
 * this class.
 */

class CPL_DLL GDALDriverManager : public GDALMajorObject
{
    int nDrivers = 0;
    GDALDriver **papoDrivers = nullptr;
    std::map<CPLString, GDALDriver *> oMapNameToDrivers{};
    std::string m_osPluginPath{};
    std::string m_osDriversIniPath{};
    mutable std::string m_osLastTriedDirectory{};
    std::set<std::string> m_oSetPluginFileNames{};
    bool m_bInDeferredDriverLoading = false;
    std::map<std::string, std::unique_ptr<GDALDriver>> m_oMapRealDrivers{};
    std::vector<std::unique_ptr<GDALDriver>> m_aoHiddenDrivers{};

    GDALDriver *GetDriver_unlocked(int iDriver)
    {
        return (iDriver >= 0 && iDriver < nDrivers) ? papoDrivers[iDriver]
                                                    : nullptr;
    }

    GDALDriver *GetDriverByName_unlocked(const char *pszName) const;

    static void CleanupPythonDrivers();

    std::string GetPluginFullPath(const char *pszFilename) const;

    int RegisterDriver(GDALDriver *, bool bHidden);

    CPL_DISALLOW_COPY_ASSIGN(GDALDriverManager)

  protected:
    friend class GDALPluginDriverProxy;
    friend GDALDatasetH CPL_STDCALL
    GDALOpenEx(const char *pszFilename, unsigned int nOpenFlags,
               const char *const *papszAllowedDrivers,
               const char *const *papszOpenOptions,
               const char *const *papszSiblingFiles);

    //! @cond Doxygen_Suppress
    static char **GetSearchPaths(const char *pszGDAL_DRIVER_PATH);
    //! @endcond

  public:
    GDALDriverManager();
    ~GDALDriverManager();

    int GetDriverCount(void) const;
    GDALDriver *GetDriver(int);
    GDALDriver *GetDriverByName(const char *);

    int RegisterDriver(GDALDriver *);
    void DeregisterDriver(GDALDriver *);

    // AutoLoadDrivers is a no-op if compiled with GDAL_NO_AUTOLOAD defined.
    void AutoLoadDrivers();
    void AutoSkipDrivers();
    void ReorderDrivers();
    static CPLErr LoadPlugin(const char *name);

    static void AutoLoadPythonDrivers();

    void DeclareDeferredPluginDriver(GDALPluginDriverProxy *poProxyDriver);

    //! @cond Doxygen_Suppress
    int GetDriverCount(bool bIncludeHidden) const;
    GDALDriver *GetDriver(int iDriver, bool bIncludeHidden);
    bool IsKnownDriver(const char *pszDriverName) const;
    GDALDriver *GetHiddenDriverByName(const char *pszName);
    //! @endcond
};

CPL_C_START
GDALDriverManager CPL_DLL *GetGDALDriverManager(void);
CPL_C_END

/* ******************************************************************** */
/*                          GDALAsyncReader                             */
/* ******************************************************************** */

/**
 * Class used as a session object for asynchronous requests.  They are
 * created with GDALDataset::BeginAsyncReader(), and destroyed with
 * GDALDataset::EndAsyncReader().
 */
class CPL_DLL GDALAsyncReader
{

    CPL_DISALLOW_COPY_ASSIGN(GDALAsyncReader)

  protected:
    //! @cond Doxygen_Suppress
    GDALDataset *poDS;
    int nXOff;
    int nYOff;
    int nXSize;
    int nYSize;
    void *pBuf;
    int nBufXSize;
    int nBufYSize;
    GDALDataType eBufType;
    int nBandCount;
    int *panBandMap;
    int nPixelSpace;
    int nLineSpace;
    int nBandSpace;
    //! @endcond

  public:
    GDALAsyncReader();
    virtual ~GDALAsyncReader();

    /** Return dataset.
     * @return dataset
     */
    GDALDataset *GetGDALDataset()
    {
        return poDS;
    }

    /** Return x offset.
     * @return x offset.
     */
    int GetXOffset() const
    {
        return nXOff;
    }

    /** Return y offset.
     * @return y offset.
     */
    int GetYOffset() const
    {
        return nYOff;
    }

    /** Return width.
     * @return width
     */
    int GetXSize() const
    {
        return nXSize;
    }

    /** Return height.
     * @return height
     */
    int GetYSize() const
    {
        return nYSize;
    }

    /** Return buffer.
     * @return buffer
     */
    void *GetBuffer()
    {
        return pBuf;
    }

    /** Return buffer width.
     * @return buffer width.
     */
    int GetBufferXSize() const
    {
        return nBufXSize;
    }

    /** Return buffer height.
     * @return buffer height.
     */
    int GetBufferYSize() const
    {
        return nBufYSize;
    }

    /** Return buffer data type.
     * @return buffer data type.
     */
    GDALDataType GetBufferType() const
    {
        return eBufType;
    }

    /** Return band count.
     * @return band count
     */
    int GetBandCount() const
    {
        return nBandCount;
    }

    /** Return band map.
     * @return band map.
     */
    int *GetBandMap()
    {
        return panBandMap;
    }

    /** Return pixel spacing.
     * @return pixel spacing.
     */
    int GetPixelSpace() const
    {
        return nPixelSpace;
    }

    /** Return line spacing.
     * @return line spacing.
     */
    int GetLineSpace() const
    {
        return nLineSpace;
    }

    /** Return band spacing.
     * @return band spacing.
     */
    int GetBandSpace() const
    {
        return nBandSpace;
    }

    virtual GDALAsyncStatusType
    GetNextUpdatedRegion(double dfTimeout, int *pnBufXOff, int *pnBufYOff,
                         int *pnBufXSize, int *pnBufYSize) = 0;
    virtual int LockBuffer(double dfTimeout = -1.0);
    virtual void UnlockBuffer();
};

/* ******************************************************************** */
/*                       Multidimensional array API                     */
/* ******************************************************************** */

class GDALMDArray;
class GDALAttribute;
class GDALDimension;
class GDALEDTComponent;

/* ******************************************************************** */
/*                         GDALExtendedDataType                         */
/* ******************************************************************** */

/**
 * Class used to represent potentially complex data types.
 * Several classes of data types are supported: numeric (based on GDALDataType),
 * compound or string.
 *
 * @since GDAL 3.1
 */
class CPL_DLL GDALExtendedDataType
{
  public:
    ~GDALExtendedDataType();

    GDALExtendedDataType(const GDALExtendedDataType &);

    GDALExtendedDataType &operator=(const GDALExtendedDataType &);
    GDALExtendedDataType &operator=(GDALExtendedDataType &&);

    static GDALExtendedDataType Create(GDALDataType eType);
    static GDALExtendedDataType
    Create(const std::string &osName, GDALDataType eBaseType,
           std::unique_ptr<GDALRasterAttributeTable>);
    static GDALExtendedDataType
    Create(const std::string &osName, size_t nTotalSize,
           std::vector<std::unique_ptr<GDALEDTComponent>> &&components);
    static GDALExtendedDataType
    CreateString(size_t nMaxStringLength = 0,
                 GDALExtendedDataTypeSubType eSubType = GEDTST_NONE);

    bool operator==(const GDALExtendedDataType &) const;

    /** Non-equality operator */
    bool operator!=(const GDALExtendedDataType &other) const
    {
        return !(operator==(other));
    }

    /** Return type name.
     *
     * This is the same as the C function GDALExtendedDataTypeGetName()
     */
    const std::string &GetName() const
    {
        return m_osName;
    }

    /** Return type class.
     *
     * This is the same as the C function GDALExtendedDataTypeGetClass()
     */
    GDALExtendedDataTypeClass GetClass() const
    {
        return m_eClass;
    }

    /** Return numeric data type (only valid when GetClass() == GEDTC_NUMERIC)
     *
     * This is the same as the C function
     * GDALExtendedDataTypeGetNumericDataType()
     */
    GDALDataType GetNumericDataType() const
    {
        return m_eNumericDT;
    }

    /** Return subtype.
     *
     * This is the same as the C function GDALExtendedDataTypeGetSubType()
     *
     * @since 3.4
     */
    GDALExtendedDataTypeSubType GetSubType() const
    {
        return m_eSubType;
    }

    /** Return the components of the data type (only valid when GetClass() ==
     * GEDTC_COMPOUND)
     *
     * This is the same as the C function GDALExtendedDataTypeGetComponents()
     */
    const std::vector<std::unique_ptr<GDALEDTComponent>> &GetComponents() const
    {
        return m_aoComponents;
    }

    /** Return data type size in bytes.
     *
     * For a string, this will be size of a char* pointer.
     *
     * This is the same as the C function GDALExtendedDataTypeGetSize()
     */
    size_t GetSize() const
    {
        return m_nSize;
    }

    /** Return the maximum length of a string in bytes.
     *
     * 0 indicates unknown/unlimited string.
     */
    size_t GetMaxStringLength() const
    {
        return m_nMaxStringLength;
    }

    /** Return associated raster attribute table, when there is one.
     *
     * For the netCDF driver, the RAT will capture enumerated types, with
     * a "value" column with an integer value and a "name" column with the
     * associated name.
     *
     * This is the same as the C function GDALExtendedDataTypeGetRAT()
     *
     * @since 3.12
     */
    const GDALRasterAttributeTable *GetRAT() const
    {
        return m_poRAT.get();
    }

    bool CanConvertTo(const GDALExtendedDataType &other) const;

    bool NeedsFreeDynamicMemory() const;

    void FreeDynamicMemory(void *pBuffer) const;

    static bool CopyValue(const void *pSrc, const GDALExtendedDataType &srcType,
                          void *pDst, const GDALExtendedDataType &dstType);

    static bool CopyValues(const void *pSrc,
                           const GDALExtendedDataType &srcType,
                           GPtrDiff_t nSrcStrideInElts, void *pDst,
                           const GDALExtendedDataType &dstType,
                           GPtrDiff_t nDstStrideInElts, size_t nValues);

  private:
    GDALExtendedDataType(size_t nMaxStringLength,
                         GDALExtendedDataTypeSubType eSubType);
    explicit GDALExtendedDataType(GDALDataType eType);
    GDALExtendedDataType(const std::string &osName, GDALDataType eBaseType,
                         std::unique_ptr<GDALRasterAttributeTable>);
    GDALExtendedDataType(
        const std::string &osName, size_t nTotalSize,
        std::vector<std::unique_ptr<GDALEDTComponent>> &&components);

    std::string m_osName{};
    GDALExtendedDataTypeClass m_eClass = GEDTC_NUMERIC;
    GDALExtendedDataTypeSubType m_eSubType = GEDTST_NONE;
    GDALDataType m_eNumericDT = GDT_Unknown;
    std::vector<std::unique_ptr<GDALEDTComponent>> m_aoComponents{};
    size_t m_nSize = 0;
    size_t m_nMaxStringLength = 0;
    std::unique_ptr<GDALRasterAttributeTable> m_poRAT{};
};

/* ******************************************************************** */
/*                            GDALEDTComponent                          */
/* ******************************************************************** */

/**
 * Class for a component of a compound extended data type.
 *
 * @since GDAL 3.1
 */
class CPL_DLL GDALEDTComponent
{
  public:
    ~GDALEDTComponent();
    GDALEDTComponent(const std::string &name, size_t offset,
                     const GDALExtendedDataType &type);
    GDALEDTComponent(const GDALEDTComponent &);

    bool operator==(const GDALEDTComponent &) const;

    /** Return the name.
     *
     * This is the same as the C function GDALEDTComponentGetName().
     */
    const std::string &GetName() const
    {
        return m_osName;
    }

    /** Return the offset (in bytes) of the component in the compound data type.
     *
     * This is the same as the C function GDALEDTComponentGetOffset().
     */
    size_t GetOffset() const
    {
        return m_nOffset;
    }

    /** Return the data type of the component.
     *
     * This is the same as the C function GDALEDTComponentGetType().
     */
    const GDALExtendedDataType &GetType() const
    {
        return m_oType;
    }

  private:
    std::string m_osName;
    size_t m_nOffset;
    GDALExtendedDataType m_oType;
};

/* ******************************************************************** */
/*                            GDALIHasAttribute                         */
/* ******************************************************************** */

/**
 * Interface used to get a single GDALAttribute or a set of GDALAttribute
 *
 * @since GDAL 3.1
 */
class CPL_DLL GDALIHasAttribute
{
  protected:
    std::shared_ptr<GDALAttribute>
    GetAttributeFromAttributes(const std::string &osName) const;

  public:
    virtual ~GDALIHasAttribute();

    virtual std::shared_ptr<GDALAttribute>
    GetAttribute(const std::string &osName) const;

    virtual std::vector<std::shared_ptr<GDALAttribute>>
    GetAttributes(CSLConstList papszOptions = nullptr) const;

    virtual std::shared_ptr<GDALAttribute>
    CreateAttribute(const std::string &osName,
                    const std::vector<GUInt64> &anDimensions,
                    const GDALExtendedDataType &oDataType,
                    CSLConstList papszOptions = nullptr);

    virtual bool DeleteAttribute(const std::string &osName,
                                 CSLConstList papszOptions = nullptr);
};

/* ******************************************************************** */
/*                               GDALGroup                              */
/* ******************************************************************** */

/* clang-format off */
/**
 * Class modeling a named container of GDALAttribute, GDALMDArray, OGRLayer or
 * other GDALGroup. Hence GDALGroup can describe a hierarchy of objects.
 *
 * This is based on the <a href="https://portal.opengeospatial.org/files/81716#_hdf5_group">HDF5 group
 * concept</a>
 *
 * @since GDAL 3.1
 */
/* clang-format on */

class CPL_DLL GDALGroup : public GDALIHasAttribute
{
  protected:
    //! @cond Doxygen_Suppress
    std::string m_osName{};

    // This is actually a path of the form "/parent_path/{m_osName}"
    std::string m_osFullName{};

    // Used for example by GDALSubsetGroup to distinguish a derived group
    //from its original, without altering its name
    const std::string m_osContext{};

    // List of types owned by the group.
    std::vector<std::shared_ptr<GDALExtendedDataType>> m_apoTypes{};

    //! Weak pointer to this
    std::weak_ptr<GDALGroup> m_pSelf{};

    //! Can be set to false by the owing group, when deleting this object
    bool m_bValid = true;

    GDALGroup(const std::string &osParentName, const std::string &osName,
              const std::string &osContext = std::string());

    const GDALGroup *
    GetInnerMostGroup(const std::string &osPathOrArrayOrDim,
                      std::shared_ptr<GDALGroup> &curGroupHolder,
                      std::string &osLastPart) const;

    void BaseRename(const std::string &osNewName);

    bool CheckValidAndErrorOutIfNot() const;

    void SetSelf(const std::shared_ptr<GDALGroup> &self)
    {
        m_pSelf = self;
    }

    virtual void NotifyChildrenOfRenaming()
    {
    }

    virtual void NotifyChildrenOfDeletion()
    {
    }

    //! @endcond

  public:
    virtual ~GDALGroup();

    /** Return the name of the group.
     *
     * This is the same as the C function GDALGroupGetName().
     */
    const std::string &GetName() const
    {
        return m_osName;
    }

    /** Return the full name of the group.
     *
     * This is the same as the C function GDALGroupGetFullName().
     */
    const std::string &GetFullName() const
    {
        return m_osFullName;
    }

    /** Return data types associated with the group (typically enumerations)
     *
     * This is the same as the C function GDALGroupGetDataTypeCount() and GDALGroupGetDataType()
     *
     * @since 3.12
     */
    const std::vector<std::shared_ptr<GDALExtendedDataType>> &
    GetDataTypes() const
    {
        return m_apoTypes;
    }

    virtual std::vector<std::string>
    GetMDArrayNames(CSLConstList papszOptions = nullptr) const;
    virtual std::shared_ptr<GDALMDArray>
    OpenMDArray(const std::string &osName,
                CSLConstList papszOptions = nullptr) const;

    std::vector<std::string> GetMDArrayFullNamesRecursive(
        CSLConstList papszGroupOptions = nullptr,
        CSLConstList papszArrayOptions = nullptr) const;

    virtual std::vector<std::string>
    GetGroupNames(CSLConstList papszOptions = nullptr) const;
    virtual std::shared_ptr<GDALGroup>
    OpenGroup(const std::string &osName,
              CSLConstList papszOptions = nullptr) const;

    virtual std::vector<std::string>
    GetVectorLayerNames(CSLConstList papszOptions = nullptr) const;
    virtual OGRLayer *
    OpenVectorLayer(const std::string &osName,
                    CSLConstList papszOptions = nullptr) const;

    virtual std::vector<std::shared_ptr<GDALDimension>>
    GetDimensions(CSLConstList papszOptions = nullptr) const;

    virtual std::shared_ptr<GDALGroup>
    CreateGroup(const std::string &osName, CSLConstList papszOptions = nullptr);

    virtual bool DeleteGroup(const std::string &osName,
                             CSLConstList papszOptions = nullptr);

    virtual std::shared_ptr<GDALDimension>
    CreateDimension(const std::string &osName, const std::string &osType,
                    const std::string &osDirection, GUInt64 nSize,
                    CSLConstList papszOptions = nullptr);

    virtual std::shared_ptr<GDALMDArray> CreateMDArray(
        const std::string &osName,
        const std::vector<std::shared_ptr<GDALDimension>> &aoDimensions,
        const GDALExtendedDataType &oDataType,
        CSLConstList papszOptions = nullptr);

    virtual bool DeleteMDArray(const std::string &osName,
                               CSLConstList papszOptions = nullptr);

    GUInt64 GetTotalCopyCost() const;

    virtual bool CopyFrom(const std::shared_ptr<GDALGroup> &poDstRootGroup,
                          GDALDataset *poSrcDS,
                          const std::shared_ptr<GDALGroup> &poSrcGroup,
                          bool bStrict, GUInt64 &nCurCost,
                          const GUInt64 nTotalCost,
                          GDALProgressFunc pfnProgress, void *pProgressData,
                          CSLConstList papszOptions = nullptr);

    virtual CSLConstList GetStructuralInfo() const;

    std::shared_ptr<GDALMDArray>
    OpenMDArrayFromFullname(const std::string &osFullName,
                            CSLConstList papszOptions = nullptr) const;

    std::shared_ptr<GDALAttribute>
    OpenAttributeFromFullname(const std::string &osFullName,
                              CSLConstList papszOptions = nullptr) const;

    std::shared_ptr<GDALMDArray>
    ResolveMDArray(const std::string &osName, const std::string &osStartingPath,
                   CSLConstList papszOptions = nullptr) const;

    std::shared_ptr<GDALGroup>
    OpenGroupFromFullname(const std::string &osFullName,
                          CSLConstList papszOptions = nullptr) const;

    std::shared_ptr<GDALDimension>
    OpenDimensionFromFullname(const std::string &osFullName) const;

    virtual void ClearStatistics();

    virtual bool Rename(const std::string &osNewName);

    std::shared_ptr<GDALGroup>
    SubsetDimensionFromSelection(const std::string &osSelection) const;

    //! @cond Doxygen_Suppress
    virtual void ParentRenamed(const std::string &osNewParentFullName);

    virtual void Deleted();

    virtual void ParentDeleted();

    const std::string &GetContext() const
    {
        return m_osContext;
    }

    //! @endcond

    //! @cond Doxygen_Suppress
    static constexpr GUInt64 COPY_COST = 1000;
    //! @endcond
};

/* ******************************************************************** */
/*                          GDALAbstractMDArray                         */
/* ******************************************************************** */

/**
 * Abstract class, implemented by GDALAttribute and GDALMDArray.
 *
 * @since GDAL 3.1
 */
class CPL_DLL GDALAbstractMDArray
{
  protected:
    //! @cond Doxygen_Suppress
    std::string m_osName{};

    // This is actually a path of the form "/parent_path/{m_osName}"
    std::string m_osFullName{};
    std::weak_ptr<GDALAbstractMDArray> m_pSelf{};

    //! Can be set to false by the owing object, when deleting this object
    bool m_bValid = true;

    GDALAbstractMDArray(const std::string &osParentName,
                        const std::string &osName);

    void SetSelf(const std::shared_ptr<GDALAbstractMDArray> &self)
    {
        m_pSelf = self;
    }

    bool CheckValidAndErrorOutIfNot() const;

    bool CheckReadWriteParams(const GUInt64 *arrayStartIdx, const size_t *count,
                              const GInt64 *&arrayStep,
                              const GPtrDiff_t *&bufferStride,
                              const GDALExtendedDataType &bufferDataType,
                              const void *buffer,
                              const void *buffer_alloc_start,
                              size_t buffer_alloc_size,
                              std::vector<GInt64> &tmp_arrayStep,
                              std::vector<GPtrDiff_t> &tmp_bufferStride) const;

    virtual bool
    IRead(const GUInt64 *arrayStartIdx,    // array of size GetDimensionCount()
          const size_t *count,             // array of size GetDimensionCount()
          const GInt64 *arrayStep,         // step in elements
          const GPtrDiff_t *bufferStride,  // stride in elements
          const GDALExtendedDataType &bufferDataType,
          void *pDstBuffer) const = 0;

    virtual bool
    IWrite(const GUInt64 *arrayStartIdx,    // array of size GetDimensionCount()
           const size_t *count,             // array of size GetDimensionCount()
           const GInt64 *arrayStep,         // step in elements
           const GPtrDiff_t *bufferStride,  // stride in elements
           const GDALExtendedDataType &bufferDataType, const void *pSrcBuffer);

    void BaseRename(const std::string &osNewName);

    virtual void NotifyChildrenOfRenaming()
    {
    }

    virtual void NotifyChildrenOfDeletion()
    {
    }

    //! @endcond

  public:
    virtual ~GDALAbstractMDArray();

    /** Return the name of an array or attribute.
     *
     * This is the same as the C function GDALMDArrayGetName() or
     * GDALAttributeGetName().
     */
    const std::string &GetName() const
    {
        return m_osName;
    }

    /** Return the name of an array or attribute.
     *
     * This is the same as the C function GDALMDArrayGetFullName() or
     * GDALAttributeGetFullName().
     */
    const std::string &GetFullName() const
    {
        return m_osFullName;
    }

    GUInt64 GetTotalElementsCount() const;

    virtual size_t GetDimensionCount() const;

    virtual const std::vector<std::shared_ptr<GDALDimension>> &
    GetDimensions() const = 0;

    virtual const GDALExtendedDataType &GetDataType() const = 0;

    virtual std::vector<GUInt64> GetBlockSize() const;

    virtual std::vector<size_t>
    GetProcessingChunkSize(size_t nMaxChunkMemory) const;

    /* clang-format off */
    /** Type of pfnFunc argument of ProcessPerChunk().
     * @param array Array on which ProcessPerChunk was called.
     * @param chunkArrayStartIdx Values representing the starting index to use
     *                           in each dimension (in [0, aoDims[i].GetSize()-1] range)
     *                           for the current chunk.
     *                           Will be nullptr for a zero-dimensional array.
     * @param chunkCount         Values representing the number of values to use in
     *                           each dimension for the current chunk.
     *                           Will be nullptr for a zero-dimensional array.
     * @param iCurChunk          Number of current chunk being processed.
     *                           In [1, nChunkCount] range.
     * @param nChunkCount        Total number of chunks to process.
     * @param pUserData          User data.
     * @return return true in case of success.
     */
    typedef bool (*FuncProcessPerChunkType)(
                        GDALAbstractMDArray *array,
                        const GUInt64 *chunkArrayStartIdx,
                        const size_t *chunkCount,
                        GUInt64 iCurChunk,
                        GUInt64 nChunkCount,
                        void *pUserData);
    /* clang-format on */

    virtual bool ProcessPerChunk(const GUInt64 *arrayStartIdx,
                                 const GUInt64 *count, const size_t *chunkSize,
                                 FuncProcessPerChunkType pfnFunc,
                                 void *pUserData);

    virtual bool
    Read(const GUInt64 *arrayStartIdx,    // array of size GetDimensionCount()
         const size_t *count,             // array of size GetDimensionCount()
         const GInt64 *arrayStep,         // step in elements
         const GPtrDiff_t *bufferStride,  // stride in elements
         const GDALExtendedDataType &bufferDataType, void *pDstBuffer,
         const void *pDstBufferAllocStart = nullptr,
         size_t nDstBufferAllocSize = 0) const;

    bool
    Write(const GUInt64 *arrayStartIdx,    // array of size GetDimensionCount()
          const size_t *count,             // array of size GetDimensionCount()
          const GInt64 *arrayStep,         // step in elements
          const GPtrDiff_t *bufferStride,  // stride in elements
          const GDALExtendedDataType &bufferDataType, const void *pSrcBuffer,
          const void *pSrcBufferAllocStart = nullptr,
          size_t nSrcBufferAllocSize = 0);

    virtual bool Rename(const std::string &osNewName);

    //! @cond Doxygen_Suppress
    virtual void Deleted();

    virtual void ParentDeleted();

    virtual void ParentRenamed(const std::string &osNewParentFullName);
    //! @endcond
};

/* ******************************************************************** */
/*                              GDALRawResult                           */
/* ******************************************************************** */

/**
 * Store the raw result of an attribute value, which might contain dynamically
 * allocated structures (like pointer to strings).
 *
 * @since GDAL 3.1
 */
class CPL_DLL GDALRawResult
{
  private:
    GDALExtendedDataType m_dt;
    size_t m_nEltCount;
    size_t m_nSize;
    GByte *m_raw;

    void FreeMe();

    GDALRawResult(const GDALRawResult &) = delete;
    GDALRawResult &operator=(const GDALRawResult &) = delete;

  protected:
    friend class GDALAttribute;
    //! @cond Doxygen_Suppress
    GDALRawResult(GByte *raw, const GDALExtendedDataType &dt, size_t nEltCount);
    //! @endcond

  public:
    ~GDALRawResult();
    GDALRawResult(GDALRawResult &&);
    GDALRawResult &operator=(GDALRawResult &&);

    /** Return byte at specified index. */
    const GByte &operator[](size_t idx) const
    {
        return m_raw[idx];
    }

    /** Return pointer to the start of data. */
    const GByte *data() const
    {
        return m_raw;
    }

    /** Return the size in bytes of the raw result. */
    size_t size() const
    {
        return m_nSize;
    }

    //! @cond Doxygen_Suppress
    GByte *StealData();
    //! @endcond
};

/* ******************************************************************** */
/*                              GDALAttribute                           */
/* ******************************************************************** */

/* clang-format off */
/**
 * Class modeling an attribute that has a name, a value and a type, and is
 * typically used to describe a metadata item. The value can be (for the
 * HDF5 format) in the general case a multidimensional array of "any" type
 * (in most cases, this will be a single value of string or numeric type)
 *
 * This is based on the <a href="https://portal.opengeospatial.org/files/81716#_hdf5_attribute">HDF5
 * attribute concept</a>
 *
 * @since GDAL 3.1
 */
/* clang-format on */

class CPL_DLL GDALAttribute : virtual public GDALAbstractMDArray
{
    mutable std::string m_osCachedVal{};

  protected:
    //! @cond Doxygen_Suppress
    GDALAttribute(const std::string &osParentName, const std::string &osName);
    //! @endcond

  public:
    //! @cond Doxygen_Suppress
    ~GDALAttribute();
    //! @endcond

    std::vector<GUInt64> GetDimensionsSize() const;

    GDALRawResult ReadAsRaw() const;
    const char *ReadAsString() const;
    int ReadAsInt() const;
    int64_t ReadAsInt64() const;
    double ReadAsDouble() const;
    CPLStringList ReadAsStringArray() const;
    std::vector<int> ReadAsIntArray() const;
    std::vector<int64_t> ReadAsInt64Array() const;
    std::vector<double> ReadAsDoubleArray() const;

    using GDALAbstractMDArray::Write;
    bool Write(const void *pabyValue, size_t nLen);
    bool Write(const char *);
    bool WriteInt(int);
    bool WriteInt64(int64_t);
    bool Write(double);
    bool Write(CSLConstList);
    bool Write(const int *, size_t);
    bool Write(const int64_t *, size_t);
    bool Write(const double *, size_t);

    //! @cond Doxygen_Suppress
    static constexpr GUInt64 COPY_COST = 100;
    //! @endcond
};

/************************************************************************/
/*                            GDALAttributeString                       */
/************************************************************************/

//! @cond Doxygen_Suppress
class CPL_DLL GDALAttributeString final : public GDALAttribute
{
    std::vector<std::shared_ptr<GDALDimension>> m_dims{};
    GDALExtendedDataType m_dt = GDALExtendedDataType::CreateString();
    std::string m_osValue;

  protected:
    bool IRead(const GUInt64 *, const size_t *, const GInt64 *,
               const GPtrDiff_t *, const GDALExtendedDataType &bufferDataType,
               void *pDstBuffer) const override;

  public:
    GDALAttributeString(const std::string &osParentName,
                        const std::string &osName, const std::string &osValue,
                        GDALExtendedDataTypeSubType eSubType = GEDTST_NONE);

    const std::vector<std::shared_ptr<GDALDimension>> &
    GetDimensions() const override;

    const GDALExtendedDataType &GetDataType() const override;
};

//! @endcond

/************************************************************************/
/*                           GDALAttributeNumeric                       */
/************************************************************************/

//! @cond Doxygen_Suppress
class CPL_DLL GDALAttributeNumeric final : public GDALAttribute
{
    std::vector<std::shared_ptr<GDALDimension>> m_dims{};
    GDALExtendedDataType m_dt;
    int m_nValue = 0;
    double m_dfValue = 0;
    std::vector<GUInt32> m_anValuesUInt32{};

  protected:
    bool IRead(const GUInt64 *, const size_t *, const GInt64 *,
               const GPtrDiff_t *, const GDALExtendedDataType &bufferDataType,
               void *pDstBuffer) const override;

  public:
    GDALAttributeNumeric(const std::string &osParentName,
                         const std::string &osName, double dfValue);
    GDALAttributeNumeric(const std::string &osParentName,
                         const std::string &osName, int nValue);
    GDALAttributeNumeric(const std::string &osParentName,
                         const std::string &osName,
                         const std::vector<GUInt32> &anValues);

    const std::vector<std::shared_ptr<GDALDimension>> &
    GetDimensions() const override;

    const GDALExtendedDataType &GetDataType() const override;
};

//! @endcond

/* ******************************************************************** */
/*                              GDALMDArray                             */
/* ******************************************************************** */

/* clang-format off */
/**
 * Class modeling a multi-dimensional array. It has a name, values organized
 * as an array and a list of GDALAttribute.
 *
 * This is based on the <a href="https://portal.opengeospatial.org/files/81716#_hdf5_dataset">HDF5
 * dataset concept</a>
 *
 * @since GDAL 3.1
 */
/* clang-format on */

class CPL_DLL GDALMDArray : virtual public GDALAbstractMDArray,
                            public GDALIHasAttribute
{
    friend class GDALMDArrayResampled;
    std::shared_ptr<GDALMDArray>
    GetView(const std::vector<GUInt64> &indices) const;

    inline std::shared_ptr<GDALMDArray>
    atInternal(const std::vector<GUInt64> &indices) const
    {
        return GetView(indices);
    }

    template <typename... GUInt64VarArg>
    // cppcheck-suppress functionStatic
    inline std::shared_ptr<GDALMDArray>
    atInternal(std::vector<GUInt64> &indices, GUInt64 idx,
               GUInt64VarArg... tail) const
    {
        indices.push_back(idx);
        return atInternal(indices, tail...);
    }

    // Used for example by GDALSubsetGroup to distinguish a derived group
    //from its original, without altering its name
    const std::string m_osContext{};

    mutable bool m_bHasTriedCachedArray = false;
    mutable std::shared_ptr<GDALMDArray> m_poCachedArray{};

  protected:
    //! @cond Doxygen_Suppress
    GDALMDArray(const std::string &osParentName, const std::string &osName,
                const std::string &osContext = std::string());

    virtual bool IAdviseRead(const GUInt64 *arrayStartIdx, const size_t *count,
                             CSLConstList papszOptions) const;

    virtual bool IsCacheable() const
    {
        return true;
    }

    virtual bool SetStatistics(bool bApproxStats, double dfMin, double dfMax,
                               double dfMean, double dfStdDev,
                               GUInt64 nValidCount, CSLConstList papszOptions);

    static std::string MassageName(const std::string &inputName);

    std::shared_ptr<GDALGroup>
    GetCacheRootGroup(bool bCanCreate, std::string &osCacheFilenameOut) const;

    // Returns if bufferStride values express a transposed view of the array
    bool IsTransposedRequest(const size_t *count,
                             const GPtrDiff_t *bufferStride) const;

    // Should only be called if IsTransposedRequest() returns true
    bool ReadForTransposedRequest(const GUInt64 *arrayStartIdx,
                                  const size_t *count, const GInt64 *arrayStep,
                                  const GPtrDiff_t *bufferStride,
                                  const GDALExtendedDataType &bufferDataType,
                                  void *pDstBuffer) const;

    bool IsStepOneContiguousRowMajorOrderedSameDataType(
        const size_t *count, const GInt64 *arrayStep,
        const GPtrDiff_t *bufferStride,
        const GDALExtendedDataType &bufferDataType) const;

    // Should only be called if IsStepOneContiguousRowMajorOrderedSameDataType()
    // returns false
    bool ReadUsingContiguousIRead(const GUInt64 *arrayStartIdx,
                                  const size_t *count, const GInt64 *arrayStep,
                                  const GPtrDiff_t *bufferStride,
                                  const GDALExtendedDataType &bufferDataType,
                                  void *pDstBuffer) const;

    static std::shared_ptr<GDALMDArray> CreateGLTOrthorectified(
        const std::shared_ptr<GDALMDArray> &poParent,
        const std::shared_ptr<GDALGroup> &poRootGroup,
        const std::shared_ptr<GDALMDArray> &poGLTX,
        const std::shared_ptr<GDALMDArray> &poGLTY, int nGLTIndexOffset,
        const std::vector<double> &adfGeoTransform, CSLConstList papszOptions);

    //! @endcond

  public:
    GUInt64 GetTotalCopyCost() const;

    virtual bool CopyFrom(GDALDataset *poSrcDS, const GDALMDArray *poSrcArray,
                          bool bStrict, GUInt64 &nCurCost,
                          const GUInt64 nTotalCost,
                          GDALProgressFunc pfnProgress, void *pProgressData);

    /** Return whether an array is writable. */
    virtual bool IsWritable() const = 0;

    /** Return the filename that contains that array.
     *
     * This is used in particular for caching.
     *
     * Might be empty if the array is not linked to a file.
     *
     * @since GDAL 3.4
     */
    virtual const std::string &GetFilename() const = 0;

    virtual CSLConstList GetStructuralInfo() const;

    virtual const std::string &GetUnit() const;

    virtual bool SetUnit(const std::string &osUnit);

    virtual bool SetSpatialRef(const OGRSpatialReference *poSRS);

    virtual std::shared_ptr<OGRSpatialReference> GetSpatialRef() const;

    virtual const void *GetRawNoDataValue() const;

    double GetNoDataValueAsDouble(bool *pbHasNoData = nullptr) const;

    int64_t GetNoDataValueAsInt64(bool *pbHasNoData = nullptr) const;

    uint64_t GetNoDataValueAsUInt64(bool *pbHasNoData = nullptr) const;

    virtual bool SetRawNoDataValue(const void *pRawNoData);

    //! @cond Doxygen_Suppress
    bool SetNoDataValue(int nNoData)
    {
        return SetNoDataValue(static_cast<int64_t>(nNoData));
    }

    //! @endcond

    bool SetNoDataValue(double dfNoData);

    bool SetNoDataValue(int64_t nNoData);

    bool SetNoDataValue(uint64_t nNoData);

    virtual bool Resize(const std::vector<GUInt64> &anNewDimSizes,
                        CSLConstList papszOptions);

    virtual double GetOffset(bool *pbHasOffset = nullptr,
                             GDALDataType *peStorageType = nullptr) const;

    virtual double GetScale(bool *pbHasScale = nullptr,
                            GDALDataType *peStorageType = nullptr) const;

    virtual bool SetOffset(double dfOffset,
                           GDALDataType eStorageType = GDT_Unknown);

    virtual bool SetScale(double dfScale,
                          GDALDataType eStorageType = GDT_Unknown);

    std::shared_ptr<GDALMDArray> GetView(const std::string &viewExpr) const;

    std::shared_ptr<GDALMDArray> operator[](const std::string &fieldName) const;

    /** Return a view of the array using integer indexing.
     *
     * Equivalent of GetView("[indices_0,indices_1,.....,indices_last]")
     *
     * Example:
     * \code
     * ar->at(0,3,2)
     * \endcode
     */
    // sphinx 4.1.0 / breathe 4.30.0 don't like typename...
    //! @cond Doxygen_Suppress
    template <typename... GUInt64VarArg>
    //! @endcond
    // cppcheck-suppress functionStatic
    std::shared_ptr<GDALMDArray> at(GUInt64 idx, GUInt64VarArg... tail) const
    {
        std::vector<GUInt64> indices;
        indices.push_back(idx);
        return atInternal(indices, tail...);
    }

    virtual std::shared_ptr<GDALMDArray>
    Transpose(const std::vector<int> &anMapNewAxisToOldAxis) const;

    std::shared_ptr<GDALMDArray> GetUnscaled(
        double dfOverriddenScale = std::numeric_limits<double>::quiet_NaN(),
        double dfOverriddenOffset = std::numeric_limits<double>::quiet_NaN(),
        double dfOverriddenDstNodata =
            std::numeric_limits<double>::quiet_NaN()) const;

    virtual std::shared_ptr<GDALMDArray>
    GetMask(CSLConstList papszOptions) const;

    virtual std::shared_ptr<GDALMDArray>
    GetResampled(const std::vector<std::shared_ptr<GDALDimension>> &apoNewDims,
                 GDALRIOResampleAlg resampleAlg,
                 const OGRSpatialReference *poTargetSRS,
                 CSLConstList papszOptions) const;

    std::shared_ptr<GDALMDArray>
    GetGridded(const std::string &osGridOptions,
               const std::shared_ptr<GDALMDArray> &poXArray = nullptr,
               const std::shared_ptr<GDALMDArray> &poYArray = nullptr,
               CSLConstList papszOptions = nullptr) const;

    static std::vector<std::shared_ptr<GDALMDArray>>
    GetMeshGrid(const std::vector<std::shared_ptr<GDALMDArray>> &apoArrays,
                CSLConstList papszOptions = nullptr);

    virtual GDALDataset *
    AsClassicDataset(size_t iXDim, size_t iYDim,
                     const std::shared_ptr<GDALGroup> &poRootGroup = nullptr,
                     CSLConstList papszOptions = nullptr) const;

    virtual CPLErr GetStatistics(bool bApproxOK, bool bForce, double *pdfMin,
                                 double *pdfMax, double *pdfMean,
                                 double *padfStdDev, GUInt64 *pnValidCount,
                                 GDALProgressFunc pfnProgress,
                                 void *pProgressData);

    virtual bool ComputeStatistics(bool bApproxOK, double *pdfMin,
                                   double *pdfMax, double *pdfMean,
                                   double *pdfStdDev, GUInt64 *pnValidCount,
                                   GDALProgressFunc, void *pProgressData,
                                   CSLConstList papszOptions);

    virtual void ClearStatistics();

    virtual std::vector<std::shared_ptr<GDALMDArray>>
    GetCoordinateVariables() const;

    bool AdviseRead(const GUInt64 *arrayStartIdx, const size_t *count,
                    CSLConstList papszOptions = nullptr) const;

    bool IsRegularlySpaced(double &dfStart, double &dfIncrement) const;

    bool GuessGeoTransform(size_t nDimX, size_t nDimY, bool bPixelIsPoint,
                           GDALGeoTransform &gt) const;

    bool GuessGeoTransform(size_t nDimX, size_t nDimY, bool bPixelIsPoint,
                           double adfGeoTransform[6]) const;

    bool Cache(CSLConstList papszOptions = nullptr) const;

    bool
    Read(const GUInt64 *arrayStartIdx,    // array of size GetDimensionCount()
         const size_t *count,             // array of size GetDimensionCount()
         const GInt64 *arrayStep,         // step in elements
         const GPtrDiff_t *bufferStride,  // stride in elements
         const GDALExtendedDataType &bufferDataType, void *pDstBuffer,
         const void *pDstBufferAllocStart = nullptr,
         size_t nDstBufferAllocSize = 0) const override final;

    virtual std::shared_ptr<GDALGroup> GetRootGroup() const;

    //! @cond Doxygen_Suppress
    static constexpr GUInt64 COPY_COST = 1000;

    bool CopyFromAllExceptValues(const GDALMDArray *poSrcArray, bool bStrict,
                                 GUInt64 &nCurCost, const GUInt64 nTotalCost,
                                 GDALProgressFunc pfnProgress,
                                 void *pProgressData);

    struct Range
    {
        GUInt64 m_nStartIdx;
        GInt64 m_nIncr;

        explicit Range(GUInt64 nStartIdx = 0, GInt64 nIncr = 0)
            : m_nStartIdx(nStartIdx), m_nIncr(nIncr)
        {
        }
    };

    struct ViewSpec
    {
        std::string m_osFieldName{};

        // or

        std::vector<size_t>
            m_mapDimIdxToParentDimIdx{};  // of size m_dims.size()
        std::vector<Range>
            m_parentRanges{};  // of size m_poParent->GetDimensionCount()
    };

    virtual std::shared_ptr<GDALMDArray>
    GetView(const std::string &viewExpr, bool bRenameDimensions,
            std::vector<ViewSpec> &viewSpecs) const;

    const std::string &GetContext() const
    {
        return m_osContext;
    }

    //! @endcond
};

//! @cond Doxygen_Suppress
bool GDALMDRasterIOFromBand(GDALRasterBand *poBand, GDALRWFlag eRWFlag,
                            size_t iDimX, size_t iDimY,
                            const GUInt64 *arrayStartIdx, const size_t *count,
                            const GInt64 *arrayStep,
                            const GPtrDiff_t *bufferStride,
                            const GDALExtendedDataType &bufferDataType,
                            void *pBuffer);

//! @endcond

/************************************************************************/
/*                     GDALMDArrayRegularlySpaced                       */
/************************************************************************/

//! @cond Doxygen_Suppress
class CPL_DLL GDALMDArrayRegularlySpaced : public GDALMDArray
{
    double m_dfStart;
    double m_dfIncrement;
    double m_dfOffsetInIncrement;
    GDALExtendedDataType m_dt = GDALExtendedDataType::Create(GDT_Float64);
    std::vector<std::shared_ptr<GDALDimension>> m_dims;
    std::vector<std::shared_ptr<GDALAttribute>> m_attributes{};
    std::string m_osEmptyFilename{};

  protected:
    bool IRead(const GUInt64 *, const size_t *, const GInt64 *,
               const GPtrDiff_t *, const GDALExtendedDataType &bufferDataType,
               void *pDstBuffer) const override;

  public:
    GDALMDArrayRegularlySpaced(const std::string &osParentName,
                               const std::string &osName,
                               const std::shared_ptr<GDALDimension> &poDim,
                               double dfStart, double dfIncrement,
                               double dfOffsetInIncrement);

    static std::shared_ptr<GDALMDArrayRegularlySpaced>
    Create(const std::string &osParentName, const std::string &osName,
           const std::shared_ptr<GDALDimension> &poDim, double dfStart,
           double dfIncrement, double dfOffsetInIncrement);

    bool IsWritable() const override
    {
        return false;
    }

    const std::string &GetFilename() const override
    {
        return m_osEmptyFilename;
    }

    const std::vector<std::shared_ptr<GDALDimension>> &
    GetDimensions() const override;

    const GDALExtendedDataType &GetDataType() const override;

    std::vector<std::shared_ptr<GDALAttribute>>
        GetAttributes(CSLConstList) const override;

    void AddAttribute(const std::shared_ptr<GDALAttribute> &poAttr);
};

//! @endcond

/* ******************************************************************** */
/*                            GDALDimension                             */
/* ******************************************************************** */

/**
 * Class modeling a a dimension / axis used to index multidimensional arrays.
 * It has a name, a size (that is the number of values that can be indexed along
 * the dimension), a type (see GDALDimension::GetType()), a direction
 * (see GDALDimension::GetDirection()), a unit and can optionally point to a
 * GDALMDArray variable, typically one-dimensional, describing the values taken
 * by the dimension. For a georeferenced GDALMDArray and its X dimension, this
 * will be typically the values of the easting/longitude for each grid point.
 *
 * @since GDAL 3.1
 */
class CPL_DLL GDALDimension
{
  public:
    //! @cond Doxygen_Suppress
    GDALDimension(const std::string &osParentName, const std::string &osName,
                  const std::string &osType, const std::string &osDirection,
                  GUInt64 nSize);
    //! @endcond

    virtual ~GDALDimension();

    /** Return the name.
     *
     * This is the same as the C function GDALDimensionGetName()
     */
    const std::string &GetName() const
    {
        return m_osName;
    }

    /** Return the full name.
     *
     * This is the same as the C function GDALDimensionGetFullName()
     */
    const std::string &GetFullName() const
    {
        return m_osFullName;
    }

    /** Return the axis type.
     *
     * Predefined values are:
     * HORIZONTAL_X, HORIZONTAL_Y, VERTICAL, TEMPORAL, PARAMETRIC
     * Other values might be returned. Empty value means unknown.
     *
     * This is the same as the C function GDALDimensionGetType()
     */
    const std::string &GetType() const
    {
        return m_osType;
    }

    /** Return the axis direction.
     *
     * Predefined values are:
     * EAST, WEST, SOUTH, NORTH, UP, DOWN, FUTURE, PAST
     * Other values might be returned. Empty value means unknown.
     *
     * This is the same as the C function GDALDimensionGetDirection()
     */
    const std::string &GetDirection() const
    {
        return m_osDirection;
    }

    /** Return the size, that is the number of values along the dimension.
     *
     * This is the same as the C function GDALDimensionGetSize()
     */
    GUInt64 GetSize() const
    {
        return m_nSize;
    }

    virtual std::shared_ptr<GDALMDArray> GetIndexingVariable() const;

    virtual bool
    SetIndexingVariable(std::shared_ptr<GDALMDArray> poIndexingVariable);

    virtual bool Rename(const std::string &osNewName);

    //! @cond Doxygen_Suppress
    virtual void ParentRenamed(const std::string &osNewParentFullName);

    virtual void ParentDeleted();
    //! @endcond

  protected:
    //! @cond Doxygen_Suppress
    std::string m_osName;
    std::string m_osFullName;
    std::string m_osType;
    std::string m_osDirection;
    GUInt64 m_nSize;

    void BaseRename(const std::string &osNewName);

    //! @endcond
};

/************************************************************************/
/*                   GDALDimensionWeakIndexingVar()                     */
/************************************************************************/

//! @cond Doxygen_Suppress
class CPL_DLL GDALDimensionWeakIndexingVar : public GDALDimension
{
    std::weak_ptr<GDALMDArray> m_poIndexingVariable{};

  public:
    GDALDimensionWeakIndexingVar(const std::string &osParentName,
                                 const std::string &osName,
                                 const std::string &osType,
                                 const std::string &osDirection, GUInt64 nSize);

    std::shared_ptr<GDALMDArray> GetIndexingVariable() const override;

    bool SetIndexingVariable(
        std::shared_ptr<GDALMDArray> poIndexingVariable) override;

    void SetSize(GUInt64 nNewSize);
};
//! @endcond

/************************************************************************/
/*                       GDALAntiRecursionGuard                         */
/************************************************************************/

//! @cond Doxygen_Suppress
struct GDALAntiRecursionStruct;

class GDALAntiRecursionGuard
{
    GDALAntiRecursionStruct *m_psAntiRecursionStruct;
    std::string m_osIdentifier;
    int m_nDepth;

    GDALAntiRecursionGuard(const GDALAntiRecursionGuard &) = delete;
    GDALAntiRecursionGuard &operator=(const GDALAntiRecursionGuard &) = delete;

  public:
    explicit GDALAntiRecursionGuard(const std::string &osIdentifier);
    GDALAntiRecursionGuard(const GDALAntiRecursionGuard &other,
                           const std::string &osIdentifier);
    ~GDALAntiRecursionGuard();

    int GetCallDepth() const
    {
        return m_nDepth;
    }
};

//! @endcond

/************************************************************************/
/*                           Relationships                              */
/************************************************************************/

/**
 * Definition of a table relationship.
 *
 * GDALRelationship describes the relationship between two tables, including
 * properties such as the cardinality of the relationship and the participating
 * tables.
 *
 * Not all relationship properties are supported by all data formats.
 *
 * @since GDAL 3.6
 */
class CPL_DLL GDALRelationship
{
  protected:
    /*! @cond Doxygen_Suppress */
    std::string m_osName{};
    std::string m_osLeftTableName{};
    std::string m_osRightTableName{};
    GDALRelationshipCardinality m_eCardinality =
        GDALRelationshipCardinality::GRC_ONE_TO_MANY;
    std::string m_osMappingTableName{};
    std::vector<std::string> m_osListLeftTableFields{};
    std::vector<std::string> m_osListRightTableFields{};
    std::vector<std::string> m_osListLeftMappingTableFields{};
    std::vector<std::string> m_osListRightMappingTableFields{};
    GDALRelationshipType m_eType = GDALRelationshipType::GRT_ASSOCIATION;
    std::string m_osForwardPathLabel{};
    std::string m_osBackwardPathLabel{};
    std::string m_osRelatedTableType{};

    /*! @endcond */

  public:
    /**
     * Constructor for a relationship between two tables.
     * @param osName relationship name
     * @param osLeftTableName left table name
     * @param osRightTableName right table name
     * @param eCardinality cardinality of relationship
     */
    GDALRelationship(const std::string &osName,
                     const std::string &osLeftTableName,
                     const std::string &osRightTableName,
                     GDALRelationshipCardinality eCardinality =
                         GDALRelationshipCardinality::GRC_ONE_TO_MANY)
        : m_osName(osName), m_osLeftTableName(osLeftTableName),
          m_osRightTableName(osRightTableName), m_eCardinality(eCardinality)
    {
    }

    /** Get the name of the relationship */
    const std::string &GetName() const
    {
        return m_osName;
    }

    /** Get the cardinality of the relationship */
    GDALRelationshipCardinality GetCardinality() const
    {
        return m_eCardinality;
    }

    /** Get the name of the left (or base/origin) table in the relationship.
     *
     * @see GetRightTableName()
     */
    const std::string &GetLeftTableName() const
    {
        return m_osLeftTableName;
    }

    /** Get the name of the right (or related/destination) table in the
     * relationship */
    const std::string &GetRightTableName() const
    {
        return m_osRightTableName;
    }

    /** Get the name of the mapping table for many-to-many relationships.
     *
     * @see SetMappingTableName()
     */
    const std::string &GetMappingTableName() const
    {
        return m_osMappingTableName;
    }

    /** Sets the name of the mapping table for many-to-many relationships.
     *
     * @see GetMappingTableName()
     */
    void SetMappingTableName(const std::string &osName)
    {
        m_osMappingTableName = osName;
    }

    /** Get the names of the participating fields from the left table in the
     * relationship.
     *
     * @see GetRightTableFields()
     * @see SetLeftTableFields()
     */
    const std::vector<std::string> &GetLeftTableFields() const
    {
        return m_osListLeftTableFields;
    }

    /** Get the names of the participating fields from the right table in the
     * relationship.
     *
     * @see GetLeftTableFields()
     * @see SetRightTableFields()
     */
    const std::vector<std::string> &GetRightTableFields() const
    {
        return m_osListRightTableFields;
    }

    /** Sets the names of the participating fields from the left table in the
     * relationship.
     *
     * @see GetLeftTableFields()
     * @see SetRightTableFields()
     */
    void SetLeftTableFields(const std::vector<std::string> &osListFields)
    {
        m_osListLeftTableFields = osListFields;
    }

    /** Sets the names of the participating fields from the right table in the
     * relationship.
     *
     * @see GetRightTableFields()
     * @see SetLeftTableFields()
     */
    void SetRightTableFields(const std::vector<std::string> &osListFields)
    {
        m_osListRightTableFields = osListFields;
    }

    /** Get the names of the mapping table fields which correspond to the
     * participating fields from the left table in the relationship.
     *
     * @see GetRightMappingTableFields()
     * @see SetLeftMappingTableFields()
     */
    const std::vector<std::string> &GetLeftMappingTableFields() const
    {
        return m_osListLeftMappingTableFields;
    }

    /** Get the names of the mapping table fields which correspond to the
     * participating fields from the right table in the relationship.
     *
     * @see GetLeftMappingTableFields()
     * @see SetRightMappingTableFields()
     */
    const std::vector<std::string> &GetRightMappingTableFields() const
    {
        return m_osListRightMappingTableFields;
    }

    /** Sets the names of the mapping table fields which correspond to the
     * participating fields from the left table in the relationship.
     *
     * @see GetLeftMappingTableFields()
     * @see SetRightMappingTableFields()
     */
    void SetLeftMappingTableFields(const std::vector<std::string> &osListFields)
    {
        m_osListLeftMappingTableFields = osListFields;
    }

    /** Sets the names of the mapping table fields which correspond to the
     * participating fields from the right table in the relationship.
     *
     * @see GetRightMappingTableFields()
     * @see SetLeftMappingTableFields()
     */
    void
    SetRightMappingTableFields(const std::vector<std::string> &osListFields)
    {
        m_osListRightMappingTableFields = osListFields;
    }

    /** Get the type of the relationship.
     *
     * @see SetType()
     */
    GDALRelationshipType GetType() const
    {
        return m_eType;
    }

    /** Sets the type of the relationship.
     *
     * @see GetType()
     */
    void SetType(GDALRelationshipType eType)
    {
        m_eType = eType;
    }

    /** Get the label of the forward path for the relationship.
     *
     * The forward and backward path labels are free-form, user-friendly strings
     * which can be used to generate descriptions of the relationship between
     * features from the right and left tables.
     *
     * E.g. when the left table contains buildings and the right table contains
     * furniture, the forward path label could be "contains" and the backward
     * path label could be "is located within". A client could then generate a
     * user friendly description string such as "fire hose 1234 is located
     * within building 15a".
     *
     * @see SetForwardPathLabel()
     * @see GetBackwardPathLabel()
     */
    const std::string &GetForwardPathLabel() const
    {
        return m_osForwardPathLabel;
    }

    /** Sets the label of the forward path for the relationship.
     *
     * The forward and backward path labels are free-form, user-friendly strings
     * which can be used to generate descriptions of the relationship between
     * features from the right and left tables.
     *
     * E.g. when the left table contains buildings and the right table contains
     * furniture, the forward path label could be "contains" and the backward
     * path label could be "is located within". A client could then generate a
     * user friendly description string such as "fire hose 1234 is located
     * within building 15a".
     *
     * @see GetForwardPathLabel()
     * @see SetBackwardPathLabel()
     */
    void SetForwardPathLabel(const std::string &osLabel)
    {
        m_osForwardPathLabel = osLabel;
    }

    /** Get the label of the backward path for the relationship.
     *
     * The forward and backward path labels are free-form, user-friendly strings
     * which can be used to generate descriptions of the relationship between
     * features from the right and left tables.
     *
     * E.g. when the left table contains buildings and the right table contains
     * furniture, the forward path label could be "contains" and the backward
     * path label could be "is located within". A client could then generate a
     * user friendly description string such as "fire hose 1234 is located
     * within building 15a".
     *
     * @see SetBackwardPathLabel()
     * @see GetForwardPathLabel()
     */
    const std::string &GetBackwardPathLabel() const
    {
        return m_osBackwardPathLabel;
    }

    /** Sets the label of the backward path for the relationship.
     *
     * The forward and backward path labels are free-form, user-friendly strings
     * which can be used to generate descriptions of the relationship between
     * features from the right and left tables.
     *
     * E.g. when the left table contains buildings and the right table contains
     * furniture, the forward path label could be "contains" and the backward
     * path label could be "is located within". A client could then generate a
     * user friendly description string such as "fire hose 1234 is located
     * within building 15a".
     *
     * @see GetBackwardPathLabel()
     * @see SetForwardPathLabel()
     */
    void SetBackwardPathLabel(const std::string &osLabel)
    {
        m_osBackwardPathLabel = osLabel;
    }

    /** Get the type string of the related table.
     *
     * This a free-form string representing the type of related features, where
     * the exact interpretation is format dependent. For instance, table types
     * from GeoPackage relationships will directly reflect the categories from
     * the GeoPackage related tables extension (i.e. "media", "simple
     * attributes", "features", "attributes" and "tiles").
     *
     * @see SetRelatedTableType()
     */
    const std::string &GetRelatedTableType() const
    {
        return m_osRelatedTableType;
    }

    /** Sets the type string of the related table.
     *
     * This a free-form string representing the type of related features, where
     * the exact interpretation is format dependent. For instance, table types
     * from GeoPackage relationships will directly reflect the categories from
     * the GeoPackage related tables extension (i.e. "media", "simple
     * attributes", "features", "attributes" and "tiles").
     *
     * @see GetRelatedTableType()
     */
    void SetRelatedTableType(const std::string &osType)
    {
        m_osRelatedTableType = osType;
    }

    /** Convert a GDALRelationship* to a GDALRelationshipH.
     */
    static inline GDALRelationshipH ToHandle(GDALRelationship *poRelationship)
    {
        return static_cast<GDALRelationshipH>(poRelationship);
    }

    /** Convert a GDALRelationshipH to a GDALRelationship*.
     */
    static inline GDALRelationship *FromHandle(GDALRelationshipH hRelationship)
    {
        return static_cast<GDALRelationship *>(hRelationship);
    }
};

/* ==================================================================== */
/*      An assortment of overview related stuff.                        */
/* ==================================================================== */

//! @cond Doxygen_Suppress
/* Only exported for drivers as plugin. Signature may change */
CPLErr CPL_DLL GDALRegenerateOverviewsMultiBand(
    int nBands, GDALRasterBand *const *papoSrcBands, int nOverviews,
    GDALRasterBand *const *const *papapoOverviewBands,
    const char *pszResampling, GDALProgressFunc pfnProgress,
    void *pProgressData, CSLConstList papszOptions);

CPLErr CPL_DLL GDALRegenerateOverviewsMultiBand(
    const std::vector<GDALRasterBand *> &apoSrcBands,
    // First level of array is indexed by band (thus aapoOverviewBands.size() must be equal to apoSrcBands.size())
    // Second level is indexed by overview
    const std::vector<std::vector<GDALRasterBand *>> &aapoOverviewBands,
    const char *pszResampling, GDALProgressFunc pfnProgress,
    void *pProgressData, CSLConstList papszOptions);

/************************************************************************/
/*                       GDALOverviewResampleArgs                       */
/************************************************************************/

/** Arguments for overview resampling function. */
// Should not contain any dataset/rasterband object, as this might be
// read in a worker thread.
struct GDALOverviewResampleArgs
{
    //! Datatype of the source band argument
    GDALDataType eSrcDataType = GDT_Unknown;
    //! Datatype of the destination/overview band
    GDALDataType eOvrDataType = GDT_Unknown;
    //! Width in pixel of the destination/overview band
    int nOvrXSize = 0;
    //! Height in pixel of the destination/overview band
    int nOvrYSize = 0;
    //! NBITS value of the destination/overview band (or 0 if not set)
    int nOvrNBITS = 0;
    //! Factor to convert from destination X to source X
    // (source width divided by destination width)
    double dfXRatioDstToSrc = 0;
    //! Factor to convert from destination Y to source Y
    // (source height divided by destination height)
    double dfYRatioDstToSrc = 0;
    //! Sub-pixel delta to add to get source X
    double dfSrcXDelta = 0;
    //! Sub-pixel delta to add to get source Y
    double dfSrcYDelta = 0;
    //! Working data type (data type of the pChunk argument)
    GDALDataType eWrkDataType = GDT_Unknown;
    //! Array of nChunkXSize * nChunkYSize values of mask, or nullptr
    const GByte *pabyChunkNodataMask = nullptr;
    //! X offset of the source chunk in the source band
    int nChunkXOff = 0;
    //! Width in pixel of the source chunk in the source band
    int nChunkXSize = 0;
    //! Y offset of the source chunk in the source band
    int nChunkYOff = 0;
    //! Height in pixel of the source chunk in the source band
    int nChunkYSize = 0;
    //! X Offset of the destination chunk in the destination band
    int nDstXOff = 0;
    //! X Offset of the end (not included) of the destination chunk in the destination band
    int nDstXOff2 = 0;
    //! Y Offset of the destination chunk in the destination band
    int nDstYOff = 0;
    //! Y Offset of the end (not included) of the destination chunk in the destination band
    int nDstYOff2 = 0;
    //! Resampling method
    const char *pszResampling = nullptr;
    //! Whether the source band has a nodata value
    bool bHasNoData = false;
    //! Source band nodata value
    double dfNoDataValue = 0;
    //! Source color table
    const GDALColorTable *poColorTable = nullptr;
    //! Whether a single contributing source pixel at nodata should result
    // in the target pixel to be at nodata too (only taken into account by
    // average resampling)
    bool bPropagateNoData = false;
};

typedef CPLErr (*GDALResampleFunction)(const GDALOverviewResampleArgs &args,
                                       const void *pChunk, void **ppDstBuffer,
                                       GDALDataType *peDstBufferDataType);

GDALResampleFunction GDALGetResampleFunction(const char *pszResampling,
                                             int *pnRadius);

std::string CPL_DLL GDALGetNormalizedOvrResampling(const char *pszResampling);

GDALDataType GDALGetOvrWorkDataType(const char *pszResampling,
                                    GDALDataType eSrcDataType);

CPL_C_START

CPLErr CPL_DLL
HFAAuxBuildOverviews(const char *pszOvrFilename, GDALDataset *poParentDS,
                     GDALDataset **ppoDS, int nBands, const int *panBandList,
                     int nNewOverviews, const int *panNewOverviewList,
                     const char *pszResampling, GDALProgressFunc pfnProgress,
                     void *pProgressData, CSLConstList papszOptions);

CPLErr CPL_DLL GTIFFBuildOverviews(const char *pszFilename, int nBands,
                                   GDALRasterBand *const *papoBandList,
                                   int nOverviews, const int *panOverviewList,
                                   const char *pszResampling,
                                   GDALProgressFunc pfnProgress,
                                   void *pProgressData,
                                   CSLConstList papszOptions);

CPLErr CPL_DLL GTIFFBuildOverviewsEx(const char *pszFilename, int nBands,
                                     GDALRasterBand *const *papoBandList,
                                     int nOverviews, const int *panOverviewList,
                                     const std::pair<int, int> *pasOverviewSize,
                                     const char *pszResampling,
                                     const char *const *papszOptions,
                                     GDALProgressFunc pfnProgress,
                                     void *pProgressData);

int CPL_DLL GDALBandGetBestOverviewLevel(GDALRasterBand *poBand, int &nXOff,
                                         int &nYOff, int &nXSize, int &nYSize,
                                         int nBufXSize, int nBufYSize)
    CPL_WARN_DEPRECATED("Use GDALBandGetBestOverviewLevel2 instead");
int CPL_DLL GDALBandGetBestOverviewLevel2(GDALRasterBand *poBand, int &nXOff,
                                          int &nYOff, int &nXSize, int &nYSize,
                                          int nBufXSize, int nBufYSize,
                                          GDALRasterIOExtraArg *psExtraArg);

int CPL_DLL GDALOvLevelAdjust(int nOvLevel, int nXSize)
    CPL_WARN_DEPRECATED("Use GDALOvLevelAdjust2 instead");
int CPL_DLL GDALOvLevelAdjust2(int nOvLevel, int nXSize, int nYSize);
int CPL_DLL GDALComputeOvFactor(int nOvrXSize, int nRasterXSize, int nOvrYSize,
                                int nRasterYSize);

GDALDataset CPL_DLL *GDALFindAssociatedAuxFile(const char *pszBasefile,
                                               GDALAccess eAccess,
                                               GDALDataset *poDependentDS);

/* ==================================================================== */
/*  Infrastructure to check that dataset characteristics are valid      */
/* ==================================================================== */

int CPL_DLL GDALCheckDatasetDimensions(int nXSize, int nYSize);
int CPL_DLL GDALCheckBandCount(int nBands, int bIsZeroAllowed);

/* Internal use only */

/* CPL_DLL exported, but only for in-tree drivers that can be built as plugins
 */
int CPL_DLL GDALReadWorldFile2(const char *pszBaseFilename,
                               const char *pszExtension,
                               double *padfGeoTransform,
                               CSLConstList papszSiblingFiles,
                               char **ppszWorldFileNameOut);
int CPL_DLL GDALReadTabFile2(const char *pszBaseFilename,
                             double *padfGeoTransform, char **ppszWKT,
                             int *pnGCPCount, GDAL_GCP **ppasGCPs,
                             CSLConstList papszSiblingFiles,
                             char **ppszTabFileNameOut);

void CPL_DLL GDALCopyRasterIOExtraArg(GDALRasterIOExtraArg *psDestArg,
                                      GDALRasterIOExtraArg *psSrcArg);

void CPL_DLL GDALExpandPackedBitsToByteAt0Or1(
    const GByte *CPL_RESTRICT pabyInput, GByte *CPL_RESTRICT pabyOutput,
    size_t nInputBits);

void CPL_DLL GDALExpandPackedBitsToByteAt0Or255(
    const GByte *CPL_RESTRICT pabyInput, GByte *CPL_RESTRICT pabyOutput,
    size_t nInputBits);

CPL_C_END

int CPL_DLL GDALReadWorldFile2(const char *pszBaseFilename,
                               const char *pszExtension, GDALGeoTransform &gt,
                               CSLConstList papszSiblingFiles,
                               char **ppszWorldFileNameOut);

std::unique_ptr<GDALDataset> CPL_DLL
GDALGetThreadSafeDataset(std::unique_ptr<GDALDataset> poDS, int nScopeFlags);

GDALDataset CPL_DLL *GDALGetThreadSafeDataset(GDALDataset *poDS,
                                              int nScopeFlags);

void GDALNullifyOpenDatasetsList();
CPLMutex **GDALGetphDMMutex();
CPLMutex **GDALGetphDLMutex();
void GDALNullifyProxyPoolSingleton();
void GDALSetResponsiblePIDForCurrentThread(GIntBig responsiblePID);
GIntBig GDALGetResponsiblePIDForCurrentThread();

CPLString GDALFindAssociatedFile(const char *pszBasename, const char *pszExt,
                                 CSLConstList papszSiblingFiles, int nFlags);

CPLErr CPL_DLL EXIFExtractMetadata(char **&papszMetadata, void *fpL,
                                   int nOffset, int bSwabflag, int nTIFFHEADER,
                                   int &nExifOffset, int &nInterOffset,
                                   int &nGPSOffset);

int GDALValidateOpenOptions(GDALDriverH hDriver,
                            const char *const *papszOptionOptions);
int GDALValidateOptions(const char *pszOptionList,
                        const char *const *papszOptionsToValidate,
                        const char *pszErrorMessageOptionType,
                        const char *pszErrorMessageContainerName);

GDALRIOResampleAlg CPL_DLL
GDALRasterIOGetResampleAlg(const char *pszResampling);
const char *GDALRasterIOGetResampleAlg(GDALRIOResampleAlg eResampleAlg);

void GDALRasterIOExtraArgSetResampleAlg(GDALRasterIOExtraArg *psExtraArg,
                                        int nXSize, int nYSize, int nBufXSize,
                                        int nBufYSize);

GDALDataset *GDALCreateOverviewDataset(GDALDataset *poDS, int nOvrLevel,
                                       bool bThisLevelOnly);

// Should cover particular cases of #3573, #4183, #4506, #6578
// Behavior is undefined if fVal1 or fVal2 are NaN (should be tested before
// calling this function)

// TODO: The expression `abs(fVal1 + fVal2)` looks strange; is this a bug?
// Should this be `abs(fVal1) + abs(fVal2)` instead?

inline bool ARE_REAL_EQUAL(float fVal1, float fVal2, int ulp = 2)
{
    using std::abs;
    return fVal1 == fVal2 || /* Should cover infinity */
           abs(fVal1 - fVal2) <
               std::numeric_limits<float>::epsilon() * abs(fVal1 + fVal2) * ulp;
}

// We are using `std::numeric_limits<float>::epsilon()` for backward
// compatibility
inline bool ARE_REAL_EQUAL(double dfVal1, double dfVal2, int ulp = 2)
{
    using std::abs;
    return dfVal1 == dfVal2 || /* Should cover infinity */
           abs(dfVal1 - dfVal2) < std::numeric_limits<float>::epsilon() *
                                      abs(dfVal1 + dfVal2) * ulp;
}

double GDALAdjustNoDataCloseToFloatMax(double dfVal);

#define DIV_ROUND_UP(a, b) (((a) % (b)) == 0 ? ((a) / (b)) : (((a) / (b)) + 1))

// Number of data samples that will be used to compute approximate statistics
// (minimum value, maximum value, etc.)
#define GDALSTAT_APPROX_NUMSAMPLES 2500

void GDALSerializeGCPListToXML(CPLXMLNode *psParentNode,
                               const std::vector<gdal::GCP> &asGCPs,
                               const OGRSpatialReference *poGCP_SRS);
void GDALDeserializeGCPListFromXML(const CPLXMLNode *psGCPList,
                                   std::vector<gdal::GCP> &asGCPs,
                                   OGRSpatialReference **ppoGCP_SRS);

void GDALSerializeOpenOptionsToXML(CPLXMLNode *psParentNode,
                                   CSLConstList papszOpenOptions);
char CPL_DLL **
GDALDeserializeOpenOptionsFromXML(const CPLXMLNode *psParentNode);

int GDALCanFileAcceptSidecarFile(const char *pszFilename);

bool GDALCanReliablyUseSiblingFileList(const char *pszFilename);

typedef enum
{
    GSF_UNSIGNED_INT,
    GSF_SIGNED_INT,
    GSF_FLOATING_POINT,
} GDALBufferSampleFormat;

bool CPL_DLL GDALBufferHasOnlyNoData(const void *pBuffer, double dfNoDataValue,
                                     size_t nWidth, size_t nHeight,
                                     size_t nLineStride, size_t nComponents,
                                     int nBitsPerSample,
                                     GDALBufferSampleFormat nSampleFormat);

bool CPL_DLL GDALCopyNoDataValue(GDALRasterBand *poDstBand,
                                 GDALRasterBand *poSrcBand,
                                 bool *pbCannotBeExactlyRepresented = nullptr);

double CPL_DLL GDALGetNoDataValueCastToDouble(int64_t nVal);
double CPL_DLL GDALGetNoDataValueCastToDouble(uint64_t nVal);

// Remove me in GDAL 4.0. See GetMetadataItem() implementation
// Internal use in GDAL only !
// Declaration copied in swig/include/gdal.i
void CPL_DLL GDALEnablePixelTypeSignedByteWarning(GDALRasterBandH hBand,
                                                  bool b);

std::string CPL_DLL GDALGetCompressionFormatForJPEG(VSILFILE *fp);
std::string CPL_DLL GDALGetCompressionFormatForJPEG(const void *pBuffer,
                                                    size_t nBufferSize);

GDALRasterAttributeTable CPL_DLL *GDALCreateRasterAttributeTableFromMDArrays(
    GDALRATTableType eTableType,
    const std::vector<std::shared_ptr<GDALMDArray>> &apoArrays,
    const std::vector<GDALRATFieldUsage> &aeUsages);

GDALColorInterp CPL_DLL
GDALGetColorInterpFromSTACCommonName(const char *pszName);
const char CPL_DLL *
GDALGetSTACCommonNameFromColorInterp(GDALColorInterp eInterp);

std::string CPL_DLL GDALGetCacheDirectory();

bool GDALDoesFileOrDatasetExist(const char *pszName,
                                const char **ppszType = nullptr,
                                GDALDriver **ppDriver = nullptr);

std::string CPL_DLL
GDALGetMessageAboutMissingPluginDriver(GDALDriver *poMissingPluginDriver);

std::string GDALPrintDriverList(int nOptions, bool bJSON);

struct GDALColorAssociation
{
    double dfVal;
    int nR;
    int nG;
    int nB;
    int nA;
};

std::vector<GDALColorAssociation> GDALLoadTextColorMap(const char *pszFilename,
                                                       GDALRasterBand *poBand);

// Macro used so that Identify and driver metadata methods in drivers built
// as plugin can be duplicated in libgdal core and in the driver under different
// names
#ifdef PLUGIN_FILENAME
#define PLUGIN_SYMBOL_NAME(x) GDAL_core_##x
#else
#define PLUGIN_SYMBOL_NAME(x) GDAL_driver_##x
#endif

//! @endcond

#endif /* ndef GDAL_PRIV_H_INCLUDED */
