// RUN: %target-swift-frontend %clang-importer-sdk %s -emit-ir
// REQUIRES: OS=macosx
// REQUIRES: objc_interop

// Test that we don't crash when producing IR.

import AppKit
class MyView: NSView {
  func drawRect() {
    var x = self.superview
    var l = self.layer
    self.layer = CALayer()
    self.nextKeyView = nil
    subviews = []
  }    
}
var m = MyView()
m.drawRect()
