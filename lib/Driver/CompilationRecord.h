//===--- CompilationRecord.h ------------------------------------*- C++ -*-===//
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

#ifndef SWIFT_DRIVER_COMPILATIONRECORD_H
#define SWIFT_DRIVER_COMPILATIONRECORD_H

#include "swift/Driver/Action.h"

namespace swift {
namespace driver {
namespace compilation_record {

/// Compilation record files (-master.swiftdeps files) are YAML files composed
/// of these top-level keys.
enum class TopLevelKey {
  /// The key for the Swift compiler version used to produce the compilation
  /// record.
  Version,
  /// The key for the list of arguments passed to the Swift compiler when
  /// producing the compilation record.
  Options,
  /// The key for the time at which the build that produced the compilation
  /// record started.
  BuildTime,
  /// The key for the list of inputs to the compilation that produced the
  /// compilation record.
  Inputs,
};

/// \returns A string representation of the given key.
inline static StringRef getName(TopLevelKey Key) {
  switch (Key) {
  case TopLevelKey::Version: return "version";
  case TopLevelKey::Options: return "options";
  case TopLevelKey::BuildTime: return "build_time";
  case TopLevelKey::Inputs: return "inputs";
  }

  // Work around MSVC warning: not all control paths return a value
  llvm_unreachable("All switch cases are covered");
}

/// \returns The string identifier used to represent the given status in a
/// compilation record file (.swiftdeps file).
///
/// \note Not every InputInfo::Status has a unique identifier. For example,
/// both NewlyAdded and NeedsCascadingBuild are represented as "!dirty".
/// Therefore, this will not cleanly round-trip between InputInfo::Status and
/// string identifiers.
inline static StringRef
getIdentifierForInputInfoStatus(CompileJobAction::InputInfo::Status Status) {
  switch (Status) {
  case CompileJobAction::InputInfo::Status::UpToDate:
    return "";
  case CompileJobAction::InputInfo::Status::NewlyAdded:
  case CompileJobAction::InputInfo::Status::NeedsCascadingBuild:
    return "!dirty";
  case CompileJobAction::InputInfo::Status::NeedsNonCascadingBuild:
    return "!private";
  }

  // Work around MSVC warning: not all control paths return a value
  llvm_unreachable("All switch cases are covered");
}

/// \returns The status corresponding to the string identifier used in a
/// compilation record file (.swiftdeps file).
inline static llvm::Optional<CompileJobAction::InputInfo::Status>
getInfoStatusForIdentifier(StringRef Identifier) {
  using InputStatus = CompileJobAction::InputInfo::Status;
  return llvm::StringSwitch<llvm::Optional<InputStatus>>(Identifier)
      .Case("", InputStatus::UpToDate)
      .Case("!dirty", InputStatus::NeedsCascadingBuild)
      .Case("!private", InputStatus::NeedsNonCascadingBuild)
      .Default(llvm::None);
}

} // end namespace compilation_record
} // end namespace driver
} // end namespace swift

#endif
