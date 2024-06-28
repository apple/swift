//===--- CaptureInfo.h - Data Structure for Capture Lists -------*- C++ -*-===//
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

#ifndef SWIFT_AST_CAPTURE_INFO_H
#define SWIFT_AST_CAPTURE_INFO_H

#include "swift/Basic/Debug.h"
#include "swift/Basic/LLVM.h"
#include "swift/Basic/OptionSet.h"
#include "swift/Basic/SourceLoc.h"
#include "swift/AST/TypeAlignments.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/PointerIntPair.h"
#include "llvm/ADT/PointerUnion.h"
#include "llvm/Support/TrailingObjects.h"

namespace swift {
class CapturedValue;
} // namespace swift

namespace swift {
namespace Lowering {
class TypeConverter;
} // namespace Lowering
} // namespace swift

namespace llvm {
class raw_ostream;
template <> struct DenseMapInfo<swift::CapturedValue>;
} // namespace llvm

namespace swift {
class ValueDecl;
class FuncDecl;
class Expr;
class OpaqueValueExpr;
class PackElementExpr;
class VarDecl;
class GenericEnvironment;
class Type;

/// CapturedValue includes both the declaration being captured, along with flags
/// that indicate how it is captured.
class CapturedValue {
  friend class Lowering::TypeConverter;

public:
  using Storage =
      llvm::PointerIntPair<llvm::PointerUnion<ValueDecl *, Expr *>, 2,
                           unsigned>;

private:
  Storage Value;
  SourceLoc Loc;

  explicit CapturedValue(Storage V, SourceLoc Loc) : Value(V), Loc(Loc) {}

public:
  friend struct llvm::DenseMapInfo<CapturedValue>;

  enum {
    /// IsDirect is set when a VarDecl with storage *and* accessors is captured
    /// by its storage address.  This happens in the accessors for the VarDecl.
    IsDirect = 1 << 0,

    /// IsNoEscape is set when a vardecl is captured by a noescape closure, and
    /// thus has its lifetime guaranteed.  It can be closed over by a fixed
    /// address if it has storage.
    IsNoEscape = 1 << 1
  };

  CapturedValue(ValueDecl *Val, unsigned Flags, SourceLoc Loc)
      : Value(Val, Flags), Loc(Loc) {}

  CapturedValue(Expr *Val, unsigned Flags);

public:
  static CapturedValue getDynamicSelfMetadata() {
    return CapturedValue((ValueDecl *)nullptr, 0, SourceLoc());
  }

  bool isDirect() const { return Value.getInt() & IsDirect; }
  bool isNoEscape() const { return Value.getInt() & IsNoEscape; }

  bool isDynamicSelfMetadata() const { return !Value.getPointer(); }

  bool isExpr() const {
    return Value.getPointer().dyn_cast<Expr *>();
  }

  bool isPackElement() const;
  bool isOpaqueValue() const;

  /// Returns true if this captured value is a local capture.
  ///
  /// NOTE: This implies that the value is not dynamic self metadata, since
  /// values with decls are the only values that are able to be local captures.
  bool isLocalCapture() const;

  CapturedValue mergeFlags(unsigned flags) const {
    return CapturedValue(Storage(Value.getPointer(), getFlags() & flags), Loc);
  }

  ValueDecl *getDecl() const {
    return Value.getPointer().dyn_cast<ValueDecl *>();
  }

  Expr *getExpr() const {
    return Value.getPointer().dyn_cast<Expr *>();
  }

  OpaqueValueExpr *getOpaqueValue() const;

  PackElementExpr *getPackElement() const;

  Type getPackElementType() const;

  SourceLoc getLoc() const { return Loc; }

  unsigned getFlags() const { return Value.getInt(); }
};

} // end swift namespace

namespace swift {

class DynamicSelfType;

/// Stores information about captured variables.
class CaptureInfo {
  class CaptureInfoStorage final
      : public llvm::TrailingObjects<CaptureInfoStorage,
                                     CapturedValue,
                                     GenericEnvironment *> {

    DynamicSelfType *DynamicSelf;
    OpaqueValueExpr *OpaqueValue;
    unsigned NumCapturedValues;
    unsigned NumGenericEnvironments;

  public:
    explicit CaptureInfoStorage(DynamicSelfType *dynamicSelf,
                                OpaqueValueExpr *opaqueValue,
                                unsigned numCapturedValues,
                                unsigned numGenericEnvironments)
      : DynamicSelf(dynamicSelf), OpaqueValue(opaqueValue),
        NumCapturedValues(numCapturedValues),
        NumGenericEnvironments(numGenericEnvironments) { }

    ArrayRef<CapturedValue> getCaptures() const;

    ArrayRef<GenericEnvironment *> getGenericEnvironments() const;

    DynamicSelfType *getDynamicSelfType() const {
      return DynamicSelf;
    }

    OpaqueValueExpr *getOpaqueValue() const {
      return OpaqueValue;
    }

    unsigned numTrailingObjects(OverloadToken<CapturedValue>) const {
      return NumCapturedValues;
    }
  };

  enum class Flags : unsigned {
    HasGenericParamCaptures = 1 << 0
  };

  llvm::PointerIntPair<const CaptureInfoStorage *, 2, OptionSet<Flags>>
      StorageAndFlags;

public:
  /// The default-constructed CaptureInfo is "not yet computed".
  CaptureInfo() = default;
  CaptureInfo(ASTContext &ctx,
              ArrayRef<CapturedValue> captures,
              DynamicSelfType *dynamicSelf, OpaqueValueExpr *opaqueValue,
              bool genericParamCaptures,
              ArrayRef<GenericEnvironment *> genericEnv=ArrayRef<GenericEnvironment*>());

  /// A CaptureInfo representing no captures at all.
  static CaptureInfo empty();

  bool hasBeenComputed() const {
    return StorageAndFlags.getPointer();
  }

  bool isTrivial() const {
    assert(hasBeenComputed());
    return getCaptures().empty() && !hasGenericParamCaptures() &&
           !hasDynamicSelfCapture() && !hasOpaqueValueCapture();
  }

  /// Returns all captured values and opaque expressions.
  ArrayRef<CapturedValue> getCaptures() const {
    assert(hasBeenComputed());
    return StorageAndFlags.getPointer()->getCaptures();
  }

  /// Returns all captured pack element environments.
  ArrayRef<GenericEnvironment *> getGenericEnvironments() const {
    assert(hasBeenComputed());
    return StorageAndFlags.getPointer()->getGenericEnvironments();
  }

  /// \returns true if the function captures the primary generic environment
  /// from its innermost declaration context.
  bool hasGenericParamCaptures() const {
    assert(hasBeenComputed());
    return StorageAndFlags.getInt().contains(Flags::HasGenericParamCaptures);
  }

  /// \returns true if the function captures the dynamic Self type.
  bool hasDynamicSelfCapture() const {
    return getDynamicSelfType() != nullptr;
  }

  /// \returns the captured dynamic Self type, if any.
  DynamicSelfType *getDynamicSelfType() const {
    assert(hasBeenComputed());
    return StorageAndFlags.getPointer()->getDynamicSelfType();
  }

  bool hasOpaqueValueCapture() const {
    assert(hasBeenComputed());
    return getOpaqueValue() != nullptr;
  }

  OpaqueValueExpr *getOpaqueValue() const {
    assert(hasBeenComputed());
    return StorageAndFlags.getPointer()->getOpaqueValue();
  }

  /// Retrieve the variable corresponding to an isolated parameter that has
  /// been captured, if there is one. This might be a capture variable
  /// that was initialized with an isolated parameter.
  VarDecl *getIsolatedParamCapture() const;

  SWIFT_DEBUG_DUMP;
  void print(raw_ostream &OS) const;
};

} // namespace swift

#endif // LLVM_SWIFT_AST_CAPTURE_INFO_H

