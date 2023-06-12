// RUN: %target-run-simple-swift(-I %S/Inputs -Xfrontend -enable-experimental-cxx-interop)

// REQUIRES: objc_interop
// REQUIRES: executable_test

import CenumsNSOptions
import StdlibUnittest

var FieldTestSuite = TestSuite("NS_Option as field")

struct HasNSOptionMember {
  var member: Bar
}

FieldTestSuite.test("NSOption as field") {
  var parent = HasNSOptionMember(member: [.SwiftOptionOneApiNotes, .SwiftOptionTwoApiNotes])
  expectEqual(parent.member, [.SwiftOptionOneApiNotes, .SwiftOptionTwoApiNotes])
  
  parent.member = [.SwiftOptionOneApiNotes]
  expectNotEqual(parent.member, [.SwiftOptionOneApiNotes, .SwiftOptionTwoApiNotes])
}

runAllTests()
