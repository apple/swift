#import "ObjCClasses.h"
#import <Foundation/NSError.h>
#include <stdio.h>
#include <assert.h>

@implementation HasHiddenIvars
@synthesize x;
@synthesize y;
@synthesize z;
@synthesize t;
@end

@implementation HasHiddenIvars2
@synthesize x;
@synthesize y;
@synthesize z;
@end

@implementation TestingNSError
+ (BOOL)throwNilError:(NSError **)error {
  return 0;
}

+ (nullable void *)maybeThrow:(BOOL)shouldThrow error:(NSError **)error {
  if (shouldThrow) {
    *error = [NSError errorWithDomain:@"pointer error" code:0 userInfo:nil];
    return 0;
  }
  return (void *)42;
}

+ (nullable void (^)(void))blockThrowError:(NSError **)error {
  *error = [NSError errorWithDomain:@"block error" code:0 userInfo:nil];
  return 0;
}

@end

@implementation Container
- (id)initWithObject:(id)anObject {
  if ((self = [super init]) != nil) {
    self.object = anObject;
  }
  return self;
}

- (void)processObjectWithBlock:(void (^)(id))block {
  block(self.object);
}

- (void)updateObjectWithBlock:(id (^)())block {
  self.object = block();
}

@synthesize object;

- (id)initWithCat1:(id)anObject {
  return [self initWithObject:anObject];
}

- (id)getCat1 {
  return self.object;
}

- (void)setCat1:(id)obj {
  self.object = obj;
}

- (id)cat1Property {
  return self.object;
}

- (void)setCat1Property:(id)prop {
  self.object = prop;
}

@end

@implementation SubContainer
@end

@implementation NestedContainer
@end

@implementation StringContainer
@end

@implementation CopyingContainer
@end

@implementation Animal
- (NSString *)noise {
  return @"eep";
}
@end

@implementation Dog
- (NSString *)noise {
  return @"woof";
}
@end

@implementation AnimalContainer
@end

#if __has_feature(objc_class_property)
static int _value = 0;
@implementation ClassWithClassProperty
+ (int)value {
  return _value;
}
+ (void)setValue:(int)newValue {
  _value = newValue;
}
+ (void)reset {
  _value = 0;
}
@end

@implementation ObjCSubclassWithClassProperty
+ (BOOL)optionalClassProp {
  return YES;
}
@end

@implementation PropertyNamingConflict
- (id)prop { return self; }
+ (id)prop { return nil; }
@end

#endif

@implementation BridgedInitializer
- (id) initWithArray: (NSArray*) array {
  _objects = array;
  return self;
}
- (NSInteger) count {
  return _objects.count;
}
@end

static unsigned counter = 0;

@implementation NSLifetimeTracked

+ (id) allocWithZone:(NSZone *)zone {
  counter++;
  return [super allocWithZone:zone];
}

- (void) dealloc {
  counter--;
}

+ (unsigned) count {
  return counter;
}

@end

@implementation TestingBool

- (void) shouldBeTrueObjCBool: (BOOL)value {
  assert(value);
}

- (void) shouldBeTrueCBool: (_Bool)value {
  assert(value);
}

@end

@implementation OuterType

- (id)init {
  if ((self = [super init]) != nil) {
  }
  return self;
}

@end

@implementation OuterTypeInnerType

- (id)init {
  if ((self = [super init]) != nil) {
    self.things = [NSArray array];
  }
  return self;
}

@end

@implementation ObjCPrintOnDealloc
- (void)dealloc {
  printf("ObjCPrintOnDealloc deinitialized!\n");
}
@end
