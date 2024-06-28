//===--- SwiftMaterializationUnit.cpp - JIT Swift ASTs ----------*- C++ -*-===//
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
// Defines the `SwiftMaterializationUnit` class, which allows you to JIT
// individual Swift AST declarations.
//
//===----------------------------------------------------------------------===//

#include <memory>
#include <optional>
#include <string>

#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/ExecutionEngine/JITLink/JITLink.h"
#include "llvm/ExecutionEngine/JITSymbol.h"
#include "llvm/ExecutionEngine/Orc/Core.h"
#include "llvm/ExecutionEngine/Orc/EPCIndirectionUtils.h"
#include "llvm/ExecutionEngine/Orc/ExecutionUtils.h"
#include "llvm/ExecutionEngine/Orc/JITTargetMachineBuilder.h"
#include "llvm/ExecutionEngine/Orc/LLJIT.h"
#include "llvm/ExecutionEngine/Orc/ObjectLinkingLayer.h"
#include "llvm/ExecutionEngine/Orc/ObjectTransformLayer.h"
#include "llvm/ExecutionEngine/Orc/Shared/ExecutorAddress.h"
#include "llvm/ExecutionEngine/Orc/Shared/MemoryFlags.h"
#include "llvm/ExecutionEngine/Orc/TargetProcess/TargetExecutionUtils.h"
#include "llvm/Support/Error.h"

#include "swift/AST/IRGenRequests.h"
#include "swift/AST/SILGenRequests.h"
#include "swift/AST/TBDGenRequests.h"
#include "swift/Basic/Assertions.h"
#include "swift/Frontend/Frontend.h"
#include "swift/Immediate/SwiftMaterializationUnit.h"
#include "swift/SIL/SILModule.h"
#include "swift/SILOptimizer/PassManager/Passes.h"
#include "swift/Subsystems.h"

#define DEBUG_TYPE "swift-immediate"

using namespace swift;

/// The suffix appended to function bodies when creating lazy reexports
static const std::string ManglingSuffix = "$impl";

/// Mangle a function for a lazy reexport
static std::string mangle(const StringRef Unmangled) {
  return Unmangled.str() + ManglingSuffix;
}

/// Whether a function name is mangled to be a lazy reexport
static bool isMangled(const StringRef Symbol) {
  return Symbol.ends_with(ManglingSuffix);
}

/// Demangle a lazy reexport
static StringRef demangle(const StringRef Mangled) {
  return isMangled(Mangled) ? Mangled.drop_back(ManglingSuffix.size())
                            : Mangled;
}

llvm::Expected<std::unique_ptr<SwiftJIT>>
SwiftJIT::Create(CompilerInstance &CI) {

  auto J = CreateLLJIT(CI);
  if (!J)
    return J.takeError();

  // Create generator to resolve symbols defined in current process
  auto &ES = (*J)->getExecutionSession();
  auto EPCIU =
      llvm::orc::EPCIndirectionUtils::Create(ES.getExecutorProcessControl());
  if (!EPCIU)
    return EPCIU.takeError();

  (*EPCIU)->createLazyCallThroughManager(
      ES, llvm::orc::ExecutorAddr::fromPtr(&handleLazyCompilationFailure));

  if (auto Err = setUpInProcessLCTMReentryViaEPCIU(**EPCIU))
    return std::move(Err);
  return std::unique_ptr<SwiftJIT>(
      new SwiftJIT(std::move(*J), std::move(*EPCIU)));
}

SwiftJIT::~SwiftJIT() {
  if (auto Err = EPCIU->cleanup())
    J->getExecutionSession().reportError(std::move(Err));
}

llvm::Expected<int> SwiftJIT::runMain(llvm::ArrayRef<std::string> Args) {
  if (auto Err = J->initialize(J->getMainJITDylib())) {
    return std::move(Err);
  }

  auto MainSym = J->lookup("main");
  if (!MainSym) {
    return MainSym.takeError();
  }

  using MainFnTy = int (*)(int, char *[]);
  MainFnTy JITMain = MainSym->toPtr<MainFnTy>();

  LLVM_DEBUG(llvm::dbgs() << "Running main\n");
  int Result = llvm::orc::runAsMain(JITMain, Args);

  LLVM_DEBUG(llvm::dbgs() << "Running static destructors\n");
  if (auto Err = J->deinitialize(J->getMainJITDylib())) {
    return std::move(Err);
  }

  return Result;
}

llvm::orc::JITDylib &SwiftJIT::getMainJITDylib() {
  return J->getMainJITDylib();
}

std::string SwiftJIT::mangle(StringRef Name) { return J->mangle(Name); }

llvm::orc::SymbolStringPtr SwiftJIT::mangleAndIntern(StringRef Name) {
  return J->mangleAndIntern(Name);
}

llvm::orc::SymbolStringPtr SwiftJIT::intern(StringRef Name) {
  return J->getExecutionSession().intern(Name);
}

llvm::orc::IRCompileLayer &SwiftJIT::getIRCompileLayer() {
  return J->getIRCompileLayer();
}

llvm::orc::ObjectTransformLayer &SwiftJIT::getObjTransformLayer() {
  return J->getObjTransformLayer();
}

llvm::Expected<std::unique_ptr<llvm::orc::LLJIT>>
SwiftJIT::CreateLLJIT(CompilerInstance &CI) {
  llvm::TargetOptions TargetOpt;
  std::string CPU;
  std::string Triple;
  std::vector<std::string> Features;
  const auto &Invocation = CI.getInvocation();
  const auto &IRGenOpts = Invocation.getIRGenOptions();
  auto &Ctx = CI.getASTContext();
  std::tie(TargetOpt, CPU, Features, Triple) =
      getIRTargetOptions(IRGenOpts, Ctx);
  auto JTMB = llvm::orc::JITTargetMachineBuilder(llvm::Triple(Triple))
                  .setRelocationModel(llvm::Reloc::PIC_)
                  .setOptions(std::move(TargetOpt))
                  .setCPU(std::move(CPU))
                  .addFeatures(Features)
                  .setCodeGenOptLevel(llvm::CodeGenOpt::Default);
  auto J = llvm::orc::LLJITBuilder()
               .setJITTargetMachineBuilder(std::move(JTMB))
               .create();
  if (!J)
    return J.takeError();
  auto G = llvm::orc::DynamicLibrarySearchGenerator::GetForCurrentProcess(
      (*J)->getDataLayout().getGlobalPrefix());
  if (!G)
    return G.takeError();
  (*J)->getMainJITDylib().addGenerator(std::move(*G));
  return J;
}

static llvm::Error
renameFunctionBodies(llvm::orc::MaterializationResponsibility &MR,
                     llvm::jitlink::LinkGraph &G) {

  using namespace llvm;
  using namespace llvm::orc;

  llvm::StringSet<> ToRename;
  for (auto &KV : MR.getSymbols()) {
    StringRef Name = *KV.first;
    if (isMangled(Name))
      // All mangled functions we are responsible for
      // materializing must be mangled at the object levels
      ToRename.insert(demangle(Name));
  }
  for (auto &Sec : G.sections()) {
    // Skip non-executable sections.
    if ((Sec.getMemProt() & MemProt::Exec) == MemProt::None)
      continue;

    for (auto *Sym : Sec.symbols()) {
      // Skip all anonymous and non-callables.
      if (!Sym->hasName() || !Sym->isCallable())
        continue;

      if (ToRename.count(Sym->getName())) {
        // FIXME: Get rid of the temporary when Swift's llvm-project is
        // updated to LLVM 17.
        auto NewName = G.allocateCString(Twine(mangle(Sym->getName())));
        Sym->setName({NewName.data(), NewName.size() - 1});
      }
    }
  }

  return llvm::Error::success();
}

void SwiftJIT::Plugin::modifyPassConfig(
    llvm::orc::MaterializationResponsibility &MR, llvm::jitlink::LinkGraph &G,
    llvm::jitlink::PassConfiguration &PassConfig) {
  PassConfig.PrePrunePasses.push_back(
      [&](llvm::jitlink::LinkGraph &G) { return renameFunctionBodies(MR, G); });
}

llvm::Error
SwiftJIT::Plugin::notifyFailed(llvm::orc::MaterializationResponsibility &MR) {
  return llvm::Error::success();
}

llvm::Error
SwiftJIT::Plugin::notifyRemovingResources(llvm::orc::JITDylib &JD,
                                          llvm::orc::ResourceKey K) {
  return llvm::Error::success();
}

void SwiftJIT::Plugin::notifyTransferringResources(
    llvm::orc::JITDylib &JD, llvm::orc::ResourceKey DstKey,
    llvm::orc::ResourceKey SrcKey) {}

void SwiftJIT::handleLazyCompilationFailure() {
  llvm::errs() << "Lazy compilation error\n";
  exit(1);
}

SwiftJIT::SwiftJIT(std::unique_ptr<llvm::orc::LLJIT> J,
                   std::unique_ptr<llvm::orc::EPCIndirectionUtils> EPCIU)
    : J(std::move(J)), EPCIU(std::move(EPCIU)),
      LCTM(this->EPCIU->getLazyCallThroughManager()),
      ISM(this->EPCIU->createIndirectStubsManager()) {}

void SwiftJIT::addRenamer() {
  static_cast<llvm::orc::ObjectLinkingLayer &>(this->J->getObjLinkingLayer())
      .addPlugin(std::make_unique<Plugin>());
}

/// IRGen the provided `SILModule` with the specified options.
/// Returns `std::nullopt` if a compiler error is encountered
static std::optional<GeneratedModule>
generateModule(const CompilerInstance &CI, std::unique_ptr<SILModule> SM) {
  // TODO: Use OptimizedIRRequest for this.
  const auto &Context = CI.getASTContext();
  auto *swiftModule = CI.getMainModule();
  const auto PSPs = CI.getPrimarySpecificPathsForAtMostOnePrimary();
  const auto &Invocation = CI.getInvocation();
  const auto &TBDOpts = Invocation.getTBDGenOptions();
  const auto &IRGenOpts = Invocation.getIRGenOptions();

  // Lower the SIL module to LLVM IR
  auto GenModule = performIRGeneration(
      swiftModule, IRGenOpts, TBDOpts, std::move(SM),
      swiftModule->getName().str(), PSPs, ArrayRef<std::string>());

  if (Context.hadError()) {
    return std::nullopt;
  }

  assert(GenModule && "Emitted no diagnostics but IR generation failed?");
  auto *Module = GenModule.getModule();

  // Run LLVM passes on the resulting module
  performLLVM(IRGenOpts, Context.Diags, /*diagMutex*/ nullptr,
              /*hash*/ nullptr, Module, GenModule.getTargetMachine(),
              CI.getPrimarySpecificPathsForAtMostOnePrimary().OutputFilename,
              CI.getOutputBackend(), Context.Stats);

  if (Context.hadError()) {
    return std::nullopt;
  }

  return GenModule;
}

/// Log a compilation error to standard error
static void logError(llvm::Error Err) {
  logAllUnhandledErrors(std::move(Err), llvm::errs(), "");
}

std::unique_ptr<LazySwiftMaterializationUnit>
LazySwiftMaterializationUnit::Create(SwiftJIT &JIT, CompilerInstance &CI) {
  auto *M = CI.getMainModule();
  TBDGenOptions Opts;
  Opts.PublicOrPackageSymbolsOnly = false;
  auto TBDDesc = TBDGenDescriptor::forModule(M, std::move(Opts));
  SymbolSourceMapRequest SourceReq{TBDDesc};
  const auto *Sources = evaluateOrFatal(
      M->getASTContext().evaluator,
      std::move(SourceReq));
  llvm::orc::SymbolFlagsMap PublicInterface;
  for (const auto &Entry : *Sources) {
    const auto &Source = Entry.getValue();
    const auto &SymbolName = Entry.getKey();
    auto Flags = Source.getJITSymbolFlags();
    if (Flags.isCallable()) {
      // Only create lazy reexports for callable symbols
      auto MangledName = mangle(SymbolName);
      PublicInterface[JIT.intern(MangledName)] = Flags;
    } else {
      PublicInterface[JIT.intern(SymbolName)] = Flags;
    }
  }
  return std::unique_ptr<LazySwiftMaterializationUnit>(
      new LazySwiftMaterializationUnit(JIT, CI, std::move(Sources),
                                       std::move(PublicInterface)));
}

StringRef LazySwiftMaterializationUnit::getName() const {
  return "SwiftMaterializationUnit";
}

LazySwiftMaterializationUnit::LazySwiftMaterializationUnit(
    SwiftJIT &JIT, CompilerInstance &CI, const SymbolSourceMap *Sources,
    llvm::orc::SymbolFlagsMap Symbols)
    : MaterializationUnit({std::move(Symbols), nullptr}), Sources(Sources),
      JIT(JIT), CI(CI) {}

void LazySwiftMaterializationUnit::materialize(
    std::unique_ptr<llvm::orc::MaterializationResponsibility> MR) {
  SymbolSources Entities;
  const auto &RS = MR->getRequestedSymbols();
  for (auto &Sym : RS) {
    auto Name = demangle(*Sym);
    auto itr = Sources->find(Name);
    assert(itr != Sources->end() && "Requested symbol doesn't have source?");
    const auto &Source = itr->getValue();
    Source.typecheck();
    if (CI.getASTContext().hadError()) {
      // If encounter type error, bail out
      MR->failMaterialization();
      return;
    }
    Entities.push_back(Source);
  }
  auto SM = performASTLowering(CI, std::move(Entities));

  // Promote linkages of SIL entities
  // defining requested symbols so they are
  // emitted during IRGen.
  SM->promoteLinkages();

  runSILDiagnosticPasses(*SM);
  runSILLoweringPasses(*SM);
  auto GM = generateModule(CI, std::move(SM));
  if (!GM) {
    MR->failMaterialization();
    return;
  }
  auto *Module = GM->getModule();

  // All renamings defined by `MR`, e.g. "foo" -> "foo$impl"
  llvm::StringMap<llvm::orc::SymbolStringPtr> Renamings;
  for (auto &[Sym, Flags] : MR->getSymbols()) {
    Renamings[demangle(*Sym)] = Sym;
  }
  // Now we must register all other public symbols defined by
  // the module with the JIT
  llvm::orc::SymbolFlagsMap LazilyDiscoveredSymbols;

  // All symbols defined by the compiled module in `MR`
  llvm::DenseSet<llvm::orc::SymbolStringPtr> DefinedSymbols;

  // Register all global values, including global
  // variables and functions
  for (auto &GV : Module->global_values()) {
    auto Name = JIT.mangle(GV.getName());
    auto itr = Renamings.find(Name);
    if (GV.hasAppendingLinkage() || GV.isDeclaration()) {
      continue;
    }
    if (itr == Renamings.end()) {
      if (GV.hasLocalLinkage()) {
        continue;
      }
      LazilyDiscoveredSymbols[JIT.intern(Name)] =
          llvm::JITSymbolFlags::fromGlobalValue(GV);
      // Ignore all symbols that will not appear in symbol table
    } else {
      // Promote linkage of requested symbols that will
      // not appear in symbol table otherwise
      if (GV.hasLocalLinkage()) {
        GV.setLinkage(llvm::GlobalValue::ExternalLinkage);
        GV.setVisibility(llvm::GlobalValue::HiddenVisibility);
      }
      DefinedSymbols.insert(itr->getValue());
    }
  }

  llvm::orc::SymbolFlagsMap UnrequestedSymbols;
  for (auto &[Sym, Flags] : MR->getSymbols()) {
    if (!DefinedSymbols.contains(Sym)) {
      UnrequestedSymbols[Sym] = Flags;
    }
  }
  std::unique_ptr<MaterializationUnit> UnrequestedMU(
      new LazySwiftMaterializationUnit(JIT, CI, Sources,
                                       std::move(UnrequestedSymbols)));
  if (auto Err = MR->replace(std::move(UnrequestedMU))) {
    logError(std::move(Err));
    MR->failMaterialization();
    return;
  }
  if (auto Err = MR->defineMaterializing(std::move(LazilyDiscoveredSymbols))) {
    logError(std::move(Err));
    MR->failMaterialization();
    return;
  }
  auto TSM = std::move(*GM).intoThreadSafeContext();
  JIT.getIRCompileLayer().emit(std::move(MR), std::move(TSM));
}

llvm::Error
SwiftJIT::addSwift(llvm::orc::JITDylib &JD,
                   std::unique_ptr<llvm::orc::MaterializationUnit> MU) {
  // Create stub map.
  llvm::orc::SymbolAliasMap Stubs;
  for (auto &[Name, Flags] : MU->getSymbols()) {
    if (isMangled(*Name)) {
      // Create a stub for mangled functions
      auto OriginalName = demangle(*Name);
      Stubs.insert(
          {J->getExecutionSession().intern(OriginalName), {Name, Flags}});
    }
  }
  assert(ISM.get() && "No ISM?");

  if (!Stubs.empty())
    if (auto Err = JD.define(lazyReexports(LCTM, *ISM, JD, std::move(Stubs))))
      return Err;

  return JD.define(std::move(MU));
}

void LazySwiftMaterializationUnit::discard(
    const llvm::orc::JITDylib &JD, const llvm::orc::SymbolStringPtr &Sym) {}

EagerSwiftMaterializationUnit::EagerSwiftMaterializationUnit(
    SwiftJIT &JIT, const CompilerInstance &CI, const IRGenOptions &IRGenOpts,
    std::unique_ptr<SILModule> SM)
    : MaterializationUnit(getInterface(JIT, CI)), JIT(JIT), CI(CI),
      IRGenOpts(IRGenOpts), SM(std::move(SM)) {}

StringRef EagerSwiftMaterializationUnit::getName() const {
  return "EagerSwiftMaterializationUnit";
}

void EagerSwiftMaterializationUnit::materialize(
    std::unique_ptr<llvm::orc::MaterializationResponsibility> R) {

  auto GenModule = generateModule(CI, std::move(SM));

  if (!GenModule) {
    R->failMaterialization();
    return;
  }

  auto *Module = GenModule->getModule();

  // Dump IR if requested
  dumpJIT(*Module);

  // Now we must register all other public symbols defined by
  // the module with the JIT
  llvm::orc::SymbolFlagsMap Symbols;
  // Register all global values, including global
  // variables and functions
  for (const auto &GV : Module->global_values()) {
    // Ignore all symbols that will not appear in symbol table
    if (GV.hasLocalLinkage() || GV.isDeclaration() || GV.hasAppendingLinkage())
      continue;
    auto Name = GV.getName();
    // The entry point is already registered up front with the
    // interface, so ignore it as well
    if (Name == CI.getASTContext().getEntryPointFunctionName())
      continue;
    auto MangledName = JIT.mangleAndIntern(Name);
    // Register this symbol with the proper flags
    Symbols[MangledName] = llvm::JITSymbolFlags::fromGlobalValue(GV);
  }
  // Register the symbols we have discovered with the JIT
  if (auto Err = R->defineMaterializing(Symbols)) {
    logError(std::move(Err));
  }
  auto TSM = std::move(*GenModule).intoThreadSafeContext();
  JIT.getIRCompileLayer().emit(std::move(R), std::move(TSM));
}

llvm::orc::MaterializationUnit::Interface
EagerSwiftMaterializationUnit::getInterface(SwiftJIT &JIT,
                                            const CompilerInstance &CI) {
  const auto &EntryPoint = CI.getASTContext().getEntryPointFunctionName();
  auto MangledEntryPoint = JIT.mangleAndIntern(EntryPoint);
  auto Flags = llvm::JITSymbolFlags::Callable | llvm::JITSymbolFlags::Exported;
  llvm::orc::SymbolFlagsMap Symbols{{MangledEntryPoint, Flags}};
  return {std::move(Symbols), nullptr};
}

void EagerSwiftMaterializationUnit::discard(
    const llvm::orc::JITDylib &JD, const llvm::orc::SymbolStringPtr &Sym) {}

static void DumpLLVMIR(const llvm::Module &M) {
  std::string path = (M.getName() + ".ll").str();
  for (size_t count = 0; llvm::sys::fs::exists(path);)
    path = (M.getName() + llvm::utostr(count++) + ".ll").str();

  std::error_code error;
  llvm::raw_fd_ostream stream(path, error);
  if (error)
    return;
  M.print(stream, /*AssemblyAnnotationWriter=*/nullptr);
}

void EagerSwiftMaterializationUnit::dumpJIT(const llvm::Module &M) {
  LLVM_DEBUG(llvm::dbgs() << "Module to be executed:\n"; M.dump());
  switch (IRGenOpts.DumpJIT) {
  case JITDebugArtifact::None:
    break;
  case JITDebugArtifact::LLVMIR:
    DumpLLVMIR(M);
    break;
  case JITDebugArtifact::Object:
    JIT.getObjTransformLayer().setTransform(llvm::orc::DumpObjects());
    break;
  }
}
