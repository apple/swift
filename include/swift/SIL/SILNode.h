//===--- SILNode.h - Node base class for SIL --------------------*- C++ -*-===//
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
// This file defines the SILNode class.
//
//===----------------------------------------------------------------------===//

#ifndef SWIFT_SIL_SILNODE_H
#define SWIFT_SIL_SILNODE_H

#include "llvm/Support/Compiler.h"
#include "llvm/Support/PointerLikeTypeTraits.h"
#include "swift/Basic/LLVM.h"
#include "swift/Basic/SwiftObjectHeader.h"
#include <type_traits>

namespace swift {

class SILBasicBlock;
class SILFunction;
class SILInstruction;
class SingleValueInstruction;
class NonSingleValueInstruction;
class SILModule;
class ValueBase;
class SILNode;
class SILValue;

/// An enumeration which contains values for all the nodes in SILNodes.def.
/// Other enumerators, like ValueKind and SILInstructionKind, ultimately
/// take their values from this enumerator.
enum class SILNodeKind {
#define NODE(ID, PARENT) \
  ID,
#define NODE_RANGE(ID, FIRST, LAST) \
  First_##ID = FIRST, \
  Last_##ID = LAST,
#include "swift/SIL/SILNodes.def"
};

enum class SILInstructionKind : std::underlying_type<SILNodeKind>::type;

/// A SILNode pointer which makes it possible to implicitly cast from all kind
/// of nodes, values and instructions (note: there is no implicit cast from
/// SILInstruction* to SILNode*).
/// It's mainly used to simplify classof-functions, but it can be used for other
/// SILNode-taking APIs, too.
/// Currently there is only a const-version of it.
class SILNodePointer {
  const SILNode *node;
public:
  SILNodePointer(const SILNode *node) : node(node) { }
  SILNodePointer(const SILInstruction *inst);
  SILNodePointer(const SingleValueInstruction *svi);
  SILNodePointer(const NonSingleValueInstruction *nsvi);
  SILNodePointer(SILValue value);
  
  const SILNode *get() const { return node; }
  const SILNode *operator->() const { return node; }
  operator const SILNode *() const { return node; }
};

/// A SILNode is a node in the use-def graph of a SILFunction.  It is
/// either an instruction or a defined value which can be used by an
/// instruction.  A defined value may be an instruction result, a basic
/// block argument, or the special 'undef' value.
///
/// The 'node' intuition is slightly imprecise because a single instruction
/// may be composed of multiple SILNodes: one for the instruction itself
/// and one for each value it produces.  When an instruction kind always
/// produces exactly one value, the cast machinery (isa, cast, and dyn_cast)
/// works to make both nodes appear to be the same object: there is a value
/// kind exactly equal to the instruction kind and the value node can be
/// directly cast to the instruction's class.  When an instruction kind
/// never produces values, it has no corresponding value kind, and it is
/// a compile-time error to attempt to cast a value node to the instruction
/// class.  When an instruction kind can have multiple values (not yet
/// implemented), its value nodes have a different kind from the
/// instruction kind and it is a static error to attempt to cast a value
/// node to the instruction kind.
///
/// Another way of interpreting SILNode is that there is a SILNode for
/// everything that can be numbered in SIL assembly (plus 'undef', which
/// is not conventionally numbered).  Instructions without results are
/// still numbered in SIL in order to describe the users lists of an
/// instruction or argument.  Instructions with multiple results are
/// numbered using their first result.
///
/// SILNode is a base class of both SILInstruction and ValueBase.
/// Because there can be multiple SILNodes within a single instruction
/// object, some care must be taken when working with SILNode pointers.
/// These precautions only apply to SILNode* and not its subclasses.
///
/// - There may have multiple SILNode* values that refer to the same
///   instruction.  Data structures and algorithms that rely on uniqueness of a
///   SILNode* should generally make sure that they're working with the
///   representative SILNode*; see getRepresentativeSILNodeInObject().
///
/// - Do not use builtin C++ casts to downcast a SILNode*.  A static_cast
///   from SILNode* to SILInstruction* only works if the referenced
///   SILNode is the base subobject of the object's SILInstruction
///   subobject.  If the SILNode is actually the base subobject of a
///   ValueBase subobject, the cast will yield a corrupted value.
///   Always use the LLVM casts (cast<>, dyn_cast<>, etc.) instead.
class alignas(8) SILNode :
  // SILNode contains a swift object header for bridging with Swift.
  // For details see SwiftCompilerSources/README.md.
  public SwiftObjectHeader {
public:
  enum { NumVOKindBits = 3 };
  enum { NumStoreOwnershipQualifierBits = 2 };
  enum { NumLoadOwnershipQualifierBits = 2 };
  enum { NumAssignOwnershipQualifierBits = 2 };
  enum { NumAssignByWrapperModeBits = 2 };
  enum { NumSILAccessKindBits = 2 };
  enum { NumSILAccessEnforcementBits = 3 };
  enum { NumAllocRefTailTypesBits = 4 };

protected:
  friend class SILInstruction;
  template <class, class> friend class SILBitfield;

  static_assert((unsigned)SILNodeKind::Last_SILNode <= (unsigned)std::numeric_limits<uint8_t>::max(),
          "too many SILNode kinds");

  uint8_t kind;

  // Used by `NodeBitfield`.
  enum { numCustomBits = 8 };

  // Used by `NodeBitfield`.
  uint8_t customBits;

  // Part of SILInstruction's debug location. Together with
  // `SILInstruction::locationStorage` this forms the SILLocation.
  uint8_t locationKindAndFlags;

  //===--------------------------------------------------------------------===//
  //               MARK: Shared 8-bit and 32-bit fields
  //
  // Several instructions share the space for integer fields to reduce SIL
  // instruction memory.
  //
  // How to define a shared field in a SIL instruction class:
  //
  // * Decide whether to use an 8-bit (e.g. a boolean) or 32-bit integer.
  // * Add the `USE_SHARED_UINT8/32` or `TEMPLATE_USE_SHARED_UINT8/32` macros
  //   to the instruction class. This is mainly used to check for accidental
  //   overlaps of shared fields in base and derived classes.
  // * Use `SHARED_FIELD` or `SHARED_TEMPLATE_FIELD` below to add the field.
  // * In the instruction implementation use `sharedUInt8/32()` to access the
  //   field.
  //===--------------------------------------------------------------------===//

/// Adds a shared field for instruction class `I`.
#define SHARED_FIELD(I, ...) \
  class { friend class I; __VA_ARGS__; } I

/// Adds a shared field for a template instruction class `I` which has a single
/// template argument of type `T`.
#define SHARED_TEMPLATE_FIELD(T, I, ...) \
  class { template <T> friend class I; __VA_ARGS__; } I

/// Special case for `InstructionBaseWithTrailingOperands`.
#define SHARED_TEMPLATE4_FIELD(T1, T2, T3, T4, I, ...) \
  class { template <T1, T2, T3, T4> friend class I; __VA_ARGS__; } I

  // clang-format off
  union SharedUInt8Fields {
    uint8_t opaque;

    SHARED_TEMPLATE_FIELD(typename, SwitchEnumInstBase, bool hasDefault);
    SHARED_TEMPLATE_FIELD(SILInstructionKind, LoadReferenceInstBase, bool isTake);
    SHARED_TEMPLATE_FIELD(SILInstructionKind, StoreReferenceInstBase, bool isInitializationOfDest);
    SHARED_FIELD(MultipleValueInstructionResult, uint8_t valueOwnershipKind);
    SHARED_FIELD(UncheckedOwnershipConversionInst, uint8_t valueOwnershipKind);
    SHARED_FIELD(StoreInst, uint8_t ownershipQualifier);
    SHARED_FIELD(LoadInst, uint8_t ownershipQualifier);
    SHARED_FIELD(AssignInst, uint8_t ownershipQualifier);
    SHARED_FIELD(AssignByWrapperInst, uint8_t mode);
    SHARED_FIELD(StringLiteralInst, uint8_t encoding);
    SHARED_FIELD(SelectEnumInstBase, bool hasDefault);
    SHARED_FIELD(SwitchValueInst, bool hasDefault);
    SHARED_FIELD(RefCountingInst, bool atomicity);
    SHARED_FIELD(EndAccessInst, bool aborting);
    SHARED_FIELD(RefElementAddrInst, bool immutable);
    SHARED_FIELD(RefTailAddrInst, bool immutable);
    SHARED_FIELD(AddressToPointerInst, bool needsStackProtection);
    SHARED_FIELD(IndexAddrInst, bool needsStackProtection);
    SHARED_FIELD(HopToExecutorInst, bool mandatory);
    SHARED_FIELD(DestroyValueInst, bool poisonRefs);
    SHARED_FIELD(EndCOWMutationInst, bool keepUnique);
    SHARED_FIELD(ConvertFunctionInst, bool withoutActuallyEscaping);
    SHARED_FIELD(BeginCOWMutationInst, bool native);

    SHARED_FIELD(SILArgument, uint8_t
                 valueOwnershipKind : NumVOKindBits,
                 reborrow : 1,
                 escaping : 1);

    SHARED_FIELD(DebugValueInst, uint8_t
                 poisonRefs : 1,
                 usesMoveableValueDebugInfo : 1,
                 trace : 1);

    SHARED_FIELD(AllocStackInst, uint8_t
                 dynamicLifetime : 1,
                 lexical : 1,
                 usesMoveableValueDebugInfo : 1,
                 hasInvalidatedVarInfo : 1);

    SHARED_FIELD(AllocBoxInst, uint8_t
                 dynamicLifetime : 1,
                 reflection : 1,
                 usesMoveableValueDebugInfo : 1);

    SHARED_FIELD(AllocRefInstBase, uint8_t
      objC : 1,
      onStack : 1,
      numTailTypes: NumAllocRefTailTypesBits);

    SHARED_FIELD(CopyAddrInst, uint8_t
      isTakeOfSrc : 1,
      isInitializationOfDest : 1);

    SHARED_FIELD(ExplicitCopyAddrInst, uint8_t
      isTakeOfSrc : 1,
      isInitializationOfDest : 1);

    SHARED_FIELD(PointerToAddressInst, uint8_t
      isStrict : 1,
      isInvariant : 1);

    SHARED_TEMPLATE_FIELD(typename, BeginAccessBase, uint8_t
      accessKind : NumSILAccessKindBits,
      enforcement : NumSILAccessEnforcementBits,
      noNestedConflict : 1,
      fromBuiltin : 1);
    
    SHARED_FIELD(EndUnpairedAccessInst, uint8_t
      enforcement : NumSILAccessEnforcementBits,
      aborting : 1,
      fromBuiltin : 1);

  // Do not use `_sharedUInt8_private` outside of SILNode.
  } _sharedUInt8_private;
  // clang-format on

  static_assert(sizeof(SharedUInt8Fields) == sizeof(uint8_t),
    "A SILNode's shared uint8 field is too large");

  // clang-format off
  union SharedUInt32Fields {
    uint32_t opaque;

    SHARED_TEMPLATE4_FIELD(SILInstructionKind, typename, typename, typename...,
      InstructionBaseWithTrailingOperands, uint32_t numOperands);
    SHARED_TEMPLATE_FIELD(typename, FieldIndexCacheBase, uint32_t fieldIndex);
    SHARED_TEMPLATE_FIELD(typename, SwitchEnumInstBase, uint32_t numCases);
    SHARED_FIELD(AllocStackInst, uint32_t numOperands);
    SHARED_FIELD(EnumInst, uint32_t caseIndex);
    SHARED_FIELD(UncheckedEnumDataInst, uint32_t caseIndex);
    SHARED_FIELD(InjectEnumAddrInst, uint32_t caseIndex);
    SHARED_FIELD(InitEnumDataAddrInst, uint32_t caseIndex);
    SHARED_FIELD(UncheckedTakeEnumDataAddrInst, uint32_t caseIndex);
    SHARED_FIELD(TupleExtractInst, uint32_t fieldNo);
    SHARED_FIELD(TupleElementAddrInst, uint32_t fieldNo);
    SHARED_FIELD(MultipleValueInstructionResult, uint32_t index);
    SHARED_FIELD(IntegerLiteralInst, uint32_t numBits);
    SHARED_FIELD(FloatLiteralInst, uint32_t numBits);
    SHARED_FIELD(StringLiteralInst, uint32_t length);
    SHARED_FIELD(PointerToAddressInst, uint32_t alignment);
    SHARED_FIELD(SILFunctionArgument, uint32_t noImplicitCopy : 1,
                 lifetimeAnnotation : 2, closureCapture : 1,
                 parameterPack : 1);

    // Do not use `_sharedUInt32_private` outside of SILNode.
  } _sharedUInt32_private;
  // clang-format on

  static_assert(sizeof(SharedUInt32Fields) == sizeof(uint32_t),
    "A SILNode's shared uint32 field is too large");

#undef SHARED_FIELD
#undef SHARED_TEMPLATE_FIELD
#undef SHARED_TEMPLATE4_FIELD

  // Used for checking field overlaps between super and derived classes.
  enum { SharedUInt8Used = 0 };
  enum { SharedUInt32Used = 0 };

#define _USE_SHARED_UINT_BASE(T, SUPER) \
  static_assert(SUPER == 0, \
    "SILNode's shared " #T " already used in super class"); \
  enum { Shared##T##Used = 1 }; \
  SILNode::Shared##T##Fields &shared##T() { \
    return SILNode::_shared##T##_private; \
  } \
  SILNode::Shared##T##Fields shared##T() const { \
    return SILNode::_shared##T##_private; \
  }

/// To be used inside a SIL instruction/value class. It declares that the
/// instruction/value uses the SILNode's shared uint8 field.
#define USE_SHARED_UINT8 \
  _USE_SHARED_UINT_BASE(UInt8, SharedUInt8Used)

/// To be used inside a SIL instruction/value class. It declares that the
/// instruction/value uses the SILNode's shared uint32 field.
#define USE_SHARED_UINT32 \
  _USE_SHARED_UINT_BASE(UInt32, SharedUInt32Used)

/// To be used inside a template SILInstruction class. It declares that the
/// instruction uses the SILNode's shared uint8 field. The `BASE` is the
/// template argument which defines the super class of the instruction.
#define TEMPLATE_USE_SHARED_UINT8(BASE) \
  _USE_SHARED_UINT_BASE(UInt8, BASE::SharedUInt8Used)

/// To be used inside a template SILInstruction class. It declares that the
/// instruction uses the SILNode's shared uint32 field. The `BASE` is the
/// template argument which defines the super class of the instruction.
#define TEMPLATE_USE_SHARED_UINT32(BASE) \
  _USE_SHARED_UINT_BASE(UInt32, BASE::SharedUInt32Used)

  //===---------------------- end of shared fields ------------------------===//

  /// The NodeBitfield ID of the last initialized bitfield in `customBits`.
  /// Example:
  ///
  ///                   Last initialized field:
  ///           lastInitializedBitfieldID == C.bitfieldID
  ///                              |
  ///                              V
  /// customBits:  <unused> EE DDD C BB AAA
  ///              31         ...         0
  ///
  /// -> AAA, BB and C are initialized,
  ///    DD and EEE are uninitialized
  ///
  /// If the ID is negative, it means that the node (in case it's an instruction)
  /// is deleted, i.e. it does not belong to the function anymore. Conceptually
  /// this results in setting all bitfields to zero, which e.g. "removes" the
  /// node from all NodeSets.
  ///
  /// See also: SILBitfield::bitfieldID, SILFunction::currentBitfieldID.
  int64_t lastInitializedBitfieldID = 0;

private:
  SwiftMetatype getSILNodeMetatype(SILNodeKind kind);

protected:
  SILNode(SILNodeKind kind) : SwiftObjectHeader(getSILNodeMetatype(kind)),
                              kind((uint8_t)kind) {
    _sharedUInt8_private.opaque = 0;
    _sharedUInt32_private.opaque = 0;
  }

  // Used by `NodeBitfield`.
  unsigned getCustomBits() const { return customBits; }
  // Used by `NodeBitfield`.
  void setCustomBits(unsigned value) { customBits = value; }

public:
  LLVM_ATTRIBUTE_ALWAYS_INLINE
  SILNodeKind getKind() const {
    return SILNodeKind(kind);
  }

  /// If this is a SILArgument or a SILInstruction get its parent basic block,
  /// otherwise return null.
  SILBasicBlock *getParentBlock() const;

  /// If this is a SILArgument or a SILInstruction get its parent function,
  /// otherwise return null.
  SILFunction *getFunction() const;

  /// If this is a SILArgument or a SILInstruction get its parent module,
  /// otherwise return null.
  SILModule *getModule() const;
  
  /// Pretty-print the node.  If the node is an instruction, the output
  /// will be valid SIL assembly; otherwise, it will be an arbitrary
  /// format suitable for debugging.
  void print(raw_ostream &OS) const;
  void dump() const;

  /// Pretty-print the node in context, preceded by its operands (if the
  /// value represents the result of an instruction) and followed by its
  /// users.
  void printInContext(raw_ostream &OS) const;
  void dumpInContext() const;

  // Cast to SingleValueInstruction.  This is an implementation detail
  // of the cast machinery.  At a high level, all you need to know is to
  // never use static_cast to downcast a SILNode.
  SILInstruction *castToInstruction();
  const SILInstruction *castToInstruction() const;
  
  // Called when transferring basic blocks from one function to another.
  void resetBitfields() {
    lastInitializedBitfieldID = 0;
  }

  void markAsDeleted() {
    lastInitializedBitfieldID = -1;
  }

  bool isMarkedAsDeleted() const { return lastInitializedBitfieldID < 0; }

  static SILNode *instAsNode(SILInstruction *inst);
  static const SILNode *instAsNode(const SILInstruction *inst);

  static bool classof(SILNodePointer node) { return true; }
};

static_assert(sizeof(SILNode) <= 4 * sizeof(uint64_t),
              "SILNode must stay small");

inline llvm::raw_ostream &operator<<(llvm::raw_ostream &OS,
                                     const SILNode &node) {
  node.print(OS);
  return OS;
}

// Simply do a pointer cast from a SILNode to a SILNode. This is always
// possible, except the To-type is SILInstruction itself.
template <class To>
struct cast_from_SILNode {
  static To *doit(SILNode *node) { return &static_cast<To&>(*node); }
};

// Handle the special case of casting a SILNode to SILInstruction itself.
// This does not apply to sub-classes of SILInstruction, because all sub-classes
// from SILInstructions are derived from SILNode.
template <>
struct cast_from_SILNode<SILInstruction> {
  static SILInstruction *doit(SILNode *node) {
    return &static_cast<SILInstruction&>(*node->castToInstruction());
  }
};
template <>
struct cast_from_SILNode<const SILInstruction> {
  static const SILInstruction *doit(SILNode *node) {
    return &static_cast<SILInstruction&>(*node->castToInstruction());
  }
};

template <class To,
          bool IsSILInstruction = std::is_base_of<SILInstruction, To>::value>
struct cast_from_SILInstruction;

// Simply do a pointer cast from a SILInstruction to a SILInstruction.
template <class To>
struct cast_from_SILInstruction<To, /*IsSILInstruction*/ true> {
  static To *doit(SILInstruction *inst) {
    return &static_cast<To&>(*inst);
  }
};

// Cast from a SILInstruction to a SILNode, which is not a SILInstruction.
template <class To>
struct cast_from_SILInstruction<To, /*IsSILInstruction*/ false> {
  static To *doit(SILInstruction *inst) {
    return &static_cast<To&>(*SILNode::instAsNode(inst));
  }
};

} // end namespace swift

namespace llvm {

/// Completely take over cast<>'ing from SILNode* and SILInstruction*.
/// A static_cast to ValueBase* or SILInstruction* can be quite wrong.
template <class To>
struct cast_convert_val<To, swift::SILNode*, swift::SILNode*> {
  using ret_type = typename cast_retty<To, swift::SILNode*>::ret_type;
  static ret_type doit(swift::SILNode *node) {
    return swift::cast_from_SILNode<To>::doit(node);
  }
};
template <class To>
struct cast_convert_val<To, const swift::SILNode *, const swift::SILNode *> {
  using ret_type = typename cast_retty<To, const swift::SILNode*>::ret_type;
  static ret_type doit(const swift::SILNode *node) {
    return swift::cast_from_SILNode<To>::doit(const_cast<swift::SILNode*>(node));
  }
};
template <class To>
struct cast_convert_val<To, swift::SILInstruction*, swift::SILInstruction*> {
  using ret_type = typename cast_retty<To, swift::SILInstruction*>::ret_type;
  static ret_type doit(swift::SILInstruction *inst) {
    return swift::cast_from_SILInstruction<To>::doit(inst);
  }
};
template <class To>
struct cast_convert_val<To, const swift::SILInstruction *,
                            const swift::SILInstruction *> {
  using ret_type = typename cast_retty<To, const swift::SILInstruction*>::ret_type;
  static ret_type doit(const swift::SILInstruction *inst) {
    return swift::cast_from_SILInstruction<To>::
             doit(const_cast<swift::SILInstruction*>(inst));
  }
};

// We don't support casting from SILNode references yet.
template <class To, class From>
struct cast_convert_val<To, swift::SILNode, From>;
template <class To, class From>
struct cast_convert_val<To, const swift::SILNode, From>;

/// ValueBase * is always at least eight-byte aligned; make the three tag bits
/// available through PointerLikeTypeTraits.
template<>
struct PointerLikeTypeTraits<swift::SILNode *> {
public:
  static inline void *getAsVoidPointer(swift::SILNode *I) {
    return (void*)I;
  }
  static inline swift::SILNode *getFromVoidPointer(void *P) {
    return (swift::SILNode *)P;
  }
  enum { NumLowBitsAvailable = 3 };
};

} // end namespace llvm

#endif
