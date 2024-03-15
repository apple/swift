// REQUIRES: objc_interop

// RUN: %empty-directory(%t)
// RUN: %empty-directory(%t/SplitSubscript)
// RUN: split-file %s %t

// RUN: %target-swift-symbolgraph-extract -sdk %clang-importer-sdk -I %t/SplitSubscript -module-name SplitSubscript -output-dir %t -pretty-print -v
// RUN: %FileCheck %s --input-file %t/SplitSubscript.symbols.json

//--- SplitSubscript/module.modulemap
module SplitSubscript {
  header "SplitSubscript.h"
}

//--- SplitSubscript/SplitSubscript.h
@import Foundation;

// Prior to this test's associated change, the two subscript methods below would get the same USR.
// We need to ensure that the Clang USR for the base-class subscript method does not appear in the
// symbol graph, and instead that the individual subscript symbols get distinct USRs.
// (rdar://117130545)

// CHECK-NOT: c:objc(cs)MyClass(im)objectAtIndexedSubscript:

@interface MyClass : NSObject
// CHECK-DAG: "precise": "s:So7MyClassCyS2ucip"
- (NSUInteger)objectAtIndexedSubscript:(NSUInteger)idx;
@end

@interface MyDerivedClass : MyClass
// CHECK-DAG: "precise": "s:So14MyDerivedClassCyS2ucip"
- (void)setObject:(NSUInteger)obj atIndexedSubscript:(NSUInteger)idx;
@end
