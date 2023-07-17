// RUN: %empty-directory(%t)
// RUN: %target-swift-ide-test -batch-code-completion -source-filename %s -filecheck %raw-FileCheck -completion-output-dir %t

struct MyStruct {
  func takeAnotherClosure(_ y: () -> Void) {}
}

func takeClosure(_ x: () -> Void) -> MyStruct {}

func foo() {
  takeClosure {
    #^COMPLETE^#
  }.takeAnotherClosure {
    if true {
      1
    } else {
      1
    }
  }
}

// COMPLETE: Begin completions
// COMPLETE: Decl[Struct]/CurrModule:            MyStruct[#MyStruct#]; name=MyStruct
// COMPLETE: End completions
