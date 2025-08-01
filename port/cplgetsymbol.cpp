/******************************************************************************
 *
 * Project:  Common Portability Library
 * Purpose:  Fetch a function pointer from a shared library / DLL.
 * Author:   Frank Warmerdam, warmerdam@pobox.com
 *
 ******************************************************************************
 * Copyright (c) 1999, Frank Warmerdam
 * Copyright (c) 2009-2013, Even Rouault <even dot rouault at spatialys.com>
 *
 * SPDX-License-Identifier: MIT
 ****************************************************************************/

#include "cpl_port.h"
#include "cpl_conv.h"

#include <cstddef>

#include "cpl_config.h"
#include "cpl_error.h"
#include "cpl_string.h"

/* ==================================================================== */
/*                  Unix Implementation                                 */
/* ==================================================================== */

/* MinGW32 might define HAVE_DLFCN_H, so skip the unix implementation */
#if defined(HAVE_DLFCN_H) && !defined(_WIN32)

#define GOT_GETSYMBOL

#include <dlfcn.h>

#include <map>
#include <mutex>

/************************************************************************/
/*                            CPLGetSymbol()                            */
/************************************************************************/

/**
 * Fetch a function pointer from a shared library / DLL.
 *
 * This function is meant to abstract access to shared libraries and
 * DLLs and performs functions similar to dlopen()/dlsym() on Unix and
 * LoadLibrary() / GetProcAddress() on Windows.
 *
 * If no support for loading entry points from a shared library is available
 * this function will always return NULL.   Rules on when this function
 * issues a CPLError() or not are not currently well defined, and will have
 * to be resolved in the future.
 *
 * Currently CPLGetSymbol() doesn't try to:
 * <ul>
 *  <li> prevent the reference count on the library from going up
 *    for every request, or given any opportunity to unload
 *    the library.
 *  <li> Attempt to look for the library in non-standard
 *    locations.
 *  <li> Attempt to try variations on the symbol name, like
 *    pre-pending or post-pending an underscore.
 * </ul>
 *
 * Some of these issues may be worked on in the future.
 *
 * @param pszLibrary the name of the shared library or DLL containing
 * the function.  May contain path to file.  If not system supplies search
 * paths will be used.
 * @param pszSymbolName the name of the function to fetch a pointer to.
 * @return A pointer to the function if found, or NULL if the function isn't
 * found, or the shared library can't be loaded.
 */

void *CPLGetSymbol(const char *pszLibrary, const char *pszSymbolName)

{
    static std::mutex mutex;
    static std::map<std::string, void *> mapLibraryNameToHandle;
    std::lock_guard oLock(mutex);

    void *pLibrary;
    const char *pszNonNullLibrary = pszLibrary ? pszLibrary : "";
    auto oIter = mapLibraryNameToHandle.find(pszNonNullLibrary);
    if (oIter == mapLibraryNameToHandle.end())
    {
        pLibrary = dlopen(pszLibrary, RTLD_LAZY);
        if (pLibrary == nullptr)
        {
            CPLError(CE_Failure, CPLE_AppDefined, "%s", dlerror());
            return nullptr;
        }
        mapLibraryNameToHandle[pszNonNullLibrary] = pLibrary;
    }
    else
    {
        pLibrary = oIter->second;
    }

    void *pSymbol = dlsym(pLibrary, pszSymbolName);

#if (defined(__APPLE__) && defined(__MACH__))
    /* On mach-o systems, C symbols have a leading underscore and depending
     * on how dlcompat is configured it may or may not add the leading
     * underscore.  If dlsym() fails, add an underscore and try again.
     */
    if (pSymbol == nullptr)
    {
        char withUnder[256] = {};
        snprintf(withUnder, sizeof(withUnder), "_%s", pszSymbolName);
        pSymbol = dlsym(pLibrary, withUnder);
    }
#endif

    if (pSymbol == nullptr)
    {
        CPLError(CE_Failure, CPLE_AppDefined, "%s", dlerror());
        return nullptr;
    }

    return (pSymbol);
}

#endif /* def __unix__ && defined(HAVE_DLFCN_H) */

/* ==================================================================== */
/*                 Windows Implementation                               */
/* ==================================================================== */
#if defined(_WIN32)

#define GOT_GETSYMBOL

#include <windows.h>

/************************************************************************/
/*                            CPLGetSymbol()                            */
/************************************************************************/

void *CPLGetSymbol(const char *pszLibrary, const char *pszSymbolName)

{
    void *pLibrary = nullptr;
    void *pSymbol = nullptr;

    // Avoid error boxes to pop up (#5211, #5525).
    UINT uOldErrorMode =
        SetErrorMode(SEM_NOOPENFILEERRORBOX | SEM_FAILCRITICALERRORS);

#if defined(_MSC_VER) || __MSVCRT_VERSION__ >= 0x0601
    if (CPLTestBool(CPLGetConfigOption("GDAL_FILENAME_IS_UTF8", "YES")))
    {
        wchar_t *pwszFilename =
            CPLRecodeToWChar(pszLibrary, CPL_ENC_UTF8, CPL_ENC_UCS2);
        pLibrary = LoadLibraryW(pwszFilename);
        CPLFree(pwszFilename);
    }
    else
#endif
    {
        pLibrary = LoadLibraryA(pszLibrary);
    }

    if (pLibrary <= (void *)HINSTANCE_ERROR)
    {
        LPVOID lpMsgBuf = nullptr;
        int nLastError = GetLastError();

        // Restore old error mode.
        SetErrorMode(uOldErrorMode);

        FormatMessage(
            FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                FORMAT_MESSAGE_IGNORE_INSERTS,
            nullptr, nLastError, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
            reinterpret_cast<LPTSTR>(&lpMsgBuf), 0, nullptr);

        CPLError(CE_Failure, CPLE_AppDefined,
                 "Can't load requested DLL: %s\n%d: %s", pszLibrary, nLastError,
                 static_cast<const char *>(lpMsgBuf));
        return nullptr;
    }

    // Restore old error mode.
    SetErrorMode(uOldErrorMode);

    pSymbol = reinterpret_cast<void *>(
        GetProcAddress(static_cast<HINSTANCE>(pLibrary), pszSymbolName));

    if (pSymbol == nullptr)
    {
        CPLError(CE_Failure, CPLE_AppDefined,
                 "Can't find requested entry point: %s", pszSymbolName);
        return nullptr;
    }

    return (pSymbol);
}

#endif  // def _WIN32

/* ==================================================================== */
/*      Dummy implementation.                                           */
/* ==================================================================== */

#ifndef GOT_GETSYMBOL

/************************************************************************/
/*                            CPLGetSymbol()                            */
/*                                                                      */
/*      Dummy implementation.                                           */
/************************************************************************/

void *CPLGetSymbol(const char *pszLibrary, const char *pszEntryPoint)

{
    CPLDebug("CPL",
             "CPLGetSymbol(%s,%s) called.  Failed as this is stub"
             " implementation.",
             pszLibrary, pszEntryPoint);
    return nullptr;
}
#endif
