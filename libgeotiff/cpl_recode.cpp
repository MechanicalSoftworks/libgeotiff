/**********************************************************************
 *
 * Name:     cpl_recode.cpp
 * Project:  CPL - Common Portability Library
 * Purpose:  Character set recoding and char/wchar_t conversions.
 * Author:   Andrey Kiselev, dron@ak4719.spb.edu
 *
 **********************************************************************
 * Copyright (c) 2011, Andrey Kiselev <dron@ak4719.spb.edu>
 * Copyright (c) 2008, Frank Warmerdam
 * Copyright (c) 2011-2014, Even Rouault <even dot rouault at spatialys.com>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 **********************************************************************/

#include "cpl_port.h"
#include "cpl_string.h"

#include <cstring>

#include "cpl_conv.h"
CPL_C_START
#include "cpl_character_sets.h"
CPL_C_END

extern void CPLClearRecodeStubWarningFlags();
extern char *CPLRecodeStub(const char *, const char *,
                           const char *) CPL_RETURNS_NONNULL;
extern char *CPLRecodeFromWCharStub(const wchar_t *, const char *,
                                    const char *);
extern wchar_t *CPLRecodeToWCharStub(const char *, const char *, const char *);

/************************************************************************/
/*                             CPLRecode()                              */
/************************************************************************/

/**
 * Convert a string from a source encoding to a destination encoding.
 *
 * The only guaranteed supported encodings are CPL_ENC_UTF8, CPL_ENC_ASCII
 * and CPL_ENC_ISO8859_1. Currently, the following conversions are supported :
 * <ul>
 *  <li>CPL_ENC_ASCII -> CPL_ENC_UTF8 or CPL_ENC_ISO8859_1 (no conversion in
 *  fact)</li>
 *  <li>CPL_ENC_ISO8859_1 -> CPL_ENC_UTF8</li>
 *  <li>CPL_ENC_UTF8 -> CPL_ENC_ISO8859_1</li>
 * </ul>
 *
 * If an error occurs an error may, or may not be posted with CPLError().
 *
 * @param pszSource a NULL terminated string.
 * @param pszSrcEncoding the source encoding.
 * @param pszDstEncoding the destination encoding.
 *
 * @return a NULL terminated string which should be freed with CPLFree().
 *
 * @since GDAL 1.6.0
 */

char CPL_DLL *CPLRecode(const char *pszSource, const char *pszSrcEncoding,
                        const char *pszDstEncoding)

{
    /* -------------------------------------------------------------------- */
    /*      Handle a few common short cuts.                                 */
    /* -------------------------------------------------------------------- */
    if (EQUAL(pszSrcEncoding, pszDstEncoding))
        return CPLStrdup(pszSource);

    if (EQUAL(pszSrcEncoding, CPL_ENC_ASCII) &&
        (EQUAL(pszDstEncoding, CPL_ENC_UTF8) ||
         EQUAL(pszDstEncoding, CPL_ENC_ISO8859_1)))
        return CPLStrdup(pszSource);

    // A few hard coded CPxxx/ISO-8859-x to UTF-8 tables
    if (EQUAL(pszDstEncoding, CPL_ENC_UTF8) &&
        CPLGetConversionTableToUTF8(pszSrcEncoding))
    {
        return CPLRecodeStub(pszSource, pszSrcEncoding, pszDstEncoding);
    }

#ifdef CPL_RECODE_ICONV
    /* -------------------------------------------------------------------- */
    /*      CPL_ENC_ISO8859_1 -> CPL_ENC_UTF8                               */
    /*      and CPL_ENC_UTF8 -> CPL_ENC_ISO8859_1 conversions are handled   */
    /*      very well by the stub implementation which is faster than the   */
    /*      iconv() route. Use a stub for these two ones and iconv()        */
    /*      everything else.                                                */
    /* -------------------------------------------------------------------- */
    if ((EQUAL(pszSrcEncoding, CPL_ENC_ISO8859_1) &&
         EQUAL(pszDstEncoding, CPL_ENC_UTF8)) ||
        (EQUAL(pszSrcEncoding, CPL_ENC_UTF8) &&
         EQUAL(pszDstEncoding, CPL_ENC_ISO8859_1)))
    {
        return CPLRecodeStub(pszSource, pszSrcEncoding, pszDstEncoding);
    }
#ifdef _WIN32
    else if (((EQUAL(pszSrcEncoding, "CP_ACP") ||
               EQUAL(pszSrcEncoding, "CP_OEMCP")) &&
              EQUAL(pszDstEncoding, CPL_ENC_UTF8)) ||
             (EQUAL(pszSrcEncoding, CPL_ENC_UTF8) &&
              (EQUAL(pszDstEncoding, "CP_ACP") ||
               EQUAL(pszDstEncoding, "CP_OEMCP"))))
    {
        return CPLRecodeStub(pszSource, pszSrcEncoding, pszDstEncoding);
    }
#endif
    else
    {
        return CPLRecodeIconv(pszSource, pszSrcEncoding, pszDstEncoding);
    }
#else   // CPL_RECODE_STUB
    return CPLRecodeStub(pszSource, pszSrcEncoding, pszDstEncoding);
#endif  // CPL_RECODE_ICONV
}

/************************************************************************/
/*                         CPLRecodeFromWChar()                         */
/************************************************************************/

/**
 * Convert wchar_t string to UTF-8.
 *
 * Convert a wchar_t string into a multibyte utf-8 string.  The only
 * guaranteed supported source encoding is CPL_ENC_UCS2, and the only
 * guaranteed supported destination encodings are CPL_ENC_UTF8, CPL_ENC_ASCII
 * and CPL_ENC_ISO8859_1.  In some cases (i.e. using iconv()) other encodings
 * may also be supported.
 *
 * Note that the wchar_t type varies in size on different systems. On
 * win32 it is normally 2 bytes, and on UNIX 4 bytes.
 *
 * If an error occurs an error may, or may not be posted with CPLError().
 *
 * @param pwszSource the source wchar_t string, terminated with a 0 wchar_t.
 * @param pszSrcEncoding the source encoding, typically CPL_ENC_UCS2.
 * @param pszDstEncoding the destination encoding, typically CPL_ENC_UTF8.
 *
 * @return a zero terminated multi-byte string which should be freed with
 * CPLFree(), or NULL if an error occurs.
 *
 * @since GDAL 1.6.0
 */

char CPL_DLL *CPLRecodeFromWChar(const wchar_t *pwszSource,
                                 const char *pszSrcEncoding,
                                 const char *pszDstEncoding)

{
#ifdef CPL_RECODE_ICONV
    /* -------------------------------------------------------------------- */
    /*      Conversions from CPL_ENC_UCS2                                   */
    /*      to CPL_ENC_UTF8, CPL_ENC_ISO8859_1 and CPL_ENC_ASCII are well   */
    /*      handled by the stub implementation.                             */
    /* -------------------------------------------------------------------- */
    if ((EQUAL(pszSrcEncoding, CPL_ENC_UCS2) ||
         EQUAL(pszSrcEncoding, "WCHAR_T")) &&
        (EQUAL(pszDstEncoding, CPL_ENC_UTF8) ||
         EQUAL(pszDstEncoding, CPL_ENC_ASCII) ||
         EQUAL(pszDstEncoding, CPL_ENC_ISO8859_1)))
    {
        return CPLRecodeFromWCharStub(pwszSource, pszSrcEncoding,
                                      pszDstEncoding);
    }

    return CPLRecodeFromWCharIconv(pwszSource, pszSrcEncoding, pszDstEncoding);

#else   // CPL_RECODE_STUB
    return CPLRecodeFromWCharStub(pwszSource, pszSrcEncoding, pszDstEncoding);
#endif  // CPL_RECODE_ICONV
}

/************************************************************************/
/*                          CPLRecodeToWChar()                          */
/************************************************************************/

/**
 * Convert UTF-8 string to a wchar_t string.
 *
 * Convert a 8bit, multi-byte per character input string into a wide
 * character (wchar_t) string.  The only guaranteed supported source encodings
 * are CPL_ENC_UTF8, CPL_ENC_ASCII and CPL_ENC_ISO8869_1 (LATIN1).  The only
 * guaranteed supported destination encoding is CPL_ENC_UCS2.  Other source
 * and destination encodings may be supported depending on the underlying
 * implementation.
 *
 * Note that the wchar_t type varies in size on different systems. On
 * win32 it is normally 2 bytes, and on UNIX 4 bytes.
 *
 * If an error occurs an error may, or may not be posted with CPLError().
 *
 * @param pszSource input multi-byte character string.
 * @param pszSrcEncoding source encoding, typically CPL_ENC_UTF8.
 * @param pszDstEncoding destination encoding, typically CPL_ENC_UCS2.
 *
 * @return the zero terminated wchar_t string (to be freed with CPLFree()) or
 * NULL on error.
 *
 * @since GDAL 1.6.0
 */

wchar_t CPL_DLL *CPLRecodeToWChar(const char *pszSource,
                                  const char *pszSrcEncoding,
                                  const char *pszDstEncoding)

{
#ifdef CPL_RECODE_ICONV
    /* -------------------------------------------------------------------- */
    /*      Conversions to CPL_ENC_UCS2                                     */
    /*      from CPL_ENC_UTF8, CPL_ENC_ISO8859_1 and CPL_ENC_ASCII are well */
    /*      handled by the stub implementation.                             */
    /* -------------------------------------------------------------------- */
    if ((EQUAL(pszDstEncoding, CPL_ENC_UCS2) ||
         EQUAL(pszDstEncoding, "WCHAR_T")) &&
        (EQUAL(pszSrcEncoding, CPL_ENC_UTF8) ||
         EQUAL(pszSrcEncoding, CPL_ENC_ASCII) ||
         EQUAL(pszSrcEncoding, CPL_ENC_ISO8859_1)))
    {
        return CPLRecodeToWCharStub(pszSource, pszSrcEncoding, pszDstEncoding);
    }

    return CPLRecodeToWCharIconv(pszSource, pszSrcEncoding, pszDstEncoding);

#else   // CPL_RECODE_STUB
    return CPLRecodeToWCharStub(pszSource, pszSrcEncoding, pszDstEncoding);
#endif  // CPL_RECODE_ICONV
}

/************************************************************************/
/*                               CPLIsASCII()                           */
/************************************************************************/

/**
 * Test if a string is encoded as ASCII.
 *
 * @param pabyData input string to test
 * @param nLen length of the input string, or -1 if the function must compute
 *             the string length. In which case it must be null terminated.
 * @return true if the string is encoded as ASCII. false otherwise
 *
 * @since GDAL 3.6.0
 */
bool CPLIsASCII(const char *pabyData, size_t nLen)
{
    if (nLen == static_cast<size_t>(-1))
        nLen = strlen(pabyData);
    for (size_t i = 0; i < nLen; ++i)
    {
        if (static_cast<unsigned char>(pabyData[i]) > 127)
            return false;
    }
    return true;
}

/************************************************************************/
/*                          CPLForceToASCII()                           */
/************************************************************************/

/**
 * Return a new string that is made only of ASCII characters. If non-ASCII
 * characters are found in the input string, they will be replaced by the
 * provided replacement character.
 *
 * This function does not make any assumption on the encoding of the input
 * string (except it must be nul-terminated if nLen equals -1, or have at
 * least nLen bytes otherwise). CPLUTF8ForceToASCII() can be used instead when
 * the input string is known to be UTF-8 encoded.
 *
 * @param pabyData input string to test
 * @param nLen length of the input string, or -1 if the function must compute
 *             the string length. In which case it must be null terminated.

 * @param chReplacementChar character which will be used when the input stream
 *                          contains a non ASCII character. Must be valid ASCII!
 *
 * @return a new string that must be freed with CPLFree().
 *
 * @since GDAL 1.7.0
 */
char *CPLForceToASCII(const char *pabyData, int nLen, char chReplacementChar)
{
    const size_t nRealLen =
        (nLen >= 0) ? static_cast<size_t>(nLen) : strlen(pabyData);
    char *pszOutputString = static_cast<char *>(CPLMalloc(nRealLen + 1));
    const char *pszPtr = pabyData;
    const char *pszEnd = pabyData + nRealLen;
    size_t i = 0;
    while (pszPtr != pszEnd)
    {
        if (*reinterpret_cast<const unsigned char *>(pszPtr) > 127)
        {
            pszOutputString[i] = chReplacementChar;
            ++pszPtr;
            ++i;
        }
        else
        {
            pszOutputString[i] = *pszPtr;
            ++pszPtr;
            ++i;
        }
    }
    pszOutputString[i] = '\0';
    return pszOutputString;
}

/************************************************************************/
/*                        CPLEncodingCharSize()                         */
/************************************************************************/

/**
 * Return bytes per character for encoding.
 *
 * This function returns the size in bytes of the smallest character
 * in this encoding.  For fixed width encodings (ASCII, UCS-2, UCS-4) this
 * is straight forward.  For encodings like UTF8 and UTF16 which represent
 * some characters as a sequence of atomic character sizes the function
 * still returns the atomic character size (1 for UTF8, 2 for UTF16).
 *
 * This function will return the correct value for well known encodings
 * with corresponding CPL_ENC_ values.  It may not return the correct value
 * for other encodings even if they are supported by the underlying iconv
 * or windows transliteration services.  Hopefully it will improve over time.
 *
 * @param pszEncoding the name of the encoding.
 *
 * @return the size of a minimal character in bytes or -1 if the size is
 * unknown.
 */

int CPLEncodingCharSize(const char *pszEncoding)

{
    if (EQUAL(pszEncoding, CPL_ENC_UTF8))
        return 1;
    else if (EQUAL(pszEncoding, CPL_ENC_UTF16) ||
             EQUAL(pszEncoding, "UTF-16LE"))
        return 2;
    else if (EQUAL(pszEncoding, CPL_ENC_UCS2) || EQUAL(pszEncoding, "UCS-2LE"))
        return 2;
    else if (EQUAL(pszEncoding, CPL_ENC_UCS4))
        return 4;
    else if (EQUAL(pszEncoding, CPL_ENC_ASCII))
        return 1;
    else if (STARTS_WITH_CI(pszEncoding, "ISO-8859-"))
        return 1;

    return -1;
}

/************************************************************************/
/*                    CPLClearRecodeWarningFlags()                      */
/************************************************************************/

void CPLClearRecodeWarningFlags()
{
#ifdef CPL_RECODE_ICONV
    CPLClearRecodeIconvWarningFlags();
#endif
    CPLClearRecodeStubWarningFlags();
}

/************************************************************************/
/*                         CPLStrlenUTF8()                              */
/************************************************************************/

/**
 * Return the number of UTF-8 characters of a nul-terminated string.
 *
 * This is different from strlen() which returns the number of bytes.
 *
 * @param pszUTF8Str a nul-terminated UTF-8 string
 *
 * @return the number of UTF-8 characters.
 */

int CPLStrlenUTF8(const char *pszUTF8Str)
{
    int nCharacterCount = 0;
    for (size_t i = 0; pszUTF8Str[i] != '\0'; ++i)
    {
        if ((pszUTF8Str[i] & 0xc0) != 0x80)
        {
            if (nCharacterCount == INT_MAX)
            {
                CPLError(CE_Failure, CPLE_AppDefined,
                         "CPLStrlenUTF8(): nCharacterCount > INT_MAX. Use "
                         "CPLStrlenUTF8Ex() instead");
                break;
            }
            ++nCharacterCount;
        }
    }
    return nCharacterCount;
}

/************************************************************************/
/*                         CPLStrlenUTF8Ex()                            */
/************************************************************************/

/**
 * Return the number of UTF-8 characters of a nul-terminated string.
 *
 * This is different from strlen() which returns the number of bytes.
 *
 * @param pszUTF8Str a nul-terminated UTF-8 string
 *
 * @return the number of UTF-8 characters.
 */

size_t CPLStrlenUTF8Ex(const char *pszUTF8Str)
{
    size_t nCharacterCount = 0;
    for (size_t i = 0; pszUTF8Str[i] != '\0'; ++i)
    {
        if ((pszUTF8Str[i] & 0xc0) != 0x80)
        {
            ++nCharacterCount;
        }
    }
    return nCharacterCount;
}
