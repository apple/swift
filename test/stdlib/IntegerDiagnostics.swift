// RUN: %target-typecheck-verify-swift -swift-version 4

func signedBitPattern() {
  _ = Int64(bitPattern: 0.0) // expected-error {{Please use Int64(bitPattern: UInt64) in combination with Double.bitPattern property.}}
  _ = Int32(bitPattern: 0.0) // expected-error {{Please use Int32(bitPattern: UInt32) in combination with Float.bitPattern property.}}
}

func bitWidthConstants() {
  // Int64 has the same size everywhere.
  if Int64.bitWidth == 32 { // expected-note {{condition always evaluates to false}}
    print("1") // expected-warning {{will never be executed}}
  }
  // The size of Int is variable across platforms, so checking it should not
  // lead to spurious warnings about dead code.
  if Int.bitWidth == 32 {
    print("2") // no warning expected
  }
}
