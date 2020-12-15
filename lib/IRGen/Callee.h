//===--- Callee.h - Information about a physical callee ---------*- C++ -*-===//
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
// This file defines the Callee type, which stores all necessary
// information about a physical callee.
//
//===----------------------------------------------------------------------===//

#ifndef SWIFT_IRGEN_CALLEE_H
#define SWIFT_IRGEN_CALLEE_H

#include <type_traits>
#include "swift/AST/IRGenOptions.h"
#include "llvm/IR/DerivedTypes.h"
#include "swift/SIL/SILType.h"
#include "IRGen.h"
#include "Signature.h"

namespace llvm {
  class ConstantInt;
}

namespace swift {

namespace irgen {
  class Callee;
  class IRGenFunction;
  class PointerAuthEntity;

  class CalleeInfo {
  public:
    /// The unsubstituted function type being called.
    CanSILFunctionType OrigFnType;

    /// The substituted result type of the function being called.
    CanSILFunctionType SubstFnType;

    /// The archetype substitutions under which the function is being
    /// called.
    SubstitutionMap Substitutions;

    CalleeInfo(CanSILFunctionType origFnType,
               CanSILFunctionType substFnType,
               SubstitutionMap substitutions)
      : OrigFnType(origFnType), SubstFnType(substFnType),
        Substitutions(substitutions) {
    }
  };

  /// Information necessary for pointer authentication.
  class PointerAuthInfo {
    unsigned Signed : 1;
    unsigned Key : 31;
    llvm::Value *Discriminator;
  public:
    PointerAuthInfo() {
      Signed = false;
    }
    PointerAuthInfo(unsigned key, llvm::Value *discriminator)
        : Discriminator(discriminator) {
      assert(discriminator->getType()->isIntegerTy() ||
             discriminator->getType()->isPointerTy());
      Signed = true;
      Key = key;
    }

    static PointerAuthInfo emit(IRGenFunction &IGF,
                                const PointerAuthSchema &schema,
                                llvm::Value *storageAddress,
                                const PointerAuthEntity &entity);

    static PointerAuthInfo forFunctionPointer(IRGenModule &IGM,
                                              CanSILFunctionType fnType);

    static llvm::ConstantInt *getOtherDiscriminator(IRGenModule &IGM,
                                           const PointerAuthSchema &schema,
                                           const PointerAuthEntity &entity);

    explicit operator bool() const {
      return isSigned();
    }

    bool isSigned() const {
      return Signed;
    }

    bool isConstant() const {
      return (!isSigned() || isa<llvm::Constant>(Discriminator));
    }

    unsigned getKey() const {
      assert(isSigned());
      return Key;
    }
    llvm::Value *getDiscriminator() const {
      assert(isSigned());
      return Discriminator;
    }

    /// Are the auth infos obviously the same?
    friend bool operator==(const PointerAuthInfo &lhs,
                           const PointerAuthInfo &rhs) {
      if (!lhs.Signed)
        return !rhs.Signed;
      if (!rhs.Signed)
        return false;

      return (lhs.Key == rhs.Key && lhs.Discriminator == rhs.Discriminator);
    }
    friend bool operator!=(const PointerAuthInfo &lhs,
                           const PointerAuthInfo &rhs) {
      return !(lhs == rhs);
    }
  };

  /// A function pointer value.
  class FunctionPointer {
  public:
    struct KindTy {
      enum class Value {
        Function,
        AsyncFunctionPointer,
      };
      static const Value Function = Value::Function;
      static const Value AsyncFunctionPointer = Value::AsyncFunctionPointer;
      Value value;
      KindTy(Value value) : value(value) {}
      KindTy(CanSILFunctionType fnType)
          : value(fnType->isAsync() ? Value::AsyncFunctionPointer
                                    : Value::Function) {}
      friend bool operator==(const KindTy &lhs, const KindTy &rhs) {
        return lhs.value == rhs.value;
      }
      friend bool operator!=(const KindTy &lhs, const KindTy &rhs) {
        return !(lhs == rhs);
      }
    };

  private:
    KindTy Kind;

    /// The actual pointer, either to the function or to its descriptor.
    llvm::Value *Value;

    PointerAuthInfo AuthInfo;

    Signature Sig;

    bool isFunctionPointerWithoutContext = false;

  public:
    /// Construct a FunctionPointer for an arbitrary pointer value.
    /// We may add more arguments to this; try to use the other
    /// constructors/factories if possible.
    explicit FunctionPointer(KindTy kind, llvm::Value *value,
                             PointerAuthInfo authInfo,
                             const Signature &signature,
                             bool isWithoutCtxt = false)
        : Kind(kind), Value(value), AuthInfo(authInfo), Sig(signature),
          isFunctionPointerWithoutContext(isWithoutCtxt) {
      // The function pointer should have function type.
      assert(value->getType()->getPointerElementType()->isFunctionTy());
      // TODO: maybe assert similarity to signature.getType()?
    }

    // Temporary only!
    explicit FunctionPointer(KindTy kind, llvm::Value *value,
                             const Signature &signature, bool
                             isWithoutCtxt = false)
        : FunctionPointer(kind, value, PointerAuthInfo(), signature, isWithoutCtxt) {}

    static FunctionPointer forDirect(IRGenModule &IGM,
                                     llvm::Constant *value,
                                     CanSILFunctionType fnType);

    static FunctionPointer forDirect(KindTy kind, llvm::Constant *value,
                                     const Signature &signature) {
      return FunctionPointer(kind, value, PointerAuthInfo(), signature);
    }

    static FunctionPointer forExplosionValue(IRGenFunction &IGF,
                                             llvm::Value *fnPtr,
                                             CanSILFunctionType fnType);

    /// Is this function pointer completely constant?  That is, can it
    /// be safely moved to a different function context?
    bool isConstant() const {
      return (isa<llvm::Constant>(Value) && AuthInfo.isConstant());
    }

    KindTy getKind() const { return Kind; }

    /// Given that this value is known to have been constructed from a direct
    /// function,  Return the name of that function.
    StringRef getName(IRGenModule &IGM) const;

    /// Return the actual function pointer.
    llvm::Value *getPointer(IRGenFunction &IGF) const;

    /// Return the actual function pointer.
    llvm::Value *getRawPointer() const { return Value; }

    /// Given that this value is known to have been constructed from
    /// a direct function, return the function pointer.
    llvm::Constant *getDirectPointer() const {
      return cast<llvm::Constant>(Value);
    }

    llvm::FunctionType *getFunctionType() const {
      return cast<llvm::FunctionType>(
                                  Value->getType()->getPointerElementType());
    }

    const PointerAuthInfo &getAuthInfo() const {
      return AuthInfo;
    }

    const Signature &getSignature() const {
      return Sig;
    }

    llvm::CallingConv::ID getCallingConv() const {
      return Sig.getCallingConv();
    }

    llvm::AttributeList getAttributes() const {
      return Sig.getAttributes();
    }
    llvm::AttributeList &getMutableAttributes() & {
      return Sig.getMutableAttributes();
    }

    ForeignFunctionInfo getForeignInfo() const {
      return Sig.getForeignInfo();
    }

    llvm::Value *getExplosionValue(IRGenFunction &IGF,
                                   CanSILFunctionType fnType) const;

    /// Form a FunctionPointer whose KindTy is ::Function.
    FunctionPointer getAsFunction(IRGenFunction &IGF) const;

    bool useStaticContextSize() const {
      return isFunctionPointerWithoutContext;
    }
  };

  class Callee {
    CalleeInfo Info;

    /// The actual function pointer to invoke.
    FunctionPointer Fn;

    /// The first data pointer required by the function invocation.
    llvm::Value *FirstData;

    /// The second data pointer required by the function invocation.
    llvm::Value *SecondData;

  public:
    Callee(const Callee &other) = delete;
    Callee &operator=(const Callee &other) = delete;

    Callee(Callee &&other) = default;
    Callee &operator=(Callee &&other) = default;

    Callee(CalleeInfo &&info, const FunctionPointer &fn,
           llvm::Value *firstData = nullptr,
           llvm::Value *secondData = nullptr);

    SILFunctionTypeRepresentation getRepresentation() const {
      return Info.OrigFnType->getRepresentation();
    }

    CanSILFunctionType getOrigFunctionType() const {
      return Info.OrigFnType;
    }
    CanSILFunctionType getSubstFunctionType() const {
      return Info.SubstFnType;
    }

    bool hasSubstitutions() const {
      return Info.Substitutions.hasAnySubstitutableParams();
    }
    
    SubstitutionMap getSubstitutions() const { return Info.Substitutions; }

    const FunctionPointer &getFunctionPointer() const { return Fn; }

    llvm::FunctionType *getLLVMFunctionType() {
      return Fn.getFunctionType();
    }

    llvm::AttributeList getAttributes() const {
      return Fn.getAttributes();
    }
    llvm::AttributeList &getMutableAttributes() & {
      return Fn.getMutableAttributes();
    }

    ForeignFunctionInfo getForeignInfo() const {
      return Fn.getForeignInfo();
    }

    const Signature &getSignature() const {
      return Fn.getSignature();
    }

    /// If this callee has a value for the Swift context slot, return
    /// it; otherwise return non-null.
    llvm::Value *getSwiftContext() const;

    /// Given that this callee is a block, return the block pointer.
    llvm::Value *getBlockObject() const;

    /// Given that this callee is a C++ method, return the self argument.
    llvm::Value *getCXXMethodSelf() const;

    /// Given that this callee is an ObjC method, return the receiver
    /// argument.  This might not be 'self' anymore.
    llvm::Value *getObjCMethodReceiver() const;

    /// Given that this callee is an ObjC method, return the receiver
    /// argument.  This might not be 'self' anymore.
    llvm::Value *getObjCMethodSelector() const;
  };

} // end namespace irgen
} // end namespace swift

#endif
