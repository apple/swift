# Swift implemented in Swift

_Libswift_ is the part of the Swift compiler, which is implemented in Swift.

With _libswift_ it is possible to add SIL optimization passes written in Swift. It allows to gradually migrate the SIL optimizer from C++ to Swift.

## Building

_Libswift_ is a static library and it is built as part of the swift compiler using build-script and CMake.

To enable _libswift_, add the build-script option `--libswift`.

In this early stage of development _libswift_ is disabled by default. Right now _libswift_ does not contain any optimizations or features which are not available in the existing optimizer. Therefore a compiler with disabled _libswift_ still behaves as a compiler with enabled _libswift_. This will change soon.

When _libswift_ is enabled, it is built with a Swift toolchain (5.3 or newer), which must be installed on the host system. The `swiftc` compiler driver is expected to be in the command search path.

Currently the `swift-frontend` and `sil-opt` tools use _libswift_. Tools, which don't use any optimization passes from _libswift_ don't need to link _libswift_. For example, all tools, which compile Swift source code, but don't optimize it, like SourceKit or lldb, don't need to link _libswift_. As long as `initializeLibSwift()` is not called there is no dependency on _libswift_.

This also means that currently it is not possible to implement mandatory passes in _libswift_, because this would break tools which compile Swift code but don't use _libswift_. When we want to implement mandatory passes in _libswift_ in the future, we'll need to link _libswift_ to all those tools.

## SIL

The design of SIL in _libswift_ matches very closely the design on the C++ side. For example, there are functions, basic blocks, instructions, SIL values, etc.

Though, there are some small deviations from the C++ SIL design. Either due to the nature of the Swift language (e.g. the SIL `Value` is a protocol, not a class), or improvements, which could be done in C++ as well.

Bridging SIL between C++ and Swift is toll-free, i.e. does not involve any "conversion" between C++ and Swift SIL.

### The bridging layer

The bridging layer is a small interface layer which enables calling into the SIL C++ API from the Swift side. Currently the bridging layer is implemented in C using C interop. In future it can be replaced by a C++ implementation by using C++ interop, which will further simplify the bridging layer or make it completely obsolete. But this is an implementation detail and does not affect the API of SIL in _libswift_.

The bridging layer consists of the C header file `SILBridging.h` and its implementation file `SILBridging.cpp`. The header file contains all the bridging functions and some C data structures like `BridgedStringRef` (once we use C++ interop, those C data structures are not required anymore and can be removed).

### SIL C++ objects in Swift

The core SIL C++ classes have corresponding classes in _libswift_, for example `Function`, `BasicBlock` or all the instruction classes.

This makes _libswift_ easy to use and it allows to program in a "Swifty" style. For example one can write

```
  for inst in block.instructions {
    if let cfi = inst as? CondFailInst {
      // ...
    }
  }
```

Bridging SIL classes is implemented by including a two word Swift object header (`SwiftObjectHeader`) in the C++ definition of a class, like in `SILFunction`, `SILBasicBlock` or `SILNode`. This enables to use SIL objects on both, the C++ and the Swift, side.

The Swift class metatypes are "registered" by `registerClass()`, called from `initializeLibSwift()`. On the C++ side, they are stored in static global variables (see `registerBridgedClass()`) and then used to initialize the object headers in the class constructors.

The reference counts in the object header are initialized to "immortal", which let's all ARC operations on SIL objects be no-ops.

The Swift classes don't define any stored properties, because those would overlap data fields in the C++ classes. Instead, data fields are accessed via computed properties, which call bridging functions to retrieve the actual data values.

### Lazy implementation

In the current state the SIL functionality and API is not completely implemented, yet. For example, not all instruction classes have a corresponding class in _libswift_. Whenever a new _libswift_ optimization needs a specific SIL feature, like an instruction, a Builder-function or an accessor to a data field, it's easy to add the missing parts.

For example, to add a new instruction class:

*  replace the macro which defines the instruction in `SILNodes.def` with a `BRIDGED-*` macro
*  add the instruction class in `Instruction.swift`
*  register the class in `registerSILClasses()`
*  if needed, add bridging functions to access the instruction's data fields.


No yet implemented instruction classes are mapped to a "placeholder" instruction, e.g `UnimplementedInstruction`. This ensures that optimizations can process any kind of SIL, even if some instructions don't have a representation in _libswift_ yet.

## The Optimizer

Similar to SIL, the optimizer also uses a small bridging layer (`OptimizerBriding.h`).
Passes are registered in `registerSwiftPasses()`, called from `initializeLibSwift()`.
The C++ PassManager can then call a _libwift_ pass like any other `SILFunctionTransform` pass.

To add a new function pass:

* add a `SWIFT_FUNCTION_PASS` entry in `Passes.def`
* create a new Swift file in `libswift/Sources/Optimizer/FunctionPasses`
* add a `FunctionPass` global
* register the pass in `registerSwiftPasses()`

All SIL modifications, which a pass can do, are going through the `FunctionPassContext` - the second parameter of its run-function. In other words, the context is the central place to make modifications. This enables automatic change notifications to the pass manager. Also, it makes it easier to build a concurrent pass manager in future.

### Instruction Passes

In addition to function passes, _libswift_ provides the infrastructure for instruction passes. Instruction passes are invoked from SILCombine (in the C++ SILOptimizer) and correspond to a visit-function in SILCombine.

With instruction passes it's possible to implement small peephole optimizations for certain instruction classes.

To add a new instruction pass:

* add a `SWIFT_INSTRUCTION_PASS` entry in `Passes.def`
* create a new Swift file in `libswift/Sources/Optimizer/InstructionPasses`
* add an `InstructionPass` global
* register the pass in `registerSwiftPasses()`
* if this passes replaces an existing `SILCombiner` visit function, remove the old visit function

## Performance

The performance of _libswift_ is very important, because compile time is critical.
Some performance considerations:

* Memory is managed on the C++ side. On the Swift side, SIL objects are treated as "immortal" objects, which avoids (most of) ARC overhead. ARC runtime functions are still being called, but no atomic reference counting operations are done. In future we could add a compiler feature to mark classes as immortal to avoid the runtime calls at all.

* Minimizing memory allocations: _libswift_ provides data structures which are malloc-free. For example `StackList` can be used in optimizations to implement work lists without any memory allocations. (Not yet done: `BasicBlockSet`, `BasicBlockData`)

But most importantly, if there are performance issues with the current compiler, the design of _libswift_ should make it possible to fix performance deficiencies with future compiler improvements.
