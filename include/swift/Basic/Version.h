//===--- Version.h - Swift Version Number -----------------------*- C++ -*-===//
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
/// Defines version macros and version-related utility functions
/// for Swift.
///
//===----------------------------------------------------------------------===//

#ifndef SWIFT_BASIC_VERSION_H
#define SWIFT_BASIC_VERSION_H


#include "swift/Basic/LLVM.h"
#include "llvm/ADT/Optional.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/VersionTuple.h"
#include <array>
#include <string>

namespace swift {

class VersionParser;
class SourceLoc;

namespace version {

/// Represents an internal compiler version, represented as a tuple of
/// integers, or "version components".
///
/// For comparison, if a `CompilerVersion` contains more than one
/// version component, the second one is ignored for comparison,
/// as it represents a compiler variant with no defined ordering.
///
/// A CompilerVersion must have no more than five components and must fit in a
/// 64-bit unsigned integer representation.
///
/// Assuming a maximal version component layout of X.Y.Z.a.b,
/// X, Y, Z, a, b are integers with the following (inclusive) ranges:
/// X: [0 - 9223371]
/// Y: [0 - 999]
/// Z: [0 - 999]
/// a: [0 - 999]
/// b: [0 - 999]
class Version {
  friend class swift::VersionParser;
  SmallVector<unsigned, 5> Components;
public:
  /// Create the empty compiler version - this always compares greater
  /// or equal to any other CompilerVersion, as in the case of building Swift
  /// from latest sources outside of a build/integration/release context.
  Version() = default;

  /// Create a literal version from a list of components.
  Version(std::initializer_list<unsigned> Values) : Components(Values) {}

  /// Return a string to be used as an internal preprocessor define.
  ///
  /// The components of the version are multiplied element-wise by
  /// \p componentWeights, then added together (a dot product operation).
  /// If either array is longer than the other, the missing elements are
  /// treated as zero.
  ///
  /// The resulting string will have the form "-DMACRO_NAME=XYYZZ".
  /// The combined value must fit in a uint64_t.
  std::string preprocessorDefinition(StringRef macroName,
                                     ArrayRef<uint64_t> componentWeights) const;

  /// Return the ith version component.
  unsigned operator[](size_t i) const {
    return Components[i];
  }

  /// Return the number of version components.
  size_t size() const {
    return Components.size();
  }

  bool empty() const {
    return Components.empty();
  }

  /// Convert to a (maximum-4-element) llvm::VersionTuple, truncating
  /// away any 5th component that might be in this version.
  operator llvm::VersionTuple() const;

  /// Returns the concrete version to use when \e this version is provided as
  /// an argument to -swift-version.
  ///
  /// This is not the same as the set of Swift versions that have ever existed,
  /// just those that we are attempting to maintain backward-compatibility
  /// support for. It's also common for valid versions to produce a different
  /// result; for example "-swift-version 3" at one point instructed the
  /// compiler to act as if it is version 3.1.
  llvm::Optional<Version> getEffectiveLanguageVersion() const;

  /// Whether this version is greater than or equal to the given major version
  /// number.
  bool isVersionAtLeast(unsigned major, unsigned minor = 0) const {
    switch (size()) {
    case 0:
      return false;
    case 1:
      return ((Components[0] == major && 0 == minor) ||
              (Components[0] > major));
    default:
      return ((Components[0] == major && Components[1] >= minor) ||
              (Components[0] > major));
    }
  }

  /// Return this Version struct with minor and sub-minor components stripped
  Version asMajorVersion() const;

  /// Return this Version struct as the appropriate version string for APINotes.
  std::string asAPINotesVersionString() const;

  /// Returns a version from the currently defined SWIFT_VERSION_MAJOR and
  /// SWIFT_VERSION_MINOR.
  static Version getCurrentLanguageVersion();

  // List of backward-compatibility versions that we permit passing as
  // -swift-version <vers>
  static std::array<StringRef, 3> getValidEffectiveVersions() {
    return {{"4", "4.2", "5"}};
  };
};

bool operator>=(const Version &lhs, const Version &rhs);
bool operator<(const Version &lhs, const Version &rhs);
bool operator==(const Version &lhs, const Version &rhs);
inline bool operator!=(const Version &lhs, const Version &rhs) {
  return !(lhs == rhs);
}

raw_ostream &operator<<(raw_ostream &os, const Version &version);

/// Retrieves the numeric {major, minor} Swift version.
///
/// Note that this is the underlying version of the language, ignoring any
/// -swift-version flags that may have been used in a particular invocation of
/// the compiler.
std::pair<unsigned, unsigned> getSwiftNumericVersion();

/// Retrieves a string representing the complete Swift version, which includes
/// the Swift supported and effective version numbers, the repository version,
/// and the vendor tag.
std::string getSwiftFullVersion(Version effectiveLanguageVersion =
                                Version::getCurrentLanguageVersion());

/// Retrieves the repository revision number (or identifier) from which
/// this Swift was built.
StringRef getSwiftRevision();

/// Is the running compiler built with a version tag for distribution?
/// When true, \c version::getCurrentCompilerVersion returns a valid version
/// and \c getCurrentCompilerTag returns the version tuple in string format.
bool isCurrentCompilerTagged();

/// Retrieves the distribution tag of the running compiler, if any.
StringRef getCurrentCompilerTag();

/// Retrieves the distribution tag of the running compiler for serialization,
/// if any. This can hold more information than \c getCurrentCompilerTag
/// depending on the vendor.
StringRef getCurrentCompilerSerializationTag();

} // end namespace version
} // end namespace swift

#endif // SWIFT_BASIC_VERSION_H
