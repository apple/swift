//===--- SpecializedEmitter.h - Special emitters for builtin ----*- C++ -*-===//
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
// Interface to the code for specially emitting builtin functions.
//
//===----------------------------------------------------------------------===//

#ifndef SWIFT_LOWERING_SPECIALIZEDEMITTER_H
#define SWIFT_LOWERING_SPECIALIZEDEMITTER_H

#include "swift/Basic/LLVM.h"
#include "swift/AST/Identifier.h"
#include "swift/AST/Types.h"

namespace swift {
class Expr;
struct SILDeclRef;
class SILLocation;
class SILModule;
  
namespace Lowering {
class ManagedValue;
class SGFContext;
class SILGenFunction;
class SILGenModule;
class PreparedArguments;

/// Some kind of specialized emitter for a builtin function.
class SpecializedEmitter {
public:
  /// A special function for emitting a call before the arguments
  /// have already been emitted.
  using EarlyEmitter = ManagedValue (SILGenFunction &,
                                     SILLocation,
                                     SubstitutionMap,
                                     PreparedArguments &&args,
                                     SGFContext);

  /// A special function for emitting a call after the arguments
  /// have already been emitted.
  using LateEmitter = ManagedValue (SILGenFunction &,
                                    SILLocation,
                                    SubstitutionMap,
                                    ArrayRef<ManagedValue>,
                                    SGFContext);

  enum class Kind {
    /// This is a builtin function that will be specially handled
    /// downstream, but doesn't require special treatment at the
    /// SILGen level.
    NamedBuiltin,

    /// This is a builtin function that needs to be specially
    /// handled in SILGen and which needs to be given the original
    /// r-value expression.
    EarlyEmitter,

    /// This is a builtin function that needs to be specially
    /// handled in SILGen, but which can be passed normally-emitted
    /// arguments.
    LateEmitter,
  };

private:
  Kind TheKind;
  union {
    EarlyEmitter *TheEarlyEmitter;
    LateEmitter *TheLateEmitter;
    Identifier TheBuiltinName;
  };

public:
  /*implicit*/ SpecializedEmitter(Identifier builtinName)
    : TheKind(Kind::NamedBuiltin), TheBuiltinName(builtinName) {}

  /*implicit*/ SpecializedEmitter(EarlyEmitter *emitter)
    : TheKind(Kind::EarlyEmitter), TheEarlyEmitter(emitter) {}

  /*implicit*/ SpecializedEmitter(LateEmitter *emitter)
    : TheKind(Kind::LateEmitter), TheLateEmitter(emitter) {}

  /// Try to find an appropriate emitter for the given declaration.
  static llvm::Optional<SpecializedEmitter> forDecl(SILGenModule &SGM,
                                                    SILDeclRef decl);

  bool isEarlyEmitter() const { return TheKind == Kind::EarlyEmitter; }
  EarlyEmitter *getEarlyEmitter() const {
    assert(isEarlyEmitter());
    return TheEarlyEmitter;
  }

  bool isLateEmitter() const { return TheKind == Kind::LateEmitter; }
  LateEmitter *getLateEmitter() const {
    assert(isLateEmitter());
    return TheLateEmitter;
  }

  bool isNamedBuiltin() const { return TheKind == Kind::NamedBuiltin; }
  Identifier getBuiltinName() const {
    assert(isNamedBuiltin());
    return TheBuiltinName;
  }
};

} // end namespace Lowering
} // end namespace swift

#endif
