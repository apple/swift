// RUN: %empty-directory(%t)
// RUN: split-file %s %t

// RUN: %target-clang %t/Test.m -c -o %t/Test.o
// RUN: %target-build-swift %t/main.swift -import-objc-header %t/Test.h %t/Test.o -o %t/main
// RUN: %target-codesign %t/main
// RUN: %target-run %t/main | %FileCheck %t/main.swift

// REQUIRES: executable_test
// REQUIRES: objc_interop
// REQUIRES: concurrency
// REQUIRES: OS=macosx

// rdar://127520993

//--- Test.h
#import <Foundation/Foundation.h>

#define SWIFT_SENDABLE __attribute__((__swift_attr__("@Sendable")))

@interface Test<N : id SWIFT_SENDABLE> : NSObject
- (void)luckWithNumber:(nullable N)number;
@end

//--- Test.m
#import "Test.h"

static void NSPrint(NSString *format, ...)
{
    va_list args;

    va_start(args, format);
    NSString *string = [[NSString alloc] initWithFormat:format arguments:args];
    va_end(args);

    fprintf(stdout, "%s\n", [string UTF8String]);

#if !__has_feature(objc_arc)
    [string release];
#endif
}

@implementation Test

- (void)luckWithNumber:(id)number
{
    NSPrint(@"Lucky number: %@", number);
}

@end

//--- main.swift
Test<NSNumber>().luck(withNumber: 5)
// CHECK: Lucky number: 5
