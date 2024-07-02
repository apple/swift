//===--- SwiftObjectNSObject.m - Test SwiftObject's NSObject interop ------===//
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

// This file is compiled and run by SwiftObjectNSObject.swift.

#include <Foundation/Foundation.h>
#include <objc/runtime.h>
#include <objc/message.h>

static int Errors;

#define expectTrue(expr)                                            \
  do {                                                              \
    if (!(expr)) {                                                  \
      printf("%s:%d: not true:  %s\n", __FILE__, __LINE__, #expr);  \
      Errors++;                                                     \
    }                                                               \
  } while (0)

#define expectFalse(expr)                                           \
  do {                                                              \
    if (expr) {                                                     \
      printf("%s:%d: not false: %s\n", __FILE__, __LINE__, #expr);  \
      Errors++;                                                     \
    }                                                               \
  } while (0)

/*
  Summary of Swift definitions from SwiftObjectNSObject.swift:

  class Swift._SwiftObject <NSObject> { ... }

  class C : Swift._SwiftObject {
  @objc func cInstanceMethod()
  @objc class func cClassMethod()
  }

  class D : C {
  @objc func dInstanceMethod()
  @objc class func dClassMethod()
  }
*/

// Add methods to class SwiftObject that can be called by performSelector: et al

static const char *SwiftObjectDemangledName;

static id Perform0(id self, SEL sel) {
    return self;
}

static id Perform1(id self, SEL sel, id one) {
  expectTrue ([one isEqual:@1]);
  return self;
}

static id Perform2(id self, SEL sel, id one, id two) {
  expectTrue ([one isEqual:@1]);
  expectTrue ([two isEqual:@2]);
  return self;
}

static __attribute__((constructor))
void HackSwiftObject()
{
    SwiftObjectDemangledName = "Swift._SwiftObject";
    Class cls = objc_getClass(SwiftObjectDemangledName);

    class_addMethod(cls, @selector(perform0), (IMP)Perform0, "@@:");
    class_addMethod(cls, @selector(perform1:), (IMP)Perform1, "@@:@");
    class_addMethod(cls, @selector(perform2::), (IMP)Perform2, "@@:@@");
}

void TestSwiftObjectNSObjectAssertNoErrors(void)
{
  printf("\nTotal: %d error%s\n",
         Errors, Errors == 1 ? "" : "s");
  if (Errors > 0) {
    exit(1);
  }
}

int CheckSwiftObjectNSObjectEquals(id e1, id e2)
{
  return [e1 isEqual:e2];
}

void TestSwiftObjectNSObjectEquals(id e1, id e2)
{
  printf("NSObjectProtocol.isEqual: Expect %s == %s\n",
	 [[e1 description] UTF8String],
	 [[e2 description] UTF8String]);
  expectTrue([e1 isEqual:e2]);
  expectTrue([e2 isEqual:e1]);
}

void TestSwiftObjectNSObjectNotEquals(id e1, id e2)
{
  printf("NSObjectProtocol.isEqual: Expect %s != %s\n",
	 [[e1 description] UTF8String],
	 [[e2 description] UTF8String]);
  expectFalse([e1 isEqual:e2]);
  expectFalse([e2 isEqual:e1]);
}

void TestSwiftObjectNSObjectHashValue(id e, NSUInteger hashValue)
{
  printf("NSObjectProtocol.hash: Expect [%s hashValue] == %lu\n",
	 [[e description] UTF8String],
	 (unsigned long)hashValue);
  expectTrue([e hash] == hashValue);
}

void TestSwiftObjectNSObjectDefaultHashValue(id e)
{
  NSUInteger hashValue = (NSUInteger)e;
  TestSwiftObjectNSObjectHashValue(e, hashValue);
}

void TestSwiftObjectNSObject(id c, id d)
{
  printf("TestSwiftObjectNSObject\n");

  Class S = objc_getClass(SwiftObjectDemangledName);
  Class C = objc_getClass("SwiftObjectNSObject.C");
  Class D = objc_getClass("SwiftObjectNSObject.D");

  Class S_meta = object_getClass(S);
  Class C_meta = object_getClass(C);
  Class D_meta = object_getClass(D);


  printf("Check connectivity.\n");

  expectTrue (S && C && D && S_meta && C_meta && D_meta && c && d);
  NSSet *distinctnessCheck =
    [NSSet setWithObjects:c, d, S, C, D, S_meta, C_meta, D_meta, nil];
  expectTrue (distinctnessCheck.count == 8);

  expectTrue (class_getSuperclass(D) == C);
  expectTrue (class_getSuperclass(C) == S);
  expectTrue (class_getSuperclass(S) == nil);

  expectTrue (class_getSuperclass(D_meta) == C_meta);
  expectTrue (class_getSuperclass(C_meta) == S_meta);
  expectTrue (class_getSuperclass(S_meta) == S);

  expectTrue (object_getClass(D_meta) == S_meta);
  expectTrue (object_getClass(C_meta) == S_meta);
  expectTrue (object_getClass(S_meta) == S_meta);


  //=== Methods from protocol NSObject ===//

  printf("NSObjectProtocol.class\n");

  expectTrue ([d class] == D);
  expectTrue ([c class] == C);
  expectTrue ([D class] == D);
  expectTrue ([C class] == C);
  expectTrue ([S class] == S);
  expectTrue ([D_meta class] == D_meta);
  expectTrue ([C_meta class] == C_meta);
  expectTrue ([S_meta class] == S_meta);


  printf("NSObjectProtocol.superclass\n");

  expectTrue ([d superclass] == C);
  expectTrue ([c superclass] == S);
  expectTrue ([D superclass] == C);
  expectTrue ([C superclass] == S);
  expectTrue ([S superclass] == nil);
  expectTrue ([D_meta superclass] == C_meta);
  expectTrue ([C_meta superclass] == S_meta);
  expectTrue ([S_meta superclass] == S);


  printf("NSObjectProtocol.isEqual\n");

  expectTrue ([d isEqual:d]);
  expectTrue ([c isEqual:c]);
  expectTrue ([D isEqual:D]);
  expectTrue ([C isEqual:C]);
  expectTrue ([S isEqual:S]);
  expectTrue ([D_meta isEqual:D_meta]);
  expectTrue ([C_meta isEqual:C_meta]);
  expectTrue ([S_meta isEqual:S_meta]);

  expectFalse([d isEqual:S_meta]);
  expectFalse([c isEqual:d]);
  expectFalse([D isEqual:c]);
  expectFalse([C isEqual:D]);
  expectFalse([S isEqual:C]);
  expectFalse([D_meta isEqual:S]);
  expectFalse([C_meta isEqual:D_meta]);
  expectFalse([S_meta isEqual:C_meta]);

  printf("NSObjectProtocol.hash\n");

  expectTrue ([d hash] + [c hash] + [D hash] + [C hash] + [S hash] +
              [D_meta hash] + [C_meta hash] + [S_meta hash] != 0);

  printf("NSObjectProtocol.self\n");

  expectTrue ([d self] == d);
  expectTrue ([c self] == c);
  expectTrue ([D self] == D);
  expectTrue ([C self] == C);
  expectTrue ([S self] == S);
  expectTrue ([D_meta self] == D_meta);
  expectTrue ([C_meta self] == C_meta);
  expectTrue ([S_meta self] == S_meta);


  printf("NSObjectProtocol.isKindOfClass\n");

  expectTrue ([d isKindOfClass:D]);
  expectTrue ([d isKindOfClass:C]);
  expectTrue ([d isKindOfClass:S]);
  expectFalse([d isKindOfClass:D_meta]);
  expectFalse([d isKindOfClass:C_meta]);
  expectFalse([d isKindOfClass:S_meta]);
  expectFalse([d isKindOfClass:[NSObject class]]);

  expectFalse([c isKindOfClass:D]);
  expectTrue ([c isKindOfClass:C]);
  expectTrue ([c isKindOfClass:S]);
  expectFalse([c isKindOfClass:D_meta]);
  expectFalse([c isKindOfClass:C_meta]);
  expectFalse([c isKindOfClass:S_meta]);
  expectFalse([c isKindOfClass:[NSObject class]]);

  expectFalse([D isKindOfClass:D]);
  expectFalse([D isKindOfClass:C]);
  expectTrue ([D isKindOfClass:S]);
  expectTrue ([D isKindOfClass:D_meta]);
  expectTrue ([D isKindOfClass:C_meta]);
  expectTrue ([D isKindOfClass:S_meta]);
  expectFalse([D isKindOfClass:[NSObject class]]);

  expectFalse([C isKindOfClass:D]);
  expectFalse([C isKindOfClass:C]);
  expectTrue ([C isKindOfClass:S]);
  expectFalse([C isKindOfClass:D_meta]);
  expectTrue ([C isKindOfClass:C_meta]);
  expectTrue ([C isKindOfClass:S_meta]);
  expectFalse([C isKindOfClass:[NSObject class]]);

  expectFalse([S isKindOfClass:D]);
  expectFalse([S isKindOfClass:C]);
  expectTrue ([S isKindOfClass:S]);
  expectFalse([S isKindOfClass:D_meta]);
  expectFalse([S isKindOfClass:C_meta]);
  expectTrue ([S isKindOfClass:S_meta]);
  expectFalse([S isKindOfClass:[NSObject class]]);

  expectFalse([D_meta isKindOfClass:D]);
  expectFalse([D_meta isKindOfClass:C]);
  expectTrue ([D_meta isKindOfClass:S]);
  expectFalse([D_meta isKindOfClass:D_meta]);
  expectFalse([D_meta isKindOfClass:C_meta]);
  expectTrue ([D_meta isKindOfClass:S_meta]);
  expectFalse([D_meta isKindOfClass:[NSObject class]]);

  expectFalse([C_meta isKindOfClass:D]);
  expectFalse([C_meta isKindOfClass:C]);
  expectTrue ([C_meta isKindOfClass:S]);
  expectFalse([C_meta isKindOfClass:D_meta]);
  expectFalse([C_meta isKindOfClass:C_meta]);
  expectTrue ([C_meta isKindOfClass:S_meta]);
  expectFalse([C_meta isKindOfClass:[NSObject class]]);

  expectFalse([S_meta isKindOfClass:D]);
  expectFalse([S_meta isKindOfClass:C]);
  expectTrue ([S_meta isKindOfClass:S]);
  expectFalse([S_meta isKindOfClass:D_meta]);
  expectFalse([S_meta isKindOfClass:C_meta]);
  expectTrue ([S_meta isKindOfClass:S_meta]);
  expectFalse([S_meta isKindOfClass:[NSObject class]]);


  printf("NSObjectProtocol.isMemberOfClass\n");

  expectTrue ([d isMemberOfClass:D]);
  expectFalse([d isMemberOfClass:C]);
  expectFalse([d isMemberOfClass:S]);
  expectFalse([d isMemberOfClass:D_meta]);
  expectFalse([d isMemberOfClass:C_meta]);
  expectFalse([d isMemberOfClass:S_meta]);
  expectFalse([d isMemberOfClass:[NSObject class]]);

  expectFalse([c isMemberOfClass:D]);
  expectTrue ([c isMemberOfClass:C]);
  expectFalse([c isMemberOfClass:S]);
  expectFalse([c isMemberOfClass:D_meta]);
  expectFalse([c isMemberOfClass:C_meta]);
  expectFalse([c isMemberOfClass:S_meta]);
  expectFalse([c isMemberOfClass:[NSObject class]]);

  expectFalse([D isMemberOfClass:D]);
  expectFalse([D isMemberOfClass:C]);
  expectFalse([D isMemberOfClass:S]);
  expectTrue ([D isMemberOfClass:D_meta]);
  expectFalse([D isMemberOfClass:C_meta]);
  expectFalse([D isMemberOfClass:S_meta]);
  expectFalse([D isMemberOfClass:[NSObject class]]);

  expectFalse([C isMemberOfClass:D]);
  expectFalse([C isMemberOfClass:C]);
  expectFalse([C isMemberOfClass:S]);
  expectFalse([C isMemberOfClass:D_meta]);
  expectTrue ([C isMemberOfClass:C_meta]);
  expectFalse([C isMemberOfClass:S_meta]);
  expectFalse([C isMemberOfClass:[NSObject class]]);

  expectFalse([S isMemberOfClass:D]);
  expectFalse([S isMemberOfClass:C]);
  expectFalse([S isMemberOfClass:S]);
  expectFalse([S isMemberOfClass:D_meta]);
  expectFalse([S isMemberOfClass:C_meta]);
  expectTrue ([S isMemberOfClass:S_meta]);
  expectFalse([S isMemberOfClass:[NSObject class]]);

  expectFalse([D_meta isMemberOfClass:D]);
  expectFalse([D_meta isMemberOfClass:C]);
  expectFalse([D_meta isMemberOfClass:S]);
  expectFalse([D_meta isMemberOfClass:D_meta]);
  expectFalse([D_meta isMemberOfClass:C_meta]);
  expectTrue ([D_meta isMemberOfClass:S_meta]);
  expectFalse([D_meta isMemberOfClass:[NSObject class]]);

  expectFalse([C_meta isMemberOfClass:D]);
  expectFalse([C_meta isMemberOfClass:C]);
  expectFalse([C_meta isMemberOfClass:S]);
  expectFalse([C_meta isMemberOfClass:D_meta]);
  expectFalse([C_meta isMemberOfClass:C_meta]);
  expectTrue ([C_meta isMemberOfClass:S_meta]);
  expectFalse([C_meta isMemberOfClass:[NSObject class]]);

  expectFalse([S_meta isMemberOfClass:D]);
  expectFalse([S_meta isMemberOfClass:C]);
  expectFalse([S_meta isMemberOfClass:S]);
  expectFalse([S_meta isMemberOfClass:D_meta]);
  expectFalse([S_meta isMemberOfClass:C_meta]);
  expectTrue ([S_meta isMemberOfClass:S_meta]);
  expectFalse([S_meta isMemberOfClass:[NSObject class]]);


  printf("NSObjectProtocol.respondsToSelector\n");

  // instance method from root class
  expectTrue ([d respondsToSelector:@selector(class)]);
  expectTrue ([c respondsToSelector:@selector(class)]);
  expectTrue ([D respondsToSelector:@selector(class)]);
  expectTrue ([C respondsToSelector:@selector(class)]);
  expectTrue ([S respondsToSelector:@selector(class)]);
  expectTrue ([D_meta respondsToSelector:@selector(class)]);
  expectTrue ([C_meta respondsToSelector:@selector(class)]);
  expectTrue ([S_meta respondsToSelector:@selector(class)]);

  // non-instance class method from root class
  expectFalse([d respondsToSelector:@selector(alloc)]);
  expectFalse([c respondsToSelector:@selector(alloc)]);
  expectTrue ([D respondsToSelector:@selector(alloc)]);
  expectTrue ([C respondsToSelector:@selector(alloc)]);
  expectTrue ([S respondsToSelector:@selector(alloc)]);
  expectTrue ([D_meta respondsToSelector:@selector(alloc)]);
  expectTrue ([C_meta respondsToSelector:@selector(alloc)]);
  expectTrue ([S_meta respondsToSelector:@selector(alloc)]);

  // instance method from subclass C
  expectTrue ([d respondsToSelector:@selector(cInstanceMethod)]);
  expectTrue ([c respondsToSelector:@selector(cInstanceMethod)]);
  expectFalse([D respondsToSelector:@selector(cInstanceMethod)]);
  expectFalse([C respondsToSelector:@selector(cInstanceMethod)]);
  expectFalse([S respondsToSelector:@selector(cInstanceMethod)]);
  expectFalse([D_meta respondsToSelector:@selector(cInstanceMethod)]);
  expectFalse([C_meta respondsToSelector:@selector(cInstanceMethod)]);
  expectFalse([S_meta respondsToSelector:@selector(cInstanceMethod)]);

  // class method from subclass C
  expectFalse([d respondsToSelector:@selector(cClassMethod)]);
  expectFalse([c respondsToSelector:@selector(cClassMethod)]);
  expectTrue ([D respondsToSelector:@selector(cClassMethod)]);
  expectTrue ([C respondsToSelector:@selector(cClassMethod)]);
  expectFalse([S respondsToSelector:@selector(cClassMethod)]);
  expectFalse([D_meta respondsToSelector:@selector(cClassMethod)]);
  expectFalse([C_meta respondsToSelector:@selector(cClassMethod)]);
  expectFalse([S_meta respondsToSelector:@selector(cClassMethod)]);

  // instance method from subclass D
  expectTrue ([d respondsToSelector:@selector(dInstanceMethod)]);
  expectFalse([c respondsToSelector:@selector(dInstanceMethod)]);
  expectFalse([D respondsToSelector:@selector(dInstanceMethod)]);
  expectFalse([C respondsToSelector:@selector(dInstanceMethod)]);
  expectFalse([S respondsToSelector:@selector(dInstanceMethod)]);
  expectFalse([D_meta respondsToSelector:@selector(dInstanceMethod)]);
  expectFalse([C_meta respondsToSelector:@selector(dInstanceMethod)]);
  expectFalse([S_meta respondsToSelector:@selector(dInstanceMethod)]);

  // class method from subclass D
  expectFalse([d respondsToSelector:@selector(dClassMethod)]);
  expectFalse([c respondsToSelector:@selector(dClassMethod)]);
  expectTrue ([D respondsToSelector:@selector(dClassMethod)]);
  expectFalse([C respondsToSelector:@selector(dClassMethod)]);
  expectFalse([S respondsToSelector:@selector(dClassMethod)]);
  expectFalse([D_meta respondsToSelector:@selector(dClassMethod)]);
  expectFalse([C_meta respondsToSelector:@selector(dClassMethod)]);
  expectFalse([S_meta respondsToSelector:@selector(dClassMethod)]);

  // nonexistent method
  expectFalse([d respondsToSelector:@selector(DESSLOK)]);
  expectFalse([c respondsToSelector:@selector(DESSLOK)]);
  expectFalse([D respondsToSelector:@selector(DESSLOK)]);
  expectFalse([C respondsToSelector:@selector(DESSLOK)]);
  expectFalse([S respondsToSelector:@selector(DESSLOK)]);
  expectFalse([D_meta respondsToSelector:@selector(DESSLOK)]);
  expectFalse([C_meta respondsToSelector:@selector(DESSLOK)]);
  expectFalse([S_meta respondsToSelector:@selector(DESSLOK)]);


  printf("NSObjectProtocol.conformsToProtocol\n");

  expectTrue ([d conformsToProtocol:@protocol(NSObject)]);
  expectTrue ([c conformsToProtocol:@protocol(NSObject)]);
  expectTrue ([D conformsToProtocol:@protocol(NSObject)]);
  expectTrue ([C conformsToProtocol:@protocol(NSObject)]);
  expectTrue ([S conformsToProtocol:@protocol(NSObject)]);
  expectTrue ([D_meta conformsToProtocol:@protocol(NSObject)]);
  expectTrue ([C_meta conformsToProtocol:@protocol(NSObject)]);
  expectTrue ([S_meta conformsToProtocol:@protocol(NSObject)]);

  expectFalse([d conformsToProtocol:@protocol(NSCoding)]);
  expectFalse([c conformsToProtocol:@protocol(NSCoding)]);
  expectFalse([D conformsToProtocol:@protocol(NSCoding)]);
  expectFalse([C conformsToProtocol:@protocol(NSCoding)]);
  expectFalse([S conformsToProtocol:@protocol(NSCoding)]);
  expectFalse([D_meta conformsToProtocol:@protocol(NSCoding)]);
  expectFalse([C_meta conformsToProtocol:@protocol(NSCoding)]);
  expectFalse([S_meta conformsToProtocol:@protocol(NSCoding)]);


  printf("NSObjectProtocol.description\n");

  expectTrue ([[d description] isEqual:@"SwiftObjectNSObject.D"]);
  expectTrue ([[c description] isEqual:@"SwiftObjectNSObject.C"]);
  expectTrue ([[D description] isEqual:@"SwiftObjectNSObject.D"]);
  expectTrue ([[C description] isEqual:@"SwiftObjectNSObject.C"]);
  expectTrue ([[D_meta description] isEqual:@"SwiftObjectNSObject.D"]);
  expectTrue ([[C_meta description] isEqual:@"SwiftObjectNSObject.C"]);
  expectTrue ([[S_meta description] isEqual:@(SwiftObjectDemangledName)]);

  // NSLog() calls -description and also some private methods.
  // This output is checked by FileCheck in SwiftObjectNSObject.swift.
  NSLog(@"c ##%@##", c);
  NSLog(@"d ##%@##", d);
  NSLog(@"S ##%@##", S);


  printf("NSObjectProtocol.debugDescription\n");

  expectTrue ([[d debugDescription] isEqual:@"SwiftObjectNSObject.D"]);
  expectTrue ([[c debugDescription] isEqual:@"SwiftObjectNSObject.C"]);
  expectTrue ([[D debugDescription] isEqual:@"SwiftObjectNSObject.D"]);
  expectTrue ([[C debugDescription] isEqual:@"SwiftObjectNSObject.C"]);
  expectTrue ([[D_meta debugDescription] isEqual:@"SwiftObjectNSObject.D"]);
  expectTrue ([[C_meta debugDescription] isEqual:@"SwiftObjectNSObject.C"]);
  expectTrue ([[S_meta debugDescription] isEqual:@(SwiftObjectDemangledName)]);


  printf("NSObjectProtocol.performSelector\n");

  SEL sel0 = @selector(perform0);
  expectTrue ([d performSelector:sel0] == d);
  expectTrue ([c performSelector:sel0] == c);
  expectTrue ([D performSelector:sel0] == D);
  expectTrue ([C performSelector:sel0] == C);
  expectTrue ([S performSelector:sel0] == S);
  expectTrue ([D_meta performSelector:sel0] == D_meta);
  expectTrue ([C_meta performSelector:sel0] == C_meta);
  expectTrue ([S_meta performSelector:sel0] == S_meta);


  printf("NSObjectProtocol.performSelector:withObject:\n");

  SEL sel1 = @selector(perform1:);
  expectTrue ([d performSelector:sel1 withObject:@1] == d);
  expectTrue ([c performSelector:sel1 withObject:@1] == c);
  expectTrue ([D performSelector:sel1 withObject:@1] == D);
  expectTrue ([C performSelector:sel1 withObject:@1] == C);
  expectTrue ([S performSelector:sel1 withObject:@1] == S);
  expectTrue ([D_meta performSelector:sel1 withObject:@1] == D_meta);
  expectTrue ([C_meta performSelector:sel1 withObject:@1] == C_meta);
  expectTrue ([S_meta performSelector:sel1 withObject:@1] == S_meta);


  printf("NSObjectProtocol.performSelector:withObject:withObject:\n");

  SEL sel2 = @selector(perform2::);
  expectTrue ([d performSelector:sel2 withObject:@1 withObject:@2] == d);
  expectTrue ([c performSelector:sel2 withObject:@1 withObject:@2] == c);
  expectTrue ([D performSelector:sel2 withObject:@1 withObject:@2] == D);
  expectTrue ([C performSelector:sel2 withObject:@1 withObject:@2] == C);
  expectTrue ([S performSelector:sel2 withObject:@1 withObject:@2] == S);
  expectTrue ([D_meta performSelector:sel2
                           withObject:@1 withObject:@2] == D_meta);
  expectTrue ([C_meta performSelector:sel2
                           withObject:@1 withObject:@2] == C_meta);
  expectTrue ([S_meta performSelector:sel2
                           withObject:@1 withObject:@2] == S_meta);


  printf("NSObjectProtocol.isProxy\n");

  expectFalse([d isProxy]);
  expectFalse([c isProxy]);
  expectFalse([D isProxy]);
  expectFalse([C isProxy]);
  expectFalse([S isProxy]);
  expectFalse([D_meta isProxy]);
  expectFalse([C_meta isProxy]);
  expectFalse([S_meta isProxy]);


  printf("NSObjectProtocol.retain\n");
  printf("NSObjectProtocol.release\n");
  printf("NSObjectProtocol.autorelease\n");
  printf("NSObjectProtocol.retainCount\n");
  @autoreleasepool {
    expectTrue ([[[d retain] autorelease] retainCount] != 0);
    expectTrue ([[[c retain] autorelease] retainCount] != 0);
    expectTrue ([[[D retain] autorelease] retainCount] != 0);
    expectTrue ([[[C retain] autorelease] retainCount] != 0);
    expectTrue ([[[S retain] autorelease] retainCount] != 0);
    expectTrue ([[[D_meta retain] autorelease] retainCount] != 0);
    expectTrue ([[[C_meta retain] autorelease] retainCount] != 0);
    expectTrue ([[[S_meta retain] autorelease] retainCount] != 0);
  }


  printf("NSObjectProtocol.zone\n");

  expectTrue ([d zone] != nil);
  expectTrue ([c zone] != nil);
  expectTrue ([D zone] != nil);
  expectTrue ([C zone] != nil);
  expectTrue ([S zone] != nil);
  expectTrue ([D_meta zone] != nil);
  expectTrue ([C_meta zone] != nil);
  expectTrue ([S_meta zone] != nil);


  //=== Other methods from class NSObject ===//

  // FIXME: mostly untested

  printf("NSObject.instancesRespondToSelector\n");

  // non-class objects need not apply
  expectFalse([d respondsToSelector:@selector(instancesRespondToSelector:)]);
  expectFalse([c respondsToSelector:@selector(instancesRespondToSelector:)]);
  expectTrue ([S respondsToSelector:@selector(instancesRespondToSelector:)]);

  // instance method from root class
  expectTrue ([D instancesRespondToSelector:@selector(class)]);
  expectTrue ([C instancesRespondToSelector:@selector(class)]);
  expectTrue ([S instancesRespondToSelector:@selector(class)]);
  expectTrue ([D_meta instancesRespondToSelector:@selector(class)]);
  expectTrue ([C_meta instancesRespondToSelector:@selector(class)]);
  expectTrue ([S_meta instancesRespondToSelector:@selector(class)]);

  // non-instance class method from root class
  expectFalse([D instancesRespondToSelector:@selector(alloc)]);
  expectFalse([C instancesRespondToSelector:@selector(alloc)]);
  expectFalse([S instancesRespondToSelector:@selector(alloc)]);
  expectTrue ([D_meta instancesRespondToSelector:@selector(alloc)]);
  expectTrue ([C_meta instancesRespondToSelector:@selector(alloc)]);
  expectTrue ([S_meta instancesRespondToSelector:@selector(alloc)]);

  // instance method from subclass C
  expectTrue ([D instancesRespondToSelector:@selector(cInstanceMethod)]);
  expectTrue ([C instancesRespondToSelector:@selector(cInstanceMethod)]);
  expectFalse([S instancesRespondToSelector:@selector(cInstanceMethod)]);
  expectFalse([D_meta instancesRespondToSelector:@selector(cInstanceMethod)]);
  expectFalse([C_meta instancesRespondToSelector:@selector(cInstanceMethod)]);
  expectFalse([S_meta instancesRespondToSelector:@selector(cInstanceMethod)]);

  // class method from subclass C
  expectFalse([D instancesRespondToSelector:@selector(cClassMethod)]);
  expectFalse([C instancesRespondToSelector:@selector(cClassMethod)]);
  expectFalse([S instancesRespondToSelector:@selector(cClassMethod)]);
  expectTrue ([D_meta instancesRespondToSelector:@selector(cClassMethod)]);
  expectTrue ([C_meta instancesRespondToSelector:@selector(cClassMethod)]);
  expectFalse([S_meta instancesRespondToSelector:@selector(cClassMethod)]);

  // instance method from subclass D
  expectTrue ([D instancesRespondToSelector:@selector(dInstanceMethod)]);
  expectFalse([C instancesRespondToSelector:@selector(dInstanceMethod)]);
  expectFalse([S instancesRespondToSelector:@selector(dInstanceMethod)]);
  expectFalse([D_meta instancesRespondToSelector:@selector(dInstanceMethod)]);
  expectFalse([C_meta instancesRespondToSelector:@selector(dInstanceMethod)]);
  expectFalse([S_meta instancesRespondToSelector:@selector(dInstanceMethod)]);

  // class method from subclass D
  expectFalse([D instancesRespondToSelector:@selector(dClassMethod)]);
  expectFalse([C instancesRespondToSelector:@selector(dClassMethod)]);
  expectFalse([S instancesRespondToSelector:@selector(dClassMethod)]);
  expectTrue ([D_meta instancesRespondToSelector:@selector(dClassMethod)]);
  expectFalse([C_meta instancesRespondToSelector:@selector(dClassMethod)]);
  expectFalse([S_meta instancesRespondToSelector:@selector(dClassMethod)]);

  // nonexistent method
  expectFalse([D instancesRespondToSelector:@selector(DESSLOK)]);
  expectFalse([C instancesRespondToSelector:@selector(DESSLOK)]);
  expectFalse([S instancesRespondToSelector:@selector(DESSLOK)]);
  expectFalse([D_meta instancesRespondToSelector:@selector(DESSLOK)]);
  expectFalse([C_meta instancesRespondToSelector:@selector(DESSLOK)]);
  expectFalse([S_meta instancesRespondToSelector:@selector(DESSLOK)]);


  printf("NSObject.methodForSelector\n");

  IMP fwd = (IMP)_objc_msgForward;
  IMP imp, imp2;

  // instance method from root class
  // that has no root metaclass override
  imp = [S methodForSelector:@selector(self)];
  expectTrue (imp != nil);
  expectTrue (imp != fwd);
  expectTrue ([c methodForSelector:@selector(self)] == imp);
  expectTrue ([d methodForSelector:@selector(self)] == imp);
  expectTrue ([C methodForSelector:@selector(self)] == imp);
  expectTrue ([D methodForSelector:@selector(self)] == imp);
  expectTrue ([S_meta methodForSelector:@selector(self)] == imp);
  expectTrue ([C_meta methodForSelector:@selector(self)] == imp);
  expectTrue ([D_meta methodForSelector:@selector(self)] == imp);

  // instance method from root class
  // that also has a root metaclass override
  imp = [c methodForSelector:@selector(class)];
  expectTrue (imp != nil);
  expectTrue (imp != fwd);
  imp2 = [C methodForSelector:@selector(class)];
  expectTrue (imp2 != nil);
  expectTrue (imp2 != fwd);
  expectTrue (imp2 != imp);
  expectTrue ([d methodForSelector:@selector(class)] == imp);
  expectTrue ([S methodForSelector:@selector(class)] == imp2);
  expectTrue ([D methodForSelector:@selector(class)] == imp2);
  expectTrue ([S_meta methodForSelector:@selector(class)] == imp2);
  expectTrue ([C_meta methodForSelector:@selector(class)] == imp2);
  expectTrue ([D_meta methodForSelector:@selector(class)] == imp2);

  // non-instance class method from root class
  imp = [S methodForSelector:@selector(alloc)];
  expectTrue (imp != nil);
  expectTrue (imp != fwd);
  expectTrue ([c methodForSelector:@selector(alloc)] == fwd);
  expectTrue ([d methodForSelector:@selector(alloc)] == fwd);
  expectTrue ([C methodForSelector:@selector(alloc)] == imp);
  expectTrue ([D methodForSelector:@selector(alloc)] == imp);
  expectTrue ([S_meta methodForSelector:@selector(alloc)] == imp);
  expectTrue ([C_meta methodForSelector:@selector(alloc)] == imp);
  expectTrue ([D_meta methodForSelector:@selector(alloc)] == imp);

  // instance method from subclass C
  imp = [c methodForSelector:@selector(cInstanceMethod)];
  expectTrue (imp != nil);
  expectTrue (imp != fwd);
  expectTrue ([d methodForSelector:@selector(cInstanceMethod)] == imp);
  expectTrue ([S methodForSelector:@selector(cInstanceMethod)] == fwd);
  expectTrue ([C methodForSelector:@selector(cInstanceMethod)] == fwd);
  expectTrue ([D methodForSelector:@selector(cInstanceMethod)] == fwd);
  expectTrue ([S_meta methodForSelector:@selector(cInstanceMethod)] == fwd);
  expectTrue ([C_meta methodForSelector:@selector(cInstanceMethod)] == fwd);
  expectTrue ([D_meta methodForSelector:@selector(cInstanceMethod)] == fwd);

  // class method from subclass C
  imp = [C methodForSelector:@selector(cClassMethod)];
  expectTrue (imp != nil);
  expectTrue (imp != fwd);
  expectTrue ([c methodForSelector:@selector(cClassMethod)] == fwd);
  expectTrue ([d methodForSelector:@selector(cClassMethod)] == fwd);
  expectTrue ([S methodForSelector:@selector(cClassMethod)] == fwd);
  expectTrue ([D methodForSelector:@selector(cClassMethod)] == imp);
  expectTrue ([S_meta methodForSelector:@selector(cClassMethod)] == fwd);
  expectTrue ([C_meta methodForSelector:@selector(cClassMethod)] == fwd);
  expectTrue ([D_meta methodForSelector:@selector(cClassMethod)] == fwd);

  // instance method from subclass D
  imp = [d methodForSelector:@selector(dInstanceMethod)];
  expectTrue (imp != nil);
  expectTrue (imp != fwd);
  expectTrue ([c methodForSelector:@selector(dInstanceMethod)] == fwd);
  expectTrue ([S methodForSelector:@selector(dInstanceMethod)] == fwd);
  expectTrue ([C methodForSelector:@selector(dInstanceMethod)] == fwd);
  expectTrue ([D methodForSelector:@selector(dInstanceMethod)] == fwd);
  expectTrue ([S_meta methodForSelector:@selector(dInstanceMethod)] == fwd);
  expectTrue ([C_meta methodForSelector:@selector(dInstanceMethod)] == fwd);
  expectTrue ([D_meta methodForSelector:@selector(dInstanceMethod)] == fwd);

  // class method from subclass D
  imp = [D methodForSelector:@selector(dClassMethod)];
  expectTrue (imp != nil);
  expectTrue (imp != fwd);
  expectTrue ([c methodForSelector:@selector(dClassMethod)] == fwd);
  expectTrue ([d methodForSelector:@selector(dClassMethod)] == fwd);
  expectTrue ([S methodForSelector:@selector(dClassMethod)] == fwd);
  expectTrue ([C methodForSelector:@selector(dClassMethod)] == fwd);
  expectTrue ([S_meta methodForSelector:@selector(dClassMethod)] == fwd);
  expectTrue ([C_meta methodForSelector:@selector(dClassMethod)] == fwd);
  expectTrue ([D_meta methodForSelector:@selector(dClassMethod)] == fwd);

  // instance method from subclass C overridden by D
  imp = [c methodForSelector:@selector(cInstanceOverride)];
  expectTrue (imp != nil);
  expectTrue (imp != fwd);
  imp2 = [d methodForSelector:@selector(cInstanceOverride)];
  expectTrue (imp2 != nil);
  expectTrue (imp2 != fwd);
  expectTrue (imp2 != imp);
  expectTrue ([S methodForSelector:@selector(cInstanceOverride)] == fwd);
  expectTrue ([C methodForSelector:@selector(cInstanceOverride)] == fwd);
  expectTrue ([D methodForSelector:@selector(cInstanceOverride)] == fwd);
  expectTrue ([S_meta methodForSelector:@selector(cInstanceOverride)] == fwd);
  expectTrue ([C_meta methodForSelector:@selector(cInstanceOverride)] == fwd);
  expectTrue ([D_meta methodForSelector:@selector(cInstanceOverride)] == fwd);

  // class method from subclass C overridden by D
  imp = [C methodForSelector:@selector(cClassOverride)];
  expectTrue (imp != nil);
  expectTrue (imp != fwd);
  imp2 = [D methodForSelector:@selector(cClassOverride)];
  expectTrue (imp2 != nil);
  expectTrue (imp2 != fwd);
  expectTrue (imp2 != imp);
  expectTrue ([c methodForSelector:@selector(cClassOverride)] == fwd);
  expectTrue ([d methodForSelector:@selector(cClassOverride)] == fwd);
  expectTrue ([S methodForSelector:@selector(cClassOverride)] == fwd);
  expectTrue ([S_meta methodForSelector:@selector(cClassOverride)] == fwd);
  expectTrue ([C_meta methodForSelector:@selector(cClassOverride)] == fwd);
  expectTrue ([D_meta methodForSelector:@selector(cClassOverride)] == fwd);

  // nonexistent method
  expectTrue ([c methodForSelector:@selector(DESSLOK)] == fwd);
  expectTrue ([d methodForSelector:@selector(DESSLOK)] == fwd);
  expectTrue ([S methodForSelector:@selector(DESSLOK)] == fwd);
  expectTrue ([C methodForSelector:@selector(DESSLOK)] == fwd);
  expectTrue ([D methodForSelector:@selector(DESSLOK)] == fwd);
  expectTrue ([S_meta methodForSelector:@selector(DESSLOK)] == fwd);
  expectTrue ([C_meta methodForSelector:@selector(DESSLOK)] == fwd);
  expectTrue ([D_meta methodForSelector:@selector(DESSLOK)] == fwd);


  printf("NSObject.instanceMethodForSelector\n");

  // non-class objects need not apply
  expectFalse([d respondsToSelector:@selector(instanceMethodForSelector:)]);
  expectFalse([c respondsToSelector:@selector(instanceMethodForSelector:)]);
  expectTrue ([S respondsToSelector:@selector(instanceMethodForSelector:)]);

  // instance method from root class
  // that has no root metaclass override
  expectTrue ([C instanceMethodForSelector:@selector(self)] == [c methodForSelector:@selector(self)]);
  expectTrue ([D instanceMethodForSelector:@selector(self)] == [d methodForSelector:@selector(self)]);
  expectTrue ([S_meta instanceMethodForSelector:@selector(self)] == [S methodForSelector:@selector(self)]);
  expectTrue ([C_meta instanceMethodForSelector:@selector(self)] == [C methodForSelector:@selector(self)]);
  expectTrue ([D_meta instanceMethodForSelector:@selector(self)] == [D methodForSelector:@selector(self)]);

  // instance method from root class
  // that also has a root metaclass override
  expectTrue ([C instanceMethodForSelector:@selector(class)] == [c methodForSelector:@selector(class)]);
  expectTrue ([D instanceMethodForSelector:@selector(class)] == [d methodForSelector:@selector(class)]);
  expectTrue ([S_meta instanceMethodForSelector:@selector(class)] == [S methodForSelector:@selector(class)]);
  expectTrue ([C_meta instanceMethodForSelector:@selector(class)] == [C methodForSelector:@selector(class)]);
  expectTrue ([D_meta instanceMethodForSelector:@selector(class)] == [D methodForSelector:@selector(class)]);

  // non-instance class method from root class
  expectTrue ([C instanceMethodForSelector:@selector(alloc)] == [c methodForSelector:@selector(alloc)]);
  expectTrue ([D instanceMethodForSelector:@selector(alloc)] == [d methodForSelector:@selector(alloc)]);
  expectTrue ([S_meta instanceMethodForSelector:@selector(alloc)] == [S methodForSelector:@selector(alloc)]);
  expectTrue ([C_meta instanceMethodForSelector:@selector(alloc)] == [C methodForSelector:@selector(alloc)]);
  expectTrue ([D_meta instanceMethodForSelector:@selector(alloc)] == [D methodForSelector:@selector(alloc)]);

  // instance method from subclass C
  expectTrue ([C instanceMethodForSelector:@selector(cInstanceMethod)] == [c methodForSelector:@selector(cInstanceMethod)]);
  expectTrue ([D instanceMethodForSelector:@selector(cInstanceMethod)] == [d methodForSelector:@selector(cInstanceMethod)]);
  expectTrue ([S_meta instanceMethodForSelector:@selector(cInstanceMethod)] == [S methodForSelector:@selector(cInstanceMethod)]);
  expectTrue ([C_meta instanceMethodForSelector:@selector(cInstanceMethod)] == [C methodForSelector:@selector(cInstanceMethod)]);
  expectTrue ([D_meta instanceMethodForSelector:@selector(cInstanceMethod)] == [D methodForSelector:@selector(cInstanceMethod)]);

  // class method from subclass C
  expectTrue ([C instanceMethodForSelector:@selector(cClassMethod)] == [c methodForSelector:@selector(cClassMethod)]);
  expectTrue ([D instanceMethodForSelector:@selector(cClassMethod)] == [d methodForSelector:@selector(cClassMethod)]);
  expectTrue ([S_meta instanceMethodForSelector:@selector(cClassMethod)] == [S methodForSelector:@selector(cClassMethod)]);
  expectTrue ([C_meta instanceMethodForSelector:@selector(cClassMethod)] == [C methodForSelector:@selector(cClassMethod)]);
  expectTrue ([D_meta instanceMethodForSelector:@selector(cClassMethod)] == [D methodForSelector:@selector(cClassMethod)]);

  // instance method from subclass D
  expectTrue ([C instanceMethodForSelector:@selector(dInstanceMethod)] == [c methodForSelector:@selector(dInstanceMethod)]);
  expectTrue ([D instanceMethodForSelector:@selector(dInstanceMethod)] == [d methodForSelector:@selector(dInstanceMethod)]);
  expectTrue ([S_meta instanceMethodForSelector:@selector(dInstanceMethod)] == [S methodForSelector:@selector(dInstanceMethod)]);
  expectTrue ([C_meta instanceMethodForSelector:@selector(dInstanceMethod)] == [C methodForSelector:@selector(dInstanceMethod)]);
  expectTrue ([D_meta instanceMethodForSelector:@selector(dInstanceMethod)] == [D methodForSelector:@selector(dInstanceMethod)]);

  // class method from subclass D
  expectTrue ([C instanceMethodForSelector:@selector(dClassMethod)] == [c methodForSelector:@selector(dClassMethod)]);
  expectTrue ([D instanceMethodForSelector:@selector(dClassMethod)] == [d methodForSelector:@selector(dClassMethod)]);
  expectTrue ([S_meta instanceMethodForSelector:@selector(dClassMethod)] == [S methodForSelector:@selector(dClassMethod)]);
  expectTrue ([C_meta instanceMethodForSelector:@selector(dClassMethod)] == [C methodForSelector:@selector(dClassMethod)]);
  expectTrue ([D_meta instanceMethodForSelector:@selector(dClassMethod)] == [D methodForSelector:@selector(dClassMethod)]);

  // instance method from subclass C overridden by D
  expectTrue ([C instanceMethodForSelector:@selector(cInstanceOverride)] == [c methodForSelector:@selector(cInstanceOverride)]);
  expectTrue ([D instanceMethodForSelector:@selector(cInstanceOverride)] == [d methodForSelector:@selector(cInstanceOverride)]);
  expectTrue ([S_meta instanceMethodForSelector:@selector(cInstanceOverride)] == [S methodForSelector:@selector(cInstanceOverride)]);
  expectTrue ([C_meta instanceMethodForSelector:@selector(cInstanceOverride)] == [C methodForSelector:@selector(cInstanceOverride)]);
  expectTrue ([D_meta instanceMethodForSelector:@selector(cInstanceOverride)] == [D methodForSelector:@selector(cInstanceOverride)]);

  // class method from subclass C overridden by D
  expectTrue ([C instanceMethodForSelector:@selector(cClassOverride)] == [c methodForSelector:@selector(cClassOverride)]);
  expectTrue ([D instanceMethodForSelector:@selector(cClassOverride)] == [d methodForSelector:@selector(cClassOverride)]);
  expectTrue ([S_meta instanceMethodForSelector:@selector(cClassOverride)] == [S methodForSelector:@selector(cClassOverride)]);
  expectTrue ([C_meta instanceMethodForSelector:@selector(cClassOverride)] == [C methodForSelector:@selector(cClassOverride)]);
  expectTrue ([D_meta instanceMethodForSelector:@selector(cClassOverride)] == [D methodForSelector:@selector(cClassOverride)]);

  // nonexistent method
  expectTrue ([S instanceMethodForSelector:@selector(DESSLOK)] == fwd);
  expectTrue ([C instanceMethodForSelector:@selector(DESSLOK)] == fwd);
  expectTrue ([D instanceMethodForSelector:@selector(DESSLOK)] == fwd);
  expectTrue ([S_meta instanceMethodForSelector:@selector(DESSLOK)] == fwd);
  expectTrue ([C_meta instanceMethodForSelector:@selector(DESSLOK)] == fwd);
  expectTrue ([D_meta instanceMethodForSelector:@selector(DESSLOK)] == fwd);
}
