/******************************************************************************
 *
 * Component: ODS formula Engine
 * Purpose:
 * Author: Even Rouault <even dot rouault at spatialys.com>
 *
 ******************************************************************************
 * Copyright (C) 2010 Frank Warmerdam <warmerdam@pobox.com>
 * Copyright (c) 2012, Even Rouault <even dot rouault at spatialys.com>
 *
 * SPDX-License-Identifier: MIT
 ****************************************************************************/

#include <cctype>
#include <cmath>

#include "cpl_conv.h"
#include "ods_formula.h"

IODSCellEvaluator::~IODSCellEvaluator() = default;

namespace
{
#include "ods_formula_parser.hpp"

int ods_formulalex(ods_formula_node **ppNode,
                   ods_formula_parse_context *context);

#include "ods_formula_parser.cpp"
} /* end of anonymous namespace */

static const SingleOpStruct apsSingleOp[] = {
    {"ABS", ODS_ABS, fabs},   {"SQRT", ODS_SQRT, sqrt},
    {"COS", ODS_COS, cos},    {"SIN", ODS_SIN, sin},
    {"TAN", ODS_TAN, tan},    {"ACOS", ODS_ACOS, acos},
    {"ASIN", ODS_ASIN, asin}, {"ATAN", ODS_ATAN, atan},
    {"EXP", ODS_EXP, exp},    {"LN", ODS_LN, log},
    {"LOG", ODS_LOG, log10},  {"LOG10", ODS_LOG, log10},
};

const SingleOpStruct *ODSGetSingleOpEntry(const char *pszName)
{
    for (size_t i = 0; i < sizeof(apsSingleOp) / sizeof(apsSingleOp[0]); i++)
    {
        if (EQUAL(pszName, apsSingleOp[i].pszName))
            return &apsSingleOp[i];
    }
    return nullptr;
}

const SingleOpStruct *ODSGetSingleOpEntry(ods_formula_op eOp)
{
    for (size_t i = 0; i < sizeof(apsSingleOp) / sizeof(apsSingleOp[0]); i++)
    {
        if (eOp == apsSingleOp[i].eOp)
            return &apsSingleOp[i];
    }
    return nullptr;
}

/************************************************************************/
/*                               swqlex()                               */
/*                                                                      */
/*      Read back a token from the input.                               */
/************************************************************************/
namespace
{
int ods_formulalex(YYSTYPE *ppNode, ods_formula_parse_context *context)
{
    const char *pszInput = context->pszNext;

    *ppNode = nullptr;

    /* -------------------------------------------------------------------- */
    /*      Do we have a start symbol to return?                            */
    /* -------------------------------------------------------------------- */
    if (context->nStartToken != 0)
    {
        int nRet = context->nStartToken;
        context->nStartToken = 0;
        return nRet;
    }

    /* -------------------------------------------------------------------- */
    /*      Skip white space.                                               */
    /* -------------------------------------------------------------------- */
    while (*pszInput == ' ' || *pszInput == '\t' || *pszInput == 10 ||
           *pszInput == 13)
        pszInput++;

    if (*pszInput == '\0')
    {
        context->pszNext = pszInput;
        return EOF;
    }

    /* -------------------------------------------------------------------- */
    /*      Handle string constants.                                        */
    /* -------------------------------------------------------------------- */
    if (*pszInput == '"')
    {
        pszInput++;

        char *token = static_cast<char *>(CPLMalloc(strlen(pszInput) + 1));
        int i_token = 0;

        while (*pszInput != '\0')
        {
            if (*pszInput == '\\' && pszInput[1] == '"')
                pszInput++;
            else if (*pszInput == '\\' && pszInput[1] == '\'')
                pszInput++;
            else if (*pszInput == '\'' && pszInput[1] == '\'')
                pszInput++;
            else if (*pszInput == '"')
            {
                pszInput++;
                break;
            }
            else if (*pszInput == '\'')
            {
                pszInput++;
                break;
            }

            token[i_token++] = *(pszInput++);
        }
        token[i_token] = '\0';

        *ppNode = new ods_formula_node(token);
        CPLFree(token);

        context->pszNext = pszInput;

        return ODST_STRING;
    }

    /* -------------------------------------------------------------------- */
    /*      Handle numbers.                                                 */
    /* -------------------------------------------------------------------- */
    else if (*pszInput >= '0' && *pszInput <= '9')
    {
        const char *pszNext = pszInput + 1;

        CPLString osToken;
        osToken += *pszInput;

        // collect non-decimal part of number
        while (*pszNext >= '0' && *pszNext <= '9')
            osToken += *(pszNext++);

        // collect decimal places.
        if (*pszNext == '.')
        {
            osToken += *(pszNext++);
            while (*pszNext >= '0' && *pszNext <= '9')
                osToken += *(pszNext++);
        }

        // collect exponent
        if (*pszNext == 'e' || *pszNext == 'E')
        {
            osToken += *(pszNext++);
            if (*pszNext == '-' || *pszNext == '+')
                osToken += *(pszNext++);
            while (*pszNext >= '0' && *pszNext <= '9')
                osToken += *(pszNext++);
        }

        context->pszNext = pszNext;

        if (strstr(osToken, ".") || strstr(osToken, "e") ||
            strstr(osToken, "E"))
        {
            *ppNode = new ods_formula_node(CPLAtof(osToken));
        }
        else
        {
            GIntBig nVal = CPLAtoGIntBig(osToken);
            if (osToken.size() >= 12 || nVal < INT_MIN || nVal > INT_MAX)
                *ppNode = new ods_formula_node(CPLAtof(osToken));
            else
                *ppNode = new ods_formula_node(static_cast<int>(nVal));
        }

        return ODST_NUMBER;
    }

    /* -------------------------------------------------------------------- */
    /*      Handle alpha-numerics.                                          */
    /* -------------------------------------------------------------------- */
    else if (*pszInput == '.' || isalnum(static_cast<unsigned char>(*pszInput)))
    {
        int nReturn = ODST_IDENTIFIER;
        const char *pszNext = pszInput + 1;

        CPLString osToken;
        osToken += *pszInput;

        // collect text characters
        while (isalnum(static_cast<unsigned char>(*pszNext)) ||
               *pszNext == '_' || ((unsigned char)*pszNext) > 127)
            osToken += *(pszNext++);

        context->pszNext = pszNext;

        /* Constants */
        if (EQUAL(osToken, "TRUE"))
        {
            *ppNode = new ods_formula_node(1);
            return ODST_NUMBER;
        }
        else if (EQUAL(osToken, "FALSE"))
        {
            *ppNode = new ods_formula_node(0);
            return ODST_NUMBER;
        }

        else if (EQUAL(osToken, "NOT"))
            nReturn = ODST_NOT;
        else if (EQUAL(osToken, "AND"))
            nReturn = ODST_AND;
        else if (EQUAL(osToken, "OR"))
            nReturn = ODST_OR;
        else if (EQUAL(osToken, "IF"))
            nReturn = ODST_IF;

        /* No-arg functions */
        else if (EQUAL(osToken, "PI"))
        {
            *ppNode = new ods_formula_node(ODS_PI);
            return ODST_FUNCTION_NO_ARG;
        }

        /* Single-arg functions */
        else if (EQUAL(osToken, "LEN"))
        {
            *ppNode = new ods_formula_node(ODS_LEN);
            return ODST_FUNCTION_SINGLE_ARG;
        }
        /*
        else if( EQUAL(osToken,"T") )
        {
            *ppNode = new ods_formula_node( ODS_T );
            return ODST_FUNCTION_SINGLE_ARG;
        }*/

        /* Tow-arg functions */
        else if (EQUAL(osToken, "MOD"))
        {
            *ppNode = new ods_formula_node(ODS_MODULUS);
            return ODST_FUNCTION_TWO_ARG;
        }
        else if (EQUAL(osToken, "LEFT"))
        {
            *ppNode = new ods_formula_node(ODS_LEFT);
            return ODST_FUNCTION_TWO_ARG;
        }
        else if (EQUAL(osToken, "RIGHT"))
        {
            *ppNode = new ods_formula_node(ODS_RIGHT);
            return ODST_FUNCTION_TWO_ARG;
        }

        /* Three-arg functions */
        else if (EQUAL(osToken, "MID"))
        {
            *ppNode = new ods_formula_node(ODS_MID);
            return ODST_FUNCTION_THREE_ARG;
        }

        /* Multiple-arg functions */
        else if (EQUAL(osToken, "SUM"))
        {
            *ppNode = new ods_formula_node(ODS_SUM);
            nReturn = ODST_FUNCTION_ARG_LIST;
        }
        else if (EQUAL(osToken, "AVERAGE"))
        {
            *ppNode = new ods_formula_node(ODS_AVERAGE);
            nReturn = ODST_FUNCTION_ARG_LIST;
        }
        else if (EQUAL(osToken, "MIN"))
        {
            *ppNode = new ods_formula_node(ODS_MIN);
            nReturn = ODST_FUNCTION_ARG_LIST;
        }
        else if (EQUAL(osToken, "MAX"))
        {
            *ppNode = new ods_formula_node(ODS_MAX);
            nReturn = ODST_FUNCTION_ARG_LIST;
        }
        else if (EQUAL(osToken, "COUNT"))
        {
            *ppNode = new ods_formula_node(ODS_COUNT);
            nReturn = ODST_FUNCTION_ARG_LIST;
        }
        else if (EQUAL(osToken, "COUNTA"))
        {
            *ppNode = new ods_formula_node(ODS_COUNTA);
            nReturn = ODST_FUNCTION_ARG_LIST;
        }

        else
        {
            const SingleOpStruct *psSingleOp = ODSGetSingleOpEntry(osToken);
            if (psSingleOp != nullptr)
            {
                *ppNode = new ods_formula_node(psSingleOp->eOp);
                nReturn = ODST_FUNCTION_SINGLE_ARG;
            }
            else
            {
                *ppNode = new ods_formula_node(osToken);
                nReturn = ODST_IDENTIFIER;
            }
        }

        return nReturn;
    }

    /* -------------------------------------------------------------------- */
    /*      Handle special tokens.                                          */
    /* -------------------------------------------------------------------- */
    else
    {
        context->pszNext = pszInput + 1;
        return *pszInput;
    }
}
} /* end of anonymous namespace */

/************************************************************************/
/*                        ods_formula_compile()                         */
/************************************************************************/

ods_formula_node *ods_formula_compile(const char *expr)

{
    ods_formula_parse_context context;

    context.pszInput = expr;
    context.pszNext = expr;
    context.nStartToken = ODST_START;

    if (ods_formulaparse(&context) == 0)
    {
        return context.poRoot;
    }

    delete context.poRoot;
    return nullptr;
}
