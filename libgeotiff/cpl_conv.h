/******************************************************************************
 *
 * Project:  CPL - Common Portability Library
 * Purpose:  Convenience functions declarations.
 *           This is intended to remain light weight.
 * Author:   Frank Warmerdam, warmerdam@pobox.com
 *
 ******************************************************************************
 * Copyright (c) 1998, Frank Warmerdam
 * Copyright (c) 2007-2013, Even Rouault <even dot rouault at spatialys.com>
 *
 * SPDX-License-Identifier: MIT
 ****************************************************************************/

#ifndef CPL_CONV_H_INCLUDED
#define CPL_CONV_H_INCLUDED

#include "cpl_port.h"

/**
 * \file cpl_conv.h
 *
 * Various convenience functions for CPL.
 *
 */

/* -------------------------------------------------------------------- */
/*      Runtime check of various configuration items.                   */
/* -------------------------------------------------------------------- */
CPL_C_START

const char CPL_DLL *CPL_STDCALL CPLGetConfigOption(const char *, const char *)
    CPL_WARN_UNUSED_RESULT;

/* -------------------------------------------------------------------- */
/*      Convert ASCII string to floating point number                  */
/*      (THESE FUNCTIONS ARE NOT LOCALE AWARE!).                        */
/* -------------------------------------------------------------------- */
double CPL_DLL CPLAtof(const char *);
double CPL_DLL CPLStrtod(const char *, char **);
double CPL_DLL CPLStrtodDelim(const char *, char **, char);

CPL_C_END

#endif /* ndef CPL_CONV_H_INCLUDED */
