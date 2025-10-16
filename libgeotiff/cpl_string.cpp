/**********************************************************************
 *
 * Name:     cpl_string.cpp
 * Project:  CPL - Common Portability Library
 * Purpose:  String and Stringlist manipulation functions.
 * Author:   Daniel Morissette, danmo@videotron.ca
 *
 **********************************************************************
 * Copyright (c) 1998, Daniel Morissette
 * Copyright (c) 2008-2013, Even Rouault <even dot rouault at spatialys.com>
 *
 * SPDX-License-Identifier: MIT
 **********************************************************************
 *
 * Independent Security Audit 2003/04/04 Andrey Kiselev:
 *   Completed audit of this module. All functions may be used without buffer
 *   overflows and stack corruptions with any kind of input data strings with
 *   except of CPLSPrintf() and CSLAppendPrintf() (see note below).
 *
 * Security Audit 2003/03/28 warmerda:
 *   Completed security audit.  I believe that this module may be safely used
 *   to parse tokenize arbitrary input strings, assemble arbitrary sets of
 *   names values into string lists, unescape and escape text even if provided
 *   by a potentially hostile source.
 *
 *   CPLSPrintf() and CSLAppendPrintf() may not be safely invoked on
 *   arbitrary length inputs since it has a fixed size output buffer on system
 *   without vsnprintf().
 *
 **********************************************************************/

#undef WARN_STANDARD_PRINTF

#include "cpl_port.h"
#include "cpl_string.h"
#include "geo_tiffp.h"

#include <cctype>
#include <climits>
#include <cmath>
#include <cstdlib>
#include <cstring>

#include <limits>

#include "cpl_config.h"

#if !defined(va_copy) && defined(__va_copy)
#define va_copy __va_copy
#endif

/************************************************************************/
/*                           CSLFindString()                            */
/************************************************************************/

/**
 * Find a string within a string list (case insensitive).
 *
 * Returns the index of the entry in the string list that contains the
 * target string.  The string in the string list must be a full match for
 * the target, but the search is case insensitive.
 *
 * @param papszList the string list to be searched.
 * @param pszTarget the string to be searched for.
 *
 * @return the index of the string within the list or -1 on failure.
 */

int CSLFindString(CSLConstList papszList, const char *pszTarget)

{
    if (papszList == nullptr)
        return -1;

    for (int i = 0; papszList[i] != nullptr; ++i)
    {
        if (EQUAL(papszList[i], pszTarget))
            return i;
    }

    return -1;
}

/************************************************************************/
/*                           CSLPartialFindString()                     */
/************************************************************************/

/**
 * Find a substring within a string list.
 *
 * Returns the index of the entry in the string list that contains the
 * target string as a substring.  The search is case sensitive (unlike
 * CSLFindString()).
 *
 * @param papszHaystack the string list to be searched.
 * @param pszNeedle the substring to be searched for.
 *
 * @return the index of the string within the list or -1 on failure.
 */

int CSLPartialFindString(CSLConstList papszHaystack, const char *pszNeedle)
{
    if (papszHaystack == nullptr || pszNeedle == nullptr)
        return -1;

    for (int i = 0; papszHaystack[i] != nullptr; ++i)
    {
        if (strstr(papszHaystack[i], pszNeedle))
            return i;
    }

    return -1;
}

/**********************************************************************
 *                       CPLSPrintf()
 *
 * NOTE: This function should move to cpl_conv.cpp.
 **********************************************************************/

// For now, assume that a 8000 chars buffer will be enough.
constexpr int CPLSPrintf_BUF_SIZE = 8000;
constexpr int CPLSPrintf_BUF_Count = 10;

/** CPLSPrintf() that works with 10 static buffer.
 *
 * It returns a ref. to a static buffer that should not be freed and
 * is valid only until the next call to CPLSPrintf().
 */

const char *CPLSPrintf(CPL_FORMAT_STRING(const char *fmt), ...)
{
    static thread_local char* pachBufRingInfo = nullptr;

    va_list args;

    /* -------------------------------------------------------------------- */
    /*      Get the thread local buffer ring data.                          */
    /* -------------------------------------------------------------------- */
#ifdef MECHSOFT_DEVIATION
    char *pachBufRingInfo = static_cast<char *>(CPLGetTLS(CTLS_CPLSPRINTF));
#endif

    if (pachBufRingInfo == nullptr)
    {
        pachBufRingInfo = static_cast<char *>(CPLCalloc(
            1, sizeof(int) + CPLSPrintf_BUF_Count * CPLSPrintf_BUF_SIZE));
#ifdef MECHSOFT_DEVIATION
        CPLSetTLS(CTLS_CPLSPRINTF, pachBufRingInfo, TRUE);
#endif
    }

    /* -------------------------------------------------------------------- */
    /*      Work out which string in the "ring" we want to use this         */
    /*      time.                                                           */
    /* -------------------------------------------------------------------- */
    int *pnBufIndex = reinterpret_cast<int *>(pachBufRingInfo);
    const size_t nOffset = sizeof(int) + *pnBufIndex * CPLSPrintf_BUF_SIZE;
    char *pachBuffer = pachBufRingInfo + nOffset;

    *pnBufIndex = (*pnBufIndex + 1) % CPLSPrintf_BUF_Count;

    /* -------------------------------------------------------------------- */
    /*      Format the result.                                              */
    /* -------------------------------------------------------------------- */

    va_start(args, fmt);

    const int ret =
        CPLvsnprintf(pachBuffer, CPLSPrintf_BUF_SIZE - 1, fmt, args);
    if (ret < 0 || ret >= CPLSPrintf_BUF_SIZE - 1)
    {
        CPLError(CE_Failure, CPLE_AppDefined,
                 "CPLSPrintf() called with too "
                 "big string. Output will be truncated !");
    }

    va_end(args);

    return pachBuffer;
}

/************************************************************************/
/*                  CPLvsnprintf_get_end_of_formatting()                */
/************************************************************************/

static const char *CPLvsnprintf_get_end_of_formatting(const char *fmt)
{
    char ch = '\0';
    // Flag.
    for (; (ch = *fmt) != '\0'; ++fmt)
    {
        if (ch == '\'')
            continue;  // Bad idea as this is locale specific.
        if (ch == '-' || ch == '+' || ch == ' ' || ch == '#' || ch == '0')
            continue;
        break;
    }

    // Field width.
    for (; (ch = *fmt) != '\0'; ++fmt)
    {
        if (ch == '$')
            return nullptr;  // Do not support this.
        if (*fmt >= '0' && *fmt <= '9')
            continue;
        break;
    }

    // Precision.
    if (ch == '.')
    {
        ++fmt;
        for (; (ch = *fmt) != '\0'; ++fmt)
        {
            if (ch == '$')
                return nullptr;  // Do not support this.
            if (*fmt >= '0' && *fmt <= '9')
                continue;
            break;
        }
    }

    // Length modifier.
    for (; (ch = *fmt) != '\0'; ++fmt)
    {
        if (ch == 'h' || ch == 'l' || ch == 'j' || ch == 'z' || ch == 't' ||
            ch == 'L')
            continue;
        else if (ch == 'I' && fmt[1] == '6' && fmt[2] == '4')
            fmt += 2;
        else
            return fmt;
    }

    return nullptr;
}

/************************************************************************/
/*                           CPLvsnprintf()                             */
/************************************************************************/

#define call_native_snprintf(type)                                             \
    local_ret = snprintf(str + offset_out, size - offset_out, localfmt,        \
                         va_arg(wrk_args, type))

/** vsnprintf() wrapper that is not sensitive to LC_NUMERIC settings.
 *
 * This function has the same contract as standard vsnprintf(), except that
 * formatting of floating-point numbers will use decimal point, whatever the
 * current locale is set.
 *
 * @param str output buffer
 * @param size size of the output buffer (including space for terminating nul)
 * @param fmt formatting string
 * @param args arguments
 * @return the number of characters (excluding terminating nul) that would be
 * written if size is big enough. Or potentially -1 with Microsoft C runtime
 * for Visual Studio < 2015.
 * @since GDAL 2.0
 */
int CPLvsnprintf(char *str, size_t size, CPL_FORMAT_STRING(const char *fmt),
                 va_list args)
{
    if (size == 0)
        return vsnprintf(str, size, fmt, args);

    va_list wrk_args;

#ifdef va_copy
    va_copy(wrk_args, args);
#else
    wrk_args = args;
#endif

    const char *fmt_ori = fmt;
    size_t offset_out = 0;
    char ch = '\0';
    bool bFormatUnknown = false;

    for (; (ch = *fmt) != '\0'; ++fmt)
    {
        if (ch == '%')
        {
            if (strncmp(fmt, "%.*f", 4) == 0)
            {
                const int precision = va_arg(wrk_args, int);
                const double val = va_arg(wrk_args, double);
                const int local_ret =
                    snprintf(str + offset_out, size - offset_out, "%.*f",
                             precision, val);
                // MSVC vsnprintf() returns -1.
                if (local_ret < 0 || offset_out + local_ret >= size)
                    break;
                for (int j = 0; j < local_ret; ++j)
                {
                    if (str[offset_out + j] == ',')
                    {
                        str[offset_out + j] = '.';
                        break;
                    }
                }
                offset_out += local_ret;
                fmt += strlen("%.*f") - 1;
                continue;
            }

            const char *ptrend = CPLvsnprintf_get_end_of_formatting(fmt + 1);
            if (ptrend == nullptr || ptrend - fmt >= 20)
            {
                bFormatUnknown = true;
                break;
            }
            char end = *ptrend;
            char end_m1 = ptrend[-1];

            char localfmt[22] = {};
            memcpy(localfmt, fmt, ptrend - fmt + 1);
            localfmt[ptrend - fmt + 1] = '\0';

            int local_ret = 0;
            if (end == '%')
            {
                if (offset_out == size - 1)
                    break;
                local_ret = 1;
                str[offset_out] = '%';
            }
            else if (end == 'd' || end == 'i' || end == 'c')
            {
                if (end_m1 == 'h')
                    call_native_snprintf(int);
                else if (end_m1 == 'l' && ptrend[-2] != 'l')
                    call_native_snprintf(long);
                else if (end_m1 == 'l' && ptrend[-2] == 'l')
                    call_native_snprintf(GIntBig);
                else if (end_m1 == '4' && ptrend[-2] == '6' &&
                         ptrend[-3] == 'I')
                    // Microsoft I64 modifier.
                    call_native_snprintf(GIntBig);
                else if (end_m1 == 'z')
                    call_native_snprintf(size_t);
                else if ((end_m1 >= 'a' && end_m1 <= 'z') ||
                         (end_m1 >= 'A' && end_m1 <= 'Z'))
                {
                    bFormatUnknown = true;
                    break;
                }
                else
                    call_native_snprintf(int);
            }
            else if (end == 'o' || end == 'u' || end == 'x' || end == 'X')
            {
                if (end_m1 == 'h')
                    call_native_snprintf(unsigned int);
                else if (end_m1 == 'l' && ptrend[-2] != 'l')
                    call_native_snprintf(unsigned long);
                else if (end_m1 == 'l' && ptrend[-2] == 'l')
                    call_native_snprintf(GUIntBig);
                else if (end_m1 == '4' && ptrend[-2] == '6' &&
                         ptrend[-3] == 'I')
                    // Microsoft I64 modifier.
                    call_native_snprintf(GUIntBig);
                else if (end_m1 == 'z')
                    call_native_snprintf(size_t);
                else if ((end_m1 >= 'a' && end_m1 <= 'z') ||
                         (end_m1 >= 'A' && end_m1 <= 'Z'))
                {
                    bFormatUnknown = true;
                    break;
                }
                else
                    call_native_snprintf(unsigned int);
            }
            else if (end == 'e' || end == 'E' || end == 'f' || end == 'F' ||
                     end == 'g' || end == 'G' || end == 'a' || end == 'A')
            {
                if (end_m1 == 'L')
                    call_native_snprintf(long double);
                else
                    call_native_snprintf(double);
                // MSVC vsnprintf() returns -1.
                if (local_ret < 0 || offset_out + local_ret >= size)
                    break;
                for (int j = 0; j < local_ret; ++j)
                {
                    if (str[offset_out + j] == ',')
                    {
                        str[offset_out + j] = '.';
                        break;
                    }
                }
            }
            else if (end == 's')
            {
                const char *pszPtr = va_arg(wrk_args, const char *);
                CPLAssert(pszPtr);
                local_ret = snprintf(str + offset_out, size - offset_out,
                                     localfmt, pszPtr);
            }
            else if (end == 'p')
            {
                call_native_snprintf(void *);
            }
            else
            {
                bFormatUnknown = true;
                break;
            }
            // MSVC vsnprintf() returns -1.
            if (local_ret < 0 || offset_out + local_ret >= size)
                break;
            offset_out += local_ret;
            fmt = ptrend;
        }
        else
        {
            if (offset_out == size - 1)
                break;
            str[offset_out++] = *fmt;
        }
    }
    if (ch == '\0' && offset_out < size)
        str[offset_out] = '\0';
    else
    {
        if (bFormatUnknown)
        {
            CPLDebug("CPL",
                     "CPLvsnprintf() called with unsupported "
                     "formatting string: %s",
                     fmt_ori);
        }
#ifdef va_copy
        va_end(wrk_args);
        va_copy(wrk_args, args);
#else
        wrk_args = args;
#endif
#if defined(HAVE_VSNPRINTF)
        offset_out = vsnprintf(str, size, fmt_ori, wrk_args);
#else
        offset_out = vsprintf(str, fmt_ori, wrk_args);
#endif
    }

#ifdef va_copy
    va_end(wrk_args);
#endif

    return static_cast<int>(offset_out);
}

/************************************************************************/
/*                           CPLsnprintf()                              */
/************************************************************************/

#if !defined(ALIAS_CPLSNPRINTF_AS_SNPRINTF)

#if defined(__clang__) && __clang_major__ == 3 && __clang_minor__ <= 2
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunknown-pragmas"
#pragma clang diagnostic ignored "-Wdocumentation"
#endif

/** snprintf() wrapper that is not sensitive to LC_NUMERIC settings.
 *
 * This function has the same contract as standard snprintf(), except that
 * formatting of floating-point numbers will use decimal point, whatever the
 * current locale is set.
 *
 * @param str output buffer
 * @param size size of the output buffer (including space for terminating nul)
 * @param fmt formatting string
 * @param ... arguments
 * @return the number of characters (excluding terminating nul) that would be
 * written if size is big enough. Or potentially -1 with Microsoft C runtime
 * for Visual Studio < 2015.
 * @since GDAL 2.0
 */

int CPLsnprintf(char *str, size_t size, CPL_FORMAT_STRING(const char *fmt), ...)
{
    va_list args;

    va_start(args, fmt);
    const int ret = CPLvsnprintf(str, size, fmt, args);
    va_end(args);
    return ret;
}

#endif  //  !defined(ALIAS_CPLSNPRINTF_AS_SNPRINTF)

/************************************************************************/
/*                           CPLsprintf()                               */
/************************************************************************/

/** sprintf() wrapper that is not sensitive to LC_NUMERIC settings.
  *
  * This function has the same contract as standard sprintf(), except that
  * formatting of floating-point numbers will use decimal point, whatever the
  * current locale is set.
  *
  * @param str output buffer (must be large enough to hold the result)
  * @param fmt formatting string
  * @param ... arguments
  * @return the number of characters (excluding terminating nul) written in
` * output buffer.
  * @since GDAL 2.0
  */
int CPLsprintf(char *str, CPL_FORMAT_STRING(const char *fmt), ...)
{
    va_list args;

    va_start(args, fmt);
    const int ret = CPLvsnprintf(str, INT_MAX, fmt, args);
    va_end(args);
    return ret;
}

/************************************************************************/
/*                           CPLprintf()                                */
/************************************************************************/

/** printf() wrapper that is not sensitive to LC_NUMERIC settings.
 *
 * This function has the same contract as standard printf(), except that
 * formatting of floating-point numbers will use decimal point, whatever the
 * current locale is set.
 *
 * @param fmt formatting string
 * @param ... arguments
 * @return the number of characters (excluding terminating nul) written in
 * output buffer.
 * @since GDAL 2.0
 */
int CPLprintf(CPL_FORMAT_STRING(const char *fmt), ...)
{
    va_list wrk_args, args;

    va_start(args, fmt);

#ifdef va_copy
    va_copy(wrk_args, args);
#else
    wrk_args = args;
#endif

    char szBuffer[4096] = {};
    // Quiet coverity by staring off nul terminated.
    int ret = CPLvsnprintf(szBuffer, sizeof(szBuffer), fmt, wrk_args);

#ifdef va_copy
    va_end(wrk_args);
#endif

    if (ret < int(sizeof(szBuffer)) - 1)
        ret = printf("%s", szBuffer); /*ok*/
    else
    {
#ifdef va_copy
        va_copy(wrk_args, args);
#else
        wrk_args = args;
#endif

        ret = vfprintf(stdout, fmt, wrk_args);

#ifdef va_copy
        va_end(wrk_args);
#endif
    }

    va_end(args);

    return ret;
}

/************************************************************************/
/*                            CSLFindName()                             */
/************************************************************************/

/**
 * Find StringList entry with given key name.
 *
 * @param papszStrList the string list to search.
 * @param pszName the key value to look for (case insensitive).
 *
 * @return -1 on failure or the list index of the first occurrence
 * matching the given key.
 */

int CSLFindName(CSLConstList papszStrList, const char *pszName)
{
    if (papszStrList == nullptr || pszName == nullptr)
        return -1;

    const size_t nLen = strlen(pszName);
    int iIndex = 0;
    while (*papszStrList != nullptr)
    {
        if (EQUALN(*papszStrList, pszName, nLen) &&
            ((*papszStrList)[nLen] == '=' || (*papszStrList)[nLen] == ':'))
        {
            return iIndex;
        }
        ++iIndex;
        ++papszStrList;
    }
    return -1;
}

/************************************************************************/
/*                         CPLTestBool()                                */
/************************************************************************/

/**
 * Test what boolean value contained in the string.
 *
 * If pszValue is "NO", "FALSE", "OFF" or "0" will be returned false.
 * Otherwise, true will be returned.
 *
 * @param pszValue the string should be tested.
 *
 * @return true or false.
 */

bool CPLTestBool(const char *pszValue)
{
    return !(EQUAL(pszValue, "NO") || EQUAL(pszValue, "FALSE") ||
             EQUAL(pszValue, "OFF") || EQUAL(pszValue, "0"));
}

/************************************************************************/
/*                     CSLFetchNameValueDefaulted()                     */
/************************************************************************/

/** Same as CSLFetchNameValue() but return pszDefault in case of no match */
const char *CSLFetchNameValueDef(CSLConstList papszStrList, const char *pszName,
                                 const char *pszDefault)

{
    const char *pszResult = CSLFetchNameValue(papszStrList, pszName);
    if (pszResult != nullptr)
        return pszResult;

    return pszDefault;
}

/**********************************************************************
 *                       CSLFetchNameValue()
 **********************************************************************/

/** In a StringList of "Name=Value" pairs, look for the
 * first value associated with the specified name.  The search is not
 * case sensitive.
 * ("Name:Value" pairs are also supported for backward compatibility
 * with older stuff.)
 *
 * Returns a reference to the value in the StringList that the caller
 * should not attempt to free.
 *
 * Returns NULL if the name is not found.
 */

const char *CSLFetchNameValue(CSLConstList papszStrList, const char *pszName)
{
    if (papszStrList == nullptr || pszName == nullptr)
        return nullptr;

    const size_t nLen = strlen(pszName);
    while (*papszStrList != nullptr)
    {
        if (EQUALN(*papszStrList, pszName, nLen) &&
            ((*papszStrList)[nLen] == '=' || (*papszStrList)[nLen] == ':'))
        {
            return (*papszStrList) + nLen + 1;
        }
        ++papszStrList;
    }
    return nullptr;
}

/************************************************************************/
/*                          CPLEscapeString()                           */
/************************************************************************/

/**
 * Apply escaping to string to preserve special characters.
 *
 * This function will "escape" a variety of special characters
 * to make the string suitable to embed within a string constant
 * or to write within a text stream but in a form that can be
 * reconstituted to its original form.  The escaping will even preserve
 * zero bytes allowing preservation of raw binary data.
 *
 * CPLES_BackslashQuotable(0): This scheme turns a binary string into
 * a form suitable to be placed within double quotes as a string constant.
 * The backslash, quote, '\\0' and newline characters are all escaped in
 * the usual C style.
 *
 * CPLES_XML(1): This scheme converts the '<', '>', '"' and '&' characters into
 * their XML/HTML equivalent (&lt;, &gt;, &quot; and &amp;) making a string safe
 * to embed as CDATA within an XML element.  The '\\0' is not escaped and
 * should not be included in the input.
 *
 * CPLES_URL(2): Everything except alphanumerics and the characters
 * '$', '-', '_', '.', '+', '!', '*', ''', '(', ')' and ',' (see RFC1738) are
 * converted to a percent followed by a two digit hex encoding of the character
 * (leading zero supplied if needed).  This is the mechanism used for encoding
 * values to be passed in URLs.
 *
 * CPLES_SQL(3): All single quotes are replaced with two single quotes.
 * Suitable for use when constructing literal values for SQL commands where
 * the literal will be enclosed in single quotes.
 *
 * CPLES_CSV(4): If the values contains commas, semicolons, tabs, double quotes,
 * or newlines it placed in double quotes, and double quotes in the value are
 * doubled. Suitable for use when constructing field values for .csv files.
 * Note that CPLUnescapeString() currently does not support this format, only
 * CPLEscapeString().  See cpl_csv.cpp for CSV parsing support.
 *
 * CPLES_SQLI(7): All double quotes are replaced with two double quotes.
 * Suitable for use when constructing identifiers for SQL commands where
 * the literal will be enclosed in double quotes.
 *
 * @param pszInput the string to escape.
 * @param nLength The number of bytes of data to preserve.  If this is -1
 * the strlen(pszString) function will be used to compute the length.
 * @param nScheme the encoding scheme to use.
 *
 * @return an escaped, zero terminated string that should be freed with
 * CPLFree() when no longer needed.
 */

char *CPLEscapeString(const char *pszInput, int nLength, int nScheme)
{
    const size_t szLength =
        (nLength < 0) ? strlen(pszInput) : static_cast<size_t>(nLength);
#define nLength no_longer_use_me

    size_t nSizeAlloc = 1;
#if SIZEOF_VOIDP < 8
    bool bWrapAround = false;
    const auto IncSizeAlloc = [&nSizeAlloc, &bWrapAround](size_t inc)
    {
        constexpr size_t SZ_MAX = std::numeric_limits<size_t>::max();
        if (nSizeAlloc > SZ_MAX - inc)
        {
            bWrapAround = true;
            nSizeAlloc = 0;
        }
        nSizeAlloc += inc;
    };
#else
    const auto IncSizeAlloc = [&nSizeAlloc](size_t inc) { nSizeAlloc += inc; };
#endif

    if (nScheme == CPLES_BackslashQuotable)
    {
        for (size_t iIn = 0; iIn < szLength; iIn++)
        {
            if (pszInput[iIn] == '\0' || pszInput[iIn] == '\n' ||
                pszInput[iIn] == '"' || pszInput[iIn] == '\\')
                IncSizeAlloc(2);
            else
                IncSizeAlloc(1);
        }
    }
    else if (nScheme == CPLES_XML || nScheme == CPLES_XML_BUT_QUOTES)
    {
        for (size_t iIn = 0; iIn < szLength; ++iIn)
        {
            if (pszInput[iIn] == '<')
            {
                IncSizeAlloc(4);
            }
            else if (pszInput[iIn] == '>')
            {
                IncSizeAlloc(4);
            }
            else if (pszInput[iIn] == '&')
            {
                IncSizeAlloc(5);
            }
            else if (pszInput[iIn] == '"' && nScheme != CPLES_XML_BUT_QUOTES)
            {
                IncSizeAlloc(6);
            }
            // Python 2 does not display the UTF-8 character corresponding
            // to the byte-order mark (BOM), so escape it.
            else if ((reinterpret_cast<const unsigned char *>(pszInput))[iIn] ==
                         0xEF &&
                     (reinterpret_cast<const unsigned char *>(
                         pszInput))[iIn + 1] == 0xBB &&
                     (reinterpret_cast<const unsigned char *>(
                         pszInput))[iIn + 2] == 0xBF)
            {
                IncSizeAlloc(8);
                iIn += 2;
            }
            else if ((reinterpret_cast<const unsigned char *>(pszInput))[iIn] <
                         0x20 &&
                     pszInput[iIn] != 0x9 && pszInput[iIn] != 0xA &&
                     pszInput[iIn] != 0xD)
            {
                // These control characters are unrepresentable in XML format,
                // so we just drop them.  #4117
            }
            else
            {
                IncSizeAlloc(1);
            }
        }
    }
    else if (nScheme == CPLES_URL)  // Untested at implementation.
    {
        for (size_t iIn = 0; iIn < szLength; ++iIn)
        {
            if ((pszInput[iIn] >= 'a' && pszInput[iIn] <= 'z') ||
                (pszInput[iIn] >= 'A' && pszInput[iIn] <= 'Z') ||
                (pszInput[iIn] >= '0' && pszInput[iIn] <= '9') ||
                pszInput[iIn] == '$' || pszInput[iIn] == '-' ||
                pszInput[iIn] == '_' || pszInput[iIn] == '.' ||
                pszInput[iIn] == '+' || pszInput[iIn] == '!' ||
                pszInput[iIn] == '*' || pszInput[iIn] == '\'' ||
                pszInput[iIn] == '(' || pszInput[iIn] == ')' ||
                pszInput[iIn] == ',')
            {
                IncSizeAlloc(1);
            }
            else
            {
                IncSizeAlloc(3);
            }
        }
    }
    else if (nScheme == CPLES_SQL || nScheme == CPLES_SQLI)
    {
        const char chQuote = nScheme == CPLES_SQL ? '\'' : '\"';
        for (size_t iIn = 0; iIn < szLength; ++iIn)
        {
            if (pszInput[iIn] == chQuote)
            {
                IncSizeAlloc(2);
            }
            else
            {
                IncSizeAlloc(1);
            }
        }
    }
    else if (nScheme == CPLES_CSV || nScheme == CPLES_CSV_FORCE_QUOTING)
    {
        if (nScheme == CPLES_CSV && strcspn(pszInput, "\",;\t\n\r") == szLength)
        {
            char *pszOutput =
                static_cast<char *>(VSI_MALLOC_VERBOSE(szLength + 1));
            if (pszOutput == nullptr)
                return nullptr;
            memcpy(pszOutput, pszInput, szLength + 1);
            return pszOutput;
        }
        else
        {
            IncSizeAlloc(1);
            for (size_t iIn = 0; iIn < szLength; ++iIn)
            {
                if (pszInput[iIn] == '\"')
                {
                    IncSizeAlloc(2);
                }
                else
                    IncSizeAlloc(1);
            }
            IncSizeAlloc(1);
        }
    }
    else
    {
        CPLError(CE_Failure, CPLE_AppDefined,
                 "Undefined escaping scheme (%d) in CPLEscapeString()",
                 nScheme);
        return CPLStrdup("");
    }

#if SIZEOF_VOIDP < 8
    if (bWrapAround)
    {
        CPLError(CE_Failure, CPLE_OutOfMemory,
                 "Out of memory in CPLEscapeString()");
        return nullptr;
    }
#endif

    char *pszOutput = static_cast<char *>(VSI_MALLOC_VERBOSE(nSizeAlloc));
    if (pszOutput == nullptr)
        return nullptr;

    size_t iOut = 0;

    if (nScheme == CPLES_BackslashQuotable)
    {
        for (size_t iIn = 0; iIn < szLength; iIn++)
        {
            if (pszInput[iIn] == '\0')
            {
                pszOutput[iOut++] = '\\';
                pszOutput[iOut++] = '0';
            }
            else if (pszInput[iIn] == '\n')
            {
                pszOutput[iOut++] = '\\';
                pszOutput[iOut++] = 'n';
            }
            else if (pszInput[iIn] == '"')
            {
                pszOutput[iOut++] = '\\';
                pszOutput[iOut++] = '\"';
            }
            else if (pszInput[iIn] == '\\')
            {
                pszOutput[iOut++] = '\\';
                pszOutput[iOut++] = '\\';
            }
            else
                pszOutput[iOut++] = pszInput[iIn];
        }
        pszOutput[iOut++] = '\0';
    }
    else if (nScheme == CPLES_XML || nScheme == CPLES_XML_BUT_QUOTES)
    {
        for (size_t iIn = 0; iIn < szLength; ++iIn)
        {
            if (pszInput[iIn] == '<')
            {
                pszOutput[iOut++] = '&';
                pszOutput[iOut++] = 'l';
                pszOutput[iOut++] = 't';
                pszOutput[iOut++] = ';';
            }
            else if (pszInput[iIn] == '>')
            {
                pszOutput[iOut++] = '&';
                pszOutput[iOut++] = 'g';
                pszOutput[iOut++] = 't';
                pszOutput[iOut++] = ';';
            }
            else if (pszInput[iIn] == '&')
            {
                pszOutput[iOut++] = '&';
                pszOutput[iOut++] = 'a';
                pszOutput[iOut++] = 'm';
                pszOutput[iOut++] = 'p';
                pszOutput[iOut++] = ';';
            }
            else if (pszInput[iIn] == '"' && nScheme != CPLES_XML_BUT_QUOTES)
            {
                pszOutput[iOut++] = '&';
                pszOutput[iOut++] = 'q';
                pszOutput[iOut++] = 'u';
                pszOutput[iOut++] = 'o';
                pszOutput[iOut++] = 't';
                pszOutput[iOut++] = ';';
            }
            // Python 2 does not display the UTF-8 character corresponding
            // to the byte-order mark (BOM), so escape it.
            else if ((reinterpret_cast<const unsigned char *>(pszInput))[iIn] ==
                         0xEF &&
                     (reinterpret_cast<const unsigned char *>(
                         pszInput))[iIn + 1] == 0xBB &&
                     (reinterpret_cast<const unsigned char *>(
                         pszInput))[iIn + 2] == 0xBF)
            {
                pszOutput[iOut++] = '&';
                pszOutput[iOut++] = '#';
                pszOutput[iOut++] = 'x';
                pszOutput[iOut++] = 'F';
                pszOutput[iOut++] = 'E';
                pszOutput[iOut++] = 'F';
                pszOutput[iOut++] = 'F';
                pszOutput[iOut++] = ';';
                iIn += 2;
            }
            else if ((reinterpret_cast<const unsigned char *>(pszInput))[iIn] <
                         0x20 &&
                     pszInput[iIn] != 0x9 && pszInput[iIn] != 0xA &&
                     pszInput[iIn] != 0xD)
            {
                // These control characters are unrepresentable in XML format,
                // so we just drop them.  #4117
            }
            else
            {
                pszOutput[iOut++] = pszInput[iIn];
            }
        }
        pszOutput[iOut++] = '\0';
    }
    else if (nScheme == CPLES_URL)  // Untested at implementation.
    {
        for (size_t iIn = 0; iIn < szLength; ++iIn)
        {
            if ((pszInput[iIn] >= 'a' && pszInput[iIn] <= 'z') ||
                (pszInput[iIn] >= 'A' && pszInput[iIn] <= 'Z') ||
                (pszInput[iIn] >= '0' && pszInput[iIn] <= '9') ||
                pszInput[iIn] == '$' || pszInput[iIn] == '-' ||
                pszInput[iIn] == '_' || pszInput[iIn] == '.' ||
                pszInput[iIn] == '+' || pszInput[iIn] == '!' ||
                pszInput[iIn] == '*' || pszInput[iIn] == '\'' ||
                pszInput[iIn] == '(' || pszInput[iIn] == ')' ||
                pszInput[iIn] == ',')
            {
                pszOutput[iOut++] = pszInput[iIn];
            }
            else
            {
                snprintf(pszOutput + iOut, nSizeAlloc - iOut, "%%%02X",
                         static_cast<unsigned char>(pszInput[iIn]));
                iOut += 3;
            }
        }
        pszOutput[iOut++] = '\0';
    }
    else if (nScheme == CPLES_SQL || nScheme == CPLES_SQLI)
    {
        const char chQuote = nScheme == CPLES_SQL ? '\'' : '\"';
        for (size_t iIn = 0; iIn < szLength; ++iIn)
        {
            if (pszInput[iIn] == chQuote)
            {
                pszOutput[iOut++] = chQuote;
                pszOutput[iOut++] = chQuote;
            }
            else
            {
                pszOutput[iOut++] = pszInput[iIn];
            }
        }
        pszOutput[iOut++] = '\0';
    }
    else if (nScheme == CPLES_CSV || nScheme == CPLES_CSV_FORCE_QUOTING)
    {
        pszOutput[iOut++] = '\"';

        for (size_t iIn = 0; iIn < szLength; ++iIn)
        {
            if (pszInput[iIn] == '\"')
            {
                pszOutput[iOut++] = '\"';
                pszOutput[iOut++] = '\"';
            }
            else
                pszOutput[iOut++] = pszInput[iIn];
        }
        pszOutput[iOut++] = '\"';
        pszOutput[iOut++] = '\0';
    }

    return pszOutput;
#undef nLength
}

/************************************************************************/
/*                         CPLUnescapeString()                          */
/************************************************************************/

/**
 * Unescape a string.
 *
 * This function does the opposite of CPLEscapeString().  Given a string
 * with special values escaped according to some scheme, it will return a
 * new copy of the string returned to its original form.
 *
 * @param pszInput the input string.  This is a zero terminated string.
 * @param pnLength location to return the length of the unescaped string,
 * which may in some cases include embedded '\\0' characters.
 * @param nScheme the escaped scheme to undo (see CPLEscapeString() for a
 * list).  Does not yet support CSV.
 *
 * @return a copy of the unescaped string that should be freed by the
 * application using CPLFree() when no longer needed.
 */

CPL_NOSANITIZE_UNSIGNED_INT_OVERFLOW
char *CPLUnescapeString(const char *pszInput, int *pnLength, int nScheme)

{
    int iOut = 0;

    // TODO: Why times 4?
    char *pszOutput = static_cast<char *>(CPLMalloc(4 * strlen(pszInput) + 1));
    pszOutput[0] = '\0';

    if (nScheme == CPLES_BackslashQuotable)
    {
        for (int iIn = 0; pszInput[iIn] != '\0'; ++iIn)
        {
            if (pszInput[iIn] == '\\')
            {
                ++iIn;
                if (pszInput[iIn] == '\0')
                    break;
                if (pszInput[iIn] == 'n')
                    pszOutput[iOut++] = '\n';
                else if (pszInput[iIn] == '0')
                    pszOutput[iOut++] = '\0';
                else
                    pszOutput[iOut++] = pszInput[iIn];
            }
            else
            {
                pszOutput[iOut++] = pszInput[iIn];
            }
        }
    }
    else if (nScheme == CPLES_XML || nScheme == CPLES_XML_BUT_QUOTES)
    {
        char ch = '\0';
        for (int iIn = 0; (ch = pszInput[iIn]) != '\0'; ++iIn)
        {
            if (ch != '&')
            {
                pszOutput[iOut++] = ch;
            }
            else if (STARTS_WITH_CI(pszInput + iIn, "&lt;"))
            {
                pszOutput[iOut++] = '<';
                iIn += 3;
            }
            else if (STARTS_WITH_CI(pszInput + iIn, "&gt;"))
            {
                pszOutput[iOut++] = '>';
                iIn += 3;
            }
            else if (STARTS_WITH_CI(pszInput + iIn, "&amp;"))
            {
                pszOutput[iOut++] = '&';
                iIn += 4;
            }
            else if (STARTS_WITH_CI(pszInput + iIn, "&apos;"))
            {
                pszOutput[iOut++] = '\'';
                iIn += 5;
            }
            else if (STARTS_WITH_CI(pszInput + iIn, "&quot;"))
            {
                pszOutput[iOut++] = '"';
                iIn += 5;
            }
            else if (STARTS_WITH_CI(pszInput + iIn, "&#x"))
            {
                wchar_t anVal[2] = {0, 0};
                iIn += 3;

                unsigned int nVal = 0;
                while (true)
                {
                    ch = pszInput[iIn++];
                    if (ch >= 'a' && ch <= 'f')
                        nVal = nVal * 16U +
                               static_cast<unsigned int>(ch - 'a' + 10);
                    else if (ch >= 'A' && ch <= 'F')
                        nVal = nVal * 16U +
                               static_cast<unsigned int>(ch - 'A' + 10);
                    else if (ch >= '0' && ch <= '9')
                        nVal = nVal * 16U + static_cast<unsigned int>(ch - '0');
                    else
                        break;
                }
                anVal[0] = static_cast<wchar_t>(nVal);
                if (ch != ';')
                    break;
                iIn--;

                char *pszUTF8 =
                    CPLRecodeFromWChar(anVal, "WCHAR_T", CPL_ENC_UTF8);
                int nLen = static_cast<int>(strlen(pszUTF8));
                memcpy(pszOutput + iOut, pszUTF8, nLen);
                CPLFree(pszUTF8);
                iOut += nLen;
            }
            else if (STARTS_WITH_CI(pszInput + iIn, "&#"))
            {
                wchar_t anVal[2] = {0, 0};
                iIn += 2;

                unsigned int nVal = 0;
                while (true)
                {
                    ch = pszInput[iIn++];
                    if (ch >= '0' && ch <= '9')
                        nVal = nVal * 10U + static_cast<unsigned int>(ch - '0');
                    else
                        break;
                }
                anVal[0] = static_cast<wchar_t>(nVal);
                if (ch != ';')
                    break;
                iIn--;

                char *pszUTF8 =
                    CPLRecodeFromWChar(anVal, "WCHAR_T", CPL_ENC_UTF8);
                const int nLen = static_cast<int>(strlen(pszUTF8));
                memcpy(pszOutput + iOut, pszUTF8, nLen);
                CPLFree(pszUTF8);
                iOut += nLen;
            }
            else
            {
                // Illegal escape sequence.
                CPLDebug("CPL",
                         "Error unescaping CPLES_XML text, '&' character "
                         "followed by unhandled escape sequence.");
                break;
            }
        }
    }
    else if (nScheme == CPLES_URL)
    {
        for (int iIn = 0; pszInput[iIn] != '\0'; ++iIn)
        {
            if (pszInput[iIn] == '%' && pszInput[iIn + 1] != '\0' &&
                pszInput[iIn + 2] != '\0')
            {
                int nHexChar = 0;

                if (pszInput[iIn + 1] >= 'A' && pszInput[iIn + 1] <= 'F')
                    nHexChar += 16 * (pszInput[iIn + 1] - 'A' + 10);
                else if (pszInput[iIn + 1] >= 'a' && pszInput[iIn + 1] <= 'f')
                    nHexChar += 16 * (pszInput[iIn + 1] - 'a' + 10);
                else if (pszInput[iIn + 1] >= '0' && pszInput[iIn + 1] <= '9')
                    nHexChar += 16 * (pszInput[iIn + 1] - '0');
                else
                    CPLDebug("CPL",
                             "Error unescaping CPLES_URL text, percent not "
                             "followed by two hex digits.");

                if (pszInput[iIn + 2] >= 'A' && pszInput[iIn + 2] <= 'F')
                    nHexChar += pszInput[iIn + 2] - 'A' + 10;
                else if (pszInput[iIn + 2] >= 'a' && pszInput[iIn + 2] <= 'f')
                    nHexChar += pszInput[iIn + 2] - 'a' + 10;
                else if (pszInput[iIn + 2] >= '0' && pszInput[iIn + 2] <= '9')
                    nHexChar += pszInput[iIn + 2] - '0';
                else
                    CPLDebug("CPL",
                             "Error unescaping CPLES_URL text, percent not "
                             "followed by two hex digits.");

                pszOutput[iOut++] = static_cast<char>(nHexChar);
                iIn += 2;
            }
            else if (pszInput[iIn] == '+')
            {
                pszOutput[iOut++] = ' ';
            }
            else
            {
                pszOutput[iOut++] = pszInput[iIn];
            }
        }
    }
    else if (nScheme == CPLES_SQL || nScheme == CPLES_SQLI)
    {
        char szQuote = nScheme == CPLES_SQL ? '\'' : '\"';
        for (int iIn = 0; pszInput[iIn] != '\0'; ++iIn)
        {
            if (pszInput[iIn] == szQuote && pszInput[iIn + 1] == szQuote)
            {
                ++iIn;
                pszOutput[iOut++] = pszInput[iIn];
            }
            else
            {
                pszOutput[iOut++] = pszInput[iIn];
            }
        }
    }
    else if (nScheme == CPLES_CSV)
    {
        CPLError(CE_Fatal, CPLE_NotSupported,
                 "CSV Unescaping not yet implemented.");
    }
    else
    {
        CPLError(CE_Fatal, CPLE_NotSupported, "Unknown escaping style.");
    }

    pszOutput[iOut] = '\0';

    if (pnLength != nullptr)
        *pnLength = iOut;

    return pszOutput;
}

/************************************************************************/
/*                              CPLStrlcpy()                            */
/************************************************************************/

/**
 * Copy source string to a destination buffer.
 *
 * This function ensures that the destination buffer is always NUL terminated
 * (provided that its length is at least 1).
 *
 * This function is designed to be a safer, more consistent, and less error
 * prone replacement for strncpy. Its contract is identical to libbsd's strlcpy.
 *
 * Truncation can be detected by testing if the return value of CPLStrlcpy
 * is greater or equal to nDestSize.

\verbatim
char szDest[5] = {};
if( CPLStrlcpy(szDest, "abcde", sizeof(szDest)) >= sizeof(szDest) )
    fprintf(stderr, "truncation occurred !\n");
\endverbatim

 * @param pszDest   destination buffer
 * @param pszSrc    source string. Must be NUL terminated
 * @param nDestSize size of destination buffer (including space for the NUL
 *     terminator character)
 *
 * @return the length of the source string (=strlen(pszSrc))
 *
 * @since GDAL 1.7.0
 */
size_t CPLStrlcpy(char *pszDest, const char *pszSrc, size_t nDestSize)
{
    if (nDestSize == 0)
        return strlen(pszSrc);

    char *pszDestIter = pszDest;
    const char *pszSrcIter = pszSrc;

    --nDestSize;
    while (nDestSize != 0 && *pszSrcIter != '\0')
    {
        *pszDestIter = *pszSrcIter;
        ++pszDestIter;
        ++pszSrcIter;
        --nDestSize;
    }
    *pszDestIter = '\0';
    return pszSrcIter - pszSrc + strlen(pszSrcIter);
}

/************************************************************************/
/*                              CPLStrlcat()                            */
/************************************************************************/

/**
 * Appends a source string to a destination buffer.
 *
 * This function ensures that the destination buffer is always NUL terminated
 * (provided that its length is at least 1 and that there is at least one byte
 * free in pszDest, that is to say strlen(pszDest_before) < nDestSize)
 *
 * This function is designed to be a safer, more consistent, and less error
 * prone replacement for strncat. Its contract is identical to libbsd's strlcat.
 *
 * Truncation can be detected by testing if the return value of CPLStrlcat
 * is greater or equal to nDestSize.

\verbatim
char szDest[5] = {};
CPLStrlcpy(szDest, "ab", sizeof(szDest));
if( CPLStrlcat(szDest, "cde", sizeof(szDest)) >= sizeof(szDest) )
    fprintf(stderr, "truncation occurred !\n");
\endverbatim

 * @param pszDest   destination buffer. Must be NUL terminated before
 *         running CPLStrlcat
 * @param pszSrc    source string. Must be NUL terminated
 * @param nDestSize size of destination buffer (including space for the
 *         NUL terminator character)
 *
 * @return the theoretical length of the destination string after concatenation
 *         (=strlen(pszDest_before) + strlen(pszSrc)).
 *         If strlen(pszDest_before) >= nDestSize, then it returns
 *         nDestSize + strlen(pszSrc)
 *
 * @since GDAL 1.7.0
 */
size_t CPLStrlcat(char *pszDest, const char *pszSrc, size_t nDestSize)
{
    char *pszDestIter = pszDest;

    while (nDestSize != 0 && *pszDestIter != '\0')
    {
        ++pszDestIter;
        --nDestSize;
    }

    return pszDestIter - pszDest + CPLStrlcpy(pszDestIter, pszSrc, nDestSize);
}

/************************************************************************/
/*                              CPLStrnlen()                            */
/************************************************************************/

/**
 * Returns the length of a NUL terminated string by reading at most
 * the specified number of bytes.
 *
 * The CPLStrnlen() function returns min(strlen(pszStr), nMaxLen).
 * Only the first nMaxLen bytes of the string will be read. Useful to
 * test if a string contains at least nMaxLen characters without reading
 * the full string up to the NUL terminating character.
 *
 * @param pszStr    a NUL terminated string
 * @param nMaxLen   maximum number of bytes to read in pszStr
 *
 * @return strlen(pszStr) if the length is lesser than nMaxLen, otherwise
 * nMaxLen if the NUL character has not been found in the first nMaxLen bytes.
 *
 * @since GDAL 1.7.0
 */

size_t CPLStrnlen(const char *pszStr, size_t nMaxLen)
{
    size_t nLen = 0;
    while (nLen < nMaxLen && *pszStr != '\0')
    {
        ++nLen;
        ++pszStr;
    }
    return nLen;
}

/************************************************************************/
/*                              CPLToupper()                            */
/************************************************************************/

/** Converts a (ASCII) lowercase character to uppercase.
 *
 * Same as standard toupper(), except that it is not locale sensitive.
 *
 * @since GDAL 3.9
 */
int CPLToupper(int c)
{
    return (c >= 'a' && c <= 'z') ? (c - 'a' + 'A') : c;
}

/************************************************************************/
/*                              CPLTolower()                            */
/************************************************************************/

/** Converts a (ASCII) uppercase character to lowercase.
 *
 * Same as standard tolower(), except that it is not locale sensitive.
 *
 * @since GDAL 3.9
 */
int CPLTolower(int c)
{
    return (c >= 'A' && c <= 'Z') ? (c - 'A' + 'a') : c;
}
