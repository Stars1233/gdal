/******************************************************************************
 *
 * Project:  DXF Translator
 * Purpose:  Definition of classes for OGR .dxf driver.
 * Author:   Frank Warmerdam, warmerdam@pobox.com
 *
 ******************************************************************************
 * Copyright (c) 2009,  Frank Warmerdam
 * Copyright (c) 2010-2013, Even Rouault <even dot rouault at spatialys.com>
 * Copyright (c) 2017, Alan Thomas <alant@outlook.com.au>
 *
 * SPDX-License-Identifier: MIT
 ****************************************************************************/

#ifndef OGR_DXF_H_INCLUDED
#define OGR_DXF_H_INCLUDED

#include "ogrsf_frmts.h"
#include "ogr_autocad_services.h"
#include "cpl_conv.h"
#include <array>
#include <vector>
#include <map>
#include <optional>
#include <set>
#include <queue>
#include <memory>
#include <utility>

class OGRDXFDataSource;
class OGRDXFFeature;

constexpr std::array<char, 22> AUTOCAD_BINARY_DXF_SIGNATURE = {
    'A', 'u', 't', 'o', 'C', 'A', 'D', ' ',  'B',  'i',    'n',
    'a', 'r', 'y', ' ', 'D', 'X', 'F', '\r', '\n', '\x1A', '\x00'};

/************************************************************************/
/*                          DXFBlockDefinition                          */
/*                                                                      */
/*      Container for info about a block.                               */
/************************************************************************/

class DXFBlockDefinition
{
  public:
    DXFBlockDefinition()
    {
    }

    ~DXFBlockDefinition();

    std::vector<OGRDXFFeature *> apoFeatures;
};

/************************************************************************/
/*                         OGRDXFFeatureQueue                           */
/************************************************************************/

class OGRDXFFeatureQueue
{
    std::queue<OGRDXFFeature *> apoFeatures;

  public:
    OGRDXFFeatureQueue()
    {
    }

    void push(OGRDXFFeature *poFeature);

    OGRDXFFeature *front() const
    {
        return apoFeatures.front();
    }

    void pop();

    bool empty() const
    {
        return apoFeatures.empty();
    }

    size_t size() const
    {
        return apoFeatures.size();
    }
};

/************************************************************************/
/*                          OGRDXFBlocksLayer                           */
/************************************************************************/

class OGRDXFBlocksLayer final : public OGRLayer
{
    OGRDXFDataSource *poDS;

    OGRFeatureDefn *poFeatureDefn;

    GIntBig iNextFID;

    std::map<CPLString, DXFBlockDefinition>::iterator oIt;
    CPLString osBlockName;

    OGRDXFFeatureQueue apoPendingFeatures;

  public:
    explicit OGRDXFBlocksLayer(OGRDXFDataSource *poDS);
    ~OGRDXFBlocksLayer();

    void ResetReading() override;
    OGRFeature *GetNextFeature() override;

    OGRFeatureDefn *GetLayerDefn() override
    {
        return poFeatureDefn;
    }

    int TestCapability(const char *) override;

    OGRDXFFeature *GetNextUnfilteredFeature();
};

/************************************************************************/
/*                       OGRDXFInsertTransformer                        */
/*                                                                      */
/*      Stores the transformation needed to insert a block reference.   */
/************************************************************************/

class OGRDXFInsertTransformer final : public OGRCoordinateTransformation
{
  public:
    OGRDXFInsertTransformer() = default;

    double dfXOffset = 0.0;
    double dfYOffset = 0.0;
    double dfZOffset = 0.0;
    double dfXScale = 1.0;
    double dfYScale = 1.0;
    double dfZScale = 1.0;
    double dfAngle = 0.0;

    OGRDXFInsertTransformer GetOffsetTransformer()
    {
        OGRDXFInsertTransformer oResult;
        oResult.dfXOffset = this->dfXOffset;
        oResult.dfYOffset = this->dfYOffset;
        oResult.dfZOffset = this->dfZOffset;
        return oResult;
    }

    OGRDXFInsertTransformer GetRotateScaleTransformer()
    {
        OGRDXFInsertTransformer oResult;
        oResult.dfXScale = this->dfXScale;
        oResult.dfYScale = this->dfYScale;
        oResult.dfZScale = this->dfZScale;
        oResult.dfAngle = this->dfAngle;
        return oResult;
    }

    OGRCoordinateTransformation *Clone() const override;

    const OGRSpatialReference *GetSourceCS() const override
    {
        return nullptr;
    }

    const OGRSpatialReference *GetTargetCS() const override
    {
        return nullptr;
    }

    int Transform(size_t nCount, double *x, double *y, double *z,
                  double * /* t */, int *pabSuccess) override
    {
        for (size_t i = 0; i < nCount; i++)
        {
            x[i] *= dfXScale;
            y[i] *= dfYScale;
            if (z)
                z[i] *= dfZScale;

            const double dfXNew = x[i] * cos(dfAngle) - y[i] * sin(dfAngle);
            const double dfYNew = x[i] * sin(dfAngle) + y[i] * cos(dfAngle);

            x[i] = dfXNew;
            y[i] = dfYNew;

            x[i] += dfXOffset;
            y[i] += dfYOffset;
            if (z)
                z[i] += dfZOffset;

            if (pabSuccess)
                pabSuccess[i] = TRUE;
        }
        return TRUE;
    }

    OGRCoordinateTransformation *GetInverse() const override
    {
        return nullptr;
    }
};

/************************************************************************/
/*                         OGRDXFAffineTransform                        */
/*                                                                      */
/*    A simple 3D affine transform used to keep track of the            */
/*    transformation to be applied to an ASM entity.                    */
/************************************************************************/

class OGRDXFAffineTransform
{
  public:
    OGRDXFAffineTransform()
        : adfData{1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0}
    {
    }

    double adfData[12];  // Column-major: adfMatrix[5] is column 2, row 3
                         // Last 3 elements are translation

    // Left composition (composes oOther o this), modifying this
    void ComposeWith(const OGRDXFInsertTransformer &oCT)
    {
        double adfNew[12];

        adfNew[0] = oCT.dfXScale * cos(oCT.dfAngle) * adfData[0] -
                    oCT.dfYScale * sin(oCT.dfAngle) * adfData[1];
        adfNew[1] = oCT.dfXScale * sin(oCT.dfAngle) * adfData[0] +
                    oCT.dfYScale * cos(oCT.dfAngle) * adfData[1];
        adfNew[2] = oCT.dfZScale * adfData[2];

        adfNew[3] = oCT.dfXScale * cos(oCT.dfAngle) * adfData[3] -
                    oCT.dfYScale * sin(oCT.dfAngle) * adfData[4];
        adfNew[4] = oCT.dfXScale * sin(oCT.dfAngle) * adfData[3] +
                    oCT.dfYScale * cos(oCT.dfAngle) * adfData[4];
        adfNew[5] = oCT.dfZScale * adfData[5];

        adfNew[6] = oCT.dfXScale * cos(oCT.dfAngle) * adfData[6] -
                    oCT.dfYScale * sin(oCT.dfAngle) * adfData[7];
        adfNew[7] = oCT.dfXScale * sin(oCT.dfAngle) * adfData[6] +
                    oCT.dfYScale * cos(oCT.dfAngle) * adfData[7];
        adfNew[8] = oCT.dfZScale * adfData[8];

        adfNew[9] = oCT.dfXScale * cos(oCT.dfAngle) * adfData[9] -
                    oCT.dfYScale * sin(oCT.dfAngle) * adfData[10] +
                    oCT.dfXOffset;
        adfNew[10] = oCT.dfXScale * sin(oCT.dfAngle) * adfData[9] +
                     oCT.dfYScale * cos(oCT.dfAngle) * adfData[10] +
                     oCT.dfYOffset;
        adfNew[11] = oCT.dfZScale * adfData[11] + oCT.dfZOffset;

        memcpy(adfData, adfNew, sizeof(adfNew));
    }

    void SetField(OGRFeature *poFeature, const char *pszFieldName) const
    {
        poFeature->SetField(pszFieldName, 12, adfData);
    }
};

/************************************************************************/
/*                         OGRDXFOCSTransformer                         */
/************************************************************************/

class OGRDXFOCSTransformer final : public OGRCoordinateTransformation
{
  private:
    double adfN[3];
    double adfAX[3];
    double adfAY[3];

    double dfDeterminant;
    double aadfInverse[4][4];

  public:
    explicit OGRDXFOCSTransformer(double adfNIn[3], bool bInverse = false);

    const OGRSpatialReference *GetSourceCS() const override
    {
        return nullptr;
    }

    const OGRSpatialReference *GetTargetCS() const override
    {
        return nullptr;
    }

    int Transform(size_t nCount, double *adfX, double *adfY, double *adfZ,
                  double *adfT, int *pabSuccess) override;

    int InverseTransform(size_t nCount, double *adfX, double *adfY,
                         double *adfZ);

    void ComposeOnto(OGRDXFAffineTransform &poCT) const;

    OGRCoordinateTransformation *Clone() const override
    {
        return new OGRDXFOCSTransformer(*this);
    }

    OGRCoordinateTransformation *GetInverse() const override
    {
        return nullptr;
    }
};

/************************************************************************/
/*                              DXFTriple                               */
/*                                                                      */
/*     Represents a triple (X, Y, Z) used for various purposes in       */
/*     DXF files.  We do not use OGRPoint for this purpose, as the      */
/*     triple does not always represent a point as such (for            */
/*     example, it could contain a scale factor for each dimension).    */
/************************************************************************/
struct DXFTriple
{
    double dfX, dfY, dfZ;

    DXFTriple() : dfX(0.0), dfY(0.0), dfZ(0.0)
    {
    }

    DXFTriple(double x, double y, double z) : dfX(x), dfY(y), dfZ(z)
    {
    }

    void ToArray(double adfOut[3]) const
    {
        adfOut[0] = dfX;
        adfOut[1] = dfY;
        adfOut[2] = dfZ;
    }

    DXFTriple &operator*=(const double dfValue)
    {
        dfX *= dfValue;
        dfY *= dfValue;
        dfZ *= dfValue;
        return *this;
    }

    DXFTriple &operator/=(const double dfValue)
    {
        dfX /= dfValue;
        dfY /= dfValue;
        dfZ /= dfValue;
        return *this;
    }

    bool operator==(const DXFTriple &oOther) const
    {
        return dfX == oOther.dfX && dfY == oOther.dfY && dfZ == oOther.dfZ;
    }
};

/************************************************************************/
/*                            OGRDXFFeature                             */
/*                                                                      */
/*     Extends OGRFeature with some DXF-specific members.               */
/************************************************************************/
class OGRDXFFeature final : public OGRFeature
{
    friend class OGRDXFLayer;

  protected:
    // The feature's Object Coordinate System (OCS) unit normal vector
    DXFTriple oOCS;

    // A list of properties that are used to construct the style string
    std::map<CPLString, CPLString> oStyleProperties;

    // Additional data for INSERT entities
    bool bIsBlockReference;
    CPLString osBlockName;
    double dfBlockAngle;
    DXFTriple oBlockScale;

    // Used for INSERT entities when DXF_INLINE_BLOCKS is false, to store
    // the OCS insertion point
    DXFTriple oOriginalCoords;

    // Used in 3D mode to store transformation parameters for ASM entities
    std::unique_ptr<OGRDXFAffineTransform> poASMTransform;

    // Additional data for ATTRIB and ATTDEF entities
    CPLString osAttributeTag;

    // Store ATTRIB entities associated with an INSERT, for use when
    // DXF_INLINE_BLOCKS is true and a block with attributes is INSERTed
    // in another block
    std::vector<std::unique_ptr<OGRDXFFeature>> apoAttribFeatures;

  public:
    explicit OGRDXFFeature(OGRFeatureDefn *poFeatureDefn);
    ~OGRDXFFeature() override;

    OGRDXFFeature *CloneDXFFeature();

    DXFTriple GetOCS() const
    {
        return oOCS;
    }

    bool IsBlockReference() const
    {
        return bIsBlockReference;
    }

    CPLString GetBlockName() const
    {
        return osBlockName;
    }

    double GetBlockAngle() const
    {
        return dfBlockAngle;
    }

    DXFTriple GetBlockScale() const
    {
        return oBlockScale;
    }

    DXFTriple GetInsertOCSCoords() const
    {
        return oOriginalCoords;
    }

    CPLString GetAttributeTag() const
    {
        return osAttributeTag;
    }

    const std::vector<std::unique_ptr<OGRDXFFeature>> &GetAttribFeatures() const
    {
        return apoAttribFeatures;
    }

    void SetInsertOCSCoords(const DXFTriple &oTriple)
    {
        oOriginalCoords = oTriple;
    }

    void ApplyOCSTransformer(OGRGeometry *const poGeometry) const;
    void ApplyOCSTransformer(OGRDXFAffineTransform *const poCT) const;
    const CPLString GetColor(OGRDXFDataSource *const poDS,
                             OGRDXFFeature *const poBlockFeature = nullptr);
};

/************************************************************************/
/*                             OGRDXFLayer                              */
/************************************************************************/
class OGRDXFLayer final : public OGRLayer
{
    friend class OGRDXFBlocksLayer;

    OGRDXFDataSource *poDS;

    OGRFeatureDefn *poFeatureDefn;
    GIntBig iNextFID;

    std::set<CPLString> oIgnoredEntities;

    OGRDXFFeatureQueue apoPendingFeatures;

    struct InsertState
    {
        OGRDXFInsertTransformer m_oTransformer{};
        CPLString m_osBlockName{};
        CPLStringList m_aosAttribs{};
        int m_nColumnCount = 0;
        int m_nRowCount = 0;
        int m_iCurCol = 0;
        int m_iCurRow = 0;
        double m_dfColumnSpacing = 0.0;
        double m_dfRowSpacing = 0.0;
        std::vector<std::unique_ptr<OGRDXFFeature>> m_apoAttribs{};
        std::unique_ptr<OGRDXFFeature> m_poTemplateFeature{};
    };

    InsertState m_oInsertState{};

    void ClearPendingFeatures();

    void TranslateGenericProperty(OGRDXFFeature *poFeature, int nCode,
                                  char *pszValue);

    void PrepareFeatureStyle(OGRDXFFeature *const poFeature,
                             OGRDXFFeature *const poBlockFeature = nullptr);
    void PrepareBrushStyle(OGRDXFFeature *const poFeature,
                           OGRDXFFeature *const poBlockFeature = nullptr);
    void PrepareLineStyle(OGRDXFFeature *const poFeature,
                          OGRDXFFeature *const poBlockFeature = nullptr);

    OGRDXFFeature *TranslatePOINT();
    OGRDXFFeature *TranslateLINE();
    OGRDXFFeature *TranslatePOLYLINE();
    OGRDXFFeature *TranslateLWPOLYLINE();
    OGRDXFFeature *TranslateMLINE();
    OGRDXFFeature *TranslateCIRCLE();
    OGRDXFFeature *TranslateELLIPSE();
    OGRDXFFeature *TranslateARC();
    OGRDXFFeature *TranslateSPLINE();
    OGRDXFFeature *Translate3DFACE();
    bool TranslateINSERT();
    OGRDXFFeature *TranslateMTEXT();
    OGRDXFFeature *TranslateTEXT(const bool bIsAttribOrAttdef);
    OGRDXFFeature *TranslateDIMENSION();
    OGRDXFFeature *TranslateHATCH();
    OGRDXFFeature *TranslateSOLID();
    OGRDXFFeature *TranslateLEADER();
    OGRDXFFeature *TranslateMLEADER();
    OGRDXFFeature *TranslateWIPEOUT();
    OGRDXFFeature *TranslateASMEntity();

    static constexpr int FORTRAN_INDEXING = 1;

    bool GenerateINSERTFeatures();
    std::unique_ptr<OGRLineString>
    InsertSplineWithChecks(const int nDegree,
                           std::vector<double> &adfControlPoints, bool bHasZ,
                           int nControlPoints, std::vector<double> &adfKnots,
                           int nKnots, std::vector<double> &adfWeights);
    static OGRGeometry *SimplifyBlockGeometry(OGRGeometryCollection *);
    OGRDXFFeature *InsertBlockInline(GUInt32 nInitialErrorCounter,
                                     const CPLString &osBlockName,
                                     OGRDXFInsertTransformer oTransformer,
                                     OGRDXFFeature *const poFeature,
                                     OGRDXFFeatureQueue &apoExtraFeatures,
                                     const bool bInlineNestedBlocks,
                                     const bool bMergeGeometry);
    static OGRDXFFeature *
    InsertBlockReference(const CPLString &osBlockName,
                         const OGRDXFInsertTransformer &oTransformer,
                         OGRDXFFeature *const poFeature);
    static void FormatDimension(CPLString &osText, const double dfValue,
                                int nPrecision);
    void InsertArrowhead(OGRDXFFeature *const poFeature,
                         const CPLString &osBlockName,
                         OGRLineString *const poLine,
                         const double dfArrowheadSize,
                         const bool bReverse = false);
    OGRErr CollectBoundaryPath(OGRGeometryCollection *poGC,
                               const double dfElevation);
    OGRErr CollectPolylinePath(OGRGeometryCollection *poGC,
                               const double dfElevation);

    CPLString TextRecode(const char *);
    CPLString TextUnescape(const char *, bool);

  public:
    explicit OGRDXFLayer(OGRDXFDataSource *poDS);
    ~OGRDXFLayer();

    void ResetReading() override;
    OGRFeature *GetNextFeature() override;

    OGRFeatureDefn *GetLayerDefn() override
    {
        return poFeatureDefn;
    }

    int TestCapability(const char *) override;

    GDALDataset *GetDataset() override;

    OGRDXFFeature *GetNextUnfilteredFeature();
};

/************************************************************************/
/*                             OGRDXFReader                             */
/*                                                                      */
/*      A class for very low level DXF reading without interpretation.  */
/************************************************************************/

#define DXF_READER_ERROR()                                                     \
    do                                                                         \
    {                                                                          \
        CPLError(CE_Failure, CPLE_AppDefined,                                  \
                 "%s, %d: error at line %d of %s", __FILE__, __LINE__,         \
                 GetLineNumber(), GetDescription());                           \
    } while (0)
#define DXF_LAYER_READER_ERROR()                                               \
    do                                                                         \
    {                                                                          \
        CPLError(CE_Failure, CPLE_AppDefined,                                  \
                 "%s, %d: error at line %d of %s", __FILE__, __LINE__,         \
                 poDS->GetLineNumber(), poDS->GetDescription());               \
    } while (0)

class OGRDXFReaderBase
{
  protected:
    OGRDXFReaderBase() = default;

  public:
    virtual ~OGRDXFReaderBase();

    void Initialize(VSILFILE *fp);

    VSILFILE *fp = nullptr;

    unsigned int nLastValueSize = 0;
    int nLineNumber = 0;

    virtual uint64_t GetCurrentFilePos() const = 0;
    virtual int ReadValue(char *pszValueBuffer, int nValueBufferSize = 81) = 0;
    virtual void UnreadValue() = 0;
    virtual void ResetReadPointer(uint64_t iNewOffset,
                                  int nNewLineNumber = 0) = 0;
};

class OGRDXFReaderASCII final : public OGRDXFReaderBase
{
    int ReadValueRaw(char *pszValueBuffer, int nValueBufferSize);
    void LoadDiskChunk();

    unsigned int iSrcBufferOffset = 0;
    unsigned int nSrcBufferBytes = 0;
    uint64_t iSrcBufferFileOffset = 0;
    std::array<char, 1025> achSrcBuffer{};

  public:
    OGRDXFReaderASCII() = default;

    uint64_t GetCurrentFilePos() const override
    {
        return iSrcBufferFileOffset + iSrcBufferOffset;
    }

    int ReadValue(char *pszValueBuffer, int nValueBufferSize) override;
    void UnreadValue() override;
    void ResetReadPointer(uint64_t, int nNewLineNumber) override;
};

class OGRDXFReaderBinary final : public OGRDXFReaderBase
{
    bool m_bIsR12 = false;
    uint64_t m_nPrevPos = static_cast<uint64_t>(-1);

  public:
    OGRDXFReaderBinary() = default;

    uint64_t GetCurrentFilePos() const override
    {
        return VSIFTellL(fp);
    }

    int ReadValue(char *pszValueBuffer, int nValueBufferSize) override;
    void UnreadValue() override;
    void ResetReadPointer(uint64_t, int nNewLineNumber) override;
};

/************************************************************************/
/*                           OGRDXFFieldModes                           */
/*                                                                      */
/*    Represents which fields should be included in the data source.    */
/************************************************************************/

enum OGRDXFFieldModes
{
    ODFM_None = 0,
    ODFM_IncludeRawCodeValues = 0x1,
    ODFM_IncludeBlockFields = 0x2,
    ODFM_Include3DModeFields = 0x4
};

/************************************************************************/
/*                           OGRDXFDataSource                           */
/************************************************************************/

class OGRDXFDataSource final : public GDALDataset
{
    VSILFILE *fp;

    std::vector<OGRLayer *> apoLayers;

    uint64_t iEntitiesOffset;
    int iEntitiesLineNumber;

    std::map<CPLString, DXFBlockDefinition> oBlockMap;
    std::map<CPLString, CPLString> oBlockRecordHandles;
    std::map<CPLString, CPLString> oHeaderVariables;

    CPLString osEncoding;

    // indexed by layer name, then by property name.
    std::map<CPLString, std::map<CPLString, CPLString>> oLayerTable;

    // indexed by style name, then by property name.
    std::map<CPLString, std::map<CPLString, CPLString>> oTextStyleTable;
    std::map<CPLString, CPLString> oTextStyleHandles;

    // indexed by dimstyle name, then by DIM... variable name
    std::map<CPLString, std::map<CPLString, CPLString>> oDimStyleTable;

    std::map<CPLString, std::vector<double>> oLineTypeTable;

    bool bInlineBlocks;
    bool bMergeBlockGeometries;
    bool bTranslateEscapeSequences;
    bool bIncludeRawCodeValues;
    bool m_bClosedLineAsPolygon = false;
    double m_dfHatchTolerance = -1.0;

    bool b3DExtensibleMode;
    bool bHaveReadSolidData;
    std::map<CPLString, std::vector<GByte>> oSolidBinaryData;

    std::unique_ptr<OGRDXFReaderBase> poReader{};

    std::vector<CPLString> aosBlockInsertionStack;

  public:
    OGRDXFDataSource();
    ~OGRDXFDataSource();

    bool Open(const char *pszFilename, VSILFILE *fpIn, bool bHeaderOnly,
              CSLConstList papszOptionsIn);

    int GetLayerCount() override
    {
        return static_cast<int>(apoLayers.size());
    }

    OGRLayer *GetLayer(int) override;

    int TestCapability(const char *) override;

    // The following is only used by OGRDXFLayer

    bool InlineBlocks() const
    {
        return bInlineBlocks;
    }

    bool ShouldMergeBlockGeometries() const
    {
        return bMergeBlockGeometries;
    }

    bool ShouldTranslateEscapes() const
    {
        return bTranslateEscapeSequences;
    }

    bool ShouldIncludeRawCodeValues() const
    {
        return bIncludeRawCodeValues;
    }

    bool In3DExtensibleMode() const
    {
        return b3DExtensibleMode;
    }

    bool ClosedLineAsPolygon() const
    {
        return m_bClosedLineAsPolygon;
    }

    double HatchTolerance() const
    {
        return m_dfHatchTolerance;
    }

    static void AddStandardFields(OGRFeatureDefn *poDef, const int nFieldModes);

    // Implemented in ogrdxf_blockmap.cpp
    bool ReadBlocksSection();
    DXFBlockDefinition *LookupBlock(const char *pszName);
    CPLString GetBlockNameByRecordHandle(const char *pszID);

    std::map<CPLString, DXFBlockDefinition> &GetBlockMap()
    {
        return oBlockMap;
    }

    bool PushBlockInsertion(const CPLString &osBlockName);

    void PopBlockInsertion()
    {
        aosBlockInsertionStack.pop_back();
    }

    // Layer and other Table Handling (ogrdatasource.cpp)
    bool ReadTablesSection();
    bool ReadLayerDefinition();
    bool ReadLineTypeDefinition();
    bool ReadTextStyleDefinition();
    bool ReadDimStyleDefinition();
    std::optional<CPLString> LookupLayerProperty(const char *pszLayer,
                                                 const char *pszProperty) const;
    const char *LookupTextStyleProperty(const char *pszTextStyle,
                                        const char *pszProperty,
                                        const char *pszDefault);
    bool LookupDimStyle(const char *pszDimstyle,
                        std::map<CPLString, CPLString> &oDimStyleProperties);

    const std::map<CPLString, std::vector<double>> &GetLineTypeTable() const
    {
        return oLineTypeTable;
    }

    std::vector<double> LookupLineType(const char *pszName);
    bool TextStyleExists(const char *pszTextStyle);
    CPLString GetTextStyleNameByHandle(const char *pszID);
    static void PopulateDefaultDimStyleProperties(
        std::map<CPLString, CPLString> &oDimStyleProperties);
    size_t GetEntryFromAcDsDataSection(const char *pszEntityHandle,
                                       const GByte **pabyBuffer);

    // Header variables.
    bool ReadHeaderSection();
    const char *GetVariable(const char *pszName,
                            const char *pszDefault = nullptr);

    const char *GetEncoding()
    {
        return osEncoding;
    }

    // reader related.
    int GetLineNumber() const
    {
        return poReader->nLineNumber;
    }

    int ReadValue(char *pszValueBuffer, int nValueBufferSize = 81)
    {
        return poReader->ReadValue(pszValueBuffer, nValueBufferSize);
    }

    void RestartEntities()
    {
        poReader->ResetReadPointer(iEntitiesOffset, iEntitiesLineNumber);
    }

    void UnreadValue()
    {
        poReader->UnreadValue();
    }

    void ResetReadPointer(uint64_t iNewOffset)
    {
        poReader->ResetReadPointer(iNewOffset);
    }
};

/************************************************************************/
/*                          OGRDXFWriterLayer                           */
/************************************************************************/

class OGRDXFWriterDS;

class OGRDXFWriterLayer final : public OGRLayer
{
    VSILFILE *fp;
    OGRFeatureDefn *poFeatureDefn;

    OGRDXFWriterDS *poDS;

    int WriteValue(int nCode, const char *pszValue);
    int WriteValue(int nCode, int nValue);
    int WriteValue(int nCode, double dfValue);

    static constexpr int PROP_RGBA_COLOR = -1;

    using CorePropertiesType = std::vector<std::pair<int, std::string>>;

    OGRErr WriteCore(OGRFeature *, const CorePropertiesType &oCoreProperties);
    OGRErr WritePOINT(OGRFeature *);
    OGRErr WriteTEXT(OGRFeature *);
    OGRErr WritePOLYLINE(OGRFeature *, const OGRGeometry * = nullptr);
    OGRErr WriteHATCH(OGRFeature *, OGRGeometry * = nullptr);
    OGRErr WriteINSERT(OGRFeature *);

    static CPLString TextEscape(const char *);
    static int ColorStringToDXFColor(const char *, bool &bPerfectMatch);
    static std::vector<double> PrepareLineTypeDefinition(OGRStylePen *);
    static std::map<CPLString, CPLString>
    PrepareTextStyleDefinition(OGRStyleLabel *);

    std::map<CPLString, std::vector<double>> oNewLineTypes;
    std::map<CPLString, std::map<CPLString, CPLString>> oNewTextStyles;
    int nNextAutoID;
    int bWriteHatch;

  public:
    OGRDXFWriterLayer(OGRDXFWriterDS *poDS, VSILFILE *fp);
    ~OGRDXFWriterLayer();

    void ResetReading() override
    {
    }

    OGRFeature *GetNextFeature() override
    {
        return nullptr;
    }

    OGRFeatureDefn *GetLayerDefn() override
    {
        return poFeatureDefn;
    }

    int TestCapability(const char *) override;
    OGRErr ICreateFeature(OGRFeature *poFeature) override;
    OGRErr CreateField(const OGRFieldDefn *poField,
                       int bApproxOK = TRUE) override;

    GDALDataset *GetDataset() override;

    void ResetFP(VSILFILE *);

    std::map<CPLString, std::vector<double>> &GetNewLineTypeMap()
    {
        return oNewLineTypes;
    }

    std::map<CPLString, std::map<CPLString, CPLString>> &GetNewTextStyleMap()
    {
        return oNewTextStyles;
    }
};

/************************************************************************/
/*                       OGRDXFBlocksWriterLayer                        */
/************************************************************************/

class OGRDXFBlocksWriterLayer final : public OGRLayer
{
    OGRFeatureDefn *poFeatureDefn;

  public:
    explicit OGRDXFBlocksWriterLayer(OGRDXFWriterDS *poDS);
    ~OGRDXFBlocksWriterLayer();

    void ResetReading() override
    {
    }

    OGRFeature *GetNextFeature() override
    {
        return nullptr;
    }

    OGRFeatureDefn *GetLayerDefn() override
    {
        return poFeatureDefn;
    }

    int TestCapability(const char *) override;
    OGRErr ICreateFeature(OGRFeature *poFeature) override;
    OGRErr CreateField(const OGRFieldDefn *poField,
                       int bApproxOK = TRUE) override;

    std::vector<OGRFeature *> apoBlocks;
    OGRFeature *FindBlock(const char *);
};

/************************************************************************/
/*                           OGRDXFWriterDS                             */
/************************************************************************/

class OGRDXFWriterDS final : public GDALDataset
{
    friend class OGRDXFWriterLayer;

    int nNextFID;

    OGRDXFWriterLayer *poLayer;
    OGRDXFBlocksWriterLayer *poBlocksLayer;
    VSILFILE *fp;
    CPLString osTrailerFile;

    CPLString osTempFilename;
    VSILFILE *fpTemp;

    CPLString osHeaderFile;
    OGRDXFDataSource oHeaderDS;
    char **papszLayersToCreate;

    vsi_l_offset nHANDSEEDOffset;

    std::vector<int> anDefaultLayerCode;
    std::vector<CPLString> aosDefaultLayerText;

    std::set<CPLString> aosUsedEntities;
    void ScanForEntities(const char *pszFilename, const char *pszTarget);

    bool WriteNewLineTypeRecords(VSILFILE *fp);
    bool WriteNewTextStyleRecords(VSILFILE *fp);
    bool WriteNewBlockRecords(VSILFILE *);
    bool WriteNewBlockDefinitions(VSILFILE *);
    bool WriteNewLayerDefinitions(VSILFILE *);
    bool TransferUpdateHeader(VSILFILE *);
    bool TransferUpdateTrailer(VSILFILE *);
    bool FixupHANDSEED(VSILFILE *);

    OGREnvelope oGlobalEnvelope;

    bool m_bHeaderFileIsTemp = false;
    bool m_bTrailerFileIsTemp = false;
    OGRSpatialReference m_oSRS{};
    std::string m_osINSUNITS = "AUTO";
    std::string m_osMEASUREMENT = "HEADER_VALUE";

  public:
    OGRDXFWriterDS();
    ~OGRDXFWriterDS();

    int Open(const char *pszFilename, char **papszOptions);

    int GetLayerCount() override;
    OGRLayer *GetLayer(int) override;

    int TestCapability(const char *) override;

    OGRLayer *ICreateLayer(const char *pszName,
                           const OGRGeomFieldDefn *poGeomFieldDefn,
                           CSLConstList papszOptions) override;

    bool CheckEntityID(const char *pszEntityID);
    bool WriteEntityID(VSILFILE *fp, unsigned int &nAssignedFID,
                       GIntBig nPreferredFID = OGRNullFID);

    void UpdateExtent(OGREnvelope *psEnvelope);
};

#endif /* ndef OGR_DXF_H_INCLUDED */
