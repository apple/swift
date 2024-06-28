// RUN: %target-swift-frontend -emit-silgen %s -swift-version 4 | %FileCheck -check-prefix CHECK -check-prefix CHECK-FRAGILE %s
// RUN: %target-swift-frontend -emit-silgen %s -swift-version 4 -enable-library-evolution | %FileCheck -check-prefix CHECK -check-prefix CHECK-RESILIENT %s

enum Enum<T> {
    case a(T), b(T)
}
// CHECK-LABEL: enum Enum<T> {
// CHECK:   case a(T), b(T)
// CHECK: }

enum NoValues {
    case a, b
}
// CHECK-LABEL: enum NoValues {
// CHECK:   case a, b
// CHECK-FRAGILE:   @_implements(Equatable, ==(_:_:)) static func __derived_enum_equals(_ a: NoValues, _ b: NoValues) -> Bool
// CHECK-RESILIENT: static func == (a: NoValues, b: NoValues) -> Bool
// CHECK:   func hash(into hasher: inout Hasher)
// CHECK:   var hashValue: Int { get }
// CHECK: }

// CHECK-LABEL: extension Enum : Equatable where T : Equatable {
// CHECK-FRAGILE:   @_implements(Equatable, ==(_:_:)) static func __derived_enum_equals(_ a: Enum<T>, _ b: Enum<T>) -> Bool
// CHECK-RESILIENT: static func == (a: Enum<T>, b: Enum<T>) -> Bool
// CHECK: }
// CHECK-LABEL: extension Enum : Hashable where T : Hashable {
// CHECK:   func hash(into hasher: inout Hasher)
// CHECK:   var hashValue: Int { get }
// CHECK: }

// CHECK-LABEL: extension NoValues : CaseIterable {
// CHECK:   typealias AllCases = [NoValues]
// CHECK:   static var allCases: [NoValues] { get }
// CHECK: }


extension Enum: Equatable where T: Equatable {}
// CHECK-FRAGILE-LABEL: // static Enum<A>.__derived_enum_equals(_:_:)
// CHECK-FRAGILE-NEXT: sil hidden [ossa] @$s28synthesized_conformance_enum4EnumOAASQRzlE010__derived_C7_equalsySbACyxG_AEtFZ : $@convention(method) <T where T : Equatable> (@in_guaranteed Enum<T>, @in_guaranteed Enum<T>, @thin Enum<T>.Type) -> Bool {
// CHECK-RESILIENT-LABEL: // static Enum<A>.== infix(_:_:)
// CHECK-RESILIENT-NEXT: sil hidden [ossa] @$s28synthesized_conformance_enum4EnumOAASQRzlE2eeoiySbACyxG_AEtFZ : $@convention(method) <T where T : Equatable> (@in_guaranteed Enum<T>, @in_guaranteed Enum<T>, @thin Enum<T>.Type) -> Bool {

extension Enum: Hashable where T: Hashable {}
// CHECK-LABEL: // Enum<A>.hash(into:)
// CHECK-NEXT: sil hidden [ossa] @$s28synthesized_conformance_enum4EnumOAASHRzlE4hash4intoys6HasherVz_tF : $@convention(method) <T where T : Hashable> (@inout Hasher, @in_guaranteed Enum<T>) -> () {

// CHECK-LABEL: // Enum<A>.hashValue.getter
// CHECK-NEXT: sil hidden [ossa] @$s28synthesized_conformance_enum4EnumOAASHRzlE9hashValueSivg : $@convention(method) <T where T : Hashable> (@in_guaranteed Enum<T>) -> Int {

extension Enum: Codable where T: Codable {}
// CHECK-LABEL: // Enum<A>.encode(to:)
// CHECK-NEXT: sil hidden [ossa] @$s28synthesized_conformance_enum4EnumOAASeRzSERzlE6encode2toys7Encoder_p_tKF : $@convention(method) <T where T : Decodable, T : Encodable> (@in_guaranteed any Encoder, @in_guaranteed Enum<T>) -> @error any Error {

// CHECK-LABEL: // Enum<A>.init(from:)
// CHECK-NEXT: sil hidden [ossa] @$s28synthesized_conformance_enum4EnumOAASeRzSERzlE4fromACyxGs7Decoder_p_tKcfC : $@convention(method) <T where T : Decodable, T : Encodable> (@in any Decoder, @thin Enum<T>.Type) -> (@out Enum<T>, @error any Error)

extension NoValues: CaseIterable {}
// CHECK-LABEL: // static NoValues.allCases.getter
// CHECK-NEXT: // Isolation: nonisolated
// CHECK-NEXT: sil hidden [ossa] @$s28synthesized_conformance_enum8NoValuesO8allCasesSayACGvgZ : $@convention(method) (@thin NoValues.Type) -> @owned Array<NoValues> {

extension NoValues: Codable {}
// CHECK-LABEL: // NoValues.encode(to:)
// CHECK-NEXT: sil hidden [ossa] @$s28synthesized_conformance_enum8NoValuesO6encode2toys7Encoder_p_tKF : $@convention(method) (@in_guaranteed any Encoder, NoValues) -> @error any Error {

// CHECK-LABEL: // NoValues.init(from:)
// CHECK-NEXT: sil hidden [ossa] @$s28synthesized_conformance_enum8NoValuesO4fromACs7Decoder_p_tKcfC : $@convention(method) (@in any Decoder, @thin NoValues.Type) -> (NoValues, @error any Error)

// Witness tables for Enum

// CHECK-LABEL: sil_witness_table hidden <T where T : Equatable> Enum<T>: Equatable module synthesized_conformance_enum {
// CHECK-NEXT:   method #Equatable."==": <Self where Self : Equatable> (Self.Type) -> (Self, Self) -> Bool : @$s28synthesized_conformance_enum4EnumOyxGSQAASQRzlSQ2eeoiySbx_xtFZTW	// protocol witness for static Equatable.== infix(_:_:) in conformance <A> Enum<A>
// CHECK-NEXT:   conditional_conformance (T: Equatable): dependent
// CHECK-NEXT: }

// CHECK-LABEL: sil_witness_table hidden <T where T : Hashable> Enum<T>: Hashable module synthesized_conformance_enum {
// CHECK-DAG:   base_protocol Equatable: <T where T : Equatable> Enum<T>: Equatable module synthesized_conformance_enum
// CHECK-DAG:   method #Hashable.hashValue!getter: <Self where Self : Hashable> (Self) -> () -> Int : @$s28synthesized_conformance_enum4EnumOyxGSHAASHRzlSH9hashValueSivgTW	// protocol witness for Hashable.hashValue.getter in conformance <A> Enum<A>
// CHECK-DAG:   method #Hashable.hash: <Self where Self : Hashable> (Self) -> (inout Hasher) -> () : @$s28synthesized_conformance_enum4EnumOyxGSHAASHRzlSH4hash4intoys6HasherVz_tFTW	// protocol witness for Hashable.hash(into:) in conformance <A> Enum<A>
// CHECK-DAG:   method #Hashable._rawHashValue: <Self where Self : Hashable> (Self) -> (Int) -> Int : @$s28synthesized_conformance_enum4EnumOyxGSHAASHRzlSH13_rawHashValue4seedS2i_tFTW // protocol witness for Hashable._rawHashValue(seed:) in conformance <A> Enum<A>
// CHECK-DAG:   conditional_conformance (T: Hashable): dependent
// CHECK: }

// CHECK-LABEL: sil_witness_table hidden <T where T : Decodable, T : Encodable> Enum<T>: Decodable module synthesized_conformance_enum {
// CHECK-NEXT:   method #Decodable.init!allocator: <Self where Self : Decodable> (Self.Type) -> (any Decoder) throws -> Self : @$s28synthesized_conformance_enum4EnumOyxGSeAASeRzSERzlSe4fromxs7Decoder_p_tKcfCTW	// protocol witness for Decodable.init(from:) in conformance <A> Enum<A>
// CHECK-NEXT:   conditional_conformance (T: Decodable): dependent
// CHECK-NEXT:   conditional_conformance (T: Encodable): dependent
// CHECK-NEXT: }

// CHECK-LABEL: sil_witness_table hidden <T where T : Decodable, T : Encodable> Enum<T>: Encodable module synthesized_conformance_enum {
// CHECK-NEXT:   method #Encodable.encode: <Self where Self : Encodable> (Self) -> (any Encoder) throws -> () : @$s28synthesized_conformance_enum4EnumOyxGSEAASeRzSERzlSE6encode2toys7Encoder_p_tKFTW	// protocol witness for Encodable.encode(to:) in conformance <A> Enum<A>
// CHECK-NEXT:   conditional_conformance (T: Decodable): dependent
// CHECK-NEXT:   conditional_conformance (T: Encodable): dependent
// CHECK-NEXT: }

// Witness tables for NoValues

// CHECK-LABEL: sil_witness_table hidden NoValues: CaseIterable module synthesized_conformance_enum {
// CHECK-NEXT:   associated_type_protocol (AllCases: Collection): [NoValues]: specialize <NoValues> (<Element> Array<Element>: Collection module Swift)
// CHECK-NEXT:   associated_type AllCases: Array<NoValues>
// CHECK-NEXT:   method #CaseIterable.allCases!getter: <Self where Self : CaseIterable> (Self.Type) -> () -> Self.AllCases : @$s28synthesized_conformance_enum8NoValuesOs12CaseIterableAAsADP8allCases03AllI0QzvgZTW // protocol witness for static CaseIterable.allCases.getter in conformance NoValues
// CHECK-NEXT: }

// CHECK-LABEL: sil_witness_table hidden NoValues: Decodable module synthesized_conformance_enum {
// CHECK-NEXT:   method #Decodable.init!allocator: <Self where Self : Decodable> (Self.Type) -> (any Decoder) throws -> Self : @$s28synthesized_conformance_enum8NoValuesOSeAASe4fromxs7Decoder_p_tKcfCTW // protocol witness for Decodable.init(from:) in conformance NoValues
// CHECK-NEXT: }

// CHECK-LABEL: sil_witness_table hidden NoValues: Encodable module synthesized_conformance_enum {
// CHECK-NEXT:   method #Encodable.encode: <Self where Self : Encodable> (Self) -> (any Encoder) throws -> () : @$s28synthesized_conformance_enum8NoValuesOSEAASE6encode2toys7Encoder_p_tKFTW // protocol witness for Encodable.encode(to:) in conformance NoValues
// CHECK-NEXT: }
