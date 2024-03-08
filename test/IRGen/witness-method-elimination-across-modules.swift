// Tests that under -enable-llvm-vfe + -internalize-at-link,
// calls to a witness method that satisfies a protcol requirement
// defined in another module are safe.

// RUN: %empty-directory(%t)
// RUN: split-file %s %t

// (1) Build the ProtocolModule swift module
// RUN: %target-build-swift -Xfrontend -enable-llvm-wme -parse-as-library -emit-module %t/ProtocolModule.swift -module-name ProtocolModule -emit-module \
// RUN:     -o %t/ProtocolModule.swiftmodule -emit-tbd -emit-tbd-path %t/libProtocol.tbd -Xfrontend -tbd-install_name=%t/libProtocol.dylib

// (2) Build the ConformingClassModule swift module
// RUN: %target-build-swift -Xfrontend -enable-llvm-wme -parse-as-library -I %t -emit-module %t/ConformingClassModule.swift -module-name ConformingClassModule -emit-module \
// RUN:     -o %t/ConformingClassModule.swiftmodule -emit-tbd -emit-tbd-path %t/libConformingClass.tbd -Xfrontend -tbd-install_name=%t/libConformingClass.dylib

// (3) Build the client
// RUN: %target-build-swift -parse-as-library -Xfrontend -enable-llvm-wme -I%t %t/Client.swift -L%t -lProtocol -lConformingClass -o %t/main

// (4) Extract a list of used symbols by client from protocol
// RUN: %llvm-nm --undefined-only -m %t/main | grep 'libProtocol' | awk '{print $3}' > %t/protocol-used-symbols

// (5) Extract a list of used symbols by client from derived
// RUN: %llvm-nm --undefined-only -m %t/main | grep 'libConformingClass' | awk '{print $3}' > %t/conforming-used-symbols

// (6) Build libConformingClass.dylib with only the requested symbols
// RUN: %target-build-swift -I%t -parse-as-library -Xfrontend -enable-llvm-wme -Xfrontend -internalize-at-link \
// RUN:    %t/ConformingClassModule.swift -lto=llvm-full %lto_flags -module-name ConformingClassModule -emit-library -o %t/libConformingClass.dylib \
// RUN:    -runtime-compatibility-version none -Xlinker -exported_symbols_list -Xlinker %t/conforming-used-symbols -Xlinker -dead_strip -L%t -lProtocol

// (7) Extract a list of used symbols by conforming class from protocol, appending to the same list as earlier
// RUN: %llvm-nm --undefined-only -m %t/libConformingClass.dylib | grep 'libProtocol' | awk '{print $3}' >> %t/protocol-used-symbols

// (8) Build libProtocol.dylib with only the symbols requested by either the client or conforming
// RUN: %target-build-swift -parse-as-library -Xfrontend -enable-llvm-wme -Xfrontend -internalize-at-link \
// RUN:    %t/ProtocolModule.swift -lto=llvm-full %lto_flags -module-name ProtocolModule -emit-library -o %t/libProtocol.dylib \
// RUN:    -runtime-compatibility-version none -Xlinker -exported_symbols_list -Xlinker %t/protocol-used-symbols -Xlinker -dead_strip

// (9) Check list of symbols in libConformingClass.dylib
// RUN: %llvm-nm --defined-only %t/libConformingClass.dylib | %FileCheck %s --check-prefix CONFORMING-SYMBOLS

// (10) Run the client
// RUN: %target-run %t/main | %FileCheck %s

// REQUIRES: executable_test
// REQUIRES: VENDOR=apple
// REQUIRES: no_asan
// UNSUPPORTED: remote_run

//--- ProtocolModule.swift

public protocol Protocol {
    func foo()
}

//--- ConformingClassModule.swift

import ProtocolModule

public class Class : Protocol {
    public func foo() {
        print("Class.foo")
    }
}

// CONFORMING-SYMBOLS: s21ConformingClassModule0B0C08ProtocolC00D0AadEP3fooyyFTW

public func getProtocolConformer() -> Protocol {
    return Class()
}

//--- Client.swift

import ProtocolModule
import ConformingClassModule

@_cdecl("main")
func main() -> Int32 {
    let p = getProtocolConformer()
    p.foo()
    // CHECK: Class.foo
    return 0
}
