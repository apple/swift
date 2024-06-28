//===--- Fingerprint.cpp - A stable identity for compiler data --*- C++ -*-===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2020 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

#include "swift/Basic/Assertions.h"
#include "swift/Basic/Fingerprint.h"
#include "swift/Basic/STLExtras.h"
#include "llvm/Support/Format.h"
#include "llvm/Support/raw_ostream.h"

#include <inttypes.h>
#include <sstream>

using namespace swift;

llvm::raw_ostream &llvm::operator<<(llvm::raw_ostream &OS,
                                    const Fingerprint &FP) {
  return OS << FP.getRawValue();
}

void swift::simple_display(llvm::raw_ostream &out, const Fingerprint &fp) {
  out << fp.getRawValue();
}

std::optional<Fingerprint> Fingerprint::fromString(llvm::StringRef value) {
  assert(value.size() == Fingerprint::DIGEST_LENGTH &&
         "Only supports 32-byte hash values!");
  auto fp = Fingerprint::ZERO();
  {
    std::istringstream s(value.drop_back(Fingerprint::DIGEST_LENGTH/2).str());
    s >> std::hex >> fp.core.first;
  }
  {
    std::istringstream s(value.drop_front(Fingerprint::DIGEST_LENGTH/2).str());
    s >> std::hex >> fp.core.second;
  }
  // If the input string is not valid hex, the conversion above can fail.
  if (value != fp.getRawValue())
    return std::nullopt;

  return fp;
}

llvm::SmallString<Fingerprint::DIGEST_LENGTH> Fingerprint::getRawValue() const {
  llvm::SmallString<Fingerprint::DIGEST_LENGTH> Str;
  llvm::raw_svector_ostream Res(Str);
  Res << llvm::format_hex_no_prefix(core.first, 16);
  Res << llvm::format_hex_no_prefix(core.second, 16);
  return Str;
}
