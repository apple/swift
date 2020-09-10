// RUN: %target-run-simple-swift | %FileCheck %s
// REQUIRES: executable_test

// REQUIRES: swift_stable_abi

protocol Observed: AnyObject {
  func broadcastValueWillChange<T>(newValue: T)
}

struct Other<Value: Equatable> {
  var value: Value

  func hello() -> String {
    return "Hello from \(value)"
  }
}

@propertyWrapper
struct Observable<Value: Equatable> {
  private var stored: Value

  
  init(wrappedValue: Value) {
    self.stored = wrappedValue
  }

  var wrappedValue: Value {
    get { fatalError("called wrappedValue getter") }
    set { fatalError("called wrappedValue setter") }
  }

  var projectedValue: Other<Value> {
    get { fatalError("called projectedValue getter") }
    set { fatalError("called projectedValue setter") }
  }
  
  static subscript<EnclosingSelf: Observed, FinalValue>(
      _enclosingInstance observed: EnclosingSelf,
      wrapped wrappedKeyPath: ReferenceWritableKeyPath<EnclosingSelf, FinalValue>,
      storage storageKeyPath: ReferenceWritableKeyPath<EnclosingSelf, Self>
    ) -> Value {
    get {
      observed[keyPath: storageKeyPath].stored
    }
    set {
      if observed[keyPath: storageKeyPath].stored != newValue {
        observed.broadcastValueWillChange(newValue: newValue)
      }
      
      observed[keyPath: storageKeyPath].stored = newValue
    }
  }

  static subscript<EnclosingSelf: Observed>(
      _enclosingInstance observed: EnclosingSelf,
      projected wrappedKeyPath: ReferenceWritableKeyPath<EnclosingSelf, Other<Value>>,
      storage storageKeyPath: ReferenceWritableKeyPath<EnclosingSelf, Self>
    ) -> Other<Value> {
    get {
      Other(value: observed[keyPath: storageKeyPath].stored)
    }
    set {
    }
  }
}

class MyType<T: Equatable>: Observed {
  @Observable var x: T

  init(x: T) {
    self.x = x
  }
  
  func broadcastValueWillChange<T>(newValue: T) {
    print("Value of 'x' is changing from \(x) to \(newValue)")
    print($x.hello())
  }
}

func testMyType(_ myType: MyType<Int>) {
  // CHECK: Value of 'x' is changing from 17 to 42
  // CHECK-NEXT: Hello from 17
  myType.x = 42

  // CHECK-NEXT: Value of 'x' is changing from 42 to 25
  // CHECK-NEXT: Hello from 42
  myType.x = 42
  myType.x = 25
}

testMyType(MyType(x: 17))
