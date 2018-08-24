//===-- NumpyConversion.swift ---------------------------------*- swift -*-===//
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
// This file defines the `ConvertibleFromNumpyArray` protocol for bridging
// `numpy.ndarray`.
//
//===----------------------------------------------------------------------===//

/// The `numpy` Python module.
/// Note: Global variables are lazy, so the following declaration won't produce
// a Python import error until it is first used.
private let np = Python.import("numpy")

/// A type that can be initialized from a `numpy.ndarray` instance represented
/// as a `PythonObject`.
public protocol ConvertibleFromNumpyArray {
  init?(numpyArray: PythonObject)
}

/// A type that is compatible with a NumPy scalar `dtype`.
public protocol NumpyScalarCompatible {
  /// Whether this type is compatible with the given Numpy scalar `dtype`.
  static func isCompatible(withNumpyScalarType dtype: PythonObject) -> Bool

  /// A single NumPy scalar `dtype` that this type is compatible with.
  static var numpyScalarType: PythonObject { get }
}

extension NumpyScalarCompatible {
  public static func isCompatible(
    withNumpyScalarType dtype: PythonObject
  ) -> Bool {
    switch dtype {
    case numpyScalarType: return true
    default: return false
    }
  }
}

extension Bool : NumpyScalarCompatible {
  public static func isCompatible(
    withNumpyScalarType dtype: PythonObject
  ) -> Bool {
    switch dtype {
    case np.bool_, Python.bool: return true
    default: return false
    }
  }
  public static let numpyScalarType = np.bool_
}

extension UInt8 : NumpyScalarCompatible {
  public static let numpyScalarType = np.uint8
}

extension Int8 : NumpyScalarCompatible {
  public static let numpyScalarType = np.int8
}

extension UInt16 : NumpyScalarCompatible {
  public static let numpyScalarType = np.uint16
}

extension Int16 : NumpyScalarCompatible {
  public static let numpyScalarType = np.int16
}

extension UInt32 : NumpyScalarCompatible {
  public static let numpyScalarType = np.uint32
}

extension Int32 : NumpyScalarCompatible {
  public static let numpyScalarType = np.int32
}

extension UInt64 : NumpyScalarCompatible {
  public static let numpyScalarType = np.uint64
}

extension Int64 : NumpyScalarCompatible {
  public static let numpyScalarType = np.int64
}

extension Float : NumpyScalarCompatible {
  public static let numpyScalarType = np.float32
}

extension Double : NumpyScalarCompatible {
  public static let numpyScalarType = np.float64
}

extension Array : ConvertibleFromNumpyArray
  where Element : NumpyScalarCompatible {
  public init?(numpyArray: PythonObject) {
    // Check if input is a `numpy.ndarray` instance.
    guard Python.isinstance(numpyArray, np.ndarray) == true else {
      return nil
    }
    // Check if the dtype of the `ndarray` is compatible with the `Element`
    // type.
    guard Element.isCompatible(withNumpyScalarType: numpyArray.dtype) else {
      return nil
    }

    // Only 1-D `ndarray` instances can be converted to `Array`.
    let pyShape = numpyArray.__array_interface__["shape"]
    guard let shape = Array<Int>(pyShape) else { return nil }
    guard shape.count == 1 else {
      return nil
    }

    // Make sure that the array is contiguous in memory. This does a copy if
    // the array is not already contiguous in memory.
    let contiguousNumpyArray = np.ascontiguousarray(numpyArray)

    guard let ptrVal =
      UInt(contiguousNumpyArray.__array_interface__["data"].tuple2.0) else {
      return nil
    }
    guard let ptr = UnsafePointer<Element>(bitPattern: ptrVal) else {
      fatalError("numpy.ndarray data pointer was nil")
    }
    // This code avoids constructing and initialize from `UnsafeBufferPointer`
    // because that uses the `init<S : Sequence>(_ elements: S)` initializer,
    // which performs unnecessary copying.
    let dummyPointer = UnsafeMutablePointer<Element>.allocate(capacity: 1)
    let scalarCount = shape.reduce(1, *)
    self.init(repeating: dummyPointer.move(), count: scalarCount)
    dummyPointer.deallocate()
    withUnsafeMutableBufferPointer { buffPtr in
      buffPtr.baseAddress!.assign(from: ptr, count: scalarCount)
    }
  }
}
