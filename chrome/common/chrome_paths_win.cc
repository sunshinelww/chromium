// Copyright (c) 2006-2008 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/common/chrome_paths_internal.h"

#include <windows.h>
#include <knownfolders.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>

#include "app/win_util.h"
#include "base/file_path.h"
#include "base/path_service.h"
#include "base/scoped_comptr_win.h"
#include "chrome/common/chrome_constants.h"

namespace chrome {

bool GetDefaultUserDataDirectory(FilePath* result) {
  if (!PathService::Get(base::DIR_LOCAL_APP_DATA, result))
    return false;
#if defined(GOOGLE_CHROME_BUILD)
  *result = result->Append(FILE_PATH_LITERAL("Google"));
#endif
  *result = result->Append(chrome::kBrowserAppName);
  *result = result->Append(chrome::kUserDataDirname);
  return true;
}

bool GetChromeFrameUserDataDirectory(FilePath* result) {
  if (!PathService::Get(base::DIR_LOCAL_APP_DATA, result))
    return false;
#if defined(GOOGLE_CHROME_BUILD)
  *result = result->Append(FILE_PATH_LITERAL("Google"));
#endif
  *result = result->Append(L"Chrome Frame");
  *result = result->Append(chrome::kUserDataDirname);
  return true;
}

bool GetUserDocumentsDirectory(FilePath* result) {
  wchar_t path_buf[MAX_PATH];
  if (FAILED(SHGetFolderPath(NULL, CSIDL_MYDOCUMENTS, NULL,
                             SHGFP_TYPE_CURRENT, path_buf)))
    return false;
  *result = FilePath(path_buf);
  return true;
}

// Return a default path for downloads that is safe.
// We just use 'Downloads' under DIR_USER_DOCUMENTS. Localizing
// 'downloads' is not a good idea because Chrome's UI language
// can be changed.
bool GetUserDownloadsDirectorySafe(FilePath* result) {
  if (!GetUserDocumentsDirectory(result))
    return false;

  *result = result->Append(L"Downloads");
  return true;
}

// On Vista and higher, use the downloads known folder. Since it can be
// relocated to point to a "dangerous" folder, callers should validate that the
// returned path is not dangerous before using it.
bool GetUserDownloadsDirectory(FilePath* result) {
  typedef HRESULT (WINAPI *GetKnownFolderPath)(
      REFKNOWNFOLDERID, DWORD, HANDLE, PWSTR*);
  GetKnownFolderPath f = reinterpret_cast<GetKnownFolderPath>(
      GetProcAddress(GetModuleHandle(L"shell32.dll"), "SHGetKnownFolderPath"));
  win_util::CoMemReleaser<wchar_t> path_buf;
  if (f && SUCCEEDED(f(FOLDERID_Downloads, 0, NULL, &path_buf))) {
    *result = FilePath(std::wstring(path_buf));
    return true;
  }
  return GetUserDownloadsDirectorySafe(result);
}

bool GetUserDesktop(FilePath* result) {
  // We need to go compute the value. It would be nice to support paths
  // with names longer than MAX_PATH, but the system functions don't seem
  // to be designed for it either, with the exception of GetTempPath
  // (but other things will surely break if the temp path is too long,
  // so we don't bother handling it.
  wchar_t system_buffer[MAX_PATH];
  system_buffer[0] = 0;
  if (FAILED(SHGetFolderPath(NULL, CSIDL_DESKTOPDIRECTORY, NULL,
                             SHGFP_TYPE_CURRENT, system_buffer)))
    return false;
  *result = FilePath(system_buffer);
  return true;
}

}  // namespace chrome
