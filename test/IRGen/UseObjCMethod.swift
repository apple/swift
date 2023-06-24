// RUN: %target-swift-frontend -import-objc-header %S/Inputs/StaticInline.h %s -emit-ir | %FileCheck %s

// REQUIRES: objc_interop
import Foundation

public class Demo: NSObject {
  public override init() {
          super.init()

          print(Demo.locale)
  }
  // This used to crash a -O.
  private static let locale = staticInlineFun()

}

@inline(never)
func testDemo() {
  let a = Demo()
}

testDemo()

// Make sure the clang importer puts the selectors and co into the llvm.compiler used variable.

// CHECK: @llvm.compiler.used = appending global [{{.*}} x ptr] [{{.*}} @"OBJC_CLASSLIST_REFERENCES_$_"{{.*}}@OBJC_METH_VAR_NAME_{{.*}}@OBJC_SELECTOR_REFERENCES_{{.*}}@OBJC_METH_VAR_NAME_.{{.*}}@OBJC_SELECTOR_REFERENCES_.{{.*}}]

