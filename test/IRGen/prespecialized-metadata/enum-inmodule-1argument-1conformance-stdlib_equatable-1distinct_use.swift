// RUN: %swift -prespecialize-generic-metadata -target %module-target-future -emit-ir %s | %FileCheck %s -DINT=i%target-ptrsize -DALIGNMENT=%target-alignment

// REQUIRES: VENDOR=apple || OS=linux-gnu
// UNSUPPORTED: CPU=i386 && OS=ios
// UNSUPPORTED: CPU=armv7 && OS=ios
// UNSUPPORTED: CPU=armv7s && OS=ios

// CHECK-NOT: @"$s4main5ValueOyAA7IntegerVGMf"
enum Value<First : Equatable> {
  case first(First)
}

struct Integer : Equatable {
  let value: Int
}

@inline(never)
func consume<T>(_ t: T) {
  withExtendedLifetime(t) { t in
  }
}

func doit() {
  consume( Value.first(Integer(value: 13)) )
}
doit()

// CHECK: ; Function Attrs: noinline nounwind readnone
// CHECK: define hidden swiftcc %swift.metadata_response @"$s4main5ValueOMa"([[INT]] %0, ptr %1, ptr %2) #{{[0-9]+}} {{(section)?.*}}{
// CHECK:      call swiftcc %swift.metadata_response @__swift_instantiateGenericMetadata(
// CHECK-SAME:     [[INT]] %0, 
// CHECK-SAME:     ptr %1, 
// CHECK-SAME:     ptr %2, 
// CHECK-SAME:     ptr undef, 
// CHECK-SAME:     $s4main5ValueOMn
// CHECK:   ret %swift.metadata_response {{%[0-9]+}}
// CHECK: }
