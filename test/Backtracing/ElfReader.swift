// RUN: %empty-directory(%t)
// RUN: %target-clang -x c -Wno-unused-command-line-argument -Wl,--build-id -g %S/Inputs/fib.c -o %t/fib
// RUN: %target-clang -x c -Wno-unused-command-line-argument -g %S/Inputs/fib.c -o %t/fib-no-uuid
// RUN: %target-clang -x c -Wno-unused-command-line-argument -Wl,--build-id -Wl,--compress-debug-sections=zlib-gnu -g %S/Inputs/fib.c -o %t/fib-compress-gnu
// RUN: %target-clang -x c -Wno-unused-command-line-argument -Wl,--build-id -Wl,--compress-debug-sections=zlib -g %S/Inputs/fib.c -o %t/fib-compress-zlib
// RUN: %target-build-swift %s -parse-as-library -g -o %t/ElfReader
// RUN: %target-run %t/ElfReader %t/fib | %FileCheck %s
// RUN: %target-run %t/ElfReader %t/fib-no-uuid | %FileCheck %s --check-prefix NOUUID
// RUN: %target-run %t/ElfReader %t/fib-compress-gnu | %FileCheck %s --check-prefix CMPGNU
// RUN: %target-run %t/ElfReader %t/fib-compress-zlib | %FileCheck %s --check-prefix CMPZLIB
// RUN: if %S/Inputs/make-minidebug %t/fib %t/fib-minidebug; then ( %target-run %t/ElfReader %t/fib-minidebug | %FileCheck %s --check-prefix MINIDEBUG ); else echo "warning: skipping minidebug test as we couldn't generate minidebug data"; fi
// RUN: libc=$(ldd %t/fib | awk '/libc\.so\.6/ { print $3 }'); if %S/Inputs/has-uuid-syms "$libc" >/dev/null; then %target-run %t/ElfReader "$libc" | %FileCheck %s --check-prefix LIBC; else echo "warning: skipping /usr/lib/debug test as libc symbols are not installed"; fi
// RUN: %S/Inputs/make-debuglink %t/fib %t/fib-stripped %t/fib.dbg && %target-run %t/ElfReader %t/fib-stripped | %FileCheck %s --check-prefix DBGLINK

// REQUIRES: OS=linux-gnu
// REQUIRES: backtracing

@_spi(ElfTest) import _Backtracing

@main
struct ElfReader {

  static func main() {
    if CommandLine.argc != 2 {
      print("usage: ElfReader <path-to-binary>")
      return
    }

    // CHECK: {{.*}}/fib is a {{(32|64)}}-bit ELF image
    // CHECK-NEXT:   uuid: {{[0-9a-f]+}}
    // CHECK-NEXT:   debug image: <none>
    // CHECK-NEXT:   .debug_info: found
    // CHECK-NEXT:   .debug_line: found
    // CHECK-NEXT:   .debug_abbrev: found
    // CHECK-NEXT:   .debug_ranges: not found
    // CHECK-NEXT:   .debug_str: found
    // CHECK-NEXT:   .debug_addr: found
    // CHECK-NEXT:   .debug_str_offsets: found
    // CHECK-NEXT:   .debug_line_str: found
    // CHECK-NEXT:   .debug_rnglists: not found

    // NOUUID: {{.*}}/fib-no-uuid is a {{(32|64)}}-bit ELF image
    // NOUUID-NEXT:   uuid: <no uuid>
    // NOUUID-NEXT:   debug image: <none>
    // NOUUID-NEXT:   .debug_info: found
    // NOUUID-NEXT:   .debug_line: found
    // NOUUID-NEXT:   .debug_abbrev: found
    // NOUUID-NEXT:   .debug_ranges: not found
    // NOUUID-NEXT:   .debug_str: found
    // NOUUID-NEXT:   .debug_addr: found
    // NOUUID-NEXT:   .debug_str_offsets: found
    // NOUUID-NEXT:   .debug_line_str: found
    // NOUUID-NEXT:   .debug_rnglists: not found

    // CMPGNU: {{.*}}/fib-compress-gnu is a {{(32|64)}}-bit ELF image
    // CMPGNU-NEXT:   uuid: {{[0-9a-f]+}}
    // CMPGNU-NEXT:   debug image: <none>
    // CMPGNU-NEXT:   .debug_info: found
    // CMPGNU-NEXT:   .debug_line: found
    // CMPGNU-NEXT:   .debug_abbrev: found
    // CMPGNU-NEXT:   .debug_ranges: not found
    // CMPGNU-NEXT:   .debug_str: found
    // CMPGNU-NEXT:   .debug_addr: found
    // CMPGNU-NEXT:   .debug_str_offsets: found
    // CMPGNU-NEXT:   .debug_line_str: found
    // CMPGNU-NEXT:   .debug_rnglists: not found

    // CMPZLIB: {{.*}}/fib-compress-zlib is a {{(32|64)}}-bit ELF image
    // CMPZLIB-NEXT:   uuid: {{[0-9a-f]+}}
    // CMPZLIB-NEXT:   debug image: <none>
    // CMPZLIB-NEXT:   .debug_info: found
    // CMPZLIB-NEXT:   .debug_line: found
    // CMPZLIB-NEXT:   .debug_abbrev: found
    // CMPZLIB-NEXT:   .debug_ranges: not found
    // CMPZLIB-NEXT:   .debug_str: found
    // CMPZLIB-NEXT:   .debug_addr: found
    // CMPZLIB-NEXT:   .debug_str_offsets: found
    // CMPZLIB-NEXT:   .debug_line_str: found
    // CMPZLIB-NEXT:   .debug_rnglists: not found

    // MINIDEBUG: {{.*}}/fib-minidebug is a {{(32|64)}}-bit ELF image
    // MINIDEBUG-NEXT:   uuid: {{[0-9a-f]+}}
    // MINIDEBUG-NEXT:   debug image: image {{[0-9a-f]+}}
    // MINIDEBUG-NEXT:   .debug_info: found
    // MINIDEBUG-NEXT:   .debug_line: found
    // MINIDEBUG-NEXT:   .debug_abbrev: found
    // MINIDEBUG-NEXT:   .debug_ranges: not found
    // MINIDEBUG-NEXT:   .debug_str: found
    // MINIDEBUG-NEXT:   .debug_addr: found
    // MINIDEBUG-NEXT:   .debug_str_offsets: found
    // MINIDEBUG-NEXT:   .debug_line_str: found
    // MINIDEBUG-NEXT:   .debug_rnglists: not found

    // LIBC: {{.*}}/libc.so.6 is a {{32|64}}-bit ELF image
    // LIBC-NEXT:   uuid: [[PREFIX:[0-9a-f]{2}]][[SUFFIX:[0-9a-f]+]]
    // LIBC-NEXT:   debug image: /usr/lib/debug/.build-id/[[PREFIX]]/[[SUFFIX]].debug
    // LIBC-NEXT:   .debug_info: found
    // LIBC-NEXT:   .debug_line: found
    // LIBC-NEXT:   .debug_abbrev: found
    // LIBC-NEXT:   .debug_ranges:
    // LIBC-NEXT:   .debug_str: found
    // LIBC-NEXT:   .debug_addr:
    // LIBC-NEXT:   .debug_str_offsets:
    // LIBC-NEXT:   .debug_line_str:
    // LIBC-NEXT:   .debug_rnglists:

    // DBGLINK: {{.*}}/fib-stripped is a {{(32|64)}}-bit ELF image
    // DBGLINK-NEXT:   uuid: {{[0-9a-f]+}}
    // DBGLINK-NEXT:   debug image: {{.*}}/fib.dbg
    // DBGLINK-NEXT:   .debug_info: found
    // DBGLINK-NEXT:   .debug_line: found
    // DBGLINK-NEXT:   .debug_abbrev: found
    // DBGLINK-NEXT:   .debug_ranges: not found
    // DBGLINK-NEXT:   .debug_str: found
    // DBGLINK-NEXT:   .debug_addr: found
    // DBGLINK-NEXT:   .debug_str_offsets: found
    // DBGLINK-NEXT:   .debug_line_str: found
    // DBGLINK-NEXT:   .debug_rnglists: not found

    if !testElfImageAt(path: CommandLine.arguments[1]) {
      exit(1)
    }
  }

}

