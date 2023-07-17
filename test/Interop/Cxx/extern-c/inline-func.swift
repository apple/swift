// RUN: %target-run-simple-swift(-I %S/Inputs/ -Xfrontend -enable-experimental-cxx-interop)
// REQUIRES: executable_test

import InlineFunc

public func test() {
  caller()
}
