//=================================================
// ** GENERIC UNIX TARGETS - linking via clang **
//=================================================

// RUN: %swiftc_driver -### -target x86_64-linux-unknown -tools-directory %S/Inputs/fake-toolchain %s 2>&1 | %FileCheck -check-prefix CLANGSUB %s
// RUN: %swiftc_driver -### -target x86_64-linux-unknown -tools-directory /Something/obviously/fake %s 2>&1 | %FileCheck -check-prefix BINUTILS %s

// CLANGSUB: swift
// CLANGSUB: -o [[OBJECTFILE:.*]]
// CLANGSUB: swift-autolink-extract{{(\.exe)?"?}} [[OBJECTFILE]]
// CLANGSUB: -o {{"?}}[[AUTOLINKFILE:.*]]
// CLANGSUB: {{[^ ]+(\\\\|/)}}Inputs{{/|\\\\}}fake-toolchain{{/|\\\\}}clang
// CLANGSUB-DAG: [[OBJECTFILE]]
// CLANGSUB-DAG: @[[AUTOLINKFILE]]
// CLANGSUB: -o tools_directory

// BINUTILS: swift
// BINUTILS: -o [[OBJECTFILE:.*]]
// BINUTILS: swift-autolink-extract{{(\.exe)?"?}} [[OBJECTFILE]]
// BINUTILS: -o {{"?}}[[AUTOLINKFILE:.*]]
// BINUTILS: clang
// BINUTILS-DAG: [[OBJECTFILE]]
// BINUTILS-DAG: @[[AUTOLINKFILE]]
// BINUTILS-DAG: -B /Something/obviously/fake
// BINUTILS: -o tools_directory

//======================================
// ** DARWIN TARGETS - linking via ld **
//======================================

// RUN: %swiftc_driver -### -target x86_64-apple-macosx10.9 -tools-directory %S/Inputs/fake-toolchain %s 2>&1 | %FileCheck -check-prefix LDSUB %s

// LDSUB: swift
// LDSUB: -o [[OBJECTFILE:.*]]
// LDSUB: {{[^ ]+(\\\\|/)}}Inputs{{/|\\\\}}fake-toolchain{{(\\\\|/)ld"?}} [[OBJECTFILE]]
// LDSUB: -o tools_directory
