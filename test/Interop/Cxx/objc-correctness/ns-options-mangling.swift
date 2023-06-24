// RUN: %target-swift-frontend -I %S/Inputs -c -cxx-interoperability-mode=swift-5.9 %s -S -o - | %FileCheck %s
// RUN: %target-swift-frontend -I %S/Inputs -c %s -S -o - | %FileCheck %s

// REQUIRES: objc_interop

// CHECK: _$sSo14UIControlStateV4main7FooableACMc
// The following check is to ensure the conformance is mangled properly:
// protocol conformance descriptor for __C.UIControlState : main.Fooable in main
import NSOptionsMangling
protocol Fooable { }
extension UIControl.State: Fooable {}
