// RUN: %target-run-simple-swift(-Xfrontend -sil-verify-all)
// RUN: %target-run-simple-swift(-O -Xfrontend -sil-verify-all)

// REQUIRES: executable_test

/// A unique value represented by a heap memory location.
struct Handle: ~Copyable {
  var address: UnsafeMutableRawPointer

  init() {
    self.address = .allocate(byteCount: 2, alignment: 2)
  }

  consuming func done() {
    let address = self.address
    discard self
    address.deallocate()
    print("deallocated handle via done()")
  }

  deinit {
    address.deallocate()
    print("deallocated handle via deinit")
  }
}

func description(of pointer: UnsafeRawPointer) -> String {
  let address = UInt(bitPattern: pointer)
  return "0x" + String(address, radix: 16)
}

func borrowHandleAndCrashNoMore(_ handle: borrowing Handle) -> String {
  var string = ""
  return string.withUTF8 { _ in
    description(of: handle.address)
  }
}

let handleDescription: String

do {
  let handle = Handle()
  handleDescription = borrowHandleAndCrashNoMore(handle)
  handle.done()
}

// CHECK: result: 0x
print("result: \(handleDescription)")
