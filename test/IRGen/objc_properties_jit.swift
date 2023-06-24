// RUN: %target-swift-frontend(mock-sdk: %clang-importer-sdk) -enable-objc-interop %s -emit-ir -disable-objc-attr-requires-foundation-module -use-jit | %FileCheck %s

import Foundation

extension NSString {
  @objc class var classProp: Int {
    get { fatalError() }
    set { fatalError() }
  }
  @objc var instanceProp: Int {
    get { fatalError() }
    set { fatalError() }
  }
}

// CHECK-LABEL: define{{( dllexport)?}}{{( protected)?}} private void @runtime_registration
// CHECK:         [[NSOBJECT_UNINIT:%.*]] = load ptr, ptr @"OBJC_CLASS_REF_$_NSString"
// CHECK:         [[NSOBJECT:%.*]] = call ptr @{{.*}}(ptr [[NSOBJECT_UNINIT]])
// CHECK:         [[GET_CLASS_PROP:%.*]] = call ptr @sel_registerName({{.*}}(classProp)
// CHECK:         call ptr @class_replaceMethod(ptr @"OBJC_METACLASS_$_NSString", ptr [[GET_CLASS_PROP]]
// CHECK:         [[SET_CLASS_PROP:%.*]] = call ptr @sel_registerName({{.*}}(setClassProp:)
// CHECK:         call ptr @class_replaceMethod(ptr @"OBJC_METACLASS_$_NSString", ptr [[SET_CLASS_PROP]]
// CHECK:         [[GET_INSTANCE_PROP:%.*]] = call ptr @sel_registerName({{.*}}(instanceProp)
// CHECK:         call ptr @class_replaceMethod(ptr [[NSOBJECT]], ptr [[GET_INSTANCE_PROP]]
// CHECK:         [[SET_INSTANCE_PROP:%.*]] = call ptr @sel_registerName({{.*}}(setInstanceProp:)
// CHECK:         call ptr @class_replaceMethod(ptr [[NSOBJECT]], ptr [[SET_INSTANCE_PROP]]
