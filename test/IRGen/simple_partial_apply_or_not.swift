// RUN: %target-swift-emit-ir -Xllvm -sil-disable-pass=Simplification -module-name test %s | %FileCheck %s
// RUN: %target-run-simple-swift -Xllvm -sil-disable-pass=Simplification %s | %FileCheck %s --check-prefix=CHECK-EXEC

// REQUIRES: executable_test

@propertyWrapper
struct State<T> {
  private class Reference {
    var value: T
    init(value: T) { self.value = value }
  }

  private let ref: Reference

  init(wrappedValue: T) {
    ref = Reference(value: wrappedValue)
  }

  var wrappedValue: T {
    get { ref.value }
    nonmutating set { ref.value = newValue }
  }
}

struct S {
  @State var value: Int = 1

  init() {
    value = 10 // CRASH
  }
}

print("Hello!")
let s = S()
print(s)

// We need to call a partial apply thunk instead of directly calling the method
// because the ABI of closure requires swiftself in the context parameter but
// the method of this self type (struct S) does not.

// CHECK: define {{.*}}swiftcc ptr @"$s4test1SVACycfC"()
// CHECK:  [[RES:%.*]] = call swiftcc ptr @"$s4test1SV5valueSivpfP"(i64 1)
// CHECK:  ret ptr [[RES]]

// This used to crash.

// CHECK-EXEC: Hello!
// CHECK-EXEC: S(_value: main.State<Swift.Int>(ref: main.State<Swift.Int>.(unknown context at {{.*}}).Reference))
