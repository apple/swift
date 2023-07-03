//===--- AccessNotes.h - Access Notes ---------------------------*- C++ -*-===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2021 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//
//
//  Implements access notes, which allow certain modifiers or attributes to be
//  added to the declarations in a module.
//
//===----------------------------------------------------------------------===//

#ifndef ACCESSNOTES_H
#define ACCESSNOTES_H

#include "swift/AST/Identifier.h"
#include "swift/AST/StorageImpl.h"
#include "swift/Basic/NullablePtr.h"
#include "llvm/ADT/Optional.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/raw_ostream.h"
#include <set>
#include <string>
#include <vector>

namespace swift {
class ASTContext;
class ValueDecl;

/// The name of the declaration an access note should be applied to.
///
/// Grammatically, this is equivalent to a \c ParsedDeclName, but supports a
/// subset of that type's features.
class AccessNoteDeclName {
public:
  /// The names of the parent/contextual declarations containing the declaration
  /// the access note should apply to.
  std::vector<Identifier> parentNames;

  /// The name of the declaration the access note should be applied to. (For
  /// accessors, this is actually the name of the storage it's attached to.)
  DeclName name;

  /// For accessors, the kind of accessor; for non-accessors, \c None.
  llvm::Optional<AccessorKind> accessorKind;

  AccessNoteDeclName(ASTContext &ctx, StringRef str);
  AccessNoteDeclName();

  /// If true, an access note with this name should apply to \p VD.
  bool matches(ValueDecl *VD) const;

  /// If true, the name is empty and invalid.
  bool empty() const;

  void print(llvm::raw_ostream &os) const;
  SWIFT_DEBUG_DUMP;
};

/// An individual access note specifying modifications to a declaration.
class AccessNote {
public:
  /// The name of the declaration to modify.
  AccessNoteDeclName Name;

  /// If \c true, add an @objc attribute; if \c false, delete an @objc
  /// attribute; if \c None, do nothing.
  llvm::Optional<bool> ObjC;

  /// If \c true, add a dynamic modifier; if \c false, delete a dynamic
  /// modifier; if \c None, do nothing.
  llvm::Optional<bool> Dynamic;

  /// If set, modify an @objc attribute to give it the specified \c ObjCName.
  /// If \c ObjC would otherwise be \c None, it will be set to \c true.
  llvm::Optional<ObjCSelector> ObjCName;

  void dump(llvm::raw_ostream &os, int indent = 0) const;
  SWIFT_DEBUG_DUMP;
};

/// A set of access notes with module-wide metadata about them.
class AccessNotesFile {
public:
  /// A human-readable string describing why the access notes are being applied.
  /// Inserted into diagnostics about access notes in the file.
  std::string Reason;

  /// Access notes to apply to the module.
  std::vector<AccessNote> Notes;

  /// Load the access notes from \p buffer, or \c None if they cannot be loaded.
  /// Diagnoses any parsing issues with the access notes file.
  static llvm::Optional<AccessNotesFile>
      load(ASTContext &ctx, const llvm::MemoryBuffer *buffer);

  /// Look up the access note in this file, if any, which applies to \p VD.
  NullablePtr<const AccessNote> lookup(ValueDecl *VD) const;

  void dump(llvm::raw_ostream &os) const;
  SWIFT_DEBUG_DUMP;
};

}

#endif
