//===--- ManglingUtils.cpp - Utilities for Swift name mangling ------------===//
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

#include "swift/Demangling/ManglingUtils.h"
#include "llvm/ADT/None.h"
#include "llvm/ADT/Optional.h"

using namespace swift;
using namespace Mangle;


bool Mangle::isNonAscii(StringRef str) {
  for (unsigned char c : str) {
    if (c >= 0x80)
      return true;
  }
  return false;
}

bool Mangle::needsPunycodeEncoding(StringRef str) {
  for (unsigned char c : str) {
    if (!isValidSymbolChar(c))
      return true;
  }
  return false;
}

/// Translate the given operator character into its mangled form.
///
/// Current operator characters:   @/=-+*%<>!&|^~ and the special operator '..'
char Mangle::translateOperatorChar(char op) {
  switch (op) {
    case '&': return 'a'; // 'and'
    case '@': return 'c'; // 'commercial at sign'
    case '/': return 'd'; // 'divide'
    case '=': return 'e'; // 'equal'
    case '>': return 'g'; // 'greater'
    case '<': return 'l'; // 'less'
    case '*': return 'm'; // 'multiply'
    case '!': return 'n'; // 'negate'
    case '|': return 'o'; // 'or'
    case '+': return 'p'; // 'plus'
    case '?': return 'q'; // 'question'
    case '%': return 'r'; // 'remainder'
    case '-': return 's'; // 'subtract'
    case '~': return 't'; // 'tilde'
    case '^': return 'x'; // 'xor'
    case '.': return 'z'; // 'zperiod' (the z is silent)
    default:
      return op;
  }
}

std::string Mangle::translateOperator(StringRef Op) {
  std::string Encoded;
  for (char ch : Op) {
    Encoded.push_back(translateOperatorChar(ch));
  }
  return Encoded;
}

llvm::Optional<StringRef> Mangle::getStandardTypeSubst(
    StringRef TypeName, bool allowConcurrencyManglings) {
#define STANDARD_TYPE(KIND, MANGLING, TYPENAME)      \
  if (TypeName == #TYPENAME) {                       \
    return StringRef(#MANGLING);                     \
  }

#define STANDARD_TYPE_CONCURRENCY(KIND, MANGLING, TYPENAME)    \
  if (allowConcurrencyManglings && TypeName == #TYPENAME) {    \
    return StringRef("c" #MANGLING);                           \
  }

#include "swift/Demangling/StandardTypesMangling.def"

  return llvm::None;
}

