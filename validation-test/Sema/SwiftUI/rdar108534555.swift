// RUN: %target-typecheck-verify-swift -target %target-cpu-apple-macosx10.15 -swift-version 5 -disable-availability-checking

// REQUIRES: objc_interop
// REQUIRES: OS=macosx

import SwiftUI

struct Item: Identifiable {
  var id: Int
  var title: String
}

struct MyGroup<Content> {
  init(@ViewBuilder _: () -> Content) {}
}

struct Test {
  let s: [Item]

  func test() {
    MyGroup {
      ForEach(s) {
        Button($0.title) // expected-error {{value of type 'String' to expected argument type 'PrimitiveButtonStyleConfiguration'}}
      }
    }
  }
}
