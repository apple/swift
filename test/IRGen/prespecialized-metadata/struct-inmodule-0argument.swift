// RUN: %swift -prespecialize-generic-metadata -target %module-target-future -emit-ir %s | %FileCheck %s -DINT=i%target-ptrsize -DALIGNMENT=%target-alignment

// REQUIRES: VENDOR=apple || OS=linux-gnu
// UNSUPPORTED: CPU=i386 && OS=ios
// UNSUPPORTED: CPU=armv7 && OS=ios
// UNSUPPORTED: CPU=armv7s && OS=ios

struct Value {
  let first: Int
}

@inline(never)
func consume<T>(_ t: T) {
  withExtendedLifetime(t) { t in
  }
}

// CHECK: define hidden swiftcc void @"$s4main4doityyF"() #{{[0-9]+}} {
// CHECK:   call swiftcc void @"$s4main7consumeyyxlF"(ptr noalias nocapture %{{[0-9]+}}, ptr getelementptr inbounds (<{ ptr, ptr, [[INT]], ptr, i32{{(, \[4 x i8\])?}}, i64 }>, ptr @"$s4main5ValueVMf", i32 0, i32 2))
// CHECK: }
func doit() {
  consume( Value(first: 13) )
}
doit()
