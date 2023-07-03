// REQUIRES: swift_swift_parser

// RUN: %empty-directory(%t)
// RUN: split-file --leading-lines %s %t

// Check that we index code expanded from macros, especially nested references
// (ie. calls within an added function).

// Create the plugin with various macros for testing
// RUN: %host-build-swift -swift-version 5 -emit-library -o %t/%target-library-name(IndexMacros) -module-name=IndexMacros %t/IndexMacros.swift -g -no-toolchain-stdlib-rpath

// Check indexed symbols
// RUN: %target-swift-ide-test -print-indexed-symbols -source-filename %t/IndexTest.swift -load-plugin-library %t/%target-library-name(IndexMacros) -parse-as-library > %t/index.out
// RUN: %FileCheck %s --input-file %t/index.out

//--- IndexTest.swift
@freestanding(expression)
macro freestandingExpr<T>(arg: T) = #externalMacro(module: "IndexMacros", type: "FreestandingExprMacro")
// CHECK: [[@LINE-1]]:7 | macro/Swift | freestandingExpr(arg:) |  [[EXPR_USR:.*]] | Def

@freestanding(declaration, names: named(TestFree))
macro freestandingDecl<T>(arg: T) = #externalMacro(module: "IndexMacros", type: "FreestandingDeclMacro")
// CHECK: [[@LINE-1]]:7 | macro/Swift | freestandingDecl(arg:) |  [[DECL_USR:.*]] | Def

@attached(accessor)
macro Accessor() = #externalMacro(module: "IndexMacros", type: "SomeAccessorMacro")
// CHECK: [[@LINE-1]]:7 | macro/Swift | Accessor() |  [[ACCESSOR_USR:.*]] | Def

@attached(conformance)
macro Conformance() = #externalMacro(module: "IndexMacros", type: "SomeConformanceMacro")
// CHECK: [[@LINE-1]]:7 | macro/Swift | Conformance() |  [[CONFORMANCE_USR:.*]] | Def

@attached(member, names: named(memberFunc))
macro Member() = #externalMacro(module: "IndexMacros", type: "SomeMemberMacro")
// CHECK: [[@LINE-1]]:7 | macro/Swift | Member() |  [[MEMBER_USR:.*]] | Def

@attached(memberAttribute)
macro MemberAttribute() = #externalMacro(module: "IndexMacros", type: "SomeMemberAttributeMacro")
// CHECK: [[@LINE-1]]:7 | macro/Swift | MemberAttribute() |  [[MEMBER_ATTRIBUTE_USR:.*]] | Def

@attached(peer, names: named(TestPeer))
macro Peer<T>(arg: T) = #externalMacro(module: "IndexMacros", type: "SomePeerMacro")
// CHECK: [[@LINE-1]]:7 | macro/Swift | Peer(arg:) |  [[PEER_USR:.*]] | Def

@attached(peer, names: named(peerMember))
macro PeerMember() = #externalMacro(module: "IndexMacros", type: "SomePeerMemberMacro")
// CHECK: [[@LINE-1]]:7 | macro/Swift | PeerMember() |  [[PEER_MEMBER_USR:.*]] | Def

protocol TestProto {}
// CHECK: [[@LINE-1]]:10 | protocol/Swift | TestProto | [[PROTO_USR:.*]] | Def

func accessorLog() {}
// CHECK: [[@LINE-1]]:6 | function/Swift | accessorLog() | [[ACC_LOG_USR:.*]] | Def
func exprLog() {}
// CHECK: [[@LINE-1]]:6 | function/Swift | exprLog() | [[EXPR_LOG_USR:.*]] | Def
func freeLog() {}
// CHECK: [[@LINE-1]]:6 | function/Swift | freeLog() | [[FREE_LOG_USR:.*]] | Def
func memberLog() {}
// CHECK: [[@LINE-1]]:6 | function/Swift | memberLog() | [[MEMBER_LOG_USR:.*]] | Def
func peerLog() {}
// CHECK: [[@LINE-1]]:6 | function/Swift | peerLog() | [[PEER_LOG_USR:.*]] | Def

// CHECK: [[@LINE+2]]:8 | struct/Swift | AddOne | [[ADD_ONE_USR:.*]] | Def
@propertyWrapper
struct AddOne {
  var value: Int = 1
  var wrappedValue: Int {
    get { value }
    set { value = newValue + 1 }
  }
  init(wrappedValue: Int) {
    self.wrappedValue = wrappedValue
  }
}

// CHECK: [[@LINE+2]]:2 | macro/Swift | freestandingDecl(arg:) | [[DECL_USR]] | Ref
// CHECK: [[@LINE+1]]:19 | struct/Swift | Double | s:Sd | Ref
#freestandingDecl<Double>(arg: 1.0)
// Creates a `TestFree` struct with `freeFunc` calling `freeLog`
// CHECK: [[@LINE-2]]:1 | struct/Swift | TestFree | [[FREE_STRUCT_USR:.*]] | Def,Impl
// CHECK: [[@LINE-3]]:1 | instance-method/Swift | freeFunc() | [[FREE_FUNC_USR:.*]] | Def,Impl,RelChild
// CHECK-NEXT: RelChild | struct/Swift | TestFree | [[FREE_STRUCT_USR]]
// CHECK: [[@LINE-5]]:1 | function/Swift | freeLog() | [[FREE_LOG_USR]] | Ref,Call,Impl,RelCall,RelCont
// CHECK-NEXT: RelCall,RelCont | instance-method/Swift | freeFunc() | [[FREE_FUNC_USR]]

func testExpr() {
  // CHECK: [[@LINE+2]]:4 | macro/Swift | freestandingExpr(arg:) | [[EXPR_USR]] | Ref
  // CHECK: [[@LINE+1]]:21 | struct/Swift | Double | s:Sd | Ref
  #freestandingExpr<Double>(arg: 1.0)
  // CHECK: [[@LINE-1]]:3 | function/Swift | exprLog() | [[EXPR_LOG_USR]] | Ref,Call,Impl,RelCall,RelCont
  // CHECK-NEXT: RelCall,RelCont | function/Swift | testExpr()
}

// CHECK: [[@LINE+5]]:40 | macro/Swift | Peer(arg:) | [[PEER_USR]] | Ref
// CHECK: [[@LINE+4]]:45 | struct/Swift | Double | s:Sd | Ref
// CHECK: [[@LINE+3]]:23 | macro/Swift | MemberAttribute() | [[MEMBER_ATTRIBUTE_USR]] | Ref
// CHECK: [[@LINE+2]]:15 | macro/Swift | Member() | [[MEMBER_USR]] | Ref
// CHECK: [[@LINE+1]]:2 | macro/Swift | Conformance() | [[CONFORMANCE_USR]] | Ref
@Conformance @Member @MemberAttribute @Peer<Double>(arg: 1.0)
struct TestAttached {
  var attachedMember: Int

  @Accessor
  var attachedMemberAccessors: Int
}
// `MemberAttribute` adds `@AddOne` to attachedMember
// CHECK: [[@LINE-8]]:22 | struct/Swift | AddOne | [[ADD_ONE_USR]] | Ref,Impl,RelCont
// CHECK-NEXT: RelCont | instance-property/Swift | attachedMember

// `Accessor` adds getters/setters to `attachedMemberAccessors` that both call `accessorLog`
// CHECK: [[@LINE-8]]:3 | function/Swift | accessorLog() | [[ACC_LOG_USR]] | Ref,Call,Impl,RelCall,RelCont
// CHECK-NEXT: RelCall,RelCont | instance-method/acc-get/Swift | getter:attachedMemberAccessors

// `Member` adds a new member `memberFunc` that calls `memberLog`
// CHECK: [[@LINE-16]]:14 | instance-method/Swift | memberFunc() | [[MEMBER_FUNC_USR:.*]] | Def,Impl,RelChild
// CHECK: [[@LINE-17]]:14 | function/Swift | memberLog() | [[MEMBER_LOG_USR]] | Ref,Call,Impl,RelCall,RelCont
// CHECK-NEXT: RelCall,RelCont | instance-method/Swift | memberFunc() | [[MEMBER_FUNC_USR]]

// `Peer` adds a new inner type `TestPeer` that contains `peerFunc` with a call to `peerLog`
// CHECK: [[@LINE-21]]:39 | struct/Swift | TestPeer | [[PEER_STRUCT_USR:.*]] | Def,Impl
// CHECK: [[@LINE-22]]:39 | instance-method/Swift | peerFunc() | [[PEER_FUNC_USR:.*]] | Def,Impl,RelChild
// CHECK-NEXT: RelChild | struct/Swift | TestPeer | [[PEER_STRUCT_USR]]
// CHECK: [[@LINE-24]]:39 | function/Swift | peerLog() | [[PEER_LOG_USR]] | Ref,Call,Impl,RelCall,RelCont
// CHECK-NEXT: RelCall,RelCont | instance-method/Swift | peerFunc() | [[PEER_FUNC_USR]]

// CHECK: [[@LINE+1]]:8 | struct/Swift | Outer | [[OUTER_USR:.*]] | Def
struct Outer {
  // CHECK: [[@LINE+1]]:4 | macro/Swift | PeerMember() | [[PEER_MEMBER_USR]] | Ref
  @PeerMember
  var anyMember: Int
  // `PeerMember` adds a new `peerMember`
  // CHECK: [[@LINE-3]]:3 | instance-property/Swift | peerMember | {{.*}} | Def,Impl,RelChild
  // CHECK-NEXT: RelChild | struct/Swift | Outer | [[OUTER_USR]]

  // CHECK: [[@LINE+2]]:17 | macro/Swift | Member() | [[MEMBER_USR]] | Ref
  // CHECK: [[@LINE+1]]:4 | macro/Swift | Conformance() | [[CONFORMANCE_USR]] | Ref
  @Conformance @Member
  struct TestInner {}
}
// `Member` adds a new member `memberFunc` that calls `memberLog`
// CHECK: [[@LINE-4]]:16 | instance-method/Swift | memberFunc() | [[INNER_FUNC_USR:.*]] | Def,Impl
// CHECK-NEXT: RelChild | struct/Swift | TestInner
// CHECK: [[@LINE-6]]:16 | function/Swift | memberLog() | [[MEMBER_LOG_USR]] | Ref,Call,Impl,RelCall,RelCont
// CHECK-NEXT: RelCall,RelCont | instance-method/Swift | memberFunc() | [[INNER_FUNC_USR]]


// Expanded extensions are visited last

// `Conformance` adds `TestProto` as a conformance on an extension of `TestAttached`
// CHECK: [[@LINE-51]]:1 | extension/ext-struct/Swift | TestAttached | {{.*}} | Def,Impl
// CHECK: [[@LINE-52]]:1 | protocol/Swift | TestProto | [[PROTO_USR]] | Ref,Impl,RelBase
// CHECK-NEXT: RelBase | extension/ext-struct/Swift | TestAttached

// `Conformance` adds `TestProto` as a conformance on an extension of `TestInner`
// CHECK: [[@LINE-18]]:3 | extension/ext-struct/Swift | TestInner | {{.*}} | Def,Impl
// CHECK: [[@LINE-19]]:3 | protocol/Swift | TestProto | [[PROTO_USR]] | Ref,Impl,RelBase
// CHECK-NEXT: RelBase | extension/ext-struct/Swift | TestInner

//--- IndexMacros.swift
import SwiftSyntax
import SwiftSyntaxBuilder
import SwiftSyntaxMacros

public struct FreestandingExprMacro: ExpressionMacro {
  public static func expansion(
    of node: some FreestandingMacroExpansionSyntax,
    in context: some MacroExpansionContext
  ) -> ExprSyntax {
    return "exprLog()"
  }
}

public struct FreestandingDeclMacro: DeclarationMacro {
  public static func expansion(
    of node: some FreestandingMacroExpansionSyntax,
    in context: some MacroExpansionContext
  ) throws -> [DeclSyntax] {
    return ["""
      struct TestFree {
        func freeFunc() {
          freeLog()
        }
      }
      """]
  }
}

public struct SomeAccessorMacro: AccessorMacro {
  public static func expansion(
    of node: AttributeSyntax,
    providingAccessorsOf declaration: some DeclSyntaxProtocol,
    in context: some MacroExpansionContext
  ) throws -> [AccessorDeclSyntax] {
    return [
      """
        get {
          accessorLog()
          return 1
        }
      """,
      """
        set {
          accessorLog()
        }
      """,
    ]
  }
}

public struct SomeConformanceMacro: ConformanceMacro {
  public static func expansion(
    of node: AttributeSyntax,
    providingConformancesOf decl: some DeclGroupSyntax,
    in context: some MacroExpansionContext
  ) throws -> [(TypeSyntax, GenericWhereClauseSyntax?)] {
    let protocolName: TypeSyntax = "TestProto"
    return [(protocolName, nil)]
  }
}

public struct SomeMemberMacro: MemberMacro {
  public static func expansion(
    of node: AttributeSyntax,
    providingMembersOf declaration: some DeclGroupSyntax,
    in context: some MacroExpansionContext
  ) throws -> [DeclSyntax] {
    let newFunc: DeclSyntax =
      """
      func memberFunc() {
        memberLog()
      }
      """
    return [
      newFunc,
    ]
  }
}

public struct SomeMemberAttributeMacro: MemberAttributeMacro {
  public static func expansion(
    of node: AttributeSyntax,
    attachedTo parent: some DeclGroupSyntax,
    providingAttributesFor member: some DeclSyntaxProtocol,
    in context: some MacroExpansionContext
  ) throws -> [AttributeSyntax] {
    guard let varDecl = member.as(VariableDeclSyntax.self),
      let binding = varDecl.bindings.first,
      let identifier = binding.pattern.as(IdentifierPatternSyntax.self)?.identifier.text,
      identifier == "attachedMember"
    else {
      return []
    }

    return [AttributeSyntax(
      attributeName: SimpleTypeIdentifierSyntax(
        name: .identifier("AddOne")
      )
    )]
  }
}

public struct SomePeerMacro: PeerMacro {
  public static func expansion(
    of node: AttributeSyntax,
    providingPeersOf declaration: some DeclSyntaxProtocol,
    in context: some MacroExpansionContext
  ) throws -> [DeclSyntax] {
    return [
      """
      struct TestPeer {
        func peerFunc() {
          peerLog()
        }
      }
      """
    ]
  }
}

public struct SomePeerMemberMacro: PeerMacro {
  public static func expansion(
    of node: AttributeSyntax,
    providingPeersOf declaration: some DeclSyntaxProtocol,
    in context: some MacroExpansionContext
  ) throws -> [DeclSyntax] {
    return [
      """
      var peerMember: Int
      """
    ]
  }
}
