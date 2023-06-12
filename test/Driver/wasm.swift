// RUN: %swiftc_driver -driver-print-jobs -target wasm32-unknown-wasi -resource-dir %S/Inputs/fake-resource-dir/lib/swift_static -v %s 2>&1 | %FileCheck %s -check-prefix=CHECK-WASM

// CHECK-WASM: swift{{.*}} -frontend -c -primary-file {{.*}} -target wasm32-unknown-wasi -disable-objc-interop
// CHECK-WASM: clang{{.*}} -target wasm32-unknown-wasi {{.*}}static-executable-args.lnk{{.*}}-v {{.*}}-o
