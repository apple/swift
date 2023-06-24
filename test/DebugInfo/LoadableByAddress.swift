// RUN: %target-swift-frontend %use_no_opaque_pointers %s -module-name A -emit-ir -g -o - | %FileCheck %s
// RUN: %target-swift-frontend %s -module-name A -emit-ir -g -o -

// REQUIRES: CPU=x86_64
public struct Continuation<A> {
   private let magicToken = "Hello World"
   fileprivate let f: (() -> A)?

   private let _makeMeBigger = 0

  public func run() {}
}

public typealias ContinuationU = Continuation<()>

// CHECK: %2 = alloca %T1A12ContinuationV, align 8
// CHECK-NEXT: call void @llvm.dbg.declare(metadata %T1A12ContinuationV* %2,
// CHECK-SAME:    metadata ![[X:.*]], metadata !DIExpression())
// CHECK: ![[X]] = !DILocalVariable(name: "x",

public func f<A>(_ xs: [Continuation<A>]) -> (() -> A?) {
   return {
       for x in xs {
           x.run()
       }
       return nil
   }
}

