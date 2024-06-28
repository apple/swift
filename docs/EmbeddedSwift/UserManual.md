# Embedded Swift -- User Manual

**⚠️ Embedded Swift is experimental. This document might be out of date with latest development.**

**‼️ Use the latest downloadable 'Trunk Development' snapshot from swift.org to use Embedded Swift. Public releases of Swift do not yet support Embedded Swift.**

For an introduction and motivation into Embedded Swift, please see "[A Vision for Embedded Swift](https://github.com/apple/swift-evolution/blob/main/visions/embedded-swift.md)", a Swift Evolution document highlighting the main goals and approaches.

The following document explains how to use Embedded Swift's support in the Swift compiler and toolchain.

## What Embedded Swift is, and what it isn't

- Embedded Swift **is** a way to produce small and freestanding binaries (with no, or trivial dependencies).
- Embedded Swift **is not** a complete one-click solution to program all embedded boards and MCUs.
- Embedded Swift **is** a compilation model that's analogous to a traditional C compiler in the sense that the compiler produces an object file (.o) that can be simply linked with your existing code, and it's not going to require you to port any libraries or runtimes.
- Embedded Swift **is not** a HAL, it's not an SDK for development, it's not a set of libraries to program peripherals using high-level APIs. It's instead a compilation mode that's suitable for creating these components.

## Using Embedded Swift

A typical setup and build + run cycle for an embedded development board involves:

- (1) Getting an SDK with the C compilers, headers and libraries for the target
- (2) Building the C source code, and Swift source code into object files.
- (3) Linking all the libraries, C object files, and Swift object files.
- (4) Post-processing the linked firmware into a flashable format (UF2, BIN, or bespoke formats)
- (5) Uploading the flashable binary to the board over a USB cable using some vendor-provided JTAG/SWD tool or by copying it to a fake USB Mass Storage volume presented by the board.
- (6) Restarting the board, observing physical effects of the firmware (LEDs light up) or UART output over USB, or presence on network, etc.

Most of these steps are out of scope for this document, instead refer to the vendor provided documentation. This document only focuses on (2) from the list above, and it's important that you first get familiar with the details of firmware development for your board without Swift in the mix. Even if you want to build a completely pure Swift firmware, you are still going to need the vendor provided tooling for linking, post-processing, uploading, etc.

## Building code using Embedded Swift

A basic way to build a set of Swift source files in Embedded Swift mode, is to simply give the compiler (1) a target triple, (2) the `-enable-experimental-feature Embedded` flag, (3) the set of source files that form the input module:

```bash
$ swiftc -target <target triple> -enable-experimental-feature Embedded -wmo \
  input1.swift input2.swift ... -c -o output.o
```

## Examples

### Building Swift firmware for an embedded target

To build Swift firmware (for now ignoring integration with SDKs, libraries and other pre-existing C code), we can use the `-target` argument to specify the CPU architecture. The target triple also decides whether the output object file will be an ELF file, or a Mach-O. For example:

```bash
# To build an ARMv7 Mach-O object file:
$ swiftc -target armv7-apple-none-macho -enable-experimental-feature Embedded -wmo \
  input1.swift input2.swift ... -c -o output.o

# To build an ARMv7 ELF object file:
$ swiftc -target armv7-none-none-eabi -enable-experimental-feature Embedded -wmo \
  input1.swift input2.swift ... -c -o output.o
```

Additionally, you probably want to specify additional Clang and/or LLVM flags to get the compiler to produce code for the exact ISA and ABI you need for your target. 

For example, a Raspberry Pi Pico / Pico W should target the ARMv6-M architecture via the `armv6m-*` target triple, but the `-mfloat-abi=soft` Clang option should also be used, and if you want to match ABI with libraries built with the GNU toolchain, you might also need `-fshort-enums`. To pass those to Swift, use the `-Xcc` prefix:

```bash
# To build an ELF object file for ARMv6-M with soft float ABI (floating-point arguments passed in integer registers) and "short enums":
$ swiftc -target armv6m-none-none-eabi -enable-experimental-feature Embedded -wmo \
   -Xcc -mfloat-abi=soft -Xcc -fshort-enums \
  input1.swift input2.swift ... -c -o output.o
```

This might not be obvious: `-Xcc` flags are typically only used to alter behavior of the Clang importer, but passing flags to Clang this way also works to specify LLVM target options like selecting a specific CPU architecture (`-march`, `-mcpu`, `-mmcu`), FPU unit availability (`-mfpu`), which registers are used to pass floating-point values (`-mfloat-abi`), and others.

### Integrating with embedded SDKs and build systems

For details and concrete examples of how to integrate with existing SDKs, see [Embedded Swift -- Integrating with embedded SDKs](IntegratingWithSDKs.md).

### Building a macOS Embedded Swift program:

It's also possible to build in Embedded Swift mode for regular non-embedded operating systems, like macOS. This is very useful for testing purposes, or if you just want to observe and experiment with Embedded Swift. A simple source code like this:

```swift
print("Hello, embedded world!")
```

...can be compiled using the `-enable-experimental-feature Embedded` flag (the implicit `-target` matches the host OS):

```bash
$ xcrun swiftc hello.swift -enable-experimental-feature Embedded -wmo
$ ./hello
Hello, embedded world!
```

Note that the resulting executable is still a *dynamically-linked executable*, so it's not fully standalone in the embedded sense. Namely is still uses `putchar` from Libsystem. But the singular object file that was used to build this executable was produced by the compiler in the same fashion that a real embedded build would. If we ask the compiler and linker to minimize the size of the outputs and to remove any unused code, we can observe that the binary has no other dependencies other than `putchar` and that the machine code section is very small (172 bytes in the `__text` section):

```bash
$ xcrun swiftc hello.swift -enable-experimental-feature Embedded -wmo -Osize -Xlinker -dead_strip
$ nm -um ./hello
                 (undefined) external _putchar (from libSystem)
$ size -m ./hello
Segment __TEXT: 16384
  Section __text: 172
...
```

## Strings

Both StaticString and String types are available in Embedded Swift. As is the case in desktop Swift, certain operations on strings require Unicode data tables for strict Unicode compliance. In Embedded Swift. these data tables are provided as a separate static library (libUnicodeDataTables.a) that users need to link in manually – if they need to use these string operations. If the library is required, linking will fail due to missing on one or more of the following symbols:

```
_swift_stdlib_getAge
_swift_stdlib_getBinaryProperties
_swift_stdlib_getCaseMapping
_swift_stdlib_getComposition
_swift_stdlib_getDecompositionEntry
_swift_stdlib_getGeneralCategory
_swift_stdlib_getGraphemeBreakProperty
_swift_stdlib_getMapping
_swift_stdlib_getMphIdx
_swift_stdlib_getNameAlias
_swift_stdlib_getNormData
_swift_stdlib_getNumericType
_swift_stdlib_getNumericValue
_swift_stdlib_getScalarBitArrayIdx
_swift_stdlib_getScalarName
_swift_stdlib_getScript
_swift_stdlib_getScriptExtensions
_swift_stdlib_getSpecialMapping
_swift_stdlib_getWordBreakProperty
_swift_stdlib_isLinkingConsonant
_swift_stdlib_nfd_decompositions
```

To resolve this, link in the libswiftUnicodeDataTables.a that's in Swift toolchain's resource directory (`lib/swift/`) under the target triple that you're using:

```bash
$ swiftc <inputs> -target armv6m-none-none-eabi -enable-experimental-feature Embedded -wmo -c -o output.o
$ ld ... -o binary output.o $(dirname `which swiftc`)/../lib/swift/embedded/armv6m-none-none-eabi/libswiftUnicodeDataTables.a
```

**Unicode data tables are required for (list not exhaustive):**

- Comparing String objects for equality
- Sorting Strings
- Using String's hash values, and in particular using String as dictionary keys
- Using String's .count property
- Using Unicode-aware string processing APIs (.split(), iterating characters, indexing)
- Using Unicode-aware conversion String APIs (.uppercased(), .lowercased(), etc.)

**For contrast, unicode data tables are *not required for* (list not exhaustive):**

- Using StaticString
- Creating, concatenating, string interpolating, and printing String objects
- Using .utf8, .utf16, and .unicodeScalars views of strings, including their .count property, using them as dictionary keys

Manually linking libUnicodeDataTables.a is required for several reasons, including acknowledging that the data tables are desirable: Since they have a non-negligible size, it's useful to be aware that you are using them.

## Conditionalizing compilation for Embedded Swift

It's often useful to have source code be compilable under both regular Swift and Embedded Swift. The following syntax is available for that (but note that as the rest of Embedded Swift, it's experimental, subject to change and not considered source stable):

```swift
func sayHello() {
  #if hasFeature(Embedded)
  print("I'm Embedded Swift")
  #else
  print("I'm regular Swift")
  #endif
}
```

Additionally, you can also use an attribute (also experimental, and not source stable) to make entire functions, types and other declarations unavailable in Embedded Swift. This can be particularly useful to explicitly mark your own code (and also entire types and conformances) that relies on features unavailable in Embedded Swift, e.g. existentials or strings -- it is explicitly allowed to use those in unavailable contexts:

```swift
@_unavailableInEmbedded
func useAnExistential(_: Any) { ... }

@_unavailableInEmbedded
extension MyStruct: CustomStringConvertible {
  var description: String { return "..." }
}
```

## Embedded Swift is a subset of Swift

Embedded Swift is a subset of the Swift language, and some features are not available in Embedded Swift, however features are available, including: Generics, protocols, enums with associated values, tuples, optionals, classes (instances are allocated on the heap and refcounted just like in regular Swift), inheritance, runtime polymorphism, arrays (heap-allocated copy-on-write just like in regular Swift) and many more.

Features that are not available:

- **Not available**: Runtime reflection (`Mirror` APIs).
- **Not available**: Values of protocol types ("existentials"), e.g. `let a: Hashable = ...`, are not allowed. `Any` and `AnyObject` are also not allowed.
- **Not available**: Metatypes, e.g. `let t = SomeClass.Type` or `type(of: value)` are not allowed.
- **Not available**: Printing and stringification of arbitrary types (achieved via reflection in desktop Swift).
- **Not available yet (under development)**: Swift Concurrency.

For a more complete list of supported features in Embedded Swift, see [Embedded Swift -- Status](EmbeddedSwiftStatus.md).

## Libraries and modules in Embedded Swift

Traditional library build and use model of Swift is that library code is compiled into a .swiftmodule, containing the interfaces, and a compiled library with binary code, either a .a static library or a .dylib/.so dynamic library. A client's build then uses the .swiftmodule at compile-time, and the static/dynamic library at link-time.

The library model in Embedded Swift works slightly differently: All Swift source code of a library is promoted into being inlineable and visible to client builds (this is necessary for generic code, and beneficial for optimizations for non-generic code), and ends up serialized into the .swiftmodule, the interface of the library. Therefore, the compiled code of a library is never needed, and doesn't even need to be produced. For example:

```bash
# Build the library, only as a .swiftmomodule. Notice that we never build the .o or .a for the library.
$ swiftc -target <target> -enable-experimental-feature Embedded -wmo \
  a.swift b.swift -module-name MyLibrary -emit-module -emit-module-path ./MyLibrary.swiftmodule

# Build the client, "-I ." add the current directory to the module search path list
$ swiftc -target <target> -enable-experimental-feature Embedded -wmo \
  client.swift -I . -c -o client.o
```

The Embedded Swift standard library is distributed in the toolchain the same way: It's strictly a .swiftmodule without any compiled code present anywhere. All the compiling into machine code is performed as part of the client's build. This has the major benefit that the client's build can provide additional ABI and ISA defining flags, such as the above-mentioned `-mfloat-abi`, `-fshort-enums`, `-mcpu`, `-march` flags, and these flags in the client's build will apply to all the library code (including standard library code) as well.

## Allocating and non-allocating Embedded Swift mode

Embedded Swift does allow instantiating and using reference types (classes) which are refcounted objects allocated on the heap. A common case of needing those is for dynamic containers like arrays and sets (they use dynamically-sized heap-allocated class instances as their storage). There is only a handful of Swift language features that cause allocations:

- creating class instances,
- escaping a closure that captures local variables,
- creating an indirect enum case with a payload referencing the enum itself
- explicitly calling allocation APIs (e.g. `UnsafeMutablePointer.allocate()`).

Outside of those cases, Embedded Swift does not perform allocations or cause heap usage.

Some embedded platforms don't have and/or don't want *any heap allocations whatsoever* and don't provide a heap at all. The `-no-allocations` compiler flag can be used to match that, which will cause the compiler to produce an error at compile time when creating class instances or calling allocation APIs.

```bash
$ cat test.swift
let p = UnsafeMutablePointer<UInt8>.allocate(capacity: 10)
$ swiftc test.swift -enable-experimental-feature Embedded -wmo -no-allocations
test.swift:1:37: error: cannot use allocating operation in -no-allocations mode
```

## External dependencies

Embedded Swift minimizes external dependencies (i.e. functions that need to be available at link-time), but they still exist. There are generally two categories of dependencies: (1) functions that the Swift standard library or Embedded Swift runtime need to call, and (2) functions/symbols that are implicitly added by LLVM and the compiler pipeline.

For (1), external dependencies are only used based on actual usage of the program under compilation:

- instantiating a class, or using UnsafeMutablePointer.allocate()
  - dependency: `int posix_memalign(void **, size_t, size_t);`
  - dependency: `void free(void *);`
- using print()
  - dependency: `int putchar(int);`
- using Hashable, Set, Dictionary, or random-number generating APIs
  - dependency: `void arc4random_buf(void *, size_t);`

For (2), external dependencies are also triggered by specific code needing them, but they are somewhat lower-level patterns where it might not be obvious that such patterns should cause external dependencies:

- **basic memory copying and zeroing functions**
  - usage added for a variety of reasons (e.g. using structs on the stack)
  - dependency: `void *memset(void *, int, size_t);`
  - dependency: `void *memcpy(void *, const void *, size_t);`
- **stack protectors** (aka stack cookies or stack canaries)
  - dependency: `void *__stack_chk_guard;`
  - dependency: `void __stack_chk_fail(void);`
  - stack protectors can be disabled with `-disable-stack-protector` swiftc flag
- **atomics instrinsics**
  - on CPU architectures that don't have direct load-acquire/store-release support in the ISA, LLVM calls helper functions for atomic operations
  - needed by refcounting in the Embedded Swift runtime (so any class usage will trigger this dependency)
  - also needed when using atomics from the Synchronization module
- **multiplication/division/modulo instrinsics**
  - on CPU architectures that don't have direct support for the math operations in the ISA
  - dependency (on Mach-O): `__divti3`
  - dependency (on Mach-O): `__modti3`
  - dependency (with EABI): `__aeabi_ldivmod`

The user and/or the platform (via basic libraries like libc or compiler builtins) is expected to provide these well-known APIs.
