// RUN: %target-typecheck-verify-swift

// https://github.com/apple/swift/issues/59058

struct MPSGraphTensor { }

struct MPSGraph {
  func addition(
    _ primaryTensor: MPSGraphTensor,
    _ secondaryTensor: MPSGraphTensor,
    name: String?
  ) {

  }
}

struct Tensor<T> {} // expected-note {{type declared here}}

struct _ExecutionContext {
  func performBinaryOp<T>(
    _ lhs: Tensor<T>,
    _ rhs: Tensor<T>,
    _ op: (MPSGraphTensor, MPSGraphTensor, String?) -> MPSGraphTensor
  ) -> Tensor<T> {
    fatalError()
  }
}

public enum _RawTFEager {
  public static func addV2<T>( // expected-error {{method cannot be declared public because its parameter uses an internal type}}
    _ x: Tensor<T>
  ) -> Tensor<T> {
    return _ExecutionContext.performBinaryOp(x, x, MPSGraph.addition)
    // expected-error@-1 {{instance member 'performBinaryOp' of type '_ExecutionContext' cannot be used in static context; did you mean to use a value of this type instead?}}
    // expected-error@-2 {{instance member 'addition' of type 'MPSGraph' cannot be used in static context; did you mean to use a value of this type instead?}}
    // expected-error@-3 {{cannot convert value of type '(MPSGraphTensor, MPSGraphTensor, String?) -> ()' to expected argument type '(MPSGraphTensor, MPSGraphTensor, String?) -> MPSGraphTensor'}}
  }
}
