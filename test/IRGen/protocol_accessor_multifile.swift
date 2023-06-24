// RUN: %target-swift-frontend %use_no_opaque_pointers -emit-ir -primary-file %s %S/Inputs/protocol_accessor_multifile_other.swift > %t.ll
// RUN: %target-swift-frontend -emit-ir -primary-file %s %S/Inputs/protocol_accessor_multifile_other.swift
// RUN: %FileCheck %s -check-prefix CHECK -check-prefix CHECK-%target-runtime < %t.ll
// RUN: %FileCheck -check-prefix NEGATIVE %s < %t.ll

// CHECK: @"$s27protocol_accessor_multifile5ProtoMp" = external{{( dllimport)?}} global
// NEGATIVE-NOT: @"$s27protocol_accessor_multifile10ClassProtoMp" =

// CHECK-LABEL: define{{.*}} void @"$s27protocol_accessor_multifile14useExistentialyyF"()
func useExistential() {
  // CHECK: [[BOX:%.+]] = alloca %T27protocol_accessor_multifile5ProtoP,
  // CHECK: call swiftcc void @"$s27protocol_accessor_multifile17globalExistentialAA5Proto_pvg"(%T27protocol_accessor_multifile5ProtoP* noalias nocapture sret({{.*}}) [[BOX]])
  // CHECK: call swiftcc void @"$s27protocol_accessor_multifile5ProtoPAAE6methodyyF"
  globalExistential.method()
  // CHECK: [[BITCAST:%.*]] = bitcast %T27protocol_accessor_multifile5ProtoP* [[BOX]] to %__opaque_existential_type_1*
  // CHECK: call void @__swift_destroy_boxed_opaque_existential_1(%__opaque_existential_type_1* [[BITCAST]])
  // CHECK: ret void
}

class GenericContext<T: Proto> {
  // CHECK-LABEL: define{{.*}} void @"$s27protocol_accessor_multifile14GenericContextC04testdE0yyFZ
  static func testGenericContext() {
    // CHECK: [[SELF:%.+]] = bitcast %swift.type* %0 to %swift.type**
    // CHECK: [[WITNESS_TABLE:%.+]] = getelementptr inbounds %swift.type*, %swift.type** [[SELF]],
    // CHECK: = load %swift.type*, %swift.type** [[WITNESS_TABLE]]
    // CHECK: ret void
  }
}

// CHECK-LABEL: define{{.*}} void @"$s27protocol_accessor_multifile19useClassExistentialyyF"()
func useClassExistential() {
  let g = getClassExistential()
  // CHECK-objc: [[G_TYPE:%.+]] = call %swift.type* @swift_getObjectType({{%.+}} {{%.+}})
  // CHECK-native: [[G_TYPE:%.+]] = load %swift.type*
  // CHECK: call swiftcc void {{%.+}}(i{{32|64}} 1, {{%.+}} {{%.+}}, %swift.type* [[G_TYPE]], i8** {{%.+}})
  g?.baseProp = 1
  // CHECK: ret void
}
