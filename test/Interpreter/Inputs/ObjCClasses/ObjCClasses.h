#ifndef SWIFT_TEST_OBJC_CLASSES_H
#define SWIFT_TEST_OBJC_CLASSES_H

#import <Foundation/NSArray.h>

NS_ASSUME_NONNULL_BEGIN

/* This class has instance variables which are not apparent in the
   interface.  Subclasses will need to be slid by the ObjC runtime. */
@interface HasHiddenIvars : NSObject
- (instancetype)init;
@property NSInteger x;
@property NSInteger y;
@property NSInteger z;
@property NSInteger t;
@end

@interface HasHiddenIvars2 : NSObject
@property id x;
@property id y;
@property id z;
@end

@interface TestingNSError : NSObject
+ (BOOL)throwNilError:(NSError**)error;
+ (nullable void *)maybeThrow:(BOOL)shouldThrow error:(NSError **)error;
+ (nullable void (^)(void))blockThrowError:(NSError **)error;
@end

@interface Container<C> : NSObject
- (id)initWithObject:(C)object NS_DESIGNATED_INITIALIZER;
- (id)init NS_UNAVAILABLE;

@property C object;

- (void)processObjectWithBlock:(void (^)(C))block;
- (void)updateObjectWithBlock:(C (^)())block;
@end

@interface Container<D> (Cat1)
- (id)initWithCat1:(D)object;
- (D)getCat1;
- (void)setCat1:(D)object;
@property D cat1Property;
@end

@interface SubContainer<E> : Container<E>
@end

@interface NestedContainer<F> : Container<Container<F> *>
@end

@interface StringContainer : Container<NSString *>
@end

@interface CopyingContainer<C: id<NSCopying>> : Container<C>
@end

@interface Animal : NSObject
@property (readonly) NSString *noise;
@end

@interface Dog : Animal
@end

@interface AnimalContainer<C: Animal *> : Container<C>
@end

#if __has_feature(objc_class_property)
@protocol ProtoWithClassProperty
+ (void)reset;
@property (class) int value;

@optional
@property (class, readonly) BOOL optionalClassProp;
@end

@interface ClassWithClassProperty : NSObject <ProtoWithClassProperty>
@end

@interface ObjCSubclassWithClassProperty : ClassWithClassProperty
// Deliberately redeclared.
@property (class) int value;
@end

@protocol PropertyNamingConflictProto
@property (nullable) id protoProp;
@property (class, nullable) id protoProp;
@end

@interface PropertyNamingConflict : NSObject
@property (readonly, nullable) id prop;
@property (class, readonly, nullable) id prop;
@end

#endif // __has_feature(objc_class_property)

@interface BridgedInitializer<T> : NSObject
@property (readwrite) NSArray<T> *objects;
@property (readonly) NSInteger count;
- (id) initWithArray: (NSArray<T>*) array;
@end

@interface NSLifetimeTracked : NSObject
+ (unsigned) count;
@end

@interface TestingBool : NSObject
- (void) shouldBeTrueObjCBool: (BOOL)value;
- (void) shouldBeTrueCBool: (_Bool)value;
@end

@interface OuterType : NSObject
@end

__attribute__((swift_name("OuterType.InnerType")))
@interface OuterTypeInnerType : NSObject
@property NSArray<OuterType *> *things;
@end

@interface ObjCPrintOnDealloc : NSObject
@end

NS_ASSUME_NONNULL_END

#endif
