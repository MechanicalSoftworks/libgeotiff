/******************************************************************************
 *
 * Project:  CPL - Common Portability Library
 * Purpose:  Convenience functions.
 * Author:   Frank Warmerdam, warmerdam@pobox.com
 *
 ******************************************************************************
 * Copyright (c) 1998, Frank Warmerdam
 * Copyright (c) 2007-2014, Even Rouault <even dot rouault at spatialys.com>
 *
 * SPDX-License-Identifier: MIT
 ****************************************************************************/

#include "cpl_config.h"

#include <string>

#include "cpl_config.h"
#include "cpl_string.h"

// Uncomment to get list of options that have been fetched and set.
// #define DEBUG_CONFIG_OPTIONS

static bool gbIgnoreEnvVariables =
    false;  // if true, only take into account configuration options set through
            // configuration file or
            // CPLSetConfigOption()/CPLSetThreadLocalConfigOption()

/************************************************************************/
/*                         CPLGetConfigOption()                         */
/************************************************************************/

/**
 * Get the value of a configuration option.
 *
 * The value is the value of a (key, value) option set with
 * CPLSetConfigOption(), or CPLSetThreadLocalConfigOption() of the same
 * thread. If the given option was no defined with
 * CPLSetConfigOption(), it tries to find it in environment variables.
 *
 * Note: the string returned by CPLGetConfigOption() might be short-lived, and
 * in particular it will become invalid after a call to CPLSetConfigOption()
 * with the same key.
 *
 * To override temporary a potentially existing option with a new value, you
 * can use the following snippet :
 * \code{.cpp}
 *     // backup old value
 *     const char* pszOldValTmp = CPLGetConfigOption(pszKey, NULL);
 *     char* pszOldVal = pszOldValTmp ? CPLStrdup(pszOldValTmp) : NULL;
 *     // override with new value
 *     CPLSetConfigOption(pszKey, pszNewVal);
 *     // do something useful
 *     // restore old value
 *     CPLSetConfigOption(pszKey, pszOldVal);
 *     CPLFree(pszOldVal);
 * \endcode
 *
 * @param pszKey the key of the option to retrieve
 * @param pszDefault a default value if the key does not match existing defined
 *     options (may be NULL)
 * @return the value associated to the key, or the default value if not found
 *
 * @see CPLSetConfigOption(), https://gdal.org/user/configoptions.html
 */
extern "C"
const char *CPL_STDCALL CPLGetConfigOption(const char *pszKey,
                                           const char *pszDefault)

{
    const char* pszResult = nullptr;

#ifdef MECHSOFT_DEVIATION
    pszResult = CPLGetThreadLocalConfigOption(pszKey, nullptr);

    if (pszResult == nullptr)
    {
        pszResult = CPLGetGlobalConfigOption(pszKey, nullptr);
    }

    if (gbIgnoreEnvVariables)
    {
        const char *pszEnvVar = getenv(pszKey);
        if (pszEnvVar != nullptr)
        {
            CPLDebug("CPL",
                     "Ignoring environment variable %s=%s because of "
                     "ignore-env-vars=yes setting in configuration file",
                     pszKey, pszEnvVar);
        }
    }
    else if (pszResult == nullptr)
#endif
    {
        pszResult = getenv(pszKey);
    }

    if (pszResult == nullptr)
        return pszDefault;

    return pszResult;
}
