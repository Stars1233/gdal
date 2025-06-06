/******************************************************************************
 *
 * Project:  OpenGIS Simple Features Reference Implementation
 * Purpose:  Implements reading of FileGDB tables
 * Author:   Even Rouault, <even dot rouault at spatialys.com>
 *
 ******************************************************************************
 * Copyright (c) 2014, Even Rouault <even dot rouault at spatialys.com>
 *
 * SPDX-License-Identifier: MIT
 ****************************************************************************/

#ifndef FILEGDBTABLE_H_INCLUDED
#define FILEGDBTABLE_H_INCLUDED

#include "ogr_core.h"
#include "cpl_progress.h"
#include "cpl_vsi.h"
#include "ogr_geometry.h"

#include <limits>
#include <string>
#include <vector>

namespace OpenFileGDB
{
constexpr uint64_t OFFSET_MINUS_ONE = static_cast<uint64_t>(-1);
constexpr int MAX_CAR_COUNT_INDEXED_STR = 80;

/************************************************************************/
/*                        FileGDBTableGeometryType                      */
/************************************************************************/

/* FGTGT = (F)ile(G)DB(T)able(G)eometry(T)ype */
typedef enum
{
    FGTGT_NONE = 0,
    FGTGT_POINT = 1,
    FGTGT_MULTIPOINT = 2,
    FGTGT_LINE = 3,
    FGTGT_POLYGON = 4,
    FGTGT_MULTIPATCH = 9
} FileGDBTableGeometryType;

/************************************************************************/
/*                          FileGDBFieldType                            */
/************************************************************************/

/* FGFT = (F)ile(G)DB(F)ield(T)ype */
typedef enum
{
    FGFT_UNDEFINED = -1,
    FGFT_INT16 = 0,
    FGFT_INT32 = 1,
    FGFT_FLOAT32 = 2,
    FGFT_FLOAT64 = 3,
    FGFT_STRING = 4,
    FGFT_DATETIME = 5,
    FGFT_OBJECTID = 6,
    FGFT_GEOMETRY = 7,
    FGFT_BINARY = 8,
    FGFT_RASTER = 9,
    FGFT_GUID = 10,
    FGFT_GLOBALID = 11,
    FGFT_XML = 12,
    FGFT_INT64 = 13,                 // added in ArcGIS Pro 3.2
    FGFT_DATE = 14,                  // added in ArcGIS Pro 3.2
    FGFT_TIME = 15,                  // added in ArcGIS Pro 3.2
    FGFT_DATETIME_WITH_OFFSET = 16,  // added in ArcGIS Pro 3.2
} FileGDBFieldType;

/************************************************************************/
/*                          FileGDBField                                */
/************************************************************************/

class FileGDBTable;
class FileGDBIndex;

class FileGDBField
{
    friend class FileGDBTable;

    FileGDBTable *m_poParent = nullptr;

    std::string m_osName{};
    std::string m_osAlias{};
    FileGDBFieldType m_eType = FGFT_UNDEFINED;

    bool m_bNullable = false;  // Bit 1 of flag field
    bool m_bRequired =
        false;  // Bit 2 of flag field. Set for ObjectID, geometry field and Shape_Area/Shape_Length
    bool m_bEditable = false;       // Bit 3 of flag field.
    bool m_bHighPrecision = false;  // for FGFT_DATETIME
    bool m_bReadAsDouble =
        false;           // used by FileGDBTable::CreateAttributeIndex()
    int m_nMaxWidth = 0; /* for string */

    OGRField m_sDefault{};

    FileGDBIndex *m_poIndex = nullptr;

    FileGDBField(const FileGDBField &) = delete;
    FileGDBField &operator=(const FileGDBField &) = delete;

  public:
    static const OGRField UNSET_FIELD;
    static constexpr int BIT_NULLABLE = 0;
    static constexpr int BIT_REQUIRED = 1;
    static constexpr int BIT_EDITABLE = 2;
    static constexpr int MASK_NULLABLE = 1 << BIT_NULLABLE;
    static constexpr int MASK_REQUIRED = 1 << BIT_REQUIRED;
    static constexpr int MASK_EDITABLE = 1 << BIT_EDITABLE;

    explicit FileGDBField(FileGDBTable *m_poParent);
    FileGDBField(const std::string &osName, const std::string &osAlias,
                 FileGDBFieldType eType, bool bNullable, bool bRequired,
                 bool bEditable, int nMaxWidth, const OGRField &sDefault);
    virtual ~FileGDBField();

    void SetParent(FileGDBTable *poParent)
    {
        m_poParent = poParent;
    }

    const std::string &GetName() const
    {
        return m_osName;
    }

    const std::string &GetAlias() const
    {
        return m_osAlias;
    }

    FileGDBFieldType GetType() const
    {
        return m_eType;
    }

    bool IsNullable() const
    {
        return m_bNullable;
    }

    bool IsRequired() const
    {
        return m_bRequired;
    }

    bool IsEditable() const
    {
        return m_bEditable;
    }

    int GetMaxWidth() const
    {
        return m_nMaxWidth;
    }

    const OGRField *GetDefault() const
    {
        return &m_sDefault;
    }

    void SetHighPrecision()
    {
        m_bHighPrecision = true;
    }

    bool IsHighPrecision() const
    {
        return m_bHighPrecision;
    }

    int HasIndex();
    FileGDBIndex *GetIndex();
};

/************************************************************************/
/*                         FileGDBGeomField                             */
/************************************************************************/

class FileGDBGeomField : public FileGDBField
{
    friend class FileGDBTable;

    static const double ESRI_NAN;

    std::string m_osWKT{};
    int m_bHasZOriginScaleTolerance = 0;
    int m_bHasMOriginScaleTolerance = 0;
    double m_dfXOrigin = 0;
    double m_dfYOrigin = 0;
    double m_dfXYScale = 0;
    double m_dfMOrigin = 0;
    double m_dfMScale = 0;
    double m_dfZOrigin = 0;
    double m_dfZScale = 0;
    double m_dfXYTolerance = 0;
    double m_dfMTolerance = 0;
    double m_dfZTolerance = 0;
    double m_dfXMin = ESRI_NAN;
    double m_dfYMin = ESRI_NAN;
    double m_dfZMin = ESRI_NAN;
    double m_dfMMin = ESRI_NAN;
    double m_dfXMax = ESRI_NAN;
    double m_dfYMax = ESRI_NAN;
    double m_dfZMax = ESRI_NAN;
    double m_dfMMax = ESRI_NAN;
    std::vector<double> m_adfSpatialIndexGridResolution{};

  public:
    explicit FileGDBGeomField(FileGDBTable *m_poParent);
    FileGDBGeomField(const std::string &osName, const std::string &osAlias,
                     bool bNullable, const std::string &osWKT, double dfXOrigin,
                     double dfYOrigin, double dfXYScale, double dfXYTolerance,
                     const std::vector<double> &adfSpatialIndexGridResolution);

    virtual ~FileGDBGeomField();

    const std::string &GetWKT() const
    {
        return m_osWKT;
    }

    double GetXMin() const
    {
        return m_dfXMin;
    }

    double GetYMin() const
    {
        return m_dfYMin;
    }

    double GetZMin() const
    {
        return m_dfZMin;
    }  // only valid for m_bGeomTypeHasZ

    double GetMMin() const
    {
        return m_dfMMin;
    }  // only valid for m_bGeomTypeHasM

    double GetXMax() const
    {
        return m_dfXMax;
    }

    double GetYMax() const
    {
        return m_dfYMax;
    }

    double GetZMax() const
    {
        return m_dfZMax;
    }  // only valid for m_bGeomTypeHasZ

    double GetMMax() const
    {
        return m_dfMMax;
    }  // only valid for m_bGeomTypeHasM

    void SetXYMinMax(double dfXMin, double dfYMin, double dfXMax,
                     double dfYMax);
    void SetZMinMax(double dfZMin, double dfZMax);
    void SetMMinMax(double dfMMin, double dfMMax);

    int HasZOriginScaleTolerance() const
    {
        return m_bHasZOriginScaleTolerance;
    }

    int HasMOriginScaleTolerance() const
    {
        return m_bHasMOriginScaleTolerance;
    }

    double GetXOrigin() const
    {
        return m_dfXOrigin;
    }

    double GetYOrigin() const
    {
        return m_dfYOrigin;
    }

    double GetXYScale() const
    {
        return m_dfXYScale;
    }

    double GetXYTolerance() const
    {
        return m_dfXYTolerance;
    }

    double GetZOrigin() const
    {
        return m_dfZOrigin;
    }

    double GetZScale() const
    {
        return m_dfZScale;
    }

    double GetZTolerance() const
    {
        return m_dfZTolerance;
    }

    void SetZOriginScaleTolerance(double dfZOrigin, double dfZScale,
                                  double dfZTolerance);

    double GetMOrigin() const
    {
        return m_dfMOrigin;
    }

    double GetMScale() const
    {
        return m_dfMScale;
    }

    double GetMTolerance() const
    {
        return m_dfMTolerance;
    }

    void SetMOriginScaleTolerance(double dfMOrigin, double dfMScale,
                                  double dfMTolerance);

    const std::vector<double> &GetSpatialIndexGridResolution() const
    {
        return m_adfSpatialIndexGridResolution;
    }
};

/************************************************************************/
/*                         FileGDBRasterField                           */
/************************************************************************/

class FileGDBRasterField : public FileGDBGeomField
{
  public:
    enum class Type
    {
        EXTERNAL,
        MANAGED,
        INLINE,
    };

  private:
    friend class FileGDBTable;

    std::string m_osRasterColumnName{};

    Type m_eRasterType = Type::EXTERNAL;

    FileGDBRasterField(const FileGDBRasterField &) = delete;
    FileGDBRasterField &operator=(const FileGDBRasterField &) = delete;

  public:
    explicit FileGDBRasterField(FileGDBTable *poParentIn)
        : FileGDBGeomField(poParentIn)
    {
    }

    virtual ~FileGDBRasterField();

    const std::string &GetRasterColumnName() const
    {
        return m_osRasterColumnName;
    }

    Type GetRasterType() const
    {
        return m_eRasterType;
    }
};

/************************************************************************/
/*                           FileGDBIndex                               */
/************************************************************************/

class FileGDBIndex
{
    friend class FileGDBTable;
    std::string m_osIndexName{};
    std::string m_osExpression{};

  public:
    FileGDBIndex() = default;

    ~FileGDBIndex();

    const std::string &GetIndexName() const
    {
        return m_osIndexName;
    }

    const std::string &GetExpression() const
    {
        return m_osExpression;
    }

    std::string GetFieldName() const;
    int GetMaxWidthInBytes(const FileGDBTable *poTable) const;

    static std::string
    GetFieldNameFromExpression(const std::string &osExpression);
};

/************************************************************************/
/*                           FileGDBTable                               */
/************************************************************************/

class FileGDBTable
{
    VSILFILE *m_fpTable = nullptr;
    VSILFILE *m_fpTableX = nullptr;

    enum class GDBTableVersion
    {
        V3 = 3,  // 32-bit object id
        V4 = 4,  // 64-bit object id (ince ArcGIS Pro 3.2)
    };
    GDBTableVersion m_eGDBTableVersion = GDBTableVersion::V3;
    vsi_l_offset m_nFileSize = 0; /* only read when needed */
    bool m_bUpdate = false;
    bool m_bReliableObjectID = true;  // can be set to false on some V4 files

    //! This flag is set when we detect that a corruption of m_nHeaderBufferMaxSize
    // prior to fix needs to  fdf39012788b1110b3bf0ae6b8422a528f0ae8b6 to be
    // repaired
    bool m_bHasWarnedAboutHeaderRepair = false;

    std::string m_osFilename{};
    std::string m_osFilenameWithLayerName{};
    bool m_bIsV9 = false;
    std::vector<std::unique_ptr<FileGDBField>> m_apoFields{};
    int m_iObjectIdField = -1;

    int m_bHasReadGDBIndexes = FALSE;
    std::vector<std::unique_ptr<FileGDBIndex>> m_apoIndexes{};

    int m_nHasSpatialIndex = -1;

    bool m_bDirtyHeader = false;
    bool m_bDirtyFieldDescriptors = false;
    bool m_bDirtyIndices = false;
    bool m_bDirtyGdbIndexesFile = false;

    uint32_t m_nHeaderBufferMaxSize = 0;
    GUIntBig m_nOffsetFieldDesc = 0;
    GUInt32 m_nFieldDescLength = 0;
    bool m_bDirtyGeomFieldBBox = false;
    bool m_bDirtyGeomFieldSpatialIndexGridRes = false;
    uint32_t m_nGeomFieldBBoxSubOffset =
        0;  // offset of geometry field bounding box
            // relative to m_nOffsetFieldDesc
    uint32_t m_nGeomFieldSpatialIndexGridResSubOffset =
        0;  // offset of geometry field spatial index grid resolution
            // relative to m_nOffsetFieldDesc

    GUInt32 m_nTablxOffsetSize =
        0;  // 4 (4 GB limit), 5 (1 TB limit), 6 (256 TB limit)
    std::vector<vsi_l_offset>
        m_anFeatureOffsets{}; /* MSb set marks deleted feature. Only used when
                                 no .gdbtablx file */

    uint64_t m_nOffsetTableXTrailer = 0;
    uint64_t m_n1024BlocksPresent = 0;
    std::vector<GByte> m_abyTablXBlockMap{};
    int m_nCountBlocksBeforeIBlockIdx = 0;   /* optimization */
    int m_nCountBlocksBeforeIBlockValue = 0; /* optimization */
    bool m_bDirtyTableXHeader = false;
    bool m_bDirtyTableXTrailer = false;

    int m_nHasFreeList = -1;
    bool m_bFreelistCanBeDeleted = false;

    char m_achGUIDBuffer[32 + 6 + 1]{0};
    int m_nChSaved = -1;

    int m_bError = FALSE;
    int64_t m_nCurRow = -1;
    int m_bHasDeletedFeaturesListed = FALSE;
    bool m_bIsDeleted = false;
    int m_nLastCol = -1;
    GByte *m_pabyIterVals = nullptr;
    int m_iAccNullable = 0;
    GUInt32 m_nRowBlobLength = 0;
    OGRField m_sCurField{};

    FileGDBTableGeometryType m_eTableGeomType = FGTGT_NONE;
    bool m_bGeomTypeHasZ = false;
    bool m_bGeomTypeHasM = false;
    bool m_bStringsAreUTF8 = true;  // if false, UTF16
    std::string m_osTempString{};   // used as a temporary to store strings
                                    // recoded from UTF16 to UTF8
    int64_t m_nValidRecordCount = 0;
    int64_t m_nTotalRecordCount = 0;
    int m_iGeomField = -1;
    int m_nCountNullableFields = 0;
    unsigned m_nNullableFieldsSizeInBytes = 0;

    std::vector<double> m_adfSpatialIndexGridResolution{};

    GUInt32 m_nRowBufferMaxSize = 0;
    std::vector<GByte> m_abyBuffer{};
    std::vector<GByte> m_abyGeomBuffer{};
    std::vector<GByte> m_abyCurvePart{};
    std::vector<uint32_t> m_anNumberPointsPerPart{};
    std::vector<double> m_adfX{};
    std::vector<double> m_adfY{};
    std::vector<double> m_adfZ{};
    std::vector<double> m_adfM{};

    std::string m_osCacheRasterFieldPath{};

    GUIntBig m_nFilterXMin = 0, m_nFilterXMax = 0, m_nFilterYMin = 0,
             m_nFilterYMax = 0;

    class WholeFileRewriter
    {
        FileGDBTable &m_oTable;
        bool m_bModifyInPlace = false;
        std::string m_osGdbTablx{};
        std::string m_osBackupValidFilename{};
        std::string m_osBackupGdbTable{};
        std::string m_osBackupGdbTablx{};
        std::string m_osTmpGdbTable{};
        std::string m_osTmpGdbTablx{};
        bool m_bOldDirtyIndices = false;
        uint64_t m_nOldFileSize = 0;
        uint64_t m_nOldOffsetFieldDesc = 0;
        uint32_t m_nOldFieldDescLength = 0;
        bool m_bIsInit = false;

        WholeFileRewriter(const WholeFileRewriter &) = delete;
        WholeFileRewriter &operator=(const WholeFileRewriter &) = delete;

      public:
        VSILFILE *m_fpOldGdbtable = nullptr;
        VSILFILE *m_fpOldGdbtablx = nullptr;
        VSILFILE *m_fpTable = nullptr;
        VSILFILE *m_fpTableX = nullptr;

        explicit WholeFileRewriter(FileGDBTable &oTable) : m_oTable(oTable)
        {
        }

        ~WholeFileRewriter();

        bool Begin();
        bool Commit();
        void Rollback();
    };

    bool WriteHeader(VSILFILE *fpTable);
    bool WriteHeaderX(VSILFILE *fpTableX);

    bool ReadTableXHeaderV3();
    bool ReadTableXHeaderV4();
    int IsLikelyFeatureAtOffset(vsi_l_offset nOffset, GUInt32 *pnSize,
                                int *pbDeletedRecord);
    bool GuessFeatureLocations();
    bool WriteFieldDescriptors(VSILFILE *fpTable);
    bool SeekIntoTableXForNewFeature(int nObjectID);
    uint64_t ReadFeatureOffset(const GByte *pabyBuffer);
    void WriteFeatureOffset(uint64_t nFeatureOffset, GByte *pabyBuffer);
    bool WriteFeatureOffset(uint64_t nFeatureOffset);
    bool EncodeFeature(const std::vector<OGRField> &asRawFields,
                       const OGRGeometry *poGeom, int iSkipField);
    bool EncodeGeometry(const FileGDBGeomField *poGeomField,
                        const OGRGeometry *poGeom);
    bool RewriteTableToAddLastAddedField();
    void CreateGdbIndexesFile();
    void RemoveIndices();
    void RefreshIndices();
    bool CreateAttributeIndex(const FileGDBIndex *poIndex);
    uint64_t GetOffsetOfFreeAreaFromFreeList(uint32_t nSize);
    void AddEntryToFreelist(uint64_t nOffset, uint32_t nSize);

    FileGDBTable(const FileGDBTable &) = delete;
    FileGDBTable &operator=(const FileGDBTable &) = delete;

  public:
    FileGDBTable();
    ~FileGDBTable();

    bool Open(const char *pszFilename, bool bUpdate,
              const char *pszLayerName = nullptr);

    bool Create(const char *pszFilename, int nTablxOffsetSize,
                FileGDBTableGeometryType eTableGeomType, bool bGeomTypeHasZ,
                bool bGeomTypeHasM);
    bool SetTextUTF16();

    bool Sync(VSILFILE *fpTable = nullptr, VSILFILE *fpTableX = nullptr);
    bool Repack(GDALProgressFunc pfnProgress, void *pProgressData);
    void RecomputeExtent();

    //! Object should no longer be used after Close()
    void Close();

    bool IsFileGDBV9() const
    {
        return m_bIsV9;
    }

    const std::string &GetFilename() const
    {
        return m_osFilename;
    }

    FileGDBTableGeometryType GetGeometryType() const
    {
        return m_eTableGeomType;
    }

    bool GetGeomTypeHasZ() const
    {
        return m_bGeomTypeHasZ;
    }

    bool GetGeomTypeHasM() const
    {
        return m_bGeomTypeHasM;
    }

    int64_t GetValidRecordCount() const
    {
        return m_nValidRecordCount;
    }

    int64_t GetTotalRecordCount() const
    {
        return m_nTotalRecordCount;
    }

    int GetFieldCount() const
    {
        return static_cast<int>(m_apoFields.size());
    }

    FileGDBField *GetField(int i) const
    {
        return m_apoFields[i].get();
    }

    int GetGeomFieldIdx() const
    {
        return m_iGeomField;
    }

    const FileGDBGeomField *GetGeomField() const
    {
        return (m_iGeomField >= 0) ? cpl::down_cast<FileGDBGeomField *>(
                                         m_apoFields[m_iGeomField].get())
                                   : nullptr;
    }

    int GetObjectIdFieldIdx() const
    {
        return m_iObjectIdField;
    }

    int GetFieldIdx(const std::string &osName) const;

    int GetIndexCount();

    const FileGDBIndex *GetIndex(int i) const
    {
        return m_apoIndexes[i].get();
    }

    /** Return if we can use attribute or spatial indices.
     * This can be false for some sparse tables with 64-bit ObjectID since
     * the format of the sparse bitmap isn't fully understood yet.
     */
    bool CanUseIndices() const
    {
        return m_bReliableObjectID;
    }

    bool HasSpatialIndex();
    bool CreateIndex(const std::string &osIndexName,
                     const std::string &osExpression);
    void ComputeOptimalSpatialIndexGridResolution();
    bool CreateSpatialIndex();

    vsi_l_offset
    GetOffsetInTableForRow(int64_t iRow,
                           vsi_l_offset *pnOffsetInTableX = nullptr);

    int HasDeletedFeaturesListed() const
    {
        return m_bHasDeletedFeaturesListed;
    }

    /* Next call to SelectRow() or GetFieldValue() invalidates previously
     * returned values */
    bool SelectRow(int64_t iRow);
    int64_t GetAndSelectNextNonEmptyRow(int64_t iRow);

    int HasGotError() const
    {
        return m_bError;
    }

    int64_t GetCurRow() const
    {
        return m_nCurRow;
    }

    bool IsCurRowDeleted() const
    {
        return m_bIsDeleted;
    }

    const OGRField *GetFieldValue(int iCol);
    std::vector<OGRField> GetAllFieldValues();
    void FreeAllFieldValues(std::vector<OGRField> &asFields);

    int GetFeatureExtent(const OGRField *psGeomField,
                         OGREnvelope *psOutFeatureEnvelope);

    const std::vector<double> &GetSpatialIndexGridResolution() const
    {
        return m_adfSpatialIndexGridResolution;
    }

    void InstallFilterEnvelope(const OGREnvelope *psFilterEnvelope);
    int DoesGeometryIntersectsFilterEnvelope(const OGRField *psGeomField);

    void GetMinMaxProjYForSpatialIndex(double &dfYMin, double &dfYMax) const;

    bool CreateField(std::unique_ptr<FileGDBField> &&psField);
    bool DeleteField(int iField);
    bool AlterField(int iField, const std::string &osName,
                    const std::string &osAlias, FileGDBFieldType eType,
                    bool bNullable, int nMaxWidth, const OGRField &sDefault);
    bool AlterGeomField(const std::string &osName, const std::string &osAlias,
                        bool bNullable, const std::string &osWKT);

    bool CreateFeature(const std::vector<OGRField> &asRawFields,
                       const OGRGeometry *poGeom, int *pnFID = nullptr);
    bool UpdateFeature(int64_t nFID, const std::vector<OGRField> &asRawFields,
                       const OGRGeometry *poGeom);
    bool DeleteFeature(int64_t nFID);

    bool CheckFreeListConsistency();
    void DeleteFreeList();
};

/************************************************************************/
/*                           FileGDBSQLOp                               */
/************************************************************************/

typedef enum
{
    FGSO_ISNOTNULL,
    FGSO_LT,
    FGSO_LE,
    FGSO_EQ,
    FGSO_GE,
    FGSO_GT,
    FGSO_ILIKE
} FileGDBSQLOp;

/************************************************************************/
/*                           FileGDBIterator                            */
/************************************************************************/

class FileGDBIterator
{
  public:
    virtual ~FileGDBIterator()
    {
    }

    virtual FileGDBTable *GetTable() = 0;
    virtual void Reset() = 0;
    virtual int64_t GetNextRowSortedByFID() = 0;
    virtual int64_t GetRowCount();

    /* Only available on a BuildIsNotNull() iterator */
    virtual const OGRField *GetMinValue(int &eOutOGRFieldType);
    virtual const OGRField *GetMaxValue(int &eOutOGRFieldType);
    /* will reset the iterator */
    virtual bool GetMinMaxSumCount(double &dfMin, double &dfMax, double &dfSum,
                                   int &nCount);

    /* Only available on a BuildIsNotNull() or Build() iterator */
    virtual int64_t GetNextRowSortedByValue();

    static FileGDBIterator *Build(FileGDBTable *poParent, int nFieldIdx,
                                  int bAscending, FileGDBSQLOp op,
                                  OGRFieldType eOGRFieldType,
                                  const OGRField *psValue);
    static FileGDBIterator *BuildIsNotNull(FileGDBTable *poParent,
                                           int nFieldIdx, int bAscending);
    static FileGDBIterator *BuildNot(FileGDBIterator *poIterBase);
    static FileGDBIterator *BuildAnd(FileGDBIterator *poIter1,
                                     FileGDBIterator *poIter2,
                                     bool bTakeOwnershipOfIterators);
    static FileGDBIterator *BuildOr(FileGDBIterator *poIter1,
                                    FileGDBIterator *poIter2,
                                    int bIteratorAreExclusive = FALSE);
};

/************************************************************************/
/*                      FileGDBSpatialIndexIterator                     */
/************************************************************************/

class FileGDBSpatialIndexIterator : virtual public FileGDBIterator
{
  public:
    virtual bool SetEnvelope(const OGREnvelope &sFilterEnvelope) = 0;

    ~FileGDBSpatialIndexIterator() override;

    static FileGDBSpatialIndexIterator *
    Build(FileGDBTable *poParent, const OGREnvelope &sFilterEnvelope);
};

/************************************************************************/
/*                       FileGDBOGRGeometryConverter                    */
/************************************************************************/

class FileGDBOGRGeometryConverter
{
  public:
    virtual ~FileGDBOGRGeometryConverter();

    virtual OGRGeometry *GetAsGeometry(const OGRField *psField) = 0;

    static FileGDBOGRGeometryConverter *
    BuildConverter(const FileGDBGeomField *poGeomField);
    static OGRwkbGeometryType
    GetGeometryTypeFromESRI(const char *pszESRIGeometryType);
};

int FileGDBDoubleDateToOGRDate(double dfVal, bool bHighPrecision,
                               OGRField *psField);
int FileGDBDoubleTimeToOGRTime(double dfVal, OGRField *psField);
int FileGDBDateTimeWithOffsetToOGRDate(double dfVal, int16_t nUTCOffset,
                                       OGRField *psField);

} /* namespace OpenFileGDB */

#endif /* ndef FILEGDBTABLE_H_INCLUDED */
