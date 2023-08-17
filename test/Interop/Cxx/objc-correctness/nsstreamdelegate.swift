// RUN: %target-typecheck-verify-swift -verify-ignore-unknown -I %S/Inputs -enable-objc-interop -enable-experimental-cxx-interop
// REQUIRES: objc_interop

import NSStreamDelegate

func foo<T: NSStreamDelegate>(_ delegate: T, stream: NSStream) {
  delegate.stream!(stream, handle: NSStreamEvent.openCompleted)
}
