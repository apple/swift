// RUN: %target-swift-frontend %s -emit-ir -g -o - | %FileCheck %s

class CGImageRef {}
class UIImage {
  init() {}
  var CGImage : CGImageRef? {
    get { return self.CGImage }
  }
}
class NSDictionary {}
class CIFilter {
  init (name: String) {}
}

class MyClass {
  // CHECK: define hidden {{.*}} ptr @"$s7iuo_arg7MyClassC11filterImageyAA7UIImageCAFSg_SbtF"
  func filterImage(_ image: UIImage!, _ doSomething:Bool) -> UIImage
	{
    // Test that image is in an alloca, but not an indirect location.
    // CHECK: call void @llvm.dbg.declare(metadata ptr %[[ALLOCA:.*]], metadata ![[IMAGE:.*]], metadata !DIExpression())
    // CHECK: store {{(i32|i64)}} %0, ptr %[[ALLOCA]], align
    // CHECK: ![[IMAGE]] = !DILocalVariable(name: "image", arg: 1
    // CHECK-NOT:                           flags:
    // CHECK-SAME:                          line: [[@LINE-7]]
    // CHECK-NOT:                           flags:
    // CHECK-SAME:                          ){{$}}
		let filter = CIFilter(name: "CIGaussianBlur")
		return image
	}
}

let a = MyClass()
let img = a.filterImage(UIImage(), true)

