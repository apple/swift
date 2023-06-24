//===---- AccessibleFunction.cpp - Swift protocol conformance checking ----===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2021 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//
//
// Checking and caching of Swift accessible functions.
//
//===----------------------------------------------------------------------===//

#include "ImageInspection.h"
#include "Private.h"
#include "swift/Basic/Lazy.h"
#include "swift/Demangling/Demangler.h"
#include "swift/Runtime/AccessibleFunction.h"
#include "swift/Runtime/Concurrent.h"
#include "swift/Runtime/Metadata.h"
#include "Tracing.h"

#include <cstdint>
#include <new>

using namespace swift;

#pragma mark Accessible function cache
namespace {

struct AccessibleFunctionsSection {
  const AccessibleFunctionRecord *__ptrauth_swift_accessible_function_record
      Begin;
  const AccessibleFunctionRecord *__ptrauth_swift_accessible_function_record
      End;

  AccessibleFunctionsSection(const AccessibleFunctionRecord *begin,
                             const AccessibleFunctionRecord *end)
      : Begin(begin), End(end) {}

  AccessibleFunctionsSection(const void *ptr, uintptr_t size) {
    auto bytes = reinterpret_cast<const char *>(ptr);
    Begin = reinterpret_cast<const AccessibleFunctionRecord *>(ptr);
    End = reinterpret_cast<const AccessibleFunctionRecord *>(bytes + size);
  }

  const AccessibleFunctionRecord *begin() const { return Begin; }
  const AccessibleFunctionRecord *end() const { return End; }
};

struct AccessibleFunctionCacheEntry {
private:
  const char *Name;
  size_t NameLength;

  const AccessibleFunctionRecord *__ptrauth_swift_accessible_function_record R;

public:
  AccessibleFunctionCacheEntry(llvm::StringRef name,
                               const AccessibleFunctionRecord *record)
      : R(record) {
    char *Name = reinterpret_cast<char *>(malloc(name.size()));
    memcpy(Name, name.data(), name.size());

    this->Name = Name;
    this->NameLength = name.size();
  }

  const AccessibleFunctionRecord *getRecord() const { return R; }

  bool matchesKey(llvm::StringRef name) {
    return name == llvm::StringRef{Name, NameLength};
  }

  friend llvm::hash_code hash_value(const AccessibleFunctionCacheEntry &value) {
    return hash_value(llvm::StringRef{value.Name, value.NameLength});
  }

  template <class... T>
  static size_t getExtraAllocationSize(T &&...ignored) {
    return 0;
  }
};

struct AccessibleFunctionsState {
  ConcurrentReadableHashMap<AccessibleFunctionCacheEntry> Cache;
  ConcurrentReadableArray<AccessibleFunctionsSection> SectionsToScan;

  AccessibleFunctionsState() {
    initializeAccessibleFunctionsLookup();
  }
};

static Lazy<AccessibleFunctionsState> Functions;

} // end anonymous namespace

static void _registerAccessibleFunctions(AccessibleFunctionsState &C,
                                         AccessibleFunctionsSection section) {
  C.SectionsToScan.push_back(section);
}

void swift::addImageAccessibleFunctionsBlockCallbackUnsafe(
  const void *baseAddress, const void *functions, uintptr_t size) {
  assert(
      size % sizeof(AccessibleFunctionRecord) == 0 &&
      "accessible function section not a multiple of AccessibleFunctionRecord");

  auto &C = Functions.unsafeGetAlreadyInitialized();
  _registerAccessibleFunctions(C, AccessibleFunctionsSection{functions, size});
}

void swift::addImageAccessibleFunctionsBlockCallback(
  const void *baseAddress, const void *functions, uintptr_t size) {
  Functions.get();
  addImageAccessibleFunctionsBlockCallbackUnsafe(baseAddress, functions, size);
}

static const AccessibleFunctionRecord *
_searchForFunctionRecord(AccessibleFunctionsState &S, llvm::StringRef name) {
  auto traceState = runtime::trace::accessible_function_scan_begin(name);

  for (const auto &section : S.SectionsToScan.snapshot()) {
    for (auto &record : section) {
      auto recordName =
          swift::Demangle::makeSymbolicMangledNameStringRef(record.Name.get());
      if (recordName == name)
        return traceState.end(&record);
    }
  }
  return nullptr;
}

SWIFT_RUNTIME_STDLIB_SPI
const AccessibleFunctionRecord *
swift::runtime::swift_findAccessibleFunction(const char *targetNameStart,
                                             size_t targetNameLength) {
  auto &S = Functions.get();

  llvm::StringRef name{targetNameStart, targetNameLength};

  // Look for an existing entry.
  {
    auto snapshot = S.Cache.snapshot();
    if (auto E = snapshot.find(name))
      return E->getRecord();
  }

  // If entry doesn't exist (either record doesn't exist, hasn't been loaded, or
  // requested yet), let's try to find it and add to the cache.

  auto *record = _searchForFunctionRecord(S, name);
  if (record) {
    S.Cache.getOrInsert(
        name, [&](AccessibleFunctionCacheEntry *entry, bool created) {
          if (created)
            ::new (entry) AccessibleFunctionCacheEntry{name, record};
          return true;
        });
  }

  return record;
}
