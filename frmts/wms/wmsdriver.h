/******************************************************************************
 *
 * Project:  WMS Client Driver
 * Purpose:  Implementation of Dataset and RasterBand classes for WMS
 *           and other similar services.
 * Author:   Adam Nowacki, nowak@xpam.de
 *
 ******************************************************************************
 * Copyright (c) 2007, Adam Nowacki
 * Copyright (c) 2008-2013, Even Rouault <even dot rouault at spatialys.com>
 * Copyright (c) 2017, Dmitry Baryshnikov, <polimax@mail.ru>
 * Copyright (c) 2017, NextGIS, <info@nextgis.com>
 *
 * SPDX-License-Identifier: MIT
 ****************************************************************************/

#ifndef WMSDRIVER_H_INCLUDED
#define WMSDRIVER_H_INCLUDED

#include <algorithm>
#include <cmath>
#include <map>
#include <set>
#include <vector>
#include <utility>

#include "cpl_conv.h"
#include "cpl_curl_priv.h"
#include "cpl_http.h"
#include "gdal_alg.h"
#include "gdal_pam.h"
#include "gdalwarper.h"
#include "ogr_spatialref.h"

#include "gdalhttp.h"

class GDALWMSDataset;
class GDALWMSRasterBand;

/* -------------------------------------------------------------------- */
/*      Helper functions.                                               */
/* -------------------------------------------------------------------- */
OGRSpatialReference ProjToSRS(const CPLString &proj);

// Decode s from encoding "base64" or "XMLencoded".
// If encoding is "file", s is the file name on input and file content on output
// If encoding is not recognized, does nothing
const char *WMSUtilDecode(CPLString &s, const char *encoding);

// Ensure that the url ends in ? or &
void URLPrepare(CPLString &url);
// void URLAppend(CPLString *url, const char *s);
// void URLAppendF(CPLString *url, const char *s, ...) CPL_PRINT_FUNC_FORMAT (2,
// 3); void URLAppend(CPLString *url, const CPLString &s);
CPLString BufferToVSIFile(GByte *buffer, size_t size);

int StrToBool(const char *p);
int URLSearchAndReplace(CPLString *base, const char *search, const char *fmt,
                        ...) CPL_PRINT_FUNC_FORMAT(3, 4);
/* Convert a.b.c.d to a * 0x1000000 + b * 0x10000 + c * 0x100 + d */
int VersionStringToInt(const char *version);

class GDALWMSImageRequestInfo
{
  public:
    double m_x0{}, m_y0{};
    double m_x1{}, m_y1{};
    int m_sx{}, m_sy{};
};

class GDALWMSDataWindow
{
  public:
    double m_x0, m_y0;
    double m_x1, m_y1;
    int m_sx, m_sy;
    int m_tx, m_ty, m_tlevel;

    enum
    {
        BOTTOM = -1,
        DEFAULT = 0,
        TOP = 1
    } m_y_origin;

    GDALWMSDataWindow()
        : m_x0(-180), m_y0(90), m_x1(180), m_y1(-90), m_sx(-1), m_sy(-1),
          m_tx(0), m_ty(0), m_tlevel(-1), m_y_origin(DEFAULT)
    {
    }
};

class GDALWMSTiledImageRequestInfo
{
  public:
    int m_x{}, m_y{};
    int m_level{};
};

/************************************************************************/
/*                         Mini Driver Related                          */
/************************************************************************/

class GDALWMSRasterIOHint
{
  public:
    GDALWMSRasterIOHint()
        : m_x0(0), m_y0(0), m_sx(0), m_sy(0), m_overview(0), m_valid(false)
    {
    }

    int m_x0;
    int m_y0;
    int m_sx;
    int m_sy;
    int m_overview;
    bool m_valid;
};

typedef enum
{
    OVERVIEW_ROUNDED,
    OVERVIEW_FLOOR
} GDALWMSOverviewDimComputationMethod;

class WMSMiniDriverCapabilities
{
  public:
    // Default capabilities, suitable in most cases
    WMSMiniDriverCapabilities() = default;

    int m_has_getinfo{0};  // Does it have meaningful implementation
    int m_has_geotransform{1};
    GDALWMSOverviewDimComputationMethod m_overview_dim_computation_method{
        OVERVIEW_ROUNDED};
};

/* All data returned by mini-driver as pointer should remain valid for
   mini-driver lifetime and should be freed by mini-driver destructor unless
   otherwise specified.
 */

// Base class for minidrivers
// A minidriver has to implement at least the Initialize and the
// TiledImageRequest
//
class WMSMiniDriver
{
    friend class GDALWMSDataset;

    CPL_DISALLOW_COPY_ASSIGN(WMSMiniDriver)

  public:
    WMSMiniDriver() : m_parent_dataset(nullptr)
    {
        m_oSRS.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
    }

    virtual ~WMSMiniDriver();

  public:
    // MiniDriver specific initialization from XML, required
    // Called once at the beginning of the dataset initialization
    virtual CPLErr Initialize(CPLXMLNode *config, char **papszOpenOptions) = 0;

    // Called once at the end of the dataset initialization
    virtual CPLErr EndInit()
    {
        return CE_None;
    }

    // Error message returned in url, required
    // Set error message in request.Error
    // If tile doesn't exist serverside, set request.range to "none"
    virtual CPLErr
    TiledImageRequest(CPL_UNUSED WMSHTTPRequest &,
                      CPL_UNUSED const GDALWMSImageRequestInfo &iri,
                      CPL_UNUSED const GDALWMSTiledImageRequestInfo &tiri) = 0;

    // change capabilities to be used by the parent
    virtual void GetCapabilities(CPL_UNUSED WMSMiniDriverCapabilities *caps)
    {
    }

    // signal by setting the m_has_getinfo in the GetCapabilities call
    virtual void
    GetTiledImageInfo(CPL_UNUSED CPLString &url,
                      CPL_UNUSED const GDALWMSImageRequestInfo &iri,
                      CPL_UNUSED const GDALWMSTiledImageRequestInfo &tiri,
                      CPL_UNUSED int nXInBlock, CPL_UNUSED int nYInBlock)
    {
    }

    virtual const OGRSpatialReference &GetSpatialRef() const
    {
        return m_oSRS;
    }

    virtual char **GetMetadataDomainList()
    {
        return nullptr;
    }

  protected:
    CPLString m_base_url{};
    OGRSpatialReference m_oSRS{};
    GDALWMSDataset *m_parent_dataset{};
};

class WMSMiniDriverFactory
{
  public:
    WMSMiniDriverFactory()
    {
    }

    virtual ~WMSMiniDriverFactory();

  public:
    virtual WMSMiniDriver *New() const = 0;
    CPLString m_name{};
};

// Interface with the global mini driver manager
WMSMiniDriver *NewWMSMiniDriver(const CPLString &name);
void WMSRegisterMiniDriverFactory(WMSMiniDriverFactory *mdf);
void WMSDeregisterMiniDrivers(GDALDriver *);

// WARNING: Called by GDALDestructor, unsafe to use any static objects
void WMSDeregister(GDALDriver *);

/************************************************************************/
/*                            GDALWMSCache                              */
/************************************************************************/
enum GDALWMSCacheItemStatus
{
    CACHE_ITEM_NOT_FOUND,
    CACHE_ITEM_OK,
    CACHE_ITEM_EXPIRED
};

class GDALWMSCacheImpl
{
  public:
    GDALWMSCacheImpl(const CPLString &soPath, CPLXMLNode * /*pConfig*/)
        : m_soPath(soPath)
    {
    }

    virtual ~GDALWMSCacheImpl();

    virtual CPLErr Insert(const char *pszKey, const CPLString &osFileName) = 0;
    virtual enum GDALWMSCacheItemStatus
    GetItemStatus(const char *pszKey) const = 0;
    virtual GDALDataset *GetDataset(const char *pszKey,
                                    char **papszOpenOptions) const = 0;
    virtual void Clean() = 0;
    virtual int GetCleanThreadRunTimeout() = 0;

  protected:
    CPLString m_soPath;
};

class GDALWMSCache
{
    friend class GDALWMSDataset;

  public:
    GDALWMSCache();
    ~GDALWMSCache();

  public:
    CPLErr Initialize(const char *pszUrl, CPLXMLNode *pConfig);
    CPLErr Insert(const char *pszKey, const CPLString &osFileName);
    enum GDALWMSCacheItemStatus GetItemStatus(const char *pszKey) const;
    GDALDataset *GetDataset(const char *pszKey, char **papszOpenOptions) const;
    void Clean();

  protected:
    CPLString CachePath() const
    {
        return m_osCachePath;
    }

  protected:
    CPLString m_osCachePath{};
    bool m_bIsCleanThreadRunning = false;
    time_t m_nCleanThreadLastRunTime = 0;

  private:
    GDALWMSCacheImpl *m_poCache = nullptr;
    CPLJoinableThread *m_hThread = nullptr;

    CPL_DISALLOW_COPY_ASSIGN(GDALWMSCache)
};

/************************************************************************/
/*                            GDALWMSDataset                            */
/************************************************************************/

class GDALWMSDataset final : public GDALPamDataset
{
    friend class GDALWMSRasterBand;

    CPL_DISALLOW_COPY_ASSIGN(GDALWMSDataset)

  public:
    GDALWMSDataset();
    virtual ~GDALWMSDataset();

    const OGRSpatialReference *GetSpatialRef() const override;
    CPLErr SetSpatialRef(const OGRSpatialReference *poSRS) override;

    virtual CPLErr GetGeoTransform(GDALGeoTransform &gt) const override;
    virtual CPLErr SetGeoTransform(const GDALGeoTransform &gt) override;
    virtual CPLErr AdviseRead(int x0, int y0, int sx, int sy, int bsx, int bsy,
                              GDALDataType bdt, int band_count, int *band_map,
                              char **options) override;

    virtual char **GetMetadataDomainList() override;
    virtual const char *GetMetadataItem(const char *pszName,
                                        const char *pszDomain = "") override;

    void SetColorTable(GDALColorTable *pct)
    {
        m_poColorTable = pct;
    }

    void mSetBand(int i, GDALRasterBand *band)
    {
        SetBand(i, band);
    }

    GDALWMSRasterBand *mGetBand(int i)
    {
        return reinterpret_cast<GDALWMSRasterBand *>(GetRasterBand(i));
    }

    const GDALWMSDataWindow *WMSGetDataWindow() const
    {
        return &m_data_window;
    }

    void WMSSetBlockSize(int x, int y)
    {
        m_block_size_x = x;
        m_block_size_y = y;
    }

    void WMSSetRasterSize(int x, int y)
    {
        nRasterXSize = x;
        nRasterYSize = y;
    }

    void WMSSetBandsCount(int count)
    {
        nBands = count;
    }

    void WMSSetClamp(bool flag = true)
    {
        m_clamp_requests = flag;
    }

    void WMSSetDataType(GDALDataType type)
    {
        m_data_type = type;
    }

    void WMSSetDataWindow(const GDALWMSDataWindow &window)
    {
        m_data_window = window;
    }

    void WMSSetDefaultBlockSize(int x, int y)
    {
        m_default_block_size_x = x;
        m_default_block_size_y = y;
    }

    void WMSSetDefaultDataWindowCoordinates(double x0, double y0, double x1,
                                            double y1)
    {
        m_default_data_window.m_x0 = x0;
        m_default_data_window.m_y0 = y0;
        m_default_data_window.m_x1 = x1;
        m_default_data_window.m_y1 = y1;
    }

    void WMSSetDefaultTileCount(int tilecountx, int tilecounty)
    {
        m_default_tile_count_x = tilecountx;
        m_default_tile_count_y = tilecounty;
    }

    void WMSSetDefaultTileLevel(int tlevel)
    {
        m_default_data_window.m_tlevel = tlevel;
    }

    void WMSSetDefaultOverviewCount(int overview_count)
    {
        m_default_overview_count = overview_count;
    }

    void WMSSetNeedsDataWindow(bool flag)
    {
        m_bNeedsDataWindow = flag;
    }

    static void list2vec(std::vector<double> &v, const char *pszList)
    {
        if ((pszList == nullptr) || (pszList[0] == 0))
            return;
        char **papszTokens = CSLTokenizeString2(
            pszList, " \t\n\r", CSLT_STRIPLEADSPACES | CSLT_STRIPENDSPACES);
        v.clear();
        for (int i = 0; i < CSLCount(papszTokens); i++)
            v.push_back(CPLStrtod(papszTokens[i], nullptr));
        CSLDestroy(papszTokens);
    }

    void WMSSetNoDataValue(const char *pszNoData)
    {
        list2vec(vNoData, pszNoData);
    }

    void WMSSetMinValue(const char *pszMin)
    {
        list2vec(vMin, pszMin);
    }

    void WMSSetMaxValue(const char *pszMax)
    {
        list2vec(vMax, pszMax);
    }

    // Set open options for tiles
    // Works like a <set>, only one entry with a give name can exist, last one
    // set wins If the value is null, the entry is deleted
    void SetTileOO(const char *pszName, const char *pszValue);

    void SetXML(const char *psz)
    {
        m_osXML.clear();
        if (psz)
            m_osXML = psz;
    }

    static GDALDataset *Open(GDALOpenInfo *poOpenInfo);
    static GDALDataset *CreateCopy(const char *pszFilename,
                                   GDALDataset *poSrcDS, int bStrict,
                                   char **papszOptions,
                                   GDALProgressFunc pfnProgress,
                                   void *pProgressData);

    const char *const *GetHTTPRequestOpts();

    static const char *GetServerConfig(const char *URI,
                                       char **papszHTTPOptions);
    static void DestroyCfgMutex();
    static void ClearConfigCache();

  protected:
    virtual CPLErr IRasterIO(GDALRWFlag rw, int x0, int y0, int sx, int sy,
                             void *buffer, int bsx, int bsy, GDALDataType bdt,
                             int band_count, BANDMAP_TYPE band_map,
                             GSpacing nPixelSpace, GSpacing nLineSpace,
                             GSpacing nBandSpace,
                             GDALRasterIOExtraArg *psExtraArg) override;
    CPLErr Initialize(CPLXMLNode *config, char **papszOpenOptions);

    GDALWMSDataWindow m_data_window{};
    WMSMiniDriver *m_mini_driver{};
    WMSMiniDriverCapabilities m_mini_driver_caps{};
    GDALWMSCache *m_cache{};
    OGRSpatialReference m_oSRS{};
    GDALColorTable *m_poColorTable{};
    std::vector<double> vNoData{};
    std::vector<double> vMin{};
    std::vector<double> vMax{};
    GDALDataType m_data_type{GDT_Unknown};
    int m_block_size_x{};
    int m_block_size_y{};
    GDALWMSRasterIOHint m_hint{};
    int m_use_advise_read{};
    int m_verify_advise_read{};
    int m_offline_mode{};
    int m_http_max_conn{};
    int m_http_timeout{};
    char **m_http_options{};
    // Open Option list for tiles
    char **m_tileOO{};
    int m_clamp_requests{};
    int m_unsafeSsl{};
    std::set<int> m_http_zeroblock_codes{};
    int m_zeroblock_on_serverexceptions{};
    CPLString m_osUserAgent{};
    CPLString m_osReferer{};
    CPLString m_osUserPwd{};
    std::string m_osAccept{};  // HTTP Accept header

    GDALWMSDataWindow m_default_data_window{};
    int m_default_block_size_x{};
    int m_default_block_size_y{};
    int m_default_tile_count_x{};
    int m_default_tile_count_y{};
    int m_default_overview_count{};

    bool m_bNeedsDataWindow{};

    CPLString m_osXML{};

    // Per session cache of server configurations
    typedef std::map<CPLString, CPLString> StringMap_t;
    static CPLMutex *cfgmtx;
    static StringMap_t cfg;
};

/************************************************************************/
/*                            GDALWMSRasterBand                         */
/************************************************************************/

class GDALWMSRasterBand final : public GDALPamRasterBand
{
    friend class GDALWMSDataset;
    void ComputeRequestInfo(GDALWMSImageRequestInfo &iri,
                            GDALWMSTiledImageRequestInfo &tiri, int x, int y);

    CPLString osMetadataItem{};
    CPLString osMetadataItemURL{};

    CPL_DISALLOW_COPY_ASSIGN(GDALWMSRasterBand)

  public:
    GDALWMSRasterBand(GDALWMSDataset *parent_dataset, int band, double scale);
    virtual ~GDALWMSRasterBand();
    bool AddOverview(double scale);
    virtual double GetNoDataValue(int *) override;
    virtual double GetMinimum(int *) override;
    virtual double GetMaximum(int *) override;
    virtual GDALColorTable *GetColorTable() override;
    virtual CPLErr AdviseRead(int x0, int y0, int sx, int sy, int bsx, int bsy,
                              GDALDataType bdt, char **options) override;

    virtual GDALColorInterp GetColorInterpretation() override;
    virtual CPLErr SetColorInterpretation(GDALColorInterp) override;
    virtual CPLErr IReadBlock(int x, int y, void *buffer) override;
    virtual CPLErr IRasterIO(GDALRWFlag rw, int x0, int y0, int sx, int sy,
                             void *buffer, int bsx, int bsy, GDALDataType bdt,
                             GSpacing nPixelSpace, GSpacing nLineSpace,
                             GDALRasterIOExtraArg *psExtraArg) override;
    virtual int HasArbitraryOverviews() override;
    virtual int GetOverviewCount() override;
    virtual GDALRasterBand *GetOverview(int n) override;

    virtual char **GetMetadataDomainList() override;
    virtual const char *GetMetadataItem(const char *pszName,
                                        const char *pszDomain = "") override;

  protected:
    CPLErr ReadBlocks(int x, int y, void *buffer, int bx0, int by0, int bx1,
                      int by1, int advise_read);
    bool IsBlockInCache(int x, int y);
    CPLErr AskMiniDriverForBlock(WMSHTTPRequest &request, int x, int y);
    CPLErr ReadBlockFromCache(const char *pszKey, int x, int y,
                              int to_buffer_band, void *buffer,
                              int advise_read);
    CPLErr ReadBlockFromFile(const CPLString &soFileName, int x, int y,
                             int to_buffer_band, void *buffer, int advise_read);
    CPLErr ReadBlockFromDataset(GDALDataset *ds, int x, int y,
                                int to_buffer_band, void *buffer,
                                int advise_read);
    CPLErr EmptyBlock(int x, int y, int to_buffer_band, void *buffer);
    static CPLErr ReportWMSException(const char *file_name);

  protected:
    GDALWMSDataset *m_parent_dataset{};
    double m_scale{};
    std::vector<GDALWMSRasterBand *> m_overviews{};
    int m_overview{};
    GDALColorInterp m_color_interp{};
    int m_nAdviseReadBX0{};
    int m_nAdviseReadBY0{};
    int m_nAdviseReadBX1{};
    int m_nAdviseReadBY1{};
};

#endif /* notdef WMSDRIVER_H_INCLUDED */
