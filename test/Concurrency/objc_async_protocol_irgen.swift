// RUN: %target-swift-frontend(mock-sdk: %clang-importer-sdk)  -disable-availability-checking -target %target-swift-abi-5.8-triple -import-objc-header %S/Inputs/Delegate.h %s -emit-ir -o - | %FileCheck %s -DALIGNMENT=%target-alignment --check-prefix=CHECK-%is-darwin
// REQUIRES: concurrency
// REQUIRES: objc_interop


let anyObject: AnyObject = (MyAsyncProtocol.self as AnyObject) // or something like this

// rdar://76192003
// Make sure we don't emit 2 copies of methods, due to a completion-handler
// version and another due to an async based version.

// CHECK-isNotDarwin-LABEL: @_PROTOCOL_INSTANCE_METHODS_MyAsyncProtocol = weak hidden constant
// CHECK-isNotDarwin-SAME: selector_data(myAsyncMethod:)
// CHECK-isNotDarwin-NOT: selector_data(myAsyncMethod:)
// CHECK-isNotDarwin-SAME: align [[ALIGNMENT]]


// CHECK-isDarwin: @OBJC_METH_VAR_NAME_ = private unnamed_addr constant [15 x i8] c"myAsyncMethod:\00"
// CHECK-isDarwin: @"_OBJC_$_PROTOCOL_INSTANCE_METHODS_MyAsyncProtocol" =
// CHECK-isDarwin-SAME: [1 x %struct._objc_method]
// CHECK-isDarwin-SAME: @OBJC_METH_VAR_NAME_
// CHECK-isDarwin-SAME: align [[ALIGNMENT]]
