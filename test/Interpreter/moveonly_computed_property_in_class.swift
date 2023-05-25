// RUN: %target-run-simple-swift(-Xfrontend -sil-verify-all)
// RUN: %target-run-simple-swift(-O -Xfrontend -sil-verify-all)

// REQUIRES: executable_test
@_moveOnly
struct FileDescriptor {
  let desc: Int

  var empty: Bool { return desc == Int.min }
  func isEmpty() -> Bool { return empty }
}

final class Wrapper {
  var val: FileDescriptor = FileDescriptor(desc: 0)

  func isEmpty_bug() -> Bool {
    // error: 'self.val' has consuming use that cannot 
    // be eliminated due to a tight exclusivity scope
    return val.empty // note: consuming use here
  }

  func isEmpty_ok() -> Bool {
    return val.isEmpty()
  }
}

let w = Wrapper()
// CHECK: is not empty
print(w.isEmpty_bug() ? "is empty" : "is not empty")
w.val = FileDescriptor(desc: Int.min)
// CHECK: is empty
print(w.isEmpty_bug() ? "is empty" : "is not empty")
