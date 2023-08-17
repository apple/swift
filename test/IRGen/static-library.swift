// RUN: %empty-directory(%t)
// RUN: %target-swiftc_driver_plain -DLIBRARY -module-name Library -emit-library -static -autolink-force-load -module-link-name Library %s -o %t/library.lib -emit-module-path %t
// RUN: %target-swiftc_driver_plain -DLIBRARY -module-name Library -emit-library -static -autolink-force-load -module-link-name Library %s -S -emit-ir -o - | %FileCheck -check-prefix CHECK-LIBRARY %s
// RUN: %target-swiftc_driver_plain -I %t -emit-library -S -emit-ir -o - %s | %FileCheck -check-prefix CHECK-EMBEDDING %s

// REQUIRES: OS=windows-msvc

#if LIBRARY

// nominal type descriptor for Library.C
// CHECK-LIBRARY: @"$s7Library1CCMn" = constant

// nominal type descriptor for Library.S
// CHECK-LIBRARY: @"$s7Library1SVMn" = constant

// method descriptor for Library.C.method
// CHECK-LBRARY: @"$s7Library1CC6methodyyFTq" = alias

// type metadata for Library.C
// CHECK-LIBRARY: @"$s7Library1CCN" = alias

// type metadata for Library.S
// CHECK-LIBRARY: @"$s7Library1SVN" = alias

public func f() {
}

// Library.f() -> ()
// CHECK-LIBRARY: define swiftcc void @"$s7Library1fyyF"()

open class C {
    var property: () -> () {
        return f
    }

    open func method() {
    }
}

// Library.C.method() -> ()
// CHECK-LIBRARY: define swiftcc void @"$s7Library1CC6methodyyF"(ptr swiftself %0)

// Library.C.deinit
// CHECK-LIBRARY: define swiftcc ptr @"$s7Library1CCfd"(ptr swiftself %0)

// Library.C.__deallocating_deinit
// CHECK-LIBRARY: define swiftcc void @"$s7Library1CCfD"(ptr swiftself %0)

// Library.C.__allocating_init() -> Library.C
// CHECK-LIBRARY: define swiftcc ptr @"$s7Library1CCACycfC"(ptr swiftself %0)

public struct S {
    var member: () -> Void = f
}

// variable initialization expression of Library.S.member : () -> ()
// CHECK-LIBRARY: define swiftcc { ptr, ptr } @"$s7Library1SV6memberyycvpfi"()

// type metadata accessor for Library.C
// CHECK-LIBRARY: define swiftcc %swift.metadata_response @"$s7Library1CCMa"(i64 %0)

// type metadata accessor for Library.S
// CHECK-LIBRARY: define swiftcc %swift.metadata_response @"$s7Library1SVMa"(i64 %0)

// CHECK-LIBRARY: define internal void @"_swift_FORCE_LOAD_$_Library"()

#else
import Library
func f() {
    Library.f()
}

// CHECK-EMBEDDING-NOT: @"_swift_FORCE_LOAD_$_Library"

#endif

