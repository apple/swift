//===--- Version.cpp - Swift Version Number -------------------------------===//
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
// This file defines several version-related utility functions for Swift.
//
//===----------------------------------------------------------------------===//

#include "swift/Basic/Assertions.h"
#include "swift/Basic/Version.h"
#include "swift/Basic/LLVM.h"
#include "clang/Basic/CharInfo.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/raw_ostream.h"

#include <vector>

#define TOSTR2(X) #X
#define TOSTR(X) TOSTR2(X)

#ifdef SWIFT_VERSION_PATCHLEVEL
/// Helper macro for SWIFT_VERSION_STRING.
#define SWIFT_MAKE_VERSION_STRING(X, Y, Z) TOSTR(X) "." TOSTR(Y) "." TOSTR(Z)

/// A string that describes the Swift version number, e.g., "1.0".
#define SWIFT_VERSION_STRING                                                   \
  SWIFT_MAKE_VERSION_STRING(SWIFT_VERSION_MAJOR, SWIFT_VERSION_MINOR,          \
                            SWIFT_VERSION_PATCHLEVEL)
#else
/// Helper macro for SWIFT_VERSION_STRING.
#define SWIFT_MAKE_VERSION_STRING(X, Y) TOSTR(X) "." TOSTR(Y)

/// A string that describes the Swift version number, e.g., "1.0".
#define SWIFT_VERSION_STRING                                                   \
  SWIFT_MAKE_VERSION_STRING(SWIFT_VERSION_MAJOR, SWIFT_VERSION_MINOR)
#endif

#include "LLVMRevision.inc"
#include "SwiftRevision.inc"

namespace swift {
namespace version {

/// Print a string of the form "LLVM xxxxx, Swift zzzzz", where each placeholder
/// is the revision for the associated repository.
static void printFullRevisionString(raw_ostream &out) {
  // Arbitrarily truncate to 15 characters. This should be enough to unique Git
  // hashes while keeping the REPL version string from overflowing 80 columns.
#if defined(LLVM_REVISION)
  out << "LLVM " << StringRef(LLVM_REVISION).slice(0, 15);
# if defined(SWIFT_REVISION)
  out << ", ";
# endif
#endif

#if defined(SWIFT_REVISION)
  out << "Swift " << StringRef(SWIFT_REVISION).slice(0, 15);
#endif
}

Version Version::getCurrentLanguageVersion() {
#if SWIFT_VERSION_PATCHLEVEL
  return {SWIFT_VERSION_MAJOR, SWIFT_VERSION_MINOR, SWIFT_VERSION_PATCHLEVEL};
#else
  return {SWIFT_VERSION_MAJOR, SWIFT_VERSION_MINOR};
#endif
}

raw_ostream &operator<<(raw_ostream &os, const Version &version) {
  if (version.empty())
    return os;
  os << version[0];
  for (size_t i = 1, e = version.size(); i != e; ++i)
    os << '.' << version[i];
  return os;
}

std::string
Version::preprocessorDefinition(StringRef macroName,
                                ArrayRef<uint64_t> componentWeights) const {
  uint64_t versionConstant = 0;

  for (size_t i = 0, e = std::min(componentWeights.size(), Components.size());
       i < e; ++i) {
    versionConstant += componentWeights[i] * Components[i];
  }

  std::string define("-D");
  llvm::raw_string_ostream(define) << macroName << '=' << versionConstant;
  // This isn't using stream.str() so that we get move semantics.
  return define;
}

Version::Version(const llvm::VersionTuple &version) {
  if (version.empty())
    return;

  Components.emplace_back(version.getMajor());

  if (auto minor = version.getMinor()) {
    Components.emplace_back(*minor);
    if (auto subminor = version.getSubminor()) {
      Components.emplace_back(*subminor);
      if (auto build = version.getBuild()) {
        Components.emplace_back(*build);
      }
    }
  }
}

Version::operator llvm::VersionTuple() const
{
  switch (Components.size()) {
 case 0:
   return llvm::VersionTuple();
 case 1:
   return llvm::VersionTuple((unsigned)Components[0]);
 case 2:
   return llvm::VersionTuple((unsigned)Components[0],
                              (unsigned)Components[1]);
 case 3:
   return llvm::VersionTuple((unsigned)Components[0],
                              (unsigned)Components[1],
                              (unsigned)Components[2]);
 case 4:
 case 5:
   return llvm::VersionTuple((unsigned)Components[0],
                              (unsigned)Components[1],
                              (unsigned)Components[2],
                              (unsigned)Components[3]);
 default:
   llvm_unreachable("swift::version::Version with 6 or more components");
  }
}

std::optional<Version> Version::getEffectiveLanguageVersion() const {
  switch (size()) {
  case 0:
    return std::nullopt;
  case 1:
    break;
  case 2:
    // The only valid explicit language version with a minor
    // component is 4.2.
    if (Components[0] == 4 && Components[1] == 2)
      break;
    return std::nullopt;
  default:
    // We do not want to permit users requesting more precise effective language
    // versions since accepting such an argument promises more than we're able
    // to deliver.
    return std::nullopt;
  }

  // FIXME: When we switch to Swift 5 by default, the "4" case should return
  // a version newer than any released 4.x compiler, and the
  // "5" case should start returning getCurrentLanguageVersion. We should
  // also check for the presence of SWIFT_VERSION_PATCHLEVEL, and if that's
  // set apply it to the "3" case, so that Swift 4.0.1 will automatically
  // have a compatibility mode of 3.2.1.
  switch (Components[0]) {
  case 4:
    // Version '4' on its own implies '4.1.50'.
    if (size() == 1)
      return Version{4, 1, 50};
    // This should be true because of the check up above.
    assert(size() == 2 && Components[0] == 4 && Components[1] == 2);
    return Version{4, 2};
  case 5:
    return Version{5, 10};
  case 6:
    static_assert(SWIFT_VERSION_MAJOR == 6,
                  "getCurrentLanguageVersion is no longer correct here");
    return Version::getCurrentLanguageVersion();
  default:
    return std::nullopt;
  }
}

Version Version::asMajorVersion() const {
  if (empty())
    return {};
  Version res;
  res.Components.push_back(Components[0]);
  return res;
}

std::string Version::asAPINotesVersionString() const {
  // Other than for "4.2.x", map the Swift major version into
  // the API notes version for Swift. This has the effect of allowing
  // API notes to effect changes only on Swift major versions,
  // not minor versions.
  if (size() >= 2 && Components[0] == 4 && Components[1] == 2)
    return "4.2";
  return llvm::itostr(Components[0]);
}

bool operator>=(const class Version &lhs,
                const class Version &rhs) {

  // The empty compiler version represents the latest possible version,
  // usually built from the source repository.
  if (lhs.empty())
    return true;

  auto n = std::max(lhs.size(), rhs.size());

  for (size_t i = 0; i < n; ++i) {
    auto lv = i < lhs.size() ? lhs[i] : 0;
    auto rv = i < rhs.size() ? rhs[i] : 0;
    if (lv < rv)
      return false;
    else if (lv > rv)
      return true;
  }
  // Equality
  return true;
}

bool operator<(const class Version &lhs, const class Version &rhs) {

  return !(lhs >= rhs);
}

bool operator==(const class Version &lhs,
                const class Version &rhs) {
  auto n = std::max(lhs.size(), rhs.size());
  for (size_t i = 0; i < n; ++i) {
    auto lv = i < lhs.size() ? lhs[i] : 0;
    auto rv = i < rhs.size() ? rhs[i] : 0;
    if (lv != rv)
      return false;
  }
  return true;
}

std::pair<unsigned, unsigned> getSwiftNumericVersion() {
  return { SWIFT_VERSION_MAJOR, SWIFT_VERSION_MINOR };
}

std::string getSwiftFullVersion(Version effectiveVersion) {
  std::string buf;
  llvm::raw_string_ostream OS(buf);

#ifdef SWIFT_VENDOR
  OS << SWIFT_VENDOR " ";
#endif

  OS << "Swift version " SWIFT_VERSION_STRING;
#ifndef SWIFT_COMPILER_VERSION
  OS << "-dev";
#endif

  if (effectiveVersion != Version::getCurrentLanguageVersion()) {
    OS << " effective-" << effectiveVersion;
  }

#if defined(SWIFT_COMPILER_VERSION)
  OS << " (swiftlang-" SWIFT_COMPILER_VERSION;
#if defined(CLANG_COMPILER_VERSION)
  OS << " clang-" CLANG_COMPILER_VERSION;
#endif
  OS << ")";
#elif defined(LLVM_REVISION) || defined(SWIFT_REVISION)
  OS << " (";
  printFullRevisionString(OS);
  OS << ")";
#endif

  // Suppress unused function warning
  (void)&printFullRevisionString;

  return OS.str();
}

StringRef getSwiftRevision() {
#ifdef SWIFT_REVISION
  return SWIFT_REVISION;
#else
  return "";
#endif
}

bool isCurrentCompilerTagged() {
#ifdef SWIFT_COMPILER_VERSION
  return true;
#else
  return false;
#endif
}

StringRef getCurrentCompilerTag() {
#ifdef SWIFT_COMPILER_VERSION
  return SWIFT_COMPILER_VERSION;
#else
  return StringRef();
#endif
}

StringRef getCurrentCompilerSerializationTag() {
#ifdef SWIFT_COMPILER_VERSION
  return SWIFT_COMPILER_VERSION;
#else
  return StringRef();
#endif
}

StringRef getCurrentCompilerChannel() {
  static const char* forceDebugChannel =
    ::getenv("SWIFT_FORCE_SWIFTMODULE_CHANNEL");
  if (forceDebugChannel)
    return forceDebugChannel;

  // Leave it to downstream compilers to define the different channels.
  return StringRef();
}

unsigned getUpcomingCxxInteropCompatVersion() {
  return SWIFT_VERSION_MAJOR + 1;
}

} // end namespace version
} // end namespace swift
