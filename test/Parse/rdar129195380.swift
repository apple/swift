// RUN: not %target-swift-frontend -experimental-skip-all-function-bodies -dump-parse %s | %FileCheck %s

// rdar://129195380 - Make sure the skipping logic can handle #if.
struct S {
  // CHECK: func_decl{{.*}}:[[@LINE+1]]:3 - line:[[@LINE+11]]:3{{.*}}"foo()"
  func foo() {
#if true
  }
#if true
  func bar() {
#else
  }
#endif
  }
#endif
  }
  // CHECK: func_decl{{.*}}:[[@LINE+1]]:3 - line:[[@LINE+1]]:15{{.*}}"baz()"
  func baz() {}
}

// The '#if' is unterminated here, so swallows the rest of the file.
// CHECK: struct_decl{{.*}}:[[@LINE+1]]:1 - line:[[@LINE+14]]:14{{.*}}"R"
struct R {
#if false
  }
#if true
  }
#endif
  }
#else
  }
  // CHECK-NOT: qux
  func qux() {}
}

func flim() {}
