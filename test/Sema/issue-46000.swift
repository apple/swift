// RUN: %target-typecheck-verify-swift

// https://github.com/apple/swift/issues/46000

struct DispatchData {
  let ptr: UnsafePointer<UInt8>
  func withUnsafeBytes<Result, ContentType>(body: (UnsafePointer<ContentType>) throws -> Result) rethrows -> Result { try body(ptr as! UnsafePointer<ContentType>) }
}
struct Data {}

extension DispatchData {
  func asFoundationData<T>(execute: (Data) throws -> T) rethrows -> T {
    return try withUnsafeBytes { (ptr: UnsafePointer<Int8>) -> Void in
      // expected-error@-1 {{cannot convert return expression of type 'Void' to return type 'T'}}
      let data = Data()
      return try execute(data) // expected-error {{cannot convert value of type 'T' to closure result type 'Void'}}
    }
  }
}
