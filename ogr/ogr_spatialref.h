/******************************************************************************
 *
 * Project:  OpenGIS Simple Features Reference Implementation
 * Purpose:  Classes for manipulating spatial reference systems in a
 *           platform non-specific manner.
 * Author:   Frank Warmerdam, warmerdam@pobox.com
 *
 ******************************************************************************
 * Copyright (c) 1999,  Les Technologies SoftMap Inc.
 * Copyright (c) 2008-2013, Even Rouault <even dot rouault at spatialys.com>
 *
 * SPDX-License-Identifier: MIT
 ****************************************************************************/

#ifndef OGR_SPATIALREF_H_INCLUDED
#define OGR_SPATIALREF_H_INCLUDED

#include "cpl_string.h"
#include "ogr_srs_api.h"

#include <cstddef>
#include <map>
#include <memory>
#include <vector>

/**
 * \file ogr_spatialref.h
 *
 * Coordinate systems services.
 */

/************************************************************************/
/*                             OGR_SRSNode                              */
/************************************************************************/

/**
 * Objects of this class are used to represent value nodes in the parsed
 * representation of the WKT SRS format.  For instance UNIT["METER",1]
 * would be rendered into three OGR_SRSNodes.  The root node would have a
 * value of UNIT, and two children, the first with a value of METER, and the
 * second with a value of 1.
 *
 * Normally application code just interacts with the OGRSpatialReference
 * object, which uses the OGR_SRSNode to implement its data structure;
 * however, this class is user accessible for detailed access to components
 * of an SRS definition.
 */

class CPL_DLL OGR_SRSNode
{
  public:
    /** Listener that is notified of modification to nodes. */
    struct Listener
    {
        virtual ~Listener();
        /** Method triggered when a node is modified. */
        virtual void notifyChange(OGR_SRSNode *) = 0;
    };

    explicit OGR_SRSNode(const char * = nullptr);
    ~OGR_SRSNode();

    /** Register a (single) listener. */
    void RegisterListener(const std::shared_ptr<Listener> &listener);

    /** Return whether this is a leaf node.
     * @return TRUE or FALSE
     */
    int IsLeafNode() const
    {
        return nChildren == 0;
    }

    int GetChildCount() const
    {
        return nChildren;
    }

    OGR_SRSNode *GetChild(int);
    const OGR_SRSNode *GetChild(int) const;

    OGR_SRSNode *GetNode(const char *);
    const OGR_SRSNode *GetNode(const char *) const;

    void InsertChild(OGR_SRSNode *, int);
    void AddChild(OGR_SRSNode *);
    int FindChild(const char *) const;
    void DestroyChild(int);
    void ClearChildren();
    void StripNodes(const char *);

    const char *GetValue() const
    {
        return pszValue;
    }

    void SetValue(const char *);

    void MakeValueSafe();

    OGR_SRSNode *Clone() const;

    OGRErr importFromWkt(char **)
        /*! @cond Doxygen_Suppress */
        CPL_WARN_DEPRECATED("Use importFromWkt(const char**)")
        /*! @endcond */
        ;
    OGRErr importFromWkt(const char **);
    OGRErr exportToWkt(char **) const;
    OGRErr exportToPrettyWkt(char **, int = 1) const;

  private:
    char *pszValue;

    OGR_SRSNode **papoChildNodes;
    OGR_SRSNode *poParent;

    int nChildren;

    int NeedsQuoting() const;
    OGRErr importFromWkt(const char **, int nRecLevel, int *pnNodes);

    std::weak_ptr<Listener> m_listener{};
    void notifyChange();

    CPL_DISALLOW_COPY_ASSIGN(OGR_SRSNode)
};

/************************************************************************/
/*                         OGRSpatialReference                          */
/************************************************************************/

/**
 * This class represents an OpenGIS Spatial Reference System, and contains
 * methods for converting between this object organization and well known
 * text (WKT) format.  This object is reference counted as one instance of
 * the object is normally shared between many OGRGeometry objects.
 *
 * Normally application code can fetch needed parameter values for this
 * SRS using GetAttrValue(), but in special cases the underlying parse tree
 * (or OGR_SRSNode objects) can be accessed more directly.
 *
 * See <a href="https://gdal.org/tutorials/osr_api_tut.html">the tutorial
 * </a> for more information on how to use this class.
 *
 * Consult also the <a href="https://gdal.org/tutorials/wktproblems.html">
 * OGC WKT Coordinate System Issues</a> page for implementation details of
 * WKT in OGR.
 */

class CPL_DLL OGRSpatialReference
{
    struct Private;
    std::unique_ptr<Private> d;

    void GetNormInfo() const;

    // No longer used with PROJ >= 8.1.0
    OGRErr importFromURNPart(const char *pszAuthority, const char *pszCode,
                             const char *pszURN);

    static CPLString lookupInDict(const char *pszDictFile, const char *pszCode);

    OGRErr GetWKT2ProjectionMethod(const char **ppszMethodName,
                                   const char **ppszMethodAuthName = nullptr,
                                   const char **ppszMethodCode = nullptr) const;

  public:
    explicit OGRSpatialReference(const char * = nullptr);
    OGRSpatialReference(const OGRSpatialReference &);
    OGRSpatialReference(OGRSpatialReference &&);

    virtual ~OGRSpatialReference();

    static void DestroySpatialReference(OGRSpatialReference *poSRS);

    OGRSpatialReference &operator=(const OGRSpatialReference &);
    OGRSpatialReference &operator=(OGRSpatialReference &&);

    OGRSpatialReference &AssignAndSetThreadSafe(const OGRSpatialReference &);

    int Reference();
    int Dereference();
    int GetReferenceCount() const;
    void Release();

    const char *GetName() const;

    OGRSpatialReference *Clone() const;
    OGRSpatialReference *CloneGeogCS() const;

    void dumpReadable();
    OGRErr exportToWkt(char **) const;
    OGRErr exportToWkt(char **ppszWKT, const char *const *papszOptions) const;
    std::string exportToWkt(const char *const *papszOptions = nullptr) const;
    OGRErr exportToPrettyWkt(char **, int = FALSE) const;
    // cppcheck-suppress functionStatic
    OGRErr exportToPROJJSON(char **, const char *const *papszOptions) const;
    OGRErr exportToProj4(char **) const;
    OGRErr exportToPCI(char **, char **, double **) const;
    OGRErr exportToUSGS(long *, long *, double **, long *) const;
    OGRErr exportToXML(char **, const char * = nullptr) const;
    OGRErr exportToPanorama(long *, long *, long *, long *, double *) const;
    OGRErr exportVertCSToPanorama(int *) const;
    OGRErr exportToERM(char *pszProj, char *pszDatum, char *pszUnits);
    OGRErr exportToMICoordSys(char **) const;
    OGRErr exportToCF1(char **ppszGridMappingName, char ***ppapszKeyValues,
                       char **ppszUnits, CSLConstList papszOptions) const;

    OGRErr importFromWkt(char **)
        /*! @cond Doxygen_Suppress */
        CPL_WARN_DEPRECATED(
            "Use importFromWkt(const char**) or importFromWkt(const char*)")
        /*! @endcond */
        ;

    OGRErr importFromWkt(const char **);
    /*! @cond Doxygen_Suppress */
    OGRErr importFromWkt(const char *pszInput, CSLConstList papszOptions);
    OGRErr importFromWkt(const char **ppszInput, CSLConstList papszOptions);
    /*! @endcond */
    OGRErr importFromWkt(const char *);
    OGRErr importFromProj4(const char *);
    OGRErr importFromEPSG(int);
    OGRErr importFromEPSGA(int);
    OGRErr importFromESRI(char **);
    OGRErr importFromPCI(const char *, const char * = nullptr,
                         const double * = nullptr);

#define USGS_ANGLE_DECIMALDEGREES 0 /**< Angle is in decimal degrees. */
#define USGS_ANGLE_PACKEDDMS                                                   \
    TRUE                     /**< Angle is in packed degree minute second. */
#define USGS_ANGLE_RADIANS 2 /**< Angle is in radians. */
    OGRErr importFromUSGS(long iProjSys, long iZone, double *padfPrjParams,
                          long iDatum,
                          int nUSGSAngleFormat = USGS_ANGLE_PACKEDDMS);
    OGRErr importFromPanorama(long, long, long, double *, bool bNorth = true);
    OGRErr importVertCSFromPanorama(int);
    OGRErr importFromOzi(const char *const *papszLines);
    OGRErr importFromWMSAUTO(const char *pszAutoDef);
    OGRErr importFromXML(const char *);
    OGRErr importFromDict(const char *pszDict, const char *pszCode);
    OGRErr importFromURN(const char *);
    OGRErr importFromCRSURL(const char *);
    OGRErr importFromERM(const char *pszProj, const char *pszDatum,
                         const char *pszUnits);
    OGRErr importFromUrl(const char *);
    OGRErr importFromMICoordSys(const char *);
    OGRErr importFromCF1(CSLConstList papszKeyValues, const char *pszUnits);

    OGRErr morphToESRI();
    OGRErr morphFromESRI();

    OGRSpatialReference *
    convertToOtherProjection(const char *pszTargetProjection,
                             const char *const *papszOptions = nullptr) const;

    OGRErr Validate() const;
    OGRErr StripVertical();

    bool StripTOWGS84IfKnownDatumAndAllowed();
    bool StripTOWGS84IfKnownDatum();

    int EPSGTreatsAsLatLong() const;
    int EPSGTreatsAsNorthingEasting() const;
    int GetAxesCount() const;
    const char *GetAxis(const char *pszTargetKey, int iAxis,
                        OGRAxisOrientation *peOrientation,
                        double *pdfConvFactor = nullptr) const;
    OGRErr SetAxes(const char *pszTargetKey, const char *pszXAxisName,
                   OGRAxisOrientation eXAxisOrientation,
                   const char *pszYAxisName,
                   OGRAxisOrientation eYAxisOrientation);

    OSRAxisMappingStrategy GetAxisMappingStrategy() const;
    void SetAxisMappingStrategy(OSRAxisMappingStrategy);
    const std::vector<int> &GetDataAxisToSRSAxisMapping() const;
    OGRErr SetDataAxisToSRSAxisMapping(const std::vector<int> &mapping);

    // Machinery for accessing parse nodes

    //! Return root node
    OGR_SRSNode *GetRoot();
    //! Return root node
    const OGR_SRSNode *GetRoot() const;
    void SetRoot(OGR_SRSNode *);

    OGR_SRSNode *GetAttrNode(const char *);
    const OGR_SRSNode *GetAttrNode(const char *) const;
    const char *GetAttrValue(const char *, int = 0) const;

    OGRErr SetNode(const char *, const char *);
    // cppcheck-suppress functionStatic
    OGRErr SetNode(const char *, double);

    OGRErr
    SetLinearUnitsAndUpdateParameters(const char *pszName, double dfInMeters,
                                      const char *pszUnitAuthority = nullptr,
                                      const char *pszUnitCode = nullptr);
    OGRErr SetLinearUnits(const char *pszName, double dfInMeters);
    OGRErr SetTargetLinearUnits(const char *pszTargetKey, const char *pszName,
                                double dfInMeters,
                                const char *pszUnitAuthority = nullptr,
                                const char *pszUnitCode = nullptr);

    double GetLinearUnits(char **) const
        /*! @cond Doxygen_Suppress */
        CPL_WARN_DEPRECATED("Use GetLinearUnits(const char**) instead")
        /*! @endcond */
        ;
    double GetLinearUnits(const char ** = nullptr) const;

    /*! @cond Doxygen_Suppress */
    double GetLinearUnits(std::nullptr_t) const
    {
        return GetLinearUnits(static_cast<const char **>(nullptr));
    }

    /*! @endcond */

    double GetTargetLinearUnits(const char *pszTargetKey,
                                char **ppszRetName) const
        /*! @cond Doxygen_Suppress */
        CPL_WARN_DEPRECATED(
            "Use GetTargetLinearUnits(const char*, const char**)")
        /*! @endcond */
        ;
    double GetTargetLinearUnits(const char *pszTargetKey,
                                const char **ppszRetName = nullptr) const;

    /*! @cond Doxygen_Suppress */
    double GetTargetLinearUnits(const char *pszTargetKey, std::nullptr_t) const
    {
        return GetTargetLinearUnits(pszTargetKey,
                                    static_cast<const char **>(nullptr));
    }

    /*! @endcond */

    OGRErr SetAngularUnits(const char *pszName, double dfInRadians);
    double GetAngularUnits(char **) const
        /*! @cond Doxygen_Suppress */
        CPL_WARN_DEPRECATED("Use GetAngularUnits(const char**) instead")
        /*! @endcond */
        ;
    double GetAngularUnits(const char ** = nullptr) const;

    /*! @cond Doxygen_Suppress */
    double GetAngularUnits(std::nullptr_t) const
    {
        return GetAngularUnits(static_cast<const char **>(nullptr));
    }

    /*! @endcond */

    double GetPrimeMeridian(char **) const
        /*! @cond Doxygen_Suppress */
        CPL_WARN_DEPRECATED("Use GetPrimeMeridian(const char**) instead")
        /*! @endcond */
        ;
    double GetPrimeMeridian(const char ** = nullptr) const;

    /*! @cond Doxygen_Suppress */
    double GetPrimeMeridian(std::nullptr_t) const
    {
        return GetPrimeMeridian(static_cast<const char **>(nullptr));
    }

    /*! @endcond */

    bool IsEmpty() const;
    int IsGeographic() const;
    int IsDerivedGeographic() const;
    int IsProjected() const;
    int IsDerivedProjected() const;
    int IsGeocentric() const;
    bool IsDynamic() const;

    // cppcheck-suppress functionStatic
    bool HasPointMotionOperation() const;

    int IsLocal() const;
    int IsVertical() const;
    int IsCompound() const;
    int IsSameGeogCS(const OGRSpatialReference *) const;
    int IsSameGeogCS(const OGRSpatialReference *,
                     const char *const *papszOptions) const;
    int IsSameVertCS(const OGRSpatialReference *) const;
    int IsSame(const OGRSpatialReference *) const;
    int IsSame(const OGRSpatialReference *,
               const char *const *papszOptions) const;

    const char *GetCelestialBodyName() const;

    void Clear();
    OGRErr SetLocalCS(const char *);
    OGRErr SetProjCS(const char *);
    OGRErr SetProjection(const char *);
    OGRErr SetGeocCS(const char *pszGeocName);
    OGRErr SetGeogCS(const char *pszGeogName, const char *pszDatumName,
                     const char *pszEllipsoidName, double dfSemiMajor,
                     double dfInvFlattening, const char *pszPMName = nullptr,
                     double dfPMOffset = 0.0, const char *pszUnits = nullptr,
                     double dfConvertToRadians = 0.0);
    OGRErr SetWellKnownGeogCS(const char *);
    OGRErr CopyGeogCSFrom(const OGRSpatialReference *poSrcSRS);
    OGRErr SetVertCS(const char *pszVertCSName, const char *pszVertDatumName,
                     int nVertDatumClass = 2005);
    OGRErr SetCompoundCS(const char *pszName,
                         const OGRSpatialReference *poHorizSRS,
                         const OGRSpatialReference *poVertSRS);

    void SetCoordinateEpoch(double dfCoordinateEpoch);
    double GetCoordinateEpoch() const;

    // cppcheck-suppress functionStatic
    OGRErr PromoteTo3D(const char *pszName);
    // cppcheck-suppress functionStatic
    OGRErr DemoteTo2D(const char *pszName);

    OGRErr SetFromUserInput(const char *);

    static const char *const SET_FROM_USER_INPUT_LIMITATIONS[];
    static CSLConstList SET_FROM_USER_INPUT_LIMITATIONS_get();

    OGRErr SetFromUserInput(const char *, CSLConstList papszOptions);

    OGRErr SetTOWGS84(double, double, double, double = 0.0, double = 0.0,
                      double = 0.0, double = 0.0);
    OGRErr GetTOWGS84(double *padfCoef, int nCoeff = 7) const;
    OGRErr AddGuessedTOWGS84();

    double GetSemiMajor(OGRErr * = nullptr) const;
    double GetSemiMinor(OGRErr * = nullptr) const;
    double GetInvFlattening(OGRErr * = nullptr) const;
    double GetEccentricity() const;
    double GetSquaredEccentricity() const;

    OGRErr SetAuthority(const char *pszTargetKey, const char *pszAuthority,
                        int nCode);

    OGRErr AutoIdentifyEPSG();
    OGRSpatialReferenceH *FindMatches(char **papszOptions, int *pnEntries,
                                      int **ppanMatchConfidence) const;
    OGRSpatialReference *
    FindBestMatch(int nMinimumMatchConfidence = 90,
                  const char *pszPreferredAuthority = "EPSG",
                  CSLConstList papszOptions = nullptr) const;

    int GetEPSGGeogCS() const;

    const char *GetAuthorityCode(const char *pszTargetKey) const;
    const char *GetAuthorityName(const char *pszTargetKey) const;
    char *GetOGCURN() const;

    bool GetAreaOfUse(double *pdfWestLongitudeDeg, double *pdfSouthLatitudeDeg,
                      double *pdfEastLongitudeDeg, double *pdfNorthLatitudeDeg,
                      const char **ppszAreaName) const;

    const char *GetExtension(const char *pszTargetKey, const char *pszName,
                             const char *pszDefault = nullptr) const;
    OGRErr SetExtension(const char *pszTargetKey, const char *pszName,
                        const char *pszValue);

    int FindProjParm(const char *pszParameter,
                     const OGR_SRSNode *poPROJCS = nullptr) const;
    OGRErr SetProjParm(const char *, double);
    double GetProjParm(const char *, double = 0.0, OGRErr * = nullptr) const;

    OGRErr SetNormProjParm(const char *, double);
    double GetNormProjParm(const char *, double = 0.0,
                           OGRErr * = nullptr) const;

    static int IsAngularParameter(const char *);
    static int IsLongitudeParameter(const char *);
    static int IsLinearParameter(const char *);

    /** Albers Conic Equal Area */
    OGRErr SetACEA(double dfStdP1, double dfStdP2, double dfCenterLat,
                   double dfCenterLong, double dfFalseEasting,
                   double dfFalseNorthing);

    /** Azimuthal Equidistant */
    OGRErr SetAE(double dfCenterLat, double dfCenterLong, double dfFalseEasting,
                 double dfFalseNorthing);

    /** Bonne */
    OGRErr SetBonne(double dfStdP1, double dfCentralMeridian,
                    double dfFalseEasting, double dfFalseNorthing);

    /** Cylindrical Equal Area */
    OGRErr SetCEA(double dfStdP1, double dfCentralMeridian,
                  double dfFalseEasting, double dfFalseNorthing);

    /** Cassini-Soldner */
    OGRErr SetCS(double dfCenterLat, double dfCenterLong, double dfFalseEasting,
                 double dfFalseNorthing);

    /** Equidistant Conic */
    OGRErr SetEC(double dfStdP1, double dfStdP2, double dfCenterLat,
                 double dfCenterLong, double dfFalseEasting,
                 double dfFalseNorthing);

    /** Eckert I */
    OGRErr SetEckert(int nVariation, double dfCentralMeridian,
                     double dfFalseEasting, double dfFalseNorthing);

    /** Eckert IV */
    OGRErr SetEckertIV(double dfCentralMeridian, double dfFalseEasting,
                       double dfFalseNorthing);

    /** Eckert VI */
    OGRErr SetEckertVI(double dfCentralMeridian, double dfFalseEasting,
                       double dfFalseNorthing);

    /** Equirectangular */
    OGRErr SetEquirectangular(double dfCenterLat, double dfCenterLong,
                              double dfFalseEasting, double dfFalseNorthing);
    /** Equirectangular generalized form : */
    OGRErr SetEquirectangular2(double dfCenterLat, double dfCenterLong,
                               double dfPseudoStdParallel1,
                               double dfFalseEasting, double dfFalseNorthing);

    /** Geostationary Satellite */
    OGRErr SetGEOS(double dfCentralMeridian, double dfSatelliteHeight,
                   double dfFalseEasting, double dfFalseNorthing);

    /** Goode Homolosine */
    OGRErr SetGH(double dfCentralMeridian, double dfFalseEasting,
                 double dfFalseNorthing);

    /** Interrupted Goode Homolosine */
    OGRErr SetIGH();

    /** Gall Stereographic */
    OGRErr SetGS(double dfCentralMeridian, double dfFalseEasting,
                 double dfFalseNorthing);

    /** Gauss Schreiber Transverse Mercator */
    OGRErr SetGaussSchreiberTMercator(double dfCenterLat, double dfCenterLong,
                                      double dfScale, double dfFalseEasting,
                                      double dfFalseNorthing);

    /** Gnomonic */
    OGRErr SetGnomonic(double dfCenterLat, double dfCenterLong,
                       double dfFalseEasting, double dfFalseNorthing);

    /** Hotine Oblique Mercator */
    OGRErr SetHOM(double dfCenterLat, double dfCenterLong, double dfAzimuth,
                  double dfRectToSkew, double dfScale, double dfFalseEasting,
                  double dfFalseNorthing);

    /**  Hotine Oblique Mercator 2 points */
    OGRErr SetHOM2PNO(double dfCenterLat, double dfLat1, double dfLong1,
                      double dfLat2, double dfLong2, double dfScale,
                      double dfFalseEasting, double dfFalseNorthing);

    /** Hotine Oblique Mercator Azimuth Center / Variant B */
    OGRErr SetHOMAC(double dfCenterLat, double dfCenterLong, double dfAzimuth,
                    double dfRectToSkew, double dfScale, double dfFalseEasting,
                    double dfFalseNorthing);

    /** Laborde Oblique Mercator */
    OGRErr SetLOM(double dfCenterLat, double dfCenterLong, double dfAzimuth,
                  double dfScale, double dfFalseEasting,
                  double dfFalseNorthing);

    /** International Map of the World Polyconic */
    OGRErr SetIWMPolyconic(double dfLat1, double dfLat2, double dfCenterLong,
                           double dfFalseEasting, double dfFalseNorthing);

    /** Krovak Oblique Conic Conformal */
    OGRErr SetKrovak(double dfCenterLat, double dfCenterLong, double dfAzimuth,
                     double dfPseudoStdParallelLat, double dfScale,
                     double dfFalseEasting, double dfFalseNorthing);

    /** Lambert Azimuthal Equal-Area */
    OGRErr SetLAEA(double dfCenterLat, double dfCenterLong,
                   double dfFalseEasting, double dfFalseNorthing);

    /** Lambert Conformal Conic */
    OGRErr SetLCC(double dfStdP1, double dfStdP2, double dfCenterLat,
                  double dfCenterLong, double dfFalseEasting,
                  double dfFalseNorthing);

    /** Lambert Conformal Conic 1SP */
    OGRErr SetLCC1SP(double dfCenterLat, double dfCenterLong, double dfScale,
                     double dfFalseEasting, double dfFalseNorthing);

    /** Lambert Conformal Conic (Belgium) */
    OGRErr SetLCCB(double dfStdP1, double dfStdP2, double dfCenterLat,
                   double dfCenterLong, double dfFalseEasting,
                   double dfFalseNorthing);

    /** Miller Cylindrical */
    OGRErr SetMC(double dfCenterLat, double dfCenterLong, double dfFalseEasting,
                 double dfFalseNorthing);

    /** Mercator 1SP */
    OGRErr SetMercator(double dfCenterLat, double dfCenterLong, double dfScale,
                       double dfFalseEasting, double dfFalseNorthing);

    /** Mercator 2SP */
    OGRErr SetMercator2SP(double dfStdP1, double dfCenterLat,
                          double dfCenterLong, double dfFalseEasting,
                          double dfFalseNorthing);

    /** Mollweide */
    OGRErr SetMollweide(double dfCentralMeridian, double dfFalseEasting,
                        double dfFalseNorthing);

    /** New Zealand Map Grid */
    OGRErr SetNZMG(double dfCenterLat, double dfCenterLong,
                   double dfFalseEasting, double dfFalseNorthing);

    /** Oblique Stereographic */
    OGRErr SetOS(double dfOriginLat, double dfCMeridian, double dfScale,
                 double dfFalseEasting, double dfFalseNorthing);

    /** Orthographic */
    OGRErr SetOrthographic(double dfCenterLat, double dfCenterLong,
                           double dfFalseEasting, double dfFalseNorthing);

    /** Polyconic */
    OGRErr SetPolyconic(double dfCenterLat, double dfCenterLong,
                        double dfFalseEasting, double dfFalseNorthing);

    /** Polar Stereographic */
    OGRErr SetPS(double dfCenterLat, double dfCenterLong, double dfScale,
                 double dfFalseEasting, double dfFalseNorthing);

    /** Robinson */
    OGRErr SetRobinson(double dfCenterLong, double dfFalseEasting,
                       double dfFalseNorthing);

    /** Sinusoidal */
    OGRErr SetSinusoidal(double dfCenterLong, double dfFalseEasting,
                         double dfFalseNorthing);

    /** Stereographic */
    OGRErr SetStereographic(double dfCenterLat, double dfCenterLong,
                            double dfScale, double dfFalseEasting,
                            double dfFalseNorthing);

    /** Swiss Oblique Cylindrical */
    OGRErr SetSOC(double dfLatitudeOfOrigin, double dfCentralMeridian,
                  double dfFalseEasting, double dfFalseNorthing);

    /** Transverse Mercator */
    OGRErr SetTM(double dfCenterLat, double dfCenterLong, double dfScale,
                 double dfFalseEasting, double dfFalseNorthing);

    /** Transverse Mercator variants. */
    OGRErr SetTMVariant(const char *pszVariantName, double dfCenterLat,
                        double dfCenterLong, double dfScale,
                        double dfFalseEasting, double dfFalseNorthing);

    /** Tunesia Mining Grid  */
    OGRErr SetTMG(double dfCenterLat, double dfCenterLong,
                  double dfFalseEasting, double dfFalseNorthing);

    /** Transverse Mercator (South Oriented) */
    OGRErr SetTMSO(double dfCenterLat, double dfCenterLong, double dfScale,
                   double dfFalseEasting, double dfFalseNorthing);

    /** Two Point Equidistant */
    OGRErr SetTPED(double dfLat1, double dfLong1, double dfLat2, double dfLong2,
                   double dfFalseEasting, double dfFalseNorthing);

    /** VanDerGrinten */
    OGRErr SetVDG(double dfCenterLong, double dfFalseEasting,
                  double dfFalseNorthing);

    /** Universal Transverse Mercator */
    OGRErr SetUTM(int nZone, int bNorth = TRUE);
    int GetUTMZone(int *pbNorth = nullptr) const;

    /** Wagner I \-- VII */
    OGRErr SetWagner(int nVariation, double dfCenterLat, double dfFalseEasting,
                     double dfFalseNorthing);

    /** Quadrilateralized Spherical Cube */
    OGRErr SetQSC(double dfCenterLat, double dfCenterLong);

    /** Spherical, Cross-track, Height */
    OGRErr SetSCH(double dfPegLat, double dfPegLong, double dfPegHeading,
                  double dfPegHgt);

    /** Vertical Perspective / Near-sided Perspective */
    OGRErr
    SetVerticalPerspective(double dfTopoOriginLat, double dfTopoOriginLon,
                           double dfTopoOriginHeight, double dfViewPointHeight,
                           double dfFalseEasting, double dfFalseNorthing);

    /** Pole rotation (GRIB convention) */
    OGRErr SetDerivedGeogCRSWithPoleRotationGRIBConvention(
        const char *pszCRSName, double dfSouthPoleLat, double dfSouthPoleLon,
        double dfAxisRotation);

    /** Pole rotation (netCDF CF convention) */
    OGRErr SetDerivedGeogCRSWithPoleRotationNetCDFCFConvention(
        const char *pszCRSName, double dfGridNorthPoleLat,
        double dfGridNorthPoleLon, double dfNorthPoleGridLon);

    /** State Plane */
    OGRErr SetStatePlane(int nZone, int bNAD83 = TRUE,
                         const char *pszOverrideUnitName = nullptr,
                         double dfOverrideUnit = 0.0);

    /** ImportFromESRIStatePlaneWKT */
    OGRErr ImportFromESRIStatePlaneWKT(int nCode, const char *pszDatumName,
                                       const char *pszUnitsName, int nPCSCode,
                                       const char *pszCRSName = nullptr);

    /** ImportFromESRIWisconsinWKT */
    OGRErr ImportFromESRIWisconsinWKT(const char *pszPrjName,
                                      double dfCentralMeridian,
                                      double dfLatOfOrigin,
                                      const char *pszUnitsName,
                                      const char *pszCRSName = nullptr);

    /*! @cond Doxygen_Suppress */
    void UpdateCoordinateSystemFromGeogCRS();
    /*! @endcond */

    static OGRSpatialReference *GetWGS84SRS();

    /** Convert a OGRSpatialReference* to a OGRSpatialReferenceH.
     * @since GDAL 2.3
     */
    static inline OGRSpatialReferenceH ToHandle(OGRSpatialReference *poSRS)
    {
        return reinterpret_cast<OGRSpatialReferenceH>(poSRS);
    }

    /** Convert a OGRSpatialReferenceH to a OGRSpatialReference*.
     * @since GDAL 2.3
     */
    static inline OGRSpatialReference *FromHandle(OGRSpatialReferenceH hSRS)
    {
        return reinterpret_cast<OGRSpatialReference *>(hSRS);
    }
};

/*! @cond Doxygen_Suppress */
struct CPL_DLL OGRSpatialReferenceReleaser
{
    void operator()(OGRSpatialReference *poSRS) const
    {
        if (poSRS)
            poSRS->Release();
    }
};

/*! @endcond */

/************************************************************************/
/*                     OGRCoordinateTransformation                      */
/*                                                                      */
/*      This is really just used as a base class for a private          */
/*      implementation.                                                 */
/************************************************************************/

/**
 * Interface for transforming between coordinate systems.
 *
 * Currently, the only implementation within OGR is OGRProjCT, which
 * requires the PROJ library.
 *
 * Also, see OGRCreateCoordinateTransformation() for creating transformations.
 */

class CPL_DLL OGRCoordinateTransformation
{
  public:
    virtual ~OGRCoordinateTransformation();

    static void DestroyCT(OGRCoordinateTransformation *poCT);

    // From CT_CoordinateTransformation

    /** Fetch internal source coordinate system. */
    virtual const OGRSpatialReference *GetSourceCS() const = 0;

    /** Fetch internal target coordinate system. */
    virtual const OGRSpatialReference *GetTargetCS() const = 0;

    /** Whether the transformer will emit CPLError */
    virtual bool GetEmitErrors() const
    {
        return false;
    }

    /** Set if the transformer must emit CPLError */
    virtual void SetEmitErrors(bool /*bEmitErrors*/)
    {
    }

    // From CT_MathTransform

    /**
     * Transform points from source to destination space.
     *
     * This method is the same as the C function OCTTransformEx().
     *
     * @param nCount number of points to transform (`size_t` type since 3.9,
     *               `int` in previous versions).
     * @param x array of nCount X vertices, modified in place. Should not be
     * NULL.
     * @param y array of nCount Y vertices, modified in place. Should not be
     * NULL.
     * @param z array of nCount Z vertices, modified in place. Might be NULL.
     * @param pabSuccess array of per-point flags set to TRUE if that point
     * transforms, or FALSE if it does not. Might be NULL.
     *
     * @return TRUE on success, or FALSE if some or all points fail to
     * transform. When FALSE is returned the pabSuccess[] array indicates which
     * points succeeded or failed to transform. When TRUE is returned, all
     * values in pabSuccess[] are set to true.
     */
    int Transform(size_t nCount, double *x, double *y, double *z = nullptr,
                  int *pabSuccess = nullptr);

    /**
     * Transform points from source to destination space.
     *
     * This method is the same as the C function OCTTransform4D().
     *
     * @param nCount number of points to transform (`size_t` type since 3.9,
     *               `int` in previous versions).
     * @param x array of nCount X vertices, modified in place. Should not be
     * NULL.
     * @param y array of nCount Y vertices, modified in place. Should not be
     * NULL.
     * @param z array of nCount Z vertices, modified in place. Might be NULL.
     * @param t array of nCount time values, modified in place. Might be NULL.
     * @param pabSuccess array of per-point flags set to TRUE if that point
     * transforms, or FALSE if it does not. Might be NULL.
     *
     * @return TRUE on success, or FALSE if some or all points fail to
     * transform. When FALSE is returned the pabSuccess[] array indicates which
     * points succeeded or failed to transform. When TRUE is returned, all
     * values in pabSuccess[] are set to true.
     * Caution: prior to GDAL 3.11, TRUE could be returned if a
     * transformation could be found but not all points may
     * have necessarily succeed to transform.
     */
    virtual int Transform(size_t nCount, double *x, double *y, double *z,
                          double *t, int *pabSuccess) = 0;

    /**
     * Transform points from source to destination space.
     *
     * This method is the same as the C function OCTTransform4DWithErrorCodes().
     *
     * @param nCount number of points to transform (`size_t` type since 3.9,
     *               `int` in previous versions).
     * @param x array of nCount X vertices, modified in place. Should not be
     * NULL.
     * @param y array of nCount Y vertices, modified in place. Should not be
     * NULL.
     * @param z array of nCount Z vertices, modified in place. Might be NULL.
     * @param t array of nCount time values, modified in place. Might be NULL.
     * @param panErrorCodes Output array of nCount value that will be set to 0
     * for success, or a non-zero value for failure. Refer to PROJ 8 public
     * error codes. Might be NULL
     * @return TRUE on success, or FALSE if some or all points fail to
     * transform. When FALSE is returned the panErrorCodes[] array indicates
     * which points succeeded or failed to transform. When TRUE is returned, all
     * values in panErrorCodes[] are set to zero.
     * Caution: prior to GDAL 3.11, TRUE could be returned if a
     * transformation could be found but not all points may
     * have necessarily succeed to transform.
     * @since GDAL 3.3, and PROJ 8 to be able to use PROJ public error codes
     */
    virtual int TransformWithErrorCodes(size_t nCount, double *x, double *y,
                                        double *z, double *t,
                                        int *panErrorCodes);

    /** \brief Transform boundary.
     *
     * This method is the same as the C function OCTTransformBounds().
     *
     * Transform boundary densifying the edges to account for nonlinear
     * transformations along these edges and extracting the outermost bounds.
     *
     * If the destination CRS is geographic, the first axis is longitude,
     * and xmax < xmin then the bounds crossed the antimeridian.
     * In this scenario there are two polygons, one on each side of the
     * antimeridian. The first polygon should be constructed with (xmin, ymin,
     * 180, ymax) and the second with (-180, ymin, xmax, ymax).
     *
     * If the destination CRS is geographic, the first axis is latitude,
     * and ymax < ymin then the bounds crossed the antimeridian.
     * In this scenario there are two polygons, one on each side of the
     * antimeridian. The first polygon should be constructed with (ymin, xmin,
     * ymax, 180) and the second with (ymin, -180, ymax, xmax).
     *
     * @param xmin Minimum bounding coordinate of the first axis in source CRS.
     * @param ymin Minimum bounding coordinate of the second axis in source CRS.
     * @param xmax Maximum bounding coordinate of the first axis in source CRS.
     * @param ymax Maximum bounding coordinate of the second axis in source CRS.
     * @param out_xmin Minimum bounding coordinate of the first axis in target
     * CRS
     * @param out_ymin Minimum bounding coordinate of the second axis in target
     * CRS.
     * @param out_xmax Maximum bounding coordinate of the first axis in target
     * CRS.
     * @param out_ymax Maximum bounding coordinate of the second axis in target
     * CRS.
     * @param densify_pts Recommended to use 21. This is the number of points
     *     to use to densify the bounding polygon in the transformation.
     * @return TRUE if successful. FALSE if failures encountered.
     * @since 3.4
     */
    virtual int TransformBounds(const double xmin, const double ymin,
                                const double xmax, const double ymax,
                                double *out_xmin, double *out_ymin,
                                double *out_xmax, double *out_ymax,
                                const int densify_pts)
    {
        (void)xmin;
        (void)xmax;
        (void)ymin;
        (void)ymax;
        (void)densify_pts;
        *out_xmin = HUGE_VAL;
        *out_ymin = HUGE_VAL;
        *out_xmax = HUGE_VAL;
        *out_ymax = HUGE_VAL;
        CPLError(CE_Failure, CPLE_AppDefined,
                 "TransformBounds not implemented.");
        return false;
    }

    /** Convert a OGRCoordinateTransformation* to a
     * OGRCoordinateTransformationH.
     * @since GDAL 2.3
     */
    static inline OGRCoordinateTransformationH
    ToHandle(OGRCoordinateTransformation *poCT)
    {
        return reinterpret_cast<OGRCoordinateTransformationH>(poCT);
    }

    /** Convert a OGRCoordinateTransformationH to a
     * OGRCoordinateTransformation*.
     * @since GDAL 2.3
     */
    static inline OGRCoordinateTransformation *
    FromHandle(OGRCoordinateTransformationH hCT)
    {
        return reinterpret_cast<OGRCoordinateTransformation *>(hCT);
    }

    /** Clone
     * @since GDAL 3.1
     */
    virtual OGRCoordinateTransformation *Clone() const = 0;

    /** Return a coordinate transformation that performs the inverse
     * transformation of the current one.
     *
     * In some cases, this is not possible, and this method might return
     * nullptr, or fail to perform the transformations.
     *
     * @return the new coordinate transformation, or nullptr in case of error.
     * @since GDAL 3.3
     */
    virtual OGRCoordinateTransformation *GetInverse() const = 0;

  protected:
    /*! @cond Doxygen_Suppress */
    OGRCoordinateTransformation() = default;
    OGRCoordinateTransformation(const OGRCoordinateTransformation &) = default;
    OGRCoordinateTransformation &
    operator=(const OGRCoordinateTransformation &) = default;
    OGRCoordinateTransformation(OGRCoordinateTransformation &&) = default;
    OGRCoordinateTransformation &
    operator=(OGRCoordinateTransformation &&) = default;
    /*! @endcond */
};

OGRCoordinateTransformation CPL_DLL *
OGRCreateCoordinateTransformation(const OGRSpatialReference *poSource,
                                  const OGRSpatialReference *poTarget);

/**
 * Context for coordinate transformation.
 *
 * @since GDAL 3.0
 */

struct CPL_DLL OGRCoordinateTransformationOptions
{
    /*! @cond Doxygen_Suppress */
  private:
    friend class OGRProjCT;
    struct Private;
    std::unique_ptr<Private> d;
    /*! @endcond */

  public:
    OGRCoordinateTransformationOptions();
    OGRCoordinateTransformationOptions(
        const OGRCoordinateTransformationOptions &);
    OGRCoordinateTransformationOptions &
    operator=(const OGRCoordinateTransformationOptions &);
    ~OGRCoordinateTransformationOptions();

    bool SetAreaOfInterest(double dfWestLongitudeDeg, double dfSouthLatitudeDeg,
                           double dfEastLongitudeDeg,
                           double dfNorthLatitudeDeg);
    bool SetDesiredAccuracy(double dfAccuracy);
    bool SetBallparkAllowed(bool bAllowBallpark);
    bool SetOnlyBest(bool bOnlyBest);

    bool SetCoordinateOperation(const char *pszCT, bool bReverseCT);
    /*! @cond Doxygen_Suppress */
    void SetSourceCenterLong(double dfCenterLong);
    void SetTargetCenterLong(double dfCenterLong);
    /*! @endcond */
};

OGRCoordinateTransformation CPL_DLL *OGRCreateCoordinateTransformation(
    const OGRSpatialReference *poSource, const OGRSpatialReference *poTarget,
    const OGRCoordinateTransformationOptions &options);

#endif /* ndef OGR_SPATIALREF_H_INCLUDED */
