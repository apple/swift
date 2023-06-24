// RUN: %empty-directory(%t)

// RUN: %target-swift-frontend -prespecialize-generic-metadata -target %module-target-future -emit-ir -I %t -L %t %s | %FileCheck %s -DINT=i%target-ptrsize -DALIGNMENT=%target-alignment --check-prefix=CHECK --check-prefix=CHECK-%target-vendor

// REQUIRES: VENDOR=apple || OS=linux-gnu
// UNSUPPORTED: CPU=i386 && OS=ios
// UNSUPPORTED: CPU=armv7 && OS=ios
// UNSUPPORTED: CPU=armv7s && OS=ios

fileprivate class Ancestor1<First> {
  let first_Ancestor1: First

  init(first: First) {
    self.first_Ancestor1 = first
  }
}

fileprivate class Ancestor2 : Ancestor1<Int> {
  let first_Ancestor2: Int

  override init(first: Int) {
    self.first_Ancestor2 = first
    super.init(first: first)
  }
}

fileprivate class Value<First> : Ancestor2 {
  let first_Value: First

  init(first: First) {
    self.first_Value = first
    super.init(first: 7373748)
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

// TODO: Prespecialize Value<Int>.

//        CHECK-LABEL: define hidden swiftcc void @"$s4main4doityyF"()
//              CHECK:   call ptr @__swift_instantiateConcreteTypeFromMangledName(ptr @"$s4main5Value{{[A-Za-z_0-9]+}}LLCySiGMD")
//              CHECK: }
