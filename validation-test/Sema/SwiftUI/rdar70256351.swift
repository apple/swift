// RUN: %target-typecheck-verify-swift -target %target-cpu-apple-macosx10.15 -swift-version 5
// REQUIRES: objc_interop
// REQUIRES: OS=macosx

import SwiftUI

struct ContentView: View {
  @State private var currentPage = "1"

  var body: some View {
    switch currentPage {
    case 1: // expected-error {{expression pattern of type 'Int' cannot match values of type 'String'}}
      Text("1")
    default:
      Text("default")
    }
  }
}
