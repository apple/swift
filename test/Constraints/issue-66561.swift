// RUN: %target-typecheck-verify-swift

// https://github.com/apple/swift/issues/66561

@propertyWrapper
struct WrapperValue<Value> {
  var value: Value
  init(wrappedValue: Value) {
    self.value = wrappedValue
  }

  var projectedValue: Self {
    return self
  }

  var wrappedValue: Value {
    get {
      self.value
    }
    set {
      self.value = newValue
    }
  }
}

func test() {
  let _ = {
    @WrapperValue var value: Bool = false
    switch value {
    case $value.wrappedValue:
      break
    default:
      break
    }
  }
}
