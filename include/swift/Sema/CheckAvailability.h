//===--- CheckAvailability.h - CheckAvailability ----------*- C++ -*-===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2024 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

#ifndef SWIFT_SEMA_CHECKAVAILABILITY_H
#define SWIFT_SEMA_CHECKAVAILABILITY_H

namespace swift {
class NominalOrBoundGenericNominalType;

/// NoncopyableGenerics, which is the generics system for inverses, does not
/// always require the runtime metadata that is only availabile in Swift 6.
///
/// \returns true if the given type needs the newer runtime.
bool requiresNoncopyableGenericsAvailabilityCheck(
    NominalOrBoundGenericNominalType *type);
}

#endif //SWIFT_SEMA_CheckAVAILABILITY_H
