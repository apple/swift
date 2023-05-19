// RUN: %target-typecheck-verify-swift -import-objc-header %S/Inputs/issue-56636.h

// REQUIRES: objc_interop

extension Letter: Strideable {
  public func distance(to other: Self) -> RawValue.Stride {
    self.rawValue.distance( to: other.rawValue )
  }

  public func advanced(by n: RawValue.Stride) -> Self {
    Self( rawValue: self.rawValue.advanced( by: n ) )!
  }
}
