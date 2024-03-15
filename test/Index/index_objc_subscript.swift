// REQUIRES: objc_interop

// RUN: %empty-directory(%t)
// RUN: split-file %s %t

// RUN: %target-swift-ide-test -print-indexed-symbols -source-filename %t/SplitSubscript.swift -import-objc-header %t/objc_impl.h > %t/output.txt
// RUN: %FileCheck %s -input-file %t/output.txt

//--- objc_impl.h
@interface NSObject
@end

@interface MyClass : NSObject
- (instancetype)init;
- (int)objectAtIndexedSubscript:(int)idx;
@end

@interface MyDerivedClass : MyClass
- (instancetype)init;
- (void)setObject:(int)obj atIndexedSubscript:(int)idx;
@end

//--- SplitSubscript.swift
let x = MyClass()!
// CHECK: instance-property/subscript/Swift | subscript(_:) | s:So7MyClassCys5Int32VADcip
// CHECK: instance-method/acc-get/Swift | getter:subscript(_:) | c:objc(cs)MyClass(im)objectAtIndexedSubscript:
let _ = x[0]

let y = MyDerivedClass()!
// CHECK: instance-property/subscript/Swift | subscript(_:) | s:So14MyDerivedClassCys5Int32VADcip
// CHECK: instance-method/acc-get/Swift | getter:subscript(_:) | c:objc(cs)MyClass(im)objectAtIndexedSubscript:
let _ = y[0]
// CHECK: instance-property/subscript/Swift | subscript(_:) | s:So14MyDerivedClassCys5Int32VADcip
// CHECK: instance-method/acc-set/Swift | setter:subscript(_:) | c:objc(cs)MyDerivedClass(im)setObject:atIndexedSubscript:
y[0] = 1
