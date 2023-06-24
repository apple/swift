//===--- Paths.cpp - Swift Runtime path utility functions -------*- C++ -*-===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2022 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//
//
// Functions that obtain paths that might be useful within the runtime.
//
//===----------------------------------------------------------------------===//

#include "swift/Runtime/Config.h"
#include "swift/Runtime/EnvironmentVariables.h"
#include "swift/Runtime/Debug.h"
#include "swift/Runtime/Paths.h"
#include "swift/Runtime/Win32.h"
#include "swift/Threading/Once.h"

#ifdef __APPLE__
#include <TargetConditionals.h>
#endif

#if !defined(_WIN32) || defined(__CYGWIN__)
#if __has_include(<sys/stat.h>)
#  include <sys/stat.h>

#  ifdef S_IFMT
#    define HAS_STAT 1
#  else
#    define HAS_STAT 0
#  endif
#else
#  define HAS_STAT 0
#endif

#if SWIFT_STDLIB_HAS_DLADDR
 #include <dlfcn.h>
#endif

#else
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <psapi.h>
#endif

#include <cerrno>
#include <cstdlib>
#include <cstring>

#if !SWIFT_STDLIB_HAS_FILESYSTEM

SWIFT_RUNTIME_EXPORT
const char *
swift_getRuntimeLibraryPath() {
  return nullptr;
}

SWIFT_RUNTIME_EXPORT
const char *
swift_getRootPath() {
  return nullptr;
}

SWIFT_RUNTIME_EXPORT
char *
swift_copyAuxiliaryExecutablePath(const char *name) {
  return nullptr;
}

#else // SWIFT_STDLIB_HAS_FILESYSTEM

namespace {

swift::once_t runtimePathToken;
const char *runtimePath;

swift::once_t rootPathToken;
const char *rootPath;

void _swift_initRuntimePath(void *);
void _swift_initRootPath(void *);
const char *_swift_getDefaultRootPath();
char *_swift_getAuxExePathIn(const char *path, const char *name);
char *_swift_tryAuxExePath(const char *name, const char *path, ...);

bool _swift_isPathSep(char ch) {
#ifdef _WIN32
  return ch == '/' || ch == '\\';
#else
  return ch == '/';
#endif
}

bool _swift_exists(const char *path);

#if !defined(_WIN32) || defined(__CYGWIN__)
#define PATHSEP_STR "/"
#define PATHSEP_CHR '/'
#else
#define PATHSEP_STR "\\"
#define PATHSEP_CHR '\\'
#endif

}

SWIFT_RUNTIME_EXPORT
const char *
swift_getRuntimeLibraryPath()
{
  swift::once(runtimePathToken, _swift_initRuntimePath, nullptr);
  return runtimePath;
}

SWIFT_RUNTIME_EXPORT
const char *
swift_getRootPath()
{
  swift::once(rootPathToken, _swift_initRootPath, nullptr);
  return rootPath;
}

namespace {

bool
_swift_lookingAtLibSwift(const char *ptr, const char *base)
{
  // /some/path/to/some/thing/lib/swift/libswiftCore.dylib
  //                         ^         ^
  //                         |         +---- ptr
  //                         +-------------- ptr - 10

  return (ptr - base >= 10
          && _swift_isPathSep(ptr[-10])
          && std::strncmp(ptr - 9, "lib", 3) == 0
          && _swift_isPathSep(ptr[-6])
          && std::strncmp(ptr - 5, "swift", 5) == 0);
}

bool
_swift_lookingAtBin(const char *ptr, const char *base)
{
  // C:\some\path\to\some\thing\bin\libswiftCore.dylib
  //                           ^   ^
  //                           |   +---- ptr
  //                           +-------- ptr - 4

  return (ptr - base > 4
          && _swift_isPathSep(ptr[-4])
          && std::strncmp(ptr - 3, "bin", 3) == 0);
}

const char *
_swift_getDefaultRootPath()
{
  const char *runtimePath = swift_getRuntimeLibraryPath();

  if (!runtimePath)
    return nullptr;

  size_t runtimePathLen = std::strlen(runtimePath);

  // Scan backwards until we find a path separator
  const char *ptr = runtimePath + runtimePathLen;
  while (ptr > runtimePath && !_swift_isPathSep(*--ptr));

  if (_swift_lookingAtLibSwift(ptr, runtimePath)) {
    // /some/path/to/some/thing/lib/swift/libswiftCore.dylib
    //                         ^         ^
    //                         |         +---- ptr
    //                         +-------------- ptr - 10
    ptr -= 10;
  } else {
    // We *might* be in a <platform> or <platform>/<arch> directory, so scan
    // backwards for that too
    bool found = false;
    const char *platform = ptr;

    for (unsigned n = 0; n < 2; ++n) {
      while (platform > runtimePath && !_swift_isPathSep(*--platform));

      if (_swift_lookingAtLibSwift(platform, runtimePath)) {

        // When we get here, we have:
        //
        //      /some/path/to/some/thing/lib/swift/macosx/libswiftCore.dylib
        //                              ^         ^      ^
        //                              |         |      +---- ptr
        //                              |         +----------- platform
        //                              +--------------------- platform - 10

        ptr = platform - 10;
        found = true;
        break;
      }
    }

    if (!found) {
      // We *might* also be in a bin directory, for instance on Windows, so
      // check if we should remove that also.
      if (_swift_lookingAtBin(ptr, runtimePath)) {
        // C:\some\path\to\some\thing\bin\libswiftCore.dylib
        //                           ^   ^
        //                           |   +---- ptr
        //                           +-------- ptr - 4
        ptr -= 4;
      }
    }
  }

  // If the result is empty, return "./" or ".\\"
  if (ptr == runtimePath) {
    return "." PATHSEP_STR;
  }

  // Duplicate the string up to and including ptr
  size_t len = ptr - runtimePath + 1;
  char *thePath = (char *)malloc(len + 1);
  std::memcpy(thePath, runtimePath, len);
  thePath[len] = 0;

  return thePath;
}

// Join paths together
char *
_swift_joinPathsV(const char *path, va_list val)
{
  va_list val2;
  size_t baseLen = 0;
  size_t totalLen = 0;
  const char *pathSeg;

  va_copy(val2, val);

  baseLen = std::strlen(path);
  while (baseLen && _swift_isPathSep(path[baseLen - 1]))
    --baseLen;

  if (!baseLen)
    totalLen = 1;
  else
    totalLen = baseLen;

  while ((pathSeg = va_arg(val2, const char *))) {
    size_t len = std::strlen(pathSeg);
    while (len && _swift_isPathSep(pathSeg[len - 1]))
      --len;
    if (len)
      totalLen += 1 + len;
  }

  char *buffer = static_cast<char *>(std::malloc(totalLen + 1));
  char *ptr = buffer;

  if (!baseLen)
    *ptr++ = PATHSEP_CHR;
  else {
    std::memcpy(ptr, path, baseLen);
    ptr += baseLen;
  }

  while ((pathSeg = va_arg(val, const char *))) {
    size_t len = std::strlen(pathSeg);
    while (len && _swift_isPathSep(pathSeg[len - 1]))
      --len;
    if (len) {
      *ptr++ = PATHSEP_CHR;
      std::memcpy(ptr, pathSeg, len);
      ptr += len;
    }
  }
  buffer[totalLen] = 0;

  return buffer;
}

char *
_swift_joinPaths(const char *path, ...)
{
  va_list val;
  char *result;

  va_start(val, path);
  result = _swift_joinPathsV(path, val);
  va_end(val);

  return result;
}

void
_swift_initRootPath(void *)
{
  // SWIFT_ROOT overrides the path returned by this function
  const char *swiftRoot = swift::runtime::environment::SWIFT_ROOT();

  if (!swiftRoot || !*swiftRoot) {
    rootPath = _swift_getDefaultRootPath();
    return;
  }

  size_t len = std::strlen(swiftRoot);

  // Ensure that there's a trailing slash
  if (_swift_isPathSep(swiftRoot[len - 1])) {
    rootPath = swiftRoot;
  } else {
    char *thePath = (char *)malloc(len + 2);
    std::memcpy(thePath, swiftRoot, len);
    thePath[len] = PATHSEP_CHR;
    thePath[len + 1] = 0;

    rootPath = thePath;
  }
}

#if _WIN32
/// Map an NT-style filename to a Win32 filename.
///
/// We can't use GetFinalPathNameByHandle() because there's no way to obtain
/// a handle (at least, not without using the internal NtCreateFile() API, which
/// we aren't supposed to be using).  Additionally, that function would resolve
/// symlinks, which we don't want to do here.
///
/// As a result, we use the approach demonstrated here:
///
///  https://learn.microsoft.com/en-us/windows/win32/memory/obtaining-a-file-name-from-a-file-handle
///
/// @param pszFilename The NT-style filename to convert.
///
/// @result A string, allocated using std::malloc(), containing the Win32-style
///         filename.
LPWSTR
_swift_win32NameFromNTName(LPWSTR pszFilename) {
  DWORD dwLen = GetLogicalDriveStringsW(0, NULL);
  if (!dwLen)
    return NULL;

  LPWSTR lpDriveStrings = (LPWSTR)std::malloc(dwLen * sizeof(WCHAR));
  if (!lpDriveStrings)
    return NULL;

  DWORD dwRet = GetLogicalDriveStringsW(dwLen, lpDriveStrings);
  if (!dwRet)
    return NULL;

  LPWSTR pszDrive = lpDriveStrings;
  while (*pszDrive) {
    size_t len = wcslen(pszDrive);
    if (len && pszDrive[len - 1] == '\\')
      pszDrive[len - 1] = 0;

    WCHAR ntPath[4096];
    dwRet = QueryDosDeviceW(pszDrive, ntPath, 4096);
    if (dwRet) {
      size_t ntLen = wcslen(ntPath);

      if (_wcsnicmp(pszFilename, ntPath, ntLen) == 0
          && pszFilename[ntLen] == '\\') {
        size_t fnLen = wcslen(pszFilename);
        size_t driveLen = wcslen(pszDrive);
        size_t pathLen = fnLen - ntLen;
        size_t newLen = driveLen + pathLen + 1;
        LPWSTR pszWin32Name = (LPWSTR)std::malloc(newLen * sizeof(WCHAR));
        if (!pszWin32Name) {
          std::free(lpDriveStrings);
          return NULL;
        }

        LPWSTR ptr = pszWin32Name;
        memcpy(ptr, pszDrive, driveLen * sizeof(WCHAR));
        ptr += driveLen;
        memcpy(ptr, pszFilename + ntLen, pathLen * sizeof(WCHAR));
        ptr += pathLen;
        *ptr = 0;

        std::free(lpDriveStrings);

        return pszWin32Name;
      }
    }

    pszDrive += len + 1;
  }

  std::free(lpDriveStrings);

  return _wcsdup(pszFilename);
}
#endif

} // namespace

SWIFT_RUNTIME_EXPORT
char *
swift_copyAuxiliaryExecutablePath(const char *name)
{
  const char *rootPath = swift_getRootPath();

  if (!rootPath)
    return nullptr;

  const char *platformName = SWIFT_LIB_SUBDIR;
  const char *archName = SWIFT_ARCH;

  // <rootPath>/libexec/swift/<platformName>
  if (char *result = _swift_tryAuxExePath(name,
                                          rootPath,
                                          "libexec", "swift",
                                          platformName, nullptr)) {
    return result;
  }

  // <rootPath>/libexec/swift/<platformName>/<arch>
  if (char *result = _swift_tryAuxExePath(name,
                                          rootPath,
                                          "libexec", "swift",
                                          platformName,
                                          archName, nullptr)) {
    return result;
  }

  // <rootPath>/libexec/swift
  if (char *result = _swift_tryAuxExePath(name,
                                          rootPath,
                                          "libexec", "swift",
                                          nullptr)) {
    return result;
  }

  // <rootPath>/libexec/swift/<arch>
  if (char *result = _swift_tryAuxExePath(name,
                                          rootPath,
                                          "libexec", "swift",
                                          archName, nullptr)) {
    return result;
  }

  // <rootPath>/bin
  if (char *result = _swift_tryAuxExePath(name,
                                          rootPath,
                                          "bin", nullptr)) {
    return result;
  }

  // <rootPath>/bin/<arch>
  if (char *result = _swift_tryAuxExePath(name,
                                          rootPath,
                                          "bin",
                                          archName, nullptr)) {
    return result;
  }

  // Otherwise, look in the root itself
  return _swift_tryAuxExePath(name, rootPath, nullptr);
}

namespace {

char *
_swift_getAuxExePathIn(const char *path, const char *name)
{
#ifdef _WIN32
  size_t nameLen = std::strlen(name);
  char *nameWithSuffix = nullptr;
  if (nameLen > 4 && strcmp(name + nameLen - 4, ".exe") != 0) {
    nameWithSuffix = (char *)std::malloc(nameLen + 4 + 1);
    std::memcpy(nameWithSuffix, name, nameLen);
    std::memcpy(nameWithSuffix + nameLen, ".exe", 4 + 1);

    name = nameWithSuffix;
  }
#endif

  char *fullPath = _swift_joinPaths(path, name, nullptr);

#ifdef _WIN32
  if (nameWithSuffix)
    std::free(nameWithSuffix);
#endif

  return fullPath;
}

char *
_swift_tryAuxExePath(const char *name, const char *path, ...)
{
  va_list val;
  char *fullPath;
  va_start(val, path);
  fullPath = _swift_joinPathsV(path, val);
  va_end(val);

  if (_swift_exists(fullPath)) {
    char *result = _swift_getAuxExePathIn(fullPath, name);

    if (_swift_exists(result)) {
      std::free(fullPath);

      return result;
    }

    std::free(result);
  }

  std::free(fullPath);

  return nullptr;
}

#if !defined(_WIN32) || defined(__CYGWIN__)
void
_swift_initRuntimePath(void *) {
#if APPLE_OS_SYSTEM
  const char *path = dyld_image_path_containing_address(_swift_initRuntimePath);

  runtimePath = ::strdup(path);
#elif SWIFT_STDLIB_HAS_DLADDR
  Dl_info dli;
  int ret = ::dladdr((void *)_swift_initRuntimePath, &dli);

  if (!ret) {
    swift::fatalError(/* flags = */ 0,
                      "Unable to obtain Swift runtime path\n");
  }

  runtimePath = ::strdup(dli.dli_fname);
#else
  runtimePath = nullptr;
#endif
}
#else

void
_swift_initRuntimePath(void *) {
  const DWORD dwBufSize = 4096;
  LPWSTR lpFilename = (LPWSTR)std::malloc(dwBufSize * sizeof(WCHAR));

  // Again, we can't use GetFinalPathNameByHandle for the reasons given
  // above.

  DWORD dwRet = GetMappedFileNameW(GetCurrentProcess(),
                                   (void *)_swift_initRuntimePath,
                                   lpFilename,
                                   dwBufSize);
  if (!dwRet) {
    swift::fatalError(/* flags = */ 0,
                      "Unable to obtain Swift runtime path\n");
  }

  // GetMappedFileNameW() returns an NT-style path, not a Win32 path; that is,
  // it starts with \Device\DeviceName rather than a drive letter.
  LPWSTR lpWin32Filename = _swift_win32NameFromNTName(lpFilename);
  if (!lpWin32Filename) {
    swift::fatalError(/* flags = */ 0,
                      "Unable to obtain Win32 path for Swift runtime\n");
  }

  std::free(lpFilename);

  runtimePath = _swift_win32_copyUTF8FromWide(lpWin32Filename);
  if (!runtimePath) {
    swift::fatalError(/* flags = */ 0,
                      "Unable to convert Swift runtime path to UTF-8: %lx, %d\n",
                      ::GetLastError(), errno);
  }

  std::free(lpWin32Filename);
}
#endif

/// Return true if a file exists at path.
///
/// On Windows, path will be in UTF-8 so can't be passed to _stat() or any
/// of the ANSI functions.
///
/// @param path The path to check
///
/// @result true iff there is a file at @a path
bool _swift_exists(const char *path)
{
#if !defined(_WIN32)
#  if !HAS_STAT
  return false;
#  else
  struct stat st;
  return stat(path, &st) == 0;
#  endif
#else
  wchar_t *wszPath = _swift_win32_copyWideFromUTF8(path);
  bool result = GetFileAttributesW(wszPath) != INVALID_FILE_ATTRIBUTES;
  free(wszPath);
  return result;
#endif // defined(_WIN32)
}

}

#endif // SWIFT_STDLIB_HAS_FILESYSTEM
