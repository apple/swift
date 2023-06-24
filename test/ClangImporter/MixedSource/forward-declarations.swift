// RUN: %target-swift-frontend -emit-ir -primary-file %s %S/Inputs/forward-declarations-other.swift -import-objc-header %S/Inputs/forward-declarations.h -enable-objc-interop -disable-objc-attr-requires-foundation-module -module-name main | %FileCheck %s

class Sub: Base {
  // CHECK-LABEL: define{{.*}} void @"$s4main3SubC4testyyF"
  func test() {
    // CHECK: [[SELECTOR:%.+]] = load ptr, ptr @"\01L_selector(getClassInstanceWithoutMentioningItsName)"
    // CHECK: [[RESULT:%.+]] = call ptr @objc_msgSend(ptr %{{[0-9]+}}, ptr [[SELECTOR]])
    // CHECK: call ptr @llvm.objc.retainAutoreleasedReturnValue(ptr [[RESULT]])
    _ = self.getClassInstanceWithoutMentioningItsName()
    // CHECK: call void @swift_release(ptr {{%.+}})
    // CHECK: ret void
  }
}
