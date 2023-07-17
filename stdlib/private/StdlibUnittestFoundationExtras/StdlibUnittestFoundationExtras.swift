//===----------------------------------------------------------------------===//
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

import ObjectiveC
import Foundation
import StdlibUnittest

/// Executes the `body` in an autorelease pool if the platform does not
/// implement the return-autoreleased optimization.
///
/// (Currently, only the i386 iOS and watchOS simulators don't implement the
/// return-autoreleased optimization.)
@inline(never)
public func autoreleasepoolIfUnoptimizedReturnAutoreleased(
  invoking body: () -> Void
) {
#if targetEnvironment(simulator) && arch(i386) && (os(iOS) || os(watchOS))
  autoreleasepool(invoking: body)
#else
  body()
#endif
}

@usableFromInline
@_silgen_name("NSArray_getObjects")
func NSArray_getObjects(
  nsArray: AnyObject,
  objects: AutoreleasingUnsafeMutablePointer<AnyObject?>?,
  rangeLocation: Int,
  rangeLength: Int)

extension NSArray {
  @nonobjc // FIXME: there should be no need in this attribute.
  public func available_getObjects(
    _ objects: AutoreleasingUnsafeMutablePointer<AnyObject?>?, range: NSRange
  ) {
    return NSArray_getObjects(
      nsArray: self,
      objects: objects,
      rangeLocation: range.location,
      rangeLength: range.length)
  }
}

@_silgen_name("NSDictionary_getObjectsAndKeysWithCount")
func NSDictionary_getObjectsAndKeysWithCount(
  nsDictionary: NSDictionary,
  objects: AutoreleasingUnsafeMutablePointer<AnyObject?>?,
  andKeys keys: AutoreleasingUnsafeMutablePointer<AnyObject?>?,
  count: Int
)

extension NSDictionary {
  @nonobjc // FIXME: there should be no need in this attribute.
  public func available_getObjects(
    _ objects: AutoreleasingUnsafeMutablePointer<AnyObject?>?,
    andKeys keys: AutoreleasingUnsafeMutablePointer<AnyObject?>?,
    count: Int
  ) {
    return NSDictionary_getObjectsAndKeysWithCount(
      nsDictionary: self,
      objects: objects,
      andKeys: keys,
      count: count)
  }
}

public func expectBridgeToNSValue<T>(_ value: T,
                                     nsValueInitializer: ((T) -> NSValue)? = nil,
                                     nsValueGetter: ((NSValue) -> T)? = nil,
                                     equal: (T, T) -> Bool) {
  let object = value as AnyObject
  let nsValue = object as! NSValue
  if let nsValueInitializer = nsValueInitializer {
    expectEqual(nsValueInitializer(value), nsValue)
  }
  if let nsValueGetter = nsValueGetter {
    expectTrue(equal(value, nsValueGetter(nsValue)))
  }
  if let nsValueInitializer = nsValueInitializer,
     let nsValueGetter = nsValueGetter {
    expectTrue(equal(value, nsValueGetter(nsValueInitializer(value))))
  }
  expectTrue(equal(value, object as! T))

}

