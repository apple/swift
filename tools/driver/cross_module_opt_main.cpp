#define DEBUG_TYPE "lto-cross-module-opt"

#include "swift/AST/DiagnosticsFrontend.h"
#include "swift/Basic/LLVMInitialize.h"
#include "swift/Frontend/Frontend.h"
#include "swift/Frontend/PrintingDiagnosticConsumer.h"
#include "swift/Option/Options.h"
#include "swift/SIL/SILModule.h"
#include "swift/SIL/TypeLowering.h"
#include "swift/Serialization/ModuleSummary.h"
#include "swift/Serialization/Validation.h"
#include "swift/Subsystems.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/Bitstream/BitstreamReader.h"
#include "llvm/Option/ArgList.h"
#include "llvm/Option/Option.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/TargetSelect.h"

using namespace llvm::opt;
using namespace swift;
using namespace modulesummary;

class MergeModuleSummaryInvocation {
public:
  std::string OutputFilename;
  std::vector<std::string> InputFilenames;

  bool parseArgs(ArrayRef<const char *> Args, DiagnosticEngine &Diags) {
    using namespace options;

    std::unique_ptr<llvm::opt::OptTable> Table = createSwiftOptTable();
    unsigned MissingIndex;
    unsigned MissingCount;
    llvm::opt::InputArgList ParsedArgs =
      Table->ParseArgs(Args, MissingIndex, MissingCount, SwiftMergeModuleSummaryOption);

    if (MissingCount) {
      Diags.diagnose(SourceLoc(), diag::error_missing_arg_value,
                     ParsedArgs.getArgString(MissingIndex), MissingCount);
      return true;
    }

    for (const Arg *A : ParsedArgs.filtered(OPT_INPUT)) {
      InputFilenames.push_back(A->getValue());
    }

    if (const Arg *A = ParsedArgs.getLastArg(OPT_o)) {
      OutputFilename = A->getValue();
    }

    std::vector<const char *> LLVMArgs {""};
    for (const Arg *A : ParsedArgs.filtered(OPT_Xllvm)) {
      LLVMArgs.push_back(A->getValue());
    }

    llvm::cl::ParseCommandLineOptions(LLVMArgs.size(), LLVMArgs.data(), "");
    return false;
  }
};

static llvm::cl::opt<std::string>
    LTOPrintLiveTrace("lto-print-live-trace", llvm::cl::init(""),
                      llvm::cl::desc("Print liveness trace for the symbol"));

static llvm::DenseSet<GUID> computePreservedGUIDs(ModuleSummaryIndex *summary) {
  llvm::DenseSet<GUID> Set(1);
  for (auto FI = summary->functions_begin(), FE = summary->functions_end();
       FI != FE; ++FI) {
    auto summary = FI->second.get();
    if (summary->isPreserved()) {
      Set.insert(FI->first);
    }
  }
  return Set;
}

class LivenessTrace {
public:
  enum ReasonTy { Preserved, StaticReferenced, IndirectReferenced };
  std::shared_ptr<LivenessTrace> markedBy;
  std::string symbol;
  GUID guid;
  ReasonTy reason;

  LivenessTrace(std::shared_ptr<LivenessTrace> markedBy, GUID guid,
                ReasonTy reason)
      : markedBy(markedBy), guid(guid), reason(reason) {}

  void setName(std::string name) { this->symbol = name; }

  void dump() { dump(llvm::errs()); }
  void dump(llvm::raw_ostream &os) {
    if (!symbol.empty()) {
      os << symbol;
    } else {
      os << "**missing name**"
         << " (" << guid << ")";
    }
    os << "is referenced by:\n";

    auto target = markedBy;
    while (target) {
      os << " - ";
      if (!target->symbol.empty()) {
        os << target->symbol;
      } else {
        os << "**missing name**";
      }
      os << " (" << target->guid << ")";
      os << "\n";
      target = target->markedBy;
    }
  }
};

VFuncSlot createVFuncSlot(FunctionSummary::Call call) {
  VFuncSlot::KindTy slotKind;
  switch (call.getKind()) {
    case FunctionSummary::Call::Witness: {
      slotKind = VFuncSlot::Witness;
      break;
    }
    case FunctionSummary::Call::VTable: {
      slotKind = VFuncSlot::VTable;
      break;
    }
    case FunctionSummary::Call::Direct: {
      llvm_unreachable("Can't get slot for static call");
    }
    case FunctionSummary::Call::kindCount: {
      llvm_unreachable("impossible");
    }
  }
  return VFuncSlot(slotKind, call.getCallee());
}

void markDeadTypeRef(ModuleSummaryIndex &summary, const llvm::DenseSet<GUID> &PreservedGUIDs) {
  SmallVector<GUID, 8> Worklist;
  SmallSetVector<GUID, 16> beenInWorklist;
  SmallSetVector<GUID, 16> UseMarkedTypes;

  Worklist.append(PreservedGUIDs.begin(), PreservedGUIDs.end());
  
  while (!Worklist.empty()) {
    auto target = Worklist.pop_back_val();
    if (!beenInWorklist.insert(target)) {
      continue;
    }
    auto maybeSummary = summary.getFunctionSummary(target);
    if (!maybeSummary) {
      llvm_unreachable("Bad GUID");
    }
    auto FS = maybeSummary;

    for (auto typeRef : FS->typeRefs()) {
      if (UseMarkedTypes.insert(typeRef.Guid)) {
        summary.markUsedType(typeRef.Guid);
      }
    }
    for (auto Call : FS->calls()) {
      switch (Call.getKind()) {
      case FunctionSummary::Call::Direct: {
        Worklist.push_back(Call.getCallee());
        continue;
      }
      case FunctionSummary::Call::Witness:
      case FunctionSummary::Call::VTable: {
        VFuncSlot slot = createVFuncSlot(Call);
        auto Impls = summary.getImplementations(slot);
        for (auto Impl : Impls) {
          Worklist.push_back(Impl.Guid);
        }
        break;
      }
      case FunctionSummary::Call::kindCount:
        llvm_unreachable("impossible");
      }
    }
  }
}

void markDeadSymbols(ModuleSummaryIndex &summary, llvm::DenseSet<GUID> &PreservedGUIDs) {

  SmallVector<std::shared_ptr<LivenessTrace>, 8> Worklist;
  auto UsedTypesList = summary.getUsedTypeList();
  std::set<GUID> UsedTypesSet(UsedTypesList.begin(), UsedTypesList.end());
  unsigned LiveSymbols = 0;

  for (auto GUID : PreservedGUIDs) {
    auto trace = std::make_shared<LivenessTrace>(
        nullptr, GUID, LivenessTrace::Preserved);
    auto maybeFS = summary.getFunctionSummary(GUID);
    if (!maybeFS) {
      llvm_unreachable("Bad GUID");
    }
    if (!maybeFS->getName().empty()) {
      trace->setName(maybeFS->getName());
    }
    Worklist.push_back(trace);
  }
  std::set<std::shared_ptr<LivenessTrace>> dumpTargets;
  while (!Worklist.empty()) {
    auto trace = Worklist.pop_back_val();

    auto maybeSummary = summary.getFunctionSummary(trace->guid);
    if (!maybeSummary) {
      llvm_unreachable("Bad GUID");
    }
    auto FS = maybeSummary;
    if (FS->isLive()) continue;

    if (!FS->getName().empty()) {
      LLVM_DEBUG(llvm::dbgs() << "Mark " << FS->getName() << " as live\n");
    } else {
      LLVM_DEBUG(llvm::dbgs() << "Mark (" << FS->getGUID() << ") as live\n");
    }
    FS->setLive(true);
    LiveSymbols++;

    auto queueWorklist = [&](std::shared_ptr<LivenessTrace> trace) {
      auto maybeCallee = summary.getFunctionSummary(trace->guid);
      if (!maybeCallee) {
        llvm_unreachable("Bad GUID");
      }
      auto Callee = maybeCallee;
      if (!Callee->getName().empty()) {
        trace->setName(Callee->getName());
        if (LTOPrintLiveTrace == Callee->getName()) {
          dumpTargets.insert(trace);
        }
      }
      Worklist.push_back(trace);
    };

    for (auto Call : FS->calls()) {
      switch (Call.getKind()) {
      case FunctionSummary::Call::Direct: {
        queueWorklist(std::make_shared<LivenessTrace>(
            trace, Call.getCallee(), LivenessTrace::StaticReferenced));
        continue;
      }
      case FunctionSummary::Call::Witness:
      case FunctionSummary::Call::VTable: {
        VFuncSlot slot = createVFuncSlot(Call);
        auto Impls = summary.getImplementations(slot);
        for (auto Impl : Impls) {
          if (UsedTypesSet.find(Impl.TypeGuid) == UsedTypesSet.end()) {
            continue;
          }
          queueWorklist(std::make_shared<LivenessTrace>(
              trace, Impl.Guid, LivenessTrace::IndirectReferenced));
        }
        break;
      }
      case FunctionSummary::Call::kindCount:
        llvm_unreachable("impossible");
      }
    }
  }
  for (auto dumpTarget : dumpTargets) {
    dumpTarget->dump();
  }
}

int cross_module_opt_main(ArrayRef<const char *> Args, const char *Argv0,
                          void *MainAddr) {
  INITIALIZE_LLVM();

  CompilerInstance Instance;
  PrintingDiagnosticConsumer PDC;
  Instance.addDiagnosticConsumer(&PDC);

  MergeModuleSummaryInvocation Invocation;
  if (Invocation.parseArgs(Args, Instance.getDiags())) {
    return true;
  }

  if (Invocation.InputFilenames.empty()) {
    Instance.getDiags().diagnose(SourceLoc(),
                                 diag::error_mode_requires_an_input_file);
    return 1;
  }

  auto TheSummary = std::make_unique<ModuleSummaryIndex>();

  for (auto Filename : Invocation.InputFilenames) {
    LLVM_DEBUG(llvm::dbgs() << "Loading module summary " << Filename << "\n");
    auto ErrOrBuf = llvm::MemoryBuffer::getFile(Filename);
    if (!ErrOrBuf) {
      Instance.getDiags().diagnose(
          SourceLoc(), diag::error_no_such_file_or_directory, Filename);
      return 1;
    }

    auto HasErr = swift::modulesummary::loadModuleSummaryIndex(
        ErrOrBuf.get()->getMemBufferRef(), *TheSummary.get());

    if (HasErr)
      llvm::report_fatal_error("Invalid module summary");
  }

  TheSummary->setName("combined");
  
  auto PreservedGUIDs = computePreservedGUIDs(TheSummary.get());
  markDeadTypeRef(*TheSummary.get(), PreservedGUIDs);
  markDeadSymbols(*TheSummary.get(), PreservedGUIDs);

  modulesummary::writeModuleSummaryIndex(*TheSummary, Instance.getDiags(),
                                         Invocation.OutputFilename);
  return 0;
}
