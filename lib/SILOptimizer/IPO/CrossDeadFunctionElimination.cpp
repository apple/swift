#define DEBUG_TYPE "sil-cross-dead-function-elimination"
#include "swift/AST/ASTMangler.h"
#include "swift/AST/DiagnosticsFrontend.h"
#include "swift/AST/ProtocolConformance.h"
#include "swift/SIL/InstructionUtils.h"
#include "swift/SIL/PatternMatch.h"
#include "swift/SIL/SILBuilder.h"
#include "swift/SIL/SILVisitor.h"
#include "swift/SILOptimizer/PassManager/Passes.h"
#include "swift/SILOptimizer/PassManager/Transforms.h"
#include "swift/Serialization/ModuleSummary.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Debug.h"
using namespace swift;

STATISTIC(NumDeadFunc, "Number of dead functions eliminated");

//===----------------------------------------------------------------------===//
//                      Pass Definition and Entry Points
//===----------------------------------------------------------------------===//

namespace {

using namespace modulesummary;

class SILCrossDeadFuncElimination : public SILModuleTransform {
private:
  ModuleSummaryIndex TheSummary;

public:
  SILCrossDeadFuncElimination() {}

  void eliminateDeadTables(SILModule &M) {
    auto &WitnessTables = M.getWitnessTableList();
    std::set<GUID> UsedTypes;
    for (auto type : TheSummary.getUsedTypeList()) {
      UsedTypes.insert(type);
    }
    Mangle::ASTMangler mangler;
    for (auto WI = WitnessTables.begin(), EI = WitnessTables.end(); WI != EI;) {
      SILWitnessTable *WT = &*WI;
      ++WI;
      CanType type = WI->getConformingType();
      std::string mangled = mangler.mangleTypeWithoutPrefix(type);
      GUID guid = getGUIDFromUniqueName(mangled);
      if (UsedTypes.find(guid) != UsedTypes.end()) {
        continue;
      }
      WT->clearMethods_if([&] (const SILWitnessTable::MethodWitness &MW) -> bool {
        return true;
      });
    }
  }

  void eliminateDeadEntriesFromTables(SILModule &M) {
    
    for (auto VT : M.getVTables()) {
      VT->removeEntries_if([&] (SILVTable::Entry &entry) -> bool {
        auto Impl = entry.getImplementation();
        GUID guid = getGUIDFromUniqueName(Impl->getName());
        auto maybeSummary = TheSummary.getFunctionSummary(guid);
        if (!maybeSummary)
          return false;
        return !maybeSummary->isLive();
      });
    }
  
    auto &WitnessTables = M.getWitnessTableList();
    for (auto WI = WitnessTables.begin(), EI = WitnessTables.end(); WI != EI;) {
      SILWitnessTable *WT = &*WI;
      ++WI;
      WT->clearMethods_if([&] (const SILWitnessTable::MethodWitness &MW) -> bool {
        auto Impl = MW.Witness;
        GUID guid = getGUIDFromUniqueName(Impl->getName());
        auto maybeSummary = TheSummary.getFunctionSummary(guid);
        if (!maybeSummary)
          return false;
        return !maybeSummary->isLive();
      });
    }

    auto DefaultWitnessTables = M.getDefaultWitnessTables();
    for (auto WI = DefaultWitnessTables.begin(),
              EI = DefaultWitnessTables.end();
         WI != EI;) {
      SILDefaultWitnessTable *WT = &*WI;
      ++WI;
      WT->clearMethods_if([&](SILFunction *MW) -> bool {
        if (!MW)
          return false;
        GUID guid = getGUIDFromUniqueName(MW->getName());
        auto maybeSummary = TheSummary.getFunctionSummary(guid);
        if (!maybeSummary)
          return false;
        return !maybeSummary->isLive();
      });
    }
  }
  
  void eliminateDeadFunctions(SILModule &M, std::vector<SILFunction *> &DeadFunctions) {
    for (SILFunction &F : M) {
      auto guid = getGUIDFromUniqueName(F.getName());
      auto summary = TheSummary.getFunctionSummary(guid);
      if (summary->isLive()) {
        continue;
      }
      F.dropAllReferences();
      DeadFunctions.push_back(&F);
      LLVM_DEBUG(llvm::dbgs() << "Eliminate " << F.getName() << "\n");
    }
  }

  void cleanupSILProperty(SILModule &M,
                          const std::vector<SILFunction *> &DeadFunctions) {
    std::set<SILFunction *> DeadFunctionsSet(DeadFunctions.begin(),
                                             DeadFunctions.end());
    for (SILProperty &P : M.getPropertyList()) {
      P.clearReferencedFunctions_if([&](SILFunction *f) {
        return DeadFunctionsSet.find(f) != DeadFunctionsSet.end();
      });
    }
  }

  void run() override {
    LLVM_DEBUG(llvm::dbgs() << "Running CrossDeadFuncElimination\n");
    auto &Context = getModule()->getASTContext();
    auto ModuleSummaryPath = getOptions().ModuleSummaryPath;
    auto ErrOrBuf = llvm::MemoryBuffer::getFile(ModuleSummaryPath);
    if (!ErrOrBuf) {
      Context.Diags.diagnose(SourceLoc(), diag::error_no_such_file_or_directory,
                             ModuleSummaryPath);
      return;
    }

    auto HasErr = modulesummary::loadModuleSummaryIndex(
        ErrOrBuf.get()->getMemBufferRef(), TheSummary);
    if (HasErr) {
      llvm::report_fatal_error("Invalid module summary");
    }

    auto &M = *getModule();
    this->eliminateDeadEntriesFromTables(M);
    std::vector<SILFunction *> DeadFunctions;
    this->eliminateDeadFunctions(M, DeadFunctions);
    this->cleanupSILProperty(M, DeadFunctions);

    while (!DeadFunctions.empty()) {
      SILFunction *F = DeadFunctions.back();
      DeadFunctions.pop_back();
      notifyWillDeleteFunction(F);
      M.eraseFunction(F);
    }
    this->invalidateFunctionTables();
  }
};

} // end anonymous namespace

SILTransform *swift::createCrossDeadFunctionElimination() {
  return new SILCrossDeadFuncElimination();
}
