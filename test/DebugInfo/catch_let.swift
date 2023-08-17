// RUN: %target-swift-frontend %s -emit-ir -g -o - | %FileCheck %s

enum MyError : Error {
  case Yikes
}

func throwing() throws -> () {
  throw MyError.Yikes
}

func use<T>(_ t: T) {}

// CHECK-LABEL: define {{.*}}explicitBinding{{.*}}
public func explicitBinding() {
  do {
    try throwing()
  }
  catch let error {
    // CHECK: call void @llvm.dbg.declare(metadata ptr %{{.*}}, metadata ![[EXPLICIT_ERROR:[0-9]+]],
    use(error)
  }
}
explicitBinding()

// CHECK-LABEL: define {{.*}}implicitBinding{{.*}}
public func implicitBinding() {
  do {
    try throwing()
  }
  catch {
    // CHECK: call void @llvm.dbg.declare(metadata ptr %{{.*}}, metadata ![[IMPLICIT_ERROR:[0-9]+]],
    use(error)
  }
}
implicitBinding()

// CHECK-LABEL: define {{.*}}multiBinding{{.*}}
public func multiBinding() {
  do {
    try throwing()
  }
  catch let error as MyError, let error as MyError {
    // CHECK: call void @llvm.dbg.declare(metadata ptr %{{.*}}, metadata ![[MULTI_BINDING_ERROR:[0-9]+]],
    // CHECK-NOT: call void @llvm.dbg.declare(metadata ptr %{{.*}}
    // CHECK: define {{.*}}MyError{{.*}}
    use(error)
  } catch {
    use(error)
  }
}
multiBinding()

// CHECK: ![[EXPLICIT_ERROR]] = !DILocalVariable(name: "error"
// CHECK: ![[IMPLICIT_ERROR]] = !DILocalVariable(name: "error"
// CHECK: ![[MULTI_BINDING_ERROR]] = !DILocalVariable(name: "error"
