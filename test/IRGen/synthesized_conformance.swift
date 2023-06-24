// RUN: %target-swift-frontend -disable-generic-metadata-prespecialization -emit-ir %s -swift-version 4 | %FileCheck %s

struct Struct<T> {
    var x: T
}

extension Struct: Equatable where T: Equatable {}
extension Struct: Hashable where T: Hashable {}
extension Struct: Codable where T: Codable {}

enum Enum<T> {
    case a(T), b(T)
}

extension Enum: Equatable where T: Equatable {}
extension Enum: Hashable where T: Hashable {}
extension Enum: Codable where T: Codable {}

final class Final<T> {
    var x: T
    init(x: T) { self.x = x }
}

extension Final: Encodable where T: Encodable {}
extension Final: Decodable where T: Decodable {}

class Nonfinal<T> {
    var x: T
    init(x: T) { self.x = x }
}
extension Nonfinal: Encodable where T: Encodable {}

func doEquality<T: Equatable>(_: T) {}
// CHECK-LABEL: define{{( dllexport| protected)?}} swiftcc void @"$s23synthesized_conformance8equalityyyF"()
public func equality() {
    // CHECK: [[Struct_Equatable:%.*]] = call ptr @"$s23synthesized_conformance6StructVySiGACyxGSQAASQRzlWl"()
    // CHECK-NEXT: call swiftcc void @"$s23synthesized_conformance10doEqualityyyxSQRzlF"(ptr noalias nocapture {{%.*}}, ptr {{%.*}}, ptr [[Struct_Equatable]])
    doEquality(Struct(x: 1))
    // CHECK: [[Enum_Equatable:%.*]] = call ptr @"$s23synthesized_conformance4EnumOySiGACyxGSQAASQRzlWl"()
    // CHECK-NEXT: call swiftcc void @"$s23synthesized_conformance10doEqualityyyxSQRzlF"(ptr noalias nocapture {{%.*}}, ptr {{%.*}}, ptr [[Enum_Equatable]])
    doEquality(Enum.a(1))
}

func doEncodable<T: Encodable>(_: T) {}
// CHECK-LABEL: define{{( dllexport| protected)?}} swiftcc void @"$s23synthesized_conformance9encodableyyF"()
public func encodable() {
    // CHECK: [[Struct_Encodable:%.*]] = call ptr @"$s23synthesized_conformance6StructVySiGACyxGSEAASeRzSERzlWl"()
    // CHECK-NEXT: call swiftcc void @"$s23synthesized_conformance11doEncodableyyxSERzlF"(ptr noalias nocapture {{%.*}}, ptr {{%.*}}, ptr [[Struct_Encodable]])
    doEncodable(Struct(x: 1))
    // CHECK: [[Enum_Encodable:%.*]] = call ptr @"$s23synthesized_conformance4EnumOySiGACyxGSEAASeRzSERzlWl"()
    // CHECK-NEXT: call swiftcc void @"$s23synthesized_conformance11doEncodableyyxSERzlF"(ptr noalias nocapture {{%.*}}, ptr {{%.*}}, ptr [[Enum_Encodable]])
    doEncodable(Enum.a(1))
    // CHECK: [[Final_Encodable:%.*]] = call ptr @"$s23synthesized_conformance5FinalCySiGACyxGSEAASERzlWl"()
    // CHECK-NEXT: call swiftcc void @"$s23synthesized_conformance11doEncodableyyxSERzlF"(ptr noalias nocapture {{%.*}}, ptr {{%.*}}, ptr [[Final_Encodable]])
    doEncodable(Final(x: 1))
    // CHECK: [[Nonfinal_Encodable:%.*]] = call ptr @"$s23synthesized_conformance8NonfinalCySiGACyxGSEAASERzlWl"()
    // CHECK-NEXT: call swiftcc void @"$s23synthesized_conformance11doEncodableyyxSERzlF"(ptr noalias nocapture {{%.*}}, ptr {{%.*}}, ptr [[Nonfinal_Encodable]])
    doEncodable(Nonfinal(x: 1))
}
