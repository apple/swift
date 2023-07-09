// RUN: %target-swift-ide-test -print-module -module-to-print=ManySpecializations -I %S/Inputs -source-filename=x -enable-experimental-cxx-interop | %FileCheck %s


// CHECK: @available(*, unavailable, message: "Un-specialized class templates are not currently supported. Please use a specialization of this type.")
// CHECK-NEXT: struct TemplateStruct<T> {
// CHECK-NEXT: }

// CHECK-NOT: typealias
