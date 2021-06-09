//===----------------------------------------------------------------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2017 - 2017 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

@_exported import Foundation // Clang module
import ObjectiveC
@_implementationOnly import _SwiftFoundationOverlayShims

// This exists to allow for dynamic dispatch on KVO methods added to NSObject.
// Extending NSObject with these methods would disallow overrides.
public protocol _KeyValueCodingAndObserving {}
extension NSObject : _KeyValueCodingAndObserving {}

public struct NSKeyValueObservedChange<Value> {
    public typealias Kind = NSKeyValueChange
    public let kind: Kind
    ///newValue and oldValue will only be non-nil if .new/.old is passed to `observe()`. In general, get the most up to date value by accessing it directly on the observed object instead.
    public let newValue: Value?
    public let oldValue: Value?
    ///indexes will be nil unless the observed KeyPath refers to an ordered to-many property
    public let indexes: IndexSet?
    ///'isPrior' will be true if this change observation is being sent before the change happens, due to .prior being passed to `observe()`
    public let isPrior:Bool
}

///Conforming to NSKeyValueObservingCustomization is not required to use Key-Value Observing. Provide an implementation of these functions if you need to disable auto-notifying for a key, or add dependent keys
public protocol NSKeyValueObservingCustomization : NSObjectProtocol {
    static func keyPathsAffectingValue(for key: AnyKeyPath) -> Set<AnyKeyPath>
    static func automaticallyNotifiesObservers(for key: AnyKeyPath) -> Bool
}

private extension NSObject {
    
    @objc class func __old_unswizzled_automaticallyNotifiesObservers(forKey key: String?) -> Bool {
        fatalError("Should never be reached")
    }
    
    @objc class func __old_unswizzled_keyPathsForValuesAffectingValue(forKey key: String?) -> Set<String> {
        fatalError("Should never be reached")
    }

}

// NOTE: older overlays called this _KVOKeyPathBridgeMachinery. The two
// must coexist, so it was renamed. The old name must not be used in the
// new runtime.
@objc private class __KVOKeyPathBridgeMachinery : NSObject {
    @nonobjc static let swizzler: () = {
        /*
         Move all our methods into place. We want the following:
         __KVOKeyPathBridgeMachinery's automaticallyNotifiesObserversForKey:, and keyPathsForValuesAffectingValueForKey: methods replaces NSObject's versions of them
         NSObject's automaticallyNotifiesObserversForKey:, and keyPathsForValuesAffectingValueForKey: methods replace NSObject's __old_unswizzled_* methods
         NSObject's _old_unswizzled_* methods replace __KVOKeyPathBridgeMachinery's methods, and are never invoked
         */
        threeWaySwizzle(#selector(NSObject.keyPathsForValuesAffectingValue(forKey:)), with: #selector(NSObject.__old_unswizzled_keyPathsForValuesAffectingValue(forKey:)))
        threeWaySwizzle(#selector(NSObject.automaticallyNotifiesObservers(forKey:)), with: #selector(NSObject.__old_unswizzled_automaticallyNotifiesObservers(forKey:)))
    }()
    
    /// Performs a 3-way swizzle between `NSObject` and `__KVOKeyPathBridgeMachinery`.
    ///
    /// The end result of this swizzle is the following:
    /// * `NSObject.selector` contains the IMP from `__KVOKeyPathBridgeMachinery.selector`
    /// * `NSObject.unswizzledSelector` contains the IMP from the original `NSObject.selector`.
    /// * __KVOKeyPathBridgeMachinery.selector` contains the (useless) IMP from `NSObject.unswizzledSelector`.
    ///
    /// This swizzle is done in a manner that modifies `NSObject.selector` last, in order to ensure thread safety
    /// (by the time `NSObject.selector` is swizzled, `NSObject.unswizzledSelector` will contain the original IMP)
    @nonobjc private static func threeWaySwizzle(_ selector: Selector, with unswizzledSelector: Selector) {
        let rootClass: AnyClass = NSObject.self
        let bridgeClass: AnyClass = __KVOKeyPathBridgeMachinery.self
        
        // Swap bridge.selector <-> NSObject.unswizzledSelector
        let unswizzledMethod = class_getClassMethod(rootClass, unswizzledSelector)!
        let bridgeMethod = class_getClassMethod(bridgeClass, selector)!
        method_exchangeImplementations(unswizzledMethod, bridgeMethod)
        
        // Swap NSObject.selector <-> NSObject.unswizzledSelector
        // NSObject.unswizzledSelector at this point contains the bridge IMP
        let rootMethod = class_getClassMethod(rootClass, selector)!
        method_exchangeImplementations(rootMethod, unswizzledMethod)
    }

    private class BridgeKey : NSObject, NSCopying {
        let value: String

        init(_ value: String) {
            self.value = value
        }

        func copy(with zone: NSZone? = nil) -> Any {
            return self
        }

        override func isEqual(_ object: Any?) -> Bool {
            return value == (object as? BridgeKey)?.value
        }

        override var hash: Int {
            var hasher = Hasher()
            hasher.combine(ObjectIdentifier(BridgeKey.self))
            hasher.combine(value)
            return hasher.finalize()
        }
    }

    /// Temporarily maps a `String` to an `AnyKeyPath` that can be retrieved with `_bridgeKeyPath(_:)`.
    ///
    /// This uses a per-thread storage so key paths on other threads don't interfere.
    @nonobjc static func _withBridgeableKeyPath(from keyPathString: String, to keyPath: AnyKeyPath, block: () -> Void) {
        _ = __KVOKeyPathBridgeMachinery.swizzler
        let key = BridgeKey(keyPathString)
        let oldValue = Thread.current.threadDictionary[key]
        Thread.current.threadDictionary[key] = keyPath
        defer { Thread.current.threadDictionary[key] = oldValue }
        block()
    }
    
    @nonobjc static func _bridgeKeyPath(_ keyPath:String?) -> AnyKeyPath? {
        guard let keyPath = keyPath else { return nil }
        return Thread.current.threadDictionary[BridgeKey(keyPath)] as? AnyKeyPath
    }
    
    @objc override class func automaticallyNotifiesObservers(forKey key: String) -> Bool {
        //This is swizzled so that it's -[NSObject automaticallyNotifiesObserversForKey:]
        if let customizingSelf = self as? NSKeyValueObservingCustomization.Type, let path = __KVOKeyPathBridgeMachinery._bridgeKeyPath(key) {
            return customizingSelf.automaticallyNotifiesObservers(for: path)
        } else {
            return self.__old_unswizzled_automaticallyNotifiesObservers(forKey: key) //swizzled to be NSObject's original implementation
        }
    }
    
    @objc override class func keyPathsForValuesAffectingValue(forKey key: String?) -> Set<String> {
        //This is swizzled so that it's -[NSObject keyPathsForValuesAffectingValueForKey:]
        if let customizingSelf = self as? NSKeyValueObservingCustomization.Type, let path = __KVOKeyPathBridgeMachinery._bridgeKeyPath(key!) {
            let resultSet = customizingSelf.keyPathsAffectingValue(for: path)
            return Set(resultSet.lazy.map {
                guard let str = $0._kvcKeyPathString else { fatalError("Could not extract a String from KeyPath \($0)") }
                return str
            })
        } else {
            return self.__old_unswizzled_keyPathsForValuesAffectingValue(forKey: key) //swizzled to be NSObject's original implementation
        }
    }
}

func _bridgeKeyPathToString(_ keyPath:AnyKeyPath) -> String {
    guard let keyPathString = keyPath._kvcKeyPathString else { fatalError("Could not extract a String from KeyPath \(keyPath)") }
    return keyPathString
}

// NOTE: older overlays called this NSKeyValueObservation. We now use
// that name in the source code, but add an underscore to the runtime
// name to avoid conflicts when both are loaded into the same process.
@objc(_NSKeyValueObservation)
public class NSKeyValueObservation : NSObject {
    // We use a private helper class as the actual observer. This lets us attach the helper as an associated object
    // to the object we're observing, thus ensuring the helper will still be alive whenever a KVO change notification
    // is broadcast, even on a background thread.
    //
    // For the associated object, we use the Helper instance itself as its own key. This guarantees key uniqueness.
    private class Helper : NSObject {
        @nonobjc weak var object : NSObject?
        @nonobjc let path: String
        @nonobjc let callback : (NSObject, NSKeyValueObservedChange<Any>) -> Void
        
        // workaround for <rdar://problem/31640524> Erroneous (?) error when using bridging in the Foundation overlay
        // specifically, overriding observeValue(forKeyPath:of:change:context:) complains that it's not Obj-C-compatible
        @nonobjc static let swizzler: () = {
            let cls = NSClassFromString("_NSKVOCompatibility") as? _NSKVOCompatibilityShim.Type
            cls?._noteProcessHasUsedKVOSwiftOverlay()
            
            let bridgeClass: AnyClass = Helper.self
            let observeSel = #selector(NSObject.observeValue(forKeyPath:of:change:context:))
            let swapSel = #selector(Helper._swizzle_me_observeValue(forKeyPath:of:change:context:))
            let swapObserveMethod = class_getInstanceMethod(bridgeClass, swapSel)!
            class_addMethod(bridgeClass, observeSel, method_getImplementation(swapObserveMethod), method_getTypeEncoding(swapObserveMethod))
        }()
        
        @nonobjc init(object: NSObject, keyPath: AnyKeyPath, options: NSKeyValueObservingOptions, callback: @escaping (NSObject, NSKeyValueObservedChange<Any>) -> Void) {
            _ = Helper.swizzler
            let path = _bridgeKeyPathToString(keyPath)
            self.object = object
            self.path = path
            self.callback = callback
            super.init()
            objc_setAssociatedObject(object, associationKey(), self, .OBJC_ASSOCIATION_RETAIN)
            __KVOKeyPathBridgeMachinery._withBridgeableKeyPath(from: path, to: keyPath) {
                object.addObserver(self, forKeyPath: path, options: options, context: nil)
            }
        }
        
        @nonobjc func invalidate() {
            guard let object = self.object else { return }
            object.removeObserver(self, forKeyPath: path, context: nil)
            objc_setAssociatedObject(object, associationKey(), nil, .OBJC_ASSOCIATION_ASSIGN)
            self.object = nil
        }
        
        @nonobjc private func associationKey() -> UnsafeRawPointer {
            return UnsafeRawPointer(Unmanaged.passUnretained(self).toOpaque())
        }
        
        @objc private func _swizzle_me_observeValue(forKeyPath keyPath: String?, of object: Any?, change: [NSString : Any]?, context: UnsafeMutableRawPointer?) {
            guard let object = object as? NSObject, object === self.object, let change = change else { return }
            let rawKind = change[NSKeyValueChangeKey.kindKey.rawValue as NSString] as! UInt
            let kind = NSKeyValueChange(rawValue: rawKind)!
            let notification = NSKeyValueObservedChange(kind: kind,
                                                        newValue: change[NSKeyValueChangeKey.newKey.rawValue as NSString],
                                                        oldValue: change[NSKeyValueChangeKey.oldKey.rawValue as NSString],
                                                        indexes: change[NSKeyValueChangeKey.indexesKey.rawValue as NSString] as! IndexSet?,
                                                        isPrior: change[NSKeyValueChangeKey.notificationIsPriorKey.rawValue as NSString] as? Bool ?? false)
            callback(object, notification)
        }
    }
    
    @nonobjc private let helper: Helper
    
    fileprivate init(object: NSObject, keyPath: AnyKeyPath, options: NSKeyValueObservingOptions, callback: @escaping (NSObject, NSKeyValueObservedChange<Any>) -> Void) {
        helper = Helper(object: object, keyPath: keyPath, options: options, callback: callback)
    }
    
    ///invalidate() will be called automatically when an NSKeyValueObservation is deinited
    @objc public func invalidate() {
        helper.invalidate()
    }
    
    deinit {
        invalidate()
    }
}

extension _KeyValueCodingAndObserving {
    private func observation<Value>(for keyPath: AnyKeyPath,
                                    options: NSKeyValueObservingOptions,
                                    changeHandler: @escaping (Self, NSKeyValueObservedChange<Value>) -> Void,
                                    valueConverter: @escaping (Any) -> Value?) -> NSKeyValueObservation {
        return NSKeyValueObservation(object: self as! NSObject, keyPath: keyPath, options: options) { obj, change in
            let notification = NSKeyValueObservedChange(kind: change.kind,
                                                        newValue: change.newValue.flatMap(valueConverter),
                                                        oldValue: change.oldValue.flatMap(valueConverter),
                                                        indexes: change.indexes,
                                                        isPrior: change.isPrior)
            changeHandler(obj as! Self, notification)
        }
    }

    ///when the returned NSKeyValueObservation is deinited or invalidated, it will stop observing
    public func observe<Value>(
            _ keyPath: KeyPath<Self, Value>,
            options: NSKeyValueObservingOptions = [],
            changeHandler: @escaping (Self, NSKeyValueObservedChange<Value>) -> Void) -> NSKeyValueObservation {
        return observation(for: keyPath, options: options, changeHandler: changeHandler, valueConverter: {
            // Special logic for keyPath having a optional target value. When the keyPath referencing a nil value, the newValue/oldValue should be in the form .some(nil) instead of .none
            // Solve https://bugs.swift.org/browse/SR-6066

            // NSNull is used by KVO to signal that the keyPath value is nil.
            // If Value == Optional<T>.self, We will get nil instead of .some(nil) when casting Optional(<null>) directly.
            // To fix this behavior, we try to cast Optional<Any>.none to Value in case casting the input to Value fails.
            // In case of NSNull, the first cast will fail producing nil, but the second cast then allows nil (Optional<Any>.none) to be converted to Value, because it is also Optional<T>.
            // In any other case, the first cast already succeeds.
            // If for some reason the input is not of type Value (nor NSNull) and Value is not optional, both casts fail producing nil.

            // Note that this is not necessary for the RawRepresentable overloads below, since there is a concrete overload for optionals.
            $0 as? Value ?? Optional<Any>.none as? Value
        })
    }

    ///when the returned NSKeyValueObservation is deinited or invalidated, it will stop observing
    public func observe<Value>(
            _ keyPath: KeyPath<Self, Value>,
            options: NSKeyValueObservingOptions = [],
            changeHandler: @escaping (Self, NSKeyValueObservedChange<Value>) -> Void) -> NSKeyValueObservation
            where Value: RawRepresentable
    {
        return observation(for: keyPath, options: options, changeHandler: changeHandler, valueConverter: {
            ($0 as? Value.RawValue).flatMap(Value.init)
        })
    }

    ///when the returned NSKeyValueObservation is deinited or invalidated, it will stop observing
    public func observe<Value>(
            _ keyPath: KeyPath<Self, Value?>,
            options: NSKeyValueObservingOptions = [],
            changeHandler: @escaping (Self, NSKeyValueObservedChange<Value?>) -> Void) -> NSKeyValueObservation
            where Value: RawRepresentable
    {
        return observation(for: keyPath, options: options, changeHandler: changeHandler, valueConverter: {
            ($0 as? Value.RawValue).flatMap(Value.init)
        })
    }
    
    public func willChangeValue<Value>(for keyPath: __owned KeyPath<Self, Value>) {
        (self as! NSObject).willChangeValue(forKey: _bridgeKeyPathToString(keyPath))
    }
    
    public func willChange<Value>(_ changeKind: NSKeyValueChange, valuesAt indexes: IndexSet, for keyPath: __owned KeyPath<Self, Value>) {
        (self as! NSObject).willChange(changeKind, valuesAt: indexes, forKey: _bridgeKeyPathToString(keyPath))
    }
    
    public func willChangeValue<Value>(for keyPath: __owned KeyPath<Self, Value>, withSetMutation mutation: NSKeyValueSetMutationKind, using set: Set<Value>) -> Void {
        (self as! NSObject).willChangeValue(forKey: _bridgeKeyPathToString(keyPath), withSetMutation: mutation, using: set)
    }
    
    public func didChangeValue<Value>(for keyPath: __owned KeyPath<Self, Value>) {
        (self as! NSObject).didChangeValue(forKey: _bridgeKeyPathToString(keyPath))
    }
    
    public func didChange<Value>(_ changeKind: NSKeyValueChange, valuesAt indexes: IndexSet, for keyPath: __owned KeyPath<Self, Value>) {
        (self as! NSObject).didChange(changeKind, valuesAt: indexes, forKey: _bridgeKeyPathToString(keyPath))
    }
    
    public func didChangeValue<Value>(for keyPath: __owned KeyPath<Self, Value>, withSetMutation mutation: NSKeyValueSetMutationKind, using set: Set<Value>) -> Void {
        (self as! NSObject).didChangeValue(forKey: _bridgeKeyPathToString(keyPath), withSetMutation: mutation, using: set)
    }
}
