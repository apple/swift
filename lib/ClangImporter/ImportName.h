//===--- ImportName.h - Imported Swift names for Clang decls ----*- C++ -*-===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2016 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//
//
// This file provides class definitions for naming-related concerns in the
// ClangImporter.
//
//===----------------------------------------------------------------------===//
#ifndef SWIFT_IMPORT_NAME_H
#define SWIFT_IMPORT_NAME_H

#include "ImportEnumInfo.h"
#include "SwiftLookupTable.h"
#include "swift/Basic/StringExtras.h"
#include "swift/AST/ASTContext.h"
#include "swift/AST/Decl.h"
#include "swift/AST/ForeignErrorConvention.h"
#include "clang/Sema/Sema.h"

// TODO: remove when we drop import name options
#include "clang/AST/Decl.h"

namespace swift {
namespace importer {
struct PlatformAvailability;

/// Information about imported error parameters.
struct ImportedErrorInfo {
  ForeignErrorConvention::Kind Kind;
  ForeignErrorConvention::IsOwned_t IsOwned;

  /// The index of the error parameter.
  unsigned ParamIndex;

  /// Whether the parameter is being replaced with "void"
  /// (vs. removed).
  bool ReplaceParamWithVoid;
};

/// The kind of accessor that an entity will be imported as.
enum class ImportedAccessorKind {
  None = 0,
  PropertyGetter,
  PropertySetter,
  SubscriptGetter,
  SubscriptSetter,
};

/// The name version
enum class ImportNameVersion : unsigned {
  /// Names as they appear in C/ObjC
  Raw = 0,

  /// Names as they appeared in Swift 2 family
  Swift2,

  /// Names as they appeared in Swift 3 family
  Swift3,

  /// Names as they appeared in Swift 4 family
  Swift4,
};
enum { NumImportNameVersions = 4 };

/// Describes a name that was imported from Clang.
class ImportedName {
  friend class NameImporter;

  /// The imported name.
  DeclName declName;

  /// The context into which this declaration will be imported.
  ///
  /// When the context into which the declaration will be imported
  /// matches a Clang declaration context (the common case), the
  /// result will be expressed as a declaration context. Otherwise,
  /// if the Clang type is not itself a declaration context (for
  /// example, a typedef that comes into Swift as a strong type),
  /// the type declaration will be provided.
  EffectiveClangContext effectiveContext;

  struct Info {
    /// The version of Swift this name corresponds to
    ImportNameVersion version;

    /// Whether this name was explicitly specified via a Clang
    /// swift_name attribute.
    bool hasCustomName = false;

    /// Whether this was one of a special class of Objective-C
    /// initializers for which we drop the variadic argument rather
    /// than refuse to import the initializer.
    bool droppedVariadic = false;

    /// Whether this is a global being imported as a member
    bool importAsMember = false;

    /// What kind of accessor this name refers to, if any.
    ImportedAccessorKind accessorKind = ImportedAccessorKind::None;

    /// For an initializer, the kind of initializer to import.
    CtorInitializerKind initKind = CtorInitializerKind::Designated;

    /// For names that map Objective-C error handling conventions into
    /// throwing Swift methods, describes how the mapping is performed.
    Optional<ImportedErrorInfo> errorInfo = None;

    /// For a declaration name that makes the declaration into an
    /// instance member, the index of the "Self" parameter.
    Optional<unsigned> selfIndex = None;
  } info;

public:
  ImportedName() = default;

  /// Produce just the imported name, for clients that don't care
  /// about the details.
  DeclName getDeclName() const { return declName; }
  operator DeclName() const { return getDeclName(); }
  void setDeclName(DeclName name) { declName = name; }

  /// The context into which this declaration will be imported.
  EffectiveClangContext getEffectiveContext() const {
    return effectiveContext;
  }
  void setEffectiveContext(EffectiveClangContext ctx) {
    effectiveContext = ctx;
  }

  /// The highest version of Swift that this name comes from
  ImportNameVersion getVersion() const { return info.version; }

  /// For an initializer, the kind of initializer to import.
  CtorInitializerKind getInitKind() const { return info.initKind; }

  /// What kind of accessor this name refers to, if any.
  ImportedAccessorKind getAccessorKind() const { return info.accessorKind; }

  /// For names that map Objective-C error handling conventions into
  /// throwing Swift methods, describes how the mapping is performed.
  Optional<ImportedErrorInfo> getErrorInfo() const {
    return info.errorInfo;
  }

  /// For a declaration name that makes the declaration into an
  /// instance member, the index of the "Self" parameter.
  Optional<unsigned> getSelfIndex() const { return info.selfIndex; }

  /// Whether this name was explicitly specified via a Clang
  /// swift_name attribute.
  bool hasCustomName() const { return info.hasCustomName; }
  void setHasCustomName() { info.hasCustomName = true; }

  /// Whether this was one of a special class of Objective-C
  /// initializers for which we drop the variadic argument rather
  /// than refuse to import the initializer.
  bool droppedVariadic() const { return info.droppedVariadic; }

  /// Whether this is a global being imported as a member
  bool importAsMember() const { return info.importAsMember; }

  /// Whether any name was imported.
  explicit operator bool() const { return static_cast<bool>(declName); }

  /// Whether this declaration is a property accessor (getter or setter).
  bool isPropertyAccessor() const {
    switch (getAccessorKind()) {
    case ImportedAccessorKind::None:
    case ImportedAccessorKind::SubscriptGetter:
    case ImportedAccessorKind::SubscriptSetter:
      return false;

    case ImportedAccessorKind::PropertyGetter:
    case ImportedAccessorKind::PropertySetter:
      return true;
    }
  }

  /// Whether this declaration is a subscript accessor (getter or setter).
  bool isSubscriptAccessor() const {
    switch (getAccessorKind()) {
    case ImportedAccessorKind::None:
    case ImportedAccessorKind::PropertyGetter:
    case ImportedAccessorKind::PropertySetter:
      return false;

    case ImportedAccessorKind::SubscriptGetter:
    case ImportedAccessorKind::SubscriptSetter:
      return true;
    }
    
    llvm_unreachable("Invalid ImportedAccessorKind.");
  }
};

/// Strips a trailing "Notification", if present. Returns {} if name doesn't end
/// in "Notification", or it there would be nothing left.
StringRef stripNotification(StringRef name);

// TODO: I'd like to remove the following
/// Flags that control the import of names in importFullName.
enum class ImportNameFlags {
  /// Suppress the factory-method-as-initializer transformation.
  SuppressFactoryMethodAsInit = 0x01,

  /// Produce the Swift 2 name of the given entity.
  Swift2Name = 0x02,
};
enum { NumImportNameFlags = 2 };

/// Options that control the import of names in importFullName.
typedef OptionSet<ImportNameFlags> ImportNameOptions;

/// Class to determine the Swift name of foreign entities. Currently fairly
/// stateless and borrows from the ClangImporter::Implementation, but in the
/// future will be more self-contained and encapsulated.
class NameImporter {
  ASTContext &swiftCtx;
  const PlatformAvailability &availability;

  clang::Sema &clangSema;
  EnumInfoCache enumInfos;
  StringScratchSpace scratch;

  const bool inferImportAsMember;

  // TODO: remove when we drop the options (i.e. import all names)
  using CacheKeyType =
      std::pair<const clang::NamedDecl *, unsigned>;

  /// Cache for repeated calls
  llvm::DenseMap<CacheKeyType, ImportedName> importNameCache;

public:
  NameImporter(ASTContext &ctx, const PlatformAvailability &avail,
               clang::Sema &cSema, bool inferIAM)
      : swiftCtx(ctx), availability(avail), clangSema(cSema),
        enumInfos(swiftCtx, clangSema.getPreprocessor()),
        inferImportAsMember(inferIAM) {}

  /// Determine the Swift name for a Clang decl
  ImportedName importName(const clang::NamedDecl *decl,
                          ImportNameVersion version);

  /// Imports the name of the given Clang macro into Swift.
  Identifier importMacroName(const clang::IdentifierInfo *clangIdentifier,
                             const clang::MacroInfo *macro);

  ASTContext &getContext() { return swiftCtx; }
  const LangOptions &getLangOpts() const { return swiftCtx.LangOpts; }

  Identifier getIdentifier(StringRef name) {
    return swiftCtx.getIdentifier(name);
  }

  StringScratchSpace &getScratch() { return scratch; }

  bool isInferImportAsMember() const { return inferImportAsMember; }

  EnumInfo getEnumInfo(const clang::EnumDecl *decl) {
    return enumInfos.getEnumInfo(decl);
  }
  EnumKind getEnumKind(const clang::EnumDecl *decl) {
    return enumInfos.getEnumKind(decl);
  }

  clang::Sema &getClangSema() { return clangSema; }
  clang::ASTContext &getClangContext() {
    return getClangSema().getASTContext();
  }
  clang::Preprocessor &getClangPreprocessor() {
    return getClangSema().getPreprocessor();
  }

private:
  bool enableObjCInterop() const { return swiftCtx.LangOpts.EnableObjCInterop; }

  /// Look for a method that will import to have the same name as the
  /// given method after importing the Nth parameter as an elided error
  /// parameter.
  bool hasErrorMethodNameCollision(const clang::ObjCMethodDecl *method,
                                   unsigned paramIndex,
                                   StringRef suffixToStrip);

  /// Test to see if there is a value with the same name as 'proposedName' in
  /// the same module as the decl
  bool hasNamingConflict(const clang::NamedDecl *decl,
                         const clang::IdentifierInfo *proposedName,
                         const clang::TypedefNameDecl *cfTypedef);

  Optional<ImportedErrorInfo>
  considerErrorImport(const clang::ObjCMethodDecl *clangDecl,
                      StringRef &baseName,
                      SmallVectorImpl<StringRef> &paramNames,
                      ArrayRef<const clang::ParmVarDecl *> params,
                      bool isInitializer, bool hasCustomName);

  /// Whether we should import this as Swift Private
  bool shouldBeSwiftPrivate(const clang::NamedDecl *, clang::Sema &clangSema);

  EffectiveClangContext determineEffectiveContext(const clang::NamedDecl *,
                                                  const clang::DeclContext *,
                                                  ImportNameOptions options);

  ImportedName importNameImpl(const clang::NamedDecl *,
                              ImportNameOptions options);
};

}
}

#endif
