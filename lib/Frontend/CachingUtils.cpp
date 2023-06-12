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
#include "swift/Basic/FileTypes.h"
#include "swift/Frontend/CompileJobCacheKey.h"
#include "clang/CAS/IncludeTree.h"
#include "clang/Frontend/CompileJobCacheResult.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/CAS/BuiltinUnifiedCASDatabases.h"
#include "llvm/CAS/CASFileSystem.h"
#include "llvm/CAS/HierarchicalTreeBuilder.h"
#include "llvm/CAS/ObjectStore.h"
#include "llvm/CAS/TreeEntry.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/VirtualFileSystem.h"
#include "llvm/Support/VirtualOutputBackends.h"
#include "llvm/Support/VirtualOutputFile.h"
#include <memory>

#define DEBUG_TYPE "cache-util"

using namespace swift;
using namespace llvm;
using namespace llvm::cas;
using namespace llvm::vfs;

namespace {
class SwiftCASOutputFile final : public OutputFileImpl {
public:
  Error keep() override { return OnKeep(Path, Bytes); }
  Error discard() override { return Error::success(); }
  raw_pwrite_stream &getOS() override { return OS; }

  using OnKeepType = llvm::unique_function<Error(StringRef, StringRef)>;
  SwiftCASOutputFile(StringRef Path, OnKeepType OnKeep)
      : Path(Path.str()), OS(Bytes), OnKeep(std::move(OnKeep)) {}

private:
  std::string Path;
  SmallString<16> Bytes;
  raw_svector_ostream OS;
  OnKeepType OnKeep;
};

class SwiftCASOutputBackend final : public OutputBackend {
  void anchor() override {}

protected:
  IntrusiveRefCntPtr<OutputBackend> cloneImpl() const override {
    return makeIntrusiveRefCnt<SwiftCASOutputBackend>(CAS, Cache, BaseKey,
                                                      InputsAndOutputs);
  }

  Expected<std::unique_ptr<OutputFileImpl>>
  createFileImpl(StringRef ResolvedPath,
                 Optional<OutputConfig> Config) override {
    auto ProducingInput = OutputToInputMap.find(ResolvedPath);
    assert(ProducingInput != OutputToInputMap.end() && "Unknown output file");

    std::string InputFilename = ProducingInput->second.first.getFileName();
    auto OutputType = ProducingInput->second.second;

    // Uncached output kind.
    if (OutputType == file_types::ID::TY_SerializedDiagnostics)
      return std::make_unique<llvm::vfs::NullOutputFileImpl>();

    return std::make_unique<SwiftCASOutputFile>(
        ResolvedPath, [=](StringRef Path, StringRef Bytes) -> Error {
          return storeCachedCompilerOutput(CAS, Cache, Path, Bytes, BaseKey,
                                           InputFilename, OutputType);
        });
  }

private:
  void initBackend(const FrontendInputsAndOutputs &InputsAndOutputs) {
    // FIXME: The output to input map might not be enough for example all the
    // outputs can be written to `-`, but the backend cannot distinguish which
    // input it actually comes from. Maybe the solution is just not to cache
    // any commands write output to `-`.
    file_types::ID mainOutputType = InputsAndOutputs.getPrincipalOutputType();
    auto addInput = [&](const InputFile &Input) {
      if (!Input.outputFilename().empty())
        OutputToInputMap.insert(
            {Input.outputFilename(), {Input, mainOutputType}});
      Input.getPrimarySpecificPaths()
          .SupplementaryOutputs.forEachSetOutputAndType(
              [&](const std::string &Out, file_types::ID ID) {
                OutputToInputMap.insert({Out, {Input, ID}});
              });
    };
    llvm::for_each(InputsAndOutputs.getAllInputs(), addInput);
  }

  file_types::ID getOutputFileType(StringRef Path) const {
    return file_types::lookupTypeForExtension(llvm::sys::path::extension(Path));
  }

public:
  SwiftCASOutputBackend(ObjectStore &CAS, ActionCache &Cache, ObjectRef BaseKey,
                        const FrontendInputsAndOutputs &InputsAndOutputs)
      : CAS(CAS), Cache(Cache), BaseKey(BaseKey),
        InputsAndOutputs(InputsAndOutputs) {
    initBackend(InputsAndOutputs);
  }

private:
  ObjectStore &CAS;
  ActionCache &Cache;
  ObjectRef BaseKey;

  StringMap<std::pair<const InputFile &, file_types::ID>> OutputToInputMap;
  const FrontendInputsAndOutputs &InputsAndOutputs;
};
}

namespace swift {

llvm::IntrusiveRefCntPtr<llvm::vfs::OutputBackend>
createSwiftCachingOutputBackend(
    llvm::cas::ObjectStore &CAS, llvm::cas::ActionCache &Cache,
    llvm::cas::ObjectRef BaseKey,
    const FrontendInputsAndOutputs &InputsAndOutputs) {
  return makeIntrusiveRefCnt<SwiftCASOutputBackend>(CAS, Cache, BaseKey,
                                                    InputsAndOutputs);
}

bool replayCachedCompilerOutputs(
    ObjectStore &CAS, ActionCache &Cache, ObjectRef BaseKey,
    DiagnosticEngine &Diag, const FrontendInputsAndOutputs &InputsAndOutputs,
    CachingDiagnosticsProcessor &CDP) {
  clang::cas::CompileJobResultSchema Schema(CAS);
  bool CanReplayAllOutput = true;
  struct OutputEntry {
    std::string Path;
    std::string Key;
    llvm::cas::ObjectProxy Proxy;
  };
  SmallVector<OutputEntry> OutputProxies;

  auto replayOutputFile = [&](StringRef InputName, file_types::ID OutputKind,
                              StringRef OutputPath) -> Optional<OutputEntry> {
    LLVM_DEBUG(llvm::dbgs()
                   << "DEBUG: lookup output \'" << OutputPath << "\' type \'"
                   << file_types::getTypeName(OutputKind) << "\' input \'"
                   << InputName << "\n";);

    auto OutputKey =
        createCompileJobCacheKeyForOutput(CAS, BaseKey, InputName, OutputKind);
    if (!OutputKey) {
      Diag.diagnose(SourceLoc(), diag::error_cas,
                    toString(OutputKey.takeError()));
      return None;
    }
    auto OutputKeyID = CAS.getID(*OutputKey);
    auto Lookup = Cache.get(OutputKeyID);
    if (!Lookup) {
      Diag.diagnose(SourceLoc(), diag::error_cas, toString(Lookup.takeError()));
      return None;
    }
    if (!*Lookup) {
      Diag.diagnose(SourceLoc(), diag::output_cache_miss, OutputPath,
                    OutputKeyID.toString());
      return None;
    }
    auto OutputRef = CAS.getReference(**Lookup);
    if (!OutputRef) {
      return None;
    }
    auto Result = Schema.load(*OutputRef);
    if (!Result) {
      Diag.diagnose(SourceLoc(), diag::error_cas, toString(Result.takeError()));
      return None;
    }
    auto MainOutput = Result->getOutput(
        clang::cas::CompileJobCacheResult::OutputKind::MainOutput);
    if (!MainOutput) {
      return None;
    }
    auto LoadedResult = CAS.getProxy(MainOutput->Object);
    if (!LoadedResult) {
      Diag.diagnose(SourceLoc(), diag::error_cas,
                    toString(LoadedResult.takeError()));
      return None;
    }

    return OutputEntry{OutputPath.str(), OutputKeyID.toString(), *LoadedResult};
  };

  auto replayOutputFromInput = [&](const InputFile &Input) {
    auto InputPath = Input.getFileName();
    if (!Input.outputFilename().empty()) {
      if (auto Result = replayOutputFile(
              InputPath, InputsAndOutputs.getPrincipalOutputType(),
              Input.outputFilename()))
        OutputProxies.emplace_back(*Result);
      else
        CanReplayAllOutput = false;
    }

    Input.getPrimarySpecificPaths()
        .SupplementaryOutputs.forEachSetOutputAndType(
            [&](const std::string &File, file_types::ID ID) {
              if (ID == file_types::ID::TY_SerializedDiagnostics)
                return;

              if (auto Result = replayOutputFile(InputPath, ID, File))
                OutputProxies.emplace_back(*Result);
              else
                CanReplayAllOutput = false;
            });
  };

  llvm::for_each(InputsAndOutputs.getAllInputs(), replayOutputFromInput);

  auto DiagnosticsOutput = replayOutputFile(
      "<cached-diagnostics>", file_types::ID::TY_CachedDiagnostics,
      "<cached-diagnostics>");
  if (!DiagnosticsOutput)
    CanReplayAllOutput = false;

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
    Diag.diagnose(SourceLoc(), diag::replay_output, Output.Path, Output.Key);
  }

  return true;
}

static Expected<std::unique_ptr<llvm::MemoryBuffer>>
loadCachedCompileResultFromCacheKeyImpl(ObjectStore &CAS, ActionCache &Cache,
                                        StringRef CacheKey,
                                        StringRef Filename) {
  auto ID = CAS.parseID(CacheKey);
  if (!ID)
    return ID.takeError();

  auto Result = Cache.get(*ID);
  if (!Result)
    return Result.takeError();
  if (!*Result)
    return nullptr;

  auto OutputRef = CAS.getReference(**Result);
  if (!OutputRef)
    return nullptr;
  clang::cas::CompileJobResultSchema Schema(CAS);
  auto CachedOutput = Schema.load(*OutputRef);

  if (!CachedOutput)
    return CachedOutput.takeError();

  auto Output = CachedOutput->getOutput(
      clang::cas::CompileJobCacheResult::OutputKind::MainOutput);
  if (!Output)
    return nullptr;

  auto Proxy = CAS.getProxy(Output->Object);
  if (!Proxy)
    return Proxy.takeError();

  return Proxy->getMemoryBuffer(Filename);
}

std::unique_ptr<llvm::MemoryBuffer>
loadCachedCompileResultFromCacheKey(ObjectStore &CAS, ActionCache &Cache,
                                    DiagnosticEngine &Diag, StringRef CacheKey,
                                    StringRef Filename) {
  auto Output =
      loadCachedCompileResultFromCacheKeyImpl(CAS, Cache, CacheKey, Filename);
  if (!Output) {
    Diag.diagnose(SourceLoc(), diag::error_cas, toString(Output.takeError()));
    return nullptr;
  }
  return std::move(*Output);
}

Error storeCachedCompilerOutput(llvm::cas::ObjectStore &CAS,
                                llvm::cas::ActionCache &Cache, StringRef Path,
                                StringRef Bytes, llvm::cas::ObjectRef BaseKey,
                                StringRef CorrespondingInput,
                                file_types::ID OutputKind) {
  Optional<ObjectRef> BytesRef;
  if (Error E = CAS.storeFromString(None, Bytes).moveInto(BytesRef))
    return E;

  auto CacheKey = createCompileJobCacheKeyForOutput(
      CAS, BaseKey, CorrespondingInput, OutputKind);
  if (!CacheKey)
    return CacheKey.takeError();

  LLVM_DEBUG(llvm::dbgs() << "DEBUG: writing output \'" << Path << "\' type \'"
                          << file_types::getTypeName(OutputKind)
                          << "\' input \'" << CorrespondingInput << "\' hash \'"
                          << CAS.getID(*CacheKey).toString() << "\'\n";);

  // Use clang compiler job output for now.
  clang::cas::CompileJobCacheResult::Builder Builder;
  Builder.addOutput(clang::cas::CompileJobCacheResult::OutputKind::MainOutput,
                    *BytesRef);
  auto Result = Builder.build(CAS);
  if (!Result)
    return Result.takeError();

  if (auto E = Cache.put(CAS.getID(*CacheKey), CAS.getID(*Result)))
    return E;

  return Error::success();
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
    auto Ref = CAS.getReference(*ID);
    if (!Ref)
      return createCASObjectNotFoundError(*ID);
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

namespace cas {

CachingTool::CachingTool(StringRef Path) {
  auto DB = llvm::cas::createOnDiskUnifiedCASDatabases(Path);
  if (!DB) {
    llvm::errs() << "Failed to create CAS at " << Path << ": "
                 << toString(DB.takeError()) << "\n";
    return;
  }

  CAS = std::move(DB->first);
  Cache = std::move(DB->second);
}

std::string CachingTool::computeCacheKey(ArrayRef<const char *> Args,
                                         StringRef InputPath,
                                         file_types::ID OutputKind) {
  auto BaseKey = createCompileJobBaseCacheKey(*CAS, Args);
  if (!BaseKey) {
    llvm::errs() << "Failed to create cache key: "
                 << toString(BaseKey.takeError()) << "\n";
    return "";
  }

  auto Key =
      createCompileJobCacheKeyForOutput(*CAS, *BaseKey, InputPath, OutputKind);
  if (!Key) {
    llvm::errs() << "Failed to create cache key: " << toString(Key.takeError())
                 << "\n";
    return "";
  }

  return CAS->getID(*Key).toString();
}

std::string CachingTool::storeContent(StringRef Content) {
  auto Result = CAS->storeFromString({}, Content);
  if (!Result) {
    llvm::errs() << "Failed to store to CAS: " << toString(Result.takeError())
                 << "\n";
    return "";
  }

  return CAS->getID(*Result).toString();
}

} // namespace cas
} // namespace swift
