/******************************************************************************
 *
 * Project:  MSSQL Spatial driver
 * Purpose:  Definition of classes for OGR MSSQL Spatial driver.
 * Author:   Tamas Szekeres, szekerest at gmail.com
 *
 ******************************************************************************
 * Copyright (c) 2010, Tamas Szekeres
 *
 * SPDX-License-Identifier: MIT
 ****************************************************************************/

#ifndef OGR_MSSQLSPATIAL_H_INCLUDED
#define OGR_MSSQLSPATIAL_H_INCLUDED

#include "ogrsf_frmts.h"
#include "cpl_odbc.h"
#include "cpl_error.h"

#ifdef SQLNCLI_VERSION
#include <sqlncli.h>
#endif
#ifdef MSODBCSQL_VERSION
#include "include_msodbcsql.h"
#endif

#include <map>

class OGRMSSQLSpatialDataSource;

/* layer status */
#define MSSQLLAYERSTATUS_ORIGINAL 0
#define MSSQLLAYERSTATUS_INITIAL 1
#define MSSQLLAYERSTATUS_CREATED 2
#define MSSQLLAYERSTATUS_DISABLED 3

/* geometry format to transfer geometry column */
#define MSSQLGEOMETRY_NATIVE 0
#define MSSQLGEOMETRY_WKB 1
#define MSSQLGEOMETRY_WKT 2
#define MSSQLGEOMETRY_WKBZM 3 /* SQL Server 2012 */

/* geometry column types */
#define MSSQLCOLTYPE_GEOMETRY 0
#define MSSQLCOLTYPE_GEOGRAPHY 1
#define MSSQLCOLTYPE_BINARY 2
#define MSSQLCOLTYPE_TEXT 3

/* sqlgeometry constants */

#define VA_KATMAI 0x01
#define VA_DENALI 0x02

#define SP_NONE 0
#define SP_HASZVALUES 1
#define SP_HASMVALUES 2
#define SP_ISVALID 4
#define SP_ISSINGLEPOINT 8
#define SP_ISSINGLELINESEGMENT 0x10
#define SP_ISLARGERTHANAHEMISPHERE 0x20

#define ST_UNKNOWN 0
#define ST_POINT 1
#define ST_LINESTRING 2
#define ST_POLYGON 3
#define ST_MULTIPOINT 4
#define ST_MULTILINESTRING 5
#define ST_MULTIPOLYGON 6
#define ST_GEOMETRYCOLLECTION 7
#define ST_CIRCULARSTRING 8
#define ST_COMPOUNDCURVE 9
#define ST_CURVEPOLYGON 10
#define ST_FULLGLOBE 11

#define FA_INTERIORRING 0x00
#define FA_STROKE 0x01
#define FA_EXTERIORRING 0x02

#define FA_NONE 0x00
#define FA_LINE 0x01
#define FA_ARC 0x02
#define FA_CURVE 0x03

#define SMT_LINE 0
#define SMT_ARC 1
#define SMT_FIRSTLINE 2
#define SMT_FIRSTARC 3

/************************************************************************/
/*                         OGRMSSQLAppendEscaped( )                     */
/************************************************************************/

void OGRMSSQLAppendEscaped(CPLODBCStatement *poStatement,
                           const char *pszStrValue);

/************************************************************************/
/*                           OGRMSSQLGeometryParser                     */
/************************************************************************/

class OGRMSSQLGeometryValidator
{
  protected:
    const OGRGeometry *const poOriginalGeometry;
    const int nGeomColumnType;
    const bool bIsValid;
    std::unique_ptr<OGRGeometry> poValidGeometry{};

  public:
    explicit OGRMSSQLGeometryValidator(const OGRGeometry *poGeom,
                                       int nGeomColumnType);

    bool IsValidLatLon(double longitude, double latitude);
    bool IsValidCircularZ(double z1, double z2);
    bool IsValidPolygonRingCount(const OGRCurve *poGeom);
    bool IsValidPolygonRingClosed(const OGRCurve *poGeom);
    bool IsValid(const OGRPoint *poGeom);
    bool IsValid(const OGRMultiPoint *poGeom);
    bool IsValid(const OGRCircularString *poGeom);
    bool IsValid(const OGRSimpleCurve *poGeom);
    bool IsValid(const OGRCompoundCurve *poGeom);
    bool IsValid(const OGRMultiLineString *poGeom);
    bool IsValid(const OGRCurvePolygon *poGeom);
    bool IsValid(const OGRMultiPolygon *poGeom);
    bool IsValid(const OGRGeometryCollection *poGeom);
    bool IsValid(const OGRGeometry *poGeom);
    void MakeValid(OGRPoint *poGeom);
    void MakeValid(OGRMultiPoint *poGeom);
    void MakeValid(OGRCircularString *poGeom);
    void MakeValid(OGRSimpleCurve *poGeom);
    void MakeValid(OGRCompoundCurve *poGeom);
    void MakeValid(OGRMultiLineString *poGeom);
    void MakeValid(OGRPolygon *poGeom);
    void MakeValid(OGRCurvePolygon *poGeom);
    void MakeValid(OGRMultiPolygon *poGeom);
    void MakeValid(OGRGeometryCollection *poGeom);
    void MakeValid(OGRGeometry *poGeom);
    bool ValidateGeometry(OGRGeometry *poGeom);

    const OGRGeometry *GetValidGeometryRef() const;

    bool IsValid() const
    {
        return bIsValid;
    }
};

/************************************************************************/
/*                           OGRMSSQLGeometryParser                     */
/************************************************************************/

class OGRMSSQLGeometryParser
{
  protected:
    const unsigned char *pszData;
    /* version information */
    char chVersion;
    /* serialization properties */
    char chProps;
    /* point array */
    int nPointSize;
    int nPointPos;
    int nNumPoints;
    /* figure array */
    int nFigurePos;
    int nNumFigures;
    /* shape array */
    int nShapePos;
    int nNumShapes;
    /* segmenttype array */
    int nSegmentPos;
    int nNumSegments;
    int iSegment;
    int nSRSId;
    /* geometry or geography */
    int nColType;

  protected:
    OGRPoint *ReadPoint(int iFigure);
    OGRMultiPoint *ReadMultiPoint(int iShape);
    OGRErr ReadSimpleCurve(OGRSimpleCurve *poCurve, int iPoint, int iNextPoint);
    OGRLineString *ReadLineString(int iFigure);
    OGRLinearRing *ReadLinearRing(int iFigure);
    OGRMultiLineString *ReadMultiLineString(int iShape);
    OGRPolygon *ReadPolygon(int iShape);
    OGRMultiPolygon *ReadMultiPolygon(int iShape);
    OGRGeometryCollection *ReadGeometryCollection(int iShape);
    OGRCircularString *ReadCircularString(int iFigure);
    OGRCompoundCurve *ReadCompoundCurve(int iFigure);
    void AddCurveSegment(OGRCompoundCurve *poCompoundCurve,
                         OGRSimpleCurve *poCurve, int iPoint, int iNextPoint);
    OGRCurvePolygon *ReadCurvePolygon(int iShape);

  public:
    explicit OGRMSSQLGeometryParser(int nGeomColumnType);
    OGRErr ParseSqlGeometry(const unsigned char *pszInput, int nLen,
                            OGRGeometry **poGeom);

    int GetSRSId()
    {
        return nSRSId;
    }
};

/************************************************************************/
/*                           OGRMSSQLGeometryWriter                     */
/************************************************************************/

class OGRMSSQLGeometryWriter
{
  protected:
    OGRGeometry *poGeom2;
    unsigned char *pszData;
    int nLen;
    /* version information */
    char chVersion;
    /* serialization properties */
    char chProps;
    /* point array */
    int nPointSize;
    int nPointPos;
    int nNumPoints;
    int iPoint;
    /* figure array */
    int nFigurePos;
    int nNumFigures;
    int iFigure;
    /* shape array */
    int nShapePos;
    int nNumShapes;
    int iShape;
    /* segmenttype array */
    int nSegmentPos;
    int nNumSegments;
    int iSegment;
    int nSRSId;
    /* geometry or geography */
    int nColType;

  protected:
    void WritePoint(OGRPoint *poGeom);
    void WritePoint(double x, double y);
    void WritePoint(double x, double y, double z);
    void WritePoint(double x, double y, double z, double m);
    void WriteSimpleCurve(OGRSimpleCurve *poGeom, bool bReversePoints);
    void WriteSimpleCurve(OGRSimpleCurve *poGeom, int iStartIndex,
                          bool bReversePoints);
    void WriteSimpleCurve(OGRSimpleCurve *poGeom, int iStartIndex, int nCount,
                          bool bReversePoints);
    void WriteCompoundCurve(OGRCompoundCurve *poGeom);
    void WriteCurve(OGRCurve *poGeom, bool bReversePoints);
    void WritePolygon(OGRPolygon *poGeom);
    void WriteCurvePolygon(OGRCurvePolygon *poGeom);
    void WriteGeometryCollection(OGRGeometryCollection *poGeom, int iParent);
    void WriteGeometry(OGRGeometry *poGeom, int iParent);
    void TrackGeometry(OGRGeometry *poGeom);

  public:
    OGRMSSQLGeometryWriter(OGRGeometry *poGeometry, int nGeomColumnType,
                           int nSRS);
    OGRErr WriteSqlGeometry(unsigned char *pszBuffer, int nBufLen);

    int GetDataLen()
    {
        return nLen;
    }
};

/************************************************************************/
/*                             OGRMSSQLSpatialLayer                     */
/************************************************************************/

class OGRMSSQLSpatialLayer CPL_NON_FINAL : public OGRLayer
{
  protected:
    OGRFeatureDefn *poFeatureDefn = nullptr;
    int nRawColumns = 0;

    CPLODBCStatement *poStmt = nullptr;
    bool m_bEOF = false;
    bool m_bResetNeeded = false;

    // Layer spatial reference system, and srid.
    OGRSpatialReference *poSRS = nullptr;
    int nSRSId = 0;

    GIntBig iNextShapeId = 0;

    OGRMSSQLSpatialDataSource *poDS = nullptr;

    int nGeomColumnType = -1;
    char *pszGeomColumn = nullptr;
    int nGeomColumnIndex = -1;
    char *pszFIDColumn = nullptr;
    int nFIDColumnIndex = -1;

    // UUID doesn't work for now in bulk copy mode
    bool m_bHasUUIDColumn = false;

    int bIsIdentityFid = FALSE;

    int nLayerStatus = MSSQLLAYERSTATUS_ORIGINAL;

    int *panFieldOrdinals = nullptr;

    void BuildFeatureDefn(const char *pszLayerName, CPLODBCStatement *poStmt);

    virtual CPLODBCStatement *GetStatement()
    {
        return poStmt;
    }

    void ClearStatement();
    OGRFeature *GetNextRawFeature();
    bool bLayerDefnNeedsRefresh = false;

  public:
    explicit OGRMSSQLSpatialLayer(OGRMSSQLSpatialDataSource *);
    virtual ~OGRMSSQLSpatialLayer();

    virtual void ResetReading() override;
    virtual OGRFeature *GetNextFeature() override;

    virtual OGRFeature *GetFeature(GIntBig nFeatureId) override;

    virtual OGRFeatureDefn *GetLayerDefn() override
    {
        return poFeatureDefn;
    }

    virtual OGRSpatialReference *GetSpatialRef() override;

    virtual OGRErr StartTransaction() override;
    virtual OGRErr CommitTransaction() override;
    virtual OGRErr RollbackTransaction() override;

    virtual const char *GetFIDColumn() override;
    virtual const char *GetGeometryColumn() override;

    virtual int TestCapability(const char *) override;
    static char *GByteArrayToHexString(const GByte *pabyData, int nLen);

    void SetLayerStatus(int nStatus)
    {
        nLayerStatus = nStatus;
    }

    int GetLayerStatus()
    {
        return nLayerStatus;
    }

    GDALDataset *GetDataset() override;
};

/************************************************************************/
/*                       OGRMSSQLSpatialTableLayer                      */
/************************************************************************/

typedef union
{
    struct
    {
        int iIndicator;
        int Value;
    } Integer;

    struct
    {
        int iIndicator;
        GIntBig Value;
    } Integer64;

    struct
    {
        int iIndicator;
        double Value;
    } Float;

    struct
    {
        SQLLEN nSize;
        char *pData[8000];
    } VarChar;

    struct
    {
        SQLLEN nSize;
        GByte *pData;
    } RawData;

} BCPData;

class OGRMSSQLSpatialTableLayer final : public OGRMSSQLSpatialLayer
{
    bool bUpdateAccess = true;
    bool bUseGeometryValidation = false;
    int bLaunderColumnNames = FALSE;
    int bPreservePrecision = FALSE;
    int bNeedSpatialIndex = FALSE;
    int bUseCopy = FALSE;
    int nBCPSize = 1000;

#ifdef SQL_SS_UDT
    int nUploadGeometryFormat = MSSQLGEOMETRY_NATIVE;
#else
    int nUploadGeometryFormat = MSSQLGEOMETRY_WKB;
#endif

    char *pszQuery = nullptr;

    SQLHANDLE hEnvBCP = nullptr;
#ifdef MSSQL_BCP_SUPPORTED
    SQLHANDLE hDBCBCP = nullptr;
    int nBCPCount = 0;
    BCPData **papstBindBuffer = nullptr;

    int bIdentityInsert = FALSE;
#endif

    CPLODBCStatement *BuildStatement(const char *pszColumns);

    CPLString BuildFields();

    virtual CPLODBCStatement *GetStatement() override;

    char *pszTableName = nullptr;
    char *pszLayerName = nullptr;
    char *pszSchemaName = nullptr;

    OGRwkbGeometryType eGeomType = wkbNone;

  public:
    explicit OGRMSSQLSpatialTableLayer(OGRMSSQLSpatialDataSource *);
    virtual ~OGRMSSQLSpatialTableLayer();

    CPLErr Initialize(const char *pszSchema, const char *pszTableName,
                      const char *pszGeomCol, int nCoordDimension, int nSRId,
                      const char *pszSRText, OGRwkbGeometryType eType);

    OGRErr CreateSpatialIndex();
    void DropSpatialIndex();

    virtual OGRErr IGetExtent(int iGeomField, OGREnvelope *psExtent,
                              bool bForce) override;

    virtual GIntBig GetFeatureCount(int) override;

    virtual OGRFeatureDefn *GetLayerDefn() override;

    virtual const char *GetName() override;

    virtual OGRErr SetAttributeFilter(const char *) override;
    virtual OGRFeature *GetNextFeature() override;

    virtual OGRErr ISetFeature(OGRFeature *poFeature) override;
    virtual OGRErr DeleteFeature(GIntBig nFID) override;
    virtual OGRErr ICreateFeature(OGRFeature *poFeature) override;

    const char *GetTableName()
    {
        return pszTableName;
    }

    const char *GetLayerName()
    {
        return pszLayerName;
    }

    const char *GetSchemaName()
    {
        return pszSchemaName;
    }

    virtual OGRErr CreateField(const OGRFieldDefn *poField,
                               int bApproxOK = TRUE) override;

    virtual OGRFeature *GetFeature(GIntBig nFeatureId) override;

    virtual int TestCapability(const char *) override;

    void SetLaunderFlag(int bFlag)
    {
        bLaunderColumnNames = bFlag;
    }

    void SetPrecisionFlag(int bFlag)
    {
        bPreservePrecision = bFlag;
    }

    void SetSpatialIndexFlag(int bFlag)
    {
        bNeedSpatialIndex = bFlag;
    }

    void SetUploadGeometryFormat(int nGeometryFormat)
    {
        nUploadGeometryFormat = nGeometryFormat;
    }

    void AppendFieldValue(CPLODBCStatement *poStatement, OGRFeature *poFeature,
                          int i, int *bind_num, void **bind_buffer);

    int FetchSRSId();

    void SetUseCopy(int bcpSize)
    {
        bUseCopy = TRUE;
        nBCPSize = bcpSize;
    }

    void SetUpdate(bool bFlag)
    {
        bUpdateAccess = bFlag;
    }

    static OGRErr StartCopy();

    // cppcheck-suppress functionStatic
    OGRErr EndCopy();

    int Failed(int nRetCode);
#ifdef MSSQL_BCP_SUPPORTED
    OGRErr CreateFeatureBCP(OGRFeature *poFeature);
    int Failed2(int nRetCode);
    int InitBCP(const char *pszDSN);
    void CloseBCP();
#endif
};

/************************************************************************/
/*                      OGRMSSQLSpatialSelectLayer                      */
/************************************************************************/

class OGRMSSQLSpatialSelectLayer final : public OGRMSSQLSpatialLayer
{
    char *pszBaseStatement;

    virtual CPLODBCStatement *GetStatement() override;

  public:
    OGRMSSQLSpatialSelectLayer(OGRMSSQLSpatialDataSource *, CPLODBCStatement *);
    virtual ~OGRMSSQLSpatialSelectLayer();

    virtual GIntBig GetFeatureCount(int) override;

    virtual OGRFeature *GetFeature(GIntBig nFeatureId) override;

    virtual int TestCapability(const char *) override;
};

/************************************************************************/
/*                           OGRODBCDataSource                          */
/************************************************************************/

class OGRMSSQLSpatialDataSource final : public GDALDataset
{
    typedef struct
    {
        int nMajor;
        int nMinor;
        int nBuild;
        int nRevision;
    } MSSQLVer;

    OGRMSSQLSpatialTableLayer **papoLayers;
    int nLayers;

    char *pszCatalog;

    bool bDSUpdate;
    CPLODBCSession oSession;

    int nGeometryFormat;

    int bUseGeometryColumns;
    bool bAlwaysOutputFid;

    int bListAllTables;

    int nBCPSize;
    int bUseCopy;

    // We maintain a list of known SRID to reduce the number of trips to
    // the database to get SRSes.
    std::map<int,
             std::unique_ptr<OGRSpatialReference, OGRSpatialReferenceReleaser>>
        m_oSRSCache{};

    OGRMSSQLSpatialTableLayer *poLayerInCopyMode;

    static void OGRMSSQLDecodeVersionString(MSSQLVer *psVersion,
                                            const char *pszVer);

    char *pszConnection;

  public:
    MSSQLVer sMSSQLVersion;

    OGRMSSQLSpatialDataSource();
    virtual ~OGRMSSQLSpatialDataSource();

    const char *GetCatalog()
    {
        return pszCatalog;
    }

    static int ParseValue(char **pszValue, char *pszSource, const char *pszKey,
                          int nStart, int nNext, int nTerm, int bRemove);

    int Open(const char *, bool bUpdate, int bTestOpen);
    int OpenTable(const char *pszSchemaName, const char *pszTableName,
                  const char *pszGeomCol, int nCoordDimension, int nSRID,
                  const char *pszSRText, OGRwkbGeometryType eType,
                  bool bUpdate);

    int GetLayerCount() override;
    OGRLayer *GetLayer(int) override;
    OGRLayer *GetLayerByName(const char *pszLayerName) override;

    int GetGeometryFormat()
    {
        return nGeometryFormat;
    }

    int UseGeometryColumns()
    {
        return bUseGeometryColumns;
    }

    bool AlwaysOutputFid()
    {
        return bAlwaysOutputFid;
    }

    virtual OGRErr DeleteLayer(int iLayer) override;
    OGRLayer *ICreateLayer(const char *pszName,
                           const OGRGeomFieldDefn *poGeomFieldDefn,
                           CSLConstList papszOptions) override;

    int TestCapability(const char *) override;

    virtual OGRLayer *ExecuteSQL(const char *pszSQLCommand,
                                 OGRGeometry *poSpatialFilter,
                                 const char *pszDialect) override;
    virtual void ReleaseResultSet(OGRLayer *poLayer) override;

    static char *LaunderName(const char *pszSrcName);
    OGRErr InitializeMetadataTables();

    OGRSpatialReference *AddSRIDToCache(
        int nId,
        std::unique_ptr<OGRSpatialReference, OGRSpatialReferenceReleaser>
            &&poSRS);

    OGRSpatialReference *FetchSRS(int nId);
    int FetchSRSId(const OGRSpatialReference *poSRS);

    OGRErr StartTransaction(CPL_UNUSED int bForce) override;
    OGRErr CommitTransaction() override;
    OGRErr RollbackTransaction() override;

    // Internal use
    CPLODBCSession *GetSession()
    {
        return &oSession;
    }

    const char *GetConnectionString()
    {
        return pszConnection;
    }

    void StartCopy(OGRMSSQLSpatialTableLayer *poMSSQLSpatialLayer);
    OGRErr EndCopy();
};

#endif /* ndef OGR_MSSQLSPATIAL_H_INCLUDED */
