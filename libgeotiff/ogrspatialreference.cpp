/******************************************************************************
 *
 * Project:  OpenGIS Simple Features Reference Implementation
 * Purpose:  The OGRSpatialReference class.
 * Author:   Frank Warmerdam, warmerdam@pobox.com
 *
 ******************************************************************************
 * Copyright (c) 1999,  Les Technologies SoftMap Inc.
 * Copyright (c) 2008-2018, Even Rouault <even.rouault at spatialys.com>
 *
 * SPDX-License-Identifier: MIT
 ****************************************************************************/

#include "cpl_port.h"
#include "ogr_spatialref.h"

#include <algorithm>
#include <atomic>
#include <string>
#include <mutex>
#include <set>
#include <vector>

#include "cpl_serv.h"
#include "cpl_conv.h"
#include "cpl_string.h"
#include "ogr_srs_api.h"
#include "geo_tiffp.h"

#include "proj.h"
#include "proj_experimental.h"
#include "proj_constants.h"

// Exists since 8.0.1
#ifndef PROJ_AT_LEAST_VERSION
#define PROJ_COMPUTE_VERSION(maj, min, patch)                                  \
    ((maj)*10000 + (min)*100 + (patch))
#define PROJ_VERSION_NUMBER                                                    \
    PROJ_COMPUTE_VERSION(PROJ_VERSION_MAJOR, PROJ_VERSION_MINOR,               \
                         PROJ_VERSION_PATCH)
#define PROJ_AT_LEAST_VERSION(maj, min, patch)                                 \
    (PROJ_VERSION_NUMBER >= PROJ_COMPUTE_VERSION(maj, min, patch))
#endif

#define STRINGIFY(s) #s
#define XSTRINGIFY(s) STRINGIFY(s)

struct OGRSpatialReference::Private
{
    struct Listener : public OGR_SRSNode::Listener
    {
        OGRSpatialReference::Private *m_poObj = nullptr;

        explicit Listener(OGRSpatialReference::Private *poObj) : m_poObj(poObj)
        {
        }

        Listener(const Listener &) = delete;
        Listener &operator=(const Listener &) = delete;

        void notifyChange(OGR_SRSNode *) override
        {
            m_poObj->nodesChanged();
        }
    };

    OGRSpatialReference *m_poSelf = nullptr;
    PJ *m_pj_crs = nullptr;

    // Temporary state used for object construction
    PJ_TYPE m_pjType = PJ_TYPE_UNKNOWN;
    CPLString m_osPrimeMeridianName{};
    CPLString m_osAngularUnits{};
    CPLString m_osLinearUnits{};
    CPLString m_osAxisName[3]{};

    std::vector<std::string> m_wktImportWarnings{};
    std::vector<std::string> m_wktImportErrors{};
    CPLString m_osAreaName{};

    bool m_bIsThreadSafe = false;
    bool m_bNodesChanged = false;
    bool m_bNodesWKT2 = false;
    OGR_SRSNode *m_poRoot = nullptr;

    double dfFromGreenwich = 0.0;
    double dfToMeter = 0.0;
    double dfToDegrees = 0.0;
    double m_dfAngularUnitToRadian = 0.0;

    std::atomic<int> nRefCount = 1;
    int bNormInfoSet = FALSE;

    PJ *m_pj_geod_base_crs_temp = nullptr;
    PJ *m_pj_proj_crs_cs_temp = nullptr;

    bool m_pj_crs_modified_during_demote = false;
    PJ *m_pj_bound_crs_target = nullptr;
    PJ *m_pj_bound_crs_co = nullptr;
    PJ *m_pj_crs_backup = nullptr;
    OGR_SRSNode *m_poRootBackup = nullptr;

    PJ_CONTEXT *m_pj_context = nullptr;

    bool m_bMorphToESRI = false;
    bool m_bHasCenterLong = false;

    std::shared_ptr<Listener> m_poListener{};

    std::recursive_mutex m_mutex{};

    OSRAxisMappingStrategy m_axisMappingStrategy = OAMS_AUTHORITY_COMPLIANT;
    std::vector<int> m_axisMapping{1, 2, 3};

    double m_coordinateEpoch = 0;  // as decimal year

    explicit Private(OGRSpatialReference *poSelf, PJ_CONTEXT* context);
    ~Private();
    Private(const Private &) = delete;
    Private &operator=(const Private &) = delete;

    void SetThreadSafe()
    {
        m_bIsThreadSafe = true;
    }

    void clear();
    void setPjCRS(PJ *pj_crsIn, bool doRefreshAxisMapping = true);
    void setRoot(OGR_SRSNode *poRoot);
    void refreshProjObj();
    void nodesChanged();
    void refreshRootFromProjObj();
    void invalidateNodes();

    void setMorphToESRI(bool b);

    PJ *getGeodBaseCRS();
    PJ *getProjCRSCoordSys();

    const char *getProjCRSName();
    OGRErr replaceConversionAndUnref(PJ *conv);

    void demoteFromBoundCRS();
    void undoDemoteFromBoundCRS();

    PJ_CONTEXT *getPROJContext()
    {
        return m_pj_context;
    }

    const char *nullifyTargetKeyIfPossible(const char *pszTargetKey);

    void refreshAxisMapping();

    // This structures enables locking during calls to OGRSpatialReference
    // public methods. Locking is only needed for instances of
    // OGRSpatialReference that have been asked to be thread-safe at
    // construction.
    // The lock is not just for a single call to OGRSpatialReference::Private,
    // but for the series of calls done by a OGRSpatialReference method.
    // We need a recursive mutex, because some OGRSpatialReference methods
    // may call other ones.
    struct OptionalLockGuard
    {
        Private &m_private;

        explicit OptionalLockGuard(Private *p) : m_private(*p)
        {
            if (m_private.m_bIsThreadSafe)
                m_private.m_mutex.lock();
        }

        ~OptionalLockGuard()
        {
            if (m_private.m_bIsThreadSafe)
                m_private.m_mutex.unlock();
        }
    };

    inline OptionalLockGuard GetOptionalLockGuard()
    {
        return OptionalLockGuard(this);
    }
};

#define TAKE_OPTIONAL_LOCK()                                                   \
    auto lock = d->GetOptionalLockGuard();                                     \
    CPL_IGNORE_RET_VAL(lock)


static OSRAxisMappingStrategy GetDefaultAxisMappingStrategy()
{
    const char *pszDefaultAMS =
        CPLGetConfigOption("OSR_DEFAULT_AXIS_MAPPING_STRATEGY", nullptr);
    if (pszDefaultAMS)
    {
        if (EQUAL(pszDefaultAMS, "AUTHORITY_COMPLIANT"))
            return OAMS_AUTHORITY_COMPLIANT;
        else if (EQUAL(pszDefaultAMS, "TRADITIONAL_GIS_ORDER"))
            return OAMS_TRADITIONAL_GIS_ORDER;
        else
        {
            CPLError(CE_Failure, CPLE_AppDefined,
                     "Illegal value for OSR_DEFAULT_AXIS_MAPPING_STRATEGY = %s",
                     pszDefaultAMS);
        }
    }
    return OAMS_AUTHORITY_COMPLIANT;
}

OGRSpatialReference::Private::Private(OGRSpatialReference *poSelf, PJ_CONTEXT* context)
    : m_poSelf(poSelf),
      m_poListener(std::shared_ptr<Listener>(new Listener(this))),
      m_pj_context(context)
{
    // Get the default value for m_axisMappingStrategy from the
    // OSR_DEFAULT_AXIS_MAPPING_STRATEGY configuration option, if set.
    m_axisMappingStrategy = GetDefaultAxisMappingStrategy();
}

static bool GDALThreadLocalDatasetCacheIsInDestruction()
{
    return false;
}

OGRSpatialReference::Private::~Private()
{
    // In case we destroy the object not in the thread that created it,
    // we need to reassign the PROJ context. Having the context bundled inside
    // PJ* deeply sucks...
    PJ_CONTEXT *pj_context_to_destroy = nullptr;
    PJ_CONTEXT *ctxt;
    if (GDALThreadLocalDatasetCacheIsInDestruction())
    {
        pj_context_to_destroy = proj_context_create();
        ctxt = pj_context_to_destroy;
    }
    else
    {
        ctxt = getPROJContext();
    }

    proj_assign_context(m_pj_crs, ctxt);
    proj_destroy(m_pj_crs);

    proj_assign_context(m_pj_geod_base_crs_temp, ctxt);
    proj_destroy(m_pj_geod_base_crs_temp);

    proj_assign_context(m_pj_proj_crs_cs_temp, ctxt);
    proj_destroy(m_pj_proj_crs_cs_temp);

    proj_assign_context(m_pj_bound_crs_target, ctxt);
    proj_destroy(m_pj_bound_crs_target);

    proj_assign_context(m_pj_bound_crs_co, ctxt);
    proj_destroy(m_pj_bound_crs_co);

    proj_assign_context(m_pj_crs_backup, ctxt);
    proj_destroy(m_pj_crs_backup);

    delete m_poRootBackup;
    delete m_poRoot;
    proj_context_destroy(pj_context_to_destroy);
}

void OGRSpatialReference::Private::clear()
{
    proj_assign_context(m_pj_crs, getPROJContext());
    proj_destroy(m_pj_crs);
    m_pj_crs = nullptr;

    delete m_poRoot;
    m_poRoot = nullptr;
    m_bNodesChanged = false;

    m_wktImportWarnings.clear();
    m_wktImportErrors.clear();

    m_pj_crs_modified_during_demote = false;
    m_pjType = PJ_TYPE_UNKNOWN;
    m_osPrimeMeridianName.clear();
    m_osAngularUnits.clear();
    m_osLinearUnits.clear();

    bNormInfoSet = FALSE;
    dfFromGreenwich = 1.0;
    dfToMeter = 1.0;
    dfToDegrees = 1.0;
    m_dfAngularUnitToRadian = 0.0;

    m_bMorphToESRI = false;
    m_bHasCenterLong = false;

    m_coordinateEpoch = 0.0;
}

void OGRSpatialReference::Private::setRoot(OGR_SRSNode *poRoot)
{
    m_poRoot = poRoot;
    if (m_poRoot)
    {
        m_poRoot->RegisterListener(m_poListener);
    }
    nodesChanged();
}

void OGRSpatialReference::Private::setPjCRS(PJ *pj_crsIn,
                                            bool doRefreshAxisMapping)
{
    auto ctxt = getPROJContext();

#if PROJ_AT_LEAST_VERSION(9, 2, 0)
    if (proj_get_type(pj_crsIn) == PJ_TYPE_COORDINATE_METADATA)
    {
        const double dfEpoch =
            proj_coordinate_metadata_get_epoch(ctxt, pj_crsIn);
        if (!std::isnan(dfEpoch))
        {
            m_poSelf->SetCoordinateEpoch(dfEpoch);
        }
        auto crs = proj_get_source_crs(ctxt, pj_crsIn);
        proj_destroy(pj_crsIn);
        pj_crsIn = crs;
    }
#endif

    proj_assign_context(m_pj_crs, ctxt);
    proj_destroy(m_pj_crs);
    m_pj_crs = pj_crsIn;
    if (m_pj_crs)
    {
        m_pjType = proj_get_type(m_pj_crs);
    }
    if (m_pj_crs_backup)
    {
        m_pj_crs_modified_during_demote = true;
    }
    invalidateNodes();
    if (doRefreshAxisMapping)
    {
        refreshAxisMapping();
    }
}

void OGRSpatialReference::Private::refreshProjObj()
{
    if (m_bNodesChanged && m_poRoot)
    {
        char *pszWKT = nullptr;
        m_poRoot->exportToWkt(&pszWKT);
        auto poRootBackup = m_poRoot;
        m_poRoot = nullptr;
        const double dfCoordinateEpochBackup = m_coordinateEpoch;
        clear();
        m_coordinateEpoch = dfCoordinateEpochBackup;
        m_bHasCenterLong = strstr(pszWKT, "CENTER_LONG") != nullptr;

        const char *const options[] = {
            "STRICT=NO",
#if PROJ_AT_LEAST_VERSION(9, 1, 0)
            "UNSET_IDENTIFIERS_IF_INCOMPATIBLE_DEF=NO",
#endif
            nullptr
        };
        PROJ_STRING_LIST warnings = nullptr;
        PROJ_STRING_LIST errors = nullptr;
        setPjCRS(proj_create_from_wkt(getPROJContext(), pszWKT, options,
                                      &warnings, &errors));
        for (auto iter = warnings; iter && *iter; ++iter)
        {
            m_wktImportWarnings.push_back(*iter);
        }
        for (auto iter = errors; iter && *iter; ++iter)
        {
            m_wktImportErrors.push_back(*iter);
        }
        proj_string_list_destroy(warnings);
        proj_string_list_destroy(errors);

        CPLFree(pszWKT);

        m_poRoot = poRootBackup;
        m_bNodesChanged = false;
    }
}

void OGRSpatialReference::Private::refreshRootFromProjObj()
{
    CPLAssert(m_poRoot == nullptr);

    if (m_pj_crs)
    {
        CPLStringList aosOptions;
        if (!m_bMorphToESRI)
        {
            aosOptions.SetNameValue("OUTPUT_AXIS", "YES");
            aosOptions.SetNameValue("MULTILINE", "NO");
        }
        aosOptions.SetNameValue("STRICT", "NO");

        const char *pszWKT;
        {
#ifdef MECHSOFT_DEVIATION
            CPLErrorStateBackuper oErrorStateBackuper(CPLQuietErrorHandler);
#endif
            pszWKT = proj_as_wkt(getPROJContext(), m_pj_crs,
                                 m_bMorphToESRI ? PJ_WKT1_ESRI : PJ_WKT1_GDAL,
                                 aosOptions.List());
            m_bNodesWKT2 = false;
        }
        if (!m_bMorphToESRI && pszWKT == nullptr)
        {
            pszWKT = proj_as_wkt(getPROJContext(), m_pj_crs, PJ_WKT2_2018,
                                 aosOptions.List());
            m_bNodesWKT2 = true;
        }
        if (pszWKT)
        {
            auto root = new OGR_SRSNode();
            setRoot(root);
            root->importFromWkt(&pszWKT);
            m_bNodesChanged = false;
        }
    }
}

static bool isNorthEastAxisOrder(PJ_CONTEXT *ctx, PJ *cs)
{
    const char *pszName1 = nullptr;
    const char *pszDirection1 = nullptr;
    proj_cs_get_axis_info(ctx, cs, 0, &pszName1, nullptr, &pszDirection1,
                          nullptr, nullptr, nullptr, nullptr);
    const char *pszName2 = nullptr;
    const char *pszDirection2 = nullptr;
    proj_cs_get_axis_info(ctx, cs, 1, &pszName2, nullptr, &pszDirection2,
                          nullptr, nullptr, nullptr, nullptr);
    if (pszDirection1 && EQUAL(pszDirection1, "north") && pszDirection2 &&
        EQUAL(pszDirection2, "east"))
    {
        return true;
    }
    if (pszDirection1 && pszDirection2 &&
        ((EQUAL(pszDirection1, "north") && EQUAL(pszDirection2, "north")) ||
         (EQUAL(pszDirection1, "south") && EQUAL(pszDirection2, "south"))) &&
        pszName1 && STARTS_WITH_CI(pszName1, "northing") && pszName2 &&
        STARTS_WITH_CI(pszName2, "easting"))
    {
        return true;
    }
    return false;
}

void OGRSpatialReference::Private::refreshAxisMapping()
{
    if (!m_pj_crs || m_axisMappingStrategy == OAMS_CUSTOM)
        return;

    bool doUndoDemote = false;
    if (m_pj_crs_backup == nullptr)
    {
        doUndoDemote = true;
        demoteFromBoundCRS();
    }
    const auto ctxt = getPROJContext();
    PJ *horizCRS = nullptr;
    int axisCount = 0;
    if (m_pjType == PJ_TYPE_VERTICAL_CRS)
    {
        axisCount = 1;
    }
    else if (m_pjType == PJ_TYPE_COMPOUND_CRS)
    {
        horizCRS = proj_crs_get_sub_crs(ctxt, m_pj_crs, 0);
        if (horizCRS && proj_get_type(horizCRS) == PJ_TYPE_BOUND_CRS)
        {
            auto baseCRS = proj_get_source_crs(ctxt, horizCRS);
            if (baseCRS)
            {
                proj_destroy(horizCRS);
                horizCRS = baseCRS;
            }
        }

        auto vertCRS = proj_crs_get_sub_crs(ctxt, m_pj_crs, 1);
        if (vertCRS)
        {
            if (proj_get_type(vertCRS) == PJ_TYPE_BOUND_CRS)
            {
                auto baseCRS = proj_get_source_crs(ctxt, vertCRS);
                if (baseCRS)
                {
                    proj_destroy(vertCRS);
                    vertCRS = baseCRS;
                }
            }

            auto cs = proj_crs_get_coordinate_system(ctxt, vertCRS);
            if (cs)
            {
                axisCount += proj_cs_get_axis_count(ctxt, cs);
                proj_destroy(cs);
            }
            proj_destroy(vertCRS);
        }
    }
    else
    {
        horizCRS = m_pj_crs;
    }

    bool bSwitchForGisFriendlyOrder = false;
    if (horizCRS)
    {
        auto cs = proj_crs_get_coordinate_system(ctxt, horizCRS);
        if (cs)
        {
            int nHorizCSAxisCount = proj_cs_get_axis_count(ctxt, cs);
            axisCount += nHorizCSAxisCount;
            if (nHorizCSAxisCount >= 2)
            {
                bSwitchForGisFriendlyOrder = isNorthEastAxisOrder(ctxt, cs);
            }
            proj_destroy(cs);
        }
    }
    if (horizCRS != m_pj_crs)
    {
        proj_destroy(horizCRS);
    }
    if (doUndoDemote)
    {
        undoDemoteFromBoundCRS();
    }

    m_axisMapping.resize(axisCount);
    if (m_axisMappingStrategy == OAMS_AUTHORITY_COMPLIANT ||
        !bSwitchForGisFriendlyOrder)
    {
        for (int i = 0; i < axisCount; i++)
        {
            m_axisMapping[i] = i + 1;
        }
    }
    else
    {
        m_axisMapping[0] = 2;
        m_axisMapping[1] = 1;
        if (axisCount == 3)
        {
            m_axisMapping[2] = 3;
        }
    }
}

void OGRSpatialReference::Private::nodesChanged()
{
    m_bNodesChanged = true;
}

void OGRSpatialReference::Private::invalidateNodes()
{
    delete m_poRoot;
    m_poRoot = nullptr;
    m_bNodesChanged = false;
}

void OGRSpatialReference::Private::setMorphToESRI(bool b)
{
    invalidateNodes();
    m_bMorphToESRI = b;
}

void OGRSpatialReference::Private::demoteFromBoundCRS()
{
    CPLAssert(m_pj_bound_crs_target == nullptr);
    CPLAssert(m_pj_bound_crs_co == nullptr);
    CPLAssert(m_poRootBackup == nullptr);
    CPLAssert(m_pj_crs_backup == nullptr);

    m_pj_crs_modified_during_demote = false;

    if (m_pjType == PJ_TYPE_BOUND_CRS)
    {
        auto baseCRS = proj_get_source_crs(getPROJContext(), m_pj_crs);
        m_pj_bound_crs_target = proj_get_target_crs(getPROJContext(), m_pj_crs);
        m_pj_bound_crs_co =
            proj_crs_get_coordoperation(getPROJContext(), m_pj_crs);

        m_poRootBackup = m_poRoot;
        m_poRoot = nullptr;
        m_pj_crs_backup = m_pj_crs;
        m_pj_crs = baseCRS;
        m_pjType = proj_get_type(m_pj_crs);
    }
}

void OGRSpatialReference::Private::undoDemoteFromBoundCRS()
{
    if (m_pj_bound_crs_target)
    {
        CPLAssert(m_poRoot == nullptr);
        CPLAssert(m_pj_crs);
        if (!m_pj_crs_modified_during_demote)
        {
            proj_destroy(m_pj_crs);
            m_pj_crs = m_pj_crs_backup;
            m_pjType = proj_get_type(m_pj_crs);
            m_poRoot = m_poRootBackup;
        }
        else
        {
            delete m_poRootBackup;
            m_poRootBackup = nullptr;
            proj_destroy(m_pj_crs_backup);
            m_pj_crs_backup = nullptr;
            setPjCRS(proj_crs_create_bound_crs(getPROJContext(), m_pj_crs,
                                               m_pj_bound_crs_target,
                                               m_pj_bound_crs_co),
                     false);
        }
    }

    m_poRootBackup = nullptr;
    m_pj_crs_backup = nullptr;
    proj_destroy(m_pj_bound_crs_target);
    m_pj_bound_crs_target = nullptr;
    proj_destroy(m_pj_bound_crs_co);
    m_pj_bound_crs_co = nullptr;
    m_pj_crs_modified_during_demote = false;
}

const char *OGRSpatialReference::Private::nullifyTargetKeyIfPossible(
    const char *pszTargetKey)
{
    if (pszTargetKey)
    {
        demoteFromBoundCRS();
        if ((m_pjType == PJ_TYPE_GEOGRAPHIC_2D_CRS ||
             m_pjType == PJ_TYPE_GEOGRAPHIC_3D_CRS) &&
            EQUAL(pszTargetKey, "GEOGCS"))
        {
            pszTargetKey = nullptr;
        }
        else if (m_pjType == PJ_TYPE_GEOCENTRIC_CRS &&
                 EQUAL(pszTargetKey, "GEOCCS"))
        {
            pszTargetKey = nullptr;
        }
        else if (m_pjType == PJ_TYPE_PROJECTED_CRS &&
                 EQUAL(pszTargetKey, "PROJCS"))
        {
            pszTargetKey = nullptr;
        }
        else if (m_pjType == PJ_TYPE_VERTICAL_CRS &&
                 EQUAL(pszTargetKey, "VERT_CS"))
        {
            pszTargetKey = nullptr;
        }
        undoDemoteFromBoundCRS();
    }
    return pszTargetKey;
}

PJ *OGRSpatialReference::Private::getGeodBaseCRS()
{
    if (m_pjType == PJ_TYPE_GEOGRAPHIC_2D_CRS ||
        m_pjType == PJ_TYPE_GEOGRAPHIC_3D_CRS)
    {
        return m_pj_crs;
    }

    auto ctxt = getPROJContext();
    if (m_pjType == PJ_TYPE_PROJECTED_CRS)
    {
        proj_assign_context(m_pj_geod_base_crs_temp, ctxt);
        proj_destroy(m_pj_geod_base_crs_temp);
        m_pj_geod_base_crs_temp = proj_crs_get_geodetic_crs(ctxt, m_pj_crs);
        return m_pj_geod_base_crs_temp;
    }

    proj_assign_context(m_pj_geod_base_crs_temp, ctxt);
    proj_destroy(m_pj_geod_base_crs_temp);
    auto cs = proj_create_ellipsoidal_2D_cs(ctxt, PJ_ELLPS2D_LATITUDE_LONGITUDE,
                                            nullptr, 0);
    m_pj_geod_base_crs_temp = proj_create_geographic_crs(
        ctxt, "WGS 84", "World Geodetic System 1984", "WGS 84",
        SRS_WGS84_SEMIMAJOR, SRS_WGS84_INVFLATTENING, SRS_PM_GREENWICH, 0.0,
        SRS_UA_DEGREE, CPLAtof(SRS_UA_DEGREE_CONV), cs);
    proj_destroy(cs);

    return m_pj_geod_base_crs_temp;
}

PJ *OGRSpatialReference::Private::getProjCRSCoordSys()
{
    auto ctxt = getPROJContext();
    if (m_pjType == PJ_TYPE_PROJECTED_CRS)
    {
        proj_assign_context(m_pj_proj_crs_cs_temp, ctxt);
        proj_destroy(m_pj_proj_crs_cs_temp);
        m_pj_proj_crs_cs_temp =
            proj_crs_get_coordinate_system(getPROJContext(), m_pj_crs);
        return m_pj_proj_crs_cs_temp;
    }

    proj_assign_context(m_pj_proj_crs_cs_temp, ctxt);
    proj_destroy(m_pj_proj_crs_cs_temp);
    m_pj_proj_crs_cs_temp = proj_create_cartesian_2D_cs(
        ctxt, PJ_CART2D_EASTING_NORTHING, nullptr, 0);
    return m_pj_proj_crs_cs_temp;
}

const char *OGRSpatialReference::Private::getProjCRSName()
{
    if (m_pjType == PJ_TYPE_PROJECTED_CRS)
    {
        return proj_get_name(m_pj_crs);
    }

    return "unnamed";
}

OGRErr OGRSpatialReference::Private::replaceConversionAndUnref(PJ *conv)
{
    refreshProjObj();

    demoteFromBoundCRS();

    auto projCRS =
        proj_create_projected_crs(getPROJContext(), getProjCRSName(),
                                  getGeodBaseCRS(), conv, getProjCRSCoordSys());
    proj_destroy(conv);

    setPjCRS(projCRS);

    undoDemoteFromBoundCRS();
    return OGRERR_NONE;
}

/************************************************************************/
/*                           ToPointer()                                */
/************************************************************************/

static inline OGRSpatialReference *ToPointer(OGRSpatialReferenceH hSRS)
{
    return OGRSpatialReference::FromHandle(hSRS);
}

/************************************************************************/
/*                           ToHandle()                                 */
/************************************************************************/

static inline OGRSpatialReferenceH ToHandle(OGRSpatialReference *poSRS)
{
    return OGRSpatialReference::ToHandle(poSRS);
}

/************************************************************************/
/*                           OGRsnPrintDouble()                         */
/************************************************************************/

void OGRsnPrintDouble(char *pszStrBuf, size_t size, double dfValue);

void OGRsnPrintDouble(char *pszStrBuf, size_t size, double dfValue)

{
    CPLsnprintf(pszStrBuf, size, "%.16g", dfValue);

    const size_t nLen = strlen(pszStrBuf);

    // The following hack is intended to truncate some "precision" in cases
    // that appear to be roundoff error.
    if (nLen > 15 && (strcmp(pszStrBuf + nLen - 6, "999999") == 0 ||
                      strcmp(pszStrBuf + nLen - 6, "000001") == 0))
    {
        CPLsnprintf(pszStrBuf, size, "%.15g", dfValue);
    }

    // Force to user periods regardless of locale.
    if (strchr(pszStrBuf, ',') != nullptr)
    {
        char *const pszDelim = strchr(pszStrBuf, ',');
        *pszDelim = '.';
    }
}

/************************************************************************/
/*                        OGRSpatialReference()                         */
/************************************************************************/

/**
 * \brief Constructor.
 *
 * This constructor takes an optional string argument which if passed
 * should be a WKT representation of an SRS.  Passing this is equivalent
 * to not passing it, and then calling importFromWkt() with the WKT string.
 *
 * Note that newly created objects are given a reference count of one.
 *
 * Starting with GDAL 3.0, coordinates associated with a OGRSpatialReference
 * object are assumed to be in the order of the axis of the CRS definition
 (which
 * for example means latitude first, longitude second for geographic CRS
 belonging
 * to the EPSG authority). It is possible to define a data axis to CRS axis
 * mapping strategy with the SetAxisMappingStrategy() method.
 *
 * Starting with GDAL 3.5, the OSR_DEFAULT_AXIS_MAPPING_STRATEGY configuration
 * option can be set to "TRADITIONAL_GIS_ORDER" / "AUTHORITY_COMPLIANT" (the
 later
 * being the default value when the option is not set) to control the value of
 the
 * data axis to CRS axis mapping strategy when a OSRSpatialReference object is
 * created. Calling SetAxisMappingStrategy() will override this default value.

 * The C function OSRNewSpatialReference() does the same thing as this
 * constructor.
 *
 * @param pszWKT well known text definition to which the object should
 * be initialized, or NULL (the default).
 */

OGRSpatialReference::OGRSpatialReference(PJ_CONTEXT *pjContext, const char *pszWKT)
    : d(new Private(this, pjContext))
{
    if (pszWKT != nullptr)
        importFromWkt(pszWKT);
}

/************************************************************************/
/*                        OGRSpatialReference()                         */
/************************************************************************/

/** Copy constructor. See also Clone().
 * @param oOther other spatial reference
 */
OGRSpatialReference::OGRSpatialReference(const OGRSpatialReference &oOther)
    : d(new Private(this, oOther.d->getPROJContext()))
{
    *this = oOther;
}

/************************************************************************/
/*                        OGRSpatialReference()                         */
/************************************************************************/

/** Move constructor.
 * @param oOther other spatial reference
 */
OGRSpatialReference::OGRSpatialReference(OGRSpatialReference &&oOther)
    : d(std::move(oOther.d))
{
}

/************************************************************************/
/*                        ~OGRSpatialReference()                        */
/************************************************************************/

/**
 * \brief OGRSpatialReference destructor.
 *
 * The C function OSRDestroySpatialReference() does the same thing as this
 * method. Preferred C++ method : OGRSpatialReference::DestroySpatialReference()
 *
 * @deprecated
 */

OGRSpatialReference::~OGRSpatialReference()

{
}

/************************************************************************/
/*                      DestroySpatialReference()                       */
/************************************************************************/

/**
 * \brief OGRSpatialReference destructor.
 *
 * This static method will destroy a OGRSpatialReference.  It is
 * equivalent to calling delete on the object, but it ensures that the
 * deallocation is properly executed within the OGR libraries heap on
 * platforms where this can matter (win32).
 *
 * This function is the same as OSRDestroySpatialReference()
 *
 * @param poSRS the object to delete
 *
 * @since GDAL 1.7.0
 */

void OGRSpatialReference::DestroySpatialReference(OGRSpatialReference *poSRS)
{
    delete poSRS;
}

/************************************************************************/
/*                               Clear()                                */
/************************************************************************/

/**
 * \brief Wipe current definition.
 *
 * Returns OGRSpatialReference to a state with no definition, as it
 * exists when first created.  It does not affect reference counts.
 */

void OGRSpatialReference::Clear()

{
    d->clear();
}

/************************************************************************/
/*                             operator=()                              */
/************************************************************************/

/** Assignment operator.
 * @param oSource SRS to assign to *this
 * @return *this
 */
OGRSpatialReference &
OGRSpatialReference::operator=(const OGRSpatialReference &oSource)

{
    if (&oSource != this)
    {
        Clear();
#ifdef CPPCHECK
        // Otherwise cppcheck would protest that nRefCount isn't modified
        d->nRefCount = (d->nRefCount + 1) - 1;
#endif

        oSource.d->refreshProjObj();
        if (oSource.d->m_pj_crs)
            d->setPjCRS(proj_clone(d->getPROJContext(), oSource.d->m_pj_crs));
        if (oSource.d->m_axisMappingStrategy == OAMS_TRADITIONAL_GIS_ORDER)
            SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
        else if (oSource.d->m_axisMappingStrategy == OAMS_CUSTOM)
            SetDataAxisToSRSAxisMapping(oSource.d->m_axisMapping);

        d->m_coordinateEpoch = oSource.d->m_coordinateEpoch;
    }

    return *this;
}

/************************************************************************/
/*                             operator=()                              */
/************************************************************************/

/** Move assignment operator.
 * @param oSource SRS to assign to *this
 * @return *this
 */
OGRSpatialReference &
OGRSpatialReference::operator=(OGRSpatialReference &&oSource)

{
    if (&oSource != this)
    {
        d = std::move(oSource.d);
    }

    return *this;
}

/************************************************************************/
/*                             Reference()                              */
/************************************************************************/

/**
 * \brief Increments the reference count by one.
 *
 * The reference count is used keep track of the number of OGRGeometry objects
 * referencing this SRS.
 *
 * The method does the same thing as the C function OSRReference().
 *
 * @return the updated reference count.
 */

int OGRSpatialReference::Reference()

{
    return ++d->nRefCount;
}

/************************************************************************/
/*                            OSRReference()                            */
/************************************************************************/

/**
 * \brief Increments the reference count by one.
 *
 * This function is the same as OGRSpatialReference::Reference()
 */
int OSRReference(OGRSpatialReferenceH hSRS)

{
    VALIDATE_POINTER1(hSRS, "OSRReference", 0);

    return ToPointer(hSRS)->Reference();
}

/************************************************************************/
/*                            Dereference()                             */
/************************************************************************/

/**
 * \brief Decrements the reference count by one.
 *
 * The method does the same thing as the C function OSRDereference().
 *
 * @return the updated reference count.
 */

int OGRSpatialReference::Dereference()

{
    if (d->nRefCount <= 0)
        CPLDebug("OSR",
                 "Dereference() called on an object with refcount %d,"
                 "likely already destroyed!",
                 d->nRefCount.load());
    return --d->nRefCount;
}

/************************************************************************/
/*                           OSRDereference()                           */
/************************************************************************/

/**
 * \brief Decrements the reference count by one.
 *
 * This function is the same as OGRSpatialReference::Dereference()
 */
int OSRDereference(OGRSpatialReferenceH hSRS)

{
    VALIDATE_POINTER1(hSRS, "OSRDereference", 0);

    return ToPointer(hSRS)->Dereference();
}

/************************************************************************/
/*                         GetReferenceCount()                          */
/************************************************************************/

/**
 * \brief Fetch current reference count.
 *
 * @return the current reference count.
 */
int OGRSpatialReference::GetReferenceCount() const
{
    return d->nRefCount;
}

/************************************************************************/
/*                              Release()                               */
/************************************************************************/

/**
 * \brief Decrements the reference count by one, and destroy if zero.
 *
 * The method does the same thing as the C function OSRRelease().
 */

void OGRSpatialReference::Release()

{
    if (Dereference() <= 0)
        delete this;
}

/************************************************************************/
/*                             OSRRelease()                             */
/************************************************************************/

/**
 * \brief Decrements the reference count by one, and destroy if zero.
 *
 * This function is the same as OGRSpatialReference::Release()
 */
void OSRRelease(OGRSpatialReferenceH hSRS)

{
    VALIDATE_POINTER0(hSRS, "OSRRelease");

    ToPointer(hSRS)->Release();
}

/************************************************************************/
/*                               GetRoot()                              */
/************************************************************************/

OGR_SRSNode *OGRSpatialReference::GetRoot()
{
    TAKE_OPTIONAL_LOCK();

    if (!d->m_poRoot)
    {
        d->refreshRootFromProjObj();
    }
    return d->m_poRoot;
}

const OGR_SRSNode *OGRSpatialReference::GetRoot() const
{
    TAKE_OPTIONAL_LOCK();

    if (!d->m_poRoot)
    {
        d->refreshRootFromProjObj();
    }
    return d->m_poRoot;
}

/************************************************************************/
/*                              SetRoot()                               */
/************************************************************************/

/**
 * \brief Set the root SRS node.
 *
 * If the object has an existing tree of OGR_SRSNodes, they are destroyed
 * as part of assigning the new root.  Ownership of the passed OGR_SRSNode is
 * is assumed by the OGRSpatialReference.
 *
 * @param poNewRoot object to assign as root.
 */

void OGRSpatialReference::SetRoot(OGR_SRSNode *poNewRoot)

{
    if (d->m_poRoot != poNewRoot)
    {
        delete d->m_poRoot;
        d->setRoot(poNewRoot);
    }
}

/************************************************************************/
/*                            GetAttrNode()                             */
/************************************************************************/

/**
 * \brief Find named node in tree.
 *
 * This method does a pre-order traversal of the node tree searching for
 * a node with this exact value (case insensitive), and returns it.  Leaf
 * nodes are not considered, under the assumption that they are just
 * attribute value nodes.
 *
 * If a node appears more than once in the tree (such as UNIT for instance),
 * the first encountered will be returned.  Use GetNode() on a subtree to be
 * more specific.
 *
 * @param pszNodePath the name of the node to search for.  May contain multiple
 * components such as "GEOGCS|UNIT".
 *
 * @return a pointer to the node found, or NULL if none.
 */

OGR_SRSNode *OGRSpatialReference::GetAttrNode(const char *pszNodePath)

{
    if (strchr(pszNodePath, '|') == nullptr)
    {
        // Fast path
        OGR_SRSNode *poNode = GetRoot();
        if (poNode)
            poNode = poNode->GetNode(pszNodePath);
        return poNode;
    }

    char **papszPathTokens =
        CSLTokenizeStringComplex(pszNodePath, "|", TRUE, FALSE);

    if (CSLCount(papszPathTokens) < 1)
    {
        CSLDestroy(papszPathTokens);
        return nullptr;
    }

    OGR_SRSNode *poNode = GetRoot();
    for (int i = 0; poNode != nullptr && papszPathTokens[i] != nullptr; i++)
    {
        poNode = poNode->GetNode(papszPathTokens[i]);
    }

    CSLDestroy(papszPathTokens);

    return poNode;
}

/**
 * \brief Find named node in tree.
 *
 * This method does a pre-order traversal of the node tree searching for
 * a node with this exact value (case insensitive), and returns it.  Leaf
 * nodes are not considered, under the assumption that they are just
 * attribute value nodes.
 *
 * If a node appears more than once in the tree (such as UNIT for instance),
 * the first encountered will be returned.  Use GetNode() on a subtree to be
 * more specific.
 *
 * @param pszNodePath the name of the node to search for.  May contain multiple
 * components such as "GEOGCS|UNIT".
 *
 * @return a pointer to the node found, or NULL if none.
 */

const OGR_SRSNode *
OGRSpatialReference::GetAttrNode(const char *pszNodePath) const

{
    OGR_SRSNode *poNode =
        const_cast<OGRSpatialReference *>(this)->GetAttrNode(pszNodePath);

    return poNode;
}

/************************************************************************/
/*                            GetAttrValue()                            */
/************************************************************************/

/**
 * \brief Fetch indicated attribute of named node.
 *
 * This method uses GetAttrNode() to find the named node, and then extracts
 * the value of the indicated child.  Thus a call to GetAttrValue("UNIT",1)
 * would return the second child of the UNIT node, which is normally the
 * length of the linear unit in meters.
 *
 * This method does the same thing as the C function OSRGetAttrValue().
 *
 * @param pszNodeName the tree node to look for (case insensitive).
 * @param iAttr the child of the node to fetch (zero based).
 *
 * @return the requested value, or NULL if it fails for any reason.
 */

const char *OGRSpatialReference::GetAttrValue(const char *pszNodeName,
                                              int iAttr) const

{
    const OGR_SRSNode *poNode = GetAttrNode(pszNodeName);
    if (poNode == nullptr)
    {
        if (d->m_bNodesWKT2 && EQUAL(pszNodeName, "PROJECTION"))
        {
            return GetAttrValue("METHOD", iAttr);
        }
        else if (d->m_bNodesWKT2 && EQUAL(pszNodeName, "PROJCS|PROJECTION"))
        {
            return GetAttrValue("PROJCRS|METHOD", iAttr);
        }
        else if (d->m_bNodesWKT2 && EQUAL(pszNodeName, "PROJCS"))
        {
            return GetAttrValue("PROJCRS", iAttr);
        }
        return nullptr;
    }

    if (iAttr < 0 || iAttr >= poNode->GetChildCount())
        return nullptr;

    return poNode->GetChild(iAttr)->GetValue();
}

/************************************************************************/
/*                             GetName()                                */
/************************************************************************/

/**
 * \brief Return the CRS name.
 *
 * The returned value is only short lived and should not be used after other
 * calls to methods on this object.
 *
 * @since GDAL 3.0
 */

const char *OGRSpatialReference::GetName() const
{
    TAKE_OPTIONAL_LOCK();

    d->refreshProjObj();
    if (!d->m_pj_crs)
        return nullptr;
    const char *pszName = proj_get_name(d->m_pj_crs);
#if PROJ_VERSION_NUMBER == PROJ_COMPUTE_VERSION(8, 2, 0)
    if (d->m_pjType == PJ_TYPE_BOUND_CRS && EQUAL(pszName, "SOURCECRS"))
    {
        // Work around a bug of PROJ 8.2.0 (fixed in 8.2.1)
        PJ *baseCRS = proj_get_source_crs(d->getPROJContext(), d->m_pj_crs);
        if (baseCRS)
        {
            pszName = proj_get_name(baseCRS);
            // pszName still remains valid after proj_destroy(), since
            // d->m_pj_crs keeps a reference to the base CRS C++ object.
            proj_destroy(baseCRS);
        }
    }
#endif
    return pszName;
}

/************************************************************************/
/*                               Clone()                                */
/************************************************************************/

/**
 * \brief Make a duplicate of this OGRSpatialReference.
 *
 * This method is the same as the C function OSRClone().
 *
 * @return a new SRS, which becomes the responsibility of the caller.
 */

OGRSpatialReference *OGRSpatialReference::Clone() const

{
    OGRSpatialReference *poNewRef = new OGRSpatialReference(d->getPROJContext());

    TAKE_OPTIONAL_LOCK();

    d->refreshProjObj();
    if (d->m_pj_crs != nullptr)
        poNewRef->d->setPjCRS(proj_clone(d->getPROJContext(), d->m_pj_crs));
    if (d->m_bHasCenterLong && d->m_poRoot)
    {
        poNewRef->d->setRoot(d->m_poRoot->Clone());
    }
    poNewRef->d->m_axisMapping = d->m_axisMapping;
    poNewRef->d->m_axisMappingStrategy = d->m_axisMappingStrategy;
    poNewRef->d->m_coordinateEpoch = d->m_coordinateEpoch;
    return poNewRef;
}

/************************************************************************/
/*                            exportToWkt()                             */
/************************************************************************/

/**
 * \brief Convert this SRS into WKT 1 format.
 *
 * Consult also the <a href="wktproblems.html">OGC WKT Coordinate System
 * Issues</a> page for implementation details of WKT 1 in OGR.
 *
 * Note that the returned WKT string should be freed with
 * CPLFree() when no longer needed.  It is the responsibility of the caller.
 *
 * The WKT version can be overridden by using the OSR_WKT_FORMAT configuration
 * option. Valid values are the one of the FORMAT option of
 * exportToWkt( char ** ppszResult, const char* const* papszOptions ) const
 *
 * This method is the same as the C function OSRExportToWkt().
 *
 * @param ppszResult the resulting string is returned in this pointer.
 *
 * @return OGRERR_NONE if successful.
 */

OGRErr OGRSpatialReference::exportToWkt(char **ppszResult) const

{
    return exportToWkt(ppszResult, nullptr);
}

/************************************************************************/
/*                GDAL_proj_crs_create_bound_crs_to_WGS84()             */
/************************************************************************/

static PJ *GDAL_proj_crs_create_bound_crs_to_WGS84(PJ_CONTEXT *ctx, PJ *pj,
                                                   bool onlyIfEPSGCode,
                                                   bool canModifyHorizPart)
{
    PJ *ret = nullptr;
    if (proj_get_type(pj) == PJ_TYPE_COMPOUND_CRS)
    {
        auto horizCRS = proj_crs_get_sub_crs(ctx, pj, 0);
        auto vertCRS = proj_crs_get_sub_crs(ctx, pj, 1);
        if (horizCRS && proj_get_type(horizCRS) != PJ_TYPE_BOUND_CRS &&
            vertCRS &&
            (!onlyIfEPSGCode || proj_get_id_auth_name(horizCRS, 0) != nullptr))
        {
            auto boundHoriz =
                canModifyHorizPart
                    ? proj_crs_create_bound_crs_to_WGS84(ctx, horizCRS, nullptr)
                    : proj_clone(ctx, horizCRS);
            auto boundVert =
                proj_crs_create_bound_crs_to_WGS84(ctx, vertCRS, nullptr);
            if (boundHoriz && boundVert)
            {
                ret = proj_create_compound_crs(ctx, proj_get_name(pj),
                                               boundHoriz, boundVert);
            }
            proj_destroy(boundHoriz);
            proj_destroy(boundVert);
        }
        proj_destroy(horizCRS);
        proj_destroy(vertCRS);
    }
    else if (proj_get_type(pj) != PJ_TYPE_BOUND_CRS &&
             (!onlyIfEPSGCode || proj_get_id_auth_name(pj, 0) != nullptr))
    {
        ret = proj_crs_create_bound_crs_to_WGS84(ctx, pj, nullptr);
    }
    return ret;
}

/************************************************************************/
/*                            exportToWkt()                             */
/************************************************************************/

/**
 * Convert this SRS into a WKT string.
 *
 * Note that the returned WKT string should be freed with
 * CPLFree() when no longer needed.  It is the responsibility of the caller.
 *
 * Consult also the <a href="wktproblems.html">OGC WKT Coordinate System
 * Issues</a> page for implementation details of WKT 1 in OGR.
 *
 * @param ppszResult the resulting string is returned in this pointer.
 * @param papszOptions NULL terminated list of options, or NULL. Currently
 * supported options are
 * <ul>
 * <li>MULTILINE=YES/NO. Defaults to NO.</li>
 * <li>FORMAT=SFSQL/WKT1_SIMPLE/WKT1/WKT1_GDAL/WKT1_ESRI/WKT2_2015/WKT2_2018/WKT2/DEFAULT.
 *     If SFSQL, a WKT1 string without AXIS, TOWGS84, AUTHORITY or EXTENSION
 *     node is returned.
 *     If WKT1_SIMPLE, a WKT1 string without AXIS, AUTHORITY or EXTENSION
 *     node is returned.
 *     WKT1 is an alias of WKT1_GDAL.
 *     WKT2 will default to the latest revision implemented (currently
 *     WKT2_2018) WKT2_2019 can be used as an alias of WKT2_2018 since GDAL 3.2
 * </li>
 * <li>ALLOW_ELLIPSOIDAL_HEIGHT_AS_VERTICAL_CRS=YES/NO. Default is NO. If set
 * to YES and FORMAT=WKT1_GDAL, a Geographic 3D CRS or a Projected 3D CRS will
 * be exported as a compound CRS whose vertical part represents an ellipsoidal
 * height (for example for use with LAS 1.4 WKT1).
 * Requires PROJ 7.2.1 and GDAL 3.2.1.</li>
 * </ul>
 *
 * Starting with GDAL 3.0.3, if the OSR_ADD_TOWGS84_ON_EXPORT_TO_WKT1
 * configuration option is set to YES, when exporting to WKT1_GDAL, this method
 * will try to add a TOWGS84[] node, if there's none attached yet to the SRS and
 * if the SRS has a EPSG code. See the AddGuessedTOWGS84() method for how this
 * TOWGS84[] node may be added.
 *
 * @return OGRERR_NONE if successful.
 * @since GDAL 3.0
 */

OGRErr OGRSpatialReference::exportToWkt(char **ppszResult,
                                        const char *const *papszOptions) const
{
    // In the past calling this method was thread-safe, even if we never
    // guaranteed it. Now proj_as_wkt() will cache the result internally,
    // so this is no longer thread-safe.
    std::lock_guard oLock(d->m_mutex);

    d->refreshProjObj();
    if (!d->m_pj_crs)
    {
        *ppszResult = CPLStrdup("");
        return OGRERR_FAILURE;
    }

    if (d->m_bHasCenterLong && d->m_poRoot && !d->m_bMorphToESRI)
    {
        return d->m_poRoot->exportToWkt(ppszResult);
    }

    auto ctxt = d->getPROJContext();
    auto wktFormat = PJ_WKT1_GDAL;
    const char *pszFormat =
        CSLFetchNameValueDef(papszOptions, "FORMAT",
                             CPLGetConfigOption("OSR_WKT_FORMAT", "DEFAULT"));
    if (EQUAL(pszFormat, "DEFAULT"))
        pszFormat = "";

    if (EQUAL(pszFormat, "WKT1_ESRI") || d->m_bMorphToESRI)
    {
        wktFormat = PJ_WKT1_ESRI;
    }
    else if (EQUAL(pszFormat, "WKT1") || EQUAL(pszFormat, "WKT1_GDAL") ||
             EQUAL(pszFormat, "WKT1_SIMPLE") || EQUAL(pszFormat, "SFSQL"))
    {
        wktFormat = PJ_WKT1_GDAL;
    }
    else if (EQUAL(pszFormat, "WKT2_2015"))
    {
        wktFormat = PJ_WKT2_2015;
    }
    else if (EQUAL(pszFormat, "WKT2") || EQUAL(pszFormat, "WKT2_2018") ||
             EQUAL(pszFormat, "WKT2_2019"))
    {
        wktFormat = PJ_WKT2_2018;
    }
    else if (pszFormat[0] == '\0')
    {
        // cppcheck-suppress knownConditionTrueFalse
        if (IsDerivedGeographic())
        {
            wktFormat = PJ_WKT2_2018;
        }
        else if ((IsGeographic() || IsProjected()) && !IsCompound() &&
                 GetAxesCount() == 3)
        {
            wktFormat = PJ_WKT2_2018;
        }
    }
    else
    {
        CPLError(CE_Failure, CPLE_AppDefined, "Unsupported value for FORMAT");
        *ppszResult = CPLStrdup("");
        return OGRERR_FAILURE;
    }

    CPLStringList aosOptions;
    if (wktFormat != PJ_WKT1_ESRI)
    {
        aosOptions.SetNameValue("OUTPUT_AXIS", "YES");
    }
    aosOptions.SetNameValue(
        "MULTILINE", CSLFetchNameValueDef(papszOptions, "MULTILINE", "NO"));

    const char *pszAllowEllpsHeightAsVertCS = CSLFetchNameValue(
        papszOptions, "ALLOW_ELLIPSOIDAL_HEIGHT_AS_VERTICAL_CRS");
    if (pszAllowEllpsHeightAsVertCS)
    {
        aosOptions.SetNameValue("ALLOW_ELLIPSOIDAL_HEIGHT_AS_VERTICAL_CRS",
                                pszAllowEllpsHeightAsVertCS);
    }

    PJ *boundCRS = nullptr;
    if (wktFormat == PJ_WKT1_GDAL &&
        CPLTestBool(CSLFetchNameValueDef(
            papszOptions, "ADD_TOWGS84_ON_EXPORT_TO_WKT1",
            CPLGetConfigOption("OSR_ADD_TOWGS84_ON_EXPORT_TO_WKT1", "NO"))))
    {
        boundCRS = GDAL_proj_crs_create_bound_crs_to_WGS84(
            d->getPROJContext(), d->m_pj_crs, true, true);
    }

    const char* pszWKT;
#ifdef MECHSOFT_DEVIATION
    CPLErrorAccumulator oErrorAccumulator;
    {
        auto oAccumulator = oErrorAccumulator.InstallForCurrentScope();
        CPL_IGNORE_RET_VAL(oAccumulator);
        pszWKT = proj_as_wkt(ctxt, boundCRS ? boundCRS : d->m_pj_crs, wktFormat,
                             aosOptions.List());
    }
    for (const auto &oError : oErrorAccumulator.GetErrors())
    {
        if (pszFormat[0] == '\0' &&
            (oError.msg.find("Unsupported conversion method") !=
                 std::string::npos ||
             oError.msg.find("can only be exported to WKT2") !=
                 std::string::npos ||
             oError.msg.find("can only be exported since WKT2:2019") !=
                 std::string::npos))
        {
            CPLErrorReset();
            // If we cannot export in the default mode (WKT1), retry with WKT2
            pszWKT = proj_as_wkt(ctxt, boundCRS ? boundCRS : d->m_pj_crs,
                                 PJ_WKT2_2018, aosOptions.List());
            break;
        }
        CPLError(oError.type, oError.no, "%s", oError.msg.c_str());
    }
#else
    pszWKT = proj_as_wkt(ctxt, boundCRS ? boundCRS : d->m_pj_crs, wktFormat,
        aosOptions.List());
    if (proj_errno(boundCRS ? boundCRS : d->m_pj_crs) && (wktFormat == PJ_WKT1_GDAL || wktFormat == PJ_WKT1_ESRI))
    {
        // If we cannot export in the default mode (WKT1), retry with WKT2
        pszWKT = proj_as_wkt(ctxt, boundCRS ? boundCRS : d->m_pj_crs,
            PJ_WKT2_2018, aosOptions.List());
    }
#endif

    if (!pszWKT)
    {
        *ppszResult = CPLStrdup("");
        proj_destroy(boundCRS);
        return OGRERR_FAILURE;
    }

    if (EQUAL(pszFormat, "SFSQL") || EQUAL(pszFormat, "WKT1_SIMPLE"))
    {
        OGR_SRSNode oRoot;
        oRoot.importFromWkt(&pszWKT);
        oRoot.StripNodes("AXIS");
        if (EQUAL(pszFormat, "SFSQL"))
        {
            oRoot.StripNodes("TOWGS84");
        }
        oRoot.StripNodes("AUTHORITY");
        oRoot.StripNodes("EXTENSION");
        OGRErr eErr;
        if (CPLTestBool(CSLFetchNameValueDef(papszOptions, "MULTILINE", "NO")))
            eErr = oRoot.exportToPrettyWkt(ppszResult, 1);
        else
            eErr = oRoot.exportToWkt(ppszResult);
        proj_destroy(boundCRS);
        return eErr;
    }

    *ppszResult = CPLStrdup(pszWKT);

#if !(PROJ_AT_LEAST_VERSION(9, 5, 0))
    if (wktFormat == PJ_WKT2_2018)
    {
        // Works around bug fixed per https://github.com/OSGeo/PROJ/pull/4166
        // related to a wrong EPSG code assigned to UTM South conversions
        char *pszPtr = strstr(*ppszResult, "CONVERSION[\"UTM zone ");
        if (pszPtr)
        {
            pszPtr += strlen("CONVERSION[\"UTM zone ");
            const int nZone = atoi(pszPtr);
            while (*pszPtr >= '0' && *pszPtr <= '9')
                ++pszPtr;
            if (nZone >= 1 && nZone <= 60 && *pszPtr == 'S' &&
                pszPtr[1] == '"' && pszPtr[2] == ',')
            {
                pszPtr += 3;
                int nLevel = 0;
                bool bInString = false;
                // Find the ID node corresponding to this CONVERSION node
                while (*pszPtr)
                {
                    if (bInString)
                    {
                        if (*pszPtr == '"' && pszPtr[1] == '"')
                        {
                            ++pszPtr;
                        }
                        else if (*pszPtr == '"')
                        {
                            bInString = false;
                        }
                    }
                    else if (nLevel == 0 && STARTS_WITH_CI(pszPtr, "ID["))
                    {
                        if (STARTS_WITH_CI(pszPtr, CPLSPrintf("ID[\"EPSG\",%d]",
                                                              17000 + nZone)))
                        {
                            CPLAssert(pszPtr[11] == '7');
                            CPLAssert(pszPtr[12] == '0');
                            pszPtr[11] = '6';
                            pszPtr[12] = '1';
                        }
                        break;
                    }
                    else if (*pszPtr == '"')
                    {
                        bInString = true;
                    }
                    else if (*pszPtr == '[')
                    {
                        ++nLevel;
                    }
                    else if (*pszPtr == ']')
                    {
                        --nLevel;
                    }

                    ++pszPtr;
                }
            }
        }
    }
#endif

    proj_destroy(boundCRS);
    return OGRERR_NONE;
}

/************************************************************************/
/*                            exportToWkt()                             */
/************************************************************************/

/**
 * Convert this SRS into a WKT string.
 *
 * Consult also the <a href="wktproblems.html">OGC WKT Coordinate System
 * Issues</a> page for implementation details of WKT 1 in OGR.
 *
 * @param papszOptions NULL terminated list of options, or NULL. Currently
 * supported options are
 * <ul>
 * <li>MULTILINE=YES/NO. Defaults to NO.</li>
 * <li>FORMAT=SFSQL/WKT1_SIMPLE/WKT1/WKT1_GDAL/WKT1_ESRI/WKT2_2015/WKT2_2018/WKT2/DEFAULT.
 *     If SFSQL, a WKT1 string without AXIS, TOWGS84, AUTHORITY or EXTENSION
 *     node is returned.
 *     If WKT1_SIMPLE, a WKT1 string without AXIS, AUTHORITY or EXTENSION
 *     node is returned.
 *     WKT1 is an alias of WKT1_GDAL.
 *     WKT2 will default to the latest revision implemented (currently
 *     WKT2_2019)
 * </li>
 * <li>ALLOW_ELLIPSOIDAL_HEIGHT_AS_VERTICAL_CRS=YES/NO. Default is NO. If set
 * to YES and FORMAT=WKT1_GDAL, a Geographic 3D CRS or a Projected 3D CRS will
 * be exported as a compound CRS whose vertical part represents an ellipsoidal
 * height (for example for use with LAS 1.4 WKT1).
 * Requires PROJ 7.2.1.</li>
 * </ul>
 *
 * If the OSR_ADD_TOWGS84_ON_EXPORT_TO_WKT1
 * configuration option is set to YES, when exporting to WKT1_GDAL, this method
 * will try to add a TOWGS84[] node, if there's none attached yet to the SRS and
 * if the SRS has a EPSG code. See the AddGuessedTOWGS84() method for how this
 * TOWGS84[] node may be added.
 *
 * @return a non-empty string if successful.
 * @since GDAL 3.9
 */

std::string
OGRSpatialReference::exportToWkt(const char *const *papszOptions) const
{
    std::string osWKT;
    char *pszWKT = nullptr;
    if (exportToWkt(&pszWKT, papszOptions) == OGRERR_NONE)
        osWKT = pszWKT;
    CPLFree(pszWKT);
    return osWKT;
}

/************************************************************************/
/*                              SetNode()                               */
/************************************************************************/

/**
 * \brief Set attribute value in spatial reference.
 *
 * Missing intermediate nodes in the path will be created if not already
 * in existence.  If the attribute has no children one will be created and
 * assigned the value otherwise the zeroth child will be assigned the value.
 *
 * This method does the same as the C function OSRSetAttrValue().
 *
 * @param pszNodePath full path to attribute to be set.  For instance
 * "PROJCS|GEOGCS|UNIT".
 *
 * @param pszNewNodeValue value to be assigned to node, such as "meter".
 * This may be NULL if you just want to force creation of the intermediate
 * path.
 *
 * @return OGRERR_NONE on success.
 */

OGRErr OGRSpatialReference::SetNode(const char *pszNodePath,
                                    const char *pszNewNodeValue)

{
    TAKE_OPTIONAL_LOCK();

    char **papszPathTokens =
        CSLTokenizeStringComplex(pszNodePath, "|", TRUE, FALSE);

    if (CSLCount(papszPathTokens) < 1)
    {
        CSLDestroy(papszPathTokens);
        return OGRERR_FAILURE;
    }

    if (GetRoot() == nullptr ||
        !EQUAL(papszPathTokens[0], GetRoot()->GetValue()))
    {
        if (EQUAL(papszPathTokens[0], "PROJCS") &&
            CSLCount(papszPathTokens) == 1)
        {
            CSLDestroy(papszPathTokens);
            return SetProjCS(pszNewNodeValue);
        }
        else
        {
            SetRoot(new OGR_SRSNode(papszPathTokens[0]));
        }
    }

    OGR_SRSNode *poNode = GetRoot();
    for (int i = 1; papszPathTokens[i] != nullptr; i++)
    {
        int j = 0;  // Used after for.

        for (; j < poNode->GetChildCount(); j++)
        {
            if (EQUAL(poNode->GetChild(j)->GetValue(), papszPathTokens[i]))
            {
                poNode = poNode->GetChild(j);
                j = -1;
                break;
            }
        }

        if (j != -1)
        {
            OGR_SRSNode *poNewNode = new OGR_SRSNode(papszPathTokens[i]);
            poNode->AddChild(poNewNode);
            poNode = poNewNode;
        }
    }

    CSLDestroy(papszPathTokens);

    if (pszNewNodeValue != nullptr)
    {
        if (poNode->GetChildCount() > 0)
            poNode->GetChild(0)->SetValue(pszNewNodeValue);
        else
            poNode->AddChild(new OGR_SRSNode(pszNewNodeValue));
    };
    return OGRERR_NONE;
}

/************************************************************************/
/*                              SetNode()                               */
/************************************************************************/

/**
 * \brief Set attribute value in spatial reference.
 *
 * Missing intermediate nodes in the path will be created if not already
 * in existence.  If the attribute has no children one will be created and
 * assigned the value otherwise the zeroth child will be assigned the value.
 *
 * This method does the same as the C function OSRSetAttrValue().
 *
 * @param pszNodePath full path to attribute to be set.  For instance
 * "PROJCS|GEOGCS|UNIT".
 *
 * @param dfValue value to be assigned to node.
 *
 * @return OGRERR_NONE on success.
 */

OGRErr OGRSpatialReference::SetNode(const char *pszNodePath, double dfValue)

{
    char szValue[64] = {'\0'};

    if (std::abs(dfValue - static_cast<int>(dfValue)) == 0.0)
        snprintf(szValue, sizeof(szValue), "%d", static_cast<int>(dfValue));
    else
        OGRsnPrintDouble(szValue, sizeof(szValue), dfValue);

    return SetNode(pszNodePath, szValue);
}

/************************************************************************/
/*                          SetAngularUnits()                           */
/************************************************************************/

/**
 * \brief Set the angular units for the geographic coordinate system.
 *
 * This method creates a UNIT subnode with the specified values as a
 * child of the GEOGCS node.
 *
 * This method does the same as the C function OSRSetAngularUnits().
 *
 * @param pszUnitsName the units name to be used.  Some preferred units
 * names can be found in ogr_srs_api.h such as SRS_UA_DEGREE.
 *
 * @param dfInRadians the value to multiple by an angle in the indicated
 * units to transform to radians.  Some standard conversion factors can
 * be found in ogr_srs_api.h.
 *
 * @return OGRERR_NONE on success.
 */

OGRErr OGRSpatialReference::SetAngularUnits(const char *pszUnitsName,
                                            double dfInRadians)

{
    TAKE_OPTIONAL_LOCK();

    d->bNormInfoSet = FALSE;

    d->refreshProjObj();
    if (!d->m_pj_crs)
        return OGRERR_FAILURE;
    auto geodCRS = proj_crs_get_geodetic_crs(d->getPROJContext(), d->m_pj_crs);
    if (!geodCRS)
        return OGRERR_FAILURE;
    proj_destroy(geodCRS);
    d->demoteFromBoundCRS();
    d->setPjCRS(proj_crs_alter_cs_angular_unit(d->getPROJContext(), d->m_pj_crs,
                                               pszUnitsName, dfInRadians,
                                               nullptr, nullptr));
    d->undoDemoteFromBoundCRS();

    d->m_osAngularUnits = pszUnitsName;
    d->m_dfAngularUnitToRadian = dfInRadians;

    return OGRERR_NONE;
}

/************************************************************************/
/*                          GetAngularUnits()                           */
/************************************************************************/

/**
 * \brief Fetch angular geographic coordinate system units.
 *
 * If no units are available, a value of "degree" and SRS_UA_DEGREE_CONV
 * will be assumed.  This method only checks directly under the GEOGCS node
 * for units.
 *
 * This method does the same thing as the C function OSRGetAngularUnits().
 *
 * @param ppszName a pointer to be updated with the pointer to the units name.
 * The returned value remains internal to the OGRSpatialReference and should
 * not be freed, or modified.  It may be invalidated on the next
 * OGRSpatialReference call.
 *
 * @return the value to multiply by angular distances to transform them to
 * radians.
 * @since GDAL 2.3.0
 */

double OGRSpatialReference::GetAngularUnits(const char **ppszName) const

{
    TAKE_OPTIONAL_LOCK();

    d->refreshProjObj();

    if (!d->m_osAngularUnits.empty())
    {
        if (ppszName != nullptr)
            *ppszName = d->m_osAngularUnits.c_str();
        return d->m_dfAngularUnitToRadian;
    }

    do
    {
        if (d->m_pj_crs == nullptr || d->m_pjType == PJ_TYPE_ENGINEERING_CRS)
        {
            break;
        }

        auto geodCRS =
            proj_crs_get_geodetic_crs(d->getPROJContext(), d->m_pj_crs);
        if (!geodCRS)
        {
            break;
        }
        auto coordSys =
            proj_crs_get_coordinate_system(d->getPROJContext(), geodCRS);
        proj_destroy(geodCRS);
        if (!coordSys)
        {
            break;
        }
        if (proj_cs_get_type(d->getPROJContext(), coordSys) !=
            PJ_CS_TYPE_ELLIPSOIDAL)
        {
            proj_destroy(coordSys);
            break;
        }

        double dfConvFactor = 0.0;
        const char *pszUnitName = nullptr;
        if (!proj_cs_get_axis_info(d->getPROJContext(), coordSys, 0, nullptr,
                                   nullptr, nullptr, &dfConvFactor,
                                   &pszUnitName, nullptr, nullptr))
        {
            proj_destroy(coordSys);
            break;
        }

        d->m_osAngularUnits = pszUnitName;

        proj_destroy(coordSys);
        d->m_dfAngularUnitToRadian = dfConvFactor;
    } while (false);

    if (d->m_osAngularUnits.empty())
    {
        d->m_osAngularUnits = "degree";
        d->m_dfAngularUnitToRadian = CPLAtof(SRS_UA_DEGREE_CONV);
    }

    if (ppszName != nullptr)
        *ppszName = d->m_osAngularUnits.c_str();
    return d->m_dfAngularUnitToRadian;
}

/**
 * \brief Fetch angular geographic coordinate system units.
 *
 * If no units are available, a value of "degree" and SRS_UA_DEGREE_CONV
 * will be assumed.  This method only checks directly under the GEOGCS node
 * for units.
 *
 * This method does the same thing as the C function OSRGetAngularUnits().
 *
 * @param ppszName a pointer to be updated with the pointer to the units name.
 * The returned value remains internal to the OGRSpatialReference and should
 * not be freed, or modified.  It may be invalidated on the next
 * OGRSpatialReference call.
 *
 * @return the value to multiply by angular distances to transform them to
 * radians.
 * @deprecated GDAL 2.3.0. Use GetAngularUnits(const char**) const.
 */

double OGRSpatialReference::GetAngularUnits(char **ppszName) const

{
    return GetAngularUnits(const_cast<const char **>(ppszName));
}

/************************************************************************/
/*                 SetLinearUnitsAndUpdateParameters()                  */
/************************************************************************/

/**
 * \brief Set the linear units for the projection.
 *
 * This method creates a UNIT subnode with the specified values as a
 * child of the PROJCS or LOCAL_CS node.   It works the same as the
 * SetLinearUnits() method, but it also updates all existing linear
 * projection parameter values from the old units to the new units.
 *
 * @param pszName the units name to be used.  Some preferred units
 * names can be found in ogr_srs_api.h such as SRS_UL_METER, SRS_UL_FOOT
 * and SRS_UL_US_FOOT.
 *
 * @param dfInMeters the value to multiple by a length in the indicated
 * units to transform to meters.  Some standard conversion factors can
 * be found in ogr_srs_api.h.
 *
 * @param pszUnitAuthority Unit authority name. Or nullptr
 *
 * @param pszUnitCode Unit code. Or nullptr
 *
 * @return OGRERR_NONE on success.
 */

OGRErr OGRSpatialReference::SetLinearUnitsAndUpdateParameters(
    const char *pszName, double dfInMeters, const char *pszUnitAuthority,
    const char *pszUnitCode)

{
    TAKE_OPTIONAL_LOCK();

    if (dfInMeters <= 0.0)
        return OGRERR_FAILURE;

    d->refreshProjObj();
    if (!d->m_pj_crs)
        return OGRERR_FAILURE;

    d->demoteFromBoundCRS();
    if (d->m_pjType == PJ_TYPE_PROJECTED_CRS)
    {
        d->setPjCRS(proj_crs_alter_parameters_linear_unit(
            d->getPROJContext(), d->m_pj_crs, pszName, dfInMeters,
            pszUnitAuthority, pszUnitCode, true));
    }
    d->setPjCRS(proj_crs_alter_cs_linear_unit(d->getPROJContext(), d->m_pj_crs,
                                              pszName, dfInMeters,
                                              pszUnitAuthority, pszUnitCode));
    d->undoDemoteFromBoundCRS();

    d->m_osLinearUnits = pszName;
    d->dfToMeter = dfInMeters;

    return OGRERR_NONE;
}

/************************************************************************/
/*                           SetLinearUnits()                           */
/************************************************************************/

/**
 * \brief Set the linear units for the projection.
 *
 * This method creates a UNIT subnode with the specified values as a
 * child of the PROJCS, GEOCCS, GEOGCS or LOCAL_CS node. When called on a
 * Geographic 3D CRS the vertical axis units will be set.
 *
 * This method does the same as the C function OSRSetLinearUnits().
 *
 * @param pszUnitsName the units name to be used.  Some preferred units
 * names can be found in ogr_srs_api.h such as SRS_UL_METER, SRS_UL_FOOT
 * and SRS_UL_US_FOOT.
 *
 * @param dfInMeters the value to multiple by a length in the indicated
 * units to transform to meters.  Some standard conversion factors can
 * be found in ogr_srs_api.h.
 *
 * @return OGRERR_NONE on success.
 */

OGRErr OGRSpatialReference::SetLinearUnits(const char *pszUnitsName,
                                           double dfInMeters)

{
    return SetTargetLinearUnits(nullptr, pszUnitsName, dfInMeters);
}

/************************************************************************/
/*                        SetTargetLinearUnits()                        */
/************************************************************************/

/**
 * \brief Set the linear units for the projection.
 *
 * This method creates a UNIT subnode with the specified values as a
 * child of the target node.
 *
 * This method does the same as the C function OSRSetTargetLinearUnits().
 *
 * @param pszTargetKey the keyword to set the linear units for.
 * i.e. "PROJCS" or "VERT_CS"
 *
 * @param pszUnitsName the units name to be used.  Some preferred units
 * names can be found in ogr_srs_api.h such as SRS_UL_METER, SRS_UL_FOOT
 * and SRS_UL_US_FOOT.
 *
 * @param dfInMeters the value to multiple by a length in the indicated
 * units to transform to meters.  Some standard conversion factors can
 * be found in ogr_srs_api.h.
 *
 * @param pszUnitAuthority Unit authority name. Or nullptr
 *
 * @param pszUnitCode Unit code. Or nullptr
 *
 * @return OGRERR_NONE on success.
 *
 * @since OGR 1.9.0
 */

OGRErr OGRSpatialReference::SetTargetLinearUnits(const char *pszTargetKey,
                                                 const char *pszUnitsName,
                                                 double dfInMeters,
                                                 const char *pszUnitAuthority,
                                                 const char *pszUnitCode)

{
    TAKE_OPTIONAL_LOCK();

    if (dfInMeters <= 0.0)
        return OGRERR_FAILURE;

    d->refreshProjObj();
    pszTargetKey = d->nullifyTargetKeyIfPossible(pszTargetKey);
    if (pszTargetKey == nullptr)
    {
        if (!d->m_pj_crs)
            return OGRERR_FAILURE;

        d->demoteFromBoundCRS();
        if (d->m_pjType == PJ_TYPE_PROJECTED_CRS)
        {
            d->setPjCRS(proj_crs_alter_parameters_linear_unit(
                d->getPROJContext(), d->m_pj_crs, pszUnitsName, dfInMeters,
                pszUnitAuthority, pszUnitCode, false));
        }
        d->setPjCRS(proj_crs_alter_cs_linear_unit(
            d->getPROJContext(), d->m_pj_crs, pszUnitsName, dfInMeters,
            pszUnitAuthority, pszUnitCode));
        d->undoDemoteFromBoundCRS();

        d->m_osLinearUnits = pszUnitsName;
        d->dfToMeter = dfInMeters;

        return OGRERR_NONE;
    }

    OGR_SRSNode *poCS = GetAttrNode(pszTargetKey);

    if (poCS == nullptr)
        return OGRERR_FAILURE;

    char szValue[128] = {'\0'};
    if (dfInMeters < std::numeric_limits<int>::max() &&
        dfInMeters > std::numeric_limits<int>::min() &&
        dfInMeters == static_cast<int>(dfInMeters))
        snprintf(szValue, sizeof(szValue), "%d", static_cast<int>(dfInMeters));
    else
        OGRsnPrintDouble(szValue, sizeof(szValue), dfInMeters);

    OGR_SRSNode *poUnits = nullptr;
    if (poCS->FindChild("UNIT") >= 0)
    {
        poUnits = poCS->GetChild(poCS->FindChild("UNIT"));
        if (poUnits->GetChildCount() < 2)
            return OGRERR_FAILURE;
        poUnits->GetChild(0)->SetValue(pszUnitsName);
        poUnits->GetChild(1)->SetValue(szValue);
        if (poUnits->FindChild("AUTHORITY") != -1)
            poUnits->DestroyChild(poUnits->FindChild("AUTHORITY"));
    }
    else
    {
        poUnits = new OGR_SRSNode("UNIT");
        poUnits->AddChild(new OGR_SRSNode(pszUnitsName));
        poUnits->AddChild(new OGR_SRSNode(szValue));

        poCS->AddChild(poUnits);
    }

    return OGRERR_NONE;
}

/************************************************************************/
/*                           GetLinearUnits()                           */
/************************************************************************/

/**
 * \brief Fetch linear projection units.
 *
 * If no units are available, a value of "Meters" and 1.0 will be assumed.
 * This method only checks directly under the PROJCS, GEOCCS, GEOGCS or
 * LOCAL_CS node for units. When called on a Geographic 3D CRS the vertical
 * axis units will be returned.
 *
 * This method does the same thing as the C function OSRGetLinearUnits()
 *
 * @param ppszName a pointer to be updated with the pointer to the units name.
 * The returned value remains internal to the OGRSpatialReference and should
 * not be freed, or modified.  It may be invalidated on the next
 * OGRSpatialReference call.
 *
 * @return the value to multiply by linear distances to transform them to
 * meters.
 * @deprecated GDAL 2.3.0. Use GetLinearUnits(const char**) const.
 */

double OGRSpatialReference::GetLinearUnits(char **ppszName) const

{
    return GetTargetLinearUnits(nullptr, const_cast<const char **>(ppszName));
}

/**
 * \brief Fetch linear projection units.
 *
 * If no units are available, a value of "Meters" and 1.0 will be assumed.
 * This method only checks directly under the PROJCS, GEOCCS or LOCAL_CS node
 * for units.
 *
 * This method does the same thing as the C function OSRGetLinearUnits()
 *
 * @param ppszName a pointer to be updated with the pointer to the units name.
 * The returned value remains internal to the OGRSpatialReference and should
 * not be freed, or modified.  It may be invalidated on the next
 * OGRSpatialReference call.
 *
 * @return the value to multiply by linear distances to transform them to
 * meters.
 * @since GDAL 2.3.0
 */

double OGRSpatialReference::GetLinearUnits(const char **ppszName) const

{
    return GetTargetLinearUnits(nullptr, ppszName);
}

/************************************************************************/
/*                        GetTargetLinearUnits()                        */
/************************************************************************/

/**
 * \brief Fetch linear units for target.
 *
 * If no units are available, a value of "Meters" and 1.0 will be assumed.
 *
 * This method does the same thing as the C function OSRGetTargetLinearUnits()
 *
 * @param pszTargetKey the key to look on. i.e. "PROJCS" or "VERT_CS". Might be
 * NULL, in which case PROJCS will be implied (and if not found, LOCAL_CS,
 * GEOCCS, GEOGCS and VERT_CS are looked up)
 * @param ppszName a pointer to be updated with the pointer to the units name.
 * The returned value remains internal to the OGRSpatialReference and should not
 * be freed, or modified.  It may be invalidated on the next
 * OGRSpatialReference call. ppszName can be set to NULL.
 *
 * @return the value to multiply by linear distances to transform them to
 * meters.
 *
 * @since OGR 1.9.0
 * @deprecated GDAL 2.3.0. Use GetTargetLinearUnits(const char*, const char**)
 * const.
 */

double OGRSpatialReference::GetTargetLinearUnits(const char *pszTargetKey,
                                                 const char **ppszName) const

{
    TAKE_OPTIONAL_LOCK();

    d->refreshProjObj();

    pszTargetKey = d->nullifyTargetKeyIfPossible(pszTargetKey);
    if (pszTargetKey == nullptr)
    {
        // Use cached result if available
        if (!d->m_osLinearUnits.empty())
        {
            if (ppszName)
                *ppszName = d->m_osLinearUnits.c_str();
            return d->dfToMeter;
        }

        while (true)
        {
            if (d->m_pj_crs == nullptr)
            {
                break;
            }

            d->demoteFromBoundCRS();
            PJ *coordSys = nullptr;
            if (d->m_pjType == PJ_TYPE_COMPOUND_CRS)
            {
                for (int iComponent = 0; iComponent < 2; iComponent++)
                {
                    auto subCRS = proj_crs_get_sub_crs(d->getPROJContext(),
                                                       d->m_pj_crs, iComponent);
                    if (subCRS && proj_get_type(subCRS) == PJ_TYPE_BOUND_CRS)
                    {
                        auto temp =
                            proj_get_source_crs(d->getPROJContext(), subCRS);
                        proj_destroy(subCRS);
                        subCRS = temp;
                    }
                    if (subCRS &&
                        (proj_get_type(subCRS) == PJ_TYPE_PROJECTED_CRS ||
                         proj_get_type(subCRS) == PJ_TYPE_ENGINEERING_CRS ||
                         proj_get_type(subCRS) == PJ_TYPE_VERTICAL_CRS))
                    {
                        coordSys = proj_crs_get_coordinate_system(
                            d->getPROJContext(), subCRS);
                        proj_destroy(subCRS);
                        break;
                    }
                    else if (subCRS)
                    {
                        proj_destroy(subCRS);
                    }
                }
                if (coordSys == nullptr)
                {
                    d->undoDemoteFromBoundCRS();
                    break;
                }
            }
            else
            {
                coordSys = proj_crs_get_coordinate_system(d->getPROJContext(),
                                                          d->m_pj_crs);
            }

            d->undoDemoteFromBoundCRS();
            if (!coordSys)
            {
                break;
            }
            auto csType = proj_cs_get_type(d->getPROJContext(), coordSys);

            if (csType != PJ_CS_TYPE_CARTESIAN &&
                csType != PJ_CS_TYPE_VERTICAL &&
                csType != PJ_CS_TYPE_ELLIPSOIDAL &&
                csType != PJ_CS_TYPE_SPHERICAL)
            {
                proj_destroy(coordSys);
                break;
            }

            int axis = 0;

            if (csType == PJ_CS_TYPE_ELLIPSOIDAL ||
                csType == PJ_CS_TYPE_SPHERICAL)
            {
                const int axisCount =
                    proj_cs_get_axis_count(d->getPROJContext(), coordSys);

                if (axisCount == 3)
                {
                    axis = 2;
                }
                else
                {
                    proj_destroy(coordSys);
                    break;
                }
            }

            double dfConvFactor = 0.0;
            const char *pszUnitName = nullptr;
            if (!proj_cs_get_axis_info(d->getPROJContext(), coordSys, axis,
                                       nullptr, nullptr, nullptr, &dfConvFactor,
                                       &pszUnitName, nullptr, nullptr))
            {
                proj_destroy(coordSys);
                break;
            }

            d->m_osLinearUnits = pszUnitName;
            d->dfToMeter = dfConvFactor;
            if (ppszName)
                *ppszName = d->m_osLinearUnits.c_str();

            proj_destroy(coordSys);
            return dfConvFactor;
        }

        d->m_osLinearUnits = "unknown";
        d->dfToMeter = 1.0;

        if (ppszName != nullptr)
            *ppszName = d->m_osLinearUnits.c_str();
        return 1.0;
    }

    const OGR_SRSNode *poCS = GetAttrNode(pszTargetKey);

    if (ppszName != nullptr)
        *ppszName = "unknown";

    if (poCS == nullptr)
        return 1.0;

    for (int iChild = 0; iChild < poCS->GetChildCount(); iChild++)
    {
        const OGR_SRSNode *poChild = poCS->GetChild(iChild);

        if (EQUAL(poChild->GetValue(), "UNIT") && poChild->GetChildCount() >= 2)
        {
            if (ppszName != nullptr)
                *ppszName = poChild->GetChild(0)->GetValue();

            return CPLAtof(poChild->GetChild(1)->GetValue());
        }
    }

    return 1.0;
}

/**
 * \brief Fetch linear units for target.
 *
 * If no units are available, a value of "Meters" and 1.0 will be assumed.
 *
 * This method does the same thing as the C function OSRGetTargetLinearUnits()
 *
 * @param pszTargetKey the key to look on. i.e. "PROJCS" or "VERT_CS". Might be
 * NULL, in which case PROJCS will be implied (and if not found, LOCAL_CS,
 * GEOCCS and VERT_CS are looked up)
 * @param ppszName a pointer to be updated with the pointer to the units name.
 * The returned value remains internal to the OGRSpatialReference and should not
 * be freed, or modified.  It may be invalidated on the next
 * OGRSpatialReference call. ppszName can be set to NULL.
 *
 * @return the value to multiply by linear distances to transform them to
 * meters.
 *
 * @since GDAL 2.3.0
 */

double OGRSpatialReference::GetTargetLinearUnits(const char *pszTargetKey,
                                                 char **ppszName) const

{
    return GetTargetLinearUnits(pszTargetKey,
                                const_cast<const char **>(ppszName));
}

/************************************************************************/
/*                          GetPrimeMeridian()                          */
/************************************************************************/

/**
 * \brief Fetch prime meridian info.
 *
 * Returns the offset of the prime meridian from greenwich in degrees,
 * and the prime meridian name (if requested).   If no PRIMEM value exists
 * in the coordinate system definition a value of "Greenwich" and an
 * offset of 0.0 is assumed.
 *
 * If the prime meridian name is returned, the pointer is to an internal
 * copy of the name. It should not be freed, altered or depended on after
 * the next OGR call.
 *
 * This method is the same as the C function OSRGetPrimeMeridian().
 *
 * @param ppszName return location for prime meridian name.  If NULL, name
 * is not returned.
 *
 * @return the offset to the GEOGCS prime meridian from greenwich in decimal
 * degrees.
 * @deprecated GDAL 2.3.0. Use GetPrimeMeridian(const char**) const.
 */

double OGRSpatialReference::GetPrimeMeridian(const char **ppszName) const

{
    TAKE_OPTIONAL_LOCK();

    d->refreshProjObj();

    if (!d->m_osPrimeMeridianName.empty())
    {
        if (ppszName != nullptr)
            *ppszName = d->m_osPrimeMeridianName.c_str();
        return d->dfFromGreenwich;
    }

    while (true)
    {
        if (!d->m_pj_crs)
            break;

        auto pm = proj_get_prime_meridian(d->getPROJContext(), d->m_pj_crs);
        if (!pm)
            break;

        d->m_osPrimeMeridianName = proj_get_name(pm);
        if (ppszName)
            *ppszName = d->m_osPrimeMeridianName.c_str();
        double dfLongitude = 0.0;
        double dfConvFactor = 0.0;
        proj_prime_meridian_get_parameters(
            d->getPROJContext(), pm, &dfLongitude, &dfConvFactor, nullptr);
        proj_destroy(pm);
        d->dfFromGreenwich =
            dfLongitude * dfConvFactor / CPLAtof(SRS_UA_DEGREE_CONV);
        return d->dfFromGreenwich;
    }

    d->m_osPrimeMeridianName = SRS_PM_GREENWICH;
    d->dfFromGreenwich = 0.0;
    if (ppszName != nullptr)
        *ppszName = d->m_osPrimeMeridianName.c_str();
    return d->dfFromGreenwich;
}

/**
 * \brief Fetch prime meridian info.
 *
 * Returns the offset of the prime meridian from greenwich in degrees,
 * and the prime meridian name (if requested).   If no PRIMEM value exists
 * in the coordinate system definition a value of "Greenwich" and an
 * offset of 0.0 is assumed.
 *
 * If the prime meridian name is returned, the pointer is to an internal
 * copy of the name. It should not be freed, altered or depended on after
 * the next OGR call.
 *
 * This method is the same as the C function OSRGetPrimeMeridian().
 *
 * @param ppszName return location for prime meridian name.  If NULL, name
 * is not returned.
 *
 * @return the offset to the GEOGCS prime meridian from greenwich in decimal
 * degrees.
 * @since GDAL 2.3.0
 */

double OGRSpatialReference::GetPrimeMeridian(char **ppszName) const

{
    return GetPrimeMeridian(const_cast<const char **>(ppszName));
}

/************************************************************************/
/*                             SetGeogCS()                              */
/************************************************************************/

/**
 * \brief Set geographic coordinate system.
 *
 * This method is used to set the datum, ellipsoid, prime meridian and
 * angular units for a geographic coordinate system.  It can be used on its
 * own to establish a geographic spatial reference, or applied to a
 * projected coordinate system to establish the underlying geographic
 * coordinate system.
 *
 * This method does the same as the C function OSRSetGeogCS().
 *
 * @param pszGeogName user visible name for the geographic coordinate system
 * (not to serve as a key).
 *
 * @param pszDatumName key name for this datum.  The OpenGIS specification
 * lists some known values, and otherwise EPSG datum names with a standard
 * transformation are considered legal keys.
 *
 * @param pszSpheroidName user visible spheroid name (not to serve as a key)
 *
 * @param dfSemiMajor the semi major axis of the spheroid.
 *
 * @param dfInvFlattening the inverse flattening for the spheroid.
 * This can be computed from the semi minor axis as
 * 1/f = 1.0 / (1.0 - semiminor/semimajor).
 *
 * @param pszPMName the name of the prime meridian (not to serve as a key)
 * If this is NULL a default value of "Greenwich" will be used.
 *
 * @param dfPMOffset the longitude of Greenwich relative to this prime
 * meridian. Always in Degrees
 *
 * @param pszAngularUnits the angular units name (see ogr_srs_api.h for some
 * standard names).  If NULL a value of "degrees" will be assumed.
 *
 * @param dfConvertToRadians value to multiply angular units by to transform
 * them to radians.  A value of SRS_UA_DEGREE_CONV will be used if
 * pszAngularUnits is NULL.
 *
 * @return OGRERR_NONE on success.
 */

OGRErr OGRSpatialReference::SetGeogCS(
    const char *pszGeogName, const char *pszDatumName,
    const char *pszSpheroidName, double dfSemiMajor, double dfInvFlattening,
    const char *pszPMName, double dfPMOffset, const char *pszAngularUnits,
    double dfConvertToRadians)

{
    TAKE_OPTIONAL_LOCK();

    d->bNormInfoSet = FALSE;
    d->m_osAngularUnits.clear();
    d->m_dfAngularUnitToRadian = 0.0;
    d->m_osPrimeMeridianName.clear();
    d->dfFromGreenwich = 0.0;

    /* -------------------------------------------------------------------- */
    /*      For a geocentric coordinate system we want to set the datum     */
    /*      and ellipsoid based on the GEOGCS.  Create the GEOGCS in a      */
    /*      temporary srs and use the copy method which has special         */
    /*      handling for GEOCCS.                                            */
    /* -------------------------------------------------------------------- */
    if (IsGeocentric())
    {
        OGRSpatialReference oGCS{ d->getPROJContext() };

        oGCS.SetGeogCS(pszGeogName, pszDatumName, pszSpheroidName, dfSemiMajor,
                       dfInvFlattening, pszPMName, dfPMOffset, pszAngularUnits,
                       dfConvertToRadians);
        return CopyGeogCSFrom(&oGCS);
    }

    auto cs = proj_create_ellipsoidal_2D_cs(
        d->getPROJContext(), PJ_ELLPS2D_LATITUDE_LONGITUDE, pszAngularUnits,
        dfConvertToRadians);
    // Prime meridian expressed in Degree
    auto obj = proj_create_geographic_crs(
        d->getPROJContext(), pszGeogName, pszDatumName, pszSpheroidName,
        dfSemiMajor, dfInvFlattening, pszPMName, dfPMOffset, nullptr, 0.0, cs);
    proj_destroy(cs);

    if (d->m_pj_crs == nullptr || d->m_pjType == PJ_TYPE_GEOGRAPHIC_2D_CRS ||
        d->m_pjType == PJ_TYPE_GEOGRAPHIC_3D_CRS)
    {
        d->setPjCRS(obj);
    }
    else if (d->m_pjType == PJ_TYPE_PROJECTED_CRS)
    {
        d->setPjCRS(
            proj_crs_alter_geodetic_crs(d->getPROJContext(), d->m_pj_crs, obj));
        proj_destroy(obj);
    }
    else
    {
        proj_destroy(obj);
    }

    return OGRERR_NONE;
}

/************************************************************************/
/*                         SetWellKnownGeogCS()                         */
/************************************************************************/

/**
 * \brief Set a GeogCS based on well known name.
 *
 * This may be called on an empty OGRSpatialReference to make a geographic
 * coordinate system, or on something with an existing PROJCS node to
 * set the underlying geographic coordinate system of a projected coordinate
 * system.
 *
 * The following well known text values are currently supported,
 * Except for "EPSG:n", the others are without dependency on EPSG data files:
 * <ul>
 * <li> "EPSG:n": where n is the code a Geographic coordinate reference system.
 * <li> "WGS84": same as "EPSG:4326" (axis order lat/long).
 * <li> "WGS72": same as "EPSG:4322" (axis order lat/long).
 * <li> "NAD83": same as "EPSG:4269" (axis order lat/long).
 * <li> "NAD27": same as "EPSG:4267" (axis order lat/long).
 * <li> "CRS84", "CRS:84": same as "WGS84" but with axis order long/lat.
 * <li> "CRS72", "CRS:72": same as "WGS72" but with axis order long/lat.
 * <li> "CRS27", "CRS:27": same as "NAD27" but with axis order long/lat.
 * </ul>
 *
 * @param pszName name of well known geographic coordinate system.
 * @return OGRERR_NONE on success, or OGRERR_FAILURE if the name isn't
 * recognised, the target object is already initialized, or an EPSG value
 * can't be successfully looked up.
 */

OGRErr OGRSpatialReference::SetWellKnownGeogCS(const char *pszName)

{
    TAKE_OPTIONAL_LOCK();

    /* -------------------------------------------------------------------- */
    /*      Check for EPSG authority numbers.                               */
    /* -------------------------------------------------------------------- */
    if (STARTS_WITH_CI(pszName, "EPSG:") || STARTS_WITH_CI(pszName, "EPSGA:"))
    {
        OGRSpatialReference oSRS2{ d->getPROJContext() };
        const OGRErr eErr = oSRS2.importFromEPSG(atoi(pszName + 5));
        if (eErr != OGRERR_NONE)
            return eErr;

        if (!oSRS2.IsGeographic())
            return OGRERR_FAILURE;

        return CopyGeogCSFrom(&oSRS2);
    }

    /* -------------------------------------------------------------------- */
    /*      Check for simple names.                                         */
    /* -------------------------------------------------------------------- */
    const char *pszWKT = nullptr;

    if (EQUAL(pszName, "WGS84"))
    {
        pszWKT = SRS_WKT_WGS84_LAT_LONG;
    }
    else if (EQUAL(pszName, "CRS84") || EQUAL(pszName, "CRS:84"))
    {
        pszWKT =
            "GEOGCRS[\"WGS 84 (CRS84)\",DATUM[\"World Geodetic System 1984\","
            "ELLIPSOID[\"WGS "
            "84\",6378137,298.257223563,LENGTHUNIT[\"metre\",1]]],"
            "PRIMEM[\"Greenwich\",0,ANGLEUNIT[\"degree\",0.0174532925199433]],"
            "CS[ellipsoidal,2],AXIS[\"geodetic longitude (Lon)\",east,ORDER[1],"
            "ANGLEUNIT[\"degree\",0.0174532925199433]],"
            "AXIS[\"geodetic latitude (Lat)\",north,ORDER[2],"
            "ANGLEUNIT[\"degree\",0.0174532925199433]],"
            "USAGE[SCOPE[\"unknown\"],AREA[\"World\"],BBOX[-90,-180,90,180]],"
            "ID[\"OGC\",\"CRS84\"]]";
    }
    else if (EQUAL(pszName, "WGS72"))
        pszWKT =
            "GEOGCS[\"WGS 72\",DATUM[\"WGS_1972\","
            "SPHEROID[\"WGS 72\",6378135,298.26,AUTHORITY[\"EPSG\",\"7043\"]],"
            "AUTHORITY[\"EPSG\",\"6322\"]],"
            "PRIMEM[\"Greenwich\",0,AUTHORITY[\"EPSG\",\"8901\"]],"
            "UNIT[\"degree\",0.0174532925199433,AUTHORITY[\"EPSG\",\"9122\"]],"
            "AXIS[\"Latitude\",NORTH],AXIS[\"Longitude\",EAST],"
            "AUTHORITY[\"EPSG\",\"4322\"]]";

    else if (EQUAL(pszName, "NAD27"))
        pszWKT =
            "GEOGCS[\"NAD27\",DATUM[\"North_American_Datum_1927\","
            "SPHEROID[\"Clarke 1866\",6378206.4,294.9786982138982,"
            "AUTHORITY[\"EPSG\",\"7008\"]],AUTHORITY[\"EPSG\",\"6267\"]],"
            "PRIMEM[\"Greenwich\",0,AUTHORITY[\"EPSG\",\"8901\"]],"
            "UNIT[\"degree\",0.0174532925199433,AUTHORITY[\"EPSG\",\"9122\"]],"
            "AXIS[\"Latitude\",NORTH],AXIS[\"Longitude\",EAST],"
            "AUTHORITY[\"EPSG\",\"4267\"]]";

    else if (EQUAL(pszName, "CRS27") || EQUAL(pszName, "CRS:27"))
        pszWKT =
            "GEOGCS[\"NAD27\",DATUM[\"North_American_Datum_1927\","
            "SPHEROID[\"Clarke 1866\",6378206.4,294.9786982138982,"
            "AUTHORITY[\"EPSG\",\"7008\"]],AUTHORITY[\"EPSG\",\"6267\"]],"
            "PRIMEM[\"Greenwich\",0,AUTHORITY[\"EPSG\",\"8901\"]],"
            "UNIT[\"degree\",0.0174532925199433,AUTHORITY[\"EPSG\",\"9122\"]],"
            "AXIS[\"Longitude\",EAST],AXIS[\"Latitude\",NORTH]]";

    else if (EQUAL(pszName, "NAD83"))
        pszWKT =
            "GEOGCS[\"NAD83\",DATUM[\"North_American_Datum_1983\","
            "SPHEROID[\"GRS 1980\",6378137,298.257222101,"
            "AUTHORITY[\"EPSG\",\"7019\"]],"
            "AUTHORITY[\"EPSG\",\"6269\"]],"
            "PRIMEM[\"Greenwich\",0,AUTHORITY[\"EPSG\",\"8901\"]],"
            "UNIT[\"degree\",0.0174532925199433,AUTHORITY[\"EPSG\",\"9122\"]],"
            "AXIS[\"Latitude\",NORTH],AXIS[\"Longitude\",EAST],AUTHORITY["
            "\"EPSG\",\"4269\"]]";

    else if (EQUAL(pszName, "CRS83") || EQUAL(pszName, "CRS:83"))
        pszWKT =
            "GEOGCS[\"NAD83\",DATUM[\"North_American_Datum_1983\","
            "SPHEROID[\"GRS 1980\",6378137,298.257222101,"
            "AUTHORITY[\"EPSG\",\"7019\"]],"
            "AUTHORITY[\"EPSG\",\"6269\"]],"
            "PRIMEM[\"Greenwich\",0,AUTHORITY[\"EPSG\",\"8901\"]],"
            "UNIT[\"degree\",0.0174532925199433,AUTHORITY[\"EPSG\",\"9122\"]],"
            "AXIS[\"Longitude\",EAST],AXIS[\"Latitude\",NORTH]]";

    else
        return OGRERR_FAILURE;

    /* -------------------------------------------------------------------- */
    /*      Import the WKT                                                  */
    /* -------------------------------------------------------------------- */
    OGRSpatialReference oSRS2{ d->getPROJContext() };
    const OGRErr eErr = oSRS2.importFromWkt(pszWKT);
    if (eErr != OGRERR_NONE)
        return eErr;

    /* -------------------------------------------------------------------- */
    /*      Copy over.                                                      */
    /* -------------------------------------------------------------------- */
    return CopyGeogCSFrom(&oSRS2);
}

/************************************************************************/
/*                           CopyGeogCSFrom()                           */
/************************************************************************/

/**
 * \brief Copy GEOGCS from another OGRSpatialReference.
 *
 * The GEOGCS information is copied into this OGRSpatialReference from another.
 * If this object has a PROJCS root already, the GEOGCS is installed within
 * it, otherwise it is installed as the root.
 *
 * @param poSrcSRS the spatial reference to copy the GEOGCS information from.
 *
 * @return OGRERR_NONE on success or an error code.
 */

OGRErr OGRSpatialReference::CopyGeogCSFrom(const OGRSpatialReference *poSrcSRS)

{
    TAKE_OPTIONAL_LOCK();

    d->bNormInfoSet = FALSE;
    d->m_osAngularUnits.clear();
    d->m_dfAngularUnitToRadian = 0.0;
    d->m_osPrimeMeridianName.clear();
    d->dfFromGreenwich = 0.0;

    d->refreshProjObj();
    poSrcSRS->d->refreshProjObj();
    if (!poSrcSRS->d->m_pj_crs)
    {
        return OGRERR_FAILURE;
    }
    auto geodCRS =
        proj_crs_get_geodetic_crs(d->getPROJContext(), poSrcSRS->d->m_pj_crs);
    if (!geodCRS)
    {
        return OGRERR_FAILURE;
    }

    /* -------------------------------------------------------------------- */
    /*      Handle geocentric coordinate systems specially.  We just        */
    /*      want to copy the DATUM.                                         */
    /* -------------------------------------------------------------------- */
    if (d->m_pjType == PJ_TYPE_GEOCENTRIC_CRS)
    {
        auto datum = proj_crs_get_datum(d->getPROJContext(), geodCRS);
#if PROJ_VERSION_MAJOR > 7 ||                                                  \
    (PROJ_VERSION_MAJOR == 7 && PROJ_VERSION_MINOR >= 2)
        if (datum == nullptr)
        {
            datum = proj_crs_get_datum_ensemble(d->getPROJContext(), geodCRS);
        }
#endif
        if (datum == nullptr)
        {
            proj_destroy(geodCRS);
            return OGRERR_FAILURE;
        }

        const char *pszUnitName = nullptr;
        double unitConvFactor = GetLinearUnits(&pszUnitName);

        auto pj_crs = proj_create_geocentric_crs_from_datum(
            d->getPROJContext(), proj_get_name(d->m_pj_crs), datum, pszUnitName,
            unitConvFactor);
        proj_destroy(datum);

        d->setPjCRS(pj_crs);
    }

    else if (d->m_pjType == PJ_TYPE_PROJECTED_CRS)
    {
        auto pj_crs = proj_crs_alter_geodetic_crs(d->getPROJContext(),
                                                  d->m_pj_crs, geodCRS);
        d->setPjCRS(pj_crs);
    }

    else
    {
        d->setPjCRS(proj_clone(d->getPROJContext(), geodCRS));
    }

    // Apply TOWGS84 of source CRS
    if (poSrcSRS->d->m_pjType == PJ_TYPE_BOUND_CRS)
    {
        auto target =
            proj_get_target_crs(d->getPROJContext(), poSrcSRS->d->m_pj_crs);
        auto co = proj_crs_get_coordoperation(d->getPROJContext(),
                                              poSrcSRS->d->m_pj_crs);
        d->setPjCRS(proj_crs_create_bound_crs(d->getPROJContext(), d->m_pj_crs,
                                              target, co));
        proj_destroy(target);
        proj_destroy(co);
    }

    proj_destroy(geodCRS);

    return OGRERR_NONE;
}

/************************************************************************/
/*                   SET_FROM_USER_INPUT_LIMITATIONS_get()              */
/************************************************************************/

/** Limitations for OGRSpatialReference::SetFromUserInput().
 *
 * Currently ALLOW_NETWORK_ACCESS=NO and ALLOW_FILE_ACCESS=NO.
 */
const char *const OGRSpatialReference::SET_FROM_USER_INPUT_LIMITATIONS[] = {
    "ALLOW_NETWORK_ACCESS=NO", "ALLOW_FILE_ACCESS=NO", nullptr};

/**
 * \brief Return OGRSpatialReference::SET_FROM_USER_INPUT_LIMITATIONS
 */
CSLConstList OGRSpatialReference::SET_FROM_USER_INPUT_LIMITATIONS_get()
{
    return SET_FROM_USER_INPUT_LIMITATIONS;
}

#ifdef MECHSOFT_DEVIATION
/************************************************************************/
/*                      RemoveIDFromMemberOfEnsembles()                 */
/************************************************************************/

// cppcheck-suppress constParameterReference
static void RemoveIDFromMemberOfEnsembles(CPLJSONObject &obj)
{
    // Remove "id" from members of datum ensembles for compatibility with
    // older PROJ versions
    // Cf https://github.com/opengeospatial/geoparquet/discussions/110
    // and https://github.com/OSGeo/PROJ/pull/3221
    if (obj.GetType() == CPLJSONObject::Type::Object)
    {
        for (auto &subObj : obj.GetChildren())
        {
            RemoveIDFromMemberOfEnsembles(subObj);
        }
    }
    else if (obj.GetType() == CPLJSONObject::Type::Array &&
             obj.GetName() == "members")
    {
        for (auto &subObj : obj.ToArray())
        {
            if (subObj.GetType() == CPLJSONObject::Type::Object)
            {
                subObj.Delete("id");
            }
        }
    }
}
#endif

/************************************************************************/
/*                          SetFromUserInput()                          */
/************************************************************************/

/**
 * \brief Set spatial reference from various text formats.
 *
 * This method will examine the provided input, and try to deduce the
 * format, and then use it to initialize the spatial reference system.  It
 * may take the following forms:
 *
 * <ol>
 * <li> Well Known Text definition - passed on to importFromWkt().
 * <li> "EPSG:n" - number passed on to importFromEPSG().
 * <li> "EPSGA:n" - number passed on to importFromEPSGA().
 * <li> "AUTO:proj_id,unit_id,lon0,lat0" - WMS auto projections.
 * <li> "urn:ogc:def:crs:EPSG::n" - ogc urns
 * <li> PROJ.4 definitions - passed on to importFromProj4().
 * <li> filename - file read for WKT, XML or PROJ.4 definition.
 * <li> well known name accepted by SetWellKnownGeogCS(), such as NAD27, NAD83,
 * WGS84 or WGS72.
 * <li> "IGNF:xxxx", "ESRI:xxxx", etc. from definitions from the PROJ database;
 * <li> PROJJSON (PROJ &gt;= 6.2)
 * </ol>
 *
 * It is expected that this method will be extended in the future to support
 * XML and perhaps a simplified "minilanguage" for indicating common UTM and
 * State Plane definitions.
 *
 * This method is intended to be flexible, but by its nature it is
 * imprecise as it must guess information about the format intended.  When
 * possible applications should call the specific method appropriate if the
 * input is known to be in a particular format.
 *
 * This method does the same thing as the OSRSetFromUserInput() function.
 *
 * @param pszDefinition text definition to try to deduce SRS from.
 *
 * @return OGRERR_NONE on success, or an error code if the name isn't
 * recognised, the definition is corrupt, or an EPSG value can't be
 * successfully looked up.
 */

OGRErr OGRSpatialReference::SetFromUserInput(const char *pszDefinition)
{
    return SetFromUserInput(pszDefinition, nullptr);
}

/**
 * \brief Set spatial reference from various text formats.
 *
 * This method will examine the provided input, and try to deduce the
 * format, and then use it to initialize the spatial reference system.  It
 * may take the following forms:
 *
 * <ol>
 * <li> Well Known Text definition - passed on to importFromWkt().
 * <li> "EPSG:n" - number passed on to importFromEPSG().
 * <li> "EPSGA:n" - number passed on to importFromEPSGA().
 * <li> "AUTO:proj_id,unit_id,lon0,lat0" - WMS auto projections.
 * <li> "urn:ogc:def:crs:EPSG::n" - ogc urns
 * <li> PROJ.4 definitions - passed on to importFromProj4().
 * <li> filename - file read for WKT, XML or PROJ.4 definition.
 * <li> well known name accepted by SetWellKnownGeogCS(), such as NAD27, NAD83,
 * WGS84 or WGS72.
 * <li> "IGNF:xxxx", "ESRI:xxxx", etc. from definitions from the PROJ database;
 * <li> PROJJSON (PROJ &gt;= 6.2)
 * </ol>
 *
 * It is expected that this method will be extended in the future to support
 * XML and perhaps a simplified "minilanguage" for indicating common UTM and
 * State Plane definitions.
 *
 * This method is intended to be flexible, but by its nature it is
 * imprecise as it must guess information about the format intended.  When
 * possible applications should call the specific method appropriate if the
 * input is known to be in a particular format.
 *
 * This method does the same thing as the OSRSetFromUserInput() and
 * OSRSetFromUserInputEx() functions.
 *
 * @param pszDefinition text definition to try to deduce SRS from.
 *
 * @param papszOptions NULL terminated list of options, or NULL.
 * <ol>
 * <li> ALLOW_NETWORK_ACCESS=YES/NO.
 *      Whether http:// or https:// access is allowed. Defaults to YES.
 * <li> ALLOW_FILE_ACCESS=YES/NO.
 *      Whether reading a file using the Virtual File System layer is allowed
 *      (can also involve network access). Defaults to YES.
 * </ol>
 *
 * @return OGRERR_NONE on success, or an error code if the name isn't
 * recognised, the definition is corrupt, or an EPSG value can't be
 * successfully looked up.
 */

OGRErr OGRSpatialReference::SetFromUserInput(const char *pszDefinition,
                                             CSLConstList papszOptions)
{
    TAKE_OPTIONAL_LOCK();

    // Skip leading white space
    while (isspace(static_cast<unsigned char>(*pszDefinition)))
        pszDefinition++;

    if (STARTS_WITH_CI(pszDefinition, "ESRI::"))
    {
        pszDefinition += 6;
    }

    /* -------------------------------------------------------------------- */
    /*      Is it a recognised syntax?                                      */
    /* -------------------------------------------------------------------- */
    const char *const wktKeywords[] = {
        // WKT1
        "GEOGCS", "GEOCCS", "PROJCS", "VERT_CS", "COMPD_CS", "LOCAL_CS",
        // WKT2"
        "GEODCRS", "GEOGCRS", "GEODETICCRS", "GEOGRAPHICCRS", "PROJCRS",
        "PROJECTEDCRS", "VERTCRS", "VERTICALCRS", "COMPOUNDCRS", "ENGCRS",
        "ENGINEERINGCRS", "BOUNDCRS", "DERIVEDPROJCRS", "COORDINATEMETADATA"};
    for (const char *keyword : wktKeywords)
    {
        if (STARTS_WITH_CI(pszDefinition, keyword))
        {
            return importFromWkt(pszDefinition);
        }
    }

    const bool bStartsWithEPSG = STARTS_WITH_CI(pszDefinition, "EPSG:");
    if (bStartsWithEPSG || STARTS_WITH_CI(pszDefinition, "EPSGA:"))
    {
        OGRErr eStatus = OGRERR_NONE;

        if (strchr(pszDefinition, '+') || strchr(pszDefinition, '@'))
        {
            // Use proj_create() as it allows things like EPSG:3157+4617
            // that are not normally supported by the below code that
            // builds manually a compound CRS
            PJ *pj = proj_create(d->getPROJContext(), pszDefinition);
            if (!pj)
            {
                return OGRERR_FAILURE;
            }
            Clear();
            d->setPjCRS(pj);
            return OGRERR_NONE;
        }
        else
        {
            eStatus =
                importFromEPSG(atoi(pszDefinition + (bStartsWithEPSG ? 5 : 6)));
        }

        return eStatus;
    }

    if (STARTS_WITH_CI(pszDefinition, "urn:ogc:def:crs:") ||
        STARTS_WITH_CI(pszDefinition, "urn:ogc:def:crs,crs:") ||
        STARTS_WITH_CI(pszDefinition, "urn:x-ogc:def:crs:") ||
        STARTS_WITH_CI(pszDefinition, "urn:opengis:crs:") ||
        STARTS_WITH_CI(pszDefinition, "urn:opengis:def:crs:") ||
        STARTS_WITH_CI(pszDefinition, "urn:ogc:def:coordinateMetadata:"))
#ifdef MECHSOFT_DEVIATION
        return importFromURN(pszDefinition);
#else
        return OGRERR_FAILURE;
#endif

    if (STARTS_WITH_CI(pszDefinition, "http://opengis.net/def/crs") ||
        STARTS_WITH_CI(pszDefinition, "https://opengis.net/def/crs") ||
        STARTS_WITH_CI(pszDefinition, "http://www.opengis.net/def/crs") ||
        STARTS_WITH_CI(pszDefinition, "https://www.opengis.net/def/crs") ||
        STARTS_WITH_CI(pszDefinition, "www.opengis.net/def/crs"))
#ifdef MECHSOFT_DEVIATION
        return importFromCRSURL(pszDefinition);
#else
        return OGRERR_FAILURE;
#endif

    if (STARTS_WITH_CI(pszDefinition, "AUTO:"))
#ifdef MECHSOFT_DEVIATION
        return importFromWMSAUTO(pszDefinition);
#else
        return OGRERR_FAILURE;
#endif

    // WMS/WCS OGC codes like OGC:CRS84.
    if (EQUAL(pszDefinition, "OGC:CRS84"))
        return SetWellKnownGeogCS(pszDefinition + 4);

    if (STARTS_WITH_CI(pszDefinition, "CRS:"))
        return SetWellKnownGeogCS(pszDefinition);

    if (STARTS_WITH_CI(pszDefinition, "DICT:") && strstr(pszDefinition, ","))
    {
        char *pszFile = CPLStrdup(pszDefinition + 5);
        char *pszCode = strstr(pszFile, ",") + 1;

        pszCode[-1] = '\0';

        OGRErr err = importFromDict(pszFile, pszCode);
        CPLFree(pszFile);

        return err;
    }

    if (EQUAL(pszDefinition, "NAD27") || EQUAL(pszDefinition, "NAD83") ||
        EQUAL(pszDefinition, "WGS84") || EQUAL(pszDefinition, "WGS72"))
    {
        Clear();
        return SetWellKnownGeogCS(pszDefinition);
    }

    // PROJJSON
    if (pszDefinition[0] == '{' && strstr(pszDefinition, "\"type\"") &&
        (strstr(pszDefinition, "GeodeticCRS") ||
         strstr(pszDefinition, "GeographicCRS") ||
         strstr(pszDefinition, "ProjectedCRS") ||
         strstr(pszDefinition, "VerticalCRS") ||
         strstr(pszDefinition, "BoundCRS") ||
         strstr(pszDefinition, "CompoundCRS") ||
         strstr(pszDefinition, "DerivedGeodeticCRS") ||
         strstr(pszDefinition, "DerivedGeographicCRS") ||
         strstr(pszDefinition, "DerivedProjectedCRS") ||
         strstr(pszDefinition, "DerivedVerticalCRS") ||
         strstr(pszDefinition, "EngineeringCRS") ||
         strstr(pszDefinition, "DerivedEngineeringCRS") ||
         strstr(pszDefinition, "ParametricCRS") ||
         strstr(pszDefinition, "DerivedParametricCRS") ||
         strstr(pszDefinition, "TemporalCRS") ||
         strstr(pszDefinition, "DerivedTemporalCRS")))
    {
        PJ *pj;
#ifdef MECHSOFT_DEVIATION
        if (strstr(pszDefinition, "datum_ensemble") != nullptr)
        {
            // PROJ < 9.0.1 doesn't like a datum_ensemble whose member have
            // a unknown id.
            CPLJSONDocument oCRSDoc;
            if (!oCRSDoc.LoadMemory(pszDefinition))
                return OGRERR_CORRUPT_DATA;
            CPLJSONObject oCRSRoot = oCRSDoc.GetRoot();
            RemoveIDFromMemberOfEnsembles(oCRSRoot);
            pj = proj_create(d->getPROJContext(), oCRSRoot.ToString().c_str());
        }
        else
#endif
        {
            pj = proj_create(d->getPROJContext(), pszDefinition);
        }
        if (!pj)
        {
            return OGRERR_FAILURE;
        }
        Clear();
        d->setPjCRS(pj);
        return OGRERR_NONE;
    }

    if (strstr(pszDefinition, "+proj") != nullptr ||
        strstr(pszDefinition, "+init") != nullptr)
        return importFromProj4(pszDefinition);

    if (STARTS_WITH_CI(pszDefinition, "http://") ||
        STARTS_WITH_CI(pszDefinition, "https://"))
    {
#ifdef MECHSOFT_DEVIATION
        if (CPLTestBool(CSLFetchNameValueDef(papszOptions,
                                             "ALLOW_NETWORK_ACCESS", "YES")))
            return importFromUrl(pszDefinition);
#endif

        CPLError(CE_Failure, CPLE_AppDefined,
                 "Cannot import %s due to ALLOW_NETWORK_ACCESS=NO",
                 pszDefinition);
        return OGRERR_FAILURE;
    }

    if (EQUAL(pszDefinition, "osgb:BNG"))
    {
        return importFromEPSG(27700);
    }

    // Used by German CityGML files
    if (EQUAL(pszDefinition, "urn:adv:crs:ETRS89_UTM32*DE_DHHN92_NH"))
    {
        // "ETRS89 / UTM Zone 32N + DHHN92 height"
        return SetFromUserInput("EPSG:25832+5783");
    }
    else if (EQUAL(pszDefinition, "urn:adv:crs:ETRS89_UTM32*DE_DHHN2016_NH"))
    {
        // "ETRS89 / UTM Zone 32N + DHHN2016 height"
        return SetFromUserInput("EPSG:25832+7837");
    }

    // Used by  Japan's Fundamental Geospatial Data (FGD) GML
    if (EQUAL(pszDefinition, "fguuid:jgd2001.bl"))
        return importFromEPSG(4612);  // JGD2000 (slight difference in years)
    else if (EQUAL(pszDefinition, "fguuid:jgd2011.bl"))
        return importFromEPSG(6668);  // JGD2011
    else if (EQUAL(pszDefinition, "fguuid:jgd2024.bl"))
    {
        // FIXME when EPSG attributes a CRS code
        return importFromWkt(
            "GEOGCRS[\"JGD2024\",\n"
            "    DATUM[\"Japanese Geodetic Datum 2024\",\n"
            "        ELLIPSOID[\"GRS 1980\",6378137,298.257222101,\n"
            "            LENGTHUNIT[\"metre\",1]]],\n"
            "    PRIMEM[\"Greenwich\",0,\n"
            "        ANGLEUNIT[\"degree\",0.0174532925199433]],\n"
            "    CS[ellipsoidal,2],\n"
            "        AXIS[\"geodetic latitude (Lat)\",north,\n"
            "            ORDER[1],\n"
            "            ANGLEUNIT[\"degree\",0.0174532925199433]],\n"
            "        AXIS[\"geodetic longitude (Lon)\",east,\n"
            "            ORDER[2],\n"
            "            ANGLEUNIT[\"degree\",0.0174532925199433]],\n"
            "    USAGE[\n"
            "        SCOPE[\"Horizontal component of 3D system.\"],\n"
            "        AREA[\"Japan - onshore and offshore.\"],\n"
            "        BBOX[17.09,122.38,46.05,157.65]]]");
    }

    // Deal with IGNF:xxx, ESRI:xxx, etc from the PROJ database
    const char *pszDot = strrchr(pszDefinition, ':');
    if (pszDot)
    {
        CPLString osPrefix(pszDefinition, pszDot - pszDefinition);
        auto authorities =
            proj_get_authorities_from_database(d->getPROJContext());
        if (authorities)
        {
            std::set<std::string> aosCandidateAuthorities;
            for (auto iter = authorities; *iter; ++iter)
            {
                if (*iter == osPrefix)
                {
                    aosCandidateAuthorities.clear();
                    aosCandidateAuthorities.insert(*iter);
                    break;
                }
                // Deal with "IAU_2015" as authority in the list and input
                // "IAU:code"
                else if (strncmp(*iter, osPrefix.c_str(), osPrefix.size()) ==
                             0 &&
                         (*iter)[osPrefix.size()] == '_')
                {
                    aosCandidateAuthorities.insert(*iter);
                }
                // Deal with "IAU_2015" as authority in the list and input
                // "IAU:2015:code"
                else if (osPrefix.find(':') != std::string::npos &&
                         osPrefix.size() == strlen(*iter) &&
                         CPLString(osPrefix).replaceAll(':', '_') == *iter)
                {
                    aosCandidateAuthorities.clear();
                    aosCandidateAuthorities.insert(*iter);
                    break;
                }
            }

            proj_string_list_destroy(authorities);

            if (!aosCandidateAuthorities.empty())
            {
                auto obj = proj_create_from_database(
                    d->getPROJContext(),
                    aosCandidateAuthorities.rbegin()->c_str(), pszDot + 1,
                    PJ_CATEGORY_CRS, false, nullptr);
                if (!obj)
                {
                    return OGRERR_FAILURE;
                }
                Clear();
                d->setPjCRS(obj);
                return OGRERR_NONE;
            }
        }
    }

    /* -------------------------------------------------------------------- */
    /*      Try to open it as a file.                                       */
    /* -------------------------------------------------------------------- */
    if (!CPLTestBool(
            CSLFetchNameValueDef(papszOptions, "ALLOW_FILE_ACCESS", "YES")))
    {
        VSIStatBufL sStat;
        if (STARTS_WITH(pszDefinition, "/vsi") ||
            VSIStatExL(pszDefinition, &sStat, VSI_STAT_EXISTS_FLAG) == 0)
        {
            CPLError(CE_Failure, CPLE_AppDefined,
                     "Cannot import %s due to ALLOW_FILE_ACCESS=NO",
                     pszDefinition);
            return OGRERR_FAILURE;
        }
        // We used to silently return an error without a CE_Failure message
        // Cf https://github.com/Toblerity/Fiona/issues/1063
        return OGRERR_CORRUPT_DATA;
    }

#ifdef MECHSOFT_DEVIATION
    CPLConfigOptionSetter oSetter("CPL_ALLOW_VSISTDIN", "NO", true);
#endif
    VSILFILE *const fp = VSIFOpenL(pszDefinition, "rt");
    if (fp == nullptr)
        return OGRERR_CORRUPT_DATA;

    const size_t nBufMax = 100000;
    char *const pszBuffer = static_cast<char *>(CPLMalloc(nBufMax));
    const size_t nBytes = VSIFReadL(pszBuffer, 1, nBufMax - 1, fp);
    VSIFCloseL(fp);

    if (nBytes == nBufMax - 1)
    {
        CPLDebug("OGR",
                 "OGRSpatialReference::SetFromUserInput(%s), opened file "
                 "but it is to large for our generous buffer.  Is it really "
                 "just a WKT definition?",
                 pszDefinition);
        CPLFree(pszBuffer);
        return OGRERR_FAILURE;
    }

    pszBuffer[nBytes] = '\0';

    char *pszBufPtr = pszBuffer;
    while (pszBufPtr[0] == ' ' || pszBufPtr[0] == '\n')
        pszBufPtr++;

    OGRErr err = OGRERR_NONE;
    if (pszBufPtr[0] == '<')
    {
#ifdef MECHSOFT_DEVIATION
        err = importFromXML(pszBufPtr);
#else
        err = OGRERR_FAILURE;
#endif
    }
    else if ((strstr(pszBuffer, "+proj") != nullptr ||
              strstr(pszBuffer, "+init") != nullptr) &&
             (strstr(pszBuffer, "EXTENSION") == nullptr &&
              strstr(pszBuffer, "extension") == nullptr))
        err = importFromProj4(pszBufPtr);
    else
    {
        if (STARTS_WITH_CI(pszBufPtr, "ESRI::"))
        {
            pszBufPtr += 6;
        }

        // coverity[tainted_data]
        err = importFromWkt(pszBufPtr);
    }

    CPLFree(pszBuffer);

    return err;
}

#ifdef MECHSOFT_DEVIATION
/************************************************************************/
/*                          ImportFromUrl()                             */
/************************************************************************/

/**
 * \brief Set spatial reference from a URL.
 *
 * This method will download the spatial reference at a given URL and
 * feed it into SetFromUserInput for you.
 *
 * This method does the same thing as the OSRImportFromUrl() function.
 *
 * @param pszUrl text definition to try to deduce SRS from.
 *
 * @return OGRERR_NONE on success, or an error code with the curl
 * error message if it is unable to download data.
 */

OGRErr OGRSpatialReference::importFromUrl(const char *pszUrl)

{
    TAKE_OPTIONAL_LOCK();

    if (!STARTS_WITH_CI(pszUrl, "http://") &&
        !STARTS_WITH_CI(pszUrl, "https://"))
    {
        CPLError(CE_Failure, CPLE_AppDefined,
                 "The given string is not recognized as a URL"
                 "starting with 'http://' -- %s",
                 pszUrl);
        return OGRERR_FAILURE;
    }

    /* -------------------------------------------------------------------- */
    /*      Fetch the result.                                               */
    /* -------------------------------------------------------------------- */
    CPLErrorReset();

    std::string osUrl(pszUrl);
    // We have historically supported "http://spatialreference.org/ref/AUTHNAME/CODE/"
    // as a valid URL since we used a "Accept: application/x-ogcwkt" header
    // to query WKT. To allow a static server to be used, rather append a
    // "ogcwkt/" suffix.
    for (const char *pszPrefix : {"https://spatialreference.org/ref/",
                                  "http://spatialreference.org/ref/"})
    {
        if (STARTS_WITH(pszUrl, pszPrefix))
        {
            const CPLStringList aosTokens(
                CSLTokenizeString2(pszUrl + strlen(pszPrefix), "/", 0));
            if (aosTokens.size() == 2)
            {
                osUrl = "https://spatialreference.org/ref/";
                osUrl += aosTokens[0];  // authority
                osUrl += '/';
                osUrl += aosTokens[1];  // code
                osUrl += "/ogcwkt/";
            }
            break;
        }
    }

    const char *pszTimeout = "TIMEOUT=10";
    char *apszOptions[] = {const_cast<char *>(pszTimeout), nullptr};

    CPLHTTPResult *psResult = CPLHTTPFetch(osUrl.c_str(), apszOptions);

    /* -------------------------------------------------------------------- */
    /*      Try to handle errors.                                           */
    /* -------------------------------------------------------------------- */

    if (psResult == nullptr)
        return OGRERR_FAILURE;
    if (psResult->nDataLen == 0 || CPLGetLastErrorNo() != 0 ||
        psResult->pabyData == nullptr)
    {
        if (CPLGetLastErrorNo() == 0)
        {
            CPLError(CE_Failure, CPLE_AppDefined,
                     "No data was returned from the given URL");
        }
        CPLHTTPDestroyResult(psResult);
        return OGRERR_FAILURE;
    }

    if (psResult->nStatus != 0)
    {
        CPLError(CE_Failure, CPLE_AppDefined, "Curl reports error: %d: %s",
                 psResult->nStatus, psResult->pszErrBuf);
        CPLHTTPDestroyResult(psResult);
        return OGRERR_FAILURE;
    }

    const char *pszData = reinterpret_cast<const char *>(psResult->pabyData);
    if (STARTS_WITH_CI(pszData, "http://") ||
        STARTS_WITH_CI(pszData, "https://"))
    {
        CPLError(CE_Failure, CPLE_AppDefined,
                 "The data that was downloaded also starts with 'http://' "
                 "and cannot be passed into SetFromUserInput.  Is this "
                 "really a spatial reference definition? ");
        CPLHTTPDestroyResult(psResult);
        return OGRERR_FAILURE;
    }
    if (OGRERR_NONE != SetFromUserInput(pszData))
    {
        CPLHTTPDestroyResult(psResult);
        return OGRERR_FAILURE;
    }

    CPLHTTPDestroyResult(psResult);
    return OGRERR_NONE;
}
#endif

/************************************************************************/
/*                         importFromURNPart()                          */
/************************************************************************/
OGRErr OGRSpatialReference::importFromURNPart(const char *pszAuthority,
                                              const char *pszCode,
                                              const char *pszURN)
{
#if PROJ_AT_LEAST_VERSION(8, 1, 0)
    (void)this;
    (void)pszAuthority;
    (void)pszCode;
    (void)pszURN;
    return OGRERR_FAILURE;
#else
    /* -------------------------------------------------------------------- */
    /*      Is this an EPSG code? Note that we import it with EPSG          */
    /*      preferred axis ordering for geographic coordinate systems.      */
    /* -------------------------------------------------------------------- */
    if (STARTS_WITH_CI(pszAuthority, "EPSG"))
        return importFromEPSGA(atoi(pszCode));

    /* -------------------------------------------------------------------- */
    /*      Is this an IAU code?  Lets try for the IAU2000 dictionary.      */
    /* -------------------------------------------------------------------- */
    if (STARTS_WITH_CI(pszAuthority, "IAU"))
        return importFromDict("IAU2000.wkt", pszCode);

    /* -------------------------------------------------------------------- */
    /*      Is this an OGC code?                                            */
    /* -------------------------------------------------------------------- */
    if (!STARTS_WITH_CI(pszAuthority, "OGC"))
    {
        CPLError(CE_Failure, CPLE_AppDefined,
                 "URN %s has unrecognized authority.", pszURN);
        return OGRERR_FAILURE;
    }

    if (STARTS_WITH_CI(pszCode, "CRS84"))
        return SetWellKnownGeogCS(pszCode);
    else if (STARTS_WITH_CI(pszCode, "CRS83"))
        return SetWellKnownGeogCS(pszCode);
    else if (STARTS_WITH_CI(pszCode, "CRS27"))
        return SetWellKnownGeogCS(pszCode);
    else if (STARTS_WITH_CI(pszCode, "84"))  // urn:ogc:def:crs:OGC:2:84
        return SetWellKnownGeogCS("CRS84");

    /* -------------------------------------------------------------------- */
    /*      Handle auto codes.  We need to convert from format              */
    /*      AUTO42001:99:8888 to format AUTO:42001,99,8888.                 */
    /* -------------------------------------------------------------------- */
    else if (STARTS_WITH_CI(pszCode, "AUTO"))
    {
        char szWMSAuto[100] = {'\0'};

        if (strlen(pszCode) > sizeof(szWMSAuto) - 2)
            return OGRERR_FAILURE;

        snprintf(szWMSAuto, sizeof(szWMSAuto), "AUTO:%s", pszCode + 4);
        for (int i = 5; szWMSAuto[i] != '\0'; i++)
        {
            if (szWMSAuto[i] == ':')
                szWMSAuto[i] = ',';
        }

        return importFromWMSAUTO(szWMSAuto);
    }

    /* -------------------------------------------------------------------- */
    /*      Not a recognise OGC item.                                       */
    /* -------------------------------------------------------------------- */
    CPLError(CE_Failure, CPLE_AppDefined, "URN %s value not supported.",
             pszURN);

    return OGRERR_FAILURE;
#endif
}

/************************************************************************/
/*                             SetGeocCS()                              */
/************************************************************************/

/**
 * \brief Set the user visible GEOCCS name.
 *
 * This method is the same as the C function OSRSetGeocCS().

 * This method will ensure a GEOCCS node is created as the root,
 * and set the provided name on it.  If used on a GEOGCS coordinate system,
 * the DATUM and PRIMEM nodes from the GEOGCS will be transferred over to
 * the GEOGCS.
 *
 * @param pszName the user visible name to assign.  Not used as a key.
 *
 * @return OGRERR_NONE on success.
 *
 * @since OGR 1.9.0
 */

OGRErr OGRSpatialReference::SetGeocCS(const char *pszName)

{
    TAKE_OPTIONAL_LOCK();

    OGRErr eErr = OGRERR_NONE;
    d->refreshProjObj();
    d->demoteFromBoundCRS();
    if (d->m_pjType == PJ_TYPE_UNKNOWN)
    {
        d->setPjCRS(proj_create_geocentric_crs(
            d->getPROJContext(), pszName, "World Geodetic System 1984",
            "WGS 84", SRS_WGS84_SEMIMAJOR, SRS_WGS84_INVFLATTENING,
            SRS_PM_GREENWICH, 0.0, SRS_UA_DEGREE, CPLAtof(SRS_UA_DEGREE_CONV),
            "Metre", 1.0));
    }
    else if (d->m_pjType == PJ_TYPE_GEOCENTRIC_CRS)
    {
        d->setPjCRS(proj_alter_name(d->getPROJContext(), d->m_pj_crs, pszName));
    }
    else if (d->m_pjType == PJ_TYPE_GEOGRAPHIC_2D_CRS ||
             d->m_pjType == PJ_TYPE_GEOGRAPHIC_3D_CRS)
    {
        auto datum = proj_crs_get_datum(d->getPROJContext(), d->m_pj_crs);
#if PROJ_VERSION_MAJOR > 7 ||                                                  \
    (PROJ_VERSION_MAJOR == 7 && PROJ_VERSION_MINOR >= 2)
        if (datum == nullptr)
        {
            datum =
                proj_crs_get_datum_ensemble(d->getPROJContext(), d->m_pj_crs);
        }
#endif
        if (datum == nullptr)
        {
            d->undoDemoteFromBoundCRS();
            return OGRERR_FAILURE;
        }

        auto pj_crs = proj_create_geocentric_crs_from_datum(
            d->getPROJContext(), proj_get_name(d->m_pj_crs), datum, nullptr,
            0.0);
        d->setPjCRS(pj_crs);

        proj_destroy(datum);
    }
    else
    {
        CPLDebug("OGR",
                 "OGRSpatialReference::SetGeocCS(%s) failed.  "
                 "It appears an incompatible object already exists.",
                 pszName);
        eErr = OGRERR_FAILURE;
    }
    d->undoDemoteFromBoundCRS();

    return eErr;
}

/************************************************************************/
/*                           importFromWkt()                            */
/************************************************************************/

/**
 * \brief Import from WKT string.
 *
 * This method will wipe the existing SRS definition, and
 * reassign it based on the contents of the passed WKT string.  Only as
 * much of the input string as needed to construct this SRS is consumed from
 * the input string, and the input string pointer
 * is then updated to point to the remaining (unused) input.
 *
 * Starting with PROJ 9.2, if invoked on a COORDINATEMETADATA[] construct,
 * the CRS contained in it will be used to fill the OGRSpatialReference object,
 * and the coordinate epoch potentially present used as the coordinate epoch
 * property of the OGRSpatialReference object.
 *
 * Consult also the <a href="wktproblems.html">OGC WKT Coordinate System
 * Issues</a> page for implementation details of WKT in OGR.
 *
 * This method is the same as the C function OSRImportFromWkt().
 *
 * @param ppszInput Pointer to pointer to input.  The pointer is updated to
 * point to remaining unused input text.
 *
 * @return OGRERR_NONE if import succeeds, or OGRERR_CORRUPT_DATA if it
 * fails for any reason.
 * @since GDAL 2.3
 */

OGRErr OGRSpatialReference::importFromWkt(const char **ppszInput)

{
    return importFromWkt(ppszInput, nullptr);
}

/************************************************************************/
/*                           importFromWkt()                            */
/************************************************************************/

/*! @cond Doxygen_Suppress */

OGRErr OGRSpatialReference::importFromWkt(const char *pszInput,
                                          CSLConstList papszOptions)

{
    return importFromWkt(&pszInput, papszOptions);
}

OGRErr OGRSpatialReference::importFromWkt(const char **ppszInput,
                                          CSLConstList papszOptions)

{
    TAKE_OPTIONAL_LOCK();

    if (!ppszInput || !*ppszInput)
        return OGRERR_FAILURE;

    if (strlen(*ppszInput) > 100 * 1000 &&
        CPLTestBool(CPLGetConfigOption("OSR_IMPORT_FROM_WKT_LIMIT", "YES")))
    {
        CPLError(CE_Failure, CPLE_NotSupported,
                 "Suspiciously large input for importFromWkt(). Rejecting it. "
                 "You can remove this limitation by definition the "
                 "OSR_IMPORT_FROM_WKT_LIMIT configuration option to NO.");
        return OGRERR_FAILURE;
    }

    Clear();

    bool canCache = false;
#ifdef MECHSOFT_DEVIATION
    auto tlsCache = OSRGetProjTLSCache();
#endif
    std::string osWkt;
    if (**ppszInput)
    {
        osWkt = *ppszInput;
#ifdef MECHSOFT_DEVIATION
        auto cachedObj = tlsCache->GetPJForWKT(osWkt);
        if (cachedObj)
        {
            d->setPjCRS(cachedObj);
        }
        else
#endif
        {
            CPLStringList aosOptions(papszOptions);
            if (aosOptions.FetchNameValue("STRICT") == nullptr)
                aosOptions.SetNameValue("STRICT", "NO");
            PROJ_STRING_LIST warnings = nullptr;
            PROJ_STRING_LIST errors = nullptr;
            auto ctxt = d->getPROJContext();
            auto pj = proj_create_from_wkt(ctxt, *ppszInput, aosOptions.List(),
                                           &warnings, &errors);
            d->setPjCRS(pj);

            for (auto iter = warnings; iter && *iter; ++iter)
            {
                d->m_wktImportWarnings.push_back(*iter);
            }
            for (auto iter = errors; iter && *iter; ++iter)
            {
                d->m_wktImportErrors.push_back(*iter);
                if (!d->m_pj_crs)
                {
                    CPLError(CE_Failure, CPLE_AppDefined, "%s", *iter);
                }
            }
            if (warnings == nullptr && errors == nullptr)
            {
                canCache = true;
            }
            proj_string_list_destroy(warnings);
            proj_string_list_destroy(errors);
        }
    }
    if (!d->m_pj_crs)
        return OGRERR_CORRUPT_DATA;

    // Only accept CRS objects
    if (!proj_is_crs(d->m_pj_crs))
    {
        Clear();
        return OGRERR_CORRUPT_DATA;
    }
    
#ifdef MECHSOFT_DEVIATION
    if (canCache)
    {
        tlsCache->CachePJForWKT(osWkt, d->m_pj_crs);
    }
#endif

    if (strstr(*ppszInput, "CENTER_LONG"))
    {
        auto poRoot = new OGR_SRSNode();
        d->setRoot(poRoot);
        const char *pszTmp = *ppszInput;
        poRoot->importFromWkt(&pszTmp);
        d->m_bHasCenterLong = true;
    }

    // TODO? we don't really update correctly since we assume that the
    // passed string is only WKT.
    *ppszInput += strlen(*ppszInput);
    return OGRERR_NONE;

#if no_longer_implemented_for_now
    /* -------------------------------------------------------------------- */
    /*      The following seems to try and detect and unconsumed            */
    /*      VERTCS[] coordinate system definition (ESRI style) and to       */
    /*      import and attach it to the existing root.  Likely we will      */
    /*      need to extend this somewhat to bring it into an acceptable     */
    /*      OGRSpatialReference organization at some point.                 */
    /* -------------------------------------------------------------------- */
    if (strlen(*ppszInput) > 0 && strstr(*ppszInput, "VERTCS"))
    {
        if (((*ppszInput)[0]) == ',')
            (*ppszInput)++;
        OGR_SRSNode *poNewChild = new OGR_SRSNode();
        poRoot->AddChild(poNewChild);
        return poNewChild->importFromWkt(ppszInput);
    }
#endif
}

/*! @endcond */

/**
 * \brief Import from WKT string.
 *
 * This method will wipe the existing SRS definition, and
 * reassign it based on the contents of the passed WKT string.  Only as
 * much of the input string as needed to construct this SRS is consumed from
 * the input string, and the input string pointer
 * is then updated to point to the remaining (unused) input.
 *
 * Consult also the <a href="wktproblems.html">OGC WKT Coordinate System
 * Issues</a> page for implementation details of WKT in OGR.
 *
 * This method is the same as the C function OSRImportFromWkt().
 *
 * @param ppszInput Pointer to pointer to input.  The pointer is updated to
 * point to remaining unused input text.
 *
 * @return OGRERR_NONE if import succeeds, or OGRERR_CORRUPT_DATA if it
 * fails for any reason.
 * @deprecated GDAL 2.3. Use importFromWkt(const char**) or importFromWkt(const
 * char*)
 */

OGRErr OGRSpatialReference::importFromWkt(char **ppszInput)

{
    return importFromWkt(const_cast<const char **>(ppszInput));
}

/**
 * \brief Import from WKT string.
 *
 * This method will wipe the existing SRS definition, and
 * reassign it based on the contents of the passed WKT string.  Only as
 * much of the input string as needed to construct this SRS is consumed from
 * the input string, and the input string pointer
 * is then updated to point to the remaining (unused) input.
 *
 * Consult also the <a href="wktproblems.html">OGC WKT Coordinate System
 * Issues</a> page for implementation details of WKT in OGR.
 *
 * @param pszInput Input WKT
 *
 * @return OGRERR_NONE if import succeeds, or OGRERR_CORRUPT_DATA if it
 * fails for any reason.
 * @since GDAL 2.3
 */

OGRErr OGRSpatialReference::importFromWkt(const char *pszInput)
{
    return importFromWkt(&pszInput);
}

/************************************************************************/
/*                             SetLocalCS()                             */
/************************************************************************/

/**
 * \brief Set the user visible LOCAL_CS name.
 *
 * This method is the same as the C function OSRSetLocalCS().
 *
 * This method will ensure a LOCAL_CS node is created as the root,
 * and set the provided name on it.  It must be used before SetLinearUnits().
 *
 * @param pszName the user visible name to assign.  Not used as a key.
 *
 * @return OGRERR_NONE on success.
 */

OGRErr OGRSpatialReference::SetLocalCS(const char *pszName)

{
    TAKE_OPTIONAL_LOCK();

    if (d->m_pjType == PJ_TYPE_UNKNOWN ||
        d->m_pjType == PJ_TYPE_ENGINEERING_CRS)
    {
        d->setPjCRS(proj_create_engineering_crs(d->getPROJContext(), pszName));
    }
    else
    {
        CPLDebug("OGR",
                 "OGRSpatialReference::SetLocalCS(%s) failed.  "
                 "It appears an incompatible object already exists.",
                 pszName);
        return OGRERR_FAILURE;
    }

    return OGRERR_NONE;
}

/************************************************************************/
/*                             SetProjCS()                              */
/************************************************************************/

/**
 * \brief Set the user visible PROJCS name.
 *
 * This method is the same as the C function OSRSetProjCS().
 *
 * This method will ensure a PROJCS node is created as the root,
 * and set the provided name on it.  If used on a GEOGCS coordinate system,
 * the GEOGCS node will be demoted to be a child of the new PROJCS root.
 *
 * @param pszName the user visible name to assign.  Not used as a key.
 *
 * @return OGRERR_NONE on success.
 */

OGRErr OGRSpatialReference::SetProjCS(const char *pszName)

{
    TAKE_OPTIONAL_LOCK();

    d->refreshProjObj();
    d->demoteFromBoundCRS();
    if (d->m_pjType == PJ_TYPE_PROJECTED_CRS)
    {
        d->setPjCRS(proj_alter_name(d->getPROJContext(), d->m_pj_crs, pszName));
    }
    else
    {
        auto dummyConv = proj_create_conversion(d->getPROJContext(), nullptr,
                                                nullptr, nullptr, nullptr,
                                                nullptr, nullptr, 0, nullptr);
        auto cs = proj_create_cartesian_2D_cs(
            d->getPROJContext(), PJ_CART2D_EASTING_NORTHING, nullptr, 0);

        auto projCRS = proj_create_projected_crs(
            d->getPROJContext(), pszName, d->getGeodBaseCRS(), dummyConv, cs);
        proj_destroy(dummyConv);
        proj_destroy(cs);

        d->setPjCRS(projCRS);
    }
    d->undoDemoteFromBoundCRS();
    return OGRERR_NONE;
}

/************************************************************************/
/*                           SetProjection()                            */
/************************************************************************/

/**
 * \brief Set a projection name.
 *
 * This method is the same as the C function OSRSetProjection().
 *
 * @param pszProjection the projection name, which should be selected from
 * the macros in ogr_srs_api.h, such as SRS_PT_TRANSVERSE_MERCATOR.
 *
 * @return OGRERR_NONE on success.
 */

OGRErr OGRSpatialReference::SetProjection(const char *pszProjection)

{
    TAKE_OPTIONAL_LOCK();

    OGR_SRSNode *poGeogCS = nullptr;

    if (GetRoot() != nullptr && EQUAL(d->m_poRoot->GetValue(), "GEOGCS"))
    {
        poGeogCS = d->m_poRoot;
        d->m_poRoot = nullptr;
    }

    if (!GetAttrNode("PROJCS"))
    {
        SetNode("PROJCS", "unnamed");
    }

    const OGRErr eErr = SetNode("PROJCS|PROJECTION", pszProjection);
    if (eErr != OGRERR_NONE)
        return eErr;

    if (poGeogCS != nullptr)
        d->m_poRoot->InsertChild(poGeogCS, 1);

    return OGRERR_NONE;
}

/************************************************************************/
/*                            SetProjParm()                             */
/************************************************************************/

/**
 * \brief Set a projection parameter value.
 *
 * Adds a new PARAMETER under the PROJCS with the indicated name and value.
 *
 * This method is the same as the C function OSRSetProjParm().
 *
 * Please check https://gdal.org/proj_list pages for
 * legal parameter names for specific projections.
 *
 *
 * @param pszParamName the parameter name, which should be selected from
 * the macros in ogr_srs_api.h, such as SRS_PP_CENTRAL_MERIDIAN.
 *
 * @param dfValue value to assign.
 *
 * @return OGRERR_NONE on success.
 */

OGRErr OGRSpatialReference::SetProjParm(const char *pszParamName,
                                        double dfValue)

{
    TAKE_OPTIONAL_LOCK();

    OGR_SRSNode *poPROJCS = GetAttrNode("PROJCS");

    if (poPROJCS == nullptr)
        return OGRERR_FAILURE;

    char szValue[64] = {'\0'};
    OGRsnPrintDouble(szValue, sizeof(szValue), dfValue);

    /* -------------------------------------------------------------------- */
    /*      Try to find existing parameter with this name.                  */
    /* -------------------------------------------------------------------- */
    for (int iChild = 0; iChild < poPROJCS->GetChildCount(); iChild++)
    {
        OGR_SRSNode *poParam = poPROJCS->GetChild(iChild);

        if (EQUAL(poParam->GetValue(), "PARAMETER") &&
            poParam->GetChildCount() == 2 &&
            EQUAL(poParam->GetChild(0)->GetValue(), pszParamName))
        {
            poParam->GetChild(1)->SetValue(szValue);
            return OGRERR_NONE;
        }
    }

    /* -------------------------------------------------------------------- */
    /*      Otherwise create a new parameter and append.                    */
    /* -------------------------------------------------------------------- */
    OGR_SRSNode *poParam = new OGR_SRSNode("PARAMETER");
    poParam->AddChild(new OGR_SRSNode(pszParamName));
    poParam->AddChild(new OGR_SRSNode(szValue));

    poPROJCS->AddChild(poParam);

    return OGRERR_NONE;
}
/************************************************************************/
/*                            FindProjParm()                            */
/************************************************************************/

/**
 * \brief Return the child index of the named projection parameter on
 * its parent PROJCS node.
 *
 * @param pszParameter projection parameter to look for
 * @param poPROJCS projection CS node to look in. If NULL is passed,
 *        the PROJCS node of the SpatialReference object will be searched.
 *
 * @return the child index of the named projection parameter. -1 on failure
 */
int OGRSpatialReference::FindProjParm(const char *pszParameter,
                                      const OGR_SRSNode *poPROJCS) const

{
    TAKE_OPTIONAL_LOCK();

    if (poPROJCS == nullptr)
        poPROJCS = GetAttrNode("PROJCS");

    if (poPROJCS == nullptr)
        return -1;

    /* -------------------------------------------------------------------- */
    /*      Search for requested parameter.                                 */
    /* -------------------------------------------------------------------- */
    bool bIsWKT2 = false;
    for (int iChild = 0; iChild < poPROJCS->GetChildCount(); iChild++)
    {
        const OGR_SRSNode *poParameter = poPROJCS->GetChild(iChild);

        if (poParameter->GetChildCount() >= 2)
        {
            const char *pszValue = poParameter->GetValue();
            if (EQUAL(pszValue, "PARAMETER") &&
                EQUAL(poPROJCS->GetChild(iChild)->GetChild(0)->GetValue(),
                      pszParameter))
            {
                return iChild;
            }
            else if (EQUAL(pszValue, "METHOD"))
            {
                bIsWKT2 = true;
            }
        }
    }

    /* -------------------------------------------------------------------- */
    /*      Try similar names, for selected parameters.                     */
    /* -------------------------------------------------------------------- */
    if (EQUAL(pszParameter, SRS_PP_LATITUDE_OF_ORIGIN))
    {
        if (bIsWKT2)
        {
            int iChild = FindProjParm(
                EPSG_NAME_PARAMETER_LATITUDE_OF_NATURAL_ORIGIN, poPROJCS);
            if (iChild == -1)
                iChild = FindProjParm(
                    EPSG_NAME_PARAMETER_LATITUDE_PROJECTION_CENTRE, poPROJCS);
            return iChild;
        }
        return FindProjParm(SRS_PP_LATITUDE_OF_CENTER, poPROJCS);
    }

    if (EQUAL(pszParameter, SRS_PP_CENTRAL_MERIDIAN))
    {
        if (bIsWKT2)
        {
            int iChild = FindProjParm(
                EPSG_NAME_PARAMETER_LONGITUDE_OF_NATURAL_ORIGIN, poPROJCS);
            if (iChild == -1)
                iChild = FindProjParm(
                    EPSG_NAME_PARAMETER_LONGITUDE_PROJECTION_CENTRE, poPROJCS);
            return iChild;
        }
        int iChild = FindProjParm(SRS_PP_LONGITUDE_OF_CENTER, poPROJCS);
        if (iChild == -1)
            iChild = FindProjParm(SRS_PP_LONGITUDE_OF_ORIGIN, poPROJCS);
        return iChild;
    }

    return -1;
}

/************************************************************************/
/*                            GetProjParm()                             */
/************************************************************************/

/**
 * \brief Fetch a projection parameter value.
 *
 * NOTE: This code should be modified to translate non degree angles into
 * degrees based on the GEOGCS unit.  This has not yet been done.
 *
 * This method is the same as the C function OSRGetProjParm().
 *
 * @param pszName the name of the parameter to fetch, from the set of
 * SRS_PP codes in ogr_srs_api.h.
 *
 * @param dfDefaultValue the value to return if this parameter doesn't exist.
 *
 * @param pnErr place to put error code on failure.  Ignored if NULL.
 *
 * @return value of parameter.
 */

double OGRSpatialReference::GetProjParm(const char *pszName,
                                        double dfDefaultValue,
                                        OGRErr *pnErr) const

{
    TAKE_OPTIONAL_LOCK();

    d->refreshProjObj();
    GetRoot();  // force update of d->m_bNodesWKT2

    if (pnErr != nullptr)
        *pnErr = OGRERR_NONE;

    /* -------------------------------------------------------------------- */
    /*      Find the desired parameter.                                     */
    /* -------------------------------------------------------------------- */
    const OGR_SRSNode *poPROJCS =
        GetAttrNode(d->m_bNodesWKT2 ? "CONVERSION" : "PROJCS");
    if (poPROJCS == nullptr)
    {
        if (pnErr != nullptr)
            *pnErr = OGRERR_FAILURE;
        return dfDefaultValue;
    }

    const int iChild = FindProjParm(pszName, poPROJCS);
    if (iChild == -1)
    {
        if (IsProjected() && GetAxesCount() == 3)
        {
            OGRSpatialReference *poSRSTmp = Clone();
            poSRSTmp->DemoteTo2D(nullptr);
            const double dfRet =
                poSRSTmp->GetProjParm(pszName, dfDefaultValue, pnErr);
            delete poSRSTmp;
            return dfRet;
        }

        if (pnErr != nullptr)
            *pnErr = OGRERR_FAILURE;
        return dfDefaultValue;
    }

    const OGR_SRSNode *poParameter = poPROJCS->GetChild(iChild);
    return CPLAtof(poParameter->GetChild(1)->GetValue());
}

/************************************************************************/
/*                          GetNormProjParm()                           */
/************************************************************************/

/**
 * \brief Fetch a normalized projection parameter value.
 *
 * This method is the same as GetProjParm() except that the value of
 * the parameter is "normalized" into degrees or meters depending on
 * whether it is linear or angular.
 *
 * This method is the same as the C function OSRGetNormProjParm().
 *
 * @param pszName the name of the parameter to fetch, from the set of
 * SRS_PP codes in ogr_srs_api.h.
 *
 * @param dfDefaultValue the value to return if this parameter doesn't exist.
 *
 * @param pnErr place to put error code on failure.  Ignored if NULL.
 *
 * @return value of parameter.
 */

double OGRSpatialReference::GetNormProjParm(const char *pszName,
                                            double dfDefaultValue,
                                            OGRErr *pnErr) const

{
    TAKE_OPTIONAL_LOCK();

    GetNormInfo();

    OGRErr nError = OGRERR_NONE;
    double dfRawResult = GetProjParm(pszName, dfDefaultValue, &nError);
    if (pnErr != nullptr)
        *pnErr = nError;

    // If we got the default just return it unadjusted.
    if (nError != OGRERR_NONE)
        return dfRawResult;

    if (d->dfToDegrees != 1.0 && IsAngularParameter(pszName))
        dfRawResult *= d->dfToDegrees;

    if (d->dfToMeter != 1.0 && IsLinearParameter(pszName))
        return dfRawResult * d->dfToMeter;

    return dfRawResult;
}

/************************************************************************/
/*                          SetNormProjParm()                           */
/************************************************************************/

/**
 * \brief Set a projection parameter with a normalized value.
 *
 * This method is the same as SetProjParm() except that the value of
 * the parameter passed in is assumed to be in "normalized" form (decimal
 * degrees for angular values, meters for linear values.  The values are
 * converted in a form suitable for the GEOGCS and linear units in effect.
 *
 * This method is the same as the C function OSRSetNormProjParm().
 *
 * @param pszName the parameter name, which should be selected from
 * the macros in ogr_srs_api.h, such as SRS_PP_CENTRAL_MERIDIAN.
 *
 * @param dfValue value to assign.
 *
 * @return OGRERR_NONE on success.
 */

OGRErr OGRSpatialReference::SetNormProjParm(const char *pszName, double dfValue)

{
    TAKE_OPTIONAL_LOCK();

    GetNormInfo();

    if (d->dfToDegrees != 0.0 &&
        (d->dfToDegrees != 1.0 || d->dfFromGreenwich != 0.0) &&
        IsAngularParameter(pszName))
    {
        dfValue /= d->dfToDegrees;
    }
    else if (d->dfToMeter != 1.0 && d->dfToMeter != 0.0 &&
             IsLinearParameter(pszName))
        dfValue /= d->dfToMeter;

    return SetProjParm(pszName, dfValue);
}

/************************************************************************/
/*                               SetTM()                                */
/************************************************************************/

OGRErr OGRSpatialReference::SetTM(double dfCenterLat, double dfCenterLong,
                                  double dfScale, double dfFalseEasting,
                                  double dfFalseNorthing)

{
    TAKE_OPTIONAL_LOCK();

    return d->replaceConversionAndUnref(
        proj_create_conversion_transverse_mercator(
            d->getPROJContext(), dfCenterLat, dfCenterLong, dfScale,
            dfFalseEasting, dfFalseNorthing, nullptr, 0.0, nullptr, 0.0));
}

/************************************************************************/
/*                            SetTMVariant()                            */
/************************************************************************/

OGRErr OGRSpatialReference::SetTMVariant(const char *pszVariantName,
                                         double dfCenterLat,
                                         double dfCenterLong, double dfScale,
                                         double dfFalseEasting,
                                         double dfFalseNorthing)

{
    TAKE_OPTIONAL_LOCK();

    SetProjection(pszVariantName);
    SetNormProjParm(SRS_PP_LATITUDE_OF_ORIGIN, dfCenterLat);
    SetNormProjParm(SRS_PP_CENTRAL_MERIDIAN, dfCenterLong);
    SetNormProjParm(SRS_PP_SCALE_FACTOR, dfScale);
    SetNormProjParm(SRS_PP_FALSE_EASTING, dfFalseEasting);
    SetNormProjParm(SRS_PP_FALSE_NORTHING, dfFalseNorthing);

    return OGRERR_NONE;
}

/************************************************************************/
/*                              SetTMSO()                               */
/************************************************************************/

OGRErr OGRSpatialReference::SetTMSO(double dfCenterLat, double dfCenterLong,
                                    double dfScale, double dfFalseEasting,
                                    double dfFalseNorthing)

{
    TAKE_OPTIONAL_LOCK();

    auto conv = proj_create_conversion_transverse_mercator_south_oriented(
        d->getPROJContext(), dfCenterLat, dfCenterLong, dfScale, dfFalseEasting,
        dfFalseNorthing, nullptr, 0.0, nullptr, 0.0);

    const char *pszName = nullptr;
    double dfConvFactor = GetTargetLinearUnits(nullptr, &pszName);
    CPLString osName = pszName ? pszName : "";

    d->refreshProjObj();

    d->demoteFromBoundCRS();

    auto cs = proj_create_cartesian_2D_cs(
        d->getPROJContext(), PJ_CART2D_WESTING_SOUTHING,
        !osName.empty() ? osName.c_str() : nullptr, dfConvFactor);
    auto projCRS =
        proj_create_projected_crs(d->getPROJContext(), d->getProjCRSName(),
                                  d->getGeodBaseCRS(), conv, cs);
    proj_destroy(conv);
    proj_destroy(cs);

    d->setPjCRS(projCRS);

    d->undoDemoteFromBoundCRS();

    return OGRERR_NONE;
}

/************************************************************************/
/*                              SetTPED()                               */
/************************************************************************/

OGRErr OGRSpatialReference::SetTPED(double dfLat1, double dfLong1,
                                    double dfLat2, double dfLong2,
                                    double dfFalseEasting,
                                    double dfFalseNorthing)

{
    TAKE_OPTIONAL_LOCK();

    return d->replaceConversionAndUnref(
        proj_create_conversion_two_point_equidistant(
            d->getPROJContext(), dfLat1, dfLong1, dfLat2, dfLong2,
            dfFalseEasting, dfFalseNorthing, nullptr, 0.0, nullptr, 0.0));
}

/************************************************************************/
/*                               SetTMG()                               */
/************************************************************************/

OGRErr OGRSpatialReference::SetTMG(double dfCenterLat, double dfCenterLong,
                                   double dfFalseEasting,
                                   double dfFalseNorthing)

{
    TAKE_OPTIONAL_LOCK();

    return d->replaceConversionAndUnref(
        proj_create_conversion_tunisia_mapping_grid(
            d->getPROJContext(), dfCenterLat, dfCenterLong, dfFalseEasting,
            dfFalseNorthing, nullptr, 0.0, nullptr, 0.0));
}

/************************************************************************/
/*                              SetACEA()                               */
/************************************************************************/

OGRErr OGRSpatialReference::SetACEA(double dfStdP1, double dfStdP2,
                                    double dfCenterLat, double dfCenterLong,
                                    double dfFalseEasting,
                                    double dfFalseNorthing)

{
    TAKE_OPTIONAL_LOCK();

    // Note different order of parameters. The one in PROJ is conformant with
    // EPSG
    return d->replaceConversionAndUnref(
        proj_create_conversion_albers_equal_area(
            d->getPROJContext(), dfCenterLat, dfCenterLong, dfStdP1, dfStdP2,
            dfFalseEasting, dfFalseNorthing, nullptr, 0.0, nullptr, 0.0));
}

/************************************************************************/
/*                               SetAE()                                */
/************************************************************************/

OGRErr OGRSpatialReference::SetAE(double dfCenterLat, double dfCenterLong,
                                  double dfFalseEasting, double dfFalseNorthing)

{
    TAKE_OPTIONAL_LOCK();

    return d->replaceConversionAndUnref(
        proj_create_conversion_azimuthal_equidistant(
            d->getPROJContext(), dfCenterLat, dfCenterLong, dfFalseEasting,
            dfFalseNorthing, nullptr, 0.0, nullptr, 0.0));
}

/************************************************************************/
/*                              SetBonne()                              */
/************************************************************************/

OGRErr OGRSpatialReference::SetBonne(double dfStdP1, double dfCentralMeridian,
                                     double dfFalseEasting,
                                     double dfFalseNorthing)

{
    TAKE_OPTIONAL_LOCK();

    return d->replaceConversionAndUnref(proj_create_conversion_bonne(
        d->getPROJContext(), dfStdP1, dfCentralMeridian, dfFalseEasting,
        dfFalseNorthing, nullptr, 0.0, nullptr, 0.0));
}

/************************************************************************/
/*                               SetCEA()                               */
/************************************************************************/

OGRErr OGRSpatialReference::SetCEA(double dfStdP1, double dfCentralMeridian,
                                   double dfFalseEasting,
                                   double dfFalseNorthing)

{
    TAKE_OPTIONAL_LOCK();

    return d->replaceConversionAndUnref(
        proj_create_conversion_lambert_cylindrical_equal_area(
            d->getPROJContext(), dfStdP1, dfCentralMeridian, dfFalseEasting,
            dfFalseNorthing, nullptr, 0.0, nullptr, 0.0));
}

/************************************************************************/
/*                               SetCS()                                */
/************************************************************************/

OGRErr OGRSpatialReference::SetCS(double dfCenterLat, double dfCenterLong,
                                  double dfFalseEasting, double dfFalseNorthing)

{
    TAKE_OPTIONAL_LOCK();

    return d->replaceConversionAndUnref(proj_create_conversion_cassini_soldner(
        d->getPROJContext(), dfCenterLat, dfCenterLong, dfFalseEasting,
        dfFalseNorthing, nullptr, 0.0, nullptr, 0.0));
}

/************************************************************************/
/*                               SetEC()                                */
/************************************************************************/

OGRErr OGRSpatialReference::SetEC(double dfStdP1, double dfStdP2,
                                  double dfCenterLat, double dfCenterLong,
                                  double dfFalseEasting, double dfFalseNorthing)

{
    TAKE_OPTIONAL_LOCK();

    // Note: different order of arguments
    return d->replaceConversionAndUnref(
        proj_create_conversion_equidistant_conic(
            d->getPROJContext(), dfCenterLat, dfCenterLong, dfStdP1, dfStdP2,
            dfFalseEasting, dfFalseNorthing, nullptr, 0.0, nullptr, 0.0));
}

/************************************************************************/
/*                             SetEckert()                              */
/************************************************************************/

OGRErr OGRSpatialReference::SetEckert(int nVariation,  // 1-6.
                                      double dfCentralMeridian,
                                      double dfFalseEasting,
                                      double dfFalseNorthing)

{
    TAKE_OPTIONAL_LOCK();

    PJ *conv;
    if (nVariation == 1)
    {
        conv = proj_create_conversion_eckert_i(
            d->getPROJContext(), dfCentralMeridian, dfFalseEasting,
            dfFalseNorthing, nullptr, 0.0, nullptr, 0.0);
    }
    else if (nVariation == 2)
    {
        conv = proj_create_conversion_eckert_ii(
            d->getPROJContext(), dfCentralMeridian, dfFalseEasting,
            dfFalseNorthing, nullptr, 0.0, nullptr, 0.0);
    }
    else if (nVariation == 3)
    {
        conv = proj_create_conversion_eckert_iii(
            d->getPROJContext(), dfCentralMeridian, dfFalseEasting,
            dfFalseNorthing, nullptr, 0.0, nullptr, 0.0);
    }
    else if (nVariation == 4)
    {
        conv = proj_create_conversion_eckert_iv(
            d->getPROJContext(), dfCentralMeridian, dfFalseEasting,
            dfFalseNorthing, nullptr, 0.0, nullptr, 0.0);
    }
    else if (nVariation == 5)
    {
        conv = proj_create_conversion_eckert_v(
            d->getPROJContext(), dfCentralMeridian, dfFalseEasting,
            dfFalseNorthing, nullptr, 0.0, nullptr, 0.0);
    }
    else if (nVariation == 6)
    {
        conv = proj_create_conversion_eckert_vi(
            d->getPROJContext(), dfCentralMeridian, dfFalseEasting,
            dfFalseNorthing, nullptr, 0.0, nullptr, 0.0);
    }
    else
    {
        CPLError(CE_Failure, CPLE_AppDefined,
                 "Unsupported Eckert variation (%d).", nVariation);
        return OGRERR_UNSUPPORTED_SRS;
    }

    return d->replaceConversionAndUnref(conv);
}

/************************************************************************/
/*                            SetEckertIV()                             */
/*                                                                      */
/*      Deprecated                                                      */
/************************************************************************/

OGRErr OGRSpatialReference::SetEckertIV(double dfCentralMeridian,
                                        double dfFalseEasting,
                                        double dfFalseNorthing)

{
    return SetEckert(4, dfCentralMeridian, dfFalseEasting, dfFalseNorthing);
}

/************************************************************************/
/*                            SetEckertVI()                             */
/*                                                                      */
/*      Deprecated                                                      */
/************************************************************************/

OGRErr OGRSpatialReference::SetEckertVI(double dfCentralMeridian,
                                        double dfFalseEasting,
                                        double dfFalseNorthing)

{
    return SetEckert(6, dfCentralMeridian, dfFalseEasting, dfFalseNorthing);
}

/************************************************************************/
/*                         SetEquirectangular()                         */
/************************************************************************/

OGRErr OGRSpatialReference::SetEquirectangular(double dfCenterLat,
                                               double dfCenterLong,
                                               double dfFalseEasting,
                                               double dfFalseNorthing)

{
    TAKE_OPTIONAL_LOCK();

    if (dfCenterLat == 0.0)
    {
        return d->replaceConversionAndUnref(
            proj_create_conversion_equidistant_cylindrical(
                d->getPROJContext(), 0.0, dfCenterLong, dfFalseEasting,
                dfFalseNorthing, nullptr, 0.0, nullptr, 0.0));
    }

    // Non-standard extension with non-zero latitude of origin
    SetProjection(SRS_PT_EQUIRECTANGULAR);
    SetNormProjParm(SRS_PP_LATITUDE_OF_ORIGIN, dfCenterLat);
    SetNormProjParm(SRS_PP_CENTRAL_MERIDIAN, dfCenterLong);
    SetNormProjParm(SRS_PP_FALSE_EASTING, dfFalseEasting);
    SetNormProjParm(SRS_PP_FALSE_NORTHING, dfFalseNorthing);

    return OGRERR_NONE;
}

/************************************************************************/
/*                         SetEquirectangular2()                        */
/* Generalized form                                                     */
/************************************************************************/

OGRErr OGRSpatialReference::SetEquirectangular2(double dfCenterLat,
                                                double dfCenterLong,
                                                double dfStdParallel1,
                                                double dfFalseEasting,
                                                double dfFalseNorthing)

{
    TAKE_OPTIONAL_LOCK();

    if (dfCenterLat == 0.0)
    {
        return d->replaceConversionAndUnref(
            proj_create_conversion_equidistant_cylindrical(
                d->getPROJContext(), dfStdParallel1, dfCenterLong,
                dfFalseEasting, dfFalseNorthing, nullptr, 0.0, nullptr, 0.0));
    }

    // Non-standard extension with non-zero latitude of origin
    SetProjection(SRS_PT_EQUIRECTANGULAR);
    SetNormProjParm(SRS_PP_LATITUDE_OF_ORIGIN, dfCenterLat);
    SetNormProjParm(SRS_PP_CENTRAL_MERIDIAN, dfCenterLong);
    SetNormProjParm(SRS_PP_STANDARD_PARALLEL_1, dfStdParallel1);
    SetNormProjParm(SRS_PP_FALSE_EASTING, dfFalseEasting);
    SetNormProjParm(SRS_PP_FALSE_NORTHING, dfFalseNorthing);

    return OGRERR_NONE;
}

/************************************************************************/
/*                               SetGS()                                */
/************************************************************************/

OGRErr OGRSpatialReference::SetGS(double dfCentralMeridian,
                                  double dfFalseEasting, double dfFalseNorthing)

{
    return d->replaceConversionAndUnref(proj_create_conversion_gall(
        d->getPROJContext(), dfCentralMeridian, dfFalseEasting, dfFalseNorthing,
        nullptr, 0.0, nullptr, 0.0));
}

/************************************************************************/
/*                               SetGH()                                */
/************************************************************************/

OGRErr OGRSpatialReference::SetGH(double dfCentralMeridian,
                                  double dfFalseEasting, double dfFalseNorthing)

{
    TAKE_OPTIONAL_LOCK();

    return d->replaceConversionAndUnref(proj_create_conversion_goode_homolosine(
        d->getPROJContext(), dfCentralMeridian, dfFalseEasting, dfFalseNorthing,
        nullptr, 0.0, nullptr, 0.0));
}

/************************************************************************/
/*                              SetIGH()                                */
/************************************************************************/

OGRErr OGRSpatialReference::SetIGH()

{
    TAKE_OPTIONAL_LOCK();

    return d->replaceConversionAndUnref(
        proj_create_conversion_interrupted_goode_homolosine(
            d->getPROJContext(), 0.0, 0.0, 0.0, nullptr, 0.0, nullptr, 0.0));
}

/************************************************************************/
/*                              SetGEOS()                               */
/************************************************************************/

OGRErr OGRSpatialReference::SetGEOS(double dfCentralMeridian,
                                    double dfSatelliteHeight,
                                    double dfFalseEasting,
                                    double dfFalseNorthing)

{
    TAKE_OPTIONAL_LOCK();

    return d->replaceConversionAndUnref(
        proj_create_conversion_geostationary_satellite_sweep_y(
            d->getPROJContext(), dfCentralMeridian, dfSatelliteHeight,
            dfFalseEasting, dfFalseNorthing, nullptr, 0.0, nullptr, 0.0));
}

/************************************************************************/
/*                       SetGaussSchreiberTMercator()                   */
/************************************************************************/

OGRErr OGRSpatialReference::SetGaussSchreiberTMercator(double dfCenterLat,
                                                       double dfCenterLong,
                                                       double dfScale,
                                                       double dfFalseEasting,
                                                       double dfFalseNorthing)

{
    TAKE_OPTIONAL_LOCK();

    return d->replaceConversionAndUnref(
        proj_create_conversion_gauss_schreiber_transverse_mercator(
            d->getPROJContext(), dfCenterLat, dfCenterLong, dfScale,
            dfFalseEasting, dfFalseNorthing, nullptr, 0.0, nullptr, 0.0));
}

/************************************************************************/
/*                            SetGnomonic()                             */
/************************************************************************/

OGRErr OGRSpatialReference::SetGnomonic(double dfCenterLat, double dfCenterLong,
                                        double dfFalseEasting,
                                        double dfFalseNorthing)

{
    TAKE_OPTIONAL_LOCK();

    return d->replaceConversionAndUnref(proj_create_conversion_gnomonic(
        d->getPROJContext(), dfCenterLat, dfCenterLong, dfFalseEasting,
        dfFalseNorthing, nullptr, 0.0, nullptr, 0.0));
}

/************************************************************************/
/*                              SetHOMAC()                              */
/************************************************************************/

/**
 * \brief Set an Hotine Oblique Mercator Azimuth Center projection using
 * azimuth angle.
 *
 * This projection corresponds to EPSG projection method 9815, also
 * sometimes known as hotine oblique mercator (variant B).
 *
 * This method does the same thing as the C function OSRSetHOMAC().
 *
 * @param dfCenterLat Latitude of the projection origin.
 * @param dfCenterLong Longitude of the projection origin.
 * @param dfAzimuth Azimuth, measured clockwise from North, of the projection
 * centerline.
 * @param dfRectToSkew Angle from Rectified to Skew Grid
 * @param dfScale Scale factor applies to the projection origin.
 * @param dfFalseEasting False easting.
 * @param dfFalseNorthing False northing.
 *
 * @return OGRERR_NONE on success.
 */

OGRErr OGRSpatialReference::SetHOMAC(double dfCenterLat, double dfCenterLong,
                                     double dfAzimuth, double dfRectToSkew,
                                     double dfScale, double dfFalseEasting,
                                     double dfFalseNorthing)

{
    TAKE_OPTIONAL_LOCK();

    return d->replaceConversionAndUnref(
        proj_create_conversion_hotine_oblique_mercator_variant_b(
            d->getPROJContext(), dfCenterLat, dfCenterLong, dfAzimuth,
            dfRectToSkew, dfScale, dfFalseEasting, dfFalseNorthing, nullptr,
            0.0, nullptr, 0.0));
}

/************************************************************************/
/*                               SetHOM()                               */
/************************************************************************/

/**
 * \brief Set a Hotine Oblique Mercator projection using azimuth angle.
 *
 * This projection corresponds to EPSG projection method 9812, also
 * sometimes known as hotine oblique mercator (variant A)..
 *
 * This method does the same thing as the C function OSRSetHOM().
 *
 * @param dfCenterLat Latitude of the projection origin.
 * @param dfCenterLong Longitude of the projection origin.
 * @param dfAzimuth Azimuth, measured clockwise from North, of the projection
 * centerline.
 * @param dfRectToSkew Angle from Rectified to Skew Grid
 * @param dfScale Scale factor applies to the projection origin.
 * @param dfFalseEasting False easting.
 * @param dfFalseNorthing False northing.
 *
 * @return OGRERR_NONE on success.
 */

OGRErr OGRSpatialReference::SetHOM(double dfCenterLat, double dfCenterLong,
                                   double dfAzimuth, double dfRectToSkew,
                                   double dfScale, double dfFalseEasting,
                                   double dfFalseNorthing)

{
    TAKE_OPTIONAL_LOCK();

    return d->replaceConversionAndUnref(
        proj_create_conversion_hotine_oblique_mercator_variant_a(
            d->getPROJContext(), dfCenterLat, dfCenterLong, dfAzimuth,
            dfRectToSkew, dfScale, dfFalseEasting, dfFalseNorthing, nullptr,
            0.0, nullptr, 0.0));
}

/************************************************************************/
/*                             SetHOM2PNO()                             */
/************************************************************************/

/**
 * \brief Set a Hotine Oblique Mercator projection using two points on
 * projection centerline.
 *
 * This method does the same thing as the C function OSRSetHOM2PNO().
 *
 * @param dfCenterLat Latitude of the projection origin.
 * @param dfLat1 Latitude of the first point on center line.
 * @param dfLong1 Longitude of the first point on center line.
 * @param dfLat2 Latitude of the second point on center line.
 * @param dfLong2 Longitude of the second point on center line.
 * @param dfScale Scale factor applies to the projection origin.
 * @param dfFalseEasting False easting.
 * @param dfFalseNorthing False northing.
 *
 * @return OGRERR_NONE on success.
 */

OGRErr OGRSpatialReference::SetHOM2PNO(double dfCenterLat, double dfLat1,
                                       double dfLong1, double dfLat2,
                                       double dfLong2, double dfScale,
                                       double dfFalseEasting,
                                       double dfFalseNorthing)

{
    TAKE_OPTIONAL_LOCK();

    return d->replaceConversionAndUnref(
        proj_create_conversion_hotine_oblique_mercator_two_point_natural_origin(
            d->getPROJContext(), dfCenterLat, dfLat1, dfLong1, dfLat2, dfLong2,
            dfScale, dfFalseEasting, dfFalseNorthing, nullptr, 0.0, nullptr,
            0.0));
}

/************************************************************************/
/*                               SetLOM()                               */
/************************************************************************/

/**
 * \brief Set a Laborde Oblique Mercator projection.
 *
 * @param dfCenterLat Latitude of the projection origin.
 * @param dfCenterLong Longitude of the projection origin.
 * @param dfAzimuth Azimuth, measured clockwise from North, of the projection
 * centerline.
 * @param dfScale Scale factor on the initiali line
 * @param dfFalseEasting False easting.
 * @param dfFalseNorthing False northing.
 *
 * @return OGRERR_NONE on success.
 */

OGRErr OGRSpatialReference::SetLOM(double dfCenterLat, double dfCenterLong,
                                   double dfAzimuth, double dfScale,
                                   double dfFalseEasting,
                                   double dfFalseNorthing)

{
    TAKE_OPTIONAL_LOCK();

    return d->replaceConversionAndUnref(
        proj_create_conversion_laborde_oblique_mercator(
            d->getPROJContext(), dfCenterLat, dfCenterLong, dfAzimuth, dfScale,
            dfFalseEasting, dfFalseNorthing, nullptr, 0.0, nullptr, 0.0));
}

/************************************************************************/
/*                            SetIWMPolyconic()                         */
/************************************************************************/

OGRErr OGRSpatialReference::SetIWMPolyconic(double dfLat1, double dfLat2,
                                            double dfCenterLong,
                                            double dfFalseEasting,
                                            double dfFalseNorthing)

{
    TAKE_OPTIONAL_LOCK();

    return d->replaceConversionAndUnref(
        proj_create_conversion_international_map_world_polyconic(
            d->getPROJContext(), dfCenterLong, dfLat1, dfLat2, dfFalseEasting,
            dfFalseNorthing, nullptr, 0.0, nullptr, 0.0));
}

/************************************************************************/
/*                             SetKrovak()                              */
/************************************************************************/

/** Krovak east-north projection.
 *
 * Note that dfAzimuth and dfPseudoStdParallel1 are ignored when exporting
 * to PROJ and should be respectively set to 30.28813972222222 and 78.5
 */
OGRErr OGRSpatialReference::SetKrovak(double dfCenterLat, double dfCenterLong,
                                      double dfAzimuth,
                                      double dfPseudoStdParallel1,
                                      double dfScale, double dfFalseEasting,
                                      double dfFalseNorthing)

{
    TAKE_OPTIONAL_LOCK();

    return d->replaceConversionAndUnref(
        proj_create_conversion_krovak_north_oriented(
            d->getPROJContext(), dfCenterLat, dfCenterLong, dfAzimuth,
            dfPseudoStdParallel1, dfScale, dfFalseEasting, dfFalseNorthing,
            nullptr, 0.0, nullptr, 0.0));
}

/************************************************************************/
/*                              SetLAEA()                               */
/************************************************************************/

OGRErr OGRSpatialReference::SetLAEA(double dfCenterLat, double dfCenterLong,
                                    double dfFalseEasting,
                                    double dfFalseNorthing)

{
    TAKE_OPTIONAL_LOCK();

    auto conv = proj_create_conversion_lambert_azimuthal_equal_area(
        d->getPROJContext(), dfCenterLat, dfCenterLong, dfFalseEasting,
        dfFalseNorthing, nullptr, 0.0, nullptr, 0.0);

    const char *pszName = nullptr;
    double dfConvFactor = GetTargetLinearUnits(nullptr, &pszName);
    CPLString osName = pszName ? pszName : "";

    d->refreshProjObj();

    d->demoteFromBoundCRS();

    auto cs = proj_create_cartesian_2D_cs(
        d->getPROJContext(),
        std::fabs(dfCenterLat - 90) < 1e-10 && dfCenterLong == 0
            ? PJ_CART2D_NORTH_POLE_EASTING_SOUTH_NORTHING_SOUTH
        : std::fabs(dfCenterLat - -90) < 1e-10 && dfCenterLong == 0
            ? PJ_CART2D_SOUTH_POLE_EASTING_NORTH_NORTHING_NORTH
            : PJ_CART2D_EASTING_NORTHING,
        !osName.empty() ? osName.c_str() : nullptr, dfConvFactor);
    auto projCRS =
        proj_create_projected_crs(d->getPROJContext(), d->getProjCRSName(),
                                  d->getGeodBaseCRS(), conv, cs);
    proj_destroy(conv);
    proj_destroy(cs);

    d->setPjCRS(projCRS);

    d->undoDemoteFromBoundCRS();

    return OGRERR_NONE;
}

/************************************************************************/
/*                               SetLCC()                               */
/************************************************************************/

OGRErr OGRSpatialReference::SetLCC(double dfStdP1, double dfStdP2,
                                   double dfCenterLat, double dfCenterLong,
                                   double dfFalseEasting,
                                   double dfFalseNorthing)

{
    TAKE_OPTIONAL_LOCK();

    return d->replaceConversionAndUnref(
        proj_create_conversion_lambert_conic_conformal_2sp(
            d->getPROJContext(), dfCenterLat, dfCenterLong, dfStdP1, dfStdP2,
            dfFalseEasting, dfFalseNorthing, nullptr, 0, nullptr, 0));
}

/************************************************************************/
/*                             SetLCC1SP()                              */
/************************************************************************/

OGRErr OGRSpatialReference::SetLCC1SP(double dfCenterLat, double dfCenterLong,
                                      double dfScale, double dfFalseEasting,
                                      double dfFalseNorthing)

{
    TAKE_OPTIONAL_LOCK();

    return d->replaceConversionAndUnref(
        proj_create_conversion_lambert_conic_conformal_1sp(
            d->getPROJContext(), dfCenterLat, dfCenterLong, dfScale,
            dfFalseEasting, dfFalseNorthing, nullptr, 0, nullptr, 0));
}

/************************************************************************/
/*                              SetLCCB()                               */
/************************************************************************/

OGRErr OGRSpatialReference::SetLCCB(double dfStdP1, double dfStdP2,
                                    double dfCenterLat, double dfCenterLong,
                                    double dfFalseEasting,
                                    double dfFalseNorthing)

{
    TAKE_OPTIONAL_LOCK();

    return d->replaceConversionAndUnref(
        proj_create_conversion_lambert_conic_conformal_2sp_belgium(
            d->getPROJContext(), dfCenterLat, dfCenterLong, dfStdP1, dfStdP2,
            dfFalseEasting, dfFalseNorthing, nullptr, 0, nullptr, 0));
}

/************************************************************************/
/*                               SetMC()                                */
/************************************************************************/

OGRErr OGRSpatialReference::SetMC(double dfCenterLat, double dfCenterLong,
                                  double dfFalseEasting, double dfFalseNorthing)

{
    TAKE_OPTIONAL_LOCK();

    (void)dfCenterLat;  // ignored

    return d->replaceConversionAndUnref(
        proj_create_conversion_miller_cylindrical(
            d->getPROJContext(), dfCenterLong, dfFalseEasting, dfFalseNorthing,
            nullptr, 0, nullptr, 0));
}

/************************************************************************/
/*                            SetMercator()                             */
/************************************************************************/

OGRErr OGRSpatialReference::SetMercator(double dfCenterLat, double dfCenterLong,
                                        double dfScale, double dfFalseEasting,
                                        double dfFalseNorthing)

{
    TAKE_OPTIONAL_LOCK();

    if (dfCenterLat != 0.0 && dfScale == 1.0)
    {
        // Not sure this is correct, but this is how it has been used
        // historically
        return SetMercator2SP(dfCenterLat, 0.0, dfCenterLong, dfFalseEasting,
                              dfFalseNorthing);
    }
    return d->replaceConversionAndUnref(
        proj_create_conversion_mercator_variant_a(
            d->getPROJContext(),
            dfCenterLat,  // should be zero
            dfCenterLong, dfScale, dfFalseEasting, dfFalseNorthing, nullptr, 0,
            nullptr, 0));
}

/************************************************************************/
/*                           SetMercator2SP()                           */
/************************************************************************/

OGRErr OGRSpatialReference::SetMercator2SP(double dfStdP1, double dfCenterLat,
                                           double dfCenterLong,
                                           double dfFalseEasting,
                                           double dfFalseNorthing)

{
    if (dfCenterLat == 0.0)
    {
        return d->replaceConversionAndUnref(
            proj_create_conversion_mercator_variant_b(
                d->getPROJContext(), dfStdP1, dfCenterLong, dfFalseEasting,
                dfFalseNorthing, nullptr, 0, nullptr, 0));
    }

    TAKE_OPTIONAL_LOCK();

    SetProjection(SRS_PT_MERCATOR_2SP);

    SetNormProjParm(SRS_PP_STANDARD_PARALLEL_1, dfStdP1);
    SetNormProjParm(SRS_PP_LATITUDE_OF_ORIGIN, dfCenterLat);
    SetNormProjParm(SRS_PP_CENTRAL_MERIDIAN, dfCenterLong);
    SetNormProjParm(SRS_PP_FALSE_EASTING, dfFalseEasting);
    SetNormProjParm(SRS_PP_FALSE_NORTHING, dfFalseNorthing);

    return OGRERR_NONE;
}

/************************************************************************/
/*                            SetMollweide()                            */
/************************************************************************/

OGRErr OGRSpatialReference::SetMollweide(double dfCentralMeridian,
                                         double dfFalseEasting,
                                         double dfFalseNorthing)

{
    TAKE_OPTIONAL_LOCK();

    return d->replaceConversionAndUnref(proj_create_conversion_mollweide(
        d->getPROJContext(), dfCentralMeridian, dfFalseEasting, dfFalseNorthing,
        nullptr, 0, nullptr, 0));
}

/************************************************************************/
/*                              SetNZMG()                               */
/************************************************************************/

OGRErr OGRSpatialReference::SetNZMG(double dfCenterLat, double dfCenterLong,
                                    double dfFalseEasting,
                                    double dfFalseNorthing)

{
    TAKE_OPTIONAL_LOCK();

    return d->replaceConversionAndUnref(
        proj_create_conversion_new_zealand_mapping_grid(
            d->getPROJContext(), dfCenterLat, dfCenterLong, dfFalseEasting,
            dfFalseNorthing, nullptr, 0, nullptr, 0));
}

/************************************************************************/
/*                               SetOS()                                */
/************************************************************************/

OGRErr OGRSpatialReference::SetOS(double dfOriginLat, double dfCMeridian,
                                  double dfScale, double dfFalseEasting,
                                  double dfFalseNorthing)

{
    TAKE_OPTIONAL_LOCK();

    return d->replaceConversionAndUnref(
        proj_create_conversion_oblique_stereographic(
            d->getPROJContext(), dfOriginLat, dfCMeridian, dfScale,
            dfFalseEasting, dfFalseNorthing, nullptr, 0, nullptr, 0));
}

/************************************************************************/
/*                          SetOrthographic()                           */
/************************************************************************/

OGRErr OGRSpatialReference::SetOrthographic(double dfCenterLat,
                                            double dfCenterLong,
                                            double dfFalseEasting,
                                            double dfFalseNorthing)

{
    TAKE_OPTIONAL_LOCK();

    return d->replaceConversionAndUnref(proj_create_conversion_orthographic(
        d->getPROJContext(), dfCenterLat, dfCenterLong, dfFalseEasting,
        dfFalseNorthing, nullptr, 0, nullptr, 0));
}

/************************************************************************/
/*                            SetPolyconic()                            */
/************************************************************************/

OGRErr OGRSpatialReference::SetPolyconic(double dfCenterLat,
                                         double dfCenterLong,
                                         double dfFalseEasting,
                                         double dfFalseNorthing)

{
    TAKE_OPTIONAL_LOCK();

    // note: it seems that by some definitions this should include a
    //       scale_factor parameter.
    return d->replaceConversionAndUnref(
        proj_create_conversion_american_polyconic(
            d->getPROJContext(), dfCenterLat, dfCenterLong, dfFalseEasting,
            dfFalseNorthing, nullptr, 0, nullptr, 0));
}

/************************************************************************/
/*                               SetPS()                                */
/************************************************************************/

/** Sets a Polar Stereographic projection.
 *
 * Two variants are possible:
 * - Polar Stereographic Variant A: dfCenterLat must be +/- 90° and is
 *   interpreted as the latitude of origin, combined with the scale factor
 * - Polar Stereographic Variant B: dfCenterLat is different from +/- 90° and
 *   is interpreted as the latitude of true scale. In that situation, dfScale
 *   must be set to 1 (it is ignored in the projection parameters)
 */
OGRErr OGRSpatialReference::SetPS(double dfCenterLat, double dfCenterLong,
                                  double dfScale, double dfFalseEasting,
                                  double dfFalseNorthing)

{
    TAKE_OPTIONAL_LOCK();

    PJ *conv;
    if (dfScale == 1.0 && std::abs(std::abs(dfCenterLat) - 90) > 1e-8)
    {
        conv = proj_create_conversion_polar_stereographic_variant_b(
            d->getPROJContext(), dfCenterLat, dfCenterLong, dfFalseEasting,
            dfFalseNorthing, nullptr, 0, nullptr, 0);
    }
    else
    {
        conv = proj_create_conversion_polar_stereographic_variant_a(
            d->getPROJContext(), dfCenterLat, dfCenterLong, dfScale,
            dfFalseEasting, dfFalseNorthing, nullptr, 0, nullptr, 0);
    }

    const char *pszName = nullptr;
    double dfConvFactor = GetTargetLinearUnits(nullptr, &pszName);
    CPLString osName = pszName ? pszName : "";

    d->refreshProjObj();

    d->demoteFromBoundCRS();

    auto cs = proj_create_cartesian_2D_cs(
        d->getPROJContext(),
        dfCenterLat > 0 ? PJ_CART2D_NORTH_POLE_EASTING_SOUTH_NORTHING_SOUTH
                        : PJ_CART2D_SOUTH_POLE_EASTING_NORTH_NORTHING_NORTH,
        !osName.empty() ? osName.c_str() : nullptr, dfConvFactor);
    auto projCRS =
        proj_create_projected_crs(d->getPROJContext(), d->getProjCRSName(),
                                  d->getGeodBaseCRS(), conv, cs);
    proj_destroy(conv);
    proj_destroy(cs);

    d->setPjCRS(projCRS);

    d->undoDemoteFromBoundCRS();

    return OGRERR_NONE;
}

/************************************************************************/
/*                            SetRobinson()                             */
/************************************************************************/

OGRErr OGRSpatialReference::SetRobinson(double dfCenterLong,
                                        double dfFalseEasting,
                                        double dfFalseNorthing)

{
    TAKE_OPTIONAL_LOCK();

    return d->replaceConversionAndUnref(proj_create_conversion_robinson(
        d->getPROJContext(), dfCenterLong, dfFalseEasting, dfFalseNorthing,
        nullptr, 0, nullptr, 0));
}

/************************************************************************/
/*                           SetSinusoidal()                            */
/************************************************************************/

OGRErr OGRSpatialReference::SetSinusoidal(double dfCenterLong,
                                          double dfFalseEasting,
                                          double dfFalseNorthing)

{
    TAKE_OPTIONAL_LOCK();

    return d->replaceConversionAndUnref(proj_create_conversion_sinusoidal(
        d->getPROJContext(), dfCenterLong, dfFalseEasting, dfFalseNorthing,
        nullptr, 0, nullptr, 0));
}

/************************************************************************/
/*                          SetStereographic()                          */
/************************************************************************/

OGRErr OGRSpatialReference::SetStereographic(double dfOriginLat,
                                             double dfCMeridian, double dfScale,
                                             double dfFalseEasting,
                                             double dfFalseNorthing)

{
    TAKE_OPTIONAL_LOCK();

    return d->replaceConversionAndUnref(proj_create_conversion_stereographic(
        d->getPROJContext(), dfOriginLat, dfCMeridian, dfScale, dfFalseEasting,
        dfFalseNorthing, nullptr, 0, nullptr, 0));
}

/************************************************************************/
/*                               SetSOC()                               */
/*                                                                      */
/*      NOTE: This definition isn't really used in practice any more    */
/*      and should be considered deprecated.  It seems that swiss       */
/*      oblique mercator is now define as Hotine_Oblique_Mercator       */
/*      with an azimuth of 90 and a rectified_grid_angle of 90.  See    */
/*      EPSG:2056 and Bug 423.                                          */
/************************************************************************/

OGRErr OGRSpatialReference::SetSOC(double dfLatitudeOfOrigin,
                                   double dfCentralMeridian,
                                   double dfFalseEasting,
                                   double dfFalseNorthing)

{
    TAKE_OPTIONAL_LOCK();

    return d->replaceConversionAndUnref(
        proj_create_conversion_hotine_oblique_mercator_variant_b(
            d->getPROJContext(), dfLatitudeOfOrigin, dfCentralMeridian, 90.0,
            90.0, 1.0, dfFalseEasting, dfFalseNorthing, nullptr, 0.0, nullptr,
            0.0));
#if 0
    SetProjection( SRS_PT_SWISS_OBLIQUE_CYLINDRICAL );
    SetNormProjParm( SRS_PP_LATITUDE_OF_CENTER, dfLatitudeOfOrigin );
    SetNormProjParm( SRS_PP_CENTRAL_MERIDIAN, dfCentralMeridian );
    SetNormProjParm( SRS_PP_FALSE_EASTING, dfFalseEasting );
    SetNormProjParm( SRS_PP_FALSE_NORTHING, dfFalseNorthing );

    return OGRERR_NONE;
#endif
}

/************************************************************************/
/*                               SetVDG()                               */
/************************************************************************/

OGRErr OGRSpatialReference::SetVDG(double dfCMeridian, double dfFalseEasting,
                                   double dfFalseNorthing)

{
    TAKE_OPTIONAL_LOCK();

    return d->replaceConversionAndUnref(proj_create_conversion_van_der_grinten(
        d->getPROJContext(), dfCMeridian, dfFalseEasting, dfFalseNorthing,
        nullptr, 0, nullptr, 0));
}

/************************************************************************/
/*                               SetUTM()                               */
/************************************************************************/

/**
 * \brief Set UTM projection definition.
 *
 * This will generate a projection definition with the full set of
 * transverse mercator projection parameters for the given UTM zone.
 * If no PROJCS[] description is set yet, one will be set to look
 * like "UTM Zone %d, {Northern, Southern} Hemisphere".
 *
 * This method is the same as the C function OSRSetUTM().
 *
 * @param nZone UTM zone.
 *
 * @param bNorth TRUE for northern hemisphere, or FALSE for southern
 * hemisphere.
 *
 * @return OGRERR_NONE on success.
 */

OGRErr OGRSpatialReference::SetUTM(int nZone, int bNorth)

{
    TAKE_OPTIONAL_LOCK();

    if (nZone < 0 || nZone > 60)
    {
        CPLError(CE_Failure, CPLE_AppDefined, "Invalid zone: %d", nZone);
        return OGRERR_FAILURE;
    }

    return d->replaceConversionAndUnref(
        proj_create_conversion_utm(d->getPROJContext(), nZone, bNorth));
}

/************************************************************************/
/*                             GetUTMZone()                             */
/*                                                                      */
/*      Returns zero if it isn't UTM.                                   */
/************************************************************************/

/**
 * \brief Get utm zone information.
 *
 * This is the same as the C function OSRGetUTMZone().
 *
 * In SWIG bindings (Python, Java, etc) the GetUTMZone() method returns a
 * zone which is negative in the southern hemisphere instead of having the
 * pbNorth flag used in the C and C++ interface.
 *
 * @param pbNorth pointer to in to set to TRUE if northern hemisphere, or
 * FALSE if southern.
 *
 * @return UTM zone number or zero if this isn't a UTM definition.
 */

int OGRSpatialReference::GetUTMZone(int *pbNorth) const

{
    TAKE_OPTIONAL_LOCK();

    if (IsProjected() && GetAxesCount() == 3)
    {
        OGRSpatialReference *poSRSTmp = Clone();
        poSRSTmp->DemoteTo2D(nullptr);
        const int nZone = poSRSTmp->GetUTMZone(pbNorth);
        delete poSRSTmp;
        return nZone;
    }

    const char *pszProjection = GetAttrValue("PROJECTION");

    if (pszProjection == nullptr ||
        !EQUAL(pszProjection, SRS_PT_TRANSVERSE_MERCATOR))
        return 0;

    if (GetNormProjParm(SRS_PP_LATITUDE_OF_ORIGIN, 0.0) != 0.0)
        return 0;

    if (GetProjParm(SRS_PP_SCALE_FACTOR, 1.0) != 0.9996)
        return 0;

    if (fabs(GetNormProjParm(SRS_PP_FALSE_EASTING, 0.0) - 500000.0) > 0.001)
        return 0;

    const double dfFalseNorthing = GetNormProjParm(SRS_PP_FALSE_NORTHING, 0.0);

    if (dfFalseNorthing != 0.0 && fabs(dfFalseNorthing - 10000000.0) > 0.001)
        return 0;

    if (pbNorth != nullptr)
        *pbNorth = (dfFalseNorthing == 0);

    const double dfCentralMeridian =
        GetNormProjParm(SRS_PP_CENTRAL_MERIDIAN, 0.0);
    const double dfZone = (dfCentralMeridian + 186.0) / 6.0;

    if (dfCentralMeridian < -177.00001 || dfCentralMeridian > 177.000001 ||
        std::isnan(dfZone) ||
        std::abs(dfZone - static_cast<int>(dfZone) - 0.5) > 0.00001)
        return 0;

    return static_cast<int>(dfZone);
}

/************************************************************************/
/*                             SetWagner()                              */
/************************************************************************/

OGRErr OGRSpatialReference::SetWagner(int nVariation,  // 1--7.
                                      double dfCenterLat, double dfFalseEasting,
                                      double dfFalseNorthing)

{
    TAKE_OPTIONAL_LOCK();

    PJ *conv;
    if (nVariation == 1)
    {
        conv = proj_create_conversion_wagner_i(d->getPROJContext(), 0.0,
                                               dfFalseEasting, dfFalseNorthing,
                                               nullptr, 0.0, nullptr, 0.0);
    }
    else if (nVariation == 2)
    {
        conv = proj_create_conversion_wagner_ii(d->getPROJContext(), 0.0,
                                                dfFalseEasting, dfFalseNorthing,
                                                nullptr, 0.0, nullptr, 0.0);
    }
    else if (nVariation == 3)
    {
        conv = proj_create_conversion_wagner_iii(
            d->getPROJContext(), dfCenterLat, 0.0, dfFalseEasting,
            dfFalseNorthing, nullptr, 0.0, nullptr, 0.0);
    }
    else if (nVariation == 4)
    {
        conv = proj_create_conversion_wagner_iv(d->getPROJContext(), 0.0,
                                                dfFalseEasting, dfFalseNorthing,
                                                nullptr, 0.0, nullptr, 0.0);
    }
    else if (nVariation == 5)
    {
        conv = proj_create_conversion_wagner_v(d->getPROJContext(), 0.0,
                                               dfFalseEasting, dfFalseNorthing,
                                               nullptr, 0.0, nullptr, 0.0);
    }
    else if (nVariation == 6)
    {
        conv = proj_create_conversion_wagner_vi(d->getPROJContext(), 0.0,
                                                dfFalseEasting, dfFalseNorthing,
                                                nullptr, 0.0, nullptr, 0.0);
    }
    else if (nVariation == 7)
    {
        conv = proj_create_conversion_wagner_vii(
            d->getPROJContext(), 0.0, dfFalseEasting, dfFalseNorthing, nullptr,
            0.0, nullptr, 0.0);
    }
    else
    {
        CPLError(CE_Failure, CPLE_AppDefined,
                 "Unsupported Wagner variation (%d).", nVariation);
        return OGRERR_UNSUPPORTED_SRS;
    }

    return d->replaceConversionAndUnref(conv);
}

/************************************************************************/
/*                            SetQSC()                     */
/************************************************************************/

OGRErr OGRSpatialReference::SetQSC(double dfCenterLat, double dfCenterLong)
{
    TAKE_OPTIONAL_LOCK();

    return d->replaceConversionAndUnref(
        proj_create_conversion_quadrilateralized_spherical_cube(
            d->getPROJContext(), dfCenterLat, dfCenterLong, 0.0, 0.0, nullptr,
            0, nullptr, 0));
}

/************************************************************************/
/*                            SetSCH()                     */
/************************************************************************/

OGRErr OGRSpatialReference::SetSCH(double dfPegLat, double dfPegLong,
                                   double dfPegHeading, double dfPegHgt)

{
    TAKE_OPTIONAL_LOCK();

    return d->replaceConversionAndUnref(
        proj_create_conversion_spherical_cross_track_height(
            d->getPROJContext(), dfPegLat, dfPegLong, dfPegHeading, dfPegHgt,
            nullptr, 0, nullptr, 0));
}

/************************************************************************/
/*                         SetVerticalPerspective()                     */
/************************************************************************/

OGRErr OGRSpatialReference::SetVerticalPerspective(
    double dfTopoOriginLat, double dfTopoOriginLon, double dfTopoOriginHeight,
    double dfViewPointHeight, double dfFalseEasting, double dfFalseNorthing)
{
    TAKE_OPTIONAL_LOCK();

    return d->replaceConversionAndUnref(
        proj_create_conversion_vertical_perspective(
            d->getPROJContext(), dfTopoOriginLat, dfTopoOriginLon,
            dfTopoOriginHeight, dfViewPointHeight, dfFalseEasting,
            dfFalseNorthing, nullptr, 0, nullptr, 0));
}

/************************************************************************/
/*             SetDerivedGeogCRSWithPoleRotationGRIBConvention()        */
/************************************************************************/

OGRErr OGRSpatialReference::SetDerivedGeogCRSWithPoleRotationGRIBConvention(
    const char *pszCRSName, double dfSouthPoleLat, double dfSouthPoleLon,
    double dfAxisRotation)
{
    TAKE_OPTIONAL_LOCK();

    d->refreshProjObj();
    if (!d->m_pj_crs)
        return OGRERR_FAILURE;
    if (d->m_pjType != PJ_TYPE_GEOGRAPHIC_2D_CRS)
        return OGRERR_FAILURE;
    auto ctxt = d->getPROJContext();
    auto conv = proj_create_conversion_pole_rotation_grib_convention(
        ctxt, dfSouthPoleLat, dfSouthPoleLon, dfAxisRotation, nullptr, 0);
    auto cs = proj_crs_get_coordinate_system(ctxt, d->m_pj_crs);
    d->setPjCRS(proj_create_derived_geographic_crs(ctxt, pszCRSName,
                                                   d->m_pj_crs, conv, cs));
    proj_destroy(conv);
    proj_destroy(cs);
    return OGRERR_NONE;
}

/************************************************************************/
/*         SetDerivedGeogCRSWithPoleRotationNetCDFCFConvention()        */
/************************************************************************/

OGRErr OGRSpatialReference::SetDerivedGeogCRSWithPoleRotationNetCDFCFConvention(
    const char *pszCRSName, double dfGridNorthPoleLat,
    double dfGridNorthPoleLon, double dfNorthPoleGridLon)
{
    TAKE_OPTIONAL_LOCK();

#if PROJ_VERSION_MAJOR > 8 ||                                                  \
    (PROJ_VERSION_MAJOR == 8 && PROJ_VERSION_MINOR >= 2)
    d->refreshProjObj();
    if (!d->m_pj_crs)
        return OGRERR_FAILURE;
    if (d->m_pjType != PJ_TYPE_GEOGRAPHIC_2D_CRS)
        return OGRERR_FAILURE;
    auto ctxt = d->getPROJContext();
    auto conv = proj_create_conversion_pole_rotation_netcdf_cf_convention(
        ctxt, dfGridNorthPoleLat, dfGridNorthPoleLon, dfNorthPoleGridLon,
        nullptr, 0);
    auto cs = proj_crs_get_coordinate_system(ctxt, d->m_pj_crs);
    d->setPjCRS(proj_create_derived_geographic_crs(ctxt, pszCRSName,
                                                   d->m_pj_crs, conv, cs));
    proj_destroy(conv);
    proj_destroy(cs);
    return OGRERR_NONE;
#else
    (void)pszCRSName;
    SetProjection("Rotated_pole");
    SetExtension(
        "PROJCS", "PROJ4",
        CPLSPrintf("+proj=ob_tran +o_proj=longlat +lon_0=%.17g +o_lon_p=%.17g "
                   "+o_lat_p=%.17g +a=%.17g +b=%.17g "
                   "+to_meter=0.0174532925199433 "
                   "+wktext",
                   180.0 + dfGridNorthPoleLon, dfNorthPoleGridLon,
                   dfGridNorthPoleLat, GetSemiMajor(nullptr),
                   GetSemiMinor(nullptr)));
    return OGRERR_NONE;
#endif
}

/************************************************************************/
/*                            SetAuthority()                            */
/************************************************************************/

/**
 * \brief Set the authority for a node.
 *
 * This method is the same as the C function OSRSetAuthority().
 *
 * @param pszTargetKey the partial or complete path to the node to
 * set an authority on.  i.e. "PROJCS", "GEOGCS" or "GEOGCS|UNIT".
 *
 * @param pszAuthority authority name, such as "EPSG".
 *
 * @param nCode code for value with this authority.
 *
 * @return OGRERR_NONE on success.
 */

OGRErr OGRSpatialReference::SetAuthority(const char *pszTargetKey,
                                         const char *pszAuthority, int nCode)

{
    TAKE_OPTIONAL_LOCK();

    d->refreshProjObj();
    pszTargetKey = d->nullifyTargetKeyIfPossible(pszTargetKey);

    if (pszTargetKey == nullptr)
    {
        if (!d->m_pj_crs)
            return OGRERR_FAILURE;
        CPLString osCode;
        osCode.Printf("%d", nCode);
        d->demoteFromBoundCRS();
        d->setPjCRS(proj_alter_id(d->getPROJContext(), d->m_pj_crs,
                                  pszAuthority, osCode.c_str()));
        d->undoDemoteFromBoundCRS();
        return OGRERR_NONE;
    }

    d->demoteFromBoundCRS();
    if (d->m_pjType == PJ_TYPE_PROJECTED_CRS && EQUAL(pszTargetKey, "GEOGCS"))
    {
        CPLString osCode;
        osCode.Printf("%d", nCode);
        auto newGeogCRS =
            proj_alter_id(d->getPROJContext(), d->getGeodBaseCRS(),
                          pszAuthority, osCode.c_str());

        auto conv =
            proj_crs_get_coordoperation(d->getPROJContext(), d->m_pj_crs);

        auto projCRS = proj_create_projected_crs(
            d->getPROJContext(), d->getProjCRSName(), newGeogCRS, conv,
            d->getProjCRSCoordSys());

        // Preserve existing id on the PROJCRS
        const char *pszProjCRSAuthName = proj_get_id_auth_name(d->m_pj_crs, 0);
        const char *pszProjCRSCode = proj_get_id_code(d->m_pj_crs, 0);
        if (pszProjCRSAuthName && pszProjCRSCode)
        {
            auto projCRSWithId =
                proj_alter_id(d->getPROJContext(), projCRS, pszProjCRSAuthName,
                              pszProjCRSCode);
            proj_destroy(projCRS);
            projCRS = projCRSWithId;
        }

        proj_destroy(newGeogCRS);
        proj_destroy(conv);

        d->setPjCRS(projCRS);
        d->undoDemoteFromBoundCRS();
        return OGRERR_NONE;
    }
    d->undoDemoteFromBoundCRS();

    /* -------------------------------------------------------------------- */
    /*      Find the node below which the authority should be put.          */
    /* -------------------------------------------------------------------- */
    OGR_SRSNode *poNode = GetAttrNode(pszTargetKey);

    if (poNode == nullptr)
        return OGRERR_FAILURE;

    /* -------------------------------------------------------------------- */
    /*      If there is an existing AUTHORITY child blow it away before     */
    /*      trying to set a new one.                                        */
    /* -------------------------------------------------------------------- */
    int iOldChild = poNode->FindChild("AUTHORITY");
    if (iOldChild != -1)
        poNode->DestroyChild(iOldChild);

    /* -------------------------------------------------------------------- */
    /*      Create a new authority node.                                    */
    /* -------------------------------------------------------------------- */
    char szCode[32] = {};

    snprintf(szCode, sizeof(szCode), "%d", nCode);

    OGR_SRSNode *poAuthNode = new OGR_SRSNode("AUTHORITY");
    poAuthNode->AddChild(new OGR_SRSNode(pszAuthority));
    poAuthNode->AddChild(new OGR_SRSNode(szCode));

    poNode->AddChild(poAuthNode);

    return OGRERR_NONE;
}

/************************************************************************/
/*                          GetAuthorityCode()                          */
/************************************************************************/

/**
 * \brief Get the authority code for a node.
 *
 * This method is used to query an AUTHORITY[] node from within the
 * WKT tree, and fetch the code value.
 *
 * While in theory values may be non-numeric, for the EPSG authority all
 * code values should be integral.
 *
 * This method is the same as the C function OSRGetAuthorityCode().
 *
 * @param pszTargetKey the partial or complete path to the node to
 * get an authority from.  i.e. "PROJCS", "GEOGCS", "GEOGCS|UNIT" or NULL to
 * search for an authority node on the root element.
 *
 * @return value code from authority node, or NULL on failure.  The value
 * returned is internal and should not be freed or modified.
 */

const char *
OGRSpatialReference::GetAuthorityCode(const char *pszTargetKey) const

{
    TAKE_OPTIONAL_LOCK();

    d->refreshProjObj();
    const char *pszInputTargetKey = pszTargetKey;
    pszTargetKey = d->nullifyTargetKeyIfPossible(pszTargetKey);
    if (pszTargetKey == nullptr)
    {
        if (!d->m_pj_crs)
        {
            return nullptr;
        }
        d->demoteFromBoundCRS();
        auto ret = proj_get_id_code(d->m_pj_crs, 0);
        if (ret == nullptr && d->m_pjType == PJ_TYPE_PROJECTED_CRS)
        {
            auto ctxt = d->getPROJContext();
            auto cs = proj_crs_get_coordinate_system(ctxt, d->m_pj_crs);
            if (cs)
            {
                const int axisCount = proj_cs_get_axis_count(ctxt, cs);
                proj_destroy(cs);
                if (axisCount == 3)
                {
                    // This might come from a COMPD_CS with a VERT_DATUM type =
                    // 2002 in which case, using the WKT1 representation will
                    // enable us to recover the EPSG code.
                    pszTargetKey = pszInputTargetKey;
                }
            }
        }
        d->undoDemoteFromBoundCRS();
        if (ret != nullptr || pszTargetKey == nullptr)
        {
            return ret;
        }
    }

    // Special key for that context
    else if (EQUAL(pszTargetKey, "HORIZCRS") &&
             d->m_pjType == PJ_TYPE_COMPOUND_CRS)
    {
        auto ctxt = d->getPROJContext();
        auto crs = proj_crs_get_sub_crs(ctxt, d->m_pj_crs, 0);
        if (crs)
        {
            const char *ret = proj_get_id_code(crs, 0);
            if (ret)
                ret = CPLSPrintf("%s", ret);
            proj_destroy(crs);
            return ret;
        }
    }
    else if (EQUAL(pszTargetKey, "VERTCRS") &&
             d->m_pjType == PJ_TYPE_COMPOUND_CRS)
    {
        auto ctxt = d->getPROJContext();
        auto crs = proj_crs_get_sub_crs(ctxt, d->m_pj_crs, 1);
        if (crs)
        {
            const char *ret = proj_get_id_code(crs, 0);
            if (ret)
                ret = CPLSPrintf("%s", ret);
            proj_destroy(crs);
            return ret;
        }
    }

    /* -------------------------------------------------------------------- */
    /*      Find the node below which the authority should be put.          */
    /* -------------------------------------------------------------------- */
    const OGR_SRSNode *poNode = GetAttrNode(pszTargetKey);

    if (poNode == nullptr)
        return nullptr;

    /* -------------------------------------------------------------------- */
    /*      Fetch AUTHORITY child if there is one.                          */
    /* -------------------------------------------------------------------- */
    if (poNode->FindChild("AUTHORITY") == -1)
        return nullptr;

    poNode = poNode->GetChild(poNode->FindChild("AUTHORITY"));

    /* -------------------------------------------------------------------- */
    /*      Create a new authority node.                                    */
    /* -------------------------------------------------------------------- */
    if (poNode->GetChildCount() < 2)
        return nullptr;

    return poNode->GetChild(1)->GetValue();
}

/************************************************************************/
/*                          GetAuthorityName()                          */
/************************************************************************/

/**
 * \brief Get the authority name for a node.
 *
 * This method is used to query an AUTHORITY[] node from within the
 * WKT tree, and fetch the authority name value.
 *
 * The most common authority is "EPSG".
 *
 * This method is the same as the C function OSRGetAuthorityName().
 *
 * @param pszTargetKey the partial or complete path to the node to
 * get an authority from.  i.e. "PROJCS", "GEOGCS", "GEOGCS|UNIT" or NULL to
 * search for an authority node on the root element.
 *
 * @return value code from authority node, or NULL on failure. The value
 * returned is internal and should not be freed or modified.
 */

const char *
OGRSpatialReference::GetAuthorityName(const char *pszTargetKey) const

{
    TAKE_OPTIONAL_LOCK();

    d->refreshProjObj();
    const char *pszInputTargetKey = pszTargetKey;
    pszTargetKey = d->nullifyTargetKeyIfPossible(pszTargetKey);
    if (pszTargetKey == nullptr)
    {
        if (!d->m_pj_crs)
        {
            return nullptr;
        }
        d->demoteFromBoundCRS();
        auto ret = proj_get_id_auth_name(d->m_pj_crs, 0);
        if (ret == nullptr && d->m_pjType == PJ_TYPE_PROJECTED_CRS)
        {
            auto ctxt = d->getPROJContext();
            auto cs = proj_crs_get_coordinate_system(ctxt, d->m_pj_crs);
            if (cs)
            {
                const int axisCount = proj_cs_get_axis_count(ctxt, cs);
                proj_destroy(cs);
                if (axisCount == 3)
                {
                    // This might come from a COMPD_CS with a VERT_DATUM type =
                    // 2002 in which case, using the WKT1 representation will
                    // enable us to recover the EPSG code.
                    pszTargetKey = pszInputTargetKey;
                }
            }
        }
        d->undoDemoteFromBoundCRS();
        if (ret != nullptr || pszTargetKey == nullptr)
        {
            return ret;
        }
    }

    // Special key for that context
    else if (EQUAL(pszTargetKey, "HORIZCRS") &&
             d->m_pjType == PJ_TYPE_COMPOUND_CRS)
    {
        auto ctxt = d->getPROJContext();
        auto crs = proj_crs_get_sub_crs(ctxt, d->m_pj_crs, 0);
        if (crs)
        {
            const char *ret = proj_get_id_auth_name(crs, 0);
            if (ret)
                ret = CPLSPrintf("%s", ret);
            proj_destroy(crs);
            return ret;
        }
    }
    else if (EQUAL(pszTargetKey, "VERTCRS") &&
             d->m_pjType == PJ_TYPE_COMPOUND_CRS)
    {
        auto ctxt = d->getPROJContext();
        auto crs = proj_crs_get_sub_crs(ctxt, d->m_pj_crs, 1);
        if (crs)
        {
            const char *ret = proj_get_id_auth_name(crs, 0);
            if (ret)
                ret = CPLSPrintf("%s", ret);
            proj_destroy(crs);
            return ret;
        }
    }

    /* -------------------------------------------------------------------- */
    /*      Find the node below which the authority should be put.          */
    /* -------------------------------------------------------------------- */
    const OGR_SRSNode *poNode = GetAttrNode(pszTargetKey);

    if (poNode == nullptr)
        return nullptr;

    /* -------------------------------------------------------------------- */
    /*      Fetch AUTHORITY child if there is one.                          */
    /* -------------------------------------------------------------------- */
    if (poNode->FindChild("AUTHORITY") == -1)
        return nullptr;

    poNode = poNode->GetChild(poNode->FindChild("AUTHORITY"));

    /* -------------------------------------------------------------------- */
    /*      Create a new authority node.                                    */
    /* -------------------------------------------------------------------- */
    if (poNode->GetChildCount() < 2)
        return nullptr;

    return poNode->GetChild(0)->GetValue();
}

/************************************************************************/
/*                   StripTOWGS84IfKnownDatumAndAllowed()               */
/************************************************************************/

/**
 * \brief Remove TOWGS84 information if the CRS has a known horizontal datum
 *        and this is allowed by the user.
 *
 * The default behavior is to remove TOWGS84 information if the CRS has a
 * known horizontal datum. This can be disabled by setting the
 * OSR_STRIP_TOWGS84 configuration option to NO.
 *
 * @return true if TOWGS84 has been removed.
 * @since OGR 3.1.0
 */

bool OGRSpatialReference::StripTOWGS84IfKnownDatumAndAllowed()
{
    if (CPLTestBool(CPLGetConfigOption("OSR_STRIP_TOWGS84", "YES")))
    {
        if (StripTOWGS84IfKnownDatum())
        {
            CPLDebug("OSR", "TOWGS84 information has been removed. "
                            "It can be kept by setting the OSR_STRIP_TOWGS84 "
                            "configuration option to NO");
            return true;
        }
    }
    return false;
}

/************************************************************************/
/*                      StripTOWGS84IfKnownDatum()                      */
/************************************************************************/

/**
 * \brief Remove TOWGS84 information if the CRS has a known horizontal datum
 *
 * @return true if TOWGS84 has been removed.
 * @since OGR 3.1.0
 */

bool OGRSpatialReference::StripTOWGS84IfKnownDatum()

{
    TAKE_OPTIONAL_LOCK();

    d->refreshProjObj();
    if (!d->m_pj_crs || d->m_pjType != PJ_TYPE_BOUND_CRS)
    {
        return false;
    }
    auto ctxt = d->getPROJContext();
    auto baseCRS = proj_get_source_crs(ctxt, d->m_pj_crs);
    if (proj_get_type(baseCRS) == PJ_TYPE_COMPOUND_CRS)
    {
        proj_destroy(baseCRS);
        return false;
    }

    // Known base CRS code ? Return base CRS
    const char *pszCode = proj_get_id_code(baseCRS, 0);
    if (pszCode)
    {
        d->setPjCRS(baseCRS);
        return true;
    }

    auto datum = proj_crs_get_datum(ctxt, baseCRS);
#if PROJ_VERSION_MAJOR > 7 ||                                                  \
    (PROJ_VERSION_MAJOR == 7 && PROJ_VERSION_MINOR >= 2)
    if (datum == nullptr)
    {
        datum = proj_crs_get_datum_ensemble(ctxt, baseCRS);
    }
#endif
    if (!datum)
    {
        proj_destroy(baseCRS);
        return false;
    }

    // Known datum code ? Return base CRS
    pszCode = proj_get_id_code(datum, 0);
    if (pszCode)
    {
        proj_destroy(datum);
        d->setPjCRS(baseCRS);
        return true;
    }

    const char *name = proj_get_name(datum);
    if (EQUAL(name, "unknown"))
    {
        proj_destroy(datum);
        proj_destroy(baseCRS);
        return false;
    }
    const PJ_TYPE type = PJ_TYPE_GEODETIC_REFERENCE_FRAME;
    PJ_OBJ_LIST *list =
        proj_create_from_name(ctxt, nullptr, name, &type, 1, false, 1, nullptr);

    bool knownDatumName = false;
    if (list)
    {
        if (proj_list_get_count(list) == 1)
        {
            knownDatumName = true;
        }
        proj_list_destroy(list);
    }

    proj_destroy(datum);
    if (knownDatumName)
    {
        d->setPjCRS(baseCRS);
        return true;
    }
    proj_destroy(baseCRS);
    return false;
}

/************************************************************************/
/*                             IsCompound()                             */
/************************************************************************/

/**
 * \brief Check if coordinate system is compound.
 *
 * This method is the same as the C function OSRIsCompound().
 *
 * @return TRUE if this is rooted with a COMPD_CS node.
 */

int OGRSpatialReference::IsCompound() const

{
    TAKE_OPTIONAL_LOCK();

    d->refreshProjObj();
    d->demoteFromBoundCRS();
    bool isCompound = d->m_pjType == PJ_TYPE_COMPOUND_CRS;
    d->undoDemoteFromBoundCRS();
    return isCompound;
}

/************************************************************************/
/*                            IsProjected()                             */
/************************************************************************/

/**
 * \brief Check if projected coordinate system.
 *
 * This method is the same as the C function OSRIsProjected().
 *
 * @return TRUE if this contains a PROJCS node indicating a it is a
 * projected coordinate system. Also if it is a CompoundCRS made of a
 * ProjectedCRS
 */

int OGRSpatialReference::IsProjected() const

{
    TAKE_OPTIONAL_LOCK();

    d->refreshProjObj();
    d->demoteFromBoundCRS();
    bool isProjected = d->m_pjType == PJ_TYPE_PROJECTED_CRS;
    if (d->m_pjType == PJ_TYPE_COMPOUND_CRS)
    {
        auto horizCRS =
            proj_crs_get_sub_crs(d->getPROJContext(), d->m_pj_crs, 0);
        if (horizCRS)
        {
            auto horizCRSType = proj_get_type(horizCRS);
            isProjected = horizCRSType == PJ_TYPE_PROJECTED_CRS;
            if (horizCRSType == PJ_TYPE_BOUND_CRS)
            {
                auto base = proj_get_source_crs(d->getPROJContext(), horizCRS);
                if (base)
                {
                    isProjected = proj_get_type(base) == PJ_TYPE_PROJECTED_CRS;
                    proj_destroy(base);
                }
            }
            proj_destroy(horizCRS);
        }
    }
    d->undoDemoteFromBoundCRS();
    return isProjected;
}

/************************************************************************/
/*                            IsGeocentric()                            */
/************************************************************************/

/**
 * \brief Check if geocentric coordinate system.
 *
 * This method is the same as the C function OSRIsGeocentric().
 *
 * @return TRUE if this contains a GEOCCS node indicating a it is a
 * geocentric coordinate system.
 *
 * @since OGR 1.9.0
 */

int OGRSpatialReference::IsGeocentric() const

{
    TAKE_OPTIONAL_LOCK();

    d->refreshProjObj();
    d->demoteFromBoundCRS();
    bool isGeocentric = d->m_pjType == PJ_TYPE_GEOCENTRIC_CRS;
    d->undoDemoteFromBoundCRS();
    return isGeocentric;
}

/************************************************************************/
/*                            IsEmpty()                                 */
/************************************************************************/

/**
 * \brief Return if the SRS is not set.
 */

bool OGRSpatialReference::IsEmpty() const
{
    TAKE_OPTIONAL_LOCK();

    d->refreshProjObj();
    return d->m_pj_crs == nullptr;
}

/************************************************************************/
/*                            IsGeographic()                            */
/************************************************************************/

/**
 * \brief Check if geographic coordinate system.
 *
 * This method is the same as the C function OSRIsGeographic().
 *
 * @return TRUE if this spatial reference is geographic ... that is the
 * root is a GEOGCS node. Also if it is a CompoundCRS made of a
 * GeographicCRS
 */

int OGRSpatialReference::IsGeographic() const

{
    TAKE_OPTIONAL_LOCK();

    d->refreshProjObj();
    d->demoteFromBoundCRS();
    bool isGeog = d->m_pjType == PJ_TYPE_GEOGRAPHIC_2D_CRS ||
                  d->m_pjType == PJ_TYPE_GEOGRAPHIC_3D_CRS;
    if (d->m_pjType == PJ_TYPE_COMPOUND_CRS)
    {
        auto horizCRS =
            proj_crs_get_sub_crs(d->getPROJContext(), d->m_pj_crs, 0);
        if (horizCRS)
        {
            auto horizCRSType = proj_get_type(horizCRS);
            isGeog = horizCRSType == PJ_TYPE_GEOGRAPHIC_2D_CRS ||
                     horizCRSType == PJ_TYPE_GEOGRAPHIC_3D_CRS;
            if (horizCRSType == PJ_TYPE_BOUND_CRS)
            {
                auto base = proj_get_source_crs(d->getPROJContext(), horizCRS);
                if (base)
                {
                    horizCRSType = proj_get_type(base);
                    isGeog = horizCRSType == PJ_TYPE_GEOGRAPHIC_2D_CRS ||
                             horizCRSType == PJ_TYPE_GEOGRAPHIC_3D_CRS;
                    proj_destroy(base);
                }
            }
            proj_destroy(horizCRS);
        }
    }
    d->undoDemoteFromBoundCRS();
    return isGeog;
}

/************************************************************************/
/*                      IsDerivedGeographic()                           */
/************************************************************************/

/**
 * \brief Check if the CRS is a derived geographic coordinate system.
 * (for example a rotated long/lat grid)
 *
 * This method is the same as the C function OSRIsDerivedGeographic().
 *
 * @since GDAL 3.1.0 and PROJ 6.3.0
 */

int OGRSpatialReference::IsDerivedGeographic() const

{
    TAKE_OPTIONAL_LOCK();

    d->refreshProjObj();
    d->demoteFromBoundCRS();
    const bool isGeog = d->m_pjType == PJ_TYPE_GEOGRAPHIC_2D_CRS ||
                        d->m_pjType == PJ_TYPE_GEOGRAPHIC_3D_CRS;
    const bool isDerivedGeographic =
        isGeog && proj_is_derived_crs(d->getPROJContext(), d->m_pj_crs);
    d->undoDemoteFromBoundCRS();
    return isDerivedGeographic ? TRUE : FALSE;
}

/************************************************************************/
/*                      IsDerivedProjected()                            */
/************************************************************************/

/**
 * \brief Check if the CRS is a derived projected coordinate system.
 *
 * This method is the same as the C function OSRIsDerivedGeographic().
 *
 * @since GDAL 3.9.0 (and may only return non-zero starting with PROJ 9.2.0)
 */

int OGRSpatialReference::IsDerivedProjected() const

{
#if PROJ_AT_LEAST_VERSION(9, 2, 0)
    TAKE_OPTIONAL_LOCK();
    d->refreshProjObj();
    d->demoteFromBoundCRS();
    const bool isDerivedProjected =
        d->m_pjType == PJ_TYPE_DERIVED_PROJECTED_CRS;
    d->undoDemoteFromBoundCRS();
    return isDerivedProjected ? TRUE : FALSE;
#else
    return FALSE;
#endif
}

/************************************************************************/
/*                              IsLocal()                               */
/************************************************************************/

/**
 * \brief Check if local coordinate system.
 *
 * This method is the same as the C function OSRIsLocal().
 *
 * @return TRUE if this spatial reference is local ... that is the
 * root is a LOCAL_CS node.
 */

int OGRSpatialReference::IsLocal() const

{
    TAKE_OPTIONAL_LOCK();
    d->refreshProjObj();
    return d->m_pjType == PJ_TYPE_ENGINEERING_CRS;
}

/************************************************************************/
/*                            IsVertical()                              */
/************************************************************************/

/**
 * \brief Check if vertical coordinate system.
 *
 * This method is the same as the C function OSRIsVertical().
 *
 * @return TRUE if this contains a VERT_CS node indicating a it is a
 * vertical coordinate system. Also if it is a CompoundCRS made of a
 * VerticalCRS
 *
 * @since OGR 1.8.0
 */

int OGRSpatialReference::IsVertical() const

{
    TAKE_OPTIONAL_LOCK();
    d->refreshProjObj();
    d->demoteFromBoundCRS();
    bool isVertical = d->m_pjType == PJ_TYPE_VERTICAL_CRS;
    if (d->m_pjType == PJ_TYPE_COMPOUND_CRS)
    {
        auto vertCRS =
            proj_crs_get_sub_crs(d->getPROJContext(), d->m_pj_crs, 1);
        if (vertCRS)
        {
            const auto vertCRSType = proj_get_type(vertCRS);
            isVertical = vertCRSType == PJ_TYPE_VERTICAL_CRS;
            if (vertCRSType == PJ_TYPE_BOUND_CRS)
            {
                auto base = proj_get_source_crs(d->getPROJContext(), vertCRS);
                if (base)
                {
                    isVertical = proj_get_type(base) == PJ_TYPE_VERTICAL_CRS;
                    proj_destroy(base);
                }
            }
            proj_destroy(vertCRS);
        }
    }
    d->undoDemoteFromBoundCRS();
    return isVertical;
}

/************************************************************************/
/*                            IsDynamic()                               */
/************************************************************************/

/**
 * \brief Check if a CRS is a dynamic CRS.
 *
 * A dynamic CRS relies on a dynamic datum, that is a datum that is not
 * plate-fixed.
 *
 * This method is the same as the C function OSRIsDynamic().
 *
 * @return true if the CRS is dynamic
 *
 * @since OGR 3.4.0
 *
 * @see HasPointMotionOperation()
 */

bool OGRSpatialReference::IsDynamic() const

{
    TAKE_OPTIONAL_LOCK();
    bool isDynamic = false;
    d->refreshProjObj();
    d->demoteFromBoundCRS();
    auto ctxt = d->getPROJContext();
    PJ *horiz = nullptr;
    if (d->m_pjType == PJ_TYPE_COMPOUND_CRS)
    {
        horiz = proj_crs_get_sub_crs(ctxt, d->m_pj_crs, 0);
    }
    else if (d->m_pj_crs)
    {
        horiz = proj_clone(ctxt, d->m_pj_crs);
    }
    if (horiz && proj_get_type(horiz) == PJ_TYPE_BOUND_CRS)
    {
        auto baseCRS = proj_get_source_crs(ctxt, horiz);
        if (baseCRS)
        {
            proj_destroy(horiz);
            horiz = baseCRS;
        }
    }
    auto datum = horiz ? proj_crs_get_datum(ctxt, horiz) : nullptr;
    if (datum)
    {
        const auto type = proj_get_type(datum);
        isDynamic = type == PJ_TYPE_DYNAMIC_GEODETIC_REFERENCE_FRAME ||
                    type == PJ_TYPE_DYNAMIC_VERTICAL_REFERENCE_FRAME;
        if (!isDynamic)
        {
            const char *auth_name = proj_get_id_auth_name(datum, 0);
            const char *code = proj_get_id_code(datum, 0);
            if (auth_name && code && EQUAL(auth_name, "EPSG") &&
                EQUAL(code, "6326"))
            {
                isDynamic = true;
            }
        }
        proj_destroy(datum);
    }
#if PROJ_VERSION_MAJOR > 7 ||                                                  \
    (PROJ_VERSION_MAJOR == 7 && PROJ_VERSION_MINOR >= 2)
    else
    {
        auto ensemble =
            horiz ? proj_crs_get_datum_ensemble(ctxt, horiz) : nullptr;
        if (ensemble)
        {
            auto member = proj_datum_ensemble_get_member(ctxt, ensemble, 0);
            if (member)
            {
                const auto type = proj_get_type(member);
                isDynamic = type == PJ_TYPE_DYNAMIC_GEODETIC_REFERENCE_FRAME ||
                            type == PJ_TYPE_DYNAMIC_VERTICAL_REFERENCE_FRAME;
                proj_destroy(member);
            }
            proj_destroy(ensemble);
        }
    }
#endif
    proj_destroy(horiz);
    d->undoDemoteFromBoundCRS();
    return isDynamic;
}

/************************************************************************/
/*                         HasPointMotionOperation()                    */
/************************************************************************/

/**
 * \brief Check if a CRS has at least an associated point motion operation.
 *
 * Some CRS are not formally declared as dynamic, but may behave as such
 * in practice due to the presence of point motion operation, to perform
 * coordinate epoch changes within the CRS. Typically NAD83(CSRS)v7
 *
 * @return true if the CRS has at least an associated point motion operation.
 *
 * @since OGR 3.8.0 and PROJ 9.4.0
 *
 * @see IsDynamic()
 */

bool OGRSpatialReference::HasPointMotionOperation() const

{
#if PROJ_VERSION_MAJOR > 9 ||                                                  \
    (PROJ_VERSION_MAJOR == 9 && PROJ_VERSION_MINOR >= 4)
    TAKE_OPTIONAL_LOCK();
    d->refreshProjObj();
    d->demoteFromBoundCRS();
    auto ctxt = d->getPROJContext();
    auto res =
        CPL_TO_BOOL(proj_crs_has_point_motion_operation(ctxt, d->m_pj_crs));
    d->undoDemoteFromBoundCRS();
    return res;
#else
    return false;
#endif
}

/************************************************************************/
/*                            CloneGeogCS()                             */
/************************************************************************/

/**
 * \brief Make a duplicate of the GEOGCS node of this OGRSpatialReference
 * object.
 *
 * @return a new SRS, which becomes the responsibility of the caller.
 */
OGRSpatialReference *OGRSpatialReference::CloneGeogCS() const

{
    TAKE_OPTIONAL_LOCK();
    d->refreshProjObj();
    if (d->m_pj_crs)
    {
        if (d->m_pjType == PJ_TYPE_ENGINEERING_CRS)
            return nullptr;

        auto geodCRS =
            proj_crs_get_geodetic_crs(d->getPROJContext(), d->m_pj_crs);
        if (geodCRS)
        {
            OGRSpatialReference *poNewSRS = new OGRSpatialReference(d->getPROJContext());
            if (d->m_pjType == PJ_TYPE_BOUND_CRS)
            {
                PJ *hub_crs =
                    proj_get_target_crs(d->getPROJContext(), d->m_pj_crs);
                PJ *co = proj_crs_get_coordoperation(d->getPROJContext(),
                                                     d->m_pj_crs);
                auto temp = proj_crs_create_bound_crs(d->getPROJContext(),
                                                      geodCRS, hub_crs, co);
                proj_destroy(geodCRS);
                geodCRS = temp;
                proj_destroy(hub_crs);
                proj_destroy(co);
            }

            /* --------------------------------------------------------------------
             */
            /*      We have to reconstruct the GEOGCS node for geocentric */
            /*      coordinate systems. */
            /* --------------------------------------------------------------------
             */
            if (proj_get_type(geodCRS) == PJ_TYPE_GEOCENTRIC_CRS)
            {
                auto datum = proj_crs_get_datum(d->getPROJContext(), geodCRS);
#if PROJ_VERSION_MAJOR > 7 ||                                                  \
    (PROJ_VERSION_MAJOR == 7 && PROJ_VERSION_MINOR >= 2)
                if (datum == nullptr)
                {
                    datum = proj_crs_get_datum_ensemble(d->getPROJContext(),
                                                        geodCRS);
                }
#endif
                if (datum)
                {
                    auto cs = proj_create_ellipsoidal_2D_cs(
                        d->getPROJContext(), PJ_ELLPS2D_LATITUDE_LONGITUDE,
                        nullptr, 0);
                    auto temp = proj_create_geographic_crs_from_datum(
                        d->getPROJContext(), "unnamed", datum, cs);
                    proj_destroy(datum);
                    proj_destroy(cs);
                    proj_destroy(geodCRS);
                    geodCRS = temp;
                }
            }

            poNewSRS->d->setPjCRS(geodCRS);
            if (d->m_axisMappingStrategy == OAMS_TRADITIONAL_GIS_ORDER)
                poNewSRS->SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
            return poNewSRS;
        }
    }
    return nullptr;
}

/************************************************************************/
/*                            IsSameGeogCS()                            */
/************************************************************************/

/**
 * \brief Do the GeogCS'es match?
 *
 * This method is the same as the C function OSRIsSameGeogCS().
 *
 * @param poOther the SRS being compared against.
 *
 * @return TRUE if they are the same or FALSE otherwise.
 */

int OGRSpatialReference::IsSameGeogCS(const OGRSpatialReference *poOther) const

{
    return IsSameGeogCS(poOther, nullptr);
}

/**
 * \brief Do the GeogCS'es match?
 *
 * This method is the same as the C function OSRIsSameGeogCS().
 *
 * @param poOther the SRS being compared against.
 * @param papszOptions options. ignored
 *
 * @return TRUE if they are the same or FALSE otherwise.
 */

int OGRSpatialReference::IsSameGeogCS(const OGRSpatialReference *poOther,
                                      const char *const *papszOptions) const

{
    TAKE_OPTIONAL_LOCK();

    CPL_IGNORE_RET_VAL(papszOptions);

    d->refreshProjObj();
    poOther->d->refreshProjObj();
    if (!d->m_pj_crs || !poOther->d->m_pj_crs)
        return FALSE;
    if (d->m_pjType == PJ_TYPE_ENGINEERING_CRS ||
        d->m_pjType == PJ_TYPE_VERTICAL_CRS ||
        poOther->d->m_pjType == PJ_TYPE_ENGINEERING_CRS ||
        poOther->d->m_pjType == PJ_TYPE_VERTICAL_CRS)
    {
        return FALSE;
    }

    auto geodCRS = proj_crs_get_geodetic_crs(d->getPROJContext(), d->m_pj_crs);
    auto otherGeodCRS =
        proj_crs_get_geodetic_crs(d->getPROJContext(), poOther->d->m_pj_crs);
    if (!geodCRS || !otherGeodCRS)
    {
        proj_destroy(geodCRS);
        proj_destroy(otherGeodCRS);
        return FALSE;
    }

    int ret = proj_is_equivalent_to(
        geodCRS, otherGeodCRS, PJ_COMP_EQUIVALENT_EXCEPT_AXIS_ORDER_GEOGCRS);

    proj_destroy(geodCRS);
    proj_destroy(otherGeodCRS);
    return ret;
}

/************************************************************************/
/*                            IsSameVertCS()                            */
/************************************************************************/

/**
 * \brief Do the VertCS'es match?
 *
 * This method is the same as the C function OSRIsSameVertCS().
 *
 * @param poOther the SRS being compared against.
 *
 * @return TRUE if they are the same or FALSE otherwise.
 */

int OGRSpatialReference::IsSameVertCS(const OGRSpatialReference *poOther) const

{
    TAKE_OPTIONAL_LOCK();

    /* -------------------------------------------------------------------- */
    /*      Does the datum name match?                                      */
    /* -------------------------------------------------------------------- */
    const char *pszThisValue = this->GetAttrValue("VERT_DATUM");
    const char *pszOtherValue = poOther->GetAttrValue("VERT_DATUM");

    if (pszThisValue == nullptr || pszOtherValue == nullptr ||
        !EQUAL(pszThisValue, pszOtherValue))
        return FALSE;

    /* -------------------------------------------------------------------- */
    /*      Do the units match?                                             */
    /* -------------------------------------------------------------------- */
    pszThisValue = this->GetAttrValue("VERT_CS|UNIT", 1);
    if (pszThisValue == nullptr)
        pszThisValue = "1.0";

    pszOtherValue = poOther->GetAttrValue("VERT_CS|UNIT", 1);
    if (pszOtherValue == nullptr)
        pszOtherValue = "1.0";

    if (std::abs(CPLAtof(pszOtherValue) - CPLAtof(pszThisValue)) > 0.00000001)
        return FALSE;

    return TRUE;
}

/************************************************************************/
/*                               IsSame()                               */
/************************************************************************/

/**
 * \brief Do these two spatial references describe the same system ?
 *
 * @param poOtherSRS the SRS being compared to.
 *
 * @return TRUE if equivalent or FALSE otherwise.
 */

int OGRSpatialReference::IsSame(const OGRSpatialReference *poOtherSRS) const

{
    return IsSame(poOtherSRS, nullptr);
}

/**
 * \brief Do these two spatial references describe the same system ?
 *
 * This also takes into account the data axis to CRS axis mapping by default
 *
 * @param poOtherSRS the SRS being compared to.
 * @param papszOptions options. NULL or NULL terminated list of options.
 * Currently supported options are:
 * <ul>
 * <li>IGNORE_DATA_AXIS_TO_SRS_AXIS_MAPPING=YES/NO. Defaults to NO</li>
 * <li>CRITERION=STRICT/EQUIVALENT/EQUIVALENT_EXCEPT_AXIS_ORDER_GEOGCRS.
 *     Defaults to EQUIVALENT_EXCEPT_AXIS_ORDER_GEOGCRS.</li>
 * <li>IGNORE_COORDINATE_EPOCH=YES/NO. Defaults to NO</li>
 * </ul>
 *
 * @return TRUE if equivalent or FALSE otherwise.
 */

int OGRSpatialReference::IsSame(const OGRSpatialReference *poOtherSRS,
                                const char *const *papszOptions) const

{
    TAKE_OPTIONAL_LOCK();

    d->refreshProjObj();
    poOtherSRS->d->refreshProjObj();
    if (!d->m_pj_crs || !poOtherSRS->d->m_pj_crs)
        return d->m_pj_crs == poOtherSRS->d->m_pj_crs;
    if (!CPLTestBool(CSLFetchNameValueDef(
            papszOptions, "IGNORE_DATA_AXIS_TO_SRS_AXIS_MAPPING", "NO")))
    {
        if (d->m_axisMapping != poOtherSRS->d->m_axisMapping)
            return false;
    }

    if (!CPLTestBool(CSLFetchNameValueDef(papszOptions,
                                          "IGNORE_COORDINATE_EPOCH", "NO")))
    {
        if (d->m_coordinateEpoch != poOtherSRS->d->m_coordinateEpoch)
            return false;
    }

    bool reboundSelf = false;
    bool reboundOther = false;
    if (d->m_pjType == PJ_TYPE_BOUND_CRS &&
        poOtherSRS->d->m_pjType != PJ_TYPE_BOUND_CRS)
    {
        d->demoteFromBoundCRS();
        reboundSelf = true;
    }
    else if (d->m_pjType != PJ_TYPE_BOUND_CRS &&
             poOtherSRS->d->m_pjType == PJ_TYPE_BOUND_CRS)
    {
        poOtherSRS->d->demoteFromBoundCRS();
        reboundOther = true;
    }

    PJ_COMPARISON_CRITERION criterion =
        PJ_COMP_EQUIVALENT_EXCEPT_AXIS_ORDER_GEOGCRS;
    const char *pszCriterion = CSLFetchNameValueDef(
        papszOptions, "CRITERION", "EQUIVALENT_EXCEPT_AXIS_ORDER_GEOGCRS");
    if (EQUAL(pszCriterion, "STRICT"))
        criterion = PJ_COMP_STRICT;
    else if (EQUAL(pszCriterion, "EQUIVALENT"))
        criterion = PJ_COMP_EQUIVALENT;
    else if (!EQUAL(pszCriterion, "EQUIVALENT_EXCEPT_AXIS_ORDER_GEOGCRS"))
    {
        CPLError(CE_Warning, CPLE_NotSupported,
                 "Unsupported value for CRITERION: %s", pszCriterion);
    }
    int ret =
        proj_is_equivalent_to(d->m_pj_crs, poOtherSRS->d->m_pj_crs, criterion);
    if (reboundSelf)
        d->undoDemoteFromBoundCRS();
    if (reboundOther)
        poOtherSRS->d->undoDemoteFromBoundCRS();

    return ret;
}

/************************************************************************/
/*                           OSRFreeSRSArray()                          */
/************************************************************************/

/**
 * \brief Free return of OSRIdentifyMatches()
 *
 * @param pahSRS array of SRS (must be NULL terminated)
 * @since GDAL 2.3
 */
void OSRFreeSRSArray(OGRSpatialReferenceH *pahSRS)
{
    if (pahSRS != nullptr)
    {
        for (int i = 0; pahSRS[i] != nullptr; ++i)
        {
            OSRRelease(pahSRS[i]);
        }
        CPLFree(pahSRS);
    }
}

/************************************************************************/
/*                         FindBestMatch()                              */
/************************************************************************/

/**
 * \brief Try to identify the best match between the passed SRS and a related
 * SRS in a catalog.
 *
 * This is a wrapper over OGRSpatialReference::FindMatches() that takes care
 * of filtering its output.
 * Only matches whose confidence is greater or equal to nMinimumMatchConfidence
 * will be considered. If there is a single match, it is returned.
 * If there are several matches, only return the one under the
 * pszPreferredAuthority, if there is a single one under that authority.
 *
 * @param nMinimumMatchConfidence Minimum match confidence (value between 0 and
 * 100). If set to 0, 90 is used.
 * @param pszPreferredAuthority Preferred CRS authority. If set to nullptr,
 * "EPSG" is used.
 * @param papszOptions NULL terminated list of options or NULL. No option is
 * defined at time of writing.
 *
 * @return a new OGRSpatialReference* object to free with Release(), or nullptr
 *
 * @since GDAL 3.6
 * @see OGRSpatialReference::FindMatches()
 */
OGRSpatialReference *
OGRSpatialReference::FindBestMatch(int nMinimumMatchConfidence,
                                   const char *pszPreferredAuthority,
                                   CSLConstList papszOptions) const
{
    TAKE_OPTIONAL_LOCK();

    CPL_IGNORE_RET_VAL(papszOptions);  // ignored for now.

    if (nMinimumMatchConfidence == 0)
        nMinimumMatchConfidence = 90;
    if (pszPreferredAuthority == nullptr)
        pszPreferredAuthority = "EPSG";

    // Try to identify the CRS with the database
    int nEntries = 0;
    int *panConfidence = nullptr;
    OGRSpatialReferenceH *pahSRS =
        FindMatches(nullptr, &nEntries, &panConfidence);
    if (nEntries == 1 && panConfidence[0] >= nMinimumMatchConfidence)
    {
        std::vector<double> adfTOWGS84(7);
        if (GetTOWGS84(&adfTOWGS84[0], 7) != OGRERR_NONE)
        {
            adfTOWGS84.clear();
        }

        auto poSRS = OGRSpatialReference::FromHandle(pahSRS[0]);

        auto poBaseGeogCRS =
            std::unique_ptr<OGRSpatialReference>(poSRS->CloneGeogCS());

        // If the base geographic SRS of the SRS is EPSG:4326
        // with TOWGS84[0,0,0,0,0,0], then just use the official
        // SRS code
        // Same with EPSG:4258 (ETRS89), since it's the only known
        // TOWGS84[] style transformation to WGS 84, and given the
        // "fuzzy" nature of both ETRS89 and WGS 84, there's little
        // chance that a non-NULL TOWGS84[] will emerge.
        const char *pszAuthorityName = nullptr;
        const char *pszAuthorityCode = nullptr;
        const char *pszBaseAuthorityName = nullptr;
        const char *pszBaseAuthorityCode = nullptr;
        if (adfTOWGS84 == std::vector<double>(7) &&
            (pszAuthorityName = poSRS->GetAuthorityName(nullptr)) != nullptr &&
            EQUAL(pszAuthorityName, "EPSG") &&
            (pszAuthorityCode = poSRS->GetAuthorityCode(nullptr)) != nullptr &&
            (pszBaseAuthorityName = poBaseGeogCRS->GetAuthorityName(nullptr)) !=
                nullptr &&
            EQUAL(pszBaseAuthorityName, "EPSG") &&
            (pszBaseAuthorityCode = poBaseGeogCRS->GetAuthorityCode(nullptr)) !=
                nullptr &&
            (EQUAL(pszBaseAuthorityCode, "4326") ||
             EQUAL(pszBaseAuthorityCode, "4258")))
        {
            poSRS->importFromEPSG(atoi(pszAuthorityCode));
        }

        CPLFree(pahSRS);
        CPLFree(panConfidence);

        return poSRS;
    }
    else
    {
        // If there are several matches >= nMinimumMatchConfidence, take the
        // only one that is under pszPreferredAuthority
        int iBestEntry = -1;
        for (int i = 0; i < nEntries; i++)
        {
            if (panConfidence[i] >= nMinimumMatchConfidence)
            {
                const char *pszAuthName =
                    OGRSpatialReference::FromHandle(pahSRS[i])
                        ->GetAuthorityName(nullptr);
                if (pszAuthName != nullptr &&
                    EQUAL(pszAuthName, pszPreferredAuthority))
                {
                    if (iBestEntry < 0)
                        iBestEntry = i;
                    else
                    {
                        iBestEntry = -1;
                        break;
                    }
                }
            }
        }
        if (iBestEntry >= 0)
        {
            auto poRet = OGRSpatialReference::FromHandle(pahSRS[0])->Clone();
            OSRFreeSRSArray(pahSRS);
            CPLFree(panConfidence);
            return poRet;
        }
    }
    OSRFreeSRSArray(pahSRS);
    CPLFree(panConfidence);
    return nullptr;
}

/************************************************************************/
/*                             SetTOWGS84()                             */
/************************************************************************/

/**
 * \brief Set the Bursa-Wolf conversion to WGS84.
 *
 * This will create the TOWGS84 node as a child of the DATUM.  It will fail
 * if there is no existing DATUM node. It will replace
 * an existing TOWGS84 node if there is one.
 *
 * The parameters have the same meaning as EPSG transformation 9606
 * (Position Vector 7-param. transformation).
 *
 * This method is the same as the C function OSRSetTOWGS84().
 *
 * @param dfDX X child in meters.
 * @param dfDY Y child in meters.
 * @param dfDZ Z child in meters.
 * @param dfEX X rotation in arc seconds (optional, defaults to zero).
 * @param dfEY Y rotation in arc seconds (optional, defaults to zero).
 * @param dfEZ Z rotation in arc seconds (optional, defaults to zero).
 * @param dfPPM scaling factor (parts per million).
 *
 * @return OGRERR_NONE on success.
 */

OGRErr OGRSpatialReference::SetTOWGS84(double dfDX, double dfDY, double dfDZ,
                                       double dfEX, double dfEY, double dfEZ,
                                       double dfPPM)

{
    TAKE_OPTIONAL_LOCK();

    d->refreshProjObj();
    if (d->m_pj_crs == nullptr)
    {
        return OGRERR_FAILURE;
    }

    // Remove existing BoundCRS
    if (d->m_pjType == PJ_TYPE_BOUND_CRS)
    {
        auto baseCRS = proj_get_source_crs(d->getPROJContext(), d->m_pj_crs);
        if (!baseCRS)
            return OGRERR_FAILURE;
        d->setPjCRS(baseCRS);
    }

    PJ_PARAM_DESCRIPTION params[7];

    params[0].name = EPSG_NAME_PARAMETER_X_AXIS_TRANSLATION;
    params[0].auth_name = "EPSG";
    params[0].code = XSTRINGIFY(EPSG_CODE_PARAMETER_X_AXIS_TRANSLATION);
    params[0].value = dfDX;
    params[0].unit_name = "metre";
    params[0].unit_conv_factor = 1.0;
    params[0].unit_type = PJ_UT_LINEAR;

    params[1].name = EPSG_NAME_PARAMETER_Y_AXIS_TRANSLATION;
    params[1].auth_name = "EPSG";
    params[1].code = XSTRINGIFY(EPSG_CODE_PARAMETER_Y_AXIS_TRANSLATION);
    params[1].value = dfDY;
    params[1].unit_name = "metre";
    params[1].unit_conv_factor = 1.0;
    params[1].unit_type = PJ_UT_LINEAR;

    params[2].name = EPSG_NAME_PARAMETER_Z_AXIS_TRANSLATION;
    params[2].auth_name = "EPSG";
    params[2].code = XSTRINGIFY(EPSG_CODE_PARAMETER_Z_AXIS_TRANSLATION);
    params[2].value = dfDZ;
    params[2].unit_name = "metre";
    params[2].unit_conv_factor = 1.0;
    params[2].unit_type = PJ_UT_LINEAR;

    params[3].name = EPSG_NAME_PARAMETER_X_AXIS_ROTATION;
    params[3].auth_name = "EPSG";
    params[3].code = XSTRINGIFY(EPSG_CODE_PARAMETER_X_AXIS_ROTATION);
    params[3].value = dfEX;
    params[3].unit_name = "arc-second";
    params[3].unit_conv_factor = 1. / 3600 * M_PI / 180;
    params[3].unit_type = PJ_UT_ANGULAR;

    params[4].name = EPSG_NAME_PARAMETER_Y_AXIS_ROTATION;
    params[4].auth_name = "EPSG";
    params[4].code = XSTRINGIFY(EPSG_CODE_PARAMETER_Y_AXIS_ROTATION);
    params[4].value = dfEY;
    params[4].unit_name = "arc-second";
    params[4].unit_conv_factor = 1. / 3600 * M_PI / 180;
    params[4].unit_type = PJ_UT_ANGULAR;

    params[5].name = EPSG_NAME_PARAMETER_Z_AXIS_ROTATION;
    params[5].auth_name = "EPSG";
    params[5].code = XSTRINGIFY(EPSG_CODE_PARAMETER_Z_AXIS_ROTATION);
    params[5].value = dfEZ;
    params[5].unit_name = "arc-second";
    params[5].unit_conv_factor = 1. / 3600 * M_PI / 180;
    params[5].unit_type = PJ_UT_ANGULAR;

    params[6].name = EPSG_NAME_PARAMETER_SCALE_DIFFERENCE;
    params[6].auth_name = "EPSG";
    params[6].code = XSTRINGIFY(EPSG_CODE_PARAMETER_SCALE_DIFFERENCE);
    params[6].value = dfPPM;
    params[6].unit_name = "parts per million";
    params[6].unit_conv_factor = 1e-6;
    params[6].unit_type = PJ_UT_SCALE;

    auto sourceCRS =
        proj_crs_get_geodetic_crs(d->getPROJContext(), d->m_pj_crs);
    if (!sourceCRS)
    {
        return OGRERR_FAILURE;
    }

    const auto sourceType = proj_get_type(sourceCRS);

    auto targetCRS = proj_create_from_database(
        d->getPROJContext(), "EPSG",
        sourceType == PJ_TYPE_GEOGRAPHIC_2D_CRS   ? "4326"
        : sourceType == PJ_TYPE_GEOGRAPHIC_3D_CRS ? "4979"
                                                  : "4978",
        PJ_CATEGORY_CRS, false, nullptr);
    if (!targetCRS)
    {
        proj_destroy(sourceCRS);
        return OGRERR_FAILURE;
    }

    CPLString osMethodCode;
    osMethodCode.Printf("%d",
                        sourceType == PJ_TYPE_GEOGRAPHIC_2D_CRS
                            ? EPSG_CODE_METHOD_POSITION_VECTOR_GEOGRAPHIC_2D
                        : sourceType == PJ_TYPE_GEOGRAPHIC_3D_CRS
                            ? EPSG_CODE_METHOD_POSITION_VECTOR_GEOGRAPHIC_3D
                            : EPSG_CODE_METHOD_POSITION_VECTOR_GEOCENTRIC);

    auto transf = proj_create_transformation(
        d->getPROJContext(), "Transformation to WGS84", nullptr, nullptr,
        sourceCRS, targetCRS, nullptr,
        sourceType == PJ_TYPE_GEOGRAPHIC_2D_CRS
            ? EPSG_NAME_METHOD_POSITION_VECTOR_GEOGRAPHIC_2D
        : sourceType == PJ_TYPE_GEOGRAPHIC_3D_CRS
            ? EPSG_NAME_METHOD_POSITION_VECTOR_GEOGRAPHIC_3D
            : EPSG_NAME_METHOD_POSITION_VECTOR_GEOCENTRIC,
        "EPSG", osMethodCode.c_str(), 7, params, -1);
    proj_destroy(sourceCRS);
    if (!transf)
    {
        proj_destroy(targetCRS);
        return OGRERR_FAILURE;
    }

    auto newBoundCRS = proj_crs_create_bound_crs(
        d->getPROJContext(), d->m_pj_crs, targetCRS, transf);
    proj_destroy(transf);
    proj_destroy(targetCRS);
    if (!newBoundCRS)
    {
        return OGRERR_FAILURE;
    }

    d->setPjCRS(newBoundCRS);
    return OGRERR_NONE;
}

/************************************************************************/
/*                             GetTOWGS84()                             */
/************************************************************************/

/**
 * \brief Fetch TOWGS84 parameters, if available.
 *
 * The parameters have the same meaning as EPSG transformation 9606
 * (Position Vector 7-param. transformation).
 *
 * @param padfCoeff array into which up to 7 coefficients are placed.
 * @param nCoeffCount size of padfCoeff - defaults to 7.
 *
 * @return OGRERR_NONE on success, or OGRERR_FAILURE if there is no
 * TOWGS84 node available.
 */

OGRErr OGRSpatialReference::GetTOWGS84(double *padfCoeff, int nCoeffCount) const

{
    TAKE_OPTIONAL_LOCK();

    d->refreshProjObj();
    if (d->m_pjType != PJ_TYPE_BOUND_CRS)
        return OGRERR_FAILURE;

    memset(padfCoeff, 0, sizeof(double) * nCoeffCount);

    auto transf = proj_crs_get_coordoperation(d->getPROJContext(), d->m_pj_crs);
    int success = proj_coordoperation_get_towgs84_values(
        d->getPROJContext(), transf, padfCoeff, nCoeffCount, false);
    proj_destroy(transf);

    return success ? OGRERR_NONE : OGRERR_FAILURE;
}
/************************************************************************/
/*                         IsAngularParameter()                         */
/************************************************************************/

/** Is the passed projection parameter an angular one?
 *
 * @return TRUE or FALSE
 */

/* static */
int OGRSpatialReference::IsAngularParameter(const char *pszParameterName)

{
    if (STARTS_WITH_CI(pszParameterName, "long") ||
        STARTS_WITH_CI(pszParameterName, "lati") ||
        EQUAL(pszParameterName, SRS_PP_CENTRAL_MERIDIAN) ||
        STARTS_WITH_CI(pszParameterName, "standard_parallel") ||
        EQUAL(pszParameterName, SRS_PP_AZIMUTH) ||
        EQUAL(pszParameterName, SRS_PP_RECTIFIED_GRID_ANGLE))
        return TRUE;

    return FALSE;
}

/************************************************************************/
/*                        IsLongitudeParameter()                        */
/************************************************************************/

/** Is the passed projection parameter an angular longitude
 * (relative to a prime meridian)?
 *
 * @return TRUE or FALSE
 */

/* static */
int OGRSpatialReference::IsLongitudeParameter(const char *pszParameterName)

{
    if (STARTS_WITH_CI(pszParameterName, "long") ||
        EQUAL(pszParameterName, SRS_PP_CENTRAL_MERIDIAN))
        return TRUE;

    return FALSE;
}

/************************************************************************/
/*                         IsLinearParameter()                          */
/************************************************************************/

/** Is the passed projection parameter an linear one measured in meters or
 * some similar linear measure.
 *
 * @return TRUE or FALSE
 */

/* static */
int OGRSpatialReference::IsLinearParameter(const char *pszParameterName)

{
    if (STARTS_WITH_CI(pszParameterName, "false_") ||
        EQUAL(pszParameterName, SRS_PP_SATELLITE_HEIGHT))
        return TRUE;

    return FALSE;
}

/************************************************************************/
/*                            GetNormInfo()                             */
/************************************************************************/

/**
 * \brief Set the internal information for normalizing linear, and angular
 * values.
 */
void OGRSpatialReference::GetNormInfo() const

{
    TAKE_OPTIONAL_LOCK();

    if (d->bNormInfoSet)
        return;

    /* -------------------------------------------------------------------- */
    /*      Initialize values.                                              */
    /* -------------------------------------------------------------------- */
    d->bNormInfoSet = TRUE;

    d->dfFromGreenwich = GetPrimeMeridian(nullptr);
    d->dfToMeter = GetLinearUnits(nullptr);
    d->dfToDegrees = GetAngularUnits(nullptr) / CPLAtof(SRS_UA_DEGREE_CONV);
    if (fabs(d->dfToDegrees - 1.0) < 0.000000001)
        d->dfToDegrees = 1.0;
}

/************************************************************************/
/*                            SetExtension()                            */
/************************************************************************/
/**
 * \brief Set extension value.
 *
 * Set the value of the named EXTENSION item for the identified
 * target node.
 *
 * @param pszTargetKey the name or path to the parent node of the EXTENSION.
 * @param pszName the name of the extension being fetched.
 * @param pszValue the value to set
 *
 * @return OGRERR_NONE on success
 */

OGRErr OGRSpatialReference::SetExtension(const char *pszTargetKey,
                                         const char *pszName,
                                         const char *pszValue)

{
    TAKE_OPTIONAL_LOCK();

    /* -------------------------------------------------------------------- */
    /*      Find the target node.                                           */
    /* -------------------------------------------------------------------- */
    OGR_SRSNode *poNode = nullptr;

    if (pszTargetKey == nullptr)
        poNode = GetRoot();
    else
        poNode = GetAttrNode(pszTargetKey);

    if (poNode == nullptr)
        return OGRERR_FAILURE;

    /* -------------------------------------------------------------------- */
    /*      Fetch matching EXTENSION if there is one.                       */
    /* -------------------------------------------------------------------- */
    for (int i = poNode->GetChildCount() - 1; i >= 0; i--)
    {
        OGR_SRSNode *poChild = poNode->GetChild(i);

        if (EQUAL(poChild->GetValue(), "EXTENSION") &&
            poChild->GetChildCount() >= 2)
        {
            if (EQUAL(poChild->GetChild(0)->GetValue(), pszName))
            {
                poChild->GetChild(1)->SetValue(pszValue);
                return OGRERR_NONE;
            }
        }
    }

    /* -------------------------------------------------------------------- */
    /*      Create a new EXTENSION node.                                    */
    /* -------------------------------------------------------------------- */
    OGR_SRSNode *poAuthNode = new OGR_SRSNode("EXTENSION");
    poAuthNode->AddChild(new OGR_SRSNode(pszName));
    poAuthNode->AddChild(new OGR_SRSNode(pszValue));

    poNode->AddChild(poAuthNode);

    return OGRERR_NONE;
}

/************************************************************************/
/*                              GetAxesCount()                          */
/************************************************************************/

/**
 * \brief Return the number of axis of the coordinate system of the CRS.
 *
 * @since GDAL 3.0
 */
int OGRSpatialReference::GetAxesCount() const
{
    TAKE_OPTIONAL_LOCK();

    int axisCount = 0;
    d->refreshProjObj();
    if (d->m_pj_crs == nullptr)
    {
        return 0;
    }
    d->demoteFromBoundCRS();
    auto ctxt = d->getPROJContext();
    if (d->m_pjType == PJ_TYPE_COMPOUND_CRS)
    {
        for (int i = 0;; i++)
        {
            auto subCRS = proj_crs_get_sub_crs(ctxt, d->m_pj_crs, i);
            if (!subCRS)
                break;
            if (proj_get_type(subCRS) == PJ_TYPE_BOUND_CRS)
            {
                auto baseCRS = proj_get_source_crs(ctxt, subCRS);
                if (baseCRS)
                {
                    proj_destroy(subCRS);
                    subCRS = baseCRS;
                }
            }
            auto cs = proj_crs_get_coordinate_system(ctxt, subCRS);
            if (cs)
            {
                axisCount += proj_cs_get_axis_count(ctxt, cs);
                proj_destroy(cs);
            }
            proj_destroy(subCRS);
        }
    }
    else
    {
        auto cs = proj_crs_get_coordinate_system(ctxt, d->m_pj_crs);
        if (cs)
        {
            axisCount = proj_cs_get_axis_count(ctxt, cs);
            proj_destroy(cs);
        }
    }
    d->undoDemoteFromBoundCRS();
    return axisCount;
}

/************************************************************************/
/*                              GetAxis()                               */
/************************************************************************/

/**
 * \brief Fetch the orientation of one axis.
 *
 * Fetches the request axis (iAxis - zero based) from the
 * indicated portion of the coordinate system (pszTargetKey) which
 * should be either "GEOGCS" or "PROJCS".
 *
 * No CPLError is issued on routine failures (such as not finding the AXIS).
 *
 * This method is equivalent to the C function OSRGetAxis().
 *
 * @param pszTargetKey the coordinate system part to query ("PROJCS" or
 * "GEOGCS").
 * @param iAxis the axis to query (0 for first, 1 for second, 2 for third).
 * @param peOrientation location into which to place the fetch orientation, may
 * be NULL.
 * @param pdfConvUnit (GDAL >= 3.4) Location into which to place axis conversion
 * factor. May be NULL. Only set if pszTargetKey == NULL
 *
 * @return the name of the axis or NULL on failure.
 */

const char *OGRSpatialReference::GetAxis(const char *pszTargetKey, int iAxis,
                                         OGRAxisOrientation *peOrientation,
                                         double *pdfConvUnit) const

{
    TAKE_OPTIONAL_LOCK();

    if (peOrientation != nullptr)
        *peOrientation = OAO_Other;
    if (pdfConvUnit != nullptr)
        *pdfConvUnit = 0;

    d->refreshProjObj();
    if (d->m_pj_crs == nullptr)
    {
        return nullptr;
    }

    pszTargetKey = d->nullifyTargetKeyIfPossible(pszTargetKey);
    if (pszTargetKey == nullptr && iAxis <= 2)
    {
        auto ctxt = d->getPROJContext();

        int iAxisModified = iAxis;

        d->demoteFromBoundCRS();

        PJ *cs = nullptr;
        if (d->m_pjType == PJ_TYPE_COMPOUND_CRS)
        {
            auto horizCRS = proj_crs_get_sub_crs(ctxt, d->m_pj_crs, 0);
            if (horizCRS)
            {
                if (proj_get_type(horizCRS) == PJ_TYPE_BOUND_CRS)
                {
                    auto baseCRS = proj_get_source_crs(ctxt, horizCRS);
                    if (baseCRS)
                    {
                        proj_destroy(horizCRS);
                        horizCRS = baseCRS;
                    }
                }
                cs = proj_crs_get_coordinate_system(ctxt, horizCRS);
                proj_destroy(horizCRS);
                if (cs)
                {
                    if (iAxisModified >= proj_cs_get_axis_count(ctxt, cs))
                    {
                        iAxisModified -= proj_cs_get_axis_count(ctxt, cs);
                        proj_destroy(cs);
                        cs = nullptr;
                    }
                }
            }

            if (cs == nullptr)
            {
                auto vertCRS = proj_crs_get_sub_crs(ctxt, d->m_pj_crs, 1);
                if (vertCRS)
                {
                    if (proj_get_type(vertCRS) == PJ_TYPE_BOUND_CRS)
                    {
                        auto baseCRS = proj_get_source_crs(ctxt, vertCRS);
                        if (baseCRS)
                        {
                            proj_destroy(vertCRS);
                            vertCRS = baseCRS;
                        }
                    }

                    cs = proj_crs_get_coordinate_system(ctxt, vertCRS);
                    proj_destroy(vertCRS);
                }
            }
        }
        else
        {
            cs = proj_crs_get_coordinate_system(ctxt, d->m_pj_crs);
        }

        if (cs)
        {
            const char *pszName = nullptr;
            const char *pszOrientation = nullptr;
            double dfConvFactor = 0.0;
            proj_cs_get_axis_info(ctxt, cs, iAxisModified, &pszName, nullptr,
                                  &pszOrientation, &dfConvFactor, nullptr,
                                  nullptr, nullptr);

            if (pdfConvUnit != nullptr)
            {
                *pdfConvUnit = dfConvFactor;
            }

            if (pszName && pszOrientation)
            {
                d->m_osAxisName[iAxis] = pszName;
                if (peOrientation)
                {
                    if (EQUAL(pszOrientation, "NORTH"))
                        *peOrientation = OAO_North;
                    else if (EQUAL(pszOrientation, "EAST"))
                        *peOrientation = OAO_East;
                    else if (EQUAL(pszOrientation, "SOUTH"))
                        *peOrientation = OAO_South;
                    else if (EQUAL(pszOrientation, "WEST"))
                        *peOrientation = OAO_West;
                    else if (EQUAL(pszOrientation, "UP"))
                        *peOrientation = OAO_Up;
                    else if (EQUAL(pszOrientation, "DOWN"))
                        *peOrientation = OAO_Down;
                }
                proj_destroy(cs);
                d->undoDemoteFromBoundCRS();
                return d->m_osAxisName[iAxis].c_str();
            }
            proj_destroy(cs);
        }
        d->undoDemoteFromBoundCRS();
    }

    /* -------------------------------------------------------------------- */
    /*      Find the target node.                                           */
    /* -------------------------------------------------------------------- */
    const OGR_SRSNode *poNode = nullptr;

    if (pszTargetKey == nullptr)
        poNode = GetRoot();
    else
        poNode = GetAttrNode(pszTargetKey);

    if (poNode == nullptr)
        return nullptr;

    /* -------------------------------------------------------------------- */
    /*      Find desired child AXIS.                                        */
    /* -------------------------------------------------------------------- */
    const OGR_SRSNode *poAxis = nullptr;
    const int nChildCount = poNode->GetChildCount();

    for (int iChild = 0; iChild < nChildCount; iChild++)
    {
        const OGR_SRSNode *poChild = poNode->GetChild(iChild);

        if (!EQUAL(poChild->GetValue(), "AXIS"))
            continue;

        if (iAxis == 0)
        {
            poAxis = poChild;
            break;
        }
        iAxis--;
    }

    if (poAxis == nullptr)
        return nullptr;

    if (poAxis->GetChildCount() < 2)
        return nullptr;

    /* -------------------------------------------------------------------- */
    /*      Extract name and orientation if possible.                       */
    /* -------------------------------------------------------------------- */
    if (peOrientation != nullptr)
    {
        const char *pszOrientation = poAxis->GetChild(1)->GetValue();

        if (EQUAL(pszOrientation, "NORTH"))
            *peOrientation = OAO_North;
        else if (EQUAL(pszOrientation, "EAST"))
            *peOrientation = OAO_East;
        else if (EQUAL(pszOrientation, "SOUTH"))
            *peOrientation = OAO_South;
        else if (EQUAL(pszOrientation, "WEST"))
            *peOrientation = OAO_West;
        else if (EQUAL(pszOrientation, "UP"))
            *peOrientation = OAO_Up;
        else if (EQUAL(pszOrientation, "DOWN"))
            *peOrientation = OAO_Down;
        else if (EQUAL(pszOrientation, "OTHER"))
            *peOrientation = OAO_Other;
        else
        {
            CPLDebug("OSR", "Unrecognized orientation value '%s'.",
                     pszOrientation);
        }
    }

    return poAxis->GetChild(0)->GetValue();
}

/************************************************************************/
/*                         OSRAxisEnumToName()                          */
/************************************************************************/

/**
 * \brief Return the string representation for the OGRAxisOrientation
 * enumeration.
 *
 * For example "NORTH" for OAO_North.
 *
 * @return an internal string
 */
const char *OSRAxisEnumToName(OGRAxisOrientation eOrientation)

{
    if (eOrientation == OAO_North)
        return "NORTH";
    if (eOrientation == OAO_East)
        return "EAST";
    if (eOrientation == OAO_South)
        return "SOUTH";
    if (eOrientation == OAO_West)
        return "WEST";
    if (eOrientation == OAO_Up)
        return "UP";
    if (eOrientation == OAO_Down)
        return "DOWN";
    if (eOrientation == OAO_Other)
        return "OTHER";

    return "UNKNOWN";
}

/************************************************************************/
/*                              SetAxes()                               */
/************************************************************************/

/**
 * \brief Set the axes for a coordinate system.
 *
 * Set the names, and orientations of the axes for either a projected
 * (PROJCS) or geographic (GEOGCS) coordinate system.
 *
 * This method is equivalent to the C function OSRSetAxes().
 *
 * @param pszTargetKey either "PROJCS" or "GEOGCS", must already exist in SRS.
 * @param pszXAxisName name of first axis, normally "Long" or "Easting".
 * @param eXAxisOrientation normally OAO_East.
 * @param pszYAxisName name of second axis, normally "Lat" or "Northing".
 * @param eYAxisOrientation normally OAO_North.
 *
 * @return OGRERR_NONE on success or an error code.
 */

OGRErr OGRSpatialReference::SetAxes(const char *pszTargetKey,
                                    const char *pszXAxisName,
                                    OGRAxisOrientation eXAxisOrientation,
                                    const char *pszYAxisName,
                                    OGRAxisOrientation eYAxisOrientation)

{
    TAKE_OPTIONAL_LOCK();

    /* -------------------------------------------------------------------- */
    /*      Find the target node.                                           */
    /* -------------------------------------------------------------------- */
    OGR_SRSNode *poNode = nullptr;

    if (pszTargetKey == nullptr)
        poNode = GetRoot();
    else
        poNode = GetAttrNode(pszTargetKey);

    if (poNode == nullptr)
        return OGRERR_FAILURE;

    /* -------------------------------------------------------------------- */
    /*      Strip any existing AXIS children.                               */
    /* -------------------------------------------------------------------- */
    while (poNode->FindChild("AXIS") >= 0)
        poNode->DestroyChild(poNode->FindChild("AXIS"));

    /* -------------------------------------------------------------------- */
    /*      Insert desired axes                                             */
    /* -------------------------------------------------------------------- */
    OGR_SRSNode *poAxis = new OGR_SRSNode("AXIS");

    poAxis->AddChild(new OGR_SRSNode(pszXAxisName));
    poAxis->AddChild(new OGR_SRSNode(OSRAxisEnumToName(eXAxisOrientation)));

    poNode->AddChild(poAxis);

    poAxis = new OGR_SRSNode("AXIS");

    poAxis->AddChild(new OGR_SRSNode(pszYAxisName));
    poAxis->AddChild(new OGR_SRSNode(OSRAxisEnumToName(eYAxisOrientation)));

    poNode->AddChild(poAxis);

    return OGRERR_NONE;
}

/************************************************************************/
/*                        OSRCalcInvFlattening()                        */
/************************************************************************/

/**
 * \brief Compute inverse flattening from semi-major and semi-minor axis
 *
 * @param dfSemiMajor Semi-major axis length.
 * @param dfSemiMinor Semi-minor axis length.
 *
 * @return inverse flattening, or 0 if both axis are equal.
 * @since GDAL 2.0
 */

double OSRCalcInvFlattening(double dfSemiMajor, double dfSemiMinor)
{
    if (fabs(dfSemiMajor - dfSemiMinor) < 1e-1)
        return 0;
    if (dfSemiMajor <= 0 || dfSemiMinor <= 0 || dfSemiMinor > dfSemiMajor)
    {
        CPLError(CE_Failure, CPLE_IllegalArg,
                 "OSRCalcInvFlattening(): Wrong input values");
        return 0;
    }

    return dfSemiMajor / (dfSemiMajor - dfSemiMinor);
}

/************************************************************************/
/*                          importFromProj4()                           */
/************************************************************************/

/**
 * \brief Import PROJ coordinate string.
 *
 * The OGRSpatialReference is initialized from the passed PROJs style
 * coordinate system string.
 *
 * Example:
 *   pszProj4 = "+proj=utm +zone=11 +datum=WGS84"
 *
 * It is also possible to import "+init=epsg:n" style definitions. Those are
 * a legacy syntax that should be avoided in the future. In particular they will
 * result in CRS objects whose axis order might not correspond to the official
 * EPSG axis order.
 *
 * This method is the equivalent of the C function OSRImportFromProj4().
 *
 * @param pszProj4 the PROJ style string.
 *
 * @return OGRERR_NONE on success or OGRERR_CORRUPT_DATA on failure.
 */

OGRErr OGRSpatialReference::importFromProj4(const char *pszProj4)

{
    TAKE_OPTIONAL_LOCK();

    if (strlen(pszProj4) >= 10000)
    {
        CPLError(CE_Failure, CPLE_AppDefined, "Too long PROJ string");
        return OGRERR_CORRUPT_DATA;
    }

    /* -------------------------------------------------------------------- */
    /*      Clear any existing definition.                                  */
    /* -------------------------------------------------------------------- */
    Clear();

    CPLString osProj4(pszProj4);
    if (osProj4.find("type=crs") == std::string::npos)
    {
        osProj4 += " +type=crs";
    }

    if (osProj4.find("+init=epsg:") != std::string::npos &&
        getenv("PROJ_USE_PROJ4_INIT_RULES") == nullptr)
    {
        CPLError(CE_Warning, CPLE_AppDefined,
                     "+init=epsg:XXXX syntax is deprecated. It might return "
                     "a CRS with a non-EPSG compliant axis order.");
    }
    proj_context_use_proj4_init_rules(d->getPROJContext(), true);
    d->setPjCRS(proj_create(d->getPROJContext(), osProj4.c_str()));
    proj_context_use_proj4_init_rules(d->getPROJContext(), false);
    return d->m_pj_crs ? OGRERR_NONE : OGRERR_CORRUPT_DATA;
}

/************************************************************************/
/*                           morphFromESRI()                            */
/************************************************************************/

/**
 * \brief Convert in place from ESRI WKT format.
 *
 * The value notes of this coordinate system are modified in various manners
 * to adhere more closely to the WKT standard.  This mostly involves
 * translating a variety of ESRI names for projections, arguments and
 * datums to "standard" names, as defined by Adam Gawne-Cain's reference
 * translation of EPSG to WKT for the CT specification.
 *
 * \note Since GDAL 3.0, this function is essentially a no-operation, since
 * morphing from ESRI is automatically done by importFromWkt(). Its only
 * effect is to undo the effect of a potential prior call to morphToESRI().
 *
 * This does the same as the C function OSRMorphFromESRI().
 *
 * @return OGRERR_NONE unless something goes badly wrong.
 * @deprecated
 */

OGRErr OGRSpatialReference::morphFromESRI()

{
    TAKE_OPTIONAL_LOCK();

    d->refreshProjObj();
    d->setMorphToESRI(false);

    return OGRERR_NONE;
}

/************************************************************************/
/*                            FindMatches()                             */
/************************************************************************/

/**
 * \brief Try to identify a match between the passed SRS and a related SRS
 * in a catalog.
 *
 * Matching may be partial, or may fail.
 * Returned entries will be sorted by decreasing match confidence (first
 * entry has the highest match confidence).
 *
 * The exact way matching is done may change in future versions. Starting with
 * GDAL 3.0, it relies on PROJ' proj_identify() function.
 *
 * This method is the same as OSRFindMatches().
 *
 * @param papszOptions NULL terminated list of options or NULL
 * @param pnEntries Output parameter. Number of values in the returned array.
 * @param ppanMatchConfidence Output parameter (or NULL). *ppanMatchConfidence
 * will be allocated to an array of *pnEntries whose values between 0 and 100
 * indicate the confidence in the match. 100 is the highest confidence level.
 * The array must be freed with CPLFree().
 *
 * @return an array of SRS that match the passed SRS, or NULL. Must be freed
 * with OSRFreeSRSArray()
 *
 * @since GDAL 2.3
 *
 * @see OGRSpatialReference::FindBestMatch()
 */
OGRSpatialReferenceH *
OGRSpatialReference::FindMatches(char **papszOptions, int *pnEntries,
                                 int **ppanMatchConfidence) const
{
    TAKE_OPTIONAL_LOCK();

    CPL_IGNORE_RET_VAL(papszOptions);

    if (pnEntries)
        *pnEntries = 0;
    if (ppanMatchConfidence)
        *ppanMatchConfidence = nullptr;

    d->refreshProjObj();
    if (!d->m_pj_crs)
        return nullptr;

    int *panConfidence = nullptr;
    auto ctxt = d->getPROJContext();
    auto list =
        proj_identify(ctxt, d->m_pj_crs, nullptr, nullptr, &panConfidence);
    if (!list)
        return nullptr;

    const int nMatches = proj_list_get_count(list);

    if (pnEntries)
        *pnEntries = static_cast<int>(nMatches);
    OGRSpatialReferenceH *pahRet = static_cast<OGRSpatialReferenceH *>(
        CPLCalloc(sizeof(OGRSpatialReferenceH), nMatches + 1));
    if (ppanMatchConfidence)
    {
        *ppanMatchConfidence =
            static_cast<int *>(CPLMalloc(sizeof(int) * (nMatches + 1)));
    }

    bool bSortAgain = false;

    for (int i = 0; i < nMatches; i++)
    {
        PJ *obj = proj_list_get(ctxt, list, i);
        CPLAssert(obj);
        OGRSpatialReference *poSRS = new OGRSpatialReference(d->getPROJContext());
        poSRS->d->setPjCRS(obj);
        pahRet[i] = ToHandle(poSRS);

        // Identify matches that only differ by axis order
        if (panConfidence[i] == 50 && GetAxesCount() == 2 &&
            poSRS->GetAxesCount() == 2 &&
            GetDataAxisToSRSAxisMapping() == std::vector<int>{1, 2})
        {
            OGRAxisOrientation eThisAxis0 = OAO_Other;
            OGRAxisOrientation eThisAxis1 = OAO_Other;
            OGRAxisOrientation eSRSAxis0 = OAO_Other;
            OGRAxisOrientation eSRSAxis1 = OAO_Other;
            GetAxis(nullptr, 0, &eThisAxis0);
            GetAxis(nullptr, 1, &eThisAxis1);
            poSRS->GetAxis(nullptr, 0, &eSRSAxis0);
            poSRS->GetAxis(nullptr, 1, &eSRSAxis1);
            if (eThisAxis0 == OAO_East && eThisAxis1 == OAO_North &&
                eSRSAxis0 == OAO_North && eSRSAxis1 == OAO_East)
            {
                auto pj_crs_normalized =
                    proj_normalize_for_visualization(ctxt, poSRS->d->m_pj_crs);
                if (pj_crs_normalized)
                {
                    if (proj_is_equivalent_to(d->m_pj_crs, pj_crs_normalized,
                                              PJ_COMP_EQUIVALENT))
                    {
                        bSortAgain = true;
                        panConfidence[i] = 90;
                        poSRS->SetDataAxisToSRSAxisMapping({2, 1});
                    }
                    proj_destroy(pj_crs_normalized);
                }
            }
        }

        if (ppanMatchConfidence)
            (*ppanMatchConfidence)[i] = panConfidence[i];
    }

    if (bSortAgain)
    {
        std::vector<int> anIndices;
        for (int i = 0; i < nMatches; ++i)
            anIndices.push_back(i);

        std::stable_sort(anIndices.begin(), anIndices.end(),
                         [&panConfidence](int i, int j)
                         { return panConfidence[i] > panConfidence[j]; });

        OGRSpatialReferenceH *pahRetSorted =
            static_cast<OGRSpatialReferenceH *>(
                CPLCalloc(sizeof(OGRSpatialReferenceH), nMatches + 1));
        for (int i = 0; i < nMatches; ++i)
        {
            pahRetSorted[i] = pahRet[anIndices[i]];
            if (ppanMatchConfidence)
                (*ppanMatchConfidence)[i] = panConfidence[anIndices[i]];
        }
        CPLFree(pahRet);
        pahRet = pahRetSorted;
    }

    pahRet[nMatches] = nullptr;
    proj_list_destroy(list);
    proj_int_list_destroy(panConfidence);

    return pahRet;
}

/************************************************************************/
/*                          importFromEPSGA()                           */
/************************************************************************/

/**
 * \brief  Initialize SRS based on EPSG geographic, projected or vertical CRS
 * code.
 *
 * This method will initialize the spatial reference based on the
 * passed in EPSG CRS code found in the PROJ database.
 *
 * Since GDAL 3.0, this method is identical to importFromEPSG().
 *
 * Before GDAL 3.0.3, this method would try to attach a 3-parameter or
 * 7-parameter Helmert transformation to WGS84 when there is one and only one
 * such method available for the CRS. This behavior might not always be
 * desirable, so starting with GDAL 3.0.3, this is no longer done unless
 * the OSR_ADD_TOWGS84_ON_IMPORT_FROM_EPSG configuration option is set to YES.
 * The AddGuessedTOWGS84() method can also be used for that purpose.
 *
 * The method will also by default substitute a deprecated EPSG code by its
 * non-deprecated replacement. If this behavior is not desired, the
 * OSR_USE_NON_DEPRECATED configuration option can be set to NO.
 *
 * This method is the same as the C function OSRImportFromEPSGA().
 *
 * @param nCode a CRS code.
 *
 * @return OGRERR_NONE on success, or an error code on failure.
 */

OGRErr OGRSpatialReference::importFromEPSGA(int nCode)

{
    TAKE_OPTIONAL_LOCK();

    Clear();

    const char *pszUseNonDeprecated =
        CPLGetConfigOption("OSR_USE_NON_DEPRECATED", nullptr);
    const bool bUseNonDeprecated =
        CPLTestBool(pszUseNonDeprecated ? pszUseNonDeprecated : "YES");
    const bool bAddTOWGS84 = CPLTestBool(
        CPLGetConfigOption("OSR_ADD_TOWGS84_ON_IMPORT_FROM_EPSG", "NO"));
#ifdef MECHSOFT_DEVIATION
    auto tlsCache = OSRGetProjTLSCache();
    if (tlsCache)
    {
        auto cachedObj =
            tlsCache->GetPJForEPSGCode(nCode, bUseNonDeprecated, bAddTOWGS84);
        if (cachedObj)
        {
            d->setPjCRS(cachedObj);
            return OGRERR_NONE;
        }
    }
#endif

    CPLString osCode;
    osCode.Printf("%d", nCode);
    PJ *obj;
    constexpr int FIRST_NON_DEPRECATED_ESRI_CODE = 53001;
    if (nCode < FIRST_NON_DEPRECATED_ESRI_CODE)
    {
        obj = proj_create_from_database(d->getPROJContext(), "EPSG",
                                        osCode.c_str(), PJ_CATEGORY_CRS, true,
                                        nullptr);
        if (!obj)
        {
            return OGRERR_FAILURE;
        }
    }
    else
    {
        // Likely to be an ESRI CRS...
        CPLErr eLastErrorType = CE_None;
        CPLErrorNum eLastErrorNum = CPLE_None;
        std::string osLastErrorMsg;
        bool bIsESRI = false;
        {
#ifdef MECHSOFT_DEVIATION
            CPLErrorStateBackuper oBackuper(CPLQuietErrorHandler);
            CPLErrorReset();
#endif
            obj = proj_create_from_database(d->getPROJContext(), "EPSG",
                                            osCode.c_str(), PJ_CATEGORY_CRS,
                                            true, nullptr);
            if (!obj)
            {
                eLastErrorType = CPLGetLastErrorType();
                eLastErrorNum = CPLGetLastErrorNo();
                osLastErrorMsg = CPLGetLastErrorMsg();
                obj = proj_create_from_database(d->getPROJContext(), "ESRI",
                                                osCode.c_str(), PJ_CATEGORY_CRS,
                                                true, nullptr);
                if (obj)
                    bIsESRI = true;
            }
        }
        if (!obj)
        {
            if (eLastErrorType != CE_None)
                CPLError(eLastErrorType, eLastErrorNum, "%s",
                         osLastErrorMsg.c_str());
            return OGRERR_FAILURE;
        }
        if (bIsESRI)
        {
            CPLError(CE_Warning, CPLE_AppDefined,
                     "EPSG:%d is not a valid CRS code, but ESRI:%d is. "
                     "Assuming ESRI:%d was meant",
                     nCode, nCode, nCode);
        }
    }

    if (bUseNonDeprecated && proj_is_deprecated(obj))
    {
        auto list = proj_get_non_deprecated(d->getPROJContext(), obj);
        if (list)
        {
            const auto count = proj_list_get_count(list);
            if (count == 1)
            {
                auto nonDeprecated =
                    proj_list_get(d->getPROJContext(), list, 0);
                if (nonDeprecated)
                {
                    if (pszUseNonDeprecated == nullptr)
                    {
                        const char *pszNewAuth =
                            proj_get_id_auth_name(nonDeprecated, 0);
                        const char *pszNewCode =
                            proj_get_id_code(nonDeprecated, 0);
                        CPLError(CE_Warning, CPLE_AppDefined,
                                 "CRS EPSG:%d is deprecated. "
                                 "Its non-deprecated replacement %s:%s "
                                 "will be used instead. "
                                 "To use the original CRS, set the "
                                 "OSR_USE_NON_DEPRECATED "
                                 "configuration option to NO.",
                                 nCode, pszNewAuth ? pszNewAuth : "(null)",
                                 pszNewCode ? pszNewCode : "(null)");
                    }
                    proj_destroy(obj);
                    obj = nonDeprecated;
                }
            }
        }
        proj_list_destroy(list);
    }

    if (bAddTOWGS84)
    {
        auto boundCRS = proj_crs_create_bound_crs_to_WGS84(d->getPROJContext(),
                                                           obj, nullptr);
        if (boundCRS)
        {
            proj_destroy(obj);
            obj = boundCRS;
        }
    }

    d->setPjCRS(obj);

#ifdef MECHSOFT_DEVIATION
    if (tlsCache)
    {
        tlsCache->CachePJForEPSGCode(nCode, bUseNonDeprecated, bAddTOWGS84,
                                     obj);
    }
#endif

    return OGRERR_NONE;
}

/************************************************************************/
/*                           importFromEPSG()                           */
/************************************************************************/

/**
 * \brief  Initialize SRS based on EPSG geographic, projected or vertical CRS
 * code.
 *
 * This method will initialize the spatial reference based on the
 * passed in EPSG CRS code found in the PROJ database.
 *
 * This method is the same as the C function OSRImportFromEPSG().
 *
 * Before GDAL 3.0.3, this method would try to attach a 3-parameter or
 * 7-parameter Helmert transformation to WGS84 when there is one and only one
 * such method available for the CRS. This behavior might not always be
 * desirable, so starting with GDAL 3.0.3, this is no longer done unless
 * the OSR_ADD_TOWGS84_ON_IMPORT_FROM_EPSG configuration option is set to YES.
 *
 * @param nCode a GCS or PCS code from the horizontal coordinate system table.
 *
 * @return OGRERR_NONE on success, or an error code on failure.
 */

OGRErr OGRSpatialReference::importFromEPSG(int nCode)

{
    return importFromEPSGA(nCode);
}

/************************************************************************/
/*                      SetAxisMappingStrategy()                        */
/************************************************************************/

/** \brief Set the data axis to CRS axis mapping strategy.
 *
 * Starting with GDAL 3.5, the OSR_DEFAULT_AXIS_MAPPING_STRATEGY configuration
 * option can be set to "TRADITIONAL_GIS_ORDER" / "AUTHORITY_COMPLIANT" (the
 * later being the default value when the option is not set) to control the
 * value of the data axis to CRS axis mapping strategy when a
 * OSRSpatialReference object is created. Calling SetAxisMappingStrategy() will
 * override this default value.
 *
 * See OGRSpatialReference::GetAxisMappingStrategy()
 * @since GDAL 3.0
 */
void OGRSpatialReference::SetAxisMappingStrategy(
    OSRAxisMappingStrategy strategy)
{
    TAKE_OPTIONAL_LOCK();

    d->m_axisMappingStrategy = strategy;
    d->refreshAxisMapping();
}

/************************************************************************/
/*                      GetDataAxisToSRSAxisMapping()                   */
/************************************************************************/

/** \brief Return the data axis to SRS axis mapping.
 *
 * The number of elements of the vector will be the number of axis of the CRS.
 * Values start at 1.
 *
 * If m = GetDataAxisToSRSAxisMapping(), then m[0] is the data axis number
 * for the first axis of the CRS.
 *
 * @since GDAL 3.0
 */
const std::vector<int> &OGRSpatialReference::GetDataAxisToSRSAxisMapping() const
{
    TAKE_OPTIONAL_LOCK();

    return d->m_axisMapping;
}

/************************************************************************/
/*                      SetDataAxisToSRSAxisMapping()                   */
/************************************************************************/

/** \brief Set a custom data axis to CRS axis mapping.
 *
 * The number of elements of the mapping vector should be the number of axis
 * of the CRS (as returned by GetAxesCount()) (although this method does not
 * check that, beyond checking there are at least 2 elements, so that this
 * method and setting the CRS can be done in any order).
 * This is taken into account by OGRCoordinateTransformation to transform the
 * order of coordinates to the order expected by the CRS before
 * transformation, and back to the data order after transformation.
 *
 * The mapping[i] value (one based) represents the data axis number for the i(th)
 * axis of the CRS. A negative value can also be used to ask for a sign
 * reversal during coordinate transformation (to deal with northing vs southing,
 * easting vs westing, heights vs depths).
 *
 * When used with OGRCoordinateTransformation,
 * - the only valid values for mapping[0] (data axis number for the first axis
 *   of the CRS) are 1, 2, -1, -2.
 * - the only valid values for mapping[1] (data axis number for the second axis
 *   of the CRS) are 1, 2, -1, -2.
 *  - the only valid values mapping[2] are 3 or -3.
 * Note: this method does not validate the values of mapping[].
 *
 * mapping=[2,1] typically expresses the inversion of axis between the data
 * axis and the CRS axis for a 2D CRS.
 *
 * Automatically implies SetAxisMappingStrategy(OAMS_CUSTOM)
 *
 * This is the same as the C function OSRSetDataAxisToSRSAxisMapping().
 *
 * @param mapping The new data axis to CRS axis mapping.
 *
 * @since GDAL 3.0
 * @see OGRSpatialReference::GetDataAxisToSRSAxisMapping()
 */
OGRErr OGRSpatialReference::SetDataAxisToSRSAxisMapping(
    const std::vector<int> &mapping)
{
    TAKE_OPTIONAL_LOCK();

    if (mapping.size() < 2)
        return OGRERR_FAILURE;
    d->m_axisMappingStrategy = OAMS_CUSTOM;
    d->m_axisMapping = mapping;
    return OGRERR_NONE;
}

/************************************************************************/
/*                    UpdateCoordinateSystemFromGeogCRS()               */
/************************************************************************/

/*! @cond Doxygen_Suppress */
/** \brief Used by gt_wkt_srs.cpp to create projected 3D CRS. Internal use only
 *
 * @since GDAL 3.1
 */
void OGRSpatialReference::UpdateCoordinateSystemFromGeogCRS()
{
    TAKE_OPTIONAL_LOCK();

    d->refreshProjObj();
    if (!d->m_pj_crs)
        return;
    if (d->m_pjType != PJ_TYPE_PROJECTED_CRS)
        return;
    if (GetAxesCount() == 3)
        return;
    auto ctxt = d->getPROJContext();
    auto baseCRS = proj_crs_get_geodetic_crs(ctxt, d->m_pj_crs);
    if (!baseCRS)
        return;
    auto baseCRSCS = proj_crs_get_coordinate_system(ctxt, baseCRS);
    if (!baseCRSCS)
    {
        proj_destroy(baseCRS);
        return;
    }
    if (proj_cs_get_axis_count(ctxt, baseCRSCS) != 3)
    {
        proj_destroy(baseCRSCS);
        proj_destroy(baseCRS);
        return;
    }
    auto projCS = proj_crs_get_coordinate_system(ctxt, d->m_pj_crs);
    if (!projCS || proj_cs_get_axis_count(ctxt, projCS) != 2)
    {
        proj_destroy(baseCRSCS);
        proj_destroy(baseCRS);
        proj_destroy(projCS);
        return;
    }

    PJ_AXIS_DESCRIPTION axis[3];
    for (int i = 0; i < 3; i++)
    {
        const char *name = nullptr;
        const char *abbreviation = nullptr;
        const char *direction = nullptr;
        double unit_conv_factor = 0;
        const char *unit_name = nullptr;
        proj_cs_get_axis_info(ctxt, i < 2 ? projCS : baseCRSCS, i, &name,
                              &abbreviation, &direction, &unit_conv_factor,
                              &unit_name, nullptr, nullptr);
        axis[i].name = CPLStrdup(name);
        axis[i].abbreviation = CPLStrdup(abbreviation);
        axis[i].direction = CPLStrdup(direction);
        axis[i].unit_name = CPLStrdup(unit_name);
        axis[i].unit_conv_factor = unit_conv_factor;
        axis[i].unit_type = PJ_UT_LINEAR;
    }
    proj_destroy(baseCRSCS);
    proj_destroy(projCS);
    auto cs = proj_create_cs(ctxt, PJ_CS_TYPE_CARTESIAN, 3, axis);
    for (int i = 0; i < 3; i++)
    {
        CPLFree(axis[i].name);
        CPLFree(axis[i].abbreviation);
        CPLFree(axis[i].direction);
        CPLFree(axis[i].unit_name);
    }
    if (!cs)
    {
        proj_destroy(baseCRS);
        return;
    }
    auto conversion = proj_crs_get_coordoperation(ctxt, d->m_pj_crs);
    auto crs = proj_create_projected_crs(ctxt, d->getProjCRSName(), baseCRS,
                                         conversion, cs);
    proj_destroy(baseCRS);
    proj_destroy(conversion);
    proj_destroy(cs);
    d->setPjCRS(crs);
}

/*! @endcond */

/************************************************************************/
/*                             DemoteTo2D()                             */
/************************************************************************/

/** \brief "Demote" a 3D CRS to a 2D CRS one.
 *
 * @param pszName New name for the CRS. If set to NULL, the previous name will
 * be used.
 * @return OGRERR_NONE if no error occurred.
 * @since GDAL 3.2 and PROJ 6.3
 */
OGRErr OGRSpatialReference::DemoteTo2D(const char *pszName)
{
    TAKE_OPTIONAL_LOCK();

    d->refreshProjObj();
    if (!d->m_pj_crs)
        return OGRERR_FAILURE;
    auto newPj =
        proj_crs_demote_to_2D(d->getPROJContext(), pszName, d->m_pj_crs);
    if (!newPj)
        return OGRERR_FAILURE;
    d->setPjCRS(newPj);
    return OGRERR_NONE;
}

/************************************************************************/
/*                          SetCoordinateEpoch()                        */
/************************************************************************/

/** Set the coordinate epoch, as decimal year.
 *
 * In a dynamic CRS, coordinates of a point on the surface of the Earth may
 * change with time. To be unambiguous the coordinates must always be qualified
 * with the epoch at which they are valid. The coordinate epoch is not
 * necessarily the epoch at which the observation was collected.
 *
 * Pedantically the coordinate epoch of an observation belongs to the
 * observation, and not to the CRS, however it is often more practical to
 * bind it to the CRS. The coordinate epoch should be specified for dynamic
 * CRS (see IsDynamic())
 *
 * This method is the same as the OSRSetCoordinateEpoch() function.
 *
 * @param dfCoordinateEpoch Coordinate epoch as decimal year (e.g. 2021.3)
 * @since OGR 3.4
 */

void OGRSpatialReference::SetCoordinateEpoch(double dfCoordinateEpoch)
{
    d->m_coordinateEpoch = dfCoordinateEpoch;
}
