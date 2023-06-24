//===- GenericSpecializationMangler.h - generic specializations -*- C++ -*-===//
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

#ifndef SWIFT_SIL_UTILS_GENERICSPECIALIZATIONMANGLER_H
#define SWIFT_SIL_UTILS_GENERICSPECIALIZATIONMANGLER_H

#include "swift/AST/ASTMangler.h"
#include "swift/Basic/NullablePtr.h"
#include "swift/Demangling/Demangler.h"
#include "swift/SIL/SILFunction.h"

namespace swift {
namespace Mangle {

enum class SpecializationKind : uint8_t {
  Generic,
  NotReAbstractedGeneric,
  FunctionSignature,
};

/// Inject SpecializationPass into the Mangle namespace.
using SpecializationPass = Demangle::SpecializationPass;

/// The base class for specialization mangles.
class SpecializationMangler : public Mangle::ASTMangler {
protected:
  /// The specialization pass.
  SpecializationPass Pass;

  IsSerialized_t Serialized;

  /// The original function which is specialized.
  SILFunction *Function;
  std::string FunctionName;

  llvm::SmallVector<char, 32> ArgOpStorage;
  llvm::raw_svector_ostream ArgOpBuffer;

protected:
  SpecializationMangler(SpecializationPass P, IsSerialized_t Serialized,
                        SILFunction *F)
      : Pass(P), Serialized(Serialized), Function(F),
        ArgOpBuffer(ArgOpStorage) {}

  SpecializationMangler(SpecializationPass P, IsSerialized_t Serialized,
                        std::string functionName)
      : Pass(P), Serialized(Serialized), Function(nullptr),
        FunctionName(functionName), ArgOpBuffer(ArgOpStorage) {}

  void beginMangling();

  /// Finish the mangling of the symbol and return the mangled name.
  std::string finalize();

  void appendSpecializationOperator(StringRef Op) {
    appendOperator(Op, StringRef(ArgOpStorage.data(), ArgOpStorage.size()));
  }
};

// The mangler for specialized generic functions.
class GenericSpecializationMangler : public SpecializationMangler {

  GenericSpecializationMangler(std::string origFuncName)
      : SpecializationMangler(SpecializationPass::GenericSpecializer,
                              IsNotSerialized, origFuncName) {}

  GenericSignature getGenericSignature() {
    assert(Function && "Need a SIL function to get a generic signature");
    return Function->getLoweredFunctionType()->getInvocationGenericSignature();
  }

  void appendSubstitutions(GenericSignature sig, SubstitutionMap subs);

  std::string manglePrespecialized(GenericSignature sig,
                                      SubstitutionMap subs);

public:
  GenericSpecializationMangler(SILFunction *F, IsSerialized_t Serialized)
      : SpecializationMangler(SpecializationPass::GenericSpecializer,
                              Serialized, F) {}

  std::string mangleNotReabstracted(SubstitutionMap subs,
                                    bool metatyeParamsRemoved);

  /// Mangle a generic specialization with re-abstracted parameters.
  ///
  /// Re-abstracted means that indirect (generic) parameters/returns are
  /// converted to direct parameters/returns.
  ///
  /// This is the default for generic specializations.
  ///
  /// \param alternativeMangling   true for specialized functions with a
  ///                              differet resilience expansion.
  /// \param metatyeParamsRemoved  true if non-generic metatype parameters are
  ///                              removed in the specialized function.
  std::string mangleReabstracted(SubstitutionMap subs, bool alternativeMangling,
                                 bool metatyeParamsRemoved = false);

  std::string mangleForDebugInfo(GenericSignature sig, SubstitutionMap subs,
                                 bool forInlining);

  std::string manglePrespecialized(SubstitutionMap subs) {
    return manglePrespecialized(getGenericSignature(), subs);
  }
                                    
  static std::string manglePrespecialization(std::string unspecializedName,
                                             GenericSignature genericSig,
                                             GenericSignature specializedSig);
};

} // end namespace Mangle
} // end namespace swift

#endif
