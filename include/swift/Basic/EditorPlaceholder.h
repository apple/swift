//===--- EditorPlaceholder.h - Handling for editor placeholders -*- C++ -*-===//
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
///
/// \file
/// Provides info about editor placeholders, <#such as this#>.
///
//===----------------------------------------------------------------------===//

#ifndef SWIFT_BASIC_EDITORPLACEHOLDER_H
#define SWIFT_BASIC_EDITORPLACEHOLDER_H

#include "llvm/ADT/Optional.h"
#include "llvm/ADT/StringRef.h"

namespace swift {

enum class EditorPlaceholderKind {
  Basic,
  Typed,
};

struct EditorPlaceholderData {
  /// Placeholder kind.
  EditorPlaceholderKind Kind;
  /// The part that is displayed in the editor.
  llvm::StringRef Display;
  /// If kind is \c Typed, this is the type string for the placeholder.
  llvm::StringRef Type;
  /// If kind is \c Typed, this is the type string to be considered for
  /// placeholder expansion.
  /// It can be same as \c Type or different if \c Type is a typealias.
  llvm::StringRef TypeForExpansion;
};

/// Deconstructs a placeholder string and returns info about it.
/// \returns None if the \c PlaceholderText is not a valid placeholder string.
llvm::Optional<EditorPlaceholderData>
parseEditorPlaceholder(llvm::StringRef PlaceholderText);

/// Checks if an identifier with the given text is an editor placeholder
bool isEditorPlaceholder(llvm::StringRef IdentifierText);
} // end namespace swift

#endif // SWIFT_BASIC_EDITORPLACEHOLDER_H
