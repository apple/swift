// RUN: %empty-directory(%t)
// RUN: %target-swift-frontend -emit-module -o %t %s
// RUN: %target-swift-frontend -parse-as-library %t/availability-real-sdk.swiftmodule -typecheck -emit-objc-header-path %t/availability-real-sdk.h -import-objc-header %S/../Inputs/empty.h
// RUN: %FileCheck %s < %t/availability-real-sdk.h
// RUN: %check-in-clang %t/availability-real-sdk.h

// REQUIRES: objc_interop


// CHECK-LABEL: @interface NSArray<ObjectType> (SWIFT_EXTENSION(main))
// CHECK-NEXT: - (id _Nonnull)deprecatedMethodInFavorOfReverseObjectEnumerator SWIFT_WARN_UNUSED_RESULT
// CHECK-SAME: SWIFT_DEPRECATED_MSG("This method is deprecated in favor to the old reverseObjectEnumerator method", "reverseObjectEnumerator");
// CHECK-NEXT: - (id _Nonnull)deprecatedMethodOnMacOSInFavorOfReverseObjectEnumerator SWIFT_WARN_UNUSED_RESULT
// CHECK-SAME: SWIFT_AVAILABILITY(macos,deprecated=0.0.1,message="'deprecatedMethodOnMacOSInFavorOfReverseObjectEnumerator' has been renamed to 'reverseObjectEnumerator': This method is deprecated in favor to the old reverseObjectEnumerator method");
// CHECK-NEXT: - (id _Nonnull)unavailableMethodInFavorOfReverseObjectEnumerator SWIFT_WARN_UNUSED_RESULT
// CHECK-SAME: SWIFT_UNAVAILABLE_MSG("'unavailableMethodInFavorOfReverseObjectEnumerator' has been renamed to 'reverseObjectEnumerator': This method is unavailable in favor to the old reverseObjectEnumerator method");
// CHECK-NEXT: - (id _Nonnull)unavailableMethodOnMacOSInFavorOfReverseObjectEnumerator SWIFT_WARN_UNUSED_RESULT
// CHECK-SAME: SWIFT_AVAILABILITY(macos,unavailable,message="'unavailableMethodOnMacOSInFavorOfReverseObjectEnumerator' has been renamed to 'reverseObjectEnumerator': This method is unavailable in favor to the old reverseObjectEnumerator method");
// CHECK-NEXT: - (NSArray * _Nonnull)deprecatedMethodInFavorOfAddingObjectWithObject:(id _Nonnull)object SWIFT_WARN_UNUSED_RESULT
// CHECK-SAME: SWIFT_DEPRECATED_MSG("This method is deprecated in favor to the old adding method", "arrayByAddingObject:");
// CHECK-NEXT: - (NSArray * _Nonnull)deprecatedMethodOnMacOSInFavorOfAddingObjectWithObject:(id _Nonnull)object SWIFT_WARN_UNUSED_RESULT
// CHECK-SAME: SWIFT_AVAILABILITY(macos,deprecated=0.0.1,message="'deprecatedMethodOnMacOSInFavorOfAddingObject' has been renamed to 'arrayByAddingObject:': This method is deprecated in favor to the old adding method");
// CHECK-NEXT: - (NSArray * _Nonnull)unavailableMethodInFavorOfAddingObjectWithObject:(id _Nonnull)object SWIFT_WARN_UNUSED_RESULT
// CHECK-SAME: SWIFT_UNAVAILABLE_MSG("'unavailableMethodInFavorOfAddingObject' has been renamed to 'arrayByAddingObject:': This method is unavailable in favor to the old adding method");
// CHECK-NEXT: - (NSArray * _Nonnull)unavailableMethodOnMacOSInFavorOfAddingObjectWithObject:(id _Nonnull)object SWIFT_WARN_UNUSED_RESULT
// CHECK-SAME: SWIFT_AVAILABILITY(macos,unavailable,message="'unavailableMethodOnMacOSInFavorOfAddingObject' has been renamed to 'arrayByAddingObject:': This method is unavailable in favor to the old adding method");
// CHECK-NEXT: @end

// CHECK-LABEL: @interface SubClassOfSet : NSSet
// CHECK-NEXT: - (id _Nonnull)deprecatedMethodInFavorOfAnyObject SWIFT_WARN_UNUSED_RESULT
// CHECK-SAME: SWIFT_DEPRECATED_MSG("This method is deprecated in favor to the old anyObject method", "anyObject");
// CHECK-NEXT: - (id _Nonnull)deprecatedMethodOnMacOSInFavorOfAnyObject SWIFT_WARN_UNUSED_RESULT
// CHECK-SAME: SWIFT_AVAILABILITY(macos,deprecated=0.0.1,message="'deprecatedMethodOnMacOSInFavorOfAnyObject' has been renamed to 'anyObject': This method is deprecated in favor to the old anyObject method");
// CHECK-NEXT: - (id _Nonnull)unavailableMethodInFavorOfAnyObject SWIFT_WARN_UNUSED_RESULT
// CHECK-SAME: SWIFT_UNAVAILABLE_MSG("'unavailableMethodInFavorOfAnyObject' has been renamed to 'anyObject': This method is unavailable in favor to the old anyObject method");
// CHECK-NEXT: - (id _Nonnull)unavailableMethodOnMacOSInFavorOfAnyObject SWIFT_WARN_UNUSED_RESULT
// CHECK-SAME: SWIFT_AVAILABILITY(macos,unavailable,message="'unavailableMethodOnMacOSInFavorOfAnyObject' has been renamed to 'anyObject': This method is unavailable in favor to the old anyObject method");
// CHECK-NEXT: @property (nonatomic, readonly) NSInteger deprecatedPropertyInFavorOfCount
// CHECK-SAME: SWIFT_DEPRECATED_MSG("This property is deprecated in favor to the old count property", "count");
// CHECK-NEXT: @property (nonatomic, readonly) NSInteger deprecatedOnMacOSPropertyInFavorOfCount
// CHECK-SAME: SWIFT_AVAILABILITY(macos,deprecated=0.0.1,message="'deprecatedOnMacOSPropertyInFavorOfCount' has been renamed to 'count': This property is deprecated in favor to the old count property");
// CHECK-NEXT: @property (nonatomic, readonly) NSInteger unavailablePropertyInFavorOfCount
// CHECK-SAME: SWIFT_UNAVAILABLE_MSG("'unavailablePropertyInFavorOfCount' has been renamed to 'count': This property is unavailable in favor to the old count property");
// CHECK-NEXT: @property (nonatomic, readonly) NSInteger unavailableOnMacOSPropertyInFavorOfCount
// CHECK-SAME: SWIFT_AVAILABILITY(macos,unavailable,message="'unavailableOnMacOSPropertyInFavorOfCount' has been renamed to 'count': This property is unavailable in favor to the old count property");

// CHECK-NEXT: - (nonnull instancetype)init OBJC_DESIGNATED_INITIALIZER;
// CHECK-NEXT: - (nonnull instancetype)initWithObjects:(id _Nonnull const * _Nullable)objects count:(NSUInteger)cnt OBJC_DESIGNATED_INITIALIZER;
// CHECK-NEXT: - (nullable instancetype)initWithCoder:(NSCoder * _Nonnull){{[a-zA-Z]+}} OBJC_DESIGNATED_INITIALIZER;
// CHECK-NEXT: @end

import Foundation


public class SubClassOfSet: NSSet {
    @available(*, deprecated,
    message: "This method is deprecated in favor to the old anyObject method",
    renamed: "anyObject()")
    @objc func deprecatedMethodInFavorOfAnyObject() -> Any { return 0 }
    @available(macOS, deprecated,
    message: "This method is deprecated in favor to the old anyObject method",
    renamed: "anyObject()")
    @objc func deprecatedMethodOnMacOSInFavorOfAnyObject() -> Any { return 0 }
    
    @available(*, unavailable,
    message: "This method is unavailable in favor to the old anyObject method",
    renamed: "anyObject()")
    @objc func unavailableMethodInFavorOfAnyObject() -> Any { return 0 }
    @available(macOS, unavailable,
    message: "This method is unavailable in favor to the old anyObject method",
    renamed: "anyObject()")
    @objc func unavailableMethodOnMacOSInFavorOfAnyObject() -> Any { return 0 }
    
    @available(*, deprecated,
    message: "This property is deprecated in favor to the old count property",
    renamed: "count")
    @objc var deprecatedPropertyInFavorOfCount: Int {
        get {
            return 0
        }
    }
    @available(macOS, deprecated,
    message: "This property is deprecated in favor to the old count property",
    renamed: "count")
    @objc var deprecatedOnMacOSPropertyInFavorOfCount: Int {
        get {
            return 0
        }
    }
    @available(*, unavailable,
    message: "This property is unavailable in favor to the old count property",
    renamed: "count")
    @objc var unavailablePropertyInFavorOfCount: Int {
        get {
            return 0
        }
    }
    @available(macOS, unavailable,
    message: "This property is unavailable in favor to the old count property",
    renamed: "count")
    @objc var unavailableOnMacOSPropertyInFavorOfCount: Int {
        get {
            return 0
        }
    }
}


extension NSArray {
    @available(*, deprecated,
    message: "This method is deprecated in favor to the old reverseObjectEnumerator method",
    renamed: "reverseObjectEnumerator()")
    @objc func deprecatedMethodInFavorOfReverseObjectEnumerator() -> Any { return 0 }
    @available(macOS, deprecated,
    message: "This method is deprecated in favor to the old reverseObjectEnumerator method",
    renamed: "reverseObjectEnumerator()")
    @objc func deprecatedMethodOnMacOSInFavorOfReverseObjectEnumerator() -> Any { return 0 }
    
    @available(*, unavailable,
    message: "This method is unavailable in favor to the old reverseObjectEnumerator method",
    renamed: "reverseObjectEnumerator()")
    @objc func unavailableMethodInFavorOfReverseObjectEnumerator() -> Any { return 0 }
    @available(macOS, unavailable,
    message: "This method is unavailable in favor to the old reverseObjectEnumerator method",
    renamed: "reverseObjectEnumerator()")
    @objc func unavailableMethodOnMacOSInFavorOfReverseObjectEnumerator() -> Any { return 0 }
    
    
    @available(*, deprecated,
    message: "This method is deprecated in favor to the old adding method",
    renamed: "adding(_:)")
    @objc func deprecatedMethodInFavorOfAddingObject(object: Any) -> NSArray {
        return self.adding(object) as NSArray
    }
    @available(macOS, deprecated,
    message: "This method is deprecated in favor to the old adding method",
    renamed: "adding(_:)")
    @objc func deprecatedMethodOnMacOSInFavorOfAddingObject(object: Any) -> NSArray {
        return self.adding(object) as NSArray
    }
    @available(*, unavailable,
    message: "This method is unavailable in favor to the old adding method",
    renamed: "adding(_:)")
    @objc func unavailableMethodInFavorOfAddingObject(object: Any) -> NSArray {
        return self.adding(object) as NSArray
    }
    @available(macOS, unavailable,
    message: "This method is unavailable in favor to the old adding method",
    renamed: "adding(_:)")
    @objc func unavailableMethodOnMacOSInFavorOfAddingObject(object: Any) -> NSArray {
        return self.adding(object) as NSArray
    }
}

