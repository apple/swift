# Swift Compiler Documentation

@Metadata {
  @TechnologyRoot
}

This page describes the overall organization of documentation for the Swift toolchain.
It is divided into the following sections:

- [Tutorials](#Tutorials)
  gently guide you towards achieving introductory tasks,
  while assuming minimal background knowledge.
- [How-To Guides](#How-To-Guides)
  help you complete specific tasks in a step-by-step fashion.
- [Explanations](#Explanations)
  discuss key subsystems and concepts, at a high level.
  They also provide background information and talk about design tradeoffs.
- [Reference Guides](#Reference-Guides)
  contain a thorough technical reference for complex topics.
  They assume some overall understanding of surrounding subsystems.
- [Recommended Practices](#Recommended-Practices)
  suggests guidelines for writing code and diagnostics.
- [Project Information](#Project-Information)
  tracks continuous integration (CI), branching and release history.
- [Evolution Documents](#Evolution-Documents)
  includes proposals and manifestos for changes to Swift.
- The [External Resources](#External-Resources) section provides links to
  valuable discussions about Swift development, in the form of talks
  and blog posts.
- The [Uncategorized](#Uncategorized) section is for documentation which does
  not fit neatly into any of the above categories. We would like to minimize
  items in this section; avoid adding new documentation here.

Sometimes documentation is not enough.
Especially if you are a new contributor, you might run into roadblocks
which are not addressed by the existing documentation.
Or they are addressed somewhere but you cannot find the relevant bits.
If you are stuck, please use the [development category][] on the Swift forums
to ask for help!

Lastly, note that we are slowly moving towards a more structured form of
documentation, inspired by the Django project [[1][Django-docs-1]]
[[2][Django-docs-2]]. Hence parts of this page are aspirational
and do not reflect how much of the existing documentation is written.
Pull requests to clean up the [Uncategorized](#Uncategorized) section,
or generally fill gaps in the documentation are very welcome.
If you would like to make major changes, such as adding entire new pieces of
documentation, please create a thread on the Swift forums under the
[development category][] to discuss your proposed changes.

[development category]: https://forums.swift.org/c/development
[Django-docs-1]: https://docs.djangoproject.com/
[Django-docs-2]: https://documentation.divio.com/#the-documentation-system

## Tutorials

- <doc:libFuzzerIntegration>:
  Using `libFuzzer` to fuzz Swift code.

## How-To Guides

- <doc:FAQ>:
  Answers "How do I do X?" for a variety of common tasks.
- <doc:FirstPullRequest>:
  Describes how to submit your first pull request. This is the place to start
  if you're new to the project!
- <doc:GettingStarted>:
  Describes how to set up a working Swift development environment
  for Linux and macOS, and get an edit-build-test-debug loop going.
- <doc:DebuggingTheCompiler>:
  Describes a variety of techniques for debugging.
- Building for Android:
  - <doc:Android>:
    How to run some simple programs and the Swift test suite on an Android device.
  - <doc:AndroidBuild>:
    How to build the Swift SDK for Android on Windows.
- Building for Windows:
  - <doc:Windows>:
    Overview on how to build Swift for Windows.
  - <doc:WindowsBuild>:
    How to build Swift on Windows using Visual Studio.
  - <doc:WindowsCrossCompile>:
    How to cross compile Swift for Windows on a non-Windows host OS.
- Building for OpenBSD:
  - <doc:OpenBSD>:
    Overview of specific steps for building on OpenBSD.
- <doc:RunningIncludeWhatYouUse>:
  Describes how to run [include-what-you-use](https://include-what-you-use.org)
  on the Swift project.

## Explanations

- <doc:WebAssembly>:
  Explains some decisions that were made while implementing the WebAssembly target.

### Compiler and Runtime Subsystems

- Driver:
  - <doc:Driver>:
    Provides an overview of the driver, compilation model, and the compiler's
    command-line options. Useful for integration into build systems other than
    SwiftPM or Xcode.
  - <doc:DriverInternals>:
    Provides a bird's eye view of the driver's implementation.
    <!-- NOTE: Outdated -->
- <doc:DependencyAnalysis>:
  Describes different kinds of dependencies across files in the same module,
  important for understanding incremental builds.
- <doc:DifferentiableProgrammingImplementation>:
  Describes how automatic differentiation is implemented in the Swift compiler.
- C and ObjC interoperability: Clang Importer and PrintAsClang
  - <doc:CToSwiftNameTranslation>:
    Describes how C and ObjC entities are imported into Swift
    by the Clang Importer.
  - <doc:CToSwiftNameTranslation-OmitNeedlessWords>:
    Describes how the "Omit Needless Words" algorithm works,
    making imported names more idiomatic.
- Type-checking and inference:
  - <doc:TypeChecker>:
    Provides an overview of how type-checking and inference work.
  - <doc:RequestEvaluator>:
    Describes the request evaluator architecture, which is used for
    lazy type-checking and efficient caching.
  - <doc:Literals>:
    Describes type-checking and inference specifically for literals.
- <doc:Serialization>:
  Gives an overview of the LLVM bitcode format used for swiftmodules.
  - <doc:StableBitcode>:
    Describes how to maintain compatibility when changing the serialization
    format.
- SIL and SIL Optimizations:
  - <doc:SILFunctionConventions>:
  - <doc:SILMemoryAccess>:
  - <doc:SILProgrammersManual>:
    Provides an overview of the implementation of SIL in the compiler.
  - <doc:OptimizerDesign>:
    Describes the design of the optimizer pipeline.
  - [HighLevelSILOptimizations.rst](/docs/HighLevelSILOptimizations.rst):
    Describes how the optimizer understands the semantics of high-level
    operations on [currency](/docs/Lexicon.md#currency-type) data types and 
    optimizes accordingly.
    Includes a thorough discussion of the `@_semantics` attribute.
- Runtime specifics:
  - [Backtracing.rst](/docs/Backtracing.rst):
    Describes Swift's backtracing and crash catching support.

### SourceKit subsystems

- <doc:SwiftLocalRefactoring>:
  Describes how refactorings work and how they can be tested.

### Language subsystems

- Swift's Object Model
  - <doc:LogicalObjects>:
    Describes the differences between logical and physical objects and
    introduces materialization and writeback.
  - [MutationModel.rst](/docs/MutationModel.rst): Outdated.
    <!-- NOTE: Outdated -->
- <doc:DocumentationComments>:
  Describes the format of Swift's documentation markup, including
  specially-recognized sections.

### Stdlib Design

- [SequencesAndCollections.rst](/docs/SequencesAndCollections.rst):
  Provides background on the design of different collection-related protocols.
- [StdlibRationales.rst](/docs/StdlibRationales.rst):
  Provides rationale for common questions/complaints regarding design decisions
  in the Swift stdlib.

## Reference Guides

- <doc:DriverParseableOutput>:
  Describes the output format of the driver's `-parseable-output` flag,
  which is suitable for consumption by editors and IDEs.
- <doc:ObjCInterop>
  Documents how Swift interoperates with ObjC code and the ObjC runtime.
- [LibraryEvolution.rst](/docs/LibraryEvolution.rst):
  Specifies what changes can be made without breaking binary compatibility.
- [SIL.rst](/docs/SIL.rst):
  Documents the Swift Intermediate Language (SIL).
  - <doc:TransparentAttr>:
    Documents the semantics of the `@_transparent` attribute.
- <doc:DynamicCasting>:
  Behavior of the dynamic casting operators `is`, `as?`, and `as!`.
- <doc:Runtime>:
  Describes the ABI interface to the Swift runtime.
  <!-- NOTE: Outdated -->
- <doc:Lexicon>:
  Canonical reference for terminology used throughout the project.
- <doc:UnderscoredAttributes>:
  Documents semantics for underscored (unstable) attributes.

### ABI

- [CallConvSummary.rst](/docs/ABI/CallConvSummary.rst):
	A concise summary of the calling conventions used for C/C++, Objective-C
	and Swift on Apple platforms.  Contains references to source documents,
	where further detail is required.
- [CallingConvention.rst](/docs/ABI/CallingConvention.rst):
  Describes in detail the Swift calling convention.
- <doc:GenericSignature>:
  Describes what generic signatures are and how they are used in the ABI,
  including the algorithms for minimization and canonicalization.
- <doc:KeyPaths>:
  Describes the layout of key path objects (instantiated by the runtime,
  and therefore not strictly ABI). \
  **TODO:** The layout of key path patterns (emitted by the compiler,
  to represent key path literals) isn't documented yet.
- [Mangling.rst](/docs/ABI/Mangling.rst):
  Describes the stable mangling scheme, which produces unique symbols for
  ABI-public declarations.
- [TypeLayout.rst](/docs/ABI/TypeLayout.rst):
  Describes the algorithms/strategies for fragile struct and tuple layout;
  class layout; fragile enum layout; and existential container layout.
- [TypeMetadata.rst](/docs/ABI/TypeMetadata.rst):
  Describes the fields, values, and layout of metadata records, which can be
  used (by reflection and debugger tools) to discover information about types.

## Recommended Practices

### Coding

- <doc:AccessControlInStdlib>:
  Describes the policy for access control modifiers and related naming
  conventions in the stdlib.
  <!-- NOTE: Outdated -->
- <doc:IndexInvalidation>:
  Describes the expected behavior of indexing APIs exposed by the stdlib.
- [StdlibAPIGuidelines.rst](/docs/StdlibAPIGuidelines.rst):
  Provides guidelines for designing stdlib APIs.
  <!-- NOTE: Outdated -->
- <doc:StandardLibraryProgrammersManual>:
  Provides guidelines for working code in the stdlib.
- [OptimizationTips.rst](/docs/OptimizationTips.rst):
  Provides guidelines for writing high-performance Swift code.

### Diagnostics

## Project Information

- <doc:Branches>:
  Describes how different branches are setup and what the automerger does.
- <doc:ContinuousIntegration>:
  Describes the continuous integration setup, including the `@swift_ci` bot.

## Evolution Documents

### Manifestos

- ABI Stability and Library Evolution
  - <doc:ABIStabilityManifesto>:
    Describes the goals and design for ABI stability.
  - <doc:LibraryEvolutionManifesto>:
    Describes the goals and design for Library Evolution.
- <doc:BuildManifesto>:
  Provides an outline for modularizing the build system for the Swift toolchain.
- <doc:CppInteroperabilityManifesto>:
  Describes the motivation and design for first-class Swift-C++ interoperability.
- <doc:DifferentiableProgramming>:
  Outlines a vision and design for first-class differentiable programming in Swift.
- <doc:GenericsManifesto>:
  Communicates a vision for making the generics system in Swift more powerful.
- <doc:OwnershipManifesto>:
  Provides a framework for understanding ownership in Swift,
  and highlights potential future directions for the language.
- <doc:StringManifesto>:
  Provides a long-term vision for the `String` type.

### Proposals

Old proposals are present in the [/docs/proposals](/docs/proposals) directory.
More recent proposals are located in the [apple/swift-evolution][] repository.
You can see the status of different proposals at
<https://apple.github.io/swift-evolution/>.

[apple/swift-evolution]: https://github.com/apple/swift-evolution

### Surveys

- [CallingConvention.rst](/docs/ABI/CallingConvention.rst):
  This whitepaper discusses the Swift calling convention (high-level semantics;
  ownership transfer; physical representation; function signature lowering).
- <doc:ErrorHandlingRationale>:
  Surveys error-handling in a variety of languages, and describes the rationale
  behind the design of error handling in Swift.
- <doc:WeakReferences>:
  Discusses weak references, including the designs in different languages,
  and proposes changes to Swift (pre-1.0).
  <!-- NOTE: Outdated -->

### Archive

These documents are known to be out-of-date and are superseded by other
documentation, primarily [The Swift Programming Language][] (TSPL).
They are preserved mostly for historical interest.

- <doc:AccessControl>
- <doc:Arrays>
  <!-- Has additional notes on bridging that may be of general interest? -->
- [Generics.rst](/docs/archive/Generics.rst)
- <doc:ErrorHandling>
- [StringDesign.rst](/docs/StringDesign.rst)
- [TextFormatting.rst](/docs/TextFormatting.rst)

[The Swift Programming Language]: https://docs.swift.org/swift-book

## External Resources

External resources are listed in <doc:ExternalResources>.
These cover a variety of topics,
such as the design of different aspects of the Swift compiler and runtime
and contributing to the project more effectively.

## Uncategorized

### Needs refactoring

The documents in this section might be worth breaking up into several documents,
and linking one document from the other. Breaking up into components will
provide greater clarity to contributors wanting to add new documentation.

- <doc:ARCOptimization>:
  Covers how ARC optimization works, with several examples.
  TODO: Not clear if this is intended to be an explanation or a reference guide.
- <doc:CompilerPerformance>:
  Thoroughly discusses different ways of measuring compiler performance
  and common pitfalls.
  TODO: Consider breaking up into one high-level explanation explaining key
  concepts and individual how-to guides that can be expanded independently.
  Also, it's not the right place to explain details of different compiler modes.
- <doc:DevelopmentTips>:
  Contains an assortment of tips for better productivity when working on the
  compiler.
  TODO: Might be worthwhile to make a dedicated how-to guide or explanation.
  It might also be valuable to introduce the tips in context, and have the
  explanation link to all the different tips.
- <doc:Diagnostics>:
  Describes how to write diagnostic messages and associated educational notes.
  TODO: Consider splitting into how-tos and recommended practices.
  For example, we could have a how-to guide on adding a new diagnostic,
  and have a recommended practices page which explains the writing style
  for diagnostics and educational notes.
- <doc:HowSwiftImportsCAPIs>:
  Contains a thorough description of the mapping between C/ObjC entities and
  Swift entities.
  TODO: Not clear if this is intended to be language documentation
  (for Swift developers), an explanation or a reference guide.
- <doc:Modules>: was written for Swift pre-1.0, but is still
  relevant and covers behavior that's underspecified in either TSPL or the
  language reference.
- <doc:OptimizerCountersAnalysis>:
  TODO: Consider breaking up into a how-to guide
  on dumping and analyzing the counters
  and an explanation for the counter collection system.
- <doc:Testing>:
  TODO: Consider splitting into a how-to guide on writing a new test case
  and an explanation for how the compiler is tested.
- <doc:SwiftIndent>:
  TODO: Unclear if this is intended to be an explanation or a reference guide.
- <doc:Random>: Stub.

### Archive

- <doc:FailableInitializers>:
  Superseded by documentation in [The Swift Programming Language][].
- [InitializerProblems.rst](/docs/InitializerProblems.rst):
  Describes some complexities around initialization in Swift.
  TODO: It would be great to have an explanation, say `InitializationModel.md`,
  that covers the initialization model and new attributes like
  `@_hasMissingDesignatedInitializers`. Some of this is covered in
  [TSPL's initialization section][] but that doesn't include newly added
  attributes.
- <doc:Swift3Compatibility>:
  Discusses the Swift 3 -> Swift 4 migration.
- [StoredAndComputedVariables.rst](/docs/StoredAndComputedVariables.rst): for Swift pre-1.0.

[TSPL's initialization section]: https://docs.swift.org/swift-book/LanguageGuide/Initialization.html
