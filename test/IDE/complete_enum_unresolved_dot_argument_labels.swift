// RUN: %empty-directory(%t)
// RUN: %target-swift-ide-test -batch-code-completion -source-filename %s -filecheck %raw-FileCheck -completion-output-dir %t

enum DragState {
  case inactive
  case dragging(translationX: Int, translationY: Int)
  case defaulted(x: Int = 2, y: Int = 3, z: Int, extra: Int)
}

func testInit() {
  var state = DragState.inactive
  state = .dragging(#^SIGNATURE^#)
  // SIGNATURE: Begin completions, 1 item
  // SIGNATURE: Pattern/CurrNominal/Flair[ArgLabels]/TypeRelation[Convertible]: ['(']{#translationX: Int#}, {#translationY: Int#}[')'][#DragState#];

  state = .dragging(translationX: 2, #^ARGUMENT^#)
  // ARGUMENT: Begin completions, 1 item
  // ARGUMENT: Pattern/Local/Flair[ArgLabels]: {#translationY: Int#}[#Int#];

  state = .defaulted(#^DEFAULTED^#)
  // DEFAULTED: Begin completions, 1 items
  // DEFAULTED: Pattern/CurrNominal/Flair[ArgLabels]/TypeRelation[Convertible]: ['(']{#x: Int#}, {#y: Int#}, {#z: Int#}, {#extra: Int#}[')'][#DragState#];

  state = .defaulted(x: 1, #^DEFAULTEDARG^#)
  state = .defaulted(x: "Wrong type", #^ERRORTYPE?check=DEFAULTEDARG^#)
  // DEFAULTEDARG: Begin completions, 2 items
  // DEFAULTEDARG: Pattern/Local/Flair[ArgLabels]:     {#y: Int#}[#Int#];
  // DEFAULTEDARG: Pattern/Local/Flair[ArgLabels]:     {#z: Int#}[#Int#];

  state = .defaulted(wrongLabel: 2, #^ERRORARG^#)
  // ERRORARG: Begin completions, 3 items
  // ERRORARG: Pattern/Local/Flair[ArgLabels]:     {#x: Int#}[#Int#];
  // ERRORARG: Pattern/Local/Flair[ArgLabels]:     {#y: Int#}[#Int#];
  // ERRORARG: Pattern/Local/Flair[ArgLabels]:     {#z: Int#}[#Int#];
}

func testMatch() {
  var state = DragState.inactive
  let localInt = 42
  switch state {
  case .dragging(translationX: 2, #^MATCH_ARGY^#):
    // FIXME: This should have an identical type relation
    // MATCH_ARGY: Decl[LocalVar]/Local/TypeRelation[Convertible]: localInt[#Int#]; name=localInt
    // FIXME: We should offer 'translationY:' (without any flair since it's optional), `let translationY`, and `_`
    break
  default:
    break
  }
}
