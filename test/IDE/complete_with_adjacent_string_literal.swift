// RUN: %target-swift-ide-test -batch-code-completion -source-filename %s -filecheck %raw-FileCheck -completion-output-dir %t

func takeClosure(x: () -> Void) {}
func takeString(_ a: String) -> MyStruct {}

struct MyStruct {
  func style(arg: Int) {}
}

func foo() {
  takeClosure {
    takeString("\(1)")
      .style(#^COMPLETE^#)
  }
}

// COMPLETE: Begin completions, 1 items
// COMPLETE: Decl[InstanceMethod]/CurrNominal/Flair[ArgLabels]/TypeRelation[Convertible]: ['(']{#arg: Int#}[')'][#Void#];
// COMPLETE: End completions

