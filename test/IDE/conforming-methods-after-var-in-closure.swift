// Check that we don't crash
// RUN: %target-swift-ide-test -conforming-methods -source-filename %s -code-completion-token=TOK

func takeClosure(_ x: () -> Void) {}

func test(name: String?) {
  takeClosure {
    guard let url#^TOK^# = name else {
    }
  }
}
