// RUN: %swift -prespecialize-generic-metadata -target %module-target-future -enable-library-evolution -emit-ir %s | %FileCheck %s -DINT=i%target-ptrsize -DALIGNMENT=%target-alignment

// REQUIRES: VENDOR=apple || OS=linux-gnu
// UNSUPPORTED: CPU=i386 && OS=ios
// UNSUPPORTED: CPU=armv7 && OS=ios
// UNSUPPORTED: CPU=armv7s && OS=ios

// CHECK: @"$s4main5ValueOySiGWV" = linkonce_odr hidden constant %swift.enum_vwtable
// CHECK: @"$s4main5ValueOySiGMf" = linkonce_odr hidden constant <{ 
// CHECK-SAME:   ptr, 
// CHECK-SAME:   [[INT]], 
// CHECK-SAME:   ptr, 
// CHECK-SAME:   ptr, 
// CHECK-SAME:   i64 
// CHECK-SAME: }> <{ 
// CHECK-SAME:   $s4main5ValueOySiGWV
// CHECK-SAME:   [[INT]] 513, 
// CHECK-SAME:   $s4main5ValueOMn
// CHECK-SAME:   $sSiN
// Payload size.
// CHECK-SAME:   [[INT]] 8,
// Trailing flags.
// CHECK-SAME:   i64 3 
// CHECK-SAME: }>, align [[ALIGNMENT]]

public struct First<Tag> {
  public let value: Int64
}

public struct Second {
  public let value: Int64
}

@frozen
public enum Value<Tag> {
  case first(First<Tag>)
  case second(Second)

  static func only(_ int: Int) -> Self {
    return .second(Second(value: Int64(int)))
  }
}

@inline(never)
func consume<T>(_ t: T) {
  withExtendedLifetime(t) { t in
  }
}

//      CHECK: define hidden swiftcc void @"$s4main4doityyF"() #{{[0-9]+}} {
//      CHECK:   call swiftcc void @"$s4main7consumeyyxlF"(
// CHECK-SAME:     ptr noalias nocapture %{{[0-9]+}}, 
// CHECK-SAME:     ptr getelementptr inbounds (
// CHECK-SAME:       %swift.full_type, 
// CHECK-SAME:       $s4main5ValueOySiGMf
// CHECK-SAME:       i32 0, 
// CHECK-SAME:       i32 2
// CHECK-SAME:     )
// CHECK-SAME:   )
//      CHECK: }
func doit() {
  consume( Value<Int>.only(13) )
}
doit()

//      CHECK: ; Function Attrs: noinline nounwind readnone
//      CHECK: define{{( protected)?}} swiftcc %swift.metadata_response @"$s4main5ValueOMa"([[INT]] %0, ptr %1) #{{[0-9]+}} {{(section)?.*}}{
//      CHECK:   call swiftcc %swift.metadata_response @__swift_instantiateCanonicalPrespecializedGenericMetadata(
// CHECK-SAME:     [[INT]] %0, 
// CHECK-SAME:     ptr %1, 
// CHECK-SAME:     ptr undef, 
// CHECK-SAME:     ptr undef, 
// CHECK-SAME:     $s4main5ValueOMn
//      CHECK:   ret %swift.metadata_response {{%[0-9]+}}
//      CHECK: }

