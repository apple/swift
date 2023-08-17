// RUN: %target-swift-frontend -emit-ir -g %s -o %t.ll
// RUN: %FileCheck %s --check-prefix IMPORT-CHECK < %t.ll
// RUN: %FileCheck %s --check-prefix LOC-CHECK < %t.ll
// RUN: %FileCheck %s --check-prefix ALLOCCTOR-CHECK < %t.ll
// RUN: llc %t.ll -filetype=obj -o %t.o
// RUN: %llvm-dwarfdump %t.o | %FileCheck %s --check-prefix DWARF-CHECK
// RUN: %llvm-dwarfdump --verify %t.o

// REQUIRES: OS=macosx

import ObjectiveC
import Foundation

class MyObject : NSObject {
  // Ensure we don't emit linetable entries for ObjC thunks.
  // LOC-CHECK: define {{.*}} @"$s4main8MyObjectC0B3ArrSo7NSArrayCvgTo"
  // LOC-CHECK: ret {{.*}}, !dbg ![[DBG:.*]]
  // LOC-CHECK: ret
  @objc var MyArr = NSArray()
  // IMPORT-CHECK: filename: "{{.*}}test-foundation.swift"
  // IMPORT-CHECK-DAG: [[FOUNDATION:[0-9]+]] = !DIModule({{.*}} name: "Foundation",{{.*}} includePath: {{.*}}Foundation.framework
  // IMPORT-CHECK-DAG: [[OVERLAY:[0-9]+]] = !DIModule({{.*}} name: "Foundation",{{.*}} includePath: {{.*}}Foundation.swiftmodule
  // IMPORT-CHECK-DAG: !DICompositeType(tag: DW_TAG_structure_type, name: "NSArray", scope: ![[NSARRAY:[0-9]+]]
  //  IMPORT-CHECK-DAG: ![[NSARRAY]] = !DIModule(scope: ![[FOUNDATION:[0-9]+]], name: "NSArray"
  // We actually imported the Foundation SDK overlay and not the Clang module
  // directly.
  // IMPORT-CHECK-DAG: !DIImportedEntity(tag: DW_TAG_imported_module, {{.*}}entity: ![[OVERLAY]]

  // ALLOCCTOR-CHECK: ![[F:.*]] = !DIFile(filename: "<compiler-generated>",
  // ALLOCCTOR-CHECK: distinct !DISubprogram(name: "init", linkageName: "$sSo7NSArrayCABycfC"
  // ALLOCCTOR-CHECK-SAME: file: ![[F]],
  @objc func foo(_ obj: MyObject) {
    return obj.foo(obj)
  }
}

// SANITY-DAG: !DISubprogram(name: "blah",{{.*}} line: [[@LINE+2]],{{.*}} DISPFlagDefinition
extension MyObject {
  @objc func blah() {
    var _ = MyObject()
  }
}

// SANITY-DAG: ![[NSOBJECT:.*]] = !DICompositeType(tag: DW_TAG_structure_type, name: "NSObject",{{.*}} identifier: "$sSo8NSObjectC"
// SANITY-DAG: !DIGlobalVariable(name: "NsObj",{{.*}} line: [[@LINE+1]],{{.*}} type: ![[NSOBJECT]],{{.*}} DISPFlagDefinition
var NsObj: NSObject
NsObj = MyObject()
var MyObj: MyObject
MyObj = NsObj as! MyObject
MyObj.blah()

public func err() {
  // DWARF-CHECK: DW_AT_name ("NSError")
  // DWARF-CHECK: DW_AT_linkage_name{{.*}}$sSo7NSErrorC
  let _ = NSError(domain: "myDomain", code: 4, 
                  userInfo: ["a":1,
                             "b":2,
                             "c":3])
}

// LOC-CHECK: define {{.*}}4date
public func date() {
  // LOC-CHECK: call {{.*}} @"$sSS21_builtinStringLiteral17utf8CodeUnitCount7isASCIISSBp_BwBi1_tcfC{{.*}}, !dbg ![[L1:.*]]
  let d1 = DateFormatter()
  d1.dateFormat = "dd. mm. yyyy" // LOC-CHECK: call{{.*}}objc_msgSend{{.*}}, !dbg ![[L2:.*]]
  // LOC-CHECK: call {{.*}} @"$sSS21_builtinStringLiteral17utf8CodeUnitCount7isASCIISSBp_BwBi1_tcfC{{.*}}, !dbg ![[L3:.*]]
  let d2 = DateFormatter()
  d2.dateFormat = "mm dd yyyy" // LOC-CHECK: call{{.*}}objc_msgSend{{.*}}, !dbg ![[L4:.*]]
}

// Make sure we build some witness tables for enums.
func useOptions(_ opt: URL.BookmarkCreationOptions)
       -> URL.BookmarkCreationOptions {
  return [opt, opt]
}

// LOC-CHECK: ![[THUNK:.*]] = distinct !DISubprogram({{.*}}linkageName: "$s4main8MyObjectC0B3ArrSo7NSArrayCvgTo"
// LOC-CHECK-NOT:                           line:
// LOC-CHECK-SAME:                          DISPFlagDefinition
// LOC-CHECK: ![[DBG]] = !DILocation(line: 0, scope: ![[THUNK]])

// These debug locations should all be in ordered by increasing line number.
// LOC-CHECK: ![[L1]] =
// LOC-CHECK: ![[L2]] =
// LOC-CHECK: ![[L3]] =
// LOC-CHECK: ![[L4]] =
