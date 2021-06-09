// RUN: %empty-directory(%t)
// RUN: %target-build-swift %s -o %t/a.out
// RUN: %target-codesign %t/a.out
// RUN: %target-run %t/a.out | grep 'check-prefix' > %t/prefix-option
// RUN: %target-run %t/a.out | %FileCheck -check-prefix=CHECK `cat %t/prefix-option` %s
// REQUIRES: executable_test

// REQUIRES: objc_interop

// SR-9838 Disable because it blocks PR testing.
// UNSUPPORTED: CPU=i386

import Foundation

struct Guts {
    var internalValue = 42
    var value: Int {
        get {
            return internalValue
        }
    }
    init(value: Int) {
        internalValue = value
    }
    init() {
    }
}

@objc enum TargetState: Int {
    case whatTarget = 1
    case why = 19
    case deadInside = -42
}

@objc class OptionalStateHolder : NSObject {
    @objc dynamic var objcState: TargetState = .whatTarget
}

class Target : NSObject, NSKeyValueObservingCustomization {
    // This dynamic property is observed by KVO
    @objc dynamic var objcValue: String
    @objc dynamic var objcValue2: String {
        willSet {
            willChangeValue(for: \.objcValue2)
        }
        didSet {
            didChangeValue(for: \.objcValue2)
        }
    }
    @objc dynamic var objcValue3: String

    @objc dynamic var objcState: TargetState
    @objc dynamic var optionalStateHolder: OptionalStateHolder?
    
    // This Swift-typed property causes vtable usage on this class.
    var swiftValue: Guts
    
    override init() {
        self.swiftValue = Guts()
        self.objcValue = ""
        self.objcValue2 = ""
        self.objcValue3 = ""
        self.objcState = .whatTarget
        self.optionalStateHolder = OptionalStateHolder()
        super.init()
    }
    
    static func keyPathsAffectingValue(for key: AnyKeyPath) -> Set<AnyKeyPath> {
        if (key == \Target.objcValue) {
            return [\Target.objcValue2, \Target.objcValue3]
        } else {
            return []
        }
    }
    
    static func automaticallyNotifiesObservers(for key: AnyKeyPath) -> Bool {
        if key == \Target.objcValue2 || key == \Target.objcValue3 {
            return false
        }
        return true
    }
}


class ObserverKVO : NSObject {
    var target: Target?
    var valueObservation: NSKeyValueObservation? = nil
    var stateObservation: NSKeyValueObservation? = nil
    var optionalStateObservation: NSKeyValueObservation? = nil
    
    override init() { target = nil; super.init() }
    
    func observeTarget(_ target: Target) {
        self.target = target
        valueObservation = target.observe(\.objcValue) { (object, change) in
            Swift.print("swiftValue \(object.swiftValue.value), objcValue \(object.objcValue)")
        }
        stateObservation = target.observe(\.objcState, options: [.old, .new]) { (object, change) in
            let oldString = change.oldValue.map { String(describing: $0.rawValue) } ?? "nil"
            let newString = change.newValue.map { String(describing: $0.rawValue) } ?? "nil"
            Swift.print("state \(object.objcState.rawValue), old \(oldString), new \(newString)")
        }
        optionalStateObservation = target.observe(\.optionalStateHolder?.objcState, options: [.old, .new]) { (object, change) in
            let currentStateString = object.optionalStateHolder.map { String(describing: $0.objcState.rawValue) } ?? "nil"
            let oldString = change.oldValue?.map { String(describing: $0.rawValue) } ?? "nil"
            let newString = change.newValue?.map { String(describing: $0.rawValue) } ?? "nil"
            Swift.print("optionalState \(currentStateString), old \(oldString), new \(newString)")
        }
    }
    
    func removeTarget() {
        valueObservation!.invalidate()
        stateObservation!.invalidate()
        optionalStateObservation!.invalidate()
    }
}


var t2 = Target()
var o2 = ObserverKVO()
print("unobserved 2")
t2.objcValue = "one"
t2.objcValue = "two"
print("registering observer 2")
o2.observeTarget(t2)
print("Now witness the firepower of this fully armed and operational panopticon!")
t2.objcValue = "three"
t2.objcValue = "four"
t2.swiftValue = Guts(value: 13)
t2.objcValue2 = "six" //should fire
t2.objcValue3 = "nothing" //should not fire
t2.objcState = .why
t2.objcState = .deadInside
t2.optionalStateHolder?.objcState = .why
t2.optionalStateHolder?.objcState = .deadInside
t2.optionalStateHolder = nil
o2.removeTarget()
t2.objcValue = "five" //make sure that we don't crash or keep posting changes if you deallocate an observation after invalidating it
t2.objcState = .whatTarget
print("target removed")

// CHECK: registering observer 2
// CHECK-NEXT: Now witness the firepower of this fully armed and operational panopticon!
// CHECK-NEXT: swiftValue 42, objcValue three
// CHECK-NEXT: swiftValue 42, objcValue four
// CHECK-NEXT: swiftValue 13, objcValue four
// The next 2 logs are actually a bug and shouldn't happen
// CHECK-NEXT: swiftValue 13, objcValue four
// CHECK-NEXT: swiftValue 13, objcValue four
// CHECK-NEXT: state 19, old 1, new 19
// CHECK-NEXT: state -42, old 19, new -42
// CHECK-NEXT: optionalState 19, old 1, new 19
// CHECK-NEXT: optionalState -42, old 19, new -42
// CHECK-NEXT: optionalState nil, old -42, new nil
// CHECK-NEXT: target removed

//===----------------------------------------------------------------------===//
// Test NSKeyValueObservingCustomization issue with observing from the callbacks
//===----------------------------------------------------------------------===//

// The following tests are only expected to pass when running with the
// Swift 5.1 and later libraries.
if #available(iOS 13, macOS 10.15, tvOS 13, watchOS 6, *) {
  print("-check-prefix=CHECK-51")
} else {
  print("-check-prefix=DONT-CHECK")
  // Need at least one check, otherwise FileCheck will complain.
  // DONT-CHECK: {{.}}
}

class Target2 : NSObject, NSKeyValueObservingCustomization {
    @objc dynamic var name: String?

    class Dummy : NSObject {
        @objc dynamic var name: String?
    }

    // In both of the callbacks, observe another property with the same key path.
    // We do it in both because we're not sure which callback is invoked first.
    // This ensures that using KVO with key paths from one callback doesn't interfere
    // with the ability to look up the key path using the other.
    static func keyPathsAffectingValue(for key: AnyKeyPath) -> Set<AnyKeyPath> {
        print("keyPathsAffectingValue: key == \\.name:", key == \Target2.name)
        withExtendedLifetime(Dummy()) { (dummy) in
			_ = dummy.observe(\.name) { (_, _) in }
		}
        return []
    }

    static func automaticallyNotifiesObservers(for key: AnyKeyPath) -> Bool {
        print("automaticallyNotifiesObservers: key == \\.name:", key == \Target2.name)
        withExtendedLifetime(Dummy()) { (dummy) in
			_ = dummy.observe(\.name) { (_, _) in }
		}
        return true
    }
}

print("registering observer for Target2")
withExtendedLifetime(Target2()) { (target) in
    _ = target.observe(\.name) { (_, _) in }
}
print("observer removed")

// CHECK-51-LABEL: registering observer for Target2
// CHECK-51-DAG: keyPathsAffectingValue: key == \.name: true
// CHECK-51-DAG: automaticallyNotifiesObservers: key == \.name: true
// CHECK-51-NEXT: observer removed

//===----------------------------------------------------------------------===//
// Test NSSortDescriptor keyPath support
//===----------------------------------------------------------------------===//

// This one doesn't really match the context of "KVO KeyPaths" but it's close enough

class Sortable1 : NSObject {
    @objc var name: String?
}

class Sortable2 : NSObject {
    @objc var name: String?
}

print("creating NSSortDescriptor")
let descriptor = NSSortDescriptor(keyPath: \Sortable1.name, ascending: true)
_ = NSSortDescriptor(keyPath: \Sortable2.name, ascending: true)
print("keyPath == \\Sortable1.name:", descriptor.keyPath == \Sortable1.name)

// CHECK-51-LABEL: creating NSSortDescriptor
// CHECK-51-NEXT: keyPath == \Sortable1.name: true

//===----------------------------------------------------------------------===//
// Test keyPath with optional value has correct oldValue/newValue behavior
//===----------------------------------------------------------------------===//

class TestClassForOptionalKeyPath : NSObject {
    
    // Should not use NSObject? as object type
    @objc dynamic var optionalObject: String?

    @objc dynamic var optionalState: OptionalStateHolder?
}

let testObjectForOptionalKeyPath = TestClassForOptionalKeyPath()

print("observe keyPath with optional value")

let optionalKeyPathObserver = testObjectForOptionalKeyPath.observe(\.optionalObject, options: [.initial, .old, .new]) { (_, change) in
    Swift.print("oldValue = \(change.oldValue as String??), newValue = \(change.newValue as String??)")
}

testObjectForOptionalKeyPath.optionalObject = nil
testObjectForOptionalKeyPath.optionalObject = "foo"

let optionalRawRepresentableKeyPathObserver = testObjectForOptionalKeyPath.observe(\.optionalState?.objcState, options: [.initial, .old, .new]) { (_, change) in
    Swift.print("[STATE] oldValue = \(change.oldValue.map { $0?.rawValue } as Int??), newValue = \(change.newValue.map { $0?.rawValue } as Int??)")
}

testObjectForOptionalKeyPath.optionalState = OptionalStateHolder()
testObjectForOptionalKeyPath.optionalState?.objcState = .why
testObjectForOptionalKeyPath.optionalState = nil

// CHECK-51-LABEL: observe keyPath with optional value
// CHECK-51-NEXT: oldValue = nil, newValue = Optional(nil)
// CHECK-51-NEXT: oldValue = Optional(nil), newValue = Optional(nil)
// CHECK-51-NEXT: oldValue = Optional(nil), newValue = Optional(Optional("foo"))
// CHECK-51-NEXT: [STATE] oldValue = nil, newValue = Optional(nil)
// CHECK-51-NEXT: [STATE] oldValue = Optional(nil), newValue = Optional(Optional(1))
// CHECK-51-NEXT: [STATE] oldValue = Optional(Optional(1)), newValue = Optional(Optional(19))
// CHECK-51-NEXT: [STATE] oldValue = Optional(Optional(19)), newValue = Optional(nil)
