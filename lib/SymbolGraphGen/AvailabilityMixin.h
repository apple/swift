//===--- AvailabilityMixin.h - Symbol Graph Symbol Availability -----------===//
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

#ifndef SWIFT_SYMBOLGRAPHGEN_AVAILABILITYMIXIN_H
#define SWIFT_SYMBOLGRAPHGEN_AVAILABILITYMIXIN_H

#include "swift/AST/Attr.h"
#include "swift/AST/Module.h"
#include "swift/Basic/LLVM.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/VersionTuple.h"

namespace swift {
namespace symbolgraphgen {

/// A mixin representing a symbol's effective availability in its module.
struct Availability {
  /// The domain to which the availability applies, such as
  /// an operating system or Swift itself.
  StringRef Domain;

  /// The domain version at which a symbol was introduced if defined.
  llvm::Optional<llvm::VersionTuple> Introduced;

  /// The domain version at which a symbol was deprecated if defined.
  llvm::Optional<llvm::VersionTuple> Deprecated;

  /// The domain version at which a symbol was obsoleted if defined.
  llvm::Optional<llvm::VersionTuple> Obsoleted;

  /// An optional message regarding a symbol's availability.
  StringRef Message;

  /// The informal spelling of a new replacement symbol if defined.
  StringRef Renamed;

  /// If \c true, is unconditionally deprecated in this \c Domain.
  bool IsUnconditionallyDeprecated;

  /// If \c true, is unconditionally unavailable in this \c Domain.
  bool IsUnconditionallyUnavailable;

  Availability(const AvailableAttr &AvAttr);

  /// Update this availability from a duplicate @available
  /// attribute with the same platform on the same declaration.
  ///
  /// e.g.
  /// @available(macOS, deprecated: 10.15)
  /// @available(macOS, deprecated: 10.12)
  /// func foo() {}
  ///
  /// Updates the first availability using the second's information.
  void updateFromDuplicate(const Availability &Other);

  /// Update this availability from a parent context's availability.
  void updateFromParent(const Availability &Parent);

  /// Returns true if this availability item doesn't have
  /// any introduced version, deprecated version, obsoleted version,
  /// or unconditional deprecation status.
  ///
  /// \note \c message and \c renamed are not considered.
  bool empty() const;

  void serialize(llvm::json::OStream &OS) const;
};

} // end namespace symbolgraphgen
} // end namespace swift

#endif // SWIFT_SYMBOLGRAPHGEN_AVAILABILITYMIXIN_H
