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
#include "ogr_spatialref.h"

#include "proj.h"

extern void GTIFGetOGISDefnAsOSR(PJ_CONTEXT* ctx, GTIF* hGTIF, GTIFDefn* psDefn, OGRSpatialReference& oSRS);

/************************************************************************/
/*                            GTIFGetWKTEx()                            */
/************************************************************************/
extern "C"
char* GTIFGetProj4DefnEX(void* ctxIn, GTIF * hGTIF, GTIFDefn* psDefn)
{
    OGRSpatialReference oSRS{ (PJ_CONTEXT*)ctxIn };

    GTIFGetOGISDefnAsOSR((PJ_CONTEXT*)ctxIn, hGTIF, psDefn, oSRS);

    char* wkt = nullptr;
    oSRS.exportToProj4(&wkt);

    return CPLStrdup(wkt);
}
