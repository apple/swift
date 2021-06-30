struct S {
  let count = 1
}

public class CC {
  func foo(_ s : S) -> Int {
    if s.count > 0 {
      return 1
    } else {
      return 0
    }
  }
  func getSelf() -> CC {
    return self
  }
  func getSelf(_ i : Int) -> CC {
    return self
  }
}

func foo1(_ c : CC) -> CC{
  _ = c.getSelf().getSelf().getSelf().getSelf()
  _ = c.getSelf(1).getSelf(1).getSelf(1).getSelf(1)
  return c.getSelf()
}

protocol Foo {
  var bar: String { get }
}
func foo(x: Foo) {
  _ = x.bar
}

func testWithoutActuallyEscaping(closure: (Int) -> Void) {
  withoutActuallyEscaping(closure) { escapable in
    _ = escapable
  }
}

class SR12957 {
  static let main = SR12957()
}

func bar(x: SR12957 = .main) {}

// RUN: %target-swift-ide-test -range -pos=7:8 -end-pos=7:19 -source-filename %s | %FileCheck %s -check-prefix=CHECK-BOOL
// CHECK-BOOL: <Type>Bool</Type>

// RUN: %target-swift-ide-test -range -pos=22:39 -end-pos=22:48 -source-filename %s | %FileCheck %s -check-prefix=CHECK-PART-EXPR
// RUN: %target-swift-ide-test -range -pos=22:29 -end-pos=22:38 -source-filename %s | %FileCheck %s -check-prefix=CHECK-PART-EXPR
// RUN: %target-swift-ide-test -range -pos=22:19 -end-pos=22:28 -source-filename %s | %FileCheck %s -check-prefix=CHECK-PART-EXPR
// RUN: %target-swift-ide-test -range -pos=22:9 -end-pos=22:18 -source-filename %s | %FileCheck %s -check-prefix=CHECK-PART-EXPR

// RUN: %target-swift-ide-test -range -pos=23:42 -end-pos=23:52 -source-filename %s | %FileCheck %s -check-prefix=CHECK-PART-EXPR1
// RUN: %target-swift-ide-test -range -pos=23:31 -end-pos=23:41 -source-filename %s | %FileCheck %s -check-prefix=CHECK-PART-EXPR1
// RUN: %target-swift-ide-test -range -pos=23:20 -end-pos=23:30 -source-filename %s | %FileCheck %s -check-prefix=CHECK-PART-EXPR1
// RUN: %target-swift-ide-test -range -pos=23:9 -end-pos=23:19 -source-filename %s | %FileCheck %s -check-prefix=CHECK-PART-EXPR1

// RUN: %target-swift-ide-test -range -pos=31:7 -end-pos=31:12 -source-filename %s | %FileCheck %s -check-prefix=CHECK-OEE-EXPR

// RUN: %target-swift-ide-test -range -pos=35:1 -end-pos=38:1 -source-filename %s | %FileCheck %s -check-prefix=CHECK-MTEE-EXPR-1
// RUN: %target-swift-ide-test -range -pos=35:27 -end-pos=35:34 -source-filename %s | %FileCheck %s -check-prefix=CHECK-MTEE-EXPR-2
// RUN: %target-swift-ide-test -range -pos=35:36 -end-pos=37:4 -source-filename %s | %FileCheck %s -check-prefix=CHECK-MTEE-EXPR-3
// RUN: %target-swift-ide-test -range -pos=44:23 -end-pos=44:28 -source-filename %s | %FileCheck %s -check-prefix=CHECK-DEFAULT-ARGUMENT

// CHECK-PART-EXPR: <Kind>PartOfExpression</Kind>
// CHECK-PART-EXPR-NEXT: <Content>getSelf()</Content>
// CHECK-PART-EXPR-NEXT: <Context>swift_ide_test.(file).foo1(_:)</Context>
// CHECK-PART-EXPR-NEXT: <Parent>Call</Parent>
// CHECK-PART-EXPR-NEXT: <ASTNodes>1</ASTNodes>
// CHECK-PART-EXPR-NEXT: <end>

// CHECK-PART-EXPR1: <Kind>PartOfExpression</Kind>
// CHECK-PART-EXPR2: <Content>getSelf(1)</Content>
// CHECK-PART-EXPR2: <Context>swift_ide_test.(file).foo1(_:)</Context>
// CHECK-PART-EXPR2: <Parent>Call</Parent>
// CHECK-PART-EXPR2: <ASTNodes>2</ASTNodes>
// CHECK-PART-EXPR2: <end>

// CHECK-OEE-EXPR: <Kind>SingleExpression</Kind>
// CHECK-OEE-EXPR-NEXT: <Content>x.bar</Content>
// CHECK-OEE-EXPR-NEXT: <Type>String</Type><Exit>false</Exit>
// CHECK-OEE-EXPR-NEXT: <Context>swift_ide_test.(file).foo(x:)</Context>
// CHECK-OEE-EXPR-NEXT: <ASTNodes>1</ASTNodes>
// CHECK-OEE-EXPR-NEXT: <end>

// CHECK-MTEE-EXPR-1: <Kind>SingleExpression</Kind>
// CHECK-MTEE-EXPR-1-NEXT: <Content>withoutActuallyEscaping(closure) { escapable in
// CHECK-MTEE-EXPR-1-NEXT:     _ = escapable
// CHECK-MTEE-EXPR-1-NEXT:   }</Content>
// CHECK-MTEE-EXPR-1-NEXT: <Type>()</Type><Exit>false</Exit>
// CHECK-MTEE-EXPR-1-NEXT: <Context>swift_ide_test.(file).testWithoutActuallyEscaping(closure:)</Context>
// CHECK-MTEE-EXPR-1-NEXT: <Declared>escapable</Declared><OutscopeReference>false</OutscopeReference>
// CHECK-MTEE-EXPR-1-NEXT: <Referenced>closure</Referenced><Type>(Int) -> Void</Type>
// CHECK-MTEE-EXPR-1-NEXT: <Referenced>escapable</Referenced><Type>(Int) -> Void</Type>
// CHECK-MTEE-EXPR-1-NEXT: <ASTNodes>1</ASTNodes>
// CHECK-MTEE-EXPR-1-NEXT: <end>

// CHECK-MTEE-EXPR-2: <Kind>SingleExpression</Kind>
// CHECK-MTEE-EXPR-2-NEXT: <Content>closure</Content>
// CHECK-MTEE-EXPR-2-NEXT: <Type>(Int) -> Void</Type><Exit>false</Exit>
// CHECK-MTEE-EXPR-2-NEXT: <Context>swift_ide_test.(file).testWithoutActuallyEscaping(closure:)</Context>
// CHECK-MTEE-EXPR-2-NEXT: <Referenced>closure</Referenced><Type>(Int) -> Void</Type>
// CHECK-MTEE-EXPR-2-NEXT: <ASTNodes>1</ASTNodes>
// CHECK-MTEE-EXPR-2-NEXT: <end>

// CHECK-MTEE-EXPR-3: <Kind>SingleExpression</Kind>
// CHECK-MTEE-EXPR-3-NEXT: <Content>{ escapable in
// CHECK-MTEE-EXPR-3-NEXT:     _ = escapable
// CHECK-MTEE-EXPR-3-NEXT:   }</Content>
// CHECK-MTEE-EXPR-3-NEXT: <Type>(@escaping (Int) -> Void) -> ()</Type><Exit>false</Exit>
// CHECK-MTEE-EXPR-3-NEXT: <Context>swift_ide_test.(file).testWithoutActuallyEscaping(closure:)</Context>
// CHECK-MTEE-EXPR-3-NEXT: <Declared>escapable</Declared><OutscopeReference>false</OutscopeReference>
// CHECK-MTEE-EXPR-3-NEXT: <Referenced>escapable</Referenced><Type>(Int) -> Void</Type>
// CHECK-MTEE-EXPR-3-NEXT: <ASTNodes>1</ASTNodes>
// CHECK-MTEE-EXPR-3-NEXT: <end>

// CHECK-DEFAULT-ARGUMENT: <Kind>SingleExpression</Kind>
// CHECK-DEFAULT-ARGUMENT-NEXT: <Content>.main</Content>
// CHECK-DEFAULT-ARGUMENT-NEXT: <Type>SR12957</Type><Exit>false</Exit>
// CHECK-DEFAULT-ARGUMENT-NEXT: <Context>swift_ide_test.(file).bar(x:)</Context>
// CHECK-DEFAULT-ARGUMENT-NEXT: <ASTNodes>1</ASTNodes>
// CHECK-DEFAULT-ARGUMENT-NEXT: <end>

