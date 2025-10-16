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

#include "geotiff.h"
#include "cpl_string.h"
#include "ogr_srs_api.h"

#include <memory>
#include <string>
#include <vector>

struct pj_ctx;

 /** Type for a OGR error */
typedef enum
{
    OGRERR_NONE,                      /**< Success */
    OGRERR_NOT_ENOUGH_DATA,           /**< Not enough data to deserialize */
    OGRERR_NOT_ENOUGH_MEMORY,         /**< Not enough memory */
    OGRERR_UNSUPPORTED_GEOMETRY_TYPE, /**< Unsupported geometry type */
    OGRERR_UNSUPPORTED_OPERATION,     /**< Unsupported operation */
    OGRERR_CORRUPT_DATA,              /**< Corrupt data */
    OGRERR_FAILURE,                   /**< Failure */
    OGRERR_UNSUPPORTED_SRS,           /**< Unsupported SRS */
    OGRERR_INVALID_HANDLE,            /**< Invalid handle */
    OGRERR_NON_EXISTING_FEATURE /**< Non existing feature. Added in GDAL 2.0 */
} OGRErr;

/** Data axis to CRS axis mapping strategy. */
typedef enum
{
    OAMS_TRADITIONAL_GIS_ORDER, /**< Traditional GIS order */
    OAMS_AUTHORITY_COMPLIANT, /**< Compliant with the order mandated by the CRS
                                 authority */
    OAMS_CUSTOM               /**< Custom */
} OSRAxisMappingStrategy;

/** Type for a OGR boolean */
typedef int OGRBoolean;

/** Type of a constant null-terminated list of nul terminated strings.
 * Seen as char** from C and const char* const* from C++ */
typedef const char* const* CSLConstList;

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
/*                          OGRSpatialReference                         */
/************************************************************************/
class CPL_DLL OGRSpatialReference
{
    struct Private;
    std::unique_ptr<Private> d;

    void GetNormInfo() const;

    pj_ctx* getPROJContext();
    
    // No longer used with PROJ >= 8.1.0
    OGRErr importFromURNPart(const char *pszAuthority, const char *pszCode,
                            const char *pszURN);

    static CPLString lookupInDict(const char* pszDictFile, const char* pszCode);

public:
    explicit OGRSpatialReference(pj_ctx *pjContext, const char * = nullptr);
    OGRSpatialReference(const OGRSpatialReference &);
    OGRSpatialReference(OGRSpatialReference &&);

    virtual ~OGRSpatialReference();

    static void DestroySpatialReference(OGRSpatialReference *poSRS);

    OGRSpatialReference &operator=(const OGRSpatialReference &);
    OGRSpatialReference &operator=(OGRSpatialReference &&);

    int Reference();
    int Dereference();
    int GetReferenceCount() const;
    void Release();

    const char *GetName() const;

    OGRSpatialReference* Clone() const;
    OGRSpatialReference* CloneGeogCS() const;

    OGRErr exportToWkt(char **) const;
    OGRErr exportToWkt(char **ppszWKT, const char *const *papszOptions) const;
    std::string exportToWkt(const char *const *papszOptions = nullptr) const;

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
    OGRErr importFromDict(const char* pszDict, const char* pszCode);

    OGRErr morphFromESRI();

    bool StripTOWGS84IfKnownDatumAndAllowed();
    bool StripTOWGS84IfKnownDatum();

    int GetAxesCount() const;
    const char* GetAxis(const char* pszTargetKey, int iAxis,
        OGRAxisOrientation* peOrientation,
        double* pdfConvFactor = nullptr) const;
    OGRErr SetAxes(const char *pszTargetKey, const char *pszXAxisName,
                OGRAxisOrientation eXAxisOrientation,
                const char *pszYAxisName,
                OGRAxisOrientation eYAxisOrientation);

    void SetAxisMappingStrategy(OSRAxisMappingStrategy);
    const std::vector<int>& GetDataAxisToSRSAxisMapping() const;
    OGRErr SetDataAxisToSRSAxisMapping(const std::vector<int>& mapping);

    // Machinery for accessing parse nodes

    //! Return root node
    OGR_SRSNode *GetRoot();
    //! Return root node
    const OGR_SRSNode *GetRoot() const;
    void SetRoot(OGR_SRSNode*);

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
    OGRErr SetWellKnownGeogCS(const char*);
    OGRErr CopyGeogCSFrom(const OGRSpatialReference* poSrcSRS);

    void SetCoordinateEpoch(double dfCoordinateEpoch);

    // cppcheck-suppress functionStatic
    OGRErr DemoteTo2D(const char *pszName);

    OGRErr SetFromUserInput(const char *);

    static const char *const SET_FROM_USER_INPUT_LIMITATIONS[];
    static CSLConstList SET_FROM_USER_INPUT_LIMITATIONS_get();

    OGRErr SetFromUserInput(const char *, CSLConstList papszOptions);

    OGRErr SetTOWGS84(double, double, double, double = 0.0, double = 0.0,
                    double = 0.0, double = 0.0);
    OGRErr GetTOWGS84(double* padfCoef, int nCoeff = 7) const;

    OGRErr SetAuthority(const char *pszTargetKey, const char *pszAuthority,
                    int nCode);

    OGRSpatialReferenceH *FindMatches(char **papszOptions, int *pnEntries,
                                      int **ppanMatchConfidence) const;
    OGRSpatialReference *
    FindBestMatch(int nMinimumMatchConfidence = 90,
                  const char *pszPreferredAuthority = "EPSG",
                  CSLConstList papszOptions = nullptr) const;

    const char* GetAuthorityCode(const char* pszTargetKey) const;
    const char* GetAuthorityName(const char* pszTargetKey) const;

    OGRErr SetExtension(const char *pszTargetKey, const char *pszName,
                    const char *pszValue);

    int FindProjParm(const char *pszParameter,
                     const OGR_SRSNode *poPROJCS = nullptr) const;
    OGRErr SetProjParm(const char*, double);
    double GetProjParm(const char*, double = 0.0, OGRErr* = nullptr) const;

    OGRErr SetNormProjParm(const char*, double);
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

    /*! @cond Doxygen_Suppress */
    void UpdateCoordinateSystemFromGeogCRS();
    /*! @endcond */

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

double CPL_DLL OSRCalcInvFlattening(double dfSemiMajor, double dfSemiMinor);

#endif /* ndef OGR_SPATIALREF_H_INCLUDED */
