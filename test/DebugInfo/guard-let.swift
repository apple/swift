// RUN: %target-swift-frontend %s -c -emit-ir -g -o - | \
// RUN:   %FileCheck %s --check-prefix=CHECK1
// RUN: %target-swift-frontend %s -c -emit-ir -g -o - | \
// RUN:   %FileCheck %s --check-prefix=CHECK2
// RUN: %target-swift-frontend %s -c -emit-ir -g -o - | \
// RUN:   %FileCheck %s --check-prefix=CHECK3

// UNSUPPORTED: OS=watchos

// With large type optimizations the string is passed indirectly on the
// following architectures so there is no shadow copy happening. As this
// tests that we're emitting the DI correctly, we can skip running on them.
// UNSUPPORTED: CPU=i386
// UNSUPPORTED: CPU=armv7
// UNSUPPORTED: CPU=armv7s
// UNSUPPORTED: CPU=armv7k

func use<T>(_ t: T) {}

public func f(_ i : Int?)
{
  // CHECK1-LABEL: define {{.*}}@"$s4main1fyySiSgF"
  // CHECK1: %[[alloca:.*]] = alloca %TSiSg
  // CHECK1: @llvm.dbg.declare(metadata ptr %i.debug
  // CHECK1: call void @llvm.memset{{.*}}(ptr align {{(4|8)}} %[[alloca]],
  // CHECK1-SAME:                         i8 0, i64 {{(5|9)}}, i1 false){{$}}
  // CHECK1: @llvm.dbg.declare(metadata ptr %val.debug,
  // CHECK1-SAME:              !dbg ![[DBG0:.*]]
  // CHECK1-LABEL: define {{.*}}@"$s4main1gyySSSgF"
  // CHECK1: ![[F:.*]] = distinct !DISubprogram(name: "f",
  // CHECK1: ![[BLK:.*]] = distinct !DILexicalBlock(scope: ![[F]],
  // CHECK1: ![[DBG0]] = !DILocation(line: [[@LINE+1]],
  guard let val = i else { return }
  use(val)
}

public func g(_ s : String?)
{
  // CHECK2: define {{.*}}@"$s4main1gyySSSgF"
  // CHECK2: %[[alloca:.*]] = alloca %TSSSg
  // CHECK2: @llvm.dbg.declare(metadata ptr
  // CHECK2: %val.debug = alloca %TSS
  // CHECK2: @llvm.dbg.declare(metadata ptr
  // CHECK2: call void @llvm.memset.{{.*}}(ptr align {{(4|8)}} %[[alloca]], i8 0
  // CHECK2: ![[G:.*]] = distinct !DISubprogram(name: "g"
  guard let val = s else { return }
  use(val)
}

public func h(_ s : String?)
{
  // CHECK3: define {{.*}}@"$s4main1hyySSSgF"
  // CHECK3: %s.debug = alloca %TSSSg
  // CHECK3: @llvm.dbg.declare(metadata ptr
  // CHECK3: %[[alloca:.*]] = alloca %TSS
  // CHECK3: @llvm.dbg.declare(metadata ptr
  // CHECK3: call void @llvm.memset.{{.*}}(ptr align {{(4|8)}} %[[alloca]], i8 0
  // CHECK3: ![[G:.*]] = distinct !DISubprogram(name: "h"
  guard let s = s else { return }
  use(s)
}

enum MyError : Error {
  case bad
}

enum Stuff {
  case array([Stuff])
  case any(Any)
  case nothing

  func toArray() throws -> [Stuff] {
    guard case .array(let array) = self else { throw MyError.bad }
    return array
  }
}
