//===------------ SwiftCaching.cpp - Swift Compiler -----------------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2020 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//
//
// Implementation of the caching APIs in libSwiftScan
//
//===----------------------------------------------------------------------===//

#include "swift/AST/DiagnosticEngine.h"
#include "swift/AST/DiagnosticsFrontend.h"
#include "swift/Basic/Defer.h"
#include "swift/Basic/FileTypes.h"
#include "swift/Basic/SourceManager.h"
#include "swift/DependencyScan/DependencyScanImpl.h"
#include "swift/DependencyScan/StringUtils.h"
#include "swift/Driver/FrontendUtil.h"
#include "swift/Frontend/CachedDiagnostics.h"
#include "swift/Frontend/CachingUtils.h"
#include "swift/Frontend/CompileJobCacheKey.h"
#include "swift/Frontend/CompileJobCacheResult.h"
#include "swift/Frontend/DiagnosticHelper.h"
#include "swift/Frontend/Frontend.h"
#include "swift/Frontend/PrintingDiagnosticConsumer.h"
#include "swift/Option/Options.h"
#include "clang/CAS/CASOptions.h"
#include "clang/Frontend/CompileJobCacheResult.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/IntrusiveRefCntPtr.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/CAS/ActionCache.h"
#include "llvm/CAS/BuiltinUnifiedCASDatabases.h"
#include "llvm/CAS/CASReference.h"
#include "llvm/CAS/ObjectStore.h"
#include "llvm/Support/Allocator.h"
#include "llvm/Support/Endian.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/PrefixMapper.h"
#include "llvm/Support/StringSaver.h"
#include "llvm/Support/VirtualOutputBackend.h"
#include "llvm/Support/VirtualOutputBackends.h"
#include "llvm/Support/raw_ostream.h"
#include <optional>
#include <variant>

namespace {
/// Helper class to manage CAS/Caching from libSwiftScan C APIs.
class SwiftScanCAS {
public:
  llvm::cas::ObjectStore &getCAS() const { return *CAS; }
  llvm::cas::ActionCache &getCache() const { return *Cache; }

  // Construct SwiftScanCAS.
  static llvm::Expected<SwiftScanCAS *>
  createSwiftScanCAS(llvm::StringRef Path);

  static llvm::Expected<SwiftScanCAS *>
  createSwiftScanCAS(clang::CASOptions &CASOpts);

private:
  SwiftScanCAS(std::shared_ptr<llvm::cas::ObjectStore> CAS,
               std::shared_ptr<llvm::cas::ActionCache> Cache)
      : CAS(CAS), Cache(Cache) {}

  std::shared_ptr<llvm::cas::ObjectStore> CAS;
  std::shared_ptr<llvm::cas::ActionCache> Cache;
};

struct SwiftCachedCompilationHandle {
  SwiftCachedCompilationHandle(llvm::cas::ObjectRef Key,
                               llvm::cas::ObjectRef Output,
                               clang::cas::CompileJobCacheResult &&Result,
                               unsigned InputIndex, SwiftScanCAS &CAS)
      : Key(Key), Output(Output), InputIndex(InputIndex), Result(Result),
        DB(CAS) {}
  SwiftCachedCompilationHandle(llvm::cas::ObjectRef Key,
                               llvm::cas::ObjectRef Output,
                               swift::cas::CompileJobCacheResult &&Result,
                               unsigned InputIndex, SwiftScanCAS &CAS)
      : Key(Key), Output(Output), InputIndex(InputIndex), Result(Result),
        DB(CAS) {}

  llvm::cas::ObjectRef Key;
  llvm::cas::ObjectRef Output;
  unsigned InputIndex;
  std::variant<swift::cas::CompileJobCacheResult,
               clang::cas::CompileJobCacheResult>
      Result;
  SwiftScanCAS &DB;
};

struct SwiftCachedOutputHandle {
  llvm::cas::ObjectRef Ref;
  swift::file_types::ID Kind;
  llvm::cas::ObjectStore &CAS;
  std::optional<llvm::cas::ObjectProxy> LoadedProxy;

  SwiftCachedOutputHandle(llvm::cas::ObjectRef Ref, swift::file_types::ID Kind,
                          llvm::cas::ObjectStore &CAS)
      : Ref(Ref), Kind(Kind), CAS(CAS) {}
};

struct SwiftScanReplayInstance {
  swift::CompilerInvocation Invocation;
  llvm::BumpPtrAllocator StringAlloc;
  llvm::SmallVector<const char *> Args;
};

struct SwiftCachedReplayResult {
  llvm::SmallVector<char> outMsg;
  llvm::SmallVector<char> errMsg;
  llvm::raw_svector_ostream outOS;
  llvm::raw_svector_ostream errOS;

  SwiftCachedReplayResult() : outOS(outMsg), errOS(errMsg) {}
};
} // namespace

DEFINE_SIMPLE_CONVERSION_FUNCTIONS(clang::CASOptions, swiftscan_cas_options_t)
DEFINE_SIMPLE_CONVERSION_FUNCTIONS(SwiftScanCAS, swiftscan_cas_t)
DEFINE_SIMPLE_CONVERSION_FUNCTIONS(SwiftCachedCompilationHandle,
                                   swiftscan_cached_compilation_t)
DEFINE_SIMPLE_CONVERSION_FUNCTIONS(SwiftCachedOutputHandle,
                                   swiftscan_cached_output_t)
DEFINE_SIMPLE_CONVERSION_FUNCTIONS(SwiftScanReplayInstance,
                                   swiftscan_cache_replay_instance_t)
DEFINE_SIMPLE_CONVERSION_FUNCTIONS(SwiftCachedReplayResult,
                                   swiftscan_cache_replay_result_t)

//=== CAS Functions ----------------------------------------------------------//

swiftscan_cas_options_t swiftscan_cas_options_create() {
  clang::CASOptions *CASOpts = new clang::CASOptions();
  return wrap(CASOpts);
}

void swiftscan_cas_options_dispose(swiftscan_cas_options_t options) {
  delete unwrap(options);
}

void swiftscan_cas_options_set_ondisk_path(swiftscan_cas_options_t options,
                                           const char *path) {
  unwrap(options)->CASPath = path;
}

void swiftscan_cas_options_set_plugin_path(swiftscan_cas_options_t options,
                                           const char *path) {
  unwrap(options)->PluginPath = path;
}

bool swiftscan_cas_options_set_plugin_option(swiftscan_cas_options_t options,
                                             const char *name,
                                             const char *value,
                                             swiftscan_string_ref_t *error) {
  unwrap(options)->PluginOptions.emplace_back(name, value);
  return false;
}

swiftscan_cas_t
swiftscan_cas_create_from_options(swiftscan_cas_options_t options,
                                  swiftscan_string_ref_t *error) {
  clang::CASOptions *opts = unwrap(options);
  auto cas = SwiftScanCAS::createSwiftScanCAS(*opts);
  if (!cas) {
    *error =
        swift::c_string_utils::create_clone(toString(cas.takeError()).c_str());
    return nullptr;
  }
  return wrap(*cas);
}

void swiftscan_cas_dispose(swiftscan_cas_t cas) { delete unwrap(cas); }

swiftscan_string_ref_t swiftscan_cas_store(swiftscan_cas_t cas, uint8_t *data,
                                           unsigned size,
                                           swiftscan_string_ref_t *error) {
  auto failure = [&](llvm::Error E) {
    *error =
        swift::c_string_utils::create_clone(toString(std::move(E)).c_str());
    return swift::c_string_utils::create_null();
  };

  auto &CAS = unwrap(cas)->getCAS();
  llvm::StringRef StrContent((char *)data, size);
  auto Result = CAS.storeFromString({}, StrContent);
  if (!Result)
    return failure(Result.takeError());

  *error = swift::c_string_utils::create_null();
  return swift::c_string_utils::create_clone(
      CAS.getID(*Result).toString().c_str());
}

int64_t swiftscan_cas_get_ondisk_size(swiftscan_cas_t cas,
                                      swiftscan_string_ref_t *error) {
  auto &CAS = unwrap(cas)->getCAS();
  std::optional<uint64_t> Size;
  if (auto E = CAS.getStorageSize().moveInto(Size)) {
    *error =
        swift::c_string_utils::create_clone(toString(std::move(E)).c_str());
    return -2;
  }

  *error = swift::c_string_utils::create_null();
  return Size ? *Size : -1;
}

bool
swiftscan_cas_set_ondisk_size_limit(swiftscan_cas_t cas, int64_t size_limit,
    swiftscan_string_ref_t *error) {
  if (size_limit < 0) {
    *error = swift::c_string_utils::create_clone(
        "invalid size limit passing to swiftscan_cas_set_ondisk_size_limit");
    return true;
  }
  auto &CAS = unwrap(cas)->getCAS();
  std::optional<uint64_t> SizeLimit;
  if (size_limit > 0)
    SizeLimit = size_limit;
  if (auto E = CAS.setSizeLimit(SizeLimit)) {
    *error =
        swift::c_string_utils::create_clone(toString(std::move(E)).c_str());
    return true;
  }
  *error = swift::c_string_utils::create_null();
  return false;
}

bool swiftscan_cas_prune_ondisk_data(swiftscan_cas_t cas,
                                     swiftscan_string_ref_t *error) {
  auto &CAS = unwrap(cas)->getCAS();
  if (auto E = CAS.pruneStorageData()) {
    *error =
        swift::c_string_utils::create_clone(toString(std::move(E)).c_str());
    return true;
  }
  *error = swift::c_string_utils::create_null();
  return false;
}

/// Expand the invocation if there is repsonseFile into Args that are passed in
/// the parameter. Return swift-frontend arguments in an ArrayRef, which has the
/// first "-frontend" option dropped if needed.
static llvm::ArrayRef<const char *>
expandSwiftInvocation(int argc, const char **argv, llvm::StringSaver &Saver,
                      llvm::SmallVectorImpl<const char *> &ArgsStorage) {
  ArgsStorage.reserve(argc);
  for (int i = 0; i < argc; ++i)
    ArgsStorage.push_back(Saver.save(argv[i]).data());
  swift::driver::ExpandResponseFilesWithRetry(Saver, ArgsStorage);

  // Drop the `-frontend` option if it is passed.
  llvm::ArrayRef<const char*> FrontendArgs(ArgsStorage);
  if (!FrontendArgs.empty() &&
      llvm::StringRef(FrontendArgs.front()) == "-frontend")
    FrontendArgs = FrontendArgs.drop_front();
  return FrontendArgs;
}

static llvm::Expected<std::string>
computeCacheKey(llvm::cas::ObjectStore &CAS, llvm::ArrayRef<const char *> Args,
                llvm::StringRef InputPath) {
  auto BaseKey = swift::createCompileJobBaseCacheKey(CAS, Args);
  if (!BaseKey)
    return BaseKey.takeError();

  // Parse the arguments to figure out the index for the input.
  swift::CompilerInvocation Invocation;
  swift::SourceManager SourceMgr;
  swift::DiagnosticEngine Diags(SourceMgr);
  llvm::SmallString<128> workingDirectory;
  llvm::sys::fs::current_path(workingDirectory);
  llvm::SmallVector<std::unique_ptr<llvm::MemoryBuffer>, 4>
      configurationFileBuffers;

  std::string MainExecutablePath = llvm::sys::fs::getMainExecutable(
      "swift-frontend", (void *)swiftscan_cache_replay_compilation);

  if (Invocation.parseArgs(Args, Diags, &configurationFileBuffers,
                           workingDirectory, MainExecutablePath))
    return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                   "Argument parsing failed");

  auto computeKey = [&](unsigned Index) -> llvm::Expected<std::string> {
    auto Key = swift::createCompileJobCacheKeyForOutput(CAS, *BaseKey, Index);
    if (!Key)
      return Key.takeError();
    return CAS.getID(*Key).toString();
  };
  auto AllInputs =
      Invocation.getFrontendOptions().InputsAndOutputs.getAllInputs();
  // First pass, check for path equal.
  for (unsigned Idx = 0; Idx < AllInputs.size(); ++Idx) {
    if (AllInputs[Idx].getFileName() == InputPath)
      return computeKey(Idx);
  }

  // If not found, slow second iteration with real_path.
  llvm::SmallString<256> InputRealPath;
  llvm::sys::fs::real_path(InputPath, InputRealPath, true);
  for (unsigned Idx = 0; Idx < AllInputs.size(); ++Idx) {
    llvm::SmallString<256> TestRealPath;
    llvm::sys::fs::real_path(AllInputs[Idx].getFileName(), TestRealPath, true);
    if (InputRealPath == TestRealPath)
      return computeKey(Idx);
  }

  return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                 "requested input not found from invocation");
}

static llvm::Expected<std::string>
computeCacheKeyFromIndex(llvm::cas::ObjectStore &CAS,
                         llvm::ArrayRef<const char *> Args,
                         unsigned InputIndex) {
  auto BaseKey = swift::createCompileJobBaseCacheKey(CAS, Args);
  if (!BaseKey)
    return BaseKey.takeError();

  auto Key =
      swift::createCompileJobCacheKeyForOutput(CAS, *BaseKey, InputIndex);
  if (!Key)
    return Key.takeError();
  return CAS.getID(*Key).toString();
}

swiftscan_string_ref_t
swiftscan_cache_compute_key(swiftscan_cas_t cas, int argc, const char **argv,
                            const char *input, swiftscan_string_ref_t *error) {
  llvm::SmallVector<const char *> ArgsStorage;
  llvm::BumpPtrAllocator Alloc;
  llvm::StringSaver Saver(Alloc);
  auto Args = expandSwiftInvocation(argc, argv, Saver, ArgsStorage);

  auto ID = computeCacheKey(unwrap(cas)->getCAS(), Args, input);
  if (!ID) {
    *error =
        swift::c_string_utils::create_clone(toString(ID.takeError()).c_str());
    return swift::c_string_utils::create_null();
  }
  *error = swift::c_string_utils::create_null();
  return swift::c_string_utils::create_clone(ID->c_str());
}

swiftscan_string_ref_t
swiftscan_cache_compute_key_from_input_index(swiftscan_cas_t cas, int argc,
                                             const char **argv,
                                             unsigned input_index,
                                             swiftscan_string_ref_t *error) {
  llvm::SmallVector<const char *> ArgsStorage;
  llvm::BumpPtrAllocator Alloc;
  llvm::StringSaver Saver(Alloc);
  auto Args = expandSwiftInvocation(argc, argv, Saver, ArgsStorage);

  auto ID =
      computeCacheKeyFromIndex(unwrap(cas)->getCAS(), Args, input_index);
  if (!ID) {
    *error =
        swift::c_string_utils::create_clone(toString(ID.takeError()).c_str());
    return swift::c_string_utils::create_null();
  }
  *error = swift::c_string_utils::create_null();
  return swift::c_string_utils::create_clone(ID->c_str());
}

// Create a non-owning string ref that is used in call backs.
static swiftscan_string_ref_t createNonOwningString(llvm::StringRef Str) {
  if (Str.empty())
    return swift::c_string_utils::create_null();

  swiftscan_string_ref_t Result;
  Result.data = Str.data();
  Result.length = Str.size();
  return Result;
}

static llvm::Error createUnsupportedSchemaError() {
  return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                 "unsupported compile result schema found");
}

static llvm::Expected<SwiftCachedCompilationHandle *>
createCachedCompilation(SwiftScanCAS &CAS, const llvm::cas::CASID &ID,
                        const llvm::cas::CASID &Key) {
  auto Ref = CAS.getCAS().getReference(ID);
  if (!Ref)
    return nullptr;

  auto Proxy = CAS.getCAS().getProxy(*Ref);
  if (!Proxy)
    return Proxy.takeError();

  // Load input file name from the key CAS object. Input file name is the data
  // blob in the root node.
  auto KeyProxy = CAS.getCAS().getProxy(Key);
  if (!KeyProxy)
    return KeyProxy.takeError();
  auto Input = KeyProxy->getData();

  unsigned Index =
      llvm::support::endian::read<uint32_t, llvm::support::little,
                                  llvm::support::unaligned>(Input.data());
  {
    swift::cas::CompileJobResultSchema Schema(CAS.getCAS());
    if (Schema.isRootNode(*Proxy)) {
      auto Result = Schema.load(Proxy->getRef());
      if (!Result)
        return Result.takeError();
      return new SwiftCachedCompilationHandle(KeyProxy->getRef(), *Ref,
                                              std::move(*Result), Index, CAS);
    }
  }
  {
    clang::cas::CompileJobResultSchema Schema(CAS.getCAS());
    if (Schema.isRootNode(*Proxy)) {
      auto Result = Schema.load(Proxy->getRef());
      if (!Result)
        return Result.takeError();
      return new SwiftCachedCompilationHandle(KeyProxy->getRef(), *Ref,
                                              std::move(*Result), Index, CAS);
    }
  }
  return createUnsupportedSchemaError();
}

swiftscan_cached_compilation_t
swiftscan_cache_query(swiftscan_cas_t cas, const char *key, bool globally,
                      swiftscan_string_ref_t *error) {
  auto failure = [&](llvm::Error E) {
    *error =
        swift::c_string_utils::create_clone(toString(std::move(E)).c_str());
    return nullptr;
  };
  auto notfound = [&]() {
    *error = swift::c_string_utils::create_null();
    return nullptr;
  };

  auto &CAS = unwrap(cas)->getCAS();
  auto &Cache = unwrap(cas)->getCache();
  auto ID = CAS.parseID(key);
  if (!ID)
    return failure(ID.takeError());

  auto Result = Cache.get(*ID, globally);
  if (!Result)
    return failure(Result.takeError());
  if (!*Result)
    return notfound();

  auto Cached = createCachedCompilation(*unwrap(cas), **Result, *ID);
  if (!Cached)
    return failure(Cached.takeError());

  if (!*Cached)
    return notfound();

  *error = swift::c_string_utils::create_null();
  return wrap(*Cached);
}

void swiftscan_cache_query_async(
    swiftscan_cas_t cas, const char *key, bool globally, void *ctx,
    void (*callback)(void *ctx, swiftscan_cached_compilation_t,
                     swiftscan_string_ref_t error),
    swiftscan_cache_cancellation_token_t *token) {
  if (token)
    *token = nullptr;
  auto &CAS = unwrap(cas)->getCAS();
  auto &Cache = unwrap(cas)->getCache();
  auto failure = [ctx, callback](llvm::Error E) {
    std::string ErrMsg = toString(std::move(E));
    return callback(ctx, nullptr, createNonOwningString(ErrMsg));
  };
  auto notfound = [ctx, callback]() {
    return callback(ctx, nullptr, swift::c_string_utils::create_null());
  };
  auto success = [ctx, callback](swiftscan_cached_compilation_t comp) {
    return callback(ctx, comp, swift::c_string_utils::create_null());
  };

  auto ID = CAS.parseID(key);
  if (!ID)
    return failure(ID.takeError());

  auto KeyID = *ID;
  Cache.getAsync(*ID, globally,
                 [failure, notfound, success, cas, KeyID](
                     llvm::Expected<std::optional<llvm::cas::CASID>> Result) {
                   if (!Result)
                     return failure(Result.takeError());
                   if (!*Result)
                     return notfound();

                   auto Cached =
                       createCachedCompilation(*unwrap(cas), **Result, KeyID);
                   if (!Cached)
                     return failure(Cached.takeError());

                   if (!*Cached)
                     return notfound();

                   return success(wrap(*Cached));
                 });
}

unsigned swiftscan_cached_compilation_get_num_outputs(
    swiftscan_cached_compilation_t id) {
  auto *Comp = unwrap(id);
  if (auto *Result =
          std::get_if<swift::cas::CompileJobCacheResult>(&Comp->Result))
    return Result->getNumOutputs();

  auto *Result = std::get_if<clang::cas::CompileJobCacheResult>(&Comp->Result);
  assert(Result && "unexpected variant");
  return Result->getNumOutputs();
}

swiftscan_cached_output_t
swiftscan_cached_compilation_get_output(swiftscan_cached_compilation_t id,
                                        unsigned idx) {
  auto *Comp = unwrap(id);
  if (auto *Result =
      std::get_if<swift::cas::CompileJobCacheResult>(&Comp->Result))
    return wrap(new SwiftCachedOutputHandle(Result->getOutput(idx).Object,
                                            Result->getOutput(idx).Kind,
                                            Comp->DB.getCAS()));

  auto *Result = std::get_if<clang::cas::CompileJobCacheResult>(&Comp->Result);
  assert(Result && "unexpected variant");
  swift::file_types::ID Kind = swift::file_types::TY_INVALID;
  switch (Result->getOutput(idx).Kind) {
  case clang::cas::CompileJobCacheResult::OutputKind::MainOutput:
    Kind = swift::file_types::TY_ClangModuleFile;
    break;
  case clang::cas::CompileJobCacheResult::OutputKind::Dependencies:
    Kind = swift::file_types::TY_Dependencies;
    break;
  case clang::cas::CompileJobCacheResult::OutputKind::SerializedDiagnostics:
    Kind = swift::file_types::TY_CachedDiagnostics;
    break;
  }
  assert(Kind != swift::file_types::TY_INVALID && "Invalid kind");
  return wrap(new SwiftCachedOutputHandle(Result->getOutput(idx).Object, Kind,
                                          Comp->DB.getCAS()));
}

bool swiftscan_cached_compilation_is_uncacheable(
    swiftscan_cached_compilation_t id) {
  // Currently, all compilations are cacheable.
  return false;
}

void swiftscan_cached_compilation_dispose(swiftscan_cached_compilation_t id) {
  delete unwrap(id);
}

bool swiftscan_cached_output_load(swiftscan_cached_output_t output,
                                  swiftscan_string_ref_t *error) {
  auto *Out = unwrap(output);
  auto failure = [&](llvm::Error E) {
    *error =
        swift::c_string_utils::create_clone(toString(std::move(E)).c_str());
    return false;
  };
  auto notfound = [&]() {
    *error = swift::c_string_utils::create_null();
    return false;
  };
  auto success = [&]() {
    *error = swift::c_string_utils::create_null();
    return true;
  };
  // If proxy exists, there is nothing to be done.
  if (Out->LoadedProxy)
    return success();

  if (auto Err = Out->CAS.getProxyIfExists(Out->Ref).moveInto(Out->LoadedProxy))
    return failure(std::move(Err));

  if (!Out->LoadedProxy)
    return notfound();

  llvm::DenseSet<llvm::cas::ObjectRef> Visited;
  llvm::SmallVector<llvm::cas::ObjectRef> WorkList;
  auto addToWorkList = [&](llvm::cas::ObjectRef Item) {
    if (Visited.insert(Item).second)
      WorkList.push_back(Item);
  };

  addToWorkList(Out->LoadedProxy->getRef());
  while (!WorkList.empty()) {
    auto Current = WorkList.pop_back_val();
    auto Proxy = Out->CAS.getProxyIfExists(Current);
    if (!Proxy)
      return failure(Proxy.takeError());

    if (!*Proxy)
      return notfound();

    if (auto Err = (*Proxy)->forEachReference(
            [&](llvm::cas::ObjectRef Ref) -> llvm::Error {
              addToWorkList(Ref);
              return llvm::Error::success();
            }))
      return failure(std::move(Err));
  }
  return success();
}

namespace {
/// Asynchronously visits the graph of the object node to ensure it's fully
/// materialized.
class AsyncObjectLoader
    : public std::enable_shared_from_this<AsyncObjectLoader> {
  void *Ctx;
  void (*Callback)(void *Ctx, bool, swiftscan_string_ref_t);
  llvm::cas::ObjectStore &CAS;

  // The output handle to update after load if set.
  SwiftCachedOutputHandle *OutputHandle;

  llvm::SmallDenseSet<llvm::cas::ObjectRef> ObjectsSeen;
  unsigned NumPending = 0;
  std::optional<llvm::cas::ObjectProxy> RootObj;
  std::atomic<bool> MissingNode{false};
  /// The first error that occurred.
  std::optional<llvm::Error> ErrOccurred;
  std::mutex Mutex;

public:
  AsyncObjectLoader(void *Ctx,
                    void (*Callback)(void *Ctx, bool, swiftscan_string_ref_t),
                    llvm::cas::ObjectStore &CAS,
                    SwiftCachedOutputHandle *Handle = nullptr)
      : Ctx(Ctx), Callback(Callback), CAS(CAS), OutputHandle(Handle) {}

  void visit(llvm::cas::ObjectRef Ref, bool IsRootNode) {
    {
      std::lock_guard<std::mutex> Guard(Mutex);
      if (!ObjectsSeen.insert(Ref).second)
        return;
      ++NumPending;
    }
    auto This = shared_from_this();
    CAS.getProxyAsync(
        Ref, [This, IsRootNode](
                 llvm::Expected<std::optional<llvm::cas::ObjectProxy>> Obj) {
          SWIFT_DEFER { This->finishedNode(); };
          if (!Obj) {
            This->encounteredError(Obj.takeError());
            return;
          }
          if (!*Obj) {
            This->MissingNode = true;
            return;
          }
          if (IsRootNode)
            This->RootObj = *Obj;
          if (auto Err = (*Obj)->forEachReference(
                  [&This](llvm::cas::ObjectRef Sub) -> llvm::Error {
                    This->visit(Sub, /*IsRootNode*/ false);
                    return llvm::Error::success();
                  })) {
            This->encounteredError(std::move(Err));
            return;
          }
        });
  }

  void finishedNode() {
    {
      std::lock_guard<std::mutex> Guard(Mutex);
      assert(NumPending);
      --NumPending;
      if (NumPending != 0)
        return;
    }
    if (ErrOccurred) {
      std::string ErrMsg = toString(std::move(*ErrOccurred));
      return Callback(Ctx, false, createNonOwningString(ErrMsg));
    }
    if (MissingNode)
      return Callback(Ctx, false, swift::c_string_utils::create_null());

    if (OutputHandle)
      OutputHandle->LoadedProxy = RootObj;
    return Callback(Ctx, true, swift::c_string_utils::create_null());
  }

  /// Only keeps the first error that occurred.
  void encounteredError(llvm::Error &&E) {
    std::lock_guard<std::mutex> Guard(Mutex);
    if (ErrOccurred) {
      llvm::consumeError(std::move(E));
      return;
    }
    ErrOccurred = std::move(E);
  }
};
} // namespace

void swiftscan_cached_output_load_async(
    swiftscan_cached_output_t output, void *ctx,
    void (*callback)(void *ctx, bool success, swiftscan_string_ref_t error),
    swiftscan_cache_cancellation_token_t *token) {
  if (token)
    *token = nullptr;

  auto *Out = unwrap(output);
  if (Out->LoadedProxy)
    return callback(ctx, true, swift::c_string_utils::create_null());

  auto Loader =
      std::make_shared<AsyncObjectLoader>(ctx, callback, Out->CAS, Out);
  Loader->visit(Out->Ref, /*IsRootNode*/ true);
}

void swiftscan_cache_download_cas_object_async(
    swiftscan_cas_t cas, const char *id, void *ctx,
    void (*callback)(void *ctx, bool success, swiftscan_string_ref_t error),
    swiftscan_cache_cancellation_token_t *token) {
  if (token)
    *token = nullptr;

  auto failure = [ctx, callback](llvm::Error E) {
    std::string ErrMsg = toString(std::move(E));
    return callback(ctx, false, createNonOwningString(ErrMsg));
  };

  auto &CAS = unwrap(cas)->getCAS();
  auto ID = CAS.parseID(id);
  if (!ID)
    return failure(ID.takeError());

  auto Ref = CAS.getReference(*ID);
  if (!Ref)
    return callback(ctx, false, swift::c_string_utils::create_null());

  auto Loader =
      std::make_shared<AsyncObjectLoader>(ctx, callback, CAS);
  Loader->visit(*Ref, /*IsRootNode*/ true);
}

bool swiftscan_cached_output_is_materialized(swiftscan_cached_output_t output) {
  auto *Out = unwrap(output);
  // Already loaded.
  if (Out->LoadedProxy)
    return true;

  auto Loaded = Out->CAS.isMaterialized(Out->Ref);
  if (!Loaded) {
    // There is an error loading, output is not materialized. Discard error and
    // return false.
    consumeError(Loaded.takeError());
    return false;
  }
  return *Loaded;
}

swiftscan_string_ref_t
swiftscan_cached_output_get_casid(swiftscan_cached_output_t output) {
  auto *Out = unwrap(output);
  auto ID = Out->CAS.getID(Out->Ref);
  return swift::c_string_utils::create_clone(ID.toString().c_str());
}

void swiftscan_cached_compilation_make_global_async(
    swiftscan_cached_compilation_t comp, void *ctx,
    void (*callback)(void *ctx, swiftscan_string_ref_t error),
    swiftscan_cache_cancellation_token_t *token) {
  if (token)
    *token = nullptr;
  auto failure = [ctx, callback](llvm::Error E) {
    std::string ErrMsg = toString(std::move(E));
    return callback(ctx, createNonOwningString(ErrMsg));
  };
  auto success = [ctx, callback]() {
    return callback(ctx, swift::c_string_utils::create_null());
  };

  auto *Compilation = unwrap(comp);
  auto &CAS = Compilation->DB.getCAS();
  auto &Cache = Compilation->DB.getCache();

  auto ID = CAS.getID(Compilation->Key);
  auto Result = CAS.getID(Compilation->Output);

  Cache.putAsync(ID, Result, /*globally=*/true,
                 [failure, success](llvm::Error E) {
                   if (E)
                     return failure(std::move(E));

                   return success();
                 });
}

swiftscan_string_ref_t
swiftscan_cached_output_get_name(swiftscan_cached_output_t output) {
  auto *Out = unwrap(output);
  return swift::c_string_utils::create_clone(
      swift::file_types::getTypeName(Out->Kind).str().c_str());
}

swiftscan_cache_replay_instance_t
swiftscan_cache_replay_instance_create(int argc, const char **argv,
                                       swiftscan_string_ref_t *error) {
  auto *Instance = new SwiftScanReplayInstance();
  llvm::SmallVector<const char *> Compilation;
  llvm::StringSaver Saver(Instance->StringAlloc);
  auto Args = expandSwiftInvocation(argc, argv, Saver, Instance->Args);

  // Capture the diagnostics when creating invocation.
  std::string err_msg;
  llvm::raw_string_ostream OS(err_msg);
  swift::PrintingDiagnosticConsumer Diags(OS);
  swift::SourceManager SrcMgr;
  swift::DiagnosticEngine DE(SrcMgr);
  DE.addConsumer(Diags);

  std::string MainExecutablePath = llvm::sys::fs::getMainExecutable(
      "swift-frontend", (void *)swiftscan_cache_replay_compilation);

  if (Instance->Invocation.parseArgs(Args, DE, nullptr, {},
                                     MainExecutablePath)) {
    delete Instance;
    *error = swift::c_string_utils::create_clone(err_msg.c_str());
    return nullptr;
  }

  if (!Instance->Invocation.getCASOptions().EnableCaching) {
    delete Instance;
    *error = swift::c_string_utils::create_clone(
        "caching is not enabled from command-line");
    return nullptr;
  }

  // Clear the LLVMArgs as `llvm::cl::ParseCommandLineOptions` is not
  // thread-safe to be called in libSwiftScan. The replay instance should not be
  // used to do compilation so clearing `-Xllvm` should not affect replay
  // result.
  Instance->Invocation.getFrontendOptions().LLVMArgs.clear();

  return wrap(Instance);
}

void swiftscan_cache_replay_instance_dispose(
    swiftscan_cache_replay_instance_t instance) {
  delete unwrap(instance);
}

namespace {
class StreamOutputFileImpl final
    : public llvm::RTTIExtends<StreamOutputFileImpl,
                               llvm::vfs::OutputFileImpl> {

public:
  StreamOutputFileImpl(llvm::raw_pwrite_stream &OS) : OS(OS) {}
  llvm::Error keep() final { return llvm::Error::success(); }
  llvm::Error discard() final { return llvm::Error::success(); }
  llvm::raw_pwrite_stream &getOS() final { return OS; }

private:
  llvm::raw_pwrite_stream &OS;
};

// The replay output backend. Currently, it redirects "-" to the
// stdout provided.
struct ReplayOutputBackend : public llvm::vfs::ProxyOutputBackend {
  llvm::Expected<std::unique_ptr<llvm::vfs::OutputFileImpl>>
  createFileImpl(llvm::StringRef Path,
                 std::optional<llvm::vfs::OutputConfig> Config) override {
    if (Path == "-")
      return std::make_unique<StreamOutputFileImpl>(StdOut);
    return llvm::vfs::ProxyOutputBackend::createFileImpl(Path, Config);
  }

  llvm::IntrusiveRefCntPtr<llvm::vfs::OutputBackend>
  cloneImpl() const override {
    return llvm::makeIntrusiveRefCnt<ReplayOutputBackend>(
        getUnderlyingBackend().clone(), StdOut);
  }

  ReplayOutputBackend(
      llvm::IntrusiveRefCntPtr<llvm::vfs::OutputBackend> UnderlyingBackend,
      llvm::raw_pwrite_stream &StdOut)
      : ProxyOutputBackend(UnderlyingBackend), StdOut(StdOut) {}

  llvm::raw_pwrite_stream &StdOut;
};
} // namespace

static llvm::Error replayCompilation(SwiftScanReplayInstance &Instance,
                                     llvm::cas::ObjectStore &CAS,
                                     SwiftCachedCompilationHandle &Comp,
                                     llvm::raw_pwrite_stream &Out,
                                     llvm::raw_pwrite_stream &Err) {
  using namespace swift;
  using namespace llvm;
  CompilerInstance Inst;
  CompilerInvocation &Invocation = Instance.Invocation;

  // Find the input file from the invocation.
  auto &InputsAndOutputs =
      Instance.Invocation.getFrontendOptions().InputsAndOutputs;
  auto AllInputs = InputsAndOutputs.getAllInputs();
  if (Comp.InputIndex >= AllInputs.size())
    return createStringError(inconvertibleErrorCode(),
                             "InputFile index too large for compilation");
  const auto &Input = AllInputs[Comp.InputIndex];

  // Setup DiagnosticsConsumers.
  DiagnosticHelper DH = DiagnosticHelper::create(Inst, Err, /*QuasiPID=*/true);

  std::string InstanceSetupError;
  if (Inst.setupForReplay(Instance.Invocation, InstanceSetupError,
                          Instance.Args))
    return createStringError(inconvertibleErrorCode(), InstanceSetupError);

  auto *CDP = Inst.getCachingDiagnosticsProcessor();
  assert(CDP && "CachingDiagnosticsProcessor needs to be setup for replay");
  // No diags are captured in replay instance.
  CDP->endDiagnosticCapture();

  // Collect the file that needs to write.
  DenseMap<file_types::ID, std::string> Outputs;
  if (!Input.outputFilename().empty())
    Outputs.try_emplace(InputsAndOutputs.getPrincipalOutputType(),
                        Input.outputFilename());
  Input.getPrimarySpecificPaths().SupplementaryOutputs.forEachSetOutputAndType(
      [&](const std::string &File, file_types::ID ID) {
        if (file_types::isProducedFromDiagnostics(ID))
          return;

        Outputs.try_emplace(ID, File);
      });
  Outputs.try_emplace(file_types::TY_CachedDiagnostics, "<cached-diagnostics>");

  // Load all the output buffer.
  bool Remarks = Instance.Invocation.getCASOptions().EnableCachingRemarks;
  struct OutputEntry {
    std::string Path;
    llvm::cas::ObjectProxy Proxy;
  };
  SmallVector<OutputEntry> OutputProxies;
  std::optional<llvm::cas::ObjectProxy> DiagnosticsOutput;

  swift::cas::CachedResultLoader Loader(CAS, Comp.Output);
  if (auto Err = Loader.replay(
          [&](file_types::ID Kind, llvm::cas::ObjectRef Ref) -> Error {
            auto OutputPath = Outputs.find(Kind);
            if (OutputPath == Outputs.end())
              return createStringError(
                  inconvertibleErrorCode(),
                  "unexpected output kind in the cached output");
            auto Proxy = CAS.getProxy(Ref);
            if (!Proxy)
              return Proxy.takeError();

            if (Kind == file_types::ID::TY_CachedDiagnostics) {
              assert(!DiagnosticsOutput && "more than 1 diagnotics found");
              DiagnosticsOutput = std::move(*Proxy);
            } else
              OutputProxies.emplace_back(
                  OutputEntry{OutputPath->second, std::move(*Proxy)});
            return Error::success();
          }))
    return Err;

  // Replay diagnostics first.
  // FIXME: Currently, the diagnostics is replay from the first file.
  if (DiagnosticsOutput) {
    DH.initDiagConsumers(Invocation);
    DH.beginMessage(Invocation, Instance.Args);

    if (auto E = CDP->replayCachedDiagnostics(DiagnosticsOutput->getData()))
      return E;

    if (Remarks)
      Inst.getDiags().diagnose(SourceLoc(), diag::replay_output,
                               "<cached-diagnostics>",
                               CAS.getID(Comp.Key).toString());
  } else {
    // Don't write anything when parseable output is requested.
    if (Invocation.getFrontendOptions().FrontendParseableOutput)
      DH.setSuppressOutput(true);
  }

  SWIFT_DEFER {
    if (DiagnosticsOutput) {
      DH.endMessage(0);
      Inst.getDiags().finishProcessing();
    }
  };

  // OutputBackend for replay.
  ReplayOutputBackend Backend(
      makeIntrusiveRefCnt<llvm::vfs::OnDiskOutputBackend>(), Out);

  for (auto &Output : OutputProxies) {
    auto File = Backend.createFile(Output.Path);
    if (!File)
      return File.takeError();

    *File << Output.Proxy.getData();
    if (auto E = File->keep())
      return E;

    if (Remarks)
      Inst.getDiags().diagnose(SourceLoc(), diag::replay_output, Output.Path,
                               CAS.getID(Comp.Key).toString());
  }

  return llvm::Error::success();
}

swiftscan_cache_replay_result_t
swiftscan_cache_replay_compilation(swiftscan_cache_replay_instance_t instance,
                                   swiftscan_cached_compilation_t comp,
                                   swiftscan_string_ref_t *error) {
  SwiftScanReplayInstance &Instance = *unwrap(instance);
  SwiftCachedCompilationHandle &Comp = *unwrap(comp);
  SwiftCachedReplayResult *Result = new SwiftCachedReplayResult();

  if (auto err = replayCompilation(Instance, Comp.DB.getCAS(), Comp,
                                   Result->outOS, Result->errOS)) {
    *error =
        swift::c_string_utils::create_clone(toString(std::move(err)).c_str());
    delete Result;
    return nullptr;
  }
  *error = swift::c_string_utils::create_null();
  return wrap(Result);
}

swiftscan_string_ref_t swiftscan_cache_replay_result_get_stdout(
    swiftscan_cache_replay_result_t result) {
  return createNonOwningString(unwrap(result)->outOS.str());
}

swiftscan_string_ref_t swiftscan_cache_replay_result_get_stderr(
    swiftscan_cache_replay_result_t result) {
  return createNonOwningString(unwrap(result)->errOS.str());
}

void swiftscan_cached_output_dispose(swiftscan_cached_output_t out) {
  delete unwrap(out);
}

void swiftscan_cache_replay_result_dispose(
    swiftscan_cache_replay_result_t result) {
  delete unwrap(result);
}

// FIXME: implement cancellation.
void swiftscan_cache_action_cancel(swiftscan_cache_cancellation_token_t) {}
void swiftscan_cache_cancellation_token_dispose(
    swiftscan_cache_cancellation_token_t) {}

llvm::Expected<SwiftScanCAS *>
SwiftScanCAS::createSwiftScanCAS(llvm::StringRef Path) {
  clang::CASOptions Opts;
  Opts.CASPath = Path;

  return createSwiftScanCAS(Opts);
}

llvm::Expected<SwiftScanCAS *>
SwiftScanCAS::createSwiftScanCAS(clang::CASOptions &CASOpts) {
  auto DB = CASOpts.getOrCreateDatabases();
  if (!DB)
    return DB.takeError();

  return new SwiftScanCAS(std::move(DB->first), std::move(DB->second));
}
