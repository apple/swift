//===--- autolink_extract_main.cpp - autolink extraction utility ----------===//
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
// Extracts autolink flags from object files so they can be passed to the
// linker directly. Mostly useful for platforms where the linker doesn't
// natively support autolinking (ie. ELF-based platforms).
//
//===----------------------------------------------------------------------===//

#include <string>
#include <vector>

#include "swift/AST/DiagnosticsFrontend.h"
#include "swift/Frontend/Frontend.h"
#include "swift/Frontend/PrintingDiagnosticConsumer.h"
#include "swift/Option/Options.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Option/Arg.h"
#include "llvm/Option/ArgList.h"
#include "llvm/Option/Option.h"
#include "llvm/Object/Archive.h"
#include "llvm/Object/ObjectFile.h"
#include "llvm/Object/ELFObjectFile.h"
#include "llvm/Object/Wasm.h"
#include "llvm/BinaryFormat/Wasm.h"

using namespace swift;
using namespace llvm::opt;

class AutolinkExtractInvocation {
private:
  std::string MainExecutablePath;
  std::string OutputFilename = "-";
  std::vector<std::string> InputFilenames;

public:
  void setMainExecutablePath(const std::string &Path) {
    MainExecutablePath = Path;
  }

  const std::string &getOutputFilename() {
    return OutputFilename;
  }

  const std::vector<std::string> &getInputFilenames() {
    return InputFilenames;
  }

  int parseArgs(ArrayRef<const char *> Args,
                DiagnosticEngine &Diags) {
    using namespace options;

    // Parse frontend command line options using Swift's option table.
    std::unique_ptr<llvm::opt::OptTable> Table = createSwiftOptTable();
    unsigned MissingIndex;
    unsigned MissingCount;
    llvm::opt::InputArgList ParsedArgs =
      Table->ParseArgs(Args, MissingIndex, MissingCount, AutolinkExtractOption);
    if (MissingCount) {
      Diags.diagnose(SourceLoc(), diag::error_missing_arg_value,
                     ParsedArgs.getArgString(MissingIndex), MissingCount);
      return 1;
    }

    if (ParsedArgs.hasArg(OPT_UNKNOWN)) {
      for (const Arg *A : ParsedArgs.filtered(OPT_UNKNOWN)) {
        Diags.diagnose(SourceLoc(), diag::error_unknown_arg,
                       A->getAsString(ParsedArgs));
      }
      return true;
    }

    if (ParsedArgs.getLastArg(OPT_help)) {
      std::string ExecutableName =
          llvm::sys::path::stem(MainExecutablePath).str();
      Table->printHelp(llvm::outs(), ExecutableName.c_str(),
                       "Swift Autolink Extract", options::AutolinkExtractOption,
                       0, /*ShowAllAliases*/false);
      return 1;
    }

    for (const Arg *A : ParsedArgs.filtered(OPT_INPUT)) {
      InputFilenames.push_back(A->getValue());
    }

    if (InputFilenames.empty()) {
      Diags.diagnose(SourceLoc(), diag::error_mode_requires_an_input_file);
      return 1;
    }

    if (const Arg *A = ParsedArgs.getLastArg(OPT_o)) {
      OutputFilename = A->getValue();
    }

    return 0;
  }
};

/// Look inside the object file 'ObjectFile' and append any linker flags found in
/// its ".swift1_autolink_entries" section to 'LinkerFlags'.
/// Return 'true' if there was an error, and 'false' otherwise.
static bool
extractLinkerFlagsFromObjectFile(const llvm::object::ObjectFile *ObjectFile,
                                 std::vector<std::string> &LinkerFlags,
                                 std::unordered_map<std::string, bool> &SwiftRuntimeLibraries,
                                 CompilerInstance &Instance) {
  // Search for the section we hold autolink entries in
  for (auto &Section : ObjectFile->sections()) {
    llvm::Expected<llvm::StringRef> SectionNameOrErr = Section.getName();
    if (!SectionNameOrErr) {
      llvm::consumeError(SectionNameOrErr.takeError());
      continue;
    }
    llvm::StringRef SectionName = *SectionNameOrErr;
    if (SectionName == ".swift1_autolink_entries") {
      llvm::Expected<llvm::StringRef> SectionData = Section.getContents();
      if (!SectionData) {
        std::string message;
        {
          llvm::raw_string_ostream os(message);
          logAllUnhandledErrors(SectionData.takeError(), os, "");
        }
        Instance.getDiags().diagnose(SourceLoc(), diag::error_open_input_file,
                                  ObjectFile->getFileName() , message);
        return true;
      }

      // entries are null-terminated, so extract them and push them into
      // the set.
      llvm::SmallVector<llvm::StringRef, 4> SplitFlags;
      SectionData->split(SplitFlags, llvm::StringRef("\0", 1), -1,
                         /*KeepEmpty=*/false);
      for (const auto &Flag : SplitFlags) {
        auto RuntimeLibEntry = SwiftRuntimeLibraries.find(Flag.str());
        if (RuntimeLibEntry == SwiftRuntimeLibraries.end())
          LinkerFlags.emplace_back(Flag.str());
        else
          RuntimeLibEntry->second = true;
      }
    }
  }
  return false;
}

/// Look inside the binary 'Bin' and append any linker flags found in its
/// ".swift1_autolink_entries" section to 'LinkerFlags'. If 'Bin' is an archive,
/// recursively look inside all children within the archive. Return 'true' if
/// there was an error, and 'false' otherwise.
static bool extractLinkerFlags(const llvm::object::Binary *Bin,
                               CompilerInstance &Instance,
                               StringRef BinaryFileName,
                               std::vector<std::string> &LinkerFlags,
                               std::unordered_map<std::string, bool> &SwiftRuntimeLibraries) {
  if (auto *ObjectFile = llvm::dyn_cast<llvm::object::ELFObjectFileBase>(Bin)) {
    return extractLinkerFlagsFromObjectFile(ObjectFile, LinkerFlags, SwiftRuntimeLibraries, Instance);
  } else if (auto *ObjectFile =
                 llvm::dyn_cast<llvm::object::WasmObjectFile>(Bin)) {
    return extractLinkerFlagsFromObjectFile(ObjectFile, LinkerFlags, SwiftRuntimeLibraries, Instance);
  } else if (auto *Archive = llvm::dyn_cast<llvm::object::Archive>(Bin)) {
    llvm::Error Error = llvm::Error::success();
    for (const auto &Child : Archive->children(Error)) {
      auto ChildBinary = Child.getAsBinary();
      // FIXME: BinaryFileName below should instead be ld-style names for
      // object files in archives, e.g. "foo.a(bar.o)".
      if (!ChildBinary) {
        Instance.getDiags().diagnose(SourceLoc(), diag::error_open_input_file,
                                     BinaryFileName,
                                     llvm::toString(ChildBinary.takeError()));
        return true;
      }
      if (extractLinkerFlags(ChildBinary->get(), Instance, BinaryFileName,
                             LinkerFlags, SwiftRuntimeLibraries)) {
        return true;
      }
    }
    return bool(Error);
  } else {
    Instance.getDiags().diagnose(SourceLoc(), diag::error_open_input_file,
                                 BinaryFileName,
                                 "Don't know how to extract from object file"
                                 "format");
    return true;
  }
}

int autolink_extract_main(ArrayRef<const char *> Args, const char *Argv0,
                          void *MainAddr) {
  CompilerInstance Instance;
  PrintingDiagnosticConsumer PDC;
  Instance.addDiagnosticConsumer(&PDC);

  AutolinkExtractInvocation Invocation;
  std::string MainExecutablePath = llvm::sys::fs::getMainExecutable(Argv0,
                                                                    MainAddr);
  Invocation.setMainExecutablePath(MainExecutablePath);

  // Parse arguments.
  if (Invocation.parseArgs(Args, Instance.getDiags()) != 0) {
    return 1;
  }

  std::vector<std::string> LinkerFlags;

  // Keep track of whether we've already added the common
  // Swift libraries that usually have autolink directives
  // in most object files

  std::vector<std::string> SwiftRuntimeLibsOrdered = {
      // Common Swift runtime libs
      "-lswiftSwiftOnoneSupport",
      "-lswiftCore",
      "-lswift_Concurrency",
      "-lswift_StringProcessing",
      "-lswift_RegexBuilder",
      "-lswift_RegexParser",
      "-lswift_Backtracing",
      "-lswiftGlibc",
      "-lBlocksRuntime",
      // Dispatch-specific Swift runtime libs
      "-ldispatch",
      "-lDispatchStubs",
      "-lswiftDispatch",
      // CoreFoundation and Foundation Swift runtime libs
      "-lCoreFoundation",
      "-lFoundation",
      "-lFoundationNetworking",
      "-lFoundationXML",
      // Foundation support libs
      "-lcurl",
      "-lxml2",
      "-luuid",
      // XCTest runtime libs (must be first due to http://github.com/apple/swift-corelibs-xctest/issues/432)
      "-lXCTest",
      // ICU Swift runtime libs
      "-licui18nswift",
      "-licuucswift",
      "-licudataswift",
      // Common-use ordering-agnostic Linux system libs
      "-lm",
      "-lpthread",
      "-lutil",
      "-ldl",
      "-lz",
  };
  std::unordered_map<std::string, bool> SwiftRuntimeLibraries;
  for (const auto &RuntimeLib : SwiftRuntimeLibsOrdered) {
    SwiftRuntimeLibraries[RuntimeLib] = false;
  }

  // Extract the linker flags from the objects.
  for (const auto &BinaryFileName : Invocation.getInputFilenames()) {
    auto BinaryOwner = llvm::object::createBinary(BinaryFileName);
    if (!BinaryOwner) {
      std::string message;
      {
        llvm::raw_string_ostream os(message);
        logAllUnhandledErrors(BinaryOwner.takeError(), os, "");
      }

      Instance.getDiags().diagnose(SourceLoc(), diag::error_open_input_file,
                                   BinaryFileName, message);
      return 1;
    }

    if (extractLinkerFlags(BinaryOwner->getBinary(), Instance, BinaryFileName,
                           LinkerFlags, SwiftRuntimeLibraries)) {
      return 1;
    }
  }

  std::string OutputFilename = Invocation.getOutputFilename();
  std::error_code EC;
  llvm::raw_fd_ostream OutOS(OutputFilename, EC, llvm::sys::fs::OF_None);
  if (OutOS.has_error() || EC) {
    Instance.getDiags().diagnose(SourceLoc(), diag::error_opening_output,
                                 OutputFilename, EC.message());
    OutOS.clear_error();
    return 1;
  }

  for (auto &Flag : LinkerFlags) {
    OutOS << Flag << '\n';
  }

  for (const auto &RuntimeLib : SwiftRuntimeLibsOrdered) {
    auto entry = SwiftRuntimeLibraries.find(RuntimeLib);
    if (entry != SwiftRuntimeLibraries.end() && entry->second) {
      OutOS << entry->first << '\n';
    }
  }


  return 0;
}
