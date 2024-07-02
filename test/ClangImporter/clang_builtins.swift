// RUN: not %target-swift-frontend -typecheck %s 2>&1 | %FileCheck -check-prefix=CHECK -check-prefix=CHECK-%target-runtime %s

#if canImport(Darwin)
  import Darwin
#elseif canImport(Glibc)
  import Glibc
#elseif canImport(Android)
  import Android
#elseif os(WASI)
  import WASILibc
#elseif os(Windows)
  import CRT
#else
#error("Unsupported platform")
#endif

func test() {
  let _: Int = strxfrm
  // CHECK: [[@LINE-1]]:16: error: cannot convert value of type '({{.+}}) -> Int'{{( [(]aka .+[)])?}} to specified type 'Int'

  let _: Int = strcspn
  // CHECK: [[@LINE-1]]:16: error: cannot convert value of type '({{.+}}) -> Int'{{( [(]aka .+[)])?}} to specified type 'Int'

  let _: Int = strspn
  // CHECK: [[@LINE-1]]:16: error: cannot convert value of type '({{.+}}) -> Int'{{( [(]aka .+[)])?}} to specified type 'Int'

  let _: Int = strlen
  // CHECK: [[@LINE-1]]:16: error: cannot convert value of type '({{.+}}) -> Int'{{( [(]aka .+[)])?}} to specified type 'Int'
}

#if canImport(Darwin)
// These functions aren't consistently available across platforms, so only
// test for them on Apple platforms.
func testApple() {
  let _: Int = strlcpy
  // CHECK-objc: [[@LINE-1]]:16: error: cannot convert value of type '({{.+}}) -> Int'{{( [(]aka .+[)])?}} to specified type 'Int'

  let _: Int = strlcat
  // CHECK-objc: [[@LINE-1]]:16: error: cannot convert value of type '({{.+}}) -> Int'{{( [(]aka .+[)])?}} to specified type 'Int'

  // wcslen is different: it wasn't a builtin until Swift 4, and so its return
  // type has always been 'Int'.
  let _: Int = wcslen
  // CHECK-objc: [[@LINE-1]]:16: error: cannot convert value of type '({{.+}}) -> Int'{{( [(]aka .+[)])?}} to specified type 'Int'
}
#endif
