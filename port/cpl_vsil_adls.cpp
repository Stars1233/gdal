/******************************************************************************
 *
 * Project:  CPL - Common Portability Library
 * Purpose:  Implement VSI large file api for Microsoft Azure Data Lake Storage
 *Gen2 Author:   Even Rouault, even.rouault at spatialys.com
 *
 ******************************************************************************
 * Copyright (c) 2020, Even Rouault <even.rouault at spatialys.com>
 *
 * SPDX-License-Identifier: MIT
 ****************************************************************************/

#include "cpl_port.h"
#include "cpl_http.h"
#include "cpl_json.h"
#include "cpl_time.h"
#include "cpl_vsil_curl_priv.h"
#include "cpl_vsil_curl_class.h"

#include <errno.h>

#include <algorithm>
#include <set>
#include <map>
#include <memory>

#include "cpl_azure.h"

#ifndef HAVE_CURL

void VSIInstallADLSFileHandler(void)
{
    // Not supported
}

#else

//! @cond Doxygen_Suppress
#ifndef DOXYGEN_SKIP

#define ENABLE_DEBUG 0

#define unchecked_curl_easy_setopt(handle, opt, param)                         \
    CPL_IGNORE_RET_VAL(curl_easy_setopt(handle, opt, param))

namespace cpl
{

/************************************************************************/
/*                         GetContinuationToken()                       */
/************************************************************************/

static std::string GetContinuationToken(const char *pszHeaders)
{
    std::string osContinuation;
    if (pszHeaders)
    {
        const char *pszContinuation = strstr(pszHeaders, "x-ms-continuation: ");
        if (pszContinuation)
        {
            pszContinuation += strlen("x-ms-continuation: ");
            const char *pszEOL = strstr(pszContinuation, "\r\n");
            if (pszEOL)
            {
                osContinuation.assign(pszContinuation,
                                      pszEOL - pszContinuation);
            }
        }
    }
    return osContinuation;
}

/************************************************************************/
/*                        RemoveTrailingSlash()                        */
/************************************************************************/

static std::string RemoveTrailingSlash(const std::string &osFilename)
{
    std::string osWithoutSlash(osFilename);
    if (!osWithoutSlash.empty() && osWithoutSlash.back() == '/')
        osWithoutSlash.pop_back();
    return osWithoutSlash;
}

/************************************************************************/
/*                             VSIDIRADLS                               */
/************************************************************************/

class VSIADLSFSHandler;

struct VSIDIRADLS : public VSIDIR
{
    int m_nRecurseDepth = 0;

    struct Iterator
    {
        std::string m_osNextMarker{};
        std::vector<std::unique_ptr<VSIDIREntry>> m_aoEntries{};
        int m_nPos = 0;

        void clear()
        {
            m_osNextMarker.clear();
            m_nPos = 0;
            m_aoEntries.clear();
        }
    };

    Iterator m_oIterWithinFilesystem{};
    Iterator m_oIterFromRoot{};

    // Backup file system listing when doing a recursive OpenDir() from
    // the account root
    bool m_bRecursiveRequestFromAccountRoot = false;

    std::string m_osFilesystem{};
    std::string m_osObjectKey{};
    VSIADLSFSHandler *m_poFS = nullptr;
    int m_nMaxFiles = 0;
    bool m_bCacheEntries = true;
    std::string
        m_osFilterPrefix{};  // client-side only. No server-side option in

    // https://docs.microsoft.com/en-us/rest/api/storageservices/datalakestoragegen2/path/list

    explicit VSIDIRADLS(VSIADLSFSHandler *poFSIn) : m_poFS(poFSIn)
    {
    }

    VSIDIRADLS(const VSIDIRADLS &) = delete;
    VSIDIRADLS &operator=(const VSIDIRADLS &) = delete;

    const VSIDIREntry *NextDirEntry() override;

    bool IssueListDir();
    bool AnalysePathList(const std::string &osBaseURL, const char *pszJSON);
    bool AnalyseFilesystemList(const std::string &osBaseURL,
                               const char *pszJSON);
    void clear();
};

/************************************************************************/
/*                       VSIADLSFSHandler                              */
/************************************************************************/

class VSIADLSFSHandler final : public IVSIS3LikeFSHandlerWithMultipartUpload
{
    CPL_DISALLOW_COPY_ASSIGN(VSIADLSFSHandler)

  protected:
    VSICurlHandle *CreateFileHandle(const char *pszFilename) override;
    std::string
    GetURLFromFilename(const std::string &osFilename) const override;

    char **GetFileList(const char *pszFilename, int nMaxFiles,
                       bool *pbGotFileList) override;

    int CopyObject(const char *oldpath, const char *newpath,
                   CSLConstList papszMetadata) override;
    int MkdirInternal(const char *pszDirname, long nMode,
                      bool bDoStatCheck) override;
    int RmdirInternal(const char *pszDirname, bool bRecursive);

    void ClearCache() override;

    bool IsAllowedHeaderForObjectCreation(const char *pszHeaderName) override
    {
        return STARTS_WITH(pszHeaderName, "x-ms-");
    }

    VSIVirtualHandleUniquePtr
    CreateWriteHandle(const char *pszFilename,
                      CSLConstList papszOptions) override;

  public:
    VSIADLSFSHandler() = default;
    ~VSIADLSFSHandler() override = default;

    std::string GetFSPrefix() const override
    {
        return "/vsiadls/";
    }

    const char *GetDebugKey() const override
    {
        return "ADLS";
    }

    int Rename(const char *oldpath, const char *newpath, GDALProgressFunc,
               void *) override;
    int Unlink(const char *pszFilename) override;
    int Mkdir(const char *, long) override;
    int Rmdir(const char *) override;
    int RmdirRecursive(const char *pszDirname) override;
    int Stat(const char *pszFilename, VSIStatBufL *pStatBuf,
             int nFlags) override;

    char **GetFileMetadata(const char *pszFilename, const char *pszDomain,
                           CSLConstList papszOptions) override;

    bool SetFileMetadata(const char *pszFilename, CSLConstList papszMetadata,
                         const char *pszDomain,
                         CSLConstList papszOptions) override;

    const char *GetOptions() override;

    char *GetSignedURL(const char *pszFilename,
                       CSLConstList papszOptions) override;

    char **GetFileList(const char *pszFilename, int nMaxFiles,
                       bool bCacheEntries, bool *pbGotFileList);

    VSIDIR *OpenDir(const char *pszPath, int nRecurseDepth,
                    const char *const *papszOptions) override;

    enum class Event
    {
        CREATE_FILE,
        APPEND_DATA,
        FLUSH
    };

    // Block list upload
    bool UploadFile(const std::string &osFilename, Event event,
                    vsi_l_offset nPosition, const void *pabyBuffer,
                    size_t nBufferSize,
                    IVSIS3LikeHandleHelper *poS3HandleHelper,
                    const CPLHTTPRetryParameters &oRetryParameters,
                    CSLConstList papszOptions);

    // Multipart upload (mapping of S3 interface)

    std::string
    InitiateMultipartUpload(const std::string &osFilename,
                            IVSIS3LikeHandleHelper *poS3HandleHelper,
                            const CPLHTTPRetryParameters &oRetryParameters,
                            CSLConstList papszOptions) override
    {
        return UploadFile(osFilename, Event::CREATE_FILE, 0, nullptr, 0,
                          poS3HandleHelper, oRetryParameters, papszOptions)
                   ? std::string("dummy")
                   : std::string();
    }

    std::string UploadPart(const std::string &osFilename, int /* nPartNumber */,
                           const std::string & /* osUploadID */,
                           vsi_l_offset nPosition, const void *pabyBuffer,
                           size_t nBufferSize,
                           IVSIS3LikeHandleHelper *poS3HandleHelper,
                           const CPLHTTPRetryParameters &oRetryParameters,
                           CSLConstList /* papszOptions */) override
    {
        return UploadFile(osFilename, Event::APPEND_DATA, nPosition, pabyBuffer,
                          nBufferSize, poS3HandleHelper, oRetryParameters,
                          nullptr)
                   ? std::string("dummy")
                   : std::string();
    }

    bool CompleteMultipart(
        const std::string &osFilename, const std::string & /* osUploadID */,
        const std::vector<std::string> & /* aosEtags */,
        vsi_l_offset nTotalSize, IVSIS3LikeHandleHelper *poS3HandleHelper,
        const CPLHTTPRetryParameters &oRetryParameters) override
    {
        return UploadFile(osFilename, Event::FLUSH, nTotalSize, nullptr, 0,
                          poS3HandleHelper, oRetryParameters, nullptr);
    }

    bool
    AbortMultipart(const std::string & /* osFilename */,
                   const std::string & /* osUploadID */,
                   IVSIS3LikeHandleHelper * /*poS3HandleHelper */,
                   const CPLHTTPRetryParameters & /*oRetryParameters*/) override
    {
        return true;
    }

    bool MultipartUploadAbort(const char *, const char *, CSLConstList) override
    {
        CPLError(CE_Failure, CPLE_NotSupported,
                 "MultipartUploadAbort() not supported by this file system");
        return false;
    }

    bool SupportsMultipartAbort() const override
    {
        return false;
    }

    //! Maximum number of parts for multipart upload
    // No limit imposed by the API. Arbitrary one here
    int GetMaximumPartCount() override
    {
        return INT_MAX;
    }

    //! Minimum size of a part for multipart upload (except last one), in MiB.
    int GetMinimumPartSizeInMiB() override
    {
        return 0;
    }

    //! Maximum size of a part for multipart upload, in MiB.
    // No limit imposed by the API. Arbitrary one here
    int GetMaximumPartSizeInMiB() override
    {
#if SIZEOF_VOIDP == 8
        return 4000;
#else
        // Cannot be larger than 4GiB, otherwise integer overflow would occur
        // 1 GiB is the maximum reasonable value on a 32-bit machine
        return 1024;
#endif
    }

    IVSIS3LikeHandleHelper *CreateHandleHelper(const char *pszURI,
                                               bool bAllowNoObject) override;

    std::string
    GetStreamingFilename(const std::string &osFilename) const override;
};

/************************************************************************/
/*                          VSIADLSWriteHandle                         */
/************************************************************************/

class VSIADLSWriteHandle final : public VSIAppendWriteHandle
{
    CPL_DISALLOW_COPY_ASSIGN(VSIADLSWriteHandle)

    std::unique_ptr<VSIAzureBlobHandleHelper> m_poHandleHelper{};
    bool m_bCreated = false;

    bool Send(bool bIsLastBlock) override;

    bool SendInternal(VSIADLSFSHandler::Event event, CSLConstList papszOptions);

    void InvalidateParentDirectory();

  public:
    VSIADLSWriteHandle(VSIADLSFSHandler *poFS, const char *pszFilename,
                       VSIAzureBlobHandleHelper *poHandleHelper);
    virtual ~VSIADLSWriteHandle();

    bool CreateFile(CSLConstList papszOptions);
};

/************************************************************************/
/*                                clear()                               */
/************************************************************************/

void VSIDIRADLS::clear()
{
    if (!m_osFilesystem.empty())
        m_oIterWithinFilesystem.clear();
    else
        m_oIterFromRoot.clear();
}

/************************************************************************/
/*                        GetUnixTimeFromRFC822()                       */
/************************************************************************/

static GIntBig GetUnixTimeFromRFC822(const char *pszRFC822DateTime)
{
    int nYear, nMonth, nDay, nHour, nMinute, nSecond;
    if (CPLParseRFC822DateTime(pszRFC822DateTime, &nYear, &nMonth, &nDay,
                               &nHour, &nMinute, &nSecond, nullptr, nullptr))
    {
        struct tm brokendowntime;
        brokendowntime.tm_year = nYear - 1900;
        brokendowntime.tm_mon = nMonth - 1;
        brokendowntime.tm_mday = nDay;
        brokendowntime.tm_hour = nHour;
        brokendowntime.tm_min = nMinute;
        brokendowntime.tm_sec = nSecond < 0 ? 0 : nSecond;
        return CPLYMDHMSToUnixTime(&brokendowntime);
    }
    return GINTBIG_MIN;
}

/************************************************************************/
/*                           AnalysePathList()                          */
/************************************************************************/

bool VSIDIRADLS::AnalysePathList(const std::string &osBaseURL,
                                 const char *pszJSON)
{
#if DEBUG_VERBOSE
    CPLDebug(m_poFS->GetDebugKey(), "%s", pszJSON);
#endif

    CPLJSONDocument oDoc;
    if (!oDoc.LoadMemory(pszJSON))
        return false;

    auto oPaths = oDoc.GetRoot().GetArray("paths");
    if (!oPaths.IsValid())
    {
        CPLError(CE_Failure, CPLE_AppDefined, "Cannot find paths[]");
        return false;
    }

    for (const auto &oPath : oPaths)
    {
        m_oIterWithinFilesystem.m_aoEntries.push_back(
            std::unique_ptr<VSIDIREntry>(new VSIDIREntry()));
        auto &entry = m_oIterWithinFilesystem.m_aoEntries.back();

        // Returns relative path to the filesystem, so for example
        // "mydir/foo.bin" for
        // https://{account}.dfs.core.windows.net/{filesystem}/mydir/foo.bin
        const std::string osName(oPath.GetString("name"));
        if (!m_osObjectKey.empty() &&
            STARTS_WITH(osName.c_str(), (m_osObjectKey + "/").c_str()))
            entry->pszName =
                CPLStrdup(osName.substr(m_osObjectKey.size() + 1).c_str());
        else if (m_bRecursiveRequestFromAccountRoot && !m_osFilesystem.empty())
            entry->pszName = CPLStrdup((m_osFilesystem + '/' + osName).c_str());
        else
            entry->pszName = CPLStrdup(osName.c_str());
        entry->nSize = static_cast<GUIntBig>(oPath.GetLong("contentLength"));
        entry->bSizeKnown = true;
        entry->nMode =
            oPath.GetString("isDirectory") == "true" ? S_IFDIR : S_IFREG;
        entry->nMode |=
            VSICurlParseUnixPermissions(oPath.GetString("permissions").c_str());
        entry->bModeKnown = true;

        std::string ETag = oPath.GetString("etag");
        if (!ETag.empty())
        {
            entry->papszExtra =
                CSLSetNameValue(entry->papszExtra, "ETag", ETag.c_str());
        }

        const GIntBig nMTime =
            GetUnixTimeFromRFC822(oPath.GetString("lastModified").c_str());
        if (nMTime != GINTBIG_MIN)
        {
            entry->nMTime = nMTime;
            entry->bMTimeKnown = true;
        }

        if (m_bCacheEntries)
        {
            FileProp prop;
            prop.eExists = EXIST_YES;
            prop.bHasComputedFileSize = true;
            prop.fileSize = entry->nSize;
            prop.bIsDirectory = CPL_TO_BOOL(VSI_ISDIR(entry->nMode));
            prop.nMode = entry->nMode;
            prop.mTime = static_cast<time_t>(entry->nMTime);
            prop.ETag = std::move(ETag);

            std::string osCachedFilename =
                osBaseURL + "/" + CPLAWSURLEncode(osName, false);
#if DEBUG_VERBOSE
            CPLDebug(m_poFS->GetDebugKey(), "Cache %s",
                     osCachedFilename.c_str());
#endif
            m_poFS->SetCachedFileProp(osCachedFilename.c_str(), prop);
        }

        if (m_nMaxFiles > 0 && m_oIterWithinFilesystem.m_aoEntries.size() >
                                   static_cast<unsigned>(m_nMaxFiles))
        {
            break;
        }
    }

    return true;
}

/************************************************************************/
/*                         AnalysePathList()                            */
/************************************************************************/

bool VSIDIRADLS::AnalyseFilesystemList(const std::string &osBaseURL,
                                       const char *pszJSON)
{
#if DEBUG_VERBOSE
    CPLDebug(m_poFS->GetDebugKey(), "%s", pszJSON);
#endif

    CPLJSONDocument oDoc;
    if (!oDoc.LoadMemory(pszJSON))
        return false;

    auto oPaths = oDoc.GetRoot().GetArray("filesystems");
    if (!oPaths.IsValid())
    {
        CPLError(CE_Failure, CPLE_AppDefined, "Cannot find filesystems[]");
        return false;
    }

    for (const auto &oPath : oPaths)
    {
        m_oIterFromRoot.m_aoEntries.push_back(
            std::unique_ptr<VSIDIREntry>(new VSIDIREntry()));
        auto &entry = m_oIterFromRoot.m_aoEntries.back();

        const std::string osName(oPath.GetString("name"));
        entry->pszName = CPLStrdup(osName.c_str());
        entry->nSize = 0;
        entry->bSizeKnown = true;
        entry->nMode = S_IFDIR;
        entry->bModeKnown = true;

        std::string ETag = oPath.GetString("etag");
        if (!ETag.empty())
        {
            entry->papszExtra =
                CSLSetNameValue(entry->papszExtra, "ETag", ETag.c_str());
        }

        const GIntBig nMTime =
            GetUnixTimeFromRFC822(oPath.GetString("lastModified").c_str());
        if (nMTime != GINTBIG_MIN)
        {
            entry->nMTime = nMTime;
            entry->bMTimeKnown = true;
        }

        if (m_bCacheEntries)
        {
            FileProp prop;
            prop.eExists = EXIST_YES;
            prop.bHasComputedFileSize = true;
            prop.fileSize = 0;
            prop.bIsDirectory = true;
            prop.mTime = static_cast<time_t>(entry->nMTime);
            prop.ETag = std::move(ETag);

            std::string osCachedFilename =
                osBaseURL + CPLAWSURLEncode(osName, false);
#if DEBUG_VERBOSE
            CPLDebug(m_poFS->GetDebugKey(), "Cache %s",
                     osCachedFilename.c_str());
#endif
            m_poFS->SetCachedFileProp(osCachedFilename.c_str(), prop);
        }

        if (m_nMaxFiles > 0 && m_oIterFromRoot.m_aoEntries.size() >
                                   static_cast<unsigned>(m_nMaxFiles))
        {
            break;
        }
    }

    return true;
}

/************************************************************************/
/*                          IssueListDir()                              */
/************************************************************************/

bool VSIDIRADLS::IssueListDir()
{
    WriteFuncStruct sWriteFuncData;

    auto &oIter =
        !m_osFilesystem.empty() ? m_oIterWithinFilesystem : m_oIterFromRoot;
    const std::string l_osNextMarker(oIter.m_osNextMarker);
    clear();

    NetworkStatisticsFileSystem oContextFS(m_poFS->GetFSPrefix().c_str());
    NetworkStatisticsAction oContextAction("ListBucket");

    CPLString osMaxKeys = CPLGetConfigOption("AZURE_MAX_RESULTS", "");
    const int AZURE_SERVER_LIMIT_SINGLE_REQUEST = 5000;
    if (m_nMaxFiles > 0 && m_nMaxFiles < AZURE_SERVER_LIMIT_SINGLE_REQUEST &&
        (osMaxKeys.empty() || m_nMaxFiles < atoi(osMaxKeys)))
    {
        osMaxKeys.Printf("%d", m_nMaxFiles);
    }

    auto poHandleHelper = std::unique_ptr<IVSIS3LikeHandleHelper>(
        m_poFS->CreateHandleHelper(m_osFilesystem.c_str(), true));
    if (poHandleHelper == nullptr)
    {
        return false;
    }

    const std::string osBaseURL(poHandleHelper->GetURLNoKVP());

    CURL *hCurlHandle = curl_easy_init();

    if (!l_osNextMarker.empty())
        poHandleHelper->AddQueryParameter("continuation", l_osNextMarker);
    if (!osMaxKeys.empty())
        poHandleHelper->AddQueryParameter("maxresults", osMaxKeys);
    if (!m_osFilesystem.empty())
    {
        poHandleHelper->AddQueryParameter("resource", "filesystem");
        poHandleHelper->AddQueryParameter(
            "recursive", m_nRecurseDepth == 0 ? "false" : "true");
        if (!m_osObjectKey.empty())
            poHandleHelper->AddQueryParameter("directory", m_osObjectKey);
    }
    else
    {
        poHandleHelper->AddQueryParameter("resource", "account");
    }

    std::string osFilename("/vsiadls/");
    if (!m_osFilesystem.empty())
    {
        osFilename += m_osFilesystem;
        if (!m_osObjectKey.empty())
            osFilename += m_osObjectKey;
    }
    const CPLStringList aosHTTPOptions(
        CPLHTTPGetOptionsFromEnv(osFilename.c_str()));

    struct curl_slist *headers = VSICurlSetOptions(
        hCurlHandle, poHandleHelper->GetURL().c_str(), aosHTTPOptions.List());
    headers = poHandleHelper->GetCurlHeaders("GET", headers);
    unchecked_curl_easy_setopt(hCurlHandle, CURLOPT_HTTPHEADER, headers);

    CurlRequestHelper requestHelper;
    const long response_code = requestHelper.perform(
        hCurlHandle, headers, m_poFS, poHandleHelper.get());

    NetworkStatisticsLogger::LogGET(sWriteFuncData.nSize);

    bool ret = false;
    if (response_code != 200)
    {
        CPLDebug(m_poFS->GetDebugKey(), "%s",
                 requestHelper.sWriteFuncData.pBuffer
                     ? requestHelper.sWriteFuncData.pBuffer
                     : "(null)");
    }
    else
    {
        if (!m_osFilesystem.empty())
        {
            // https://docs.microsoft.com/en-us/rest/api/storageservices/datalakestoragegen2/path/list
            ret = AnalysePathList(osBaseURL,
                                  requestHelper.sWriteFuncData.pBuffer);
        }
        else
        {
            // https://docs.microsoft.com/en-us/rest/api/storageservices/datalakestoragegen2/filesystem/list
            ret = AnalyseFilesystemList(osBaseURL,
                                        requestHelper.sWriteFuncData.pBuffer);
        }

        // Get continuation token for response headers
        oIter.m_osNextMarker =
            GetContinuationToken(requestHelper.sWriteFuncHeaderData.pBuffer);
    }

    curl_easy_cleanup(hCurlHandle);
    return ret;
}

/************************************************************************/
/*                           NextDirEntry()                             */
/************************************************************************/

const VSIDIREntry *VSIDIRADLS::NextDirEntry()
{
    while (true)
    {
        auto &oIter =
            !m_osFilesystem.empty() ? m_oIterWithinFilesystem : m_oIterFromRoot;
        if (oIter.m_nPos < static_cast<int>(oIter.m_aoEntries.size()))
        {
            auto &entry = oIter.m_aoEntries[oIter.m_nPos];
            oIter.m_nPos++;
            if (m_bRecursiveRequestFromAccountRoot)
            {
                // If we just read an entry from the account root, it is a
                // filesystem name, and we want the next iteration to read
                // into it.
                if (m_osFilesystem.empty())
                {
                    m_osFilesystem = entry->pszName;
                    if (!IssueListDir())
                    {
                        return nullptr;
                    }
                }
            }
            if (!m_osFilterPrefix.empty() &&
                !STARTS_WITH(entry->pszName, m_osFilterPrefix.c_str()))
            {
                continue;
            }
            return entry.get();
        }
        if (oIter.m_osNextMarker.empty())
        {
            if (m_bRecursiveRequestFromAccountRoot)
            {
                // If we have no more entries at the filesystem level, go back
                // to the root level.
                if (!m_osFilesystem.empty())
                {
                    m_osFilesystem.clear();
                    continue;
                }
            }
            return nullptr;
        }
        if (!IssueListDir())
        {
            return nullptr;
        }
    }
}

/************************************************************************/
/*                          VSIADLSHandle                              */
/************************************************************************/

class VSIADLSHandle final : public VSICurlHandle
{
    CPL_DISALLOW_COPY_ASSIGN(VSIADLSHandle)

    std::unique_ptr<VSIAzureBlobHandleHelper> m_poHandleHelper{};

  protected:
    virtual struct curl_slist *
    GetCurlHeaders(const std::string &osVerb,
                   struct curl_slist *psHeaders) override;
    bool CanRestartOnError(const char *, const char *, bool) override;

  public:
    VSIADLSHandle(VSIADLSFSHandler *poFS, const char *pszFilename,
                  VSIAzureBlobHandleHelper *poHandleHelper);
};

/************************************************************************/
/*                          CreateFileHandle()                          */
/************************************************************************/

VSICurlHandle *VSIADLSFSHandler::CreateFileHandle(const char *pszFilename)
{
    VSIAzureBlobHandleHelper *poHandleHelper =
        VSIAzureBlobHandleHelper::BuildFromURI(
            pszFilename + GetFSPrefix().size(), GetFSPrefix().c_str());
    if (poHandleHelper == nullptr)
        return nullptr;
    return new VSIADLSHandle(this, pszFilename, poHandleHelper);
}

/************************************************************************/
/*                          CreateWriteHandle()                         */
/************************************************************************/

VSIVirtualHandleUniquePtr
VSIADLSFSHandler::CreateWriteHandle(const char *pszFilename,
                                    CSLConstList papszOptions)
{
    VSIAzureBlobHandleHelper *poHandleHelper =
        VSIAzureBlobHandleHelper::BuildFromURI(
            pszFilename + GetFSPrefix().size(), GetFSPrefix().c_str());
    if (poHandleHelper == nullptr)
        return nullptr;
    auto poHandle =
        std::make_unique<VSIADLSWriteHandle>(this, pszFilename, poHandleHelper);
    if (!poHandle->CreateFile(papszOptions))
    {
        return nullptr;
    }
    return VSIVirtualHandleUniquePtr(poHandle.release());
}

/************************************************************************/
/*                                Stat()                                */
/************************************************************************/

int VSIADLSFSHandler::Stat(const char *pszFilename, VSIStatBufL *pStatBuf,
                           int nFlags)
{
    if (!STARTS_WITH_CI(pszFilename, GetFSPrefix().c_str()))
        return -1;

    if ((nFlags & VSI_STAT_CACHE_ONLY) != 0)
        return VSICurlFilesystemHandlerBase::Stat(pszFilename, pStatBuf,
                                                  nFlags);

    const std::string osFilenameWithoutSlash(RemoveTrailingSlash(pszFilename));

    // Stat("/vsiadls/") ?
    if (osFilenameWithoutSlash + "/" == GetFSPrefix())
    {
        // List file systems (stop at the first one), to confirm that the
        // account is correct
        bool bGotFileList = false;
        CSLDestroy(GetFileList(GetFSPrefix().c_str(), 1, false, &bGotFileList));
        if (bGotFileList)
        {
            memset(pStatBuf, 0, sizeof(VSIStatBufL));
            pStatBuf->st_mode = S_IFDIR;
            return 0;
        }
        return -1;
    }

    const CPLStringList aosHTTPOptions(CPLHTTPGetOptionsFromEnv(pszFilename));

    // Stat("/vsiadls/filesystem") ?
    if (osFilenameWithoutSlash.size() > GetFSPrefix().size() &&
        osFilenameWithoutSlash.substr(GetFSPrefix().size()).find('/') ==
            std::string::npos)
    {
        // Use
        // https://docs.microsoft.com/en-us/rest/api/storageservices/datalakestoragegen2/filesystem/getproperties

        NetworkStatisticsFileSystem oContextFS(GetFSPrefix().c_str());
        NetworkStatisticsAction oContextAction("GetProperties");

        const std::string osFilesystem(
            osFilenameWithoutSlash.substr(GetFSPrefix().size()));
        auto poHandleHelper = std::unique_ptr<IVSIS3LikeHandleHelper>(
            CreateHandleHelper(osFilesystem.c_str(), true));
        if (poHandleHelper == nullptr)
        {
            return -1;
        }

        CURL *hCurlHandle = curl_easy_init();

        poHandleHelper->AddQueryParameter("resource", "filesystem");

        struct curl_slist *headers =
            VSICurlSetOptions(hCurlHandle, poHandleHelper->GetURL().c_str(),
                              aosHTTPOptions.List());

        headers = poHandleHelper->GetCurlHeaders("HEAD", headers);
        unchecked_curl_easy_setopt(hCurlHandle, CURLOPT_HTTPHEADER, headers);

        unchecked_curl_easy_setopt(hCurlHandle, CURLOPT_NOBODY, 1);

        CurlRequestHelper requestHelper;
        const long response_code = requestHelper.perform(
            hCurlHandle, headers, this, poHandleHelper.get());

        NetworkStatisticsLogger::LogHEAD();

        if (response_code != 200 ||
            requestHelper.sWriteFuncHeaderData.pBuffer == nullptr)
        {
            curl_easy_cleanup(hCurlHandle);
            return -1;
        }

        memset(pStatBuf, 0, sizeof(VSIStatBufL));
        pStatBuf->st_mode = S_IFDIR;

        const char *pszLastModified = strstr(
            requestHelper.sWriteFuncHeaderData.pBuffer, "Last-Modified: ");
        if (pszLastModified)
        {
            pszLastModified += strlen("Last-Modified: ");
            const char *pszEOL = strstr(pszLastModified, "\r\n");
            if (pszEOL)
            {
                std::string osLastModified;
                osLastModified.assign(pszLastModified,
                                      pszEOL - pszLastModified);

                const GIntBig nMTime =
                    GetUnixTimeFromRFC822(osLastModified.c_str());
                if (nMTime != GINTBIG_MIN)
                {
                    pStatBuf->st_mtime = static_cast<time_t>(nMTime);
                }
            }
        }

        curl_easy_cleanup(hCurlHandle);

        return 0;
    }

    return VSICurlFilesystemHandlerBase::Stat(osFilenameWithoutSlash.c_str(),
                                              pStatBuf, nFlags);
}

/************************************************************************/
/*                          GetFileMetadata()                           */
/************************************************************************/

char **VSIADLSFSHandler::GetFileMetadata(const char *pszFilename,
                                         const char *pszDomain,
                                         CSLConstList papszOptions)
{
    if (!STARTS_WITH_CI(pszFilename, GetFSPrefix().c_str()))
        return nullptr;

    if (pszDomain == nullptr ||
        (!EQUAL(pszDomain, "STATUS") && !EQUAL(pszDomain, "ACL")))
    {
        return VSICurlFilesystemHandlerBase::GetFileMetadata(
            pszFilename, pszDomain, papszOptions);
    }

    auto poHandleHelper = std::unique_ptr<IVSIS3LikeHandleHelper>(
        CreateHandleHelper(pszFilename + GetFSPrefix().size(), false));
    if (poHandleHelper == nullptr)
    {
        return nullptr;
    }

    NetworkStatisticsFileSystem oContextFS(GetFSPrefix().c_str());
    NetworkStatisticsAction oContextAction("GetFileMetadata");

    bool bRetry;
    bool bError = true;

    const CPLStringList aosHTTPOptions(CPLHTTPGetOptionsFromEnv(pszFilename));
    const CPLHTTPRetryParameters oRetryParameters(aosHTTPOptions);
    CPLHTTPRetryContext oRetryContext(oRetryParameters);

    CPLStringList aosMetadata;
    do
    {
        bRetry = false;
        CURL *hCurlHandle = curl_easy_init();
        poHandleHelper->AddQueryParameter("action", EQUAL(pszDomain, "STATUS")
                                                        ? "getStatus"
                                                        : "getAccessControl");

        struct curl_slist *headers =
            VSICurlSetOptions(hCurlHandle, poHandleHelper->GetURL().c_str(),
                              aosHTTPOptions.List());

        headers = poHandleHelper->GetCurlHeaders("HEAD", headers);
        unchecked_curl_easy_setopt(hCurlHandle, CURLOPT_HTTPHEADER, headers);

        unchecked_curl_easy_setopt(hCurlHandle, CURLOPT_NOBODY, 1);

        CurlRequestHelper requestHelper;
        const long response_code = requestHelper.perform(
            hCurlHandle, headers, this, poHandleHelper.get());

        NetworkStatisticsLogger::LogHEAD();

        if (response_code != 200 ||
            requestHelper.sWriteFuncHeaderData.pBuffer == nullptr)
        {
            // Look if we should attempt a retry
            if (oRetryContext.CanRetry(
                    static_cast<int>(response_code),
                    requestHelper.sWriteFuncHeaderData.pBuffer,
                    requestHelper.szCurlErrBuf))
            {
                CPLError(CE_Warning, CPLE_AppDefined,
                         "HTTP error code: %d - %s. "
                         "Retrying again in %.1f secs",
                         static_cast<int>(response_code),
                         poHandleHelper->GetURL().c_str(),
                         oRetryContext.GetCurrentDelay());
                CPLSleep(oRetryContext.GetCurrentDelay());
                bRetry = true;
            }
            else
            {
                CPLDebug(GetDebugKey(), "GetFileMetadata failed on %s: %s",
                         pszFilename,
                         requestHelper.sWriteFuncData.pBuffer
                             ? requestHelper.sWriteFuncData.pBuffer
                             : "(null)");
            }
        }
        else
        {
            char **papszHeaders = CSLTokenizeString2(
                requestHelper.sWriteFuncHeaderData.pBuffer, "\r\n", 0);
            for (int i = 0; papszHeaders[i]; ++i)
            {
                char *pszKey = nullptr;
                const char *pszValue =
                    CPLParseNameValue(papszHeaders[i], &pszKey);
                if (pszKey && pszValue && !EQUAL(pszKey, "Server") &&
                    !EQUAL(pszKey, "Date"))
                {
                    aosMetadata.SetNameValue(pszKey, pszValue);
                }
                CPLFree(pszKey);
            }
            CSLDestroy(papszHeaders);
            bError = false;
        }

        curl_easy_cleanup(hCurlHandle);
    } while (bRetry);
    return bError ? nullptr : CSLDuplicate(aosMetadata.List());
}

/************************************************************************/
/*                          SetFileMetadata()                           */
/************************************************************************/

bool VSIADLSFSHandler::SetFileMetadata(const char *pszFilename,
                                       CSLConstList papszMetadata,
                                       const char *pszDomain,
                                       CSLConstList papszOptions)
{
    if (!STARTS_WITH_CI(pszFilename, GetFSPrefix().c_str()))
        return false;

    if (pszDomain == nullptr ||
        !(EQUAL(pszDomain, "PROPERTIES") || EQUAL(pszDomain, "ACL")))
    {
        CPLError(CE_Failure, CPLE_NotSupported,
                 "Only PROPERTIES and ACL domain are supported");
        return false;
    }

    auto poHandleHelper = std::unique_ptr<IVSIS3LikeHandleHelper>(
        CreateHandleHelper(pszFilename + GetFSPrefix().size(), false));
    if (poHandleHelper == nullptr)
    {
        return false;
    }

    const bool bRecursive =
        CPLTestBool(CSLFetchNameValueDef(papszOptions, "RECURSIVE", "FALSE"));
    const char *pszMode = CSLFetchNameValue(papszOptions, "MODE");
    if (!EQUAL(pszDomain, "PROPERTIES") && bRecursive && pszMode == nullptr)
    {
        CPLError(CE_Failure, CPLE_AppDefined,
                 "For setAccessControlRecursive, the MODE option should be set "
                 "to: 'set', 'modify' or 'remove'");
        return false;
    }

    NetworkStatisticsFileSystem oContextFS(GetFSPrefix().c_str());
    NetworkStatisticsAction oContextAction("SetFileMetadata");

    bool bRetry;
    bool bRet = false;

    const CPLStringList aosHTTPOptions(CPLHTTPGetOptionsFromEnv(pszFilename));
    const CPLHTTPRetryParameters oRetryParameters(aosHTTPOptions);
    CPLHTTPRetryContext oRetryContext(oRetryParameters);

    do
    {
        bRetry = false;
        CURL *hCurlHandle = curl_easy_init();
        poHandleHelper->AddQueryParameter(
            "action", EQUAL(pszDomain, "PROPERTIES") ? "setProperties"
                      : bRecursive ? "setAccessControlRecursive"
                                   : "setAccessControl");
        if (pszMode)
        {
            poHandleHelper->AddQueryParameter("mode",
                                              CPLString(pszMode).tolower());
        }
        unchecked_curl_easy_setopt(hCurlHandle, CURLOPT_CUSTOMREQUEST, "PATCH");

        struct curl_slist *headers = static_cast<struct curl_slist *>(
            CPLHTTPSetOptions(hCurlHandle, poHandleHelper->GetURL().c_str(),
                              aosHTTPOptions.List()));

        CPLStringList aosList;
        for (CSLConstList papszIter = papszMetadata; papszIter && *papszIter;
             ++papszIter)
        {
            char *pszKey = nullptr;
            const char *pszValue = CPLParseNameValue(*papszIter, &pszKey);
            if (pszKey && pszValue)
            {
                if ((EQUAL(pszDomain, "PROPERTIES") &&
                     (EQUAL(pszKey, "x-ms-lease-id") ||
                      EQUAL(pszKey, "x-ms-cache-control") ||
                      EQUAL(pszKey, "x-ms-content-type") ||
                      EQUAL(pszKey, "x-ms-content-disposition") ||
                      EQUAL(pszKey, "x-ms-content-encoding") ||
                      EQUAL(pszKey, "x-ms-content-language") ||
                      EQUAL(pszKey, "x-ms-content-md5") ||
                      EQUAL(pszKey, "x-ms-properties") ||
                      EQUAL(pszKey, "x-ms-client-request-id") ||
                      STARTS_WITH_CI(pszKey, "If-"))) ||
                    (!EQUAL(pszDomain, "PROPERTIES") && !bRecursive &&
                     (EQUAL(pszKey, "x-ms-lease-id") ||
                      EQUAL(pszKey, "x-ms-owner") ||
                      EQUAL(pszKey, "x-ms-group") ||
                      EQUAL(pszKey, "x-ms-permissions") ||
                      EQUAL(pszKey, "x-ms-acl") ||
                      EQUAL(pszKey, "x-ms-client-request-id") ||
                      STARTS_WITH_CI(pszKey, "If-"))) ||
                    (!EQUAL(pszDomain, "PROPERTIES") && bRecursive &&
                     (EQUAL(pszKey, "x-ms-lease-id") ||
                      EQUAL(pszKey, "x-ms-acl") ||
                      EQUAL(pszKey, "x-ms-client-request-id") ||
                      STARTS_WITH_CI(pszKey, "If-"))))
                {
                    const char *pszHeader =
                        CPLSPrintf("%s: %s", pszKey, pszValue);
                    aosList.AddString(pszHeader);
                    headers = curl_slist_append(headers, pszHeader);
                }
                else
                {
                    CPLDebug(GetDebugKey(), "Ignorizing metadata item %s",
                             *papszIter);
                }
            }
            CPLFree(pszKey);
        }

        headers = poHandleHelper->GetCurlHeaders("PATCH", headers);
        unchecked_curl_easy_setopt(hCurlHandle, CURLOPT_HTTPHEADER, headers);

        NetworkStatisticsLogger::LogPUT(0);

        CurlRequestHelper requestHelper;
        const long response_code = requestHelper.perform(
            hCurlHandle, headers, this, poHandleHelper.get());

        if (response_code != 200 && response_code != 202)
        {
            // Look if we should attempt a retry
            if (oRetryContext.CanRetry(
                    static_cast<int>(response_code),
                    requestHelper.sWriteFuncHeaderData.pBuffer,
                    requestHelper.szCurlErrBuf))
            {
                CPLError(CE_Warning, CPLE_AppDefined,
                         "HTTP error code: %d - %s. "
                         "Retrying again in %.1f secs",
                         static_cast<int>(response_code),
                         poHandleHelper->GetURL().c_str(),
                         oRetryContext.GetCurrentDelay());
                CPLSleep(oRetryContext.GetCurrentDelay());
                bRetry = true;
            }
            else
            {
                CPLDebug(GetDebugKey(), "SetFileMetadata on %s failed: %s",
                         pszFilename,
                         requestHelper.sWriteFuncData.pBuffer
                             ? requestHelper.sWriteFuncData.pBuffer
                             : "(null)");
            }
        }
        else
        {
            bRet = true;
        }

        curl_easy_cleanup(hCurlHandle);
    } while (bRetry);
    return bRet;
}

/************************************************************************/
/*                       VSIADLSWriteHandle()                          */
/************************************************************************/

VSIADLSWriteHandle::VSIADLSWriteHandle(VSIADLSFSHandler *poFS,
                                       const char *pszFilename,
                                       VSIAzureBlobHandleHelper *poHandleHelper)
    : VSIAppendWriteHandle(poFS, poFS->GetFSPrefix().c_str(), pszFilename,
                           GetAzureAppendBufferSize()),
      m_poHandleHelper(poHandleHelper)
{
}

/************************************************************************/
/*                      ~VSIADLSWriteHandle()                          */
/************************************************************************/

VSIADLSWriteHandle::~VSIADLSWriteHandle()
{
    Close();
}

/************************************************************************/
/*                    InvalidateParentDirectory()                       */
/************************************************************************/

void VSIADLSWriteHandle::InvalidateParentDirectory()
{
    m_poFS->InvalidateCachedData(m_poHandleHelper->GetURLNoKVP().c_str());

    const std::string osFilenameWithoutSlash(RemoveTrailingSlash(m_osFilename));
    m_poFS->InvalidateDirContent(
        CPLGetDirnameSafe(osFilenameWithoutSlash.c_str()));
}

/************************************************************************/
/*                          CreateFile()                                */
/************************************************************************/

bool VSIADLSWriteHandle::CreateFile(CSLConstList papszOptions)
{
    m_bCreated =
        SendInternal(VSIADLSFSHandler::Event::CREATE_FILE, papszOptions);
    return m_bCreated;
}

/************************************************************************/
/*                             Send()                                   */
/************************************************************************/

bool VSIADLSWriteHandle::Send(bool bIsLastBlock)
{
    if (!m_bCreated)
        return false;
    // If we have a non-empty buffer, append it
    if (m_nBufferOff != 0 &&
        !SendInternal(VSIADLSFSHandler::Event::APPEND_DATA, nullptr))
        return false;
    // If we are the last block, send the flush event
    if (bIsLastBlock && !SendInternal(VSIADLSFSHandler::Event::FLUSH, nullptr))
        return false;

    InvalidateParentDirectory();

    return true;
}

/************************************************************************/
/*                          SendInternal()                              */
/************************************************************************/

bool VSIADLSWriteHandle::SendInternal(VSIADLSFSHandler::Event event,
                                      CSLConstList papszOptions)
{
    return cpl::down_cast<VSIADLSFSHandler *>(m_poFS)->UploadFile(
        m_osFilename, event,
        event == VSIADLSFSHandler::Event::CREATE_FILE ? 0
        : event == VSIADLSFSHandler::Event::APPEND_DATA
            ? m_nCurOffset - m_nBufferOff
            : m_nCurOffset,
        m_pabyBuffer, m_nBufferOff, m_poHandleHelper.get(), m_oRetryParameters,
        papszOptions);
}

/************************************************************************/
/*                            ClearCache()                              */
/************************************************************************/

void VSIADLSFSHandler::ClearCache()
{
    IVSIS3LikeFSHandler::ClearCache();

    VSIAzureBlobHandleHelper::ClearCache();
}

/************************************************************************/
/*                          GetURLFromFilename()                        */
/************************************************************************/

std::string
VSIADLSFSHandler::GetURLFromFilename(const std::string &osFilename) const
{
    const std::string osFilenameWithoutPrefix =
        osFilename.substr(GetFSPrefix().size());
    auto poHandleHelper = std::unique_ptr<VSIAzureBlobHandleHelper>(
        VSIAzureBlobHandleHelper::BuildFromURI(osFilenameWithoutPrefix.c_str(),
                                               GetFSPrefix().c_str()));
    if (!poHandleHelper)
        return std::string();
    return poHandleHelper->GetURLNoKVP();
}

/************************************************************************/
/*                          CreateHandleHelper()                        */
/************************************************************************/

IVSIS3LikeHandleHelper *VSIADLSFSHandler::CreateHandleHelper(const char *pszURI,
                                                             bool)
{
    return VSIAzureBlobHandleHelper::BuildFromURI(pszURI,
                                                  GetFSPrefix().c_str());
}

/************************************************************************/
/*                               Rename()                               */
/************************************************************************/

int VSIADLSFSHandler::Rename(const char *oldpath, const char *newpath,
                             GDALProgressFunc, void *)
{
    if (!STARTS_WITH_CI(oldpath, GetFSPrefix().c_str()))
        return -1;
    if (!STARTS_WITH_CI(newpath, GetFSPrefix().c_str()))
        return -1;

    NetworkStatisticsFileSystem oContextFS(GetFSPrefix().c_str());
    NetworkStatisticsAction oContextAction("Rename");

    VSIStatBufL sStat;
    if (VSIStatL(oldpath, &sStat) != 0)
    {
        CPLDebug(GetDebugKey(), "%s is not a object", oldpath);
        errno = ENOENT;
        return -1;
    }

    // POSIX says renaming on the same file is OK
    if (strcmp(oldpath, newpath) == 0)
        return 0;

    auto poHandleHelper = std::unique_ptr<IVSIS3LikeHandleHelper>(
        CreateHandleHelper(newpath + GetFSPrefix().size(), false));
    if (poHandleHelper == nullptr)
    {
        return -1;
    }

    std::string osContinuation;
    int nRet = 0;
    bool bRetry;

    InvalidateCachedData(GetURLFromFilename(oldpath).c_str());
    InvalidateCachedData(GetURLFromFilename(newpath).c_str());
    InvalidateDirContent(CPLGetDirnameSafe(oldpath));

    const CPLStringList aosHTTPOptions(CPLHTTPGetOptionsFromEnv(oldpath));
    const CPLHTTPRetryParameters oRetryParameters(aosHTTPOptions);
    CPLHTTPRetryContext oRetryContext(oRetryParameters);

    do
    {
        bRetry = false;

        CURL *hCurlHandle = curl_easy_init();
        unchecked_curl_easy_setopt(hCurlHandle, CURLOPT_CUSTOMREQUEST, "PUT");

        poHandleHelper->ResetQueryParameters();
        if (!osContinuation.empty())
            poHandleHelper->AddQueryParameter("continuation", osContinuation);

        struct curl_slist *headers = static_cast<struct curl_slist *>(
            CPLHTTPSetOptions(hCurlHandle, poHandleHelper->GetURL().c_str(),
                              aosHTTPOptions.List()));
        headers = curl_slist_append(headers, "Content-Length: 0");
        std::string osRenameSource("x-ms-rename-source: /");
        osRenameSource +=
            CPLAWSURLEncode(oldpath + GetFSPrefix().size(), false);
        headers = curl_slist_append(headers, osRenameSource.c_str());
        headers = poHandleHelper->GetCurlHeaders("PUT", headers);
        unchecked_curl_easy_setopt(hCurlHandle, CURLOPT_HTTPHEADER, headers);

        CurlRequestHelper requestHelper;
        const long response_code = requestHelper.perform(
            hCurlHandle, headers, this, poHandleHelper.get());

        NetworkStatisticsLogger::LogPUT(0);

        if (response_code != 201)
        {
            // Look if we should attempt a retry
            if (oRetryContext.CanRetry(
                    static_cast<int>(response_code),
                    requestHelper.sWriteFuncHeaderData.pBuffer,
                    requestHelper.szCurlErrBuf))
            {
                CPLError(CE_Warning, CPLE_AppDefined,
                         "HTTP error code: %d - %s. "
                         "Retrying again in %.1f secs",
                         static_cast<int>(response_code),
                         poHandleHelper->GetURL().c_str(),
                         oRetryContext.GetCurrentDelay());
                CPLSleep(oRetryContext.GetCurrentDelay());
                bRetry = true;
            }
            else
            {
                CPLDebug(GetDebugKey(), "Renaming of %s failed: %s", oldpath,
                         requestHelper.sWriteFuncData.pBuffer
                             ? requestHelper.sWriteFuncData.pBuffer
                             : "(null)");
                nRet = -1;
            }
        }
        else
        {
            // Get continuation token for response headers
            osContinuation = GetContinuationToken(
                requestHelper.sWriteFuncHeaderData.pBuffer);
            if (!osContinuation.empty())
            {
                oRetryContext.ResetCounter();
                bRetry = true;
            }
        }

        curl_easy_cleanup(hCurlHandle);
    } while (bRetry);

    return nRet;
}

/************************************************************************/
/*                               Unlink()                               */
/************************************************************************/

int VSIADLSFSHandler::Unlink(const char *pszFilename)
{
    return IVSIS3LikeFSHandler::Unlink(pszFilename);
}

/************************************************************************/
/*                               Mkdir()                                */
/************************************************************************/

int VSIADLSFSHandler::MkdirInternal(const char *pszDirname, long nMode,
                                    bool bDoStatCheck)
{
    if (!STARTS_WITH_CI(pszDirname, GetFSPrefix().c_str()))
        return -1;

    NetworkStatisticsFileSystem oContextFS(GetFSPrefix().c_str());
    NetworkStatisticsAction oContextAction("Mkdir");

    const std::string osDirname(pszDirname);

    if (bDoStatCheck)
    {
        VSIStatBufL sStat;
        if (VSIStatL(osDirname.c_str(), &sStat) == 0)
        {
            CPLDebug(GetDebugKey(), "Directory or file %s already exists",
                     osDirname.c_str());
            errno = EEXIST;
            return -1;
        }
    }

    const std::string osDirnameWithoutEndSlash(RemoveTrailingSlash(osDirname));
    auto poHandleHelper =
        std::unique_ptr<IVSIS3LikeHandleHelper>(CreateHandleHelper(
            osDirnameWithoutEndSlash.c_str() + GetFSPrefix().size(), false));
    if (poHandleHelper == nullptr)
    {
        return -1;
    }

    InvalidateCachedData(GetURLFromFilename(osDirname.c_str()).c_str());
    InvalidateCachedData(
        GetURLFromFilename(osDirnameWithoutEndSlash.c_str()).c_str());
    InvalidateDirContent(CPLGetDirnameSafe(osDirnameWithoutEndSlash.c_str()));

    int nRet = 0;

    bool bRetry;

    const CPLStringList aosHTTPOptions(CPLHTTPGetOptionsFromEnv(pszDirname));
    const CPLHTTPRetryParameters oRetryParameters(aosHTTPOptions);
    CPLHTTPRetryContext oRetryContext(oRetryParameters);

    do
    {
        bRetry = false;
        CURL *hCurlHandle = curl_easy_init();
        unchecked_curl_easy_setopt(hCurlHandle, CURLOPT_CUSTOMREQUEST, "PUT");

        poHandleHelper->ResetQueryParameters();
        poHandleHelper->AddQueryParameter(
            "resource", osDirnameWithoutEndSlash.find(
                            '/', GetFSPrefix().size()) == std::string::npos
                            ? "filesystem"
                            : "directory");

        struct curl_slist *headers = static_cast<struct curl_slist *>(
            CPLHTTPSetOptions(hCurlHandle, poHandleHelper->GetURL().c_str(),
                              aosHTTPOptions.List()));
        headers = curl_slist_append(headers, "Content-Length: 0");
        CPLString osPermissions;  // keep in this scope
        if ((nMode & 0777) != 0)
        {
            osPermissions.Printf("x-ms-permissions: 0%03o",
                                 static_cast<int>(nMode));
            headers = curl_slist_append(headers, osPermissions.c_str());
        }
        if (bDoStatCheck)
        {
            headers = curl_slist_append(headers, "If-None-Match: \"*\"");
        }

        headers = poHandleHelper->GetCurlHeaders("PUT", headers);
        unchecked_curl_easy_setopt(hCurlHandle, CURLOPT_HTTPHEADER, headers);

        CurlRequestHelper requestHelper;
        const long response_code = requestHelper.perform(
            hCurlHandle, headers, this, poHandleHelper.get());

        NetworkStatisticsLogger::LogPUT(0);

        if (response_code != 201)
        {
            // Look if we should attempt a retry
            if (oRetryContext.CanRetry(
                    static_cast<int>(response_code),
                    requestHelper.sWriteFuncHeaderData.pBuffer,
                    requestHelper.szCurlErrBuf))
            {
                CPLError(CE_Warning, CPLE_AppDefined,
                         "HTTP error code: %d - %s. "
                         "Retrying again in %.1f secs",
                         static_cast<int>(response_code),
                         poHandleHelper->GetURL().c_str(),
                         oRetryContext.GetCurrentDelay());
                CPLSleep(oRetryContext.GetCurrentDelay());
                bRetry = true;
            }
            else
            {
                CPLDebug(GetDebugKey(), "Creation of %s failed: %s",
                         osDirname.c_str(),
                         requestHelper.sWriteFuncData.pBuffer
                             ? requestHelper.sWriteFuncData.pBuffer
                             : "(null)");
                nRet = -1;
            }
        }

        curl_easy_cleanup(hCurlHandle);
    } while (bRetry);

    return nRet;
}

int VSIADLSFSHandler::Mkdir(const char *pszDirname, long nMode)
{
    return MkdirInternal(pszDirname, nMode, true);
}

/************************************************************************/
/*                          RmdirInternal()                             */
/************************************************************************/

int VSIADLSFSHandler::RmdirInternal(const char *pszDirname, bool bRecursive)
{
    const std::string osDirname(pszDirname);
    const std::string osDirnameWithoutEndSlash(
        RemoveTrailingSlash(osDirname.c_str()));

    const bool bIsFileSystem =
        osDirnameWithoutEndSlash.find('/', GetFSPrefix().size()) ==
        std::string::npos;

    if (!bRecursive && bIsFileSystem)
    {
        // List content, to confirm it is empty first, as filesystem deletion
        // is recursive by default.
        bool bGotFileList = false;
        CSLDestroy(GetFileList(osDirnameWithoutEndSlash.c_str(), 1, false,
                               &bGotFileList));
        if (bGotFileList)
        {
            CPLDebug(GetDebugKey(), "Cannot delete filesystem with "
                                    "non-recursive method as it is not empty");
            errno = ENOTEMPTY;
            return -1;
        }
    }

    if (!bIsFileSystem)
    {
        VSIStatBufL sStat;
        if (VSIStatL(osDirname.c_str(), &sStat) != 0)
        {
            CPLDebug(GetDebugKey(), "Object %s does not exist",
                     osDirname.c_str());
            errno = ENOENT;
            return -1;
        }
        if (!VSI_ISDIR(sStat.st_mode))
        {
            CPLDebug(GetDebugKey(), "Object %s is not a directory",
                     osDirname.c_str());
            errno = ENOTDIR;
            return -1;
        }
    }

    auto poHandleHelper =
        std::unique_ptr<IVSIS3LikeHandleHelper>(CreateHandleHelper(
            osDirnameWithoutEndSlash.c_str() + GetFSPrefix().size(), false));
    if (poHandleHelper == nullptr)
    {
        return -1;
    }

    InvalidateCachedData(GetURLFromFilename(osDirname.c_str()).c_str());
    InvalidateCachedData(
        GetURLFromFilename(osDirnameWithoutEndSlash.c_str()).c_str());
    InvalidateDirContent(CPLGetDirnameSafe(osDirnameWithoutEndSlash.c_str()));
    if (bRecursive)
    {
        PartialClearCache(osDirnameWithoutEndSlash.c_str());
    }

    std::string osContinuation;
    int nRet = 0;
    bool bRetry;

    const CPLStringList aosHTTPOptions(CPLHTTPGetOptionsFromEnv(pszDirname));
    const CPLHTTPRetryParameters oRetryParameters(aosHTTPOptions);
    CPLHTTPRetryContext oRetryContext(oRetryParameters);

    do
    {
        bRetry = false;
        CURL *hCurlHandle = curl_easy_init();
        unchecked_curl_easy_setopt(hCurlHandle, CURLOPT_CUSTOMREQUEST,
                                   "DELETE");

        poHandleHelper->ResetQueryParameters();
        if (bIsFileSystem)
        {
            poHandleHelper->AddQueryParameter("resource", "filesystem");
        }
        else
        {
            poHandleHelper->AddQueryParameter("recursive",
                                              bRecursive ? "true" : "false");
            if (!osContinuation.empty())
                poHandleHelper->AddQueryParameter("continuation",
                                                  osContinuation);
        }

        struct curl_slist *headers = static_cast<struct curl_slist *>(
            CPLHTTPSetOptions(hCurlHandle, poHandleHelper->GetURL().c_str(),
                              aosHTTPOptions.List()));
        headers = poHandleHelper->GetCurlHeaders("DELETE", headers);

        CurlRequestHelper requestHelper;
        const long response_code = requestHelper.perform(
            hCurlHandle, headers, this, poHandleHelper.get());

        NetworkStatisticsLogger::LogDELETE();

        // 200 for path deletion
        // 202 for filesystem deletion
        if (response_code != 200 && response_code != 202)
        {
            // Look if we should attempt a retry
            if (oRetryContext.CanRetry(
                    static_cast<int>(response_code),
                    requestHelper.sWriteFuncHeaderData.pBuffer,
                    requestHelper.szCurlErrBuf))
            {
                CPLError(CE_Warning, CPLE_AppDefined,
                         "HTTP error code: %d - %s. "
                         "Retrying again in %.1f secs",
                         static_cast<int>(response_code),
                         poHandleHelper->GetURL().c_str(),
                         oRetryContext.GetCurrentDelay());
                CPLSleep(oRetryContext.GetCurrentDelay());
                bRetry = true;
            }
            else
            {
                CPLDebug(GetDebugKey(), "Delete of %s failed: %s",
                         osDirname.c_str(),
                         requestHelper.sWriteFuncData.pBuffer
                             ? requestHelper.sWriteFuncData.pBuffer
                             : "(null)");
                if (requestHelper.sWriteFuncData.pBuffer != nullptr)
                {
                    VSIError(VSIE_ObjectStorageGenericError, "%s",
                             requestHelper.sWriteFuncData.pBuffer);
                    if (strstr(requestHelper.sWriteFuncData.pBuffer,
                               "PathNotFound"))
                    {
                        errno = ENOENT;
                    }
                    else if (strstr(requestHelper.sWriteFuncData.pBuffer,
                                    "DirectoryNotEmpty"))
                    {
                        errno = ENOTEMPTY;
                    }
                }
                nRet = -1;
            }
        }
        else
        {
            // Get continuation token for response headers
            osContinuation = GetContinuationToken(
                requestHelper.sWriteFuncHeaderData.pBuffer);
            if (!osContinuation.empty())
            {
                oRetryContext.ResetCounter();
                bRetry = true;
            }
        }

        curl_easy_cleanup(hCurlHandle);
    } while (bRetry);

    return nRet;
}

/************************************************************************/
/*                               Rmdir()                                */
/************************************************************************/

int VSIADLSFSHandler::Rmdir(const char *pszDirname)
{
    if (!STARTS_WITH_CI(pszDirname, GetFSPrefix().c_str()))
        return -1;

    NetworkStatisticsFileSystem oContextFS(GetFSPrefix().c_str());
    NetworkStatisticsAction oContextAction("Rmdir");

    return RmdirInternal(pszDirname, false);
}

/************************************************************************/
/*                          RmdirRecursive()                            */
/************************************************************************/

int VSIADLSFSHandler::RmdirRecursive(const char *pszDirname)
{
    if (!STARTS_WITH_CI(pszDirname, GetFSPrefix().c_str()))
        return -1;

    NetworkStatisticsFileSystem oContextFS(GetFSPrefix().c_str());
    NetworkStatisticsAction oContextAction("RmdirRecursive");

    return RmdirInternal(pszDirname, true);
}

/************************************************************************/
/*                            CopyObject()                              */
/************************************************************************/

int VSIADLSFSHandler::CopyObject(const char *oldpath, const char *newpath,
                                 CSLConstList /* papszMetadata */)
{
    // There is no CopyObject in ADLS... So use the base Azure blob one...

    NetworkStatisticsFileSystem oContextFS(GetFSPrefix().c_str());
    NetworkStatisticsAction oContextAction("CopyObject");

    std::string osTargetNameWithoutPrefix = newpath + GetFSPrefix().size();
    auto poAzHandleHelper = std::unique_ptr<IVSIS3LikeHandleHelper>(
        VSIAzureBlobHandleHelper::BuildFromURI(
            osTargetNameWithoutPrefix.c_str(), "/vsiaz/"));
    if (poAzHandleHelper == nullptr)
    {
        return -1;
    }

    std::string osSourceNameWithoutPrefix = oldpath + GetFSPrefix().size();
    auto poAzHandleHelperSource = std::unique_ptr<IVSIS3LikeHandleHelper>(
        VSIAzureBlobHandleHelper::BuildFromURI(
            osSourceNameWithoutPrefix.c_str(), "/vsiaz/"));
    if (poAzHandleHelperSource == nullptr)
    {
        return -1;
    }

    std::string osSourceHeader("x-ms-copy-source: ");
    osSourceHeader += poAzHandleHelperSource->GetURLNoKVP();

    int nRet = 0;

    bool bRetry;

    const CPLStringList aosHTTPOptions(CPLHTTPGetOptionsFromEnv(oldpath));
    const CPLHTTPRetryParameters oRetryParameters(aosHTTPOptions);
    CPLHTTPRetryContext oRetryContext(oRetryParameters);

    do
    {
        bRetry = false;
        CURL *hCurlHandle = curl_easy_init();
        unchecked_curl_easy_setopt(hCurlHandle, CURLOPT_CUSTOMREQUEST, "PUT");

        struct curl_slist *headers = static_cast<struct curl_slist *>(
            CPLHTTPSetOptions(hCurlHandle, poAzHandleHelper->GetURL().c_str(),
                              aosHTTPOptions.List()));
        headers = curl_slist_append(headers, osSourceHeader.c_str());
        headers = curl_slist_append(headers, "Content-Length: 0");
        headers = VSICurlSetContentTypeFromExt(headers, newpath);
        headers = poAzHandleHelper->GetCurlHeaders("PUT", headers);
        unchecked_curl_easy_setopt(hCurlHandle, CURLOPT_HTTPHEADER, headers);

        CurlRequestHelper requestHelper;
        const long response_code = requestHelper.perform(
            hCurlHandle, headers, this, poAzHandleHelper.get());

        NetworkStatisticsLogger::LogPUT(0);

        if (response_code != 202)
        {
            // Look if we should attempt a retry
            if (oRetryContext.CanRetry(
                    static_cast<int>(response_code),
                    requestHelper.sWriteFuncHeaderData.pBuffer,
                    requestHelper.szCurlErrBuf))
            {
                CPLError(CE_Warning, CPLE_AppDefined,
                         "HTTP error code: %d - %s. "
                         "Retrying again in %.1f secs",
                         static_cast<int>(response_code),
                         poAzHandleHelper->GetURL().c_str(),
                         oRetryContext.GetCurrentDelay());
                CPLSleep(oRetryContext.GetCurrentDelay());
                bRetry = true;
            }
            else
            {
                CPLDebug(GetDebugKey(), "%s",
                         requestHelper.sWriteFuncData.pBuffer
                             ? requestHelper.sWriteFuncData.pBuffer
                             : "(null)");
                CPLError(CE_Failure, CPLE_AppDefined, "Copy of %s to %s failed",
                         oldpath, newpath);
                nRet = -1;
            }
        }
        else
        {
            auto poADLSHandleHelper = std::unique_ptr<IVSIS3LikeHandleHelper>(
                VSIAzureBlobHandleHelper::BuildFromURI(
                    osTargetNameWithoutPrefix.c_str(), GetFSPrefix().c_str()));
            if (poADLSHandleHelper != nullptr)
                InvalidateCachedData(poADLSHandleHelper->GetURLNoKVP().c_str());

            const std::string osFilenameWithoutSlash(
                RemoveTrailingSlash(newpath));
            InvalidateDirContent(
                CPLGetDirnameSafe(osFilenameWithoutSlash.c_str()));
        }

        curl_easy_cleanup(hCurlHandle);
    } while (bRetry);

    return nRet;
}

/************************************************************************/
/*                          UploadFile()                                */
/************************************************************************/

bool VSIADLSFSHandler::UploadFile(
    const std::string &osFilename, Event event, vsi_l_offset nPosition,
    const void *pabyBuffer, size_t nBufferSize,
    IVSIS3LikeHandleHelper *poHandleHelper,
    const CPLHTTPRetryParameters &oRetryParameters, CSLConstList papszOptions)
{
    NetworkStatisticsFileSystem oContextFS(GetFSPrefix().c_str());
    NetworkStatisticsFile oContextFile(osFilename.c_str());
    NetworkStatisticsAction oContextAction("UploadFile");

    if (event == Event::CREATE_FILE)
    {
        InvalidateCachedData(poHandleHelper->GetURLNoKVP().c_str());
        InvalidateDirContent(CPLGetDirnameSafe(osFilename.c_str()));
    }

    const CPLStringList aosHTTPOptions(
        CPLHTTPGetOptionsFromEnv(osFilename.c_str()));

    bool bSuccess = true;
    CPLHTTPRetryContext oRetryContext(oRetryParameters);
    bool bRetry;
    do
    {
        bRetry = false;

        CURL *hCurlHandle = curl_easy_init();

        poHandleHelper->ResetQueryParameters();
        if (event == Event::CREATE_FILE)
        {
            // Cf https://learn.microsoft.com/en-us/rest/api/storageservices/datalakestoragegen2/path/create?view=rest-storageservices-datalakestoragegen2-2019-12-12
            poHandleHelper->AddQueryParameter("resource", "file");
        }
        else if (event == Event::APPEND_DATA)
        {
            // Cf https://learn.microsoft.com/en-us/rest/api/storageservices/datalakestoragegen2/path/update?view=rest-storageservices-datalakestoragegen2-2019-12-12
            poHandleHelper->AddQueryParameter("action", "append");
            poHandleHelper->AddQueryParameter(
                "position",
                CPLSPrintf(CPL_FRMT_GUIB, static_cast<GUIntBig>(nPosition)));
        }
        else
        {
            // Cf https://learn.microsoft.com/en-us/rest/api/storageservices/datalakestoragegen2/path/update?view=rest-storageservices-datalakestoragegen2-2019-12-12
            poHandleHelper->AddQueryParameter("action", "flush");
            poHandleHelper->AddQueryParameter("close", "true");
            poHandleHelper->AddQueryParameter(
                "position",
                CPLSPrintf(CPL_FRMT_GUIB, static_cast<GUIntBig>(nPosition)));
        }

        unchecked_curl_easy_setopt(hCurlHandle, CURLOPT_UPLOAD, 1L);
        unchecked_curl_easy_setopt(hCurlHandle, CURLOPT_READFUNCTION,
                                   PutData::ReadCallBackBuffer);
        PutData putData;
        putData.pabyData = static_cast<const GByte *>(pabyBuffer);
        putData.nOff = 0;
        putData.nTotalSize = nBufferSize;
        unchecked_curl_easy_setopt(hCurlHandle, CURLOPT_READDATA, &putData);

        struct curl_slist *headers = static_cast<struct curl_slist *>(
            CPLHTTPSetOptions(hCurlHandle, poHandleHelper->GetURL().c_str(),
                              aosHTTPOptions.List()));
        headers = VSICurlSetCreationHeadersFromOptions(headers, papszOptions,
                                                       osFilename.c_str());

        CPLString osContentLength;  // leave it in this scope

        if (event == Event::APPEND_DATA)
        {
            unchecked_curl_easy_setopt(hCurlHandle, CURLOPT_INFILESIZE,
                                       static_cast<int>(nBufferSize));
            // Disable "Expect: 100-continue" which doesn't hurt, but is not
            // needed
            headers = curl_slist_append(headers, "Expect:");
            osContentLength.Printf("Content-Length: %d",
                                   static_cast<int>(nBufferSize));
            headers = curl_slist_append(headers, osContentLength.c_str());
        }
        else
        {
            unchecked_curl_easy_setopt(hCurlHandle, CURLOPT_INFILESIZE, 0);
            headers = curl_slist_append(headers, "Content-Length: 0");
        }

        const char *pszVerb = (event == Event::CREATE_FILE) ? "PUT" : "PATCH";
        unchecked_curl_easy_setopt(hCurlHandle, CURLOPT_CUSTOMREQUEST, pszVerb);
        headers = poHandleHelper->GetCurlHeaders(pszVerb, headers);
        unchecked_curl_easy_setopt(hCurlHandle, CURLOPT_HTTPHEADER, headers);

        CurlRequestHelper requestHelper;
        const long response_code =
            requestHelper.perform(hCurlHandle, headers, this, poHandleHelper);

        NetworkStatisticsLogger::LogPUT(
            event == Event::APPEND_DATA ? nBufferSize : 0);

        // 200 for PATCH flush
        // 201 for PUT create
        // 202 for PATCH append
        if (response_code != 200 && response_code != 201 &&
            response_code != 202)
        {
            // Look if we should attempt a retry
            if (oRetryContext.CanRetry(
                    static_cast<int>(response_code),
                    requestHelper.sWriteFuncHeaderData.pBuffer,
                    requestHelper.szCurlErrBuf))
            {
                CPLError(CE_Warning, CPLE_AppDefined,
                         "HTTP error code: %d - %s. "
                         "Retrying again in %.1f secs",
                         static_cast<int>(response_code),
                         poHandleHelper->GetURL().c_str(),
                         oRetryContext.GetCurrentDelay());
                CPLSleep(oRetryContext.GetCurrentDelay());
                bRetry = true;
            }
            else
            {
                CPLDebug(GetDebugKey(), "%s of %s failed: %s", pszVerb,
                         osFilename.c_str(),
                         requestHelper.sWriteFuncData.pBuffer
                             ? requestHelper.sWriteFuncData.pBuffer
                             : "(null)");
                bSuccess = false;
            }
        }

        curl_easy_cleanup(hCurlHandle);
    } while (bRetry);

    return bSuccess;
}

/************************************************************************/
/*                           GetFileList()                              */
/************************************************************************/

char **VSIADLSFSHandler::GetFileList(const char *pszDirname, int nMaxFiles,
                                     bool *pbGotFileList)
{
    return GetFileList(pszDirname, nMaxFiles, true, pbGotFileList);
}

char **VSIADLSFSHandler::GetFileList(const char *pszDirname, int nMaxFiles,
                                     bool bCacheEntries, bool *pbGotFileList)
{
    if (ENABLE_DEBUG)
        CPLDebug(GetDebugKey(), "GetFileList(%s)", pszDirname);

    *pbGotFileList = false;

    char **papszOptions =
        CSLSetNameValue(nullptr, "MAXFILES", CPLSPrintf("%d", nMaxFiles));
    papszOptions = CSLSetNameValue(papszOptions, "CACHE_ENTRIES",
                                   bCacheEntries ? "YES" : "NO");
    auto dir = OpenDir(pszDirname, 0, papszOptions);
    CSLDestroy(papszOptions);
    if (!dir)
    {
        return nullptr;
    }
    CPLStringList aosFileList;
    while (true)
    {
        auto entry = dir->NextDirEntry();
        if (!entry)
        {
            break;
        }
        aosFileList.AddString(entry->pszName);

        if (nMaxFiles > 0 && aosFileList.size() >= nMaxFiles)
            break;
    }
    delete dir;
    *pbGotFileList = true;
    return aosFileList.StealList();
}

/************************************************************************/
/*                           GetOptions()                               */
/************************************************************************/

const char *VSIADLSFSHandler::GetOptions()
{
    static std::string osOptions(
        std::string("<Options>") +
        "  <Option name='AZURE_STORAGE_CONNECTION_STRING' type='string' "
        "description='Connection string that contains account name and "
        "secret key'/>"
        "  <Option name='AZURE_STORAGE_ACCOUNT' type='string' "
        "description='Storage account. To use with AZURE_STORAGE_ACCESS_KEY'/>"
        "  <Option name='AZURE_STORAGE_ACCESS_KEY' type='string' "
        "description='Secret key'/>"
        "  <Option name='VSIAZ_CHUNK_SIZE' type='int' "
        "description='Size in MB for chunks of files that are uploaded' "
        "default='4' min='1' max='4'/>" +
        VSICurlFilesystemHandlerBase::GetOptionsStatic() + "</Options>");
    return osOptions.c_str();
}

/************************************************************************/
/*                           GetSignedURL()                             */
/************************************************************************/

char *VSIADLSFSHandler::GetSignedURL(const char *pszFilename,
                                     CSLConstList papszOptions)
{
    if (!STARTS_WITH_CI(pszFilename, GetFSPrefix().c_str()))
        return nullptr;

    auto poHandleHelper = std::unique_ptr<VSIAzureBlobHandleHelper>(
        VSIAzureBlobHandleHelper::BuildFromURI(pszFilename +
                                                   GetFSPrefix().size(),
                                               "/vsiaz/",  // use Azure blob
                                               nullptr, papszOptions));
    if (poHandleHelper == nullptr)
    {
        return nullptr;
    }

    std::string osRet(poHandleHelper->GetSignedURL(papszOptions));

    return CPLStrdup(osRet.c_str());
}

/************************************************************************/
/*                            OpenDir()                                 */
/************************************************************************/

VSIDIR *VSIADLSFSHandler::OpenDir(const char *pszPath, int nRecurseDepth,
                                  const char *const *papszOptions)
{
    if (nRecurseDepth > 0)
    {
        return VSIFilesystemHandler::OpenDir(pszPath, nRecurseDepth,
                                             papszOptions);
    }

    if (!STARTS_WITH_CI(pszPath, GetFSPrefix().c_str()))
        return nullptr;

    NetworkStatisticsFileSystem oContextFS(GetFSPrefix().c_str());
    NetworkStatisticsAction oContextAction("OpenDir");

    const std::string osDirnameWithoutPrefix =
        RemoveTrailingSlash(pszPath + GetFSPrefix().size());
    std::string osFilesystem(osDirnameWithoutPrefix);
    std::string osObjectKey;
    size_t nSlashPos = osDirnameWithoutPrefix.find('/');
    if (nSlashPos != std::string::npos)
    {
        osFilesystem = osDirnameWithoutPrefix.substr(0, nSlashPos);
        osObjectKey = osDirnameWithoutPrefix.substr(nSlashPos + 1);
    }

    VSIDIRADLS *dir = new VSIDIRADLS(this);
    dir->m_nRecurseDepth = nRecurseDepth;
    dir->m_poFS = this;
    dir->m_bRecursiveRequestFromAccountRoot =
        osFilesystem.empty() && nRecurseDepth < 0;
    dir->m_osFilesystem = std::move(osFilesystem);
    dir->m_osObjectKey = std::move(osObjectKey);
    dir->m_nMaxFiles =
        atoi(CSLFetchNameValueDef(papszOptions, "MAXFILES", "0"));
    dir->m_bCacheEntries =
        CPLTestBool(CSLFetchNameValueDef(papszOptions, "CACHE_ENTRIES", "YES"));
    dir->m_osFilterPrefix = CSLFetchNameValueDef(papszOptions, "PREFIX", "");
    if (!dir->IssueListDir())
    {
        delete dir;
        return nullptr;
    }

    return dir;
}

/************************************************************************/
/*                      GetStreamingFilename()                          */
/************************************************************************/

std::string
VSIADLSFSHandler::GetStreamingFilename(const std::string &osFilename) const
{
    if (STARTS_WITH(osFilename.c_str(), GetFSPrefix().c_str()))
        return "/vsiaz_streaming/" + osFilename.substr(GetFSPrefix().size());
    return osFilename;
}

/************************************************************************/
/*                           VSIADLSHandle()                           */
/************************************************************************/

VSIADLSHandle::VSIADLSHandle(VSIADLSFSHandler *poFSIn, const char *pszFilename,
                             VSIAzureBlobHandleHelper *poHandleHelper)
    : VSICurlHandle(poFSIn, pszFilename, poHandleHelper->GetURLNoKVP().c_str()),
      m_poHandleHelper(poHandleHelper)
{
    m_osQueryString = poHandleHelper->GetSASQueryString();
}

/************************************************************************/
/*                          GetCurlHeaders()                            */
/************************************************************************/

struct curl_slist *VSIADLSHandle::GetCurlHeaders(const std::string &osVerb,
                                                 struct curl_slist *psHeaders)
{
    return m_poHandleHelper->GetCurlHeaders(osVerb, psHeaders);
}

/************************************************************************/
/*                          CanRestartOnError()                         */
/************************************************************************/

bool VSIADLSHandle::CanRestartOnError(const char *pszErrorMsg,
                                      const char *pszHeaders, bool bSetError)
{
    return m_poHandleHelper->CanRestartOnError(pszErrorMsg, pszHeaders,
                                               bSetError);
}

} /* end of namespace cpl */

#endif  // DOXYGEN_SKIP
//! @endcond

/************************************************************************/
/*                      VSIInstallADLSFileHandler()                    */
/************************************************************************/

/*!
 \brief Install /vsiaz/ Microsoft Azure Data Lake Storage Gen2 file system
 handler (requires libcurl)

 \verbatim embed:rst
 See :ref:`/vsiadls/ documentation <vsiadls>`
 \endverbatim

 @since GDAL 3.3
 */

void VSIInstallADLSFileHandler(void)
{
    VSIFileManager::InstallHandler("/vsiadls/", new cpl::VSIADLSFSHandler);
}

#endif /* HAVE_CURL */
