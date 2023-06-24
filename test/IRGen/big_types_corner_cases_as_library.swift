// RUN: %target-swift-frontend -disable-type-layout %s -emit-ir  -parse-as-library | %FileCheck %s

public struct BigStruct {
  var i0 : Int32 = 0
  var i1 : Int32 = 1
  var i2 : Int32 = 2
  var i3 : Int32 = 3
  var i4 : Int32 = 4
  var i5 : Int32 = 5
  var i6 : Int32 = 6
  var i7 : Int32 = 7
  var i8 : Int32 = 8
}

// CHECK-LABEL: define{{( dllexport)?}}{{( protected)?}} internal ptr @"$s33big_types_corner_cases_as_library9BigStructVwCP"
// CHECK: ret ptr {{.*}}
let bigStructGlobalArray : [BigStruct] = [
  BigStruct()
]
