// REQUIRES: swift_swift_parser
// RUN: %empty-directory(%t)
// RUN: echo "[MyProto]" > %t/protocols.json

// RUN: %host-build-swift -swift-version 5 -emit-library -o %t/%target-library-name(MacroDefinition) -module-name=MacroDefinition %S/Inputs/Macros.swift -g -no-toolchain-stdlib-rpath

// RUN: %target-swift-frontend -typecheck -emit-const-values-path %t/ExtractFromMacroExpansion.swiftconstvalues -const-gather-protocols-file %t/protocols.json -primary-file %s -load-plugin-library %t/%target-library-name(MacroDefinition)
// RUN: cat %t/ExtractFromMacroExpansion.swiftconstvalues 2>&1 | %FileCheck %s

// Do the same, but ensure the WMO compilation flow produces the same result
// RUN: %target-swift-frontend -typecheck -emit-const-values-path %t/ExtractFromMacroExpansionWMO.swiftconstvalues -const-gather-protocols-file %t/protocols.json -O %s -load-plugin-library %t/%target-library-name(MacroDefinition)
// RUN: cat %t/ExtractFromMacroExpansionWMO.swiftconstvalues 2>&1 | %FileCheck %s

protocol MyProto { }

@freestanding(declaration, names: named(MacroAddedStruct))
macro AddMacroAddedStruct() = #externalMacro(module: "MacroDefinition", type: "AddStructDeclMacro")

@freestanding(declaration, names: named(macroAddedVar))
macro AddMacroAddedVar() = #externalMacro(module: "MacroDefinition", type: "AddVarDeclMacro")

@attached(extension, conformances: MyProto, names: prefixed(_extension_), named(_Extension_MyProto))
macro AddExtension() = #externalMacro(module: "MacroDefinition", type: "AddExtensionMacro")

@attached(peer, names: prefixed(_peer_))
macro AddPeerVar() = #externalMacro(module: "MacroDefinition", type: "AddPeerVarMacro")

@attached(member, names: prefixed(_member_))
macro AddMemberVar() = #externalMacro(module: "MacroDefinition", type: "AddMemberMacro")

@attached(memberAttribute)
macro AddMacro() = #externalMacro(module: "MacroDefinition", type: "AddMemberAttributeMacro")

@attached(accessor)
macro AddGetter() = #externalMacro(module: "MacroDefinition", type: "GetterMacro")

@attached(peer, names: prefixed(_Peer_))
macro AddPeerStruct() = #externalMacro(module: "MacroDefinition", type: "AddPeerStructMacro")


#AddMacroAddedStruct

@AddExtension
@AddMemberVar
@AddPeerStruct
struct MyStruct {
  #AddMacroAddedVar
  
  @AddPeerVar
  @AddExtension
  @AddMemberVar
  @AddPeerStruct
  struct Inner { }
}

@AddMacro
extension MyStruct {
  func fromFunc() { }
  
  @AddGetter
  var fromVar = 123
}

// CHECK: "typeName": "ExtractFromMacroExpansion.MacroAddedStruct",
// CHECK: "properties": [
// CHECK:   "label": "macroAddedStructMember",
// CHECK:   "type": "Swift.Int",
// CHECK:   "valueKind": "RawLiteral",
// CHECK:   "value": "1"

// CHECK:   "label": "_extension_MacroAddedStruct",
// CHECK:   "type": "Swift.Int",
// CHECK:   "valueKind": "RawLiteral",
// CHECK:   "value": "3"


// CHECK: "typeName": "ExtractFromMacroExpansion.MyStruct",
// CHECK: "properties": [
// CHECK:   "label": "macroAddedVar",
// CHECK:   "type": "Swift.Int",
// CHECK:   "valueKind": "RawLiteral",
// CHECK:   "value": "2"

// CHECK:   "label": "_peer_Inner",
// CHECK:   "type": "Swift.Int",
// CHECK:   "valueKind": "RawLiteral",
// CHECK:   "value": "4"

// CHECK:   "label": "_member_MyStruct",
// CHECK:   "type": "Swift.Int",
// CHECK:   "valueKind": "RawLiteral",
// CHECK:   "value": "5"

// CHECK:   "label": "_peer_fromFunc",
// CHECK:   "type": "Swift.Int",
// CHECK:   "valueKind": "RawLiteral",
// CHECK:   "value": "4"

// CHECK:   "label": "_peer_fromVar",
// CHECK:   "type": "Swift.Int",
// CHECK:   "valueKind": "RawLiteral",
// CHECK:   "value": "4"

// CHECK:   "label": "fromVar",
// CHECK:   "type": "Swift.Int",
// CHECK:   "valueKind": "RawLiteral",
// CHECK:   "value": "123"

// CHECK:   "label": "_extension_MyStruct",
// CHECK:   "type": "Swift.Int",
// CHECK:   "valueKind": "RawLiteral",
// CHECK:   "value": "3"


// CHECK: "typeName": "ExtractFromMacroExpansion._Peer_MyStruct",
// CHECK: "properties": [
// CHECK:   "label": "peerMacroVar",
// CHECK:   "type": "Swift.Int",
// CHECK:   "valueKind": "RawLiteral",
// CHECK:   "value": "7"

// CHECK:   "label": "macroAddedVar",
// CHECK:   "type": "Swift.Int",
// CHECK:   "valueKind": "RawLiteral",
// CHECK:   "value": "2"

// CHECK:   "label": "_peer_peerMacroVar",
// CHECK:   "type": "Swift.Int",
// CHECK:   "valueKind": "RawLiteral",
// CHECK:   "value": "4"

// CHECK:   "label": "_member__Peer_MyStruct",
// CHECK:   "type": "Swift.Int",
// CHECK:   "valueKind": "RawLiteral",
// CHECK:   "value": "5"

// CHECK:   "label": "_extension__Peer_MyStruct",
// CHECK:   "type": "Swift.Int",
// CHECK:   "valueKind": "RawLiteral",
// CHECK:   "value": "3"


// CHECK: "typeName": "ExtractFromMacroExpansion.MyStruct.Inner",
// CHECK: "properties": [
// CHECK:   "label": "_member_Inner",
// CHECK:   "type": "Swift.Int",
// CHECK:   "valueKind": "RawLiteral",
// CHECK:   "value": "5"

// CHECK:   "label": "_extension_Inner",
// CHECK:   "type": "Swift.Int",
// CHECK:   "valueKind": "RawLiteral",
// CHECK:   "value": "3"


// CHECK: "typeName": "ExtractFromMacroExpansion.MyStruct._Peer_Inner",
// CHECK: "properties": [
// CHECK:   "label": "peerMacroVar",
// CHECK:   "type": "Swift.Int",
// CHECK:   "valueKind": "RawLiteral",
// CHECK:   "value": "7"

// CHECK:   "label": "macroAddedVar",
// CHECK:   "type": "Swift.Int",
// CHECK:   "valueKind": "RawLiteral",
// CHECK:   "value": "2"

// CHECK:   "label": "_peer_peerMacroVar",
// CHECK:   "type": "Swift.Int",
// CHECK:   "valueKind": "RawLiteral",
// CHECK:   "value": "4"

// CHECK:   "label": "_member__Peer_Inner",
// CHECK:   "type": "Swift.Int",
// CHECK:   "valueKind": "RawLiteral",
// CHECK:   "value": "5"

// CHECK:   "label": "_extension__Peer_Inner",
// CHECK:   "type": "Swift.Int",
// CHECK:   "valueKind": "RawLiteral",
// CHECK:   "value": "3"


// CHECK: "typeName": "ExtractFromMacroExpansion.MacroAddedStruct._Extension_MyProto",
// CHECK: "properties": [
// CHECK:   "label": "nested",
// CHECK:   "type": "Swift.Int",
// CHECK:   "valueKind": "RawLiteral",
// CHECK:   "value": "8"


// CHECK: "typeName": "ExtractFromMacroExpansion._Peer_MyStruct._Extension_MyProto",
// CHECK: "properties": [
// CHECK:   "label": "nested",
// CHECK:   "type": "Swift.Int",
// CHECK:   "valueKind": "RawLiteral",
// CHECK:   "value": "8"

// CHECK: "typeName": "ExtractFromMacroExpansion.MyStruct._Extension_MyProto",
// CHECK: "properties": [
// CHECK:   "label": "nested",
// CHECK:   "type": "Swift.Int",
// CHECK:   "valueKind": "RawLiteral",
// CHECK:   "value": "8"


// CHECK: "typeName": "ExtractFromMacroExpansion.MyStruct._Peer_Inner._Extension_MyProto",
// CHECK: "properties": [
// CHECK:   "label": "nested",
// CHECK:   "type": "Swift.Int",
// CHECK:   "valueKind": "RawLiteral",
// CHECK:   "value": "8"


// CHECK: "typeName": "ExtractFromMacroExpansion.MyStruct.Inner._Extension_MyProto",
// CHECK: "properties": [
// CHECK:   "label": "nested",
// CHECK:   "type": "Swift.Int",
// CHECK:   "valueKind": "RawLiteral",
// CHECK:   "value": "8"
