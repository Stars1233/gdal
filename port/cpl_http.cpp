/******************************************************************************
 *
 * Project:  libcurl based HTTP client
 * Purpose:  libcurl based HTTP client
 * Author:   Frank Warmerdam, warmerdam@pobox.com
 *
 ******************************************************************************
 * Copyright (c) 2006, Frank Warmerdam
 * Copyright (c) 2008-2013, Even Rouault <even dot rouault at spatialys.com>
 *
 * SPDX-License-Identifier: MIT
 ****************************************************************************/

#include "cpl_port.h"
#include "cpl_http.h"

#include <cstddef>
#include <cstring>

#include <algorithm>
#include <array>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "cpl_http.h"
#include "cpl_error.h"
#include "cpl_multiproc.h"
#include "cpl_vsi_virtual.h"
#include "cpl_vsil_curl_class.h"

// gcc or clang complains about C-style cast in #define like
// CURL_ZERO_TERMINATED
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wold-style-cast"
#endif

#ifdef HAVE_CURL

#include "cpl_curl_priv.h"

#ifdef HAVE_OPENSSL_CRYPTO
#include <openssl/err.h>
#include <openssl/ssl.h>
#include <openssl/x509v3.h>

#if defined(_WIN32)
#include <wincrypt.h>
#endif

#endif

#ifdef HAVE_SIGACTION
#include <signal.h>
#endif

#define unchecked_curl_easy_setopt(handle, opt, param)                         \
    CPL_IGNORE_RET_VAL(curl_easy_setopt(handle, opt, param))

#endif  // HAVE_CURL

// list of named persistent http sessions

#ifdef HAVE_CURL
static std::map<CPLString, CURL *> *poSessionMap = nullptr;
static std::map<CPLString, CURLM *> *poSessionMultiMap = nullptr;
static CPLMutex *hSessionMapMutex = nullptr;
static bool bHasCheckVersion = false;
static bool bSupportGZip = false;
static bool bSupportHTTP2 = false;
#if defined(_WIN32) && defined(HAVE_OPENSSL_CRYPTO)
static std::vector<X509 *> *poWindowsCertificateList = nullptr;

#if (OPENSSL_VERSION_NUMBER < 0x10100000L)
#define EVP_PKEY_get0_RSA(x) (x->pkey.rsa)
#define EVP_PKEY_get0_DSA(x) (x->pkey.dsa)
#define X509_get_extension_flags(x) (x->ex_flags)
#define X509_get_key_usage(x) (x->ex_kusage)
#define X509_get_extended_key_usage(x) (x->ex_xkusage)
#endif

#endif  // defined(_WIN32) && defined(HAVE_OPENSSL_CRYPTO)

#if defined(HAVE_OPENSSL_CRYPTO) && OPENSSL_VERSION_NUMBER < 0x10100000

// Ported from https://curl.haxx.se/libcurl/c/opensslthreadlock.html
static CPLMutex **pahSSLMutex = nullptr;

static void CPLOpenSSLLockingFunction(int mode, int n, const char * /*file*/,
                                      int /*line*/)
{
    if (mode & CRYPTO_LOCK)
    {
        CPLAcquireMutex(pahSSLMutex[n], 3600.0);
    }
    else
    {
        CPLReleaseMutex(pahSSLMutex[n]);
    }
}

static unsigned long CPLOpenSSLIdCallback(void)
{
    return static_cast<unsigned long>(CPLGetPID());
}

static void CPLOpenSSLInit()
{
    if (strstr(curl_version(), "OpenSSL") &&
        CPLTestBool(CPLGetConfigOption("CPL_OPENSSL_INIT_ENABLED", "YES")) &&
        CRYPTO_get_id_callback() == nullptr)
    {
        pahSSLMutex = static_cast<CPLMutex **>(
            CPLMalloc(CRYPTO_num_locks() * sizeof(CPLMutex *)));
        for (int i = 0; i < CRYPTO_num_locks(); i++)
        {
            pahSSLMutex[i] = CPLCreateMutex();
            CPLReleaseMutex(pahSSLMutex[i]);
        }
        CRYPTO_set_id_callback(CPLOpenSSLIdCallback);
        CRYPTO_set_locking_callback(CPLOpenSSLLockingFunction);
    }
}

static void CPLOpenSSLCleanup()
{
    if (pahSSLMutex)
    {
        for (int i = 0; i < CRYPTO_num_locks(); i++)
        {
            CPLDestroyMutex(pahSSLMutex[i]);
        }
        CPLFree(pahSSLMutex);
        pahSSLMutex = nullptr;
        CRYPTO_set_id_callback(nullptr);
        CRYPTO_set_locking_callback(nullptr);
    }
}

#endif

#if defined(_WIN32) && defined(HAVE_OPENSSL_CRYPTO)

/************************************************************************/
/*                    CPLWindowsCertificateListCleanup()                */
/************************************************************************/

static void CPLWindowsCertificateListCleanup()
{
    if (poWindowsCertificateList)
    {
        for (auto &&pX509 : *poWindowsCertificateList)
        {
            X509_free(pX509);
        }
        delete poWindowsCertificateList;
        poWindowsCertificateList = nullptr;
    }
}

/************************************************************************/
/*                       LoadCAPICertificates()                         */
/************************************************************************/

static CPLErr LoadCAPICertificates(const char *pszName,
                                   std::vector<X509 *> *poCertificateList)
{
    CPLAssert(pszName);
    CPLAssert(poCertificateList);

    HCERTSTORE pCertStore = CertOpenSystemStore(
        reinterpret_cast<HCRYPTPROV_LEGACY>(nullptr), pszName);
    if (pCertStore == nullptr)
    {
        CPLError(CE_Failure, CPLE_AppDefined,
                 "CPLLoadCAPICertificates(): Unable open system "
                 "certificate store %s.",
                 pszName);
        return CE_Failure;
    }

    PCCERT_CONTEXT pCertificate =
        CertEnumCertificatesInStore(pCertStore, nullptr);
    while (pCertificate != nullptr)
    {
        X509 *pX509 = d2i_X509(
            nullptr,
            const_cast<unsigned char const **>(&pCertificate->pbCertEncoded),
            pCertificate->cbCertEncoded);
        if (pX509 == nullptr)
        {
            CPLError(CE_Warning, CPLE_AppDefined,
                     "CPLLoadCAPICertificates(): CertEnumCertificatesInStore() "
                     "returned a null certificate, skipping.");
        }
        else
        {
#ifdef DEBUG_VERBOSE
            char szSubject[256] = {0};
            CPLString osSubject;
            X509_NAME *pName = X509_get_subject_name(pX509);
            if (pName)
            {
                X509_NAME_oneline(pName, szSubject, sizeof(szSubject));
                osSubject = szSubject;
            }
            if (!osSubject.empty())
                CPLDebug("HTTP", "SSL Certificate: %s", osSubject.c_str());
#endif
            poCertificateList->push_back(pX509);
        }
        pCertificate = CertEnumCertificatesInStore(pCertStore, pCertificate);
    }
    CertCloseStore(pCertStore, 0);
    return CE_None;
}

/************************************************************************/
/*                       CPL_ssl_ctx_callback()                         */
/************************************************************************/

// Load certificates from Windows Crypto API store.
static CURLcode CPL_ssl_ctx_callback(CURL *, void *pSSL, void *)
{
    SSL_CTX *pSSL_CTX = static_cast<SSL_CTX *>(pSSL);
    if (pSSL_CTX == nullptr)
    {
        CPLError(CE_Failure, CPLE_AppDefined,
                 "CPL_ssl_ctx_callback(): OpenSSL context pointer is NULL.");
        return CURLE_ABORTED_BY_CALLBACK;
    }

    static std::mutex goMutex;
    {
        std::lock_guard<std::mutex> oLock(goMutex);
        if (poWindowsCertificateList == nullptr)
        {
            poWindowsCertificateList = new std::vector<X509 *>();
            if (!poWindowsCertificateList)
            {
                CPLError(CE_Failure, CPLE_AppDefined,
                         "CPL_ssl_ctx_callback(): Unable to allocate "
                         "structure to hold certificates.");
                return CURLE_FAILED_INIT;
            }

            const std::array<const char *, 3> aszStores{
                {"CA", "AuthRoot", "ROOT"}};
            for (auto &&pszStore : aszStores)
            {
                if (LoadCAPICertificates(pszStore, poWindowsCertificateList) ==
                    CE_Failure)
                {
                    CPLError(
                        CE_Failure, CPLE_AppDefined,
                        "CPL_ssl_ctx_callback(): Unable to load certificates "
                        "from '%s' store.",
                        pszStore);
                    return CURLE_FAILED_INIT;
                }
            }

            CPLDebug("HTTP", "Loading %d certificates from Windows store.",
                     static_cast<int>(poWindowsCertificateList->size()));
        }
    }

    X509_STORE *pX509Store = SSL_CTX_get_cert_store(pSSL_CTX);
    for (X509 *x509 : *poWindowsCertificateList)
        X509_STORE_add_cert(pX509Store, x509);

    return CURLE_OK;
}

#endif  // defined(_WIN32) && defined (HAVE_OPENSSL_CRYPTO)

/************************************************************************/
/*                       CheckCurlFeatures()                            */
/************************************************************************/

static void CheckCurlFeatures()
{
    CPLMutexHolder oHolder(&hSessionMapMutex);
    if (!bHasCheckVersion)
    {
        const char *pszVersion = curl_version();
        CPLDebug("HTTP", "%s", pszVersion);
        bSupportGZip = strstr(pszVersion, "zlib/") != nullptr;
        bSupportHTTP2 = strstr(curl_version(), "nghttp2/") != nullptr;
        bHasCheckVersion = true;

        curl_version_info_data *data = curl_version_info(CURLVERSION_NOW);
        if (data->version_num < LIBCURL_VERSION_NUM)
        {
            CPLError(CE_Warning, CPLE_AppDefined,
                     "GDAL was built against curl %d.%d.%d, but is "
                     "running against %s. Runtime failure is likely !",
                     LIBCURL_VERSION_MAJOR, LIBCURL_VERSION_MINOR,
                     LIBCURL_VERSION_PATCH, data->version);
        }
        else if (data->version_num > LIBCURL_VERSION_NUM)
        {
            CPLDebug("HTTP",
                     "GDAL was built against curl %d.%d.%d, but is "
                     "running against %s.",
                     LIBCURL_VERSION_MAJOR, LIBCURL_VERSION_MINOR,
                     LIBCURL_VERSION_PATCH, data->version);
        }

#if defined(HAVE_OPENSSL_CRYPTO) && OPENSSL_VERSION_NUMBER < 0x10100000
        CPLOpenSSLInit();
#endif
    }
}

/************************************************************************/
/*                            CPLWriteFct()                             */
/*                                                                      */
/*      Append incoming text to our collection buffer, reallocating     */
/*      it larger as needed.                                            */
/************************************************************************/

class CPLHTTPResultWithLimit
{
  public:
    CPLHTTPResult *psResult = nullptr;
    int nMaxFileSize = 0;
};

static size_t CPLWriteFct(void *buffer, size_t size, size_t nmemb,
                          void *reqInfo)

{
    CPLHTTPResultWithLimit *psResultWithLimit =
        static_cast<CPLHTTPResultWithLimit *>(reqInfo);
    CPLHTTPResult *psResult = psResultWithLimit->psResult;

    int nBytesToWrite = static_cast<int>(nmemb) * static_cast<int>(size);
    int nNewSize = psResult->nDataLen + nBytesToWrite + 1;
    if (nNewSize > psResult->nDataAlloc)
    {
        psResult->nDataAlloc = static_cast<int>(nNewSize * 1.25 + 100);
        GByte *pabyNewData = static_cast<GByte *>(
            VSIRealloc(psResult->pabyData, psResult->nDataAlloc));
        if (pabyNewData == nullptr)
        {
            VSIFree(psResult->pabyData);
            psResult->pabyData = nullptr;
            psResult->pszErrBuf = CPLStrdup(CPLString().Printf(
                "Out of memory allocating %d bytes for HTTP data buffer.",
                psResult->nDataAlloc));
            psResult->nDataAlloc = psResult->nDataLen = 0;

            return 0;
        }
        psResult->pabyData = pabyNewData;
    }

    memcpy(psResult->pabyData + psResult->nDataLen, buffer, nBytesToWrite);

    psResult->nDataLen += nBytesToWrite;
    psResult->pabyData[psResult->nDataLen] = 0;

    if (psResultWithLimit->nMaxFileSize > 0 &&
        psResult->nDataLen > psResultWithLimit->nMaxFileSize)
    {
        CPLError(CE_Failure, CPLE_AppDefined, "Maximum file size reached");
        return 0;
    }

    return nmemb;
}

/************************************************************************/
/*                           CPLHdrWriteFct()                           */
/************************************************************************/
static size_t CPLHdrWriteFct(void *buffer, size_t size, size_t nmemb,
                             void *reqInfo)
{
    CPLHTTPResult *psResult = static_cast<CPLHTTPResult *>(reqInfo);
    // Copy the buffer to a char* and initialize with zeros (zero
    // terminate as well).
    size_t nBytes = size * nmemb;
    char *pszHdr = static_cast<char *>(CPLCalloc(1, nBytes + 1));
    memcpy(pszHdr, buffer, nBytes);
    size_t nIdx = nBytes - 1;
    // Remove end of line characters
    while (nIdx > 0 && (pszHdr[nIdx] == '\r' || pszHdr[nIdx] == '\n'))
    {
        pszHdr[nIdx] = 0;
        nIdx--;
    }
    char *pszKey = nullptr;
    const char *pszValue = CPLParseNameValue(pszHdr, &pszKey);
    if (pszKey && pszValue)
    {
        psResult->papszHeaders =
            CSLAddNameValue(psResult->papszHeaders, pszKey, pszValue);
    }
    CPLFree(pszHdr);
    CPLFree(pszKey);
    return nmemb;
}

/************************************************************************/
/*                        CPLHTTPReadFunction()                         */
/************************************************************************/
static size_t CPLHTTPReadFunction(char *buffer, size_t size, size_t nitems,
                                  void *arg)
{
    return VSIFReadL(buffer, size, nitems, static_cast<VSILFILE *>(arg));
}

/************************************************************************/
/*                        CPLHTTPSeekFunction()                         */
/************************************************************************/
static int CPLHTTPSeekFunction(void *arg, curl_off_t offset, int origin)
{
    if (VSIFSeekL(static_cast<VSILFILE *>(arg), offset, origin) == 0)
        return CURL_SEEKFUNC_OK;
    else
        return CURL_SEEKFUNC_FAIL;
}

/************************************************************************/
/*                        CPLHTTPFreeFunction()                         */
/************************************************************************/
static void CPLHTTPFreeFunction(void *arg)
{
    VSIFCloseL(static_cast<VSILFILE *>(arg));
}

typedef struct
{
    GDALProgressFunc pfnProgress;
    void *pProgressArg;
} CurlProcessData, *CurlProcessDataL;

static int NewProcessFunction(void *p, curl_off_t dltotal, curl_off_t dlnow,
                              curl_off_t ultotal, curl_off_t ulnow)
{
    CurlProcessDataL pData = static_cast<CurlProcessDataL>(p);
    if (nullptr != pData && pData->pfnProgress)
    {
        if (dltotal > 0)
        {
            const double dfDone = double(dlnow) / dltotal;
            return pData->pfnProgress(dfDone, "Downloading ...",
                                      pData->pProgressArg) == TRUE
                       ? 0
                       : 1;
        }
        else if (ultotal > 0)
        {
            const double dfDone = double(ulnow) / ultotal;
            return pData->pfnProgress(dfDone, "Uploading ...",
                                      pData->pProgressArg) == TRUE
                       ? 0
                       : 1;
        }
    }
    return 0;
}

#endif /* def HAVE_CURL */

/************************************************************************/
/*                     CPLHTTPSetDefaultUserAgent()                     */
/************************************************************************/

static std::string gosDefaultUserAgent;

/**
 * \brief Set the default user agent.
 *
 * GDAL core will by default call this method with "GDAL/x.y.z" where x.y.z
 * is the GDAL version number (during driver initialization). Applications may
 * override it.
 *
 * @param pszUserAgent String (or nullptr to cancel the default user agent)
 *
 * @since GDAL 3.7
 */
void CPLHTTPSetDefaultUserAgent(const char *pszUserAgent)
{
    gosDefaultUserAgent = pszUserAgent ? pszUserAgent : "";
}

/************************************************************************/
/*                       CPLHTTPGetOptionsFromEnv()                     */
/************************************************************************/

typedef struct
{
    const char *pszEnvVar;
    const char *pszOptionName;
} TupleEnvVarOptionName;

constexpr TupleEnvVarOptionName asAssocEnvVarOptionName[] = {
    {"GDAL_HTTP_VERSION", "HTTP_VERSION"},
    {"GDAL_HTTP_CONNECTTIMEOUT", "CONNECTTIMEOUT"},
    {"GDAL_HTTP_TIMEOUT", "TIMEOUT"},
    {"GDAL_HTTP_LOW_SPEED_TIME", "LOW_SPEED_TIME"},
    {"GDAL_HTTP_LOW_SPEED_LIMIT", "LOW_SPEED_LIMIT"},
    {"GDAL_HTTP_USERPWD", "USERPWD"},
    {"GDAL_HTTP_PROXY", "PROXY"},
    {"GDAL_HTTPS_PROXY", "HTTPS_PROXY"},
    {"GDAL_HTTP_PROXYUSERPWD", "PROXYUSERPWD"},
    {"GDAL_PROXY_AUTH", "PROXYAUTH"},
    {"GDAL_HTTP_NETRC", "NETRC"},
    {"GDAL_HTTP_NETRC_FILE", "NETRC_FILE"},
    {"GDAL_HTTP_MAX_RETRY", "MAX_RETRY"},
    {"GDAL_HTTP_RETRY_DELAY", "RETRY_DELAY"},
    {"GDAL_HTTP_RETRY_CODES", "RETRY_CODES"},
    {"GDAL_CURL_CA_BUNDLE", "CAINFO"},
    {"CURL_CA_BUNDLE", "CAINFO"},
    {"SSL_CERT_FILE", "CAINFO"},
    {"GDAL_HTTP_CAPATH", "CAPATH"},
    {"GDAL_HTTP_SSL_VERIFYSTATUS", "SSL_VERIFYSTATUS"},
    {"GDAL_HTTP_USE_CAPI_STORE", "USE_CAPI_STORE"},
    {"GDAL_HTTP_HEADERS", "HEADERS"},
    {"GDAL_HTTP_HEADER_FILE", "HEADER_FILE"},
    {"GDAL_HTTP_AUTH", "HTTPAUTH"},
    {"GDAL_GSSAPI_DELEGATION", "GSSAPI_DELEGATION"},
    {"GDAL_HTTP_BEARER", "HTTP_BEARER"},
    {"GDAL_HTTP_COOKIE", "COOKIE"},
    {"GDAL_HTTP_COOKIEFILE", "COOKIEFILE"},
    {"GDAL_HTTP_COOKIEJAR", "COOKIEJAR"},
    {"GDAL_HTTP_MAX_RETRY", "MAX_RETRY"},
    {"GDAL_HTTP_RETRY_DELAY", "RETRY_DELAY"},
    {"GDAL_HTTP_TCP_KEEPALIVE", "TCP_KEEPALIVE"},
    {"GDAL_HTTP_TCP_KEEPIDLE", "TCP_KEEPIDLE"},
    {"GDAL_HTTP_TCP_KEEPINTVL", "TCP_KEEPINTVL"},
};

char **CPLHTTPGetOptionsFromEnv(const char *pszFilename)
{
    CPLStringList aosOptions;
    std::string osNonStreamingFilename;
    if (pszFilename && STARTS_WITH(pszFilename, "/vsi"))
    {
        VSIFilesystemHandler *poFSHandler =
            VSIFileManager::GetHandler(pszFilename);
        osNonStreamingFilename =
            poFSHandler->GetNonStreamingFilename(pszFilename);
        if (osNonStreamingFilename == pszFilename)
        {
            osNonStreamingFilename.clear();
        }
        else
        {
            // CPLDebug("HTTP", "Non-streaming filename for %s: %s", pszFilename, osNonStreamingFilename.c_str());
        }
    }
    for (const auto &sTuple : asAssocEnvVarOptionName)
    {
        const char *pszVal = nullptr;
        if (pszFilename)
        {
            pszVal = VSIGetPathSpecificOption(pszFilename, sTuple.pszEnvVar,
                                              nullptr);
            if (!pszVal && !osNonStreamingFilename.empty())
            {
                pszVal = VSIGetPathSpecificOption(
                    osNonStreamingFilename.c_str(), sTuple.pszEnvVar, nullptr);
            }
        }
        if (!pszVal)
        {
            pszVal = CPLGetConfigOption(sTuple.pszEnvVar, nullptr);
        }
        if (pszVal)
        {
            aosOptions.AddNameValue(sTuple.pszOptionName, pszVal);
        }
    }
    return aosOptions.StealList();
}

/************************************************************************/
/*                      CPLHTTPGetNewRetryDelay()                       */
/************************************************************************/

/** Return the new retry delay.
 *
 * This takes into account the HTTP response code, the previous delay, the
 * HTTP payload error message, the Curl error message and a potential list of
 * retriable HTTP codes.
 *
 * @param response_code HTTP response code (e.g. 400)
 * @param dfOldDelay Previous delay (nominally in second)
 * @param pszErrBuf HTTP response body of the failed request (may be NULL)
 * @param pszCurlError Curl error as returned by CURLOPT_ERRORBUFFER (may be NULL)
 * @param pszRetriableCodes nullptr to limit to the default hard-coded scenarios,
 * "ALL" to ask to retry for all non-200 codes, or a comma-separated list of
 * HTTP codes (e.g. "400,500") that are accepted for retry.
 * @return the new delay, or 0 if no retry should be attempted.
 */
static double CPLHTTPGetNewRetryDelay(int response_code, double dfOldDelay,
                                      const char *pszErrBuf,
                                      const char *pszCurlError,
                                      const char *pszRetriableCodes)
{
    bool bRetry = false;
    if (pszRetriableCodes && pszRetriableCodes[0])
    {
        bRetry = EQUAL(pszRetriableCodes, "ALL") ||
                 strstr(pszRetriableCodes, CPLSPrintf("%d", response_code));
    }
    else if (response_code == 429 || response_code == 500 ||
             (response_code >= 502 && response_code <= 504) ||
             // S3 sends some client timeout errors as 400 Client Error
             (response_code == 400 && pszErrBuf &&
              strstr(pszErrBuf, "RequestTimeout")) ||
             (pszCurlError &&
              (strstr(pszCurlError, "Connection timed out") ||
               strstr(pszCurlError, "Operation timed out") ||
               strstr(pszCurlError, "Connection reset by peer") ||
               strstr(pszCurlError, "Connection was reset") ||
               strstr(pszCurlError, "SSL connection timeout"))))
    {
        bRetry = true;
    }
    if (bRetry)
    {
        // 'Operation timed out': seen during some long running operation 'hang'
        // no error but no response from server and we are in the cURL loop
        // infinitely.

        // 'Connection was reset': was found with Azure: server resets
        // connection during TLS handshake (10054 error code). It seems like
        // the server process crashed or something forced TCP reset;
        // the request succeeds on retry.

        // Use an exponential backoff factor of 2 plus some random jitter
        // We don't care about cryptographic quality randomness, hence:
#ifndef __COVERITY__
        return dfOldDelay * (2 + rand() * 0.5 / RAND_MAX);
#else
        return dfOldDelay * 2;
#endif
    }
    else
    {
        return 0;
    }
}

/*! @cond Doxygen_Suppress */

/************************************************************************/
/*                      CPLHTTPRetryParameters()                        */
/************************************************************************/

/** Constructs a CPLHTTPRetryParameters instance from configuration
 * options or path-specific options.
 *
 * @param aosHTTPOptions HTTP options returned by CPLHTTPGetOptionsFromEnv()
 */
CPLHTTPRetryParameters::CPLHTTPRetryParameters(
    const CPLStringList &aosHTTPOptions)
    : nMaxRetry(atoi(aosHTTPOptions.FetchNameValueDef(
          "MAX_RETRY", CPLSPrintf("%d", CPL_HTTP_MAX_RETRY)))),
      dfInitialDelay(CPLAtof(aosHTTPOptions.FetchNameValueDef(
          "RETRY_DELAY", CPLSPrintf("%f", CPL_HTTP_RETRY_DELAY)))),
      osRetryCodes(aosHTTPOptions.FetchNameValueDef("RETRY_CODES", ""))
{
}

/************************************************************************/
/*                        CPLHTTPRetryContext()                         */
/************************************************************************/

/** Constructor */
CPLHTTPRetryContext::CPLHTTPRetryContext(const CPLHTTPRetryParameters &oParams)
    : m_oParameters(oParams), m_dfNextDelay(oParams.dfInitialDelay)
{
}

/************************************************************************/
/*                     CPLHTTPRetryContext::CanRetry()                  */
/************************************************************************/

/** Returns whether we can attempt a new retry, based on the retry counter,
 * and increment that counter.
 */
bool CPLHTTPRetryContext::CanRetry()
{
    if (m_nRetryCount >= m_oParameters.nMaxRetry)
        return false;
    m_nRetryCount++;
    return true;
}

/** Returns whether we can attempt a new retry, based on the retry counter,
 * the response code, payload and curl error buffers.
 *
 * If successful, the retry counter is incremented, and GetCurrentDelay()
 * returns the delay to apply with CPLSleep().
 */
bool CPLHTTPRetryContext::CanRetry(int response_code, const char *pszErrBuf,
                                   const char *pszCurlError)
{
    if (m_nRetryCount >= m_oParameters.nMaxRetry)
        return false;
    m_dfCurDelay = m_dfNextDelay;
    m_dfNextDelay = CPLHTTPGetNewRetryDelay(response_code, m_dfNextDelay,
                                            pszErrBuf, pszCurlError,
                                            m_oParameters.osRetryCodes.c_str());
    if (m_dfNextDelay == 0.0)
        return false;
    m_nRetryCount++;
    return true;
}

/************************************************************************/
/*                CPLHTTPRetryContext::GetCurrentDelay()                */
/************************************************************************/

/** Returns the delay to apply. Only valid after a successful call to CanRetry() */
double CPLHTTPRetryContext::GetCurrentDelay() const
{
    if (m_nRetryCount == 0)
        CPLDebug("CPL",
                 "GetCurrentDelay() should only be called after CanRetry()");
    return m_dfCurDelay;
}

/*! @endcond Doxygen_Suppress */

#ifdef HAVE_CURL

/************************************************************************/
/*                      CPLHTTPEmitFetchDebug()                         */
/************************************************************************/

static void CPLHTTPEmitFetchDebug(const char *pszURL,
                                  const char *pszExtraDebug = "")
{
    const char *pszArobase = strchr(pszURL, '@');
    const char *pszSlash = strchr(pszURL, '/');
    const char *pszColon = (pszSlash) ? strchr(pszSlash, ':') : nullptr;
    if (pszArobase != nullptr && pszColon != nullptr &&
        pszArobase - pszColon > 0)
    {
        /* http://user:password@www.example.com */
        char *pszSanitizedURL = CPLStrdup(pszURL);
        pszSanitizedURL[pszColon - pszURL] = 0;
        CPLDebug("HTTP", "Fetch(%s:#password#%s%s)", pszSanitizedURL,
                 pszArobase, pszExtraDebug);
        CPLFree(pszSanitizedURL);
    }
    else
    {
        CPLDebug("HTTP", "Fetch(%s%s)", pszURL, pszExtraDebug);
    }
}

#endif

#ifdef HAVE_CURL

/************************************************************************/
/*                      class CPLHTTPPostFields                         */
/************************************************************************/

class CPLHTTPPostFields
{
  public:
    CPLHTTPPostFields() = default;
    CPLHTTPPostFields &operator=(const CPLHTTPPostFields &) = delete;
    CPLHTTPPostFields(const CPLHTTPPostFields &) = delete;

    CPLErr Fill(CURL *http_handle, CSLConstList papszOptions)
    {
        // Fill POST form if present
        const char *pszFormFilePath =
            CSLFetchNameValue(papszOptions, "FORM_FILE_PATH");
        const char *pszParametersCount =
            CSLFetchNameValue(papszOptions, "FORM_ITEM_COUNT");

        if (pszFormFilePath != nullptr || pszParametersCount != nullptr)
        {
            mime = curl_mime_init(http_handle);
            curl_mimepart *mimepart = curl_mime_addpart(mime);
            if (pszFormFilePath != nullptr)
            {
                const char *pszFormFileName =
                    CSLFetchNameValue(papszOptions, "FORM_FILE_NAME");
                const char *pszFilename = CPLGetFilename(pszFormFilePath);
                if (pszFormFileName == nullptr)
                {
                    pszFormFileName = pszFilename;
                }

                VSIStatBufL sStat;
                if (VSIStatL(pszFormFilePath, &sStat) == 0)
                {
                    VSILFILE *mime_fp = VSIFOpenL(pszFormFilePath, "rb");
                    if (mime_fp != nullptr)
                    {
                        curl_mime_name(mimepart, pszFormFileName);
                        CPL_IGNORE_RET_VAL(
                            curl_mime_filename(mimepart, pszFilename));
                        curl_mime_data_cb(
                            mimepart, sStat.st_size, CPLHTTPReadFunction,
                            CPLHTTPSeekFunction, CPLHTTPFreeFunction, mime_fp);
                    }
                    else
                    {
                        osErrMsg = CPLSPrintf("Failed to open file %s",
                                              pszFormFilePath);
                        return CE_Failure;
                    }

                    CPLDebug("HTTP", "Send file: %s, COPYNAME: %s",
                             pszFormFilePath, pszFormFileName);
                }
                else
                {
                    osErrMsg =
                        CPLSPrintf("File '%s' not found", pszFormFilePath);
                    return CE_Failure;
                }
            }

            int nParametersCount = 0;
            if (pszParametersCount != nullptr)
            {
                nParametersCount = atoi(pszParametersCount);
            }

            for (int i = 0; i < nParametersCount; ++i)
            {
                const char *pszKey = CSLFetchNameValue(
                    papszOptions, CPLSPrintf("FORM_KEY_%d", i));
                const char *pszValue = CSLFetchNameValue(
                    papszOptions, CPLSPrintf("FORM_VALUE_%d", i));

                if (nullptr == pszKey)
                {
                    osErrMsg = CPLSPrintf("Key #%d is not exists. Maybe wrong "
                                          "count of form items",
                                          i);
                    return CE_Failure;
                }

                if (nullptr == pszValue)
                {
                    osErrMsg = CPLSPrintf("Value #%d is not exists. Maybe "
                                          "wrong count of form items",
                                          i);
                    return CE_Failure;
                }

                mimepart = curl_mime_addpart(mime);
                curl_mime_name(mimepart, pszKey);
                CPL_IGNORE_RET_VAL(
                    curl_mime_data(mimepart, pszValue, CURL_ZERO_TERMINATED));

                CPLDebug("HTTP", "COPYNAME: %s, COPYCONTENTS: %s", pszKey,
                         pszValue);
            }

            unchecked_curl_easy_setopt(http_handle, CURLOPT_MIMEPOST, mime);
        }
        return CE_None;
    }

    ~CPLHTTPPostFields()
    {
        if (mime != nullptr)
        {
            curl_mime_free(mime);
        }
    }

    std::string GetErrorMessage() const
    {
        return osErrMsg;
    }

  private:
    curl_mime *mime = nullptr;
    std::string osErrMsg{};
};

/************************************************************************/
/*                       CPLHTTPFetchCleanup()                          */
/************************************************************************/

static void CPLHTTPFetchCleanup(CURL *http_handle, struct curl_slist *headers,
                                const char *pszPersistent,
                                CSLConstList papszOptions)
{
    if (CSLFetchNameValue(papszOptions, "POSTFIELDS"))
        unchecked_curl_easy_setopt(http_handle, CURLOPT_POST, 0);
    unchecked_curl_easy_setopt(http_handle, CURLOPT_HTTPHEADER, nullptr);

    if (!pszPersistent)
        curl_easy_cleanup(http_handle);

    curl_slist_free_all(headers);
}
#endif  // HAVE_CURL

struct CPLHTTPFetchContext
{
    std::vector<std::pair<CPLHTTPFetchCallbackFunc, void *>> stack{};
};

/************************************************************************/
/*                        GetHTTPFetchContext()                         */
/************************************************************************/

static CPLHTTPFetchContext *GetHTTPFetchContext(bool bAlloc)
{
    int bError = FALSE;
    CPLHTTPFetchContext *psCtx = static_cast<CPLHTTPFetchContext *>(
        CPLGetTLSEx(CTLS_HTTPFETCHCALLBACK, &bError));
    if (bError)
        return nullptr;

    if (psCtx == nullptr && bAlloc)
    {
        const auto FreeFunc = [](void *pData)
        { delete static_cast<CPLHTTPFetchContext *>(pData); };
        psCtx = new CPLHTTPFetchContext();
        CPLSetTLSWithFreeFuncEx(CTLS_HTTPFETCHCALLBACK, psCtx, FreeFunc,
                                &bError);
        if (bError)
        {
            delete psCtx;
            psCtx = nullptr;
        }
    }
    return psCtx;
}

/************************************************************************/
/*                      CPLHTTPSetFetchCallback()                       */
/************************************************************************/

static CPLHTTPFetchCallbackFunc gpsHTTPFetchCallbackFunc = nullptr;
static void *gpHTTPFetchCallbackUserData = nullptr;

/** Installs an alternate callback to the default implementation of
 * CPLHTTPFetchEx().
 *
 * This callback will be used by all threads, unless contextual callbacks are
 * installed with CPLHTTPPushFetchCallback().
 *
 * It is the responsibility of the caller to make sure this function is not
 * called concurrently, or during CPLHTTPFetchEx() execution.
 *
 * @param pFunc Callback function to be called with CPLHTTPFetchEx() is called
 *              (or NULL to restore default handler)
 * @param pUserData Last argument to provide to the pFunc callback.
 *
 * @since GDAL 3.2
 */
void CPLHTTPSetFetchCallback(CPLHTTPFetchCallbackFunc pFunc, void *pUserData)
{
    gpsHTTPFetchCallbackFunc = pFunc;
    gpHTTPFetchCallbackUserData = pUserData;
}

/************************************************************************/
/*                      CPLHTTPPushFetchCallback()                      */
/************************************************************************/

/** Installs an alternate callback to the default implementation of
 * CPLHTTPFetchEx().
 *
 * This callback will only be used in the thread where this function has been
 * called. It must be un-installed by CPLHTTPPopFetchCallback(), which must also
 * be called from the same thread.
 *
 * @param pFunc Callback function to be called with CPLHTTPFetchEx() is called.
 * @param pUserData Last argument to provide to the pFunc callback.
 * @return TRUE in case of success.
 *
 * @since GDAL 3.2
 */
int CPLHTTPPushFetchCallback(CPLHTTPFetchCallbackFunc pFunc, void *pUserData)
{
    auto psCtx = GetHTTPFetchContext(true);
    if (psCtx == nullptr)
        return false;
    psCtx->stack.emplace_back(
        std::pair<CPLHTTPFetchCallbackFunc, void *>(pFunc, pUserData));
    return true;
}

/************************************************************************/
/*                       CPLHTTPPopFetchCallback()                      */
/************************************************************************/

/** Uninstalls a callback set by CPLHTTPPushFetchCallback().
 *
 * @see CPLHTTPPushFetchCallback()
 * @return TRUE in case of success.
 * @since GDAL 3.2
 */
int CPLHTTPPopFetchCallback(void)
{
    auto psCtx = GetHTTPFetchContext(false);
    if (psCtx == nullptr || psCtx->stack.empty())
    {
        CPLError(
            CE_Failure, CPLE_AppDefined,
            "CPLHTTPPushFetchCallback / CPLHTTPPopFetchCallback not balanced");
        return false;
    }
    else
    {
        psCtx->stack.pop_back();
        return true;
    }
}

/************************************************************************/
/*                           CPLHTTPFetch()                             */
/************************************************************************/

// NOTE: when adding an option below, add it in asAssocEnvVarOptionName[]

// clang-format off
/**
 * \brief Fetch a document from an url and return in a string.
 *
 * Different options may be passed through the papszOptions function parameter,
 * or for most of them through a configuration option:
 * <ul>
 * <li>CONNECTTIMEOUT=val, where
 * val is in seconds (possibly with decimals). This is the maximum delay for the
 * connection to be established before being aborted (GDAL >= 2.2).
 * Corresponding configuration option: GDAL_HTTP_CONNECTTIMEOUT.
 * </li>
 * <li>TIMEOUT=val, where val is in seconds. This is the maximum delay for the
 * whole request to complete before being aborted.
 * Corresponding configuration option: GDAL_HTTP_TIMEOUT.
 * </li>
 * <li>LOW_SPEED_TIME=val,
 * where val is in seconds. This is the maximum time where the transfer speed
 * should be below the LOW_SPEED_LIMIT (if not specified 1b/s), before the
 * transfer to be considered too slow and aborted. (GDAL >= 2.1).
 * Corresponding configuration option: GDAL_HTTP_LOW_SPEED_TIME.
 * </li>
 * <li>LOW_SPEED_LIMIT=val, where val is in bytes/second. See LOW_SPEED_TIME.
 * Has only effect if LOW_SPEED_TIME is specified too. (GDAL >= 2.1).
 * Corresponding configuration option: GDAL_HTTP_LOW_SPEED_LIMIT.
 * </li>
 * <li>HEADERS=val, where val is an extra header to use when getting a web page.
 *                  For example "Accept: application/x-ogcwkt"
 * Corresponding configuration option: GDAL_HTTP_HEADERS.
 * Starting with GDAL 3.6, the GDAL_HTTP_HEADERS configuration option can also
 * be used to specify a comma separated list of key: value pairs. This is an
 * alternative to the GDAL_HTTP_HEADER_FILE mechanism. If a comma or a
 * double-quote character is needed in the value, then the key: value pair must
 * be enclosed in double-quote characters. In that situation, backslash and
 * double quote character must be backslash-escaped. e.g GDAL_HTTP_HEADERS=Foo:
 * Bar,"Baz: escaped backslash \\, escaped double-quote \", end of
 * value",Another: Header
 * </li>
 * <li>HEADER_FILE=filename: filename of a text file with "key: value" headers.
 *     The content of the file is not cached, and thus it is read again before
 *     issuing each HTTP request. (GDAL >= 2.2)
 * Corresponding configuration option: GDAL_HTTP_HEADER_FILE.
 * </li>
 * <li>HTTPAUTH=[BASIC/NTLM/NEGOTIATE/ANY/ANYSAFE/BEARER] to specify an
 * authentication scheme to use.
 * Corresponding configuration option: GDAL_HTTP_AUTH.
 * </li>
 * <li>USERPWD=userid:password to specify a user and password for
 * authentication.
 * Corresponding configuration option: GDAL_HTTP_USERPWD.
 * </li>
 * <li>GSSAPI_DELEGATION=[NONE/POLICY/ALWAYS] set allowed
 * GSS-API delegation. Relevant only with HTTPAUTH=NEGOTIATE (GDAL >= 3.3).
 * Corresponding configuration option: GDAL_GSSAPI_DELEGATION (note: no "HTTP_" in the name)
 * </li>
 * <li>HTTP_BEARER=val set OAuth 2.0 Bearer Access Token.
 * Relevant only with HTTPAUTH=BEARER (GDAL >= 3.9).
 * Corresponding configuration option: GDAL_HTTP_BEARER
 * </li>
 * <li>POSTFIELDS=val, where val is a nul-terminated string to be passed to the
 * server with a POST request.
 * No Corresponding configuration option.
 * </li>
 * <li>PROXY=val, to make requests go through a
 * proxy server, where val is of the form proxy.server.com:port_number. This
 * option affects both HTTP and HTTPS URLs.
 * Corresponding configuration option: GDAL_HTTP_PROXY.
 * </li>
 * <li>HTTPS_PROXY=val (GDAL
 * >= 2.4), the same meaning as PROXY, but this option is taken into account
 * only for HTTPS URLs.
 * Corresponding configuration option: GDAL_HTTPS_PROXY.
 * </li>
 * <li>PROXYUSERPWD=val, where val is of the form username:password.
 * Corresponding configuration option: GDAL_HTTP_PROXYUSERPWD
 * </li>
 * <li>PROXYAUTH=[BASIC/NTLM/DIGEST/NEGOTIATE/ANY/ANYSAFE] to
 * specify an proxy authentication scheme to use..
 * Corresponding configuration option: GDAL_PROXYAUTH (note: no "HTTP_" in the name)
 * </li>
 * <li>NETRC=[YES/NO] to
 * enable or disable use of $HOME/.netrc (or NETRC_FILE), default YES.
 * Corresponding configuration option: GDAL_HTTP_NETRC.
 * </li>
 * <li>NETRC_FILE=file name to read .netrc info from  (GDAL >= 3.7).
 * Corresponding configuration option: GDAL_HTTP_NETRC_FILE.
 * </li>
 * <li>CUSTOMREQUEST=val, where val is GET, PUT, POST, DELETE, etc...
 * No corresponding configuration option.
 * </li>
 * <li>FORM_FILE_NAME=val, where val is upload file name. If this
 * option and FORM_FILE_PATH present, request type will set to POST.
 * No corresponding configuration option.
 * </li>
 * <li>FORM_FILE_PATH=val, where val is upload file path.
 * No corresponding configuration option.
 * </li>
 * <li>FORM_KEY_0=val...FORM_KEY_N, where val is name of form item.
 * No corresponding configuration option.
 * </li>
 * <li>FORM_VALUE_0=val...FORM_VALUE_N, where val is value of the form
 * item.
 * No corresponding configuration option.
 * </li>
 * <li>FORM_ITEM_COUNT=val, where val is count of form items.
 * No corresponding configuration option.
 * </li>
 * <li>COOKIE=val, where val is formatted as COOKIE1=VALUE1; COOKIE2=VALUE2;...
 * Corresponding configuration option: GDAL_HTTP_COOKIE.</li>
 * <li>COOKIEFILE=val, where val is file name to read cookies from
 * (GDAL >= 2.4).
 * Corresponding configuration option: GDAL_HTTP_COOKIEFILE.</li>
 * <li>COOKIEJAR=val, where val is file name to store cookies to (GDAL >= 2.4).
 * Corresponding configuration option: GDAL_HTTP_COOKIEJAR.</li>
 * <li>MAX_RETRY=val, where val is the maximum number of
 * retry attempts, when a retry is allowed (cf RETRY_CODES option).
 * Default is 0, meaning no retry.
 * Corresponding configuration option: GDAL_HTTP_MAX_RETRY.
 * </li>
 * <li>RETRY_DELAY=val, where val is the number of seconds
 * between retry attempts. Default is 30.
 * Corresponding configuration option: GDAL_HTTP_RETRY_DELAY.
 * <li>RETRY_CODES=val, where val is "ALL" or a comma-separated list of HTTP
 * codes that are considered for retry. By default, 429, 500, 502, 503 or 504
 * HTTP errors are considered, as well as other situations with a particular
 * HTTP or Curl error message. (GDAL >= 3.10).
 * Corresponding configuration option: GDAL_HTTP_RETRY_CODES.
 * </li>
 * <li>MAX_FILE_SIZE=val, where val is a number of bytes (GDAL >= 2.2)
 * No corresponding configuration option.
 * </li>
 * <li>CAINFO=/path/to/bundle.crt. This is path to Certificate Authority (CA)
 *     bundle file. By default, it will be looked for in a system location. If
 *     the CAINFO option is not defined, GDAL will also look in the
 *     CURL_CA_BUNDLE and SSL_CERT_FILE environment variables respectively
 *     and use the first one found as the CAINFO value (GDAL >= 2.1.3). The
 *     GDAL_CURL_CA_BUNDLE environment variable may also be used to set the
 *     CAINFO value in GDAL >= 3.2.</li>
 * <li>HTTP_VERSION=1.0/1.1/2/2TLS (GDAL >= 2.3)/2PRIOR_KNOWLEDGE (GDAL >= 3.10).
 *     Specify HTTP version to use.
 *     Will default to 1.1 generally (except on some controlled environments,
 *     like Google Compute Engine VMs, where 2TLS will be the default).
 *     Support for HTTP/2 requires curl 7.33 or later, built against nghttp2.
 *     "2TLS" means that HTTP/2 will be attempted for HTTPS connections only.
 *     Whereas "2" means that HTTP/2 will be attempted for HTTP or HTTPS.
 *     "2PRIOR_KNOWLEDGE" means that the server will be assumed to support
 *     HTTP/2.
 *     Corresponding configuration option: GDAL_HTTP_VERSION.
 * </li>
 * <li>SSL_VERIFYSTATUS=YES/NO (GDAL >= 2.3, and curl >= 7.41): determines
 * whether the status of the server cert using the "Certificate Status Request"
 * TLS extension (aka. OCSP stapling) should be checked. If this option is
 * enabled but the server does not support the TLS extension, the verification
 * will fail. Default to NO.
 * Corresponding configuration option: GDAL_HTTP_SSL_VERIFYSTATUS.
 * </li>
 * <li>USE_CAPI_STORE=YES/NO (GDAL >= 2.3,
 * Windows only): whether CA certificates from the Windows certificate store.
 * Defaults to NO.
 * Corresponding configuration option: GDAL_HTTP_USE_CAPI_STORE.
 * </li>
 * <li>TCP_KEEPALIVE=YES/NO (GDAL >= 3.6): whether to
 * enable TCP keep-alive. Defaults to NO.
 * Corresponding configuration option: GDAL_HTTP_TCP_KEEPALIVE.
 * </li>
 * <li>TCP_KEEPIDLE=integer, in
 * seconds (GDAL >= 3.6): keep-alive idle time. Defaults to 60. Only taken into
 * account if TCP_KEEPALIVE=YES.
 * Corresponding configuration option: GDAL_HTTP_TCP_KEEPIDLE.
 * </li>
 * <li>TCP_KEEPINTVL=integer, in seconds
 * (GDAL >= 3.6): interval time between keep-alive probes. Defaults to 60. Only
 * taken into account if TCP_KEEPALIVE=YES.
 * Corresponding configuration option: GDAL_HTTP_TCP_KEEPINTVL.
 * </li>
 * <li>USERAGENT=string: value of User-Agent header. Starting with GDAL 3.7,
 * GDAL core sets it by default (during driver initialization) to GDAL/x.y.z
 * where x.y.z is the GDAL version number. Applications may override it with the
 * CPLHTTPSetDefaultUserAgent() function.
 * Corresponding configuration option: GDAL_HTTP_USERAGENT.
 * </li>
 * <li>SSLCERT=filename (GDAL >= 3.7): Filename of the the SSL client certificate.
 * Cf https://curl.se/libcurl/c/CURLOPT_SSLCERT.html.
 * Corresponding configuration option: GDAL_HTTP_SSLCERT.
 * </li>
 * <li>SSLCERTTYPE=string (GDAL >= 3.7): Format of the SSL certificate: "PEM"
 * or "DER". Cf https://curl.se/libcurl/c/CURLOPT_SSLCERTTYPE.html.
 * Corresponding configuration option: GDAL_HTTP_SSLCERTTYPE.
 * </li>
 * <li>SSLKEY=filename (GDAL >= 3.7): Private key file for TLS and SSL client
 * certificate. Cf https://curl.se/libcurl/c/CURLOPT_SSLKEY.html.
 * Corresponding configuration option: GDAL_HTTP_SSLKEY.
 * </li>
 * <li>KEYPASSWD=string (GDAL >= 3.7): Passphrase to private key.
 * Cf https://curl.se/libcurl/c/CURLOPT_KEYPASSWD.html.
 * Corresponding configuration option: GDAL_HTTP_KEYPASSWD.
 * </ul>
 *
 * If an option is specified through papszOptions and as a configuration option,
 * the former takes precedence over the later.
 *
 * Starting with GDAL 3.7, the above configuration options can also be specified
 * as path-specific options with VSISetPathSpecificOption().
 *
 * @param pszURL valid URL recognized by underlying download library (libcurl)
 * @param papszOptions option list as a NULL-terminated array of strings. May be
 * NULL.
 *
 * @return a CPLHTTPResult* structure that must be freed by
 * CPLHTTPDestroyResult(), or NULL if libcurl support is disabled
 */
// clang-format on
CPLHTTPResult *CPLHTTPFetch(const char *pszURL, CSLConstList papszOptions)
{
    return CPLHTTPFetchEx(pszURL, papszOptions, nullptr, nullptr, nullptr,
                          nullptr);
}

/**
 * Fetch a document from an url and return in a string.
 * @param  pszURL       Url to fetch document from web.
 * @param  papszOptions Option list as a NULL-terminated array of strings.
 * Available keys see in CPLHTTPFetch.
 * @param  pfnProgress  Callback for reporting algorithm progress matching the
 * GDALProgressFunc() semantics. May be NULL.
 * @param  pProgressArg Callback argument passed to pfnProgress.
 * @param  pfnWrite     Write function pointer matching the CPLHTTPWriteFunc()
 * semantics. May be NULL.
 * @param  pWriteArg    Argument which will pass to a write function.
 * @return              A CPLHTTPResult* structure that must be freed by
 * CPLHTTPDestroyResult(), or NULL if libcurl support is disabled.
 */
CPLHTTPResult *CPLHTTPFetchEx(const char *pszURL, CSLConstList papszOptions,
                              GDALProgressFunc pfnProgress, void *pProgressArg,
                              CPLHTTPFetchWriteFunc pfnWrite, void *pWriteArg)

{
    if (STARTS_WITH(pszURL, "/vsimem/") &&
        // Disabled by default for potential security issues.
        CPLTestBool(CPLGetConfigOption("CPL_CURL_ENABLE_VSIMEM", "FALSE")))
    {
        CPLString osURL(pszURL);
        const char *pszCustomRequest =
            CSLFetchNameValue(papszOptions, "CUSTOMREQUEST");
        if (pszCustomRequest != nullptr)
        {
            osURL += "&CUSTOMREQUEST=";
            osURL += pszCustomRequest;
        }
        const char *pszUserPwd = CSLFetchNameValue(papszOptions, "USERPWD");
        if (pszUserPwd != nullptr)
        {
            osURL += "&USERPWD=";
            osURL += pszUserPwd;
        }
        const char *pszPost = CSLFetchNameValue(papszOptions, "POSTFIELDS");
        if (pszPost != nullptr)  // Hack: We append post content to filename.
        {
            osURL += "&POSTFIELDS=";
            osURL += pszPost;
        }
        const char *pszHeaders = CSLFetchNameValue(papszOptions, "HEADERS");
        if (pszHeaders != nullptr &&
            CPLTestBool(
                CPLGetConfigOption("CPL_CURL_VSIMEM_PRINT_HEADERS", "FALSE")))
        {
            osURL += "&HEADERS=";
            osURL += pszHeaders;
        }
        vsi_l_offset nLength = 0;
        CPLHTTPResult *psResult =
            static_cast<CPLHTTPResult *>(CPLCalloc(1, sizeof(CPLHTTPResult)));
        GByte *pabyData = VSIGetMemFileBuffer(osURL, &nLength, FALSE);
        if (pabyData == nullptr)
        {
            CPLDebug("HTTP", "Cannot find %s", osURL.c_str());
            psResult->nStatus = 1;
            psResult->pszErrBuf =
                CPLStrdup(CPLSPrintf("HTTP error code : %d", 404));
            CPLError(CE_Failure, CPLE_AppDefined, "%s", psResult->pszErrBuf);
        }
        else if (nLength != 0)
        {
            psResult->nDataLen = static_cast<int>(nLength);
            psResult->pabyData = static_cast<GByte *>(
                CPLMalloc(static_cast<size_t>(nLength) + 1));
            memcpy(psResult->pabyData, pabyData, static_cast<size_t>(nLength));
            psResult->pabyData[static_cast<size_t>(nLength)] = 0;
        }

        if (psResult->pabyData != nullptr &&
            STARTS_WITH(reinterpret_cast<char *>(psResult->pabyData),
                        "Content-Type: "))
        {
            const char *pszContentType =
                reinterpret_cast<char *>(psResult->pabyData) +
                strlen("Content-type: ");
            const char *pszEOL = strchr(pszContentType, '\r');
            if (pszEOL)
                pszEOL = strchr(pszContentType, '\n');
            if (pszEOL)
            {
                size_t nContentLength = pszEOL - pszContentType;
                psResult->pszContentType =
                    static_cast<char *>(CPLMalloc(nContentLength + 1));
                memcpy(psResult->pszContentType, pszContentType,
                       nContentLength);
                psResult->pszContentType[nContentLength] = 0;
            }
        }

        return psResult;
    }

    // Try to user alternate network layer if set.
    auto pCtx = GetHTTPFetchContext(false);
    if (pCtx)
    {
        for (size_t i = pCtx->stack.size(); i > 0;)
        {
            --i;
            const auto &cbk = pCtx->stack[i];
            auto cbkFunc = cbk.first;
            auto pUserData = cbk.second;
            auto res = cbkFunc(pszURL, papszOptions, pfnProgress, pProgressArg,
                               pfnWrite, pWriteArg, pUserData);
            if (res)
            {
                if (CSLFetchNameValue(papszOptions, "CLOSE_PERSISTENT"))
                {
                    CPLHTTPDestroyResult(res);
                    res = nullptr;
                }
                return res;
            }
        }
    }

    if (gpsHTTPFetchCallbackFunc)
    {
        auto res = gpsHTTPFetchCallbackFunc(pszURL, papszOptions, pfnProgress,
                                            pProgressArg, pfnWrite, pWriteArg,
                                            gpHTTPFetchCallbackUserData);
        if (res)
        {
            if (CSLFetchNameValue(papszOptions, "CLOSE_PERSISTENT"))
            {
                CPLHTTPDestroyResult(res);
                res = nullptr;
            }
            return res;
        }
    }

#ifndef HAVE_CURL
    CPLError(CE_Failure, CPLE_NotSupported,
             "GDAL/OGR not compiled with libcurl support, "
             "remote requests not supported.");
    return nullptr;
#else

    /* -------------------------------------------------------------------- */
    /*      Are we using a persistent named session?  If so, search for     */
    /*      or create it.                                                   */
    /*                                                                      */
    /*      Currently this code does not attempt to protect against         */
    /*      multiple threads asking for the same named session.  If that    */
    /*      occurs it will be in use in multiple threads at once, which     */
    /*      will lead to potential crashes in libcurl.                      */
    /* -------------------------------------------------------------------- */
    CURL *http_handle = nullptr;

    const char *pszPersistent = CSLFetchNameValue(papszOptions, "PERSISTENT");
    const char *pszClosePersistent =
        CSLFetchNameValue(papszOptions, "CLOSE_PERSISTENT");
    if (pszPersistent)
    {
        CPLString osSessionName = pszPersistent;
        CPLMutexHolder oHolder(&hSessionMapMutex);

        if (poSessionMap == nullptr)
            poSessionMap = new std::map<CPLString, CURL *>;
        if (poSessionMap->count(osSessionName) == 0)
        {
            (*poSessionMap)[osSessionName] = curl_easy_init();
            CPLDebug("HTTP", "Establish persistent session named '%s'.",
                     osSessionName.c_str());
        }

        http_handle = (*poSessionMap)[osSessionName];
    }
    /* -------------------------------------------------------------------- */
    /*      Are we requested to close a persistent named session?           */
    /* -------------------------------------------------------------------- */
    else if (pszClosePersistent)
    {
        CPLString osSessionName = pszClosePersistent;
        CPLMutexHolder oHolder(&hSessionMapMutex);

        if (poSessionMap)
        {
            std::map<CPLString, CURL *>::iterator oIter =
                poSessionMap->find(osSessionName);
            if (oIter != poSessionMap->end())
            {
                curl_easy_cleanup(oIter->second);
                poSessionMap->erase(oIter);
                if (poSessionMap->empty())
                {
                    delete poSessionMap;
                    poSessionMap = nullptr;
                }
                CPLDebug("HTTP", "Ended persistent session named '%s'.",
                         osSessionName.c_str());
            }
            else
            {
                CPLDebug("HTTP",
                         "Could not find persistent session named '%s'.",
                         osSessionName.c_str());
            }
        }

        return nullptr;
    }
    else
        http_handle = curl_easy_init();

    /* -------------------------------------------------------------------- */
    /*      Setup the request.                                              */
    /* -------------------------------------------------------------------- */
    char szCurlErrBuf[CURL_ERROR_SIZE + 1] = {};

    CPLHTTPEmitFetchDebug(pszURL);

    CPLHTTPResult *psResult =
        static_cast<CPLHTTPResult *>(CPLCalloc(1, sizeof(CPLHTTPResult)));

    struct curl_slist *headers = reinterpret_cast<struct curl_slist *>(
        CPLHTTPSetOptions(http_handle, pszURL, papszOptions));
    if (headers != nullptr)
        unchecked_curl_easy_setopt(http_handle, CURLOPT_HTTPHEADER, headers);

    // Are we making a head request.
    const char *pszNoBody = nullptr;
    if ((pszNoBody = CSLFetchNameValue(papszOptions, "NO_BODY")) != nullptr)
    {
        if (CPLTestBool(pszNoBody))
        {
            CPLDebug("HTTP", "HEAD Request: %s", pszURL);
            unchecked_curl_easy_setopt(http_handle, CURLOPT_NOBODY, 1L);
        }
    }

    // Capture response headers.
    unchecked_curl_easy_setopt(http_handle, CURLOPT_HEADERDATA, psResult);
    unchecked_curl_easy_setopt(http_handle, CURLOPT_HEADERFUNCTION,
                               CPLHdrWriteFct);

    CPLHTTPResultWithLimit sResultWithLimit;
    if (nullptr == pfnWrite)
    {
        pfnWrite = CPLWriteFct;

        sResultWithLimit.psResult = psResult;
        sResultWithLimit.nMaxFileSize = 0;
        const char *pszMaxFileSize =
            CSLFetchNameValue(papszOptions, "MAX_FILE_SIZE");
        if (pszMaxFileSize != nullptr)
        {
            sResultWithLimit.nMaxFileSize = atoi(pszMaxFileSize);
            // Only useful if size is returned by server before actual download.
            unchecked_curl_easy_setopt(http_handle, CURLOPT_MAXFILESIZE,
                                       sResultWithLimit.nMaxFileSize);
        }
        pWriteArg = &sResultWithLimit;
    }

    unchecked_curl_easy_setopt(http_handle, CURLOPT_WRITEDATA, pWriteArg);
    unchecked_curl_easy_setopt(http_handle, CURLOPT_WRITEFUNCTION, pfnWrite);

    CurlProcessData stProcessData = {pfnProgress, pProgressArg};
    if (nullptr != pfnProgress)
    {
        unchecked_curl_easy_setopt(http_handle, CURLOPT_XFERINFOFUNCTION,
                                   NewProcessFunction);
        unchecked_curl_easy_setopt(http_handle, CURLOPT_XFERINFODATA,
                                   &stProcessData);
        unchecked_curl_easy_setopt(http_handle, CURLOPT_NOPROGRESS, 0L);
    }

    szCurlErrBuf[0] = '\0';

    unchecked_curl_easy_setopt(http_handle, CURLOPT_ERRORBUFFER, szCurlErrBuf);

    bool bGZipRequested = false;
    if (bSupportGZip && CPLTestBool(CPLGetConfigOption("CPL_CURL_GZIP", "YES")))
    {
        bGZipRequested = true;
        unchecked_curl_easy_setopt(http_handle, CURLOPT_ENCODING, "gzip");
    }

    CPLHTTPPostFields oPostFields;
    if (oPostFields.Fill(http_handle, papszOptions) != CE_None)
    {
        psResult->nStatus = 34;  // CURLE_HTTP_POST_ERROR
        psResult->pszErrBuf = CPLStrdup(oPostFields.GetErrorMessage().c_str());
        CPLError(CE_Failure, CPLE_AppDefined, "%s", psResult->pszErrBuf);
        CPLHTTPFetchCleanup(http_handle, headers, pszPersistent, papszOptions);
        return psResult;
    }

    /* -------------------------------------------------------------------- */
    /*      Depending on status code, retry this HTTP call until max        */
    /*      retry has been reached                                          */
    /* -------------------------------------------------------------------- */
    const char *pszRetryDelay = CSLFetchNameValue(papszOptions, "RETRY_DELAY");
    if (pszRetryDelay == nullptr)
        pszRetryDelay = CPLGetConfigOption(
            "GDAL_HTTP_RETRY_DELAY", CPLSPrintf("%f", CPL_HTTP_RETRY_DELAY));
    const char *pszMaxRetries = CSLFetchNameValue(papszOptions, "MAX_RETRY");
    if (pszMaxRetries == nullptr)
        pszMaxRetries = CPLGetConfigOption(
            "GDAL_HTTP_MAX_RETRY", CPLSPrintf("%d", CPL_HTTP_MAX_RETRY));
    // coverity[tainted_data]
    double dfRetryDelaySecs = CPLAtof(pszRetryDelay);
    int nMaxRetries = atoi(pszMaxRetries);
    const char *pszRetryCodes = CSLFetchNameValue(papszOptions, "RETRY_CODES");
    if (!pszRetryCodes)
        pszRetryCodes = CPLGetConfigOption("GDAL_HTTP_RETRY_CODES", nullptr);
    int nRetryCount = 0;

    while (true)
    {
        /* --------------------------------------------------------------------
         */
        /*      Execute the request, waiting for results. */
        /* --------------------------------------------------------------------
         */
        void *old_handler = CPLHTTPIgnoreSigPipe();
        psResult->nStatus = static_cast<int>(curl_easy_perform(http_handle));
        CPLHTTPRestoreSigPipeHandler(old_handler);

        /* --------------------------------------------------------------------
         */
        /*      Fetch content-type if possible. */
        /* --------------------------------------------------------------------
         */
        psResult->pszContentType = nullptr;
        curl_easy_getinfo(http_handle, CURLINFO_CONTENT_TYPE,
                          &(psResult->pszContentType));
        if (psResult->pszContentType != nullptr)
            psResult->pszContentType = CPLStrdup(psResult->pszContentType);

        long response_code = 0;
        curl_easy_getinfo(http_handle, CURLINFO_RESPONSE_CODE, &response_code);
        if (response_code != 200)
        {
            const double dfNewRetryDelay = CPLHTTPGetNewRetryDelay(
                static_cast<int>(response_code), dfRetryDelaySecs,
                reinterpret_cast<const char *>(psResult->pabyData),
                szCurlErrBuf, pszRetryCodes);
            if (dfNewRetryDelay > 0 && nRetryCount < nMaxRetries)
            {
                CPLError(CE_Warning, CPLE_AppDefined,
                         "HTTP error code: %d - %s. "
                         "Retrying again in %.1f secs",
                         static_cast<int>(response_code), pszURL,
                         dfRetryDelaySecs);
                CPLSleep(dfRetryDelaySecs);
                dfRetryDelaySecs = dfNewRetryDelay;
                nRetryCount++;

                CPLFree(psResult->pszContentType);
                psResult->pszContentType = nullptr;
                CSLDestroy(psResult->papszHeaders);
                psResult->papszHeaders = nullptr;
                CPLFree(psResult->pabyData);
                psResult->pabyData = nullptr;
                psResult->nDataLen = 0;
                psResult->nDataAlloc = 0;

                continue;
            }
        }

        /* --------------------------------------------------------------------
         */
        /*      Have we encountered some sort of error? */
        /* --------------------------------------------------------------------
         */
        if (strlen(szCurlErrBuf) > 0)
        {
            bool bSkipError = false;
            const char *pszContentLength =
                CSLFetchNameValue(psResult->papszHeaders, "Content-Length");
            // Some servers such as
            // http://115.113.193.14/cgi-bin/world/qgis_mapserv.fcgi?VERSION=1.1.1&SERVICE=WMS&REQUEST=GetCapabilities
            // invalidly return Content-Length as the uncompressed size, with
            // makes curl to wait for more data and time-out finally. If we got
            // the expected data size, then we don't emit an error but turn off
            // GZip requests.
            if (bGZipRequested &&
                strstr(szCurlErrBuf, "transfer closed with") &&
                strstr(szCurlErrBuf, "bytes remaining to read"))
            {
                if (pszContentLength && psResult->nDataLen != 0 &&
                    atoi(pszContentLength) == psResult->nDataLen)
                {
                    const char *pszCurlGZIPOption =
                        CPLGetConfigOption("CPL_CURL_GZIP", nullptr);
                    if (pszCurlGZIPOption == nullptr)
                    {
                        CPLSetConfigOption("CPL_CURL_GZIP", "NO");
                        CPLDebug("HTTP",
                                 "Disabling CPL_CURL_GZIP, "
                                 "because %s doesn't support it properly",
                                 pszURL);
                    }
                    psResult->nStatus = 0;
                    bSkipError = true;
                }
            }

            // Ignore SSL errors about non-properly terminated connection,
            // often due to HTTP proxies
            else if (pszContentLength == nullptr &&
                     // Cf https://github.com/curl/curl/pull/3148
                     (strstr(szCurlErrBuf,
                             "GnuTLS recv error (-110): The TLS connection was "
                             "non-properly terminated") != nullptr ||
                      // Cf https://github.com/curl/curl/issues/9024
                      strstr(szCurlErrBuf,
                             "SSL_read: error:0A000126:SSL "
                             "routines::unexpected eof while reading") !=
                          nullptr))
            {
                psResult->nStatus = 0;
                bSkipError = true;
            }
            else if (CPLTestBool(
                         CPLGetConfigOption("CPL_CURL_IGNORE_ERROR", "NO")))
            {
                psResult->nStatus = 0;
                bSkipError = true;
            }

            if (!bSkipError)
            {
                psResult->pszErrBuf = CPLStrdup(szCurlErrBuf);
                if (psResult->nDataLen > 0)
                {
                    CPLError(CE_Failure, CPLE_AppDefined,
                             "%s. You may set the CPL_CURL_IGNORE_ERROR "
                             "configuration option to YES to try to ignore it.",
                             szCurlErrBuf);
                }
                else
                {
                    CPLError(CE_Failure, CPLE_AppDefined, "%s", szCurlErrBuf);
                }
            }
        }
        else
        {
            if (response_code >= 400 && response_code < 600)
            {
                psResult->pszErrBuf = CPLStrdup(CPLSPrintf(
                    "HTTP error code : %d", static_cast<int>(response_code)));
                CPLError(CE_Failure, CPLE_AppDefined, "%s",
                         psResult->pszErrBuf);
            }
        }
        break;
    }

    CPLHTTPFetchCleanup(http_handle, headers, pszPersistent, papszOptions);

    return psResult;
#endif /* def HAVE_CURL */
}

#ifdef HAVE_CURL
/************************************************************************/
/*                       CPLMultiPerformWait()                          */
/************************************************************************/

bool CPLMultiPerformWait(void *hCurlMultiHandleIn, int & /*repeats*/)
{
    CURLM *hCurlMultiHandle = static_cast<CURLM *>(hCurlMultiHandleIn);

    // Wait for events on the sockets

    // Using curl_multi_poll() is preferred to avoid hitting the 1024 file
    // descriptor limit

    int numfds = 0;
    if (curl_multi_poll(hCurlMultiHandle, nullptr, 0, 1000, &numfds) !=
        CURLM_OK)
    {
        CPLError(CE_Failure, CPLE_AppDefined, "curl_multi_poll() failed");
        return false;
    }
    return true;
}

class CPLHTTPErrorBuffer
{
  public:
    char szBuffer[CURL_ERROR_SIZE + 1];

    CPLHTTPErrorBuffer()
    {
        szBuffer[0] = '\0';
    }
};

#endif  // HAVE_CURL

/************************************************************************/
/*                           CPLHTTPMultiFetch()                        */
/************************************************************************/

/**
 * \brief Fetch several documents at once
 *
 * @param papszURL array of valid URLs recognized by underlying download library
 * (libcurl)
 * @param nURLCount number of URLs of papszURL
 * @param nMaxSimultaneous maximum number of downloads to issue simultaneously.
 *                         Any negative or zer value means unlimited.
 * @param papszOptions option list as a NULL-terminated array of strings. May be
 * NULL. Refer to CPLHTTPFetch() for valid options.
 * @return an array of CPLHTTPResult* structures that must be freed by
 * CPLHTTPDestroyMultiResult() or NULL if libcurl support is disabled
 *
 * @since GDAL 2.3
 */
CPLHTTPResult **CPLHTTPMultiFetch(const char *const *papszURL, int nURLCount,
                                  int nMaxSimultaneous,
                                  CSLConstList papszOptions)
{
#ifndef HAVE_CURL
    (void)papszURL;
    (void)nURLCount;
    (void)nMaxSimultaneous;
    (void)papszOptions;

    CPLError(CE_Failure, CPLE_NotSupported,
             "GDAL/OGR not compiled with libcurl support, "
             "remote requests not supported.");
    return nullptr;
#else  /* def HAVE_CURL */

    /* -------------------------------------------------------------------- */
    /*      Are we using a persistent named session?  If so, search for     */
    /*      or create it.                                                   */
    /*                                                                      */
    /*      Currently this code does not attempt to protect against         */
    /*      multiple threads asking for the same named session.  If that    */
    /*      occurs it will be in use in multiple threads at once, which     */
    /*      will lead to potential crashes in libcurl.                      */
    /* -------------------------------------------------------------------- */
    CURLM *hCurlMultiHandle = nullptr;

    const char *pszPersistent = CSLFetchNameValue(papszOptions, "PERSISTENT");
    const char *pszClosePersistent =
        CSLFetchNameValue(papszOptions, "CLOSE_PERSISTENT");
    if (pszPersistent)
    {
        CPLString osSessionName = pszPersistent;
        CPLMutexHolder oHolder(&hSessionMapMutex);

        if (poSessionMultiMap == nullptr)
            poSessionMultiMap = new std::map<CPLString, CURLM *>;
        if (poSessionMultiMap->count(osSessionName) == 0)
        {
            (*poSessionMultiMap)[osSessionName] = curl_multi_init();
            CPLDebug("HTTP", "Establish persistent session named '%s'.",
                     osSessionName.c_str());
        }

        hCurlMultiHandle = (*poSessionMultiMap)[osSessionName];
    }
    /* -------------------------------------------------------------------- */
    /*      Are we requested to close a persistent named session?           */
    /* -------------------------------------------------------------------- */
    else if (pszClosePersistent)
    {
        CPLString osSessionName = pszClosePersistent;
        CPLMutexHolder oHolder(&hSessionMapMutex);

        if (poSessionMultiMap)
        {
            auto oIter = poSessionMultiMap->find(osSessionName);
            if (oIter != poSessionMultiMap->end())
            {
                VSICURLMultiCleanup(oIter->second);
                poSessionMultiMap->erase(oIter);
                if (poSessionMultiMap->empty())
                {
                    delete poSessionMultiMap;
                    poSessionMultiMap = nullptr;
                }
                CPLDebug("HTTP", "Ended persistent session named '%s'.",
                         osSessionName.c_str());
            }
            else
            {
                CPLDebug("HTTP",
                         "Could not find persistent session named '%s'.",
                         osSessionName.c_str());
            }
        }

        return nullptr;
    }
    else
    {
        hCurlMultiHandle = curl_multi_init();
    }

    CPLHTTPResult **papsResults = static_cast<CPLHTTPResult **>(
        CPLCalloc(nURLCount, sizeof(CPLHTTPResult *)));
    std::vector<CURL *> asHandles;
    std::vector<CPLHTTPResultWithLimit> asResults;
    asResults.resize(nURLCount);
    std::vector<struct curl_slist *> aHeaders;
    aHeaders.resize(nURLCount);
    std::vector<CPLHTTPErrorBuffer> asErrorBuffers;
    asErrorBuffers.resize(nURLCount);

    for (int i = 0; i < nURLCount; i++)
    {
        papsResults[i] =
            static_cast<CPLHTTPResult *>(CPLCalloc(1, sizeof(CPLHTTPResult)));

        const char *pszURL = papszURL[i];
        CURL *http_handle = curl_easy_init();

        aHeaders[i] = reinterpret_cast<struct curl_slist *>(
            CPLHTTPSetOptions(http_handle, pszURL, papszOptions));

        // Set Headers.
        const char *pszHeaders = CSLFetchNameValue(papszOptions, "HEADERS");
        if (pszHeaders != nullptr)
        {
            char **papszTokensHeaders =
                CSLTokenizeString2(pszHeaders, "\r\n", 0);
            for (int j = 0; papszTokensHeaders[j] != nullptr; ++j)
                aHeaders[i] =
                    curl_slist_append(aHeaders[i], papszTokensHeaders[j]);
            CSLDestroy(papszTokensHeaders);
        }

        if (aHeaders[i] != nullptr)
            unchecked_curl_easy_setopt(http_handle, CURLOPT_HTTPHEADER,
                                       aHeaders[i]);

        // Capture response headers.
        unchecked_curl_easy_setopt(http_handle, CURLOPT_HEADERDATA,
                                   papsResults[i]);
        unchecked_curl_easy_setopt(http_handle, CURLOPT_HEADERFUNCTION,
                                   CPLHdrWriteFct);

        asResults[i].psResult = papsResults[i];
        const char *pszMaxFileSize =
            CSLFetchNameValue(papszOptions, "MAX_FILE_SIZE");
        if (pszMaxFileSize != nullptr)
        {
            asResults[i].nMaxFileSize = atoi(pszMaxFileSize);
            // Only useful if size is returned by server before actual download.
            unchecked_curl_easy_setopt(http_handle, CURLOPT_MAXFILESIZE,
                                       asResults[i].nMaxFileSize);
        }

        unchecked_curl_easy_setopt(http_handle, CURLOPT_WRITEDATA,
                                   &asResults[i]);
        unchecked_curl_easy_setopt(http_handle, CURLOPT_WRITEFUNCTION,
                                   CPLWriteFct);

        unchecked_curl_easy_setopt(http_handle, CURLOPT_ERRORBUFFER,
                                   asErrorBuffers[i].szBuffer);

        if (bSupportGZip &&
            CPLTestBool(CPLGetConfigOption("CPL_CURL_GZIP", "YES")))
        {
            unchecked_curl_easy_setopt(http_handle, CURLOPT_ENCODING, "gzip");
        }

        asHandles.push_back(http_handle);
    }

    int iCurRequest = 0;
    for (;
         iCurRequest <
         std::min(nURLCount, nMaxSimultaneous > 0 ? nMaxSimultaneous : INT_MAX);
         iCurRequest++)
    {
        CPLHTTPEmitFetchDebug(papszURL[iCurRequest],
                              CPLSPrintf(" %d/%d", iCurRequest + 1, nURLCount));
        curl_multi_add_handle(hCurlMultiHandle, asHandles[iCurRequest]);
    }

    int repeats = 0;
    void *old_handler = CPLHTTPIgnoreSigPipe();
    while (true)
    {
        int still_running = 0;
        while (curl_multi_perform(hCurlMultiHandle, &still_running) ==
               CURLM_CALL_MULTI_PERFORM)
        {
            // loop
        }
        if (!still_running && iCurRequest == nURLCount)
        {
            break;
        }

        bool bRequestsAdded = false;
        CURLMsg *msg;
        do
        {
            int msgq = 0;
            msg = curl_multi_info_read(hCurlMultiHandle, &msgq);
            if (msg && (msg->msg == CURLMSG_DONE))
            {
                if (iCurRequest < nURLCount)
                {
                    CPLHTTPEmitFetchDebug(
                        papszURL[iCurRequest],
                        CPLSPrintf(" %d/%d", iCurRequest + 1, nURLCount));
                    curl_multi_add_handle(hCurlMultiHandle,
                                          asHandles[iCurRequest]);
                    iCurRequest++;
                    bRequestsAdded = true;
                }
            }
        } while (msg);

        if (!bRequestsAdded)
            CPLMultiPerformWait(hCurlMultiHandle, repeats);
    }
    CPLHTTPRestoreSigPipeHandler(old_handler);

    for (int i = 0; i < nURLCount; i++)
    {
        if (asErrorBuffers[i].szBuffer[0] != '\0')
            papsResults[i]->pszErrBuf = CPLStrdup(asErrorBuffers[i].szBuffer);
        else
        {
            long response_code = 0;
            curl_easy_getinfo(asHandles[i], CURLINFO_RESPONSE_CODE,
                              &response_code);

            if (response_code >= 400 && response_code < 600)
            {
                papsResults[i]->pszErrBuf = CPLStrdup(CPLSPrintf(
                    "HTTP error code : %d", static_cast<int>(response_code)));
            }
        }

        curl_easy_getinfo(asHandles[i], CURLINFO_CONTENT_TYPE,
                          &(papsResults[i]->pszContentType));
        if (papsResults[i]->pszContentType != nullptr)
            papsResults[i]->pszContentType =
                CPLStrdup(papsResults[i]->pszContentType);

        curl_multi_remove_handle(hCurlMultiHandle, asHandles[i]);
        curl_easy_cleanup(asHandles[i]);
    }

    if (!pszPersistent)
        VSICURLMultiCleanup(hCurlMultiHandle);

    for (size_t i = 0; i < aHeaders.size(); i++)
        curl_slist_free_all(aHeaders[i]);

    return papsResults;
#endif /* def HAVE_CURL */
}

/************************************************************************/
/*                      CPLHTTPDestroyMultiResult()                     */
/************************************************************************/
/**
 * \brief Clean the memory associated with the return value of
 * CPLHTTPMultiFetch()
 *
 * @param papsResults pointer to the return value of CPLHTTPMultiFetch()
 * @param nCount value of the nURLCount parameter passed to CPLHTTPMultiFetch()
 * @since GDAL 2.3
 */
void CPLHTTPDestroyMultiResult(CPLHTTPResult **papsResults, int nCount)
{
    if (papsResults)
    {
        for (int i = 0; i < nCount; i++)
        {
            CPLHTTPDestroyResult(papsResults[i]);
        }
        CPLFree(papsResults);
    }
}

#ifdef HAVE_CURL

#ifdef _WIN32

#include <windows.h>

/************************************************************************/
/*                     CPLFindWin32CurlCaBundleCrt()                    */
/************************************************************************/

static const char *CPLFindWin32CurlCaBundleCrt()
{
    DWORD nResLen;
    const DWORD nBufSize = MAX_PATH + 1;
    char *pszFilePart = nullptr;

    char *pszPath = static_cast<char *>(CPLCalloc(1, nBufSize));

    nResLen = SearchPathA(nullptr, "curl-ca-bundle.crt", nullptr, nBufSize,
                          pszPath, &pszFilePart);
    if (nResLen > 0)
    {
        const char *pszRet = CPLSPrintf("%s", pszPath);
        CPLFree(pszPath);
        return pszRet;
    }
    CPLFree(pszPath);
    return nullptr;
}

#endif  // WIN32

/************************************************************************/
/*                     CPLHTTPCurlDebugFunction()                       */
/************************************************************************/

static int CPLHTTPCurlDebugFunction(CURL *handle, curl_infotype type,
                                    char *data, size_t size, void *userp)
{
    (void)handle;
    (void)userp;

    const char *pszDebugKey = nullptr;
    if (type == CURLINFO_TEXT)
    {
        pszDebugKey = "CURL_INFO_TEXT";
    }
    else if (type == CURLINFO_HEADER_OUT)
    {
        pszDebugKey = "CURL_INFO_HEADER_OUT";
    }
    else if (type == CURLINFO_HEADER_IN)
    {
        pszDebugKey = "CURL_INFO_HEADER_IN";
    }
    else if (type == CURLINFO_DATA_IN &&
             CPLTestBool(CPLGetConfigOption("CPL_CURL_VERBOSE_DATA_IN", "NO")))
    {
        pszDebugKey = "CURL_INFO_DATA_IN";
    }

    if (pszDebugKey)
    {
        std::string osMsg(data, size);
        if (!osMsg.empty() && osMsg.back() == '\n')
            osMsg.pop_back();
        CPLDebug(pszDebugKey, "%s", osMsg.c_str());
    }
    return 0;
}

/************************************************************************/
/*                         CPLHTTPSetOptions()                          */
/************************************************************************/

// Note: papszOptions must be kept alive until curl_easy/multi_perform()
// has completed, and we must be careful not to set short lived strings
// with unchecked_curl_easy_setopt(), as long as we need to support curl < 7.17
// see https://curl.haxx.se/libcurl/c/unchecked_curl_easy_setopt.html
// caution: if we remove that assumption, we'll needto use
// CURLOPT_COPYPOSTFIELDS

void *CPLHTTPSetOptions(void *pcurl, const char *pszURL,
                        const char *const *papszOptions)
{
    CheckCurlFeatures();

    CURL *http_handle = reinterpret_cast<CURL *>(pcurl);

    unchecked_curl_easy_setopt(http_handle, CURLOPT_URL, pszURL);

    if (CPLTestBool(CPLGetConfigOption("CPL_CURL_VERBOSE", "NO")))
    {
        unchecked_curl_easy_setopt(http_handle, CURLOPT_VERBOSE, 1);

        if (CPLIsDebugEnabled())
        {
            unchecked_curl_easy_setopt(http_handle, CURLOPT_DEBUGFUNCTION,
                                       CPLHTTPCurlDebugFunction);
        }
    }

    const char *pszHttpVersion =
        CSLFetchNameValue(papszOptions, "HTTP_VERSION");
    if (pszHttpVersion == nullptr)
        pszHttpVersion = CPLGetConfigOption("GDAL_HTTP_VERSION", nullptr);
    if (pszHttpVersion && strcmp(pszHttpVersion, "1.0") == 0)
        unchecked_curl_easy_setopt(http_handle, CURLOPT_HTTP_VERSION,
                                   CURL_HTTP_VERSION_1_0);
    else if (pszHttpVersion && strcmp(pszHttpVersion, "1.1") == 0)
        unchecked_curl_easy_setopt(http_handle, CURLOPT_HTTP_VERSION,
                                   CURL_HTTP_VERSION_1_1);
    else if (pszHttpVersion && (strcmp(pszHttpVersion, "2") == 0 ||
                                strcmp(pszHttpVersion, "2.0") == 0))
    {
        if (bSupportHTTP2)
        {
            // Try HTTP/2 both for HTTP and HTTPS. With fallback to HTTP/1.1
            unchecked_curl_easy_setopt(http_handle, CURLOPT_HTTP_VERSION,
                                       CURL_HTTP_VERSION_2_0);
        }
    }
    else if (pszHttpVersion && strcmp(pszHttpVersion, "2PRIOR_KNOWLEDGE") == 0)
    {
        if (bSupportHTTP2)
        {
            // Assume HTTP/2 is supported by the server. The cURL docs indicate
            // that it makes no difference for HTTPS, but it does seem to work
            // in practice.
            unchecked_curl_easy_setopt(http_handle, CURLOPT_HTTP_VERSION,
                                       CURL_HTTP_VERSION_2_PRIOR_KNOWLEDGE);
        }
    }
    else if (pszHttpVersion == nullptr || strcmp(pszHttpVersion, "2TLS") == 0)
    {
        if (bSupportHTTP2)
        {
            // Only enable this mode if explicitly required, or if the
            // machine is a GCE instance. On other networks, requesting a
            // file in HTTP/2 is found to be significantly slower than HTTP/1.1
            // for unknown reasons.
            if (pszHttpVersion != nullptr || CPLIsMachineForSureGCEInstance())
            {
                static bool bDebugEmitted = false;
                if (!bDebugEmitted)
                {
                    CPLDebug("HTTP", "Using HTTP/2 for HTTPS when possible");
                    bDebugEmitted = true;
                }

                // CURL_HTTP_VERSION_2TLS means for HTTPS connection, try to
                // negotiate HTTP/2 with the server (and fallback to HTTP/1.1
                // otherwise), and for HTTP connection do HTTP/1
                unchecked_curl_easy_setopt(http_handle, CURLOPT_HTTP_VERSION,
                                           CURL_HTTP_VERSION_2TLS);
            }
        }
    }
    else
    {
        CPLError(CE_Warning, CPLE_NotSupported, "HTTP_VERSION=%s not supported",
                 pszHttpVersion);
    }

    // Default value is 1 since curl 7.50.2. But worth applying it on
    // previous versions as well.
    const char *pszTCPNoDelay =
        CSLFetchNameValueDef(papszOptions, "TCP_NODELAY", "1");
    unchecked_curl_easy_setopt(http_handle, CURLOPT_TCP_NODELAY,
                               atoi(pszTCPNoDelay));

    /* Support control over HTTPAUTH */
    const char *pszHttpAuth = CSLFetchNameValue(papszOptions, "HTTPAUTH");
    if (pszHttpAuth == nullptr)
        pszHttpAuth = CPLGetConfigOption("GDAL_HTTP_AUTH", nullptr);
    if (pszHttpAuth == nullptr)
    {
        /* do nothing */;
    }
    else if (EQUAL(pszHttpAuth, "BASIC"))
        unchecked_curl_easy_setopt(http_handle, CURLOPT_HTTPAUTH,
                                   CURLAUTH_BASIC);
    else if (EQUAL(pszHttpAuth, "NTLM"))
        unchecked_curl_easy_setopt(http_handle, CURLOPT_HTTPAUTH,
                                   CURLAUTH_NTLM);
    else if (EQUAL(pszHttpAuth, "ANY"))
        unchecked_curl_easy_setopt(http_handle, CURLOPT_HTTPAUTH, CURLAUTH_ANY);
    else if (EQUAL(pszHttpAuth, "ANYSAFE"))
        unchecked_curl_easy_setopt(http_handle, CURLOPT_HTTPAUTH,
                                   CURLAUTH_ANYSAFE);
    else if (EQUAL(pszHttpAuth, "BEARER"))
    {
        const char *pszAuthorizationHeaderAllowed = CSLFetchNameValueDef(
            papszOptions, "AUTHORIZATION_HEADER_ALLOWED", "YES");
        const bool bAuthorizationHeaderAllowed =
            CPLTestBool(pszAuthorizationHeaderAllowed);
        if (bAuthorizationHeaderAllowed)
        {
            const char *pszBearer =
                CSLFetchNameValue(papszOptions, "HTTP_BEARER");
            if (pszBearer == nullptr)
                pszBearer = CPLGetConfigOption("GDAL_HTTP_BEARER", nullptr);
            if (pszBearer != nullptr)
                unchecked_curl_easy_setopt(http_handle, CURLOPT_XOAUTH2_BEARER,
                                           pszBearer);
            unchecked_curl_easy_setopt(http_handle, CURLOPT_HTTPAUTH,
                                       CURLAUTH_BEARER);
        }
    }
    else if (EQUAL(pszHttpAuth, "NEGOTIATE"))
        unchecked_curl_easy_setopt(http_handle, CURLOPT_HTTPAUTH,
                                   CURLAUTH_NEGOTIATE);
    else
    {
        CPLError(CE_Warning, CPLE_AppDefined,
                 "Unsupported HTTPAUTH value '%s', ignored.", pszHttpAuth);
    }

    const char *pszGssDelegation =
        CSLFetchNameValue(papszOptions, "GSSAPI_DELEGATION");
    if (pszGssDelegation == nullptr)
        pszGssDelegation =
            CPLGetConfigOption("GDAL_GSSAPI_DELEGATION", nullptr);
    if (pszGssDelegation == nullptr)
    {
    }
    else if (EQUAL(pszGssDelegation, "NONE"))
    {
        unchecked_curl_easy_setopt(http_handle, CURLOPT_GSSAPI_DELEGATION,
                                   CURLGSSAPI_DELEGATION_NONE);
    }
    else if (EQUAL(pszGssDelegation, "POLICY"))
    {
        unchecked_curl_easy_setopt(http_handle, CURLOPT_GSSAPI_DELEGATION,
                                   CURLGSSAPI_DELEGATION_POLICY_FLAG);
    }
    else if (EQUAL(pszGssDelegation, "ALWAYS"))
    {
        unchecked_curl_easy_setopt(http_handle, CURLOPT_GSSAPI_DELEGATION,
                                   CURLGSSAPI_DELEGATION_FLAG);
    }
    else
    {
        CPLError(CE_Warning, CPLE_AppDefined,
                 "Unsupported GSSAPI_DELEGATION value '%s', ignored.",
                 pszGssDelegation);
    }

    // Support use of .netrc - default enabled.
    const char *pszHttpNetrc = CSLFetchNameValue(papszOptions, "NETRC");
    if (pszHttpNetrc == nullptr)
        pszHttpNetrc = CPLGetConfigOption("GDAL_HTTP_NETRC", "YES");
    if (pszHttpNetrc == nullptr || CPLTestBool(pszHttpNetrc))
        unchecked_curl_easy_setopt(http_handle, CURLOPT_NETRC, 1L);

    // Custom .netrc file location
    const char *pszHttpNetrcFile =
        CSLFetchNameValue(papszOptions, "NETRC_FILE");
    if (pszHttpNetrcFile == nullptr)
        pszHttpNetrcFile = CPLGetConfigOption("GDAL_HTTP_NETRC_FILE", nullptr);
    if (pszHttpNetrcFile)
        unchecked_curl_easy_setopt(http_handle, CURLOPT_NETRC_FILE,
                                   pszHttpNetrcFile);

    // Support setting userid:password.
    const char *pszUserPwd = CSLFetchNameValue(papszOptions, "USERPWD");
    if (pszUserPwd == nullptr)
        pszUserPwd = CPLGetConfigOption("GDAL_HTTP_USERPWD", nullptr);
    if (pszUserPwd != nullptr)
        unchecked_curl_easy_setopt(http_handle, CURLOPT_USERPWD, pszUserPwd);

    // Set Proxy parameters.
    const char *pszProxy = CSLFetchNameValue(papszOptions, "PROXY");
    if (pszProxy == nullptr)
        pszProxy = CPLGetConfigOption("GDAL_HTTP_PROXY", nullptr);
    if (pszProxy)
        unchecked_curl_easy_setopt(http_handle, CURLOPT_PROXY, pszProxy);

    const char *pszHttpsProxy = CSLFetchNameValue(papszOptions, "HTTPS_PROXY");
    if (pszHttpsProxy == nullptr)
        pszHttpsProxy = CPLGetConfigOption("GDAL_HTTPS_PROXY", nullptr);
    if (pszHttpsProxy && (STARTS_WITH(pszURL, "https")))
        unchecked_curl_easy_setopt(http_handle, CURLOPT_PROXY, pszHttpsProxy);

    const char *pszProxyUserPwd =
        CSLFetchNameValue(papszOptions, "PROXYUSERPWD");
    if (pszProxyUserPwd == nullptr)
        pszProxyUserPwd = CPLGetConfigOption("GDAL_HTTP_PROXYUSERPWD", nullptr);
    if (pszProxyUserPwd)
        unchecked_curl_easy_setopt(http_handle, CURLOPT_PROXYUSERPWD,
                                   pszProxyUserPwd);

    // Support control over PROXYAUTH.
    const char *pszProxyAuth = CSLFetchNameValue(papszOptions, "PROXYAUTH");
    if (pszProxyAuth == nullptr)
        pszProxyAuth = CPLGetConfigOption("GDAL_PROXY_AUTH", nullptr);
    if (pszProxyAuth == nullptr)
    {
        // Do nothing.
    }
    else if (EQUAL(pszProxyAuth, "BASIC"))
        unchecked_curl_easy_setopt(http_handle, CURLOPT_PROXYAUTH,
                                   CURLAUTH_BASIC);
    else if (EQUAL(pszProxyAuth, "NTLM"))
        unchecked_curl_easy_setopt(http_handle, CURLOPT_PROXYAUTH,
                                   CURLAUTH_NTLM);
    else if (EQUAL(pszProxyAuth, "DIGEST"))
        unchecked_curl_easy_setopt(http_handle, CURLOPT_PROXYAUTH,
                                   CURLAUTH_DIGEST);
    else if (EQUAL(pszProxyAuth, "ANY"))
        unchecked_curl_easy_setopt(http_handle, CURLOPT_PROXYAUTH,
                                   CURLAUTH_ANY);
    else if (EQUAL(pszProxyAuth, "ANYSAFE"))
        unchecked_curl_easy_setopt(http_handle, CURLOPT_PROXYAUTH,
                                   CURLAUTH_ANYSAFE);
    else if (EQUAL(pszProxyAuth, "NEGOTIATE"))
        unchecked_curl_easy_setopt(http_handle, CURLOPT_PROXYAUTH,
                                   CURLAUTH_NEGOTIATE);
    else
    {
        CPLError(CE_Warning, CPLE_AppDefined,
                 "Unsupported PROXYAUTH value '%s', ignored.", pszProxyAuth);
    }

    unchecked_curl_easy_setopt(http_handle, CURLOPT_SUPPRESS_CONNECT_HEADERS,
                               1L);

    unchecked_curl_easy_setopt(http_handle, CURLOPT_FOLLOWLOCATION, 1);
    const char *pszUnrestrictedAuth = CPLGetConfigOption(
        "CPL_VSIL_CURL_AUTHORIZATION_HEADER_ALLOWED_IF_REDIRECT",
        "IF_SAME_HOST");
    if (!EQUAL(pszUnrestrictedAuth, "IF_SAME_HOST") &&
        CPLTestBool(pszUnrestrictedAuth))
    {
        unchecked_curl_easy_setopt(http_handle, CURLOPT_UNRESTRICTED_AUTH, 1);
    }

    unchecked_curl_easy_setopt(http_handle, CURLOPT_MAXREDIRS, 10);
    unchecked_curl_easy_setopt(http_handle, CURLOPT_POSTREDIR,
                               CURL_REDIR_POST_ALL);

    // Set connect timeout.
    const char *pszConnectTimeout =
        CSLFetchNameValue(papszOptions, "CONNECTTIMEOUT");
    if (pszConnectTimeout == nullptr)
        pszConnectTimeout =
            CPLGetConfigOption("GDAL_HTTP_CONNECTTIMEOUT", nullptr);
    if (pszConnectTimeout != nullptr)
    {
        // coverity[tainted_data]
        unchecked_curl_easy_setopt(
            http_handle, CURLOPT_CONNECTTIMEOUT_MS,
            static_cast<int>(1000 * CPLAtof(pszConnectTimeout)));
    }

    // Set timeout.
    const char *pszTimeout = CSLFetchNameValue(papszOptions, "TIMEOUT");
    if (pszTimeout == nullptr)
        pszTimeout = CPLGetConfigOption("GDAL_HTTP_TIMEOUT", nullptr);
    if (pszTimeout != nullptr)
    {
        // coverity[tainted_data]
        unchecked_curl_easy_setopt(
            http_handle, CURLOPT_TIMEOUT_MS,
            static_cast<int>(1000 * CPLAtof(pszTimeout)));
    }

    // Set low speed time and limit.
    const char *pszLowSpeedTime =
        CSLFetchNameValue(papszOptions, "LOW_SPEED_TIME");
    if (pszLowSpeedTime == nullptr)
        pszLowSpeedTime =
            CPLGetConfigOption("GDAL_HTTP_LOW_SPEED_TIME", nullptr);
    if (pszLowSpeedTime != nullptr)
    {
        unchecked_curl_easy_setopt(http_handle, CURLOPT_LOW_SPEED_TIME,
                                   atoi(pszLowSpeedTime));

        const char *pszLowSpeedLimit =
            CSLFetchNameValue(papszOptions, "LOW_SPEED_LIMIT");
        if (pszLowSpeedLimit == nullptr)
            pszLowSpeedLimit =
                CPLGetConfigOption("GDAL_HTTP_LOW_SPEED_LIMIT", "1");
        unchecked_curl_easy_setopt(http_handle, CURLOPT_LOW_SPEED_LIMIT,
                                   atoi(pszLowSpeedLimit));
    }

    /* Disable some SSL verification */
    const char *pszUnsafeSSL = CSLFetchNameValue(papszOptions, "UNSAFESSL");
    if (pszUnsafeSSL == nullptr)
        pszUnsafeSSL = CPLGetConfigOption("GDAL_HTTP_UNSAFESSL", nullptr);
    if (pszUnsafeSSL != nullptr && CPLTestBool(pszUnsafeSSL))
    {
        unchecked_curl_easy_setopt(http_handle, CURLOPT_SSL_VERIFYPEER, 0L);
        unchecked_curl_easy_setopt(http_handle, CURLOPT_SSL_VERIFYHOST, 0L);
    }

    const char *pszUseCAPIStore =
        CSLFetchNameValue(papszOptions, "USE_CAPI_STORE");
    if (pszUseCAPIStore == nullptr)
        pszUseCAPIStore = CPLGetConfigOption("GDAL_HTTP_USE_CAPI_STORE", "NO");
    if (CPLTestBool(pszUseCAPIStore))
    {
#if defined(_WIN32) && defined(HAVE_OPENSSL_CRYPTO)
        // Use certificates from Windows certificate store; requires
        // crypt32.lib, OpenSSL crypto and ssl libraries.
        unchecked_curl_easy_setopt(http_handle, CURLOPT_SSL_CTX_FUNCTION,
                                   *CPL_ssl_ctx_callback);
#else   // defined(_WIN32) && defined(HAVE_OPENSSL_CRYPTO)
        CPLError(CE_Warning, CPLE_NotSupported,
                 "GDAL_HTTP_USE_CAPI_STORE requested, but libcurl too old, "
                 "non-Windows platform or OpenSSL missing.");
#endif  // defined(_WIN32) && defined(HAVE_OPENSSL_CRYPTO)
    }

    // Enable OCSP stapling if requested.
    const char *pszSSLVerifyStatus =
        CSLFetchNameValue(papszOptions, "SSL_VERIFYSTATUS");
    if (pszSSLVerifyStatus == nullptr)
        pszSSLVerifyStatus =
            CPLGetConfigOption("GDAL_HTTP_SSL_VERIFYSTATUS", "NO");
    if (CPLTestBool(pszSSLVerifyStatus))
    {
        unchecked_curl_easy_setopt(http_handle, CURLOPT_SSL_VERIFYSTATUS, 1L);
    }

    // Custom path to SSL certificates.
    const char *pszCAInfo = CSLFetchNameValue(papszOptions, "CAINFO");
    if (pszCAInfo == nullptr)
        // Name of GDAL environment variable for the CA Bundle path
        pszCAInfo = CPLGetConfigOption("GDAL_CURL_CA_BUNDLE", nullptr);
    if (pszCAInfo == nullptr)
        // Name of environment variable used by the curl binary
        pszCAInfo = CPLGetConfigOption("CURL_CA_BUNDLE", nullptr);
    if (pszCAInfo == nullptr)
        // Name of environment variable used by the curl binary (tested
        // after CURL_CA_BUNDLE
        pszCAInfo = CPLGetConfigOption("SSL_CERT_FILE", nullptr);
#ifdef _WIN32
    if (pszCAInfo == nullptr)
    {
        pszCAInfo = CPLFindWin32CurlCaBundleCrt();
    }
#endif
    if (pszCAInfo != nullptr)
    {
        unchecked_curl_easy_setopt(http_handle, CURLOPT_CAINFO, pszCAInfo);
    }

    const char *pszCAPath = CSLFetchNameValue(papszOptions, "CAPATH");
    if (pszCAPath != nullptr)
    {
        unchecked_curl_easy_setopt(http_handle, CURLOPT_CAPATH, pszCAPath);
    }

    // Support for SSL client certificates

    // Filename of the the client certificate
    const char *pszSSLCert = CSLFetchNameValue(papszOptions, "SSLCERT");
    if (!pszSSLCert)
        pszSSLCert = CPLGetConfigOption("GDAL_HTTP_SSLCERT", nullptr);
    if (pszSSLCert)
        unchecked_curl_easy_setopt(http_handle, CURLOPT_SSLCERT, pszSSLCert);

    // private key file for TLS and SSL client cert
    const char *pszSSLKey = CSLFetchNameValue(papszOptions, "SSLKEY");
    if (!pszSSLKey)
        pszSSLKey = CPLGetConfigOption("GDAL_HTTP_SSLKEY", nullptr);
    if (pszSSLKey)
        unchecked_curl_easy_setopt(http_handle, CURLOPT_SSLKEY, pszSSLKey);

    // type of client SSL certificate ("PEM", "DER", ...)
    const char *pszSSLCertType = CSLFetchNameValue(papszOptions, "SSLCERTTYPE");
    if (!pszSSLCertType)
        pszSSLCertType = CPLGetConfigOption("GDAL_HTTP_SSLCERTTYPE", nullptr);
    if (pszSSLCertType)
        unchecked_curl_easy_setopt(http_handle, CURLOPT_SSLCERTTYPE,
                                   pszSSLCertType);

    // passphrase to private key
    const char *pszKeyPasswd = CSLFetchNameValue(papszOptions, "KEYPASSWD");
    if (!pszKeyPasswd)
        pszKeyPasswd = CPLGetConfigOption("GDAL_HTTP_KEYPASSWD", nullptr);
    if (pszKeyPasswd)
        unchecked_curl_easy_setopt(http_handle, CURLOPT_KEYPASSWD,
                                   pszKeyPasswd);

    /* Set Referer */
    const char *pszReferer = CSLFetchNameValue(papszOptions, "REFERER");
    if (pszReferer != nullptr)
        unchecked_curl_easy_setopt(http_handle, CURLOPT_REFERER, pszReferer);

    /* Set User-Agent */
    const char *pszUserAgent = CSLFetchNameValue(papszOptions, "USERAGENT");
    if (pszUserAgent == nullptr)
        pszUserAgent = CPLGetConfigOption("GDAL_HTTP_USERAGENT",
                                          gosDefaultUserAgent.c_str());
    if (pszUserAgent != nullptr && !EQUAL(pszUserAgent, ""))
    {
        unchecked_curl_easy_setopt(http_handle, CURLOPT_USERAGENT,
                                   pszUserAgent);
    }

    /* NOSIGNAL should be set to true for timeout to work in multithread
     * environments on Unix, requires libcurl 7.10 or more recent.
     * (this force avoiding the use of signal handlers)
     */
    unchecked_curl_easy_setopt(http_handle, CURLOPT_NOSIGNAL, 1);

    const char *pszFormFilePath =
        CSLFetchNameValue(papszOptions, "FORM_FILE_PATH");
    const char *pszParametersCount =
        CSLFetchNameValue(papszOptions, "FORM_ITEM_COUNT");
    if (pszFormFilePath == nullptr && pszParametersCount == nullptr)
    {
        /* Set POST mode */
        const char *pszPost = CSLFetchNameValue(papszOptions, "POSTFIELDS");
        if (pszPost != nullptr)
        {
            CPLDebug("HTTP", "These POSTFIELDS were sent:%.4000s", pszPost);
            unchecked_curl_easy_setopt(http_handle, CURLOPT_POST, 1);
            unchecked_curl_easy_setopt(http_handle, CURLOPT_POSTFIELDS,
                                       pszPost);
        }
    }

    const char *pszCustomRequest =
        CSLFetchNameValue(papszOptions, "CUSTOMREQUEST");
    if (pszCustomRequest != nullptr)
    {
        unchecked_curl_easy_setopt(http_handle, CURLOPT_CUSTOMREQUEST,
                                   pszCustomRequest);
    }

    const char *pszCookie = CSLFetchNameValue(papszOptions, "COOKIE");
    if (pszCookie == nullptr)
        pszCookie = CPLGetConfigOption("GDAL_HTTP_COOKIE", nullptr);
    if (pszCookie != nullptr)
        unchecked_curl_easy_setopt(http_handle, CURLOPT_COOKIE, pszCookie);

    const char *pszCookieFile = CSLFetchNameValue(papszOptions, "COOKIEFILE");
    if (pszCookieFile == nullptr)
        pszCookieFile = CPLGetConfigOption("GDAL_HTTP_COOKIEFILE", nullptr);
    if (pszCookieFile != nullptr)
        unchecked_curl_easy_setopt(http_handle, CURLOPT_COOKIEFILE,
                                   pszCookieFile);

    const char *pszCookieJar = CSLFetchNameValue(papszOptions, "COOKIEJAR");
    if (pszCookieJar == nullptr)
        pszCookieJar = CPLGetConfigOption("GDAL_HTTP_COOKIEJAR", nullptr);
    if (pszCookieJar != nullptr)
        unchecked_curl_easy_setopt(http_handle, CURLOPT_COOKIEJAR,
                                   pszCookieJar);

    // TCP keep-alive
    const char *pszTCPKeepAlive =
        CSLFetchNameValue(papszOptions, "TCP_KEEPALIVE");
    if (pszTCPKeepAlive == nullptr)
        pszTCPKeepAlive = CPLGetConfigOption("GDAL_HTTP_TCP_KEEPALIVE", "YES");
    if (pszTCPKeepAlive != nullptr && CPLTestBool(pszTCPKeepAlive))
    {
        // Set keep-alive interval.
        int nKeepAliveInterval = 60;
        const char *pszKeepAliveInterval =
            CSLFetchNameValue(papszOptions, "TCP_KEEPINTVL");
        if (pszKeepAliveInterval == nullptr)
            pszKeepAliveInterval =
                CPLGetConfigOption("GDAL_HTTP_TCP_KEEPINTVL", nullptr);
        if (pszKeepAliveInterval != nullptr)
            nKeepAliveInterval = atoi(pszKeepAliveInterval);

        // Set keep-alive idle wait time.
        int nKeepAliveIdle = 60;
        const char *pszKeepAliveIdle =
            CSLFetchNameValue(papszOptions, "TCP_KEEPIDLE");
        if (pszKeepAliveIdle == nullptr)
            pszKeepAliveIdle =
                CPLGetConfigOption("GDAL_HTTP_TCP_KEEPIDLE", nullptr);
        if (pszKeepAliveIdle != nullptr)
            nKeepAliveIdle = atoi(pszKeepAliveIdle);

        unchecked_curl_easy_setopt(http_handle, CURLOPT_TCP_KEEPALIVE, 1L);
        unchecked_curl_easy_setopt(http_handle, CURLOPT_TCP_KEEPINTVL,
                                   nKeepAliveInterval);
        unchecked_curl_easy_setopt(http_handle, CURLOPT_TCP_KEEPIDLE,
                                   nKeepAliveIdle);
    }

    struct curl_slist *headers = nullptr;
    const char *pszAccept = CSLFetchNameValue(papszOptions, "ACCEPT");
    if (pszAccept)
    {
        headers =
            curl_slist_append(headers, CPLSPrintf("Accept: %s", pszAccept));
    }

    const auto AddHeader = [&headers, pszAccept](const char *pszHeader)
    {
        if (STARTS_WITH_CI(pszHeader, "Accept:") && pszAccept)
        {
            const char *pszVal = pszHeader + strlen("Accept:");
            while (*pszVal == ' ')
                ++pszVal;
            if (!EQUAL(pszVal, pszAccept))
            {
                // Cf https://github.com/OSGeo/gdal/issues/7691#issuecomment-2873711603
                CPLDebug(
                    "HTTP",
                    "Ignoring '%s' since ACCEPT option = '%s' is specified",
                    pszHeader, pszAccept);
            }
        }
        else
        {
            headers = curl_slist_append(headers, pszHeader);
        }
    };

    const char *pszHeaderFile = CSLFetchNameValue(papszOptions, "HEADER_FILE");
    if (pszHeaderFile == nullptr)
        pszHeaderFile = CPLGetConfigOption("GDAL_HTTP_HEADER_FILE", nullptr);
    if (pszHeaderFile != nullptr)
    {
        VSILFILE *fp = nullptr;
        // Do not allow /vsicurl/ access from /vsicurl because of
        // GetCurlHandleFor() e.g. "/vsicurl/,HEADER_FILE=/vsicurl/,url= " would
        // cause use of memory after free
        if (!STARTS_WITH(pszHeaderFile, "/vsi") ||
            STARTS_WITH(pszHeaderFile, "/vsimem/"))
        {
            fp = VSIFOpenL(pszHeaderFile, "rb");
        }
        if (fp == nullptr)
        {
            CPLError(CE_Failure, CPLE_FileIO, "Cannot read %s", pszHeaderFile);
        }
        else
        {
            const char *pszLine = nullptr;
            while ((pszLine = CPLReadLineL(fp)) != nullptr)
            {
                AddHeader(pszLine);
            }
            VSIFCloseL(fp);
        }
    }

    const char *pszHeaders = CSLFetchNameValue(papszOptions, "HEADERS");
    if (pszHeaders == nullptr)
        pszHeaders = CPLGetConfigOption("GDAL_HTTP_HEADERS", nullptr);
    if (pszHeaders)
    {
        bool bHeadersDone = false;
        // Compatibility hack for "HEADERS=Accept: text/plain, application/json"
        if (strstr(pszHeaders, "\r\n") == nullptr)
        {
            const char *pszComma = strchr(pszHeaders, ',');
            if (pszComma != nullptr && strchr(pszComma, ':') == nullptr)
            {
                AddHeader(pszHeaders);
                bHeadersDone = true;
            }
        }
        if (!bHeadersDone)
        {
            const char *pszAuthorizationHeaderAllowed = CSLFetchNameValueDef(
                papszOptions, "AUTHORIZATION_HEADER_ALLOWED", "YES");
            const bool bAuthorizationHeaderAllowed =
                CPLTestBool(pszAuthorizationHeaderAllowed);

            // We accept both raw headers with \r\n as a separator, or as
            // a comma separated list of foo: bar values.
            const CPLStringList aosTokens(
                strstr(pszHeaders, "\r\n")
                    ? CSLTokenizeString2(pszHeaders, "\r\n", 0)
                    : CSLTokenizeString2(pszHeaders, ",", CSLT_HONOURSTRINGS));
            for (int i = 0; i < aosTokens.size(); ++i)
            {
                if (bAuthorizationHeaderAllowed ||
                    !STARTS_WITH_CI(aosTokens[i], "Authorization:"))
                {
                    AddHeader(aosTokens[i]);
                }
            }
        }
    }

    return headers;
}

/************************************************************************/
/*                         CPLHTTPIgnoreSigPipe()                       */
/************************************************************************/

/* If using OpenSSL with Curl, openssl can cause SIGPIPE to be triggered */
/* As we set CURLOPT_NOSIGNAL = 1, we must manually handle this situation */

void *CPLHTTPIgnoreSigPipe()
{
#if defined(SIGPIPE) && defined(HAVE_SIGACTION)
    struct sigaction old_pipe_act;
    struct sigaction action;
    /* Get previous handler */
    memset(&old_pipe_act, 0, sizeof(struct sigaction));
    sigaction(SIGPIPE, nullptr, &old_pipe_act);

    /* Install new handler */
    action = old_pipe_act;
    action.sa_handler = SIG_IGN;
    sigaction(SIGPIPE, &action, nullptr);

    void *ret = CPLMalloc(sizeof(old_pipe_act));
    memcpy(ret, &old_pipe_act, sizeof(old_pipe_act));
    return ret;
#else
    return nullptr;
#endif
}

/************************************************************************/
/*                     CPLHTTPRestoreSigPipeHandler()                   */
/************************************************************************/

void CPLHTTPRestoreSigPipeHandler(void *old_handler)
{
#if defined(SIGPIPE) && defined(HAVE_SIGACTION)
    sigaction(SIGPIPE, static_cast<struct sigaction *>(old_handler), nullptr);
    CPLFree(old_handler);
#else
    (void)old_handler;
#endif
}

#endif  // def HAVE_CURL

/************************************************************************/
/*                           CPLHTTPEnabled()                           */
/************************************************************************/

/**
 * \brief Return if CPLHTTP services can be useful
 *
 * Those services depend on GDAL being build with libcurl support.
 *
 * @return TRUE if libcurl support is enabled
 */
int CPLHTTPEnabled()

{
#ifdef HAVE_CURL
    return TRUE;
#else
    return FALSE;
#endif
}

/************************************************************************/
/*                           CPLHTTPCleanup()                           */
/************************************************************************/

/**
 * \brief Cleanup function to call at application termination
 */
void CPLHTTPCleanup()

{
#ifdef HAVE_CURL
    if (!hSessionMapMutex)
        return;

    {
        CPLMutexHolder oHolder(&hSessionMapMutex);
        if (poSessionMap)
        {
            for (auto &kv : *poSessionMap)
            {
                curl_easy_cleanup(kv.second);
            }
            delete poSessionMap;
            poSessionMap = nullptr;
        }
        if (poSessionMultiMap)
        {
            for (auto &kv : *poSessionMultiMap)
            {
                VSICURLMultiCleanup(kv.second);
            }
            delete poSessionMultiMap;
            poSessionMultiMap = nullptr;
        }
    }

    // Not quite a safe sequence.
    CPLDestroyMutex(hSessionMapMutex);
    hSessionMapMutex = nullptr;

#if defined(_WIN32) && defined(HAVE_OPENSSL_CRYPTO)
    // This cleanup must be absolutely done before CPLOpenSSLCleanup()
    // for some unknown reason, but otherwise X509_free() in
    // CPLWindowsCertificateListCleanup() will crash.
    CPLWindowsCertificateListCleanup();
#endif

#if defined(HAVE_OPENSSL_CRYPTO) && OPENSSL_VERSION_NUMBER < 0x10100000
    CPLOpenSSLCleanup();
#endif

#endif
}

/************************************************************************/
/*                        CPLHTTPDestroyResult()                        */
/************************************************************************/

/**
 * \brief Clean the memory associated with the return value of CPLHTTPFetch()
 *
 * @param psResult pointer to the return value of CPLHTTPFetch()
 */
void CPLHTTPDestroyResult(CPLHTTPResult *psResult)

{
    if (psResult)
    {
        CPLFree(psResult->pabyData);
        CPLFree(psResult->pszErrBuf);
        CPLFree(psResult->pszContentType);
        CSLDestroy(psResult->papszHeaders);

        for (int i = 0; i < psResult->nMimePartCount; i++)
        {
            CSLDestroy(psResult->pasMimePart[i].papszHeaders);
        }
        CPLFree(psResult->pasMimePart);

        CPLFree(psResult);
    }
}

/************************************************************************/
/*                     CPLHTTPParseMultipartMime()                      */
/************************************************************************/

/**
 * \brief Parses a MIME multipart message.
 *
 * This function will iterate over each part and put it in a separate
 * element of the pasMimePart array of the provided psResult structure.
 *
 * @param psResult pointer to the return value of CPLHTTPFetch()
 * @return TRUE if the message contains MIME multipart message.
 */
int CPLHTTPParseMultipartMime(CPLHTTPResult *psResult)

{
    /* -------------------------------------------------------------------- */
    /*      Is it already done?                                             */
    /* -------------------------------------------------------------------- */
    if (psResult->nMimePartCount > 0)
        return TRUE;

    /* -------------------------------------------------------------------- */
    /*      Find the boundary setting in the content type.                  */
    /* -------------------------------------------------------------------- */
    const char *pszBound = nullptr;

    if (psResult->pszContentType != nullptr)
        pszBound = strstr(psResult->pszContentType, "boundary=");

    if (pszBound == nullptr)
    {
        CPLError(CE_Failure, CPLE_AppDefined,
                 "Unable to parse multi-part mime, no boundary setting.");
        return FALSE;
    }

    CPLString osBoundary;
    char **papszTokens =
        CSLTokenizeStringComplex(pszBound + 9, "\n ;", TRUE, FALSE);

    if (CSLCount(papszTokens) == 0 || strlen(papszTokens[0]) == 0)
    {
        CPLError(CE_Failure, CPLE_AppDefined,
                 "Unable to parse multi-part mime, boundary not parsable.");
        CSLDestroy(papszTokens);
        return FALSE;
    }

    osBoundary = "--";
    osBoundary += papszTokens[0];
    CSLDestroy(papszTokens);

    /* -------------------------------------------------------------------- */
    /*      Find the start of the first chunk.                              */
    /* -------------------------------------------------------------------- */
    char *pszNext = psResult->pabyData
                        ? strstr(reinterpret_cast<char *>(psResult->pabyData),
                                 osBoundary.c_str())
                        : nullptr;

    if (pszNext == nullptr)
    {
        CPLError(CE_Failure, CPLE_AppDefined, "No parts found.");
        return FALSE;
    }

    pszNext += osBoundary.size();
    while (*pszNext != '\n' && *pszNext != '\r' && *pszNext != '\0')
        pszNext++;
    if (*pszNext == '\r')
        pszNext++;
    if (*pszNext == '\n')
        pszNext++;

    /* -------------------------------------------------------------------- */
    /*      Loop over parts...                                              */
    /* -------------------------------------------------------------------- */
    while (true)
    {
        psResult->nMimePartCount++;
        psResult->pasMimePart = static_cast<CPLMimePart *>(
            CPLRealloc(psResult->pasMimePart,
                       sizeof(CPLMimePart) * psResult->nMimePartCount));

        CPLMimePart *psPart =
            psResult->pasMimePart + psResult->nMimePartCount - 1;

        memset(psPart, 0, sizeof(CPLMimePart));

        /* --------------------------------------------------------------------
         */
        /*      Collect headers. */
        /* --------------------------------------------------------------------
         */
        while (*pszNext != '\n' && *pszNext != '\r' && *pszNext != '\0')
        {
            if (!STARTS_WITH(pszNext, "Content-"))
            {
                break;
            }
            char *pszEOL = strstr(pszNext, "\n");

            if (pszEOL == nullptr)
            {
                CPLError(CE_Failure, CPLE_AppDefined,
                         "Error while parsing multipart content (at line %d)",
                         __LINE__);
                return FALSE;
            }

            *pszEOL = '\0';
            bool bRestoreAntislashR = false;
            if (pszEOL - pszNext > 1 && pszEOL[-1] == '\r')
            {
                bRestoreAntislashR = true;
                pszEOL[-1] = '\0';
            }
            char *pszKey = nullptr;
            const char *pszValue = CPLParseNameValue(pszNext, &pszKey);
            if (pszKey && pszValue)
            {
                psPart->papszHeaders =
                    CSLSetNameValue(psPart->papszHeaders, pszKey, pszValue);
            }
            CPLFree(pszKey);
            if (bRestoreAntislashR)
                pszEOL[-1] = '\r';
            *pszEOL = '\n';

            pszNext = pszEOL + 1;
        }

        if (*pszNext == '\r')
            pszNext++;
        if (*pszNext == '\n')
            pszNext++;

        /* --------------------------------------------------------------------
         */
        /*      Work out the data block size. */
        /* --------------------------------------------------------------------
         */
        psPart->pabyData = reinterpret_cast<GByte *>(pszNext);

        int nBytesAvail = psResult->nDataLen -
                          static_cast<int>(pszNext - reinterpret_cast<char *>(
                                                         psResult->pabyData));

        while (nBytesAvail > 0 &&
               (*pszNext != '-' ||
                strncmp(pszNext, osBoundary, osBoundary.size()) != 0))
        {
            pszNext++;
            nBytesAvail--;
        }

        if (nBytesAvail == 0)
        {
            CPLError(CE_Failure, CPLE_AppDefined,
                     "Error while parsing multipart content (at line %d)",
                     __LINE__);
            return FALSE;
        }

        psPart->nDataLen = static_cast<int>(
            pszNext - reinterpret_cast<char *>(psPart->pabyData));
        // Normally the part should end with "\r\n--boundary_marker"
        if (psPart->nDataLen >= 2 && pszNext[-2] == '\r' && pszNext[-1] == '\n')
        {
            psPart->nDataLen -= 2;
        }

        pszNext += osBoundary.size();

        if (STARTS_WITH(pszNext, "--"))
        {
            break;
        }

        if (*pszNext == '\r')
            pszNext++;
        if (*pszNext == '\n')
            pszNext++;
        else
        {
            CPLError(CE_Failure, CPLE_AppDefined,
                     "Error while parsing multipart content (at line %d)",
                     __LINE__);
            return FALSE;
        }
    }

    return TRUE;
}

#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif
