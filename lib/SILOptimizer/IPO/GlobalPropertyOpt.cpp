//===--- GlobalPropertyOpt.cpp - Optimizes global array properties --------===//
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

#define DEBUG_TYPE "globalpropertyopt"
#include "swift/Basic/Assertions.h"
#include "swift/SIL/SILArgument.h"
#include "swift/SIL/SILBuilder.h"
#include "swift/SIL/SILFunction.h"
#include "swift/SIL/SILInstruction.h"
#include "swift/SIL/SILModule.h"
#include "swift/SILOptimizer/Analysis/ArraySemantic.h"
#include "swift/SILOptimizer/PassManager/Passes.h"
#include "swift/SILOptimizer/PassManager/Transforms.h"
#include "swift/SILOptimizer/Utils/InstOptUtils.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseMapInfo.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/Support/Allocator.h"
#include "llvm/Support/Debug.h"

using namespace swift;

STATISTIC(NumPropertiesReplaced, "Number of array property calls replaced");

namespace {

/// The GlobalPropertyOpt performs an analysis on the whole module to determine
/// the values of high-level properties.
///
/// Currently only one property is handled and that's the isNativeTypeChecked
/// property for arrays. If the property can be proved to be true, the
/// corresponding semantics-call is replaced by a true-literal.
class GlobalPropertyOpt {
  
  /// An entry in the dependency graph. An entry can represent
  ///   *) a value of type Array,
  ///   *) a value of type tuple, which contains an Array,
  ///   *) an AllocStack instruction which allocates an Array or
  ///   *) a struct or class field of type Array.
  struct Entry {
    
    Entry(SILValue Value, VarDecl *Field) :
    Value(Value), Field(Field), isNativeTypeChecked(true) {
    }
    
    /// Non-null if the entry represents an array value, a tuple with an array
    /// or an AllocStack of an array.
    SILValue Value;
    
    /// Non-null if the entry represents a struct or class field.
    VarDecl *Field;
    
    /// The property which we want to track: is the value/field a native swift
    /// array which doesn't need deferred type check.
    bool isNativeTypeChecked;
    
    /// The edges in the dependency graph, i.e. entries, which depend on this
    /// entry.
    SmallVector<Entry *, 8> Dependencies;

#ifndef NDEBUG
    friend raw_ostream &operator<<(raw_ostream &os, const Entry &entry) {
      if (entry.Field)
        return os << "field " << entry.Field->getName() << '\n';
      if (!entry.Value)
        return os << "unknown-address\n";
      if (auto *Inst = entry.Value->getDefiningInstruction())
        return os << Inst->getFunction()->getName() << ": " << entry.Value;
      if (auto *Arg = dyn_cast<SILArgument>(entry.Value))
        return os << Arg->getFunction()->getName() << ": " << entry.Value;
      return os << entry.Value;
    }
#endif
  };
  
  /// The module that we are optimizing.
  SILModule &M;
  
  NominalTypeDecl *ArrayType;
  
  /// All entries of the dependency graph, which represent values or AllocStack.
  llvm::DenseMap<SILValue, Entry *> ValueEntries;
  
  /// All entries of the dependency graph, which represent fields.
  llvm::DenseMap<VarDecl *, Entry *> FieldEntries;
  
  llvm::SpecificBumpPtrAllocator<Entry> EntryAllocator;
  
  /// Represents an address of an unknown array.
  Entry unknownAddressEntry = Entry(SILValue(), nullptr);
  
  /// All found calls to get-property semantic functions.
  std::vector<ApplyInst *> propertyCalls;

  llvm::SetVector<SILFunction *> ChangedFunctions;

  /// Contains entries with a false property value, which must be propagated
  /// to their dependencies.
  llvm::SmallVector<Entry *, 32> WorkList;
  
  bool isArrayType(SILType type) {
    return type.getNominalOrBoundGenericNominal() == ArrayType &&
           !type.isAddress();
  }
  
  bool isArrayAddressType(SILType type) {
    return type.getNominalOrBoundGenericNominal() == ArrayType &&
           type.isAddress();
  }
  
  /// Returns true if the type is a tuple which contains at least one array
  /// (we don't check for arrays in nested tuples).
  bool isTupleWithArray(CanType type) {
    if (auto tuple = dyn_cast<TupleType>(type)) {
      for (Type subType : tuple->getElementTypes()) {
        if (CanType(subType).getNominalOrBoundGenericNominal() == ArrayType)
          return true;
      }
    }
    return false;
  }

  static bool canAddressEscape(SILValue V, bool acceptStore);

  /// Gets the entry for a struct or class field.
  Entry *getFieldEntry(VarDecl *Field) {
    Entry * &entry = FieldEntries[Field];
    if (!entry) {
      entry = new (EntryAllocator.Allocate()) Entry(SILValue(), Field);
      if (M.isVisibleExternally(Field))
        setAddressEscapes(entry);
    }
    return entry;
  }

  /// Gets the entry for a value at an address, e.g. a struct/class field or
  /// an alloc_stack.
  Entry *getAddrEntry(SILValue value) {
    ValueBase *def = value;
    if (auto *MDI = dyn_cast<MarkDependenceInst>(def)) {
      return getAddrEntry(MDI->getOperand(0));
    }
    if (auto *RAI = dyn_cast<RefElementAddrInst>(def)) {
      return getFieldEntry(RAI->getField());
    }
    if (auto *SEI = dyn_cast<StructElementAddrInst>(def)) {
      return getFieldEntry(SEI->getField());
    }
    if (isa<AllocStackInst>(def)) {
      Entry * &entry = ValueEntries[value];
      if (!entry) {
        entry = new (EntryAllocator.Allocate()) Entry(value, nullptr);
        if (canAddressEscape(value, true))
          setAddressEscapes(entry);
      }
      return entry;
    }
    return &unknownAddressEntry;
  }
  
  /// Gets the entry for a SIL value, e.g. an array-value or a tuple containing
  /// an array.
  Entry *getValueEntry(SILValue value) {
    Entry * &entry = ValueEntries[value];
    if (!entry) {
      entry = new (EntryAllocator.Allocate()) Entry(value, nullptr);
    }
    return entry;
  }
  
  void setAddressEscapes(Entry *entry) {
    LLVM_DEBUG(llvm::dbgs() << "     address escapes: " << *entry);
    setNotNative(entry);
  }
  
  void setNotNative(Entry *entry) {
    if (entry->isNativeTypeChecked) {
      LLVM_DEBUG(llvm::dbgs() << "      set not-native: " << *entry);
      entry->isNativeTypeChecked = false;
      WorkList.push_back(entry);
    }
  }
  
  void addDependency(Entry *from, Entry *to) {
    LLVM_DEBUG(llvm::dbgs() << "    add dependency from: " << *from
                            << "      to: " << *to);
    from->Dependencies.push_back(to);
  }
  
  void scanInstruction(swift::SILInstruction *Inst);
  
  void scanInstructions();
  
  void propagatePropertiesInGraph();
  
  void replacePropertyCalls();
  
public:
  GlobalPropertyOpt(SILModule &Module) :
      M(Module), ArrayType(nullptr) {}
  
  void run(SILModuleTransform *T);
};

/// Checks if an address value does escape. If \p acceptStore is false, then
/// we handle a store to the address like if the address would escape.
bool GlobalPropertyOpt::canAddressEscape(SILValue V, bool acceptStore) {
  for (auto UI : V->getUses()) {
    auto *User = UI->getUser();
    
    // These instructions do not cause the address to escape.
    if (isa<LoadInst>(User) ||
        isa<DebugValueInst>(User) ||
        isa<StrongReleaseInst>(User) ||
        isa<StrongRetainInst>(User) ||
        isa<DeallocBoxInst>(User) ||
        isa<DeallocStackInst>(User))
      continue;
    
    if (acceptStore) {
      if (auto *Store = dyn_cast<StoreInst>(User)) {
        if (Store->getDest() == UI->get())
          continue;
      }
    }
    
    // These instructions only cause the value to escape if they are used in
    // a way that escapes.  Recursively check that the uses of the instruction
    // don't escape.
    if (isa<StructElementAddrInst>(User) || isa<TupleElementAddrInst>(User) ||
        isa<AddressToPointerInst>(User) || isa<PointerToAddressInst>(User)) {
      // We don't handle these instructions if we see them in store addresses.
      // So going through them lets stores be as bad as if the address would
      // escape.
      auto value = cast<SingleValueInstruction>(User);
      if (canAddressEscape(value, false))
        return true;
      continue;
    }
    if (auto markDependence = dyn_cast<MarkDependenceInst>(User)) {
      unsigned opNum = UI->getOperandNumber();
      if (opNum == 0 && canAddressEscape(markDependence, acceptStore))
        return true;
      continue;
    }
    if (auto apply = dyn_cast<ApplyInst>(User)) {
      // Check if the value is the this-argument of the array method.
      ArraySemanticsCall Call(apply);
      if (Call && Call.hasSelf() && &Call.getSelfOperand() == UI)
        continue;
    }
    return true;
  }
  return false;
}

/// Scan an instruction and build dependencies for it.
void GlobalPropertyOpt::scanInstruction(swift::SILInstruction *Inst) {
  if (auto *AI = dyn_cast<ApplyInst>(Inst)) {
    ArraySemanticsCall semCall(AI);
    switch (semCall.getKind()) {
      case ArrayCallKind::kArrayInit:
      case ArrayCallKind::kArrayInitEmpty:
      case ArrayCallKind::kArrayUninitialized:
      case ArrayCallKind::kMutateUnknown:
      case ArrayCallKind::kMakeMutable:
        // The return value of those calls (if any) do not return a non-native
        // swift array.
        LLVM_DEBUG(llvm::dbgs() << "      array semantics call: " << *AI);
        return;
      case ArrayCallKind::kArrayPropsIsNativeTypeChecked:
        // Remember the property-calls for later.
        LLVM_DEBUG(llvm::dbgs() << "      property check: " << *AI);
        propertyCalls.push_back(AI);
        break;
      default:
        break;
    }
  } else if (auto *LI = dyn_cast<LoadInst>(Inst)) {
    if (isArrayType(LI->getType())) {
      // Add a dependency from the value at the address to the loaded value.
      SILValue loadAddr = LI->getOperand();
      assert(loadAddr->getType().isAddress());
      addDependency(getAddrEntry(loadAddr), getValueEntry(LI));
      return;
    }
  } else if (auto *SI = dyn_cast<StoreInst>(Inst)) {
    SILValue src = SI->getSrc();
    if (isArrayType(src->getType())) {
      // Add a dependency from the operand to the value at the store-address.
      //
      SILValue dst = SI->getDest();
      assert(dst->getType().isAddress());
      addDependency(getValueEntry(src), getAddrEntry(dst));
      return;
    }
  } else if (isa<RefElementAddrInst>(Inst) || isa<StructElementAddrInst>(Inst)) {
    auto projection = cast<SingleValueInstruction>(Inst);
    if (isArrayAddressType(projection->getType())) {
      // If the address of an array-field escapes, we give up for that field.
      if (canAddressEscape(projection, true)) {
        setAddressEscapes(getAddrEntry(projection));
        LLVM_DEBUG(llvm::dbgs() << "      field address escapes: "
                                << *projection);
      }
      return;
    }
  } else if (auto *SEI = dyn_cast<StructExtractInst>(Inst)) {
    if (isArrayType(SEI->getType())) {
      // Add a dependency from the field to the extracted value.
      VarDecl *Field = SEI->getField();
      addDependency(getFieldEntry(Field), getValueEntry(SEI));
      return;
    }
  } else if (auto *TEI = dyn_cast<TupleExtractInst>(Inst)) {
    if (isArrayType(TEI->getType())) {
      // Add a dependency from the tuple itself to the extracted element.
      SILValue tuple = TEI->getOperand();
      addDependency(getValueEntry(tuple), getValueEntry(TEI));
      return;
    }
  } else if (auto *TI = dyn_cast<TupleInst>(Inst)) {
    if (isTupleWithArray(TI->getType().getASTType())) {
      // Add dependencies from array elements to the tuple itself.
      for (Operand &Op : TI->getAllOperands()) {
        SILValue V = Op.get();
        if (isArrayType(V->getType())) {
          addDependency(getValueEntry(V), getValueEntry(TI));
        }
      }
      return;
    }
  } else if (auto *SI = dyn_cast<StructInst>(Inst)) {
    // Add dependencies from the array operands to the struct array-fields.
    StructDecl *S = SI->getStructDecl();
    auto Props = S->getStoredProperties();
    auto Operands = SI->getAllOperands();
    for (unsigned I = 0, E = Props.size(); I < E; ++I) {
      VarDecl *VD = Props[I];
      const Operand &Op = Operands[I];
      if (isArrayType(Op.get()->getType())) {
        addDependency(getValueEntry(Op.get()), getFieldEntry(VD));
      }
    }
  } else if (isa<AllocStackInst>(Inst)) {
    // An alloc_stack itself does not introduce any non-native swift arrays.
    return;
  }
  // TODO: handle enums with array data.

  // For everything else which we didn't handle above: we set the property of
  // the instruction value to false.
  for (auto result : Inst->getResults()) {
    SILType Type = result->getType();
    if (isArrayType(Type) || isTupleWithArray(Type.getASTType())) {
      LLVM_DEBUG(llvm::dbgs() << "      value could be non-native array: "
                              << *result);
      setNotNative(getValueEntry(result));
    }
  }
}

/// Scans all instructions of the module and builds the dependency graph.
void GlobalPropertyOpt::scanInstructions() {
  for (auto &F : M) {
    LLVM_DEBUG(llvm::dbgs() << "  scan function " << F.getName() << "\n");
    for (auto &BB : F) {
      LLVM_DEBUG(llvm::dbgs() << "    scan basic block " << BB.getDebugID()
                              << "\n");

      // Add dependencies from predecessor's terminator operands to the block
      // arguments.
      int argIdx = 0;
      for (auto *BBArg : BB.getArguments()) {
        bool hasPreds = false;
        SILType Type = BBArg->getType();
        if (isArrayType(Type) || isTupleWithArray(Type.getASTType())) {
          for (auto *Pred : BB.getPredecessorBlocks()) {
            hasPreds = true;
            auto *Term = Pred->getTerminator();
            SILValue PredArg;
            if (auto *BI = dyn_cast<BranchInst>(Term)) {
              PredArg = BI->getArg(argIdx);
            } else if (auto *CBI = dyn_cast<CondBranchInst>(Term)) {
              PredArg = CBI->getArgForDestBB(&BB, BBArg);
            }
            if (PredArg) {
              addDependency(getValueEntry(PredArg), getValueEntry(BBArg));
            } else {
              // Some unknown terminator instruction.
              setNotNative(getValueEntry(BBArg));
              break;
            }
          }
          if (!hasPreds) {
            // This is the case for the function entry block.
            setNotNative(getValueEntry(BBArg));
            LLVM_DEBUG(llvm::dbgs() << "    unknown entry argument " << *BBArg);
          }
        }
        ++argIdx;
      }
      // Go through all instructions of the block.
      for (auto &Inst : BB) {
        scanInstruction(&Inst);
      }
    }
  }
}

/// Propagates the properties through the graph.
void GlobalPropertyOpt::propagatePropertiesInGraph() {
  LLVM_DEBUG(llvm::dbgs() << "  propagate properties\n");

  setAddressEscapes(&unknownAddressEntry);
  
  while (!WorkList.empty()) {
    Entry *entry = WorkList.pop_back_val();
    LLVM_DEBUG(llvm::dbgs() << "    handle non-native entry: " << *entry);
    assert(!entry->isNativeTypeChecked);
    
    // Propagate the false-value to the dependent entries.
    for (Entry *depEntry : entry->Dependencies) {
      setNotNative(depEntry);
    }
  }
}

/// Replaces all get-property calls, which we can prove to be true, with
/// true-literals.
void GlobalPropertyOpt::replacePropertyCalls() {
  for (ApplyInst *AI : propertyCalls) {
    SILFunction *F = AI->getFunction();
    // Don't optimize functions that are marked with the opt.never attribute.
    if (!F->shouldOptimize())
      continue;

    ChangedFunctions.insert(F);

    SILValue array = AI->getArgument(0);
    
    // Is the argument a native swift array?
    if (ValueEntries.count(array) != 0 &&
        getValueEntry(array)->isNativeTypeChecked) {
      
      ArraySemanticsCall semCall(AI);
      assert(
        (semCall.getKind() == ArrayCallKind::kArrayPropsIsNativeTypeChecked) &&
             "invalid semantics type");
  
      LLVM_DEBUG(llvm::dbgs() << "  remove property check in function "
                              << AI->getParent()->getParent()->getName()
                              << ": " << *AI);
      SILBuilder B(AI);
      SILType IntBoolTy = SILType::getBuiltinIntegerType(1, B.getASTContext());
      auto C1 = B.createIntegerLiteral(AI->getLoc(), IntBoolTy, 1);
      auto TrueStruct = B.createStruct(AI->getLoc(), AI->getType(), {C1});
      AI->replaceAllUsesWith(TrueStruct);
      
      semCall.removeCall();
      ++NumPropertiesReplaced;
    }
  }
}
 
/// The main entry point to the optimization.
void GlobalPropertyOpt::run(SILModuleTransform *T) {
  
  assert(WorkList.empty());
  assert(FieldEntries.empty() && ValueEntries.empty());
  
  ArrayType = M.getASTContext().getArrayDecl();
  
  // Step 1: scan the whole module and build the dependency graph.
  scanInstructions();
  
  // Step 2: propagate the flags through the dependency graph.
  propagatePropertiesInGraph();

  // Step 3: replace get-property calls with literals.
  replacePropertyCalls();

  for (SILFunction *ChangedFn : ChangedFunctions) {
    T->invalidateAnalysis(ChangedFn,
                          SILAnalysis::InvalidationKind::CallsAndInstructions);
  }
}

/// The module pass, which runs the optimization.
class GlobalPropertyOptPass : public SILModuleTransform {
  
  void run() override {
    SILModule *M = getModule();
    
    LLVM_DEBUG(llvm::dbgs() << "** GlobalPropertyOpt **\n");
    
    GlobalPropertyOpt(*M).run(this);
  }
  
};

} // end anonymous namespace

SILTransform *swift::createGlobalPropertyOpt() {
  return new GlobalPropertyOptPass();
}
