// RUN: %empty-directory(%t)

// RUN: %target-build-swift -emit-library -module-name TestModule -module-link-name TestModule %S/Inputs/class-open-0argument.swift -emit-module-interface -swift-version 5 -o %t/%target-library-name(TestModule) -enable-library-evolution
// RUN: %target-swift-frontend -prespecialize-generic-metadata -target %module-target-future -emit-ir -I %t -L %t %s | %FileCheck %s -DINT=i%target-ptrsize -DALIGNMENT=%target-alignment --check-prefix=CHECK --check-prefix=CHECK-%target-vendor

// REQUIRES: VENDOR=apple || OS=linux-gnu
// UNSUPPORTED: CPU=i386 && OS=ios
// UNSUPPORTED: CPU=armv7 && OS=ios
// UNSUPPORTED: CPU=armv7s && OS=ios

import TestModule

// CHECK-NOT: @"$s4main5Value{{[A-Za-z_0-9]+}}LLCySiGMf" =

fileprivate class Value<First> : Ancestor1 {
  let first_Value: First

  init(first: First) {
    self.first_Value = first
    super.init(32)
  }
}

@inline(never)
func consume<T>(_ t: T) {
  withExtendedLifetime(t) { t in
  }
}

func doit() {
  consume( Value(first: 13) )
}
doit()

// CHECK-LABEL: define hidden swiftcc void @"$s4main4doityyF"()
//       CHECK:   call ptr @__swift_instantiateConcreteTypeFromMangledName(ptr @"$s4main5Value{{[A-Za-z_0-9]+}}LLCySiGMD")
//       CHECK: }
