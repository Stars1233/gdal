/******************************************************************************
 * Project:  OGR
 * Purpose:  OGRGMLASDriver implementation
 * Author:   Even Rouault, <even dot rouault at spatialys dot com>
 *
 * Initial development funded by the European Earth observation programme
 * Copernicus
 *
 ******************************************************************************
 * Copyright (c) 2016, Even Rouault, <even dot rouault at spatialys dot com>
 *
 * SPDX-License-Identifier: MIT
 ****************************************************************************/

#ifndef OGR_GMLAS_INCLUDED
#define OGR_GMLAS_INCLUDED

#include "xercesc_headers.h"
#include "ogr_xerces.h"

#include "cpl_vsi_virtual.h"
#include "gdal_priv.h"
#include "ogrsf_frmts.h"

#include "ogr_gmlas_consts.h"

#include <list>
#include <set>
#include <map>
#include <vector>

typedef std::pair<CPLString, CPLString> PairURIFilename;

typedef enum
{
    GMLAS_SWAP_AUTO,
    GMLAS_SWAP_YES,
    GMLAS_SWAP_NO,
} GMLASSwapCoordinatesEnum;

GDALDataset *OGRGMLASDriverCreateCopy(const char *pszFilename,
                                      GDALDataset *poSrcDS, int /*bStrict*/,
                                      char **papszOptions,
                                      GDALProgressFunc pfnProgress,
                                      void *pProgressData);

/************************************************************************/
/*                          IGMLASInputSourceClosing                    */
/************************************************************************/

class IGMLASInputSourceClosing
{
  public:
    virtual ~IGMLASInputSourceClosing();

    virtual void notifyClosing(const CPLString &osFilename) = 0;
};

/************************************************************************/
/*                         GMLASResourceCache                           */
/************************************************************************/

class GMLASResourceCache
{
  protected:
    bool m_bHasCheckedCacheDirectory = false;
    std::string m_osCacheDirectory{};
    bool m_bRefresh = false;
    bool m_bAllowDownload = false;
    std::set<std::string> m_aoSetRefreshedFiles{};

    static bool
    RecursivelyCreateDirectoryIfNeeded(const std::string &osDirname);
    bool RecursivelyCreateDirectoryIfNeeded();

    std::string GetCachedFilename(const std::string &osResource);

  public:
    void SetCacheDirectory(const std::string &osCacheDirectory);

    void SetRefreshMode(bool bRefresh)
    {
        m_bRefresh = bRefresh;
    }

    void SetAllowDownload(bool bVal)
    {
        m_bAllowDownload = bVal;
    }
};

/************************************************************************/
/*                          GMLASXSDCache                               */
/************************************************************************/

class GMLASXSDCache final : public GMLASResourceCache
{
    bool CacheAllGML321();
    bool CacheAllISO20070417();

  public:
    VSILFILE *Open(const std::string &osResource, const std::string &osBasePath,
                   std::string &osOutFilename);
};

/************************************************************************/
/*                     GMLASBaseEntityResolver                          */
/************************************************************************/

class GMLASBaseEntityResolver : public EntityResolver,
                                public IGMLASInputSourceClosing
{
  protected:
    std::vector<CPLString> m_aosPathStack{};
    GMLASXSDCache &m_oCache;
    CPLString m_osGMLVersionFound{};
    std::set<CPLString> m_oSetSchemaURLs{};
    bool m_bFoundNonOfficialGMLSchemaLocation = false;
    bool m_bSubstituteWithOGCSchemaLocation = false;

  public:
    GMLASBaseEntityResolver(const CPLString &osBasePath, GMLASXSDCache &oCache);
    virtual ~GMLASBaseEntityResolver();

    void SetBasePath(const CPLString &osBasePath);

    const CPLString &GetGMLVersionFound() const
    {
        return m_osGMLVersionFound;
    }

    const std::set<CPLString> &GetSchemaURLS() const
    {
        return m_oSetSchemaURLs;
    }

    void SetSubstituteWithOGCSchemaLocation(bool b)
    {
        m_bSubstituteWithOGCSchemaLocation = b;
    }

    bool GetFoundNonOfficialGMLSchemaLocation() const
    {
        return m_bFoundNonOfficialGMLSchemaLocation;
    }

    virtual void notifyClosing(const CPLString &osFilename) override;
    virtual InputSource *resolveEntity(const XMLCh *const publicId,
                                       const XMLCh *const systemId) override;

    virtual void
    DoExtraSchemaProcessing(const CPLString &osFilename,
                            const std::shared_ptr<VSIVirtualHandle> &fp);
};

/************************************************************************/
/*                          GMLASInputSource                            */
/************************************************************************/

class GMLASInputSource final : public InputSource
{
    std::shared_ptr<VSIVirtualHandle> m_fp{};
    int m_nCounter = 0;
    int *m_pnCounter = nullptr;
    CPLString m_osFilename{};
    IGMLASInputSourceClosing *m_cbk = nullptr;

    CPL_DISALLOW_COPY_ASSIGN(GMLASInputSource)

  public:
    GMLASInputSource(
        const char *pszFilename, const std::shared_ptr<VSIVirtualHandle> &fp,
        MemoryManager *const manager = XMLPlatformUtils::fgMemoryManager);
    virtual ~GMLASInputSource();

    virtual BinInputStream *makeStream() const override;

    void SetClosingCallback(IGMLASInputSourceClosing *cbk);
};

/************************************************************************/
/*                            GMLASErrorHandler                         */
/************************************************************************/

class GMLASErrorHandler : public ErrorHandler
{
  public:
    GMLASErrorHandler() = default;

    void SetSchemaFullCheckingEnabled(bool b)
    {
        m_bSchemaFullChecking = b;
    }

    void SetHandleMultipleImportsEnabled(bool b)
    {
        m_bHandleMultipleImports = b;
    }

    void SetHideGMLTypeNotFound(bool b)
    {
        m_bHideGMLTypeNotFound = b;
    }

    const std::string &GetGMLTypeNotFoundError() const
    {
        return m_osGMLTypeNotFoundError;
    }

    bool hasFailed() const
    {
        return m_bFailed;
    }

    virtual void warning(const SAXParseException &e) override;
    virtual void error(const SAXParseException &e) override;
    virtual void fatalError(const SAXParseException &e) override;

    virtual void resetErrors() override
    {
        m_bFailed = false;
    }

  private:
    bool m_bFailed = false;
    bool m_bSchemaFullChecking = false;
    bool m_bHandleMultipleImports = false;
    bool m_bHideGMLTypeNotFound = false;
    std::string m_osGMLTypeNotFoundError{};

    void handle(const SAXParseException &e, CPLErr eErr);
};

/************************************************************************/
/*                        GMLASXLinkResolutionConf                      */
/************************************************************************/

class GMLASXLinkResolutionConf
{
  public:
    /* See data/gmlasconf.xsd for docomentation of the fields */

    typedef enum
    {
        RawContent,
        FieldsFromXPath
    } ResolutionMode;

    int m_nTimeOut = 0;

    int m_nMaxFileSize = MAX_FILE_SIZE_DEFAULT;

    int m_nMaxGlobalResolutionTime = 0;

    CPLString m_osProxyServerPort{};

    CPLString m_osProxyUserPassword{};

    CPLString m_osProxyAuth{};

    CPLString m_osCacheDirectory{};

    bool m_bDefaultResolutionEnabled = DEFAULT_RESOLUTION_ENABLED_DEFAULT;

    bool m_bDefaultAllowRemoteDownload = ALLOW_REMOTE_DOWNLOAD_DEFAULT;

    ResolutionMode m_eDefaultResolutionMode = RawContent;

    int m_nDefaultResolutionDepth = 1;

    bool m_bDefaultCacheResults = CACHE_RESULTS_DEFAULT;

    bool m_bResolveInternalXLinks = INTERNAL_XLINK_RESOLUTION_DEFAULT;

    struct URLSpecificResolution
    {
        CPLString m_osURLPrefix{};

        std::vector<std::pair<CPLString, CPLString>>
            m_aosNameValueHTTPHeaders{};

        bool m_bAllowRemoteDownload = false;

        ResolutionMode m_eResolutionMode = RawContent;

        int m_nResolutionDepth = 1;

        bool m_bCacheResults = false;

        struct XPathDerivedField
        {
            CPLString m_osName{};

            CPLString m_osType{};

            CPLString m_osXPath{};
        };

        std::vector<XPathDerivedField> m_aoFields{};
    };

    std::vector<URLSpecificResolution> m_aoURLSpecificRules{};

    GMLASXLinkResolutionConf() = default;

    bool LoadFromXML(CPLXMLNode *psRoot);
};

/************************************************************************/
/*                          GMLASConfiguration                          */
/************************************************************************/

class GMLASConfiguration
{
  public:
    /** Whether remote schemas are allowed to be download. */
    bool m_bAllowRemoteSchemaDownload = ALLOW_REMOTE_SCHEMA_DOWNLOAD_DEFAULT;

    /** Whether a ogr_pkid attribute should always be generated. */
    bool m_bAlwaysGenerateOGRId = ALWAYS_GENERATE_OGR_ID_DEFAULT;

    /** Whether to remove layers found to be unused in initial scan pass */
    bool m_bRemoveUnusedLayers = REMOVE_UNUSED_LAYERS_DEFAULT;

    /** Whether to remove fields found to be unused in initial scan pass */
    bool m_bRemoveUnusedFields = REMOVE_UNUSED_FIELDS_DEFAULT;

    /** Whether repeated strings, integers, reals should be in corresponding
        OGR array types. */
    bool m_bUseArrays = USE_ARRAYS_DEFAULT;

    /** Whether OGR field null state should be used. */
    bool m_bUseNullState = USE_NULL_STATE_DEFAULT;

    /** Whether geometries should be stored as XML in a OGR string field. */
    bool m_bIncludeGeometryXML = INCLUDE_GEOMETRY_XML_DEFAULT;

    /** Whether, when dealing with schemas that import the
        GML namespace, and that at least one of them has
        elements that derive from gml:_Feature or
        gml:AbstractFeatureonly, only such elements should be
        instantiated as OGR layers, during the first pass that
        iterates over top level elements of the imported
        schemas. */
    bool m_bInstantiateGMLFeaturesOnly = INSTANTIATE_GML_FEATURES_ONLY_DEFAULT;

    /** Maximum length of layer and field identifiers*/
    int m_nIdentifierMaxLength = 0;

    /** Whether case insensitive comparison should be used for identifier
     * equality testing */
    bool m_bCaseInsensitiveIdentifier = CASE_INSENSITIVE_IDENTIFIER_DEFAULT;

    /** Whether to launder identifiers like postgresql does */
    bool m_bPGIdentifierLaundering = PG_IDENTIFIER_LAUNDERING_DEFAULT;

    /* Maximum number of fields in an element considered for flattening. */
    int m_nMaximumFieldsForFlattening = MAXIMUM_FIELDS_FLATTENING_DEFAULT;

    /** Whether remote XSD schemas should be locally cached. */
    bool m_bAllowXSDCache = ALLOW_XSD_CACHE_DEFAULT;

    /** Cache directory for cached XSD schemas. */
    CPLString m_osXSDCacheDirectory{};

    /** Whether to enable schema full checking. */
    bool m_bSchemaFullChecking = SCHEMA_FULL_CHECKING_DEFAULT;

    /** Whether to allow multiple imports of the same namespace. */
    bool m_bHandleMultipleImports = HANDLE_MULTIPLE_IMPORTS_DEFAULT;

    /** Whether validation of document against schema should be done.  */
    bool m_bValidate = VALIDATE_DEFAULT;

    /** Whether a validation error should prevent dataset opening.  */
    bool m_bFailIfValidationError = FAIL_IF_VALIDATION_ERROR_DEFAULT;

    /** Whether technical layers should be exposed.  */
    bool m_bExposeMetadataLayers = WARN_IF_EXCLUDED_XPATH_FOUND_DEFAULT;

    /** For flatening rules, map prefix namespace to its URI */
    std::map<CPLString, CPLString> m_oMapPrefixToURIFlatteningRules{};

    std::vector<CPLString> m_osForcedFlattenedXPath{};

    std::vector<CPLString> m_osDisabledFlattenedXPath{};

    enum SWEActivationMode
    {
        SWE_ACTIVATE_IF_NAMESPACE_FOUND,
        SWE_ACTIVATE_TRUE,
        SWE_ACTIVATE_FALSE
    };

    /** If and when activate SWE special processings */
    SWEActivationMode m_eSWEActivationMode = SWE_ACTIVATE_IF_NAMESPACE_FOUND;

    /** If enabling swe:DataRecord parsing */
    bool m_bSWEProcessDataRecord = SWE_PROCESS_DATA_RECORD_DEFAULT;

    /** If enabling swe:DataArray parsing */
    bool m_bSWEProcessDataArray = SWE_PROCESS_DATA_ARRAY_DEFAULT;

    /** For ignored xpaths, map prefix namespace to its URI */
    std::map<CPLString, CPLString> m_oMapPrefixToURIIgnoredXPaths{};

    /** Ignored xpaths */
    std::vector<CPLString> m_aosIgnoredXPaths{};

    /** For type constraints, map prefix namespace to its URI */
    std::map<CPLString, CPLString> m_oMapPrefixToURITypeConstraints{};

    /** Map an XPath to a list of potential types for its children */
    std::map<CPLString, std::vector<CPLString>>
        m_oMapChildrenElementsConstraints{};

    /* Beginning of Writer config */

    /** Number of spaces for indentation */
    int m_nIndentSize = INDENT_SIZE_DEFAULT;

    CPLString m_osComment{};

    /** End of line format: "CRLF" or "LR" */
    CPLString m_osLineFormat{};

    /** "SHORT", "OGC_URN" or "OGC_URL" */
    CPLString m_osSRSNameFormat = szSRSNAME_DEFAULT;

    /** "WFS2_FEATURECOLLECTION" or "GMLAS_FEATURECOLLECTION" */
    CPLString m_osWrapping = szWFS2_FEATURECOLLECTION;

    /** XML datetime or empty for current time */
    CPLString m_osTimestamp{};

    /** Path or URL to OGC WFS 2.0 schema. */
    CPLString m_osWFS20SchemaLocation = szWFS20_SCHEMALOCATION;

    /* End of Writer config */

    /** Whether a warning should be emitted when an element or attribute is
        found in the document parsed, but ignored because of the ignored
        XPath defined.  */
    std::map<CPLString, bool> m_oMapIgnoredXPathToWarn{};

    GMLASXLinkResolutionConf m_oXLinkResolution{};

    GMLASConfiguration() = default;

    bool Load(const char *pszFilename);
    void Finalize();

    static std::string GetDefaultConfFile(bool &bUnlinkAfterUse);
};

/************************************************************************/
/*                          GMLASXLinkResolver                          */
/************************************************************************/

class GMLASXLinkResolver final : public GMLASResourceCache
{
    GMLASXLinkResolutionConf m_oConf{};
    int m_nGlobalResolutionTime = 0;

    std::map<CPLString, CPLString> m_oMapURLToContent{};
    std::map<size_t, std::vector<CPLString>> m_oMapFileSizeToURLs{};
    size_t m_nMaxRAMCacheSize = 0;
    size_t m_nCurrentRAMCacheSize = 0;

    CPLString FetchRawContent(const CPLString &osURL, const char *pszHeaders);

    CPLString GetRawContent(const CPLString &osURL, const char *pszHeaders,
                            bool bAllowRemoteDownload, bool bCacheResults);

  public:
    GMLASXLinkResolver();

    void SetConf(const GMLASXLinkResolutionConf &oConf);

    const GMLASXLinkResolutionConf &GetConf() const
    {
        return m_oConf;
    }

    bool IsRawContentResolutionEnabled() const;
    int GetMatchingResolutionRule(const CPLString &osURL) const;
    CPLString GetRawContent(const CPLString &osURL);
    CPLString GetRawContentForRule(const CPLString &osURL, int nIdxRule);
};

/************************************************************************/
/*                           GMLASXPathMatcher                          */
/************************************************************************/

/** Object to compares a user provided XPath against a set of test XPaths */
class GMLASXPathMatcher
{
    struct XPathComponent
    {
        CPLString m_osValue{};
        bool m_bDirectChild = false;
    };

    /** For reference xpaths, map prefix namespace to its URI */
    std::map<CPLString, CPLString> m_oMapPrefixToURIReferenceXPaths{};

    /** Reference xpaths */
    std::vector<CPLString> m_aosReferenceXPathsUncompiled{};

    /** Reference xpaths "compiled" */
    std::vector<std::vector<XPathComponent>> m_aosReferenceXPaths{};

    static bool MatchesRefXPath(const CPLString &osXPath,
                                const std::vector<XPathComponent> &oRefXPath);

  public:
    void SetRefXPaths(
        const std::map<CPLString, CPLString> &oMapPrefixToURIReferenceXPaths,
        const std::vector<CPLString> &aosReferenceXPaths);

    void SetDocumentMapURIToPrefix(
        const std::map<CPLString, CPLString> &oMapURIToPrefix);

    /** Return true if osXPath matches one of the XPath of
        m_aosReferenceXPaths */
    bool MatchesRefXPath(const CPLString &osXPath,
                         CPLString &osOutMatchedXPath) const;

    const std::map<CPLString, CPLString> &GetMapPrefixToURI() const
    {
        return m_oMapPrefixToURIReferenceXPaths;
    }
};

/************************************************************************/
/*                            GMLASFieldType                            */
/************************************************************************/

/** Enumeration for XML primitive types */
typedef enum
{
    GMLAS_FT_STRING,
    GMLAS_FT_ID,
    GMLAS_FT_BOOLEAN,
    GMLAS_FT_SHORT,
    GMLAS_FT_INT32,
    GMLAS_FT_INT64,
    GMLAS_FT_FLOAT,
    GMLAS_FT_DOUBLE,
    GMLAS_FT_DECIMAL,
    GMLAS_FT_DATE,
    GMLAS_FT_GYEAR,
    GMLAS_FT_GYEAR_MONTH,
    GMLAS_FT_TIME,
    GMLAS_FT_DATETIME,
    GMLAS_FT_BASE64BINARY,
    GMLAS_FT_HEXBINARY,
    GMLAS_FT_ANYURI,
    GMLAS_FT_ANYTYPE,
    GMLAS_FT_ANYSIMPLETYPE,
    GMLAS_FT_GEOMETRY,  // this one isn't a XML primitive type.
} GMLASFieldType;

/************************************************************************/
/*                              GMLASField                              */
/************************************************************************/

class GMLASField
{
  public:
    typedef enum
    {
        /** Field that is going to be instantiated as a OGR field */
        REGULAR,

        /** Non-instanciable field. The corresponding element to the XPath
            is stored in a child layer that will reference back to the
            main layer. */
        PATH_TO_CHILD_ELEMENT_NO_LINK,

        /** Field that will store the PKID of a child element */
        PATH_TO_CHILD_ELEMENT_WITH_LINK,

        /** Non-instanciable field. The corresponding element to the XPath
            is stored in a child layer. And the link between both will be
            done through a junction table. */
        PATH_TO_CHILD_ELEMENT_WITH_JUNCTION_TABLE,

        /** Non-instanciable field. Corresponds to a group of an element. */
        GROUP
    } Category;

  private:
    CPLString m_osName{};                     /**< Field name */
    GMLASFieldType m_eType = GMLAS_FT_STRING; /**< Field type */
    OGRwkbGeometryType m_eGeomType = wkbNone; /**< Field geometry type */
    CPLString m_osTypeName{};                 /**< Original XSD type */
    int m_nWidth = 0;                         /**< Field width */
    bool m_bNotNullable = false; /**< If the field is not nullable */

    /** If the field is an array (from OGR types point of view) */
    bool m_bArray = false;
    bool m_bList = false; /**< If the field is a list (a xs:list) */

    /** Category of the field. */
    Category m_eCategory = REGULAR;

    /** XPath of the field. */
    CPLString m_osXPath{};

    /** Set of XPath that are linked to this field.
        This is used for cases where a gml:AbstractGeometry element is
        referenced. In which case all possible realizations of this
        element are listed. Will be used with eType == GMLAS_FT_ANYTYPE
        to store XML blob on parsing. */
    std::vector<CPLString> m_aosXPath{};

    CPLString m_osFixedValue{};   /**< Value of fixed='' attribute */
    CPLString m_osDefaultValue{}; /**< Value of default='' attribute */

    /** Minimum number of occurrences. Might be -1 if unset */
    int m_nMinOccurs = -1;

    /** Maximum number of occurrences, or MAXOCCURS_UNLIMITED. Might be
        -1 if unset. */
    int m_nMaxOccurs = -1;

    /** For a PATH_TO_CHILD_ELEMENT_NO_LINK, whether maxOccurs>1 is on the
        sequence rather than on the element */
    bool m_bRepetitionOnSequence = false;

    /** In case of m_eType == GMLAS_FT_ANYTYPE whether the current element
        must be stored in the XML blob (if false, only its children) */
    bool m_bIncludeThisEltInBlob = false;

    /** Only used for PATH_TO_CHILD_ELEMENT_WITH_JUNCTION_TABLE. The XPath
        of the abstract element (the concrete XPath is in m_osXPath).
        e.g myns:mainElt/myns:subEltAbstract whereas the concrete XPath
        is myns:mainElt/myns:subEltRealization */
    CPLString m_osAbstractElementXPath{};

    /** Only used for PATH_TO_CHILD_ELEMENT_WITH_LINK and
        PATH_TO_CHILD_ELEMENT_WITH_JUNCTION_TABLE (and also for
        PATH_TO_CHILD_ELEMENT_NO_LINK and GROUP but for metadata layers only).
        The XPath of the child element. */
    CPLString m_osRelatedClassXPath{};

    /** Only use for PATH_TO_CHILD_ELEMENT_WITH_JUNCTION_TABLE. Name of
        the junction layer to consult for this field. Only used by
        writer code. */
    CPLString m_osJunctionLayer{};

    /** Dirty hack to register attributes with fixed values, despite being
        in the XPath ignored list. Needed to avoid warning when doing validation
     */
    bool m_bIgnored = false;

    /** Documentation from schema */
    CPLString m_osDoc{};

    /** For elements within xs:choice */
    bool m_bMayAppearOutOfOrder = false;

  public:
    GMLASField() = default;

    void SetName(const CPLString &osName)
    {
        m_osName = osName;
    }

    void SetType(GMLASFieldType eType, const char *pszTypeName);

    void SetGeomType(OGRwkbGeometryType eGeomType)
    {
        m_eGeomType = eGeomType;
    }

    void SetWidth(int nWidth)
    {
        m_nWidth = nWidth;
    }

    void SetNotNullable(bool bNotNullable)
    {
        m_bNotNullable = bNotNullable;
    }

    void SetArray(bool bArray)
    {
        m_bArray = bArray;
    }

    void SetList(bool bList)
    {
        m_bList = bList;
    }

    void SetXPath(const CPLString &osXPath)
    {
        m_osXPath = osXPath;
    }

    void AddAlternateXPath(const CPLString &osXPath)
    {
        m_aosXPath.push_back(osXPath);
    }

    void SetFixedValue(const CPLString &osFixedValue)
    {
        m_osFixedValue = osFixedValue;
    }

    void SetDefaultValue(const CPLString &osDefaultValue)
    {
        m_osDefaultValue = osDefaultValue;
    }

    void SetCategory(Category eCategory)
    {
        m_eCategory = eCategory;
    }

    void SetMinOccurs(int nMinOccurs)
    {
        m_nMinOccurs = nMinOccurs;
    }

    void SetMaxOccurs(int nMaxOccurs)
    {
        m_nMaxOccurs = nMaxOccurs;
    }

    void SetRepetitionOnSequence(bool b)
    {
        m_bRepetitionOnSequence = b;
    }

    void SetIncludeThisEltInBlob(bool b)
    {
        m_bIncludeThisEltInBlob = b;
    }

    void SetAbstractElementXPath(const CPLString &osName)
    {
        m_osAbstractElementXPath = osName;
    }

    void SetRelatedClassXPath(const CPLString &osName)
    {
        m_osRelatedClassXPath = osName;
    }

    void SetJunctionLayer(const CPLString &osName)
    {
        m_osJunctionLayer = osName;
    }

    void SetIgnored()
    {
        m_bIgnored = true;
    }

    void SetDocumentation(const CPLString &osDoc)
    {
        m_osDoc = osDoc;
    }

    void SetMayAppearOutOfOrder(bool b)
    {
        m_bMayAppearOutOfOrder = b;
    }

    static CPLString
    MakePKIDFieldXPathFromXLinkHrefXPath(const CPLString &osBaseXPath)
    {
        return "{" + osBaseXPath + "}_pkid";
    }

    static CPLString MakeXLinkRawContentFieldXPathFromXLinkHrefXPath(
        const CPLString &osBaseXPath)
    {
        return "{" + osBaseXPath + "}_rawcontent";
    }

    static CPLString
    MakeXLinkDerivedFieldXPathFromXLinkHrefXPath(const CPLString &osBaseXPath,
                                                 const CPLString &osName)
    {
        return "{" + osBaseXPath + "}_derived_" + osName;
    }

    const CPLString &GetName() const
    {
        return m_osName;
    }

    const CPLString &GetXPath() const
    {
        return m_osXPath;
    }

    const std::vector<CPLString> &GetAlternateXPaths() const
    {
        return m_aosXPath;
    }

    GMLASFieldType GetType() const
    {
        return m_eType;
    }

    OGRwkbGeometryType GetGeomType() const
    {
        return m_eGeomType;
    }

    const CPLString &GetTypeName() const
    {
        return m_osTypeName;
    }

    int GetWidth() const
    {
        return m_nWidth;
    }

    bool IsNotNullable() const
    {
        return m_bNotNullable;
    }

    bool IsArray() const
    {
        return m_bArray;
    }

    bool IsList() const
    {
        return m_bList;
    }

    const CPLString &GetFixedValue() const
    {
        return m_osFixedValue;
    }

    const CPLString &GetDefaultValue() const
    {
        return m_osDefaultValue;
    }

    Category GetCategory() const
    {
        return m_eCategory;
    }

    int GetMinOccurs() const
    {
        return m_nMinOccurs;
    }

    int GetMaxOccurs() const
    {
        return m_nMaxOccurs;
    }

    bool GetRepetitionOnSequence() const
    {
        return m_bRepetitionOnSequence;
    }

    bool GetIncludeThisEltInBlob() const
    {
        return m_bIncludeThisEltInBlob;
    }

    const CPLString &GetAbstractElementXPath() const
    {
        return m_osAbstractElementXPath;
    }

    const CPLString &GetJunctionLayer() const
    {
        return m_osJunctionLayer;
    }

    const CPLString &GetRelatedClassXPath() const
    {
        return m_osRelatedClassXPath;
    }

    bool IsIgnored() const
    {
        return m_bIgnored;
    }

    const CPLString &GetDocumentation() const
    {
        return m_osDoc;
    }

    bool MayAppearOutOfOrder() const
    {
        return m_bMayAppearOutOfOrder;
    }

    static GMLASFieldType GetTypeFromString(const CPLString &osType);
};

/************************************************************************/
/*                            GMLASFeatureClass                         */
/************************************************************************/

class GMLASFeatureClass
{
    /** User facing name */
    CPLString m_osName{};

    /** XPath to the main element of the feature class */
    CPLString m_osXPath{};

    /** List of fields */
    std::vector<GMLASField> m_aoFields{};

    /** Child nested classes */
    std::vector<GMLASFeatureClass> m_aoNestedClasses{};

    /** Whether this layer corresponds to a (multiple instantiated) xs:group
        or a repeated sequence */
    bool m_bIsRepeatedSequence = false;

    /** Whether this is a repeated group. Should be set together with
     * m_bIsRepeatedSequence */
    bool m_bIsGroup = false;

    /** Only used for junction tables. The XPath to the parent table */
    CPLString m_osParentXPath{};

    /** Only used for junction tables. The XPath to the child table */
    CPLString m_osChildXPath{};

    /** Whether this corresponds to a top-level XSD element in the schema */
    bool m_bIsTopLevelElt = false;

    /** Documentation from schema */
    CPLString m_osDoc{};

  public:
    GMLASFeatureClass() = default;

    void SetName(const CPLString &osName);
    void SetXPath(const CPLString &osXPath);
    void AddField(const GMLASField &oField);
    void PrependFields(const std::vector<GMLASField> &aoFields);
    void AppendFields(const std::vector<GMLASField> &aoFields);
    void AddNestedClass(const GMLASFeatureClass &oNestedClass);

    void SetIsRepeatedSequence(bool bIsRepeatedSequence)
    {
        m_bIsRepeatedSequence = bIsRepeatedSequence;
    }

    void SetIsGroup(bool bIsGroup)
    {
        m_bIsGroup = bIsGroup;
    }

    void SetParentXPath(const CPLString &osXPath)
    {
        m_osParentXPath = osXPath;
    }

    void SetChildXPath(const CPLString &osXPath)
    {
        m_osChildXPath = osXPath;
    }

    void SetIsTopLevelElt(bool bIsTopLevelElt)
    {
        m_bIsTopLevelElt = bIsTopLevelElt;
    }

    void SetDocumentation(const CPLString &osDoc)
    {
        m_osDoc = osDoc;
    }

    const CPLString &GetName() const
    {
        return m_osName;
    }

    const CPLString &GetXPath() const
    {
        return m_osXPath;
    }

    const std::vector<GMLASField> &GetFields() const
    {
        return m_aoFields;
    }

    std::vector<GMLASField> &GetFields()
    {
        return m_aoFields;
    }

    const std::vector<GMLASFeatureClass> &GetNestedClasses() const
    {
        return m_aoNestedClasses;
    }

    std::vector<GMLASFeatureClass> &GetNestedClasses()
    {
        return m_aoNestedClasses;
    }

    bool IsRepeatedSequence() const
    {
        return m_bIsRepeatedSequence;
    }

    bool IsGroup() const
    {
        return m_bIsGroup;
    }

    const CPLString &GetParentXPath() const
    {
        return m_osParentXPath;
    }

    const CPLString &GetChildXPath() const
    {
        return m_osChildXPath;
    }

    bool IsTopLevelElt() const
    {
        return m_bIsTopLevelElt;
    }

    const CPLString &GetDocumentation() const
    {
        return m_osDoc;
    }
};

/************************************************************************/
/*                         GMLASSchemaAnalyzer                          */
/************************************************************************/

class GMLASSchemaAnalyzer
{
    GMLASXPathMatcher &m_oIgnoredXPathMatcher;

    GMLASXPathMatcher &m_oChildrenElementsConstraintsXPathMatcher;

    GMLASXPathMatcher &m_oForcedFlattenedXPathMatcher;

    GMLASXPathMatcher &m_oDisabledFlattenedXPathMatcher;

    std::map<CPLString, std::vector<CPLString>>
        m_oMapChildrenElementsConstraints{};

    /** Whether repeated strings, integers, reals should be in corresponding
        OGR array types. */
    bool m_bUseArrays = true;

    /** Whether OGR field null state should be used. */
    bool m_bUseNullState = false;

    /** Whether, when dealing with schemas that import the
        GML namespace, and that at least one of them has
        elements that derive from gml:_Feature or
        gml:AbstractFeatureonly, only such elements should be
        instantiated as OGR layers, during the first pass that
        iterates over top level elements of the imported
        schemas. */
    bool m_bInstantiateGMLFeaturesOnly = true;

    /** Vector of feature classes */
    std::vector<GMLASFeatureClass> m_aoClasses{};

    /** Map from a namespace URI to the corresponding prefix */
    std::map<CPLString, CPLString> m_oMapURIToPrefix{};

    /** Map element XPath to its XSElementDeclaration* */
    std::map<CPLString, XSElementDeclaration *> m_oMapXPathToEltDecl{};

    typedef std::map<XSElementDeclaration *,
                     std::vector<XSElementDeclaration *>>
        tMapParentEltToChildElt;
    /** Map from a base/parent element to a vector of derived/children
        elements that are substitutionGroup of it. The map only
        addresses the direct derived types, and not the 2nd level or more
        derived ones. For that recursion in the map must be used.*/
    tMapParentEltToChildElt m_oMapParentEltToChildElt{};

    /** Map from a XSModelGroup* object to the name of its group definition. */
    std::map<XSModelGroup *, XSModelGroupDefinition *> m_oMapModelGroupToMGD{};

    /** Map from (non namespace prefixed) element names to the number of
        elements that share the same namespace (in different namespaces) */
    std::map<CPLString, int> m_oMapEltNamesToInstanceCount{};

    /** Set of elements that match a OGR layer */
    std::set<XSElementDeclaration *> m_oSetEltsForTopClass{};

    /** Set of elements that are simple enough to be inlined whenever they
        are referenced with cardinality 1. The use case if base:identifier
        used by Inspire schemas. */
    std::set<XSElementDeclaration *> m_oSetSimpleEnoughElts{};

    /** Maximum length of layer and field identifiers*/
    int m_nIdentifierMaxLength = 0;

    /** Whether case insensitive comparison should be used for identifier
     * equality testing */
    bool m_bCaseInsensitiveIdentifier = CASE_INSENSITIVE_IDENTIFIER_DEFAULT;

    /** Whether to launder identifiers like postgresql does */
    bool m_bPGIdentifierLaundering = PG_IDENTIFIER_LAUNDERING_DEFAULT;

    /* Maximum number of fields in an element considered for flattening. */
    int m_nMaximumFieldsForFlattening = MAXIMUM_FIELDS_FLATTENING_DEFAULT;

    /** GML version found: 2.1.1, 3.1.1 or 3.2.1 or empty*/
    CPLString m_osGMLVersionFound{};

    /** Set of schemas opened */
    std::set<CPLString> m_oSetSchemaURLs{};

    /** Map from namespace URI to namespace prefix coming from the
     * examination of xmlns:foo=bar attributes of the top element of the
     * GML document */
    std::map<CPLString, CPLString> m_oMapDocNSURIToPrefix{};

    bool m_bAlwaysGenerateOGRId = ALWAYS_GENERATE_OGR_ID_DEFAULT;

    static bool IsSame(const XSModelGroup *poModelGroup1,
                       const XSModelGroup *poModelGroup2);
    XSModelGroupDefinition *
    GetGroupDefinition(const XSModelGroup *poModelGroup);
    bool SetFieldFromAttribute(GMLASField &oField, XSAttributeUse *poAttr,
                               const CPLString &osXPathPrefix,
                               const CPLString &osNamePrefix = CPLString());
    void GetConcreteImplementationTypes(
        XSElementDeclaration *poParentElt,
        std::vector<XSElementDeclaration *> &apoImplEltList);
    std::vector<XSElementDeclaration *>
    GetConstraintChildrenElements(const CPLString &osFullXPath);
    bool FindElementsWithMustBeToLevel(
        const CPLString &osParentXPath, XSModelGroup *poModelGroup,
        int nRecursionCounter,
        std::set<XSElementDeclaration *> &oSetVisitedEltDecl,
        std::set<XSModelGroup *> &oSetVisitedModelGroups,
        std::vector<XSElementDeclaration *> &oVectorEltsForTopClass,
        std::set<CPLString> &aoSetXPathEltsForTopClass, XSModel *poModel,
        bool &bSimpleEnoughOut, int &nCountSubEltsOut);
    static void BuildMapCountOccurrencesOfSameName(
        XSModelGroup *poModelGroup,
        std::map<CPLString, int> &oMapCountOccurrencesOfSameName);
    bool ExploreModelGroup(
        XSModelGroup *psMainModelGroup, XSAttributeUseList *poMainAttrList,
        GMLASFeatureClass &oClass, int nRecursionCounter,
        std::set<XSModelGroup *> &oSetVisitedModelGroups, XSModel *poModel,
        const std::map<CPLString, int> &oMapCountOccurrencesOfSameName);
    void SetFieldTypeAndWidthFromDefinition(XSSimpleTypeDefinition *poST,
                                            GMLASField &oField);
    CPLString GetPrefix(const CPLString &osNamespaceURI);
    CPLString MakeXPath(const CPLString &osNamespace, const CPLString &osName);
    bool LaunderFieldNames(GMLASFeatureClass &oClass);
    void LaunderClassNames();

    XSElementDeclaration *
    GetTopElementDeclarationFromXPath(const CPLString &osXPath,
                                      XSModel *poModel);

    bool InstantiateClassFromEltDeclaration(XSElementDeclaration *poEltDecl,
                                            XSModel *poModel, bool &bError);
    void CreateNonNestedRelationship(
        XSElementDeclaration *poElt,
        std::vector<XSElementDeclaration *> &apoSubEltList,
        GMLASFeatureClass &oClass, int nMaxOccurs, bool bEltNameWillNeedPrefix,
        bool bForceJunctionTable, bool bCaseOfConstraintChildren);

    bool IsGMLNamespace(const CPLString &osURI);

    bool DerivesFromGMLFeature(XSElementDeclaration *poEltDecl);

    bool IsIgnoredXPath(const CPLString &osXPath);

    static void
    CollectClassesReferences(GMLASFeatureClass &oClass,
                             std::vector<GMLASFeatureClass *> &aoClasses);

    CPL_DISALLOW_COPY_ASSIGN(GMLASSchemaAnalyzer)

  public:
    GMLASSchemaAnalyzer(
        GMLASXPathMatcher &oIgnoredXPathMatcher,
        GMLASXPathMatcher &oChildrenElementsConstraintsXPathMatcher,
        const std::map<CPLString, std::vector<CPLString>>
            &oMapChildrenElementsConstraints,
        GMLASXPathMatcher &oForcedFlattenedXPathMatcher,
        GMLASXPathMatcher &oDisabledFlattenedXPathMatcher);

    void SetUseArrays(bool b)
    {
        m_bUseArrays = b;
    }

    void SetUseNullState(bool b)
    {
        m_bUseNullState = b;
    }

    void SetInstantiateGMLFeaturesOnly(bool b)
    {
        m_bInstantiateGMLFeaturesOnly = b;
    }

    void SetIdentifierMaxLength(int nLength)
    {
        m_nIdentifierMaxLength = nLength;
    }

    void SetCaseInsensitiveIdentifier(bool b)
    {
        m_bCaseInsensitiveIdentifier = b;
    }

    void SetPGIdentifierLaundering(bool b)
    {
        m_bPGIdentifierLaundering = b;
    }

    void SetMaximumFieldsForFlattening(int n)
    {
        m_nMaximumFieldsForFlattening = n;
    }

    void SetMapDocNSURIToPrefix(const std::map<CPLString, CPLString> &oMap)
    {
        m_oMapDocNSURIToPrefix = oMap;
    }

    void SetAlwaysGenerateOGRId(bool b)
    {
        m_bAlwaysGenerateOGRId = b;
    }

    bool Analyze(GMLASXSDCache &oCache, const CPLString &osBaseDirname,
                 std::vector<PairURIFilename> &aoXSDs, bool bSchemaFullChecking,
                 bool bHandleMultipleImports);

    const std::vector<GMLASFeatureClass> &GetClasses() const
    {
        return m_aoClasses;
    }

    const std::map<CPLString, CPLString> &GetMapURIToPrefix() const
    {
        return m_oMapURIToPrefix;
    }

    const CPLString &GetGMLVersionFound() const
    {
        return m_osGMLVersionFound;
    }

    const std::set<CPLString> &GetSchemaURLS() const
    {
        return m_oSetSchemaURLs;
    }

    static CPLString BuildJunctionTableXPath(const CPLString &osEltXPath,
                                             const CPLString &osSubEltXPath)
    {
        return osEltXPath + "|" + osSubEltXPath;
    }
};

/************************************************************************/
/*                           OGRGMLASDataSource                         */
/************************************************************************/

class OGRGMLASLayer;
class GMLASReader;

class OGRGMLASDataSource final : public GDALDataset
{
    struct XercesInitializer
    {
        XercesInitializer();
        ~XercesInitializer();
    };

    // MUST be first member, to get destroyed last after we have cleaned up
    // all other Xerces dependent objects.
    XercesInitializer m_oXercesInitializer{};

    std::vector<std::unique_ptr<OGRGMLASLayer>> m_apoLayers{};
    std::map<CPLString, CPLString> m_oMapURIToPrefix{};
    CPLString m_osGMLFilename{};
    std::unique_ptr<OGRLayer> m_poFieldsMetadataLayer{};
    std::unique_ptr<OGRLayer> m_poLayersMetadataLayer{};
    std::unique_ptr<OGRLayer> m_poRelationshipsLayer{};
    std::unique_ptr<OGRLayer> m_poOtherMetadataLayer{};
    std::vector<OGRLayer *> m_apoRequestedMetadataLayers{};
    std::shared_ptr<VSIVirtualHandle> m_fpGML{};
    std::shared_ptr<VSIVirtualHandle> m_fpGMLParser{};
    bool m_bLayerInitFinished = false;
    bool m_bSchemaFullChecking = false;
    bool m_bHandleMultipleImports = false;
    bool m_bValidate = false;
    bool m_bRemoveUnusedLayers = false;
    bool m_bRemoveUnusedFields = false;
    bool m_bFirstPassDone = false;
    /** Map from a SRS name to a boolean indicating if its coordinate
        order is inverted. */
    std::map<CPLString, bool> m_oMapSRSNameToInvertedAxis{};

    /** Map from geometry field definition to its expected SRSName */
    std::map<OGRGeomFieldDefn *, CPLString> m_oMapGeomFieldDefnToSRSName{};

    /* map the ID attribute to its belonging layer, e.g foo.1 -> layer Foo */
    std::map<CPLString, OGRGMLASLayer *> m_oMapElementIdToLayer{};

    /* map the ID attribute to the feature PKID (when different from itself) */
    std::map<CPLString, CPLString> m_oMapElementIdToPKID{};

    std::vector<PairURIFilename> m_aoXSDsManuallyPassed{};

    /** Default value for srsDimension attribute. */
    int m_nDefaultSrsDimension = 0;

    GMLASConfiguration m_oConf{};

    /** Schema cache */
    GMLASXSDCache m_oCache{};

    GMLASXPathMatcher m_oIgnoredXPathMatcher{};

    GMLASXPathMatcher m_oChildrenElementsConstraintsXPathMatcher{};

    GMLASXPathMatcher m_oForcedFlattenedXPathMatcher{};

    GMLASXPathMatcher m_oDisabledFlattenedXPathMatcher{};

    GMLASSwapCoordinatesEnum m_eSwapCoordinates = GMLAS_SWAP_AUTO;

    /** Base unique identifier */
    CPLString m_osHash{};

    vsi_l_offset m_nFileSize = 0;

    std::unique_ptr<GMLASReader> m_poReader{};

    bool m_bEndOfReaderLayers = false;

    int m_nCurMetadataLayerIdx = -1;

    GMLASXLinkResolver m_oXLinkResolver{};

    CPLString m_osGMLVersionFound{};

    bool m_bFoundSWE = false;

    // Pointers are also included in m_apoLayers
    std::vector<OGRGMLASLayer *> m_apoSWEDataArrayLayersRef{};

    // Path to gmlasconf.xml. It is a /vsimem temporary file if
    // m_bUnlinkConfigFileAfterUse is set.
    std::string m_osConfigFile{};

    // Whether m_osConfigFile should be removed at closing.
    bool m_bUnlinkConfigFileAfterUse = false;

    void TranslateClasses(OGRGMLASLayer *poParentLayer,
                          const GMLASFeatureClass &oFC);

    bool RunFirstPassIfNeeded(GMLASReader *poReader,
                              GDALProgressFunc pfnProgress,
                              void *pProgressData);

    void FillOtherMetadataLayer(GDALOpenInfo *poOpenInfo,
                                const CPLString &osConfigFile,
                                const std::vector<PairURIFilename> &aoXSDs,
                                const std::set<CPLString> &oSetSchemaURLs);

    static std::vector<PairURIFilename>
    BuildXSDVector(const CPLString &osXSDFilenames);

    void InitReaderWithFirstPassElements(GMLASReader *poReader);

  public:
    OGRGMLASDataSource();

    ~OGRGMLASDataSource();

    virtual int GetLayerCount() override;
    virtual OGRLayer *GetLayer(int) override;
    virtual OGRLayer *GetLayerByName(const char *pszName) override;

    virtual void ResetReading() override;
    virtual OGRFeature *GetNextFeature(OGRLayer **ppoBelongingLayer,
                                       double *pdfProgressPct,
                                       GDALProgressFunc pfnProgress,
                                       void *pProgressData) override;
    virtual int TestCapability(const char *) override;

    bool Open(GDALOpenInfo *poOpenInfo);

    std::vector<std::unique_ptr<OGRGMLASLayer>> &GetLayers()
    {
        return m_apoLayers;
    }

    const std::map<CPLString, CPLString> &GetMapURIToPrefix() const
    {
        return m_oMapURIToPrefix;
    }

    const CPLString &GetGMLFilename() const
    {
        return m_osGMLFilename;
    }

    const CPLString &GetGMLVersionFound() const
    {
        return m_osGMLVersionFound;
    }

    OGRLayer *GetFieldsMetadataLayer()
    {
        return m_poFieldsMetadataLayer.get();
    }

    OGRLayer *GetLayersMetadataLayer()
    {
        return m_poLayersMetadataLayer.get();
    }

    OGRLayer *GetRelationshipsLayer()
    {
        return m_poRelationshipsLayer.get();
    }

    OGRGMLASLayer *GetLayerByXPath(const CPLString &osXPath);

    GMLASReader *CreateReader(std::shared_ptr<VSIVirtualHandle> &fpGML,
                              GDALProgressFunc pfnProgress = nullptr,
                              void *pProgressData = nullptr);

    GMLASXSDCache &GetCache()
    {
        return m_oCache;
    }

    void PushUnusedGMLFilePointer(std::shared_ptr<VSIVirtualHandle> &fpGML);
    std::shared_ptr<VSIVirtualHandle> PopUnusedGMLFilePointer();

    bool IsLayerInitFinished() const
    {
        return m_bLayerInitFinished;
    }

    GMLASSwapCoordinatesEnum GetSwapCoordinates() const
    {
        return m_eSwapCoordinates;
    }

    const std::map<CPLString, bool> &GetMapIgnoredXPathToWarn() const
    {
        return m_oConf.m_oMapIgnoredXPathToWarn;
    }

    const GMLASXPathMatcher &GetIgnoredXPathMatcher() const
    {
        return m_oIgnoredXPathMatcher;
    }

    const GMLASConfiguration &GetConf() const
    {
        return m_oConf;
    }

    const std::vector<PairURIFilename> &GetXSDsManuallyPassed() const
    {
        return m_aoXSDsManuallyPassed;
    }
};

/************************************************************************/
/*                             OGRGMLASLayer                            */
/************************************************************************/

class OGRGMLASLayer final : public OGRLayer
{
    friend class OGRGMLASDataSource;

    OGRGMLASDataSource *m_poDS = nullptr;
    GMLASFeatureClass m_oFC{};
    bool m_bLayerDefnFinalized = false;
    int m_nMaxFieldIndex = 0;
    OGRFeatureDefn *m_poFeatureDefn = nullptr;

    /** Map from XPath to corresponding field index in OGR layer
        definition */
    std::map<CPLString, int> m_oMapFieldXPathToOGRFieldIdx{};

    /** Map from XPath to corresponding geometry field index in OGR layer
        definition */
    std::map<CPLString, int> m_oMapFieldXPathToOGRGeomFieldIdx{};

    /** Map from a OGR field index to the corresponding field index in
        m_oFC.GetFields() */
    std::map<int, int> m_oMapOGRFieldIdxtoFCFieldIdx{};
    std::map<int, int> m_oMapOGRGeomFieldIdxtoFCFieldIdx{};

    /** Map from XPath to corresponding field index in m_oFC.GetFields() */
    std::map<CPLString, int> m_oMapFieldXPathToFCFieldIdx{};

    bool m_bEOF = false;
    std::unique_ptr<GMLASReader> m_poReader{};
    std::shared_ptr<VSIVirtualHandle> m_fpGML{};
    /** OGR field index of the ID field */
    int m_nIDFieldIdx = -1;
    /** Whether the ID field is generated, or comes from the XML content */
    bool m_bIDFieldIsGenerated = false;
    /** Pointer to parent layer */
    OGRGMLASLayer *m_poParentLayer = nullptr;
    /** OGR field index of the field that points to the parent ID */
    int m_nParentIDFieldIdx = -1;

    std::map<CPLString, CPLString> m_oMapSWEFieldToOGRFieldName{};

    OGRFeature *GetNextRawFeature();

    bool InitReader();

    void SetLayerDefnFinalized(bool bVal)
    {
        m_bLayerDefnFinalized = bVal;
    }

    CPLString LaunderFieldName(const CPLString &osFieldName);

    CPLString GetXPathFromOGRFieldIndex(int nIdx) const;

    CPL_DISALLOW_COPY_ASSIGN(OGRGMLASLayer)

  public:
    OGRGMLASLayer(OGRGMLASDataSource *poDS, const GMLASFeatureClass &oFC,
                  OGRGMLASLayer *poParentLayer, bool bAlwaysGenerateOGRPKId);
    explicit OGRGMLASLayer(const char *pszLayerName);
    virtual ~OGRGMLASLayer();

    virtual const char *GetName() override
    {
        return GetDescription();
    }

    virtual OGRFeatureDefn *GetLayerDefn() override;
    virtual void ResetReading() override;
    virtual OGRFeature *GetNextFeature() override;

    virtual int TestCapability(const char *) override
    {
        return FALSE;
    }

    void SetDataSource(OGRGMLASDataSource *poDS)
    {
        m_poDS = poDS;
    }

    void PostInit(bool bIncludeGeometryXML);
    void
    ProcessDataRecordCreateFields(CPLXMLNode *psDataRecord,
                                  const std::vector<OGRFeature *> &apoFeatures,
                                  OGRLayer *poFieldsMetadataLayer);
    void ProcessDataRecordFillFeature(CPLXMLNode *psDataRecord,
                                      OGRFeature *poFeature);
    void
    ProcessDataRecordOfDataArrayCreateFields(OGRGMLASLayer *poParentLayer,
                                             CPLXMLNode *psDataRecord,
                                             OGRLayer *poFieldsMetadataLayer);
    void CreateCompoundFoldedMappings();

    const GMLASFeatureClass &GetFeatureClass() const
    {
        return m_oFC;
    }

    int GetOGRFieldIndexFromXPath(const CPLString &osXPath) const;
    int GetOGRGeomFieldIndexFromXPath(const CPLString &osXPath) const;

    int GetIDFieldIdx() const
    {
        return m_nIDFieldIdx;
    }

    bool IsGeneratedIDField() const
    {
        return m_bIDFieldIsGenerated;
    }

    OGRGMLASLayer *GetParent()
    {
        return m_poParentLayer;
    }

    int GetParentIDFieldIdx() const
    {
        return m_nParentIDFieldIdx;
    }

    int GetFCFieldIndexFromOGRFieldIdx(int iOGRFieldIdx) const;
    int GetFCFieldIndexFromOGRGeomFieldIdx(int iOGRGeomFieldIdx) const;
    int GetFCFieldIndexFromXPath(const CPLString &osXPath) const;

    bool EvaluateFilter(OGRFeature *poFeature);

    bool RemoveField(int nIdx);
    void InsertNewField(int nInsertPos, const OGRFieldDefn &oFieldDefn,
                        const CPLString &osXPath);

    CPLString
    GetXPathOfFieldLinkForAttrToOtherLayer(const CPLString &osFieldName,
                                           const CPLString &osTargetLayerXPath);
    CPLString
    CreateLinkForAttrToOtherLayer(const CPLString &osFieldName,
                                  const CPLString &osTargetLayerXPath);

    const std::map<CPLString, int> &GetMapFieldXPathToOGRFieldIdx() const
    {
        return m_oMapFieldXPathToOGRFieldIdx;
    }
};

/************************************************************************/
/*                              GMLASReader                             */
/************************************************************************/

class GMLASReader final : public DefaultHandler
{
    /** Schema cache */
    GMLASXSDCache &m_oCache;

    /** Object to tell if a XPath must be ignored */
    const GMLASXPathMatcher &m_oIgnoredXPathMatcher;

    /** XLink resolver */
    GMLASXLinkResolver &m_oXLinkResolver;

    /** Whether we should stop parsing */
    bool m_bParsingError = false;

    /** Xerces reader object */
    std::unique_ptr<SAX2XMLReader> m_poSAXReader{};

    /** Token for Xerces */
    XMLPScanToken m_oToFill{};

    /** File descriptor */
    std::shared_ptr<VSIVirtualHandle> m_fp{};

    /** Input source */
    std::unique_ptr<GMLASInputSource> m_GMLInputSource{};

    /** Whether we are at the first iteration */
    bool m_bFirstIteration = true;

    /** Whether we have reached end of file (or an error) */
    bool m_bEOF = false;

    /** Whether GetNextFeature() has been user interrupted (progress cbk) */
    bool m_bInterrupted = false;

    /** Error handler (for Xerces reader) */
    GMLASErrorHandler m_oErrorHandler{};

    /** Map URI namespaces to their prefix */
    std::map<CPLString, CPLString> m_oMapURIToPrefix{};

    /** List of OGR layers */
    std::vector<std::unique_ptr<OGRGMLASLayer>> *m_apoLayers = nullptr;

    /** Vector of features ready for consumption */
    std::list<std::pair<std::unique_ptr<OGRFeature>, OGRGMLASLayer *>>
        m_aoFeaturesReady{};

    /** OGR field index of the current field */
    int m_nCurFieldIdx = -1;

    /** OGR geometry field index of the current field */
    int m_nCurGeomFieldIdx = -1;

    /** XML nested level of current field */
    int m_nCurFieldLevel = 0;

    /** Whether we should store all content of the current field as XML */
    bool m_bIsXMLBlob = false;
    bool m_bIsXMLBlobIncludeUpper = false;

    /** Content of the current field */
    CPLString m_osTextContent{};

    /** For list field types, list of content */
    CPLStringList m_osTextContentList{};
    /** Estimated memory footprint of m_osTextContentList */
    size_t m_nTextContentListEstimatedSize = 0;

    /** Which layer is of interest for the reader, or NULL for all */
    OGRGMLASLayer *m_poLayerOfInterest = nullptr;

    /** Stack of length of split XPath components */
    std::vector<size_t> m_anStackXPathLength{};

    /** Current absolute XPath */
    CPLString m_osCurXPath{};

    /** Current XPath, relative to top-level feature */
    CPLString m_osCurSubXPath{};

    /** Current XML nesting level */
    int m_nLevel = 0;

    /** Whether we are in a gml:boundedBy element at level 1 */
    bool m_bInGMLBoundedByLevel1 = false;

    /** Default value for srsDimension attribute. */
    int m_nDefaultSrsDimension = 0;

    /** Map layer to global FID */
    std::map<OGRLayer *, int> m_oMapGlobalCounter{};

    /** Parsing context */
    struct Context
    {
        /** XML nesting level */
        int m_nLevel = 0;

        /** Current feature */
        OGRFeature *m_poFeature = nullptr;

        /** Layer of m_poFeature */
        OGRGMLASLayer *m_poLayer = nullptr;

        /** Current layer in a repeated group */
        OGRGMLASLayer *m_poGroupLayer = nullptr;

        /** Nesting level of m_poCurGroupLayer */
        int m_nGroupLayerLevel = -1;

        /** Index of the last processed OGR field in m_poCurGroupLayer */
        int m_nLastFieldIdxGroupLayer = -1;

        /** Map layer to local FID */
        std::map<OGRLayer *, int> m_oMapCounter{};

        /** Current XPath, relative to (current) top-level feature */
        CPLString m_osCurSubXPath{};

        void Dump() const;
    };

    /** Current context */
    Context m_oCurCtxt{};

    /** Stack of saved contexts */
    std::vector<Context> m_aoStackContext{};

    /** Context used in m_apsXMLNodeStack */
    struct NodeLastChild
    {
        /** Current node */
        CPLXMLNode *psNode = nullptr;

        /** Last child of psNode (for fast append operations) */
        CPLXMLNode *psLastChild = nullptr;
    };

    /** Stack of contexts to build XML tree of GML Geometry */
    std::vector<NodeLastChild> m_apsXMLNodeStack{};

    /** Counter used to prevent XML billion laugh attacks */
    int m_nEntityCounter = 0;

    /** Maximum allowed number of XML nesting level */
    int m_nMaxLevel = 100;

    /** Maximum allowed size of XML content in byte */
    size_t m_nMaxContentSize = 512000000;

    /** Map from a SRS name to a boolean indicating if its coordinate
        order is inverted. */
    std::map<CPLString, bool> m_oMapSRSNameToInvertedAxis{};

    /** Set of geometry fields with unknown SRS */
    std::set<OGRGeomFieldDefn *> m_oSetGeomFieldsWithUnknownSRS{};

    /** Map from geometry field definition to its expected SRSName.
        This is used to know if reprojection must be done */
    std::map<OGRGeomFieldDefn *, CPLString> m_oMapGeomFieldDefnToSRSName{};

    /** Whether this parsing involves schema validation */
    bool m_bValidate = false;

    /** Entity resolver used during schema validation */
    std::unique_ptr<GMLASBaseEntityResolver> m_poEntityResolver{};

    /** First level from which warnings about ignored XPath should be
        silent. */
    int m_nLevelSilentIgnoredXPath = -1;

    /** Whether a warning should be emitted when an element or attribute is
        found in the document parsed, but ignored because of the ignored
        XPath defined.  */
    std::map<CPLString, bool> m_oMapIgnoredXPathToWarn{};

    /** Policy to decide when to invert coordinates */
    GMLASSwapCoordinatesEnum m_eSwapCoordinates = GMLAS_SWAP_AUTO;

    /** Initial pass to guess SRS, etc... */
    bool m_bInitialPass = false;

    /** Whether to process swe:DataArray in a special way */
    bool m_bProcessSWEDataArray = false;

    /** Whether to process swe:DataArray in a special way */
    bool m_bProcessSWEDataRecord = false;

    /** Depth level of the swe:DataArray element */
    int m_nSWEDataArrayLevel = -1;

    /** Field name to which the DataArray belongs to */
    CPLString m_osSWEDataArrayParentField{};

    /** Depth level of the swe:DataRecord element */
    int m_nSWEDataRecordLevel = -1;

    OGRLayer *m_poFieldsMetadataLayer = nullptr;
    OGRLayer *m_poLayersMetadataLayer = nullptr;
    OGRLayer *m_poRelationshipsLayer = nullptr;

    /** Base unique identifier */
    CPLString m_osHash{};

    vsi_l_offset m_nFileSize = 0;

    bool m_bWarnUnexpected = false;

    /** Map from layer to a map of field XPath to a set of matching
        URL specific resolution rule index */
    std::map<OGRGMLASLayer *, std::map<CPLString, std::set<int>>>
        m_oMapXLinkFields{};

    /** Variables that could be local but more efficient to have same
        persistent, so as to save many memory allocations/deallocations */
    CPLString m_osLocalname{};
    CPLString m_osNSUri{};
    CPLString m_osNSPrefix{};
    CPLString m_osXPath{};
    CPLString m_osLayerXPath{};
    CPLString m_osAttrNSUri{};
    CPLString m_osAttrNSPrefix{};
    CPLString m_osAttrLocalName{};
    CPLString m_osAttrXPath{};
    CPLString m_osAttrValue{};
    CPLString m_osText{};

    std::vector<OGRGMLASLayer *> m_apoSWEDataArrayLayersRef{};
    std::vector<std::unique_ptr<OGRGMLASLayer>> m_apoSWEDataArrayLayersOwned{};

    int m_nSWEDataArrayLayerIdx = 0;

    /* Set of 3 maps used for xlink:href="#xxxx" internal links resolution */
    /* 1) map the ID attribute to its belonging layer, e.g foo.1 -> layer Foo */
    std::map<CPLString, OGRGMLASLayer *> m_oMapElementIdToLayer{};
    /* 2) map the ID attribute to the feature PKID (when different from itself)
     */
    std::map<CPLString, CPLString> m_oMapElementIdToPKID{};
    /* 3) map each (layer, field_xpath) to the list of ID it refers to */
    /*    e.g  (layer Bar, field_xpath) -> [foo.1, foo.2] */
    std::map<std::pair<OGRGMLASLayer *, CPLString>, std::vector<CPLString>>
        m_oMapFieldXPathToLinkValue{};

    /* Map layer's XPath to layer (for layers that are not group) */
    std::map<CPLString, OGRGMLASLayer *> m_oMapXPathToLayer{};

    /* Map OGR field XPath to layer (for layers that are group) */
    std::map<CPLString, OGRGMLASLayer *> m_oMapFieldXPathToGroupLayer{};

    /* Map layer's XPath to layer (for layers that are repeated sequences) */
    std::map<CPLString, std::vector<OGRGMLASLayer *>>
        m_oMapXPathToLayerRepeadedSequence{};

    void SetField(OGRFeature *poFeature, OGRGMLASLayer *poLayer, int nAttrIdx,
                  const CPLString &osAttrValue);

    void CreateNewFeature(const CPLString &osLocalname);

    void PushFeatureReady(std::unique_ptr<OGRFeature> &&,
                          OGRGMLASLayer *poLayer);

    void PushContext(const Context &oContext);
    void PopContext();

    void BuildXMLBlobStartElement(const CPLString &osXPath,
                                  const Attributes &attrs);

    OGRGMLASLayer *GetLayerByXPath(const CPLString &osXPath);

    void AttachAsLastChild(CPLXMLNode *psNode);

    void ProcessSWEDataArray(CPLXMLNode *psRoot);
    void ProcessSWEDataRecord(CPLXMLNode *psRoot);
    void ProcessGeometry(CPLXMLNode *psRoot);

    void ProcessAttributes(const Attributes &attrs);
    void ProcessXLinkHref(int nAttrIdx, const CPLString &osAttrXPath,
                          const CPLString &osAttrValue);
    void
    ExploreXMLDoc(const CPLString &osAttrXPath,
                  const GMLASXLinkResolutionConf::URLSpecificResolution &oRule,
                  CPLXMLNode *psNode, const CPLString &osParentXPath,
                  const GMLASXPathMatcher &oMatcher,
                  const std::map<CPLString, size_t> &oMapFieldXPathToIdx);

    void CreateFieldsForURLSpecificRules();
    void CreateFieldsForURLSpecificRule(
        OGRGMLASLayer *poLayer, int nFieldIdx, const CPLString &osFieldXPath,
        int &nInsertFieldIdx,
        const GMLASXLinkResolutionConf::URLSpecificResolution &oRule);

    bool FillTextContent() const
    {
        return !m_bInitialPass && m_nCurFieldIdx >= 0;
    }

    void ProcessInternalXLinkFirstPass(
        bool bRemoveUnusedFields,
        std::map<OGRGMLASLayer *, std::set<CPLString>> &oMapUnusedFields);

    CPL_DISALLOW_COPY_ASSIGN(GMLASReader)

  public:
    GMLASReader(GMLASXSDCache &oCache,
                const GMLASXPathMatcher &oIgnoredXPathMatcher,
                GMLASXLinkResolver &oXLinkResolver);
    ~GMLASReader();

    bool Init(const char *pszFilename,
              const std::shared_ptr<VSIVirtualHandle> &fp,
              const std::map<CPLString, CPLString> &oMapURIToPrefix,
              std::vector<std::unique_ptr<OGRGMLASLayer>> &apoLayers,
              bool bValidate, const std::vector<PairURIFilename> &aoXSDs,
              bool bSchemaFullChecking, bool bHandleMultipleImports);

    void SetLayerOfInterest(OGRGMLASLayer *poLayer);

    void SetMapIgnoredXPathToWarn(const std::map<CPLString, bool> &oMap)
    {
        m_oMapIgnoredXPathToWarn = oMap;
    }

    void SetSwapCoordinates(GMLASSwapCoordinatesEnum eVal)
    {
        m_eSwapCoordinates = eVal;
    }

    const std::shared_ptr<VSIVirtualHandle> &GetFP() const
    {
        return m_fp;
    }

    const std::map<CPLString, bool> &GetMapSRSNameToInvertedAxis() const
    {
        return m_oMapSRSNameToInvertedAxis;
    }

    void SetMapSRSNameToInvertedAxis(const std::map<CPLString, bool> &oMap)
    {
        m_oMapSRSNameToInvertedAxis = oMap;
    }

    const std::map<OGRGeomFieldDefn *, CPLString> &
    GetMapGeomFieldDefnToSRSName() const
    {
        return m_oMapGeomFieldDefnToSRSName;
    }

    void SetMapGeomFieldDefnToSRSName(
        const std::map<OGRGeomFieldDefn *, CPLString> &oMap)
    {
        m_oMapGeomFieldDefnToSRSName = oMap;
    }

    const std::map<CPLString, OGRGMLASLayer *> &GetMapElementIdToLayer() const
    {
        return m_oMapElementIdToLayer;
    }

    void
    SetMapElementIdToLayer(const std::map<CPLString, OGRGMLASLayer *> &oMap)
    {
        m_oMapElementIdToLayer = oMap;
    }

    const std::map<CPLString, CPLString> &GetMapElementIdToPKID() const
    {
        return m_oMapElementIdToPKID;
    }

    void SetMapElementIdToPKID(const std::map<CPLString, CPLString> &oMap)
    {
        m_oMapElementIdToPKID = oMap;
    }

    int GetDefaultSrsDimension() const
    {
        return m_nDefaultSrsDimension;
    }

    void SetDefaultSrsDimension(int nDim)
    {
        m_nDefaultSrsDimension = nDim;
    }

    void SetHash(const CPLString &osHash)
    {
        m_osHash = osHash;
    }

    void SetFileSize(vsi_l_offset nFileSize)
    {
        m_nFileSize = nFileSize;
    }

    OGRFeature *GetNextFeature(OGRGMLASLayer **ppoBelongingLayer = nullptr,
                               GDALProgressFunc pfnProgress = nullptr,
                               void *pProgressData = nullptr);

    virtual void startElement(const XMLCh *const uri,
                              const XMLCh *const localname,
                              const XMLCh *const qname,
                              const Attributes &attrs) override;
    virtual void endElement(const XMLCh *const uri,
                            const XMLCh *const localname,
                            const XMLCh *const qname) override;

    virtual void characters(const XMLCh *const chars,
                            const XMLSize_t length) override;

    void startEntity(const XMLCh *const name) override;

    bool RunFirstPass(GDALProgressFunc pfnProgress, void *pProgressData,
                      bool bRemoveUnusedLayers, bool bRemoveUnusedFields,
                      bool bProcessSWEDataArray,
                      OGRLayer *poFieldsMetadataLayer,
                      OGRLayer *poLayersMetadataLayer,
                      OGRLayer *poRelationshipsLayer,
                      std::set<CPLString> &aoSetRemovedLayerNames);

    static bool LoadXSDInParser(SAX2XMLReader *poParser, GMLASXSDCache &oCache,
                                GMLASBaseEntityResolver &oXSDEntityResolver,
                                const CPLString &osBaseDirname,
                                const CPLString &osXSDFilename,
                                Grammar **ppoGrammar, bool bSchemaFullChecking,
                                bool bHandleMultipleImports);

    void SetSWEDataArrayLayersRef(const std::vector<OGRGMLASLayer *> &ar);

    void SetProcessDataRecord(bool b)
    {
        m_bProcessSWEDataRecord = b;
    }

    std::vector<std::unique_ptr<OGRGMLASLayer>> StealSWEDataArrayLayersOwned()
    {
        return std::move(m_apoSWEDataArrayLayersOwned);
    }
};

CPLString OGRGMLASTruncateIdentifier(const CPLString &osName,
                                     int nIdentMaxLength);

CPLString OGRGMLASAddSerialNumber(const CPLString &osNameIn, int iOccurrence,
                                  size_t nOccurrences, int nIdentMaxLength);

#endif  // OGR_GMLAS_INCLUDED
