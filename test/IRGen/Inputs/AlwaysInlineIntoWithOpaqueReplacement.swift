public protocol P {
}

extension Int : P {
}

extension Double : P {
}

@usableFromInline
func usableFromInline() -> some P {
  return 3
}
