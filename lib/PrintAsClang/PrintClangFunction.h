//===--- PrintClangFunction.h - Printer for C/C++ functions -----*- C++ -*-===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2022 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

#ifndef SWIFT_PRINTASCLANG_PRINTCLANGFUNCTION_H
#define SWIFT_PRINTASCLANG_PRINTCLANGFUNCTION_H

#include "OutputLanguageMode.h"
#include "swift/AST/Type.h"
#include "swift/Basic/LLVM.h"
#include "swift/ClangImporter/ClangImporter.h"
#include "swift/IRGen/GenericRequirement.h"
#include "swift/IRGen/IRABIDetailsProvider.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/MapVector.h"
#include "llvm/ADT/Optional.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/raw_ostream.h"

namespace swift {

class AbstractFunctionDecl;
class AccessorDecl;
class AnyFunctionType;
class FuncDecl;
class GenericTypeParamType;
class ModuleDecl;
class NominalTypeDecl;
class LoweredFunctionSignature;
class ParamDecl;
class ParameterList;
class PrimitiveTypeMapping;
class SwiftToClangInteropContext;
class DeclAndTypePrinter;

struct ClangRepresentation {
  enum Kind { representable, unsupported };

  ClangRepresentation(Kind kind) : kind(kind) {}

  /// Returns true if the given Swift node is unsupported in Clang in any
  /// language mode.
  bool isUnsupported() const { return kind == unsupported; }

  const ClangRepresentation &merge(ClangRepresentation other) {
    if (kind != unsupported)
      kind = other.kind;
    return *this;
  }

private:
  Kind kind;
};

/// Responsible for printing a Swift function decl or type in C or C++ mode, to
/// be included in a Swift module's generated clang header.
class DeclAndTypeClangFunctionPrinter {
public:
  DeclAndTypeClangFunctionPrinter(raw_ostream &os, raw_ostream &cPrologueOS,
                                  PrimitiveTypeMapping &typeMapping,
                                  SwiftToClangInteropContext &interopContext,
                                  DeclAndTypePrinter &declPrinter)
      : os(os), cPrologueOS(cPrologueOS), typeMapping(typeMapping),
        interopContext(interopContext), declPrinter(declPrinter) {}

  /// What kind of function signature should be emitted for the given Swift
  /// function.
  enum class FunctionSignatureKind {
    /// Emit a signature for the C function prototype.
    CFunctionProto,
    /// Emit a signature for the inline C++ function thunk.
    CxxInlineThunk
  };

  /// Optional modifiers that can be applied to function signature.
  struct FunctionSignatureModifiers {
    /// Additional qualifier to add before the function's name.
    const NominalTypeDecl *qualifierContext = nullptr;
    bool isStatic = false;
    bool isInline = false;
    bool isConst = false;
    bool isNoexcept = false;
    bool hasSymbolUSR = true;
    /// Specific declaration that should be used to emit the symbol's
    /// USR instead of the original function declaration.
    const ValueDecl *symbolUSROverride = nullptr;

    FunctionSignatureModifiers() {}
  };

  /// Print the C function declaration or the C++ function thunk that
  /// corresponds to the given function declaration.
  ///
  /// \return value describing in which Clang language mode the function is
  /// supported, if any.
  ClangRepresentation printFunctionSignature(
      const AbstractFunctionDecl *FD, const LoweredFunctionSignature &signature,
      StringRef name, Type resultTy, FunctionSignatureKind kind,
      FunctionSignatureModifiers modifiers = {});

  /// Print the body of the inline C++ function thunk that calls the underlying
  /// Swift function.
  void printCxxThunkBody(
      const AbstractFunctionDecl *FD, const LoweredFunctionSignature &signature,
      StringRef swiftSymbolName, const NominalTypeDecl *typeDeclContext,
      const ModuleDecl *moduleContext, Type resultTy,
      const ParameterList *params, bool hasThrows = false,
      const AnyFunctionType *funcType = nullptr, bool isStaticMethod = false,
      llvm::Optional<IRABIDetailsProvider::MethodDispatchInfo> dispatchInfo =
          llvm::None);

  /// Print the Swift method as C++ method declaration/definition, including
  /// constructors.
  void printCxxMethod(
      DeclAndTypePrinter &declAndTypePrinter,
      const NominalTypeDecl *typeDeclContext, const AbstractFunctionDecl *FD,
      const LoweredFunctionSignature &signature, StringRef swiftSymbolName,
      Type resultTy, bool isStatic, bool isDefinition,
      llvm::Optional<IRABIDetailsProvider::MethodDispatchInfo> dispatchInfo);

  /// Print the C++ getter/setter method signature.
  void printCxxPropertyAccessorMethod(
      DeclAndTypePrinter &declAndTypePrinter,
      const NominalTypeDecl *typeDeclContext, const AccessorDecl *accessor,
      const LoweredFunctionSignature &signature, StringRef swiftSymbolName,
      Type resultTy, bool isStatic, bool isDefinition,
      llvm::Optional<IRABIDetailsProvider::MethodDispatchInfo> dispatchInfo);

  /// Print the C++ subscript method.
  void printCxxSubscriptAccessorMethod(
      DeclAndTypePrinter &declAndTypePrinter,
      const NominalTypeDecl *typeDeclContext, const AccessorDecl *accessor,
      const LoweredFunctionSignature &signature, StringRef swiftSymbolName,
      Type resultTy, bool isDefinition,
      llvm::Optional<IRABIDetailsProvider::MethodDispatchInfo> dispatchInfo);

  /// Print Swift type as C/C++ type, as the return type of a C/C++ function.
  ClangRepresentation
  printClangFunctionReturnType(Type ty, OptionalTypeKind optKind,
                               ModuleDecl *moduleContext,
                               OutputLanguageMode outputLang);

  static void printGenericReturnSequence(
      raw_ostream &os, const GenericTypeParamType *gtpt,
      llvm::function_ref<void(StringRef)> invocationPrinter,
      llvm::Optional<StringRef> initializeWithTakeFromValue = llvm::None);

  using PrinterTy =
      llvm::function_ref<void(llvm::MapVector<Type, std::string> &)>;

  /// Print generated C++ helper function
  void printCustomCxxFunction(const SmallVector<Type> &neededTypes,
                              bool NeedsReturnTypes,
                              PrinterTy retTypeAndNamePrinter,
                              PrinterTy paramPrinter, bool isConstFunc,
                              PrinterTy bodyPrinter, ValueDecl *valueDecl,
                              ModuleDecl *emittedModule,
                              raw_ostream &outOfLineOS);

  static ClangRepresentation
  getTypeRepresentation(PrimitiveTypeMapping &typeMapping,
                        SwiftToClangInteropContext &interopContext,
                        DeclAndTypePrinter &declPrinter,
                        const ModuleDecl *emittedModule, Type ty);

private:
  void printCxxToCFunctionParameterUse(Type type, StringRef name,
                                       const ModuleDecl *moduleContext,
                                       bool isInOut, bool isIndirect,
                                       std::string directTypeEncoding,
                                       bool isSelf);

  // Print out the full type specifier that refers to the
  // _impl::_impl_<typename> C++ class for the given Swift type.
  void printTypeImplTypeSpecifier(Type type, const ModuleDecl *moduleContext);

  bool hasKnownOptionalNullableCxxMapping(Type type);

  raw_ostream &os;
  raw_ostream &cPrologueOS;
  PrimitiveTypeMapping &typeMapping;
  SwiftToClangInteropContext &interopContext;
  DeclAndTypePrinter &declPrinter;
};

} // end namespace swift

#endif
