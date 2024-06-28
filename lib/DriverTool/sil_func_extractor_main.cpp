//===--- sil_func_extractor_main.cpp --------------------------------------===//
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
///
/// \file
///
/// This utility is meant to help simplify the extraction of test cases from sil
/// files by removing (currently only) functions that do not match a list of
/// string. It also allows for the inverse to be selected. Eventually this
/// should have additional capabilities like stripping globals, vtables, etc.
///
//===----------------------------------------------------------------------===//

#define DEBUG_TYPE "sil-func-extractor"
#include "swift/Basic/Assertions.h"
#include "swift/Basic/FileTypes.h"
#include "swift/Basic/LLVM.h"
#include "swift/Basic/LLVMInitialize.h"
#include "swift/Demangling/Demangle.h"
#include "swift/Demangling/ManglingMacros.h"
#include "swift/Frontend/DiagnosticVerifier.h"
#include "swift/Frontend/Frontend.h"
#include "swift/Frontend/PrintingDiagnosticConsumer.h"
#include "swift/SIL/SILBuilder.h"
#include "swift/SIL/SILUndef.h"
#include "swift/SILOptimizer/Analysis/Analysis.h"
#include "swift/SILOptimizer/PassManager/PassManager.h"
#include "swift/SILOptimizer/PassManager/Passes.h"
#include "swift/Serialization/SerializedModuleLoader.h"
#include "swift/Serialization/SerializationOptions.h"
#include "swift/Serialization/SerializedSILLoader.h"
#include "swift/SymbolGraphGen/SymbolGraphOptions.h"
#include "swift/Subsystems.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/ManagedStatic.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/Signals.h"
#include <cstdio>

using namespace swift;

struct SILFuncExtractorOptions {
  llvm::cl::opt<std::string>
    InputFilename = llvm::cl::opt<std::string>(llvm::cl::desc("input file"),
                                                llvm::cl::init("-"),
                                                llvm::cl::Positional);

  llvm::cl::opt<std::string>
    OutputFilename = llvm::cl::opt<std::string>("o", llvm::cl::desc("output filename"), llvm::cl::init("-"));

  llvm::cl::opt<bool>
    EmitVerboseSIL = llvm::cl::opt<bool>("emit-verbose-sil",
                   llvm::cl::desc("Emit locations during sil emission."));

  llvm::cl::list<std::string>
    CommandLineFunctionNames = llvm::cl::list<std::string>("func",
                             llvm::cl::desc("Function names to extract."));
  llvm::cl::opt<std::string>
    FunctionNameFile = llvm::cl::opt<std::string>(
    "func-file", llvm::cl::desc("File to load additional function names from"));

  llvm::cl::opt<bool>
    EmitSIB = llvm::cl::opt<bool>("emit-sib",
        llvm::cl::desc("Emit a sib file as output instead of a sil file"));

  llvm::cl::opt<bool>
    InvertMatch = llvm::cl::opt<bool>(
                  "invert",
                  llvm::cl::desc("Match functions whose name do not "
                  "match the names of the functions to be extracted"));

  llvm::cl::list<std::string>
    ImportPaths = llvm::cl::list<std::string>("I",
                llvm::cl::desc("add a directory to the import search path"));

  llvm::cl::opt<std::string>
    ModuleName = llvm::cl::opt<std::string>("module-name",
               llvm::cl::desc("The name of the module if processing"
                              " a module. Necessary for processing "
                              "stdin."));

  llvm::cl::opt<std::string>
    ModuleCachePath = llvm::cl::opt<std::string>("module-cache-path",
                    llvm::cl::desc("Clang module cache path"));

  llvm::cl::opt<std::string>
    ResourceDir = llvm::cl::opt<std::string>(
                "resource-dir",
                llvm::cl::desc("The directory that holds the compiler resource files"));

  llvm::cl::opt<std::string>
    SDKPath = llvm::cl::opt<std::string>("sdk", llvm::cl::desc("The path to the SDK for use with the clang "
                                  "importer."),
              llvm::cl::init(""));

  llvm::cl::opt<std::string>
    Triple = llvm::cl::opt<std::string>("target",
                                        llvm::cl::desc("target triple"));

  llvm::cl::opt<bool>
    EmitSortedSIL = llvm::cl::opt<bool>("emit-sorted-sil", llvm::cl::Hidden, llvm::cl::init(false),
                  llvm::cl::desc("Sort Functions, VTables, Globals, "
                                 "WitnessTables by name to ease diffing."));

  llvm::cl::opt<bool>
    DisableASTDump = llvm::cl::opt<bool>("sil-disable-ast-dump", llvm::cl::Hidden,
                 llvm::cl::init(false),
                 llvm::cl::desc("Do not dump AST."));

  llvm::cl::opt<bool>
    EnableOSSAModules = llvm::cl::opt<bool>(
      "enable-ossa-modules",
      llvm::cl::desc("Do we always serialize SIL in OSSA form? If "
                     "this is disabled we do not serialize in OSSA "
                     "form when optimizing."));

  llvm::cl::opt<llvm::cl::boolOrDefault>
    EnableObjCInterop = llvm::cl::opt<llvm::cl::boolOrDefault>(
      "enable-objc-interop",
      llvm::cl::desc("Whether the Objective-C interop should be enabled. "
                     "The value is `true` by default on Darwin platforms."));
};

static void getFunctionNames(std::vector<std::string> &Names,
                             const SILFuncExtractorOptions &options) {
  std::copy(options.CommandLineFunctionNames.begin(), options.CommandLineFunctionNames.end(),
            std::back_inserter(Names));

  if (!options.FunctionNameFile.empty()) {
    llvm::ErrorOr<std::unique_ptr<llvm::MemoryBuffer>> FileBufOrErr =
        llvm::MemoryBuffer::getFileOrSTDIN(options.FunctionNameFile);
    if (!FileBufOrErr) {
      fprintf(stderr, "Error! Failed to open file: %s\n",
              options.InputFilename.c_str());
      exit(-1);
    }
    StringRef Buffer = FileBufOrErr.get()->getBuffer();
    while (!Buffer.empty()) {
      StringRef Token, NewBuffer;
      std::tie(Token, NewBuffer) = llvm::getToken(Buffer, "\n");
      if (Token.empty()) {
        break;
      }
      Names.push_back(Token.str());
      Buffer = NewBuffer;
    }
  }
}

static bool stringInSortedArray(
    StringRef str, ArrayRef<std::string> list,
    llvm::function_ref<bool(const std::string &, const std::string &)> &&cmp) {
  if (list.empty())
    return false;
  auto iter = std::lower_bound(list.begin(), list.end(), str.str(), cmp);
  // If we didn't find str, return false.
  if (list.end() == iter)
    return false;

  return !cmp(str.str(), *iter);
}

void removeUnwantedFunctions(SILModule *M, ArrayRef<std::string> MangledNames,
                             ArrayRef<std::string> DemangledNames,
                             const SILFuncExtractorOptions &options) {
  assert((!MangledNames.empty() || !DemangledNames.empty()) &&
         "Expected names of function we want to retain!");
  assert(M && "Expected a SIL module to extract from.");

  std::vector<SILFunction *> DeadFunctions;
  for (auto &F : M->getFunctionList()) {
    StringRef MangledName = F.getName();
    std::string DemangledName =
        swift::Demangle::demangleSymbolAsString(MangledName);
    DemangledName = DemangledName.substr(0, DemangledName.find_first_of(" <("));
    LLVM_DEBUG(llvm::dbgs() << "Visiting New Func:\n"
                            << "    Mangled: " << MangledName
                            << "\n    Demangled: " << DemangledName << "\n");

    bool FoundMangledName = stringInSortedArray(MangledName, MangledNames,
                                                std::less<std::string>());
    bool FoundDemangledName = stringInSortedArray(
        DemangledName, DemangledNames,
        [](const std::string &str1, const std::string &str2) -> bool {
          return str1.substr(0, str1.find(' ')) <
                 str2.substr(0, str2.find(' '));
        });
    if ((FoundMangledName || FoundDemangledName) ^ options.InvertMatch) {
      LLVM_DEBUG(llvm::dbgs() << "    Not removing!\n");
      continue;
    }

    LLVM_DEBUG(llvm::dbgs() << "    Removing!\n");

    // If F has no body, there is nothing further to do.
    if (!F.size())
      continue;

    SILBasicBlock &BB = F.front();
    SILLocation Loc = BB.back().getLoc();
    BB.split(BB.begin());
    // Make terminator unreachable.
    SILBuilder(&BB).createUnreachable(Loc);
    DeadFunctions.push_back(&F);
  }

  // After running this pass all of the functions we will remove
  // should consist only of one basic block terminated by
  // UnreachableInst.
  performSILDiagnoseUnreachable(M);

  // Now mark all of these functions as public and remove their bodies.
  for (auto &F : DeadFunctions) {
    F->setLinkage(SILLinkage::PublicExternal);
    F->clear();
  }

  // Remove dead functions.
  performSILDeadFunctionElimination(M);
}

int sil_func_extractor_main(ArrayRef<const char *> argv, void *MainAddr) {
  INITIALIZE_LLVM();

  SILFuncExtractorOptions options;

  llvm::cl::ParseCommandLineOptions(argv.size(), argv.data(), "Swift SIL Extractor\n");

  CompilerInvocation Invocation;

  Invocation.setMainExecutablePath(llvm::sys::fs::getMainExecutable(argv[0], MainAddr));

  // Give the context the list of search paths to use for modules.
  Invocation.setImportSearchPaths(options.ImportPaths);
  // Set the SDK path and target if given.
  if (options.SDKPath.getNumOccurrences() == 0) {
    const char *SDKROOT = getenv("SDKROOT");
    if (SDKROOT)
      options.SDKPath = SDKROOT;
  }
  if (!options.SDKPath.empty())
    Invocation.setSDKPath(options.SDKPath);
  if (!options.Triple.empty())
    Invocation.setTargetTriple(options.Triple);
  if (!options.ResourceDir.empty())
    Invocation.setRuntimeResourcePath(options.ResourceDir);
  Invocation.getClangImporterOptions().ModuleCachePath = options.ModuleCachePath;
  Invocation.setParseStdlib();
  Invocation.getLangOptions().DisableAvailabilityChecking = true;
  Invocation.getLangOptions().EnableAccessControl = false;
  Invocation.getLangOptions().EnableObjCAttrRequiresFoundation = false;

  if (options.EnableObjCInterop == llvm::cl::BOU_UNSET) {
    Invocation.getLangOptions().EnableObjCInterop =
        Invocation.getLangOptions().Target.isOSDarwin();
  } else {
    Invocation.getLangOptions().EnableObjCInterop =
    options.EnableObjCInterop == llvm::cl::BOU_TRUE;
  }

  SILOptions &Opts = Invocation.getSILOptions();
  Opts.EmitVerboseSIL = options.EmitVerboseSIL;
  Opts.EmitSortedSIL = options.EmitSortedSIL;
  Opts.EnableOSSAModules = options.EnableOSSAModules;

  serialization::ExtendedValidationInfo extendedInfo;
  llvm::ErrorOr<std::unique_ptr<llvm::MemoryBuffer>> FileBufOrErr =
      Invocation.setUpInputForSILTool(options.InputFilename, options.ModuleName,
                                      /*alwaysSetModuleToMain*/ true,
                                      /*bePrimary*/ false, extendedInfo);
  if (!FileBufOrErr) {
    fprintf(stderr, "Error! Failed to open file: %s\n", options.InputFilename.c_str());
    exit(-1);
  }

  CompilerInstance CI;
  PrintingDiagnosticConsumer PrintDiags;
  CI.addDiagnosticConsumer(&PrintDiags);

  std::string InstanceSetupError;
  if (CI.setup(Invocation, InstanceSetupError)) {
    llvm::errs() << InstanceSetupError << '\n';
    return 1;
  }
  CI.performSema();

  // If parsing produced an error, don't run any passes.
  if (CI.getASTContext().hadError())
    return 1;

  auto SILMod = performASTLowering(CI.getMainModule(), CI.getSILTypes(),
                                   CI.getSILOptions());

  if (options.CommandLineFunctionNames.empty() && options.FunctionNameFile.empty())
    return CI.getASTContext().hadError();

  // For efficient usage, we separate our names into two separate sorted
  // lists, one of managled names, and one of unmangled names.
  std::vector<std::string> Names;
  getFunctionNames(Names, options);

  // First partition our function names into mangled/demangled arrays.
  auto FirstDemangledName = std::partition(
      Names.begin(), Names.end(), [](const std::string &Name) -> bool {
        StringRef NameRef(Name);
        return NameRef.starts_with("_T") ||
               NameRef.starts_with(MANGLING_PREFIX_STR);
      });

  // Then grab offsets to avoid any issues with iterator invalidation when we
  // sort.
  unsigned NumMangled = std::distance(Names.begin(), FirstDemangledName);
  unsigned NumNames = Names.size();

  // Then sort the two partitioned arrays.
  std::sort(Names.begin(), FirstDemangledName);
  std::sort(FirstDemangledName, Names.end());

  // Finally construct our ArrayRefs into the sorted std::vector for our
  // mangled and demangled names.
  ArrayRef<std::string> MangledNames(&*Names.begin(), NumMangled);
  ArrayRef<std::string> DemangledNames(&*std::next(Names.begin(), NumMangled),
                                       NumNames - NumMangled);

  LLVM_DEBUG(llvm::errs() << "MangledNames to keep:\n";
             std::for_each(MangledNames.begin(), MangledNames.end(),
                           [](const std::string &str) {
                             llvm::errs() << "    " << str << '\n';
                           }));
  LLVM_DEBUG(llvm::errs() << "DemangledNames to keep:\n";
             std::for_each(DemangledNames.begin(), DemangledNames.end(),
                           [](const std::string &str) {
                             llvm::errs() << "    " << str << '\n';
                           }));

  removeUnwantedFunctions(SILMod.get(), MangledNames, DemangledNames, options);

  if (options.EmitSIB) {
    llvm::SmallString<128> OutputFile;
    if (options.OutputFilename.size()) {
      OutputFile = options.OutputFilename;
    } else if (options.ModuleName.size()) {
      OutputFile = options.ModuleName;
      llvm::sys::path::replace_extension(
          OutputFile, file_types::getExtension(file_types::TY_SIB));
    } else {
      OutputFile = CI.getMainModule()->getName().str();
      llvm::sys::path::replace_extension(
          OutputFile, file_types::getExtension(file_types::TY_SIB));
    }

    SerializationOptions serializationOpts;
    serializationOpts.OutputPath = OutputFile;
    serializationOpts.SerializeAllSIL = true;
    serializationOpts.IsSIB = true;

    symbolgraphgen::SymbolGraphOptions symbolGraphOpts;

    serialize(CI.getMainModule(), serializationOpts, symbolGraphOpts, SILMod.get());
  } else {
    const StringRef OutputFile =
    options.OutputFilename.size() ? StringRef(options.OutputFilename) : "-";

    auto SILOpts = SILOptions();
    SILOpts.EmitVerboseSIL = options.EmitVerboseSIL;
    SILOpts.EmitSortedSIL = options.EmitSortedSIL;

    if (OutputFile == "-") {
      SILMod->print(llvm::outs(), CI.getMainModule(), SILOpts, !options.DisableASTDump);
    } else {
      std::error_code EC;
      llvm::raw_fd_ostream OS(OutputFile, EC, llvm::sys::fs::OF_None);
      if (EC) {
        llvm::errs() << "while opening '" << OutputFile << "': " << EC.message()
                     << '\n';
        return 1;
      }
      SILMod->print(OS, CI.getMainModule(), SILOpts, !options.DisableASTDump);
    }
  }
  return 0;
}
