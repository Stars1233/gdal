/******************************************************************************
 *
 * Project:  OpenGIS Simple Features Reference Implementation
 * Purpose:  Implements OGRPGLayer class  which implements shared handling
 *           of feature geometry and so forth needed by OGRPGResultLayer and
 *           OGRPGTableLayer.
 * Author:   Frank Warmerdam, warmerdam@pobox.com
 *
 ******************************************************************************
 * Copyright (c) 2000, Frank Warmerdam
 * Copyright (c) 2008-2013, Even Rouault <even dot rouault at spatialys.com>
 *
 * SPDX-License-Identifier: MIT
 ****************************************************************************/

/* Some functions have been extracted from PostgreSQL code base  */
/* The applicable copyright & licence notice is the following one : */
/*
PostgreSQL Database Management System
(formerly known as Postgres, then as Postgres95)

Portions Copyright (c) 1996-2006, PostgreSQL Global Development Group

Portions Copyright (c) 1994, The Regents of the University of California

Permission to use, copy, modify, and distribute this software and its
documentation for any purpose, without fee, and without a written agreement
is hereby granted, provided that the above copyright notice and this
paragraph and the following two paragraphs appear in all copies.

IN NO EVENT SHALL THE UNIVERSITY OF CALIFORNIA BE LIABLE TO ANY PARTY FOR
DIRECT, INDIRECT, SPECIAL, INCIDENTAL, OR CONSEQUENTIAL DAMAGES, INCLUDING
LOST PROFITS, ARISING OUT OF THE USE OF THIS SOFTWARE AND ITS
DOCUMENTATION, EVEN IF THE UNIVERSITY OF CALIFORNIA HAS BEEN ADVISED OF THE
POSSIBILITY OF SUCH DAMAGE.

THE UNIVERSITY OF CALIFORNIA SPECIFICALLY DISCLAIMS ANY WARRANTIES,
INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY
AND FITNESS FOR A PARTICULAR PURPOSE.  THE SOFTWARE PROVIDED HEREUNDER IS
ON AN "AS IS" BASIS, AND THE UNIVERSITY OF CALIFORNIA HAS NO OBLIGATIONS TO
PROVIDE MAINTENANCE, SUPPORT, UPDATES, ENHANCEMENTS, OR MODIFICATIONS.
*/

#include "ogr_pg.h"
#include "ogr_p.h"
#include "cpl_conv.h"
#include "cpl_string.h"

#include <limits>

#define PQexec this_is_an_error

// These originally are defined in libpq-fs.h.

#ifndef INV_WRITE
#define INV_WRITE 0x00020000
#define INV_READ 0x00040000
#endif

/************************************************************************/
/*                           OGRPGLayer()                               */
/************************************************************************/

OGRPGLayer::OGRPGLayer()
    : nCursorPage(atoi(CPLGetConfigOption("OGR_PG_CURSOR_PAGE", "500")))
{
    pszCursorName = CPLStrdup(CPLSPrintf("OGRPGLayerReader%p", this));
}

/************************************************************************/
/*                            ~OGRPGLayer()                             */
/************************************************************************/

OGRPGLayer::~OGRPGLayer()

{
    if (m_nFeaturesRead > 0 && poFeatureDefn != nullptr)
    {
        CPLDebug("PG", CPL_FRMT_GIB " features read on layer '%s'.",
                 m_nFeaturesRead, poFeatureDefn->GetName());
    }

    CloseCursor();

    CPLFree(pszFIDColumn);
    CPLFree(pszQueryStatement);
    CPLFree(m_panMapFieldNameToIndex);
    CPLFree(m_panMapFieldNameToGeomIndex);
    CPLFree(pszCursorName);

    if (poFeatureDefn)
    {
        poFeatureDefn->UnsetLayer();
        poFeatureDefn->Release();
    }
}

/************************************************************************/
/*                            CloseCursor()                             */
/************************************************************************/

void OGRPGLayer::CloseCursor()
{
    PGconn *hPGConn = poDS->GetPGConn();

    if (hCursorResult != nullptr)
    {
        OGRPGClearResult(hCursorResult);

        CPLString osCommand;
        osCommand.Printf("CLOSE %s", pszCursorName);

        /* In case of interleaving read in different layers we might have */
        /* close the transaction, and thus implicitly the cursor, so be */
        /* quiet about errors. This is potentially an issue by the way */
        hCursorResult = OGRPG_PQexec(hPGConn, osCommand.c_str(), FALSE, TRUE);
        OGRPGClearResult(hCursorResult);

        poDS->SoftCommitTransaction();

        hCursorResult = nullptr;
    }
}

/************************************************************************/
/*                       InvalidateCursor()                             */
/************************************************************************/

void OGRPGLayer::InvalidateCursor()
{
    CloseCursor();
    bInvalidated = TRUE;
}

/************************************************************************/
/*                            ResetReading()                            */
/************************************************************************/

void OGRPGLayer::ResetReading()

{
    GetLayerDefn();

    iNextShapeId = 0;

    CloseCursor();
    bInvalidated = FALSE;
}

#if defined(BINARY_CURSOR_ENABLED)
/************************************************************************/
/*                    OGRPGGetStrFromBinaryNumeric()                    */
/************************************************************************/

/* Adaptation of get_str_from_var() from pgsql/src/backend/utils/adt/numeric.c
 */

typedef short NumericDigit;

typedef struct NumericVar
{
    int ndigits;          /* # of digits in digits[] - can be 0! */
    int weight;           /* weight of first digit */
    int sign;             /* NUMERIC_POS, NUMERIC_NEG, or NUMERIC_NAN */
    int dscale;           /* display scale */
    NumericDigit *digits; /* base-NBASE digits */
} NumericVar;

#define NUMERIC_POS 0x0000
#define NUMERIC_NEG 0x4000
#define NUMERIC_NAN 0xC000

#define DEC_DIGITS 4

/*
 * get_str_from_var() -
 *
 *       Convert a var to text representation (guts of numeric_out).
 *       CAUTION: var's contents may be modified by rounding!
 *       Returns a malloc'd string.
 */
static char *OGRPGGetStrFromBinaryNumeric(NumericVar *var)
{
    const int dscale = var->dscale;

    /*
     * Allocate space for the result.
     *
     * i is set to to # of decimal digits before decimal point. dscale is the
     * # of decimal digits we will print after decimal point. We may generate
     * as many as DEC_DIGITS-1 excess digits at the end, and in addition we
     * need room for sign, decimal point, null terminator.
     */
    int i = (var->weight + 1) * DEC_DIGITS;
    if (i <= 0)
        i = 1;

    char *str = (char *)CPLMalloc(i + dscale + DEC_DIGITS + 2);
    char *cp = str;

    /*
     * Output a dash for negative values
     */
    if (var->sign == NUMERIC_NEG)
        *cp++ = '-';

    /*
     * Output all digits before the decimal point
     */
    int d = 0;
    if (var->weight < 0)
    {
        d = var->weight + 1;
        *cp++ = '0';
    }
    else
    {
        for (d = 0; d <= var->weight; d++)
        {
            NumericDigit dig = (d < var->ndigits) ? var->digits[d] : 0;
            CPL_MSBPTR16(&dig);
            // In the first digit, suppress extra leading decimal zeroes.
            {
                bool putit = (d > 0);

                NumericDigit d1;
                d1 = dig / 1000;
                dig -= d1 * 1000;
                putit |= (d1 > 0);
                if (putit)
                    *cp++ = (char)(d1 + '0');
                d1 = dig / 100;
                dig -= d1 * 100;
                putit |= (d1 > 0);
                if (putit)
                    *cp++ = (char)(d1 + '0');
                d1 = dig / 10;
                dig -= d1 * 10;
                putit |= (d1 > 0);
                if (putit)
                    *cp++ = (char)(d1 + '0');
                *cp++ = (char)(dig + '0');
            }
        }
    }

    /*
     * If requested, output a decimal point and all the digits that follow it.
     * We initially put out a multiple of DEC_DIGITS digits, then truncate if
     * needed.
     */
    if (dscale > 0)
    {
        *cp++ = '.';
        char *endcp = cp + dscale;
        for (i = 0; i < dscale; d++, i += DEC_DIGITS)
        {
            NumericDigit dig =
                (d >= 0 && d < var->ndigits) ? var->digits[d] : 0;
            CPL_MSBPTR16(&dig);
            NumericDigit d1 = dig / 1000;
            dig -= d1 * 1000;
            *cp++ = (char)(d1 + '0');
            d1 = dig / 100;
            dig -= d1 * 100;
            *cp++ = (char)(d1 + '0');
            d1 = dig / 10;
            dig -= d1 * 10;
            *cp++ = (char)(d1 + '0');
            *cp++ = (char)(dig + '0');
        }
        cp = endcp;
    }

    /*
     * terminate the string and return it
     */
    *cp = '\0';
    return str;
}

/************************************************************************/
/*                         OGRPGj2date()                            */
/************************************************************************/

/* Coming from j2date() in pgsql/src/backend/utils/adt/datetime.c */

#define POSTGRES_EPOCH_JDATE 2451545 /* == date2j(2000, 1, 1) */

static void OGRPGj2date(int jd, int *year, int *month, int *day)
{
    unsigned int julian = jd + 32044;
    unsigned int quad = julian / 146097;
    const unsigned int extra = (julian - quad * 146097) * 4 + 3;
    julian += 60 + quad * 3 + extra / 146097;
    quad = julian / 1461;
    julian -= quad * 1461;
    int y = julian * 4 / 1461;
    julian = ((y != 0) ? ((julian + 305) % 365) : ((julian + 306) % 366)) + 123;
    y += quad * 4;
    *year = y - 4800;
    quad = julian * 2141 / 65536;
    *day = julian - 7834 * quad / 256;
    *month = (quad + 10) % 12 + 1;

    return;
} /* j2date() */

/************************************************************************/
/*                              OGRPGdt2time()                          */
/************************************************************************/

#define USECS_PER_SEC 1000000
#define USECS_PER_MIN ((GIntBig)60 * USECS_PER_SEC)
#define USECS_PER_HOUR ((GIntBig)3600 * USECS_PER_SEC)
#define USECS_PER_DAY ((GIntBig)3600 * 24 * USECS_PER_SEC)

/* Coming from dt2time() in pgsql/src/backend/utils/adt/timestamp.c */

static void OGRPGdt2timeInt8(GIntBig jd, int *hour, int *min, int *sec,
                             double *fsec)
{
    GIntBig time = jd;

    *hour = (int)(time / USECS_PER_HOUR);
    time -= (GIntBig)(*hour) * USECS_PER_HOUR;
    *min = (int)(time / USECS_PER_MIN);
    time -= (GIntBig)(*min) * USECS_PER_MIN;
    *sec = (int)time / USECS_PER_SEC;
    *fsec = (double)(time - *sec * USECS_PER_SEC);
} /* dt2time() */

static void OGRPGdt2timeFloat8(double jd, int *hour, int *min, int *sec,
                               double *fsec)
{
    double time = jd;

    *hour = (int)(time / 3600.);
    time -= (*hour) * 3600.;
    *min = (int)(time / 60.);
    time -= (*min) * 60.;
    *sec = (int)time;
    *fsec = time - *sec;
}

/************************************************************************/
/*                        OGRPGTimeStamp2DMYHMS()                       */
/************************************************************************/

#define TMODULO(t, q, u)                                                       \
    do                                                                         \
    {                                                                          \
        (q) = ((t) / (u));                                                     \
        if ((q) != 0)                                                          \
            (t) -= ((q) * (u));                                                \
    } while (false)

/* Coming from timestamp2tm() in pgsql/src/backend/utils/adt/timestamp.c */

static int OGRPGTimeStamp2DMYHMS(GIntBig dt, int *year, int *month, int *day,
                                 int *hour, int *min, double *pdfSec)
{
    GIntBig time = dt;
    GIntBig date = 0;
    TMODULO(time, date, USECS_PER_DAY);

    if (time < 0)
    {
        time += USECS_PER_DAY;
        date -= 1;
    }

    /* add offset to go from J2000 back to standard Julian date */
    date += POSTGRES_EPOCH_JDATE;

    /* Julian day routine does not work for negative Julian days */
    if (date < 0 || date > (double)INT_MAX)
        return -1;

    OGRPGj2date((int)date, year, month, day);
    int nSec = 0;
    double dfSec = 0.0;
    OGRPGdt2timeInt8(time, hour, min, &nSec, &dfSec);
    *pdfSec += nSec + dfSec;

    return 0;
}

#endif  // defined(BINARY_CURSOR_ENABLED)

/************************************************************************/
/*                   TokenizeStringListFromText()                       */
/*                                                                      */
/* Tokenize a varchar[] returned as a text                              */
/************************************************************************/

static void OGRPGTokenizeStringListUnescapeToken(char *pszToken)
{
    if (EQUAL(pszToken, "NULL"))
    {
        pszToken[0] = '\0';
        return;
    }

    int iSrc = 0, iDst = 0;
    for (iSrc = 0; pszToken[iSrc] != '\0'; iSrc++)
    {
        pszToken[iDst] = pszToken[iSrc];
        if (pszToken[iSrc] != '\\')
            iDst++;
    }
    pszToken[iDst] = '\0';
}

/* {"a\",b",d,NULL,e}  should be tokenized into 3 pieces :  a",b     d
 * empty_string    e */
static char **OGRPGTokenizeStringListFromText(const char *pszText)
{
    char **papszTokens = nullptr;
    const char *pszCur = strchr(pszText, '{');
    if (pszCur == nullptr)
    {
        CPLError(CE_Warning, CPLE_AppDefined, "Incorrect string list : %s",
                 pszText);
        return papszTokens;
    }

    const char *pszNewTokenStart = nullptr;
    int bInDoubleQuotes = FALSE;
    pszCur++;
    while (*pszCur)
    {
        if (*pszCur == '\\')
        {
            pszCur++;
            if (*pszCur == 0)
                break;
            pszCur++;
            continue;
        }

        if (*pszCur == '"')
        {
            bInDoubleQuotes = !bInDoubleQuotes;
            if (bInDoubleQuotes)
                pszNewTokenStart = pszCur + 1;
            else
            {
                if (pszCur[1] == ',' || pszCur[1] == '}')
                {
                    if (pszNewTokenStart != nullptr &&
                        pszCur > pszNewTokenStart)
                    {
                        char *pszNewToken = static_cast<char *>(
                            CPLMalloc(pszCur - pszNewTokenStart + 1));
                        memcpy(pszNewToken, pszNewTokenStart,
                               pszCur - pszNewTokenStart);
                        pszNewToken[pszCur - pszNewTokenStart] = 0;
                        OGRPGTokenizeStringListUnescapeToken(pszNewToken);
                        papszTokens = CSLAddString(papszTokens, pszNewToken);
                        CPLFree(pszNewToken);
                    }
                    pszNewTokenStart = nullptr;
                    if (pszCur[1] == ',')
                        pszCur++;
                    else
                        return papszTokens;
                }
                else
                {
                    /* error */
                    break;
                }
            }
        }
        if (!bInDoubleQuotes)
        {
            if (*pszCur == '{')
            {
                /* error */
                break;
            }
            else if (*pszCur == '}')
            {
                if (pszNewTokenStart != nullptr && pszCur > pszNewTokenStart)
                {
                    char *pszNewToken = static_cast<char *>(
                        CPLMalloc(pszCur - pszNewTokenStart + 1));
                    memcpy(pszNewToken, pszNewTokenStart,
                           pszCur - pszNewTokenStart);
                    pszNewToken[pszCur - pszNewTokenStart] = 0;
                    OGRPGTokenizeStringListUnescapeToken(pszNewToken);
                    papszTokens = CSLAddString(papszTokens, pszNewToken);
                    CPLFree(pszNewToken);
                }
                return papszTokens;
            }
            else if (*pszCur == ',')
            {
                if (pszNewTokenStart != nullptr && pszCur > pszNewTokenStart)
                {
                    char *pszNewToken = static_cast<char *>(
                        CPLMalloc(pszCur - pszNewTokenStart + 1));
                    memcpy(pszNewToken, pszNewTokenStart,
                           pszCur - pszNewTokenStart);
                    pszNewToken[pszCur - pszNewTokenStart] = 0;
                    OGRPGTokenizeStringListUnescapeToken(pszNewToken);
                    papszTokens = CSLAddString(papszTokens, pszNewToken);
                    CPLFree(pszNewToken);
                }
                pszNewTokenStart = pszCur + 1;
            }
            else if (pszNewTokenStart == nullptr)
                pszNewTokenStart = pszCur;
        }
        pszCur++;
    }

    CPLError(CE_Warning, CPLE_AppDefined, "Incorrect string list : %s",
             pszText);
    return papszTokens;
}

/************************************************************************/
/*                          RecordToFeature()                           */
/*                                                                      */
/*      Convert the indicated record of the current result set into     */
/*      a feature.                                                      */
/************************************************************************/

OGRFeature *OGRPGLayer::RecordToFeature(PGresult *hResult,
                                        const int *panMapFieldNameToIndex,
                                        const int *panMapFieldNameToGeomIndex,
                                        int iRecord)

{
    /* -------------------------------------------------------------------- */
    /*      Create a feature from the current result.                       */
    /* -------------------------------------------------------------------- */
    OGRFeature *poFeature = new OGRFeature(poFeatureDefn);

    poFeature->SetFID(iNextShapeId);
    m_nFeaturesRead++;

    /* ==================================================================== */
    /*      Transfer all result fields we can.                              */
    /* ==================================================================== */
    for (int iField = 0; iField < PQnfields(hResult); iField++)
    {
#if defined(BINARY_CURSOR_ENABLED)
        const Oid nTypeOID = PQftype(hResult, iField);
#endif
        const char *pszFieldName = PQfname(hResult, iField);

        /* --------------------------------------------------------------------
         */
        /*      Handle FID. */
        /* --------------------------------------------------------------------
         */
        if (pszFIDColumn != nullptr && EQUAL(pszFieldName, pszFIDColumn))
        {
#if defined(BINARY_CURSOR_ENABLED)
            if (PQfformat(hResult, iField) == 1)  // Binary data representation
            {
                if (nTypeOID == INT4OID)
                {
                    int nVal = 0;
                    CPLAssert(PQgetlength(hResult, iRecord, iField) ==
                              sizeof(int));
                    memcpy(&nVal, PQgetvalue(hResult, iRecord, iField),
                           sizeof(int));
                    CPL_MSBPTR32(&nVal);
                    poFeature->SetFID(nVal);
                }
                else if (nTypeOID == INT8OID)
                {
                    GIntBig nVal = 0;
                    CPLAssert(PQgetlength(hResult, iRecord, iField) ==
                              sizeof(GIntBig));
                    memcpy(&nVal, PQgetvalue(hResult, iRecord, iField),
                           sizeof(GIntBig));
                    CPL_MSBPTR64(&nVal);
                    poFeature->SetFID(nVal);
                }
                else
                {
                    CPLDebug("PG", "FID. Unhandled OID %d.", nTypeOID);
                    continue;
                }
            }
            else
#endif /* defined(BINARY_CURSOR_ENABLED) */
            {
                char *pabyData = PQgetvalue(hResult, iRecord, iField);
                /* ogr_pg_20 may crash if PostGIS is unavailable and we don't
                 * test pabyData */
                if (pabyData)
                    poFeature->SetFID(CPLAtoGIntBig(pabyData));
                else
                    continue;
            }
        }

        /* --------------------------------------------------------------------
         */
        /*      Handle PostGIS geometry */
        /* --------------------------------------------------------------------
         */
        int iOGRGeomField = panMapFieldNameToGeomIndex[iField];
        OGRPGGeomFieldDefn *poGeomFieldDefn = nullptr;
        if (iOGRGeomField >= 0)
            poGeomFieldDefn = poFeatureDefn->GetGeomFieldDefn(iOGRGeomField);
        if (poGeomFieldDefn &&
            (poGeomFieldDefn->ePostgisType == GEOM_TYPE_GEOMETRY ||
             poGeomFieldDefn->ePostgisType == GEOM_TYPE_GEOGRAPHY))
        {
            if (STARTS_WITH_CI(pszFieldName, "ST_AsBinary") ||
                STARTS_WITH_CI(pszFieldName, "AsBinary"))
            {
                const char *pszVal = PQgetvalue(hResult, iRecord, iField);

                int nLength = PQgetlength(hResult, iRecord, iField);

                /* No geometry */
                if (nLength == 0)
                    continue;

                OGRGeometry *poGeom = nullptr;
                if (!poDS->bUseBinaryCursor && nLength >= 4 &&
                    /* escaped byea data */
                    (STARTS_WITH(pszVal, "\\000") ||
                     STARTS_WITH(pszVal, "\\001") ||
                     /* hex bytea data (PostgreSQL >= 9.0) */
                     STARTS_WITH(pszVal, "\\x00") ||
                     STARTS_WITH(pszVal, "\\x01")))
                {
                    poGeom = BYTEAToGeometry(pszVal);
                }
                else
                {
                    const GByte *pabyVal =
                        reinterpret_cast<const GByte *>(pszVal);
                    OGRGeometryFactory::createFromWkb(
                        pabyVal, nullptr, &poGeom, nLength, wkbVariantOldOgc);
                }

                if (poGeom != nullptr)
                {
                    poGeom->assignSpatialReference(
                        poGeomFieldDefn->GetSpatialRef());
                    poFeature->SetGeomFieldDirectly(iOGRGeomField, poGeom);
                }

                continue;
            }
            else if (!poDS->bUseBinaryCursor &&
                     STARTS_WITH_CI(pszFieldName, "EWKBBase64"))
            {
                const GByte *pabyData = reinterpret_cast<const GByte *>(
                    PQgetvalue(hResult, iRecord, iField));

                int nLength = PQgetlength(hResult, iRecord, iField);

                /* No geometry */
                if (nLength == 0)
                    continue;

                // Potentially dangerous to modify the result of PQgetvalue...
                nLength = CPLBase64DecodeInPlace(const_cast<GByte *>(pabyData));
                OGRGeometry *poGeom = OGRGeometryFromEWKB(
                    const_cast<GByte *>(pabyData), nLength, nullptr, false);

                if (poGeom != nullptr)
                {
                    poGeom->assignSpatialReference(
                        poGeomFieldDefn->GetSpatialRef());
                    poFeature->SetGeomFieldDirectly(iOGRGeomField, poGeom);
                }

                continue;
            }
            else if (poDS->bUseBinaryCursor ||
                     EQUAL(pszFieldName, "ST_AsEWKB") ||
                     EQUAL(pszFieldName, "AsEWKB"))
            {
                /* Handle HEX result or EWKB binary cursor result */
                const char *pabyData = PQgetvalue(hResult, iRecord, iField);

                int nLength = PQgetlength(hResult, iRecord, iField);

                /* No geometry */
                if (nLength == 0)
                    continue;

                OGRGeometry *poGeom = nullptr;

                if (!poDS->bUseBinaryCursor &&
                    (STARTS_WITH(pabyData, "\\x00") ||
                     STARTS_WITH(pabyData, "\\x01") ||
                     STARTS_WITH(pabyData, "\\000") ||
                     STARTS_WITH(pabyData, "\\001")))
                {
                    GByte *pabyEWKB = BYTEAToGByteArray(pabyData, &nLength);
                    poGeom =
                        OGRGeometryFromEWKB(pabyEWKB, nLength, nullptr, false);
                    CPLFree(pabyEWKB);
                }
                else if (nLength >= 2 && (STARTS_WITH_CI(pabyData, "00") ||
                                          STARTS_WITH_CI(pabyData, "01")))
                {
                    poGeom = OGRGeometryFromHexEWKB(pabyData, nullptr, false);
                }
                else
                {
                    // Potentially dangerous to modify the result of
                    // PQgetvalue...
                    poGeom = OGRGeometryFromEWKB(
                        const_cast<GByte *>(
                            reinterpret_cast<const GByte *>(pabyData)),
                        nLength, nullptr, false);
                }

                if (poGeom != nullptr)
                {
                    poGeom->assignSpatialReference(
                        poGeomFieldDefn->GetSpatialRef());
                    poFeature->SetGeomFieldDirectly(iOGRGeomField, poGeom);
                }

                continue;
            }
            else /*if (EQUAL(pszFieldName,"asEWKT") ||
                     EQUAL(pszFieldName,"asText") ||
                     EQUAL(pszFieldName,"ST_AsEWKT") ||
                     EQUAL(pszFieldName,"ST_AsText") )*/
            {
                /* Handle WKT */
                const char *pszWKT = PQgetvalue(hResult, iRecord, iField);
                const char *pszPostSRID = pszWKT;

                // optionally strip off PostGIS SRID identifier.  This
                // happens if we got a raw geometry field.
                if (STARTS_WITH_CI(pszPostSRID, "SRID="))
                {
                    while (*pszPostSRID != '\0' && *pszPostSRID != ';')
                        pszPostSRID++;
                    if (*pszPostSRID == ';')
                        pszPostSRID++;
                }

                OGRGeometry *poGeometry = nullptr;
                if (STARTS_WITH_CI(pszPostSRID, "00") ||
                    STARTS_WITH_CI(pszPostSRID, "01"))
                {
                    poGeometry = OGRGeometryFromHexEWKB(pszWKT, nullptr, false);
                }
                else
                    OGRGeometryFactory::createFromWkt(pszPostSRID, nullptr,
                                                      &poGeometry);
                if (poGeometry != nullptr)
                {
                    poGeometry->assignSpatialReference(
                        poGeomFieldDefn->GetSpatialRef());
                    poFeature->SetGeomFieldDirectly(iOGRGeomField, poGeometry);
                }

                continue;
            }
        }
        /* --------------------------------------------------------------------
         */
        /*      Handle raw binary geometry ... this hasn't been tested in a */
        /*      while. */
        /* --------------------------------------------------------------------
         */
        else if (poGeomFieldDefn &&
                 poGeomFieldDefn->ePostgisType == GEOM_TYPE_WKB)
        {
            OGRGeometry *poGeometry = nullptr;
            const char *pszData = PQgetvalue(hResult, iRecord, iField);

            if (bWkbAsOid)
            {
                poGeometry = OIDToGeometry(static_cast<Oid>(atoi(pszData)));
            }
            else
            {
#if defined(BINARY_CURSOR_ENABLED)
                if (poDS->bUseBinaryCursor && PQfformat(hResult, iField) == 1)
                {
                    int nLength = PQgetlength(hResult, iRecord, iField);
                    const GByte *pabyData =
                        reinterpret_cast<const GByte *>(pszData);
                    poGeometry =
                        OGRGeometryFromEWKB(pabyData, nLength, NULL, false);
                }
                if (poGeometry == nullptr)
#endif
                {
                    poGeometry = BYTEAToGeometry(pszData);
                }
            }

            if (poGeometry != nullptr)
            {
                poGeometry->assignSpatialReference(
                    poGeomFieldDefn->GetSpatialRef());
                poFeature->SetGeomFieldDirectly(iOGRGeomField, poGeometry);
            }

            continue;
        }

        /* --------------------------------------------------------------------
         */
        /*      Transfer regular data fields. */
        /* --------------------------------------------------------------------
         */
        const int iOGRField = panMapFieldNameToIndex[iField];

        if (iOGRField < 0)
            continue;

        if (PQgetisnull(hResult, iRecord, iField))
        {
            poFeature->SetFieldNull(iOGRField);
            continue;
        }

        OGRFieldType eOGRType =
            poFeatureDefn->GetFieldDefn(iOGRField)->GetType();

        if (eOGRType == OFTIntegerList)
        {
            int *panList, nCount, i;

#if defined(BINARY_CURSOR_ENABLED)
            if (PQfformat(hResult, iField) == 1)  // Binary data representation
            {
                if (nTypeOID == INT2ARRAYOID || nTypeOID == INT4ARRAYOID)
                {
                    char *pData = PQgetvalue(hResult, iRecord, iField);

                    // goto number of array elements
                    pData += 3 * sizeof(int);
                    memcpy(&nCount, pData, sizeof(int));
                    CPL_MSBPTR32(&nCount);

                    panList =
                        static_cast<int *>(CPLCalloc(sizeof(int), nCount));

                    // goto first array element
                    pData += 2 * sizeof(int);

                    for (i = 0; i < nCount; i++)
                    {
                        // get element size
                        int nSize = *(int *)(pData);
                        CPL_MSBPTR32(&nSize);
                        pData += sizeof(int);

                        if (nTypeOID == INT4ARRAYOID)
                        {
                            CPLAssert(nSize == sizeof(int));
                            memcpy(&panList[i], pData, nSize);
                            CPL_MSBPTR32(&panList[i]);
                        }
                        else
                        {
                            CPLAssert(nSize == sizeof(GInt16));
                            GInt16 nVal = 0;
                            memcpy(&nVal, pData, nSize);
                            CPL_MSBPTR16(&nVal);
                            panList[i] = nVal;
                        }

                        pData += nSize;
                    }
                }
                else
                {
                    CPLDebug(
                        "PG",
                        "Field %d: Incompatible OID (%d) with OFTIntegerList.",
                        iOGRField, nTypeOID);
                    continue;
                }
            }
            else
#endif
            {
                char **papszTokens = CSLTokenizeStringComplex(
                    PQgetvalue(hResult, iRecord, iField), "{,}", FALSE, FALSE);

                nCount = CSLCount(papszTokens);
                panList = static_cast<int *>(CPLCalloc(sizeof(int), nCount));

                if (poFeatureDefn->GetFieldDefn(iOGRField)->GetSubType() ==
                    OFSTBoolean)
                {
                    for (i = 0; i < nCount; i++)
                        panList[i] = EQUAL(papszTokens[i], "t");
                }
                else
                {
                    for (i = 0; i < nCount; i++)
                        panList[i] = atoi(papszTokens[i]);
                }
                CSLDestroy(papszTokens);
            }
            poFeature->SetField(iOGRField, nCount, panList);
            CPLFree(panList);
        }

        else if (eOGRType == OFTInteger64List)
        {
            int nCount = 0;
            GIntBig *panList = nullptr;

#if defined(BINARY_CURSOR_ENABLED)
            if (PQfformat(hResult, iField) == 1)  // Binary data representation
            {
                if (nTypeOID == INT8ARRAYOID)
                {
                    char *pData = PQgetvalue(hResult, iRecord, iField);

                    // goto number of array elements
                    pData += 3 * sizeof(int);
                    memcpy(&nCount, pData, sizeof(int));
                    CPL_MSBPTR32(&nCount);

                    panList = static_cast<GIntBig *>(
                        CPLCalloc(sizeof(GIntBig), nCount));

                    // goto first array element
                    pData += 2 * sizeof(int);

                    for (int i = 0; i < nCount; i++)
                    {
                        // get element size
                        int nSize = *(int *)(pData);
                        CPL_MSBPTR32(&nSize);

                        CPLAssert(nSize == sizeof(GIntBig));

                        pData += sizeof(int);

                        memcpy(&panList[i], pData, nSize);
                        CPL_MSBPTR64(&panList[i]);

                        pData += nSize;
                    }
                }
                else
                {
                    CPLDebug("PG",
                             "Field %d: Incompatible OID (%d) with "
                             "OFTInteger64List.",
                             iOGRField, nTypeOID);
                    continue;
                }
            }
            else
#endif
            {
                char **papszTokens = CSLTokenizeStringComplex(
                    PQgetvalue(hResult, iRecord, iField), "{,}", FALSE, FALSE);

                nCount = CSLCount(papszTokens);
                panList =
                    static_cast<GIntBig *>(CPLCalloc(sizeof(GIntBig), nCount));

                if (poFeatureDefn->GetFieldDefn(iOGRField)->GetSubType() ==
                    OFSTBoolean)
                {
                    for (int i = 0; i < nCount; i++)
                        panList[i] = EQUAL(papszTokens[i], "t");
                }
                else
                {
                    for (int i = 0; i < nCount; i++)
                        panList[i] = CPLAtoGIntBig(papszTokens[i]);
                }
                CSLDestroy(papszTokens);
            }
            poFeature->SetField(iOGRField, nCount, panList);
            CPLFree(panList);
        }

        else if (eOGRType == OFTRealList)
        {
            int nCount, i;
            double *padfList = nullptr;

#if defined(BINARY_CURSOR_ENABLED)
            if (PQfformat(hResult, iField) == 1)  // Binary data representation
            {
                if (nTypeOID == FLOAT8ARRAYOID || nTypeOID == FLOAT4ARRAYOID)
                {
                    char *pData = PQgetvalue(hResult, iRecord, iField);

                    // goto number of array elements
                    pData += 3 * sizeof(int);
                    memcpy(&nCount, pData, sizeof(int));
                    CPL_MSBPTR32(&nCount);

                    padfList = static_cast<double *>(
                        CPLCalloc(sizeof(double), nCount));

                    // goto first array element
                    pData += 2 * sizeof(int);

                    for (i = 0; i < nCount; i++)
                    {
                        // get element size
                        int nSize = *(int *)(pData);
                        CPL_MSBPTR32(&nSize);

                        pData += sizeof(int);

                        if (nTypeOID == FLOAT8ARRAYOID)
                        {
                            CPLAssert(nSize == sizeof(double));

                            memcpy(&padfList[i], pData, nSize);
                            CPL_MSBPTR64(&padfList[i]);
                        }
                        else
                        {
                            CPLAssert(nSize == sizeof(float));

                            float fVal = 0.0f;
                            memcpy(&fVal, pData, nSize);
                            CPL_MSBPTR32(&fVal);

                            padfList[i] = fVal;
                        }

                        pData += nSize;
                    }
                }
                else
                {
                    CPLDebug(
                        "PG",
                        "Field %d: Incompatible OID (%d) with OFTRealList.",
                        iOGRField, nTypeOID);
                    continue;
                }
            }
            else
#endif
            {
                char **papszTokens = CSLTokenizeStringComplex(
                    PQgetvalue(hResult, iRecord, iField), "{,}", FALSE, FALSE);

                nCount = CSLCount(papszTokens);
                padfList =
                    static_cast<double *>(CPLCalloc(sizeof(double), nCount));

                for (i = 0; i < nCount; i++)
                    padfList[i] = CPLAtof(papszTokens[i]);
                CSLDestroy(papszTokens);
            }

            poFeature->SetField(iOGRField, nCount, padfList);
            CPLFree(padfList);
        }

        else if (eOGRType == OFTStringList)
        {
            char **papszTokens = nullptr;

#if defined(BINARY_CURSOR_ENABLED)
            if (PQfformat(hResult, iField) == 1)  // Binary data representation
            {
                char *pData = PQgetvalue(hResult, iRecord, iField);
                int nCount, i;

                // goto number of array elements
                pData += 3 * sizeof(int);
                memcpy(&nCount, pData, sizeof(int));
                CPL_MSBPTR32(&nCount);

                // goto first array element
                pData += 2 * sizeof(int);

                for (i = 0; i < nCount; i++)
                {
                    // get element size
                    int nSize = *(int *)(pData);
                    CPL_MSBPTR32(&nSize);

                    pData += sizeof(int);

                    if (nSize <= 0)
                        papszTokens = CSLAddString(papszTokens, "");
                    else
                    {
                        if (pData[nSize] == '\0')
                            papszTokens = CSLAddString(papszTokens, pData);
                        else
                        {
                            char *pszToken = (char *)CPLMalloc(nSize + 1);
                            memcpy(pszToken, pData, nSize);
                            pszToken[nSize] = '\0';
                            papszTokens = CSLAddString(papszTokens, pszToken);
                            CPLFree(pszToken);
                        }

                        pData += nSize;
                    }
                }
            }
            else
#endif
            {
                papszTokens = OGRPGTokenizeStringListFromText(
                    PQgetvalue(hResult, iRecord, iField));
            }

            if (papszTokens)
            {
                poFeature->SetField(iOGRField, papszTokens);
                CSLDestroy(papszTokens);
            }
        }

        else if (eOGRType == OFTDate || eOGRType == OFTTime ||
                 eOGRType == OFTDateTime)
        {
#if defined(BINARY_CURSOR_ENABLED)
            if (PQfformat(hResult, iField) == 1)  // Binary data
            {
                if (nTypeOID == DATEOID)
                {
                    int nVal, nYear, nMonth, nDay;
                    CPLAssert(PQgetlength(hResult, iRecord, iField) ==
                              sizeof(int));
                    memcpy(&nVal, PQgetvalue(hResult, iRecord, iField),
                           sizeof(int));
                    CPL_MSBPTR32(&nVal);
                    OGRPGj2date(nVal + POSTGRES_EPOCH_JDATE, &nYear, &nMonth,
                                &nDay);
                    poFeature->SetField(iOGRField, nYear, nMonth, nDay);
                }
                else if (nTypeOID == TIMEOID)
                {
                    int nHour = 0;
                    int nMinute = 0;
                    int nSecond = 0;
                    char szTime[32];
                    double dfsec = 0.0f;
                    CPLAssert(PQgetlength(hResult, iRecord, iField) == 8);
                    if (poDS->bBinaryTimeFormatIsInt8)
                    {
                        unsigned int nVal[2];
                        GIntBig llVal = 0;
                        memcpy(nVal, PQgetvalue(hResult, iRecord, iField), 8);
                        CPL_MSBPTR32(&nVal[0]);
                        CPL_MSBPTR32(&nVal[1]);
                        llVal =
                            (GIntBig)((((GUIntBig)nVal[0]) << 32) | nVal[1]);
                        OGRPGdt2timeInt8(llVal, &nHour, &nMinute, &nSecond,
                                         &dfsec);
                    }
                    else
                    {
                        double dfVal = 0.0;
                        memcpy(&dfVal, PQgetvalue(hResult, iRecord, iField), 8);
                        CPL_MSBPTR64(&dfVal);
                        OGRPGdt2timeFloat8(dfVal, &nHour, &nMinute, &nSecond,
                                           &dfsec);
                    }
                    snprintf(szTime, sizeof(szTime), "%02d:%02d:%02d", nHour,
                             nMinute, nSecond);
                    poFeature->SetField(iOGRField, szTime);
                }
                else if (nTypeOID == TIMESTAMPOID || nTypeOID == TIMESTAMPTZOID)
                {
                    unsigned int nVal[2];
                    GIntBig llVal = 0;
                    int nYear = 0;
                    int nMonth = 0;
                    int nDay = 0;
                    int nHour = 0;
                    int nMinute = 0;
                    double dfSecond = 0.0;
                    CPLAssert(PQgetlength(hResult, iRecord, iField) == 8);
                    memcpy(nVal, PQgetvalue(hResult, iRecord, iField), 8);
                    CPL_MSBPTR32(&nVal[0]);
                    CPL_MSBPTR32(&nVal[1]);
                    llVal = (GIntBig)((((GUIntBig)nVal[0]) << 32) | nVal[1]);
                    if (OGRPGTimeStamp2DMYHMS(llVal, &nYear, &nMonth, &nDay,
                                              &nHour, &nMinute, &dfSecond) == 0)
                        poFeature->SetField(iOGRField, nYear, nMonth, nDay,
                                            nHour, nMinute, (float)dfSecond,
                                            100);
                }
                else if (nTypeOID == TEXTOID)
                {
                    OGRField sFieldValue;

                    if (OGRParseDate(PQgetvalue(hResult, iRecord, iField),
                                     &sFieldValue, 0))
                    {
                        poFeature->SetField(iOGRField, &sFieldValue);
                    }
                }
                else
                {
                    CPLDebug("PG",
                             "Binary DATE format not yet implemented. OID = %d",
                             nTypeOID);
                }
            }
            else
#endif
            {
                OGRField sFieldValue;

                if (OGRParseDate(PQgetvalue(hResult, iRecord, iField),
                                 &sFieldValue, 0))
                {
                    poFeature->SetField(iOGRField, &sFieldValue);
                }
            }
        }
        else if (eOGRType == OFTBinary)
        {
#if defined(BINARY_CURSOR_ENABLED)
            if (PQfformat(hResult, iField) == 1)
            {
                int nLength = PQgetlength(hResult, iRecord, iField);
                GByte *pabyData = reinterpret_cast<GByte *>(
                    PQgetvalue(hResult, iRecord, iField));
                poFeature->SetField(iOGRField, nLength, pabyData);
            }
            else
#endif /* defined(BINARY_CURSOR_ENABLED) */
            {
                int nLength = PQgetlength(hResult, iRecord, iField);
                const char *pszBytea = PQgetvalue(hResult, iRecord, iField);
                GByte *pabyData = BYTEAToGByteArray(pszBytea, &nLength);
                poFeature->SetField(iOGRField, nLength, pabyData);
                CPLFree(pabyData);
            }
        }
        else
        {
#if defined(BINARY_CURSOR_ENABLED)
            if (PQfformat(hResult, iField) == 1 &&
                eOGRType != OFTString)  // Binary data
            {
                if (nTypeOID == BOOLOID)
                {
                    CPLAssert(PQgetlength(hResult, iRecord, iField) ==
                              sizeof(char));
                    const char cVal = *PQgetvalue(hResult, iRecord, iField);
                    poFeature->SetField(iOGRField, cVal);
                }
                else if (nTypeOID == NUMERICOID)
                {
                    char *pabyData = PQgetvalue(hResult, iRecord, iField);
                    unsigned short sLen = 0;
                    memcpy(&sLen, pabyData, sizeof(short));
                    pabyData += sizeof(short);
                    CPL_MSBPTR16(&sLen);
                    short sWeight = 0;
                    memcpy(&sWeight, pabyData, sizeof(short));
                    pabyData += sizeof(short);
                    CPL_MSBPTR16(&sWeight);
                    unsigned short sSign = 0;
                    memcpy(&sSign, pabyData, sizeof(short));
                    pabyData += sizeof(short);
                    CPL_MSBPTR16(&sSign);
                    unsigned short sDscale = 0;
                    memcpy(&sDscale, pabyData, sizeof(short));
                    pabyData += sizeof(short);
                    CPL_MSBPTR16(&sDscale);
                    CPLAssert(PQgetlength(hResult, iRecord, iField) ==
                              (int)((4 + sLen) * sizeof(short)));

                    NumericVar var;
                    var.ndigits = sLen;
                    var.weight = sWeight;
                    var.sign = sSign;
                    var.dscale = sDscale;
                    var.digits = (NumericDigit *)pabyData;
                    char *str = OGRPGGetStrFromBinaryNumeric(&var);
                    poFeature->SetField(iOGRField, CPLAtof(str));
                    CPLFree(str);
                }
                else if (nTypeOID == INT2OID)
                {
                    CPLAssert(PQgetlength(hResult, iRecord, iField) ==
                              sizeof(short));
                    short sVal = 0;
                    memcpy(&sVal, PQgetvalue(hResult, iRecord, iField),
                           sizeof(short));
                    CPL_MSBPTR16(&sVal);
                    poFeature->SetField(iOGRField, sVal);
                }
                else if (nTypeOID == INT4OID)
                {
                    CPLAssert(PQgetlength(hResult, iRecord, iField) ==
                              sizeof(int));
                    int nVal = 0;
                    memcpy(&nVal, PQgetvalue(hResult, iRecord, iField),
                           sizeof(int));
                    CPL_MSBPTR32(&nVal);
                    poFeature->SetField(iOGRField, nVal);
                }
                else if (nTypeOID == INT8OID)
                {
                    CPLAssert(PQgetlength(hResult, iRecord, iField) == 8);
                    unsigned int nVal[2] = {0, 0};
                    memcpy(nVal, PQgetvalue(hResult, iRecord, iField), 8);
                    CPL_MSBPTR32(&nVal[0]);
                    CPL_MSBPTR32(&nVal[1]);
                    GIntBig llVal =
                        (GIntBig)((((GUIntBig)nVal[0]) << 32) | nVal[1]);
                    poFeature->SetField(iOGRField, llVal);
                }
                else if (nTypeOID == FLOAT4OID)
                {
                    CPLAssert(PQgetlength(hResult, iRecord, iField) ==
                              sizeof(float));
                    float fVal = 0.0f;
                    memcpy(&fVal, PQgetvalue(hResult, iRecord, iField),
                           sizeof(float));
                    CPL_MSBPTR32(&fVal);
                    poFeature->SetField(iOGRField, fVal);
                }
                else if (nTypeOID == FLOAT8OID)
                {
                    CPLAssert(PQgetlength(hResult, iRecord, iField) ==
                              sizeof(double));
                    double dfVal = 0.0;
                    memcpy(&dfVal, PQgetvalue(hResult, iRecord, iField),
                           sizeof(double));
                    CPL_MSBPTR64(&dfVal);
                    poFeature->SetField(iOGRField, dfVal);
                }
                else
                {
                    CPLDebug(
                        "PG", "Field %d(%s): Incompatible OID (%d) with %s.",
                        iOGRField,
                        poFeatureDefn->GetFieldDefn(iOGRField)->GetNameRef(),
                        nTypeOID, OGRFieldDefn::GetFieldTypeName(eOGRType));
                    continue;
                }
            }
            else
#endif /* defined(BINARY_CURSOR_ENABLED) */
            {
                if (eOGRType == OFTInteger &&
                    poFeatureDefn->GetFieldDefn(iOGRField)->GetWidth() == 1)
                {
                    char *pabyData = PQgetvalue(hResult, iRecord, iField);
                    if (STARTS_WITH_CI(pabyData, "T"))
                        poFeature->SetField(iOGRField, 1);
                    else if (STARTS_WITH_CI(pabyData, "F"))
                        poFeature->SetField(iOGRField, 0);
                    else
                    {
                        // coverity[tainted_data]
                        poFeature->SetField(iOGRField, pabyData);
                    }
                }
                else if (eOGRType == OFTReal)
                {
                    poFeature->SetField(
                        iOGRField,
                        CPLAtof(PQgetvalue(hResult, iRecord, iField)));
                }
                else
                {
                    poFeature->SetField(iOGRField,
                                        PQgetvalue(hResult, iRecord, iField));
                }
            }
        }
    }

    return poFeature;
}

/************************************************************************/
/*                    OGRPGIsKnownGeomFuncPrefix()                      */
/************************************************************************/

static const char *const apszKnownGeomFuncPrefixes[] = {
    "ST_AsBinary", "ST_AsEWKT", "ST_AsEWKB", "EWKBBase64", "ST_AsText",
    "AsBinary",    "asEWKT",    "asEWKB",    "asText"};

static int OGRPGIsKnownGeomFuncPrefix(const char *pszFieldName)
{
    for (size_t i = 0; i < sizeof(apszKnownGeomFuncPrefixes) / sizeof(char *);
         i++)
    {
        if (EQUALN(pszFieldName, apszKnownGeomFuncPrefixes[i],
                   static_cast<int>(strlen(apszKnownGeomFuncPrefixes[i]))))
            return static_cast<int>(i);
    }
    return -1;
}

/************************************************************************/
/*                CreateMapFromFieldNameToIndex()                       */
/************************************************************************/

/* Evaluating GetFieldIndex() on each field of each feature can be very */
/* expensive if the layer has many fields (total complexity of O(n^2) where */
/* n is the number of fields), so it is valuable to compute the map from */
/* the fetched fields to the OGR field index */
void OGRPGLayer::CreateMapFromFieldNameToIndex(PGresult *hResult,
                                               OGRFeatureDefn *poFeatureDefn,
                                               int *&panMapFieldNameToIndex,
                                               int *&panMapFieldNameToGeomIndex)
{
    CPLFree(panMapFieldNameToIndex);
    panMapFieldNameToIndex = nullptr;
    CPLFree(panMapFieldNameToGeomIndex);
    panMapFieldNameToGeomIndex = nullptr;
    if (PQresultStatus(hResult) == PGRES_TUPLES_OK)
    {
        panMapFieldNameToIndex =
            static_cast<int *>(CPLMalloc(sizeof(int) * PQnfields(hResult)));
        panMapFieldNameToGeomIndex =
            static_cast<int *>(CPLMalloc(sizeof(int) * PQnfields(hResult)));
        for (int iField = 0; iField < PQnfields(hResult); iField++)
        {
            const char *pszName = PQfname(hResult, iField);
            panMapFieldNameToIndex[iField] =
                poFeatureDefn->GetFieldIndex(pszName);
            if (panMapFieldNameToIndex[iField] < 0)
            {
                panMapFieldNameToGeomIndex[iField] =
                    poFeatureDefn->GetGeomFieldIndex(pszName);
                if (panMapFieldNameToGeomIndex[iField] < 0)
                {
                    int iKnownPrefix = OGRPGIsKnownGeomFuncPrefix(pszName);
                    if (iKnownPrefix >= 0 &&
                        pszName[strlen(
                            apszKnownGeomFuncPrefixes[iKnownPrefix])] == '_')
                    {
                        panMapFieldNameToGeomIndex[iField] =
                            poFeatureDefn->GetGeomFieldIndex(
                                pszName +
                                strlen(
                                    apszKnownGeomFuncPrefixes[iKnownPrefix]) +
                                1);
                    }
                }
            }
            else
                panMapFieldNameToGeomIndex[iField] = -1;
        }
    }
}

/************************************************************************/
/*                     SetInitialQueryCursor()                          */
/************************************************************************/

void OGRPGLayer::SetInitialQueryCursor()
{
    PGconn *hPGConn = poDS->GetPGConn();
    CPLString osCommand;

    CPLAssert(pszQueryStatement != nullptr);

    poDS->SoftStartTransaction();

#if defined(BINARY_CURSOR_ENABLED)
    if (poDS->bUseBinaryCursor && bCanUseBinaryCursor)
        osCommand.Printf("DECLARE %s BINARY CURSOR for %s", pszCursorName,
                         pszQueryStatement);
    else
#endif
        osCommand.Printf("DECLARE %s CURSOR for %s", pszCursorName,
                         pszQueryStatement);

    hCursorResult = OGRPG_PQexec(hPGConn, osCommand);
    if (!hCursorResult || PQresultStatus(hCursorResult) != PGRES_COMMAND_OK)
    {
        CPLError(CE_Failure, CPLE_AppDefined, "%s", PQerrorMessage(hPGConn));
        poDS->SoftRollbackTransaction();
    }
    OGRPGClearResult(hCursorResult);

    osCommand.Printf("FETCH %d in %s", nCursorPage, pszCursorName);
    hCursorResult = OGRPG_PQexec(hPGConn, osCommand);

    CreateMapFromFieldNameToIndex(hCursorResult, poFeatureDefn,
                                  m_panMapFieldNameToIndex,
                                  m_panMapFieldNameToGeomIndex);

    nResultOffset = 0;
}

/************************************************************************/
/*                         GetNextRawFeature()                          */
/************************************************************************/

OGRFeature *OGRPGLayer::GetNextRawFeature()

{
    PGconn *hPGConn = poDS->GetPGConn();
    CPLString osCommand;

    if (bInvalidated)
    {
        CPLError(CE_Failure, CPLE_AppDefined,
                 "Cursor used to read layer has been closed due to a COMMIT. "
                 "ResetReading() must be explicitly called to restart reading");
        return nullptr;
    }

    /* -------------------------------------------------------------------- */
    /*      Do we need to establish an initial query?                       */
    /* -------------------------------------------------------------------- */
    if (iNextShapeId == 0 && hCursorResult == nullptr)
    {
        SetInitialQueryCursor();
    }

    /* -------------------------------------------------------------------- */
    /*      Are we in some sort of error condition?                         */
    /* -------------------------------------------------------------------- */
    if (hCursorResult == nullptr ||
        PQresultStatus(hCursorResult) != PGRES_TUPLES_OK)
    {
        CPLDebug("PG", "PQclear() on an error condition");

        OGRPGClearResult(hCursorResult);

        iNextShapeId = MAX(1, iNextShapeId);
        return nullptr;
    }

    /* -------------------------------------------------------------------- */
    /*      Do we need to fetch more records?                               */
    /* -------------------------------------------------------------------- */

    /* We test for PQntuples(hCursorResult) == 1 in the case the previous */
    /* request was a SetNextByIndex() */
    if ((PQntuples(hCursorResult) == 1 ||
         PQntuples(hCursorResult) == nCursorPage) &&
        nResultOffset == PQntuples(hCursorResult))
    {
        OGRPGClearResult(hCursorResult);

        osCommand.Printf("FETCH %d in %s", nCursorPage, pszCursorName);
        hCursorResult = OGRPG_PQexec(hPGConn, osCommand);

        nResultOffset = 0;
    }

    /* -------------------------------------------------------------------- */
    /*      Are we out of results?  If so complete the transaction, and     */
    /*      cleanup, but don't reset the next shapeid.                      */
    /* -------------------------------------------------------------------- */
    if (nResultOffset == PQntuples(hCursorResult))
    {
        CloseCursor();

        iNextShapeId = MAX(1, iNextShapeId);

        return nullptr;
    }

    /* -------------------------------------------------------------------- */
    /*      Create a feature from the current result.                       */
    /* -------------------------------------------------------------------- */
    OGRFeature *poFeature =
        RecordToFeature(hCursorResult, m_panMapFieldNameToIndex,
                        m_panMapFieldNameToGeomIndex, nResultOffset);

    nResultOffset++;
    iNextShapeId++;

    return poFeature;
}

/************************************************************************/
/*                           SetNextByIndex()                           */
/************************************************************************/

OGRErr OGRPGLayer::SetNextByIndex(GIntBig nIndex)

{
    GetLayerDefn();

    if (!TestCapability(OLCFastSetNextByIndex))
        return OGRLayer::SetNextByIndex(nIndex);

    if (nIndex == iNextShapeId)
    {
        return OGRERR_NONE;
    }

    if (nIndex < 0)
    {
        CPLError(CE_Failure, CPLE_AppDefined, "Invalid index");
        return OGRERR_FAILURE;
    }

    if (nIndex == 0)
    {
        ResetReading();
        return OGRERR_NONE;
    }

    PGconn *hPGConn = poDS->GetPGConn();
    CPLString osCommand;

    if (hCursorResult == nullptr)
    {
        SetInitialQueryCursor();
    }

    OGRPGClearResult(hCursorResult);

    osCommand.Printf("FETCH ABSOLUTE " CPL_FRMT_GIB " in %s", nIndex + 1,
                     pszCursorName);
    hCursorResult = OGRPG_PQexec(hPGConn, osCommand);

    if (PQresultStatus(hCursorResult) != PGRES_TUPLES_OK ||
        PQntuples(hCursorResult) != 1)
    {
        CPLError(CE_Failure, CPLE_AppDefined,
                 "Attempt to read feature at invalid index (" CPL_FRMT_GIB ").",
                 nIndex);

        CloseCursor();

        iNextShapeId = 0;

        return OGRERR_FAILURE;
    }

    nResultOffset = 0;
    iNextShapeId = nIndex;

    return OGRERR_NONE;
}

/************************************************************************/
/*                        BYTEAToGByteArray()                           */
/************************************************************************/

GByte *OGRPGLayer::BYTEAToGByteArray(const char *pszBytea, int *pnLength)
{
    if (pszBytea == nullptr)
    {
        if (pnLength)
            *pnLength = 0;
        return nullptr;
    }

    /* hex bytea data (PostgreSQL >= 9.0) */
    if (pszBytea[0] == '\\' && pszBytea[1] == 'x')
        return CPLHexToBinary(pszBytea + 2, pnLength);

    /* +1 just to please Coverity that thinks we allocate for a null-terminate
     * string */
    GByte *pabyData = static_cast<GByte *>(CPLMalloc(strlen(pszBytea) + 1));

    int iSrc = 0;
    int iDst = 0;
    while (pszBytea[iSrc] != '\0')
    {
        if (pszBytea[iSrc] == '\\')
        {
            if (pszBytea[iSrc + 1] >= '0' && pszBytea[iSrc + 1] <= '9')
            {
                if (pszBytea[iSrc + 2] == '\0' || pszBytea[iSrc + 3] == '\0')
                    break;

                pabyData[iDst++] = (pszBytea[iSrc + 1] - 48) * 64 +
                                   (pszBytea[iSrc + 2] - 48) * 8 +
                                   (pszBytea[iSrc + 3] - 48) * 1;
                iSrc += 4;
            }
            else
            {
                if (pszBytea[iSrc + 1] == '\0')
                    break;

                pabyData[iDst++] = pszBytea[iSrc + 1];
                iSrc += 2;
            }
        }
        else
        {
            pabyData[iDst++] = pszBytea[iSrc++];
        }
    }
    if (pnLength)
        *pnLength = iDst;

    return pabyData;
}

/************************************************************************/
/*                          BYTEAToGeometry()                           */
/************************************************************************/

OGRGeometry *OGRPGLayer::BYTEAToGeometry(const char *pszBytea)

{
    if (pszBytea == nullptr)
        return nullptr;

    int nLen = 0;
    GByte *pabyWKB = BYTEAToGByteArray(pszBytea, &nLen);

    OGRGeometry *poGeometry = nullptr;
    OGRGeometryFactory::createFromWkb(pabyWKB, nullptr, &poGeometry, nLen,
                                      wkbVariantOldOgc);

    CPLFree(pabyWKB);
    return poGeometry;
}

/************************************************************************/
/*                          GeometryToBYTEA()                           */
/************************************************************************/

char *OGRPGLayer::GeometryToBYTEA(const OGRGeometry *poGeometry,
                                  int nPostGISMajor, int nPostGISMinor)

{
    const size_t nWkbSize = poGeometry->WkbSize();

    GByte *pabyWKB = static_cast<GByte *>(VSI_MALLOC_VERBOSE(nWkbSize));
    if (pabyWKB == nullptr)
        return CPLStrdup("");

    if ((nPostGISMajor > 2 || (nPostGISMajor == 2 && nPostGISMinor >= 2)) &&
        wkbFlatten(poGeometry->getGeometryType()) == wkbPoint &&
        poGeometry->IsEmpty())
    {
        if (poGeometry->exportToWkb(wkbNDR, pabyWKB, wkbVariantIso) !=
            OGRERR_NONE)
        {
            CPLFree(pabyWKB);
            return CPLStrdup("");
        }
    }
    else if (poGeometry->exportToWkb(wkbNDR, pabyWKB,
                                     (nPostGISMajor < 2)
                                         ? wkbVariantPostGIS1
                                         : wkbVariantOldOgc) != OGRERR_NONE)
    {
        CPLFree(pabyWKB);
        return CPLStrdup("");
    }

    char *pszTextBuf = OGRPGCommonGByteArrayToBYTEA(pabyWKB, nWkbSize);
    CPLFree(pabyWKB);

    return pszTextBuf;
}

/************************************************************************/
/*                          OIDToGeometry()                             */
/************************************************************************/

OGRGeometry *OGRPGLayer::OIDToGeometry(Oid oid)

{
    if (oid == 0)
        return nullptr;

    PGconn *hPGConn = poDS->GetPGConn();
    const int fd = lo_open(hPGConn, oid, INV_READ);
    if (fd < 0)
        return nullptr;

    constexpr int MAX_WKB = 500000;
    GByte *pabyWKB = static_cast<GByte *>(CPLMalloc(MAX_WKB));
    const int nBytes =
        lo_read(hPGConn, fd, reinterpret_cast<char *>(pabyWKB), MAX_WKB);
    lo_close(hPGConn, fd);

    OGRGeometry *poGeometry = nullptr;
    OGRGeometryFactory::createFromWkb(pabyWKB, nullptr, &poGeometry, nBytes,
                                      wkbVariantOldOgc);

    CPLFree(pabyWKB);

    return poGeometry;
}

/************************************************************************/
/*                           GeometryToOID()                            */
/************************************************************************/

Oid OGRPGLayer::GeometryToOID(OGRGeometry *poGeometry)

{
    PGconn *hPGConn = poDS->GetPGConn();
    const size_t nWkbSize = poGeometry->WkbSize();
    if (nWkbSize > static_cast<size_t>(std::numeric_limits<int>::max()))
    {
        CPLError(CE_Failure, CPLE_NotSupported, "Too large geometry");
        return 0;
    }

    GByte *pabyWKB = static_cast<GByte *>(VSI_MALLOC_VERBOSE(nWkbSize));
    if (pabyWKB == nullptr)
        return 0;
    if (poGeometry->exportToWkb(wkbNDR, pabyWKB, wkbVariantOldOgc) !=
        OGRERR_NONE)
        return 0;

    Oid oid = lo_creat(hPGConn, INV_READ | INV_WRITE);

    const int fd = lo_open(hPGConn, oid, INV_WRITE);
    const int nBytesWritten =
        lo_write(hPGConn, fd, reinterpret_cast<char *>(pabyWKB),
                 static_cast<int>(nWkbSize));
    lo_close(hPGConn, fd);

    if (nBytesWritten != static_cast<int>(nWkbSize))
    {
        CPLDebug("PG",
                 "Only wrote %d bytes of %d intended for (fd=%d,oid=%d).\n",
                 nBytesWritten, static_cast<int>(nWkbSize), fd, oid);
    }

    CPLFree(pabyWKB);

    return oid;
}

/************************************************************************/
/*                          StartTransaction()                          */
/************************************************************************/

OGRErr OGRPGLayer::StartTransaction()

{
    return poDS->StartTransaction();
}

/************************************************************************/
/*                         CommitTransaction()                          */
/************************************************************************/

OGRErr OGRPGLayer::CommitTransaction()

{
    return poDS->CommitTransaction();
}

/************************************************************************/
/*                        RollbackTransaction()                         */
/************************************************************************/

OGRErr OGRPGLayer::RollbackTransaction()

{
    return poDS->RollbackTransaction();
}

/************************************************************************/
/*                            GetFIDColumn()                            */
/************************************************************************/

const char *OGRPGLayer::GetFIDColumn()

{
    GetLayerDefn();

    if (pszFIDColumn != nullptr)
        return pszFIDColumn;
    else
        return "";
}

/************************************************************************/
/*                            IGetExtent()                              */
/*                                                                      */
/*      For PostGIS use internal Extend(geometry) function              */
/*      in other cases we use standard OGRLayer::GetExtent()            */
/************************************************************************/

OGRErr OGRPGLayer::IGetExtent(int iGeomField, OGREnvelope *psExtent,
                              bool bForce)
{
    CPLString osCommand;

    OGRPGGeomFieldDefn *poGeomFieldDefn =
        poFeatureDefn->GetGeomFieldDefn(iGeomField);

    if (TestCapability(OLCFastGetExtent))
    {
        /* Do not take the spatial filter into account */
        osCommand.Printf(
            "SELECT ST_Extent(%s) FROM %s AS ogrpgextent",
            OGRPGEscapeColumnName(poGeomFieldDefn->GetNameRef()).c_str(),
            GetFromClauseForGetExtent().c_str());
    }
    else if (poGeomFieldDefn->ePostgisType == GEOM_TYPE_GEOGRAPHY)
    {
        /* Probably not very efficient, but more efficient than client-side
         * implementation */
        osCommand.Printf(
            "SELECT ST_Extent(ST_GeomFromWKB(ST_AsBinary(%s))) FROM %s AS "
            "ogrpgextent",
            OGRPGEscapeColumnName(poGeomFieldDefn->GetNameRef()).c_str(),
            GetFromClauseForGetExtent().c_str());
    }

    if (!osCommand.empty())
    {
        if (RunGetExtentRequest(*psExtent, bForce, osCommand, FALSE) ==
            OGRERR_NONE)
            return OGRERR_NONE;
    }

    return OGRLayer::IGetExtent(iGeomField, psExtent, bForce);
}

OGRErr OGRPGLayer::IGetExtent3D(int iGeomField, OGREnvelope3D *psExtent3D,
                                bool bForce)
{
    auto poLayerDefn = GetLayerDefn();

    // If the geometry field is not 3D go for 2D
    if (poLayerDefn->GetGeomFieldCount() > iGeomField &&
        !OGR_GT_HasZ(CPLAssertNotNull(poLayerDefn->GetGeomFieldDefn(iGeomField))
                         ->GetType()))
    {
        const OGRErr retVal{GetExtent(iGeomField, psExtent3D, bForce)};
        psExtent3D->MinZ = std::numeric_limits<double>::infinity();
        psExtent3D->MaxZ = -std::numeric_limits<double>::infinity();
        return retVal;
    }

    CPLString osCommand;

    OGRPGGeomFieldDefn *poGeomFieldDefn =
        poLayerDefn->GetGeomFieldDefn(iGeomField);

    if (TestCapability(OLCFastGetExtent3D))
    {
        /* Do not take the spatial filter into account */
        osCommand.Printf(
            "SELECT ST_Extent(%s) FROM %s AS ogrpgextent",
            OGRPGEscapeColumnName(poGeomFieldDefn->GetNameRef()).c_str(),
            GetFromClauseForGetExtent().c_str());
    }
    else if (poGeomFieldDefn->ePostgisType == GEOM_TYPE_GEOGRAPHY)
    {
        /* Probably not very efficient, but more efficient than client-side
         * implementation */
        osCommand.Printf(
            "SELECT ST_Extent(ST_GeomFromWKB(ST_AsBinary(%s))) FROM %s AS "
            "ogrpgextent",
            OGRPGEscapeColumnName(poGeomFieldDefn->GetNameRef()).c_str(),
            GetFromClauseForGetExtent().c_str());
    }

    if (!osCommand.empty())
    {
        if (RunGetExtent3DRequest(*psExtent3D, osCommand, FALSE) == OGRERR_NONE)
            return OGRERR_NONE;
    }

    return OGRLayer::IGetExtent3D(iGeomField, psExtent3D, bForce);
}

/************************************************************************/
/*                             GetExtent()                              */
/************************************************************************/

OGRErr OGRPGLayer::RunGetExtentRequest(OGREnvelope &sExtent,
                                       CPL_UNUSED int bForce,
                                       const std::string &osCommand,
                                       int bErrorAsDebug)
{
    PGconn *hPGConn = poDS->GetPGConn();
    PGresult *hResult =
        OGRPG_PQexec(hPGConn, osCommand.c_str(), FALSE, bErrorAsDebug);
    if (!hResult || PQresultStatus(hResult) != PGRES_TUPLES_OK ||
        PQgetisnull(hResult, 0, 0))
    {
        OGRPGClearResult(hResult);
        CPLDebug("PG", "Unable to get extent by PostGIS.");
        return OGRERR_FAILURE;
    }

    char *pszBox = PQgetvalue(hResult, 0, 0);
    char *ptr, *ptrEndParenthesis;
    char szVals[64 * 6 + 6];

    ptr = strchr(pszBox, '(');
    if (ptr)
        ptr++;
    if (ptr == nullptr || (ptrEndParenthesis = strchr(ptr, ')')) == nullptr ||
        ptrEndParenthesis - ptr > static_cast<int>(sizeof(szVals) - 1))
    {
        CPLError(CE_Failure, CPLE_IllegalArg, "Bad extent representation: '%s'",
                 pszBox);

        OGRPGClearResult(hResult);
        return OGRERR_FAILURE;
    }

    strncpy(szVals, ptr, ptrEndParenthesis - ptr);
    szVals[ptrEndParenthesis - ptr] = '\0';

    const CPLStringList aosTokens(
        CSLTokenizeString2(szVals, " ,", CSLT_HONOURSTRINGS));
    constexpr int nTokenCnt = 4;

    if (aosTokens.size() != nTokenCnt)
    {
        CPLError(CE_Failure, CPLE_IllegalArg, "Bad extent representation: '%s'",
                 pszBox);

        OGRPGClearResult(hResult);
        return OGRERR_FAILURE;
    }

    // Take X,Y coords
    // For PostGIS ver >= 1.0.0 -> Tokens: X1 Y1 X2 Y2 (nTokenCnt = 4)
    // For PostGIS ver < 1.0.0 -> Tokens: X1 Y1 Z1 X2 Y2 Z2 (nTokenCnt = 6)
    // =>   X2 index calculated as nTokenCnt/2
    //      Y2 index calculated as nTokenCnt/2+1

    sExtent.MinX = CPLAtof(aosTokens[0]);
    sExtent.MinY = CPLAtof(aosTokens[1]);
    sExtent.MaxX = CPLAtof(aosTokens[nTokenCnt / 2]);
    sExtent.MaxY = CPLAtof(aosTokens[nTokenCnt / 2 + 1]);

    OGRPGClearResult(hResult);

    return OGRERR_NONE;
}

OGRErr OGRPGLayer::RunGetExtent3DRequest(OGREnvelope3D &sExtent3D,
                                         const std::string &osCommand,
                                         int bErrorAsDebug)
{
    PGconn *hPGConn = poDS->GetPGConn();
    PGresult *hResult =
        OGRPG_PQexec(hPGConn, osCommand.c_str(), FALSE, bErrorAsDebug);
    if (!hResult || PQresultStatus(hResult) != PGRES_TUPLES_OK ||
        PQgetisnull(hResult, 0, 0))
    {
        OGRPGClearResult(hResult);
        CPLDebug("PG", "Unable to get extent 3D by PostGIS.");
        return OGRERR_FAILURE;
    }

    char *pszBox = PQgetvalue(hResult, 0, 0);
    char *ptr, *ptrEndParenthesis;
    char szVals[64 * 6 + 6];

    ptr = strchr(pszBox, '(');
    if (ptr)
        ptr++;
    if (ptr == nullptr || (ptrEndParenthesis = strchr(ptr, ')')) == nullptr ||
        ptrEndParenthesis - ptr > static_cast<int>(sizeof(szVals) - 1))
    {
        CPLError(CE_Failure, CPLE_IllegalArg, "Bad extent representation: '%s'",
                 pszBox);

        OGRPGClearResult(hResult);
        return OGRERR_FAILURE;
    }

    strncpy(szVals, ptr, ptrEndParenthesis - ptr);
    szVals[ptrEndParenthesis - ptr] = '\0';

    char **papszTokens = CSLTokenizeString2(szVals, " ,", CSLT_HONOURSTRINGS);
    if (CSLCount(papszTokens) != 6)
    {
        CPLError(CE_Failure, CPLE_IllegalArg,
                 "Bad extent 3D representation: '%s'", pszBox);
        CSLDestroy(papszTokens);

        OGRPGClearResult(hResult);
        return OGRERR_FAILURE;
    }

    sExtent3D.MinX = CPLAtof(papszTokens[0]);
    sExtent3D.MinY = CPLAtof(papszTokens[1]);
    sExtent3D.MinZ = CPLAtof(papszTokens[2]);
    sExtent3D.MaxX = CPLAtof(papszTokens[3]);
    sExtent3D.MaxY = CPLAtof(papszTokens[4]);
    sExtent3D.MaxZ = CPLAtof(papszTokens[5]);

    CSLDestroy(papszTokens);
    OGRPGClearResult(hResult);

    return OGRERR_NONE;
}

/************************************************************************/
/*                        ReadResultDefinition()                        */
/*                                                                      */
/*      Build a schema from the current resultset.                      */
/************************************************************************/

int OGRPGLayer::ReadResultDefinition(PGresult *hInitialResultIn)

{
    PGresult *hResult = hInitialResultIn;

    /* -------------------------------------------------------------------- */
    /*      Parse the returned table information.                           */
    /* -------------------------------------------------------------------- */
    poFeatureDefn = new OGRPGFeatureDefn("sql_statement");
    SetDescription(poFeatureDefn->GetName());

    poFeatureDefn->Reference();

    for (int iRawField = 0; iRawField < PQnfields(hResult); iRawField++)
    {
        OGRFieldDefn oField(PQfname(hResult, iRawField), OFTString);
        const Oid nTypeOID = PQftype(hResult, iRawField);

        int iGeomFuncPrefix = 0;
        if (EQUAL(oField.GetNameRef(), "ogc_fid"))
        {
            if (pszFIDColumn)
            {
                CPLError(CE_Warning, CPLE_AppDefined,
                         "More than one ogc_fid column was found in the result "
                         "of the SQL request. Only last one will be used");
            }
            CPLFree(pszFIDColumn);
            pszFIDColumn = CPLStrdup(oField.GetNameRef());
            continue;
        }
        else if ((iGeomFuncPrefix =
                      OGRPGIsKnownGeomFuncPrefix(oField.GetNameRef())) >= 0 ||
                 nTypeOID == poDS->GetGeometryOID() ||
                 nTypeOID == poDS->GetGeographyOID())
        {
            auto poGeomFieldDefn =
                std::make_unique<OGRPGGeomFieldDefn>(this, oField.GetNameRef());
            if (iGeomFuncPrefix >= 0 &&
                oField.GetNameRef()[strlen(
                    apszKnownGeomFuncPrefixes[iGeomFuncPrefix])] == '_')
            {
                poGeomFieldDefn->SetName(
                    oField.GetNameRef() +
                    strlen(apszKnownGeomFuncPrefixes[iGeomFuncPrefix]) + 1);
            }
            if (nTypeOID == poDS->GetGeographyOID())
            {
                poGeomFieldDefn->ePostgisType = GEOM_TYPE_GEOGRAPHY;
                if (!(poDS->sPostGISVersion.nMajor >= 3 ||
                      (poDS->sPostGISVersion.nMajor == 2 &&
                       poDS->sPostGISVersion.nMinor >= 2)))
                {
                    // EPSG:4326 was a requirement for geography before
                    // PostGIS 2.2
                    poGeomFieldDefn->nSRSId = 4326;
                }
            }
            else
                poGeomFieldDefn->ePostgisType = GEOM_TYPE_GEOMETRY;
            poFeatureDefn->AddGeomFieldDefn(std::move(poGeomFieldDefn));
            continue;
        }
        else if (EQUAL(oField.GetNameRef(), "WKB_GEOMETRY"))
        {
            if (nTypeOID == OIDOID)
                bWkbAsOid = TRUE;
            auto poGeomFieldDefn =
                std::make_unique<OGRPGGeomFieldDefn>(this, oField.GetNameRef());
            poGeomFieldDefn->ePostgisType = GEOM_TYPE_WKB;
            poFeatureDefn->AddGeomFieldDefn(std::move(poGeomFieldDefn));
            continue;
        }

        // CPLDebug("PG", "Field %s, oid %d", oField.GetNameRef(), nTypeOID);

        if (nTypeOID == BYTEAOID)
        {
            oField.SetType(OFTBinary);
        }
        else if (nTypeOID == CHAROID || nTypeOID == TEXTOID ||
                 nTypeOID == BPCHAROID || nTypeOID == VARCHAROID)
        {
            oField.SetType(OFTString);

            /* See
             * http://www.mail-archive.com/pgsql-hackers@postgresql.org/msg57726.html
             */
            /* nTypmod = width + 4 */
            int nTypmod = PQfmod(hResult, iRawField);
            if (nTypmod >= 4 &&
                (nTypeOID == BPCHAROID || nTypeOID == VARCHAROID))
            {
                oField.SetWidth(nTypmod - 4);
            }
        }
        else if (nTypeOID == BOOLOID)
        {
            oField.SetType(OFTInteger);
            oField.SetSubType(OFSTBoolean);
            oField.SetWidth(1);
        }
        else if (nTypeOID == INT2OID)
        {
            oField.SetType(OFTInteger);
            oField.SetSubType(OFSTInt16);
            oField.SetWidth(5);
        }
        else if (nTypeOID == INT4OID)
        {
            oField.SetType(OFTInteger);
        }
        else if (nTypeOID == INT8OID)
        {
            oField.SetType(OFTInteger64);
        }
        else if (nTypeOID == FLOAT4OID)
        {
            oField.SetType(OFTReal);
            oField.SetSubType(OFSTFloat32);
        }
        else if (nTypeOID == FLOAT8OID)
        {
            oField.SetType(OFTReal);
        }
        else if (nTypeOID == NUMERICOID || nTypeOID == NUMERICARRAYOID)
        {
            /* See
             * http://www.mail-archive.com/pgsql-hackers@postgresql.org/msg57726.html
             */
            /* typmod = (width << 16) + precision + 4 */
            int nTypmod = PQfmod(hResult, iRawField);
            if (nTypmod >= 4)
            {
                int nWidth = (nTypmod - 4) >> 16;
                int nPrecision = (nTypmod - 4) & 0xFFFF;
                if (nWidth <= 10 && nPrecision == 0)
                {
                    oField.SetType((nTypeOID == NUMERICOID) ? OFTInteger
                                                            : OFTIntegerList);
                    oField.SetWidth(nWidth);
                }
                else
                {
                    oField.SetType((nTypeOID == NUMERICOID) ? OFTReal
                                                            : OFTRealList);
                    oField.SetWidth(nWidth);
                    oField.SetPrecision(nPrecision);
                }
            }
            else
                oField.SetType((nTypeOID == NUMERICOID) ? OFTReal
                                                        : OFTRealList);
        }
        else if (nTypeOID == BOOLARRAYOID)
        {
            oField.SetType(OFTIntegerList);
            oField.SetSubType(OFSTBoolean);
            oField.SetWidth(1);
        }
        else if (nTypeOID == INT2ARRAYOID)
        {
            oField.SetType(OFTIntegerList);
            oField.SetSubType(OFSTInt16);
        }
        else if (nTypeOID == INT4ARRAYOID)
        {
            oField.SetType(OFTIntegerList);
        }
        else if (nTypeOID == INT8ARRAYOID)
        {
            oField.SetType(OFTInteger64List);
        }
        else if (nTypeOID == FLOAT4ARRAYOID)
        {
            oField.SetType(OFTRealList);
            oField.SetSubType(OFSTFloat32);
        }
        else if (nTypeOID == FLOAT8ARRAYOID)
        {
            oField.SetType(OFTRealList);
        }
        else if (nTypeOID == TEXTARRAYOID || nTypeOID == BPCHARARRAYOID ||
                 nTypeOID == VARCHARARRAYOID)
        {
            oField.SetType(OFTStringList);
        }
        else if (nTypeOID == DATEOID)
        {
            oField.SetType(OFTDate);
        }
        else if (nTypeOID == TIMEOID)
        {
            oField.SetType(OFTTime);
        }
        else if (nTypeOID == TIMESTAMPOID || nTypeOID == TIMESTAMPTZOID)
        {
#if defined(BINARY_CURSOR_ENABLED)
            /* We can't deserialize properly timestamp with time zone */
            /* with binary cursors */
            if (nTypeOID == TIMESTAMPTZOID)
                bCanUseBinaryCursor = FALSE;
#endif

            oField.SetType(OFTDateTime);
        }
        else if (nTypeOID == JSONOID || nTypeOID == JSONBOID)
        {
            oField.SetType(OFTString);
            oField.SetSubType(OFSTJSON);
        }
        else if (nTypeOID == UUIDOID)
        {
            oField.SetType(OFTString);
            oField.SetSubType(OFSTUUID);
        }
        else /* unknown type */
        {
            CPLDebug("PG",
                     "Unhandled OID (%d) for column %s. Defaulting to String.",
                     nTypeOID, oField.GetNameRef());
            oField.SetType(OFTString);
        }

        poFeatureDefn->AddFieldDefn(&oField);
    }

    return TRUE;
}

/************************************************************************/
/*                          GetSpatialRef()                             */
/************************************************************************/

const OGRSpatialReference *OGRPGGeomFieldDefn::GetSpatialRef() const
{
    if (poLayer == nullptr)
        return nullptr;
    if (nSRSId == UNDETERMINED_SRID)
        poLayer->ResolveSRID(this);

    if (poSRS == nullptr && nSRSId > 0)
    {
        poSRS = poLayer->GetDS()->FetchSRS(nSRSId);
        if (poSRS != nullptr)
            const_cast<OGRSpatialReference *>(poSRS)->Reference();
    }
    return poSRS;
}

/************************************************************************/
/*                             GetDataset()                             */
/************************************************************************/

GDALDataset *OGRPGLayer::GetDataset()
{
    return poDS;
}
