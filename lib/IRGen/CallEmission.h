//===--- CallEmission.h - Utility for emitting calls ------------*- C++ -*-===//
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
//  This file defines the CallEmitter class.
//
//===----------------------------------------------------------------------===//

#ifndef SWIFT_IRGEN_CALLEMISSION_H
#define SWIFT_IRGEN_CALLEMISSION_H

#include "Address.h"
#include "Callee.h"
#include "Explosion.h"
#include "Temporary.h"

namespace llvm {
  class CallSite;
}

namespace swift {
namespace irgen {

class Explosion;
class LoadableTypeInfo;
struct WitnessMetadata;
class FunctionPointer;

/// A plan for emitting a series of calls.
class CallEmission {
  enum class State { Emitting, Finished };
  State state = State::Emitting;

public:
  IRGenFunction &IGF;

protected:
  llvm::Value *selfValue;

  /// The builtin/special arguments to pass to the call.
  SmallVector<llvm::Value*, 8> Args;

  /// Temporaries required by the call.
  TemporarySet Temporaries;

  /// Temporaries required by the call that are not mapped to any
  /// SIL value.
  SmallVector<StackAddress, 8> RawTempraries;

  /// The function we're going to call.
  Callee CurCallee;

  /// Only valid if the SIL function has indirect return values.
  /// If the function has multiple indirect return values, this is the address
  /// of the first indirect return value.
  Address indirectReturnAddress;

  /// For C-functions: true if the return is indirect in SIL, but direct for a C-function.
  /// That can happen for "sensitive" structs.
  bool convertDirectToIndirectReturn = false;

  unsigned LastArgWritten;

  /// Whether this is a coroutine invocation.
  bool IsCoroutine;

  /// Whether we've emitted the call for the current callee yet.  This
  /// is just for debugging purposes --- e.g. the destructor asserts
  /// that it's true --- but is otherwise derivable from
  /// RemainingArgsForCallee, at least between calls.
  bool EmittedCall;

  /// The basic block to which the call to a potentially throwing foreign
  /// function should jump to continue normal execution of the program.
  llvm::BasicBlock *invokeNormalDest = nullptr;

  /// The basic block to which the call to a potentially throwing foreign
  /// function should jump to in case an exception has been thrown during the
  /// invocation of the call.
  llvm::BasicBlock *invokeUnwindDest = nullptr;

  unsigned IndirectTypedErrorArgIdx = 0;

  std::optional<Explosion> typedErrorExplosion;

  virtual void setFromCallee();
  void emitToUnmappedMemory(Address addr);
  void emitToUnmappedExplosion(Explosion &out);
  virtual void emitCallToUnmappedExplosion(llvm::CallBase *call,
                                           Explosion &out) = 0;
  void emitYieldsToExplosion(Explosion &out);
  void setKeyPathAccessorArguments(Explosion &in, bool isOutlined,
                                   Explosion &out);
  virtual FunctionPointer getCalleeFunctionPointer() = 0;
  llvm::CallBase *emitCallSite();

  virtual llvm::CallBase *createCall(const FunctionPointer &fn,
                                     ArrayRef<llvm::Value *> args) = 0;

  void externalizeArguments(IRGenFunction &IGF, const Callee &callee,
                                   Explosion &in, Explosion &out,
                                   TemporarySet &temporaries,
                                   bool isOutlined);

  CallEmission(IRGenFunction &IGF, llvm::Value *selfValue, Callee &&callee)
      : IGF(IGF), selfValue(selfValue), CurCallee(std::move(callee)) {}

public:
  CallEmission(const CallEmission &other) = delete;
  CallEmission(CallEmission &&other);
  CallEmission &operator=(const CallEmission &other) = delete;
  virtual ~CallEmission();

  const Callee &getCallee() const { return CurCallee; }

  SubstitutionMap getSubstitutions() const {
    return CurCallee.getSubstitutions();
  }

  std::optional<Explosion> &getTypedErrorExplosion() {
    return typedErrorExplosion;
  }

  virtual void begin();
  virtual void end();
  virtual SILType getParameterType(unsigned index) = 0;
  /// Set the arguments to the function from an explosion.
  virtual void setArgs(Explosion &arg, bool isOutlined,
                       WitnessMetadata *witnessMetadata);
  virtual Address getCalleeErrorSlot(SILType errorType, bool isCalleeAsync) = 0;

  void addFnAttribute(llvm::Attribute::AttrKind Attr);

  void setIndirectReturnAddress(Address addr) { indirectReturnAddress = addr; }

  void addParamAttribute(unsigned ParamIndex, llvm::Attribute::AttrKind Attr);

  void emitToMemory(Address addr, const LoadableTypeInfo &substResultTI,
                    bool isOutlined);
  void emitToExplosion(Explosion &out, bool isOutlined);

  llvm::CallBase *emitCoroutineAsOrdinaryFunction() {
    assert(IsCoroutine);
    IsCoroutine = false;

    return emitCallSite();
  }

  TemporarySet claimTemporaries() {
    // Move the actual temporary set out.
    auto result = std::move(Temporaries);

    // Flag that we've cleared the set.
    Temporaries.clear();

    return result;
  }

  virtual llvm::Value *getResumeFunctionPointer() = 0;
  virtual llvm::Value *getAsyncContext() = 0;

  void setIndirectTypedErrorResultSlot(llvm::Value *addr) {
    Args[IndirectTypedErrorArgIdx] = addr;
  }

  void setIndirectTypedErrorResultSlotArgsIndex(unsigned idx) {
    assert(idx != 0);
    IndirectTypedErrorArgIdx = idx;
  }
};

std::unique_ptr<CallEmission>
getCallEmission(IRGenFunction &IGF, llvm::Value *selfValue, Callee &&callee);

} // end namespace irgen
} // end namespace swift

#endif
