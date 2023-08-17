// RUN: %target-typecheck-verify-swift -verify-ignore-unknown -I %S/Inputs -enable-objc-interop -enable-experimental-cxx-interop
// REQUIRES: OS=macosx

import Darwin

_ = pthread_mutexattr_t() // expected-warning {{'init()' is deprecated: This zero-initializes the backing memory of the struct}}
