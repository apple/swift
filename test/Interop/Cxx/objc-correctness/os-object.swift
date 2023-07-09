// RUN: %target-swift-frontend -typecheck -verify -I %S/Inputs -cxx-interoperability-mode=swift-5.9 %s
// REQUIRES: objc_interop

import OSObject

extension my_object_t {
  func dummy() {}
}
