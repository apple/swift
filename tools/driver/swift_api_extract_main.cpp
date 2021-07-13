//===--- swift_api_extract_main.cpp - Swift module API extract tool -------===//
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
//  Extracts APIs from a .swiftmodule file.
//
//===----------------------------------------------------------------------===//

#include "swift/AST/DiagnosticsFrontend.h"
#include "swift/Basic/LLVM.h"
#include "swift/Basic/LLVMInitialize.h"
#include "swift/Basic/Version.h"
#include "swift/Frontend/Frontend.h"
#include "swift/Frontend/PrintingDiagnosticConsumer.h"
#include "swift/Option/Options.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/VirtualFileSystem.h"
#include "llvm/Support/raw_ostream.h"
#include <system_error>

using namespace swift;
using namespace llvm::opt;

class SwiftAPIExtractInvocation {
private:
  CompilerInvocation Invocation;
  CompilerInstance Instance;
  PrintingDiagnosticConsumer PDC;
  std::string MainExecutablePath;
  std::string OutputFilename = "-";
  std::string InputModulePath;
  std::string InputModuleName;
  bool PrettyPrint = false;

public:
  SwiftAPIExtractInvocation(const std::string &ExecPath)
      : MainExecutablePath(ExecPath) {
    Instance.addDiagnosticConsumer(&PDC);
  }

  int parseArgs(ArrayRef<const char *> Args) {
    using namespace options;
    auto &Diags = Instance.getDiags();

    std::unique_ptr<llvm::opt::OptTable> Table = createSwiftOptTable();
    unsigned MissingIndex;
    unsigned MissingCount;
    llvm::opt::InputArgList ParsedArgs = Table->ParseArgs(
        Args, MissingIndex, MissingCount, SwiftAPIExtractOption);
    if (MissingCount) {
      Diags.diagnose(SourceLoc(), diag::error_missing_arg_value,
                     ParsedArgs.getArgString(MissingIndex), MissingCount);
      return 1;
    }

    if (ParsedArgs.getLastArg(OPT_help)) {
      Table->PrintHelp(llvm::outs(), "swift api-extract",
                       "Swift API Extract", options::SwiftAPIExtractOption, 0,
                       /*ShowAllAliases*/ false);
      return 1;
    }

    if (auto *A = ParsedArgs.getLastArg(OPT_module_name))
      InputModuleName = A->getValue();
    else {
      Diags.diagnose(SourceLoc(), diag::error_module_name_required);
      return 1;
    }

    if (auto *A = ParsedArgs.getLastArg(OPT_INPUT))
      InputModulePath = A->getValue();

    if (auto *A = ParsedArgs.getLastArg(OPT_o))
      OutputFilename = A->getValue();

    if (auto *A = ParsedArgs.getLastArg(OPT_target))
      Invocation.setTargetTriple(A->getValue());

    if (auto *A = ParsedArgs.getLastArg(OPT_swift_version)) {
      bool isValid = false;
      if (auto Version = version::Version::parseVersionString(
              A->getValue(), SourceLoc(), nullptr)) {
        if (auto Effective = Version.getValue().getEffectiveLanguageVersion()) {
          Invocation.getLangOptions().EffectiveLanguageVersion = *Effective;
          isValid = true;
        }
      }
      if (!isValid) {
        Diags.diagnose(SourceLoc(), diag::error_invalid_arg_value,
                       A->getAsString(ParsedArgs), A->getValue());
        return 1;
      }
    }

    Invocation.getLangOptions().EnableModuleLoadingRemarks = true;

    if (auto *A = ParsedArgs.getLastArg(OPT_sdk))
      Invocation.setSDKPath(A->getValue());

    std::vector<SearchPathOptions::FrameworkSearchPath> FrameworkSearchPaths;
    for (auto *A : ParsedArgs.filtered(OPT_F))
      FrameworkSearchPaths.emplace_back(A->getValue(), /*isSystem*/ false);

    for (auto *A : ParsedArgs.filtered(OPT_Fsystem))
      FrameworkSearchPaths.emplace_back(A->getValue(), /*isSystem*/ true);
    Invocation.setFrameworkSearchPaths(FrameworkSearchPaths);

    std::vector<std::string> LibrarySearchPaths;
    for (auto *A : ParsedArgs.filtered(OPT_L))
      LibrarySearchPaths.push_back(A->getValue());
    Invocation.getSearchPathOptions().LibrarySearchPaths = LibrarySearchPaths;

    std::vector<std::string> ImportSearchPaths;
    if (!InputModulePath.empty()) {
      // FIXME: this needs to move to tapi.
      // Create an overlay of in memory file system of everything inside the
      // input swiftmodule.
      // The input module is mapped to "/@input" to avoid finding duplicated
      // dependency modules in case there are other modules next to the input
      // module.
      llvm::vfs::YAMLVFSWriter VFSWriter;
      SmallString<256> Prefix("/@input");
      llvm::sys::path::append(Prefix,
                              llvm::sys::path::filename(InputModulePath));
      std::error_code EC;
      if (llvm::sys::fs::is_directory(InputModulePath)) {
        for (llvm::sys::fs::recursive_directory_iterator F(InputModulePath, EC),
             FE;
             F != FE; F.increment(EC)) {
          if (EC) {
            Diags.diagnose(SourceLoc(), diag::cannot_open_file, F->path(),
                           EC.message());
            return 1;
          }
          SmallString<256> MappedPath(Prefix);
          StringRef RelativePath(F->path());
          RelativePath.consume_front(InputModulePath);
          llvm::sys::path::append(MappedPath, RelativePath);
          VFSWriter.addFileMapping(MappedPath, F->path());
        }
      } else
        VFSWriter.addFileMapping(Prefix, InputModulePath);

      SmallString<256> VFSMapFile;
      EC = llvm::sys::fs::createTemporaryFile("vfsmap", "map", VFSMapFile);
      if (EC) {
        Diags.diagnose(SourceLoc(), diag::cannot_open_file, VFSMapFile,
                       EC.message());
        return 1;
      }
      llvm::raw_fd_ostream SS(VFSMapFile, EC);
      if (EC) {
        Diags.diagnose(SourceLoc(), diag::cannot_open_file, VFSMapFile,
                       EC.message());
        return 1;
      }
      VFSWriter.write(SS);
      Invocation.getSearchPathOptions().VFSOverlayFiles.push_back(
          VFSMapFile.str().str());
      // Put the scanning module in the beginning of the import search path.
      ImportSearchPaths.push_back("/@input");
    }
    for (auto *A : ParsedArgs.filtered(OPT_I))
      ImportSearchPaths.push_back(A->getValue());
    Invocation.setImportSearchPaths(ImportSearchPaths);

    for (auto *A : ParsedArgs.filtered(OPT_vfsoverlay))
      Invocation.getSearchPathOptions().VFSOverlayFiles.push_back(
          A->getValue());

    if (ParsedArgs.hasArg(OPT_pretty_print))
      PrettyPrint = true;

    if (auto *A = ParsedArgs.getLastArg(OPT_module_cache_path))
      Invocation.setClangModuleCachePath(A->getValue());

    Invocation.setMainExecutablePath(MainExecutablePath);
    Invocation.setModuleName("swift_api_extract");
    Invocation.getLangOptions().EnableObjCInterop =
        llvm::Triple(Invocation.getTargetTriple()).isOSDarwin();
    Invocation.getLangOptions().DebuggerSupport = true;
    Invocation.getFrontendOptions().EnableLibraryEvolution = true;
    Invocation.setDefaultPrebuiltCacheIfNecessary();

    return 0;
  }

  int extractAPI() {
    if (Instance.setup(Invocation)) {
      llvm::outs() << "Failed to setup compiler instance\n";
      return 1;
    }

    auto *M = Instance.getASTContext().getModuleByName(InputModuleName);
    if (!M) {
      Instance.getDiags().diagnose(SourceLoc(), diag::unknown_swift_module_name,
                                   InputModuleName);
      return 1;
    }

    // If there are errors emitted when loading module, exit with error.
    if (Instance.getASTContext().hadError())
      return 1;

    if (OutputFilename == "-") {
      writeAPIJSONFile(M, llvm::outs(), PrettyPrint);
      return 0;
    }

    std::error_code EC;
    llvm::raw_fd_ostream OS(OutputFilename, EC);
    if (EC) {
      Instance.getDiags().diagnose(SourceLoc(), diag::cannot_open_file,
                                   OutputFilename, EC.message());
      return 1;
    }

    writeAPIJSONFile(M, OS, PrettyPrint);
    return 0;
  }
};

int swift_api_extract_main(ArrayRef<const char *> Args, const char *Argv0,
                           void *MainAddr) {
  INITIALIZE_LLVM();

  SwiftAPIExtractInvocation Invocation(
      llvm::sys::fs::getMainExecutable(Argv0, MainAddr));

  if (Invocation.parseArgs(Args) != 0)
    return EXIT_FAILURE;

  if (Invocation.extractAPI() != 0)
    return EXIT_FAILURE;

  return EXIT_SUCCESS;
}
