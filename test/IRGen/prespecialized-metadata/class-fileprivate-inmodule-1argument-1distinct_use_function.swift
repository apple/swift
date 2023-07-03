// RUN: %swift -prespecialize-generic-metadata -target %module-target-future -emit-ir %s | %FileCheck %s -DINT=i%target-ptrsize -DALIGNMENT=%target-alignment --check-prefix=CHECK --check-prefix=CHECK-%target-vendor

// REQUIRES: VENDOR=apple || OS=linux-gnu
// UNSUPPORTED: CPU=i386 && OS=ios
// UNSUPPORTED: CPU=armv7 && OS=ios
// UNSUPPORTED: CPU=armv7s && OS=ios

fileprivate class Value<First> {
  let first_Value: First

  init(first: First) {
    self.first_Value = first
  }
}

@inline(never)
func consume<T>(_ t: T) {
  withExtendedLifetime(t) { t in
  }
}

// TODO: Once metadata records for structural types are prespecialized, there
//       should be no call to __swift_instantiateConcreteTypeFromMangledName.

//      CHECK: define hidden swiftcc void @"$s4main4doityyF"() #{{[0-9]+}} {
//      CHECK:   [[METADATA:%[0-9]+]] = call ptr @__swift_instantiateConcreteTypeFromMangledName(
// CHECK-SAME:     @"$s4main5Value[[UNIQUE_ID_1:[A-Za-z0-9_]+]]LLCySSSicGMD"
//      CHECK:   {{%[0-9]+}} = call swiftcc ptr @"$s4main5Value[[UNIQUE_ID_1]]LLC5firstADyxGx_tcfC"(
// CHECK-SAME:     ptr noalias nocapture {{%[0-9]+}}, 
// CHECK-SAME:     ptr swiftself [[METADATA]]
// CHECK-SAME:   )
// CHECK:   call swiftcc void @"$s4main7consumeyyxlF"(
// CHECK-SAME:     ptr noalias nocapture {{%[0-9]+}}, 
// CHECK-SAME:     ptr [[METADATA]])
// CHECK: }
func doit() {
  consume( Value(first: { (i: Int) -> String in fatalError() }) )
}
doit()
