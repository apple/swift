// RUN: %target-swift-frontend -emit-ir -primary-file %s %S/Inputs/protocol_accessor_multifile_other.swift > %t.ll
// RUN: %FileCheck %s -check-prefix CHECK -check-prefix CHECK-%target-runtime < %t.ll
// RUN: %FileCheck -check-prefix NEGATIVE %s < %t.ll

// CHECK: @"$s27protocol_accessor_multifile5ProtoMp" = external{{( dllimport)?}} global
// NEGATIVE-NOT: @"$s27protocol_accessor_multifile10ClassProtoMp" =

// CHECK-LABEL: define{{.*}} void @"$s27protocol_accessor_multifile14useExistentialyyF"()
func useExistential() {
  // CHECK: [[BOX:%.+]] = alloca %T27protocol_accessor_multifile5ProtoP,
  // CHECK: call swiftcc void @"$s27protocol_accessor_multifile17globalExistentialAA5Proto_pvg"(ptr noalias nocapture sret({{.*}}) [[BOX]])
  // CHECK: call swiftcc void @"$s27protocol_accessor_multifile5ProtoPAAE6methodyyF"
  globalExistential.method()
  // CHECK: call void @__swift_destroy_boxed_opaque_existential_1(ptr [[BOX]])
  // CHECK: ret void
}

class GenericContext<T: Proto> {
  // CHECK-LABEL: define{{.*}} void @"$s27protocol_accessor_multifile14GenericContextC04testdE0yyFZ
  static func testGenericContext() {
    // CHECK: [[WITNESS_TABLE:%.+]] = getelementptr inbounds ptr, ptr %0,
    // CHECK: = load ptr, ptr [[WITNESS_TABLE]]
    // CHECK: ret void
  }
}

// CHECK-LABEL: define{{.*}} void @"$s27protocol_accessor_multifile19useClassExistentialyyF"()
func useClassExistential() {
  let g = getClassExistential()
  // CHECK-objc: [[G_TYPE:%.+]] = call ptr @swift_getObjectType(ptr {{%.+}})
  // CHECK-native: [[G_TYPE:%.+]] = load ptr
  // CHECK: call swiftcc void {{%.+}}(i{{32|64}} 1, ptr swiftself {{%.+}}, ptr [[G_TYPE]], ptr {{%.+}})
  g?.baseProp = 1
  // CHECK: ret void
}
