//===--- LoadableTypeInfo.h - Supplement for loadable types -----*- C++ -*-===//
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
// This file defines LoadableTypeInfo, which supplements the TypeInfo
// interface for types that support being loaded and stored as
// Explosions.
//
//===----------------------------------------------------------------------===//

#ifndef SWIFT_IRGEN_LOADABLETYPEINFO_H
#define SWIFT_IRGEN_LOADABLETYPEINFO_H

#include "FixedTypeInfo.h"

namespace clang {
namespace CodeGen {
namespace swiftcall {
  class SwiftAggLowering;
}
}
}

namespace swift {
namespace irgen {
  class EnumPayload;
  class IRBuilder;
  using clang::CodeGen::swiftcall::SwiftAggLowering;

struct LoadedRef {
  llvm::PointerIntPair<llvm::Value*, 1> ValAndNonNull;
  ReferenceCounting style;
public:
  LoadedRef(llvm::Value *V, bool nonNull, ReferenceCounting style): ValAndNonNull(V, nonNull), style(style) {}
  LoadedRef(const LoadedRef &ref, bool nonNull): ValAndNonNull(ref.getValue(), nonNull), style(ref.getStyle()) {}
  llvm::Value *getValue() const { return ValAndNonNull.getPointer(); }
  bool isNonNull() const { return ValAndNonNull.getInt(); }
  ReferenceCounting getStyle() const { return style; }
};

/// LoadableTypeInfo - A refinement of FixedTypeInfo designed for use
/// when implementing a type that can be loaded into an explosion.
/// Types that are not loadable are called address-only; this is the
/// same concept as exists in SIL.
///
/// The semantics of most of these operations are specified as if an
/// exploded value were a normal object that is merely not located in
/// memory.
class LoadableTypeInfo : public FixedTypeInfo {
protected:
  LoadableTypeInfo(llvm::Type *type, Size size,
                   const SpareBitVector &spareBits,
                   Alignment align,
                   IsTriviallyDestroyable_t pod,
                   IsCopyable_t copy,
                   IsFixedSize_t alwaysFixedSize,
                   SpecialTypeInfoKind stik = SpecialTypeInfoKind::Loadable)
      : FixedTypeInfo(type, size, spareBits, align, pod,
                      // All currently implemented loadable types are bitwise-takable.
                      IsBitwiseTakable,
                      copy, alwaysFixedSize, stik) {
    assert(isLoadable());
  }

  LoadableTypeInfo(llvm::Type *type, Size size,
                   SpareBitVector &&spareBits,
                   Alignment align,
                   IsTriviallyDestroyable_t pod,
                   IsCopyable_t copy,
                   IsFixedSize_t alwaysFixedSize,
                   SpecialTypeInfoKind stik = SpecialTypeInfoKind::Loadable)
      : FixedTypeInfo(type, size, std::move(spareBits), align, pod,
                      // All currently implemented loadable types are bitwise-takable.
                      IsBitwiseTakable,
                      copy, alwaysFixedSize, stik) {
    assert(isLoadable());
  }

public:
  // This is useful for metaprogramming.
  static bool isLoadable() { return true; }
  
  /// Return the number of elements in an explosion of this type.
  virtual unsigned getExplosionSize() const = 0;

  /// Load an explosion of values from an address as if copy-initializing
  /// a set of registers.
  virtual void loadAsCopy(IRGenFunction &IGF, Address addr,
                          Explosion &explosion) const = 0;

  /// Load an explosion of values from an address as if
  /// take-initializing a set of registers.
  virtual void loadAsTake(IRGenFunction &IGF, Address addr,
                          Explosion &explosion) const = 0;

  /// Assign a set of exploded values into an address.  The values are
  /// consumed out of the explosion.
  virtual void assign(IRGenFunction &IGF, Explosion &explosion, Address addr,
                      bool isOutlined, SILType T) const = 0;

  /// Initialize an address by consuming values out of an explosion.
  virtual void initialize(IRGenFunction &IGF, Explosion &explosion,
                          Address addr, bool isOutlined) const = 0;

  // We can give this a reasonable default implementation.
  void initializeWithCopy(IRGenFunction &IGF, Address destAddr, Address srcAddr,
                          SILType T, bool isOutlined) const override;

  /// Consume a bunch of values which have exploded at one explosion
  /// level and produce them at another.
  ///
  /// Essentially, this is like take-initializing the new explosion.
  virtual void reexplode(Explosion &sourceExplosion,
                         Explosion &targetExplosion) const = 0;

  /// Shift values from the source explosion to the target explosion
  /// as if by copy-initialization.
  virtual void copy(IRGenFunction &IGF, Explosion &sourceExplosion,
                    Explosion &targetExplosion, Atomicity atomicity) const = 0;
  
  /// Release reference counts or other resources owned by the explosion.
  virtual void consume(IRGenFunction &IGF, Explosion &explosion,
                       Atomicity atomicity,
                       SILType T) const = 0;

  /// Fix the lifetime of the source explosion by creating opaque calls to
  /// swift_fixLifetime for all reference types in the explosion.
  virtual void fixLifetime(IRGenFunction &IGF, Explosion &explosion) const = 0;
  
  /// Pack the source explosion into an enum payload.
  virtual void packIntoEnumPayload(IRGenModule &IGM,
                                   IRBuilder &builder,
                                   EnumPayload &payload,
                                   Explosion &sourceExplosion,
                                   unsigned offset) const = 0;
  
  /// Unpack an enum payload containing a valid value of the type into the
  /// destination explosion.
  virtual void unpackFromEnumPayload(IRGenFunction &IGF,
                                     const EnumPayload &payload,
                                     Explosion &targetExplosion,
                                     unsigned offset) const = 0;

  /// Load a reference counted pointer from an address.
  /// Return the loaded pointer value.
  virtual LoadedRef loadRefcountedPtr(IRGenFunction &IGF, SourceLoc loc,
                                      Address addr) const;

  /// Add this type to the given aggregate lowering.
  virtual void addToAggLowering(IRGenModule &IGM, SwiftAggLowering &lowering,
                                Size offset) const = 0;

  static void addScalarToAggLowering(IRGenModule &IGM,
                                     SwiftAggLowering &lowering,
                                     llvm::Type *type, Size offset,
                                     Size storageSize);

  static bool classof(const LoadableTypeInfo *type) { return true; }
  static bool classof(const TypeInfo *type) { return type->isLoadable(); }
};

}
}

#endif
