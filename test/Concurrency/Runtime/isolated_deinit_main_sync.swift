// RUN: %target-run-simple-swift(-Xfrontend -disable-availability-checking) | %FileCheck %s

var isDead: Bool = false

public class Foo {
  @MainActor
  deinit {
    print("DEINIT")
    isDead = true
  }
}

func main() {
  print("isDead = \(isDead)")
  do {
    _ = Foo()
  }
  print("isDead = \(isDead)")
}

// CHECK: isDead = false
// CHECK: DEINIT
// CHECK: isDead = true
main()
