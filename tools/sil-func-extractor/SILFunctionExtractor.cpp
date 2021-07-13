//===--- SILFunctionExtractor.cpp - SIL function extraction utility -------===//
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

static llvm::cl::opt<std::string> InputFilename(llvm::cl::desc("input file"),
                                                llvm::cl::init("-"),
                                                llvm::cl::Positional);

static llvm::cl::opt<std::string>
    OutputFilename("o", llvm::cl::desc("output filename"), llvm::cl::init("-"));

static llvm::cl::opt<bool>
    EmitVerboseSIL("emit-verbose-sil",
                   llvm::cl::desc("Emit locations during sil emission."));

static llvm::cl::list<std::string>
    CommandLineFunctionNames("func",
                             llvm::cl::desc("Function names to extract."));
static llvm::cl::opt<std::string> FunctionNameFile(
    "func-file", llvm::cl::desc("File to load additional function names from"));

static llvm::cl::opt<bool>
EmitSIB("emit-sib",
        llvm::cl::desc("Emit a sib file as output instead of a sil file"));

static llvm::cl::opt<bool> InvertMatch(
    "invert",
    llvm::cl::desc("Match functions whose name do not "
                   "match the names of the functions to be extracted"));

static llvm::cl::list<std::string>
    ImportPaths("I",
                llvm::cl::desc("add a directory to the import search path"));

static llvm::cl::opt<std::string>
    ModuleName("module-name",
               llvm::cl::desc("The name of the module if processing"
                              " a module. Necessary for processing "
                              "stdin."));

static llvm::cl::opt<std::string>
    ModuleCachePath("module-cache-path",
                    llvm::cl::desc("Clang module cache path"));

static llvm::cl::opt<std::string> ResourceDir(
    "resource-dir",
    llvm::cl::desc("The directory that holds the compiler resource files"));

static llvm::cl::opt<std::string>
    SDKPath("sdk", llvm::cl::desc("The path to the SDK for use with the clang "
                                  "importer."),
            llvm::cl::init(""));

static llvm::cl::opt<std::string> Triple("target",
                                         llvm::cl::desc("target triple"));

static llvm::cl::opt<bool>
    EmitSortedSIL("emit-sorted-sil", llvm::cl::Hidden, llvm::cl::init(false),
                  llvm::cl::desc("Sort Functions, VTables, Globals, "
                                 "WitnessTables by name to ease diffing."));

static llvm::cl::opt<bool>
DisableASTDump("sil-disable-ast-dump", llvm::cl::Hidden,
               llvm::cl::init(false),
               llvm::cl::desc("Do not dump AST."));

static llvm::cl::opt<bool> EnableOSSAModules(
    "enable-ossa-modules",
    llvm::cl::desc("Do we always serialize SIL in OSSA form? If "
                   "this is disabled we do not serialize in OSSA "
                   "form when optimizing."));

// This function isn't referenced outside its translation unit, but it
// can't use the "static" keyword because its address is used for
// getMainExecutable (since some platforms don't support taking the
// address of main, and some platforms can't implement getMainExecutable
// without being given the address of a function in the main executable).
void anchorForGetMainExecutable() {}

static void getFunctionNames(std::vector<std::string> &Names) {
  std::copy(CommandLineFunctionNames.begin(), CommandLineFunctionNames.end(),
            std::back_inserter(Names));

  if (!FunctionNameFile.empty()) {
    llvm::ErrorOr<std::unique_ptr<llvm::MemoryBuffer>> FileBufOrErr =
        llvm::MemoryBuffer::getFileOrSTDIN(FunctionNameFile);
    if (!FileBufOrErr) {
      fprintf(stderr, "Error! Failed to open file: %s\n",
              InputFilename.c_str());
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
                             ArrayRef<std::string> DemangledNames) {
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
    if ((FoundMangledName || FoundDemangledName) ^ InvertMatch) {
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

int main(int argc, char **argv) {
  PROGRAM_START(argc, argv);
  INITIALIZE_LLVM();

  llvm::cl::ParseCommandLineOptions(argc, argv, "Swift SIL Extractor\n");

  CompilerInvocation Invocation;

  Invocation.setMainExecutablePath(llvm::sys::fs::getMainExecutable(
      argv[0], reinterpret_cast<void *>(&anchorForGetMainExecutable)));

  // Give the context the list of search paths to use for modules.
  Invocation.setImportSearchPaths(ImportPaths);
  // Set the SDK path and target if given.
  if (SDKPath.getNumOccurrences() == 0) {
    const char *SDKROOT = getenv("SDKROOT");
    if (SDKROOT)
      SDKPath = SDKROOT;
  }
  if (!SDKPath.empty())
    Invocation.setSDKPath(SDKPath);
  if (!Triple.empty())
    Invocation.setTargetTriple(Triple);
  if (!ResourceDir.empty())
    Invocation.setRuntimeResourcePath(ResourceDir);
  Invocation.getClangImporterOptions().ModuleCachePath = ModuleCachePath;
  Invocation.setParseStdlib();
  Invocation.getLangOptions().DisableAvailabilityChecking = true;
  Invocation.getLangOptions().EnableAccessControl = false;
  Invocation.getLangOptions().EnableObjCAttrRequiresFoundation = false;
  if (Invocation.getLangOptions().Target.isOSBinFormatWasm())
    Invocation.getLangOptions().EnableObjCInterop = false;

  SILOptions &Opts = Invocation.getSILOptions();
  Opts.EmitVerboseSIL = EmitVerboseSIL;
  Opts.EmitSortedSIL = EmitSortedSIL;
  Opts.EnableOSSAModules = EnableOSSAModules;

  serialization::ExtendedValidationInfo extendedInfo;
  llvm::ErrorOr<std::unique_ptr<llvm::MemoryBuffer>> FileBufOrErr =
      Invocation.setUpInputForSILTool(InputFilename, ModuleName,
                                      /*alwaysSetModuleToMain*/ true,
                                      /*bePrimary*/ false, extendedInfo);
  if (!FileBufOrErr) {
    fprintf(stderr, "Error! Failed to open file: %s\n", InputFilename.c_str());
    exit(-1);
  }

  CompilerInstance CI;
  PrintingDiagnosticConsumer PrintDiags;
  CI.addDiagnosticConsumer(&PrintDiags);

  if (CI.setup(Invocation))
    return 1;
  CI.performSema();

  // If parsing produced an error, don't run any passes.
  if (CI.getASTContext().hadError())
    return 1;

  auto SILMod = performASTLowering(CI.getMainModule(), CI.getSILTypes(),
                                   CI.getSILOptions());

  if (CommandLineFunctionNames.empty() && FunctionNameFile.empty())
    return CI.getASTContext().hadError();

  // For efficient usage, we separate our names into two separate sorted
  // lists, one of managled names, and one of unmangled names.
  std::vector<std::string> Names;
  getFunctionNames(Names);

  // First partition our function names into mangled/demangled arrays.
  auto FirstDemangledName = std::partition(
      Names.begin(), Names.end(), [](const std::string &Name) -> bool {
        StringRef NameRef(Name);
        return NameRef.startswith("_T") ||
               NameRef.startswith(MANGLING_PREFIX_STR);
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

  removeUnwantedFunctions(SILMod.get(), MangledNames, DemangledNames);

  if (EmitSIB) {
    llvm::SmallString<128> OutputFile;
    if (OutputFilename.size()) {
      OutputFile = OutputFilename;
    } else if (ModuleName.size()) {
      OutputFile = ModuleName;
      llvm::sys::path::replace_extension(
          OutputFile, file_types::getExtension(file_types::TY_SIB));
    } else {
      OutputFile = CI.getMainModule()->getName().str();
      llvm::sys::path::replace_extension(
          OutputFile, file_types::getExtension(file_types::TY_SIB));
    }

    SerializationOptions serializationOpts;
    serializationOpts.OutputPath = OutputFile.c_str();
    serializationOpts.SerializeAllSIL = true;
    serializationOpts.IsSIB = true;

    serialize(CI.getMainModule(), serializationOpts, SILMod.get());
  } else {
    const StringRef OutputFile =
        OutputFilename.size() ? StringRef(OutputFilename) : "-";

    auto SILOpts = SILOptions();
    SILOpts.EmitVerboseSIL = EmitVerboseSIL;
    SILOpts.EmitSortedSIL = EmitSortedSIL;

    if (OutputFile == "-") {
      SILMod->print(llvm::outs(), CI.getMainModule(), SILOpts, !DisableASTDump);
    } else {
      std::error_code EC;
      llvm::raw_fd_ostream OS(OutputFile, EC, llvm::sys::fs::F_None);
      if (EC) {
        llvm::errs() << "while opening '" << OutputFile << "': " << EC.message()
                     << '\n';
        return 1;
      }
      SILMod->print(OS, CI.getMainModule(), SILOpts, !DisableASTDump);
    }
  }
}
