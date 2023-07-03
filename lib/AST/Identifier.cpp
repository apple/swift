//===--- Identifier.cpp - Uniqued Identifier ------------------------------===//
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
// This file implements the Identifier interface.
//
//===----------------------------------------------------------------------===//

#include "swift/AST/ASTContext.h"
#include "swift/AST/Identifier.h"
#include "swift/Parse/Lexer.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Support/ConvertUTF.h"
#include "clang/Basic/CharInfo.h"
using namespace swift;

constexpr const Identifier::Aligner DeclBaseName::SubscriptIdentifierData{};
constexpr const Identifier::Aligner DeclBaseName::ConstructorIdentifierData{};
constexpr const Identifier::Aligner DeclBaseName::DestructorIdentifierData{};

raw_ostream &llvm::operator<<(raw_ostream &OS, Identifier I) {
  if (I.get() == nullptr)
    return OS << "_";
  return OS << I.get();
}

raw_ostream &llvm::operator<<(raw_ostream &OS, DeclBaseName D) {
  return OS << D.userFacingName();
}

raw_ostream &llvm::operator<<(raw_ostream &OS, DeclName I) {
  if (I.isSimpleName())
    return OS << I.getBaseName();

  OS << I.getBaseName() << "(";
  for (auto c : I.getArgumentNames()) {
    OS << c << ':';
  }
  OS << ")";
  return OS;
}

void swift::simple_display(llvm::raw_ostream &out, DeclName name) {
  out << "'" << name << "'";
}

raw_ostream &llvm::operator<<(raw_ostream &OS, DeclNameRef I) {
  OS << I.getFullName();
  return OS;
}

void swift::simple_display(llvm::raw_ostream &out, DeclNameRef name) {
  out << "'" << name << "'";
}

raw_ostream &llvm::operator<<(raw_ostream &OS, swift::ObjCSelector S) {
  unsigned n = S.getNumArgs();
  if (n == 0) {
    OS << S.getSelectorPieces()[0];
    return OS;
  }

  for (auto piece : S.getSelectorPieces()) {
    if (!piece.empty())
      OS << piece;
    OS << ":";
  }
  return OS;
}

bool Identifier::isOperatorSlow() const {
  StringRef data = str();
  auto *s = reinterpret_cast<llvm::UTF8 const *>(data.begin()),
  *end = reinterpret_cast<llvm::UTF8 const *>(data.end());
  llvm::UTF32 codePoint;
  llvm::ConversionResult res =
    llvm::convertUTF8Sequence(&s, end, &codePoint, llvm::strictConversion);
  assert(res == llvm::conversionOK && "invalid UTF-8 in identifier?!");
  (void)res;
  return !empty() && isOperatorStartCodePoint(codePoint);
}

int Identifier::compare(Identifier other) const {
  // Handle empty identifiers.
  if (empty() || other.empty()) {
    if (empty() != other.empty()) {
      return other.empty() ? -1 : 1;
    }

    return 0;
  }

  return str().compare(other.str());
}

int DeclName::compare(DeclName other) const {
  // Compare base names.
  if (int result = getBaseName().compare(other.getBaseName()))
    return result;

  // Compare argument names.
  auto argNames = getArgumentNames();
  auto otherArgNames = other.getArgumentNames();
  for (unsigned i = 0, n = std::min(argNames.size(), otherArgNames.size());
       i != n; ++i) {
    if (int result = argNames[i].compare(otherArgNames[i]))
      return result;
  }

  if (argNames.size() == otherArgNames.size())
    return 0;

  return argNames.size() < otherArgNames.size() ? -1 : 1;
}

static bool equals(ArrayRef<Identifier> idents, ArrayRef<StringRef> strings) {
  if (idents.size() != strings.size())
    return false;
  for (size_t i = 0, e = idents.size(); i != e; ++i) {
    if (!idents[i].is(strings[i]))
      return false;
  }
  return true;
}

bool DeclName::isCompoundName(DeclBaseName baseName,
                              ArrayRef<StringRef> argNames) const {
  return (isCompoundName() &&
          getBaseName() == baseName &&
          equals(getArgumentNames(), argNames));
}

bool DeclName::isCompoundName(StringRef baseName,
                              ArrayRef<StringRef> argNames) const {
  return (isCompoundName() &&
          getBaseName() == baseName &&
          equals(getArgumentNames(), argNames));
}

void DeclName::dump() const {
  llvm::errs() << *this << "\n";
}

StringRef DeclName::getString(llvm::SmallVectorImpl<char> &scratch,
                              bool skipEmptyArgumentNames) const {
  {
    llvm::raw_svector_ostream out(scratch);
    print(out, skipEmptyArgumentNames);
  }

  return StringRef(scratch.data(), scratch.size());
}

llvm::raw_ostream &DeclName::print(llvm::raw_ostream &os,
                                   bool skipEmptyArgumentNames) const {
  // Print the base name.
  os << getBaseName();

  // If this is a simple name, we're done.
  if (isSimpleName())
    return os;

  if (skipEmptyArgumentNames) {
    // If there is more than one argument yet none of them have names,
    // we're done.
    if (!getArgumentNames().empty()) {
      bool anyNonEmptyNames = false;
      for (auto c : getArgumentNames()) {
        if (!c.empty()) {
          anyNonEmptyNames = true;
          break;
        }
      }

      if (!anyNonEmptyNames)
        return os;
    }
  }

  // Print the argument names.
  os << "(";
  for (auto c : getArgumentNames()) {
    os << c << ':';
  }
  os << ")";
  return os;

}

llvm::raw_ostream &DeclName::printPretty(llvm::raw_ostream &os) const {
  return print(os, /*skipEmptyArgumentNames=*/!isSpecial());
}

void DeclNameRef::dump() const {
  llvm::errs() << *this << "\n";
}

StringRef DeclNameRef::getString(llvm::SmallVectorImpl<char> &scratch,
                             bool skipEmptyArgumentNames) const {
  return FullName.getString(scratch, skipEmptyArgumentNames);
}

llvm::raw_ostream &DeclNameRef::print(llvm::raw_ostream &os,
                                  bool skipEmptyArgumentNames) const {
  return FullName.print(os, skipEmptyArgumentNames);
}

llvm::raw_ostream &DeclNameRef::printPretty(llvm::raw_ostream &os) const {
  return FullName.printPretty(os);
}

ObjCSelector::ObjCSelector(ASTContext &ctx, unsigned numArgs,
                           ArrayRef<Identifier> pieces) {
  if (numArgs == 0) {
    assert(pieces.size() == 1 && "No-argument selector requires one piece");
    Storage = DeclName(pieces[0]);
    return;
  }

  assert(numArgs == pieces.size() && "Wrong number of selector pieces");
  Storage = DeclName(ctx, Identifier(), pieces);
}

llvm::Optional<ObjCSelector>
ObjCSelector::parse(ASTContext &ctx, StringRef string) {
  // Find the first colon.
  auto colonPos = string.find(':');

  // If there is no colon, we have a nullary selector.
  if (colonPos == StringRef::npos) {
    if (string.empty() || !Lexer::isIdentifier(string))
      return llvm::None;
    return ObjCSelector(ctx, 0, { ctx.getIdentifier(string) });
  }

  SmallVector<Identifier, 2> pieces;
  do {
    // Check whether we have a valid selector piece.
    auto piece = string.substr(0, colonPos);
    if (piece.empty()) {
      pieces.push_back(Identifier());
    } else {
      if (!Lexer::isIdentifier(piece))
        return llvm::None;
      pieces.push_back(ctx.getIdentifier(piece));
    }

    // Move to the next piece.
    string = string.substr(colonPos+1);
    colonPos = string.find(':');
  } while (colonPos != StringRef::npos);

  // If anything remains of the string, it's not a selector.
  if (!string.empty())
    return llvm::None;

  return ObjCSelector(ctx, pieces.size(), pieces);
}

ObjCSelectorFamily ObjCSelector::getSelectorFamily() const {
  StringRef text = getSelectorPieces().front().get();
  while (!text.empty() && text[0] == '_') text = text.substr(1);

  // Does the given selector start with the given string as a prefix, in the
  // sense of the selector naming conventions?
  // This implementation matches the one used by
  // clang::Selector::getMethodFamily, to make sure we behave the same as
  // Clang ARC. We're not just calling that method here because it means
  // allocating a clang::IdentifierInfo, which requires a Clang ASTContext.
  auto hasPrefix = [](StringRef text, StringRef prefix) {
    if (!text.startswith(prefix)) return false;
    if (text.size() == prefix.size()) return true;
    assert(text.size() > prefix.size());
    return !clang::isLowercase(text[prefix.size()]);
  };

  if (false) /*for #define purposes*/;
#define OBJC_SELECTOR_FAMILY(LABEL, PREFIX) \
  else if (hasPrefix(text, PREFIX)) return ObjCSelectorFamily::LABEL;
#include "swift/AST/ObjCSelectorFamily.def"
  else return ObjCSelectorFamily::None;
}

StringRef ObjCSelector::getString(llvm::SmallVectorImpl<char> &scratch) const {
  // Fast path for zero-argument selectors.
  if (getNumArgs() == 0) {
    auto name = getSelectorPieces()[0];
    if (name.empty())
      return "";
    return name.str();
  }

  scratch.clear();
  llvm::raw_svector_ostream os(scratch);
  os << *this;
  return os.str();
}

void ObjCSelector::dump() const {
  llvm::errs() << *this << "\n";
}


