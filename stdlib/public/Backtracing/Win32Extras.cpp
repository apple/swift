//===--- Win32Extras.cpp - Windows support functions ------------*- C++ -*-===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2023 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//
//
//  Defines some extra functions that aren't available in the OS or C library
//  on Windows.
//
//===----------------------------------------------------------------------===//

#ifdef _WIN32

#include <windows.h>

#include "modules/OS/Libc.h"

extern "C" ssize_t pread(int fd, void *buf, size_t nbyte, off_t offset) {
  HANDLE hFile = _get_osfhandle(fd);
  OVERLAPPED ovl = {0};
  DWORD dwBytesRead = 0;

  ovl.Offset = (DWORD)offset;
  ovl.OffsetHigh = (DWORD)(offset >> 32);

  if (!ReadFile(hFile, buf, (DWORD)count, &dwBytesRead, &ovl)) {
    errno = EIO;
    return -1;
  }

  return dwBytesRead;
}

extern "C" ssize_t pwrite(int fd, const void *buf, size_t nbyte, off_t offset) {
  HANDLE hFile = _get_osfhandle(fd);
  OVERLAPPED ovl = {0};
  DWORD dwBytesRead = 0;

  ovl.Offset = (DWORD)offset;
  ovl.OffsetHigh = (DWORD)(offset >> 32);

  if (!WriteFile(hFile, buf, (DWORD)count, &dwBytesRead, &ovl)) {
    errno = EIO;
    return -1;
  }

  return dwBytesRead;
}

#endif // _WIN32

