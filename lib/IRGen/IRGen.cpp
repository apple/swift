//===--- IRGen.cpp - Swift LLVM IR Generation -----------------------------===//
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
//  This file implements the entrypoints into IR generation.
//
//===----------------------------------------------------------------------===//

#define DEBUG_TYPE "irgen"
#include "IRGenModule.h"
#include "swift/ABI/MetadataValues.h"
#include "swift/AST/DiagnosticsIRGen.h"
#include "swift/AST/IRGenOptions.h"
#include "swift/AST/IRGenRequests.h"
#include "swift/AST/LinkLibrary.h"
#include "swift/AST/ProtocolConformance.h"
#include "swift/AST/SILGenRequests.h"
#include "swift/AST/SILOptimizerRequests.h"
#include "swift/AST/TBDGenRequests.h"
#include "swift/Basic/Defer.h"
#include "swift/Basic/Dwarf.h"
#include "swift/Basic/Platform.h"
#include "swift/Basic/Statistic.h"
#include "swift/Basic/Version.h"
#include "swift/ClangImporter/ClangImporter.h"
#include "swift/ClangImporter/ClangModule.h"
#include "swift/IRGen/IRGenPublic.h"
#include "swift/IRGen/IRGenSILPasses.h"
#include "swift/LLVMPasses/Passes.h"
#include "swift/LLVMPasses/PassesFwd.h"
#include "swift/SIL/SILModule.h"
#include "swift/SIL/SILRemarkStreamer.h"
#include "swift/SILOptimizer/PassManager/PassManager.h"
#include "swift/SILOptimizer/PassManager/PassPipeline.h"
#include "swift/SILOptimizer/PassManager/Passes.h"
#include "swift/Subsystems.h"
#include "swift/TBDGen/TBDGen.h"
#include "../Serialization/ModuleFormat.h"
#include "clang/Basic/TargetInfo.h"
#include "clang/Frontend/CompilerInstance.h"
#include "llvm/ADT/StringSet.h"
#include "llvm/Analysis/AliasAnalysis.h"
#include "llvm/Bitcode/BitcodeWriter.h"
#include "llvm/Bitcode/BitcodeWriterPass.h"
#include "llvm/CodeGen/BasicTTIImpl.h"
#include "llvm/CodeGen/TargetSubtargetInfo.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/IRPrintingPasses.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/LegacyPassManager.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/ValueSymbolTable.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Linker/Linker.h"
#include "llvm/MC/SubtargetFeature.h"
#include "llvm/Object/ObjectFile.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/FormattedStream.h"
#include "llvm/Support/MD5.h"
#include "llvm/Support/Mutex.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/TargetRegistry.h"
#include "llvm/Target/TargetMachine.h"
#include "llvm/Transforms/Coroutines.h"
#include "llvm/Transforms/IPO.h"
#include "llvm/Transforms/IPO/AlwaysInliner.h"
#include "llvm/Transforms/IPO/PassManagerBuilder.h"
#include "llvm/Transforms/Instrumentation.h"
#include "llvm/Transforms/Instrumentation/AddressSanitizer.h"
#include "llvm/Transforms/Instrumentation/SanitizerCoverage.h"
#include "llvm/Transforms/Instrumentation/ThreadSanitizer.h"
#include "llvm/Transforms/ObjCARC.h"

#include <thread>

#if HAVE_UNISTD_H
#include <unistd.h>
#endif

using namespace swift;
using namespace irgen;
using namespace llvm;

static cl::opt<bool> DisableObjCARCContract(
    "disable-objc-arc-contract", cl::Hidden,
    cl::desc("Disable running objc arc contract for testing purposes"));

// This option is for performance benchmarking: to ensure a consistent
// performance data, modules are aligned to the page size.
// Warning: this blows up the text segment size. So use this option only for
// performance benchmarking.
static cl::opt<bool> AlignModuleToPageSize(
    "align-module-to-page-size", cl::Hidden,
    cl::desc("Align the text section of all LLVM modules to the page size"));

namespace {
// We need this to access IRGenOptions from extension functions
class PassManagerBuilderWrapper : public PassManagerBuilder {
public:
  const IRGenOptions &IRGOpts;
  PassManagerBuilderWrapper(const IRGenOptions &IRGOpts)
      : PassManagerBuilder(), IRGOpts(IRGOpts) {}
};
} // end anonymous namespace

static void addSwiftARCOptPass(const PassManagerBuilder &Builder,
                               PassManagerBase &PM) {
  if (Builder.OptLevel > 0)
    PM.add(createSwiftARCOptPass());
}

static void addSwiftContractPass(const PassManagerBuilder &Builder,
                               PassManagerBase &PM) {
  if (Builder.OptLevel > 0)
    PM.add(createSwiftARCContractPass());
}

static void addAddressSanitizerPasses(const PassManagerBuilder &Builder,
                                      legacy::PassManagerBase &PM) {
  auto &BuilderWrapper =
      static_cast<const PassManagerBuilderWrapper &>(Builder);
  auto recover =
      bool(BuilderWrapper.IRGOpts.SanitizersWithRecoveryInstrumentation &
           SanitizerKind::Address);
  auto useODRIndicator = BuilderWrapper.IRGOpts.SanitizeAddressUseODRIndicator;
  PM.add(createAddressSanitizerFunctionPass(/*CompileKernel=*/false, recover));
  PM.add(createModuleAddressSanitizerLegacyPassPass(
      /*CompileKernel=*/false, recover, /*UseGlobalsGC=*/true,
      useODRIndicator));
}

static void addThreadSanitizerPass(const PassManagerBuilder &Builder,
                                   legacy::PassManagerBase &PM) {
  PM.add(createThreadSanitizerLegacyPassPass());
}

static void addSanitizerCoveragePass(const PassManagerBuilder &Builder,
                                     legacy::PassManagerBase &PM) {
  const PassManagerBuilderWrapper &BuilderWrapper =
      static_cast<const PassManagerBuilderWrapper &>(Builder);
  PM.add(createModuleSanitizerCoverageLegacyPassPass(
      BuilderWrapper.IRGOpts.SanitizeCoverage));
}

std::tuple<llvm::TargetOptions, std::string, std::vector<std::string>,
           std::string>
swift::getIRTargetOptions(const IRGenOptions &Opts, ASTContext &Ctx) {
  // Things that maybe we should collect from the command line:
  //   - relocation model
  //   - code model
  // FIXME: We should do this entirely through Clang, for consistency.
  TargetOptions TargetOpts;

  // Explicitly request debugger tuning for LLDB which is the default
  // on Darwin platforms but not on others.
  TargetOpts.DebuggerTuning = llvm::DebuggerKind::LLDB;
  TargetOpts.FunctionSections = Opts.FunctionSections;

  auto *Clang = static_cast<ClangImporter *>(Ctx.getClangModuleLoader());

  // WebAssembly doesn't support atomics yet, see https://bugs.swift.org/browse/SR-12097
  // for more details.
  if (Clang->getTargetInfo().getTriple().isOSBinFormatWasm())
    TargetOpts.ThreadModel = llvm::ThreadModel::Single;

  clang::TargetOptions &ClangOpts = Clang->getTargetInfo().getTargetOpts();
  return std::make_tuple(TargetOpts, ClangOpts.CPU, ClangOpts.Features, ClangOpts.Triple);
}

void setModuleFlags(IRGenModule &IGM) {

  auto *Module = IGM.getModule();

  // These module flags don't affect code generation; they just let us
  // error during LTO if the user tries to combine files across ABIs.
  Module->addModuleFlag(llvm::Module::Error, "Swift Version",
                        IRGenModule::swiftVersion);
}

void swift::performLLVMOptimizations(const IRGenOptions &Opts,
                                     llvm::Module *Module,
                                     llvm::TargetMachine *TargetMachine) {
  // Set up a pipeline.
  PassManagerBuilderWrapper PMBuilder(Opts);

  // If we're generating a profile, add the lowering pass now.
  if (Opts.GenerateProfile) {
    // TODO: Surface the option to emit atomic profile counter increments at
    // the driver level.
    // Configure the module passes.
    legacy::PassManager ModulePasses;
    ModulePasses.add(createTargetTransformInfoWrapperPass(
        TargetMachine->getTargetIRAnalysis()));
    InstrProfOptions Options;
    Options.Atomic = bool(Opts.Sanitizers & SanitizerKind::Thread);
    ModulePasses.add(createInstrProfilingLegacyPass(Options));
    ModulePasses.run(*Module);
  }

  if (Opts.shouldOptimize() && !Opts.DisableLLVMOptzns) {
    PMBuilder.OptLevel = 2; // -Os
    PMBuilder.SizeLevel = 1; // -Os
    PMBuilder.Inliner = llvm::createFunctionInliningPass(200);
    PMBuilder.SLPVectorize = true;
    PMBuilder.LoopVectorize = true;
    PMBuilder.MergeFunctions = true;
  } else {
    PMBuilder.OptLevel = 0;
    if (!Opts.DisableLLVMOptzns)
      PMBuilder.Inliner =
        llvm::createAlwaysInlinerLegacyPass(/*insertlifetime*/false);
  }

  bool RunSwiftSpecificLLVMOptzns =
      !Opts.DisableSwiftSpecificLLVMOptzns && !Opts.DisableLLVMOptzns;

  // If the optimizer is enabled, we run the ARCOpt pass in the scalar optimizer
  // and the Contract pass as late as possible.
  if (RunSwiftSpecificLLVMOptzns) {
    PMBuilder.addExtension(PassManagerBuilder::EP_ScalarOptimizerLate,
                           addSwiftARCOptPass);
    PMBuilder.addExtension(PassManagerBuilder::EP_OptimizerLast,
                           addSwiftContractPass);
  }

  if (!Opts.DisableSwiftSpecificLLVMOptzns)
    addCoroutinePassesToExtensionPoints(PMBuilder);

  if (Opts.Sanitizers & SanitizerKind::Address) {
    PMBuilder.addExtension(PassManagerBuilder::EP_OptimizerLast,
                           addAddressSanitizerPasses);
    PMBuilder.addExtension(PassManagerBuilder::EP_EnabledOnOptLevel0,
                           addAddressSanitizerPasses);
  }
  
  if (Opts.Sanitizers & SanitizerKind::Thread) {
    PMBuilder.addExtension(PassManagerBuilder::EP_OptimizerLast,
                           addThreadSanitizerPass);
    PMBuilder.addExtension(PassManagerBuilder::EP_EnabledOnOptLevel0,
                           addThreadSanitizerPass);
  }

  if (Opts.SanitizeCoverage.CoverageType !=
      llvm::SanitizerCoverageOptions::SCK_None) {

    PMBuilder.addExtension(PassManagerBuilder::EP_OptimizerLast,
                           addSanitizerCoveragePass);
    PMBuilder.addExtension(PassManagerBuilder::EP_EnabledOnOptLevel0,
                           addSanitizerCoveragePass);
  }
  if (RunSwiftSpecificLLVMOptzns) {
    PMBuilder.addExtension(PassManagerBuilder::EP_OptimizerLast,
      [&](const PassManagerBuilder &Builder, PassManagerBase &PM) {
        if (Builder.OptLevel > 0) {
          const PointerAuthSchema &schema = Opts.PointerAuth.FunctionPointers;
          unsigned key = (schema.isEnabled() ? schema.getKey() : 0);
          PM.add(createSwiftMergeFunctionsPass(schema.isEnabled(), key));
        }
      });
  }

  // Configure the function passes.
  legacy::FunctionPassManager FunctionPasses(Module);
  FunctionPasses.add(createTargetTransformInfoWrapperPass(
      TargetMachine->getTargetIRAnalysis()));
  if (Opts.Verify)
    FunctionPasses.add(createVerifierPass());
  PMBuilder.populateFunctionPassManager(FunctionPasses);

  // The PMBuilder only knows about LLVM AA passes.  We should explicitly add
  // the swift AA pass after the other ones.
  if (RunSwiftSpecificLLVMOptzns) {
    FunctionPasses.add(createSwiftAAWrapperPass());
    FunctionPasses.add(createExternalAAWrapperPass([](Pass &P, Function &,
                                                      AAResults &AAR) {
      if (auto *WrapperPass = P.getAnalysisIfAvailable<SwiftAAWrapperPass>())
        AAR.addAAResult(WrapperPass->getResult());
    }));
  }

  // Run the function passes.
  FunctionPasses.doInitialization();
  for (auto I = Module->begin(), E = Module->end(); I != E; ++I)
    if (!I->isDeclaration())
      FunctionPasses.run(*I);
  FunctionPasses.doFinalization();

  // Configure the module passes.
  legacy::PassManager ModulePasses;
  ModulePasses.add(createTargetTransformInfoWrapperPass(
      TargetMachine->getTargetIRAnalysis()));

  PMBuilder.populateModulePassManager(ModulePasses);

  // The PMBuilder only knows about LLVM AA passes.  We should explicitly add
  // the swift AA pass after the other ones.
  if (RunSwiftSpecificLLVMOptzns) {
    ModulePasses.add(createSwiftAAWrapperPass());
    ModulePasses.add(createExternalAAWrapperPass([](Pass &P, Function &,
                                                    AAResults &AAR) {
      if (auto *WrapperPass = P.getAnalysisIfAvailable<SwiftAAWrapperPass>())
        AAR.addAAResult(WrapperPass->getResult());
    }));
  }

  if (Opts.Verify)
    ModulePasses.add(createVerifierPass());

  if (Opts.PrintInlineTree)
    ModulePasses.add(createInlineTreePrinterPass());

  // Make sure we do ARC contraction under optimization.  We don't
  // rely on any other LLVM ARC transformations, but we do need ARC
  // contraction to add the objc_retainAutoreleasedReturnValue
  // assembly markers and remove clang.arc.used.
  if (Opts.shouldOptimize() && !DisableObjCARCContract)
    ModulePasses.add(createObjCARCContractPass());

  // Do it.
  ModulePasses.run(*Module);

  if (AlignModuleToPageSize) {
    // For performance benchmarking: Align the module to the page size by
    // aligning the first function of the module.
    unsigned pageSize =
#if HAVE_UNISTD_H
      sysconf(_SC_PAGESIZE));
#else
      4096; // Use a default value
#endif
    for (auto I = Module->begin(), E = Module->end(); I != E; ++I) {
      if (!I->isDeclaration()) {
        I->setAlignment(llvm::MaybeAlign(pageSize));
        break;
      }
    }
  }
}

namespace {
/// An output stream which calculates the MD5 hash of the streamed data.
class MD5Stream : public llvm::raw_ostream {
private:

  uint64_t Pos = 0;
  llvm::MD5 Hash;

  void write_impl(const char *Ptr, size_t Size) override {
    Hash.update(ArrayRef<uint8_t>(reinterpret_cast<const uint8_t *>(Ptr), Size));
    Pos += Size;
  }

  uint64_t current_pos() const override { return Pos; }

public:

  void final(MD5::MD5Result &Result) {
    flush();
    Hash.final(Result);
  }
};
} // end anonymous namespace

/// Computes the MD5 hash of the llvm \p Module including the compiler version
/// and options which influence the compilation.
static MD5::MD5Result getHashOfModule(const IRGenOptions &Opts,
                                      const llvm::Module *Module) {
  // Calculate the hash of the whole llvm module.
  MD5Stream HashStream;
  llvm::WriteBitcodeToFile(*Module, HashStream);

  // Update the hash with the compiler version. We want to recompile if the
  // llvm pipeline of the compiler changed.
  HashStream << version::getSwiftFullVersion();

  // Add all options which influence the llvm compilation but are not yet
  // reflected in the llvm module itself.
  Opts.writeLLVMCodeGenOptionsTo(HashStream);

  MD5::MD5Result result;
  HashStream.final(result);
  return result;
}

/// Returns false if the hash of the current module \p HashData matches the
/// hash which is stored in an existing output object file.
static bool needsRecompile(StringRef OutputFilename, ArrayRef<uint8_t> HashData,
                           llvm::GlobalVariable *HashGlobal,
                           llvm::sys::Mutex *DiagMutex) {
  if (OutputFilename.empty())
    return true;

  auto BinaryOwner = object::createBinary(OutputFilename);
  if (!BinaryOwner) {
    consumeError(BinaryOwner.takeError());
    return true;
  }
  auto *ObjectFile = dyn_cast<object::ObjectFile>(BinaryOwner->getBinary());
  if (!ObjectFile)
    return true;

  StringRef HashSectionName = HashGlobal->getSection();
  // Strip the segment name. For mach-o the GlobalVariable's section name format
  // is <segment>,<section>.
  size_t Comma = HashSectionName.find_last_of(',');
  if (Comma != StringRef::npos)
    HashSectionName = HashSectionName.substr(Comma + 1);

  // Search for the section which holds the hash.
  for (auto &Section : ObjectFile->sections()) {
    llvm::Expected<StringRef> SectionNameOrErr = Section.getName();
    if (!SectionNameOrErr) {
      llvm::consumeError(SectionNameOrErr.takeError());
      continue;
    }

    StringRef SectionName = *SectionNameOrErr;
    if (SectionName == HashSectionName) {
      llvm::Expected<llvm::StringRef> SectionData = Section.getContents();
      if (!SectionData) {
        return true;
      }
      ArrayRef<uint8_t> PrevHashData(
          reinterpret_cast<const uint8_t *>(SectionData->data()),
          SectionData->size());
      LLVM_DEBUG(if (PrevHashData.size() == sizeof(MD5::MD5Result)) {
        if (DiagMutex) DiagMutex->lock();
        SmallString<32> HashStr;
        MD5::stringifyResult(
            *reinterpret_cast<MD5::MD5Result *>(
                const_cast<unsigned char *>(PrevHashData.data())),
            HashStr);
        llvm::dbgs() << OutputFilename << ": prev MD5=" << HashStr <<
          (HashData == PrevHashData ? " skipping\n" : " recompiling\n");
        if (DiagMutex) DiagMutex->unlock();
      });
      if (HashData == PrevHashData)
        return false;

      return true;
    }
  }
  return true;
}

static void countStatsPostIRGen(UnifiedStatsReporter &Stats,
                                const llvm::Module& Module) {
  auto &C = Stats.getFrontendCounters();
  // FIXME: calculate these in constant time if possible.
  C.NumIRGlobals += Module.getGlobalList().size();
  C.NumIRFunctions += Module.getFunctionList().size();
  C.NumIRAliases += Module.getAliasList().size();
  C.NumIRIFuncs += Module.getIFuncList().size();
  C.NumIRNamedMetaData += Module.getNamedMDList().size();
  C.NumIRValueSymbols += Module.getValueSymbolTable().size();
  C.NumIRComdatSymbols += Module.getComdatSymbolTable().size();
  for (auto const &Func : Module) {
    for (auto const &BB : Func) {
      ++C.NumIRBasicBlocks;
      C.NumIRInsts += BB.size();
    }
  }
}

template<typename ...ArgTypes>
void
diagnoseSync(DiagnosticEngine &Diags, llvm::sys::Mutex *DiagMutex,
             SourceLoc Loc, Diag<ArgTypes...> ID,
             typename swift::detail::PassArgument<ArgTypes>::type... Args) {
  if (DiagMutex)
    DiagMutex->lock();

  Diags.diagnose(Loc, ID, std::move(Args)...);

  if (DiagMutex)
    DiagMutex->unlock();
}

/// Run the LLVM passes. In multi-threaded compilation this will be done for
/// multiple LLVM modules in parallel.
bool swift::performLLVM(const IRGenOptions &Opts,
                        DiagnosticEngine &Diags,
                        llvm::sys::Mutex *DiagMutex,
                        llvm::GlobalVariable *HashGlobal,
                        llvm::Module *Module,
                        llvm::TargetMachine *TargetMachine,
                        StringRef OutputFilename,
                        UnifiedStatsReporter *Stats) {

  if (Opts.UseIncrementalLLVMCodeGen && HashGlobal) {
    // Check if we can skip the llvm part of the compilation if we have an
    // existing object file which was generated from the same llvm IR.
    auto hash = getHashOfModule(Opts, Module);

    LLVM_DEBUG(
      if (DiagMutex) DiagMutex->lock();
      SmallString<32> ResultStr;
      MD5::stringifyResult(hash, ResultStr);
      llvm::dbgs() << OutputFilename << ": MD5=" << ResultStr << '\n';
      if (DiagMutex) DiagMutex->unlock();
    );

    ArrayRef<uint8_t> HashData(reinterpret_cast<uint8_t *>(&hash),
                               sizeof(hash));
    if (Opts.OutputKind == IRGenOutputKind::ObjectFile &&
        !Opts.PrintInlineTree &&
        !needsRecompile(OutputFilename, HashData, HashGlobal, DiagMutex)) {
      // The llvm IR did not change. We don't need to re-create the object file.
      return false;
    }

    // Store the hash in the global variable so that it is written into the
    // object file.
    auto *HashConstant = ConstantDataArray::get(Module->getContext(), HashData);
    HashGlobal->setInitializer(HashConstant);
  }

  Optional<raw_fd_ostream> RawOS;
  if (!OutputFilename.empty()) {
    // Try to open the output file.  Clobbering an existing file is fine.
    // Open in binary mode if we're doing binary output.
    llvm::sys::fs::OpenFlags OSFlags = llvm::sys::fs::F_None;
    std::error_code EC;
    RawOS.emplace(OutputFilename, EC, OSFlags);

    if (RawOS->has_error() || EC) {
      diagnoseSync(Diags, DiagMutex,
                   SourceLoc(), diag::error_opening_output,
                   OutputFilename, EC.message());
      RawOS->clear_error();
      return true;
    }
    if (Opts.OutputKind == IRGenOutputKind::LLVMAssemblyBeforeOptimization) {
      Module->print(RawOS.getValue(), nullptr);
      return false;
    }
  } else {
    assert(Opts.OutputKind == IRGenOutputKind::Module && "no output specified");
  }

  performLLVMOptimizations(Opts, Module, TargetMachine);

  if (Stats) {
    if (DiagMutex)
      DiagMutex->lock();
    countStatsPostIRGen(*Stats, *Module);
    if (DiagMutex)
      DiagMutex->unlock();
  }

  if (!RawOS)
    return false;

  return compileAndWriteLLVM(Module, TargetMachine, Opts, Stats, Diags, *RawOS,
                             DiagMutex);
}

bool swift::compileAndWriteLLVM(llvm::Module *module,
                                llvm::TargetMachine *targetMachine,
                                const IRGenOptions &opts,
                                UnifiedStatsReporter *stats,
                                DiagnosticEngine &diags,
                                llvm::raw_pwrite_stream &out,
                                llvm::sys::Mutex *diagMutex) {
  legacy::PassManager EmitPasses;

  // Set up the final emission passes.
  switch (opts.OutputKind) {
  case IRGenOutputKind::LLVMAssemblyBeforeOptimization:
    llvm_unreachable("Should be handled earlier.");
  case IRGenOutputKind::Module:
    break;
  case IRGenOutputKind::LLVMAssemblyAfterOptimization:
    EmitPasses.add(createPrintModulePass(out));
    break;
  case IRGenOutputKind::LLVMBitcode: {
    if (opts.LLVMLTOKind == IRGenLLVMLTOKind::Thin) {
      EmitPasses.add(createWriteThinLTOBitcodePass(out));
    } else {
      EmitPasses.add(createBitcodeWriterPass(out));
    }
    break;
  }
  case IRGenOutputKind::NativeAssembly:
  case IRGenOutputKind::ObjectFile: {
    CodeGenFileType FileType;
    FileType =
        (opts.OutputKind == IRGenOutputKind::NativeAssembly ? CGFT_AssemblyFile
                                                            : CGFT_ObjectFile);
    EmitPasses.add(createTargetTransformInfoWrapperPass(
        targetMachine->getTargetIRAnalysis()));

    bool fail = targetMachine->addPassesToEmitFile(EmitPasses, out, nullptr,
                                                   FileType, !opts.Verify);
    if (fail) {
      diagnoseSync(diags, diagMutex, SourceLoc(),
                   diag::error_codegen_init_fail);
      return true;
    }
    break;
  }
  }

  EmitPasses.run(*module);

  if (stats) {
    if (diagMutex)
      diagMutex->lock();
    stats->getFrontendCounters().NumLLVMBytesOutput += out.tell();
    if (diagMutex)
      diagMutex->unlock();
  }
  return false;
}

static void setPointerAuthOptions(PointerAuthOptions &opts,
                                  const clang::PointerAuthOptions &clangOpts){
  // Intentionally do a slice-assignment to copy over the clang options.
  static_cast<clang::PointerAuthOptions&>(opts) = clangOpts;

  assert(clangOpts.FunctionPointers);
  if (clangOpts.FunctionPointers.getKind() != PointerAuthSchema::Kind::ARM8_3)
    return;

  using Discrimination = PointerAuthSchema::Discrimination;

  // A key suitable for code pointers that might be used anywhere in the ABI.
  auto codeKey = clangOpts.FunctionPointers.getARM8_3Key();

  // A key suitable for data pointers that might be used anywhere in the ABI.
  // Using a data key for data pointers and vice-versa is important for
  // ABI future-proofing.
  auto dataKey = PointerAuthSchema::ARM8_3Key::ASDA;

  // A key suitable for code pointers that are only used in private
  // situations.  Do not use this key for any sort of signature that
  // might end up on a global constant initializer.
  auto nonABICodeKey = PointerAuthSchema::ARM8_3Key::ASIB;

  // A key suitable for data pointers that are only used in private
  // situations.  Do not use this key for any sort of signature that
  // might end up on a global constant initializer.
  auto nonABIDataKey = PointerAuthSchema::ARM8_3Key::ASDB;

  // If you change anything here, be sure to update <ptrauth.h>.
  opts.SwiftFunctionPointers =
    PointerAuthSchema(codeKey, /*address*/ false, Discrimination::Type);
  opts.KeyPaths =
    PointerAuthSchema(codeKey, /*address*/ true, Discrimination::Decl);
  opts.ValueWitnesses =
    PointerAuthSchema(codeKey, /*address*/ true, Discrimination::Decl);
  opts.ProtocolWitnesses =
    PointerAuthSchema(codeKey, /*address*/ true, Discrimination::Decl);
  opts.ProtocolAssociatedTypeAccessFunctions =
    PointerAuthSchema(codeKey, /*address*/ true, Discrimination::Decl);
  opts.ProtocolAssociatedTypeWitnessTableAccessFunctions =
    PointerAuthSchema(codeKey, /*address*/ true, Discrimination::Decl);
  opts.SwiftClassMethods =
    PointerAuthSchema(codeKey, /*address*/ true, Discrimination::Decl);
  opts.SwiftClassMethodPointers =
    PointerAuthSchema(codeKey, /*address*/ false, Discrimination::Decl);
  opts.HeapDestructors =
    PointerAuthSchema(codeKey, /*address*/ true, Discrimination::Decl);

  // Partial-apply captures are not ABI and can use a more aggressive key.
  opts.PartialApplyCapture =
    PointerAuthSchema(nonABICodeKey, /*address*/ true, Discrimination::Decl);

  opts.TypeDescriptors =
    PointerAuthSchema(dataKey, /*address*/ true, Discrimination::Decl);
  opts.TypeDescriptorsAsArguments =
    PointerAuthSchema(dataKey, /*address*/ false, Discrimination::Decl);

  opts.SwiftDynamicReplacements =
    PointerAuthSchema(codeKey, /*address*/ true, Discrimination::Decl);
  opts.SwiftDynamicReplacementKeys =
    PointerAuthSchema(dataKey, /*address*/ true, Discrimination::Decl);

  opts.ProtocolConformanceDescriptors =
      PointerAuthSchema(dataKey, /*address*/ true, Discrimination::Decl);
  opts.ProtocolConformanceDescriptorsAsArguments =
      PointerAuthSchema(dataKey, /*address*/ false, Discrimination::Decl);

  // Coroutine resumption functions are never stored globally in the ABI,
  // so we can do some things that aren't normally okay to do.  However,
  // we can't use ASIB because that would break ARM64 interoperation.
  // The address used in the discrimination is not the address where the
  // function pointer is signed, but the address of the coroutine buffer.
  opts.YieldManyResumeFunctions =
      PointerAuthSchema(codeKey, /*address*/ true, Discrimination::Type);
  opts.YieldOnceResumeFunctions =
      PointerAuthSchema(codeKey, /*address*/ true, Discrimination::Type);

  opts.ResilientClassStubInitCallbacks =
      PointerAuthSchema(codeKey, /*address*/ true, Discrimination::Constant,
      SpecialPointerAuthDiscriminators::ResilientClassStubInitCallback);

  opts.AsyncSwiftFunctionPointers =
      PointerAuthSchema(dataKey, /*address*/ false, Discrimination::Type);

  opts.AsyncSwiftClassMethods =
      PointerAuthSchema(dataKey, /*address*/ true, Discrimination::Decl);

  opts.AsyncProtocolWitnesses =
      PointerAuthSchema(dataKey, /*address*/ true, Discrimination::Decl);

  opts.AsyncSwiftClassMethodPointers =
      PointerAuthSchema(dataKey, /*address*/ false, Discrimination::Decl);

  opts.AsyncSwiftDynamicReplacements =
      PointerAuthSchema(dataKey, /*address*/ true, Discrimination::Decl);

  opts.AsyncPartialApplyCapture =
      PointerAuthSchema(nonABIDataKey, /*address*/ true, Discrimination::Decl);

  opts.AsyncContextParent =
      PointerAuthSchema(dataKey, /*address*/ true, Discrimination::Constant,
                        SpecialPointerAuthDiscriminators::AsyncContextParent);

  opts.AsyncContextResume =
      PointerAuthSchema(codeKey, /*address*/ true, Discrimination::Constant,
                        SpecialPointerAuthDiscriminators::AsyncContextResume);

  opts.TaskResumeFunction =
      PointerAuthSchema(codeKey, /*address*/ true, Discrimination::Constant,
                        SpecialPointerAuthDiscriminators::TaskResumeFunction);

  opts.TaskResumeContext =
      PointerAuthSchema(dataKey, /*address*/ true, Discrimination::Constant,
                        SpecialPointerAuthDiscriminators::TaskResumeContext);

  opts.AsyncContextExtendedFrameEntry = PointerAuthSchema(
      dataKey, /*address*/ true, Discrimination::Constant,
      SpecialPointerAuthDiscriminators::SwiftAsyncContextExtendedFrameEntry);
}

std::unique_ptr<llvm::TargetMachine>
swift::createTargetMachine(const IRGenOptions &Opts, ASTContext &Ctx) {
  CodeGenOpt::Level OptLevel = Opts.shouldOptimize()
                                   ? CodeGenOpt::Default // -Os
                                   : CodeGenOpt::None;

  // Set up TargetOptions and create the target features string.
  TargetOptions TargetOpts;
  std::string CPU;
  std::string EffectiveClangTriple;
  std::vector<std::string> targetFeaturesArray;
  std::tie(TargetOpts, CPU, targetFeaturesArray, EffectiveClangTriple)
    = getIRTargetOptions(Opts, Ctx);
  const llvm::Triple &EffectiveTriple = llvm::Triple(EffectiveClangTriple);
  std::string targetFeatures;
  if (!targetFeaturesArray.empty()) {
    llvm::SubtargetFeatures features;
    for (const std::string &feature : targetFeaturesArray)
      if (!shouldRemoveTargetFeature(feature)) {
        features.AddFeature(feature);
      }
    targetFeatures = features.getString();
  }

  // Set up pointer-authentication.
  if (auto loader = Ctx.getClangModuleLoader()) {
    auto &clangInstance = loader->getClangInstance();
    if (clangInstance.getLangOpts().PointerAuthCalls) {
      // FIXME: This is gross. This needs to be done in the Frontend
      // after the module loaders are set up, and where these options are
      // formally not const.
      setPointerAuthOptions(const_cast<IRGenOptions &>(Opts).PointerAuth,
                            clangInstance.getCodeGenOpts().PointerAuth);
    }
  }

  std::string Error;
  const Target *Target =
      TargetRegistry::lookupTarget(EffectiveTriple.str(), Error);
  if (!Target) {
    Ctx.Diags.diagnose(SourceLoc(), diag::no_llvm_target, EffectiveTriple.str(),
                       Error);
    return nullptr;
  }


  // On Cygwin 64 bit, dlls are loaded above the max address for 32 bits.
  // This means that the default CodeModel causes generated code to segfault
  // when run.
  Optional<CodeModel::Model> cmodel = None;
  if (EffectiveTriple.isArch64Bit() && EffectiveTriple.isWindowsCygwinEnvironment())
    cmodel = CodeModel::Large;

  // Create a target machine.
  llvm::TargetMachine *TargetMachine = Target->createTargetMachine(
      EffectiveTriple.str(), CPU, targetFeatures, TargetOpts, Reloc::PIC_,
      cmodel, OptLevel);
  if (!TargetMachine) {
    Ctx.Diags.diagnose(SourceLoc(), diag::no_llvm_target,
                       EffectiveTriple.str(), "no LLVM target machine");
    return nullptr;
  }
  return std::unique_ptr<llvm::TargetMachine>(TargetMachine);
}

IRGenerator::IRGenerator(const IRGenOptions &options, SILModule &module)
  : Opts(options), SIL(module), QueueIndex(0) {
}

std::unique_ptr<llvm::TargetMachine> IRGenerator::createTargetMachine() {
  return ::createTargetMachine(Opts, SIL.getASTContext());
}

// With -embed-bitcode, save a copy of the llvm IR as data in the
// __LLVM,__bitcode section and save the command-line options in the
// __LLVM,__swift_cmdline section.
static void embedBitcode(llvm::Module *M, const IRGenOptions &Opts)
{
  if (Opts.EmbedMode == IRGenEmbedMode::None)
    return;

  // Save llvm.compiler.used and remove it.
  SmallVector<llvm::Constant*, 2> UsedArray;
  SmallSet<llvm::GlobalValue*, 4> UsedGlobals;
  auto *UsedElementType =
    llvm::Type::getInt8Ty(M->getContext())->getPointerTo(0);
  llvm::GlobalVariable *Used =
    collectUsedGlobalVariables(*M, UsedGlobals, true);
  for (auto *GV : UsedGlobals) {
    if (GV->getName() != "llvm.embedded.module" &&
        GV->getName() != "llvm.cmdline")
      UsedArray.push_back(
          ConstantExpr::getPointerBitCastOrAddrSpaceCast(GV, UsedElementType));
  }
  if (Used)
    Used->eraseFromParent();

  // Embed the bitcode for the llvm module.
  std::string Data;
  llvm::raw_string_ostream OS(Data);
  if (Opts.EmbedMode == IRGenEmbedMode::EmbedBitcode)
    llvm::WriteBitcodeToFile(*M, OS);

  ArrayRef<uint8_t> ModuleData(
      reinterpret_cast<const uint8_t *>(OS.str().data()), OS.str().size());
  llvm::Constant *ModuleConstant =
    llvm::ConstantDataArray::get(M->getContext(), ModuleData);
  llvm::GlobalVariable *GV = new llvm::GlobalVariable(*M,
                                       ModuleConstant->getType(), true,
                                       llvm::GlobalValue::PrivateLinkage,
                                       ModuleConstant);
  UsedArray.push_back(
    llvm::ConstantExpr::getPointerBitCastOrAddrSpaceCast(GV, UsedElementType));
  GV->setSection("__LLVM,__bitcode");
  if (llvm::GlobalVariable *Old =
      M->getGlobalVariable("llvm.embedded.module", true)) {
    GV->takeName(Old);
    Old->replaceAllUsesWith(GV);
    delete Old;
  } else {
    GV->setName("llvm.embedded.module");
  }

  // Embed command-line options.
  ArrayRef<uint8_t>
      CmdData(reinterpret_cast<const uint8_t *>(Opts.CmdArgs.data()),
              Opts.CmdArgs.size());
  llvm::Constant *CmdConstant =
    llvm::ConstantDataArray::get(M->getContext(), CmdData);
  GV = new llvm::GlobalVariable(*M, CmdConstant->getType(), true,
                                llvm::GlobalValue::PrivateLinkage,
                                CmdConstant);
  GV->setSection("__LLVM,__swift_cmdline");
  UsedArray.push_back(
    llvm::ConstantExpr::getPointerBitCastOrAddrSpaceCast(GV, UsedElementType));
  if (llvm::GlobalVariable *Old = M->getGlobalVariable("llvm.cmdline", true)) {
    GV->takeName(Old);
    Old->replaceAllUsesWith(GV);
    delete Old;
  } else {
    GV->setName("llvm.cmdline");
  }

  if (UsedArray.empty())
    return;

  // Recreate llvm.compiler.used.
  auto *ATy = llvm::ArrayType::get(UsedElementType, UsedArray.size());
  auto *NewUsed = new GlobalVariable(
           *M, ATy, false, llvm::GlobalValue::AppendingLinkage,
           llvm::ConstantArray::get(ATy, UsedArray), "llvm.compiler.used");
  NewUsed->setSection("llvm.metadata");
}

static void initLLVMModule(const IRGenModule &IGM, SILModule &SIL) {
  auto *Module = IGM.getModule();
  assert(Module && "Expected llvm:Module for IR generation!");
  
  Module->setTargetTriple(IGM.Triple.str());

  if (IGM.Context.LangOpts.SDKVersion) {
    if (Module->getSDKVersion().empty())
      Module->setSDKVersion(*IGM.Context.LangOpts.SDKVersion);
    else
      assert(Module->getSDKVersion() == *IGM.Context.LangOpts.SDKVersion);
  }

  // Set the module's string representation.
  Module->setDataLayout(IGM.DataLayout.getStringRepresentation());

  auto *MDNode = IGM.getModule()->getOrInsertNamedMetadata("swift.module.flags");
  auto &Context = IGM.getModule()->getContext();
  auto *Value = SIL.getSwiftModule()->isStdlibModule()
              ? llvm::ConstantInt::getTrue(Context)
              : llvm::ConstantInt::getFalse(Context);
  MDNode->addOperand(llvm::MDTuple::get(Context,
                                        {llvm::MDString::get(Context,
                                                             "standard-library"),
                                         llvm::ConstantAsMetadata::get(Value)}));

  if (auto *streamer = SIL.getSILRemarkStreamer()) {
    streamer->intoLLVMContext(Module->getContext());
  }
}

std::pair<IRGenerator *, IRGenModule *>
swift::irgen::createIRGenModule(SILModule *SILMod, StringRef OutputFilename,
                                StringRef MainInputFilenameForDebugInfo,
                                StringRef PrivateDiscriminator) {

  IRGenOptions Opts;
  IRGenerator *irgen = new IRGenerator(Opts, *SILMod);
  auto targetMachine = irgen->createTargetMachine();
  if (!targetMachine)
    return std::make_pair(nullptr, nullptr);

  // Create the IR emitter.
  IRGenModule *IGM = new IRGenModule(
      *irgen, std::move(targetMachine), nullptr, "", OutputFilename,
      MainInputFilenameForDebugInfo, PrivateDiscriminator);

  initLLVMModule(*IGM, *SILMod);

  return std::pair<IRGenerator *, IRGenModule *>(irgen, IGM);
}

void swift::irgen::deleteIRGenModule(
    std::pair<IRGenerator *, IRGenModule *> &IRGenPair) {
  delete IRGenPair.second;
  delete IRGenPair.first;
}

/// Run the IRGen preparation SIL pipeline. Passes have access to the
/// IRGenModule.
static void runIRGenPreparePasses(SILModule &Module,
                                  irgen::IRGenModule &IRModule) {
  auto &opts = Module.getOptions();
  auto plan = SILPassPipelinePlan::getIRGenPreparePassPipeline(opts);
  executePassPipelinePlan(&Module, plan, /*isMandatory*/ true, &IRModule);
}

namespace {
using IREntitiesToEmit = SmallVector<LinkEntity, 1>;

struct SymbolSourcesToEmit {
  SILRefsToEmit silRefsToEmit;
  IREntitiesToEmit irEntitiesToEmit;
};

static Optional<SymbolSourcesToEmit>
getSymbolSourcesToEmit(const IRGenDescriptor &desc) {
  if (!desc.SymbolsToEmit)
    return None;

  assert(!desc.SILMod && "Already emitted SIL?");

  // First retrieve the symbol source map to figure out what we need to build,
  // making sure to include non-public symbols.
  auto &ctx = desc.getParentModule()->getASTContext();
  auto tbdDesc = desc.getTBDGenDescriptor();
  tbdDesc.getOptions().PublicSymbolsOnly = false;
  auto symbolMap =
      llvm::cantFail(ctx.evaluator(SymbolSourceMapRequest{std::move(tbdDesc)}));

  // Then split up the symbols so they can be emitted by the appropriate part
  // of the pipeline.
  SILRefsToEmit silRefsToEmit;
  IREntitiesToEmit irEntitiesToEmit;
  for (const auto &symbol : *desc.SymbolsToEmit) {
    auto source = symbolMap.find(symbol);
    assert(source && "Couldn't find symbol");
    switch (source->kind) {
    case SymbolSource::Kind::SIL:
      silRefsToEmit.push_back(source->getSILDeclRef());
      break;
    case SymbolSource::Kind::IR:
      irEntitiesToEmit.push_back(source->getIRLinkEntity());
      break;
    case SymbolSource::Kind::LinkerDirective:
    case SymbolSource::Kind::Unknown:
      llvm_unreachable("Not supported");
    }
  }
  return SymbolSourcesToEmit{silRefsToEmit, irEntitiesToEmit};
}
} // end of anonymous namespace

/// Generates LLVM IR, runs the LLVM passes and produces the output file.
/// All this is done in a single thread.
GeneratedModule IRGenRequest::evaluate(Evaluator &evaluator,
                                       IRGenDescriptor desc) const {
  const auto &Opts = desc.Opts;
  const auto &PSPs = desc.PSPs;
  auto *M = desc.getParentModule();
  auto &Ctx = M->getASTContext();
  assert(!Ctx.hadError());

  auto symsToEmit = getSymbolSourcesToEmit(desc);
  assert(!symsToEmit || symsToEmit->irEntitiesToEmit.empty() &&
         "IR symbol emission not implemented yet");

  // If we've been provided a SILModule, use it. Otherwise request the lowered
  // SIL for the file or module.
  auto SILMod = std::unique_ptr<SILModule>(desc.SILMod);
  if (!SILMod) {
    auto loweringDesc = ASTLoweringDescriptor{
        desc.Ctx, desc.Conv, desc.SILOpts,
        symsToEmit.map([](const auto &x) { return x.silRefsToEmit; })};
    SILMod = llvm::cantFail(Ctx.evaluator(LoweredSILRequest{loweringDesc}));

    // If there was an error, bail.
    if (Ctx.hadError())
      return GeneratedModule::null();
  }

  auto filesToEmit = desc.getFilesToEmit();
  auto *primaryFile =
      dyn_cast_or_null<SourceFile>(desc.Ctx.dyn_cast<FileUnit *>());

  IRGenerator irgen(Opts, *SILMod);

  auto targetMachine = irgen.createTargetMachine();
  if (!targetMachine) return GeneratedModule::null();

  // Create the IR emitter.
  IRGenModule IGM(irgen, std::move(targetMachine), primaryFile, desc.ModuleName,
                  PSPs.OutputFilename, PSPs.MainInputFilenameForDebugInfo,
                  desc.PrivateDiscriminator);

  initLLVMModule(IGM, *SILMod);

  // Run SIL level IRGen preparation passes.
  runIRGenPreparePasses(*SILMod, IGM);

  {
    FrontendStatsTracer tracer(Ctx.Stats, "IRGen");

    // Emit the module contents.
    irgen.emitGlobalTopLevel(desc.getLinkerDirectives());

    for (auto *file : filesToEmit) {
      if (auto *nextSF = dyn_cast<SourceFile>(file)) {
        IGM.emitSourceFile(*nextSF);
      } else if (auto *nextSFU = dyn_cast<SynthesizedFileUnit>(file)) {
        IGM.emitSynthesizedFileUnit(*nextSFU);
      } else {
        file->collectLinkLibraries([&IGM](LinkLibrary LinkLib) {
          IGM.addLinkLibrary(LinkLib);
        });
      }
    }

    // Okay, emit any definitions that we suddenly need.
    irgen.emitLazyDefinitions();

    // Register our info with the runtime if needed.
    if (Opts.UseJIT) {
      IGM.emitBuiltinReflectionMetadata();
      IGM.emitRuntimeRegistration();
    } else {
      // Emit protocol conformances into a section we can recognize at runtime.
      // In JIT mode these are manually registered above.
      IGM.emitSwiftProtocols();
      IGM.emitProtocolConformances();
      IGM.emitTypeMetadataRecords();
      IGM.emitBuiltinReflectionMetadata();
      IGM.emitReflectionMetadataVersion();
      irgen.emitEagerClassInitialization();
      irgen.emitDynamicReplacements();
    }

    // Emit symbols for eliminated dead methods.
    IGM.emitVTableStubs();

    // Verify type layout if we were asked to.
    if (!Opts.VerifyTypeLayoutNames.empty())
      IGM.emitTypeVerifier();

    std::for_each(Opts.LinkLibraries.begin(), Opts.LinkLibraries.end(),
                  [&](LinkLibrary linkLib) {
      IGM.addLinkLibrary(linkLib);
    });

    if (!IGM.finalize())
      return GeneratedModule::null();

    setModuleFlags(IGM);
  }

  // Bail out if there are any errors.
  if (Ctx.hadError()) return GeneratedModule::null();

  // Free the memory occupied by the SILModule.
  // Execute this task in parallel to the embedding of bitcode.
  auto SILModuleRelease = [&SILMod]() {
    SILMod.reset(nullptr);
    SILModule::checkForLeaksAfterDestruction();
  };
  auto Thread = std::thread(SILModuleRelease);
  // Wait for the thread to terminate.
  SWIFT_DEFER { Thread.join(); };

  embedBitcode(IGM.getModule(), Opts);

  // TODO: Turn the module hash into an actual output.
  if (auto **outModuleHash = desc.outModuleHash) {
    *outModuleHash = IGM.ModuleHash;
  }
  return std::move(IGM).intoGeneratedModule();
}

namespace {
struct LLVMCodeGenThreads {

  struct Thread {
    LLVMCodeGenThreads &parent;
    unsigned threadIndex;
#ifdef __APPLE__
    pthread_t threadId;
#else
    std::thread thread;
#endif

    Thread(LLVMCodeGenThreads &parent, unsigned threadIndex)
        : parent(parent), threadIndex(threadIndex)
    {}

    /// Run llvm codegen.
    void run() {
      auto *diagMutex = parent.diagMutex;
      while (IRGenModule *IGM = parent.irgen->fetchFromQueue()) {
        LLVM_DEBUG(diagMutex->lock();
                   dbgs() << "thread " << threadIndex << ": fetched "
                          << IGM->OutputFilename << "\n";
                   diagMutex->unlock(););
        embedBitcode(IGM->getModule(), parent.irgen->Opts);
        performLLVM(parent.irgen->Opts, IGM->Context.Diags, diagMutex,
                    IGM->ModuleHash, IGM->getModule(), IGM->TargetMachine.get(),
                    IGM->OutputFilename, IGM->Context.Stats);
        if (IGM->Context.Diags.hadAnyError())
          return;
      }
      LLVM_DEBUG(diagMutex->lock();
                 dbgs() << "thread " << threadIndex << ": done\n";
                 diagMutex->unlock(););
      return;
    }
  };

  IRGenerator *irgen;
  llvm::sys::Mutex *diagMutex;
  std::vector<Thread> threads;

  LLVMCodeGenThreads(IRGenerator *irgen, llvm::sys::Mutex *diagMutex,
                     unsigned numThreads)
      : irgen(irgen), diagMutex(diagMutex) {
    threads.reserve(numThreads);
    for (unsigned idx = 0; idx < numThreads; ++idx) {
      // the 0-th thread is executed by the main thread.
      threads.push_back(Thread(*this, idx + 1));
    }
  }

  static void *runThread(void *arg) {
    auto *thread = reinterpret_cast<Thread *>(arg);
    thread->run();
    return nullptr;
  }

  void startThreads() {
#ifdef __APPLE__
    // Increase the thread stack size on macosx to 8MB (default is 512KB). This
    // matches the main thread.
    pthread_attr_t stackSizeAttribute;
    int err = pthread_attr_init(&stackSizeAttribute);
    assert(!err);
    err = pthread_attr_setstacksize(&stackSizeAttribute, 8 * 1024 * 1024);
    assert(!err);

    for (auto &thread : threads) {
      pthread_create(&thread.threadId, &stackSizeAttribute,
                     LLVMCodeGenThreads::runThread, &thread);
    }

    pthread_attr_destroy(&stackSizeAttribute);
#else
    for (auto &thread : threads) {
      thread.thread = std::thread(runThread, &thread);
    }
#endif

  }

  void runMainThread() {
    Thread mainThread(*this, 0);
    mainThread.run();
  }

  void join() {
#ifdef __APPLE__
    for (auto &thread : threads)
      pthread_join(thread.threadId, 0);
#else
    for (auto &thread: threads) {
      thread.thread.join();
    }
#endif
  }
};
}

/// Generates LLVM IR, runs the LLVM passes and produces the output files.
/// All this is done in multiple threads.
static void performParallelIRGeneration(IRGenDescriptor desc) {
  const auto &Opts = desc.Opts;
  auto outputFilenames = desc.parallelOutputFilenames;
  auto SILMod = std::unique_ptr<SILModule>(desc.SILMod);
  auto *M = desc.getParentModule();

  IRGenerator irgen(Opts, *SILMod);

  // Enter a cleanup to delete all the IGMs and their associated LLVMContexts
  // that have been associated with the IRGenerator.
  struct IGMDeleter {
    IRGenerator &IRGen;
    IGMDeleter(IRGenerator &irgen) : IRGen(irgen) {}
    ~IGMDeleter() {
      for (auto it = IRGen.begin(); it != IRGen.end(); ++it) {
        IRGenModule *IGM = it->second;
        delete IGM;
      }
    }
  } _igmDeleter(irgen);

  auto OutputIter = outputFilenames.begin();
  bool IGMcreated = false;

  auto &Ctx = M->getASTContext();
  // Create an IRGenModule for each source file.
  bool DidRunSILCodeGenPreparePasses = false;
  for (auto *File : M->getFiles()) {
    auto nextSF = dyn_cast<SourceFile>(File);
    if (!nextSF)
      continue;
    
    // There must be an output filename for each source file.
    // We ignore additional output filenames.
    if (OutputIter == outputFilenames.end()) {
      Ctx.Diags.diagnose(SourceLoc(), diag::too_few_output_filenames);
      return;
    }

    auto targetMachine = irgen.createTargetMachine();
    if (!targetMachine) continue;

    // Create the IR emitter.
    IRGenModule *IGM =
        new IRGenModule(irgen, std::move(targetMachine), nextSF,
                        desc.ModuleName, *OutputIter++, nextSF->getFilename(),
                        nextSF->getPrivateDiscriminator().str());
    IGMcreated = true;

    initLLVMModule(*IGM, *SILMod);
    if (!DidRunSILCodeGenPreparePasses) {
      // Run SIL level IRGen preparation passes on the module the first time
      // around.
      runIRGenPreparePasses(*SILMod, *IGM);
      DidRunSILCodeGenPreparePasses = true;
    }
  }
  
  if (!IGMcreated) {
    // TODO: Check this already at argument parsing.
    Ctx.Diags.diagnose(SourceLoc(), diag::no_input_files_for_mt);
    return;
  }

  // Emit the module contents.
  irgen.emitGlobalTopLevel(desc.getLinkerDirectives());

  for (auto *File : M->getFiles()) {
    if (auto *SF = dyn_cast<SourceFile>(File)) {
      CurrentIGMPtr IGM = irgen.getGenModule(SF);
      IGM->emitSourceFile(*SF);
    } else if (auto *nextSFU = dyn_cast<SynthesizedFileUnit>(File)) {
      CurrentIGMPtr IGM = irgen.getGenModule(nextSFU);
      IGM->emitSynthesizedFileUnit(*nextSFU);
    } else {
      File->collectLinkLibraries([&](LinkLibrary LinkLib) {
        irgen.getPrimaryIGM()->addLinkLibrary(LinkLib);
      });
    }
  }
  
  // Okay, emit any definitions that we suddenly need.
  irgen.emitLazyDefinitions();

  irgen.emitSwiftProtocols();

  irgen.emitDynamicReplacements();

  irgen.emitProtocolConformances();

  irgen.emitTypeMetadataRecords();

  irgen.emitReflectionMetadataVersion();

  irgen.emitEagerClassInitialization();

  // Emit reflection metadata for builtin and imported types.
  irgen.emitBuiltinReflectionMetadata();

  IRGenModule *PrimaryGM = irgen.getPrimaryIGM();

  // Emit symbols for eliminated dead methods.
  PrimaryGM->emitVTableStubs();
    
  // Verify type layout if we were asked to.
  if (!Opts.VerifyTypeLayoutNames.empty())
    PrimaryGM->emitTypeVerifier();
  
  std::for_each(Opts.LinkLibraries.begin(), Opts.LinkLibraries.end(),
                [&](LinkLibrary linkLib) {
                  PrimaryGM->addLinkLibrary(linkLib);
                });
  
  llvm::DenseSet<StringRef> referencedGlobals;

  for (auto it = irgen.begin(); it != irgen.end(); ++it) {
    IRGenModule *IGM = it->second;
    llvm::Module *M = IGM->getModule();
    auto collectReference = [&](llvm::GlobalValue &G) {
      if (G.isDeclaration()
          && (G.getLinkage() == GlobalValue::LinkOnceODRLinkage ||
              G.getLinkage() == GlobalValue::ExternalLinkage)) {
        referencedGlobals.insert(G.getName());
        G.setLinkage(GlobalValue::ExternalLinkage);
      }
    };
    for (llvm::GlobalVariable &G : M->getGlobalList()) {
      collectReference(G);
    }
    for (llvm::Function &F : M->getFunctionList()) {
      collectReference(F);
    }
    for (llvm::GlobalAlias &A : M->getAliasList()) {
      collectReference(A);
    }
  }

  for (auto it = irgen.begin(); it != irgen.end(); ++it) {
    IRGenModule *IGM = it->second;
    llvm::Module *M = IGM->getModule();
    
    // Update the linkage of shared functions/globals.
    // If a shared function/global is referenced from another file it must have
    // weak instead of linkonce linkage. Otherwise LLVM would remove the
    // definition (if it's not referenced in the same file).
    auto updateLinkage = [&](llvm::GlobalValue &G) {
      if (!G.isDeclaration()
          && G.getLinkage() == GlobalValue::LinkOnceODRLinkage
          && referencedGlobals.count(G.getName()) != 0) {
        G.setLinkage(GlobalValue::WeakODRLinkage);
      }
    };
    for (llvm::GlobalVariable &G : M->getGlobalList()) {
      updateLinkage(G);
    }
    for (llvm::Function &F : M->getFunctionList()) {
      updateLinkage(F);
    }
    for (llvm::GlobalAlias &A : M->getAliasList()) {
      updateLinkage(A);
    }

    if (!IGM->finalize())
      return;

    setModuleFlags(*IGM);
  }

  // Bail out if there are any errors.
  if (Ctx.hadError()) return;

  FrontendStatsTracer tracer(Ctx.Stats, "LLVM pipeline");

  llvm::sys::Mutex DiagMutex;

  // Start all the threads and do the LLVM compilation.

  LLVMCodeGenThreads codeGenThreads(&irgen, &DiagMutex, Opts.NumThreads - 1);
  codeGenThreads.startThreads();

  // Free the memory occupied by the SILModule.
  // Execute this task in parallel to the LLVM compilation.
  auto SILModuleRelease = [&SILMod]() {
    SILMod.reset(nullptr);
    SILModule::checkForLeaksAfterDestruction();
  };
  auto releaseModuleThread = std::thread(SILModuleRelease);

  codeGenThreads.runMainThread();

  // Wait for all threads.
  releaseModuleThread.join();
  codeGenThreads.join();
}

GeneratedModule swift::performIRGeneration(
    swift::ModuleDecl *M, const IRGenOptions &Opts,
    const TBDGenOptions &TBDOpts, std::unique_ptr<SILModule> SILMod,
    StringRef ModuleName, const PrimarySpecificPaths &PSPs,
    ArrayRef<std::string> parallelOutputFilenames,
    llvm::GlobalVariable **outModuleHash) {
  // Get a pointer to the SILModule to avoid a potential use-after-move.
  const auto *SILModPtr = SILMod.get();
  const auto &SILOpts = SILModPtr->getOptions();
  auto desc = IRGenDescriptor::forWholeModule(
      M, Opts, TBDOpts, SILOpts, SILModPtr->Types, std::move(SILMod),
      ModuleName, PSPs, /*symsToEmit*/ None, parallelOutputFilenames,
      outModuleHash);

  if (Opts.shouldPerformIRGenerationInParallel() &&
      !parallelOutputFilenames.empty()) {
    ::performParallelIRGeneration(desc);
    // TODO: Parallel LLVM compilation cannot be used if a (single) module is
    // needed as return value.
    return GeneratedModule::null();
  }
  return llvm::cantFail(M->getASTContext().evaluator(IRGenRequest{desc}));
}

GeneratedModule swift::
performIRGeneration(FileUnit *file, const IRGenOptions &Opts,
                    const TBDGenOptions &TBDOpts,
                    std::unique_ptr<SILModule> SILMod,
                    StringRef ModuleName, const PrimarySpecificPaths &PSPs,
                    StringRef PrivateDiscriminator,
                    llvm::GlobalVariable **outModuleHash) {
  // Get a pointer to the SILModule to avoid a potential use-after-move.
  const auto *SILModPtr = SILMod.get();
  const auto &SILOpts = SILModPtr->getOptions();
  auto desc = IRGenDescriptor::forFile(
      file, Opts, TBDOpts, SILOpts, SILModPtr->Types, std::move(SILMod),
      ModuleName, PSPs, PrivateDiscriminator, /*symsToEmit*/ None,
      outModuleHash);
  return llvm::cantFail(file->getASTContext().evaluator(IRGenRequest{desc}));
}

void
swift::createSwiftModuleObjectFile(SILModule &SILMod, StringRef Buffer,
                                   StringRef OutputPath) {
  auto &Ctx = SILMod.getASTContext();
  assert(!Ctx.hadError());

  IRGenOptions Opts;
  // This tool doesn't pass  the necessary runtime library path to
  // TypeConverter, because this feature isn't needed.
  Opts.DisableLegacyTypeInfo = true;

  Opts.OutputKind = IRGenOutputKind::ObjectFile;
  IRGenerator irgen(Opts, SILMod);

  auto targetMachine = irgen.createTargetMachine();
  if (!targetMachine) return;

  IRGenModule IGM(irgen, std::move(targetMachine), nullptr,
                  OutputPath, OutputPath, "", "");
  initLLVMModule(IGM, SILMod);
  auto *Ty = llvm::ArrayType::get(IGM.Int8Ty, Buffer.size());
  auto *Data =
      llvm::ConstantDataArray::getString(IGM.getLLVMContext(),
                                         Buffer, /*AddNull=*/false);
  auto &M = *IGM.getModule();
  auto *ASTSym = new llvm::GlobalVariable(M, Ty, /*constant*/ true,
                                          llvm::GlobalVariable::InternalLinkage,
                                          Data, "__Swift_AST");
  std::string Section;
  switch (IGM.TargetInfo.OutputObjectFormat) {
  case llvm::Triple::GOFF:
  case llvm::Triple::UnknownObjectFormat:
    llvm_unreachable("unknown object format");
  case llvm::Triple::XCOFF:
  case llvm::Triple::COFF:
    Section = COFFASTSectionName;
    break;
  case llvm::Triple::ELF:
    Section = ELFASTSectionName;
    break;
  case llvm::Triple::MachO:
    Section = std::string(MachOASTSegmentName) + "," + MachOASTSectionName;
    break;
  case llvm::Triple::Wasm:
    Section = WasmASTSectionName;
    break;
  }
  ASTSym->setSection(Section);
  ASTSym->setAlignment(llvm::MaybeAlign(serialization::SWIFTMODULE_ALIGNMENT));
  ::performLLVM(Opts, Ctx.Diags, nullptr, nullptr, IGM.getModule(),
                IGM.TargetMachine.get(),
                OutputPath, Ctx.Stats);
}

bool swift::performLLVM(const IRGenOptions &Opts, ASTContext &Ctx,
                        llvm::Module *Module, StringRef OutputFilename) {
  // Build TargetMachine.
  auto TargetMachine = createTargetMachine(Opts, Ctx);
  if (!TargetMachine)
    return true;

  auto *Clang = static_cast<ClangImporter *>(Ctx.getClangModuleLoader());
  // Use clang's datalayout.
  Module->setDataLayout(Clang->getTargetInfo().getDataLayout());

  embedBitcode(Module, Opts);
  if (::performLLVM(Opts, Ctx.Diags, nullptr, nullptr, Module,
                    TargetMachine.get(),
                    OutputFilename, Ctx.Stats))
    return true;
  return false;
}

GeneratedModule OptimizedIRRequest::evaluate(Evaluator &evaluator,
                                             IRGenDescriptor desc) const {
  auto *parentMod = desc.getParentModule();
  auto &ctx = parentMod->getASTContext();

  // Resolve imports for all the source files.
  for (auto *file : parentMod->getFiles()) {
    if (auto *SF = dyn_cast<SourceFile>(file))
      performImportResolution(*SF);
  }

  bindExtensions(*parentMod);

  if (ctx.hadError())
    return GeneratedModule::null();

  auto irMod = llvm::cantFail(evaluator(IRGenRequest{desc}));
  if (!irMod)
    return irMod;

  performLLVMOptimizations(desc.Opts, irMod.getModule(),
                           irMod.getTargetMachine());
  return irMod;
}

StringRef SymbolObjectCodeRequest::evaluate(Evaluator &evaluator,
                                            IRGenDescriptor desc) const {
  auto &ctx = desc.getParentModule()->getASTContext();
  auto mod = cantFail(evaluator(OptimizedIRRequest{desc}));
  auto *targetMachine = mod.getTargetMachine();

  // Add the passes to emit the LLVM module as object code.
  // TODO: Use compileAndWriteLLVM.
  legacy::PassManager emitPasses;
  emitPasses.add(createTargetTransformInfoWrapperPass(
      targetMachine->getTargetIRAnalysis()));

  SmallString<0> output;
  raw_svector_ostream os(output);
  targetMachine->addPassesToEmitFile(emitPasses, os, nullptr, CGFT_ObjectFile);
  emitPasses.run(*mod.getModule());
  os << '\0';
  return ctx.AllocateCopy(output.str());
}
