// Tests that under -enable-llvm-vfe + internalize-at-link,
// virtual calls made to a type hierarchy split across modules is safe

// RUN: %empty-directory(%t)
// RUN: split-file %s %t

// (1) Build the base swiftmodule
// RUN: %target-build-swift -parse-as-library -Xfrontend -enable-llvm-vfe %t/BaseModule.swift -module-name BaseModule -emit-module -o %t/BaseModule.swiftmodule \
// RUN:                     -emit-tbd -emit-tbd-path %t/libBase.tbd -Xfrontend -tbd-install_name=%t/libBase.dylib

// (2) Build the derived swiftmodule
// RUN: %target-build-swift -parse-as-library -I%t -Xfrontend -enable-llvm-vfe %t/DerivedModule.swift -module-name DerivedModule -emit-module -o %t/DerivedModule.swiftmodule \
// RUN:                     -emit-tbd -emit-tbd-path %t/libDerived.tbd -Xfrontend -tbd-install_name=%t/libDerived.dylib
                            
// (3) Build the client
// RUN: %target-build-swift -parse-as-library -Xfrontend -enable-llvm-vfe -I%t %t/Client.swift -L%t -lBase -lDerived -o %t/main

// (4) Extract a list of used symbols by client from base
// RUN: %llvm-nm --undefined-only -m %t/main | grep 'from libBase' | awk '{print $3}' > %t/base-used-symbols

// (5) Extract a list of used symbols by client from derived
// RUN: %llvm-nm --undefined-only -m %t/main | grep 'from libDerived' | awk '{print $3}' > %t/derived-used-symbols

// (6) Build libDerived.dylib with only the requested symbols
// RUN: %target-build-swift -I%t -parse-as-library -Xfrontend -enable-llvm-vfe -Xfrontend -internalize-at-link \
// RUN:    %t/DerivedModule.swift -lto=llvm-full %lto_flags -module-name DerivedModule -emit-library -o %t/libDerived.dylib \
// RUN:    -runtime-compatibility-version none -Xlinker -exported_symbols_list -Xlinker %t/derived-used-symbols -Xlinker -dead_strip -L%t -lBase

// (4) Extract a list of used symbols by derived from base, appending to the same list as earlier
// RUN: %llvm-nm --undefined-only -m %t/libDerived.dylib | grep 'from libBase' | awk '{print $3}' >> %t/base-used-symbols

// (5) Build libBase.dylib with only the symbols requested by either the client or derived
// RUN: %target-build-swift -parse-as-library -Xfrontend -enable-llvm-vfe -Xfrontend -internalize-at-link \
// RUN:    %t/BaseModule.swift -lto=llvm-full %lto_flags -module-name BaseModule -emit-library -o %t/libBase.dylib \
// RUN:    -runtime-compatibility-version none -Xlinker -exported_symbols_list -Xlinker %t/base-used-symbols -Xlinker -dead_strip

// (6) Check list of symbols in libDerived.dylib
// RUN: %llvm-nm --defined-only %t/libDerived.dylib | %FileCheck --check-prefix DERIVED-SYMBOLS %s // This fails

// (7) Run client
// RUN: %target-run %t/main | %FileCheck %s // This also fails in that we don't see Derived.foo printed. I'm surprised it doesn't crash all together

//--- BaseModule.swift

open class Base {
    public init() {}
    open func foo() {
        print("Base.foo")
    }
}

//--- DerivedModule.swift

import BaseModule

public class Derived : Base {
    override public func foo() {
        print("Derived.foo")
    }
}

public func getBase() -> Base {
    return Derived()
}

// DERIVED-SYMBOLS: s13DerivedModule0A0C3fooyyF

//--- Client.swift

import BaseModule
import DerivedModule

@_cdecl("main")
func main() -> Int32 {
    let base = getBase()
    base.foo()
    // CHECK: Derived.foo
    return 0;
}
