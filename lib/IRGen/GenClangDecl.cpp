//===--- GenClangDecl.cpp - Swift IRGen for imported Clang declarations ---===//
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

#include "IRGenModule.h"
#include "swift/AST/ASTContext.h"
#include "swift/AST/IRGenOptions.h"
#include "clang/AST/Decl.h"
#include "clang/AST/DeclCXX.h"
#include "clang/AST/DeclGroup.h"
#include "clang/AST/Expr.h"
#include "clang/AST/ExprCXX.h"
#include "clang/AST/GlobalDecl.h"
#include "clang/AST/RecursiveASTVisitor.h"
#include "clang/CodeGen/ModuleBuilder.h"
#include "clang/Sema/Sema.h"
#include "llvm/ADT/SmallPtrSet.h"

using namespace swift;
using namespace irgen;

namespace {
class ClangDeclFinder
    : public clang::RecursiveASTVisitor<ClangDeclFinder> {
  std::function<void(const clang::Decl *)> callback;
public:
  template <typename Fn>
  explicit ClangDeclFinder(Fn fn) : callback(fn) {}

  bool VisitDeclRefExpr(clang::DeclRefExpr *DRE) {
    if (isa<clang::FunctionDecl>(DRE->getDecl()) ||
        isa<clang::VarDecl>(DRE->getDecl())) {
      callback(DRE->getDecl());
    }

    return true;
  }

  bool VisitMemberExpr(clang::MemberExpr *ME) {
    if (isa<clang::FunctionDecl>(ME->getMemberDecl()) ||
        isa<clang::VarDecl>(ME->getMemberDecl()) || 
        isa<clang::FieldDecl>(ME->getMemberDecl())) {
      callback(ME->getMemberDecl());
    }
    return true;
  }

  bool VisitCXXConstructorDecl(clang::CXXConstructorDecl *CXXCD) {
    callback(CXXCD);
    for (clang::CXXCtorInitializer *CXXCI : CXXCD->inits()) {
      if (clang::FieldDecl *FD = CXXCI->getMember())
        callback(FD);
    }
    return true;
  }

  bool VisitCXXConstructExpr(clang::CXXConstructExpr *CXXCE) {
    callback(CXXCE->getConstructor());
    return true;
  }

  bool VisitCXXDeleteExpr(clang::CXXDeleteExpr *deleteExpr) {
    if (auto cxxRecord = deleteExpr->getDestroyedType()->getAsCXXRecordDecl())
      if (auto dtor = cxxRecord->getDestructor())
        callback(dtor);
    return true;
  }

  bool VisitVarDecl(clang::VarDecl *VD) {
    if (auto cxxRecord = VD->getType()->getAsCXXRecordDecl())
      if (auto dtor = cxxRecord->getDestructor())
        callback(dtor);

    return true;
  }

  bool VisitCXXBindTemporaryExpr(clang::CXXBindTemporaryExpr *BTE) {
    // This is a temporary value with a custom destructor. C++ will implicitly
    // call the destructor at some point. Make sure we emit IR for it.
    callback(BTE->getTemporary()->getDestructor());
    return true;
  }

  bool VisitCXXNewExpr(clang::CXXNewExpr *NE) {
    callback(NE->getOperatorNew());
    return true;
  }

  bool VisitBindingDecl(clang::BindingDecl *BD) {
    if (auto *holdingVar = BD->getHoldingVar()) {
      if (holdingVar->hasInit())
        TraverseStmt(holdingVar->getInit());
    }
    return true;
  }

  bool VisitCXXInheritedCtorInitExpr(clang::CXXInheritedCtorInitExpr *CIE) {
    if (auto ctor = CIE->getConstructor())
      callback(ctor);
    return true;
  }

  // Do not traverse unevaluated expressions. Doing to might result in compile
  // errors if we try to instantiate an un-instantiatable template.

  bool TraverseCXXNoexceptExpr(clang::CXXNoexceptExpr *NEE) { return true; }

  bool TraverseCXXTypeidExpr(clang::CXXTypeidExpr *TIE) {
    if (TIE->isPotentiallyEvaluated())
      clang::RecursiveASTVisitor<ClangDeclFinder>::TraverseCXXTypeidExpr(TIE);
    return true;
  }

  bool TraverseRequiresExpr(clang::RequiresExpr *RE) { return true; }

  // Do not traverse type locs, as they might contain expressions that reference
  // code that should not be instantiated and/or emitted.
  bool TraverseTypeLoc(clang::TypeLoc TL) { return true; }

  bool shouldVisitTemplateInstantiations() const { return true; }
  bool shouldVisitImplicitCode() const { return true; }
};

// If any (re)declaration of `decl` contains executable code, returns that
// redeclaration; otherwise, returns nullptr.
// In the case of a function, executable code is contained in the function
// definition. In the case of a variable, executable code can be contained in
// the initializer of the variable.
clang::Decl *getDeclWithExecutableCode(clang::Decl *decl) {
  if (auto fd = dyn_cast<clang::FunctionDecl>(decl)) {
    const clang::FunctionDecl *definition;
    if (fd->hasBody(definition)) {
      return const_cast<clang::FunctionDecl *>(definition);
    }

    // If this is a potentially not-yet-instantiated template, we might
    // still have a body.
    if (fd->getTemplateInstantiationPattern())
      return fd;
  } else if (auto vd = dyn_cast<clang::VarDecl>(decl)) {
    clang::VarDecl *initializingDecl = vd->getInitializingDeclaration();
    if (initializingDecl) {
      return initializingDecl;
    }
  } else if (auto fd = dyn_cast<clang::FieldDecl>(decl)) {
    if(fd->hasInClassInitializer()) {
      return fd;
    }
  }
  return nullptr;
}

} // end anonymous namespace

void IRGenModule::emitClangDecl(const clang::Decl *decl) {
  // Ignore this decl if we've seen it before.
  if (!GlobalClangDecls.insert(decl->getCanonicalDecl()).second)
    return;

  // Fast path for the case where `decl` doesn't contain executable code, so it
  // can't reference any other declarations that we would need to emit.
  if (getDeclWithExecutableCode(const_cast<clang::Decl *>(decl)) == nullptr) {
    ClangCodeGen->HandleTopLevelDecl(
                          clang::DeclGroupRef(const_cast<clang::Decl*>(decl)));
    return;
  }

  SmallVector<const clang::Decl *, 8> stack;
  stack.push_back(decl);

  auto callback = [&](const clang::Decl *D) {
    for (auto *DC = D->getDeclContext();; DC = DC->getParent()) {
      // Check that this is not a local declaration inside a function.
      if (DC->isFunctionOrMethod()) {
        return;
      }
      if (DC->isFileContext()) {
        break;
      }
      if (isa<clang::TagDecl>(DC)) {
        break;
      }
      if (isa<clang::LinkageSpecDecl>(DC)) {
        break;
      }
      D = cast<const clang::Decl>(DC);
    }
    if (!GlobalClangDecls.insert(D->getCanonicalDecl()).second) {
      return;
    }

    stack.push_back(D);
  };

  ClangDeclFinder refFinder(callback);

  auto &clangSema = Context.getClangModuleLoader()->getClangSema();

  while (!stack.empty()) {
    auto *next = const_cast<clang::Decl *>(stack.pop_back_val());

    // If this is a static member of a class, it might be defined out of line.
    // If the class is templated, the definition of its static member might be
    // templated as well. If it is, instantiate it here.
    if (auto var = dyn_cast<clang::VarDecl>(next)) {
      if (var->isStaticDataMember() &&
          var->getTemplateSpecializationKind() ==
              clang::TemplateSpecializationKind::TSK_ImplicitInstantiation)
        clangSema.InstantiateVariableDefinition(var->getLocation(), var);
    }

    // If a function calls another method in a class template specialization, we
    // need to instantiate that other function. Do that here.
    if (auto *fn = dyn_cast<clang::FunctionDecl>(next)) {
      // Make sure that this method is part of a class template specialization.
      if (fn->getTemplateInstantiationPattern())
        clangSema.InstantiateFunctionDefinition(fn->getLocation(), fn);
    }

    if (clang::Decl *executableDecl = getDeclWithExecutableCode(next)) {
        refFinder.TraverseDecl(executableDecl);
        next = executableDecl;
    }

    // Unfortunately, implicitly defined CXXDestructorDecls don't have a real
    // body, so we need to traverse these manually.
    if (auto *dtor = dyn_cast<clang::CXXDestructorDecl>(next)) {
      if (dtor->isImplicit() || dtor->hasBody()) {
        auto cxxRecord = dtor->getParent();

        for (auto field : cxxRecord->fields()) {
          if (auto fieldCxxRecord = field->getType()->getAsCXXRecordDecl())
            if (auto *fieldDtor = fieldCxxRecord->getDestructor())
              callback(fieldDtor);
        }

        for (auto base : cxxRecord->bases()) {
          if (auto baseCxxRecord = base.getType()->getAsCXXRecordDecl())
            if (auto *baseDtor = baseCxxRecord->getDestructor())
              callback(baseDtor);
        }
      }
    }

    // If something from a C++ class is used, emit all virtual methods of this
    // class because they might be emitted in the vtable even if not used
    // directly from Swift.
    if (auto *record = dyn_cast<clang::CXXRecordDecl>(next->getDeclContext())) {
      for (auto *method : record->methods()) {
        if (method->isVirtual()) {
          callback(method);
        }
      }
    }

    if (auto var = dyn_cast<clang::VarDecl>(next))
      if (!var->isFileVarDecl())
        continue;
    if (isa<clang::FieldDecl>(next)) {
      continue;
    }

    ClangCodeGen->HandleTopLevelDecl(clang::DeclGroupRef(next));
  }
}

llvm::Constant *
IRGenModule::getAddrOfClangGlobalDecl(clang::GlobalDecl global,
                                      ForDefinition_t forDefinition) {
  // Register the decl with the clang code generator.
  if (auto decl = global.getDecl())
    emitClangDecl(decl);

  return ClangCodeGen->GetAddrOfGlobal(global, (bool) forDefinition);
}

void IRGenModule::finalizeClangCodeGen() {
  // FIXME: We try to avoid looking for PragmaCommentDecls unless we need to,
  // since clang::DeclContext::decls_begin() can trigger expensive
  // de-serialization.
  if (Triple.isWindowsMSVCEnvironment() || Triple.isWindowsItaniumEnvironment() ||
      IRGen.Opts.LLVMLTOKind != IRGenLLVMLTOKind::None) {
    // Ensure that code is emitted for any `PragmaCommentDecl`s. (These are
    // always guaranteed to be directly below the TranslationUnitDecl.)
    // In Clang, this happens automatically during the Sema phase, but here we
    // need to take care of it manually because our Clang CodeGenerator is not
    // attached to Clang Sema as an ASTConsumer.
    for (const auto *D : ClangASTContext->getTranslationUnitDecl()->decls()) {
      if (const auto *PCD = dyn_cast<clang::PragmaCommentDecl>(D)) {
        emitClangDecl(PCD);
      }
    }
  }

  ClangCodeGen->HandleTranslationUnit(
      *const_cast<clang::ASTContext *>(ClangASTContext));
}
