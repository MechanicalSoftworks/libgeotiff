/******************************************************************************
 * $Id$
 *
 * Project:  libgeotiff
 * Purpose:  Code to convert a normalized GeoTIFF definition into a WKT
 *           compatible projection string.
 *           Most of this code was derived from GDAL. Also copies the GDAL
 *           license.
 * Author:   Lucas Zadrozny
 *
 ******************************************************************************
 * SPDX-License-Identifier: MIT
 ******************************************************************************
 */

#include "cpl_serv.h"
#include "geotiff.h"
#include "geo_normalize.h"
#include "geovalues.h"
#include "geo_tiffp.h"
#include "ogr_spatialref.h"
#include "ogr_srs_api.h"
#include "gt_citation.h"
#include "proj.h"
#include "proj_experimental.h"
#include "cpl_string.h"
#include "cpl_conv.h"

#include <string>
#include <cmath>

static const geokey_t ProjLinearUnitsInterpCorrectGeoKey =
    static_cast<geokey_t>(3059);

void GTIFGetOGISDefnAsOSR(PJ_CONTEXT* ctx, GTIF* hGTIF, GTIFDefn* psDefn, OGRSpatialReference& oSRS);

/************************************************************************/
/*                             GTIFGetWKT()                             */
/************************************************************************/
extern "C"
char * GTIFGetWKT( GTIF * gtif, GTIFDefn * psDefn )

{
	PJ_CONTEXT* ctx = proj_context_create();
	char * ret = GTIFGetWKTEx(ctx, gtif, psDefn);
	proj_context_destroy(ctx);
	return ret;
}


/************************************************************************/
/*                            GTIFGetWKTEx()                            */
/************************************************************************/
extern "C"
char* GTIFGetWKTEx(void* ctxIn, GTIF * hGTIF, GTIFDefn* psDefn)
{
    OGRSpatialReference oSRS{ (PJ_CONTEXT*)ctxIn };

    GTIFGetOGISDefnAsOSR((PJ_CONTEXT*)ctxIn, hGTIF, psDefn, oSRS);

    const char* const apszWKTOptions[] = { "FORMAT=WKT2", "MULTILINE=YES",
                                          nullptr };

    const auto wkt = oSRS.exportToWkt(apszWKTOptions);

    return CPLStrdup(wkt.c_str());
}

/************************************************************************/
/*                       GTIFToCPLRecyleString()                        */
/*                                                                      */
/*      This changes a string from the libgeotiff heap to the GDAL      */
/*      heap.                                                           */
/************************************************************************/

static void GTIFToCPLRecycleString(char **ppszTarget)

{
    if (*ppszTarget == nullptr)
        return;

    char *pszTempString = CPLStrdup(*ppszTarget);
    GTIFFreeMemory(*ppszTarget);
    *ppszTarget = pszTempString;
}

/************************************************************************/
/*                      GTIFCleanupImageineNames()                      */
/*                                                                      */
/*      Erdas Imagine sometimes emits big copyright messages, and       */
/*      other stuff into citations.  These can be pretty messy when     */
/*      turned into WKT, so we try to trim and clean the strings        */
/*      somewhat.                                                       */
/************************************************************************/

/* For example:
   GTCitationGeoKey (Ascii,215): "IMAGINE GeoTIFF Support\nCopyright 1991 - 2001
   by ERDAS, Inc. All Rights Reserved\n@(#)$RCSfile$ $Revision: 34309 $ $Date:
   2016-05-29 11:29:40 -0700 (Sun, 29 May 2016) $\nProjection Name = UTM\nUnits
   = meters\nGeoTIFF Units = meters"

   GeogCitationGeoKey (Ascii,267): "IMAGINE GeoTIFF Support\nCopyright 1991 -
   2001 by ERDAS, Inc. All Rights Reserved\n@(#)$RCSfile$ $Revision: 34309 $
   $Date: 2016-05-29 11:29:40 -0700 (Sun, 29 May 2016) $\nUnable to match
   Ellipsoid (Datum) to a GeographicTypeGeoKey value\nEllipsoid = Clarke
   1866\nDatum = NAD27 (CONUS)"

   PCSCitationGeoKey (Ascii,214): "IMAGINE GeoTIFF Support\nCopyright 1991 -
   2001 by ERDAS, Inc. All Rights Reserved\n@(#)$RCSfile$ $Revision: 34309 $
   $Date: 2016-05-29 11:29:40 -0700 (Sun, 29 May 2016) $\nUTM Zone
   10N\nEllipsoid = Clarke 1866\nDatum = NAD27 (CONUS)"
*/

static void GTIFCleanupImagineNames(char *pszCitation)

{
    if (strstr(pszCitation, "IMAGINE GeoTIFF") == nullptr)
        return;

    /* -------------------------------------------------------------------- */
    /*      First, we skip past all the copyright, and RCS stuff.  We       */
    /*      assume that this will have a "$" at the end of it all.          */
    /* -------------------------------------------------------------------- */
    char *pszSkip = pszCitation + strlen(pszCitation) - 1;

    for (; pszSkip != pszCitation && *pszSkip != '$'; pszSkip--)
    {
    }

    if (*pszSkip == '$')
        pszSkip++;
    if (*pszSkip == '\n')
        pszSkip++;

    memmove(pszCitation, pszSkip, strlen(pszSkip) + 1);

    /* -------------------------------------------------------------------- */
    /*      Convert any newlines into spaces, they really gum up the        */
    /*      WKT.                                                            */
    /* -------------------------------------------------------------------- */
    for (int i = 0; pszCitation[i] != '\0'; i++)
    {
        if (pszCitation[i] == '\n')
            pszCitation[i] = ' ';
    }
}

/************************************************************************/
/*                    FillCompoundCRSWithManualVertCS()                 */
/************************************************************************/

static void FillCompoundCRSWithManualVertCS(GTIF *hGTIF,
                                            OGRSpatialReference &oSRS,
                                            const char *pszVertCSName,
                                            int verticalDatum,
                                            int verticalUnits)
{
    /* -------------------------------------------------------------------- */
    /*      Setup VERT_CS with citation if present.                         */
    /* -------------------------------------------------------------------- */
    oSRS.SetNode("COMPD_CS|VERT_CS", pszVertCSName);

    /* -------------------------------------------------------------------- */
    /*      Setup the vertical datum.                                       */
    /* -------------------------------------------------------------------- */
    std::string osVDatumName = "unknown";
    const char *pszVDatumType = "2005";  // CS_VD_GeoidModelDerived
    std::string osVDatumAuthName;
    int nVDatumCode = 0;

    if (verticalDatum > 0 && verticalDatum != KvUserDefined)
    {
        osVDatumAuthName = "EPSG";
        nVDatumCode = verticalDatum;

        char szCode[12];
        snprintf(szCode, sizeof(szCode), "%d", verticalDatum);
        auto ctx =
            static_cast<PJ_CONTEXT *>(GTIFGetPROJContext(hGTIF, true, nullptr));
        auto datum = proj_create_from_database(ctx, "EPSG", szCode,
                                               PJ_CATEGORY_DATUM, 0, nullptr);
        if (datum)
        {
            const char *pszName = proj_get_name(datum);
            if (pszName)
            {
                osVDatumName = pszName;
            }
            proj_destroy(datum);
        }
    }
    else if (verticalDatum == KvUserDefined)
    {
        // If the vertical datum is unknown, try to find the vertical CRS
        // from the database, and extra the datum information from it.
        auto ctx =
            static_cast<PJ_CONTEXT *>(GTIFGetPROJContext(hGTIF, true, nullptr));
        const auto type = PJ_TYPE_VERTICAL_CRS;
        auto list = proj_create_from_name(ctx, nullptr, pszVertCSName, &type, 1,
                                          /* approximateMatch = */ false,
                                          -1,  // result set limit size,
                                          nullptr);
        if (list)
        {
            // If we have several matches, check they all refer to the
            // same datum
            bool bGoOn = true;
            int ncount = proj_list_get_count(list);
            for (int i = 0; bGoOn && i < ncount; ++i)
            {
                auto crs = proj_list_get(ctx, list, i);
                if (crs)
                {
                    auto datum = proj_crs_get_datum(ctx, crs);
                    if (datum)
                    {
                        const char *pszAuthName =
                            proj_get_id_auth_name(datum, 0);
                        const char *pszCode = proj_get_id_code(datum, 0);
                        if (pszCode && atoi(pszCode) && pszAuthName)
                        {
                            if (osVDatumAuthName.empty())
                            {
                                osVDatumName = proj_get_name(datum);
                                osVDatumAuthName = pszAuthName;
                                nVDatumCode = atoi(pszCode);
                            }
                            else if (osVDatumAuthName != pszAuthName ||
                                     nVDatumCode != atoi(pszCode))
                            {
                                osVDatumName = "unknown";
                                osVDatumAuthName.clear();
                                nVDatumCode = 0;
                                bGoOn = false;
                            }
                        }
                        proj_destroy(datum);
                    }
                    proj_destroy(crs);
                }
            }
        }
        proj_list_destroy(list);
    }

    oSRS.SetNode("COMPD_CS|VERT_CS|VERT_DATUM", osVDatumName.c_str());
    oSRS.GetAttrNode("COMPD_CS|VERT_CS|VERT_DATUM")
        ->AddChild(new OGR_SRSNode(pszVDatumType));
    if (!osVDatumAuthName.empty())
        oSRS.SetAuthority("COMPD_CS|VERT_CS|VERT_DATUM",
                          osVDatumAuthName.c_str(), nVDatumCode);

    /* -------------------------------------------------------------------- */
    /*      Set the vertical units.                                         */
    /* -------------------------------------------------------------------- */
    if (verticalUnits > 0 && verticalUnits != KvUserDefined &&
        verticalUnits != 9001)
    {
        char szCode[12];
        snprintf(szCode, sizeof(szCode), "%d", verticalUnits);
        auto ctx =
            static_cast<PJ_CONTEXT *>(GTIFGetPROJContext(hGTIF, true, nullptr));
        const char *pszName = nullptr;
        double dfInMeters = 0.0;
        if (proj_uom_get_info_from_database(ctx, "EPSG", szCode, &pszName,
                                            &dfInMeters, nullptr))
        {
            if (pszName)
                oSRS.SetNode("COMPD_CS|VERT_CS|UNIT", pszName);

            char szInMeters[128] = {};
            CPLsnprintf(szInMeters, sizeof(szInMeters), "%.16g", dfInMeters);
            oSRS.GetAttrNode("COMPD_CS|VERT_CS|UNIT")
                ->AddChild(new OGR_SRSNode(szInMeters));
        }

        oSRS.SetAuthority("COMPD_CS|VERT_CS|UNIT", "EPSG", verticalUnits);
    }
    else
    {
        oSRS.SetNode("COMPD_CS|VERT_CS|UNIT", "metre");
        oSRS.GetAttrNode("COMPD_CS|VERT_CS|UNIT")
            ->AddChild(new OGR_SRSNode("1.0"));
        oSRS.SetAuthority("COMPD_CS|VERT_CS|UNIT", "EPSG", 9001);
    }

    /* -------------------------------------------------------------------- */
    /*      Set the axis and VERT_CS authority.                             */
    /* -------------------------------------------------------------------- */
    oSRS.SetNode("COMPD_CS|VERT_CS|AXIS", "Up");
    oSRS.GetAttrNode("COMPD_CS|VERT_CS|AXIS")->AddChild(new OGR_SRSNode("UP"));
}

/************************************************************************/
/*                    GTIFGetEPSGOfficialName()                         */
/************************************************************************/

static char *GTIFGetEPSGOfficialName(GTIF *hGTIF, PJ_TYPE searchType,
                                     const char *pszName)
{
    char *pszRet = nullptr;
    /* Search in database the corresponding EPSG 'official' name */
    auto ctx =
        static_cast<PJ_CONTEXT *>(GTIFGetPROJContext(hGTIF, true, nullptr));
    auto list =
        proj_create_from_name(ctx, "EPSG", pszName, &searchType, 1,
                              /* approximateMatch = */ false, 1, nullptr);
    if (list)
    {
        const auto listSize = proj_list_get_count(list);
        if (listSize == 1)
        {
            auto obj = proj_list_get(ctx, list, 0);
            if (obj)
            {
                const char *pszOfficialName = proj_get_name(obj);
                if (pszOfficialName)
                {
                    pszRet = CPLStrdup(pszOfficialName);
                }
            }
            proj_destroy(obj);
        }
        proj_list_destroy(list);
    }
    return pszRet;
}

/************************************************************************/
/*                      GTIFGetOGISDefnAsOSR()                          */
/************************************************************************/

void GTIFGetOGISDefnAsOSR(PJ_CONTEXT* projContext, GTIF* hGTIF, GTIFDefn* psDefn, OGRSpatialReference& oSRS)
{
#if LIBGEOTIFF_VERSION >= 1600
    //void *projContext = GTIFGetPROJContext(hGTIF, FALSE, nullptr);
#endif

    /* -------------------------------------------------------------------- */
    /*  Handle non-standard coordinate systems where GTModelTypeGeoKey      */
    /*  is not defined, but ProjectedCSTypeGeoKey is defined (ticket #3019) */
    /* -------------------------------------------------------------------- */
    if (psDefn->Model == KvUserDefined && psDefn->PCS != KvUserDefined)
    {
        psDefn->Model = ModelTypeProjected;
    }

    /* ==================================================================== */
    /*      Read keys related to vertical component.                        */
    /* ==================================================================== */
    unsigned short verticalCSType = 0;
    unsigned short verticalDatum = 0;
    unsigned short verticalUnits = 0;

    GDALGTIFKeyGetSHORT(hGTIF, VerticalCSTypeGeoKey, &verticalCSType, 0, 1);
    GDALGTIFKeyGetSHORT(hGTIF, VerticalDatumGeoKey, &verticalDatum, 0, 1);
    GDALGTIFKeyGetSHORT(hGTIF, VerticalUnitsGeoKey, &verticalUnits, 0, 1);

    if (verticalCSType != 0 || verticalDatum != 0 || verticalUnits != 0)
    {
        int versions[3];
        GTIFDirectoryInfo(hGTIF, versions, nullptr);
        // GeoTIFF 1.0
        if (versions[0] == 1 && versions[1] == 1 && versions[2] == 0)
        {
            /* --------------------------------------------------------------------
             */
            /*      The original geotiff specification appears to have */
            /*      misconstrued the EPSG codes 5101 to 5106 to be vertical */
            /*      coordinate system codes, when in fact they are vertical */
            /*      datum codes.  So if these are found in the */
            /*      VerticalCSTypeGeoKey move them to the VerticalDatumGeoKey */
            /*      and insert the "normal" corresponding VerticalCSTypeGeoKey
             */
            /*      value. */
            /* --------------------------------------------------------------------
             */
            if ((verticalCSType >= 5101 && verticalCSType <= 5112) &&
                verticalDatum == 0)
            {
                verticalDatum = verticalCSType;
                verticalCSType = verticalDatum + 600;
            }

            /* --------------------------------------------------------------------
             */
            /*      This addresses another case where the EGM96 Vertical Datum
             * code */
            /*      is misused as a Vertical CS code (#4922). */
            /* --------------------------------------------------------------------
             */
            if (verticalCSType == 5171)
            {
                verticalDatum = 5171;
                verticalCSType = 5773;
            }
        }

        /* --------------------------------------------------------------------
         */
        /*      Somewhat similarly, codes 5001 to 5033 were treated as */
        /*      vertical coordinate systems based on ellipsoidal heights. */
        /*      We use the corresponding geodetic datum as the vertical */
        /*      datum and clear the vertical coordinate system code since */
        /*      there isn't one in EPSG. */
        /* --------------------------------------------------------------------
         */
        if ((verticalCSType >= 5001 && verticalCSType <= 5033) &&
            verticalDatum == 0)
        {
            verticalDatum = verticalCSType + 1000;
            verticalCSType = 0;
        }
    }

    /* -------------------------------------------------------------------- */
    /*      Handle non-standard coordinate systems as LOCAL_CS.             */
    /* -------------------------------------------------------------------- */
    if (psDefn->Model != ModelTypeProjected &&
        psDefn->Model != ModelTypeGeographic &&
        psDefn->Model != ModelTypeGeocentric)
    {
        char szPeStr[2400] = {'\0'};

        /** check if there is a pe string citation key **/
        if (GDALGTIFKeyGetASCII(hGTIF, PCSCitationGeoKey, szPeStr,
                                sizeof(szPeStr)) &&
            strstr(szPeStr, "ESRI PE String = "))
        {
            const char *pszWKT = szPeStr + strlen("ESRI PE String = ");
            oSRS.importFromWkt(pszWKT);

            if (strstr(pszWKT,
                       "PROJCS[\"WGS_1984_Web_Mercator_Auxiliary_Sphere\""))
            {
                oSRS.SetExtension(
                    "PROJCS", "PROJ4",
                    "+proj=merc +a=6378137 +b=6378137 +lat_ts=0.0 +lon_0=0.0 "
                    "+x_0=0.0 +y_0=0 +k=1.0 +units=m +nadgrids=@null "
                    "+wktext  +no_defs");  // TODO(schwehr): Why 2 spaces?
            }

            return;
        }
        else
        {
            char *pszUnitsName = nullptr;
            char szPCSName[300] = {'\0'};
            int nKeyCount = 0;
            int anVersion[3] = {0};

            GTIFDirectoryInfo(hGTIF, anVersion, &nKeyCount);

            if (nKeyCount > 0)  // Use LOCAL_CS if we have any geokeys at all.
            {
                // Handle citation.
                strcpy(szPCSName, "unnamed");
                if (!GDALGTIFKeyGetASCII(hGTIF, GTCitationGeoKey, szPCSName,
                                         sizeof(szPCSName)))
                    GDALGTIFKeyGetASCII(hGTIF, GeogCitationGeoKey, szPCSName,
                                        sizeof(szPCSName));

                GTIFCleanupImagineNames(szPCSName);
                oSRS.SetLocalCS(szPCSName);

                // Handle units
                if (psDefn->UOMLength != KvUserDefined)
                {
#if LIBGEOTIFF_VERSION >= 1600
                    GTIFGetUOMLengthInfoEx(projContext,
#else
                    GTIFGetUOMLengthInfo(
#endif
                                           psDefn->UOMLength, &pszUnitsName,
                                           nullptr);
                }

                if (pszUnitsName != nullptr)
                {
                    char szUOMLength[12];
                    snprintf(szUOMLength, sizeof(szUOMLength), "%d",
                             psDefn->UOMLength);
                    oSRS.SetTargetLinearUnits(nullptr, pszUnitsName,
                                              psDefn->UOMLengthInMeters, "EPSG",
                                              szUOMLength);
                }
                else
                    oSRS.SetLinearUnits("unknown", psDefn->UOMLengthInMeters);

                if (verticalUnits != 0)
                {
                    char szVertCSCitation[2048] = {0};
                    if (GDALGTIFKeyGetASCII(hGTIF, VerticalCitationGeoKey,
                                            szVertCSCitation,
                                            sizeof(szVertCSCitation)))
                    {
                        if (STARTS_WITH_CI(szVertCSCitation, "VCS Name = "))
                        {
                            memmove(szVertCSCitation,
                                    szVertCSCitation + strlen("VCS Name = "),
                                    strlen(szVertCSCitation +
                                           strlen("VCS Name = ")) +
                                        1);
                            char *pszPipeChar = strchr(szVertCSCitation, '|');
                            if (pszPipeChar)
                                *pszPipeChar = '\0';
                        }
                    }
                    else
                    {
                        strcpy(szVertCSCitation, "unknown");
                    }

                    const char *pszHorizontalName = oSRS.GetName();
                    const std::string osHorizontalName(
                        pszHorizontalName ? pszHorizontalName : "unnamed");
                    /* --------------------------------------------------------------------
                     */
                    /*      Promote to being a compound coordinate system. */
                    /* --------------------------------------------------------------------
                     */
                    OGR_SRSNode *poOldRoot = oSRS.GetRoot()->Clone();

                    oSRS.Clear();

                    /* --------------------------------------------------------------------
                     */
                    /*      Set COMPD_CS name. */
                    /* --------------------------------------------------------------------
                     */
                    char szCTString[512];
                    szCTString[0] = '\0';
                    if (GDALGTIFKeyGetASCII(hGTIF, GTCitationGeoKey, szCTString,
                                            sizeof(szCTString)) &&
                        strstr(szCTString, " = ") == nullptr)
                    {
                        oSRS.SetNode("COMPD_CS", szCTString);
                    }
                    else
                    {
                        oSRS.SetNode("COMPD_CS", (osHorizontalName + " + " +
                                                  szVertCSCitation)
                                                     .c_str());
                    }

                    oSRS.GetRoot()->AddChild(poOldRoot);

                    FillCompoundCRSWithManualVertCS(
                        hGTIF, oSRS, szVertCSCitation, verticalDatum,
                        verticalUnits);
                }

                GTIFFreeMemory(pszUnitsName);
            }
            return;
        }
    }

    /* -------------------------------------------------------------------- */
    /*      Handle Geocentric coordinate systems.                           */
    /* -------------------------------------------------------------------- */
    if (psDefn->Model == ModelTypeGeocentric)
    {
        char szName[300] = {'\0'};

        strcpy(szName, "unnamed");
        if (!GDALGTIFKeyGetASCII(hGTIF, GTCitationGeoKey, szName,
                                 sizeof(szName)))
            GDALGTIFKeyGetASCII(hGTIF, GeogCitationGeoKey, szName,
                                sizeof(szName));

        oSRS.SetGeocCS(szName);

        char *pszUnitsName = nullptr;

        if (psDefn->UOMLength != KvUserDefined)
        {
#if LIBGEOTIFF_VERSION >= 1600
            GTIFGetUOMLengthInfoEx(projContext,
#else
            GTIFGetUOMLengthInfo(
#endif
                                   psDefn->UOMLength, &pszUnitsName, nullptr);
        }

        if (pszUnitsName != nullptr)
        {
            char szUOMLength[12];
            snprintf(szUOMLength, sizeof(szUOMLength), "%d", psDefn->UOMLength);
            oSRS.SetTargetLinearUnits(nullptr, pszUnitsName,
                                      psDefn->UOMLengthInMeters, "EPSG",
                                      szUOMLength);
        }
        else
            oSRS.SetLinearUnits("unknown", psDefn->UOMLengthInMeters);

        GTIFFreeMemory(pszUnitsName);
    }

    /* -------------------------------------------------------------------- */
    /*      #3901: In libgeotiff 1.3.0 and earlier we incorrectly           */
    /*      interpreted linear projection parameter geokeys (false          */
    /*      easting/northing) as being in meters instead of the             */
    /*      coordinate system of the file.   The following code attempts    */
    /*      to provide mechanisms for fixing the issue if we are linked     */
    /*      with an older version of libgeotiff.                            */
    /* -------------------------------------------------------------------- */
    const char *pszLinearUnits =
        CPLGetConfigOption("GTIFF_LINEAR_UNITS", "DEFAULT");

    /* -------------------------------------------------------------------- */
    /*      #3901: If folks have broken GeoTIFF files generated with        */
    /*      older versions of GDAL+libgeotiff, then they may need a         */
    /*      hack to allow them to be read properly.  This is that           */
    /*      hack.  We basically try to undue the conversion applied by      */
    /*      libgeotiff to meters (or above) to simulate the old             */
    /*      behavior.                                                       */
    /* -------------------------------------------------------------------- */
    unsigned short bLinearUnitsMarkedCorrect = FALSE;

    GDALGTIFKeyGetSHORT(hGTIF, ProjLinearUnitsInterpCorrectGeoKey,
                        &bLinearUnitsMarkedCorrect, 0, 1);

    if (EQUAL(pszLinearUnits, "BROKEN") &&
        psDefn->Projection == KvUserDefined && !bLinearUnitsMarkedCorrect)
    {
        for (int iParam = 0; iParam < psDefn->nParms; iParam++)
        {
            switch (psDefn->ProjParmId[iParam])
            {
                case ProjFalseEastingGeoKey:
                case ProjFalseNorthingGeoKey:
                case ProjFalseOriginEastingGeoKey:
                case ProjFalseOriginNorthingGeoKey:
                case ProjCenterEastingGeoKey:
                case ProjCenterNorthingGeoKey:
                    if (psDefn->UOMLengthInMeters != 0 &&
                        psDefn->UOMLengthInMeters != 1.0)
                    {
                        psDefn->ProjParm[iParam] /= psDefn->UOMLengthInMeters;
                        CPLDebug(
                            "GTIFF",
                            "Converting geokey to accommodate old broken file "
                            "due to GTIFF_LINEAR_UNITS=BROKEN setting.");
                    }
                    break;

                default:
                    break;
            }
        }
    }

    /* -------------------------------------------------------------------- */
    /*      If this is a projected SRS we set the PROJCS keyword first      */
    /*      to ensure that the GEOGCS will be a child.                      */
    /* -------------------------------------------------------------------- */
    OGRBoolean linearUnitIsSet = FALSE;
    if (psDefn->Model == ModelTypeProjected)
    {
        char szCTString[512] = {'\0'};
        if (psDefn->PCS != KvUserDefined)
        {
            char *pszPCSName = nullptr;

#if LIBGEOTIFF_VERSION >= 1600
            GTIFGetPCSInfoEx(projContext,
#else
            GTIFGetPCSInfo(
#endif
                             psDefn->PCS, &pszPCSName, nullptr, nullptr,
                             nullptr);

            oSRS.SetProjCS(pszPCSName ? pszPCSName : "unnamed");
            if (pszPCSName)
                GTIFFreeMemory(pszPCSName);

            oSRS.SetLinearUnits("unknown", 1.0);
        }
        else
        {
            bool bTryGTCitationGeoKey = true;
            if (GDALGTIFKeyGetASCII(hGTIF, PCSCitationGeoKey, szCTString,
                                    sizeof(szCTString)))
            {
                bTryGTCitationGeoKey = false;
                if (!SetCitationToSRS(hGTIF, szCTString, sizeof(szCTString),
                                      PCSCitationGeoKey, &oSRS,
                                      &linearUnitIsSet))
                {
                    if (!STARTS_WITH_CI(szCTString, "LUnits = "))
                    {
                        oSRS.SetProjCS(szCTString);
                        oSRS.SetLinearUnits("unknown", 1.0);
                    }
                    else
                    {
                        bTryGTCitationGeoKey = true;
                    }
                }
            }

            if (bTryGTCitationGeoKey)
            {
                if (GDALGTIFKeyGetASCII(hGTIF, GTCitationGeoKey, szCTString,
                                        sizeof(szCTString)) &&
                    !SetCitationToSRS(hGTIF, szCTString, sizeof(szCTString),
                                      GTCitationGeoKey, &oSRS,
                                      &linearUnitIsSet))
                {
                    oSRS.SetNode("PROJCS", szCTString);
                    oSRS.SetLinearUnits("unknown", 1.0);
                }
                else
                {
                    oSRS.SetNode("PROJCS", "unnamed");
                    oSRS.SetLinearUnits("unknown", 1.0);
                }
            }
        }

        /* Handle ESRI/Erdas style state plane and UTM in citation key */
        if (CheckCitationKeyForStatePlaneUTM(hGTIF, psDefn, &oSRS,
                                             &linearUnitIsSet))
        {
            return;
        }

        /* Handle ESRI PE string in citation */
        szCTString[0] = '\0';
        if (GDALGTIFKeyGetASCII(hGTIF, GTCitationGeoKey, szCTString,
                                sizeof(szCTString)))
            SetCitationToSRS(hGTIF, szCTString, sizeof(szCTString),
                             GTCitationGeoKey, &oSRS, &linearUnitIsSet);
    }

    /* ==================================================================== */
    /*      Setup the GeogCS                                                */
    /* ==================================================================== */
    char *pszGeogName = nullptr;
    char *pszDatumName = nullptr;
    char *pszPMName = nullptr;
    char *pszSpheroidName = nullptr;
    char *pszAngularUnits = nullptr;
    char szGCSName[512] = {'\0'};

    if (!
#if LIBGEOTIFF_VERSION >= 1600
        GTIFGetGCSInfoEx(projContext,
#else
        GTIFGetGCSInfo(
#endif
                         psDefn->GCS, &pszGeogName, nullptr, nullptr,
                         nullptr) &&
        GDALGTIFKeyGetASCII(hGTIF, GeogCitationGeoKey, szGCSName,
                            sizeof(szGCSName)))
    {
        GetGeogCSFromCitation(szGCSName, sizeof(szGCSName), GeogCitationGeoKey,
                              &pszGeogName, &pszDatumName, &pszPMName,
                              &pszSpheroidName, &pszAngularUnits);
    }
    else
    {
        GTIFToCPLRecycleString(&pszGeogName);
    }

    if (pszGeogName && STARTS_WITH(pszGeogName, "GCS_"))
    {
        // Morph from potential ESRI name
        char *pszOfficialName = GTIFGetEPSGOfficialName(
            hGTIF, PJ_TYPE_GEOGRAPHIC_2D_CRS, pszGeogName);
        if (pszOfficialName)
        {
            CPLFree(pszGeogName);
            pszGeogName = pszOfficialName;
        }
    }

    if (pszDatumName && strchr(pszDatumName, '_'))
    {
        // Morph from potential ESRI name
        char *pszOfficialName = GTIFGetEPSGOfficialName(
            hGTIF, PJ_TYPE_GEODETIC_REFERENCE_FRAME, pszDatumName);
        if (pszOfficialName)
        {
            CPLFree(pszDatumName);
            pszDatumName = pszOfficialName;
        }
    }

    if (pszSpheroidName && strchr(pszSpheroidName, '_'))
    {
        // Morph from potential ESRI name
        char *pszOfficialName =
            GTIFGetEPSGOfficialName(hGTIF, PJ_TYPE_ELLIPSOID, pszSpheroidName);
        if (pszOfficialName)
        {
            CPLFree(pszSpheroidName);
            pszSpheroidName = pszOfficialName;
        }
    }

    if (!pszDatumName)
    {
#if LIBGEOTIFF_VERSION >= 1600
        GTIFGetDatumInfoEx(projContext,
#else
        GTIFGetDatumInfo(
#endif
                           psDefn->Datum, &pszDatumName, nullptr);
        GTIFToCPLRecycleString(&pszDatumName);
    }

    double dfSemiMajor = 0.0;
    double dfInvFlattening = 0.0;
    if (!pszSpheroidName)
    {
#if LIBGEOTIFF_VERSION >= 1600
        GTIFGetEllipsoidInfoEx(projContext,
#else
        GTIFGetEllipsoidInfo(
#endif
                               psDefn->Ellipsoid, &pszSpheroidName, nullptr,
                               nullptr);
        GTIFToCPLRecycleString(&pszSpheroidName);
    }
    else
    {
        CPL_IGNORE_RET_VAL(GDALGTIFKeyGetDOUBLE(hGTIF, GeogSemiMajorAxisGeoKey,
                                                &(psDefn->SemiMajor), 0, 1));
        CPL_IGNORE_RET_VAL(GDALGTIFKeyGetDOUBLE(hGTIF, GeogInvFlatteningGeoKey,
                                                &dfInvFlattening, 0, 1));
        if (std::isinf(dfInvFlattening))
        {
            // Deal with the non-nominal case of
            // https://github.com/OSGeo/PROJ/issues/2317
            dfInvFlattening = 0;
        }
    }
    if (!pszPMName)
    {
#if LIBGEOTIFF_VERSION >= 1600
        GTIFGetPMInfoEx(projContext,
#else
        GTIFGetPMInfo(
#endif
                        psDefn->PM, &pszPMName, nullptr);
        GTIFToCPLRecycleString(&pszPMName);
    }
    else
    {
        CPL_IGNORE_RET_VAL(
            GDALGTIFKeyGetDOUBLE(hGTIF, GeogPrimeMeridianLongGeoKey,
                                 &(psDefn->PMLongToGreenwich), 0, 1));
    }

    if (!pszAngularUnits)
    {
#if LIBGEOTIFF_VERSION >= 1600
        GTIFGetUOMAngleInfoEx(projContext,
#else
        GTIFGetUOMAngleInfo(
#endif
                              psDefn->UOMAngle, &pszAngularUnits,
                              &psDefn->UOMAngleInDegrees);
        if (pszAngularUnits == nullptr)
            pszAngularUnits = CPLStrdup("unknown");
        else
            GTIFToCPLRecycleString(&pszAngularUnits);
    }
    else
    {
        double dfRadians = 0.0;
        if (GDALGTIFKeyGetDOUBLE(hGTIF, GeogAngularUnitSizeGeoKey, &dfRadians,
                                 0, 1))
        {
            psDefn->UOMAngleInDegrees = dfRadians / CPLAtof(SRS_UA_DEGREE_CONV);
        }
    }

    // Avoid later division by zero.
    if (psDefn->UOMAngleInDegrees == 0)
    {
        CPLError(CE_Warning, CPLE_AppDefined,
                 "Invalid value for GeogAngularUnitSizeGeoKey.");
        psDefn->UOMAngleInDegrees = 1;
    }

    dfSemiMajor = psDefn->SemiMajor;
    if (dfSemiMajor == 0.0)
    {
        CPLFree(pszSpheroidName);
        pszSpheroidName = CPLStrdup("unretrievable - using WGS84");
        dfSemiMajor = SRS_WGS84_SEMIMAJOR;
        dfInvFlattening = SRS_WGS84_INVFLATTENING;
    }
    else if (dfInvFlattening == 0.0 &&
             ((psDefn->SemiMinor / psDefn->SemiMajor) < 0.99999999999999999 ||
              (psDefn->SemiMinor / psDefn->SemiMajor) > 1.00000000000000001))
    {
        dfInvFlattening =
            OSRCalcInvFlattening(psDefn->SemiMajor, psDefn->SemiMinor);

        /* Take official inverse flattening definition in the WGS84 case */
        if (fabs(dfSemiMajor - SRS_WGS84_SEMIMAJOR) < 1e-10 &&
            fabs(dfInvFlattening - SRS_WGS84_INVFLATTENING) < 1e-10)
            dfInvFlattening = SRS_WGS84_INVFLATTENING;
    }
    if (!pszGeogName || strlen(pszGeogName) == 0)
    {
        CPLFree(pszGeogName);
        pszGeogName = CPLStrdup(pszDatumName ? pszDatumName : "unknown");
    }

    oSRS.SetGeogCS(pszGeogName, pszDatumName, pszSpheroidName, dfSemiMajor,
                   dfInvFlattening, pszPMName, psDefn->PMLongToGreenwich,
                   pszAngularUnits,
                   psDefn->UOMAngleInDegrees * CPLAtof(SRS_UA_DEGREE_CONV));

    bool bGeog3DCRS = false;
    bool bSetDatumEllipsoidCode = true;
    bool bHasWarnedInconsistentGeogCRSEPSG = false;
    {
        const int nGCS = psDefn->GCS;
        if (nGCS != KvUserDefined && nGCS > 0 &&
            psDefn->Model != ModelTypeGeocentric)
        {
            OGRSpatialReference oSRSGeog{ projContext };
            const bool bGCSCodeValid =
                oSRSGeog.importFromEPSG(nGCS) == OGRERR_NONE;

            const std::string osGTiffSRSSource =
                CPLGetConfigOption("GTIFF_SRS_SOURCE", "");

            // GeoTIFF 1.0 might put a Geographic 3D code in GeodeticCRSGeoKey
            bool bTryCompareToEPSG = oSRSGeog.GetAxesCount() == 2;

            if (psDefn->Datum != KvUserDefined)
            {
                char szCode[12];
                snprintf(szCode, sizeof(szCode), "%d", psDefn->Datum);
                auto ctx = static_cast<PJ_CONTEXT *>(
                    GTIFGetPROJContext(hGTIF, true, nullptr));
                auto datum = proj_create_from_database(
                    ctx, "EPSG", szCode, PJ_CATEGORY_DATUM, 0, nullptr);
                if (datum)
                {
                    if (proj_get_type(datum) ==
                        PJ_TYPE_DYNAMIC_GEODETIC_REFERENCE_FRAME)
                    {
                        // Current PROJ versions will not manage to
                        // consider a CRS with a regular datum and another one
                        // with a dynamic datum as being equivalent.
                        bTryCompareToEPSG = false;
                    }
                    proj_destroy(datum);
                }
            }

            if (bTryCompareToEPSG && !oSRSGeog.IsSameGeogCS(&oSRS) &&
                osGTiffSRSSource.empty())
            {
                // See https://github.com/OSGeo/gdal/issues/5399
                // where a file has inconsistent GeogSemiMinorAxisGeoKey /
                // GeogInvFlatteningGeoKey values, which cause its datum to be
                // considered as non-equivalent to the EPSG one.
                CPLError(
                    CE_Warning, CPLE_AppDefined,
                    "The definition of geographic CRS EPSG:%d got from GeoTIFF "
                    "keys "
                    "is not the same as the one from the EPSG registry, "
                    "which may cause issues during reprojection operations. "
                    "Set GTIFF_SRS_SOURCE configuration option to EPSG to "
                    "use official parameters (overriding the ones from GeoTIFF "
                    "keys), "
                    "or to GEOKEYS to use custom values from GeoTIFF keys "
                    "and drop the EPSG code.",
                    nGCS);
                bHasWarnedInconsistentGeogCRSEPSG = true;
            }
            if (EQUAL(osGTiffSRSSource.c_str(), "EPSG"))
            {
                oSRS.CopyGeogCSFrom(&oSRSGeog);
            }
            else if (osGTiffSRSSource.empty() && oSRSGeog.IsDynamic() &&
                     psDefn->Model == ModelTypeGeographic)
            {
                // We should perhaps be more careful and detect overrides
                // of geokeys...
                oSRS = oSRSGeog;
                bSetDatumEllipsoidCode = false;
            }
            else if (bGCSCodeValid && osGTiffSRSSource.empty())
            {
                oSRS.SetAuthority("GEOGCS", "EPSG", nGCS);
            }
            else
            {
                bSetDatumEllipsoidCode = false;
            }

            int nVertSRSCode = verticalCSType;
            if (verticalDatum == 6030 && nGCS == 4326)  // DatumE_WGS84
            {
                nVertSRSCode = 4979;
            }

            // Try to reconstruct a Geographic3D CRS from the
            // GeodeticCRSGeoKey and the VerticalGeoKey, when they are
            // consistent
            if (nVertSRSCode > 0 && nVertSRSCode != KvUserDefined)
            {
                OGRSpatialReference oTmpVertSRS{ projContext };
                if (oSRSGeog.IsGeographic() && oSRSGeog.GetAxesCount() == 2 &&
                    oTmpVertSRS.importFromEPSG(nVertSRSCode) == OGRERR_NONE &&
                    oTmpVertSRS.IsGeographic() &&
                    oTmpVertSRS.GetAxesCount() == 3)
                {
                    const char *pszTmpCode =
                        oSRSGeog.GetAuthorityCode("GEOGCS|DATUM");
                    const char *pszTmpVertCode =
                        oTmpVertSRS.GetAuthorityCode("GEOGCS|DATUM");
                    if (pszTmpCode && pszTmpVertCode &&
                        atoi(pszTmpCode) == atoi(pszTmpVertCode))
                    {
                        verticalCSType = 0;
                        verticalDatum = 0;
                        verticalUnits = 0;
                        oSRS.CopyGeogCSFrom(&oTmpVertSRS);
                        bSetDatumEllipsoidCode = false;
                        bGeog3DCRS = true;
                    }
                }
            }
        }
    }
    if (bSetDatumEllipsoidCode)
    {
        if (psDefn->Datum != KvUserDefined)
            oSRS.SetAuthority("DATUM", "EPSG", psDefn->Datum);

        if (psDefn->Ellipsoid != KvUserDefined)
            oSRS.SetAuthority("SPHEROID", "EPSG", psDefn->Ellipsoid);
    }

    CPLFree(pszGeogName);
    CPLFree(pszDatumName);
    CPLFree(pszSpheroidName);
    CPLFree(pszPMName);
    CPLFree(pszAngularUnits);

    /* -------------------------------------------------------------------- */
    /*      Set projection units if not yet done                            */
    /* -------------------------------------------------------------------- */
    if (psDefn->Model == ModelTypeProjected && !linearUnitIsSet)
    {
        char *pszUnitsName = nullptr;

        if (psDefn->UOMLength != KvUserDefined)
        {
#if LIBGEOTIFF_VERSION >= 1600
            GTIFGetUOMLengthInfoEx(projContext,
#else
            GTIFGetUOMLengthInfo(
#endif
                                   psDefn->UOMLength, &pszUnitsName, nullptr);
        }

        if (pszUnitsName != nullptr)
        {
            char szUOMLength[12];
            snprintf(szUOMLength, sizeof(szUOMLength), "%d", psDefn->UOMLength);
            oSRS.SetTargetLinearUnits(nullptr, pszUnitsName,
                                      psDefn->UOMLengthInMeters, "EPSG",
                                      szUOMLength);
        }
        else
            oSRS.SetLinearUnits("unknown", psDefn->UOMLengthInMeters);

        GTIFFreeMemory(pszUnitsName);
    }

    /* ==================================================================== */
    /*      Try to import PROJCS from ProjectedCSTypeGeoKey if we           */
    /*      have essentially only it. We could relax a bit the constraints  */
    /*      but that should do for now. This may mask shortcomings in the   */
    /*      libgeotiff GTIFGetDefn() function.                              */
    /* ==================================================================== */
    unsigned short tmp = 0;
    bool bGotFromEPSG = false;
    if (psDefn->Model == ModelTypeProjected && psDefn->PCS != KvUserDefined &&
        GDALGTIFKeyGetSHORT(hGTIF, ProjectionGeoKey, &tmp, 0, 1) == 0 &&
        GDALGTIFKeyGetSHORT(hGTIF, ProjCoordTransGeoKey, &tmp, 0, 1) == 0 &&
        GDALGTIFKeyGetSHORT(hGTIF, GeographicTypeGeoKey, &tmp, 0, 1) == 0 &&
        GDALGTIFKeyGetSHORT(hGTIF, GeogGeodeticDatumGeoKey, &tmp, 0, 1) == 0 &&
        GDALGTIFKeyGetSHORT(hGTIF, GeogEllipsoidGeoKey, &tmp, 0, 1) == 0 &&
        CPLTestBool(CPLGetConfigOption("GTIFF_IMPORT_FROM_EPSG", "YES")))
    {
#ifdef MECHSOFT_DEVIATION
        // Save error state as importFromEPSGA() will call CPLReset()
        CPLErrorNum errNo = CPLGetLastErrorNo();
        CPLErr eErr = CPLGetLastErrorType();
        const char *pszTmp = CPLGetLastErrorMsg();
        char *pszLastErrorMsg = CPLStrdup(pszTmp ? pszTmp : "");
        CPLPushErrorHandler(CPLQuietErrorHandler);
#endif
        OGRSpatialReference oSRSTmp{ projContext };
        OGRErr eImportErr = oSRSTmp.importFromEPSG(psDefn->PCS);
#ifdef MECHSOFT_DEVIATION
        CPLPopErrorHandler();
        // Restore error state
        CPLErrorSetState(eErr, errNo, pszLastErrorMsg);
        CPLFree(pszLastErrorMsg);
#endif
        bGotFromEPSG = eImportErr == OGRERR_NONE;

        if (bGotFromEPSG)
        {
            // See #6210. In case there's an overridden linear units, take it
            // into account
            const char *pszUnitsName = nullptr;
            double dfUOMLengthInMeters = oSRS.GetLinearUnits(&pszUnitsName);
            // Non exact comparison, as there's a slight difference between
            // the evaluation of US Survey foot hardcoded in geo_normalize.c to
            // 12.0 / 39.37, and the corresponding value returned by
            // PROJ >= 6.0.0 and <= 7.0.0 for EPSG:9003
            if (fabs(dfUOMLengthInMeters - oSRSTmp.GetLinearUnits(nullptr)) >
                1e-15 * dfUOMLengthInMeters)
            {
                CPLDebug("GTiff", "Modify EPSG:%d to have %s linear units...",
                         psDefn->PCS, pszUnitsName ? pszUnitsName : "unknown");

                const char *pszUnitAuthorityCode =
                    oSRS.GetAuthorityCode("PROJCS|UNIT");
                const char *pszUnitAuthorityName =
                    oSRS.GetAuthorityName("PROJCS|UNIT");

                if (pszUnitsName)
                    oSRSTmp.SetLinearUnitsAndUpdateParameters(
                        pszUnitsName, dfUOMLengthInMeters, pszUnitAuthorityCode,
                        pszUnitAuthorityName);
            }

            if (bGeog3DCRS)
            {
                oSRSTmp.CopyGeogCSFrom(&oSRS);
                oSRSTmp.UpdateCoordinateSystemFromGeogCRS();
            }
            oSRS = std::move(oSRSTmp);
        }
    }

#if !defined(GEO_NORMALIZE_DISABLE_TOWGS84)
    if (psDefn->TOWGS84Count > 0 && bGotFromEPSG &&
        CPLTestBool(CPLGetConfigOption("OSR_STRIP_TOWGS84", "YES")))
    {
        CPLDebug("OSR", "TOWGS84 information has been removed. "
                        "It can be kept by setting the OSR_STRIP_TOWGS84 "
                        "configuration option to NO");
    }
    else if (psDefn->TOWGS84Count > 0 &&
             (!bGotFromEPSG ||
              !CPLTestBool(CPLGetConfigOption("OSR_STRIP_TOWGS84", "YES"))))
    {
        if (bGotFromEPSG)
        {
            double adfTOWGS84[7] = {0.0};
            CPL_IGNORE_RET_VAL(oSRS.GetTOWGS84(adfTOWGS84));
            bool bSame = true;
            for (int i = 0; i < 7; i++)
            {
                if (fabs(adfTOWGS84[i] - psDefn->TOWGS84[i]) > 1e-5)
                {
                    bSame = false;
                    break;
                }
            }
            if (!bSame)
            {
                CPLDebug("GTiff",
                         "Modify EPSG:%d to have "
                         "TOWGS84=%f,%f,%f,%f,%f,%f,%f "
                         "coming from GeogTOWGS84GeoKey, instead of "
                         "%f,%f,%f,%f,%f,%f,%f coming from EPSG",
                         psDefn->PCS, psDefn->TOWGS84[0], psDefn->TOWGS84[1],
                         psDefn->TOWGS84[2], psDefn->TOWGS84[3],
                         psDefn->TOWGS84[4], psDefn->TOWGS84[5],
                         psDefn->TOWGS84[6], adfTOWGS84[0], adfTOWGS84[1],
                         adfTOWGS84[2], adfTOWGS84[3], adfTOWGS84[4],
                         adfTOWGS84[5], adfTOWGS84[6]);
            }
        }

        oSRS.SetTOWGS84(psDefn->TOWGS84[0], psDefn->TOWGS84[1],
                        psDefn->TOWGS84[2], psDefn->TOWGS84[3],
                        psDefn->TOWGS84[4], psDefn->TOWGS84[5],
                        psDefn->TOWGS84[6]);
    }
#endif

    /* ==================================================================== */
    /*      Handle projection parameters.                                   */
    /* ==================================================================== */
    if (psDefn->Model == ModelTypeProjected && !bGotFromEPSG)
    {
        /* --------------------------------------------------------------------
         */
        /*      Make a local copy of params, and convert back into the */
        /*      angular units of the GEOGCS and the linear units of the */
        /*      projection. */
        /* --------------------------------------------------------------------
         */
        double adfParam[10] = {0.0};
        int i = 0;  // Used after for.

        for (; i < std::min(10, psDefn->nParms); i++)
            adfParam[i] = psDefn->ProjParm[i];

        for (; i < 10; i++)
            adfParam[i] = 0.0;

#if LIBGEOTIFF_VERSION <= 1730
        // libgeotiff <= 1.7.3 is unfortunately inconsistent. When it synthetizes the
        // projection parameters from the EPSG ProjectedCRS code, it returns
        // them normalized in degrees. But when it gets them from
        // ProjCoordTransGeoKey and other Proj....GeoKey's it return them in
        // a raw way, that is in the units of GeogAngularUnitSizeGeoKey
        // The below oSRS.SetXXXXX() methods assume the angular projection
        // parameters to be in degrees, so convert them to degrees in that later case.
        // From GDAL 3.0 to 3.9.0, we didn't do that conversion...
        // And all versions of GDAL <= 3.9.0 when writing those geokeys, wrote
        // them as degrees, hence this GTIFF_READ_ANGULAR_PARAMS_IN_DEGREE
        // config option that can be set to YES to avoid that conversion and
        // assume that the angular parameters have been written as degree.
        if (GDALGTIFKeyGetSHORT(hGTIF, ProjCoordTransGeoKey, &tmp, 0, 1) &&
            !CPLTestBool(CPLGetConfigOption(
                "GTIFF_READ_ANGULAR_PARAMS_IN_DEGREE", "NO")))
        {
            adfParam[0] *= psDefn->UOMAngleInDegrees;
            adfParam[1] *= psDefn->UOMAngleInDegrees;
            adfParam[2] *= psDefn->UOMAngleInDegrees;
            adfParam[3] *= psDefn->UOMAngleInDegrees;
        }
#else
        // If GTIFF_READ_ANGULAR_PARAMS_IN_DEGREE=YES (non-nominal case), undo
        // the conversion to degrees, that has been done by libgeotiff > 1.7.3
        if (GDALGTIFKeyGetSHORT(hGTIF, ProjCoordTransGeoKey, &tmp, 0, 1) &&
            psDefn->UOMAngleInDegrees != 0 && psDefn->UOMAngleInDegrees != 1 &&
            CPLTestBool(CPLGetConfigOption(
                "GTIFF_READ_ANGULAR_PARAMS_IN_DEGREE", "NO")))
        {
            adfParam[0] /= psDefn->UOMAngleInDegrees;
            adfParam[1] /= psDefn->UOMAngleInDegrees;
            adfParam[2] /= psDefn->UOMAngleInDegrees;
            adfParam[3] /= psDefn->UOMAngleInDegrees;
        }
#endif

        /* --------------------------------------------------------------------
         */
        /*      Translation the fundamental projection. */
        /* --------------------------------------------------------------------
         */
        switch (psDefn->CTProjection)
        {
            case CT_TransverseMercator:
                oSRS.SetTM(adfParam[0], adfParam[1], adfParam[4], adfParam[5],
                           adfParam[6]);
                break;

            case CT_TransvMercator_SouthOriented:
                oSRS.SetTMSO(adfParam[0], adfParam[1], adfParam[4], adfParam[5],
                             adfParam[6]);
                break;

            case CT_Mercator:
                // If a lat_ts was specified use 2SP, otherwise use 1SP.
                if (psDefn->ProjParmId[2] == ProjStdParallel1GeoKey)
                {
                    if (psDefn->ProjParmId[4] == ProjScaleAtNatOriginGeoKey)
                        CPLError(CE_Warning, CPLE_AppDefined,
                                 "Mercator projection should not define "
                                 "both StdParallel1 and ScaleAtNatOrigin.  "
                                 "Using StdParallel1 and ignoring "
                                 "ScaleAtNatOrigin.");
                    oSRS.SetMercator2SP(adfParam[2], adfParam[0], adfParam[1],
                                        adfParam[5], adfParam[6]);
                }
                else
                    oSRS.SetMercator(adfParam[0], adfParam[1], adfParam[4],
                                     adfParam[5], adfParam[6]);

                // Override hack for google mercator.
                if (psDefn->Projection == 1024 || psDefn->Projection == 9841)
                {
                    oSRS.SetExtension(
                        "PROJCS", "PROJ4",
                        "+proj=merc +a=6378137 +b=6378137 +lat_ts=0.0 "
                        "+lon_0=0.0 "
                        "+x_0=0.0 +y_0=0 +k=1.0 +units=m +nadgrids=@null "
                        "+wktext  +no_defs");  // TODO(schwehr): Why 2 spaces?
                }
                break;

            case CT_ObliqueStereographic:
                oSRS.SetOS(adfParam[0], adfParam[1], adfParam[4], adfParam[5],
                           adfParam[6]);
                break;

            case CT_Stereographic:
                oSRS.SetStereographic(adfParam[0], adfParam[1], adfParam[4],
                                      adfParam[5], adfParam[6]);
                break;

            case CT_ObliqueMercator:  // Hotine.
                oSRS.SetHOM(adfParam[0], adfParam[1], adfParam[2], adfParam[3],
                            adfParam[4], adfParam[5], adfParam[6]);
                break;

            case CT_HotineObliqueMercatorAzimuthCenter:
                oSRS.SetHOMAC(adfParam[0], adfParam[1], adfParam[2],
                              adfParam[3], adfParam[4], adfParam[5],
                              adfParam[6]);
                break;

            case CT_ObliqueMercator_Laborde:
                oSRS.SetLOM(adfParam[0], adfParam[1], adfParam[2], adfParam[4],
                            adfParam[5], adfParam[6]);
                break;

            case CT_EquidistantConic:
                oSRS.SetEC(adfParam[0], adfParam[1], adfParam[2], adfParam[3],
                           adfParam[5], adfParam[6]);
                break;

            case CT_CassiniSoldner:
                oSRS.SetCS(adfParam[0], adfParam[1], adfParam[5], adfParam[6]);
                break;

            case CT_Polyconic:
                oSRS.SetPolyconic(adfParam[0], adfParam[1], adfParam[5],
                                  adfParam[6]);
                break;

            case CT_AzimuthalEquidistant:
                oSRS.SetAE(adfParam[0], adfParam[1], adfParam[5], adfParam[6]);
                break;

            case CT_MillerCylindrical:
                oSRS.SetMC(adfParam[0], adfParam[1], adfParam[5], adfParam[6]);
                break;

            case CT_Equirectangular:
                oSRS.SetEquirectangular2(adfParam[0], adfParam[1], adfParam[2],
                                         adfParam[5], adfParam[6]);
                break;

            case CT_Gnomonic:
                oSRS.SetGnomonic(adfParam[0], adfParam[1], adfParam[5],
                                 adfParam[6]);
                break;

            case CT_LambertAzimEqualArea:
                oSRS.SetLAEA(adfParam[0], adfParam[1], adfParam[5],
                             adfParam[6]);
                break;

            case CT_Orthographic:
                oSRS.SetOrthographic(adfParam[0], adfParam[1], adfParam[5],
                                     adfParam[6]);
                break;

            case CT_Robinson:
                oSRS.SetRobinson(adfParam[1], adfParam[5], adfParam[6]);
                break;

            case CT_Sinusoidal:
                oSRS.SetSinusoidal(adfParam[1], adfParam[5], adfParam[6]);
                break;

            case CT_VanDerGrinten:
                oSRS.SetVDG(adfParam[1], adfParam[5], adfParam[6]);
                break;

            case CT_PolarStereographic:
                oSRS.SetPS(adfParam[0], adfParam[1], adfParam[4], adfParam[5],
                           adfParam[6]);
                break;

            case CT_LambertConfConic_2SP:
                oSRS.SetLCC(adfParam[2], adfParam[3], adfParam[0], adfParam[1],
                            adfParam[5], adfParam[6]);
                break;

            case CT_LambertConfConic_1SP:
                oSRS.SetLCC1SP(adfParam[0], adfParam[1], adfParam[4],
                               adfParam[5], adfParam[6]);
                break;

            case CT_AlbersEqualArea:
                oSRS.SetACEA(adfParam[0], adfParam[1], adfParam[2], adfParam[3],
                             adfParam[5], adfParam[6]);
                break;

            case CT_NewZealandMapGrid:
                oSRS.SetNZMG(adfParam[0], adfParam[1], adfParam[5],
                             adfParam[6]);
                break;

            case CT_CylindricalEqualArea:
                oSRS.SetCEA(adfParam[0], adfParam[1], adfParam[5], adfParam[6]);
                break;
            default:
                if (oSRS.IsProjected())
                {
                    const char *pszName = oSRS.GetName();
                    std::string osName(pszName ? pszName : "unnamed");
                    oSRS.Clear();
                    oSRS.SetLocalCS(osName.c_str());
                }
                break;
        }
    }

    if (psDefn->Model == ModelTypeProjected && psDefn->PCS != KvUserDefined &&
        !bGotFromEPSG)
    {
        OGRSpatialReference oSRSTest(oSRS);
        OGRSpatialReference oSRSTmp{ projContext };

        const bool bPCSCodeValid =
            oSRSTmp.importFromEPSG(psDefn->PCS) == OGRERR_NONE;
        oSRSTmp.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);

        // Force axis to avoid issues with SRS with northing, easting order
        oSRSTest.SetAxes(nullptr, "X", OAO_East, "Y", OAO_North);
        oSRSTmp.SetAxes(nullptr, "X", OAO_East, "Y", OAO_North);

        const std::string osGTiffSRSSource =
            CPLGetConfigOption("GTIFF_SRS_SOURCE", "");
        const char *const apszOptions[] = {
            "IGNORE_DATA_AXIS_TO_SRS_AXIS_MAPPING=YES", nullptr};
        if (!bHasWarnedInconsistentGeogCRSEPSG &&
            !oSRSTmp.IsSame(&oSRS, apszOptions) &&
            EQUAL(osGTiffSRSSource.c_str(), ""))
        {
            // See https://github.com/OSGeo/gdal/issues/5399
            // where a file has inconsistent GeogSemiMinorAxisGeoKey /
            // GeogInvFlatteningGeoKey values, which cause its datum to be
            // considered as non-equivalent to the EPSG one.
            CPLError(
                CE_Warning, CPLE_AppDefined,
                "The definition of projected CRS EPSG:%d got from GeoTIFF keys "
                "is not the same as the one from the EPSG registry, "
                "which may cause issues during reprojection operations. "
                "Set GTIFF_SRS_SOURCE configuration option to EPSG to "
                "use official parameters (overriding the ones from GeoTIFF "
                "keys), "
                "or to GEOKEYS to use custom values from GeoTIFF keys "
                "and drop the EPSG code.",
                psDefn->PCS);
        }
        if (EQUAL(osGTiffSRSSource.c_str(), "EPSG"))
        {
            oSRS = std::move(oSRSTmp);
        }
        else if (bPCSCodeValid && EQUAL(osGTiffSRSSource.c_str(), ""))
        {
            oSRS.SetAuthority(nullptr, "EPSG", psDefn->PCS);
        }
    }

    if (oSRS.IsProjected() && oSRS.GetAxesCount() == 2)
    {
        const char *pszProjCRSName = oSRS.GetAttrValue("PROJCS");
        if (pszProjCRSName)
        {
            // Hack to be able to read properly what we have written for
            // ESRI:102113 (ESRI ancient WebMercator).
            if (EQUAL(pszProjCRSName, "WGS_1984_Web_Mercator"))
                oSRS.SetFromUserInput("ESRI:102113");
            // And for EPSG:900913
            else if (EQUAL(pszProjCRSName, "Google Maps Global Mercator"))
                oSRS.importFromEPSG(900913);
            else if (strchr(pszProjCRSName, '_'))
            {
                // Morph from potential ESRI name
                char *pszOfficialName = GTIFGetEPSGOfficialName(
                    hGTIF, PJ_TYPE_PROJECTED_CRS, pszProjCRSName);
                if (pszOfficialName)
                {
                    oSRS.SetProjCS(pszOfficialName);
                    CPLFree(pszOfficialName);
                }
            }
        }
    }

    /* ==================================================================== */
    /*      Handle vertical coordinate system information if we have it.    */
    /* ==================================================================== */
    bool bNeedManualVertCS = false;
    char citation[2048] = {'\0'};

    // See https://github.com/OSGeo/gdal/pull/4197
    if (verticalCSType > KvUserDefined || verticalDatum > KvUserDefined ||
        verticalUnits > KvUserDefined)
    {
        CPLError(CE_Warning, CPLE_AppDefined,
                 "At least one of VerticalCSTypeGeoKey, VerticalDatumGeoKey or "
                 "VerticalUnitsGeoKey has a value in the private user range. "
                 "Ignoring vertical information.");
        verticalCSType = 0;
        verticalDatum = 0;
        verticalUnits = 0;
    }

    if ((verticalCSType != 0 || verticalDatum != 0 || verticalUnits != 0) &&
        (oSRS.IsGeographic() || oSRS.IsProjected() || oSRS.IsLocal()))
    {
        std::string osVertCRSName;
        if (GDALGTIFKeyGetASCII(hGTIF, VerticalCitationGeoKey, citation,
                                sizeof(citation)))
        {
            if (STARTS_WITH_CI(citation, "VCS Name = "))
            {
                memmove(citation, citation + strlen("VCS Name = "),
                        strlen(citation + strlen("VCS Name = ")) + 1);
                char *pszPipeChar = strchr(citation, '|');
                if (pszPipeChar)
                    *pszPipeChar = '\0';
                osVertCRSName = citation;
            }
        }

        OGRSpatialReference oVertSRS{ projContext };
        bool bCanBuildCompoundCRS = oSRS.GetRoot() != nullptr;
        if (verticalCSType != KvUserDefined && verticalCSType > 0)
        {
            if (!(oVertSRS.importFromEPSG(verticalCSType) == OGRERR_NONE &&
                  oVertSRS.IsVertical()))
            {
                bCanBuildCompoundCRS = false;
            }
            else
            {
                osVertCRSName = oVertSRS.GetName();
            }
        }
        if (osVertCRSName.empty())
            osVertCRSName = "unknown";

        if (bCanBuildCompoundCRS)
        {
            const bool bHorizontalHasCode =
                oSRS.GetAuthorityCode(nullptr) != nullptr;
            const char *pszHorizontalName = oSRS.GetName();
            const std::string osHorizontalName(
                pszHorizontalName ? pszHorizontalName : "unnamed");
            /* --------------------------------------------------------------------
             */
            /*      Promote to being a compound coordinate system. */
            /* --------------------------------------------------------------------
             */
            OGR_SRSNode *poOldRoot = oSRS.GetRoot()->Clone();

            oSRS.Clear();

            /* --------------------------------------------------------------------
             */
            /*      Set COMPD_CS name. */
            /* --------------------------------------------------------------------
             */
            char szCTString[512];
            szCTString[0] = '\0';
            if (GDALGTIFKeyGetASCII(hGTIF, GTCitationGeoKey, szCTString,
                                    sizeof(szCTString)) &&
                strstr(szCTString, " = ") == nullptr)
            {
                oSRS.SetNode("COMPD_CS", szCTString);
            }
            else
            {
                oSRS.SetNode(
                    "COMPD_CS",
                    (osHorizontalName + " + " + osVertCRSName).c_str());
            }

            oSRS.GetRoot()->AddChild(poOldRoot);

            /* --------------------------------------------------------------------
             */
            /*      If we have the vertical cs, try to look it up, and use the
             */
            /*      definition provided by that. */
            /* --------------------------------------------------------------------
             */
            bNeedManualVertCS = true;

            if (!oVertSRS.IsEmpty())
            {
                oSRS.GetRoot()->AddChild(oVertSRS.GetRoot()->Clone());
                bNeedManualVertCS = false;

                // GeoTIFF doesn't store EPSG code of CompoundCRS, so
                // if we have an EPSG code for both the horizontal and vertical
                // parts, check if there's a known CompoundCRS associating
                // both
                if (bHorizontalHasCode && verticalCSType != KvUserDefined &&
                    verticalCSType > 0)
                {
                    const auto *poSRSMatch = oSRS.FindBestMatch(100);
                    if (poSRSMatch)
                        oSRS = *poSRSMatch;
                    delete poSRSMatch;
                }
            }
        }
    }

    /* -------------------------------------------------------------------- */
    /*      Collect some information from the VerticalCS if not provided    */
    /*      via geokeys.                                                    */
    /* -------------------------------------------------------------------- */
    if (bNeedManualVertCS)
    {
        FillCompoundCRSWithManualVertCS(hGTIF, oSRS, citation, verticalDatum,
                                        verticalUnits);
    }

    // Hack for tiff_read.py:test_tiff_grads so as to normalize angular
    // parameters to grad
    if (psDefn->UOMAngleInDegrees != 1.0)
    {
        char *pszWKT = nullptr;
        const char *const apszOptions[] = {
            "FORMAT=WKT1", "ADD_TOWGS84_ON_EXPORT_TO_WKT1=NO", nullptr};
        if (oSRS.exportToWkt(&pszWKT, apszOptions) == OGRERR_NONE)
        {
            const char *const apszOptionsImport[] = {
#if PROJ_AT_LEAST_VERSION(9, 1, 0)
                "UNSET_IDENTIFIERS_IF_INCOMPATIBLE_DEF=NO",
#endif
                nullptr
            };
            oSRS.importFromWkt(pszWKT, apszOptionsImport);
        }
        CPLFree(pszWKT);
    }

    oSRS.StripTOWGS84IfKnownDatumAndAllowed();

    double dfCoordinateEpoch = 0.0;
    if (GDALGTIFKeyGetDOUBLE(hGTIF, CoordinateEpochGeoKey, &dfCoordinateEpoch,
                             0, 1))
    {
        oSRS.SetCoordinateEpoch(dfCoordinateEpoch);
    }
}