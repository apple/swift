// RUN: %empty-directory(%t)
// RUN: %target-swift-ide-test -batch-code-completion -source-filename %s -filecheck %raw-FileCheck -completion-output-dir %t

{
  1.#^LITERAL1^#
}
// LITERAL1-DAG:      Decl[InstanceVar]/Super/IsSystem:       bigEndian[#Int#]; name=bigEndian{{$}}
// LITERAL1-DAG:      Decl[InstanceVar]/Super/IsSystem:       littleEndian[#Int#]; name=littleEndian{{$}}
// LITERAL1-DAG:      Decl[InstanceVar]/CurrNominal/IsSystem: byteSwapped[#Int#]; name=byteSwapped{{$}}
// LITERAL1-DAG:      Decl[InstanceVar]/CurrNominal/IsSystem: nonzeroBitCount[#Int#]; name=nonzeroBitCount{{$}}

{
  1.1.#^LITERAL2^#
}
// LITERAL2-DAG:     Decl[InstanceVar]/CurrNominal/IsSystem: isNormal[#Bool#]; name=isNormal{{$}}
// LITERAL2-DAG:     Decl[InstanceVar]/CurrNominal/IsSystem: isFinite[#Bool#]; name=isFinite{{$}}
// LITERAL2-DAG:     Decl[InstanceVar]/CurrNominal/IsSystem: isZero[#Bool#]; name=isZero{{$}}
// LITERAL2-DAG:     Decl[InstanceVar]/CurrNominal/IsSystem: isSubnormal[#Bool#]; name=isSubnormal{{$}}
// LITERAL2-DAG:     Decl[InstanceVar]/CurrNominal/IsSystem: isInfinite[#Bool#]; name=isInfinite{{$}}
// LITERAL2-DAG:     Decl[InstanceVar]/CurrNominal/IsSystem: isNaN[#Bool#]; name=isNaN{{$}}

{
  true.#^LITERAL3^#
}
// LITERAL3-DAG:     Decl[InstanceVar]/CurrNominal/IsSystem: description[#String#]; name=description{{$}}
// LITERAL3-DAG:     Decl[InstanceVar]/CurrNominal/IsSystem: hashValue[#Int#]; name=hashValue{{$}}

{
  "swift".#^LITERAL4^#
}

// LITERAL4-DAG:     Decl[InstanceMethod]/CurrNominal/IsSystem: withCString({#(body): (UnsafePointer<Int8>) throws -> Result##(UnsafePointer<Int8>) throws -> Result#})[' rethrows'][#Result#]; name=withCString(:){{$}}

// FIXME: we should show the qualified String.Index type.
// rdar://problem/20788802
// LITERAL4-DAG:     Decl[InstanceVar]/CurrNominal/IsSystem:    startIndex[#String.Index#]; name=startIndex{{$}}
// LITERAL4-DAG:     Decl[InstanceVar]/CurrNominal/IsSystem:    endIndex[#String.Index#]; name=endIndex{{$}}
// LITERAL4-DAG:     Decl[InstanceMethod]/CurrNominal/IsSystem: append({#(c): Character#})[#Void#]; name=append(:){{$}}
// LITERAL4-DAG:     Decl[InstanceMethod]/CurrNominal/IsSystem: append({#contentsOf: Sequence#})[#Void#]; name=append(contentsOf:){{$}}
// LITERAL4-DAG:     Decl[InstanceMethod]/CurrNominal/IsSystem: insert({#contentsOf: Collection#}, {#at: String.Index#})[#Void#]; name=insert(contentsOf:at:){{$}}
// LITERAL4-DAG:     Decl[InstanceMethod]/CurrNominal/IsSystem: remove({#at: String.Index#})[#Character#]; name=remove(at:){{$}}
// LITERAL4-DAG:     Decl[InstanceMethod]/CurrNominal/IsSystem: lowercased()[#String#]; name=lowercased(){{$}}

func giveMeAString() -> Int {
  // rdar://22637799
  return "Here's a string".#^LITERAL5^# // try .characters.count here
}

// LITERAL5-DAG:     Decl[InstanceVar]/CurrNominal/IsSystem:      endIndex[#String.Index#]{{; name=.+$}}
// LITERAL5-DAG:     Decl[InstanceMethod]/CurrNominal/IsSystem/TypeRelation[Invalid]: reserveCapacity({#(n): Int#})[#Void#]{{; name=.+$}}
// LITERAL5-DAG:     Decl[InstanceMethod]/CurrNominal/IsSystem/TypeRelation[Invalid]: append({#(c): Character#})[#Void#]{{; name=.+$}}
// LITERAL5-DAG:     Decl[InstanceMethod]/CurrNominal/IsSystem/TypeRelation[Invalid]: append({#contentsOf: Sequence#})[#Void#]{{; name=.+$}}

struct MyColor: _ExpressibleByColorLiteral {
  init(_colorLiteralRed: Float, green: Float, blue: Float, alpha: Float) { red = colorLiteralRed }
  var red: Float
}
public typealias _ColorLiteralType = MyColor
func testColor11() {
  let y: MyColor
  y = #colorLiteral(red: 1.0, green: 0.1, blue: 0.5, alpha: 1.0).#^LITERAL6^#
}
// LITERAL6: Decl[InstanceVar]/CurrNominal:      red[#Float#]; name=red
func testColor12() {
  let y: MyColor
  y = #colorLiteral(red: 1.0, green: 0.1, blue: 0.5, alpha: 1.0) #^LITERAL7^#
}
// LITERAL7: Decl[InstanceVar]/CurrNominal:      .red[#Float#]; name=red

func testArray(f1: Float) {
  _ = [1, 2, f1] #^LITERAL8^#
}
// LITERAL8-DAG: Decl[InstanceVar]/CurrNominal/IsSystem: .count[#Int#]; name=count
// LITERAL8-DAG: Decl[InstanceVar]/Super/IsSystem:       .first[#Any?#]; name=first

func testDict(f1: Float) {
  _ = ["foo": f1, "bar": "baz"] #^LITERAL9^#
}
// LITERAL9-DAG: Decl[InstanceVar]/CurrNominal/IsSystem: .keys[#Dictionary<String, Any>.Keys#]; name=keys
// LITERAL9-DAG: Decl[InstanceVar]/CurrNominal/IsSystem: .isEmpty[#Bool#]; name=isEmpty

func testEditorPlaceHolder() {
  _ = <#T##foo##String#> #^LITERAL10^#
}
// LITERAL10-DAG: Decl[InstanceVar]/CurrNominal/IsSystem: .utf16[#String.UTF16View#]; name=utf16
// LITERAL10-DAG: Decl[InstanceVar]/CurrNominal/IsSystem: .utf8[#String.UTF8View#]; name=utf8
