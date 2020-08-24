//===--- swift_lto_main.cpp -----------------------------------------------===//
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
///
/// \file
///
/// This is a tool for reading sib files and running LTO passes upon them.
///
//===----------------------------------------------------------------------===//

#include "swift/Basic/LLVMInitialize.h"
#include "swift/Frontend/Frontend.h"
#include "swift/LTO/LTO.h"
#include "swift/Strings.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/MemoryBuffer.h"
#include <cstdio>
#include <memory>
using namespace swift;

llvm::cl::SubCommand LTOSubcommand("lto", "Swift LTO Tool");

static llvm::cl::list<std::string>
    InputFilenames(llvm::cl::Positional, llvm::cl::desc("[input files...]"),
                   llvm::cl::OneOrMore, llvm::cl::sub(LTOSubcommand));

// This function isn't referenced outside its translation unit, but it
// can't use the "static" keyword because its address is used for
// getMainExecutable (since some platforms don't support taking the
// address of main, and some platforms can't implement getMainExecutable
// without being given the address of a function in the main executable).
void anchorForGetMainExecutableInSwiftLTO() {}

// This tool is combined with sil-llvm-gen to reduce link time.
// This entrypoint is invoked from SILLLVMGen.cpp when user invoke
// lto subcommand.
int swift_lto_main(int argc, char **argv) {
  CompilerInvocation Invocation;

  Invocation.setMainExecutablePath(llvm::sys::fs::getMainExecutable(
      argv[0],
      reinterpret_cast<void *>(&anchorForGetMainExecutableInSwiftLTO)));

  auto SearchPathOpts = Invocation.getSearchPathOptions();
  llvm::SmallVector<llvm::StringRef, 4> RuntimeLibraryPaths;
  llvm::SmallVector<llvm::StringRef, 4> RuntimeLibraryImportPaths;
  RuntimeLibraryPaths.insert(RuntimeLibraryPaths.begin(),
                             SearchPathOpts.RuntimeLibraryPaths.begin(),
                             SearchPathOpts.RuntimeLibraryPaths.end());
  RuntimeLibraryImportPaths.insert(
      RuntimeLibraryImportPaths.begin(),
      SearchPathOpts.RuntimeLibraryImportPaths.begin(),
      SearchPathOpts.RuntimeLibraryImportPaths.end());
  lto::LTOPipeline Pipeline(RuntimeLibraryPaths, RuntimeLibraryImportPaths,
                            SearchPathOpts.RuntimeResourcePath);

  for (auto InputFilename : InputFilenames) {
    // Load the input file.
    auto FileBufOrErr = llvm::MemoryBuffer::getFileOrSTDIN(InputFilename);
    if (!FileBufOrErr) {
      fprintf(stderr, "Error! Failed to open file: %s\n",
              InputFilename.c_str());
      exit(-1);
    }

    if (Pipeline.addModule(std::move(FileBufOrErr.get()))) {
      fprintf(stderr, "Error! Failed to load serialized module: %s\n",
              InputFilename.c_str());
      exit(-1);
    }
  }

  Pipeline.emitLLVMModules([&](StringRef ModuleName) {
    std::error_code EC;
    std::unique_ptr<llvm::raw_ostream> RawOS =
        std::make_unique<llvm::raw_fd_ostream>(ModuleName.str() + ".bc", EC);
    if (EC)
      return std::unique_ptr<llvm::raw_ostream>(nullptr);
    return RawOS;
  });
  return 0;
}
