// RUN: %target-run-simple-swift | %FileCheck %s
// REQUIRES: executable_test

// REQUIRES: objc_interop

import Foundation
import ObjectiveC

func DoSwizzle<T: AnyObject>(_ c: T.Type) -> T.Type {
    let name = String(utf8String: class_getName(c))!
    let subclass: AnyClass = objc_allocateClassPair(c, "\(name)Subclass", 0)!
    objc_registerClassPair(subclass);
    let subclassSubclass: AnyClass = objc_allocateClassPair(subclass, "\(name)SubclassSubclass", 0)!
    objc_registerClassPair(subclassSubclass);
    return subclassSubclass as! T.Type
}

_ = DoSwizzle(NSArray.self)
print("Swizzled NSArray")
// CHECK: Swizzled NSArray

// Ensure that we can dynamically subclass, instantiate, and destroy Swift
// classes, both NSObject-inheriting and native Swift.
class MySwiftClassToBeSwizzled {
  required init() {}
}

let swiftSubclass = DoSwizzle(MySwiftClassToBeSwizzled.self)
print("Swizzled MySwiftClassToBeSwizzled")
// CHECK: Swizzled MySwiftClassToBeSwizzled
print("Instantiated the subclass: \(swiftSubclass.init())")
// CHECK: Instantiated the subclass:

class MyNSObjectSwiftClassToBeSwizzled: NSObject {
  required override init() {}
}

let swiftNSObjectSubclass = DoSwizzle(MyNSObjectSwiftClassToBeSwizzled.self)
print("Swizzled MyNSObjectSwiftClassToBeSwizzled")
// CHECK: Swizzled MyNSObjectSwiftClassToBeSwizzled
print("Instantiated the subclass: \(swiftNSObjectSubclass.init())")
// CHECK: Instantiated the subclass:
