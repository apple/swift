//===--- EditorPlaceholder.cpp - Handling for editor placeholders ---------===//
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

#include "swift/Basic/Assertions.h"
#include "swift/Basic/EditorPlaceholder.h"
#include <optional>

using namespace swift;

// Placeholder text must start with '<#' and end with
// '#>'.
//
// Placeholder kinds:
//
// Typed:
//   'T##' display-string '##' type-string ('##' type-for-expansion-string)?
//   'T##' display-and-type-string
//
// Basic:
//   display-string
//
// NOTE: It is required that '##' is not a valid substring of display-string
// or type-string. If this ends up not the case for some reason, we can consider
// adding escaping for '##'.

std::optional<EditorPlaceholderData>
swift::parseEditorPlaceholder(llvm::StringRef PlaceholderText) {
  if (!PlaceholderText.starts_with("<#") ||
      !PlaceholderText.ends_with("#>"))
    return std::nullopt;

  PlaceholderText = PlaceholderText.drop_front(2).drop_back(2);
  EditorPlaceholderData PHDataBasic;
  PHDataBasic.Kind = EditorPlaceholderKind::Basic;
  PHDataBasic.Display = PlaceholderText;

  if (!PlaceholderText.starts_with("T##")) {
    // Basic.
    return PHDataBasic;
  }

  // Typed.
  EditorPlaceholderData PHDataTyped;
  PHDataTyped.Kind = EditorPlaceholderKind::Typed;

  assert(PlaceholderText.starts_with("T##"));
  PlaceholderText = PlaceholderText.drop_front(3);
  size_t Pos = PlaceholderText.find("##");
  if (Pos == llvm::StringRef::npos) {
    PHDataTyped.Display = PHDataTyped.Type = PHDataTyped.TypeForExpansion =
      PlaceholderText;
    return PHDataTyped;
  }
  PHDataTyped.Display = PlaceholderText.substr(0, Pos);

  PlaceholderText = PlaceholderText.substr(Pos+2);
  Pos = PlaceholderText.find("##");
  if (Pos == llvm::StringRef::npos) {
    PHDataTyped.Type = PHDataTyped.TypeForExpansion = PlaceholderText;
  } else {
    PHDataTyped.Type = PlaceholderText.substr(0, Pos);
    PHDataTyped.TypeForExpansion = PlaceholderText.substr(Pos+2);
  }

  return PHDataTyped;
}

bool swift::isEditorPlaceholder(llvm::StringRef IdentifierText) {
  return IdentifierText.starts_with("<#");
}
