//===--- ImageInspectionWin32.cpp - Win32 image inspection ----------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2017 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

#if !defined(__ELF__) && !defined(__MACH__) && !defined(__wasm__)

#include "ImageInspection.h"

#if defined(__CYGWIN__)
#include <dlfcn.h>
#elif defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <DbgHelp.h>
#endif

using namespace swift;

int swift::lookupSymbol(const void *address, SymbolInfo *info) {
#if defined(__CYGWIN__)
  Dl_info dlinfo;
  if (dladdr(address, &dlinfo) == 0) {
    return 0;
  }

  info->fileName = dlinfo.dli_fname;
  info->baseAddress = dlinfo.dli_fbase;
  info->symbolName = dli_info.dli_sname;
  info->symbolAddress = dli_saddr;
  return 1;
#elif defined(_WIN32)
  static const constexpr size_t kSymbolMaxNameLen = 1024;
  static bool bInitialized = false;

  if (bInitialized == false) {
    if (SymInitialize(GetCurrentProcess(), /*UserSearchPath=*/NULL,
                      /*fInvadeProcess=*/TRUE) == FALSE)
      return 0;
    bInitialized = true;
  }

  char buffer[sizeof(SYMBOL_INFO) + kSymbolMaxNameLen];
  PSYMBOL_INFO pSymbol = reinterpret_cast<PSYMBOL_INFO>(buffer);
  pSymbol->SizeOfStruct = sizeof(SYMBOL_INFO);
  pSymbol->MaxNameLen = kSymbolMaxNameLen;

  DWORD64 dwDisplacement = 0;

  if (SymFromAddr(GetCurrentProcess(), reinterpret_cast<const DWORD64>(address),
                  &dwDisplacement, pSymbol) == FALSE)
    return 0;

  info->fileName = NULL;
  info->baseAddress = reinterpret_cast<void *>(pSymbol->ModBase);
  info->symbolName.reset(_strdup(pSymbol->Name));
  info->symbolAddress = reinterpret_cast<void *>(pSymbol->Address);

  return 1;
#else
  return 0;
#endif // defined(__CYGWIN__) || defined(_WIN32)
}

#endif // !defined(__ELF__) && !defined(__MACH__)
