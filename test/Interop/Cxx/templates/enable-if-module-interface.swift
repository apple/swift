// RUN: %target-swift-ide-test -print-module -module-to-print=EnableIf -I %S/Inputs -source-filename=x -enable-experimental-cxx-interop | %FileCheck %s

// The `init<T>` constructor template is not yet supported in Swift.

// CHECK: struct HasConstructorWithEnableIf {
// CHECK-NEXT:  @available(*, deprecated, message: "This zero-initializes the backing memory of the struct, which is unsafe for some C++ structs. Consider adding an explicit default initializer for this C++ struct.")
// CHECK-NEXT:  init()
// CHECK-NEXT:}

// CHECK: struct HasConstructorWithEnableIfUsed {
// CHECK-NEXT:  init<T, U>(_: T, _: U)
