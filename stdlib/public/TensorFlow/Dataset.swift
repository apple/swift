//===-- Dataset.swift -----------------------------------------*- swift -*-===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2017 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//
//
// The dataset API.
//
//===----------------------------------------------------------------------===//

/// Represents a potentially large set of elements.
///
/// A `SingleValueDataset` can be used to represent an input pipeline as a
/// collection of element tensors.
@_fixed_layout
public struct SingleValueDataset {
  // NOTE: SingleValueDataset is currently specialzed on `Float` to work around
  // deabstraction bug SR-8463. This is to avoid indirect passing in SIL. When
  // the bug gets resolved, `SingleValueDataset` will have generic parameter
  // `Scalar`.
  public typealias Scalar = Float
  @usableFromInline let _handle: VariantHandle
  public let elementShape: TensorShape

  @usableFromInline @inline(__always)
  internal init(_handle: VariantHandle, elementShape: TensorShape) {
    self._handle = _handle
    self.elementShape = elementShape
  }
}

public extension SingleValueDataset {
  /// Creates a dataset from a batch of elements as a tensor.
  ///
  /// - Parameter
  ///   - elements: The batch of elements.
  ///   - elementShape: The shape that the 
  ///
  // FIXME: Due to inability to perform shape inference during deabstraction,
  // users must specify the element shape for today.
  @inlinable @inline(__always)
  public init(elements: Tensor<Scalar>, elementShape: TensorShape) {
    self.init(
      _handle: #tfop(
        "TensorSliceDataset", [elements],
        Toutput_types: [Scalar.self],
        output_shapes: [elementShape]
      ),
      elementShape: elementShape
    )
  }
}

extension SingleValueDataset : Sequence {
  public typealias Element = Tensor<Scalar>

  public typealias Iterator = SingleValueDatasetIterator

  /// Returns an iterator over the elements of this dataset.
  @inlinable @inline(__always)
  public func makeIterator() -> SingleValueDatasetIterator {
    let resource: ResourceHandle = #tfop("AnonymousIterator",
                                         output_types: [Scalar.self],
                                         output_shapes: [elementShape])
    #tfop("MakeIterator", _handle, resource) as Void
    return SingleValueDatasetIterator(_handle: resource,
                                      elementShape: elementShape)
  }
}

public extension SingleValueDataset {
  @inlinable @inline(__always)
  func map<T>(
    _ transform: @convention(tensorflow) (Tensor<Scalar>) -> Tensor<T>
  ) -> SingleValueDataset {
    return SingleValueDataset(
      _handle: #tfop(
        "MapDataset", _handle, [], f: transform, Targuments: [],
        output_types: [Scalar.self], output_shapes: [elementShape]
      ),
      elementShape: elementShape
    )
  }

  @inlinable @inline(__always)
  func filter(
    _ isIncluded: @convention(tensorflow) (Tensor<Scalar>) -> Tensor<Bool>
  ) -> SingleValueDataset {
    return SingleValueDataset(
      _handle: #tfop(
        "FilterDataset", _handle, [], predicate: isIncluded, Targuments: [],
        output_types: [Scalar.self], output_shapes: [elementShape]
      ),
      elementShape: elementShape
    )
  }
}

/// Represents the state of iterating through a `SingleValueDataset`.
@_fixed_layout
public struct SingleValueDatasetIterator {
  // NOTE: SingleValueDataset is currently specialzed on `Float` to work around
  // deabstraction bug SR-8463. This is to avoid indirect passing in SIL. When
  // the bug gets resolved, `SingleValueDataset` will have generic parameter
  // `Scalar`.
  public typealias Scalar = Float
  @usableFromInline let handle: ResourceHandle
  public let elementShape: TensorShape

  @usableFromInline @inline(__always)
  internal init(_handle: ResourceHandle, elementShape: TensorShape) {
    self.handle = _handle
    self.elementShape = elementShape
  }
}

extension SingleValueDatasetIterator : IteratorProtocol {
  public typealias Element = Tensor<Scalar>

  // Advances to the next element and returns it, or nil if no next element
  // exists.
  @inlinable @inline(__always)
  // FIXME: Make this `mutating` when SR-8463 is fixed.
  public /*mutating*/ func next() -> Tensor<Scalar>? {
    let optional: VariantHandle =
      #tfop("IteratorGetNextAsOptional", handle,
            output_types: [Scalar.self], output_shapes: [elementShape])
    guard _TFGetScalarOrDie(#tfop("OptionalHasValue", optional)) else {
      return nil
    }
    return Tensor(handle: #tfop("OptionalGetValue", optional,
                                output_types: [Scalar.self],
                                output_shapes: [elementShape]))
  }
}
