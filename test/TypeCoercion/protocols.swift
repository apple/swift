// RUN: %target-typecheck-verify-swift

protocol MyPrintable {
  func print()
}

protocol Titled {
  var title : String { get set }
}

struct IsPrintable1 : FormattedPrintable, Titled, Document {
  var title = ""
  func print() {}
  func print(_: TestFormat) {}
}

// Printability is below
struct IsPrintable2 { }

struct IsNotPrintable1 { }
struct IsNotPrintable2 {
  func print(_: Int) -> Int {}
}

struct Book : Titled {
  var title : String
}

struct Lackey : Titled {
  var title : String {
    get {}
    set {}
  }
}

struct Number {
  var title : Int
}

func testPrintableCoercion(_ ip1: IsPrintable1,
                           ip2: IsPrintable2,
                           inp1: IsNotPrintable1,
                           inp2: IsNotPrintable2,
                           op: OtherPrintable) {
  var p : MyPrintable = ip1 // okay
  let _: MyPrintable & Titled = Book(title: "")
  // expected-error@-1 {{value of type 'Book' does not conform to specified type 'MyPrintable'}}
  p = ip1 // okay
  p = ip2 // okay
  p = inp1 // expected-error{{cannot assign value of type 'IsNotPrintable1' to type 'any MyPrintable'}}
  let _: MyPrintable = inp1
  // expected-error@-1 {{value of type 'IsNotPrintable1' does not conform to specified type 'MyPrintable'}}
  p = inp2 // expected-error{{cannot assign value of type 'IsNotPrintable2' to type 'any MyPrintable'}}
  p = op // expected-error{{value of type 'any OtherPrintable' does not conform to 'MyPrintable' in assignment}}
  _ = p
}

func testTitledCoercion(_ ip1: IsPrintable1, book: Book, lackey: Lackey,
                        number: Number, ip2: IsPrintable2) {
  var t : Titled = ip1 // okay
  t = ip1
  t = book
  t = lackey
  t = number // expected-error{{cannot assign value of type 'Number' to type 'any Titled'}}
  t = ip2 // expected-error{{cannot assign value of type 'IsPrintable2' to type 'any Titled'}}
  _ = t
}




extension IsPrintable2 : MyPrintable {
  func print() {}
}

protocol OtherPrintable {
  func print()
}

struct TestFormat {}

protocol FormattedPrintable : MyPrintable { 
  func print(_: TestFormat)
}

struct NotFormattedPrintable1 {
  func print(_: TestFormat) { }
}

func testFormattedPrintableCoercion(_ ip1: IsPrintable1,
                                    ip2: IsPrintable2,
                                    fp: inout FormattedPrintable,
                                    p: inout MyPrintable,
                                    op: inout OtherPrintable,
                                    nfp1: NotFormattedPrintable1) {
  fp = ip1
  fp = ip2 // expected-error{{cannot assign value of type 'IsPrintable2' to type 'any FormattedPrintable'}}
  fp = nfp1 // expected-error{{cannot assign value of type 'NotFormattedPrintable1' to type 'any FormattedPrintable'}}
  p = fp
  op = fp // expected-error{{value of type 'any FormattedPrintable' does not conform to 'OtherPrintable' in assignment}}
  fp = op // expected-error{{value of type 'any OtherPrintable' does not conform to 'FormattedPrintable' in assignment}}
}

protocol Document : Titled, MyPrintable {
}

func testMethodsAndVars(_ fp: FormattedPrintable, f: TestFormat, doc: inout Document) {
  fp.print(f)
  fp.print()
  doc.title = "Gone with the Wind"
  doc.print()
}

func testDocumentCoercion(_ doc: inout Document, ip1: IsPrintable1, l: Lackey) {
  doc = ip1
  doc = l // expected-error{{cannot assign value of type 'Lackey' to type 'any Document'}}
}

// Check coercion of references.
func refCoercion(_ p: inout MyPrintable) { }
var p : MyPrintable = IsPrintable1()
var fp : FormattedPrintable = IsPrintable1()
// expected-note@-1{{change variable type to 'any MyPrintable' if it doesn't need to be declared as 'any FormattedPrintable'}} {{10-28=any MyPrintable}}
var ip1 : IsPrintable1
// expected-note@-1{{change variable type to 'any MyPrintable' if it doesn't need to be declared as 'IsPrintable1'}} {{11-23=any MyPrintable}}

refCoercion(&p)
refCoercion(&fp)
// expected-error@-1{{inout argument could be set to a value with a type other than 'any FormattedPrintable'; use a value declared as type 'any MyPrintable' instead}}
refCoercion(&ip1)
// expected-error@-1{{inout argument could be set to a value with a type other than 'IsPrintable1'; use a value declared as type 'any MyPrintable' instead}}

do {
  var fp_2 = fp
  // expected-note@-1{{change variable type to 'any MyPrintable' if it doesn't need to be declared as 'any FormattedPrintable'}} {{11-11=: any MyPrintable}}
  var ip1_2 = ip1
  // expected-note@-1{{change variable type to 'any MyPrintable' if it doesn't need to be declared as 'IsPrintable1'}} {{12-12=: any MyPrintable}}
  refCoercion(&fp_2)
  // expected-error@-1{{inout argument could be set to a value with a type other than 'any FormattedPrintable'; use a value declared as type 'any MyPrintable' instead}}
  refCoercion(&ip1_2)
  // expected-error@-1{{inout argument could be set to a value with a type other than 'IsPrintable1'; use a value declared as type 'any MyPrintable' instead}}
}

do {
  var fp_2 : FormattedPrintable = fp, ip1_2 = ip1
  // expected-note@-1{{change variable type to 'any MyPrintable' if it doesn't need to be declared as 'any FormattedPrintable'}} {{14-32=any MyPrintable}}
  // expected-note@-2{{change variable type to 'any MyPrintable' if it doesn't need to be declared as 'IsPrintable1'}} {{44-44=: any MyPrintable}}
  refCoercion(&fp_2)
  // expected-error@-1{{inout argument could be set to a value with a type other than 'any FormattedPrintable'; use a value declared as type 'any MyPrintable' instead}}
  refCoercion(&ip1_2)
  // expected-error@-1{{inout argument could be set to a value with a type other than 'IsPrintable1'; use a value declared as type 'any MyPrintable' instead}}
}

do {
  var fp_2, fp_3 : FormattedPrintable
  // expected-note@-1{{change variable type to 'any MyPrintable' if it doesn't need to be declared as 'any FormattedPrintable'}} {{20-38=any MyPrintable}}
  // expected-note@-2{{change variable type to 'any MyPrintable' if it doesn't need to be declared as 'any FormattedPrintable'}} {{20-38=any MyPrintable}}
  fp_2 = fp
  fp_3 = fp
  refCoercion(&fp_2)
  // expected-error@-1{{inout argument could be set to a value with a type other than 'any FormattedPrintable'; use a value declared as type 'any MyPrintable' instead}}
  refCoercion(&fp_3)
  // expected-error@-1{{inout argument could be set to a value with a type other than 'any FormattedPrintable'; use a value declared as type 'any MyPrintable' instead}}
}

do {
  func wrapRefCoercion1(fp_2: inout FormattedPrintable,
                        ip1_2: inout IsPrintable1) {
    // expected-note@-2{{change variable type to 'any MyPrintable' if it doesn't need to be declared as 'any FormattedPrintable'}} {{31-55=any MyPrintable}}
    // expected-note@-2{{change variable type to 'any MyPrintable' if it doesn't need to be declared as 'IsPrintable1'}} {{32-50=any MyPrintable}}
    refCoercion(&fp_2)
    // expected-error@-1{{inout argument could be set to a value with a type other than 'any FormattedPrintable'; use a value declared as type 'any MyPrintable' instead}}
    refCoercion(&ip1_2)
    // expected-error@-1{{inout argument could be set to a value with a type other than 'IsPrintable1'; use a value declared as type 'any MyPrintable' instead}}
  }
}

do {
  // Make sure we don't add the fix-it for tuples:
  var (fp_2, ip1_2) = (fp, ip1)
  refCoercion(&fp_2)
  // expected-error@-1{{inout argument could be set to a value with a type other than 'any FormattedPrintable'; use a value declared as type 'any MyPrintable' instead}}
  refCoercion(&ip1_2)
  // expected-error@-1{{inout argument could be set to a value with a type other than 'IsPrintable1'; use a value declared as type 'any MyPrintable' instead}}
}

do {
  // Make sure we don't add the fix-it for vars in different scopes:
  enum ParentScope { static var fp_2 = fp }
  refCoercion(&ParentScope.fp_2)
  // expected-error@-1{{inout argument could be set to a value with a type other than 'any FormattedPrintable'; use a value declared as type 'any MyPrintable' instead}}
}

protocol IntSubscriptable {
  subscript(i: Int) -> Int { get }
}

struct IsIntSubscriptable : IntSubscriptable {
  subscript(i: Int) -> Int { get {} set {} }
}

struct IsDoubleSubscriptable {
  subscript(d: Double) -> Int { get {} set {} }
}

struct IsIntToStringSubscriptable {
  subscript(i: Int) -> String { get {} set {} }
}

func testIntSubscripting(i_s: inout IntSubscriptable,
                         iis: IsIntSubscriptable,
                         ids: IsDoubleSubscriptable,
                         iiss: IsIntToStringSubscriptable) {
  var x = i_s[17]
  i_s[5] = 7 // expected-error{{cannot assign through subscript: subscript is get-only}}

  i_s = iis
  i_s = ids // expected-error{{cannot assign value of type 'IsDoubleSubscriptable' to type 'any IntSubscriptable'}}
  i_s = iiss // expected-error{{cannot assign value of type 'IsIntToStringSubscriptable' to type 'any IntSubscriptable'}}
}

protocol MyREPLPrintable {
  func myReplPrint()
}

extension Int : MyREPLPrintable {
  func myReplPrint() {}
}
extension String : MyREPLPrintable {
  func myReplPrint() {}
}

func doREPLPrint(_ p: MyREPLPrintable) {
  p.myReplPrint()
}

func testREPLPrintable() {
  let i : Int = 1
  _ = i as MyREPLPrintable
  doREPLPrint(i)
  doREPLPrint(1)
  doREPLPrint("foo")
}

// Bool coercion
if true as Bool {}
