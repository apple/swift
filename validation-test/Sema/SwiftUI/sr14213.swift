// RUN: %target-typecheck-verify-swift -target %target-cpu-apple-macosx10.15 -swift-version 5

// REQUIRES: objc_interop
// REQUIRES: OS=macosx

import SwiftUI

struct Experiment: View {
  var body: some View {
    HStack { // expected-error {{generic parameter 'Content' could not be inferred}} expected-note {{explicitly specify the generic arguments to fix this issue}}emacs
      Slider(value: <#T##Binding<BinaryFloatingPoint>#>, in: <#T##ClosedRange<BinaryFloatingPoint>#>, label: <#T##() -> _#>) // expected-error 3 {{editor placeholder in source file}}
      // expected-error@-1 {{protocol 'BinaryFloatingPoint' as a type cannot conform to 'Comparable'}}
      // expected-note@-2 {{only concrete types such as structs, enums and classes can conform to protocols}}
    }
  }
}
