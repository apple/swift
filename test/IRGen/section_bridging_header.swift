// RUN: %empty-directory(%t)
// RUN: %{python} %utils/split_file.py -o %t %s
// RUN: %target-swift-frontend -enable-experimental-feature SymbolLinkageMarkers -enforce-exclusivity=unchecked -parse-as-library -emit-sil -import-objc-header %t/bridge.h %t/file.swift -o /dev/null

// REQUIRES: swift_in_compiler

// BEGIN bridge.h
struct MyStruct1 {
	int x;
	void (*fptr1)();
	void (*fptr2)(int);
	void (*fptr3)(int, char, void *);
};

// BEGIN file.swift

func foo() { }
@_section("__TEXT,__mysection") var my_global1 = MyStruct1(
	x: 42,
	fptr1: foo,
	fptr2: nil,
	fptr3: { arg1, arg2, arg3 in print(arg1, arg2, arg3) }
	)
