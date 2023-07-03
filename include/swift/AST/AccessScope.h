//===--- AccessScope.h - Swift Access Scope ---------------------*- C++ -*-===//
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

#ifndef SWIFT_ACCESSSCOPE_H
#define SWIFT_ACCESSSCOPE_H

#include "swift/AST/AttrKind.h"
#include "swift/AST/DeclContext.h"
#include "swift/Basic/Debug.h"
#include "llvm/ADT/None.h"
#include "llvm/ADT/Optional.h"
#include "llvm/ADT/PointerIntPair.h"

namespace swift {

/// The wrapper around the outermost DeclContext from which
/// a particular declaration can be accessed.
class AccessScope {
  /// To validate access of a decl, access scope check for both decl site
  /// and use site needs to be done. The underlying mechanism uses a
  /// <DeclContext*, bool> pair to determine the scope, as shown
  /// in the table below.
  ///
  /// <DeclContext*, bool>          AccessScope        AccessLevel
  /// ----------------------------------------------------------------
  ///  <nullptr, false>              Public           public or open
  ///  <nullptr, true>               Public           public `@_spi`
  ///  <PackageUnit*, _>             Package            package
  ///  <ModuleDecl*, _>              Module             internal
  ///  <FileUnit*, false>            FileScope        fileprivate
  ///  <FileUnit*, true>             Private            private
  ///
  /// For example, if a decl with `public` access level is referenced outside of
  /// its defining module, it will be maped to the <nullptr, false> pair during
  /// the access scope check. This pair is determined based on the decl's access
  /// level in  \c getAccessScopeForFormalAccess and passed to
  /// \c checkAccessUsingAccessScope which compares access scope of the
  /// use site and decl site.
  ///
  /// \see AccessScope::getAccessScopeForFormalAccess
  /// \see AccessScope::checkAccessUsingAccessScope
  /// \see DeclContext::ASTHierarchy
  llvm::PointerIntPair<const DeclContext *, 1, bool> Value;

public:
  AccessScope(const DeclContext *DC, bool isPrivate = false);

  static AccessScope getPublic() { return AccessScope(nullptr, false); }

  /// Check if private access is allowed. This is a lexical scope check in Swift
  /// 3 mode. In Swift 4 mode, declarations and extensions of the same type will
  /// also allow access.
  static bool allowsPrivateAccess(const DeclContext *useDC, const DeclContext *sourceDC);

  /// Returns nullptr if access scope is public.
  const DeclContext *getDeclContext() const { return Value.getPointer(); }

  bool operator==(AccessScope RHS) const { return Value == RHS.Value; }
  bool operator!=(AccessScope RHS) const { return !(*this == RHS); }
  bool hasEqualDeclContextWith(AccessScope RHS) const {
    return getDeclContext() == RHS.getDeclContext();
  }

  bool isPublic() const { return !Value.getPointer(); }
  bool isPrivate() const { return Value.getPointer() && Value.getInt(); }
  bool isFileScope() const;
  bool isInternal() const;
  bool isPackage() const;

  /// Checks if the DeclContext of this (use site) access scope is more
  /// restrictive than that of the argument (decl site) based on the DeclContext
  /// hierarchy: (most to least restrictive)
  /// decl/expr (e.g. ClassDecl) -> FileUnit -> ModuleDecl -> PackageUnit -> nullptr
  ///
  /// A few things to note:
  /// 1. If both have the same DeclContext, returns false as one is _not_ a
  /// child of the other.
  /// 2. This function does _not_ check the restrictiveness of the _access
  /// level_ between two decls.
  /// 3. The DeclContext of this (use site) may not be null even if the use site
  /// has a `public` access level.
  ///
  /// Here's an example while typechecking a file with the following code.
  ///
  /// ```
  /// import OtherModule
  ///
  /// // `Foo` is a `public` struct defined in `OtherModule`
  /// public func myFunc(_ arg: OtherModule.Foo) {}
  /// ```
  ///
  /// The use site of `Foo`is a function `myFunc`, and its DeclContext is
  /// non-null (FileUnit) even though the function decl itself has a `public`
  /// access level. When `isChildOf` is called, the argument passed in is a pair
  /// <nullptr, false> created in \c getAccessScopeForFormalAccess based on the
  /// access level of the decl `Foo`. Since FileUnit is a child of nullptr in
  /// the DeclContext hierarchy (described above), it returns true.
  ///
  /// \see AccessScope::getAccessScopeForFormalAccess
  /// \see AccessScope::checkAccessUsingAccessScope
  /// \see DeclContext::ASTHierarchy
  bool isChildOf(AccessScope AS) const {
    if (isInContext()) {
      if (AS.isInContext())
        return allowsPrivateAccess(getDeclContext(), AS.getDeclContext());
      else
        return AS.isPublic();
    } else { // It's public, so can't be a child of public or less argument scope
      return false;
    }
  }

  bool isInContext() const { return getDeclContext() != nullptr; }

  /// Returns the associated access level for diagnostic purposes.
  AccessLevel accessLevelForDiagnostics() const;

  /// Returns the minimum access level required to access
  /// associated DeclContext for diagnostic purposes.
  AccessLevel requiredAccessForDiagnostics() const {
    return isFileScope()
      ? AccessLevel::FilePrivate
      : accessLevelForDiagnostics();
  }

  /// Returns the narrowest access scope if this and the specified access scope
  /// have common intersection, or None if scopes don't intersect.
  const llvm::Optional<AccessScope>
  intersectWith(AccessScope accessScope) const {
    if (hasEqualDeclContextWith(accessScope)) {
      if (isPrivate())
        return *this;
      return accessScope;
    }
    if (isChildOf(accessScope))
      return *this;
    if (accessScope.isChildOf(*this))
      return accessScope;

    return llvm::None;
  }

  SWIFT_DEBUG_DUMP;
};

} // end namespace swift

#endif
