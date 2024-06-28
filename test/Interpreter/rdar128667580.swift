// RUN: %empty-directory(%t)
// RUN: split-file %s %t

// RUN: %target-clang %t/Impl.m -c -o %t/Test.o
// RUN: %target-build-swift %t/main.swift -import-objc-header %t/Test.h %t/Test.o -Xfrontend -disable-concrete-type-metadata-mangled-name-accessors -o %t/main
// RUN: %target-codesign %t/main
// RUN: %target-run %t/main | %FileCheck %s

// REQUIRES: executable_test
// REQUIRES: objc_interop
// REQUIRES: concurrency

//--- Test.h
#import "Foundation/Foundation.h"

@interface Test : NSObject { }
@end

//--- Impl.m
#import "Test.h"

@implementation Test
@end

//--- main.swift
print((any Test & Sendable).self)
// CHECK: Test
