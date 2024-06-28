//===--- LetPropertiesOpts.cpp - Optimize let properties ------------------===//
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
// Promote values of non-static let properties initialized by means
// of constant values of simple types into their uses.
//
// For any given non-static let property this optimization is only possible
// if this pass can prove that it has analyzed all assignments of an initial
// value to this property and all those assignments assign the same value
// to this property.
//
// FIXME:
//
// This pass makes assumptions about the visibility of a type's memory
// based on the visibility of its properties. This is the wrong way to think
// about memory visibility.
//
// This pass wants assume that the contents of a property is known based on
// whether the property is declared as a 'let' and the visibility of the
// initializers that access the property. For example:
//
// public struct X<T> {
//   public let hidden: T
//
//   init(t: T) { self.hidden = t }
// }
//
// The pass currently assumes that `X` only takes on values that are
// assigned by the invocations of `X.init`, which is only visible in `X`s
// module. This is wrong if the layout of `Impl` is exposed to other
// modules. A struct's memory may be initialized by any module with
// access to the struct's layout.
// 
// In fact, this assumption is wrong even if the struct, and it's let
// property cannot be accessed externally by name. In this next example,
// external modules cannot access `Impl` or `Impl.hidden` by name, but
// can still access the memory because the layout is exposed via a public type
// that contains it.
// 
// ```
// internal struct Impl<T> {
//   let hidden: T
// 
//   init(t: T) { self.hidden = t }
// }
// 
// public struct Wrapper<T> {
//   var impl: Impl<T>
//   
//   public var property: T {
//     get {
//       return impl.hidden
//     }
//   }
// }
// ```
// 
// As long as `Wrapper`s layout is exposed to other modules, the contents of
// `Wrapper`, `Impl`, and `hidden' can all be initialized in another
// module. This following code is legal if Wrapper's home module is *not*
// built with library evolution (or if Wrapper is declared `@frozen`).
// 
// func inExternalModule(buffer: UnsafeRawPointer) -> Wrapper<Int64> {
//   return buffer.load(as: Wrapper<Int64>.self)
// }
// 
// If library evolution is enabled and a `public` struct is not declared
// `@frozen` then external modules cannot assume its layout, and therefore
// cannot initialize the struct memory. In that case, it is possible to optimize
// `X.hidden` and `Impl.hidden` as if the properties are only initialized inside
// their home module.
// 
// The right way to view a type's memory visibility is to consider whether
// external modules have access to the layout of the type. If not, then the
// property can still be optimized As long as a struct is never enclosed in a
// public effectively-`@frozen` type. However, finding all places where a struct
// is explicitly created is still insufficient. Instead, the optimization needs
// to find all uses of enclosing types and determine if every use has a known
// constant initialization, or is simply copied from another value. If an
// escaping unsafe pointer to any enclosing type is created, then the
// optimization is not valid.
// 
// When viewed this way, the fact that a property is declared 'let' is mostly
// irrelevant to this optimization--it can be expanded to handle non-'let'
// properties. The more salient feature is whether the property has a public
// setter.
//
// For now, this optimization only recognizes class properties because class
// properties are only accessibly via a ref_element_addr instruction. This is a
// side effect of the fact that accessing a class property requires a "formal
// access". This means that begin_access marker must be emitted directly on the
// address produced by a ref_element_addr. Struct properties are not handled, as
// explained above, because they can be indirectly accessed via addresses of
// outer types.
//
// Note: Propagating the initialized constants of non-addressable aggregate
// values (formation of 'struct's and 'tuple's) is a significantly different
// problem. It can be done better in a separate constant-propagation pass that
// propagates partial-constants into call arguments and out of returned values.
// ===---------------------------------------------------------------------===//

#define DEBUG_TYPE "let-properties-opt"
#include "swift/Basic/Assertions.h"
#include "swift/SIL/DebugUtils.h"
#include "swift/SIL/InstructionUtils.h"
#include "swift/SIL/MemAccessUtils.h"
#include "swift/SIL/SILBasicBlock.h"
#include "swift/SIL/SILInstruction.h"
#include "swift/SIL/SILLinkage.h"
#include "swift/SILOptimizer/PassManager/Passes.h"
#include "swift/SILOptimizer/PassManager/Transforms.h"
#include "swift/SILOptimizer/Utils/BasicBlockOptUtils.h"
#include "swift/SILOptimizer/Utils/InstructionDeleter.h"
#include "llvm/ADT/MapVector.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Debug.h"
using namespace swift;

namespace {

using InstructionList = SmallVector<SILInstruction *, 8>;

struct InitSequence {
  InstructionList Instructions;
  SILValue Result;

  bool isValid() const {
    return (bool) Result;
  }
};

/// Promote values of non-static let properties initialized by means
/// of constant values of simple types into their uses.
///
/// TODO: Don't occupy any storage for such let properties with constant
/// initializers.
///
/// Note: Storage from a 'let' property can only be removed if this property if
/// the type is resilient (not fixed-layout) and the property cannot be read
/// from another module.
class LetPropertiesOpt {
  SILModule *Module;

  typedef SmallVector<VarDecl *, 4> Properties;

  llvm::SetVector<SILFunction *> ChangedFunctions;

  // Map each let property to a set of instructions accessing it.
  llvm::MapVector<VarDecl *, InstructionList> AccessMap;
  // Map each let property to the instruction sequence which initializes it.
  llvm::MapVector<VarDecl *, InitSequence> InitMap;
  // Properties in this set should not be processed by this pass
  // anymore.
  llvm::SmallPtrSet<VarDecl *, 16> SkipProcessing;
  // Types in this set should not be processed by this pass
  // anymore.
  llvm::SmallPtrSet<NominalTypeDecl *, 16> SkipTypeProcessing;
  // Properties in this set cannot be removed.
  llvm::SmallPtrSet<VarDecl *, 16> CannotRemove;
  // Set of let properties in a given nominal type.
  llvm::MapVector<NominalTypeDecl *, Properties> NominalTypeLetProperties;
  // Set of properties which already fulfill all conditions, except
  // the available of constant, statically known initializer.
  llvm::SmallPtrSet<VarDecl *, 16> PotentialConstantLetProperty;

public:
  LetPropertiesOpt(SILModule *M): Module(M) {}

  void run(SILModuleTransform *T);

protected:
  bool isConstantLetProperty(VarDecl *Property);
  void collectPropertyAccess(SingleValueInstruction *I, VarDecl *Property,
                             bool NonRemovable);
  void optimizeLetPropertyAccess(VarDecl *SILG, const InitSequence &Init);
  bool analyzeInitValue(SILInstruction *I, VarDecl *Prop);
};

/// Helper class to copy only a set of SIL instructions providing in the
/// constructor.
class InitSequenceCloner : public SILClonerWithScopes<InitSequenceCloner> {
  friend class SILInstructionVisitor<InitSequenceCloner>;
  friend class SILCloner<InitSequenceCloner>;

  const InitSequence &Init;

public:
  InitSequenceCloner(const InitSequence &init, SILInstruction *destIP)
    : SILClonerWithScopes(*destIP->getFunction()), Init(init) {
    Builder.setInsertionPoint(destIP);
  }

  void process(SILInstruction *I) { visit(I); }

  SILBasicBlock *remapBasicBlock(SILBasicBlock *BB) { return BB; }

  SILValue getMappedValue(SILValue Value) {
    return SILCloner<InitSequenceCloner>::getMappedValue(Value);
  }

  /// Clone all the instructions from Insns into the destination function,
  /// immediately before the destination block, and return the value of
  /// the result.
  SILValue clone() {
    for (auto I : Init.Instructions)
      process(I);
    return getMappedValue(Init.Result);
  }
};

} // end anonymous namespace

#ifndef NDEBUG
// For debugging only.
static raw_ostream &operator<<(raw_ostream &OS, const VarDecl &decl) {
  auto *Ty = dyn_cast<NominalTypeDecl>(decl.getDeclContext());
  if (Ty)
    OS << Ty->getName() << "::";
  OS << decl.getName();
  return OS;
}
#endif

/// Optimize access to the let property, which is known
/// to have a constant value. Replace all loads from the
/// property by its constant value.
void LetPropertiesOpt::optimizeLetPropertyAccess(VarDecl *Property,
                                                 const InitSequence &init) {
  assert(init.isValid());

  if (SkipProcessing.count(Property))
    return;

  auto *Ty = dyn_cast<NominalTypeDecl>(Property->getDeclContext());
  // Ty is null for properties declared inside an extension of an ObjC type.
  if (!Ty || SkipTypeProcessing.count(Ty))
    return;

  LLVM_DEBUG(llvm::dbgs() << "Replacing access to property '" << *Property
                          << "' by its constant initializer\n");

  auto PropertyAccess = Property->getEffectiveAccess();
  auto TypeAccess = Ty->getEffectiveAccess();
  auto CanRemove = false;

  // Check if a given let property can be removed, because it
  // is not accessible elsewhere. This can happen if this property
  // is private or if it is internal and WMO mode is used.
  if (TypeAccess <= AccessLevel::FilePrivate ||
      PropertyAccess <= AccessLevel::FilePrivate
      || ((TypeAccess <= AccessLevel::Internal ||
          PropertyAccess <= AccessLevel::Internal) &&
          Module->isWholeModule())) {
    CanRemove = true;
    LLVM_DEBUG(llvm::dbgs() << "Storage for property '" << *Property
                            << "' can be eliminated\n");
  }

  if (CannotRemove.count(Property))
    CanRemove = false;

  if (!AccessMap.count(Property)) {
    LLVM_DEBUG(llvm::dbgs() << "Property '" << *Property <<"' is never read\n");
    if (CanRemove) {
      // TODO: Remove the let property, because it is never accessed.
    }
    return;
  }

  InstructionDeleter deleter;

  auto &Loads = AccessMap[Property];

  unsigned NumReplaced = 0;

  for (auto Load: Loads) {
    SILFunction *F = Load->getFunction();

    // A helper function to copy the initializer into the target function
    // at the target insertion point.
    auto cloneInitAt = [&](SILInstruction *insertionPoint) -> SILValue {
      InitSequenceCloner cloner(init, insertionPoint);
      return cloner.clone();
    };

    // Look for any instructions accessing let properties.
    auto *proj = cast<RefElementAddrInst>(Load);

    // Copy the initializer into the function
    // Replace the access to a let property by the value
    // computed by this initializer.
    SILValue clonedInit = cloneInitAt(proj);
    for (auto UI = proj->use_begin(), E = proj->use_end(); UI != E;) {
      auto *User = UI->getUser();
      ++UI;

      if (!canReplaceLoadSequence(User))
        continue;

      replaceLoadSequence(User, clonedInit);
      deleter.forceDeleteWithUsers(User);
      ++NumReplaced;
    }
    ChangedFunctions.insert(F);
  }
  deleter.cleanupDeadInstructions();

  LLVM_DEBUG(llvm::dbgs() << "Access to " << *Property << " was replaced "
                          << NumReplaced << " time(s)\n");

  if (CanRemove) {
    // TODO: Remove the let property, because it is never accessed.
  }
}

/// Compare two SILValues structurally.
static bool isStructurallyIdentical(SILValue LHS, SILValue RHS) {
  if (LHS == RHS)
    return true;

  if (LHS->getType() != RHS->getType())
    return false;

  auto lResult = LHS->getDefiningInstructionResult();
  auto rResult = RHS->getDefiningInstructionResult();
  assert(lResult && rResult &&
         "operands of instructions approved by analyzeStaticInitializer "
         "should always be defined by instructions");
  return (lResult->ResultIndex == rResult->ResultIndex &&
          lResult->Instruction->isIdenticalTo(rResult->Instruction,
                                              isStructurallyIdentical));
}

/// Compare two sequences of SIL instructions. They should be structurally
/// equivalent.
static bool isSameInitSequence(const InitSequence &LHS,
                               const InitSequence &RHS) {
  assert(LHS.isValid() && RHS.isValid());
  // This will recursively check all the instructions.  It's possible
  // that they'll be composed slightly differently, but it shouldn't matter.
  return isStructurallyIdentical(LHS.Result, RHS.Result);
}

/// Check if a given let property can be assigned externally.
static bool isAssignableExternally(VarDecl *Property, SILModule *Module) {
  if (Module->isVisibleExternally(Property)) {
    // If at least one of the properties of the enclosing type cannot be
    // used externally, then no initializer can be implemented externally as
    // it wouldn't be able to initialize such a property.
    // More over, for classes, only the class itself can initialize its
    // let properties. Subclasses and extensions cannot do it.
    // For structs, external extensions may initialize let properties. But to do
    // that they need to be able to initialize all properties, i.e. all
    // properties should be accessible by the extension.

    auto *Ty = dyn_cast<NominalTypeDecl>(Property->getDeclContext());

    // Check for "unusual" decl contexts, e.g. ObjC extensions.
    if (!Ty)
      return true;

    // Initializer for a let property of a class cannot exist externally.
    // It cannot be defined by an extension or a derived class.
    if (isa<ClassDecl>(Ty))
      return false;

    // Check if there are any private properties or any internal properties and
    // it is a whole module compilation. In this case, no external initializer
    // may exist.
    for (auto SP : Ty->getStoredProperties()) {
      auto storedPropertyAccess = SP->getEffectiveAccess();
      if (storedPropertyAccess <= AccessLevel::FilePrivate ||
          (storedPropertyAccess <= AccessLevel::Internal &&
           Module->isWholeModule())) {
       LLVM_DEBUG(llvm::dbgs() << "Property " << *Property
                               << " cannot be set externally\n");
       return false;
      }
    }

    LLVM_DEBUG(llvm::dbgs() << "Property " << *Property
                            << " can be used externally\n");
    return true;
  }

  return false;
}

// Checks if a given property may have any unknown uses which cannot
// be analyzed by this pass.
static bool mayHaveUnknownUses(VarDecl *Property, SILModule *Module) {
  if (Property->getDeclContext()->getParentModule() !=
      Module->getSwiftModule()) {
    LLVM_DEBUG(llvm::dbgs() << "Property " << *Property
                            << " is defined in a different module\n");
    // We don't see the bodies of initializers from a different module
    // unless all of them are fragile.
    // TODO: Support fragile initializers.
    return true;
  }

  // If let properties can be assigned externally, we don't know
  // the values they may get.
  if (isAssignableExternally(Property, Module)) {
    return true;
  }

  return false;
}


/// Check if a given property is a non-static let property
/// with known constant value.
bool LetPropertiesOpt::isConstantLetProperty(VarDecl *Property) {
  // Process only non-static let properties here.
  if (!Property->isLet() || Property->isStatic())
    return false;

  // Do not re-process already known properties.
  if (SkipProcessing.count(Property))
    return false;

  // If these checks were performed already, no need to
  // repeat them.
  if (PotentialConstantLetProperty.count(Property))
    return true;

  // Check the visibility of this property. If its visibility
  // implies that this optimization pass cannot analyze all uses,
  // don't process it.
  if (mayHaveUnknownUses(Property, Module)) {
    LLVM_DEBUG(llvm::dbgs() << "Property '" << *Property
                            << "' may have unknown uses\n");
    SkipProcessing.insert(Property);
    return false;
  }

  LLVM_DEBUG(llvm::dbgs() << "Property '" << *Property
                          << "' has no unknown uses\n");

  PotentialConstantLetProperty.insert(Property);

  return true;
}

static bool isProjectionOfProperty(SILValue addr, VarDecl *Property) {
  addr = stripAccessMarkers(addr);
  if (auto *REA = dyn_cast<RefElementAddrInst>(addr)) {
    return REA->getField() == Property;
  }
  return false;
}

// Analyze the init value being stored by the instruction into a property.
bool
LetPropertiesOpt::analyzeInitValue(SILInstruction *I, VarDecl *Property) {
  SILValue value;
  SILValue dest;
  if (auto SI = dyn_cast<StoreInst>(I)) {
    dest = stripAccessMarkers(SI->getDest());
    value = SI->getSrc();
  } else if (auto *copyAddr = dyn_cast<CopyAddrInst>(I)) {
    dest = stripAccessMarkers(copyAddr->getDest());
    value = copyAddr->getSrc();
  } else {
    return false;
  }
  assert(isProjectionOfProperty(dest, Property)
         && "Store instruction should store into a proper let property");
  (void)dest;
  // Check if it's just a copy from another instance of the struct.
  if (auto *LI = dyn_cast<LoadInst>(value)) {
    SILValue addr = LI->getOperand();
    if (isProjectionOfProperty(addr, Property))
      return true;
  }

  // Bail if a value of a property is not a statically known constant init.
  InitSequence sequence;
  sequence.Result = value;
  if (!analyzeStaticInitializer(value, sequence.Instructions))
    return false;

  auto &cachedSequence = InitMap[Property];
  if (cachedSequence.isValid() &&
      !isSameInitSequence(cachedSequence, sequence)) {
    // The found init value is different from the already seen init value.
    return false;
  } else {
    LLVM_DEBUG(llvm::dbgs() << "The value of property '" << *Property
                            << "' is statically known so far\n");
    // Remember the statically known value.
    cachedSequence = std::move(sequence);
    return true;
  }
}

/// Check if I is a sequence of projections followed by a load.
/// Since it is supposed to be a load from a let property with
/// statically known constant initializer, only struct_element_addr
/// and tuple_element_addr projections are considered.
static bool isValidPropertyLoad(SILInstruction *I) {
  if (isa<LoadInst>(I))
    return true;

  if (isa<StructElementAddrInst>(I) || isa<TupleElementAddrInst>(I)
      || isa<BeginAccessInst>(I)) {
    auto projection = cast<SingleValueInstruction>(I);
    for (auto Use : getNonDebugUses(projection)) {
      if (isIncidentalUse(Use->getUser()))
        continue;
      if (!isValidPropertyLoad(Use->getUser()))
        return false;
    }
    return true;
  }

  return false;
}


/// Remember where this property is accessed.
void LetPropertiesOpt::collectPropertyAccess(SingleValueInstruction *I,
                                             VarDecl *Property,
                                             bool NonRemovable) {
  if (!isConstantLetProperty(Property))
    return;

  LLVM_DEBUG(llvm::dbgs() << "Collecting property access for property '"
                          << *Property << "':\n";
             llvm::dbgs() << "The instructions are:\n"; I->dumpInContext());

  // Ignore the possibility of duplicate worklist entries. They cannot effect
  // the SkipProcessing result, and we don't expect any exponential path
  // explosion because none of the instructions have multiple address operands.
  SmallVector<SingleValueInstruction *, 8> worklist = {I};
  while (!worklist.empty()) {
    // Check if there is a store to this property.
    auto *projection = worklist.pop_back_val();
    for (auto Use : getNonDebugUses(projection)) {
      auto *User = Use->getUser();
      if (isIncidentalUse(User)) {
        continue;
      }
      if (auto *bai = dyn_cast<BeginAccessInst>(User)) {
        worklist.push_back(bai);
        continue;
      }
      if (auto *copyAddr = dyn_cast<CopyAddrInst>(User)) {
        if (copyAddr->getDest() != projection ||
            !analyzeInitValue(copyAddr, Property)) {
          SkipProcessing.insert(Property);
          return;
        }
        continue;
      }

      if (auto *SI = dyn_cast<StoreInst>(User)) {
        // There is a store into this property.
        // Analyze the assigned value and check if it is a constant
        // statically known initializer.
        if (SI->getDest() != projection || !analyzeInitValue(SI, Property)) {
          SkipProcessing.insert(Property);
          return;
        }
        continue;
      }

      // Follow the chain of projections and check if it ends up with a load.
      // If this is not the case, it is potentially a store into sub-property
      // of a property.
      // We cannot handle such cases yet, so bail.
      if (!isValidPropertyLoad(User)) {
        SkipProcessing.insert(Property);
        return;
      }
    }
  }

  AccessMap[Property].push_back(I);
  // If any property is marked as non-removable, their initialization
  // and storage cannot be completely removed. But their constant
  // values can still be propagated into their uses whenever possible.
  if (NonRemovable)
    CannotRemove.insert(Property);
}

void LetPropertiesOpt::run(SILModuleTransform *T) {
  // Collect property access information for the whole module.
  for (auto &F : *Module) {
    // Take into account even those functions that should not be
    // optimized, because they may contain access to the let
    // properties.
    bool NonRemovable = !F.shouldOptimize();

    for (auto &BB : F) {
      for (auto &I : BB) {
        if (auto *REAI = dyn_cast<RefElementAddrInst>(&I))
          collectPropertyAccess(REAI, REAI->getField(), NonRemovable);
      }
    }
  }

  for (auto &Init: InitMap) {
    optimizeLetPropertyAccess(Init.first, Init.second);
  }

  for (SILFunction *ChangedFn : ChangedFunctions) {
    // Program flow is not changed by this pass.
    T->invalidateAnalysis(ChangedFn,
                          SILAnalysis::InvalidationKind::Instructions);
  }
}

namespace {
class LetPropertiesOptPass : public SILModuleTransform
{
  void run() override {
    LetPropertiesOpt(getModule()).run(this);
  }

};
} // end anonymous namespace

SILTransform *swift::createLetPropertiesOpt() {
  return new LetPropertiesOptPass();
}
