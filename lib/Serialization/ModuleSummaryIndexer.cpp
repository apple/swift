#include "swift/SIL/SILDeclRef.h"
#include "swift/SIL/SILFunction.h"
#include "swift/SIL/SILModule.h"
#include "swift/Serialization/ModuleSummary.h"
#include "llvm/Support/MD5.h"
#include "llvm/Support/raw_ostream.h"

#define DEBUG_TYPE "module-summary-index"

using namespace swift;
using namespace modulesummary;

GUID modulesummary::getGUIDFromUniqueName(llvm::StringRef Name) {
  return llvm::MD5Hash(Name);
}

namespace {
class FunctionSummaryIndexer {
  const SILFunction &F;
  std::unique_ptr<FunctionSummary> TheSummary;

  void indexDirectFunctionCall(const SILFunction &Callee);
  void indexIndirectFunctionCall(const SILDeclRef &Callee,
                                 FunctionSummary::Call::KindTy Kind);
  void indexInstruction(const SILInstruction *I);

public:
  FunctionSummaryIndexer(const SILFunction &F) : F(F) {}
  void indexFunction();

  std::unique_ptr<FunctionSummary> takeSummary() {
    return std::move(TheSummary);
  }
};

void FunctionSummaryIndexer::indexDirectFunctionCall(
    const SILFunction &Callee) {
  GUID guid = getGUIDFromUniqueName(Callee.getName());
  FunctionSummary::Call call(guid, Callee.getName().str(),
                             FunctionSummary::Call::Direct);
  TheSummary->addCall(call);
}

void FunctionSummaryIndexer::indexIndirectFunctionCall(
    const SILDeclRef &Callee, FunctionSummary::Call::KindTy Kind) {
  std::string mangledName = Callee.mangle();
  GUID guid = getGUIDFromUniqueName(mangledName);
  FunctionSummary::Call call(guid, mangledName, Kind);
  TheSummary->addCall(call);
}

void FunctionSummaryIndexer::indexInstruction(const SILInstruction *I) {
  if (auto *FRI = dyn_cast<FunctionRefInst>(I)) {
    SILFunction *callee = FRI->getReferencedFunctionOrNull();
    assert(callee);
    indexDirectFunctionCall(*callee);
    return;
  }

  if (auto *WMI = dyn_cast<WitnessMethodInst>(I)) {
    indexIndirectFunctionCall(WMI->getMember(), FunctionSummary::Call::Witness);
    return;
  }

  if (auto *MI = dyn_cast<MethodInst>(I)) {
    indexIndirectFunctionCall(MI->getMember(), FunctionSummary::Call::VTable);
    return;
  }

  if (auto *FRI = dyn_cast<DynamicFunctionRefInst>(I)) {
    SILFunction *callee = FRI->getInitiallyReferencedFunction();
    assert(callee);
    indexDirectFunctionCall(*callee);
    return;
  }

  if (auto *FRI = dyn_cast<PreviousDynamicFunctionRefInst>(I)) {
    SILFunction *callee = FRI->getInitiallyReferencedFunction();
    assert(callee);
    indexDirectFunctionCall(*callee);
    return;
  }

  if (auto *KPI = dyn_cast<KeyPathInst>(I)) {
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
}

bool shouldPreserveFunction(const SILFunction &F) {
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
  TheSummary = std::make_unique<FunctionSummary>(guid);
  TheSummary->setName(F.getName().str());
  for (auto &BB : F) {
    for (auto &I : BB) {
      indexInstruction(&I);
    }
  }
  TheSummary->setPreserved(shouldPreserveFunction(F));
}

class ModuleSummaryIndexer {
  std::unique_ptr<ModuleSummaryIndex> TheSummary;
  const SILModule &Mod;
  void ensurePreserved(const SILFunction &F);
  void ensurePreserved(const SILDeclRef &Ref, VFuncSlot::KindTy Kind);
  void preserveKeyPathFunctions(const SILProperty &P);
  void indexWitnessTable(const SILWitnessTable &WT);
  void indexVTable(const SILVTable &VT);

public:
  ModuleSummaryIndexer(const SILModule &M) : Mod(M) {}
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

  for (GUID Impl : Impls) {
    auto FS = TheSummary->getFunctionSummary(Impl);
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
  for (auto entry : WT.getEntries()) {
    if (entry.getKind() != SILWitnessTable::Method)
      continue;

    auto methodWitness = entry.getMethodWitness();
    auto Witness = methodWitness.Witness;
    if (!Witness)
      continue;
    auto slot = createVFuncSlot(methodWitness.Requirement, VFuncSlot::Witness);
    TheSummary->addImplementation(slot,
                                  getGUIDFromUniqueName(Witness->getName()));

    if (isPossibllyUsedExternally) {
      ensurePreserved(*Witness);
    }
  }
}

void ModuleSummaryIndexer::indexVTable(const SILVTable &VT) {

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
    TheSummary->addImplementation(slot, getGUIDFromUniqueName(Impl->getName()));
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
    FS->setPreserved(shouldPreserveFunction(F));
    TheSummary->addFunctionSummary(std::move(FS));
  }

  // FIXME: KeyPaths can be eliminated but now preserved conservatively.
  for (auto &P : Mod.getPropertyList()) {
    preserveKeyPathFunctions(P);
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
