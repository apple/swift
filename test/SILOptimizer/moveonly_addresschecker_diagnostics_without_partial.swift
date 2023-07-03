// RUN: %target-swift-emit-sil -O -sil-verify-all -verify -enable-experimental-feature NoImplicitCopy -enable-experimental-feature MoveOnlyClasses %s

struct Test: ~Copyable {
  public let baseAddress: UnsafeRawPointer
  public let count: Int
}

extension Test {

  public static func ==(lhs: borrowing Test, rhs: borrowing Test) -> Bool {
    let count = lhs.count
    guard count == rhs.count else { return false }

    if count == 0 { return true } else if lhs.baseAddress == rhs.baseAddress { return true }
    if count == 1 || lhs.baseAddress == rhs.baseAddress { return true }

    return lhs.baseAddress.load(as: UInt8.self) == rhs.baseAddress.load(as: UInt8.self)
  }
}
