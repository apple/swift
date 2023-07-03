//===--- TypeCheckRegex.cpp - Regex type checking utilities ---------------===//
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

#include "TypeCheckRegex.h"

#include "swift/AST/ASTContext.h"
#include "swift/AST/Decl.h"
#include "swift/AST/Expr.h"
#include "swift/AST/Type.h"
#include "swift/AST/Types.h"

using namespace swift;

// Encoding rules:
// encode(〚`T`〛) ==> <version>, 〚`T`〛, .end
// 〚`T` (atom)〛 ==> .atom
// 〚`name: T` (atom)〛 ==> .atom, `name`, '\0'
// 〚`[T]`〛 ==> 〚`T`〛, .formArray
// 〚`T?`〛 ==> 〚`T`〛, .formOptional
// 〚`(T0, T1, ...)` (top level)〛 ==> 〚`T0`〛, 〚`T1`〛, ...
// 〚`(T0, T1, ...)`〛 ==> .beginTuple, 〚`T0`〛, 〚`T1`〛, ..., .endTuple
//
// For details, see apple/swift-experimental-string-processing.
bool swift::decodeRegexCaptureTypes(ASTContext &ctx,
                                    ArrayRef<uint8_t> serialization,
                                    Type atomType,
                                    SmallVectorImpl<TupleTypeElt> &result) {
  using Version = RegexLiteralExpr::CaptureStructureSerializationVersion;
  static const Version implVersion = 1;
  unsigned size = serialization.size();
  // A serialization should store a version and `.end` at the very least.
  unsigned minSize = sizeof(Version) + sizeof(RegexCaptureStructureCode);
  if (size < minSize)
    return false;
  // Read version.
  Version version = *reinterpret_cast<const Version *>(serialization.data());
  if (version != implVersion)
    return true;
  // Read contents.
  SmallVector<SmallVector<TupleTypeElt, 4>, 4> scopes(1);
  unsigned offset = sizeof(Version);
  auto consumeCode = [&]() -> llvm::Optional<RegexCaptureStructureCode> {
    auto rawValue = serialization[offset];
    if (rawValue >= (uint8_t)RegexCaptureStructureCode::CaseCount)
      return llvm::None;
    offset += sizeof(RegexCaptureStructureCode);
    return (RegexCaptureStructureCode)rawValue;
  };
  do {
    auto code = consumeCode();
    if (!code)
      return false;
    switch (*code) {
    case RegexCaptureStructureCode::End:
      offset = size;
      break;
    case RegexCaptureStructureCode::Atom:
      scopes.back().push_back(atomType);
      break;
    case RegexCaptureStructureCode::NamedAtom: {
      auto *namePtr = reinterpret_cast<const char *>(
          serialization.slice(offset).data());
      auto length = strnlen(namePtr, size - offset);
      if (length >= size - offset)
        return true; // Unterminated string.
      StringRef name(namePtr, length);
      scopes.back().push_back(
          TupleTypeElt(atomType, ctx.getIdentifier(name)));
      offset += length + /*NUL*/ 1;
      break;
    }
    case RegexCaptureStructureCode::FormArray: {
      auto &element = scopes.back().back();
      element = TupleTypeElt(ArraySliceType::get(element.getType()),
                             element.getName());
      break;
    }
    case RegexCaptureStructureCode::FormOptional: {
      auto &element = scopes.back().back();
      element = TupleTypeElt(OptionalType::get(element.getType()),
                             element.getName());
      break;
    }
    case RegexCaptureStructureCode::BeginTuple:
      scopes.push_back({});
      break;
    case RegexCaptureStructureCode::EndTuple: {
      auto children = scopes.pop_back_val();
      assert(children.size() > 1);
      auto type = TupleType::get(children, ctx);
      scopes.back().push_back(Type(type));
      break;
    }
    case RegexCaptureStructureCode::CaseCount:
      llvm_unreachable("Handled earlier");
    }
  } while (offset < size);
  if (scopes.size() != 1)
    return true; // Unterminated tuple.
  auto &elements = scopes.back();
  result.append(elements.begin(), elements.end());
  return false;
}
