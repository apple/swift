//===--- ExperimentalDependencies.h - Keys for swiftdeps files --*- C++ -*-===//
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

#ifndef ExperimentalDependencies_h
#define ExperimentalDependencies_h

namespace swift {
/// Experimental dependencies evolve Swift towards finer-grained dependencies
/// and faster incremental rebuilds.

namespace ExperimentalDependencies {
// Use blank since it is illegal in Swift ids
const char separator = ' ';

struct Utils {
  static std::string combineNames(std::string a, std::string b) {
    assert(a.find(separator) == std::string::npos);
    return a + std::string(1, separator) + b;
  }
  static std::pair<StringRef, StringRef> separateNames(StringRef s) {
    const size_t sepIndex = s.find(separator);
    assert(sepIndex != StringRef::npos);
    return std::make_pair(s.take_front(sepIndex), s.drop_front(sepIndex + 1));
  }
};

struct InterfaceHashes {
  const std::string normal;
  const std::string experimental;

  InterfaceHashes(StringRef normal, StringRef experimental)
      : normal(normal.str()), experimental(experimental.str()) {}
  InterfaceHashes(StringRef combined)
      : InterfaceHashes(Utils::separateNames(combined)) {}
  InterfaceHashes(std::pair<StringRef, StringRef> ne)
      : normal(ne.first.str()), experimental(ne.second.str()) {}

  std::string combined() { return Utils::combineNames(normal, experimental); }
};

} // namespace ExperimentalDependencies

} // end namespace swift

#endif /* ExperimentalDependencies_h */
