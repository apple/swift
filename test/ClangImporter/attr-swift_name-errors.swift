// RUN: %target-swift-frontend(mock-sdk: %clang-importer-sdk) -I %S/Inputs/custom-modules -typecheck %s \
// RUN:     -verify -verify-additional-file %S/Inputs/custom-modules/ConflictingNames.h -verify-ignore-unknown

// REQUIRES: objc_interop

import ConflictingNames // expected-error {{could not build Objective-C module 'ConflictingNames'}}
