//===--- GenCall.h - IR generation for calls and prologues ------*- C++ -*-===//
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
//  This file provides the private interface to the function call
//  and prologue emission support code.
//
//===----------------------------------------------------------------------===//

#ifndef SWIFT_IRGEN_GENCALL_H
#define SWIFT_IRGEN_GENCALL_H

#include <stdint.h>

#include "swift/AST/Types.h"
#include "swift/Basic/LLVM.h"
#include "swift/SIL/ApplySite.h"
#include "llvm/IR/CallingConv.h"

#include "Callee.h"
#include "GenHeap.h"
#include "IRGenModule.h"

namespace llvm {
  class AttributeList;
  class Constant;
  class Twine;
  class Type;
  class Value;
}

namespace clang {
  template <class> class CanQual;
  class Type;
}

namespace swift {
namespace irgen {
  class Address;
  class Alignment;
  class Callee;
  class CalleeInfo;
  class Explosion;
  class ExplosionSchema;
  class ForeignFunctionInfo;
  class IRGenFunction;
  class IRGenModule;
  class LoadableTypeInfo;
  class NativeCCEntryPointArgumentEmission;
  class Size;
  class TypeInfo;

  enum class TranslationDirection : bool {
    ToForeign,
    ToNative
  };
  inline TranslationDirection reverse(TranslationDirection direction) {
    return TranslationDirection(!bool(direction));
  }

  // struct SwiftContext {
  //   SwiftContext * __ptrauth(...) callerContext;
  //   SwiftPartialFunction * __ptrauth(...) returnToCaller;
  //   SwiftActor * __ptrauth(...) callerActor;
  //   SwiftPartialFunction * __ptrauth(...) yieldToCaller?;
  // };
  struct AsyncContextLayout : StructLayout {
    struct ArgumentInfo {
      SILType type;
      ParameterConvention convention;
    };
    struct TrailingWitnessInfo {};

  private:
    enum class FixedIndex : unsigned {
      Parent = 0,
      ResumeParent = 1,
      Flags = 2,
    };
    enum class FixedCount : unsigned {
      Parent = 1,
      ResumeParent = 1,
    };
    CanSILFunctionType originalType;
    CanSILFunctionType substitutedType;
    SubstitutionMap substitutionMap;
  
    unsigned getParentIndex() { return (unsigned)FixedIndex::Parent; }
    unsigned getResumeParentIndex() {
      return (unsigned)FixedIndex::ResumeParent;
    }
    unsigned getFlagsIndex() { return (unsigned)FixedIndex::Flags; }

  public:
    ElementLayout getParentLayout() { return getElement(getParentIndex()); }
    ElementLayout getResumeParentLayout() {
      return getElement(getResumeParentIndex());
    }
    ElementLayout getFlagsLayout() { return getElement(getFlagsIndex()); }

    AsyncContextLayout(
        IRGenModule &IGM, LayoutStrategy strategy, ArrayRef<SILType> fieldTypes,
        ArrayRef<const TypeInfo *> fieldTypeInfos,
        CanSILFunctionType originalType, CanSILFunctionType substitutedType,
        SubstitutionMap substitutionMap);
  };

  AsyncContextLayout getAsyncContextLayout(IRGenModule &IGM,
                                           SILFunction *function);

  AsyncContextLayout getAsyncContextLayout(IRGenModule &IGM,
                                           CanSILFunctionType originalType,
                                           CanSILFunctionType substitutedType,
                                           SubstitutionMap substitutionMap);

  /// Given an async function, get the pointer to the function to be called and
  /// the size of the context to be allocated.
  ///
  /// \param values Whether any code should be emitted to retrieve the function
  ///               pointer and the size, respectively.  If false is passed, no
  ///               code will be emitted to generate that value and null will
  ///               be returned for it.
  ///
  /// \return {function, size}
  std::pair<llvm::Value *, llvm::Value *> getAsyncFunctionAndSize(
      IRGenFunction &IGF, SILFunctionTypeRepresentation representation,
      FunctionPointer functionPointer, llvm::Value *thickContext,
      std::pair<bool, bool> values = {true, true});
  llvm::CallingConv::ID expandCallingConv(IRGenModule &IGM,
                                     SILFunctionTypeRepresentation convention,
                                     bool isAsync);

  Signature emitCastOfFunctionPointer(IRGenFunction &IGF, llvm::Value *&fnPtr,
                                      CanSILFunctionType fnType,
                                      bool forAsyncReturn = false);

  /// Does the given function have a self parameter that should be given
  /// the special treatment for self parameters?
  bool hasSelfContextParameter(CanSILFunctionType fnType);

  /// Add function attributes to an attribute set for a byval argument.
  void addByvalArgumentAttributes(IRGenModule &IGM,
                                  llvm::AttributeList &attrs,
                                  unsigned argIndex,
                                  Alignment align,
                                  llvm::Type *storageType);

  /// Can a series of values be simply pairwise coerced to (or from) an
  /// explosion schema, or do they need to traffic through memory?
  bool canCoerceToSchema(IRGenModule &IGM,
                         ArrayRef<llvm::Type*> types,
                         const ExplosionSchema &schema);

  void emitForeignParameter(IRGenFunction &IGF, Explosion &params,
                            ForeignFunctionInfo foreignInfo,
                            unsigned foreignParamIndex, SILType paramTy,
                            const LoadableTypeInfo &paramTI,
                            Explosion &paramExplosion, bool isOutlined);

  void emitClangExpandedParameter(IRGenFunction &IGF,
                                  Explosion &in, Explosion &out,
                                  clang::CanQual<clang::Type> clangType,
                                  SILType swiftType,
                                  const LoadableTypeInfo &swiftTI);

  bool addNativeArgument(IRGenFunction &IGF,
                         Explosion &in,
                         CanSILFunctionType fnTy,
                         SILParameterInfo origParamInfo, Explosion &args,
                         bool isOutlined);

  /// Allocate a stack buffer of the appropriate size to bitwise-coerce a value
  /// between two LLVM types.
  std::pair<Address, Size>
  allocateForCoercion(IRGenFunction &IGF,
                      llvm::Type *fromTy,
                      llvm::Type *toTy,
                      const llvm::Twine &basename);

  void extractScalarResults(IRGenFunction &IGF, llvm::Type *bodyType,
                            llvm::Value *call, Explosion &out);

  Callee getBlockPointerCallee(IRGenFunction &IGF, llvm::Value *blockPtr,
                               CalleeInfo &&info);

  Callee getCFunctionPointerCallee(IRGenFunction &IGF, llvm::Value *fnPtr,
                                   CalleeInfo &&info);

  Callee getSwiftFunctionPointerCallee(IRGenFunction &IGF,
                                       llvm::Value *fnPtr,
                                       llvm::Value *contextPtr,
                                       CalleeInfo &&info,
                                       bool castOpaqueToRefcountedContext,
                                       bool isClosure);

  Address emitAllocYieldOnceCoroutineBuffer(IRGenFunction &IGF);
  void emitDeallocYieldOnceCoroutineBuffer(IRGenFunction &IGF, Address buffer);
  void
  emitYieldOnceCoroutineEntry(IRGenFunction &IGF,
                              CanSILFunctionType coroutineType,
                              NativeCCEntryPointArgumentEmission &emission);

  Address emitAllocYieldManyCoroutineBuffer(IRGenFunction &IGF);
  void emitDeallocYieldManyCoroutineBuffer(IRGenFunction &IGF, Address buffer);
  void
  emitYieldManyCoroutineEntry(IRGenFunction &IGF,
                              CanSILFunctionType coroutineType,
                              NativeCCEntryPointArgumentEmission &emission);

  void emitTaskCancel(IRGenFunction &IGF, llvm::Value *task);

  llvm::Value *addEmbeddedSwiftResultTypeInfo(IRGenFunction &IGF,
                                              llvm::Value *taskOptions,
                                              SubstitutionMap subs);

  /// Emit a call to swift_task_create[_f] with the given flags, options, and
  /// task function.
  llvm::Value *emitTaskCreate(
          IRGenFunction &IGF,
          llvm::Value *flags,
          llvm::Value *taskGroup,
          llvm::Value *executor1,
          llvm::Value *executor2,
          llvm::Value *futureResultType,
          llvm::Value *taskFunction,
          llvm::Value *localContextInfo,
          SubstitutionMap subs);

  /// Allocate task local storage for the provided dynamic size.
  Address emitAllocAsyncContext(IRGenFunction &IGF, llvm::Value *sizeValue);
  void emitDeallocAsyncContext(IRGenFunction &IGF, Address context);
  Address emitStaticAllocAsyncContext(IRGenFunction &IGF, Size size);
  void emitStaticDeallocAsyncContext(IRGenFunction &IGF, Address context,
                                     Size size);

  void emitAsyncFunctionEntry(IRGenFunction &IGF,
                              const AsyncContextLayout &layout,
                              LinkEntity asyncFunction,
                              unsigned asyncContextIndex);

  /// Yield the given values from the current continuation.
  ///
  /// \return an i1 indicating whether the caller wants to unwind this
  ///   coroutine instead of resuming it normally
  llvm::Value *emitYield(IRGenFunction &IGF,
                         CanSILFunctionType coroutineType,
                         Explosion &yieldedValues);

  enum class AsyncFunctionArgumentIndex : unsigned {
    Context = 0,
  };

  void emitAsyncReturn(
      IRGenFunction &IGF, AsyncContextLayout &layout, CanSILFunctionType fnType,
      llvm::Optional<ArrayRef<llvm::Value *>> nativeResultArgs = llvm::None);

  void emitAsyncReturn(IRGenFunction &IGF, AsyncContextLayout &layout,
                       SILType funcResultTypeInContext,
                       CanSILFunctionType fnType, Explosion &result,
                       Explosion &error);
  void emitYieldOnceCoroutineResult(IRGenFunction &IGF, Explosion &result,
                                    SILType funcResultType, SILType returnResultType);

  Address emitAutoDiffCreateLinearMapContextWithType(
      IRGenFunction &IGF, llvm::Value *topLevelSubcontextMetatype);

  Address emitAutoDiffProjectTopLevelSubcontext(
      IRGenFunction &IGF, Address context);

  Address
  emitAutoDiffAllocateSubcontextWithType(IRGenFunction &IGF, Address context,
                                         llvm::Value *subcontextMetatype);

  FunctionPointer getFunctionPointerForDispatchCall(IRGenModule &IGM,
                                                    const FunctionPointer &fn);
  void forwardAsyncCallResult(IRGenFunction &IGF, CanSILFunctionType fnType,
                              AsyncContextLayout &layout, llvm::CallInst *call);

} // end namespace irgen
} // end namespace swift

#endif
