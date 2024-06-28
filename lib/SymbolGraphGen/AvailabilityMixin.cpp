//===--- AvailabilityMixin.cpp - Symbol Graph Symbol Availability ---------===//
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

#include "AvailabilityMixin.h"
#include "JSON.h"
#include "swift/Basic/Assertions.h"

using namespace swift;
using namespace symbolgraphgen;

namespace {
StringRef getDomain(const AvailableAttr &AvAttr) {
  switch (AvAttr.getPlatformAgnosticAvailability()) {
    // SPM- and Swift-specific availability.
    case PlatformAgnosticAvailabilityKind::PackageDescriptionVersionSpecific:
      return { "SwiftPM" };
    case PlatformAgnosticAvailabilityKind::SwiftVersionSpecific:
    case PlatformAgnosticAvailabilityKind::UnavailableInSwift:
      return { "Swift" };
    // Although these are in the agnostic kinds, they are actually a signal
    // that there is either platform-specific or completely platform-agnostic.
    // They'll be handled below.
    case PlatformAgnosticAvailabilityKind::Deprecated:
    case PlatformAgnosticAvailabilityKind::Unavailable:
    case PlatformAgnosticAvailabilityKind::None:
    case PlatformAgnosticAvailabilityKind::NoAsync:
      break;
  }

  // Platform-specific availability.
  switch (AvAttr.Platform) {
    case swift::PlatformKind::iOS:
      return { "iOS" };
    case swift::PlatformKind::macCatalyst:
      return { "macCatalyst" };
    case swift::PlatformKind::macOS:
      return { "macOS" };
    case swift::PlatformKind::tvOS:
      return { "tvOS" };
    case swift::PlatformKind::watchOS:
      return { "watchOS" };
    case swift::PlatformKind::visionOS:
      return { "visionOS" };
    case swift::PlatformKind::iOSApplicationExtension:
      return { "iOSAppExtension" };
    case swift::PlatformKind::macCatalystApplicationExtension:
      return { "macCatalystAppExtension" };
    case swift::PlatformKind::macOSApplicationExtension:
      return { "macOSAppExtension" };
    case swift::PlatformKind::tvOSApplicationExtension:
      return { "tvOSAppExtension" };
    case swift::PlatformKind::watchOSApplicationExtension:
      return { "watchOSAppExtension" };
    case swift::PlatformKind::visionOSApplicationExtension:
      return { "visionOSAppExtension" };
    case swift::PlatformKind::OpenBSD:
      return { "OpenBSD" };
    case swift::PlatformKind::Windows:
      return { "Windows" };
    case swift::PlatformKind::none:
      return { "*" };
  }
  llvm_unreachable("invalid platform kind");
}
} // end anonymous namespace

Availability::Availability(const AvailableAttr &AvAttr)
  : Domain(getDomain(AvAttr)),
    Introduced(AvAttr.Introduced),
    Deprecated(AvAttr.Deprecated),
    Obsoleted(AvAttr.Obsoleted),
    Message(AvAttr.Message),
    Renamed(AvAttr.Rename),
    IsUnconditionallyDeprecated(AvAttr.isUnconditionallyDeprecated()),
    IsUnconditionallyUnavailable(AvAttr.isUnconditionallyUnavailable()) {
      assert(!Domain.empty());
}

void
Availability::updateFromDuplicate(const Availability &Other) {
  assert(Domain == Other.Domain);

  // The highest `introduced` version always wins
  // regardless of the order in which they appeared in the source.
  if (!Introduced) {
    Introduced = Other.Introduced;
  } else if (Other.Introduced && *Other.Introduced > *Introduced) {
    Introduced = Other.Introduced;
  }

  // The `deprecated` version that appears last in the source always wins,
  // allowing even to erase a previous deprecated.
  Deprecated = Other.Deprecated;

  // Same for `deprecated` with no version.
  IsUnconditionallyDeprecated = Other.IsUnconditionallyDeprecated;

  // Same for `unavailable` with no version.
  IsUnconditionallyUnavailable = Other.IsUnconditionallyUnavailable;

  // Same for `obsoleted`.
  Obsoleted = Other.Obsoleted;

  // The `message` that appears last in the source always wins.
  Message = Other.Message;

  // Same for `renamed`.
  Renamed = Other.Renamed;
}

void
Availability::updateFromParent(const Availability &Parent) {
  assert(Domain == Parent.Domain);

  // Allow filling, but never replace a child's existing `introduced`
  // availability because it can never be less available than the parent anyway.
  //
  // e.g. you cannot write this:
  // @available(macos, introduced: 10.15)
  // struct S {
  //   @available(macos, introduced: 10.14)
  //   func foo() {}
  // }
  //
  // So the child's `introduced` availability will always
  // be >= the parent's.
  if (!Introduced) {
    Introduced = Parent.Introduced;
  }

  // Allow filling from the parent.
  // For replacement, we will consider a parent's
  // earlier deprecation to supersede a child's later deprecation.
  if (!Deprecated) {
    Deprecated = Parent.Deprecated;
  } else if (Parent.Deprecated && *Parent.Deprecated < *Deprecated) {
    Deprecated = Parent.Deprecated;
  }

  // The above regarding `deprecated` also will apply to `obsoleted`.
  if (!Obsoleted) {
    Obsoleted = Parent.Obsoleted;
  } else if (Parent.Obsoleted && *Parent.Obsoleted < *Obsoleted) {
    Obsoleted = Parent.Obsoleted;
  }

  // Never replace or fill a child's `message` with a parent's because
  // there may be context at the parent that doesn't apply at the child,
  // i.e. it might not always make sense.

  // Never replace or fill a child's `renamed` field because it
  // doesn't make sense. Just because a parent is renamed doesn't
  // mean its child is renamed to the same thing.

  // If a parent is unconditionally deprecated, then so are all
  // of its children.
  IsUnconditionallyDeprecated |= Parent.IsUnconditionallyDeprecated;

  // If a parent is unconditionally unavailable, then so are all
  // of its children.
  IsUnconditionallyUnavailable |= Parent.IsUnconditionallyUnavailable;
}

void Availability::serialize(llvm::json::OStream &OS) const {
  OS.object([&](){
    OS.attribute("domain", Domain);
    if (Introduced) {
      AttributeRAII IntroducedAttribute("introduced", OS);
      symbolgraphgen::serialize(*Introduced, OS);
    }
    if (Deprecated) {
      AttributeRAII DeprecatedAttribute("deprecated", OS);
      symbolgraphgen::serialize(*Deprecated, OS);
    }
    if (Obsoleted) {
      AttributeRAII ObsoletedAttribute("obsoleted", OS);
      symbolgraphgen::serialize(*Obsoleted, OS);
    }
    if (!Message.empty()) {
      OS.attribute("message", Message);
    }
    if (!Renamed.empty()) {
      OS.attribute("renamed", Renamed);
    }
    if (IsUnconditionallyDeprecated) {
      OS.attribute("isUnconditionallyDeprecated", true);
    }
    if (IsUnconditionallyUnavailable) {
      OS.attribute("isUnconditionallyUnavailable", true);
    }
  }); // end availability object
}

bool Availability::empty() const {
  return !Introduced.has_value() &&
         !Deprecated.has_value() &&
         !Obsoleted.has_value() &&
         !IsUnconditionallyDeprecated &&
         !IsUnconditionallyUnavailable;
}
