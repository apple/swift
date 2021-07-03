// RUN: %target-swift-frontend -typecheck -diagnostics-editor-mode -verify %s
// REQUIRES: objc_interop

import Foundation

// SR-13515
// Tailored diagnostic when there are witnesses to '@objc'
// requirements which are default implementations.

@objc protocol ObjCProtocol {
  @objc func requiredMethod() // expected-note {{requirement 'requiredMethod()' declared here}}
  @objc var requiredProperty: Int { get } // expected-note {{requirement 'requiredProperty' declared here}}
  @objc subscript(required: Int) -> Int { get } // expected-note {{requirement 'subscript(_:)' declared here}}
}

extension ObjCProtocol {
  func requiredMethod() {}
  var requiredProperty: Int { 0 }
  subscript(required: Int) -> Int { 1 }
}

class ConformingClass: NSObject, ObjCProtocol {} 
// expected-error@-1 {{type 'ConformingClass' does not conform to protocol 'ObjCProtocol'}}
// expected-note@-2 {{default implementations cannot be used to satisfy requirements of '@objc' protocols}}
// expected-note@-3 {{do you want to add protocol stubs?}} {{47-47=\n    func requiredMethod() {\n        <#code#>\n    \}\n\n    var requiredProperty: Int\n\n    subscript(required: Int) -> Int {\n        <#code#>\n    \}\n}}
