// RUN: %target-swift-frontend -emit-sil -verify %s
// RUN: %target-swift-frontend -emit-sil -O -verify %s

// rdar://110675352

struct NonCopyableStruct: ~Copyable {}

var globFn: () -> () = {}
func forceEscaping(_ esc: @escaping () -> ()) {
  globFn = esc
}

func closureDiagnosticsSimple() {
  var s = NonCopyableStruct()
  let f = {
    _ = consume s
    s = NonCopyableStruct()
  }
  f()
}
