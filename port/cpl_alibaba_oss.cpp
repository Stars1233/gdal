/**********************************************************************
 *
 * Name:     cpl_alibaba_oss.h
 * Project:  CPL - Common Portability Library
 * Purpose:  Alibaba Cloud Object Storage Service
 * Author:   Even Rouault <even.rouault at spatialys.com>
 *
 **********************************************************************
 * Copyright (c) 2017, Even Rouault <even.rouault at spatialys.com>
 *
 * SPDX-License-Identifier: MIT
 ****************************************************************************/

//! @cond Doxygen_Suppress

#include "cpl_alibaba_oss.h"
#include "cpl_vsi_error.h"
#include "cpl_time.h"
#include "cpl_minixml.h"
#include "cpl_multiproc.h"
#include "cpl_http.h"
#include "cpl_sha1.h"
#include <algorithm>

// #define DEBUG_VERBOSE 1

#ifdef HAVE_CURL

/************************************************************************/
/*                            GetSignature()                            */
/************************************************************************/

static std::string GetSignature(const std::string &osStringToSign,
                                const std::string &osSecretAccessKey)
{

    /* -------------------------------------------------------------------- */
    /*      Compute signature.                                              */
    /* -------------------------------------------------------------------- */
    GByte abySignature[CPL_SHA1_HASH_SIZE] = {};
    CPL_HMAC_SHA1(osSecretAccessKey.c_str(), osSecretAccessKey.size(),
                  osStringToSign.c_str(), osStringToSign.size(), abySignature);
    char *pszBase64 = CPLBase64Encode(sizeof(abySignature), abySignature);
    std::string osSignature(pszBase64);
    CPLFree(pszBase64);

    return osSignature;
}

/************************************************************************/
/*                         CPLGetOSSHeaders()                           */
/************************************************************************/

// See:
// https://www.alibabacloud.com/help/doc-detail/31951.htm?spm=a3c0i.o31982en.b99.178.5HUTqV
static struct curl_slist *
CPLGetOSSHeaders(const std::string &osSecretAccessKey,
                 const std::string &osAccessKeyId, const std::string &osVerb,
                 struct curl_slist *psHeaders,
                 const std::string &osCanonicalizedResource)
{
    std::string osDate = CPLGetConfigOption("CPL_OSS_TIMESTAMP", "");
    if (osDate.empty())
    {
        osDate = IVSIS3LikeHandleHelper::GetRFC822DateTime();
    }

    std::map<std::string, std::string> oSortedMapHeaders;
    std::string osCanonicalizedHeaders(
        IVSIS3LikeHandleHelper::BuildCanonicalizedHeaders(oSortedMapHeaders,
                                                          psHeaders, "x-oss-"));

    std::string osStringToSign;
    osStringToSign += osVerb + "\n";
    osStringToSign += CPLAWSGetHeaderVal(psHeaders, "Content-MD5") + "\n";
    osStringToSign += CPLAWSGetHeaderVal(psHeaders, "Content-Type") + "\n";
    osStringToSign += osDate + "\n";
    osStringToSign += osCanonicalizedHeaders;
    osStringToSign += osCanonicalizedResource;
#ifdef DEBUG_VERBOSE
    CPLDebug("OSS", "osStringToSign = %s", osStringToSign.c_str());
#endif

    /* -------------------------------------------------------------------- */
    /*      Build authorization header.                                     */
    /* -------------------------------------------------------------------- */

    std::string osAuthorization("OSS ");
    osAuthorization += osAccessKeyId;
    osAuthorization += ":";
    osAuthorization += GetSignature(osStringToSign, osSecretAccessKey);

#ifdef DEBUG_VERBOSE
    CPLDebug("OSS", "osAuthorization='%s'", osAuthorization.c_str());
#endif

    psHeaders =
        curl_slist_append(psHeaders, CPLSPrintf("Date: %s", osDate.c_str()));
    psHeaders = curl_slist_append(
        psHeaders, CPLSPrintf("Authorization: %s", osAuthorization.c_str()));
    return psHeaders;
}

/************************************************************************/
/*                         VSIOSSHandleHelper()                         */
/************************************************************************/
VSIOSSHandleHelper::VSIOSSHandleHelper(const std::string &osSecretAccessKey,
                                       const std::string &osAccessKeyId,
                                       const std::string &osEndpoint,
                                       const std::string &osBucket,
                                       const std::string &osObjectKey,
                                       bool bUseHTTPS, bool bUseVirtualHosting)
    : m_osURL(BuildURL(osEndpoint, osBucket, osObjectKey, bUseHTTPS,
                       bUseVirtualHosting)),
      m_osSecretAccessKey(osSecretAccessKey), m_osAccessKeyId(osAccessKeyId),
      m_osEndpoint(osEndpoint), m_osBucket(osBucket),
      m_osObjectKey(osObjectKey), m_bUseHTTPS(bUseHTTPS),
      m_bUseVirtualHosting(bUseVirtualHosting)
{
    VSIOSSUpdateParams::UpdateHandleFromMap(this);
}

/************************************************************************/
/*                        ~VSIOSSHandleHelper()                         */
/************************************************************************/

VSIOSSHandleHelper::~VSIOSSHandleHelper()
{
    for (size_t i = 0; i < m_osSecretAccessKey.size(); i++)
        m_osSecretAccessKey[i] = 0;
}

/************************************************************************/
/*                           BuildURL()                                 */
/************************************************************************/

std::string VSIOSSHandleHelper::BuildURL(const std::string &osEndpoint,
                                         const std::string &osBucket,
                                         const std::string &osObjectKey,
                                         bool bUseHTTPS,
                                         bool bUseVirtualHosting)
{
    const char *pszProtocol = (bUseHTTPS) ? "https" : "http";
    if (osBucket.empty())
    {
        return CPLSPrintf("%s://%s", pszProtocol, osEndpoint.c_str());
    }
    else if (bUseVirtualHosting)
        return CPLSPrintf("%s://%s.%s/%s", pszProtocol, osBucket.c_str(),
                          osEndpoint.c_str(),
                          CPLAWSURLEncode(osObjectKey, false).c_str());
    else
        return CPLSPrintf("%s://%s/%s/%s", pszProtocol, osEndpoint.c_str(),
                          osBucket.c_str(),
                          CPLAWSURLEncode(osObjectKey, false).c_str());
}

/************************************************************************/
/*                           RebuildURL()                               */
/************************************************************************/

void VSIOSSHandleHelper::RebuildURL()
{
    m_osURL = BuildURL(m_osEndpoint, m_osBucket, m_osObjectKey, m_bUseHTTPS,
                       m_bUseVirtualHosting);
    m_osURL += GetQueryString(false);
}

/************************************************************************/
/*                        GetConfiguration()                            */
/************************************************************************/

bool VSIOSSHandleHelper::GetConfiguration(const std::string &osPathForOption,
                                          CSLConstList papszOptions,
                                          std::string &osSecretAccessKey,
                                          std::string &osAccessKeyId)
{
    osSecretAccessKey = CSLFetchNameValueDef(
        papszOptions, "OSS_SECRET_ACCESS_KEY",
        VSIGetPathSpecificOption(osPathForOption.c_str(),
                                 "OSS_SECRET_ACCESS_KEY", ""));

    if (!osSecretAccessKey.empty())
    {
        osAccessKeyId = CSLFetchNameValueDef(
            papszOptions, "OSS_ACCESS_KEY_ID",
            VSIGetPathSpecificOption(osPathForOption.c_str(),
                                     "OSS_ACCESS_KEY_ID", ""));
        if (osAccessKeyId.empty())
        {
            VSIError(VSIE_InvalidCredentials,
                     "OSS_ACCESS_KEY_ID configuration option not defined");
            return false;
        }

        return true;
    }

    VSIError(VSIE_InvalidCredentials,
             "OSS_SECRET_ACCESS_KEY configuration option not defined");
    return false;
}

/************************************************************************/
/*                          BuildFromURI()                              */
/************************************************************************/

VSIOSSHandleHelper *VSIOSSHandleHelper::BuildFromURI(const char *pszURI,
                                                     const char *pszFSPrefix,
                                                     bool bAllowNoObject,
                                                     CSLConstList papszOptions)
{
    std::string osPathForOption("/vsioss/");
    if (pszURI)
        osPathForOption += pszURI;

    std::string osSecretAccessKey;
    std::string osAccessKeyId;
    if (!GetConfiguration(osPathForOption, papszOptions, osSecretAccessKey,
                          osAccessKeyId))
    {
        return nullptr;
    }

    const std::string osEndpoint = CSLFetchNameValueDef(
        papszOptions, "OSS_ENDPOINT",
        VSIGetPathSpecificOption(osPathForOption.c_str(), "OSS_ENDPOINT",
                                 "oss-us-east-1.aliyuncs.com"));
    std::string osBucket;
    std::string osObjectKey;
    if (pszURI != nullptr && pszURI[0] != '\0' &&
        !GetBucketAndObjectKey(pszURI, pszFSPrefix, bAllowNoObject, osBucket,
                               osObjectKey))
    {
        return nullptr;
    }
    const bool bUseHTTPS = CPLTestBool(
        VSIGetPathSpecificOption(osPathForOption.c_str(), "OSS_HTTPS", "YES"));
    const bool bIsValidNameForVirtualHosting =
        osBucket.find('.') == std::string::npos;
    const bool bUseVirtualHosting = CPLTestBool(VSIGetPathSpecificOption(
        osPathForOption.c_str(), "OSS_VIRTUAL_HOSTING",
        bIsValidNameForVirtualHosting ? "TRUE" : "FALSE"));
    return new VSIOSSHandleHelper(osSecretAccessKey, osAccessKeyId, osEndpoint,
                                  osBucket, osObjectKey, bUseHTTPS,
                                  bUseVirtualHosting);
}

/************************************************************************/
/*                           GetCurlHeaders()                           */
/************************************************************************/

struct curl_slist *VSIOSSHandleHelper::GetCurlHeaders(
    const std::string &osVerb, struct curl_slist *psHeaders,
    const void * /*pabyDataContent*/, size_t /*nBytesContent*/) const
{
    std::string osCanonicalQueryString;
    if (!m_osObjectKey.empty())
    {
        osCanonicalQueryString = GetQueryString(false);
    }

    std::string osCanonicalizedResource(
        m_osBucket.empty() ? std::string("/")
                           : "/" + m_osBucket + "/" + m_osObjectKey);
    osCanonicalizedResource += osCanonicalQueryString;

    return CPLGetOSSHeaders(m_osSecretAccessKey, m_osAccessKeyId, osVerb,
                            psHeaders, osCanonicalizedResource);
}

/************************************************************************/
/*                          CanRestartOnError()                         */
/************************************************************************/

bool VSIOSSHandleHelper::CanRestartOnError(const char *pszErrorMsg,
                                           const char *, bool bSetError)
{
#ifdef DEBUG_VERBOSE
    CPLDebug("OSS", "%s", pszErrorMsg);
#endif

    if (!STARTS_WITH(pszErrorMsg, "<?xml"))
    {
        if (bSetError)
        {
            VSIError(VSIE_ObjectStorageGenericError, "Invalid OSS response: %s",
                     pszErrorMsg);
        }
        return false;
    }

    CPLXMLNode *psTree = CPLParseXMLString(pszErrorMsg);
    if (psTree == nullptr)
    {
        if (bSetError)
        {
            VSIError(VSIE_ObjectStorageGenericError,
                     "Malformed OSS XML response: %s", pszErrorMsg);
        }
        return false;
    }

    const char *pszCode = CPLGetXMLValue(psTree, "=Error.Code", nullptr);
    if (pszCode == nullptr)
    {
        CPLDestroyXMLNode(psTree);
        if (bSetError)
        {
            VSIError(VSIE_ObjectStorageGenericError,
                     "Malformed OSS XML response: %s", pszErrorMsg);
        }
        return false;
    }

    if (EQUAL(pszCode, "AccessDenied"))
    {
        const char *pszEndpoint =
            CPLGetXMLValue(psTree, "=Error.Endpoint", nullptr);
        if (pszEndpoint && pszEndpoint != m_osEndpoint)
        {
            SetEndpoint(pszEndpoint);
            CPLDebug("OSS", "Switching to endpoint %s", m_osEndpoint.c_str());
            CPLDestroyXMLNode(psTree);

            VSIOSSUpdateParams::UpdateMapFromHandle(this);

            return true;
        }
    }

    if (bSetError)
    {
        // Translate AWS errors into VSI errors.
        const char *pszMessage =
            CPLGetXMLValue(psTree, "=Error.Message", nullptr);

        if (pszMessage == nullptr)
        {
            VSIError(VSIE_ObjectStorageGenericError, "%s", pszErrorMsg);
        }
        else if (EQUAL(pszCode, "AccessDenied"))
        {
            VSIError(VSIE_AccessDenied, "%s", pszMessage);
        }
        else if (EQUAL(pszCode, "NoSuchBucket"))
        {
            VSIError(VSIE_BucketNotFound, "%s", pszMessage);
        }
        else if (EQUAL(pszCode, "NoSuchKey"))
        {
            VSIError(VSIE_ObjectNotFound, "%s", pszMessage);
        }
        else if (EQUAL(pszCode, "SignatureDoesNotMatch"))
        {
            VSIError(VSIE_SignatureDoesNotMatch, "%s", pszMessage);
        }
        else
        {
            VSIError(VSIE_ObjectStorageGenericError, "%s", pszMessage);
        }
    }

    CPLDestroyXMLNode(psTree);

    return false;
}

/************************************************************************/
/*                            SetEndpoint()                             */
/************************************************************************/

void VSIOSSHandleHelper::SetEndpoint(const std::string &osStr)
{
    m_osEndpoint = osStr;
    RebuildURL();
}

/************************************************************************/
/*                           GetSignedURL()                             */
/************************************************************************/

std::string VSIOSSHandleHelper::GetSignedURL(CSLConstList papszOptions)
{
    GIntBig nStartDate = static_cast<GIntBig>(time(nullptr));
    const char *pszStartDate = CSLFetchNameValue(papszOptions, "START_DATE");
    if (pszStartDate)
    {
        int nYear, nMonth, nDay, nHour, nMin, nSec;
        if (sscanf(pszStartDate, "%04d%02d%02dT%02d%02d%02dZ", &nYear, &nMonth,
                   &nDay, &nHour, &nMin, &nSec) == 6)
        {
            struct tm brokendowntime;
            brokendowntime.tm_year = nYear - 1900;
            brokendowntime.tm_mon = nMonth - 1;
            brokendowntime.tm_mday = nDay;
            brokendowntime.tm_hour = nHour;
            brokendowntime.tm_min = nMin;
            brokendowntime.tm_sec = nSec;
            nStartDate = CPLYMDHMSToUnixTime(&brokendowntime);
        }
    }
    GIntBig nExpiresIn =
        nStartDate +
        atoi(CSLFetchNameValueDef(papszOptions, "EXPIRATION_DELAY", "3600"));
    std::string osExpires(CSLFetchNameValueDef(
        papszOptions, "EXPIRES", CPLSPrintf(CPL_FRMT_GIB, nExpiresIn)));

    std::string osVerb(CSLFetchNameValueDef(papszOptions, "VERB", "GET"));

    std::string osCanonicalizedResource(
        m_osBucket.empty() ? std::string("/")
                           : "/" + m_osBucket + "/" + m_osObjectKey);

    std::string osStringToSign;
    osStringToSign += osVerb + "\n";
    osStringToSign += "\n";
    osStringToSign += "\n";
    osStringToSign += osExpires + "\n";
    // osStringToSign += ; // osCanonicalizedHeaders;
    osStringToSign += osCanonicalizedResource;
#ifdef DEBUG_VERBOSE
    CPLDebug("OSS", "osStringToSign = %s", osStringToSign.c_str());
#endif

    std::string osSignature(GetSignature(osStringToSign, m_osSecretAccessKey));

    ResetQueryParameters();
    //  Note:
    //  https://www.alibabacloud.com/help/doc-detail/31952.htm?spm=a3c0i.o32002en.b99.294.6d70a0fc7cRJfJ
    //  is wrong on the name of the OSSAccessKeyId parameter !
    AddQueryParameter("OSSAccessKeyId", m_osAccessKeyId);
    AddQueryParameter("Expires", osExpires);
    AddQueryParameter("Signature", osSignature);
    return m_osURL;
}

/************************************************************************/
/*                         UpdateMapFromHandle()                        */
/************************************************************************/

std::mutex VSIOSSUpdateParams::gsMutex{};

std::map<std::string, VSIOSSUpdateParams>
    VSIOSSUpdateParams::goMapBucketsToOSSParams{};

void VSIOSSUpdateParams::UpdateMapFromHandle(
    VSIOSSHandleHelper *poOSSHandleHelper)
{
    std::lock_guard<std::mutex> guard(gsMutex);

    goMapBucketsToOSSParams[poOSSHandleHelper->GetBucket()] =
        VSIOSSUpdateParams(poOSSHandleHelper);
}

/************************************************************************/
/*                         UpdateHandleFromMap()                        */
/************************************************************************/

void VSIOSSUpdateParams::UpdateHandleFromMap(
    VSIOSSHandleHelper *poOSSHandleHelper)
{
    std::lock_guard<std::mutex> guard(gsMutex);

    std::map<std::string, VSIOSSUpdateParams>::iterator oIter =
        goMapBucketsToOSSParams.find(poOSSHandleHelper->GetBucket());
    if (oIter != goMapBucketsToOSSParams.end())
    {
        oIter->second.UpdateHandlerHelper(poOSSHandleHelper);
    }
}

/************************************************************************/
/*                            ClearCache()                              */
/************************************************************************/

void VSIOSSUpdateParams::ClearCache()
{
    std::lock_guard<std::mutex> guard(gsMutex);

    goMapBucketsToOSSParams.clear();
}

#endif  // HAVE_CURL

//! @endcond
