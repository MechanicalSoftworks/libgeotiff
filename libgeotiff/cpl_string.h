/**********************************************************************
 *
 * Name:     cpl_string.h
 * Project:  CPL - Common Portability Library
 * Purpose:  String and StringList functions.
 * Author:   Daniel Morissette, dmorissette@mapgears.com
 *
 **********************************************************************
 * Copyright (c) 1998, Daniel Morissette
 * Copyright (c) 2008-2014, Even Rouault <even dot rouault at spatialys.com>
 *
 * SPDX-License-Identifier: MIT
 ****************************************************************************/

#ifndef CPL_STRING_H_INCLUDED
#define CPL_STRING_H_INCLUDED

#include "cpl_serv.h"
#include "cpl_port.h"

#include <stdbool.h>

/**
 * \file cpl_string.h
 *
 * Various convenience functions for working with strings and string lists.
 *
 * A StringList is just an array of strings with the last pointer being
 * NULL.  An empty StringList may be either a NULL pointer, or a pointer to
 * a pointer memory location with a NULL value.
 *
 * A common convention for StringLists is to use them to store name/value
 * lists.  In this case the contents are treated like a dictionary of
 * name/value pairs.  The actual data is formatted with each string having
 * the format "<name>:<value>" (though "=" is also an acceptable separator).
 * A number of the functions in the file operate on name/value style
 * string lists (such as CSLSetNameValue(), and CSLFetchNameValue()).
 *
 * To some extent the CPLStringList C++ class can be used to abstract
 * managing string lists a bit but still be able to return them from C
 * functions.
 *
 */

CPL_C_START

bool CPL_DLL CPLTestBool(const char* pszValue);

int CPL_DLL CSLFindString(CSLConstList papszList, const char* pszTarget);
int CPL_DLL CSLPartialFindString(CSLConstList papszHaystack,
                                 const char *pszNeedle);
int CPL_DLL CSLFindName(CSLConstList papszStrList, const char *pszName);

const char CPL_DLL *CSLFetchNameValue(CSLConstList papszStrList,
                                      const char *pszName);
const char CPL_DLL *CSLFetchNameValueDef(CSLConstList papszStrList,
                                         const char *pszName,
                                         const char *pszDefault);

/** Scheme for CPLEscapeString()/CPLUnescapeString() for backlash quoting */
#define CPLES_BackslashQuotable 0
/** Scheme for CPLEscapeString()/CPLUnescapeString() for XML */
#define CPLES_XML 1
/** Scheme for CPLEscapeString()/CPLUnescapeString() for URL */
#define CPLES_URL 2
/** Scheme for CPLEscapeString()/CPLUnescapeString() for SQL */
#define CPLES_SQL 3
/** Scheme for CPLEscapeString()/CPLUnescapeString() for CSV */
#define CPLES_CSV 4
/** Scheme for CPLEscapeString()/CPLUnescapeString() for XML (preserves quotes)
 */
#define CPLES_XML_BUT_QUOTES 5
/** Scheme for CPLEscapeString()/CPLUnescapeString() for CSV (forced quoting) */
#define CPLES_CSV_FORCE_QUOTING 6
/** Scheme for CPLEscapeString()/CPLUnescapeString() for SQL identifiers */
#define CPLES_SQLI 7

char CPL_DLL *CPLEscapeString(const char *pszString, int nLength,
                              int nScheme) CPL_WARN_UNUSED_RESULT;
char CPL_DLL *CPLUnescapeString(const char *pszString, int *pnLength,
                                int nScheme) CPL_WARN_UNUSED_RESULT;

int CPL_DLL CPLToupper(int c);
int CPL_DLL CPLTolower(int c);

size_t CPL_DLL CPLStrlcpy(char *pszDest, const char *pszSrc, size_t nDestSize);
size_t CPL_DLL CPLStrlcat(char *pszDest, const char *pszSrc, size_t nDestSize);
size_t CPL_DLL CPLStrnlen(const char *pszStr, size_t nMaxLen);

/* -------------------------------------------------------------------- */
/*      Locale independent formatting functions.                        */
/* -------------------------------------------------------------------- */
int CPL_DLL CPLvsnprintf(char *str, size_t size,
                         CPL_FORMAT_STRING(const char *fmt), va_list args)
    CPL_PRINT_FUNC_FORMAT(3, 0);

/* ALIAS_CPLSNPRINTF_AS_SNPRINTF might be defined to enable GCC 7 */
/* -Wformat-truncation= warnings, but shouldn't be set for normal use */
#if defined(ALIAS_CPLSNPRINTF_AS_SNPRINTF)
#define CPLsnprintf snprintf
#else
int CPL_DLL CPLsnprintf(char *str, size_t size,
                        CPL_FORMAT_STRING(const char *fmt), ...)
    CPL_PRINT_FUNC_FORMAT(3, 4);
#endif

/*! @cond Doxygen_Suppress */
#if defined(GDAL_COMPILATION) && !defined(DONT_DEPRECATE_SPRINTF)
int CPL_DLL CPLsprintf(char *str, CPL_FORMAT_STRING(const char *fmt), ...)
    CPL_PRINT_FUNC_FORMAT(2, 3) CPL_WARN_DEPRECATED("Use CPLsnprintf instead");
#else
int CPL_DLL CPLsprintf(char *str, CPL_FORMAT_STRING(const char *fmt), ...)
    CPL_PRINT_FUNC_FORMAT(2, 3);
#endif
/*! @endcond */

const char CPL_DLL* CPLSPrintf(CPL_FORMAT_STRING(const char* fmt), ...)
CPL_PRINT_FUNC_FORMAT(1, 2) CPL_WARN_UNUSED_RESULT;

/* -------------------------------------------------------------------- */
/*      RFC 23 character set conversion/recoding API (cpl_recode.cpp).  */
/* -------------------------------------------------------------------- */
/** Encoding of the current locale */
#define CPL_ENC_LOCALE ""
/** UTF-8 encoding */
#define CPL_ENC_UTF8 "UTF-8"
/** UTF-16 encoding */
#define CPL_ENC_UTF16 "UTF-16"
/** UCS-2 encoding */
#define CPL_ENC_UCS2 "UCS-2"
/** UCS-4 encoding */
#define CPL_ENC_UCS4 "UCS-4"
/** ASCII encoding */
#define CPL_ENC_ASCII "ASCII"
/** ISO-8859-1 (LATIN1) encoding */
#define CPL_ENC_ISO8859_1 "ISO-8859-1"

char CPL_DLL *CPLRecode(const char *pszSource, const char *pszSrcEncoding,
                        const char *pszDstEncoding)
    CPL_WARN_UNUSED_RESULT CPL_RETURNS_NONNULL;
char CPL_DLL *
CPLRecodeFromWChar(const wchar_t *pwszSource, const char *pszSrcEncoding,
                   const char *pszDstEncoding) CPL_WARN_UNUSED_RESULT;

CPL_C_END

/************************************************************************/
/*                              CPLString                               */
/************************************************************************/

#if defined(__cplusplus) && !defined(CPL_SUPRESS_CPLUSPLUS)

extern "C++"
{
#ifndef DOXYGEN_SKIP
#include <string>
#include <vector>
#endif

// VC++ implicitly applies __declspec(dllexport) to template base classes
// of classes marked with __declspec(dllexport).
// Hence, if marked with CPL_DLL, VC++ would export symbols for the
// specialization of std::basic_string<char>, since it is a base class of
// CPLString. As a result, if an application linked both gdal.dll and a static
// library that (implicitly) instantiates std::string (almost all do!), then the
// linker would emit an error concerning duplicate symbols for std::string. The
// least intrusive solution is to not mark the whole class with
// __declspec(dllexport) for VC++, but only its non-inline methods.
#ifdef _MSC_VER
#define CPLSTRING_CLASS_DLL
#define CPLSTRING_METHOD_DLL CPL_DLL
#else
/*! @cond Doxygen_Suppress */
#define CPLSTRING_CLASS_DLL CPL_DLL
#define CPLSTRING_METHOD_DLL
/*! @endcond */
#endif

    //! Convenient string class based on std::string.
    class CPLSTRING_CLASS_DLL CPLString : public std::string
    {
      public:
        /** Constructor */
        CPLString(void)
        {
        }

        /** Constructor */
        // cppcheck-suppress noExplicitConstructor
        CPLString(const std::string &oStr) : std::string(oStr)
        {
        }

        /** Constructor */
        // cppcheck-suppress noExplicitConstructor
        CPLString(const char *pszStr) : std::string(pszStr)
        {
        }

        /** Constructor */
        CPLString(const char *pszStr, size_t n) : std::string(pszStr, n)
        {
        }

        /** Return string as zero terminated character array */
        operator const char *(void) const
        {
            return c_str();
        }

        /** Return character at specified index */
        char &operator[](std::string::size_type i)
        {
            return std::string::operator[](i);
        }

        /** Return character at specified index */
        const char &operator[](std::string::size_type i) const
        {
            return std::string::operator[](i);
        }

        /** Return character at specified index */
        char &operator[](int i)
        {
            return std::string::operator[](
                static_cast<std::string::size_type>(i));
        }

        /** Return character at specified index */
        const char &operator[](int i) const
        {
            return std::string::operator[](
                static_cast<std::string::size_type>(i));
        }

        /** Clear the string */
        void Clear()
        {
            resize(0);
        }

        /** Assign specified string and take ownership of it (assumed to be
         * allocated with CPLMalloc()). NULL can be safely passed to clear the
         * string. */
        void Seize(char* pszValue);

        /* There seems to be a bug in the way the compiler count indices...
         * Should be CPL_PRINT_FUNC_FORMAT (1, 2) */
        CPLSTRING_METHOD_DLL CPLString &
        Printf(CPL_FORMAT_STRING(const char *pszFormat), ...)
            CPL_PRINT_FUNC_FORMAT(2, 3);
        CPLSTRING_METHOD_DLL CPLString &
        vPrintf(CPL_FORMAT_STRING(const char *pszFormat), va_list args)
            CPL_PRINT_FUNC_FORMAT(2, 0);
        CPLSTRING_METHOD_DLL CPLString &
        FormatC(double dfValue, const char *pszFormat = nullptr);
        CPLSTRING_METHOD_DLL CPLString &Trim();
        CPLSTRING_METHOD_DLL CPLString &Recode(const char *pszSrcEncoding,
                                               const char *pszDstEncoding);
        CPLSTRING_METHOD_DLL CPLString &replaceAll(const std::string &osBefore,
                                                   const std::string &osAfter);
        CPLSTRING_METHOD_DLL CPLString &replaceAll(const std::string &osBefore,
                                                   char chAfter);
        CPLSTRING_METHOD_DLL CPLString &replaceAll(char chBefore,
                                                   const std::string &osAfter);
        CPLSTRING_METHOD_DLL CPLString &replaceAll(char chBefore, char chAfter);

        /* case insensitive find alternates */
        CPLSTRING_METHOD_DLL size_t ifind(const std::string &str,
                                          size_t pos = 0) const;
        CPLSTRING_METHOD_DLL size_t ifind(const char *s, size_t pos = 0) const;
        CPLSTRING_METHOD_DLL CPLString &toupper(void);
        CPLSTRING_METHOD_DLL CPLString &tolower(void);

        CPLSTRING_METHOD_DLL bool endsWith(const std::string &osStr) const;
    };

#undef CPLSTRING_CLASS_DLL
#undef CPLSTRING_METHOD_DLL

    CPLString CPL_DLL CPLOPrintf(CPL_FORMAT_STRING(const char *pszFormat), ...)
        CPL_PRINT_FUNC_FORMAT(1, 2);
    CPLString CPL_DLL CPLOvPrintf(CPL_FORMAT_STRING(const char *pszFormat),
                                  va_list args) CPL_PRINT_FUNC_FORMAT(1, 0);
    CPLString CPL_DLL CPLQuotedSQLIdentifier(const char *pszIdent);

    /* -------------------------------------------------------------------- */
    /*      URL processing functions, here since they depend on CPLString.  */
    /* -------------------------------------------------------------------- */
    CPLString CPL_DLL CPLURLGetValue(const char *pszURL, const char *pszKey);
    CPLString CPL_DLL CPLURLAddKVP(const char *pszURL, const char *pszKey,
                                   const char *pszValue);

    /************************************************************************/
    /*                            CPLStringList                             */
    /************************************************************************/

    //! String list class designed around our use of C "char**" string lists.
    class CPL_DLL CPLStringList
    {
        char **papszList = nullptr;
        mutable int nCount = 0;
        mutable int nAllocation = 0;
        bool bOwnList = false;
        bool bIsSorted = false;

        bool MakeOurOwnCopy();
        bool EnsureAllocation(int nMaxLength);
        int FindSortedInsertionPoint(const char *pszLine);

      public:
        CPLStringList();
        explicit CPLStringList(char **papszList, int bTakeOwnership = TRUE);
        explicit CPLStringList(CSLConstList papszList);
        explicit CPLStringList(const std::vector<std::string> &aosList);
        explicit CPLStringList(std::initializer_list<const char *> oInitList);
        CPLStringList(const CPLStringList &oOther);
        CPLStringList(CPLStringList &&oOther);
        ~CPLStringList();

        static const CPLStringList BoundToConstList(CSLConstList papszList);

        CPLStringList &Clear();

        /** Clear the list */
        inline void clear()
        {
            Clear();
        }

        /** Return size of list */
        int size() const
        {
            return Count();
        }

        int Count() const;

        /** Return whether the list is empty. */
        bool empty() const
        {
            return Count() == 0;
        }

        CPLStringList &AddString(const char *pszNewString);
        CPLStringList &AddString(const std::string &newString);
        CPLStringList &AddStringDirectly(char *pszNewString);

        /** Add a string to the list */
        void push_back(const char *pszNewString)
        {
            AddString(pszNewString);
        }

        /** Add a string to the list */
        void push_back(const std::string &osStr)
        {
            AddString(osStr.c_str());
        }

        CPLStringList &InsertString(int nInsertAtLineNo, const char *pszNewLine)
        {
            return InsertStringDirectly(nInsertAtLineNo, CPLStrdup(pszNewLine));
        }

        CPLStringList &InsertStringDirectly(int nInsertAtLineNo,
                                            char *pszNewLine);

        // CPLStringList &InsertStrings( int nInsertAtLineNo, char
        // **papszNewLines ); CPLStringList &RemoveStrings( int
        // nFirstLineToDelete, int nNumToRemove=1 );

        /** Return index of pszTarget in the list, or -1 */
        int FindString(const char *pszTarget) const
        {
            return CSLFindString(papszList, pszTarget);
        }

        /** Return index of pszTarget in the list (using partial search), or -1
         */
        int PartialFindString(const char *pszNeedle) const
        {
            return CSLPartialFindString(papszList, pszNeedle);
        }

        int FindName(const char *pszName) const;
        bool FetchBool(const char *pszKey, bool bDefault) const;
        // Deprecated.
        int FetchBoolean(const char *pszKey, int bDefault) const;
        const char *FetchNameValue(const char *pszKey) const;
        const char *FetchNameValueDef(const char *pszKey,
                                      const char *pszDefault) const;
        CPLStringList &AddNameValue(const char *pszKey, const char *pszValue);
        CPLStringList &SetNameValue(const char *pszKey, const char *pszValue);

        CPLStringList &Assign(char **papszListIn, int bTakeOwnership = TRUE);

        /** Assignment operator */
        CPLStringList &operator=(char **papszListIn)
        {
            return Assign(papszListIn, TRUE);
        }

        /** Assignment operator */
        CPLStringList &operator=(const CPLStringList &oOther);
        /** Assignment operator */
        CPLStringList &operator=(CSLConstList papszListIn);
        /** Move assignment operator */
        CPLStringList &operator=(CPLStringList &&oOther);

        /** Return string at specified index */
        char *operator[](int i);

        /** Return string at specified index */
        char *operator[](size_t i)
        {
            return (*this)[static_cast<int>(i)];
        }

        /** Return string at specified index */
        const char *operator[](int i) const;

        /** Return string at specified index */
        const char *operator[](size_t i) const
        {
            return (*this)[static_cast<int>(i)];
        }

        /** Return value corresponding to pszKey, or nullptr */
        const char *operator[](const char *pszKey) const
        {
            return FetchNameValue(pszKey);
        }

        /** Return first element */
        inline const char *front() const
        {
            return papszList[0];
        }

        /** Return last element */
        inline const char *back() const
        {
            return papszList[size() - 1];
        }

        /** begin() implementation */
        const char *const *begin() const
        {
            return papszList ? &papszList[0] : nullptr;
        }

        /** end() implementation */
        const char *const *end() const
        {
            return papszList ? &papszList[size()] : nullptr;
        }

        /** Return list. Ownership remains to the object */
        char **List()
        {
            return papszList;
        }

        /** Return list. Ownership remains to the object */
        CSLConstList List() const
        {
            return papszList;
        }

        char **StealList();

        CPLStringList &Sort();

        /** Returns whether the list is sorted */
        int IsSorted() const
        {
            return bIsSorted;
        }

        /** Return lists */
        operator char **(void)
        {
            return List();
        }

        /** Return lists */
        operator CSLConstList(void) const
        {
            return List();
        }

        /** Return the list as a vector of strings */
        operator std::vector<std::string>(void) const
        {
            return std::vector<std::string>{begin(), end()};
        }
    };

}  // extern "C++"

#endif /* def __cplusplus && !CPL_SUPRESS_CPLUSPLUS */

#endif /* CPL_STRING_H_INCLUDED */
