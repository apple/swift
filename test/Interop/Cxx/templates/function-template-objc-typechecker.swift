// RUN: not %target-swift-frontend -typecheck %s -I %S/Inputs -enable-experimental-cxx-interop 2>&1 | %FileCheck %s

// REQUIRES: objc_interop

import FunctionTemplates
import ObjectiveC

// Verify that ObjCBool type is banned from C++ template parameters.

takesValue(ObjCBool(true))
// CHECK: error: could not generate C++ types from the generic Swift types provided; the following Swift type(s) provided to 'takesValue' could not be converted: ObjCBool
constLvalueReference(ObjCBool(true))
// CHECK: error: could not generate C++ types from the generic Swift types provided; the following Swift type(s) provided to 'constLvalueReference' could not be converted: ObjCBool
