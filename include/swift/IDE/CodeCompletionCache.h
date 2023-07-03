//===--- CodeCompletionCache.h - Routines for code completion caching -----===//
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

#ifndef SWIFT_IDE_CODE_COMPLETIONCACHE_H
#define SWIFT_IDE_CODE_COMPLETIONCACHE_H

#include "swift/Basic/ThreadSafeRefCounted.h"
#include "swift/IDE/CodeCompletion.h"
#include "swift/IDE/CodeCompletionResult.h"
#include "swift/IDE/CodeCompletionString.h"
#include "llvm/ADT/Hashing.h"
#include "llvm/ADT/IntrusiveRefCntPtr.h"
#include "llvm/Support/Chrono.h"
#include <system_error>

namespace swift {
namespace ide {

struct CodeCompletionCacheImpl;
class OnDiskCodeCompletionCache;

/// In-memory per-module code completion result cache.
///
/// These results persist between multiple code completion requests and can be
/// used with different ASTContexts.
class CodeCompletionCache {
  std::unique_ptr<CodeCompletionCacheImpl> Impl;
  OnDiskCodeCompletionCache *nextCache;

public:
  /// Cache key.
  struct Key {
    std::string ModuleFilename;
    std::string ModuleName;
    std::vector<std::string> AccessPath;
    bool ResultsHaveLeadingDot;
    bool ForTestableLookup;
    bool ForPrivateImportLookup;
    /// Must be sorted alphabetically for stable identity.
    llvm::SmallVector<std::string, 2> SpiGroups;
    bool AddInitsInToplevel;
    bool AddCallWithNoDefaultArgs;
    bool Annotated;

    friend bool operator==(const Key &LHS, const Key &RHS) {
      return LHS.ModuleFilename == RHS.ModuleFilename &&
             LHS.ModuleName == RHS.ModuleName &&
             LHS.AccessPath == RHS.AccessPath &&
             LHS.ResultsHaveLeadingDot == RHS.ResultsHaveLeadingDot &&
             LHS.ForTestableLookup == RHS.ForTestableLookup &&
             LHS.ForPrivateImportLookup == RHS.ForPrivateImportLookup &&
             LHS.SpiGroups == RHS.SpiGroups &&
             LHS.AddInitsInToplevel == RHS.AddInitsInToplevel &&
             LHS.AddCallWithNoDefaultArgs == RHS.AddCallWithNoDefaultArgs &&
             LHS.Annotated == RHS.Annotated;
    }
  };

  struct Value : public llvm::ThreadSafeRefCountedBase<Value> {
    /// The allocator used to allocate the results stored in this cache.
    std::shared_ptr<llvm::BumpPtrAllocator> Allocator;

    llvm::sys::TimePoint<> ModuleModificationTime;

    std::vector<const ContextFreeCodeCompletionResult *> Results;

    /// The arena that contains the \c USRBasedTypes of the
    /// \c ContextFreeCodeCompletionResult in this cache value.
    USRBasedTypeArena USRTypeArena;

    Value() : Allocator(std::make_shared<llvm::BumpPtrAllocator>()) {}
  };
  using ValueRefCntPtr = llvm::IntrusiveRefCntPtr<Value>;

  CodeCompletionCache(OnDiskCodeCompletionCache *nextCache = nullptr);
  ~CodeCompletionCache();

  static ValueRefCntPtr createValue();
  llvm::Optional<ValueRefCntPtr> get(const Key &K);
  void set(const Key &K, ValueRefCntPtr V) { setImpl(K, V, /*setChain*/ true); }

private:
  void setImpl(const Key &K, ValueRefCntPtr V, bool setChain);
};

/// On-disk per-module code completion result cache.
///
/// These results persist between multiple code completion requests and can be
/// used with different ASTContexts.
class OnDiskCodeCompletionCache {
  std::string cacheDirectory;

public:
  using Key = CodeCompletionCache::Key;
  using Value = CodeCompletionCache::Value;
  using ValueRefCntPtr = CodeCompletionCache::ValueRefCntPtr;

  OnDiskCodeCompletionCache(Twine cacheDirectory);
  ~OnDiskCodeCompletionCache();

  llvm::Optional<ValueRefCntPtr> get(const Key &K);
  std::error_code set(const Key &K, ValueRefCntPtr V);

  static llvm::Optional<ValueRefCntPtr> getFromFile(StringRef filename);
};

struct RequestedCachedModule {
  CodeCompletionCache::Key Key;
  const ModuleDecl *TheModule;
  CodeCompletionFilter Filter;
};

} // end namespace ide
} // end namespace swift

namespace llvm {
template<>
struct DenseMapInfo<swift::ide::CodeCompletionCache::Key> {
  using KeyTy = swift::ide::CodeCompletionCache::Key;
  static inline KeyTy getEmptyKey() {
    return KeyTy{/*ModuleFilename=*/"",
                 /*ModuleName=*/"",
                 /*AccessPath=*/{},
                 /*ResultsHaveLeadingDot=*/false,
                 /*ForTestableLookup=*/false,
                 /*ForPrivateImportLookup=*/false,
                 /*SpiGroups=*/{},
                 /*AddInitsInToplevel=*/false,
                 /*AddCallWithNoDefaultArgs=*/false,
                 /*Annotated=*/false};
  }
  static inline KeyTy getTombstoneKey() {
    return KeyTy{/*ModuleFilename=*/"",
                 /*ModuleName=*/"",
                 /*AccessPath=*/{},
                 /*ResultsHaveLeadingDot=*/true,
                 /*ForTestableLookup=*/false,
                 /*ForPrivateImportLookup=*/false,
                 /*SpiGroups=*/{},
                 /*AddInitsInToplevel=*/false,
                 /*AddCallWithNoDefaultArgs=*/false,
                 /*Annotated=*/false};
  }
  static unsigned getHashValue(const KeyTy &Val) {
    return llvm::hash_combine(
        Val.ModuleFilename, Val.ModuleName,
        llvm::hash_combine_range(Val.AccessPath.begin(), Val.AccessPath.end()),
        Val.ResultsHaveLeadingDot, Val.ForTestableLookup,
        llvm::hash_combine_range(Val.SpiGroups.begin(), Val.SpiGroups.end()),
        Val.ForPrivateImportLookup, Val.AddInitsInToplevel,
        Val.AddCallWithNoDefaultArgs, Val.Annotated);
  }
  static bool isEqual(const KeyTy &LHS, const KeyTy &RHS) {
    return LHS == RHS;
  }
};
} // end namespace llvm

#endif // SWIFT_IDE_CODE_COMPLETIONCACHE_H
