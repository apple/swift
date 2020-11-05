// RUN: %target-swift-ide-test -code-completion -source-filename %s -code-completion-token=AVAILABILITY1 | %FileCheck %s -check-prefix=AVAILABILITY1
// RUN: %target-swift-ide-test -code-completion -source-filename %s -code-completion-token=AVAILABILITY2 | %FileCheck %s -check-prefix=AVAILABILITY2
// RUN: %target-swift-ide-test -code-completion -source-filename %s -code-completion-token=KEYWORD2 | %FileCheck %s -check-prefix=KEYWORD2
// RUN: %target-swift-ide-test -code-completion -source-filename %s -code-completion-token=KEYWORD3 | %FileCheck %s -check-prefix=KEYWORD3
// RUN: %target-swift-ide-test -code-completion -source-filename %s -code-completion-token=KEYWORD3_2 | %FileCheck %s -check-prefix=KEYWORD3
// RUN: %target-swift-ide-test -code-completion -source-filename %s -code-completion-token=KEYWORD4 | %FileCheck %s -check-prefix=KEYWORD4
// RUN: %target-swift-ide-test -code-completion -source-filename %s -code-completion-token=KEYWORD5 | %FileCheck %s -check-prefix=KEYWORD5
// RUN: %target-swift-ide-test -code-completion -source-filename %s -code-completion-token=ON_GLOBALVAR | %FileCheck %s -check-prefix=ON_GLOBALVAR
// RUN: %target-swift-ide-test -code-completion -source-filename %s -code-completion-token=ON_INIT | %FileCheck %s -check-prefix=ON_INIT
// RUN: %target-swift-ide-test -code-completion -source-filename %s -code-completion-token=ON_PROPERTY | %FileCheck %s -check-prefix=ON_PROPERTY
// RUN: %target-swift-ide-test -code-completion -source-filename %s -code-completion-token=ON_METHOD | %FileCheck %s -check-prefix=ON_METHOD
// RUN: %target-swift-ide-test -code-completion -source-filename %s -code-completion-token=ON_PARAM_1 | %FileCheck %s -check-prefix=ON_PARAM
// RUN: %target-swift-ide-test -code-completion -source-filename %s -code-completion-token=ON_PARAM_2 | %FileCheck %s -check-prefix=ON_PARAM
// RUN: %target-swift-ide-test -code-completion -source-filename %s -code-completion-token=ON_MEMBER_INDEPENDENT_1 | %FileCheck %s -check-prefix=ON_MEMBER_LAST
// RUN: %target-swift-ide-test -code-completion -source-filename %s -code-completion-token=ON_MEMBER_INDEPENDENT_2 | %FileCheck %s -check-prefix=ON_MEMBER_LAST
// RUN: %target-swift-ide-test -code-completion -source-filename %s -code-completion-token=ON_MEMBER_LAST | %FileCheck %s -check-prefix=ON_MEMBER_LAST
// RUN: %target-swift-ide-test -code-completion -source-filename %s -code-completion-token=KEYWORD_INDEPENDENT_1 | %FileCheck %s -check-prefix=KEYWORD_LAST
// RUN: %target-swift-ide-test -code-completion -source-filename %s -code-completion-token=KEYWORD_INDEPENDENT_2 | %FileCheck %s -check-prefix=KEYWORD_LAST
// RUN: %target-swift-ide-test -code-completion -source-filename %s -code-completion-token=KEYWORD_LAST | %FileCheck %s -check-prefix=KEYWORD_LAST

struct MyStruct {}

@available(#^AVAILABILITY1^#)

// NOTE: Please do not include the ", N items" after "Begin completions". The
// item count creates needless merge conflicts given that we use the "-NEXT"
// feature of FileCheck and because an "End completions" line exists for each
// test.

// AVAILABILITY1: Begin completions
// AVAILABILITY1-NEXT: Keyword/None:                       *[#Platform#]; name=*{{$}}
// AVAILABILITY1-NEXT: Keyword/None:                       iOS[#Platform#]; name=iOS{{$}}
// AVAILABILITY1-NEXT: Keyword/None:                       tvOS[#Platform#]; name=tvOS{{$}}
// AVAILABILITY1-NEXT: Keyword/None:                       watchOS[#Platform#]; name=watchOS{{$}}
// AVAILABILITY1-NEXT: Keyword/None:                       macOS[#Platform#]; name=macOS{{$}}
// AVAILABILITY1-NEXT: Keyword/None:                       iOSApplicationExtension[#Platform#]; name=iOSApplicationExtension{{$}}
// AVAILABILITY1-NEXT: Keyword/None:                       tvOSApplicationExtension[#Platform#]; name=tvOSApplicationExtension{{$}}
// AVAILABILITY1-NEXT: Keyword/None:                       watchOSApplicationExtension[#Platform#]; name=watchOSApplicationExtension{{$}}
// AVAILABILITY1-NEXT: Keyword/None:                       macOSApplicationExtension[#Platform#]; name=macOSApplicationExtension{{$}}
// AVAILABILITY1-NEXT: Keyword/None:                       macCatalyst[#Platform#]; name=macCatalyst
// AVAILABILITY1-NEXT: Keyword/None:                       macCatalystApplicationExtension[#Platform#]; name=macCatalystApplicationExtension
// AVAILABILITY1-NEXT: Keyword/None:                       OpenBSD[#Platform#]; name=OpenBSD{{$}}
// AVAILABILITY1-NEXT: End completions

@available(*, #^AVAILABILITY2^#)

// AVAILABILITY2:             Begin completions
// AVAILABILITY2-NEXT:        Keyword/None:                       unavailable; name=unavailable{{$}}
// AVAILABILITY2-NEXT:        Keyword/None:                       message: [#Specify message#]; name=message{{$}}
// AVAILABILITY2-NEXT:        Keyword/None:                       renamed: [#Specify replacing name#]; name=renamed{{$}}
// AVAILABILITY2-NEXT:        Keyword/None:                       introduced: [#Specify version number#]; name=introduced{{$}}
// AVAILABILITY2-NEXT:        Keyword/None:                       deprecated: [#Specify version number#]; name=deprecated{{$}}
// AVAILABILITY2-NEXT:        End completions

@#^KEYWORD2^# func method(){}

// KEYWORD2:                  Begin completions
// KEYWORD2-NEXT:             Keyword/None:                       available[#Func Attribute#]; name=available{{$}}
// KEYWORD2-NEXT:             Keyword/None:                       objc[#Func Attribute#]; name=objc{{$}}
// KEYWORD2-NEXT:             Keyword/None:                       IBAction[#Func Attribute#]; name=IBAction{{$}}
// KEYWORD2-NEXT:             Keyword/None:                       NSManaged[#Func Attribute#]; name=NSManaged{{$}}
// KEYWORD2-NEXT:             Keyword/None:                       inline[#Func Attribute#]; name=inline{{$}}
// KEYWORD2-NEXT:             Keyword/None:                       nonobjc[#Func Attribute#]; name=nonobjc{{$}}
// KEYWORD2-NEXT:             Keyword/None:                       inlinable[#Func Attribute#]; name=inlinable{{$}}
// KEYWORD2-NEXT:             Keyword/None:                       warn_unqualified_access[#Func Attribute#]; name=warn_unqualified_access{{$}}
// KEYWORD2-NEXT:             Keyword/None:                       usableFromInline[#Func Attribute#]; name=usableFromInline
// KEYWORD2-NEXT:             Keyword/None:                       discardableResult[#Func Attribute#]; name=discardableResult
// KEYWORD2-NEXT:             Keyword/None:                       differentiable[#Func Attribute#]; name=differentiable
// KEYWORD2-NEXT:             Keyword/None:                       IBSegueAction[#Func Attribute#]; name=IBSegueAction{{$}}
// KEYWORD2-NEXT:             Keyword/None:                       derivative[#Func Attribute#]; name=derivative
// KEYWORD2-NEXT:             Keyword/None:                       transpose[#Func Attribute#]; name=transpose
// KEYWORD2-NEXT:             Keyword/None:                       noDerivative[#Func Attribute#]; name=noDerivative
// KEYWORD2-NOT:              Keyword
// KEYWORD2:                  Decl[Struct]/CurrModule:            MyStruct[#MyStruct#]; name=MyStruct
// KEYWORD2:                  End completions

@#^KEYWORD3^# class C {}

// KEYWORD3:                  Begin completions
// KEYWORD3-NEXT:             Keyword/None:                       available[#Class Attribute#]; name=available{{$}}
// KEYWORD3-NEXT:             Keyword/None:                       objc[#Class Attribute#]; name=objc{{$}}
// KEYWORD3-NEXT:             Keyword/None:                       dynamicCallable[#Class Attribute#]; name=dynamicCallable{{$}}
// KEYWORD3-NEXT:             Keyword/None:                       main[#Class Attribute#]; name=main
// KEYWORD3-NEXT:             Keyword/None:                       dynamicMemberLookup[#Class Attribute#]; name=dynamicMemberLookup{{$}}
// KEYWORD3-NEXT:             Keyword/None:                       IBDesignable[#Class Attribute#]; name=IBDesignable{{$}}
// KEYWORD3-NEXT:             Keyword/None:                       UIApplicationMain[#Class Attribute#]; name=UIApplicationMain{{$}}
// KEYWORD3-NEXT:             Keyword/None:                       requires_stored_property_inits[#Class Attribute#]; name=requires_stored_property_inits{{$}}
// KEYWORD3-NEXT:             Keyword/None:                       objcMembers[#Class Attribute#]; name=objcMembers{{$}}
// KEYWORD3-NEXT:             Keyword/None:                       NSApplicationMain[#Class Attribute#]; name=NSApplicationMain{{$}}
// KEYWORD3-NEXT:             Keyword/None:                       usableFromInline[#Class Attribute#]; name=usableFromInline
// KEYWORD3-NEXT:             Keyword/None:                       propertyWrapper[#Class Attribute#]; name=propertyWrapper
// KEYWORD3-NEXT:             Keyword/None:                       resultBuilder[#Class Attribute#]; name=resultBuilder
// KEYWORD3-NEXT:             Keyword/None:                       reflectable[#Class Attribute#]; name=reflectable
// KEYWORD3-NEXT:             End completions

@#^KEYWORD3_2^#IB class C2 {}
// Same as KEYWORD3.

@#^KEYWORD4^# enum E {}
// KEYWORD4:                  Begin completions
// KEYWORD4-NEXT:             Keyword/None:                       available[#Enum Attribute#]; name=available{{$}}
// KEYWORD4-NEXT:             Keyword/None:                       objc[#Enum Attribute#]; name=objc{{$}}
// KEYWORD4-NEXT:             Keyword/None:                       dynamicCallable[#Enum Attribute#]; name=dynamicCallable
// KEYWORD4-NEXT:             Keyword/None:                       main[#Enum Attribute#]; name=main
// KEYWORD4-NEXT:             Keyword/None:                       dynamicMemberLookup[#Enum Attribute#]; name=dynamicMemberLookup
// KEYWORD4-NEXT:             Keyword/None:                       usableFromInline[#Enum Attribute#]; name=usableFromInline
// KEYWORD4-NEXT:             Keyword/None:                       frozen[#Enum Attribute#]; name=frozen
// KEYWORD4-NEXT:             Keyword/None:                       propertyWrapper[#Enum Attribute#]; name=propertyWrapper
// KEYWORD4-NEXT:             Keyword/None:                       resultBuilder[#Enum Attribute#]; name=resultBuilder
// KEYWORD4-NEXT:             Keyword/None:                       reflectable[#Enum Attribute#]; name=reflectable
// KEYWORD4-NEXT:             End completions


@#^KEYWORD5^# struct S{}
// KEYWORD5:                  Begin completions
// KEYWORD5-NEXT:             Keyword/None:                       available[#Struct Attribute#]; name=available{{$}}
// KEYWORD5-NEXT:             Keyword/None:                       dynamicCallable[#Struct Attribute#]; name=dynamicCallable
// KEYWORD5-NEXT:             Keyword/None:                       main[#Struct Attribute#]; name=main
// KEYWORD5-NEXT:             Keyword/None:                       dynamicMemberLookup[#Struct Attribute#]; name=dynamicMemberLookup
// KEYWORD5-NEXT:             Keyword/None:                       usableFromInline[#Struct Attribute#]; name=usableFromInline
// KEYWORD5-NEXT:             Keyword/None:                       frozen[#Struct Attribute#]; name=frozen
// KEYWORD5-NEXT:             Keyword/None:                       propertyWrapper[#Struct Attribute#]; name=propertyWrapper
// KEYWORD5-NEXT:             Keyword/None:                       resultBuilder[#Struct Attribute#]; name=resultBuilder
// KEYWORD5-NEXT:             Keyword/None:                       reflectable[#Struct Attribute#]; name=reflectable
// KEYWORD5-NEXT:             End completions

@#^ON_GLOBALVAR^# var globalVar
// ON_GLOBALVAR: Begin completions
// ON_GLOBALVAR-DAG: Keyword/None:                       available[#Var Attribute#]; name=available
// ON_GLOBALVAR-DAG: Keyword/None:                       objc[#Var Attribute#]; name=objc
// ON_GLOBALVAR-DAG: Keyword/None:                       NSCopying[#Var Attribute#]; name=NSCopying
// ON_GLOBALVAR-DAG: Keyword/None:                       IBInspectable[#Var Attribute#]; name=IBInspectable
// ON_GLOBALVAR-DAG: Keyword/None:                       IBOutlet[#Var Attribute#]; name=IBOutlet
// ON_GLOBALVAR-DAG: Keyword/None:                       NSManaged[#Var Attribute#]; name=NSManaged
// ON_GLOBALVAR-DAG: Keyword/None:                       inline[#Var Attribute#]; name=inline
// ON_GLOBALVAR-DAG: Keyword/None:                       nonobjc[#Var Attribute#]; name=nonobjc
// ON_GLOBALVAR-DAG: Keyword/None:                       inlinable[#Var Attribute#]; name=inlinable
// ON_GLOBALVAR-DAG: Keyword/None:                       usableFromInline[#Var Attribute#]; name=usableFromInline
// ON_GLOBALVAR-DAG: Keyword/None:                       GKInspectable[#Var Attribute#]; name=GKInspectable
// ON_GLOBALVAR-DAG: Keyword/None:                       differentiable[#Var Attribute#]; name=differentiable
// ON_GLOBALVAR-DAG: Keyword/None:                       noDerivative[#Var Attribute#]; name=noDerivative
// ON_GLOBALVAR-NOT: Keyword
// ON_GLOBALVAR: Decl[Struct]/CurrModule:            MyStruct[#MyStruct#]; name=MyStruct
// ON_GLOBALVAR: End completions

struct _S {
  @#^ON_INIT^# init()
// ON_INIT: Begin completions
// ON_INIT-DAG: Keyword/None:                       available[#Constructor Attribute#]; name=available
// ON_INIT-DAG: Keyword/None:                       objc[#Constructor Attribute#]; name=objc
// ON_INIT-DAG: Keyword/None:                       inline[#Constructor Attribute#]; name=inline
// ON_INIT-DAG: Keyword/None:                       nonobjc[#Constructor Attribute#]; name=nonobjc
// ON_INIT-DAG: Keyword/None:                       inlinable[#Constructor Attribute#]; name=inlinable
// ON_INIT-DAG: Keyword/None:                       usableFromInline[#Constructor Attribute#]; name=usableFromInline
// ON_INIT-DAG: Keyword/None:                       discardableResult[#Constructor Attribute#]; name=discardableResult
// ON_INIT: End completions

  @#^ON_PROPERTY^# var foo
// ON_PROPERTY: Begin completions
// ON_PROPERTY-DAG: Keyword/None:                       available[#Var Attribute#]; name=available
// ON_PROPERTY-DAG: Keyword/None:                       objc[#Var Attribute#]; name=objc
// ON_PROPERTY-DAG: Keyword/None:                       NSCopying[#Var Attribute#]; name=NSCopying
// ON_PROPERTY-DAG: Keyword/None:                       IBInspectable[#Var Attribute#]; name=IBInspectable
// ON_PROPERTY-DAG: Keyword/None:                       IBOutlet[#Var Attribute#]; name=IBOutlet
// ON_PROPERTY-DAG: Keyword/None:                       NSManaged[#Var Attribute#]; name=NSManaged
// ON_PROPERTY-DAG: Keyword/None:                       inline[#Var Attribute#]; name=inline
// ON_PROPERTY-DAG: Keyword/None:                       nonobjc[#Var Attribute#]; name=nonobjc
// ON_PROPERTY-DAG: Keyword/None:                       inlinable[#Var Attribute#]; name=inlinable
// ON_PROPERTY-DAG: Keyword/None:                       usableFromInline[#Var Attribute#]; name=usableFromInline
// ON_PROPERTY-DAG: Keyword/None:                       GKInspectable[#Var Attribute#]; name=GKInspectable
// ON_PROPERTY-DAG: Keyword/None:                       differentiable[#Var Attribute#]; name=differentiable
// ON_PROPERTY-DAG: Keyword/None:                       noDerivative[#Var Attribute#]; name=noDerivative
// ON_PROPERTY-NOT: Keyword
// ON_PROPERTY: Decl[Struct]/CurrModule:            MyStruct[#MyStruct#]; name=MyStruct
// ON_PROPERTY-NOT: Decl[PrecedenceGroup]
// ON_PROPERTY: End completions

  @#^ON_METHOD^# private
  func foo()
// ON_METHOD: Begin completions
// ON_METHOD-DAG: Keyword/None:                       available[#Func Attribute#]; name=available
// ON_METHOD-DAG: Keyword/None:                       objc[#Func Attribute#]; name=objc
// ON_METHOD-DAG: Keyword/None:                       IBAction[#Func Attribute#]; name=IBAction
// ON_METHOD-DAG: Keyword/None:                       NSManaged[#Func Attribute#]; name=NSManaged
// ON_METHOD-DAG: Keyword/None:                       inline[#Func Attribute#]; name=inline
// ON_METHOD-DAG: Keyword/None:                       nonobjc[#Func Attribute#]; name=nonobjc
// ON_METHOD-DAG: Keyword/None:                       inlinable[#Func Attribute#]; name=inlinable
// ON_METHOD-DAG: Keyword/None:                       warn_unqualified_access[#Func Attribute#]; name=warn_unqualified_access
// ON_METHOD-DAG: Keyword/None:                       usableFromInline[#Func Attribute#]; name=usableFromInline
// ON_METHOD-DAG: Keyword/None:                       discardableResult[#Func Attribute#]; name=discardableResult
// ON_METHOD-DAG: Keyword/None:                       IBSegueAction[#Func Attribute#]; name=IBSegueAction
// ON_METHOD-DAG: Keyword/None:                       differentiable[#Func Attribute#]; name=differentiable
// ON_METHOD-DAG: Keyword/None:                       derivative[#Func Attribute#]; name=derivative
// ON_METHOD-DAG: Keyword/None:                       transpose[#Func Attribute#]; name=transpose
// ON_METHOD-DAG: Keyword/None:                       noDerivative[#Func Attribute#]; name=noDerivative
// ON_METHOD-NOT: Keyword
// ON_METHOD: Decl[Struct]/CurrModule:            MyStruct[#MyStruct#]; name=MyStruct
// ON_METHOD: End completions

  func bar(@#^ON_PARAM_1^#)
// ON_PARAM: Begin completions
// ON_PARAM-NOT: Keyword
// ON_PARAM: Decl[Struct]/CurrModule:            MyStruct[#MyStruct#]; name=MyStruct
// ON_PARAM-NOT: Keyword
// ON_PARAM: End completions

  func bar(
    @#^ON_PARAM_2^#

    arg: Int
  )
// Same as ON_PARAM.

  @#^ON_MEMBER_INDEPENDENT_1^#

  func dummy1() {}
// Same as ON_MEMBER_LAST.

  @#^ON_MEMBER_INDEPENDENT_2^#
  func dummy2() {}
// Same as ON_MEMBER_LAST.


  @#^ON_MEMBER_LAST^#
// ON_MEMBER_LAST: Begin completions
// ON_MEMBER_LAST-DAG: Keyword/None:                       available[#Declaration Attribute#]; name=available
// ON_MEMBER_LAST-DAG: Keyword/None:                       objc[#Declaration Attribute#]; name=objc
// ON_MEMBER_LAST-DAG: Keyword/None:                       dynamicCallable[#Declaration Attribute#]; name=dynamicCallable
// ON_MEMBER_LAST-DAG: Keyword/None:                       main[#Declaration Attribute#]; name=main
// ON_MEMBER_LAST-DAG: Keyword/None:                       dynamicMemberLookup[#Declaration Attribute#]; name=dynamicMemberLookup
// ON_MEMBER_LAST-DAG: Keyword/None:                       NSCopying[#Declaration Attribute#]; name=NSCopying
// ON_MEMBER_LAST-DAG: Keyword/None:                       IBAction[#Declaration Attribute#]; name=IBAction
// ON_MEMBER_LAST-DAG: Keyword/None:                       IBDesignable[#Declaration Attribute#]; name=IBDesignable
// ON_MEMBER_LAST-DAG: Keyword/None:                       IBInspectable[#Declaration Attribute#]; name=IBInspectable
// ON_MEMBER_LAST-DAG: Keyword/None:                       IBOutlet[#Declaration Attribute#]; name=IBOutlet
// ON_MEMBER_LAST-DAG: Keyword/None:                       NSManaged[#Declaration Attribute#]; name=NSManaged
// ON_MEMBER_LAST-DAG: Keyword/None:                       UIApplicationMain[#Declaration Attribute#]; name=UIApplicationMain
// ON_MEMBER_LAST-DAG: Keyword/None:                       inline[#Declaration Attribute#]; name=inline
// ON_MEMBER_LAST-DAG: Keyword/None:                       requires_stored_property_inits[#Declaration Attribute#]; name=requires_stored_property_inits
// ON_MEMBER_LAST-DAG: Keyword/None:                       nonobjc[#Declaration Attribute#]; name=nonobjc
// ON_MEMBER_LAST-DAG: Keyword/None:                       inlinable[#Declaration Attribute#]; name=inlinable
// ON_MEMBER_LAST-DAG: Keyword/None:                       objcMembers[#Declaration Attribute#]; name=objcMembers
// ON_MEMBER_LAST-DAG: Keyword/None:                       NSApplicationMain[#Declaration Attribute#]; name=NSApplicationMain
// ON_MEMBER_LAST-DAG: Keyword/None:                       warn_unqualified_access[#Declaration Attribute#]; name=warn_unqualified_access
// ON_MEMBER_LAST-DAG: Keyword/None:                       usableFromInline[#Declaration Attribute#]; name=usableFromInline
// ON_MEMBER_LAST-DAG: Keyword/None:                       discardableResult[#Declaration Attribute#]; name=discardableResult
// ON_MEMBER_LAST-DAG: Keyword/None:                       GKInspectable[#Declaration Attribute#]; name=GKInspectable
// ON_MEMBER_LAST-DAG: Keyword/None:                       IBSegueAction[#Declaration Attribute#]; name=IBSegueAction
// ON_MEMBER_LAST-DAG: Keyword/None:                       propertyWrapper[#Declaration Attribute#]; name=propertyWrapper
// ON_MEMBER_LAST-DAG: Keyword/None:                       resultBuilder[#Declaration Attribute#]; name=resultBuilder
// ON_MEMBER_LAST-DAG: Keyword/None:                       differentiable[#Declaration Attribute#]; name=differentiable
// ON_MEMBER_LAST-DAG: Keyword/None:                       derivative[#Declaration Attribute#]; name=derivative
// ON_MEMBER_LAST-DAG: Keyword/None:                       transpose[#Declaration Attribute#]; name=transpose
// ON_MEMBER_LAST-DAG: Keyword/None:                       noDerivative[#Declaration Attribute#]; name=noDerivative
// ON_MEMBER_LAST-DAG: Keyword/None:                       reflectable[#Declaration Attribute#]; name=reflectable
// ON_MEMBER_LAST-NOT: Keyword
// ON_MEMBER_LAST: Decl[Struct]/CurrModule:            MyStruct[#MyStruct#]; name=MyStruct
// ON_MEMBER_LAST-NOT: Decl[PrecedenceGroup]
// ON_MEMBER_LAST: End completions
}

@#^KEYWORD_INDEPENDENT_1^#

func dummy1() {}
// Same as KEYWORD_LAST.

@#^KEYWORD_INDEPENDENT_2^#
func dummy2() {}
// Same as KEYWORD_LAST.

@#^KEYWORD_LAST^#

// KEYWORD_LAST:                  Begin completions
// KEYWORD_LAST-NEXT:             Keyword/None:                       available[#Declaration Attribute#]; name=available{{$}}
// KEYWORD_LAST-NEXT:             Keyword/None:                       objc[#Declaration Attribute#]; name=objc{{$}}
// KEYWORD_LAST-NEXT:             Keyword/None:                       dynamicCallable[#Declaration Attribute#]; name=dynamicCallable
// KEYWORD_LAST-NEXT:             Keyword/None:                       main[#Declaration Attribute#]; name=main
// KEYWORD_LAST-NEXT:             Keyword/None:                       dynamicMemberLookup[#Declaration Attribute#]; name=dynamicMemberLookup
// KEYWORD_LAST-NEXT:             Keyword/None:                       NSCopying[#Declaration Attribute#]; name=NSCopying{{$}}
// KEYWORD_LAST-NEXT:             Keyword/None:                       IBAction[#Declaration Attribute#]; name=IBAction{{$}}
// KEYWORD_LAST-NEXT:             Keyword/None:                       IBDesignable[#Declaration Attribute#]; name=IBDesignable{{$}}
// KEYWORD_LAST-NEXT:             Keyword/None:                       IBInspectable[#Declaration Attribute#]; name=IBInspectable{{$}}
// KEYWORD_LAST-NEXT:             Keyword/None:                       IBOutlet[#Declaration Attribute#]; name=IBOutlet{{$}}
// KEYWORD_LAST-NEXT:             Keyword/None:                       NSManaged[#Declaration Attribute#]; name=NSManaged{{$}}
// KEYWORD_LAST-NEXT:             Keyword/None:                       UIApplicationMain[#Declaration Attribute#]; name=UIApplicationMain{{$}}
// KEYWORD_LAST-NEXT:             Keyword/None:                       inline[#Declaration Attribute#]; name=inline{{$}}
// KEYWORD_LAST-NEXT:             Keyword/None:                       requires_stored_property_inits[#Declaration Attribute#]; name=requires_stored_property_inits{{$}}
// KEYWORD_LAST-NEXT:             Keyword/None:                       nonobjc[#Declaration Attribute#]; name=nonobjc{{$}}
// KEYWORD_LAST-NEXT:             Keyword/None:                       inlinable[#Declaration Attribute#]; name=inlinable{{$}}
// KEYWORD_LAST-NEXT:             Keyword/None:                       objcMembers[#Declaration Attribute#]; name=objcMembers{{$}}
// KEYWORD_LAST-NEXT:             Keyword/None:                       NSApplicationMain[#Declaration Attribute#]; name=NSApplicationMain{{$}}
// KEYWORD_LAST-NEXT:             Keyword/None:                       warn_unqualified_access[#Declaration Attribute#]; name=warn_unqualified_access
// KEYWORD_LAST-NEXT:             Keyword/None:                       usableFromInline[#Declaration Attribute#]; name=usableFromInline{{$}}
// KEYWORD_LAST-NEXT:             Keyword/None:                       discardableResult[#Declaration Attribute#]; name=discardableResult
// KEYWORD_LAST-NEXT:             Keyword/None:                       GKInspectable[#Declaration Attribute#]; name=GKInspectable{{$}}
// KEYWORD_LAST-NEXT:             Keyword/None:                       frozen[#Declaration Attribute#]; name=frozen
// KEYWORD_LAST-NEXT:             Keyword/None:                       propertyWrapper[#Declaration Attribute#]; name=propertyWrapper
// KEYWORD_LAST-NEXT:             Keyword/None:                       resultBuilder[#Declaration Attribute#]; name=resultBuilder
// KEYWORD_LAST-NEXT:             Keyword/None:                       differentiable[#Declaration Attribute#]; name=differentiable
// KEYWORD_LAST-NEXT:             Keyword/None:                       IBSegueAction[#Declaration Attribute#]; name=IBSegueAction{{$}}
// KEYWORD_LAST-NEXT:             Keyword/None:                       derivative[#Declaration Attribute#]; name=derivative
// KEYWORD_LAST-NEXT:             Keyword/None:                       transpose[#Declaration Attribute#]; name=transpose
// KEYWORD_LAST-NEXT:             Keyword/None:                       noDerivative[#Declaration Attribute#]; name=noDerivative
// KEYWORD_LAST-NEXT:             Keyword/None:                       reflectable[#Declaration Attribute#]; name=reflectable
// KEYWORD_LAST-NOT:              Keyword
// KEYWORD_LAST: Decl[Struct]/CurrModule:            MyStruct[#MyStruct#]; name=MyStruct
// KEYWORD_LAST:                  End completions
