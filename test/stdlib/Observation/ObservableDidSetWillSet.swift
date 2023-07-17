// REQUIRES: swift_swift_parser, executable_test

// RUN: %target-run-simple-swift( -Xfrontend -disable-availability-checking -enable-experimental-feature InitAccessors -enable-experimental-feature Macros -enable-experimental-feature ExtensionMacros -Xfrontend -plugin-path -Xfrontend %swift-host-lib-dir/plugins) | %FileCheck %s

// Asserts is required for '-enable-experimental-feature InitAccessors'.
// REQUIRES: asserts

// REQUIRES: observation
// REQUIRES: concurrency
// REQUIRES: objc_interop
// UNSUPPORTED: use_os_stdlib
// UNSUPPORTED: back_deployment_runtime

import Observation

@Observable
public class Model {
  public enum State {
    case initializing
    case running
    case complete
  }

  public var state: State = .initializing {
    willSet {
      print("new state=\(String(describing: newValue))")
    }

    didSet {
      guard oldValue != state else { return }
      print("old state=\(String(describing: oldValue))")
    }
  }
}


let m = Model()

// CHECK: new state=running
// CHECK: old state=initializing
m.state = .running
// CHECK: new state=complete
// CHECK: old state=running
m.state = .complete
