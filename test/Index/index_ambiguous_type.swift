// RUN: %empty-directory(%t)
// RUN: split-file %s %t

// Validate that we get index references for almost ambiguous types

// RUN: %target-swift-frontend -emit-module -o %t %t/file1.swift
// RUN: %target-swift-frontend -emit-module -o %t %t/file2.swift

// RUN: %target-swift-ide-test -print-indexed-symbols -source-filename %t/file3.swift -I %t > %t/output.txt
// RUN: %FileCheck %s < %t/output.txt

//--- file1.swift
public struct Thing {
  public init(string: String) {}

  public struct Nested {
    public init(string: String) {}
  }

  public struct Nested2<T> {
    public init(value: T) {}
  }
}
//--- file2.swift
public struct Thing {}

//--- file3.swift
import file1
import file2

func foo() {
  // CHECK: 7:7 | struct/Swift | Thing | s:5file15ThingV | Ref,RelCont | rel: 1
  // CHECK: 7:7 | constructor/Swift | init(string:) | s:5file15ThingV6stringACSS_tcfc | Ref,Call,RelCall,RelCont | rel: 1
  _ = Thing(string: "lol")
  // CHECK: 10:7 | struct/Swift | Thing | s:5file15ThingV | Ref,RelCont | rel: 1
  // CHECK: 10:13 | constructor/Swift | init(string:) | s:5file15ThingV6stringACSS_tcfc | Ref,Call,RelCall,RelCont | rel: 1
  _ = Thing.init(string: "lol")
  // CHECK: 14:7 | struct/Swift | Thing | s:5file15ThingV | Ref,RelCont | rel: 1
  // CHECK: 14:13 | struct/Swift | Nested | s:5file15ThingV6NestedV | Ref,RelCont | rel: 1
  // TODO: 14:13 | constructor/Swift | init(string:)
  _ = Thing.Nested(string: "lol")
  // CHECK: 18:7 | struct/Swift | Thing | s:5file15ThingV | Ref,RelCont | rel: 1
  // CHECK: 18:13 | struct/Swift | Nested | s:5file15ThingV6NestedV | Ref,RelCont | rel: 1
  // CHECK: 18:20 | constructor/Swift | init(string:) | s:5file15ThingV6NestedV6stringAESS_tcfc | Ref,Call,RelCall,RelCont | rel: 1
  _ = Thing.Nested.init(string: "lol")
  // CHECK: 23:7 | struct/Swift | Thing | s:5file15ThingV | Ref,RelCont | rel: 1
  // CHECK: 23:13 | struct/Swift | Nested2 | s:5file15ThingV7Nested2V | Ref,RelCont | rel: 1
  // TODO: 23:13 | constructor/Swift | init(value:)
  // TODO: 23:21 | struct/Swift | Int | s:Si | Ref,RelCont | rel: 1
  _ = Thing.Nested2<Int>(value: 0)
  // CHECK: 28:7 | struct/Swift | Thing | s:5file15ThingV | Ref,RelCont | rel: 1
  // CHECK: 28:13 | struct/Swift | Nested2 | s:5file15ThingV7Nested2V | Ref,RelCont | rel: 1
  // TODO: 28:21 | struct/Swift | Int | s:Si | Ref,RelCont | rel: 1
  // CHECK: 28:26 | constructor/Swift | init(value:) | s:5file15ThingV7Nested2V5valueAEy_xGx_tcfc | Ref,Call,RelCall,RelCont | rel: 1
  _ = Thing.Nested2<Int>.init(value: 0)
  // CHECK: 34:7 | module/Swift | file1 | c:@M@file1 | Ref,RelCont | rel: 1
  // CHECK: 34:13 | struct/Swift | Thing | s:5file15ThingV | Ref,RelCont | rel: 1
  // CHECK: 34:19 | struct/Swift | Nested2 | s:5file15ThingV7Nested2V | Ref,RelCont | rel: 1
  // CHECK: 34:19 | constructor/Swift | init(value:) | s:5file15ThingV7Nested2V5valueAEy_xGx_tcfc | Ref,Call,RelCall,RelCont | rel: 1
  // CHECK: 34:27 | struct/Swift | Int | s:Si | Ref,RelCont | rel: 1
  _ = file1.Thing.Nested2<Int>(value: 0)
}
