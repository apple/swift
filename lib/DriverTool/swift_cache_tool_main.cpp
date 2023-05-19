//===--- swift_cache_tool_main.cpp - Swift caching tool for inspection ----===//
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
//
// Utility tool for inspecting and accessing swift cache.
//
//===----------------------------------------------------------------------===//
//
#include "swift/AST/DiagnosticsFrontend.h"
#include "swift/Basic/LLVM.h"
#include "swift/Basic/LLVMInitialize.h"
#include "swift/Basic/Version.h"
#include "swift/Frontend/CachingUtils.h"
#include "swift/Frontend/CompileJobCacheKey.h"
#include "swift/Frontend/Frontend.h"
#include "swift/Frontend/PrintingDiagnosticConsumer.h"
#include "swift/Parse/ParseVersion.h"
#include "llvm/CAS/ActionCache.h"
#include "llvm/CAS/BuiltinUnifiedCASDatabases.h"
#include "llvm/CAS/ObjectStore.h"
#include "llvm/Option/OptTable.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/MemoryBuffer.h"
#include <memory>

using namespace swift;
using namespace llvm::opt;
using namespace llvm::cas;

namespace {

enum class SwiftCacheToolAction {
  Invalid,
  PrintBaseKey,
  PrintOutputKeys,
  ValidateOutputs,
  RenderDiags
};

struct OutputEntry {
  std::string InputPath;
  std::string OutputPath;
  std::string OutputKind;
  std::string CacheKey;
};

enum ID {
  OPT_INVALID = 0, // This is not an option ID.
#define OPTION(PREFIX, NAME, ID, KIND, GROUP, ALIAS, ALIASARGS, FLAGS, PARAM,  \
               HELPTEXT, METAVAR, VALUES)                                      \
  OPT_##ID,
#include "SwiftCacheToolOptions.inc"
  LastOption
#undef OPTION
};

#define PREFIX(NAME, VALUE) static const char *const NAME[] = VALUE;
#include "SwiftCacheToolOptions.inc"
#undef PREFIX

static const OptTable::Info InfoTable[] = {
#define OPTION(PREFIX, NAME, ID, KIND, GROUP, ALIAS, ALIASARGS, FLAGS, PARAM,  \
               HELPTEXT, METAVAR, VALUES)                                      \
  {PREFIX, NAME,  HELPTEXT,    METAVAR,     OPT_##ID,  Option::KIND##Class,    \
   PARAM,  FLAGS, OPT_##GROUP, OPT_##ALIAS, ALIASARGS, VALUES},
#include "SwiftCacheToolOptions.inc"
#undef OPTION
};

class CacheToolOptTable : public llvm::opt::OptTable {
public:
  CacheToolOptTable() : OptTable(InfoTable) {}
};

class SwiftCacheToolInvocation {
private:
  CompilerInstance Instance;
  CompilerInvocation Invocation;
  PrintingDiagnosticConsumer PDC;
  std::string MainExecutablePath;
  std::string CASPath;
  std::vector<std::string> Inputs;
  std::vector<std::string> FrontendArgs;
  SwiftCacheToolAction ActionKind = SwiftCacheToolAction::Invalid;

public:
  SwiftCacheToolInvocation(const std::string &ExecPath)
      : MainExecutablePath(ExecPath) {
    Instance.addDiagnosticConsumer(&PDC);
  }

  std::unique_ptr<llvm::opt::OptTable> createOptTable() {
    return std::unique_ptr<OptTable>(new CacheToolOptTable());
  }

  int parseArgs(ArrayRef<const char *> Args) {
    auto &Diags = Instance.getDiags();

    std::unique_ptr<llvm::opt::OptTable> Table = createOptTable();
    unsigned MissingIndex;
    unsigned MissingCount;
    llvm::opt::InputArgList ParsedArgs =
        Table->ParseArgs(Args, MissingIndex, MissingCount);
    if (MissingCount) {
      Diags.diagnose(SourceLoc(), diag::error_missing_arg_value,
                     ParsedArgs.getArgString(MissingIndex), MissingCount);
      return 1;
    }

    if (ParsedArgs.getLastArg(OPT_help)) {
      std::string ExecutableName =
          llvm::sys::path::stem(MainExecutablePath).str();
      Table->printHelp(llvm::outs(), ExecutableName.c_str(), "Swift Cache Tool",
                       0, 0, /*ShowAllAliases*/ false);
      return 0;
    }

    CASPath =
        ParsedArgs.getLastArgValue(OPT_cas_path, getDefaultOnDiskCASPath());

    Inputs = ParsedArgs.getAllArgValues(OPT_INPUT);
    FrontendArgs = ParsedArgs.getAllArgValues(OPT__DASH_DASH);
    if (auto *A = ParsedArgs.getLastArg(OPT_cache_tool_action))
      ActionKind =
          llvm::StringSwitch<SwiftCacheToolAction>(A->getValue())
              .Case("print-base-key", SwiftCacheToolAction::PrintBaseKey)
              .Case("print-output-keys", SwiftCacheToolAction::PrintOutputKeys)
              .Case("validate-outputs", SwiftCacheToolAction::ValidateOutputs)
              .Case("render-diags", SwiftCacheToolAction::RenderDiags)
              .Default(SwiftCacheToolAction::Invalid);

    if (ActionKind == SwiftCacheToolAction::Invalid) {
      llvm::errs()
          << "Invalid option specified for -cache-tool-action: "
          << "print-base-key|print-output-keys|validate-outputs|render-diags\n";
      return 1;
    }

    return 0;
  }

  int run() {
    switch (ActionKind) {
    case SwiftCacheToolAction::PrintBaseKey:
      return printBaseKey();
    case SwiftCacheToolAction::PrintOutputKeys:
      return printOutputKeys();
    case SwiftCacheToolAction::ValidateOutputs:
      return validateOutputs();
    case SwiftCacheToolAction::RenderDiags:
      return renderDiags();
    case SwiftCacheToolAction::Invalid:
      return 0; // No action. Probably just print help. Return.
    }
  }

private:
  bool setupCompiler() {
    // Setup invocation.
    SmallString<128> workingDirectory;
    llvm::sys::fs::current_path(workingDirectory);

    // Parse arguments.
    if (FrontendArgs.empty()) {
      llvm::errs() << "missing swift-frontend command-line after --\n";
      return true;
    }
    // drop swift-frontend executable path from command-line.
    if (StringRef(FrontendArgs[0]).endswith("swift-frontend"))
      FrontendArgs.erase(FrontendArgs.begin());

    SmallVector<std::unique_ptr<llvm::MemoryBuffer>, 4>
        configurationFileBuffers;
    std::vector<const char*> Args;
    for (auto &A: FrontendArgs)
      Args.emplace_back(A.c_str());

    // Make sure CASPath is the same between invocation and cache-tool by
    // appending the cas-path since the option doesn't affect cache key.
    Args.emplace_back("-cas-path");
    Args.emplace_back(CASPath.c_str());

    if (Invocation.parseArgs(Args, Instance.getDiags(),
                             &configurationFileBuffers, workingDirectory,
                             MainExecutablePath))
      return true;

    if (!Invocation.getFrontendOptions().EnableCAS) {
      llvm::errs() << "Requested command-line arguments do not enable CAS\n";
      return true;
    }

    // Setup instance.
    std::string InstanceSetupError;
    if (Instance.setup(Invocation, InstanceSetupError, Args)) {
      llvm::errs() << "swift-frontend invocation setup error: "
                   << InstanceSetupError << "\n";
      return true;
    }

    // Disable diagnostic caching from this fake instance.
    if (auto *CDP = Instance.getCachingDiagnosticsProcessor())
      CDP->endDiagnosticCapture();

    return false;
  }

  Optional<ObjectRef> getBaseKey() {
    auto BaseKey = Instance.getCompilerBaseKey();
    if (!BaseKey) {
      Instance.getDiags().diagnose(SourceLoc(), diag::error_cas,
                                   "Base Key doesn't exist");
      return None;
    }

    return *BaseKey;
  }

  int printBaseKey() {
    if (setupCompiler())
      return 1;

    auto &CAS = Instance.getObjectStore();
    auto BaseKey = getBaseKey();
    if (!BaseKey)
      return 1;

    if (ActionKind == SwiftCacheToolAction::PrintBaseKey)
      llvm::outs() << CAS.getID(*BaseKey).toString() << "\n";

    return 0;
  }

  int printOutputKeys();
  int validateOutputs();
  int renderDiags();
};

} // end anonymous namespace

int SwiftCacheToolInvocation::printOutputKeys() {
  if (setupCompiler())
    return 1;

  auto &CAS = Instance.getObjectStore();
  auto BaseKey = getBaseKey();
  if (!BaseKey)
    return 1;

  std::vector<OutputEntry> OutputKeys;
  bool hasError = false;
  auto addOutputKey = [&](StringRef InputPath, file_types::ID OutputKind,
                          StringRef OutputPath) {
    auto OutputKey =
        createCompileJobCacheKeyForOutput(CAS, *BaseKey, InputPath, OutputKind);
    if (!OutputKey) {
      llvm::errs() << "cannot create cache key for " << OutputPath << ": "
                   << toString(OutputKey.takeError()) << "\n";
      hasError = true;
    }
    OutputKeys.emplace_back(
        OutputEntry{InputPath.str(), OutputPath.str(),
                    file_types::getTypeName(OutputKind).str(),
                    CAS.getID(*OutputKey).toString()});
  };
  auto addFromInputFile = [&](const InputFile &Input) {
    auto InputPath = Input.getFileName();
    if (!Input.outputFilename().empty())
      addOutputKey(InputPath,
                   Invocation.getFrontendOptions()
                       .InputsAndOutputs.getPrincipalOutputType(),
                   Input.outputFilename());
    Input.getPrimarySpecificPaths()
        .SupplementaryOutputs.forEachSetOutputAndType(
            [&](const std::string &File, file_types::ID ID) {
              // Dont print serialized diagnostics.
              if (ID == file_types::ID::TY_SerializedDiagnostics)
                return;

              addOutputKey(InputPath, ID, File);
            });
  };
  llvm::for_each(
      Invocation.getFrontendOptions().InputsAndOutputs.getAllInputs(),
      addFromInputFile);

  // Add diagnostics file.
  addOutputKey("<cached-diagnostics>", file_types::ID::TY_CachedDiagnostics,
               "<cached-diagnostics>");

  if (hasError)
    return 1;

  llvm::json::OStream Out(llvm::outs(), /*IndentSize=*/4);
  Out.array([&] {
    for (const auto &E : OutputKeys) {
      Out.object([&] {
        Out.attribute("OutputPath", E.OutputPath);
        Out.attribute("OutputKind", E.OutputKind);
        Out.attribute("Input", E.InputPath);
        Out.attribute("CacheKey", E.CacheKey);
      });
    }
  });

  return 0;
}

static llvm::Expected<llvm::json::Array>
readOutputEntriesFromFile(StringRef Path) {
  auto JSONContent = llvm::MemoryBuffer::getFile(Path);
  if (!JSONContent)
    return llvm::createStringError(JSONContent.getError(),
                                   "failed to read input file");

  auto JSONValue = llvm::json::parse((*JSONContent)->getBuffer());
  if (!JSONValue)
    return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                   "failed to parse input file as JSON");

  auto Keys = JSONValue->getAsArray();
  if (!Keys)
    return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                   "invalid JSON format for input file");

  return *Keys;
}

int SwiftCacheToolInvocation::validateOutputs() {
  auto DB = llvm::cas::createOnDiskUnifiedCASDatabases(CASPath);
  if (!DB)
    report_fatal_error(DB.takeError());

  PrintingDiagnosticConsumer PDC;
  Instance.getDiags().addConsumer(PDC);

  auto validateCacheKeysFromFile = [&](const std::string &Path) {
    auto Keys = readOutputEntriesFromFile(Path);
    if (!Keys) {
      llvm::errs() << "cannot read file " << Path << ": "
                   << toString(Keys.takeError()) << "\n";
      return true;
    }

    for (const auto& Entry : *Keys) {
      if (auto *Obj = Entry.getAsObject()) {
        if (auto Key = Obj->getString("CacheKey")) {
          if (auto Buffer = loadCachedCompileResultFromCacheKey(
                  *DB->first, *DB->second, Instance.getDiags(), *Key))
            continue;
          llvm::errs() << "failed to find output for cache key " << *Key
                       << "\n";
          return true;
        }
      }
      llvm::errs() << "can't read cache key from " << Path << "\n";
      return true;
    }

    return false;
  };

  return llvm::any_of(Inputs, validateCacheKeysFromFile);
}

int SwiftCacheToolInvocation::renderDiags() {
  if (setupCompiler())
    return 1;

  auto *CDP = Instance.getCachingDiagnosticsProcessor();
  if (!CDP) {
    llvm::errs() << "provided commandline doesn't support cached diagnostics\n";
    return 1;
  }

  auto renderDiagsFromFile = [&](const std::string &Path) {
    auto Keys = readOutputEntriesFromFile(Path);
    if (!Keys) {
      llvm::errs() << "cannot read file " << Path << ": "
                   << toString(Keys.takeError()) << "\n";
      return true;
    }

    for (const auto& Entry : *Keys) {
      if (auto *Obj = Entry.getAsObject()) {
        if (auto Kind = Obj->getString("OutputKind")) {
          if (*Kind != "cached-diagnostics")
            continue;
        }
        if (auto Key = Obj->getString("CacheKey")) {
          if (auto Buffer = loadCachedCompileResultFromCacheKey(
                  Instance.getObjectStore(), Instance.getActionCache(),
                  Instance.getDiags(), *Key)) {
            if (auto E = CDP->replayCachedDiagnostics(Buffer->getBuffer())) {
              llvm::errs() << "failed to replay cache: "
                           << toString(std::move(E)) << "\n";
              return true;
            }
            return false;
          }
        }
      }
    }
    llvm::errs() << "cannot locate cached diagnostics in file\n";
    return true;
  };

  return llvm::any_of(Inputs, renderDiagsFromFile);
}

int swift_cache_tool_main(ArrayRef<const char *> Args, const char *Argv0,
                          void *MainAddr) {
  INITIALIZE_LLVM();

  SwiftCacheToolInvocation Invocation(
      llvm::sys::fs::getMainExecutable(Argv0, MainAddr));

  if (Invocation.parseArgs(Args) != 0)
    return EXIT_FAILURE;

  if (Invocation.run() != 0)
    return EXIT_FAILURE;

  return EXIT_SUCCESS;
}
