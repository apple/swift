//===--- Serialization.cpp - Write Swift modules --------------------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2022 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

#include "swift/Serialization/Serialization.h"
#include "swift/APIDigester/ModuleAnalyzerNodes.h"
#include "swift/AST/DiagnosticsFrontend.h"
#include "swift/AST/FileSystem.h"
#include "swift/Subsystems.h"
#include "swift/SymbolGraphGen/SymbolGraphGen.h"
#include "swift/SymbolGraphGen/SymbolGraphOptions.h"
#include "llvm/Support/SmallVectorMemoryBuffer.h"
#include "llvm/Support/VirtualOutputBackend.h"

using namespace swift;

static ModuleDecl *getModule(ModuleOrSourceFile DC) {
  if (auto M = DC.dyn_cast<ModuleDecl *>())
    return M;
  return DC.get<SourceFile *>()->getParentModule();
}

static ASTContext &getContext(ModuleOrSourceFile DC) {
  return getModule(DC)->getASTContext();
}

static void emitABIDescriptor(ModuleOrSourceFile DC,
                              const SerializationOptions &options) {
  using namespace swift::ide::api;
  if (!options.ABIDescriptorPath.empty()) {
    if (DC.is<ModuleDecl *>()) {
      auto &OutputBackend = getContext(DC).getOutputBackend();
      auto ABIDesFile = OutputBackend.createFile(options.ABIDescriptorPath);
      if (!ABIDesFile) {
        getContext(DC).Diags.diagnose(SourceLoc(), diag::error_opening_output,
                                      options.ABIDescriptorPath,
                                      toString(ABIDesFile.takeError()));
        return;
      }
      SWIFT_DEFER {
        if (auto E = ABIDesFile->keep()) {
          getContext(DC).Diags.diagnose(SourceLoc(), diag::error_closing_output,
                                        options.ABIDescriptorPath,
                                        toString(std::move(E)));
          return;
        }
      };
      dumpModuleContent(DC.get<ModuleDecl *>(), *ABIDesFile, true,
                        options.emptyABIDescriptor);
    }
  }
}

void swift::serializeToBuffers(
    ModuleOrSourceFile DC, const SerializationOptions &options,
    std::unique_ptr<llvm::MemoryBuffer> *moduleBuffer,
    std::unique_ptr<llvm::MemoryBuffer> *moduleDocBuffer,
    std::unique_ptr<llvm::MemoryBuffer> *moduleSourceInfoBuffer,
    const SILModule *M) {
  // Serialization output is disabled.
  if (options.OutputPath.empty())
    return;

  {
    FrontendStatsTracer tracer(getContext(DC).Stats,
                               "Serialization, swiftmodule, to buffer");
    llvm::SmallString<1024> buf;
    llvm::raw_svector_ostream stream(buf);
    serialization::writeToStream(stream, DC, M, options,
                                 /*dependency info*/ nullptr);
    bool hadError = withOutputPath(
        getContext(DC).Diags, getContext(DC).getOutputBackend(),
        options.OutputPath, [&](raw_ostream &out) {
          out << stream.str();
          return false;
        });
    if (hadError)
      return;

    emitABIDescriptor(DC, options);
    if (moduleBuffer)
      *moduleBuffer = std::make_unique<llvm::SmallVectorMemoryBuffer>(
          std::move(buf), options.OutputPath,
          /*RequiresNullTerminator=*/false);
  }

  if (!options.DocOutputPath.empty()) {
    FrontendStatsTracer tracer(getContext(DC).Stats,
                               "Serialization, swiftdoc, to buffer");
    llvm::SmallString<1024> buf;
    llvm::raw_svector_ostream stream(buf);
    serialization::writeDocToStream(stream, DC, options.GroupInfoPath);
    (void)withOutputPath(getContext(DC).Diags,
                            getContext(DC).getOutputBackend(),
                            options.DocOutputPath, [&](raw_ostream &out) {
                              out << stream.str();
                              return false;
                            });
    if (moduleDocBuffer)
      *moduleDocBuffer = std::make_unique<llvm::SmallVectorMemoryBuffer>(
          std::move(buf), options.DocOutputPath,
          /*RequiresNullTerminator=*/false);
  }

  if (!options.SourceInfoOutputPath.empty()) {
    FrontendStatsTracer tracer(getContext(DC).Stats,
                               "Serialization, swiftsourceinfo, to buffer");
    llvm::SmallString<1024> buf;
    llvm::raw_svector_ostream stream(buf);
    serialization::writeSourceInfoToStream(stream, DC);
    (void)withOutputPath(
        getContext(DC).Diags, getContext(DC).getOutputBackend(),
        options.SourceInfoOutputPath, [&](raw_ostream &out) {
          out << stream.str();
          return false;
        });
    if (moduleSourceInfoBuffer)
      *moduleSourceInfoBuffer = std::make_unique<llvm::SmallVectorMemoryBuffer>(
          std::move(buf), options.SourceInfoOutputPath,
          /*RequiresNullTerminator=*/false);
  }
}

void swift::serialize(
    ModuleOrSourceFile DC, const SerializationOptions &options,
    const symbolgraphgen::SymbolGraphOptions &symbolGraphOptions,
    const SILModule *M,
    const fine_grained_dependencies::SourceFileDepGraph *DG) {
  assert(!options.OutputPath.empty());

  if (options.OutputPath == "-") {
    // Special-case writing to stdout.
    serialization::writeToStream(llvm::outs(), DC, M, options, DG);
    assert(options.DocOutputPath.empty());
    return;
  }

  bool hadError = withOutputPath(
      getContext(DC).Diags, getContext(DC).getOutputBackend(),
      options.OutputPath, [&](raw_ostream &out) {
        FrontendStatsTracer tracer(getContext(DC).Stats,
                                   "Serialization, swiftmodule");
        serialization::writeToStream(out, DC, M, options, DG);
        return false;
      });
  if (hadError)
    return;

  if (!options.DocOutputPath.empty()) {
    (void)withOutputPath(
        getContext(DC).Diags, getContext(DC).getOutputBackend(),
        options.DocOutputPath, [&](raw_ostream &out) {
          FrontendStatsTracer tracer(getContext(DC).Stats,
                                     "Serialization, swiftdoc");
          serialization::writeDocToStream(out, DC, options.GroupInfoPath);
          return false;
        });
  }

  if (!options.SourceInfoOutputPath.empty()) {
    (void)withOutputPath(
        getContext(DC).Diags, getContext(DC).getOutputBackend(),
        options.SourceInfoOutputPath, [&](raw_ostream &out) {
          FrontendStatsTracer tracer(getContext(DC).Stats,
                                     "Serialization, swiftsourceinfo");
          serialization::writeSourceInfoToStream(out, DC);
          return false;
        });
  }

  if (!symbolGraphOptions.OutputDir.empty()) {
    if (DC.is<ModuleDecl *>()) {
      auto *M = DC.get<ModuleDecl *>();
      FrontendStatsTracer tracer(getContext(DC).Stats,
                                 "Serialization, symbolgraph");
      symbolgraphgen::emitSymbolGraphForModule(M, symbolGraphOptions);
    }
  }
  emitABIDescriptor(DC, options);
}
