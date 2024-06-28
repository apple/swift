//===--- CachingUtils.cpp ---------------------------------------*- C++ -*-===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2018 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

#include "swift/Frontend/CachingUtils.h"

#include "swift/AST/DiagnosticsFrontend.h"
#include "swift/Basic/Assertions.h"
#include "swift/Basic/FileTypes.h"
#include "swift/Basic/LLVM.h"
#include "swift/Frontend/CASOutputBackends.h"
#include "swift/Frontend/CompileJobCacheKey.h"
#include "swift/Frontend/CompileJobCacheResult.h"
#include "swift/Frontend/FrontendOptions.h"
#include "swift/Option/Options.h"
#include "clang/CAS/CASOptions.h"
#include "clang/CAS/IncludeTree.h"
#include "clang/Frontend/CompileJobCacheResult.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/CAS/BuiltinUnifiedCASDatabases.h"
#include "llvm/CAS/CASFileSystem.h"
#include "llvm/CAS/HierarchicalTreeBuilder.h"
#include "llvm/CAS/ObjectStore.h"
#include "llvm/CAS/TreeEntry.h"
#include "llvm/Option/ArgList.h"
#include "llvm/Option/OptTable.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/VirtualFileSystem.h"
#include "llvm/Support/VirtualOutputBackends.h"
#include "llvm/Support/VirtualOutputFile.h"
#include <memory>

#define DEBUG_TYPE "cache-util"

using namespace swift;
using namespace swift::cas;
using namespace llvm;
using namespace llvm::cas;
using namespace llvm::vfs;

namespace swift {

llvm::IntrusiveRefCntPtr<SwiftCASOutputBackend>
createSwiftCachingOutputBackend(
    llvm::cas::ObjectStore &CAS, llvm::cas::ActionCache &Cache,
    llvm::cas::ObjectRef BaseKey,
    const FrontendInputsAndOutputs &InputsAndOutputs,
    FrontendOptions::ActionType Action) {
  return makeIntrusiveRefCnt<SwiftCASOutputBackend>(CAS, Cache, BaseKey,
                                                    InputsAndOutputs, Action);
}

Error cas::CachedResultLoader::replay(CallbackTy Callback) {
  auto ResultProxy = CAS.getProxy(OutputRef);
  if (!ResultProxy)
    return ResultProxy.takeError();

  {
    swift::cas::CompileJobResultSchema Schema(CAS);
    if (Schema.isRootNode(*ResultProxy)) {
      auto Result = Schema.load(OutputRef);
      if (!Result)
        return Result.takeError();

      if (auto Err = Result->forEachOutput(
              [&](swift::cas::CompileJobCacheResult::Output Output) -> Error {
                return Callback(Output.Kind, Output.Object);
              }))
        return Err;

      return Error::success();
    }
  }
  {
    clang::cas::CompileJobResultSchema Schema(CAS);
    if (Schema.isRootNode(*ResultProxy)) {
      auto Result = Schema.load(OutputRef);
      if (!Result)
        return Result.takeError();
      if (auto Err = Result->forEachOutput(
          [&](clang::cas::CompileJobCacheResult::Output Output) -> Error {
            file_types::ID OutputKind = file_types::ID::TY_INVALID;
            switch (Output.Kind) {
            case clang::cas::CompileJobCacheResult::OutputKind::MainOutput:
              OutputKind = file_types::ID::TY_ClangModuleFile;
              break;
            case clang::cas::CompileJobCacheResult::OutputKind::Dependencies:
              OutputKind = file_types::ID::TY_Dependencies;
              break;
            case clang::cas::CompileJobCacheResult::OutputKind::
                SerializedDiagnostics:
              OutputKind = file_types::ID::TY_CachedDiagnostics;
              break;
            }
            assert(OutputKind != file_types::ID::TY_INVALID &&
                   "Unexpected output kind in clang cached result");
            return Callback(OutputKind, Output.Object);
          }))
        return Err;

      return Error::success();
    }
  }

  return createStringError(inconvertibleErrorCode(),
                           "unexpected output schema for cached result");
}

static Expected<std::optional<ObjectRef>>
lookupCacheKey(ObjectStore &CAS, ActionCache &Cache, ObjectRef CacheKey) {
  // Lookup the cache key for the input file.
  auto OutID = CAS.getID(CacheKey);
  auto Lookup = Cache.get(OutID);
  if (!Lookup)
    return Lookup.takeError();

  if (!*Lookup)
    return std::nullopt;

  return CAS.getReference(**Lookup);
}

bool replayCachedCompilerOutputs(
    ObjectStore &CAS, ActionCache &Cache, ObjectRef BaseKey,
    DiagnosticEngine &Diag, const FrontendInputsAndOutputs &InputsAndOutputs,
    CachingDiagnosticsProcessor &CDP, bool CacheRemarks) {
  bool CanReplayAllOutput = true;
  struct OutputEntry {
    std::string Path;
    CASID Key;
    ObjectProxy Proxy;
  };
  SmallVector<OutputEntry> OutputProxies;
  std::optional<OutputEntry> DiagnosticsOutput;

  auto replayOutputsForInputFile = [&](const std::string &InputPath,
                                       unsigned InputIndex,
                                       const DenseMap<file_types::ID,
                                                      std::string> &Outputs) {
    auto lookupFailed = [&CanReplayAllOutput] { CanReplayAllOutput = false; };
    auto OutputKey =
        createCompileJobCacheKeyForOutput(CAS, BaseKey, InputIndex);

    if (!OutputKey) {
      Diag.diagnose(SourceLoc(), diag::error_cas,
                    toString(OutputKey.takeError()));
      return lookupFailed();
    }

    auto OutID = CAS.getID(*OutputKey);
    auto OutputRef = lookupCacheKey(CAS, Cache, *OutputKey);
    if (!OutputRef) {
      Diag.diagnose(SourceLoc(), diag::error_cas,
                    toString(OutputRef.takeError()));
      return lookupFailed();
    }

    if (!*OutputRef) {
      if (CacheRemarks)
        Diag.diagnose(SourceLoc(), diag::output_cache_miss, InputPath,
                      OutID.toString());
      return lookupFailed();
    }

    CachedResultLoader Loader(CAS, **OutputRef);
    LLVM_DEBUG(llvm::dbgs() << "DEBUG: lookup cache key \'" << OutID.toString()
                            << "\' for input \'" << InputPath << "\n";);

    if (auto Err = Loader.replay([&](file_types::ID Kind,
                                     ObjectRef Ref) -> Error {
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
            DiagnosticsOutput = OutputEntry{OutputPath->second, OutID, *Proxy};
          } else
            OutputProxies.emplace_back(
                OutputEntry{OutputPath->second, OutID, *Proxy});
          return Error::success();
        })) {
      Diag.diagnose(SourceLoc(), diag::error_cas, toString(std::move(Err)));
      return lookupFailed();
    }
  };

  auto replayOutputFromInput = [&](const InputFile &Input,
                                   unsigned InputIndex) {
    auto InputPath = Input.getFileName();
    DenseMap<file_types::ID, std::string> Outputs;
    if (!Input.outputFilename().empty())
      Outputs.try_emplace(InputsAndOutputs.getPrincipalOutputType(),
                          Input.outputFilename());

    Input.getPrimarySpecificPaths()
        .SupplementaryOutputs.forEachSetOutputAndType(
            [&](const std::string &File, file_types::ID ID) {
              if (file_types::isProducedFromDiagnostics(ID))
                return;

              Outputs.try_emplace(ID, File);
            });

    // If this input doesn't produce any outputs, don't try to look up cache.
    // This can be a standalone emitModule action that only one input produces
    // output.
    if (Outputs.empty())
      return;

    // Add cached diagnostic entry for lookup. Output path doesn't matter here.
    Outputs.try_emplace(file_types::ID::TY_CachedDiagnostics,
                        "<cached-diagnostics>");

    return replayOutputsForInputFile(InputPath, InputIndex, Outputs);
  };

  auto AllInputs = InputsAndOutputs.getAllInputs();
  // If there are primary inputs, look up only the primary input files.
  // Otherwise, prepare to do cache lookup for all inputs.
  for (unsigned Index = 0; Index < AllInputs.size(); ++Index) {
    const auto &Input = AllInputs[Index];
    if (InputsAndOutputs.hasPrimaryInputs() && !Input.isPrimary())
      continue;

    replayOutputFromInput(Input, Index);
  }

  if (!CanReplayAllOutput)
    return false;

  // If there is not diagnostic output, this is a job that produces no output
  // and only diagnostics, like `typecheck-module-from-interface`, look up
  // diagnostics from first file.
  if (!DiagnosticsOutput)
    replayOutputsForInputFile(
        "<cached-diagnostics>",
        InputsAndOutputs.getIndexOfFirstOutputProducingInput(),
        {{file_types::ID::TY_CachedDiagnostics, "<cached-diagnostics>"}});

  // Check again to make sure diagnostics is fetched successfully.
  if (!CanReplayAllOutput)
    return false;

  // Replay Diagnostics first so the output failures comes after.
  // Also if the diagnostics replay failed, proceed to re-compile.
  if (auto E = CDP.replayCachedDiagnostics(
          DiagnosticsOutput->Proxy.getData())) {
    Diag.diagnose(SourceLoc(), diag::error_replay_cached_diag,
                  toString(std::move(E)));
    return false;
  }

  if (CacheRemarks)
    Diag.diagnose(SourceLoc(), diag::replay_output, "<cached-diagnostics>",
                  DiagnosticsOutput->Key.toString());

  // Replay the result only when everything is resolved.
  // Use on disk output backend directly here to write to disk.
  llvm::vfs::OnDiskOutputBackend Backend;
  for (auto &Output : OutputProxies) {
    auto File = Backend.createFile(Output.Path);
    if (!File) {
      Diag.diagnose(SourceLoc(), diag::error_opening_output, Output.Path,
                    toString(File.takeError()));
      continue;
    }
    *File << Output.Proxy.getData();
    if (auto E = File->keep()) {
      Diag.diagnose(SourceLoc(), diag::error_closing_output, Output.Path,
                    toString(std::move(E)));
      continue;
    }
    if (CacheRemarks)
      Diag.diagnose(SourceLoc(), diag::replay_output, Output.Path,
                    Output.Key.toString());
  }

  return true;
}

std::unique_ptr<llvm::MemoryBuffer>
loadCachedCompileResultFromCacheKey(ObjectStore &CAS, ActionCache &Cache,
                                    DiagnosticEngine &Diag, StringRef CacheKey,
                                    file_types::ID Kind, StringRef Filename) {
  auto failure = [&](Error Err) {
    Diag.diagnose(SourceLoc(), diag::error_cas, toString(std::move(Err)));
    return nullptr;
  };
  auto ID = CAS.parseID(CacheKey);
  if (!ID)
    return failure(ID.takeError());
  auto Ref = CAS.getReference(*ID);
  if (!Ref)
    return nullptr;

  auto OutputRef = lookupCacheKey(CAS, Cache, *Ref);
  if (!OutputRef)
    return failure(OutputRef.takeError());

  if (!*OutputRef)
    return nullptr;

  CachedResultLoader Loader(CAS, **OutputRef);
  std::unique_ptr<llvm::MemoryBuffer> Buffer;
  if (auto Err =
          Loader.replay([&](file_types::ID Type, ObjectRef Ref) -> Error {
            if (Kind != Type)
              return Error::success();

            auto Proxy = CAS.getProxy(Ref);
            if (!Proxy)
              return Proxy.takeError();

            Buffer = Proxy->getMemoryBuffer(Filename);
            return Error::success();
          }))
    return failure(std::move(Err));

  return Buffer;
}

static llvm::Error createCASObjectNotFoundError(const llvm::cas::CASID &ID) {
  return createStringError(llvm::inconvertibleErrorCode(),
                           "CASID missing from Object Store " + ID.toString());
}

static Expected<ObjectRef> mergeCASFileSystem(ObjectStore &CAS,
                                              ArrayRef<std::string> FSRoots) {
  llvm::cas::HierarchicalTreeBuilder Builder;
  for (auto &Root : FSRoots) {
    auto ID = CAS.parseID(Root);
    if (!ID)
      return ID.takeError();

    auto Ref = CAS.getReference(*ID);
    if (!Ref)
      return createCASObjectNotFoundError(*ID);
    Builder.pushTreeContent(*Ref, "");
  }

  auto NewRoot = Builder.create(CAS);
  if (!NewRoot)
    return NewRoot.takeError();

  return NewRoot->getRef();
}

Expected<IntrusiveRefCntPtr<vfs::FileSystem>>
createCASFileSystem(ObjectStore &CAS, ArrayRef<std::string> FSRoots,
                    ArrayRef<std::string> IncludeTrees) {
  assert(!FSRoots.empty() || !IncludeTrees.empty() && "no root ID provided");
  if (FSRoots.size() == 1 && IncludeTrees.empty()) {
    auto ID = CAS.parseID(FSRoots.front());
    if (!ID)
      return ID.takeError();
    return createCASFileSystem(CAS, *ID);
  }

  auto NewRoot = mergeCASFileSystem(CAS, FSRoots);
  if (!NewRoot)
    return NewRoot.takeError();

  auto FS = createCASFileSystem(CAS, CAS.getID(*NewRoot));
  if (!FS)
    return FS.takeError();

  auto CASFS = makeIntrusiveRefCnt<vfs::OverlayFileSystem>(std::move(*FS));
  // Push all Include File System onto overlay.
  for (auto &Tree : IncludeTrees) {
    auto ID = CAS.parseID(Tree);
    if (!ID)
      return ID.takeError();

    auto Ref = CAS.getReference(*ID);
    if (!Ref)
      return createCASObjectNotFoundError(*ID);
    auto IT = clang::cas::IncludeTreeRoot::get(CAS, *Ref);
    if (!IT)
      return IT.takeError();

    auto ITFS = clang::cas::createIncludeTreeFileSystem(*IT);
    if (!ITFS)
      return ITFS.takeError();
    CASFS->pushOverlay(std::move(*ITFS));
  }

  return CASFS;
}

std::vector<std::string> remapPathsFromCommandLine(
    ArrayRef<std::string> commandLine,
    llvm::function_ref<std::string(StringRef)> RemapCallback) {
  // parse and remap options that is path and not cache invariant.
  unsigned MissingIndex;
  unsigned MissingCount;
  std::vector<const char *> Args;
  std::for_each(commandLine.begin(), commandLine.end(),
                [&](const std::string &arg) { Args.push_back(arg.c_str()); });
  std::unique_ptr<llvm::opt::OptTable> Table = createSwiftOptTable();
  llvm::opt::InputArgList ParsedArgs = Table->ParseArgs(
      Args, MissingIndex, MissingCount, options::FrontendOption);
  SmallVector<const char *, 16> newArgs;
  std::vector<std::string> newCommandLine;
  llvm::BumpPtrAllocator Alloc;
  llvm::StringSaver Saver(Alloc);
  for (auto *Arg : ParsedArgs) {
    Arg->render(ParsedArgs, newArgs);
    const auto &Opt = Arg->getOption();
    if (Opt.matches(options::OPT_INPUT) ||
        (!Opt.hasFlag(options::CacheInvariant) &&
         Opt.hasFlag(options::ArgumentIsPath))) {
      StringRef newPath = Saver.save(RemapCallback(Arg->getValue()));
      newArgs.back() = newPath.data();
    }
  }
  std::for_each(newArgs.begin(), newArgs.end(),
                [&](const char *arg) { newCommandLine.emplace_back(arg); });

  return newCommandLine;
}

} // namespace swift
