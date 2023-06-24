// RUN: %target-swift-frontend %use_no_opaque_pointers -prespecialize-generic-metadata -target %module-target-future -emit-ir %s -swift-version 4 | %FileCheck %s -DINT=i%target-ptrsize -DALIGNMENT=%target-alignment
// RUN: %target-swift-frontend -prespecialize-generic-metadata -target %module-target-future -emit-ir %s -swift-version 4

// REQUIRES: VENDOR=apple || OS=linux-gnu
// UNSUPPORTED: CPU=i386 && OS=ios
// UNSUPPORTED: CPU=armv7 && OS=ios
// UNSUPPORTED: CPU=armv7s && OS=ios

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
// CHECK-LABEL: define{{( dllexport| protected)?}} swiftcc void @"$s30synthesized_conformance_future8equalityyyF"()
public func equality() {
    // CHECK: [[Struct_Equatable:%.*]] = call i8** @"$s30synthesized_conformance_future6StructVySiGACyxGSQAASQRzlWl"()

    // CHECK-NEXT: call swiftcc void @"$s30synthesized_conformance_future10doEqualityyyxSQRzlF"(
    // CHECK-SAME:   %swift.opaque* noalias nocapture {{[^,]*}},
    // CHECK-SAME:   %swift.type* getelementptr inbounds (
    // CHECK-SAME:     %swift.full_type,
    // CHECK-SAME:     %swift.full_type* bitcast (
    // CHECK-SAME:       <{
    // CHECK-SAME:         i8*,
    // CHECK-SAME:         i8**,
    // CHECK-SAME:         [[INT]],
    // CHECK-SAME:         %swift.type_descriptor*,
    // CHECK-SAME:         %swift.type*,
    // CHECK-SAME:         i32, 
    // CHECK-SAME:         {{(\[4 x i8\],)?}}
    // CHECK-SAME:         i64
    // CHECK-SAME:       }>* @"$s30synthesized_conformance_future6StructVySiGMf"
    // CHECK-SAME:       to %swift.full_type*
    // CHECK-SAME:     ),
    // CHECK-SAME:     i32 0,
    // CHECK-SAME:     i32 2
    // CHECK-SAME:   ),
    // CHECK-SAME:   i8** [[Struct_Equatable]]
    // CHECK-SAME: )
    doEquality(Struct(x: 1))
    // CHECK: [[Enum_Equatable:%.*]] = call i8** @"$s30synthesized_conformance_future4EnumOySiGACyxGSQAASQRzlWl"()
    // CHECK-NEXT: call swiftcc void @"$s30synthesized_conformance_future10doEqualityyyxSQRzlF"(
    // CHECK-SAME:   %swift.opaque* noalias nocapture {{%[^,]+}}, 
    // CHECK-SAME:   %swift.type* getelementptr inbounds (
    // CHECK-SAME:     %swift.full_type, 
    // CHECK-SAME:     %swift.full_type* bitcast (
    // CHECK-SAME:       <{ 
    // CHECK-SAME:         i8*,
    // CHECK-SAME:         i8**, 
    // CHECK-SAME:         [[INT]], 
    // CHECK-SAME:         %swift.type_descriptor*, 
    // CHECK-SAME:         %swift.type*, 
    // CHECK-SAME:         [[INT]], 
    // CHECK-SAME:         i64 
    // CHECK-SAME:       }>* @"$s30synthesized_conformance_future4EnumOySiGMf" 
    // CHECK-SAME:       to %swift.full_type*
    // CHECK-SAME:     ), 
    // CHECK-SAME:     i32 0,
    // CHECK-SAME:     i32 2
    // CHECK-SAME:   ), 
    // CHECK-SAME:   i8** [[Enum_Equatable]]
    // CHECK-SAME: )
    doEquality(Enum.a(1))
}

func doEncodable<T: Encodable>(_: T) {}
// CHECK-LABEL: define{{( dllexport| protected)?}} swiftcc void @"$s30synthesized_conformance_future9encodableyyF"()
public func encodable() {
    // CHECK: [[Struct_Encodable:%.*]] = call i8** @"$s30synthesized_conformance_future6StructVySiGACyxGSEAASeRzSERzlWl"()
    // CHECK-NEXT: call swiftcc void @"$s30synthesized_conformance_future11doEncodableyyxSERzlF"(
    // CHECK-SAME:   %swift.opaque* noalias nocapture {{[^,]*}}, 
    // CHECK-SAME:   %swift.type* getelementptr inbounds (
    // CHECK-SAME:     %swift.full_type, 
    // CHECK-SAME:     %swift.full_type* bitcast (
    // CHECK-SAME:       <{ 
    // CHECK-SAME:         i8*,
    // CHECK-SAME:         i8**, 
    // CHECK-SAME:         [[INT]], 
    // CHECK-SAME:         %swift.type_descriptor*, 
    // CHECK-SAME:         %swift.type*, 
    // CHECK-SAME:         i32, 
    // CHECK-SAME:         {{(\[4 x i8\],)?}}
    // CHECK-SAME:         i64 
    // CHECK-SAME:       }>* @"$s30synthesized_conformance_future6StructVySiGMf" 
    // CHECK-SAME:       to %swift.full_type*
    // CHECK-SAME:     ), 
    // CHECK-SAME:     i32 0, 
    // CHECK-SAME:     i32 2
    // CHECK-SAME:   ), 
    // CHECK-SAME:   i8** [[Struct_Encodable]]
    // CHECK-SAME: )
    doEncodable(Struct(x: 1))
    // CHECK: [[Enum_Encodable:%.*]] = call i8** @"$s30synthesized_conformance_future4EnumOySiGACyxGSEAASeRzSERzlWl"()
    // CHECK-NEXT: call swiftcc void @"$s30synthesized_conformance_future11doEncodableyyxSERzlF"(
    // CHECK-SAME:   %swift.opaque* noalias nocapture {{[^,]*}},
    // CHECK-SAME:   %swift.type* getelementptr inbounds (
    // CHECK-SAME:     %swift.full_type,
    // CHECK-SAME:     %swift.full_type* bitcast (
    // CHECK-SAME:       <{
    // CHECK-SAME:         i8*,
    // CHECK-SAME:         i8**,
    // CHECK-SAME:         [[INT]],
    // CHECK-SAME:         %swift.type_descriptor*,
    // CHECK-SAME:         %swift.type*,
    // CHECK-SAME:         [[INT]],
    // CHECK-SAME:         i64
    // CHECK-SAME:       }>* @"$s30synthesized_conformance_future4EnumOySiGMf"
    // CHECK-SAME:       to %swift.full_type*
    // CHECK-SAME:     ),
    // CHECK-SAME:     i32 0,
    // CHECK-SAME:     i32 2
    // CHECK-SAME:   ),
    // CHECK-SAME:   i8** [[Enum_Encodable]]
    // CHECK-SAME: )
    doEncodable(Enum.a(1))
    // CHECK: [[Final_Encodable:%.*]] = call i8** @"$s30synthesized_conformance_future5FinalCySiGACyxGSEAASERzlWl"()
    // CHECK-NEXT: call swiftcc void @"$s30synthesized_conformance_future11doEncodableyyxSERzlF"(%swift.opaque* noalias nocapture {{%.*}}, %swift.type* {{%.*}}, i8** [[Final_Encodable]])
    doEncodable(Final(x: 1))
    // CHECK: [[Nonfinal_Encodable:%.*]] = call i8** @"$s30synthesized_conformance_future8NonfinalCySiGACyxGSEAASERzlWl"()
    // CHECK-NEXT: call swiftcc void @"$s30synthesized_conformance_future11doEncodableyyxSERzlF"(%swift.opaque* noalias nocapture {{%.*}}, %swift.type* {{%.*}}, i8** [[Nonfinal_Encodable]])
    doEncodable(Nonfinal(x: 1))
}
