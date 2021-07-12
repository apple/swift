// RUN: %target-typecheck-verify-swift -I %S/Inputs

// Some types such as pid_t that glibc defines are defined using the following
// construct:
//
// #ifndef __pid_t_defined
// typedef __pid_t pid_t;
// #define __pid_t_defined
// #endif
//
// glibc defines pid_t in this way in multiple header files, as required by
// POSIX (e.g. signal.h, unistd.h, and sys/types.h). A number of other types
// use the same construct (e.g. ssize_t, intptr_t, uid_t, gid_t).
//
// Because Swift does not set the -fmodules-local-submodule-visibility flag,
// the __pid_t_defined macro can leak from the first submodule that defines it
// into all submodules that follow.
//
// As a consequence, a C header file may fail to compile when imported into
// Swift, even though it compiles cleanly outside Swift with the same system
// headers.
//
// This is a regression test for a bug that was once present in SwiftGlibc
// module layout. Originally SwiftGlibc defined one submodule per header, and
// types such as pid_t were defined by the first module that encountered the
// ifndef construct.
//
// See https://forums.swift.org/t/problems-with-swiftglibc-and-proposed-fix/37594
// for further details.

// REQUIRES: OS=linux-gnu || OS=linux-android

import IncludeSignal
