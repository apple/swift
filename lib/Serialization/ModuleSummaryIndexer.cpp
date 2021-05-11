#include "swift/AST/ASTMangler.h"
#include "swift/SIL/SILDeclRef.h"
#include "swift/SIL/SILFunction.h"
#include "swift/SIL/SILModule.h"
#include "swift/SIL/SILVisitor.h"
#include "swift/Serialization/ModuleSummary.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/MD5.h"
#include "llvm/Support/raw_ostream.h"

#define DEBUG_TYPE "module-summary-index"

static llvm::cl::opt<bool> EagerElimination(
    "module-summary-eager-elim", llvm::cl::init(false),
    llvm::cl::desc(
        "Enable eager elimination which doesn't support debug print"));

using namespace swift;
using namespace modulesummary;

GUID modulesummary::getGUIDFromUniqueName(llvm::StringRef Name) {
  return llvm::MD5Hash(Name);
}

static GUID getTypeGUID(NominalTypeDecl *type) {
  Mangle::ASTMangler mangler;
  std::string mangled = mangler.mangleNominalType(type);
  return getGUIDFromUniqueName(mangled);
}

namespace {
class FunctionSummaryIndexer : public SILInstructionVisitor<FunctionSummaryIndexer> {
  friend SILInstructionVisitor<FunctionSummaryIndexer>;

  SILFunction &F;
  std::unique_ptr<FunctionSummary> TheSummary;

  std::set<GUID> RecordedTypes;
  std::set<GUID> RecordedDirectTargets;
  std::set<GUID> RecordedVTableTargets;
  std::set<GUID> RecordedWitnessTargets;

  void indexDirectFunctionCall(const SILFunction &Callee);
  void indexIndirectFunctionCall(const SILDeclRef &Callee,
                                 FunctionSummary::Call::KindTy Kind);

  /// Index that the conformances of type can be used from this function.
  void indexUseOfType(CanType type);

  void visitFunctionRefInst(FunctionRefInst *FRI);
  void visitWitnessMethodInst(WitnessMethodInst *WMI);
  void visitMethodInst(MethodInst *MI);
  void visitDynamicFunctionRefInst(DynamicFunctionRefInst *FRI);
  void visitPreviousDynamicFunctionRefInst(PreviousDynamicFunctionRefInst *FRI);
  void visitKeyPathInst(KeyPathInst *KPI);

  void visitAllocExistentialBoxInst(AllocExistentialBoxInst *AEBI);
  void visitAllocGlobalInst(AllocGlobalInst *AGI);
  void visitAllocRefInst(AllocRefInst *ARI);
  void visitAllocStackInst(AllocStackInst *ASI);
  void visitAllocValueBufferInst(AllocValueBufferInst *AVBI);
  void visitApplyInst(ApplyInst *AI);
  void visitBeginApplyInst(BeginApplyInst *BAI);
  void visitBuiltinInst(BuiltinInst *BI);
  void visitCheckedCastBranchInst(CheckedCastBranchInst *CCBI);
  void visitCheckedCastAddrBranchInst(CheckedCastAddrBranchInst *CCABI);
  void visitCheckedCastValueBranchInst(CheckedCastValueBranchInst *CCVBI);
  void visitCopyAddrInst(CopyAddrInst *CAI);
  void visitCopyValueInst(CopyValueInst *CVI);
  void visitDestroyAddrInst(DestroyAddrInst *DAI);
  void visitDestroyValueInst(DestroyValueInst *DVI);
  void visitGlobalAddrInst(GlobalAddrInst *GAI);
  void visitGlobalValueInst(GlobalValueInst *GVI);
  //  void visitKeyPathInst(KeyPathInst *KPI);
  void visitInitEnumDataAddrInst(InitEnumDataAddrInst *IEDAI);
  void visitInjectEnumAddrInst(InjectEnumAddrInst *IEAI);
  void visitInitExistentialAddrInst(InitExistentialAddrInst *IEAI);
  void visitInitExistentialMetatypeInst(InitExistentialMetatypeInst *IEMI);
  void visitInitExistentialRefInst(InitExistentialRefInst *IERI);
  void visitInitExistentialValueInst(InitExistentialValueInst *IEVI);
  void visitMetatypeInst(MetatypeInst *MI);
  void visitPartialApplyInst(PartialApplyInst *PAI);
  void visitSelectEnumAddrInst(SelectEnumAddrInst *SEAI);
  void visitStructElementAddrInst(StructElementAddrInst *SEAI);
  void visitTryApplyInst(TryApplyInst *TAI);
  void visitTupleElementAddrInst(TupleElementAddrInst *TEAI);
  void visitUnconditionalCheckedCastInst(UnconditionalCheckedCastInst *UCCI);
  void visitUnconditionalCheckedCastAddrInst(
      UnconditionalCheckedCastAddrInst *UCCAI);
  void
  visitUncheckedTakeEnumDataAddrInst(UncheckedTakeEnumDataAddrInst *UTEDAI);
  //  void visitWitnessMethodInst(WitnessMethodInst *WMI);

  void visitSILInstruction(SILInstruction *I) {}
public:
  FunctionSummaryIndexer(SILFunction &F) : F(F) {}
  void indexFunction();

  std::unique_ptr<FunctionSummary> takeSummary() {
    return std::move(TheSummary);
  }
};

void FunctionSummaryIndexer::indexDirectFunctionCall(
    const SILFunction &Callee) {
  GUID guid = getGUIDFromUniqueName(Callee.getName());
  if (!RecordedDirectTargets.insert(guid).second) {
    return;
  }
  FunctionSummary::Call call(guid, Callee.getName().str(),
                             FunctionSummary::Call::Direct);
  TheSummary->addCall(call);
}

void FunctionSummaryIndexer::indexIndirectFunctionCall(
    const SILDeclRef &Callee, FunctionSummary::Call::KindTy Kind) {
  std::string mangledName = Callee.mangle();
  GUID guid = getGUIDFromUniqueName(mangledName);
  std::set<GUID> &RecordedTargets = Kind == FunctionSummary::Call::VTable ?
    RecordedVTableTargets : RecordedWitnessTargets;
  if (!RecordedTargets.insert(guid).second) {
    return;
  }
  FunctionSummary::Call call(guid, mangledName, Kind);
  TheSummary->addCall(call);
}

void FunctionSummaryIndexer::indexUseOfType(CanType type) {
  Mangle::ASTMangler mangler;
  type.visit([&](Type t) {
    auto *decl = t->getAnyNominal();
    if (!decl) {
      return;
    }
    std::string mangled = mangler.mangleNominalType(decl);
    GUID guid = getGUIDFromUniqueName(mangled);
    if (RecordedTypes.insert(guid).second) {
      TheSummary->addTypeRef({guid, mangled});
    }
  });
}

void FunctionSummaryIndexer::visitAllocExistentialBoxInst(
    AllocExistentialBoxInst *AEBI) {
  indexUseOfType(AEBI->getFormalConcreteType());
}

void FunctionSummaryIndexer::visitAllocGlobalInst(AllocGlobalInst *AGI) {
  indexUseOfType(AGI->getReferencedGlobal()->getLoweredType().getASTType());
}

void FunctionSummaryIndexer::visitAllocRefInst(AllocRefInst *ARI) {
  indexUseOfType(ARI->getType().getASTType());
}
void FunctionSummaryIndexer::visitAllocStackInst(AllocStackInst *ASI) {
  indexUseOfType(ASI->getType().getASTType());
}
void FunctionSummaryIndexer::visitAllocValueBufferInst(
    AllocValueBufferInst *AVBI) {
  indexUseOfType(AVBI->getType().getASTType());
}
void FunctionSummaryIndexer::visitApplyInst(ApplyInst *AI) {
  indexUseOfType(AI->getSubstCalleeType());
}
void FunctionSummaryIndexer::visitBeginApplyInst(BeginApplyInst *BAI) {
  indexUseOfType(BAI->getSubstCalleeType());
}
void FunctionSummaryIndexer::visitBuiltinInst(BuiltinInst *BI) {
  // FIXME: Need to index substitution map?
}
void FunctionSummaryIndexer::visitCheckedCastBranchInst(
    CheckedCastBranchInst *CCBI) {
  indexUseOfType(CCBI->getSourceFormalType());
  indexUseOfType(CCBI->getTargetFormalType());
}
void FunctionSummaryIndexer::visitCheckedCastAddrBranchInst(
    CheckedCastAddrBranchInst *CCABI) {
  indexUseOfType(CCABI->getSourceFormalType());
  indexUseOfType(CCABI->getTargetFormalType());
}
void FunctionSummaryIndexer::visitCheckedCastValueBranchInst(
    CheckedCastValueBranchInst *CCVBI) {
  indexUseOfType(CCVBI->getSourceFormalType());
  indexUseOfType(CCVBI->getTargetFormalType());
}
void FunctionSummaryIndexer::visitCopyAddrInst(CopyAddrInst *CAI) {
  indexUseOfType(CAI->getSrc()->getType().getASTType());
  indexUseOfType(CAI->getDest()->getType().getASTType());
}
void FunctionSummaryIndexer::visitCopyValueInst(CopyValueInst *CVI) {
  indexUseOfType(CVI->getOperand()->getType().getASTType());
}
void FunctionSummaryIndexer::visitDestroyAddrInst(DestroyAddrInst *DAI) {
  indexUseOfType(DAI->getOperand()->getType().getASTType());
}
void FunctionSummaryIndexer::visitDestroyValueInst(DestroyValueInst *DVI) {
  indexUseOfType(DVI->getOperand()->getType().getASTType());
}
void FunctionSummaryIndexer::visitGlobalAddrInst(GlobalAddrInst *GAI) {
  indexUseOfType(GAI->getReferencedGlobal()->getLoweredType().getASTType());
}
void FunctionSummaryIndexer::visitGlobalValueInst(GlobalValueInst *GVI) {
  indexUseOfType(GVI->getReferencedGlobal()->getLoweredType().getASTType());
}
void FunctionSummaryIndexer::visitInitEnumDataAddrInst(
    InitEnumDataAddrInst *IEDAI) {
  indexUseOfType(IEDAI->getOperand()->getType().getASTType());
}
void FunctionSummaryIndexer::visitInjectEnumAddrInst(InjectEnumAddrInst *IEAI) {
  indexUseOfType(IEAI->getOperand()->getType().getASTType());
}
void FunctionSummaryIndexer::visitInitExistentialAddrInst(
    InitExistentialAddrInst *IEAI) {
  indexUseOfType(IEAI->getFormalConcreteType());
}
void FunctionSummaryIndexer::visitInitExistentialMetatypeInst(
    InitExistentialMetatypeInst *IEMI) {
  indexUseOfType(IEMI->getOperand()->getType().getASTType());
}
void FunctionSummaryIndexer::visitInitExistentialRefInst(
    InitExistentialRefInst *IERI) {
  indexUseOfType(IERI->getFormalConcreteType());
}
void FunctionSummaryIndexer::visitInitExistentialValueInst(
    InitExistentialValueInst *IEVI) {
  indexUseOfType(IEVI->getFormalConcreteType());
}
void FunctionSummaryIndexer::visitMetatypeInst(MetatypeInst *MI) {
  indexUseOfType(MI->getType().getASTType());
}
void FunctionSummaryIndexer::visitPartialApplyInst(PartialApplyInst *PAI) {
  indexUseOfType(PAI->getSubstCalleeType());
}
void FunctionSummaryIndexer::visitSelectEnumAddrInst(SelectEnumAddrInst *SEAI) {
  indexUseOfType(SEAI->getEnumOperand()->getType().getASTType());
}
void FunctionSummaryIndexer::visitStructElementAddrInst(
    StructElementAddrInst *SEAI) {
  indexUseOfType(SEAI->getOperand()->getType().getASTType());
}
void FunctionSummaryIndexer::visitTryApplyInst(TryApplyInst *TAI) {
  indexUseOfType(TAI->getSubstCalleeType());
}
void FunctionSummaryIndexer::visitTupleElementAddrInst(
    TupleElementAddrInst *TEAI) {
  indexUseOfType(TEAI->getOperand()->getType().getASTType());
}
void FunctionSummaryIndexer::visitUnconditionalCheckedCastInst(
    UnconditionalCheckedCastInst *UCCI) {
  indexUseOfType(UCCI->getSourceFormalType());
  indexUseOfType(UCCI->getTargetFormalType());
}
void FunctionSummaryIndexer::visitUnconditionalCheckedCastAddrInst(
    UnconditionalCheckedCastAddrInst *UCCAI) {
  indexUseOfType(UCCAI->getSourceFormalType());
  indexUseOfType(UCCAI->getTargetFormalType());
}
void FunctionSummaryIndexer::visitUncheckedTakeEnumDataAddrInst(
    UncheckedTakeEnumDataAddrInst *UTEDAI) {
  indexUseOfType(UTEDAI->getOperand()->getType().getASTType());
}

void FunctionSummaryIndexer::visitFunctionRefInst(FunctionRefInst *FRI) {
  SILFunction *callee = FRI->getReferencedFunctionOrNull();
  assert(callee);
  indexDirectFunctionCall(*callee);
}

void FunctionSummaryIndexer::visitWitnessMethodInst(WitnessMethodInst *WMI) {
  indexIndirectFunctionCall(WMI->getMember(), FunctionSummary::Call::Witness);
}

void FunctionSummaryIndexer::visitMethodInst(MethodInst *MI) {
  indexIndirectFunctionCall(MI->getMember(), FunctionSummary::Call::VTable);
}

void FunctionSummaryIndexer::visitDynamicFunctionRefInst(DynamicFunctionRefInst *FRI) {
  SILFunction *callee = FRI->getInitiallyReferencedFunction();
  assert(callee);
  indexDirectFunctionCall(*callee);
}

void FunctionSummaryIndexer::visitPreviousDynamicFunctionRefInst(PreviousDynamicFunctionRefInst *FRI) {
  SILFunction *callee = FRI->getInitiallyReferencedFunction();
  assert(callee);
  indexDirectFunctionCall(*callee);
}

void FunctionSummaryIndexer::visitKeyPathInst(KeyPathInst *KPI) {
  for (auto &component : KPI->getPattern()->getComponents()) {
    component.visitReferencedFunctionsAndMethods(
        [this](SILFunction *F) {
          assert(F);
          indexDirectFunctionCall(*F);
        },
        [this](SILDeclRef method) {
          auto decl = cast<AbstractFunctionDecl>(method.getDecl());
          if (auto clas = dyn_cast<ClassDecl>(decl->getDeclContext())) {
            indexIndirectFunctionCall(method, FunctionSummary::Call::VTable);
          } else if (isa<ProtocolDecl>(decl->getDeclContext())) {
            indexIndirectFunctionCall(method, FunctionSummary::Call::Witness);
          } else {
            llvm_unreachable(
                "key path keyed by a non-class, non-protocol method");
          }
        });
  }
}

bool shouldPreserveFunction(const SILFunction &F) {
  if (EagerElimination &&
      (F.getName().equals("swift_unexpectedError") ||
       F.getName().equals("swift_errorInMain") ||
       F.getName().equals("$ss23_getErrorDomainNSStringyyXlSPyxGs0B0RzlF"))) {
    return false;
  }
  if (F.getName().equals(F.getASTContext().getEntryPointFunctionName())) {
    return true;
  }

  if (F.getRepresentation() == SILFunctionTypeRepresentation::ObjCMethod) {
    return true;
  }
  if (F.hasCReferences()) {
    return true;
  }
  if (F.isDynamicallyReplaceable()) {
    return true;
  }
  return false;
}

void FunctionSummaryIndexer::indexFunction() {
  GUID guid = getGUIDFromUniqueName(F.getName());
  uint32_t instSize = 0;
  TheSummary = std::make_unique<FunctionSummary>(guid);
  TheSummary->setName(F.getName().str());
  for (auto &BB : F) {
    for (auto &I : BB) {
      visit(&I);
      instSize++;
    }
  }
  TheSummary->setPreserved(shouldPreserveFunction(F));
  TheSummary->setInstSize(instSize);
}

class ModuleSummaryIndexer {
  std::unique_ptr<ModuleSummaryIndex> TheSummary;
  SILModule &Mod;
  void ensurePreserved(const SILFunction &F);
  void ensurePreserved(const SILDeclRef &Ref, VFuncSlot::KindTy Kind);
  void preserveKeyPathFunctions(const SILProperty &P);
  void indexWitnessTable(const SILWitnessTable &WT);
  void indexVTable(const SILVTable &VT);

public:
  ModuleSummaryIndexer(SILModule &M) : Mod(M) {}
  void indexModule();
  std::unique_ptr<ModuleSummaryIndex> takeSummary() {
    return std::move(TheSummary);
  }
};

void ModuleSummaryIndexer::ensurePreserved(const SILFunction &F) {
  GUID guid = getGUIDFromUniqueName(F.getName());
  auto FS = TheSummary->getFunctionSummary(guid);
  assert(FS);
  FS->setPreserved(true);
}

static VFuncSlot createVFuncSlot(SILDeclRef VFuncRef, VFuncSlot::KindTy Kind) {
  return VFuncSlot(Kind, getGUIDFromUniqueName(VFuncRef.mangle()));
}

void ModuleSummaryIndexer::ensurePreserved(const SILDeclRef &Ref,
                                           VFuncSlot::KindTy Kind) {
  auto slot = createVFuncSlot(Ref, Kind);
  auto Impls = TheSummary->getImplementations(slot);
  if (Impls.empty())
    return;

  for (VFuncImpl Impl : Impls) {
    auto FS = TheSummary->getFunctionSummary(Impl.Guid);
    assert(FS);
    FS->setPreserved(true);
  }
}

void ModuleSummaryIndexer::preserveKeyPathFunctions(const SILProperty &P) {
  auto maybeComponent = P.getComponent();
  if (!maybeComponent)
    return;

  KeyPathPatternComponent component = maybeComponent.getValue();
  component.visitReferencedFunctionsAndMethods(
      [&](SILFunction *F) { ensurePreserved(*F); },
      [&](SILDeclRef method) {
        auto decl = cast<AbstractFunctionDecl>(method.getDecl());
        if (isa<ClassDecl>(decl->getDeclContext())) {
          ensurePreserved(method, VFuncSlot::VTable);
        } else if (isa<ProtocolDecl>(decl->getDeclContext())) {
          ensurePreserved(method, VFuncSlot::Witness);
        } else {
          llvm_unreachable(
              "key path keyed by a non-class, non-protocol method");
        }
      });
}

void ModuleSummaryIndexer::indexWitnessTable(const SILWitnessTable &WT) {
  auto isPossibllyUsedExternally =
      WT.getDeclContext()->getParentModule() != Mod.getSwiftModule() ||
      WT.getProtocol()->getParentModule() != Mod.getSwiftModule();
  auto typeGUID = getTypeGUID(WT.getConformingType()->getAnyNominal());
  for (auto entry : WT.getEntries()) {
    if (entry.getKind() != SILWitnessTable::Method)
      continue;

    auto methodWitness = entry.getMethodWitness();
    auto Witness = methodWitness.Witness;
    if (!Witness)
      continue;
    auto slot = createVFuncSlot(methodWitness.Requirement, VFuncSlot::Witness);
    TheSummary->addImplementation(slot,
                                  getGUIDFromUniqueName(Witness->getName()),
                                  typeGUID);

    if (isPossibllyUsedExternally) {
      ensurePreserved(*Witness);
    }
  }
}

void ModuleSummaryIndexer::indexVTable(const SILVTable &VT) {
  auto typeGUID = getTypeGUID(VT.getClass());
  for (auto entry : VT.getEntries()) {
    auto Impl = entry.getImplementation();
    if (entry.getMethod().kind == SILDeclRef::Kind::Deallocator ||
        entry.getMethod().kind == SILDeclRef::Kind::IVarDestroyer) {
      // Destructors are preserved because they can be called from swift_release
      // dynamically
      ensurePreserved(*Impl);
    }
    auto methodModule = entry.getMethod().getDecl()->getModuleContext();
    auto isExternalMethod = methodModule != Mod.getSwiftModule();

    if (entry.getKind() == SILVTableEntry::Override && isExternalMethod) {
      ensurePreserved(*Impl);
    }
    auto slot = createVFuncSlot(entry.getMethod(), VFuncSlot::VTable);
    TheSummary->addImplementation(slot, getGUIDFromUniqueName(Impl->getName()), typeGUID);
  }
}

void ModuleSummaryIndexer::indexModule() {
  TheSummary = std::make_unique<ModuleSummaryIndex>();
  auto moduleName = Mod.getSwiftModule()->getName().str();
  TheSummary->setName(moduleName.str());

  for (auto &F : Mod) {
    FunctionSummaryIndexer indexer(F);
    indexer.indexFunction();
    std::unique_ptr<FunctionSummary> FS = indexer.takeSummary();
    TheSummary->addFunctionSummary(std::move(FS));
  }

  for (auto &WT : Mod.getWitnessTableList()) {
    indexWitnessTable(WT);
  }

  for (auto VT : Mod.getVTables()) {
    indexVTable(*VT);
  }
}
}; // namespace

std::unique_ptr<ModuleSummaryIndex>
modulesummary::buildModuleSummaryIndex(SILModule &M) {
  ModuleSummaryIndexer indexer(M);
  indexer.indexModule();
  return indexer.takeSummary();
}
