//===--- NameLookup.cpp - Swift Name Lookup Routines ----------------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2018 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//
//
// This file implements interfaces for performing name lookup.
//
//===----------------------------------------------------------------------===//

#include "swift/AST/NameLookup.h"
#include "swift/AST/ASTContext.h"
#include "swift/AST/ASTVisitor.h"
#include "swift/AST/ASTWalker.h"
#include "swift/AST/ClangModuleLoader.h"
#include "swift/AST/DebuggerClient.h"
#include "swift/AST/ExistentialLayout.h"
#include "swift/AST/GenericParamList.h"
#include "swift/AST/GenericSignature.h"
#include "swift/AST/ImportCache.h"
#include "swift/AST/Initializer.h"
#include "swift/AST/LazyResolver.h"
#include "swift/AST/MacroDeclaration.h"
#include "swift/AST/ModuleNameLookup.h"
#include "swift/AST/NameLookupRequests.h"
#include "swift/AST/ParameterList.h"
#include "swift/AST/PotentialMacroExpansions.h"
#include "swift/AST/PropertyWrappers.h"
#include "swift/AST/SourceFile.h"
#include "swift/AST/TypeCheckRequests.h"
#include "swift/Basic/Debug.h"
#include "swift/Basic/STLExtras.h"
#include "swift/Basic/SourceManager.h"
#include "swift/Basic/Statistic.h"
#include "swift/ClangImporter/ClangImporterRequests.h"
#include "swift/Parse/Lexer.h"
#include "swift/Strings.h"
#include "clang/AST/DeclObjC.h"
#include "clang/Basic/Specifiers.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/TinyPtrVector.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"

#define DEBUG_TYPE "namelookup"

using namespace swift;
using namespace swift::namelookup;

void VisibleDeclConsumer::anchor() {}
void VectorDeclConsumer::anchor() {}

ValueDecl *LookupResultEntry::getBaseDecl() const {
  if (BaseDC == nullptr)
    return nullptr;

  return BaseDecl;
}

void LookupResult::filter(
    llvm::function_ref<bool(LookupResultEntry, bool)> pred) {
  size_t index = 0;
  size_t originalFirstOuter = IndexOfFirstOuterResult;
  Results.erase(std::remove_if(Results.begin(), Results.end(),
                               [&](LookupResultEntry result) -> bool {
                                 auto isInner = index < originalFirstOuter;
                                 ++index;
                                 if (pred(result, !isInner))
                                   return false;

                                 // Need to remove this, which means, if it is
                                 // an inner result, the outer results need to
                                 // shift down.
                                 if (isInner)
                                   --IndexOfFirstOuterResult;
                                 return true;
                               }),
                Results.end());
}

void LookupResult::shiftDownResults() {
  // Remove inner results.
  Results.erase(Results.begin(), Results.begin() + IndexOfFirstOuterResult);
  IndexOfFirstOuterResult = 0;

  if (Results.empty())
    return;

  // Compute IndexOfFirstOuterResult.
  const DeclContext *dcInner = Results.front().getValueDecl()->getDeclContext();
  for (auto &&result : Results) {
    const DeclContext *dc = result.getValueDecl()->getDeclContext();
    if (dc == dcInner ||
        (dc->isModuleScopeContext() && dcInner->isModuleScopeContext()))
      ++IndexOfFirstOuterResult;
    else
      break;
  }
}

void swift::simple_display(llvm::raw_ostream &out,
                           UnqualifiedLookupOptions options) {
  using Flag = std::pair<UnqualifiedLookupFlags, StringRef>;
  Flag possibleFlags[] = {
      {UnqualifiedLookupFlags::AllowProtocolMembers, "AllowProtocolMembers"},
      {UnqualifiedLookupFlags::IgnoreAccessControl, "IgnoreAccessControl"},
      {UnqualifiedLookupFlags::IncludeOuterResults, "IncludeOuterResults"},
      {UnqualifiedLookupFlags::TypeLookup, "TypeLookup"},
  };

  auto flagsToPrint = llvm::make_filter_range(
      possibleFlags, [&](Flag flag) { return options.contains(flag.first); });

  out << "{ ";
  interleave(
      flagsToPrint, [&](Flag flag) { out << flag.second; },
      [&] { out << ", "; });
  out << " }";
}

void DebuggerClient::anchor() {}

void AccessFilteringDeclConsumer::foundDecl(
    ValueDecl *D, DeclVisibilityKind reason,
    DynamicLookupInfo dynamicLookupInfo) {
  if (!D->isAccessibleFrom(DC))
    return;

  ChainedConsumer.foundDecl(D, reason, dynamicLookupInfo);
}

void UsableFilteringDeclConsumer::foundDecl(ValueDecl *D,
    DeclVisibilityKind reason, DynamicLookupInfo dynamicLookupInfo) {
  // Skip when Loc is within the decl's own initializer
  if (auto *VD = dyn_cast<VarDecl>(D)) {
    Expr *init = VD->getParentInitializer();
    if (auto *PD = dyn_cast<ParamDecl>(D)) {
      init = PD->getStructuralDefaultExpr();
    }

    // Only check if the VarDecl has the same (or parent) context to avoid
    // grabbing the end location for every decl with an initializer
    if (init != nullptr) {
      auto *varContext = VD->getDeclContext();
      if (DC == varContext || DC->isChildContextOf(varContext)) {
        auto initRange = Lexer::getCharSourceRangeFromSourceRange(
            SM, init->getSourceRange());
        if (initRange.isValid() && initRange.contains(Loc))
          return;
      }
    }
  }

  switch (reason) {
  case DeclVisibilityKind::LocalVariable:
  case DeclVisibilityKind::FunctionParameter:
    // Skip if Loc is before the found decl if the decl is a var/let decl.
    // Type and func decls can be referenced before its declaration, or from
    // within nested type decls.
    if (isa<VarDecl>(D)) {
      if (reason == DeclVisibilityKind::LocalVariable) {
        // Workaround for fast-completion. A loc in the current context might be
        // in a loc
        auto tmpLoc = Loc;
        if (D->getDeclContext() != DC) {
          if (auto *contextD = DC->getAsDecl())
            tmpLoc = contextD->getStartLoc();
        }
        auto declLoc = DC->getParentModule()->getOriginalLocation(D->getLoc()).second;
        if (!SM.isBeforeInBuffer(declLoc, tmpLoc))
          return;
      }

      // A type context cannot close over values defined in outer type contexts.
      if (D->getDeclContext()->getInnermostTypeContext() != typeContext)
        return;
    }
    break;

  case DeclVisibilityKind::MemberOfOutsideNominal:
    // A type context cannot close over members of outer type contexts, except
    // for type decls.
    if (!isa<TypeDecl>(D) && !D->isStatic())
      return;
    break;

  case DeclVisibilityKind::MemberOfCurrentNominal:
  case DeclVisibilityKind::MemberOfSuper:
  case DeclVisibilityKind::MemberOfProtocolConformedToByCurrentNominal:
  case DeclVisibilityKind::MemberOfProtocolDerivedByCurrentNominal:
  case DeclVisibilityKind::DynamicLookup:
    // Members on 'Self' including inherited/derived ones are always usable.
    break;

  case DeclVisibilityKind::GenericParameter:
    // Generic params are type decls and are always usable from nested context.
    break;

  case DeclVisibilityKind::VisibleAtTopLevel:
    // The rest of the file is currently skipped, so no need to check
    // decl location for VisibleAtTopLevel.
    break;
  }

  // Filter out shadowed decls. Do this for only usable values even though
  // unusable values actually can shadow outer values, because compilers might
  // be able to diagnose it with fix-it to add the qualification. E.g.
  //   func foo(global: T) {}
  //   struct Outer {
  //     func foo(outer: T) {}
  //     func test() {
  //       struct Inner {
  //         func test() {
  //           <HERE>
  //         }
  //       }
  //     }
  //   }
  // In this case 'foo(global:)' is shadowed by 'foo(outer:)', but 'foo(outer:)'
  // is _not_ usable because it's outside the current type context, whereas
  // 'foo(global:)' is still usable with 'ModuleName.' qualification.
  // FIXME: (for code completion,) If a global value or a static type member is
  // shadowd, we should suggest it with prefix (e.g. 'ModuleName.value').
  auto inserted = SeenNames.insert({D->getBaseName(), {D, reason}});
  if (!inserted.second) {
    auto shadowingReason = inserted.first->second.second;
    auto *shadowingD = inserted.first->second.first;

    // A type decl cannot have overloads, and shadows everything outside the
    // scope.
    if (isa<TypeDecl>(shadowingD))
      return;

    switch (shadowingReason) {
    case DeclVisibilityKind::LocalVariable:
    case DeclVisibilityKind::FunctionParameter:
      // Local func and var/let with a conflicting name.
      //   func foo() {
      //     func value(arg: Int) {}
      //     var value = ""
      //   }
      // In this case, 'var value' wins, regardless of their source order.
      // So, for confilicting local values in the same decl context, even if the
      // 'var value' is reported after 'func value', don't shadow it, but we
      // shadow everything with the name after that.
      if (reason == DeclVisibilityKind::LocalVariable &&
          isa<VarDecl>(D) && !isa<VarDecl>(shadowingD) &&
          shadowingD->getDeclContext() == D->getDeclContext()) {
        // Replace the shadowing decl so we shadow subsequent conflicting decls.
        inserted.first->second = {D, reason};
        break;
      }

      // Otherwise, a local value shadows everything outside the scope.
      return;

    case DeclVisibilityKind::GenericParameter:
      // A Generic parameter is a type name. It shadows everything outside the
      // generic context.
      return;

    case DeclVisibilityKind::MemberOfCurrentNominal:
    case DeclVisibilityKind::MemberOfSuper:
    case DeclVisibilityKind::MemberOfProtocolConformedToByCurrentNominal:
    case DeclVisibilityKind::MemberOfProtocolDerivedByCurrentNominal:
    case DeclVisibilityKind::DynamicLookup:
      switch (reason) {
      case DeclVisibilityKind::MemberOfCurrentNominal:
      case DeclVisibilityKind::MemberOfSuper:
      case DeclVisibilityKind::MemberOfProtocolConformedToByCurrentNominal:
      case DeclVisibilityKind::MemberOfProtocolDerivedByCurrentNominal:
      case DeclVisibilityKind::DynamicLookup:
        // Members on the current type context don't shadow members with the
        // same base name on the current type contxt. They are overloads.
        break;
      default:
        // Members of a type context shadows values/types outside.
        return;
      }
      break;

    case DeclVisibilityKind::MemberOfOutsideNominal:
      // For static values, it's unclear _which_ type context (i.e. this type,
      // super classes, conforming protocols) this decl was found in. For now,
      // consider all the outer nominals are the same.

      if (reason == DeclVisibilityKind::MemberOfOutsideNominal)
        break;

      // Values outside the nominal are shadowed.
      return;

    case DeclVisibilityKind::VisibleAtTopLevel:
      // Top level decls don't shadow anything.
      // Well, that's not true. Decls in the current module shadows decls in
      // the imported modules. But we don't care them here.
      break;
    }
  }

  ChainedConsumer.foundDecl(D, reason, dynamicLookupInfo);
}

void LookupResultEntry::print(llvm::raw_ostream& out) const {
  getValueDecl()->print(out);
  if (auto dc = getBaseDecl()) {
    out << "\nbase: ";
    dc->print(out);
    out << "\n";
  } else
    out << "\n(no-base)\n";
}


bool swift::removeOverriddenDecls(SmallVectorImpl<ValueDecl*> &decls) {
  if (decls.size() < 2)
    return false;

  llvm::SmallPtrSet<ValueDecl*, 8> overridden;
  for (auto decl : decls) {
    // Don't look at the overrides of operators in protocols. The global
    // lookup of operators means that we can find overriding operators that
    // aren't relevant to the types in hand, and will fail to type check.
    if (isa<ProtocolDecl>(decl->getDeclContext())) {
      if (auto func = dyn_cast<FuncDecl>(decl))
        if (func->isOperator())
          continue;
    }

    while (auto overrides = decl->getOverriddenDecl()) {
      overridden.insert(overrides);

      // Because initializers from Objective-C base classes have greater
      // visibility than initializers written in Swift classes, we can
      // have a "break" in the set of declarations we found, where
      // C.init overrides B.init overrides A.init, but only C.init and
      // A.init are in the chain. Make sure we still remove A.init from the
      // set in this case.
      if (decl->getBaseName() == DeclBaseName::createConstructor()) {
        /// FIXME: Avoid the possibility of an infinite loop by fixing the root
        ///        cause instead (incomplete circularity detection).
        assert(decl != overrides && "Circular class inheritance?");
        decl = overrides;
        continue;
      }

      break;
    }
  }

  // If no methods were overridden, we're done.
  if (overridden.empty()) return false;

  // Erase any overridden declarations
  bool anyOverridden = false;
  decls.erase(std::remove_if(decls.begin(), decls.end(),
                             [&](ValueDecl *decl) -> bool {
                               if (overridden.count(decl) > 0) {
                                 anyOverridden = true;
                                 return true;
                               }

                               return false;
                             }),
              decls.end());

  return anyOverridden;
}

enum class ConstructorComparison {
  Worse,
  Same,
  Better,
};

/// Determines whether \p ctor1 is a "better" initializer than \p ctor2.
static ConstructorComparison compareConstructors(ConstructorDecl *ctor1,
                                                 ConstructorDecl *ctor2,
                                                 const swift::ASTContext &ctx) {
  bool available1 = !ctor1->getAttrs().isUnavailable(ctx);
  bool available2 = !ctor2->getAttrs().isUnavailable(ctx);

  // An unavailable initializer is always worse than an available initializer.
  if (available1 < available2)
    return ConstructorComparison::Worse;

  if (available1 > available2)
    return ConstructorComparison::Better;

  CtorInitializerKind kind1 = ctor1->getInitKind();
  CtorInitializerKind kind2 = ctor2->getInitKind();

  if (kind1 > kind2)
    return ConstructorComparison::Worse;

  if (kind1 < kind2)
    return ConstructorComparison::Better;

  return ConstructorComparison::Same;
}

/// Given a set of declarations whose names and interface types have matched,
/// figure out which of these declarations have been shadowed by others.
template <typename T>
static void recordShadowedDeclsAfterTypeMatch(
                              ArrayRef<T> decls,
                              const DeclContext *dc,
                              llvm::SmallPtrSetImpl<T> &shadowed) {
  assert(decls.size() > 1 && "Nothing collided");

  // Compare each declaration to every other declaration. This is
  // unavoidably O(n^2) in the number of declarations, but because they
  // all have the same signature, we expect n to remain small.
  auto *curModule = dc->getParentModule();
  ASTContext &ctx = curModule->getASTContext();
  auto &imports = ctx.getImportCache();

  for (unsigned firstIdx : indices(decls)) {
    auto firstDecl = decls[firstIdx];
    auto firstModule = firstDecl->getModuleContext();
    bool firstTopLevel = firstDecl->getDeclContext()->isModuleScopeContext();

    auto name = firstDecl->getBaseName();

    auto isShadowed = [&](ArrayRef<ImportPath::Access> paths) {
      for (auto path : paths) {
        if (path.matches(name))
          return false;
      }

      return true;
    };

    auto isScopedImport = [&](ArrayRef<ImportPath::Access> paths) {
      for (auto path : paths) {
        if (path.empty())
          continue;
        if (path.matches(name))
          return true;
      }

      return false;
    };

    auto isPrivateImport = [&](ModuleDecl *module) {
      auto file = dc->getParentSourceFile();
      if (!file) return false;
      for (const auto &import : file->getImports()) {
        if (import.options.contains(ImportFlags::PrivateImport)
            && import.module.importedModule == module
            && import.module.accessPath.matches(name))
          return true;
      }
      return false;
    };

    bool firstPrivate = isPrivateImport(firstModule);

    for (unsigned secondIdx : range(firstIdx + 1, decls.size())) {
      // Determine whether one module takes precedence over another.
      auto secondDecl = decls[secondIdx];
      auto secondModule = secondDecl->getModuleContext();
      bool secondTopLevel = secondDecl->getDeclContext()->isModuleScopeContext();
      bool secondPrivate = isPrivateImport(secondModule);

      // For member types, we skip most of the below rules. Instead, we allow
      // member types defined in a subclass to shadow member types defined in
      // a superclass.
      if (isa<TypeDecl>(firstDecl) &&
          isa<TypeDecl>(secondDecl) &&
          !firstTopLevel &&
          !secondTopLevel) {
        auto *firstClass = firstDecl->getDeclContext()->getSelfClassDecl();
        auto *secondClass = secondDecl->getDeclContext()->getSelfClassDecl();
        if (firstClass && secondClass && firstClass != secondClass) {
          if (firstClass->isSuperclassOf(secondClass)) {
            shadowed.insert(firstDecl);
            continue;
          } else if (secondClass->isSuperclassOf(firstClass)) {
            shadowed.insert(secondDecl);
            continue;
          }
        }

        // If one declaration is in a protocol or extension thereof and the
        // other is not, prefer the one that is not.
        if ((bool)firstDecl->getDeclContext()->getSelfProtocolDecl() !=
              (bool)secondDecl->getDeclContext()->getSelfProtocolDecl()) {
          if (firstDecl->getDeclContext()->getSelfProtocolDecl()) {
            shadowed.insert(firstDecl);
            break;
          } else {
            shadowed.insert(secondDecl);
            continue;
          }
        }

        continue;
      }

      // Top-level type declarations in a module shadow other declarations
      // visible through the module's imports.
      //
      // [Backward compatibility] Note that members of types have the same
      // shadowing check, but we do it after dropping unavailable members.
      if (firstModule != secondModule &&
          firstTopLevel && secondTopLevel) {
        auto firstPaths = imports.getAllAccessPathsNotShadowedBy(
          firstModule, secondModule, dc);
        auto secondPaths = imports.getAllAccessPathsNotShadowedBy(
          secondModule, firstModule, dc);

        // Check if one module shadows the other.
        if (isShadowed(firstPaths)) {
          shadowed.insert(firstDecl);
          break;
        } else if (isShadowed(secondPaths)) {
          shadowed.insert(secondDecl);
          continue;
        }

        // If neither module shadows the other, but one was imported with
        // '@_private import' in dc, we want to favor that module. This makes
        // name lookup in this file behave more like name lookup in the file we
        // imported from, avoiding headaches for source-transforming tools.
        if (!firstPrivate && secondPrivate) {
          shadowed.insert(firstDecl);
          break;
        } else if (firstPrivate && !secondPrivate) {
          shadowed.insert(secondDecl);
          continue;
        }

        // We might be in a situation where neither module shadows the
        // other, but one declaration is visible via a scoped import.
        bool firstScoped = isScopedImport(firstPaths);
        bool secondScoped = isScopedImport(secondPaths);
        if (!firstScoped && secondScoped) {
          shadowed.insert(firstDecl);
          break;
        } else if (firstScoped && !secondScoped) {
          shadowed.insert(secondDecl);
          continue;
        }
      }

      // Swift 4 compatibility hack: Don't shadow properties defined in
      // extensions of generic types with properties defined elsewhere.
      // This is due to the fact that in Swift 4, we only gave custom overload
      // types to properties in extensions of generic types, otherwise we
      // used the null type.
      if (!ctx.isSwiftVersionAtLeast(5) && isa<ValueDecl>(firstDecl)) {
        auto secondSig = cast<ValueDecl>(secondDecl)->getOverloadSignature();
        auto firstSig = cast<ValueDecl>(firstDecl)->getOverloadSignature();
        if (firstSig.IsVariable && secondSig.IsVariable)
          if (firstSig.InExtensionOfGenericType !=
              secondSig.InExtensionOfGenericType)
            continue;
      }

      // If one declaration is in a protocol or extension thereof and the
      // other is not, prefer the one that is not.
      if ((bool)firstDecl->getDeclContext()->getSelfProtocolDecl() !=
            (bool)secondDecl->getDeclContext()->getSelfProtocolDecl()) {
        if (firstDecl->getDeclContext()->getSelfProtocolDecl()) {
          shadowed.insert(firstDecl);
          break;
        } else {
          shadowed.insert(secondDecl);
          continue;
        }
      }

      // If one declaration is available and the other is not, prefer the
      // available one.
      if (firstDecl->getAttrs().isUnavailable(ctx) !=
            secondDecl->getAttrs().isUnavailable(ctx)) {
       if (firstDecl->getAttrs().isUnavailable(ctx)) {
         shadowed.insert(firstDecl);
         break;
       } else {
         shadowed.insert(secondDecl);
         continue;
       }
      }

      // Don't apply module-shadowing rules to members of protocol types.
      if (isa<ProtocolDecl>(firstDecl->getDeclContext()) ||
          isa<ProtocolDecl>(secondDecl->getDeclContext()))
        continue;

      // [Backward compatibility] For members of types, the general module
      // shadowing check is performed after unavailable candidates have
      // already been dropped.
      if (firstModule != secondModule &&
          !firstTopLevel && !secondTopLevel) {
        auto firstPaths = imports.getAllAccessPathsNotShadowedBy(
          firstModule, secondModule, dc);
        auto secondPaths = imports.getAllAccessPathsNotShadowedBy(
          secondModule, firstModule, dc);

        // Check if one module shadows the other.
        if (isShadowed(firstPaths)) {
          shadowed.insert(firstDecl);
          break;
        } else if (isShadowed(secondPaths)) {
          shadowed.insert(secondDecl);
          continue;
        }
      }

      // Prefer declarations in the any module over those in the standard
      // library module.
      if (auto swiftModule = ctx.getStdlibModule()) {
        if ((firstModule == swiftModule) != (secondModule == swiftModule)) {
          // If the second module is the standard library module, the second
          // declaration is shadowed by the first.
          if (secondModule == swiftModule) {
            shadowed.insert(secondDecl);
            continue;
          }

          // Otherwise, the first declaration is shadowed by the second. There is
          // no point in continuing to compare the first declaration to others.
          shadowed.insert(firstDecl);
          break;
        }
      }

      // Next, prefer any other module over the _Concurrency module.
      if (auto concurModule = ctx.getLoadedModule(ctx.Id_Concurrency)) {
        if ((firstModule == concurModule) != (secondModule == concurModule)) {
          // If second module is _Concurrency, then it is shadowed by first.
          if (secondModule == concurModule) {
            shadowed.insert(secondDecl);
            continue;
          }

          // Otherwise, the first declaration is shadowed by the second.
          shadowed.insert(firstDecl);
          break;
        }
      }

      // Next, prefer any other module over the _StringProcessing module.
      if (auto spModule = ctx.getLoadedModule(ctx.Id_StringProcessing)) {
        if ((firstModule == spModule) != (secondModule == spModule)) {
          // If second module is _StringProcessing, then it is shadowed by
          // first.
          if (secondModule == spModule) {
            shadowed.insert(secondDecl);
            continue;
          }

          // Otherwise, the first declaration is shadowed by the second.
          shadowed.insert(firstDecl);
          break;
        }
      }

      // Next, prefer any other module over the _Backtracing module.
      if (auto spModule = ctx.getLoadedModule(ctx.Id_Backtracing)) {
        if ((firstModule == spModule) != (secondModule == spModule)) {
          // If second module is _StringProcessing, then it is shadowed by
          // first.
          if (secondModule == spModule) {
            shadowed.insert(secondDecl);
            continue;
          }

          // Otherwise, the first declaration is shadowed by the second.
          shadowed.insert(firstDecl);
          break;
        }
      }

      // The Foundation overlay introduced Data.withUnsafeBytes, which is
      // treated as being ambiguous with SwiftNIO's Data.withUnsafeBytes
      // extension. Apply a special-case name shadowing rule to use the
      // latter rather than the former, which be the consequence of a more
      // significant change to name shadowing in the future.
      if (auto owningStruct1
            = firstDecl->getDeclContext()->getSelfStructDecl()) {
        if (auto owningStruct2
              = secondDecl->getDeclContext()->getSelfStructDecl()) {
          if (owningStruct1 == owningStruct2 &&
              owningStruct1->getName().is("Data") &&
              isa<FuncDecl>(firstDecl) && isa<FuncDecl>(secondDecl) &&
              firstDecl->getName() == secondDecl->getName() &&
              firstDecl->getBaseName().userFacingName() == "withUnsafeBytes") {
            // If the second module is the Foundation module and the first
            // is the NIOFoundationCompat module, the second is shadowed by the
            // first.
            if (firstDecl->getModuleContext()->getName()
                  .is("NIOFoundationCompat") &&
                secondDecl->getModuleContext()->getName().is("Foundation")) {
              shadowed.insert(secondDecl);
              continue;
            }

            // If it's the other way around, the first declaration is shadowed
            // by the second.
            if (secondDecl->getModuleContext()->getName()
                  .is("NIOFoundationCompat") &&
                firstDecl->getModuleContext()->getName().is("Foundation")) {
              shadowed.insert(firstDecl);
              break;
            }
          }
        }
      }

      // Prefer declarations in an overlay to similar declarations in
      // the Clang module it customizes.
      if (firstDecl->hasClangNode() != secondDecl->hasClangNode()) {
        auto clangLoader = ctx.getClangModuleLoader();
        if (!clangLoader) continue;

        if (clangLoader->isInOverlayModuleForImportedModule(
                                              firstDecl->getDeclContext(),
                                              secondDecl->getDeclContext())) {
          shadowed.insert(secondDecl);
          continue;
        }

        if (clangLoader->isInOverlayModuleForImportedModule(
                                               secondDecl->getDeclContext(),
                                               firstDecl->getDeclContext())) {
          shadowed.insert(firstDecl);
          break;
        }
      }
    }
  }
}

/// Given a set of declarations whose names and generic signatures have matched,
/// figure out which of these declarations have been shadowed by others.
static void recordShadowedDeclsAfterSignatureMatch(
                              ArrayRef<ValueDecl *> decls,
                              const DeclContext *dc,
                              llvm::SmallPtrSetImpl<ValueDecl *> &shadowed) {
  assert(decls.size() > 1 && "Nothing collided");

  // Categorize all of the declarations based on their overload types.
  llvm::SmallDenseMap<CanType, llvm::TinyPtrVector<ValueDecl *>> collisions;
  llvm::SmallVector<CanType, 2> collisionTypes;

  for (auto decl : decls) {
    assert(!isa<TypeDecl>(decl));

    CanType type;

    // FIXME: The type of a variable or subscript doesn't include
    // enough context to distinguish entities from different
    // constrained extensions, so use the overload signature's
    // type. This is layering a partial fix upon a total hack.
    if (auto asd = dyn_cast<AbstractStorageDecl>(decl))
      type = asd->getOverloadSignatureType();
    else
      type = decl->getInterfaceType()->getCanonicalType();

    // Record this declaration based on its signature.
    auto &known = collisions[type];
    if (known.size() == 1) {
      collisionTypes.push_back(type);
    }
    known.push_back(decl);
  }

  // Check whether we have shadowing for signature collisions.
  for (auto type : collisionTypes) {
    ArrayRef<ValueDecl *> collidingDecls = collisions[type];
    recordShadowedDeclsAfterTypeMatch(collidingDecls, dc,
                                      shadowed);
  }
}

/// Look through the given set of declarations (that all have the same name),
/// recording those that are shadowed by another declaration in the
/// \c shadowed set.
static void recordShadowedDeclsForImportedInits(
                                ArrayRef<ConstructorDecl *> ctors,
                                llvm::SmallPtrSetImpl<ValueDecl *> &shadowed) {
  assert(ctors.size() > 1 && "No collisions");

  ASTContext &ctx = ctors.front()->getASTContext();

  // Find the "best" constructor with this signature.
  ConstructorDecl *bestCtor = ctors[0];
  for (auto ctor : ctors.slice(1)) {
    auto comparison = compareConstructors(ctor, bestCtor, ctx);
    if (comparison == ConstructorComparison::Better)
      bestCtor = ctor;
  }

  // Shadow any initializers that are worse.
  for (auto ctor : ctors) {
    auto comparison = compareConstructors(ctor, bestCtor, ctx);
    if (comparison == ConstructorComparison::Worse)
      shadowed.insert(ctor);
  }
}

/// Look through the given set of declarations (that all have the same name),
/// recording those that are shadowed by another declaration in the
/// \c shadowed set.
static void recordShadowedDecls(ArrayRef<ValueDecl *> decls,
                                const DeclContext *dc,
                                llvm::SmallPtrSetImpl<ValueDecl *> &shadowed) {
  if (decls.size() < 2)
    return;

  llvm::TinyPtrVector<ValueDecl *> typeDecls;

  // Categorize all of the declarations based on their overload signatures.
  llvm::SmallDenseMap<const GenericSignatureImpl *,
                      llvm::TinyPtrVector<ValueDecl *>> collisions;
  llvm::SmallVector<const GenericSignatureImpl *, 2> collisionSignatures;
  llvm::SmallDenseMap<NominalTypeDecl *,
                      llvm::TinyPtrVector<ConstructorDecl *>>
    importedInitializerCollisions;
  llvm::TinyPtrVector<NominalTypeDecl *> importedInitializerCollisionTypes;

  for (auto decl : decls) {
    if (auto *typeDecl = dyn_cast<TypeDecl>(decl)) {
      typeDecls.push_back(typeDecl);
      continue;
    }

    // Specifically keep track of imported initializers, which can come from
    // Objective-C init methods, Objective-C factory methods, renamed C
    // functions, or be synthesized by the importer.
    if (decl->hasClangNode() ||
        (isa<NominalTypeDecl>(decl->getDeclContext()) &&
         cast<NominalTypeDecl>(decl->getDeclContext())->hasClangNode())) {
      if (auto ctor = dyn_cast<ConstructorDecl>(decl)) {
        auto nominal = ctor->getDeclContext()->getSelfNominalTypeDecl();
        auto &knownInits = importedInitializerCollisions[nominal];
        if (knownInits.size() == 1) {
          importedInitializerCollisionTypes.push_back(nominal);
        }
        knownInits.push_back(ctor);
      }
    }

    // If the decl is currently being validated, this is likely a recursive
    // reference and we'll want to skip ahead so as to avoid having its type
    // attempt to desugar itself.
    if (decl->isRecursiveValidation())
      continue;

    // Record this declaration based on its signature.
    auto *dc = decl->getInnermostDeclContext();
    auto signature = dc->getGenericSignatureOfContext().getCanonicalSignature();
    auto &known = collisions[signature.getPointer()];
    if (known.size() == 1) {
      collisionSignatures.push_back(signature.getPointer());
    }

    known.push_back(decl);
  }

  // Check whether we have shadowing for type declarations.
  if (typeDecls.size() > 1) {
    ArrayRef<ValueDecl *> collidingDecls = typeDecls;
    recordShadowedDeclsAfterTypeMatch(collidingDecls, dc, shadowed);
  }

  // Check whether we have shadowing for signature collisions.
  for (auto signature : collisionSignatures) {
    ArrayRef<ValueDecl *> collidingDecls = collisions[signature];
    recordShadowedDeclsAfterSignatureMatch(collidingDecls, dc, shadowed);
  }

  // Check whether we have shadowing for imported initializer collisions.
  for (auto nominal : importedInitializerCollisionTypes) {
    recordShadowedDeclsForImportedInits(importedInitializerCollisions[nominal],
                                        shadowed);
  }
}

static void
recordShadowedDecls(ArrayRef<OperatorDecl *> decls, const DeclContext *dc,
                    llvm::SmallPtrSetImpl<OperatorDecl *> &shadowed) {
  // Always considered to have the same signature.
  recordShadowedDeclsAfterTypeMatch(decls, dc, shadowed);
}

static void
recordShadowedDecls(ArrayRef<PrecedenceGroupDecl *> decls,
                    const DeclContext *dc,
                    llvm::SmallPtrSetImpl<PrecedenceGroupDecl *> &shadowed) {
  // Always considered to have the same type.
  recordShadowedDeclsAfterTypeMatch(decls, dc, shadowed);
}

template <typename T, typename Container>
static bool removeShadowedDeclsImpl(Container &decls, const DeclContext *dc) {
  // Collect declarations with the same (full) name.
  llvm::SmallDenseMap<DeclName, llvm::TinyPtrVector<T>> collidingDeclGroups;
  bool anyCollisions = false;
  for (auto decl : decls) {
    // Record this declaration based on its full name.
    auto &knownDecls = collidingDeclGroups[decl->getName()];
    if (!knownDecls.empty())
      anyCollisions = true;

    knownDecls.push_back(decl);
  }

  // If nothing collided, we're done.
  if (!anyCollisions)
    return false;

  // Walk through the declarations again, marking any declarations that shadow.
  llvm::SmallPtrSet<T, 4> shadowed;
  for (auto decl : decls) {
    auto known = collidingDeclGroups.find(decl->getName());
    if (known == collidingDeclGroups.end()) {
      // We already handled this group.
      continue;
    }

    recordShadowedDecls(known->second, dc, shadowed);
    collidingDeclGroups.erase(known);
  }

  // If no declarations were shadowed, we're done.
  if (shadowed.empty())
    return false;

  // Remove shadowed declarations from the list of declarations.
  bool anyRemoved = false;
  decls.erase(std::remove_if(decls.begin(), decls.end(),
                             [&](T decl) {
                               if (shadowed.count(decl) > 0) {
                                 anyRemoved = true;
                                 return true;
                               }

                               return false;
                             }),
              decls.end());

  return anyRemoved;
}

bool swift::removeShadowedDecls(SmallVectorImpl<ValueDecl *> &decls,
                                const DeclContext *dc) {
  return removeShadowedDeclsImpl<ValueDecl *>(decls, dc);
}

bool swift::removeShadowedDecls(TinyPtrVector<OperatorDecl *> &decls,
                                const DeclContext *dc) {
#ifndef NDEBUG
  // Make sure all the operators have the same fixity.
  if (decls.size() > 1) {
    for (auto *op : decls)
      assert(op->getFixity() == decls[0]->getFixity());
  }
#endif
  return removeShadowedDeclsImpl<OperatorDecl *>(decls, dc);
}

bool swift::removeShadowedDecls(TinyPtrVector<PrecedenceGroupDecl *> &decls,
                                const DeclContext *dc) {
  return removeShadowedDeclsImpl<PrecedenceGroupDecl *>(decls, dc);
}

namespace {
enum class DiscriminatorMatch {
  NoDiscriminator,
  Matches,
  Different
};
} // end anonymous namespace

static DiscriminatorMatch matchDiscriminator(Identifier discriminator,
                                             const ValueDecl *value) {
  if (value->getFormalAccess() > AccessLevel::FilePrivate)
    return DiscriminatorMatch::NoDiscriminator;

  auto containingFile =
    dyn_cast<FileUnit>(value->getDeclContext()->getModuleScopeContext());
  if (!containingFile)
    return DiscriminatorMatch::Different;

  if (discriminator == containingFile->getDiscriminatorForPrivateDecl(value))
    return DiscriminatorMatch::Matches;

  return DiscriminatorMatch::Different;
}

static DiscriminatorMatch
matchDiscriminator(Identifier discriminator,
                   LookupResultEntry lookupResult) {
  return matchDiscriminator(discriminator, lookupResult.getValueDecl());
}

template <typename Result>
void namelookup::filterForDiscriminator(SmallVectorImpl<Result> &results,
                                        DebuggerClient *debugClient) {
  if (debugClient == nullptr)
    return;
  Identifier discriminator = debugClient->getPreferredPrivateDiscriminator();
  if (discriminator.empty())
    return;

  auto lastMatchIter = std::find_if(results.rbegin(), results.rend(),
                                    [discriminator](Result next) -> bool {
    return
      matchDiscriminator(discriminator, next) == DiscriminatorMatch::Matches;
  });
  if (lastMatchIter == results.rend())
    return;

  Result lastMatch = *lastMatchIter;

  auto newEnd = std::remove_if(results.begin(), lastMatchIter.base()-1,
                               [discriminator](Result next) -> bool {
    return
      matchDiscriminator(discriminator, next) == DiscriminatorMatch::Different;
  });
  results.erase(newEnd, results.end());
  results.push_back(lastMatch);
}

template void namelookup::filterForDiscriminator<LookupResultEntry>(
    SmallVectorImpl<LookupResultEntry> &results, DebuggerClient *debugClient);

namespace {
  /// Whether we're looking up outer results or not.
  enum class LookupOuterResults {
    Excluded,
    Included
  };
}

/// Retrieve the set of type declarations that are directly referenced from
/// the given parsed type representation.
static DirectlyReferencedTypeDecls
directReferencesForTypeRepr(Evaluator &evaluator, ASTContext &ctx,
                            TypeRepr *typeRepr, DeclContext *dc,
                            bool allowUsableFromInline=false);

/// Retrieve the set of type declarations that are directly referenced from
/// the given type.
static DirectlyReferencedTypeDecls directReferencesForType(Type type);

/// Given a set of type declarations, find all of the nominal type declarations
/// that they reference, looking through typealiases as appropriate.
static TinyPtrVector<NominalTypeDecl *>
resolveTypeDeclsToNominal(Evaluator &evaluator,
                          ASTContext &ctx,
                          ArrayRef<TypeDecl *> typeDecls,
                          SmallVectorImpl<ModuleDecl *> &modulesFound,
                          bool &anyObject);

SelfBounds SelfBoundsFromWhereClauseRequest::evaluate(
    Evaluator &evaluator,
    llvm::PointerUnion<const TypeDecl *, const ExtensionDecl *> decl) const {
  auto *typeDecl = decl.dyn_cast<const TypeDecl *>();
  auto *protoDecl = dyn_cast_or_null<const ProtocolDecl>(typeDecl);
  auto *extDecl = decl.dyn_cast<const ExtensionDecl *>();

  const DeclContext *dc =
      protoDecl ? (const DeclContext *)protoDecl : (const DeclContext *)extDecl;

  // A protocol or extension 'where' clause can reference associated types of
  // the protocol itself, so we have to start unqualified lookup from 'dc'.
  //
  // However, the right hand side of a 'Self' conformance constraint must be
  // resolved before unqualified lookup into 'dc' can work, so we make an
  // exception here and begin lookup from the parent context instead.
  auto *lookupDC = dc->getParent();
  auto requirements = protoDecl ? protoDecl->getTrailingWhereClause()
                                : extDecl->getTrailingWhereClause();

  ASTContext &ctx = dc->getASTContext();

  SelfBounds result;

  if (requirements == nullptr)
    return result;

  for (const auto &req : requirements->getRequirements()) {
    // We only care about type constraints.
    if (req.getKind() != RequirementReprKind::TypeConstraint)
      continue;

    // The left-hand side of the type constraint must be 'Self'.
    bool isSelfLHS = false;
    if (auto typeRepr = req.getSubjectRepr()) {
      if (auto identTypeRepr = dyn_cast<SimpleIdentTypeRepr>(typeRepr))
        isSelfLHS = (identTypeRepr->getNameRef().getBaseIdentifier() ==
                     ctx.Id_Self);
    }
    if (!isSelfLHS)
      continue;

    // Resolve the right-hand side.
    DirectlyReferencedTypeDecls rhsDecls;
    if (auto typeRepr = req.getConstraintRepr()) {
      rhsDecls = directReferencesForTypeRepr(evaluator, ctx, typeRepr, lookupDC);
    }

    SmallVector<ModuleDecl *, 2> modulesFound;
    auto rhsNominals = resolveTypeDeclsToNominal(evaluator, ctx, rhsDecls,
                                                 modulesFound,
                                                 result.anyObject);
    result.decls.insert(result.decls.end(),
                        rhsNominals.begin(),
                        rhsNominals.end());
  }

  return result;
}

SelfBounds swift::getSelfBoundsFromWhereClause(
    llvm::PointerUnion<const TypeDecl *, const ExtensionDecl *> decl) {
  auto *typeDecl = decl.dyn_cast<const TypeDecl *>();
  auto *extDecl = decl.dyn_cast<const ExtensionDecl *>();
  auto &ctx = typeDecl ? typeDecl->getASTContext()
                       : extDecl->getASTContext();
  return evaluateOrDefault(ctx.evaluator,
                           SelfBoundsFromWhereClauseRequest{decl}, {});
}

SelfBounds SelfBoundsFromGenericSignatureRequest::evaluate(
    Evaluator &evaluator, const ExtensionDecl *extDecl) const {
  SelfBounds result;
  ASTContext &ctx = extDecl->getASTContext();
  auto selfType = extDecl->getSelfInterfaceType();
  for (const auto &req : extDecl->getGenericRequirements()) {
    auto kind = req.getKind();
    if (kind != RequirementKind::Conformance &&
        kind != RequirementKind::Superclass)
      continue;
    // The left-hand side of the type constraint must be 'Self'.
    bool isSelfLHS = selfType->isEqual(req.getFirstType());
    if (!isSelfLHS)
      continue;

    auto rhsDecls = directReferencesForType(req.getSecondType());
    SmallVector<ModuleDecl *, 2> modulesFound;
    auto rhsNominals = resolveTypeDeclsToNominal(
        evaluator, ctx, rhsDecls, modulesFound, result.anyObject);
    result.decls.insert(result.decls.end(), rhsNominals.begin(),
                        rhsNominals.end());
  }

  return result;
}

SelfBounds
swift::getSelfBoundsFromGenericSignature(const ExtensionDecl *extDecl) {
  auto &ctx = extDecl->getASTContext();
  return evaluateOrDefault(ctx.evaluator,
                           SelfBoundsFromGenericSignatureRequest{extDecl}, {});
}

TinyPtrVector<TypeDecl *>
TypeDeclsFromWhereClauseRequest::evaluate(Evaluator &evaluator,
                                          ExtensionDecl *ext) const {
  ASTContext &ctx = ext->getASTContext();

  TinyPtrVector<TypeDecl *> result;
  auto resolve = [&](TypeRepr *typeRepr) {
    auto decls = directReferencesForTypeRepr(evaluator, ctx, typeRepr, ext);
    result.insert(result.end(), decls.begin(), decls.end());
  };

  if (auto *whereClause = ext->getTrailingWhereClause()) {
    for (const auto &req : whereClause->getRequirements()) {
      switch (req.getKind()) {
      case RequirementReprKind::TypeConstraint:
        resolve(req.getSubjectRepr());
        resolve(req.getConstraintRepr());
        break;

      case RequirementReprKind::SameType:
        resolve(req.getFirstTypeRepr());
        resolve(req.getSecondTypeRepr());
        break;

      case RequirementReprKind::LayoutConstraint:
        resolve(req.getSubjectRepr());
        break;
      }
    }
  }

  return result;
}




#pragma mark Member lookup table

void LazyMemberLoader::anchor() {}

void LazyConformanceLoader::anchor() {}

/// Lookup table used to store members of a nominal type (and its extensions)
/// for fast retrieval.
class swift::MemberLookupTable : public ASTAllocated<swift::MemberLookupTable> {
  /// The type of the internal lookup table.
  typedef llvm::DenseMap<DeclName, llvm::TinyPtrVector<ValueDecl *>>
    LookupTable;

  /// Lookup table mapping names to the set of declarations with that name.
  LookupTable Lookup;

  /// List of containers that have lazily-loaded members
  llvm::SmallVector<ExtensionDecl *, 2> ExtensionsWithLazyMembers;

  /// The set of names of lazily-loaded members that the lookup table has a
  /// complete accounting of with respect to all known extensions of its
  /// parent nominal type.
  llvm::DenseSet<DeclBaseName> LazilyCompleteNames;

  struct {
    /// Whether we have computed the `containersWithMacroExpansions`.
    bool ComputedContainersWithMacroExpansions = false;

    /// The nominal type and any extensions that have macro expansions, which
    /// is used to restrict the set of places one will lookup for a member
    /// produced by a macro expansion.
    llvm::SmallVector<TypeOrExtensionDecl, 2> ContainersWithMacroExpansions;

    /// The set of names for which we have expanded relevant macros for in the
    /// parent nominal type.
    llvm::DenseSet<DeclName> LazilyCompleteNames;
  } LazyMacroExpansionState;
public:
  /// Create a new member lookup table.
  explicit MemberLookupTable(ASTContext &ctx);

  /// Add the given member to the lookup table.
  void addMember(Decl *members);

  /// Add the given members to the lookup table.
  void addMembers(DeclRange members);

  void addExtensionWithLazyMembers(ExtensionDecl *ext) {
    ExtensionsWithLazyMembers.push_back(ext);
  }

  ArrayRef<ExtensionDecl *> getExtensionsWithLazyMembers() const {
    return ExtensionsWithLazyMembers;
  }

  /// Returns \c true if the lookup table has a complete accounting of the
  /// given name.
  bool isLazilyComplete(DeclBaseName name) const {
    return LazilyCompleteNames.contains(name);
  }

  /// Mark a given lazily-loaded name as being complete.
  void markLazilyComplete(DeclBaseName name) {
    LazilyCompleteNames.insert(name);
  }

  /// Clears the cache of lazily-complete names.  This _must_ be called when
  /// new extensions with lazy members are added to the type, or direct lookup
  /// will return inconsistent or stale results.
  void clearLazilyCompleteCache() {
    LazilyCompleteNames.clear();
  }

  /// Retrieve an array containing the set of containers for this type (
  /// i.e., the nominal type and any extensions) that can produce members via
  /// macro expansion.
  ArrayRef<TypeOrExtensionDecl> getContainersWithMacroExpansions(
      NominalTypeDecl *nominal) {
    if (LazyMacroExpansionState.ComputedContainersWithMacroExpansions)
      return LazyMacroExpansionState.ContainersWithMacroExpansions;

    LazyMacroExpansionState.ComputedContainersWithMacroExpansions = true;

    // Does the type have macro expansions?
    addContainerWithMacroExpansions(nominal);

    // Check each extension for macro expansions.
    for (auto ext : nominal->getExtensions())
      addContainerWithMacroExpansions(ext);

    return LazyMacroExpansionState.ContainersWithMacroExpansions;
  }

  void addContainerWithMacroExpansions(TypeOrExtensionDecl container){
    if (LazyMacroExpansionState.ComputedContainersWithMacroExpansions &&
        evaluateOrDefault(
                container.getAsDecl()->getASTContext().evaluator,
                PotentialMacroExpansionsInContextRequest{container}, {}))
      LazyMacroExpansionState.ContainersWithMacroExpansions.push_back(
          container);
  }

  /// Determine whether the given container has any macro-introduced names that
  /// match the given declaration.
  bool hasAnyMacroNamesMatching(TypeOrExtensionDecl container, DeclName name);

  bool isLazilyCompleteForMacroExpansion(DeclName name) const {
    assert(!MacroDecl::isUniqueMacroName(name.getBaseName()));
    // If we've already expanded macros for a simple name, we must have expanded
    // all macros that produce names with the same base identifier.
    bool isBaseNameComplete = name.isCompoundName() &&
        isLazilyCompleteForMacroExpansion(DeclName(name.getBaseName()));
    return isBaseNameComplete ||
        LazyMacroExpansionState.LazilyCompleteNames.contains(name);
  }

  void markLazilyCompleteForMacroExpansion(DeclName name) {
    assert(!MacroDecl::isUniqueMacroName(name.getBaseName()));
    LazyMacroExpansionState.LazilyCompleteNames.insert(name);
  }

  void clearLazilyCompleteForMacroExpansionCache() {
    LazyMacroExpansionState.LazilyCompleteNames.clear();
  }

  /// Iterator into the lookup table.
  typedef LookupTable::iterator iterator;

  iterator begin() { return Lookup.begin(); }
  iterator end() { return Lookup.end(); }

  iterator find(DeclName name) {
    return Lookup.find(name);
  }

  void dump(llvm::raw_ostream &os) const {
    os << "Lookup:\n  ";
    for (auto &pair : Lookup) {
      pair.getFirst().print(os);
      if (isLazilyComplete(pair.getFirst().getBaseName())) {
        os << " (lazily complete)";
      }
      os << ":\n  ";
      for (auto &decl : pair.getSecond()) {
        os << "- ";
        decl->dumpRef(os);
        os << "\n  ";
      }
    }
    os << "\n";
  }

  SWIFT_DEBUG_DUMP {
    dump(llvm::errs());
  }
};

namespace {
  /// Stores the set of Objective-C methods with a given selector within the
  /// Objective-C method lookup table.
  struct StoredObjCMethods {
    /// The generation count at which this list was last updated.
    unsigned Generation = 0;

    /// The set of methods with the given selector.
    llvm::TinyPtrVector<AbstractFunctionDecl *> Methods;
  };
} // end anonymous namespace

/// Class member lookup table, which is a member lookup table with a second
/// table for lookup based on Objective-C selector.
class swift::ObjCMethodLookupTable
        : public llvm::DenseMap<std::pair<ObjCSelector, char>,
                                StoredObjCMethods>,
          public ASTAllocated<ObjCMethodLookupTable>
{
  SWIFT_DEBUG_DUMP {
    llvm::errs() << "ObjCMethodLookupTable:\n";
    for (auto pair : *this) {
      auto selector = pair.getFirst().first;
      auto isInstanceMethod = pair.getFirst().second;
      auto &methods = pair.getSecond();

      llvm::errs() << "  \"" << (isInstanceMethod ? "-" : "+") << selector
                   << "\":\n";
      for (auto method : methods.Methods) {
        llvm::errs() << "  - \"";
        method->dumpRef(llvm::errs());
        llvm::errs() << "\"\n";
      }
    }
  }
};

MemberLookupTable::MemberLookupTable(ASTContext &ctx) {
  // Register a cleanup with the ASTContext to call the lookup table
  // destructor.
  ctx.addCleanup([this]() {
    this->~MemberLookupTable();
  });
}

void MemberLookupTable::addMember(Decl *member) {
  // Only value declarations matter.
  auto vd = dyn_cast<ValueDecl>(member);
  if (!vd)
    return;

  // @_implements members get added under their declared name.
  auto A = vd->getAttrs().getAttribute<ImplementsAttr>();

  // Unnamed entities w/o @_implements synonyms cannot be found by name lookup.
  if (!A && !vd->hasName())
    return;

  // If this declaration is already in the lookup table, don't add it
  // again.
  if (vd->isAlreadyInLookupTable()) {
    return;
  }
  vd->setAlreadyInLookupTable();

  // Add this declaration to the lookup set under its compound name and simple
  // name.
  vd->getName().addToLookupTable(Lookup, vd);

  // And if given a synonym, under that name too.
  if (A)
    A->getMemberName().addToLookupTable(Lookup, vd);
}

void MemberLookupTable::addMembers(DeclRange members) {
  for (auto member : members) {
    addMember(member);
  }
}

void NominalTypeDecl::addedExtension(ExtensionDecl *ext) {
  if (!LookupTable.getInt())
    return;

  auto *table = LookupTable.getPointer();
  assert(table);

  if (ext->wasDeserialized() || ext->hasClangNode()) {
    table->addMembers(ext->getCurrentMembersWithoutLoading());
    table->clearLazilyCompleteCache();
    table->clearLazilyCompleteForMacroExpansionCache();
    table->addExtensionWithLazyMembers(ext);
  } else {
    table->addMembers(ext->getMembers());
  }

  table->addContainerWithMacroExpansions(ext);
}

void NominalTypeDecl::addedMember(Decl *member) {
  // If we have a lookup table, add the new member to it. If not, we'll pick up
  // this member when we first create the table.
  auto *vd = dyn_cast<ValueDecl>(member);
  if (!vd || !LookupTable.getInt())
    return;

  auto *table = LookupTable.getPointer();
  assert(table);

  table->addMember(vd);
}

void ExtensionDecl::addedMember(Decl *member) {
  // If this extension has already been bound to a nominal, add the new member
  // to the nominal's lookup table.
  if (NextExtension.getInt()) {
    auto nominal = getExtendedNominal();
    if (nominal)
      nominal->addedMember(member);
  }
}

void NominalTypeDecl::addMemberToLookupTable(Decl *member) {
  getLookupTable()->addMember(member);
}

// For lack of anywhere more sensible to put it, here's a diagram of the pieces
// involved in finding members and extensions of a NominalTypeDecl.
//
// ┌────────────────────────────┬─┐
// │IterableDeclContext         │ │     ┌─────────────────────────────┐
// │-------------------         │ │     │┌───────────────┬┐           ▼
// │Decl *LastDecl   ───────────┼─┼─────┘│Decl           ││  ┌───────────────┬┐
// │Decl *FirstDecl  ───────────┼─┼─────▶│----           ││  │Decl           ││
// │                            │ │      │Decl  *NextDecl├┼─▶│----           ││
// │bool HasLazyMembers         │ │      ├───────────────┘│  │Decl *NextDecl ││
// │IterableDeclContextKind Kind│ │      │                │  ├───────────────┘│
// │                            │ │      │ValueDecl       │  │                │
// ├────────────────────────────┘ │      │---------       │  │ValueDecl       │
// │                              │      │DeclName Name   │  │---------       │
// │NominalTypeDecl               │      └────────────────┘  │DeclName Name   │
// │---------------               │               ▲          └────────────────┘
// │ExtensionDecl *FirstExtension─┼────────┐      │                   ▲
// │ExtensionDecl *LastExtension ─┼───────┐│      │                   └───┐
// │                              │       ││      └──────────────────────┐│
// │MemberLookupTable *LookupTable├─┐     ││                             ││
// └──────────────────────────────┘ │     ││     ┌─────────────────┐     ││
//                                  │     ││     │ExtensionDecl    │     ││
//                                  │     ││     │-------------    │     ││
//                    ┌─────────────┘     │└────▶│ExtensionDecl    │     ││
//                    │                   │      │  *NextExtension ├──┐  ││
//                    ▼                   │      └─────────────────┘  │  ││
// ┌─────────────────────────────────────┐│      ┌─────────────────┐  │  ││
// │MemberLookupTable                    ││      │ExtensionDecl    │  │  ││
// │-----------------                    ││      │-------------    │  │  ││
// │ExtensionDecl *LastExtensionIncluded ├┴─────▶│ExtensionDecl    │◀─┘  ││
// │                                     │       │  *NextExtension │     ││
// │┌───────────────────────────────────┐│       └─────────────────┘     ││
// ││DenseMap<Declname, ...> LookupTable││                               ││
// ││-----------------------------------││  ┌──────────────────────────┐ ││
// ││[NameA] TinyPtrVector<ValueDecl *> ││  │TinyPtrVector<ValueDecl *>│ ││
// ││[NameB] TinyPtrVector<ValueDecl *> ││  │--------------------------│ ││
// ││[NameC] TinyPtrVector<ValueDecl *>─┼┼─▶│[0] ValueDecl *      ─────┼─┘│
// │└───────────────────────────────────┘│  │[1] ValueDecl *      ─────┼──┘
// └─────────────────────────────────────┘  └──────────────────────────┘
//
// The HasLazyMembers, Kind, and LookupTableComplete fields are packed into
// PointerIntPairs so don't go grepping for them; but for purposes of
// illustration they are effectively their own fields.
//
// MemberLookupTable is populated en-masse when the IterableDeclContext's
// (IDC's) list of Decls is populated. But MemberLookupTable can also be
// populated incrementally by one-name-at-a-time lookups by lookupDirect, in
// which case those Decls are _not_ added to the IDC's list. They are cached in
// the loader they come from, lifecycle-wise, and are added to the
// MemberLookupTable to accelerate subsequent retrieval, but the IDC is not
// considered populated until someone calls getMembers().
//
// If the IDC list is later populated and/or an extension is added _after_
// MemberLookupTable is constructed (and possibly has entries in it),
// MemberLookupTable is incrementally reconstituted with new members.

static void
populateLookupTableEntryFromLazyIDCLoader(ASTContext &ctx,
                                          MemberLookupTable &LookupTable,
                                          DeclBaseName name,
                                          IterableDeclContext *IDC) {
  if (!IDC->hasLazyMembers())
    return;

  auto ci = ctx.getOrCreateLazyIterableContextData(IDC,
                                                   /*lazyLoader=*/nullptr);
  auto res = ci->loader->loadNamedMembers(IDC, name, ci->memberData);
  if (auto s = ctx.Stats) {
    ++s->getFrontendCounters().NamedLazyMemberLoadSuccessCount;
  }
  for (auto d : res) {
    LookupTable.addMember(d);
  }
}

static void
populateLookupTableEntryFromExtensions(ASTContext &ctx,
                                       MemberLookupTable &table,
                                       DeclBaseName name,
                                       NominalTypeDecl *nominal) {
  assert(!table.isLazilyComplete(name) &&
         "Should not be searching extensions for complete name!");

  for (auto e : table.getExtensionsWithLazyMembers()) {
    // If there's no lazy members to look at, all the members of this extension
    // are present in the lookup table.
    if (!e->hasLazyMembers()) {
      continue;
    }

    assert(e->wasDeserialized() || e->hasClangNode() &&
           "Extension without deserializable content has lazy members!");
    assert(!e->hasUnparsedMembers());

    populateLookupTableEntryFromLazyIDCLoader(ctx, table, name, e);
  }
}

/// Adjust the given name to make it a proper key for the lazy macro expansion
/// cache, which maps all uniquely-generated names down to a single placeholder
/// key.
static DeclName adjustLazyMacroExpansionNameKey(
    ASTContext &ctx, DeclName name) {
  if (MacroDecl::isUniqueMacroName(name.getBaseName()))
    return MacroDecl::getUniqueNamePlaceholder(ctx);

  return name;
}

SmallVector<MacroDecl *, 1>
namelookup::lookupMacros(DeclContext *dc, DeclNameRef macroName,
                         MacroRoles roles) {
  SmallVector<MacroDecl *, 1> choices;
  auto moduleScopeDC = dc->getModuleScopeContext();
  ASTContext &ctx = moduleScopeDC->getASTContext();

  // Macro lookup should always exclude macro expansions; macro
  // expansions cannot introduce new macro declarations. Note that
  // the source location here doesn't matter.
  UnqualifiedLookupDescriptor descriptor{
    macroName, moduleScopeDC, SourceLoc(),
    UnqualifiedLookupFlags::ExcludeMacroExpansions
  };

  auto lookup = evaluateOrDefault(
      ctx.evaluator, UnqualifiedLookupRequest{descriptor}, {});
  for (const auto &found : lookup.allResults()) {
    if (auto macro = dyn_cast<MacroDecl>(found.getValueDecl())) {
      auto candidateRoles = macro->getMacroRoles();
      if ((candidateRoles && roles.contains(candidateRoles)) ||
          // FIXME: `externalMacro` should have all roles.
          macro->getBaseIdentifier().str() == "externalMacro") {
        choices.push_back(macro);
      }
    }
  }
  return choices;
}

bool
namelookup::isInMacroArgument(SourceFile *sourceFile, SourceLoc loc) {
  bool inMacroArgument = false;

  ASTScope::lookupEnclosingMacroScope(
      sourceFile, loc,
      [&](auto potentialMacro) -> bool {
        UnresolvedMacroReference macro(potentialMacro);

        if (macro.getFreestanding()) {
          inMacroArgument = true;
        } else if (auto *attr = macro.getAttr()) {
          auto *moduleScope = sourceFile->getModuleScopeContext();
          auto results = lookupMacros(moduleScope, macro.getMacroName(),
                                      getAttachedMacroRoles());
          inMacroArgument = !results.empty();
        }

        return inMacroArgument;
      });

  return inMacroArgument;
}

/// Call the given function body with each macro declaration and its associated
/// role attribute for the given role.
///
/// This routine intentionally avoids calling `forEachAttachedMacro`, which
/// triggers request cycles.
void namelookup::forEachPotentialResolvedMacro(
    DeclContext *moduleScopeCtx, DeclNameRef macroName, MacroRole role,
    llvm::function_ref<void(MacroDecl *, const MacroRoleAttr *)> body
) {
  ASTContext &ctx = moduleScopeCtx->getASTContext();
  UnqualifiedLookupDescriptor lookupDesc{
    macroName, moduleScopeCtx, SourceLoc(),
    UnqualifiedLookupFlags::ExcludeMacroExpansions
  };

  auto lookup = evaluateOrDefault(
      ctx.evaluator, UnqualifiedLookupRequest{lookupDesc}, {});
  for (auto result : lookup.allResults()) {
    auto *vd = result.getValueDecl();
    auto *macro = dyn_cast<MacroDecl>(vd);
    if (!macro)
      continue;

    auto *macroRoleAttr = macro->getMacroRoleAttr(role);
    if (!macroRoleAttr)
      continue;

    body(macro, macroRoleAttr);
  }
}

/// For each macro with the given role that might be attached to the given
/// declaration, call the body.
void namelookup::forEachPotentialAttachedMacro(
    Decl *decl, MacroRole role,
    llvm::function_ref<void(MacroDecl *macro, const MacroRoleAttr *)> body
) {
  // We intentionally avoid calling `forEachAttachedMacro` in order to avoid
  // a request cycle.
  auto moduleScopeCtx = decl->getDeclContext()->getModuleScopeContext();
  for (auto attrConst : decl->getSemanticAttrs().getAttributes<CustomAttr>()) {
    auto *attr = const_cast<CustomAttr *>(attrConst);
    UnresolvedMacroReference macroRef(attr);
    auto macroName = macroRef.getMacroName();
    forEachPotentialResolvedMacro(moduleScopeCtx, macroName, role, body);
  }
}

namespace {
  /// Function object that tracks macro-introduced names.
  struct MacroIntroducedNameTracker {
    ValueDecl *attachedTo = nullptr;

    PotentialMacroExpansions potentialExpansions;

    /// Augment the set of names with those introduced by the given macro.
    void operator()(MacroDecl *macro, const MacroRoleAttr *attr) {
      potentialExpansions.noteExpandedMacro();

      // First check for arbitrary names.
      if (attr->hasNameKind(MacroIntroducedDeclNameKind::Arbitrary)) {
        potentialExpansions.noteIntroducesArbitraryNames();
      }

      // If this introduces arbitrary names, there's nothing more to do.
      if (potentialExpansions.introducesArbitraryNames())
        return;

      SmallVector<DeclName, 4> introducedNames;
      macro->getIntroducedNames(
          attr->getMacroRole(), attachedTo, introducedNames);
      for (auto name : introducedNames)
        potentialExpansions.addIntroducedMacroName(name);
    }

    bool shouldExpandForName(DeclName name) const {
      return potentialExpansions.shouldExpandForName(name);
    }
  };
}

PotentialMacroExpansions PotentialMacroExpansionsInContextRequest::evaluate(
    Evaluator &evaluator, TypeOrExtensionDecl container) const {
  /// The implementation here needs to be kept in sync with
  /// populateLookupTableEntryFromMacroExpansions.
  MacroIntroducedNameTracker nameTracker;

  // Member macros on the type or extension.
  auto containerDecl = container.getAsDecl();
  forEachPotentialAttachedMacro(containerDecl, MacroRole::Member, nameTracker);

  // Peer and freestanding declaration macros.
  auto dc = container.getAsDeclContext();
  auto idc = container.getAsIterableDeclContext();
  for (auto *member : idc->getCurrentMembersWithoutLoading()) {
    if (auto *med = dyn_cast<MacroExpansionDecl>(member)) {
      nameTracker.attachedTo = nullptr;
      forEachPotentialResolvedMacro(
          dc->getModuleScopeContext(), med->getMacroName(),
          MacroRole::Declaration, nameTracker);
    } else if (auto *vd = dyn_cast<ValueDecl>(member)) {
      nameTracker.attachedTo = dyn_cast<ValueDecl>(member);
      forEachPotentialAttachedMacro(member, MacroRole::Peer, nameTracker);
    }
  }

  nameTracker.attachedTo = nullptr;
  return nameTracker.potentialExpansions;
}

bool MemberLookupTable::hasAnyMacroNamesMatching(
    TypeOrExtensionDecl container, DeclName name) {
  ASTContext &ctx = container.getAsDecl()->getASTContext();
  auto potentialExpansions = evaluateOrDefault(
      ctx.evaluator, PotentialMacroExpansionsInContextRequest{container},
      PotentialMacroExpansions());

  return potentialExpansions.shouldExpandForName(name);
}

static void
populateLookupTableEntryFromMacroExpansions(ASTContext &ctx,
                                            MemberLookupTable &table,
                                            DeclName name,
                                            TypeOrExtensionDecl container) {
  // If there are no macro-introduced names in this container that match the
  // given name, do nothing. This avoids an expensive walk over the members
  // and attributes for the common case where there are no macros.
  if (!table.hasAnyMacroNamesMatching(container, name))
    return;

  // Trigger the expansion of member macros on the container, if any of the
  // names match.
  {
    MacroIntroducedNameTracker nameTracker;
    auto decl = container.getAsDecl();
    forEachPotentialAttachedMacro(decl, MacroRole::Member, nameTracker);
    if (nameTracker.shouldExpandForName(name)) {
      (void)evaluateOrDefault(
          ctx.evaluator,
          ExpandSynthesizedMemberMacroRequest{decl},
          false);
    }
  }

  auto dc = container.getAsDeclContext();
  auto *module = dc->getParentModule();
  auto idc = container.getAsIterableDeclContext();
  for (auto *member : idc->getCurrentMembersWithoutLoading()) {
    // Collect all macro introduced names, along with its corresponding macro
    // reference. We need the macro reference to prevent adding auxiliary decls
    // that weren't introduced by the macro.
    MacroIntroducedNameTracker nameTracker;
    if (auto *med = dyn_cast<MacroExpansionDecl>(member)) {
      forEachPotentialResolvedMacro(
          dc->getModuleScopeContext(), med->getMacroName(),
          MacroRole::Declaration, nameTracker);
    } else if (auto *vd = dyn_cast<ValueDecl>(member)) {
      nameTracker.attachedTo = dyn_cast<ValueDecl>(member);
      forEachPotentialAttachedMacro(member, MacroRole::Peer, nameTracker);
    }

    // Expand macros on this member.
    if (nameTracker.shouldExpandForName(name)) {
      member->visitAuxiliaryDecls([&](Decl *decl) {
        auto *sf = module->getSourceFileContainingLocation(decl->getLoc());
        // Bail out if the auxiliary decl was not produced by a macro.
        if (!sf || sf->Kind != SourceFileKind::MacroExpansion) return;
        table.addMember(decl);
      });
    }
  }
}

MemberLookupTable *NominalTypeDecl::getLookupTable() {
  if (!LookupTable.getPointer()) {
    auto &ctx = getASTContext();
    LookupTable.setPointer(new (ctx) MemberLookupTable(ctx));
  }

  return LookupTable.getPointer();
}

void NominalTypeDecl::prepareLookupTable() {
  // If we have already prepared the lookup table, then there's nothing further
  // to do.
  if (LookupTable.getInt())
    return;

  auto *table = getLookupTable();

  // Otherwise start the first fill.
  if (hasLazyMembers()) {
    assert(!hasUnparsedMembers());
    table->addMembers(getCurrentMembersWithoutLoading());
  } else {
    table->addMembers(getMembers());
  }

  // Note: this calls prepareExtensions()
  for (auto e : getExtensions()) {
    // If we can lazy-load this extension, only take the members we've loaded
    // so far.
    //
    // FIXME: This should be 'e->hasLazyMembers()' but that crashes` because
    // some imported extensions don't have a Clang node, and only support
    // LazyMemberLoader::loadAllMembers() and not
    // LazyMemberLoader::loadNamedMembers().
    if (e->wasDeserialized() || e->hasClangNode()) {
      table->addMembers(e->getCurrentMembersWithoutLoading());
      table->addExtensionWithLazyMembers(e);
      continue;
    }

    // Else, load all the members into the table.
    table->addMembers(e->getMembers());
  }

  // Any extensions added after this point will add their members to the
  // lookup table.
  LookupTable.setInt(true);
}

static TinyPtrVector<ValueDecl *>
maybeFilterOutUnwantedDecls(TinyPtrVector<ValueDecl *> decls,
                            DeclName name,
                            bool includeAttrImplements,
                            bool excludeMacroExpansions) {
  if (includeAttrImplements && !excludeMacroExpansions)
    return decls;
  TinyPtrVector<ValueDecl*> result;
  for (auto V : decls) {
    // If we're supposed to exclude anything that comes from a macro expansion,
    // check whether the source location of the declaration is in a macro
    // expansion, and skip this declaration if it does.
    if (excludeMacroExpansions) {
      auto sourceFile =
          V->getModuleContext()->getSourceFileContainingLocation(V->getLoc());
      if (sourceFile && sourceFile->Kind == SourceFileKind::MacroExpansion)
        continue;
    }

    // Filter-out any decl that doesn't have the name we're looking for
    // (asserting as a consistency-check that such entries all have
    // @_implements attrs for the name!)
    if (V->getName().matchesRef(name)) {
      result.push_back(V);
    } else {
      auto A = V->getAttrs().getAttribute<ImplementsAttr>();
      (void)A;
      assert(A && A->getMemberName().matchesRef(name));
    }
  }
  return result;
}

TinyPtrVector<ValueDecl *>
NominalTypeDecl::lookupDirect(DeclName name, SourceLoc loc,
                              OptionSet<LookupDirectFlags> flags) {
  return evaluateOrDefault(getASTContext().evaluator,
                           DirectLookupRequest({this, name, flags}, loc), {});
}

TinyPtrVector<ValueDecl *>
DirectLookupRequest::evaluate(Evaluator &evaluator,
                              DirectLookupDescriptor desc) const {
  const auto &name = desc.Name;
  const auto flags = desc.Options;
  auto *decl = desc.DC;

  ASTContext &ctx = decl->getASTContext();
  const bool includeAttrImplements =
      flags.contains(NominalTypeDecl::LookupDirectFlags::IncludeAttrImplements);
  const bool excludeMacroExpansions =
      flags.contains(NominalTypeDecl::LookupDirectFlags::ExcludeMacroExpansions);

  LLVM_DEBUG(llvm::dbgs() << decl->getNameStr() << ".lookupDirect("
                          << name << ")"
                          << ", excludeMacroExpansions="
                          << excludeMacroExpansions
                          << "\n");

  decl->prepareLookupTable();

  // Call prepareExtensions() to ensure we properly invalidate the
  // lazily-complete cache for any extensions brought in by modules
  // loaded after-the-fact. This can happen with the LLDB REPL.
  decl->prepareExtensions();

  auto &Table = *decl->getLookupTable();
  if (!Table.isLazilyComplete(name.getBaseName())) {
    DeclBaseName baseName(name.getBaseName());

    if (isa_and_nonnull<clang::NamespaceDecl>(decl->getClangDecl())) {
      auto allFound = evaluateOrDefault(
          ctx.evaluator, CXXNamespaceMemberLookup({cast<EnumDecl>(decl), name}),
          {});
      populateLookupTableEntryFromExtensions(ctx, Table, baseName, decl);

      // Bypass the regular member lookup table if we find something in
      // the original C++ namespace. We don't want to store the C++ decl in the
      // lookup table as the decl can be referenced  from multiple namespace
      // declarations due to inline namespaces. We still merge in the other
      // entries found in the lookup table, to support finding members in
      // namespace extensions.
      if (!allFound.empty()) {
        auto known = Table.find(name);
        if (known != Table.end()) {
          auto swiftLookupResult = maybeFilterOutUnwantedDecls(
              known->second, name, includeAttrImplements,
              excludeMacroExpansions);
          for (auto foundSwiftDecl : swiftLookupResult) {
            allFound.push_back(foundSwiftDecl);
          }
        }
        return allFound;
      }
    } else if (isa_and_nonnull<clang::RecordDecl>(decl->getClangDecl())) {
      auto allFound = evaluateOrDefault(
          ctx.evaluator,
          ClangRecordMemberLookup({cast<NominalTypeDecl>(decl), name}), {});
      // Add all the members we found, later we'll combine these with the
      // existing members.
      for (auto found : allFound)
        Table.addMember(found);

      populateLookupTableEntryFromExtensions(ctx, Table, baseName, decl);
    } else {
      // The lookup table believes it doesn't have a complete accounting of this
      // name - either because we're never seen it before, or another extension
      // was registered since the last time we searched. Ask the loaders to give
      // us a hand.
      populateLookupTableEntryFromLazyIDCLoader(ctx, Table, baseName, decl);
      populateLookupTableEntryFromExtensions(ctx, Table, baseName, decl);
    }

    Table.markLazilyComplete(baseName);
  }

  DeclName macroExpansionKey = adjustLazyMacroExpansionNameKey(ctx, name);
  if (!excludeMacroExpansions &&
      !Table.isLazilyCompleteForMacroExpansion(macroExpansionKey)) {
    for (auto container : Table.getContainersWithMacroExpansions(decl)) {
      populateLookupTableEntryFromMacroExpansions(
          ctx, Table, macroExpansionKey, container);
    }
    Table.markLazilyCompleteForMacroExpansion(macroExpansionKey);
  }

  // Look for a declaration with this name.
  auto known = Table.find(name);
  if (known == Table.end()) {
    return TinyPtrVector<ValueDecl *>();
  }

  // We found something; return it.
  return maybeFilterOutUnwantedDecls(known->second, name,
                                     includeAttrImplements,
                                     excludeMacroExpansions);
}

bool NominalTypeDecl::createObjCMethodLookup() {
  assert(!ObjCMethodLookup && "Already have an Objective-C member table");

  // Most types cannot have ObjC methods.
  if (!(isa<ClassDecl>(this) || isa<ProtocolDecl>(this)))
    return false;

  auto &ctx = getASTContext();
  ObjCMethodLookup = new (ctx) ObjCMethodLookupTable();

  // Register a cleanup with the ASTContext to call the lookup table
  // destructor.
  ctx.addDestructorCleanup(*ObjCMethodLookup);

  return true;
}

TinyPtrVector<AbstractFunctionDecl *>
NominalTypeDecl::lookupDirect(ObjCSelector selector, bool isInstance) {
  if (!ObjCMethodLookup && !createObjCMethodLookup())
    return {};

  // If any modules have been loaded since we did the search last (or if we
  // hadn't searched before), look in those modules, too.
  auto &stored = (*ObjCMethodLookup)[{selector, isInstance}];
  ASTContext &ctx = getASTContext();
  if (ctx.getCurrentGeneration() > stored.Generation) {
    ctx.loadObjCMethods(this, selector, isInstance, stored.Generation,
                        stored.Methods);
    stored.Generation = ctx.getCurrentGeneration();
  }

  return stored.Methods;
}

static bool inObjCImplExtension(AbstractFunctionDecl *newDecl) {
  if (auto ext = dyn_cast<ExtensionDecl>(newDecl->getDeclContext()))
    return ext->isObjCImplementation();
  return false;
}

/// If there is an apparent conflict between \p newDecl and one of the methods
/// in \p vec, should we diagnose it?
static bool
shouldDiagnoseConflict(NominalTypeDecl *ty, AbstractFunctionDecl *newDecl,
                       llvm::TinyPtrVector<AbstractFunctionDecl *> &vec) {
  // Conflicts between member implementations and their interfaces, or
  // inherited inits and their overrides in @_objcImpl extensions, are spurious.
  if (newDecl->isObjCMemberImplementation()
      || (isa<ConstructorDecl>(newDecl) && inObjCImplExtension(newDecl)
          && newDecl->getAttrs().hasAttribute<OverrideAttr>()))
    return false;

  // Are all conflicting methods imported from ObjC and in our ObjC half or a
  // bridging header? Some code bases implement ObjC methods in Swift even
  // though it's not exactly supported.
  auto newDeclModuleName = newDecl->getModuleContext()->getName();
  auto newDeclPrivateModuleName = newDecl->getASTContext().getIdentifier(
                     (llvm::Twine(newDeclModuleName.str()) + "_Private").str());
  auto bridgingHeaderModuleName = newDecl->getASTContext().getIdentifier(
                                                     CLANG_HEADER_MODULE_NAME);
  if (llvm::all_of(vec, [&](AbstractFunctionDecl *oldDecl) {
    if (!oldDecl->hasClangNode())
      return false;
    auto oldDeclModuleName = oldDecl->getModuleContext()->getName();
    return oldDeclModuleName == newDeclModuleName
               || oldDeclModuleName == newDeclPrivateModuleName
               || oldDeclModuleName == bridgingHeaderModuleName;
  }))
    return false;

  return true;
}

void NominalTypeDecl::recordObjCMethod(AbstractFunctionDecl *method,
                                       ObjCSelector selector) {
  if (!ObjCMethodLookup && !createObjCMethodLookup())
    return;

  // Record the method.
  bool isInstanceMethod = method->isObjCInstanceMethod();
  auto &vec = (*ObjCMethodLookup)[{selector, isInstanceMethod}].Methods;

  // Check whether we have a duplicate. This only checks more than one
  // element in ill-formed code, so the linear search is acceptable.
  if (std::find(vec.begin(), vec.end(), method) != vec.end())
    return;

  if (auto *sf = method->getParentSourceFile()) {
    if (vec.empty()) {
      sf->ObjCMethodList.push_back(method);
    } else if (shouldDiagnoseConflict(this, method, vec)) {
      // We have a conflict.
      sf->ObjCMethodConflicts.insert({ this, selector, isInstanceMethod });
    }
  }

  vec.push_back(method);
}

/// Determine whether the given declaration is an acceptable lookup
/// result when searching from the given DeclContext.
static bool isAcceptableLookupResult(const DeclContext *dc,
                                     NLOptions options,
                                     ValueDecl *decl,
                                     bool onlyCompleteObjectInits) {
  // Filter out designated initializers, if requested.
  if (onlyCompleteObjectInits) {
    if (auto ctor = dyn_cast<ConstructorDecl>(decl)) {
      if (isa<ClassDecl>(ctor->getDeclContext()) && !ctor->isInheritable())
        return false;
    } else {
      return false;
    }
  }

  // Ignore stub implementations.
  if (auto ctor = dyn_cast<ConstructorDecl>(decl)) {
    if (ctor->hasStubImplementation())
      return false;
  }

  // Check access.
  if (!(options & NL_IgnoreAccessControl) &&
      !dc->getASTContext().isAccessControlDisabled()) {
    bool allowUsableFromInline = options & NL_IncludeUsableFromInline;
    return decl->isAccessibleFrom(dc, /*forConformance*/ false,
                                  allowUsableFromInline);
  }

  return true;
}

void namelookup::pruneLookupResultSet(const DeclContext *dc, NLOptions options,
                                      SmallVectorImpl<ValueDecl *> &decls) {
  // If we're supposed to remove overridden declarations, do so now.
  if (options & NL_RemoveOverridden)
    removeOverriddenDecls(decls);

  // If we're supposed to remove shadowed/hidden declarations, do so now.
  if (options & NL_RemoveNonVisible)
    removeShadowedDecls(decls, dc);

  ModuleDecl *M = dc->getParentModule();
  filterForDiscriminator(decls, M->getDebugClient());
}

// An unfortunate hack to kick the decl checker into adding semantic members to
// the current type before we attempt a semantic lookup. The places this method
// looks needs to be in sync with \c extractDirectlyReferencedNominalTypes.
// See the note in \c synthesizeSemanticMembersIfNeeded about a better, more
// just, and peaceful world.
void namelookup::installSemanticMembersIfNeeded(Type type, DeclNameRef name) {
  // Look-through class-bound archetypes to ensure we synthesize e.g.
  // inherited constructors.
  if (auto archetypeTy = type->getAs<ArchetypeType>()) {
    if (auto super = archetypeTy->getSuperclass()) {
      type = super;
    }
  }

  if (type->isExistentialType()) {
    auto layout = type->getExistentialLayout();
    if (auto super = layout.explicitSuperclass) {
      type = super;
    }
  }

  if (auto *current = type->getAnyNominal()) {
    current->synthesizeSemanticMembersIfNeeded(name.getFullName());
  }
}

/// Inspect the given type to determine which nominal type declarations it
/// directly references, to facilitate name lookup into those types.
void namelookup::extractDirectlyReferencedNominalTypes(
    Type type, SmallVectorImpl<NominalTypeDecl *> &decls) {
  if (auto nominal = type->getAnyNominal()) {
    decls.push_back(nominal);
    return;
  }

  if (auto unbound = type->getAs<UnboundGenericType>()) {
    if (auto nominal = dyn_cast<NominalTypeDecl>(unbound->getDecl()))
      decls.push_back(nominal);
    return;
  }

  if (auto archetypeTy = type->getAs<ArchetypeType>()) {
    // Look in the protocols to which the archetype conforms (always).
    for (auto proto : archetypeTy->getConformsTo())
      decls.push_back(proto);

    // Look into the superclasses of this archetype.
    if (auto superclass = archetypeTy->getSuperclass()) {
      if (auto superclassDecl = superclass->getClassOrBoundGenericClass())
        decls.push_back(superclassDecl);
    }

    return;
  }

  if (auto compositionTy = type->getAs<ProtocolCompositionType>()) {
    auto layout = compositionTy->getExistentialLayout();

    for (auto protoDecl : layout.getProtocols()) {
      decls.push_back(protoDecl);
    }

    if (auto superclass = layout.explicitSuperclass) {
      auto *superclassDecl = superclass->getClassOrBoundGenericClass();
      if (superclassDecl)
        decls.push_back(superclassDecl);
    }

    return;
  }

  if (auto existential = type->getAs<ExistentialType>()) {
    extractDirectlyReferencedNominalTypes(
        existential->getConstraintType(), decls);
    return;
  }

  if (type->is<TupleType>()) {
    decls.push_back(type->getASTContext().getBuiltinTupleDecl());
    return;
  }

  llvm_unreachable("Not a type containing nominal types?");
}

void namelookup::tryExtractDirectlyReferencedNominalTypes(
    Type type, SmallVectorImpl<NominalTypeDecl *> &decls) {
  if (!type->is<ModuleType>() && type->mayHaveMembers())
    namelookup::extractDirectlyReferencedNominalTypes(type, decls);
}

bool DeclContext::lookupQualified(Type type,
                                  DeclNameRef member,
                                  SourceLoc loc,
                                  NLOptions options,
                                  SmallVectorImpl<ValueDecl *> &decls) const {
  using namespace namelookup;
  assert(decls.empty() && "additive lookup not supported");

  // Handle AnyObject lookup.
  if (type->isAnyObject()) {
    AnyObjectLookupRequest req(this, member, options);
    decls = evaluateOrDefault(getASTContext().evaluator, req, {});
    return !decls.empty();
  }

  // Handle lookup in a module.
  if (auto moduleTy = type->getAs<ModuleType>())
    return lookupQualified(moduleTy->getModule(), member,
                           loc, options, decls);

  // Figure out which nominal types we will look into.
  SmallVector<NominalTypeDecl *, 4> nominalTypesToLookInto;
  namelookup::extractDirectlyReferencedNominalTypes(type,
                                                    nominalTypesToLookInto);

  return lookupQualified(nominalTypesToLookInto, member,
                         loc, options, decls);
}

static void installPropertyWrapperMembersIfNeeded(NominalTypeDecl *target,
                                                  DeclNameRef member) {
  auto &Context = target->getASTContext();
  auto baseName = member.getBaseName();
  if (!member.isSimpleName() || baseName.isSpecial())
    return;

  if ((!baseName.getIdentifier().str().startswith("$") &&
       !baseName.getIdentifier().str().startswith("_")) ||
      baseName.getIdentifier().str().size() <= 1) {
    return;
  }

  // $- and _-prefixed variables can be generated by properties that have
  // attached property wrappers.
  auto originalPropertyName =
      Context.getIdentifier(baseName.getIdentifier().str().substr(1));
  for (auto member : target->lookupDirect(originalPropertyName)) {
    if (auto var = dyn_cast<VarDecl>(member)) {
      if (var->hasAttachedPropertyWrapper()) {
        auto sourceFile = var->getDeclContext()->getParentSourceFile();
        if (sourceFile && sourceFile->Kind != SourceFileKind::Interface) {
          (void)var->getPropertyWrapperAuxiliaryVariables();
          (void)var->getPropertyWrapperInitializerInfo();
        }
      }
    }
  }
}

bool DeclContext::lookupQualified(ArrayRef<NominalTypeDecl *> typeDecls,
                                  DeclNameRef member,
                                  SourceLoc loc, NLOptions options,
                                  SmallVectorImpl<ValueDecl *> &decls) const {
  assert(decls.empty() && "additive lookup not supported");
  QualifiedLookupRequest req{this, {typeDecls.begin(), typeDecls.end()},
                             member, loc, options};
  decls = evaluateOrDefault(getASTContext().evaluator, req, {});
  return !decls.empty();
}

QualifiedLookupResult
QualifiedLookupRequest::evaluate(Evaluator &eval, const DeclContext *DC,
                                 SmallVector<NominalTypeDecl *, 4> typeDecls,
                                 DeclNameRef member, NLOptions options) const {
  using namespace namelookup;
  QualifiedLookupResult decls;

  // Tracking for the nominal types we'll visit.
  SmallVector<NominalTypeDecl *, 4> stack;
  llvm::SmallPtrSet<NominalTypeDecl *, 4> visited;
  bool sawClassDecl = false;

  // Add the given nominal type to the stack.
  auto addNominalType = [&](NominalTypeDecl *nominal) {
    if (!visited.insert(nominal).second)
      return false;

    if (isa<ClassDecl>(nominal))
      sawClassDecl = true;

    stack.push_back(nominal);
    return true;
  };

  // Add all of the nominal types to the stack.
  for (auto nominal : typeDecls) {
    addNominalType(nominal);
  }

  // Whether we only want to return complete object initializers.
  bool onlyCompleteObjectInits = false;

  // Visit all of the nominal types we know about, discovering any others
  // we need along the way.
  bool wantProtocolMembers = (options & NL_ProtocolMembers);
  while (!stack.empty()) {
    auto current = stack.back();
    stack.pop_back();

    // Make sure we've resolved property wrappers, if we need them.
    installPropertyWrapperMembersIfNeeded(current, member);

    // Look for results within the current nominal type and its extensions.
    bool currentIsProtocol = isa<ProtocolDecl>(current);
    auto flags = OptionSet<NominalTypeDecl::LookupDirectFlags>();
    if (options & NL_IncludeAttributeImplements)
      flags |= NominalTypeDecl::LookupDirectFlags::IncludeAttrImplements;
    if (options & NL_ExcludeMacroExpansions)
      flags |= NominalTypeDecl::LookupDirectFlags::ExcludeMacroExpansions;

    // Note that the source loc argument doesn't matter, because excluding
    // macro expansions is already propagated through the lookup flags above.
    for (auto decl : current->lookupDirect(member.getFullName(),
                                           SourceLoc(), flags)) {
      // If we're performing a type lookup, don't even attempt to validate
      // the decl if its not a type.
      if ((options & NL_OnlyTypes) && !isa<TypeDecl>(decl))
        continue;

      if (isAcceptableLookupResult(DC, options, decl, onlyCompleteObjectInits))
        decls.push_back(decl);
    }

    // Visit superclass.
    if (auto classDecl = dyn_cast<ClassDecl>(current)) {
      // If we're looking for initializers, only look at the superclass if the
      // current class permits inheritance. Even then, only find complete
      // object initializers.
      bool visitSuperclass = true;
      if (member.getBaseName() == DeclBaseName::createConstructor()) {
        if (classDecl->inheritsSuperclassInitializers())
          onlyCompleteObjectInits = true;
        else
          visitSuperclass = false;
      }

      if (visitSuperclass) {
        if (auto superclassDecl = classDecl->getSuperclassDecl())
          if (visited.insert(superclassDecl).second)
            stack.push_back(superclassDecl);
      }
    }

    // If we're not looking at a protocol and we're not supposed to
    // visit the protocols that this type conforms to, skip the next
    // step.
    if (!wantProtocolMembers && !currentIsProtocol)
      continue;

    if (auto *protoDecl = dyn_cast<ProtocolDecl>(current)) {
      // If we haven't seen a class declaration yet, look into the protocol.
      if (!sawClassDecl) {
        if (auto superclassDecl = protoDecl->getSuperclassDecl()) {
          visited.insert(superclassDecl);
          stack.push_back(superclassDecl);
        }
      }

      // Collect inherited protocols.
      for (auto inheritedProto : protoDecl->getInheritedProtocols()) {
        addNominalType(inheritedProto);
      }
    } else {
      // Collect the protocols to which the nominal type conforms.
      for (auto proto : current->getAllProtocols()) {
        if (visited.insert(proto).second) {
          stack.push_back(proto);
        }
      }

      // For a class, we don't need to visit the protocol members of the
      // superclass: that's already handled.
      if (isa<ClassDecl>(current))
        wantProtocolMembers = false;
    }
  }

  pruneLookupResultSet(DC, options, decls);
  if (auto *debugClient = DC->getParentModule()->getDebugClient()) {
    debugClient->finishLookupInNominals(DC, typeDecls, member.getFullName(),
                                        options, decls);
  }

  return decls;
}

bool DeclContext::lookupQualified(ModuleDecl *module, DeclNameRef member,
                                  SourceLoc loc, NLOptions options,
                                  SmallVectorImpl<ValueDecl *> &decls) const {
  assert(decls.empty() && "additive lookup not supported");
  ModuleQualifiedLookupRequest req{this, module, member, loc, options};
  decls = evaluateOrDefault(getASTContext().evaluator, req, {});
  return !decls.empty();
}

QualifiedLookupResult
ModuleQualifiedLookupRequest::evaluate(Evaluator &eval, const DeclContext *DC,
                                       ModuleDecl *module, DeclNameRef member,
                                       NLOptions options) const {
  using namespace namelookup;
  QualifiedLookupResult decls;

  auto kind = (options & NL_OnlyTypes
               ? ResolutionKind::TypesOnly
               : ResolutionKind::Overloadable);
  auto topLevelScope = DC->getModuleScopeContext();
  if (module == topLevelScope->getParentModule()) {
    lookupInModule(module, member.getFullName(), decls, NLKind::QualifiedLookup,
                   kind, topLevelScope, SourceLoc(), options);
  } else {
    // Note: This is a lookup into another module. Unless we're compiling
    // multiple modules at once, or if the other module re-exports this one,
    // it shouldn't be possible to have a dependency from that module on
    // anything in this one.

    // Perform the lookup in all imports of this module.
    auto &ctx = DC->getASTContext();
    auto accessPaths = ctx.getImportCache().getAllVisibleAccessPaths(
        module, topLevelScope);
    if (llvm::any_of(accessPaths,
                     [&](ImportPath::Access accessPath) {
                       return accessPath.matches(member.getFullName());
                     })) {
      lookupInModule(module, member.getFullName(), decls,
                     NLKind::QualifiedLookup, kind, topLevelScope,
                     SourceLoc(), options);
    }
  }

  pruneLookupResultSet(DC, options, decls);

  if (auto *debugClient = DC->getParentModule()->getDebugClient()) {
    debugClient->finishLookupInModule(DC, module, member.getFullName(),
                                      options, decls);
  }

  return decls;
}

QualifiedLookupResult
AnyObjectLookupRequest::evaluate(Evaluator &evaluator, const DeclContext *dc,
                                 DeclNameRef member, NLOptions options) const {
  using namespace namelookup;
  QualifiedLookupResult decls;

  // Type-only lookup won't find anything on AnyObject.
  if (options & NL_OnlyTypes)
    return decls;

  // Collect all of the visible declarations.
  SmallVector<ValueDecl *, 4> allDecls;
  for (auto import : namelookup::getAllImports(dc)) {
    import.importedModule->lookupClassMember(import.accessPath,
                                             member.getFullName(), allDecls);
  }

  // For each declaration whose context is not something we've
  // already visited above, add it to the list of declarations.
  llvm::SmallPtrSet<ValueDecl *, 4> knownDecls;
  for (auto decl : allDecls) {
    // If the declaration is not @objc, it cannot be called dynamically.
    if (!decl->isObjC())
      continue;

    // If the declaration is objc_direct, it cannot be called dynamically.
    if (auto clangDecl = decl->getClangDecl()) {
      if (auto objCMethod = dyn_cast<clang::ObjCMethodDecl>(clangDecl)) {
        if (objCMethod->isDirectMethod())
          continue;
      } else if (auto objCProperty = dyn_cast<clang::ObjCPropertyDecl>(clangDecl)) {
        if (objCProperty->isDirectProperty())
          continue;
      }
    }

    // If the declaration has an override, name lookup will also have
    // found the overridden method. Skip this declaration, because we
    // prefer the overridden method.
    if (decl->getOverriddenDecl())
      continue;

    assert(decl->getDeclContext()->isTypeContext() &&
           "Couldn't find nominal type?");

    // If we didn't see this declaration before, and it's an acceptable
    // result, add it to the list.
    if (knownDecls.insert(decl).second &&
        isAcceptableLookupResult(dc, options, decl,
                                 /*onlyCompleteObjectInits=*/false))
      decls.push_back(decl);
  }

  pruneLookupResultSet(dc, options, decls);
  if (auto *debugClient = dc->getParentModule()->getDebugClient()) {
    debugClient->finishLookupInAnyObject(dc, member.getFullName(), options,
                                         decls);
  }
  return decls;
}

void DeclContext::lookupAllObjCMethods(
       ObjCSelector selector,
       SmallVectorImpl<AbstractFunctionDecl *> &results) const {
  // Collect all of the methods with this selector.
  for (auto import : namelookup::getAllImports(this)) {
    import.importedModule->lookupObjCMethods(selector, results);
  }

  // Filter out duplicates.
  llvm::SmallPtrSet<AbstractFunctionDecl *, 8> visited;
  results.erase(
    std::remove_if(results.begin(), results.end(),
                   [&](AbstractFunctionDecl *func) -> bool {
                     return !visited.insert(func).second;
                   }),
    results.end());
}

/// Given a set of type declarations, find all of the nominal type declarations
/// that they reference, looking through typealiases as appropriate.
static TinyPtrVector<NominalTypeDecl *>
resolveTypeDeclsToNominal(Evaluator &evaluator,
                          ASTContext &ctx,
                          ArrayRef<TypeDecl *> typeDecls,
                          SmallVectorImpl<ModuleDecl *> &modulesFound,
                          bool &anyObject,
                          llvm::SmallPtrSetImpl<TypeAliasDecl *> &typealiases) {
  SmallPtrSet<NominalTypeDecl *, 4> knownNominalDecls;
  TinyPtrVector<NominalTypeDecl *> nominalDecls;
  auto addNominalDecl = [&](NominalTypeDecl *nominal) {
    if (knownNominalDecls.insert(nominal).second)
      nominalDecls.push_back(nominal);
  };

  for (auto typeDecl : typeDecls) {
    // Nominal type declarations get copied directly.
    if (auto nominalDecl = dyn_cast<NominalTypeDecl>(typeDecl)) {
      addNominalDecl(nominalDecl);
      continue;
    }

    // Recursively resolve typealiases.
    if (auto typealias = dyn_cast<TypeAliasDecl>(typeDecl)) {
      // FIXME: Ad hoc recursion breaking, so we don't look through the
      // same typealias multiple times.
      if (!typealiases.insert(typealias).second)
        continue;

      auto underlyingTypeReferences = evaluateOrDefault(evaluator,
        UnderlyingTypeDeclsReferencedRequest{typealias}, {});

      auto underlyingNominalReferences
        = resolveTypeDeclsToNominal(evaluator, ctx, underlyingTypeReferences,
                                    modulesFound, anyObject, typealiases);
      std::for_each(underlyingNominalReferences.begin(),
                    underlyingNominalReferences.end(),
                    addNominalDecl);

      // Recognize Swift.AnyObject directly.
      if (typealias->getName().is("AnyObject")) {
        // TypeRepr version: Builtin.AnyObject
        if (auto typeRepr = typealias->getUnderlyingTypeRepr()) {
          if (auto memberTR = dyn_cast<MemberTypeRepr>(typeRepr)) {
            if (auto identBase =
                    dyn_cast<IdentTypeRepr>(memberTR->getBaseComponent())) {
              auto memberComps = memberTR->getMemberComponents();
              if (memberComps.size() == 1 &&
                  identBase->getNameRef().isSimpleName("Builtin") &&
                  memberComps.front()->getNameRef().isSimpleName("AnyObject")) {
                anyObject = true;
              }
            }
          }
        }

        // Type version: an empty class-bound existential.
        if (typealias->hasInterfaceType()) {
          if (auto type = typealias->getUnderlyingType())
            if (type->isAnyObject())
              anyObject = true;
        }
      }

      continue;
    }

    // Keep track of modules we see.
    if (auto module = dyn_cast<ModuleDecl>(typeDecl)) {
      modulesFound.push_back(module);
      continue;
    }

    // Make sure we didn't miss some interesting kind of type declaration.
    assert(isa<GenericTypeParamDecl>(typeDecl) ||
           isa<AssociatedTypeDecl>(typeDecl));
  }

  return nominalDecls;
}

static TinyPtrVector<NominalTypeDecl *>
resolveTypeDeclsToNominal(Evaluator &evaluator,
                          ASTContext &ctx,
                          ArrayRef<TypeDecl *> typeDecls,
                          SmallVectorImpl<ModuleDecl *> &modulesFound,
                          bool &anyObject) {
  llvm::SmallPtrSet<TypeAliasDecl *, 4> typealiases;
  return resolveTypeDeclsToNominal(evaluator, ctx, typeDecls, modulesFound,
                                   anyObject, typealiases);
}

/// Perform unqualified name lookup for types at the given location.
static DirectlyReferencedTypeDecls
directReferencesForUnqualifiedTypeLookup(DeclNameRef name,
                                         SourceLoc loc, DeclContext *dc,
                                         LookupOuterResults lookupOuter,
                                         bool allowUsableFromInline=false) {
  UnqualifiedLookupOptions options =
      UnqualifiedLookupFlags::TypeLookup |
      UnqualifiedLookupFlags::AllowProtocolMembers;
  if (lookupOuter == LookupOuterResults::Included)
    options |= UnqualifiedLookupFlags::IncludeOuterResults;

  if (allowUsableFromInline)
    options |= UnqualifiedLookupFlags::IncludeUsableFromInline;

  // Manually exclude macro expansions here since the source location
  // is overridden below.
  if (namelookup::isInMacroArgument(dc->getParentSourceFile(), loc))
    options |= UnqualifiedLookupFlags::ExcludeMacroExpansions;

  // In a protocol or protocol extension, the 'where' clause can refer to
  // associated types without 'Self' qualification:
  //
  // protocol MyProto where AssocType : Q { ... }
  //
  // extension MyProto where AssocType == Int { ... }
  //
  // For this reason, ASTScope maps source locations inside the 'where'
  // clause to a scope that performs the lookup into the protocol or
  // protocol extension.
  //
  // However, protocol and protocol extensions can also put bounds on 'Self',
  // for example:
  //
  // protocol MyProto where Self : MyClass { ... }
  //
  // We must start searching for 'MyClass' at the top level, otherwise
  // we end up with a cycle, because qualified lookup wants to resolve
  // 'Self' bounds to build the set of declarations to search inside of.
  //
  // To make this work, we handle the top-level lookup case explicitly
  // here, bypassing unqualified lookup and ASTScope altogether.
  if (dc->isModuleScopeContext())
    loc = SourceLoc();

  DirectlyReferencedTypeDecls results;

  auto &ctx = dc->getASTContext();
  auto descriptor = UnqualifiedLookupDescriptor(name, dc, loc, options);
  auto lookup = evaluateOrDefault(ctx.evaluator,
                                  UnqualifiedLookupRequest{descriptor}, {});

  unsigned nominalTypeDeclCount = 0;
  for (const auto &result : lookup.allResults()) {
    auto typeDecl = cast<TypeDecl>(result.getValueDecl());

    if (isa<NominalTypeDecl>(typeDecl))
      nominalTypeDeclCount++;

    results.push_back(typeDecl);
  }

  // If we saw multiple nominal type declarations with the same name,
  // the result of the lookup is definitely ambiguous.
  if (nominalTypeDeclCount > 1)
    results.clear();

  return results;
}

/// Perform qualified name lookup for types.
static DirectlyReferencedTypeDecls
directReferencesForQualifiedTypeLookup(Evaluator &evaluator,
                                       ASTContext &ctx,
                                       ArrayRef<TypeDecl *> baseTypes,
                                       DeclNameRef name,
                                       DeclContext *dc,
                                       SourceLoc loc,
                                       bool allowUsableFromInline=false) {
  DirectlyReferencedTypeDecls result;
  auto addResults = [&result](ArrayRef<ValueDecl *> found){
    for (auto decl : found){
      assert(isa<TypeDecl>(decl) &&
             "Lookup should only have found type declarations");
      result.push_back(cast<TypeDecl>(decl));
    }
  };

  {
    // Look into the base types.
    SmallVector<ValueDecl *, 4> members;
    auto options = NL_RemoveNonVisible | NL_OnlyTypes;

    if (allowUsableFromInline)
      options |= NL_IncludeUsableFromInline;

    // Look through the type declarations we were given, resolving them down
    // to nominal type declarations, module declarations, and
    SmallVector<ModuleDecl *, 2> moduleDecls;
    bool anyObject = false;
    auto nominalTypeDecls =
      resolveTypeDeclsToNominal(ctx.evaluator, ctx, baseTypes, moduleDecls,
                                anyObject);

    dc->lookupQualified(nominalTypeDecls, name, loc, options, members);

    // Search all of the modules.
    for (auto module : moduleDecls) {
      auto innerOptions = options;
      innerOptions &= ~NL_RemoveOverridden;
      innerOptions &= ~NL_RemoveNonVisible;
      SmallVector<ValueDecl *, 4> moduleMembers;
      dc->lookupQualified(module, name, loc, innerOptions, moduleMembers);
      members.append(moduleMembers.begin(), moduleMembers.end());
    }

    addResults(members);
  }

  return result;
}

/// Determine the types directly referenced by the given identifier type.
static DirectlyReferencedTypeDecls
directReferencesForDeclRefTypeRepr(Evaluator &evaluator, ASTContext &ctx,
                                   DeclRefTypeRepr *repr, DeclContext *dc,
                                   bool allowUsableFromInline) {
  DirectlyReferencedTypeDecls current;

  auto *baseComp = repr->getBaseComponent();
  if (auto *identBase = dyn_cast<IdentTypeRepr>(baseComp)) {
    // If we already set a declaration, use it.
    if (auto *typeDecl = identBase->getBoundDecl()) {
      current = {1, typeDecl};
    } else {
      // For the base component, perform unqualified name lookup.
      current = directReferencesForUnqualifiedTypeLookup(
          identBase->getNameRef(), identBase->getLoc(), dc,
          LookupOuterResults::Excluded, allowUsableFromInline);
    }
  } else {
    current = directReferencesForTypeRepr(evaluator, ctx, baseComp, dc,
                                          allowUsableFromInline);
  }

  auto *memberTR = dyn_cast<MemberTypeRepr>(repr);
  if (!memberTR)
    return current;

  // If we didn't find anything, fail now.
  if (current.empty())
    return current;

  for (const auto &component : memberTR->getMemberComponents()) {
    // If we already set a declaration, use it.
    if (auto typeDecl = component->getBoundDecl()) {
      current = {1, typeDecl};
      continue;
    }

    // For subsequent components, perform qualified name lookup.
    current =
        directReferencesForQualifiedTypeLookup(evaluator, ctx, current,
                                               component->getNameRef(), dc,
                                               component->getLoc(),
                                               allowUsableFromInline);
    if (current.empty())
      return current;
  }

  return current;
}

static DirectlyReferencedTypeDecls
directReferencesForTypeRepr(Evaluator &evaluator,
                            ASTContext &ctx, TypeRepr *typeRepr,
                            DeclContext *dc, bool allowUsableFromInline) {
  switch (typeRepr->getKind()) {
  case TypeReprKind::Array:
    return {1, ctx.getArrayDecl()};

  case TypeReprKind::Attributed: {
    auto attributed = cast<AttributedTypeRepr>(typeRepr);
    return directReferencesForTypeRepr(evaluator, ctx,
                                       attributed->getTypeRepr(), dc,
                                       allowUsableFromInline);
  }

  case TypeReprKind::Composition: {
    DirectlyReferencedTypeDecls result;
    auto composition = cast<CompositionTypeRepr>(typeRepr);
    for (auto component : composition->getTypes()) {
      auto componentResult =
          directReferencesForTypeRepr(evaluator, ctx, component, dc,
                                      allowUsableFromInline);
      result.insert(result.end(),
                    componentResult.begin(),
                    componentResult.end());
    }
    return result;
  }

  case TypeReprKind::Member:
  case TypeReprKind::GenericIdent:
  case TypeReprKind::SimpleIdent:
    return directReferencesForDeclRefTypeRepr(evaluator, ctx,
                                              cast<DeclRefTypeRepr>(typeRepr),
                                              dc, allowUsableFromInline);

  case TypeReprKind::Dictionary:
    return { 1, ctx.getDictionaryDecl()};

  case TypeReprKind::Tuple: {
    auto tupleRepr = cast<TupleTypeRepr>(typeRepr);
    if (tupleRepr->isParenType()) {
      return directReferencesForTypeRepr(evaluator, ctx,
                                         tupleRepr->getElementType(0), dc,
                                         allowUsableFromInline);
    }
    return { };
  }

  case TypeReprKind::Vararg: {
    auto packExpansionRepr = cast<VarargTypeRepr>(typeRepr);
    return directReferencesForTypeRepr(evaluator, ctx,
                                       packExpansionRepr->getElementType(), dc,
                                       allowUsableFromInline);
  }

  case TypeReprKind::PackExpansion: {
    auto packExpansionRepr = cast<PackExpansionTypeRepr>(typeRepr);
    return directReferencesForTypeRepr(evaluator, ctx,
                                       packExpansionRepr->getPatternType(), dc,
                                       allowUsableFromInline);
  }

  case TypeReprKind::PackElement: {
    auto packReferenceRepr = cast<PackElementTypeRepr>(typeRepr);
    return directReferencesForTypeRepr(evaluator, ctx,
                                       packReferenceRepr->getPackType(), dc,
                                       allowUsableFromInline);
  }

  case TypeReprKind::Error:
  case TypeReprKind::Function:
  case TypeReprKind::Ownership:
  case TypeReprKind::Isolated:
  case TypeReprKind::CompileTimeConst:
  case TypeReprKind::Metatype:
  case TypeReprKind::Protocol:
  case TypeReprKind::SILBox:
  case TypeReprKind::Placeholder:
  case TypeReprKind::Pack:
    return { };

  case TypeReprKind::OpaqueReturn:
  case TypeReprKind::NamedOpaqueReturn:
  case TypeReprKind::Existential:
    return { };

  case TypeReprKind::Fixed:
    llvm_unreachable("Cannot get fixed TypeReprs in name lookup");
  case TypeReprKind::Self:
    llvm_unreachable("Cannot get fixed SelfTypeRepr in name lookup");

  case TypeReprKind::Optional:
  case TypeReprKind::ImplicitlyUnwrappedOptional:
    return { 1, ctx.getOptionalDecl() };
  }
  llvm_unreachable("unhandled kind");
}

static DirectlyReferencedTypeDecls directReferencesForType(Type type) {
  // If it's a typealias, return that.
  if (auto aliasType = dyn_cast<TypeAliasType>(type.getPointer()))
    return { 1, aliasType->getDecl() };

  // If there is a generic declaration, return it.
  if (auto genericDecl = type->getAnyGeneric())
    return { 1, genericDecl };

  if (type->isExistentialType()) {
    DirectlyReferencedTypeDecls result;
    const auto &layout = type->getExistentialLayout();

    // Superclass.
    if (auto superclassType = layout.explicitSuperclass) {
      if (auto superclassDecl = superclassType->getAnyGeneric()) {
        result.push_back(superclassDecl);
      }
    }

    // Protocols.
    for (auto protoDecl : layout.getProtocols())
      result.push_back(protoDecl);
    return result;
  }

  return { };
}

DirectlyReferencedTypeDecls InheritedDeclsReferencedRequest::evaluate(
    Evaluator &evaluator,
    llvm::PointerUnion<const TypeDecl *, const ExtensionDecl *> decl,
    unsigned index) const {

  // Prefer syntactic information when we have it.
  const TypeLoc &typeLoc = getInheritedTypeLocAtIndex(decl, index);
  if (auto typeRepr = typeLoc.getTypeRepr()) {
    // Figure out the context in which name lookup will occur.
    DeclContext *dc;
    if (auto typeDecl = decl.dyn_cast<const TypeDecl *>())
      dc = typeDecl->getInnermostDeclContext();
    else
      dc = (DeclContext *)decl.get<const ExtensionDecl *>();

    return directReferencesForTypeRepr(evaluator, dc->getASTContext(), typeRepr,
                                       const_cast<DeclContext *>(dc));
  }

  // Fall back to semantic types.
  // FIXME: In the long run, we shouldn't need this. Non-syntactic results
  // should be cached.
  if (auto type = typeLoc.getType()) {
    return directReferencesForType(type);
  }

  return { };
}

DirectlyReferencedTypeDecls UnderlyingTypeDeclsReferencedRequest::evaluate(
    Evaluator &evaluator,
    TypeAliasDecl *typealias) const {
  // Prefer syntactic information when we have it.
  if (auto typeRepr = typealias->getUnderlyingTypeRepr()) {
    return directReferencesForTypeRepr(evaluator, typealias->getASTContext(),
                                       typeRepr, typealias);
  }

  // Fall back to semantic types.
  // FIXME: In the long run, we shouldn't need this. Non-syntactic results
  // should be cached.
  if (auto type = typealias->getUnderlyingType()) {
    return directReferencesForType(type);
  }

  return { };
}

/// Evaluate a superclass declaration request.
ClassDecl *
SuperclassDeclRequest::evaluate(Evaluator &evaluator,
                                NominalTypeDecl *subject) const {
  auto &Ctx = subject->getASTContext();

  // Protocols may get their superclass bound from a `where Self : Superclass`
  // clause.
  if (auto *proto = dyn_cast<ProtocolDecl>(subject)) {
    // If the protocol came from a serialized module, compute the superclass via
    // its generic signature.
    if (proto->wasDeserialized()) {
      auto superTy = proto->getGenericSignature()
          ->getSuperclassBound(proto->getSelfInterfaceType());
      if (superTy)
        return superTy->getClassOrBoundGenericClass();
    }

    // Otherwise check the where clause.
    auto selfBounds = getSelfBoundsFromWhereClause(proto);
    for (auto inheritedNominal : selfBounds.decls)
      if (auto classDecl = dyn_cast<ClassDecl>(inheritedNominal))
        return classDecl;
  }

  for (unsigned i : indices(subject->getInherited())) {
    // Find the inherited declarations referenced at this position.
    auto inheritedTypes = evaluateOrDefault(evaluator,
      InheritedDeclsReferencedRequest{subject, i}, {});

    // Resolve those type declarations to nominal type declarations.
    SmallVector<ModuleDecl *, 2> modulesFound;
    bool anyObject = false;
    auto inheritedNominalTypes
      = resolveTypeDeclsToNominal(evaluator, Ctx,
                                  inheritedTypes, modulesFound, anyObject);

    // Look for a class declaration.
    ClassDecl *superclass = nullptr;
    for (auto inheritedNominal : inheritedNominalTypes) {
      if (auto classDecl = dyn_cast<ClassDecl>(inheritedNominal)) {
        superclass = classDecl;
        break;
      }
    }

    // If we found a superclass, ensure that we don't have a circular
    // inheritance hierarchy by evaluating its superclass. This forces the
    // diagnostic at this point and then suppresses the superclass failure.
    if (superclass) {
      auto result = Ctx.evaluator(SuperclassDeclRequest{superclass});
      bool hadCycle = false;
      if (auto err = result.takeError()) {
        llvm::handleAllErrors(std::move(err),
          [&hadCycle](const CyclicalRequestError<SuperclassDeclRequest> &E) {
          hadCycle = true;
          });

        if (hadCycle)
          return nullptr;
      }

      return superclass;
    }
  }

  return nullptr;
}

ArrayRef<ProtocolDecl *>
InheritedProtocolsRequest::evaluate(Evaluator &evaluator,
                                    ProtocolDecl *PD) const {
  llvm::SmallVector<ProtocolDecl *, 2> result;
  SmallPtrSet<const ProtocolDecl *, 2> known;
  known.insert(PD);
  bool anyObject = false;
  for (const auto &found : getDirectlyInheritedNominalTypeDecls(PD, anyObject)) {
    if (auto proto = dyn_cast<ProtocolDecl>(found.Item)) {
      if (known.insert(proto).second)
        result.push_back(proto);
    }
  }

  return PD->getASTContext().AllocateCopy(result);
}

ArrayRef<ValueDecl *>
ProtocolRequirementsRequest::evaluate(Evaluator &evaluator,
                                      ProtocolDecl *PD) const {
  SmallVector<ValueDecl *, 4> requirements;

  for (auto *member : PD->getABIMembers()) {
    auto *VD = dyn_cast<ValueDecl>(member);
    if (VD && VD->isProtocolRequirement())
      requirements.push_back(VD);
  }

  return PD->getASTContext().AllocateCopy(requirements);
}

NominalTypeDecl *
ExtendedNominalRequest::evaluate(Evaluator &evaluator,
                                 ExtensionDecl *ext) const {
  auto typeRepr = ext->getExtendedTypeRepr();
  if (!typeRepr)
    // We must've seen 'extension { ... }' during parsing.
    return nullptr;

  ASTContext &ctx = ext->getASTContext();
  DirectlyReferencedTypeDecls referenced =
    directReferencesForTypeRepr(evaluator, ctx, typeRepr, ext->getParent(),
                                ext->isInSpecializeExtensionContext());

  // Resolve those type declarations to nominal type declarations.
  SmallVector<ModuleDecl *, 2> modulesFound;
  bool anyObject = false;
  auto nominalTypes
    = resolveTypeDeclsToNominal(evaluator, ctx, referenced, modulesFound,
                                anyObject);

  // If there is more than 1 element, we will emit a warning or an error
  // elsewhere, so don't handle that case here.
  if (nominalTypes.empty())
    return nullptr;

  // Diagnose experimental tuple extensions.
  if (isa<BuiltinTupleDecl>(nominalTypes[0]) &&
      !ctx.LangOpts.hasFeature(Feature::TupleConformances)) {
    ext->diagnose(diag::experimental_tuple_extension);
  }

  return nominalTypes[0];
}

/// Whether there are only associated types in the set of declarations.
static bool declsAreAssociatedTypes(ArrayRef<TypeDecl *> decls) {
  if (decls.empty())
    return false;

  for (auto decl : decls) {
    if (!isa<AssociatedTypeDecl>(decl))
      return false;
  }

  return true;
}

/// Verify there is at least one protocols in the set of declarations.
static bool declsAreProtocols(ArrayRef<TypeDecl *> decls) {
  if (decls.empty())
    return false;  // Below, check outer type repr is a protocol, if not bail early
  return llvm::any_of(decls, [&](const TypeDecl *decl) {
    if (auto *alias = dyn_cast<TypeAliasDecl>(decl)) {
      auto ty = alias->getUnderlyingType();
      decl = ty->getNominalOrBoundGenericNominal();
      if (decl == nullptr || ty->is<ExistentialType>())
        return false;
    }
    return isa<ProtocolDecl>(decl);
  });
}

bool TypeRepr::isProtocolOrProtocolComposition(DeclContext *dc){
  auto &ctx = dc->getASTContext();
    return declsAreProtocols(directReferencesForTypeRepr(ctx.evaluator, ctx, this, dc));
}

static GenericParamList *
createExtensionGenericParams(ASTContext &ctx,
                             ExtensionDecl *ext,
                             NominalTypeDecl *nominal) {
  // Collect generic parameters from all outer contexts.
  SmallVector<GenericParamList *, 2> allGenericParams;
  nominal->forEachGenericContext([&](GenericParamList *gpList) {
    allGenericParams.push_back(gpList->clone(ext));
  });

  GenericParamList *toParams = nullptr;
  for (auto *gpList : llvm::reverse(allGenericParams)) {
    gpList->setOuterParameters(toParams);
    toParams = gpList;
  }

  return toParams;
}

CollectedOpaqueReprs swift::collectOpaqueTypeReprs(TypeRepr *r, ASTContext &ctx,
                                                   DeclContext *d) {
  class Walker : public ASTWalker {
    CollectedOpaqueReprs &Reprs;
    ASTContext &Ctx;
    DeclContext *dc;

  public:
    explicit Walker(CollectedOpaqueReprs &reprs, ASTContext &ctx, DeclContext *d) : Reprs(reprs), Ctx(ctx), dc(d) {}

    /// Walk everything that's available.
    MacroWalking getMacroWalkingBehavior() const override {
      return MacroWalking::ArgumentsAndExpansion;
    }

    PreWalkAction walkToTypeReprPre(TypeRepr *repr) override {

      // Don't allow variadic opaque parameter or return types.
      if (isa<PackExpansionTypeRepr>(repr) || isa<VarargTypeRepr>(repr))
        return Action::SkipChildren();

      if (auto opaqueRepr = dyn_cast<OpaqueReturnTypeRepr>(repr)) {
        Reprs.push_back(opaqueRepr);
        if (Ctx.LangOpts.hasFeature(Feature::ImplicitSome))
          return Action::SkipChildren();
      }

      if (!Ctx.LangOpts.hasFeature(Feature::ImplicitSome))
        return Action::Continue();
      
      if (auto existential = dyn_cast<ExistentialTypeRepr>(repr)) {
        return Action::SkipChildren();
      } else if (auto composition = dyn_cast<CompositionTypeRepr>(repr)) {
        if (!composition->isTypeReprAny())
          Reprs.push_back(composition);
        return Action::SkipChildren();
      } else if (auto generic = dyn_cast<GenericIdentTypeRepr>(repr)) {
        if (generic->isProtocolOrProtocolComposition(dc)){
          Reprs.push_back(generic);
          return Action::SkipChildren();
        }
        return Action::Continue();
      } else if (auto declRef = dyn_cast<DeclRefTypeRepr>(repr)) {
        if (declRef->isProtocolOrProtocolComposition(dc))
          Reprs.push_back(declRef);
      }
      return Action::Continue();
    }

  };

  CollectedOpaqueReprs reprs;
  r->walk(Walker(reprs, ctx, d));
  return reprs;
}

/// If there are opaque parameters in the given declaration, create the
/// generic parameters associated with them.
static SmallVector<GenericTypeParamDecl *, 2>
createOpaqueParameterGenericParams(GenericContext *genericContext, GenericParamList *parsedGenericParams) {
  ASTContext &ctx = genericContext->getASTContext();
    
  auto value = dyn_cast_or_null<ValueDecl>(genericContext->getAsDecl());
  if (!value)
    return { };

  // Functions, initializers, and subscripts can contain opaque parameters.
  ParameterList *params = nullptr;
  if (auto func = dyn_cast<AbstractFunctionDecl>(value))
    params = func->getParameters();
  else if (auto subscript = dyn_cast<SubscriptDecl>(value))
    params = subscript->getIndices();
  else
    return { };

  // Look for parameters that have "some" types in them.
  unsigned index = parsedGenericParams ? parsedGenericParams->size() : 0;
  SmallVector<GenericTypeParamDecl *, 2> implicitGenericParams;
  auto dc = value->getInnermostDeclContext();
  for (auto param : *params) {
    auto typeRepr = param->getTypeRepr();
    if (!typeRepr)
      continue;

    // Plain protocols should imply 'some' with experimetal feature
    CollectedOpaqueReprs typeReprs;
    typeReprs = collectOpaqueTypeReprs(typeRepr, ctx, dc);

    for (auto repr : typeReprs) {
   
      // Allocate a new generic parameter to represent this opaque type.
      auto *gp = GenericTypeParamDecl::createImplicit(
          dc, Identifier(), GenericTypeParamDecl::InvalidDepth, index++,
          /*isParameterPack*/ false, /*isOpaqueType*/ true, repr,
          /*nameLoc*/ repr->getStartLoc());

      // Use the underlying constraint as the constraint on the generic parameter.
      //  The underlying constraint is only present for OpaqueReturnTypeReprs
      if (auto opaque = dyn_cast<OpaqueReturnTypeRepr>(repr)) {
              InheritedEntry inherited[1] = {
                  { TypeLoc(opaque->getConstraint()) }
              };
              gp->setInherited(ctx.AllocateCopy(inherited));
      } else {
            InheritedEntry inherited[1] = {
                { TypeLoc(repr) }
            };
            gp->setInherited(ctx.AllocateCopy(inherited));
      }
      implicitGenericParams.push_back(gp);
    }
  }

  return implicitGenericParams;
}

GenericParamList *
GenericParamListRequest::evaluate(Evaluator &evaluator, GenericContext *value) const {
  if (auto *tupleDecl = dyn_cast<BuiltinTupleDecl>(value)) {
    auto &ctx = value->getASTContext();

    // Builtin.TheTupleType has a single pack generic parameter: <Elements...>
    auto *genericParam = GenericTypeParamDecl::createImplicit(
        tupleDecl->getDeclContext(), ctx.Id_Elements, /*depth*/ 0, /*index*/ 0,
        /*isParameterPack*/ true);

    return GenericParamList::create(ctx, SourceLoc(), genericParam,
                                    SourceLoc());
  }

  if (auto *ext = dyn_cast<ExtensionDecl>(value)) {
    // Create the generic parameter list for the extension by cloning the
    // generic parameter lists of the nominal and any of its parent types.
    auto &ctx = value->getASTContext();
    auto *nominal = ext->getExtendedNominal();
    if (!nominal) {
      return nullptr;
    }
    auto *genericParams = createExtensionGenericParams(ctx, ext, nominal);

    // Protocol extensions need an inheritance clause due to how name lookup
    // is implemented.
    if (auto *proto = ext->getExtendedProtocolDecl()) {
      auto protoType = proto->getDeclaredInterfaceType();
      InheritedEntry selfInherited[1] = {
        InheritedEntry(TypeLoc::withoutLoc(protoType)) };
      genericParams->getParams().front()->setInherited(
        ctx.AllocateCopy(selfInherited));
    }

    // Set the depth of every generic parameter.
    unsigned depth = nominal->getGenericContextDepth();
    for (auto *outerParams = genericParams;
         outerParams != nullptr;
         outerParams = outerParams->getOuterParameters())
      outerParams->setDepth(depth--);

    return genericParams;
  }

  if (auto *proto = dyn_cast<ProtocolDecl>(value)) {
    // The generic parameter 'Self'.
    auto &ctx = value->getASTContext();
    auto selfId = ctx.Id_Self;
    auto selfDecl = GenericTypeParamDecl::createImplicit(
        proto, selfId, /*depth*/ 0, /*index*/ 0);
    auto protoType = proto->getDeclaredInterfaceType();
    InheritedEntry selfInherited[1] = {
      InheritedEntry(TypeLoc::withoutLoc(protoType)) };
    selfDecl->setInherited(ctx.AllocateCopy(selfInherited));
    selfDecl->setImplicit();

    // The generic parameter list itself.
    auto result = GenericParamList::create(ctx, SourceLoc(), selfDecl,
                                           SourceLoc());
    return result;
  }

  // AccessorDecl generic parameter list is the same of its storage
  // context.
  if (auto *AD = dyn_cast<AccessorDecl>(value)) {
    auto *GC = AD->getStorage()->getAsGenericContext();
    if (!GC)
      return nullptr;

    auto *GP = GC->getGenericParams();
    if (!GP)
      return nullptr;

    return GP->clone(AD->getDeclContext());
  }

  auto parsedGenericParams = value->getParsedGenericParams();

  // Create implicit generic parameters due to opaque parameters, if we need
  // them.
  auto implicitGenericParams =
      createOpaqueParameterGenericParams(value, parsedGenericParams);
  if (implicitGenericParams.empty())
    return parsedGenericParams;

  // If there were no parsed generic parameters, create a fully-implicit
  // generic parameter list.
  ASTContext &ctx = value->getASTContext();
  if (!parsedGenericParams) {
    return GenericParamList::create(
        ctx, SourceLoc(), implicitGenericParams, SourceLoc());
  }

  // Combine the existing generic parameters with the implicit ones.
  SmallVector<GenericTypeParamDecl *, 4> allGenericParams;
  allGenericParams.reserve(
      parsedGenericParams->size() + implicitGenericParams.size());
  allGenericParams.append(parsedGenericParams->begin(),
                          parsedGenericParams->end());
  allGenericParams.append(implicitGenericParams);
  return GenericParamList::create(
      ctx, parsedGenericParams->getLAngleLoc(), allGenericParams,
      parsedGenericParams->getWhereLoc(),
      parsedGenericParams->getRequirements(),
      parsedGenericParams->getRAngleLoc());
}

void swift::findMacroForCustomAttr(CustomAttr *attr, DeclContext *dc,
                                   llvm::TinyPtrVector<ValueDecl *> &macros) {
  auto *identTypeRepr = dyn_cast_or_null<IdentTypeRepr>(attr->getTypeRepr());
  if (!identTypeRepr)
    return;

  // Look for macros at module scope. They can only occur at module scope, and
  // we need to be sure not to trigger name lookup into type contexts along
  // the way.
  auto moduleScopeDC = dc->getModuleScopeContext();
  ASTContext &ctx = moduleScopeDC->getASTContext();
  UnqualifiedLookupDescriptor descriptor(
      identTypeRepr->getNameRef(), moduleScopeDC
  );
  auto lookup = evaluateOrDefault(
      ctx.evaluator, UnqualifiedLookupRequest{descriptor}, {});
  for (const auto &result : lookup.allResults()) {
    // Only keep attached macros, which can be spelled as custom attributes.
    if (auto macro = dyn_cast<MacroDecl>(result.getValueDecl()))
      if (isAttachedMacro(macro->getMacroRoles()))
        macros.push_back(macro);
  }
}

NominalTypeDecl *
CustomAttrNominalRequest::evaluate(Evaluator &evaluator,
                                   CustomAttr *attr, DeclContext *dc) const {
  // Look for names at module scope, so we don't trigger name lookup for
  // nested scopes. At this point, we're looking to see whether there are
  // any suitable macros.
  llvm::TinyPtrVector<ValueDecl *> macros;
  findMacroForCustomAttr(attr, dc, macros);
  if (!macros.empty())
    return nullptr;

  // Find the types referenced by the custom attribute.
  auto &ctx = dc->getASTContext();
  DirectlyReferencedTypeDecls decls;
  if (auto *typeRepr = attr->getTypeRepr()) {
    decls = directReferencesForTypeRepr(
        evaluator, ctx, typeRepr, dc);
  } else if (Type type = attr->getType()) {
    decls = directReferencesForType(type);
  }

  // Dig out the nominal type declarations.
  SmallVector<ModuleDecl *, 2> modulesFound;
  bool anyObject = false;
  auto nominals = resolveTypeDeclsToNominal(evaluator, ctx, decls,
                                            modulesFound, anyObject);
  if (nominals.size() == 1 && !isa<ProtocolDecl>(nominals.front()))
    return nominals.front();

  // If we found declarations that are associated types, look outside of
  // the current context to see if we can recover.
  if (declsAreAssociatedTypes(decls)) {
    if (auto typeRepr = attr->getTypeRepr()) {
      if (auto identTypeRepr = dyn_cast<SimpleIdentTypeRepr>(typeRepr)) {
        auto assocType = cast<AssociatedTypeDecl>(decls.front());

        modulesFound.clear();
        anyObject = false;
        decls = directReferencesForUnqualifiedTypeLookup(
            identTypeRepr->getNameRef(), identTypeRepr->getLoc(), dc,
            LookupOuterResults::Included);
        nominals = resolveTypeDeclsToNominal(evaluator, ctx, decls,
                                             modulesFound, anyObject);
        if (nominals.size() == 1 && !isa<ProtocolDecl>(nominals.front())) {
          auto nominal = nominals.front();
          if (nominal->getDeclContext()->isModuleScopeContext()) {
            // Complain, producing module qualification in a Fix-It.
            auto moduleName = nominal->getParentModule()->getName();
            ctx.Diags.diagnose(typeRepr->getLoc(),
                               diag::warn_property_wrapper_module_scope,
                               identTypeRepr->getNameRef(),
                               moduleName)
              .fixItInsert(typeRepr->getLoc(),
                           moduleName.str().str() + ".");
            ctx.Diags.diagnose(assocType, diag::kind_declname_declared_here,
                               assocType->getDescriptiveKind(),
                               assocType->getName());

            auto *baseComp = new (ctx) SimpleIdentTypeRepr(
                identTypeRepr->getNameLoc(), DeclNameRef(moduleName));

            auto *newTE = new (ctx) TypeExpr(
                MemberTypeRepr::create(ctx, baseComp, {identTypeRepr}));
            attr->resetTypeInformation(newTE);
            return nominal;
          }
        }
      }
    }
  }

  return nullptr;
}

void swift::getDirectlyInheritedNominalTypeDecls(
    llvm::PointerUnion<const TypeDecl *, const ExtensionDecl *> decl,
    unsigned i, llvm::SmallVectorImpl<InheritedNominalEntry> &result,
    bool &anyObject) {
  auto typeDecl = decl.dyn_cast<const TypeDecl *>();
  auto extDecl = decl.dyn_cast<const ExtensionDecl *>();

  ASTContext &ctx = typeDecl ? typeDecl->getASTContext()
                             : extDecl->getASTContext();

  // Find inherited declarations.
  auto referenced = evaluateOrDefault(ctx.evaluator,
    InheritedDeclsReferencedRequest{decl, i}, {});

  // Resolve those type declarations to nominal type declarations.
  SmallVector<ModuleDecl *, 2> modulesFound;
  auto nominalTypes
    = resolveTypeDeclsToNominal(ctx.evaluator, ctx, referenced, modulesFound,
                                anyObject);

  // Dig out the source location
  // FIXME: This is a hack. We need cooperation from
  // InheritedDeclsReferencedRequest to make this work.
  SourceLoc loc;
  SourceLoc uncheckedLoc;
  if (TypeRepr *typeRepr = typeDecl ? typeDecl->getInherited()[i].getTypeRepr()
                                    : extDecl->getInherited()[i].getTypeRepr()){
    loc = typeRepr->getLoc();
    uncheckedLoc = typeRepr->findUncheckedAttrLoc();
  }

  // Form the result.
  for (auto nominal : nominalTypes) {
    result.push_back({nominal, loc, uncheckedLoc});
  }
}

SmallVector<InheritedNominalEntry, 4>
swift::getDirectlyInheritedNominalTypeDecls(
    llvm::PointerUnion<const TypeDecl *, const ExtensionDecl *> decl,
    bool &anyObject) {
  auto typeDecl = decl.dyn_cast<const TypeDecl *>();
  auto extDecl = decl.dyn_cast<const ExtensionDecl *>();

  // Gather results from all of the inherited types.
  unsigned numInherited = typeDecl ? typeDecl->getInherited().size()
                                   : extDecl->getInherited().size();
  SmallVector<InheritedNominalEntry, 4> result;
  for (unsigned i : range(numInherited)) {
    getDirectlyInheritedNominalTypeDecls(decl, i, result, anyObject);
  }

  auto *protoDecl = dyn_cast_or_null<ProtocolDecl>(typeDecl);
  if (protoDecl == nullptr)
    return result;

  // FIXME: Refactor SelfBoundsFromWhereClauseRequest to dig out
  // the source location.
  SourceLoc loc = SourceLoc();

  // For a deserialized protocol, the where clause isn't going to tell us
  // anything. Ask the requirement signature instead.
  if (protoDecl->wasDeserialized()) {
    auto protoSelfTy = protoDecl->getSelfInterfaceType();
    for (auto &req : protoDecl->getRequirementSignature().getRequirements()) {
      // Dig out a conformance requirement...
      if (req.getKind() != RequirementKind::Conformance)
        continue;

      // constraining Self.
      if (!req.getFirstType()->isEqual(protoSelfTy))
        continue;

      result.emplace_back(req.getProtocolDecl(), loc, SourceLoc());
    }
    return result;
  }

  // Else we have access to this information on the where clause.
  auto selfBounds = getSelfBoundsFromWhereClause(decl);
  anyObject |= selfBounds.anyObject;

  for (auto inheritedNominal : selfBounds.decls)
    result.emplace_back(inheritedNominal, loc, SourceLoc());

  return result;
}

bool IsCallAsFunctionNominalRequest::evaluate(Evaluator &evaluator,
                                              NominalTypeDecl *decl,
                                              DeclContext *dc) const {
  auto &ctx = dc->getASTContext();

  // Do a qualified lookup for `callAsFunction`. We want to ignore access, as
  // that will be checked when we actually try to solve with a `callAsFunction`
  // member access.
  SmallVector<ValueDecl *, 4> results;
  auto opts = NL_QualifiedDefault | NL_ProtocolMembers | NL_IgnoreAccessControl;
  dc->lookupQualified(decl, DeclNameRef(ctx.Id_callAsFunction),
                      decl->getLoc(), opts, results);

  return llvm::any_of(results, [](ValueDecl *decl) -> bool {
    if (auto *fd = dyn_cast<FuncDecl>(decl))
      return fd->isCallAsFunctionMethod();
    return false;
  });
}

bool TypeBase::isCallAsFunctionType(DeclContext *dc) {
  // We can perform the lookup at module scope to allow us to better cache the
  // result across different contexts. Given we'll be doing a qualified lookup,
  // this shouldn't make a difference.
  dc = dc->getModuleScopeContext();

  // Note this excludes AnyObject.
  SmallVector<NominalTypeDecl *, 4> decls;
  tryExtractDirectlyReferencedNominalTypes(this, decls);

  auto &ctx = dc->getASTContext();
  return llvm::any_of(decls, [&](auto *decl) {
    IsCallAsFunctionNominalRequest req(decl, dc);
    return evaluateOrDefault(ctx.evaluator, req, false);
  });
}

template <class DynamicAttribute, class Req>
static bool checkForDynamicAttribute(Evaluator &eval, NominalTypeDecl *decl) {
  // If this type has the attribute on it, then yes!
  if (decl->getAttrs().hasAttribute<DynamicAttribute>())
    return true;

  auto hasAttribute = [&](NominalTypeDecl *decl) -> bool {
    return evaluateOrDefault(eval, Req{decl}, false);
  };

  if (auto *proto = dyn_cast<ProtocolDecl>(decl)) {
    // Check inherited protocols of a protocol.
    for (auto *otherProto : proto->getInheritedProtocols())
      if (hasAttribute(otherProto))
        return true;
  } else {
    // Check the protocols the type conforms to.
    for (auto *otherProto : decl->getAllProtocols()) {
      if (hasAttribute(otherProto))
        return true;
    }
  }

  // Check the superclass if present.
  if (auto *classDecl = dyn_cast<ClassDecl>(decl)) {
    if (auto *superclass = classDecl->getSuperclassDecl()) {
      if (hasAttribute(superclass))
        return true;
    }
  }
  return false;
}

bool HasDynamicMemberLookupAttributeRequest::evaluate(
    Evaluator &eval, NominalTypeDecl *decl) const {
  using Req = HasDynamicMemberLookupAttributeRequest;
  return checkForDynamicAttribute<DynamicMemberLookupAttr, Req>(eval, decl);
}

bool TypeBase::hasDynamicMemberLookupAttribute() {
  SmallVector<NominalTypeDecl *, 4> decls;
  tryExtractDirectlyReferencedNominalTypes(this, decls);

  auto &ctx = getASTContext();
  return llvm::any_of(decls, [&](auto *decl) {
    HasDynamicMemberLookupAttributeRequest req(decl);
    return evaluateOrDefault(ctx.evaluator, req, false);
  });
}

bool HasDynamicCallableAttributeRequest::evaluate(Evaluator &eval,
                                                  NominalTypeDecl *decl) const {
  using Req = HasDynamicCallableAttributeRequest;
  return checkForDynamicAttribute<DynamicCallableAttr, Req>(eval, decl);
}

bool TypeBase::hasDynamicCallableAttribute() {
  SmallVector<NominalTypeDecl *, 4> decls;
  tryExtractDirectlyReferencedNominalTypes(this, decls);

  auto &ctx = getASTContext();
  return llvm::any_of(decls, [&](auto *decl) {
    HasDynamicCallableAttributeRequest req(decl);
    return evaluateOrDefault(ctx.evaluator, req, false);
  });
}

ProtocolDecl *ImplementsAttrProtocolRequest::evaluate(
    Evaluator &evaluator, const ImplementsAttr *attr, DeclContext *dc) const {

  auto typeRepr = attr->getProtocolTypeRepr();

  ASTContext &ctx = dc->getASTContext();
  DirectlyReferencedTypeDecls referenced =
    directReferencesForTypeRepr(evaluator, ctx, typeRepr, dc);

  // Resolve those type declarations to nominal type declarations.
  SmallVector<ModuleDecl *, 2> modulesFound;
  bool anyObject = false;
  auto nominalTypes
    = resolveTypeDeclsToNominal(evaluator, ctx, referenced, modulesFound,
                                anyObject);

  if (nominalTypes.empty())
    return nullptr;

  return dyn_cast<ProtocolDecl>(nominalTypes.front());
}

void FindLocalVal::checkPattern(const Pattern *Pat, DeclVisibilityKind Reason) {
  Pat->forEachVariable([&](VarDecl *VD) { checkValueDecl(VD, Reason); });
}
void FindLocalVal::checkValueDecl(ValueDecl *D, DeclVisibilityKind Reason) {
  if (!D)
    return;
  if (auto var = dyn_cast<VarDecl>(D)) {
    auto dc = var->getDeclContext();
    if ((isa<AbstractFunctionDecl>(dc) || isa<ClosureExpr>(dc)) &&
        var->hasAttachedPropertyWrapper()) {
      // FIXME: This is currently required to set the interface type of the
      // auxiliary variables (unless 'var' is a closure param).
      (void)var->getPropertyWrapperBackingPropertyType();

      auto vars = var->getPropertyWrapperAuxiliaryVariables();
      if (vars.backingVar) {
        Consumer.foundDecl(vars.backingVar, Reason);
      }
      if (vars.projectionVar) {
        Consumer.foundDecl(vars.projectionVar, Reason);
      }
      if (vars.localWrappedValueVar) {
        Consumer.foundDecl(vars.localWrappedValueVar, Reason);

        // If 'localWrappedValueVar' exists, the original var is shadowed.
        return;
      }
    }
  }
  Consumer.foundDecl(D, Reason);
}

void FindLocalVal::checkParameterList(const ParameterList *params) {
  for (auto param : *params) {
    checkValueDecl(param, DeclVisibilityKind::FunctionParameter);
  }
}

void FindLocalVal::checkGenericParams(GenericParamList *Params) {
  if (!Params)
    return;

  for (auto P : *Params) {
    if (P->isOpaqueType()) {
      // Generic param for 'some' parameter type is not "visible".
      continue;
    }
    checkValueDecl(P, DeclVisibilityKind::GenericParameter);
  }
}

void FindLocalVal::checkSourceFile(const SourceFile &SF) {
  for (Decl *D : SF.getTopLevelDecls())
    if (auto *TLCD = dyn_cast<TopLevelCodeDecl>(D))
      visitBraceStmt(TLCD->getBody(), /*isTopLevel=*/true);
}

void FindLocalVal::checkStmtCondition(const StmtCondition &Cond) {
  SourceLoc start = SourceLoc();
  for (auto entry : Cond) {
    if (start.isInvalid())
      start = entry.getStartLoc();
    if (auto *P = entry.getPatternOrNull()) {
      SourceRange previousConditionsToHere = SourceRange(start, entry.getEndLoc());
      if (!isReferencePointInRange(previousConditionsToHere))
        checkPattern(P, DeclVisibilityKind::LocalVariable);
    }
  }
}

void FindLocalVal::visitIfStmt(IfStmt *S) {
  if (!isReferencePointInRange(S->getSourceRange()))
    return;

  if (!S->getElseStmt() ||
      !isReferencePointInRange(S->getElseStmt()->getSourceRange())) {
    checkStmtCondition(S->getCond());
  }

  visit(S->getThenStmt());
  if (S->getElseStmt())
    visit(S->getElseStmt());
}

void FindLocalVal::visitGuardStmt(GuardStmt *S) {
  if (SM.isBeforeInBuffer(Loc, S->getStartLoc()))
    return;

  // Names in the guard aren't visible until after the body.
  if (S->getBody()->isImplicit() ||
      !isReferencePointInRange(S->getBody()->getSourceRange()))
    checkStmtCondition(S->getCond());

  visit(S->getBody());
}

void FindLocalVal::visitWhileStmt(WhileStmt *S) {
  if (!isReferencePointInRange(S->getSourceRange()))
    return;

  checkStmtCondition(S->getCond());
  visit(S->getBody());
}
void FindLocalVal::visitRepeatWhileStmt(RepeatWhileStmt *S) {
  visit(S->getBody());
}
void FindLocalVal::visitDoStmt(DoStmt *S) {
  visit(S->getBody());
}

void FindLocalVal::visitForEachStmt(ForEachStmt *S) {
  if (!isReferencePointInRange(S->getSourceRange()))
    return;
  visit(S->getBody());
  if (!isReferencePointInRange(S->getParsedSequence()->getSourceRange()))
    checkPattern(S->getPattern(), DeclVisibilityKind::LocalVariable);
}

void FindLocalVal::visitBraceStmt(BraceStmt *S, bool isTopLevelCode) {
  if (isTopLevelCode) {
    if (SM.isBeforeInBuffer(Loc, S->getStartLoc()))
      return;
  } else {
    SourceRange CheckRange = S->getSourceRange();
    if (S->isImplicit()) {
      // If the brace statement is implicit, it doesn't have an explicit '}'
      // token. Thus, the last token in the brace stmt could be a string
      // literal token, which can *contain* its interpolation segments.
      // If one of these interpolation segments is the reference point, we'd
      // return false from `isReferencePointInRange` because the string
      // literal token's start location is before the interpolation token.
      // To fix this, adjust the range we are checking to range until the end of
      // the potential string interpolation token.
      CheckRange.End = Lexer::getLocForEndOfToken(SM, CheckRange.End);
    }
    if (!isReferencePointInRange(CheckRange))
      return;
  }

  // Visit inner statements first before reporting local decls in the current
  // scope.
  for (auto elem : S->getElements()) {
    // If we have a SingleValueStmtExpr, there may be local bindings in the
    // wrapped statement.
    if (auto *E = elem.dyn_cast<Expr *>()) {
      if (auto *SVE = dyn_cast<SingleValueStmtExpr>(E))
        visit(SVE->getStmt());
      continue;
    }

    if (auto *S = elem.dyn_cast<Stmt*>()) {
      visit(S);
      continue;
    }
  }

  auto visitDecl = [&](Decl *D) {
    if (auto *VD = dyn_cast<ValueDecl>(D))
      checkValueDecl(VD, DeclVisibilityKind::LocalVariable);
    D->visitAuxiliaryDecls([&](Decl *D) {
      if (auto *VD = dyn_cast<ValueDecl>(D))
        checkValueDecl(VD, DeclVisibilityKind::LocalVariable);
      // FIXME: Recursively call `visitDecl` to handle nested macros.
    });
  };

  for (auto elem : S->getElements()) {
    if (auto *E = elem.dyn_cast<Expr *>()) {
      // 'MacroExpansionExpr' at code-item position may introduce value decls.
      // NOTE: the expression must be type checked.
      // FIXME: In code-completion local expressions are _not_ type checked.
      if (auto *mee = dyn_cast<MacroExpansionExpr>(E)) {
        if (auto *med = mee->getSubstituteDecl()) {
          visitDecl(med);
        }
      }
      continue;
    }
    if (auto *D = elem.dyn_cast<Decl*>()) {
      visitDecl(D);
      continue;
    }
  }
}
  
void FindLocalVal::visitSwitchStmt(SwitchStmt *S) {
  if (!isReferencePointInRange(S->getSourceRange()))
    return;
  for (CaseStmt *C : S->getCases()) {
    visit(C);
  }
}

void FindLocalVal::visitCaseStmt(CaseStmt *S) {
  // The last token in a case stmt can be a string literal token, which can
  // *contain* its interpolation segments. If one of these interpolation
  // segments is the reference point, we'd return false from
  // `isReferencePointInRange` because the string literal token's start location
  // is before the interpolation token. To fix this, adjust the range we are
  // checking to range until the end of the potential string interpolation
  // token.
  SourceRange CheckRange = {S->getStartLoc(),
                            Lexer::getLocForEndOfToken(SM, S->getEndLoc())};
  if (!isReferencePointInRange(CheckRange))
    return;
  // Pattern names aren't visible in the patterns themselves,
  // just in the body or in where guards.
  bool inPatterns = isReferencePointInRange(S->getLabelItemsRange());
  auto items = S->getCaseLabelItems();
  if (inPatterns) {
    for (const auto &CLI : items) {
      auto guard = CLI.getGuardExpr();
      if (guard && isReferencePointInRange(guard->getSourceRange())) {
        checkPattern(CLI.getPattern(), DeclVisibilityKind::LocalVariable);
        break;
      }
    }
  }

  if (!inPatterns && !items.empty()) {
    for (auto *vd : S->getCaseBodyVariablesOrEmptyArray()) {
      checkValueDecl(vd, DeclVisibilityKind::LocalVariable);
    }
  }
  visit(S->getBody());
}

void FindLocalVal::visitDoCatchStmt(DoCatchStmt *S) {
  if (!isReferencePointInRange(S->getSourceRange()))
    return;
  visit(S->getBody());
  for (CaseStmt *C : S->getCatches()) {
    visit(C);
  }
}

void swift::simple_display(llvm::raw_ostream &out, NLKind kind) {
  switch (kind) {
  case NLKind::QualifiedLookup:
    out << "QualifiedLookup";
    return;
  case NLKind::UnqualifiedLookup:
    out << "UnqualifiedLookup";
    return;
  }
  llvm_unreachable("Unhandled case in switch");
}

void swift::simple_display(llvm::raw_ostream &out, NLOptions options) {
  using Flag = std::pair<NLOptions, StringRef>;
  Flag possibleFlags[] = {
#define FLAG(Name) {Name, #Name},
    FLAG(NL_ProtocolMembers)
    FLAG(NL_RemoveNonVisible)
    FLAG(NL_RemoveOverridden)
    FLAG(NL_IgnoreAccessControl)
    FLAG(NL_OnlyTypes)
    FLAG(NL_IncludeAttributeImplements)
#undef FLAG
  };

  auto flagsToPrint = llvm::make_filter_range(
      possibleFlags, [&](Flag flag) { return options & flag.first; });

  out << "{ ";
  interleave(
      flagsToPrint, [&](Flag flag) { out << flag.second; },
      [&] { out << ", "; });
  out << " }";
}
