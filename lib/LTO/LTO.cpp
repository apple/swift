//===--- LTO.cpp - Swift LTO ----------------------------------------------===//
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

#include "swift/LTO/LTO.h"
#include "swift/AST/DiagnosticsFrontend.h"
#include "swift/AST/IRGenRequests.h"
#include "swift/ClangImporter/ClangImporter.h"
#include "swift/SIL/SILModule.h"
#include "swift/Serialization/SerializedModuleLoader.h"
#include "llvm/Bitcode/BitcodeWriter.h"
#include "llvm/IR/Module.h"

namespace swift {

namespace lto {

using namespace llvm;

bool LTOPipeline::addModule(std::unique_ptr<MemoryBuffer> Buffer) {
  serialization::ExtendedValidationInfo extendedInfo;
  serialization::ValidationInfo info =
      serialization::validateSerializedAST(Buffer->getBuffer(), &extendedInfo);
  if (info.status != serialization::Status::Valid) {
    Diags.diagnose(SourceLoc(), diag::invalid_serialized_module);
    return true;
  }

  if (!Ctx) {
    Ctx.reset(createASTContext(info, extendedInfo));
  }

  MBL->registerMemoryBuffer(info.name, std::move(Buffer));

  ModuleNames.emplace_back(Ctx->getIdentifier(info.name));
  return false;
}

bool LTOPipeline::emitLLVMModules(GetStreamFn GetStream) {
  IRGenOptions Opts = {};
  Opts.OutputKind = IRGenOutputKind::Module;

  for (auto &ModuleName : ModuleNames) {
    std::vector<swift::Located<swift::Identifier>> AccessPath;
    AccessPath.emplace_back(ModuleName, SourceLoc());
    auto SwiftModule = Ctx->getModule(AccessPath);
    if (!SwiftModule) {
      Diags.diagnose(SourceLoc(), diag::unable_to_load_serialized_module,
                     ModuleName.get());
      return true;
    }
    Lowering::TypeConverter Types(*SwiftModule);
    SILOptions SILOpts = {};
    auto SM = performASTLowering(SwiftModule, Types, SILOpts);
    // TODO: Propagate input file name through SIB to enable debug info
    const PrimarySpecificPaths PSPs;
    auto GeneratedMod =
        performIRGeneration(Opts, SM->getSwiftModule(), std::move(SM),
                            ModuleName.get(), PSPs, ArrayRef<std::string>());
    auto LLVMMod = GeneratedMod.getModule();
    if (auto OS = GetStream(LLVMMod->getName())) {
      WriteBitcodeToFile(*LLVMMod, *OS);
    }
  }
  return false;
}

ASTContext *
LTOPipeline::createASTContext(serialization::ValidationInfo info,
                              serialization::ExtendedValidationInfo extInfo) {
  auto Ctx = ASTContext::get(LangOpts, TCOpts, SearchPathOpts, SM, Diags);
  Diags.addConsumer(PrintDiags);
  LangOpts.setTarget(Triple(info.targetTriple));
  SearchPathOpts.SDKPath = extInfo.getSDKPath();

  SearchPathOpts.RuntimeLibraryPaths.insert(
      SearchPathOpts.RuntimeLibraryPaths.end(), RuntimeLibraryPaths.begin(),
      RuntimeLibraryPaths.end());
  SearchPathOpts.RuntimeLibraryImportPaths.insert(
      SearchPathOpts.RuntimeLibraryImportPaths.end(),
      RuntimeLibraryImportPaths.begin(), RuntimeLibraryImportPaths.end());
  SearchPathOpts.RuntimeResourcePath = RuntimeResourcePath;

  // MARK: Setup module loaders
  std::unique_ptr<ClangImporter> clangImporter =
      ClangImporter::create(*Ctx, ClangOpts, "", nullptr);
  auto const &Clang = clangImporter->getClangInstance();
  std::string ModuleCachePath = getModuleCachePathFromClang(Clang);

  auto MIL = ModuleInterfaceLoader::create(*Ctx, ModuleCachePath, "", nullptr,
                                           ModuleLoadingMode::PreferSerialized);
  Ctx->addModuleLoader(std::move(MIL));
  auto MBL = MemoryBufferSerializedModuleLoader::create(
      *Ctx, nullptr, ModuleLoadingMode::OnlySerialized, true);
  this->MBL = MBL.get();

  auto SML = SerializedModuleLoader::create(
      *Ctx, nullptr, ModuleLoadingMode::OnlySerialized, true);

  Ctx->addModuleLoader(std::move(MBL));
  Ctx->addModuleLoader(std::move(SML));
  Ctx->addModuleLoader(std::move(clangImporter), /*isClang*/ true);

  registerIRGenRequestFunctions(Ctx->evaluator);
  registerSILOptimizerRequestFunctions(Ctx->evaluator);
  registerParseRequestFunctions(Ctx->evaluator);
  registerTypeCheckerRequestFunctions(Ctx->evaluator);
  registerSILGenRequestFunctions(Ctx->evaluator);
  registerIRGenSILTransforms(*Ctx);
  return Ctx;
}

} // namespace lto
} // namespace swift
