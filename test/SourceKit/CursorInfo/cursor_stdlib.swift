import Foundation

var x = NSUTF8StringEncoding

var d : AnyIterator<Int>

func foo1(_ a : inout [Int]) {
  a = a.sorted()
  a.append(1)
}

struct S1 {}

func foo2(_ a : inout [S1]) {
  a = a.sorted(by: { (a, b) -> Bool in
    return false
  })
  a.append(S1())
}

import Swift
func foo3(a: Float, b: Bool) {}

struct MyColor: _ExpressibleByColorLiteral {
    init(_colorLiteralRed: Float, green: Float, blue: Float, alpha: Float) { red = colorLiteralRed }
    var red: Float
}
public typealias _ColorLiteralType = MyColor
let colorResource = #colorLiteral(red: 0.8549019694, green: 0.250980407, blue: 0.4784313738, alpha: 1)

let arrLiteral = [1, 2, 3]
let arrNonConst = [1, 2, d]

let dictLiteral = [1:2, 3:4]
let dictNonCost = [1:2, 3:d]

// REQUIRES: objc_interop

// RUN: %empty-directory(%t)
// RUN: %build-clang-importer-objc-overlays

// RUN: %sourcekitd-test -req=cursor -pos=3:18 %s -- %s -target %target-triple %clang-importer-sdk-nosource -I %t | %FileCheck -check-prefix=CHECK-OVERLAY %s
// CHECK-OVERLAY:      source.lang.swift.ref.var.global
// CHECK-OVERLAY-NEXT: NSUTF8StringEncoding
// CHECK-OVERLAY-NEXT: s:10Foundation20NSUTF8StringEncodingSuv
// CHECK-OVERLAY-NEXT: source.lang.swift
// CHECK-OVERLAY-NEXT: UInt
// CHECK-OVERLAY-NEXT: $sSuD
// CHECK-OVERLAY-NEXT: Foundation
// CHECK-OVERLAY-NEXT: <Declaration>let NSUTF8StringEncoding: <Type usr="s:Su">UInt</Type></Declaration>

// RUN: %sourcekitd-test -req=cursor -pos=5:13 %s -- %s -target %target-triple %clang-importer-sdk-nosource -I %t | %FileCheck -check-prefix=CHECK-ITERATOR %s
// CHECK-ITERATOR-NOT: _AnyIteratorBase
// CHECK-ITERATOR: <Group>Collection/Type-erased</Group>

// RUN: %sourcekitd-test -req=cursor -pos=8:10 %s -- %s -target %target-triple %clang-importer-sdk-nosource -I %t | %FileCheck -check-prefix=CHECK-REPLACEMENT1 %s
// CHECK-REPLACEMENT1: <Group>Collection/Array</Group>
// CHECK-REPLACEMENT1: <Declaration>{{.*}}func sorted() -&gt; [<Type usr="s:Si">Int</Type>]</Declaration>
// CHECK-REPLACEMENT1: RELATED BEGIN
// CHECK-REPLACEMENT1: sorted(by:)</RelatedName>
// CHECK-REPLACEMENT1: RELATED END

// RUN: %sourcekitd-test -req=cursor -pos=9:8 %s -- %s -target %target-triple %clang-importer-sdk-nosource -I %t | %FileCheck -check-prefix=CHECK-REPLACEMENT2 %s
// CHECK-REPLACEMENT2: <Group>Collection/Array</Group>
// CHECK-REPLACEMENT2: <Declaration>{{.*}}mutating func append(_ newElement: <Type usr="s:Si">Int</Type>)</Declaration>

// RUN: %sourcekitd-test -req=cursor -pos=15:10 %s -- %s -target %target-triple %clang-importer-sdk-nosource -I %t | %FileCheck -check-prefix=CHECK-REPLACEMENT3 %s
// CHECK-REPLACEMENT3: <Group>Collection/Array</Group>
// CHECK-REPLACEMENT3: func sorted(by areInIncreasingOrder: (<Type usr="s:13cursor_stdlib2S1V">S1</Type>
// CHECK-REPLACEMENT3: sorted()</RelatedName>

// RUN: %sourcekitd-test -req=cursor -req-opts=retrieve_symbol_graph=1 -pos=18:8 %s -- %s -target %target-triple %clang-importer-sdk-nosource -I %t | %FileCheck -check-prefix=CHECK-REPLACEMENT4 %s
// CHECK-REPLACEMENT4: <Group>Collection/Array</Group>
// CHECK-REPLACEMENT4: <Declaration>{{.*}}mutating func append(_ newElement: <Type usr="s:13cursor_stdlib2S1V">S1</Type>)</Declaration>
// CHECK-REPLACEMENT4: SYMBOL GRAPH BEGIN
// CHECK-REPLACEMENT4: {
// CHECK-REPLACEMENT4:   "module": {
// CHECK-REPLACEMENT4:     "name": "Swift",
// CHECK-REPLACEMENT4:   },
// CHECK-REPLACEMENT4:   "relationships": [
// CHECK-REPLACEMENT4:     {
// CHECK-REPLACEMENT4:       "kind": "memberOf",
// CHECK-REPLACEMENT4:       "source": "s:Sa6appendyyxnF",
// CHECK-REPLACEMENT4:       "target": "s:Sa"
// CHECK-REPLACEMENT4:     }
// CHECK-REPLACEMENT4:   ],
// CHECK-REPLACEMENT4:   "symbols": [
// CHECK-REPLACEMENT4:     {
// CHECK-REPLACEMENT4:       "accessLevel": "public",
// CHECK-REPLACEMENT4:       "declarationFragments": [
// CHECK-REPLACEMENT4:         {
// CHECK-REPLACEMENT4:           "kind": "keyword",
// CHECK-REPLACEMENT4:           "spelling": "mutating"
// CHECK-REPLACEMENT4:         },
// CHECK-REPLACEMENT4:         {
// CHECK-REPLACEMENT4:           "kind": "text",
// CHECK-REPLACEMENT4:           "spelling": " "
// CHECK-REPLACEMENT4:         },
// CHECK-REPLACEMENT4:         {
// CHECK-REPLACEMENT4:           "kind": "keyword",
// CHECK-REPLACEMENT4:           "spelling": "func"
// CHECK-REPLACEMENT4:         },
// CHECK-REPLACEMENT4:         {
// CHECK-REPLACEMENT4:           "kind": "text",
// CHECK-REPLACEMENT4:           "spelling": " "
// CHECK-REPLACEMENT4:         },
// CHECK-REPLACEMENT4:         {
// CHECK-REPLACEMENT4:           "kind": "identifier",
// CHECK-REPLACEMENT4:           "spelling": "append"
// CHECK-REPLACEMENT4:         },
// CHECK-REPLACEMENT4:         {
// CHECK-REPLACEMENT4:           "kind": "text",
// CHECK-REPLACEMENT4:           "spelling": "("
// CHECK-REPLACEMENT4:         },
// CHECK-REPLACEMENT4:         {
// CHECK-REPLACEMENT4:           "kind": "externalParam",
// CHECK-REPLACEMENT4:           "spelling": "_"
// CHECK-REPLACEMENT4:         },
// CHECK-REPLACEMENT4:         {
// CHECK-REPLACEMENT4:           "kind": "text",
// CHECK-REPLACEMENT4:           "spelling": " "
// CHECK-REPLACEMENT4:         },
// CHECK-REPLACEMENT4:         {
// CHECK-REPLACEMENT4:           "kind": "internalParam",
// CHECK-REPLACEMENT4:           "spelling": "newElement"
// CHECK-REPLACEMENT4:         },
// CHECK-REPLACEMENT4:         {
// CHECK-REPLACEMENT4:           "kind": "text",
// CHECK-REPLACEMENT4:           "spelling": ": "
// CHECK-REPLACEMENT4:         },
// CHECK-REPLACEMENT4:         {
// CHECK-REPLACEMENT4:           "kind": "typeIdentifier",
// CHECK-REPLACEMENT4:           "preciseIdentifier": "s:13cursor_stdlib2S1V",
// CHECK-REPLACEMENT4:           "spelling": "S1"
// CHECK-REPLACEMENT4:         },
// CHECK-REPLACEMENT4:         {
// CHECK-REPLACEMENT4:           "kind": "text",
// CHECK-REPLACEMENT4:           "spelling": ")"
// CHECK-REPLACEMENT4:         }
// CHECK-REPLACEMENT4:       ],
// CHECK-REPLACEMENT4:       "docComment": {
// CHECK-REPLACEMENT4:         "lines": [
// CHECK-REPLACEMENT4:           {
// CHECK-REPLACEMENT4:             "text": "Adds a new element at the end of the array."
// CHECK-REPLACEMENT4:           },
// CHECK-REPLACEMENT4:         ]
// CHECK-REPLACEMENT4:       },
// CHECK-REPLACEMENT4:       "functionSignature": {
// CHECK-REPLACEMENT4:         "parameters": [
// CHECK-REPLACEMENT4:           {
// CHECK-REPLACEMENT4:             "declarationFragments": [
// CHECK-REPLACEMENT4:               {
// CHECK-REPLACEMENT4:                 "kind": "identifier",
// CHECK-REPLACEMENT4:                 "spelling": "newElement"
// CHECK-REPLACEMENT4:               },
// CHECK-REPLACEMENT4:               {
// CHECK-REPLACEMENT4:                 "kind": "text",
// CHECK-REPLACEMENT4:                 "spelling": ": "
// CHECK-REPLACEMENT4:               },
// CHECK-REPLACEMENT4:               {
// CHECK-REPLACEMENT4:                 "kind": "typeIdentifier",
// CHECK-REPLACEMENT4:                 "preciseIdentifier": "s:13cursor_stdlib2S1V",
// CHECK-REPLACEMENT4:                 "spelling": "S1"
// CHECK-REPLACEMENT4:               }
// CHECK-REPLACEMENT4:             ],
// CHECK-REPLACEMENT4:             "name": "newElement"
// CHECK-REPLACEMENT4:           }
// CHECK-REPLACEMENT4:         ],
// CHECK-REPLACEMENT4:         "returns": [
// CHECK-REPLACEMENT4:           {
// CHECK-REPLACEMENT4:             "kind": "text",
// CHECK-REPLACEMENT4:             "spelling": "()"
// CHECK-REPLACEMENT4:           }
// CHECK-REPLACEMENT4:         ]
// CHECK-REPLACEMENT4:       },
// CHECK-REPLACEMENT4:       "identifier": {
// CHECK-REPLACEMENT4:         "interfaceLanguage": "swift",
// CHECK-REPLACEMENT4:         "precise": "s:Sa6appendyyxnF"
// CHECK-REPLACEMENT4:       },
// CHECK-REPLACEMENT4:       "kind": {
// CHECK-REPLACEMENT4:         "displayName": "Instance Method",
// CHECK-REPLACEMENT4:         "identifier": "swift.method"
// CHECK-REPLACEMENT4:       },
// CHECK-REPLACEMENT4:       "names": {
// CHECK-REPLACEMENT4:         "subHeading": [
// CHECK-REPLACEMENT4:           {
// CHECK-REPLACEMENT4:             "kind": "keyword",
// CHECK-REPLACEMENT4:             "spelling": "func"
// CHECK-REPLACEMENT4:           },
// CHECK-REPLACEMENT4:           {
// CHECK-REPLACEMENT4:             "kind": "text",
// CHECK-REPLACEMENT4:             "spelling": " "
// CHECK-REPLACEMENT4:           },
// CHECK-REPLACEMENT4:           {
// CHECK-REPLACEMENT4:             "kind": "identifier",
// CHECK-REPLACEMENT4:             "spelling": "append"
// CHECK-REPLACEMENT4:           },
// CHECK-REPLACEMENT4:           {
// CHECK-REPLACEMENT4:             "kind": "text",
// CHECK-REPLACEMENT4:             "spelling": "("
// CHECK-REPLACEMENT4:           },
// CHECK-REPLACEMENT4:           {
// CHECK-REPLACEMENT4:             "kind": "typeIdentifier",
// CHECK-REPLACEMENT4:             "preciseIdentifier": "s:13cursor_stdlib2S1V",
// CHECK-REPLACEMENT4:             "spelling": "S1"
// CHECK-REPLACEMENT4:           },
// CHECK-REPLACEMENT4:           {
// CHECK-REPLACEMENT4:             "kind": "text",
// CHECK-REPLACEMENT4:             "spelling": ")"
// CHECK-REPLACEMENT4:           }
// CHECK-REPLACEMENT4:         ],
// CHECK-REPLACEMENT4:         "title": "append(_:)"
// CHECK-REPLACEMENT4:       },
// CHECK-REPLACEMENT4:       "pathComponents": [
// CHECK-REPLACEMENT4:         "Array",
// CHECK-REPLACEMENT4:         "append(_:)"
// CHECK-REPLACEMENT4:       ],
// CHECK-REPLACEMENT4:       "swiftExtension": {
// CHECK-REPLACEMENT4:         "extendedModule": "Swift"
// CHECK-REPLACEMENT4:       }
// CHECK-REPLACEMENT4:     }
// CHECK-REPLACEMENT4:   ]
// CHECK-REPLACEMENT4: }
// CHECK-REPLACEMENT4: SYMBOL GRAPH END

// RUN: %sourcekitd-test -req=cursor -pos=21:10 %s -- %s -target %target-triple %clang-importer-sdk-nosource -I %t | %FileCheck -check-prefix=CHECK-MODULE-GROUP1 %s
// CHECK-MODULE-GROUP1: MODULE GROUPS BEGIN
// CHECK-MODULE-GROUP1-DAG: Math
// CHECK-MODULE-GROUP1-DAG: Collection
// CHECK-MODULE-GROUP1-DAG: Collection/Array
// CHECK-MODULE-GROUP1: MODULE GROUPS END

// RUN: %sourcekitd-test -req=cursor -pos=22:17 %s -- %s -target %target-triple %clang-importer-sdk-nosource -I %t | %FileCheck -check-prefix=CHECK-FLOAT1 %s
// CHECK-FLOAT1: s:Sf

// RUN: %sourcekitd-test -req=cursor -pos=22:25 %s -- %s -target %target-triple %clang-importer-sdk-nosource -I %t | %FileCheck -check-prefix=CHECK-BOOL1 %s
// CHECK-BOOL1: s:Sb

// RUN: %sourcekitd-test -req=cursor -pos=29:29 %s -- %s -target %target-triple %clang-importer-sdk-nosource -I %t | %FileCheck -check-prefix=CHECK-OBJ-LITERAL %s
// CHECK-OBJ-LITERAL: source.lang.swift.ref.struct (24:8-24:15)
// CHECK-OBJ-LITERAL-NEXT: MyColor
// CHECK-OBJ-LITERAL-NEXT: s:13cursor_stdlib7MyColorV
// CHECK-OBJ-LITERAL-NEXT: source.lang.swift
// CHECK-OBJ-LITERAL-NEXT: MyColor.Type
// CHECK-OBJ-LITERAL-NEXT: $s13cursor_stdlib7MyColorVmD
// CHECK-OBJ-LITERAL-NEXT: cursor_stdlib
// CHECK-OBJ-LITERAL-NEXT: <Declaration>struct MyColor : <Type usr="s:s26_ExpressibleByColorLiteralP">_ExpressibleByColorLiteral</Type></Declaration>
// CHECK-OBJ-LITERAL-NEXT: <decl.struct><syntaxtype.keyword>struct</syntaxtype.keyword> <decl.name>MyColor</decl.name> : <ref.protocol usr="s:s26_ExpressibleByColorLiteralP">_ExpressibleByColorLiteral</ref.protocol></decl.struct>

// RUN: %sourcekitd-test -req=cursor -pos=31:18 %s -- %s -target %target-triple %clang-importer-sdk-nosource -I %t | %FileCheck -check-prefix=CHECK-ARRAY1 %s
// CHECK-ARRAY1: source.lang.swift.ref.function.constructor
// CHECK-ARRAY1-NEXT: init(arrayLiteral:)
// CHECK-ARRAY1-NEXT: s:Sa12arrayLiteralSayxGxd_tcfc
// CHECK-ARRAY1-NEXT: source.lang.swift
// CHECK-ARRAY1-NEXT: <Element> (Array<Element>.Type) -> (Element...) -> Array<Element>
// CHECK-ARRAY1-NEXT: $s12arrayLiteralSayxGxd_tcD
// CHECK-ARRAY1-NEXT: Swift
// CHECK-ARRAY1-NEXT: <Group>Collection/Array</Group>
// CHECK-ARRAY1-NEXT: SYSTEM

// RUN: %sourcekitd-test -req=cursor -pos=32:19 %s -- %s -target %target-triple %clang-importer-sdk-nosource -I %t | %FileCheck -check-prefix=CHECK-ARRAY2 %s
// CHECK-ARRAY2: source.lang.swift.ref.function.constructor
// CHECK-ARRAY2-NEXT: init(arrayLiteral:)
// CHECK-ARRAY2-NEXT: s:Sa12arrayLiteralSayxGxd_tcfc
// CHECK-ARRAY2-NEXT: source.lang.swift
// CHECK-ARRAY2-NEXT: <Element> (Array<Element>.Type) -> (Element...) -> Array<Element>
// CHECK-ARRAY2-NEXT: $s12arrayLiteralSayxGxd_tcD
// CHECK-ARRAY2-NEXT: Swift
// CHECK-ARRAY2-NEXT: <Group>Collection/Array</Group>
// CHECK-ARRAY2-NEXT: SYSTEM

// RUN: %sourcekitd-test -req=cursor -pos=34:19 %s -- %s -target %target-triple %clang-importer-sdk-nosource -I %t | %FileCheck -check-prefix=CHECK-DICT1 %s
// CHECK-DICT1: source.lang.swift.ref.function.constructor
// CHECK-DICT1-NEXT: init(dictionaryLiteral:)
// CHECK-DICT1-NEXT: s:SD17dictionaryLiteralSDyxq_Gx_q_td_tcfc
// CHECK-DICT1-NEXT: source.lang.swift
// CHECK-DICT1-NEXT: <Key, Value where Key : Hashable> (Dictionary<Key, Value>.Type) -> ((Key, Value)...) -> Dictionary<Key, Value>
// CHECK-DICT1-NEXT: $s17dictionaryLiteralSDyxq_Gx_q_td_tcD
// CHECK-DICT1-NEXT: Swift
// CHECK-DICT1-NEXT: <Group>Collection/HashedCollections</Group>
// CHECK-DICT1-NEXT: SYSTEM

// RUN: %sourcekitd-test -req=cursor -pos=35:19 %s -- %s -target %target-triple %clang-importer-sdk-nosource -I %t | %FileCheck -check-prefix=CHECK-DICT2 %s
// CHECK-DICT2: source.lang.swift.ref.function.constructor
// CHECK-DICT2-NEXT: init(dictionaryLiteral:)
// CHECK-DICT2-NEXT: s:SD17dictionaryLiteralSDyxq_Gx_q_td_tcfc
// CHECK-DICT2-NEXT: source.lang.swift
// CHECK-DICT2-NEXT: <Key, Value where Key : Hashable> (Dictionary<Key, Value>.Type) -> ((Key, Value)...) -> Dictionary<Key, Value>
// CHECK-DICT2-NEXT: $s17dictionaryLiteralSDyxq_Gx_q_td_tcD
// CHECK-DICT2-NEXT: Swift
// CHECK-DICT2-NEXT: <Group>Collection/HashedCollections</Group>
// CHECK-DICT2-NEXT: SYSTEM
