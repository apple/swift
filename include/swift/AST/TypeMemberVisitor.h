//===--- TypeMemberVisitor.h - ASTVisitor specialization --------*- C++ -*-===//
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
// This file defines the curiously-recursive TypeMemberVisitor class
// and a few specializations thereof.
//
//===----------------------------------------------------------------------===//

#ifndef SWIFT_AST_TYPEMEMBERVISITOR_H
#define SWIFT_AST_TYPEMEMBERVISITOR_H

#include "swift/AST/ASTVisitor.h"

namespace swift {
  
/// TypeMemberVisitor - This is a convenience adapter of DeclVisitor
/// which filters out a few common declaration kinds that are never
/// members of nominal types.
template<typename ImplClass, typename RetTy = void> 
class TypeMemberVisitor : public DeclVisitor<ImplClass, RetTy> {
protected:
  ImplClass &asImpl() { return static_cast<ImplClass&>(*this); }

public:
#define BAD_MEMBER(KIND) \
  RetTy visit##KIND##Decl(KIND##Decl *D) { \
    llvm_unreachable(#KIND "Decls cannot be members of nominal types"); \
  }
  BAD_MEMBER(Extension)
  BAD_MEMBER(Import)
  BAD_MEMBER(Protocol)
  BAD_MEMBER(TopLevelCode)
  BAD_MEMBER(Operator)
  BAD_MEMBER(PrecedenceGroup)
  BAD_MEMBER(Macro)

  // The children of these are automatically inserted into the
  // surrounding context.
  RetTy visitIfConfigDecl(IfConfigDecl *D) {
    return RetTy();
  }

  // These decls are disregarded.
  RetTy visitPoundDiagnosticDecl(PoundDiagnosticDecl *D) {
    return RetTy();
  }

  RetTy visitMacroExpansionDecl(MacroExpansionDecl *D) {
    // Expansion already visited as auxiliary decls.
    return RetTy();
  }

  /// A convenience method to visit all the members.
  void visitMembers(NominalTypeDecl *D) {
    for (Decl *member : D->getAllMembers()) {
      asImpl().visit(member);
    }
  }

  /// A convenience method to visit all the members in the implementation
  /// context.
  ///
  /// \seealso IterableDeclContext::getImplementationContext()
  void visitImplementationMembers(NominalTypeDecl *D) {
    for (Decl *member : D->getImplementationContext()->getMembers()) {
      asImpl().visit(member);
    }
    
    // If this is a main-interface @_objcImplementation extension and the class
    // has a synthesized destructor, visit it now.
    if (auto cd = dyn_cast_or_null<ClassDecl>(D)) {
      auto dd = cd->getDestructor();
      if (dd->getDeclContext() == cd && cd->getImplementationContext() != cd)
        asImpl().visit(dd);
    }
  }
};

template<typename ImplClass, typename RetTy = void>
class ClassMemberVisitor : public TypeMemberVisitor<ImplClass, RetTy> {
public:
  BAD_MEMBER(EnumElement)
  BAD_MEMBER(EnumCase)

  void visitMembers(ClassDecl *D) {
    TypeMemberVisitor<ImplClass, RetTy>::visitMembers(D);
  }

  void visitImplementationMembers(ClassDecl *D) {
    TypeMemberVisitor<ImplClass, RetTy>::visitImplementationMembers(D);
  }
};

#undef BAD_MEMBER

} // end namespace swift
  
#endif
