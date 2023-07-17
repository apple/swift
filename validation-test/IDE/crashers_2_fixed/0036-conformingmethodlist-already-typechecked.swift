// RUN: %target-swift-ide-test -conforming-methods -source-filename %s -code-completion-token COMPLETE

// This test used to crash while PostfixExpr completion was migrated to solver-based.

struct MyPublisher {
  func removeDuplicates() {}
}

func handleEvents(receiveOutput: ((String) -> Void)? = nil) -> MyPublisher {}

protocol AnyCancellable {}

class CategoriesSearchViewModel {
  func foo() {
    var searchCancellable: AnyCancellable = handleEvents(receiveOutput: { [weak self] _ in }).removeDuplicates #^COMPLETE^#
  }
}
