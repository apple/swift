//===--- DIMemoryUseCollector.h -------------------------------------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2018 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//
//
// This file declares logic used by definitive analysis related passes that look
// at all the instructions that access a memory object.  This is quite specific
// to definitive analysis in that it is tuple element sensitive instead of
// relying on SROA.
//
//===----------------------------------------------------------------------===//

#ifndef SWIFT_SILOPTIMIZER_PASSMANAGER_DIMEMORYUSECOLLECTOR_H
#define SWIFT_SILOPTIMIZER_PASSMANAGER_DIMEMORYUSECOLLECTOR_H

#include "swift/Basic/LLVM.h"
#include "swift/SIL/SILInstruction.h"
#include "swift/SIL/SILType.h"
#include "llvm/ADT/APInt.h"
#include "llvm/ADT/TinyPtrVector.h"

namespace swift {

class SILBuilder;

namespace ownership {

struct DIElementUseInfo;

/// This struct holds information about the memory object being analyzed that is
/// required to correctly break it down into elements.
///
/// This includes a collection of utilities for reasoning about (potentially
/// recursively) exploded aggregate elements, and computing access paths and
/// indexes into the flattened namespace.
///
/// The flattened namespace is assigned lexicographically.  For example, in:
///   (Int, ((Float, (), Double)))
/// the Int member is numbered 0, the Float is numbered 1, and the Double is
/// numbered 2.  Empty tuples don't get numbered since they contain no state.
///
/// Structs and classes have their elements exploded when we are analyzing the
/// 'self' member in an initializer for the aggregate.
///
/// Derived classes have an additional field at the end that models whether or
/// not super.init() has been called or not.
class DIMemoryObjectInfo {
  /// The uninitialized memory that we are analyzing.
  MarkUninitializedInst *MemoryInst;

  /// This is the base type of the memory allocation.
  SILType MemorySILType;

  /// This is the count of elements being analyzed.  For memory objects that are
  /// tuples, this is the flattened element count.  For 'self' members in init
  /// methods, this is the local field count (+1 for super/self classes were
  /// initialized).
  unsigned NumElements;

  /// True if the memory object being analyzed represents a 'let', which is
  /// initialize-only (reassignments are not allowed).
  bool IsLet = false;

  /// True if NumElements has a dummy value in it to force a struct to be
  /// non-empty.
  bool HasDummyElement = false;

  /// True if this object has a single user of type ProjectBoxInst.
  bool IsBox = false;

public:
  DIMemoryObjectInfo(MarkUninitializedInst *MemoryInst);

  SILLocation getLoc() const { return MemoryInst->getLoc(); }
  SILFunction &getFunction() const { return *MemoryInst->getFunction(); }
  SILModule &getModule() const { return MemoryInst->getModule(); }
  SILBasicBlock *getParentBlock() const { return MemoryInst->getParent(); }

  /// Return the first instruction of the function containing the memory object.
  SILInstruction *getFunctionEntryPoint() const;

  CanType getASTType() const { return MemorySILType.getASTType(); }
  SILType getType() const { return MemorySILType; }

  /// Returns true if this memory object is of trivial type.
  bool hasTrivialType() const { return MemorySILType.isTrivial(getFunction()); }

  /// Returns true if NumElements has a dummy value in it to force a struct to
  /// be non-empty.
  bool hasDummyElement() const { return HasDummyElement; }

  /// Return the actual 'uninitialized' memory. In the case of alloc_ref,
  /// alloc_stack, this always just returns the actual mark_uninitialized
  /// instruction. For alloc_box though it returns the project_box associated
  /// with the memory info.
  SingleValueInstruction *getUninitializedValue() const;

  /// Return the number of elements, without the extra "super.init" tracker in
  /// initializers of derived classes.
  unsigned getNumMemoryElements() const {
    return NumElements - (unsigned)isDerivedClassSelf();
  }

  /// Return the number of elements, including the extra "super.init" tracker in
  /// initializers of derived classes.
  ///
  /// \see getNumMemoryElements() for the number of elements, excluding the
  /// extra "super.init" tracker in the initializers of derived classes.
  unsigned getNumElements() const { return NumElements; }

  /// Return true if this is 'self' in any kind of initializer.
  bool isAnyInitSelf() const {
    return !MemoryInst->isVar() && !MemoryInst->isOut();
  }

  /// Return uninitialized value of 'self' if current memory object
  /// is located in an initializer (of any kind).
  SingleValueInstruction *findUninitializedSelfValue() const;

  /// True if the memory object is the 'self' argument of a struct initializer.
  bool isStructInitSelf() const {
    if (MemoryInst->isRootSelf() || MemoryInst->isCrossModuleRootSelf()) {
      if (auto decl = getASTType()->getAnyNominal()) {
        if (isa<StructDecl>(decl)) {
          return true;
        }
      }
    }
    return false;
  }

  /// True if the memory object is the 'self' argument of a non-delegating
  /// cross-module struct initializer.
  bool isCrossModuleStructInitSelf() const {
    if (MemoryInst->isCrossModuleRootSelf()) {
      assert(isStructInitSelf());
      return true;
    }
    return false;
  }

  /// True if the memory object is the 'self' argument of a class designated
  /// initializer.
  bool isClassInitSelf() const {
    if (MemoryInst->isDelegatingSelf())
      return false;

    if (!MemoryInst->isVar() && !MemoryInst->isOut()) {
      if (auto decl = getASTType()->getAnyNominal()) {
        if (isa<ClassDecl>(decl)) {
          return true;
        }
      }
    }
    return false;
  }

  /// Returns the initializer if the memory use is 'self' and appears in an
  /// actor's initializer. Otherwise, returns nullptr.
  ConstructorDecl *getActorInitSelf() const;

  /// True if this memory object is the 'self' of a derived class initializer.
  bool isDerivedClassSelf() const { return MemoryInst->isDerivedClassSelf(); }

  /// True if this memory object is the 'self' of a derived class initializer for
  /// which we can assume that all ivars have been initialized.
  bool isDerivedClassSelfOnly() const {
    return MemoryInst->isDerivedClassSelfOnly();
  }

  /// True if this memory object is the 'self' of a derived class init method,
  /// regardless of whether we're tracking ivar initializations or not.
  bool isAnyDerivedClassSelf() const {
    return MemoryInst->isDerivedClassSelf() ||
           MemoryInst->isDerivedClassSelfOnly();
  }

  /// True if this memory object is the 'self' of a root class init method.
  bool isRootClassSelf() const {
    return isClassInitSelf() && MemoryInst->isRootSelf();
  }

  /// True if this memory object is the 'self' of a non-root class init method.
  bool isNonRootClassSelf() const {
    return isClassInitSelf() && !MemoryInst->isRootSelf();
  }

  /// True if this is a delegating initializer, one that calls 'self.init'.
  bool isDelegatingInit() const {
    return MemoryInst->isDelegatingSelf() ||
           MemoryInst->isDelegatingSelfAllocated();
  }

  /// True if this is an initializer that initializes stored properties.
  bool isNonDelegatingInit() const {
    switch (MemoryInst->getMarkUninitializedKind()) {
    case MarkUninitializedInst::Var:
    case MarkUninitializedInst::Out:
      return false;
    case MarkUninitializedInst::RootSelf:
    case MarkUninitializedInst::CrossModuleRootSelf:
    case MarkUninitializedInst::DerivedSelf:
    case MarkUninitializedInst::DerivedSelfOnly:
      return true;
    case MarkUninitializedInst::DelegatingSelf:
    case MarkUninitializedInst::DelegatingSelfAllocated:
      return false;
    }
    return false;
  }

  bool isRootSelf() const {
    return MemoryInst->getMarkUninitializedKind() ==
           MarkUninitializedInst::RootSelf;
  }

  bool isDelegatingSelfAllocated() const {
    return MemoryInst->isDelegatingSelfAllocated();
  }

  bool isOut() const { return MemoryInst->isOut(); }

  enum class EndScopeKind { Borrow, Access };

  /// Given an element number (in the flattened sense) return a pointer to a
  /// leaf element of the specified number.
  SILValue emitElementAddressForDestroy(
      unsigned TupleEltNo, SILLocation Loc, SILBuilder &B,
      SmallVectorImpl<std::pair<SILValue, EndScopeKind>> &EndScopeList) const;

  /// Return the swift type of the specified element.
  SILType getElementType(unsigned EltNo) const;

  /// Push the symbolic path name to the specified element number onto the
  /// specified std::string.  If the actual decl (or a subelement thereof) can
  /// be determined, return it.  Otherwise, return null.
  ValueDecl *getPathStringToElement(unsigned Element,
                                    std::string &Result) const;

  /// If the specified value is a 'let' property in an initializer, return true.
  bool isElementLetProperty(unsigned Element) const;
};

enum DIUseKind {
  /// The instruction is a Load.
  Load,

  /// The instruction is either an initialization or an assignment, we don't
  /// know which.  This classification only happens with values of trivial type
  /// where the different isn't significant.
  InitOrAssign,

  /// The instruction is an initialization of the tuple element.
  Initialization,

  /// The instruction is an assignment, overwriting an already initialized
  /// value.
  Assign,

  /// The instruction is a setter call for a computed property after all of
  /// self is initialized. This is used for property wrappers and for init
  /// accessors.
  Set,

  /// The instruction is a store to a member of a larger struct value.
  PartialStore,

  /// This instruction is an init, assignment, or store to a
  /// @_compilerInitialized field that was _not_ automatically generated
  BadExplicitStore,

  /// An 'inout' argument of a function application.
  InOutArgument,

  /// An 'inout' self argument of a function application.
  InOutSelfArgument,

  /// An indirect 'in' parameter of an Apply instruction.
  IndirectIn,

  /// This instruction is a general escape of the value, e.g. a call to a
  /// closure that captures it.
  Escape,

  /// This instruction is a call to 'self.init' in a delegating initializer,
  /// or a call to 'super.init' in a designated initializer of a derived class..
  SelfInit,
  
  /// This instruction is a load that's only used to answer a `type(of: self)`
  /// question.
  LoadForTypeOfSelf,

  /// This instruction is a value_metatype on the address of 'self'.
  TypeOfSelf
};

/// This struct represents a single classified access to the memory object
/// being analyzed, along with classification information about the access.
struct DIMemoryUse {
  /// This is the instruction accessing the memory.
  SILInstruction *Inst;

  /// This is what kind of access it is, load, store, escape, etc.
  DIUseKind Kind;

  /// For memory objects of (potentially recursive) tuple type, this keeps
  /// track of which tuple elements are affected.
  unsigned FirstElement, NumElements;

  NullablePtr<VarDecl> Field;

  DIMemoryUse(SILInstruction *Inst, DIUseKind Kind, unsigned FE, unsigned NE,
              NullablePtr<VarDecl> Field = 0)
      : Inst(Inst), Kind(Kind), FirstElement(FE), NumElements(NE),
        Field(Field) {}

  DIMemoryUse() : Inst(nullptr) {}

  bool isInvalid() const { return Inst == nullptr; }
  bool isValid() const { return Inst != nullptr; }

  bool usesElement(unsigned i) const {
    return i >= FirstElement &&
           i < static_cast<unsigned>(FirstElement + NumElements);
  }

  /// onlyTouchesTrivialElements - Return true if all of the accessed elements
  /// have trivial type and the access itself is a trivial instruction.
  bool onlyTouchesTrivialElements(const DIMemoryObjectInfo &MemoryInfo) const;

  /// getElementBitmask - Return a bitmask with the touched tuple elements
  /// set.
  APInt getElementBitmask(unsigned NumMemoryTupleElements) const {
    return APInt::getBitsSet(NumMemoryTupleElements, FirstElement,
                             FirstElement + NumElements);
  }
};

struct DIElementUseInfo {
  SmallVector<DIMemoryUse, 16> Uses;
  SmallVector<SILInstruction *, 4> Releases;
  TinyPtrVector<SILInstruction *> StoresToSelf;

  void trackUse(DIMemoryUse Use) { Uses.push_back(Use); }

  void trackDestroy(SILInstruction *Destroy) { Releases.push_back(Destroy); }

  void trackStoreToSelf(SILInstruction *I);
};

/// collectDIElementUsesFrom - Analyze all uses of the specified allocation
/// instruction (alloc_box, alloc_stack or mark_uninitialized), classifying them
/// and storing the information found into the Uses and Releases lists.
void collectDIElementUsesFrom(const DIMemoryObjectInfo &MemoryInfo,
                              DIElementUseInfo &UseInfo);

} // end namespace ownership
} // end namespace swift

#endif
