
# Cross-Compilation Model

## Components

When compiling Swift code, the compiler will consult three different sources of
inputs outside of the user provided code.

1. Compiler Resources
2. System (C/C++) Headers (Platform SDK)
3. Platform Swift Modules (Swift SDK)

These pieces compose in a particular manner to build a system image to build
code against.

The compiler resources are content shipped with the toolchain and are tied to
that specific version and build of the compiler. In the case of the Swift
compiler, this includes the Swift shims. Whilst this is a compiler resource, the
packaging may not necessarily be part of the toolchain due to interdependencies.
Some of this content is required to process the system headers themselves (e.g.
clang's builtin resource headers).

The C/C++ system headers (and libraries) are what is traditionally called the
Unix "sysroot". On most Unix systems, this is the set of headers that are
associated with the system library set which normally may be found in
`/usr/include`. On Darwin, this is included in the unified SDK which is shipped
to the user, while on Windows, this is called the Windows SDK, which is a
separate installable component. For simplicity, we will refer to this as the
Platform SDK universally.

The Swift SDK contains the libraries and overlays that provide the core
language runtime and expose system libraries to Swift code in an ergonomic
manner. This may be in the form of API Notes, module maps, wrapper types, or
Swift `extension`s, or entire libraries. This code may or may not be fully
inlined into the client code and thus be part of the platform ABI. In some
distributions, the Swift SDK and Platform SDK are combined where the system C,
C++, and Swift libraries are shipped together, such as the SDKs for the Apple
platforms.

## Flags

The compiler resources are controlled via the driver flag `-resource-dir`. This
allows the driver to select the correct location in most cases while allowing
the developer control to override the value if required. Normally, you should
not need to set this flag as the location of these files is intrinsic to the
compiler.

The Platform SDK contains C/C++ content which is actually consumed through the
clang importer rather than the Swift compiler. The Swift toolchain uses clang as
the C/C++ compiler on all platforms as it is embedded to generate inline foreign
function interface (FFI) to enable seamless C/C++ bridging to Swift. The flag
used by clang is derived from the GCC toolchain, and is spelt `--sysroot`. The
compiler driver is responsible for identifying the structure of the sysroot.
When cross-compiling, there isn't a consistent location for these files, so the
driver must expose an argument to specify where to find these files.

On Darwin platforms, the Platform SDK and Swift SDK are shipped combined into a
single SDK. As a result the singular `-sdk` flag allows control over the
Platform SDK and Swift SDK. Windows uses a split model as the Windows SDK is
split into multiple components and can be controlled individually (i.e. UCRT
version, SDK version, VCRuntime version). The `-sdk` flag is used to specify the
location of the Swift SDK which is merged with the Platform SDK. By default, the
environment variable `SDKROOT` is used to seed the value of `-sdk`, though the
user may specify the value explicitly. Other platforms do not currently have a
flag to control this location and the toolchain defaults to a set of relative
paths to locate the content. This prevents cross-compilation as the included
content would be for a single platform.

> [!NOTE]
> `-resource-dir` historically influenced the driver's search for the SDK content, locating the standard library relative to the resource directory. This behaviour is considered deprecated but remains for compatibility. The `-sdk` parameter is given precedence and is the preferred mechanism for controlling the behaviour of the driver to locate the SDK content.

## Solution

Generalising the above structure and sharing the common sharing gives way to the
following set of flags for cross-compilation:

1. `-target`: specifies the triple for the host
2. `-sysroot`: specifies the Platform SDK for the host platform content
3. `-sdk`: specifies the Swift SDK for the host

The values for these may be defaulted by the driver on a per-platform basis.

The `-sysroot` flag identifies the location for the C/C++ headers and libraries
required for compilation. This is primarily used by non-Darwin, non-Windows
hosts as Darwin has its own SDK concept that allows for co-installation and
Windows uses a different model which merges multiple locations in memory.

The `-sdk` flag identifies the location of the Swift SDK, which provides the
necessary content for Swift compilation (including binary swiftmodules). This
includes the standard library and the core libraries (dispatch, Foundation, and
possibly XCTest - Windows isolates XCTest from the rest of the SDK). The Swift
shims are also provided by this location as they are a dependency for properly
processing the Swift core library.

## Compatibility

In order to retain compatibility with older toolchain releases which did not
include support for the `-sysroot` flag, the driver shall default the value to
the value provided to `-sdk`. This allows us to transition between the existing
toolchains which expected a single root containing all the necessary components.
This allows the driver to make the most appropriate choice for the host that is
being compiled for without loss of generality. A platform may opt to ignore one
or more of these flags (e.g. Windows does not use `-sysroot` as the system
headers are not organised like the traditional unix layout).
