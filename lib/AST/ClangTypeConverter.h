//===-- ClangTypeConverter.h - Converting Swift types to C types-*- C++ -*-===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2019 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//
//
//  This file defines utilities for translating Swift types to C types.
//
//===----------------------------------------------------------------------===//

#ifndef SWIFT_AST_CLANG_TYPE_CONVERTER_H
#define SWIFT_AST_CLANG_TYPE_CONVERTER_H

#include "swift/AST/ASTContext.h"
#include "swift/AST/ClangModuleLoader.h"
#include "swift/AST/Type.h"
#include "swift/AST/TypeVisitor.h"
#include "clang/AST/ASTContext.h"
#include "clang/AST/Type.h"

namespace swift {

/// Compute C types corresponding to Swift AST types.
class ClangTypeConverter :
    public TypeVisitor<ClangTypeConverter, clang::QualType> {

  using super = TypeVisitor<ClangTypeConverter, clang::QualType>;

  llvm::DenseMap<Type, clang::QualType> Cache;
  llvm::DenseMap<const clang::Decl *, swift::Decl *> ReversedExportMap;

  bool StdlibTypesAreCached = false;

  ASTContext &Context;

  clang::ASTContext &ClangASTContext;

  const llvm::Triple Triple;

  ClangTypeConverter(const ClangTypeConverter &) = delete;
  ClangTypeConverter &operator=(const ClangTypeConverter &) = delete;

public:

  /// Create a ClangTypeConverter.
  ClangTypeConverter(ASTContext &ctx, clang::ASTContext &clangCtx,
                     llvm::Triple triple)
    : Context(ctx), ClangASTContext(clangCtx), Triple(triple)
  {
  };

  /// Compute the C function type for a Swift function type.
  ///
  /// It is the caller's responsibility to make sure this method is only
  /// called in the appropriate context. For example, it makes sense to use
  /// this method for the output type of a @convention(c) function.
  ///
  /// Since we do not check the context, the translation is unconditional.
  /// For example, String will automatically get translated to NSString
  /// when bridging is available.
  ///
  /// Additionally, the API is expected to be used only from 
  ///
  /// \returns The appropriate clang type on success, nullptr on failure.
  ///
  /// Precondition: The representation argument must be C-compatible.
  const clang::Type *getFunctionType(
    ArrayRef<AnyFunctionType::Param> params, Type resultTy,
    AnyFunctionType::Representation repr);

  /// Compute the C function type for a SIL function type.
  const clang::Type *getFunctionType(ArrayRef<SILParameterInfo> params,
                                     llvm::Optional<SILResultInfo> result,
                                     SILFunctionType::Representation repr);

  /// Check whether the given Clang declaration is an export of a Swift
  /// declaration introduced by this converter, and if so, return the original
  /// Swift declaration.
  Decl *getSwiftDeclForExportedClangDecl(const clang::Decl *decl) const;

  /// Translate Swift generic arguments to Clang C++ template arguments.
  ///
  /// \p templateArgs must be empty. \p templateParams and \p genericArgs must
  /// be equal in size.
  ///
  /// \returns nullptr if successful. If an error occurs, returns a list of
  /// types that couldn't be converted.
  std::unique_ptr<TemplateInstantiationError> getClangTemplateArguments(
      const clang::TemplateParameterList *templateParams,
      ArrayRef<Type> genericArgs,
      SmallVectorImpl<clang::TemplateArgument> &templateArgs);

private:
  friend ASTContext; // HACK: expose `convert` method to ASTContext

  clang::QualType convert(Type type);

  clang::QualType convertMemberType(NominalTypeDecl *DC,
                                    StringRef memberName);

  void registerExportedClangDecl(Decl *swiftDecl,
                                 const clang::Decl *clangDecl);

  clang::QualType reverseBuiltinTypeMapping(StructType *type);

  friend TypeVisitor<ClangTypeConverter, clang::QualType>;

  clang::QualType visitStructType(StructType *type);
  clang::QualType visitTupleType(TupleType *type);
  clang::QualType visitMetatypeType(MetatypeType *type);
  clang::QualType visitExistentialMetatypeType(ExistentialMetatypeType *type);
  clang::QualType visitProtocolType(ProtocolType *type);
  clang::QualType visitClassType(ClassType *type);
  clang::QualType visitBoundGenericClassType(BoundGenericClassType *type);
  clang::QualType visitBoundGenericType(BoundGenericType *type);
  clang::QualType visitEnumType(EnumType *type);
  clang::QualType visitFunctionType(FunctionType *type);
  clang::QualType visitProtocolCompositionType(ProtocolCompositionType *type);
  clang::QualType visitExistentialType(ExistentialType *type);
  clang::QualType visitBuiltinRawPointerType(BuiltinRawPointerType *type);
  clang::QualType visitBuiltinIntegerType(BuiltinIntegerType *type);
  clang::QualType visitBuiltinFloatType(BuiltinFloatType *type);
  clang::QualType visitArchetypeType(ArchetypeType *type);
  clang::QualType visitDependentMemberType(DependentMemberType *type);
  clang::QualType visitSILFunctionType(SILFunctionType *type);
  clang::QualType visitGenericTypeParamType(GenericTypeParamType *type);
  clang::QualType visitDynamicSelfType(DynamicSelfType *type);
  clang::QualType visitSILBlockStorageType(SILBlockStorageType *type);
  clang::QualType visitSugarType(SugarType *type);
  clang::QualType visitType(TypeBase *type);
  clang::QualType visit(Type type);
};

} // end namespace swift

#endif /* SWIFT_AST_CLANG_TYPE_CONVERTER_H */
