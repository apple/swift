//===--- SILDebugInfoGenerator.cpp - Writes a SIL file for debugging ------===//
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

#define DEBUG_TYPE "sil-based-debuginfo-gen"
#include "swift/AST/SILOptions.h"
#include "swift/Basic/Assertions.h"
#include "swift/SIL/SILPrintContext.h"
#include "swift/SIL/SILModule.h"
#include "swift/SILOptimizer/PassManager/Transforms.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/MemoryBuffer.h"

using namespace swift;

namespace {

/// A pass for generating debug info on SIL level.
///
/// This pass is only enabled if SILOptions::SILOutputFileNameForDebugging is
/// set (i.e. if the -sil-based-debuginfo frontend option is specified).
/// The pass writes all SIL functions into one or multiple output files,
/// depending on the size of the SIL. The names of the output files are derived
/// from the main output file.
///
///     output file name = <main-output-filename>.sil_dbg_<n>.sil
///
/// Where <n> is a consecutive number. The files are stored in the same
/// same directory as the main output file.
/// The debug locations and scopes of all functions and instructions are changed
/// to point to the generated SIL output files.
/// This enables debugging and profiling on SIL level.
class SILDebugInfoGenerator : public SILModuleTransform {

  enum {
    /// To prevent extra large output files, e.g. when compiling the stdlib.
    LineLimitPerFile = 10000
  };

  /// A stream for counting line numbers.
  struct LineCountStream : public llvm::raw_ostream {
    llvm::raw_ostream &Underlying;
    int LineNum = 1;
    uint64_t Pos = 0;

    void write_impl(const char *Ptr, size_t Size) override {
      for (size_t Idx = 0; Idx < Size; ++Idx) {
        char c = Ptr[Idx];
        if (c == '\n')
        ++LineNum;
      }
      Underlying.write(Ptr, Size);
      Pos += Size;
    }

    uint64_t current_pos() const override { return Pos; }

    LineCountStream(llvm::raw_ostream &Underlying) :
      llvm::raw_ostream(/* unbuffered = */ true),
      Underlying(Underlying) { }
    
    ~LineCountStream() override {
      flush();
    }
  };

  /// A print context which records the line numbers where instructions are
  /// printed.
  struct PrintContext : public SILPrintContext {

    LineCountStream LCS;

    llvm::DenseMap<const SILInstruction *, int> LineNums;

    void printInstructionCallBack(const SILInstruction *I) override {
      // Record the current line number of the instruction.
      LineNums[I] = LCS.LineNum;
    }

    PrintContext(llvm::raw_ostream &OS) : SILPrintContext(LCS), LCS(OS) { }

    ~PrintContext() override { }
  };

  void run() override {
    SILModule *M = getModule();
    StringRef FileBaseName = M->getOptions().SILOutputFileNameForDebugging;
    if (FileBaseName.empty())
      return;

    LLVM_DEBUG(llvm::dbgs() << "** SILDebugInfoGenerator **\n");

    std::vector<SILFunction *> PrintedFuncs;
    int FileIdx = 0;
    auto FIter = M->begin();
    while (FIter != M->end()) {

      std::string FileName;
      llvm::raw_string_ostream NameOS(FileName);
      NameOS << FileBaseName << ".sil_dbg_" << FileIdx++ << ".sil";
      NameOS.flush();

      char *FileNameBuf = (char *)M->allocate(FileName.size() + 1, 1);
      strcpy(FileNameBuf, FileName.c_str());

      LLVM_DEBUG(llvm::dbgs() << "Write debug SIL file " << FileName << '\n');

      std::error_code EC;
      llvm::raw_fd_ostream OutFile(FileName, EC,
                                   llvm::sys::fs::OpenFlags::OF_None);
      assert(!OutFile.has_error() && !EC && "Can't write SIL debug file");

      PrintContext Ctx(OutFile);

      // Write functions until we reach the LineLimitPerFile.
      do {
        SILFunction *F = &*FIter++;
        PrintedFuncs.push_back(F);

        // Set the debug scope for the function.
        RegularLocation Loc(SILLocation::FilenameAndLocation::alloc(
             Ctx.LCS.LineNum, 1,FileNameBuf, *M));
        SILDebugScope *Scope = new (*M) SILDebugScope(Loc, F);
        F->setSILDebugScope(Scope);

        // Ensure that the function is visible for debugging.
        F->setBare(IsNotBare);

        // Print it to the output file.
        F->print(Ctx);
      } while (FIter != M->end() && Ctx.LCS.LineNum < LineLimitPerFile);

      // Set the debug locations of all instructions.
      for (SILFunction *F : PrintedFuncs) {
        const SILDebugScope *Scope = F->getDebugScope();
        for (SILBasicBlock &BB : *F) {
          for (auto iter = BB.begin(), end = BB.end(); iter != end;) {
            SILInstruction *I = &*iter;
            ++iter;
            if (isa<DebugValueInst>(I)) {
              // debug_value instructions are not needed anymore.
              // Also, keeping them might trigger a verifier error.
              I->eraseFromParent();
              continue;
            }
            if (auto *ASI = dyn_cast<AllocStackInst>(I))
              // Remove the debug variable scope enclosed
              // within the SILDebugVariable such that we won't
              // trigger a verification error.
              ASI->setDebugVarScope(nullptr);

            SILLocation Loc = I->getLoc();
            auto *filePos = SILLocation::FilenameAndLocation::alloc(
                Ctx.LineNums[I], 1, FileNameBuf, *M);
            assert(filePos->line && "no line set for instruction");
            if (Loc.is<ReturnLocation>()) {
              I->setDebugLocation(
                SILDebugLocation(ReturnLocation(filePos), Scope));
            } else if (Loc.is<ImplicitReturnLocation>()) {
              I->setDebugLocation(
                SILDebugLocation(ImplicitReturnLocation(filePos), Scope));
            } else {
              I->setDebugLocation(
                SILDebugLocation(RegularLocation(filePos), Scope));
            }
          }
        }
      }
      PrintedFuncs.clear();
    }
  }

};

} // end anonymous namespace

SILTransform *swift::createSILDebugInfoGenerator() {
  return new SILDebugInfoGenerator();
}
