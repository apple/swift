# How to Set Up an Edit-Build-Test-Debug Loop

This document describes how to set up a development loop for people interested
in contributing to Swift.

If you are only interested in building the
toolchain as a one-off, there are a couple of differences:
1. You can ignore the parts related to Sccache.
2. You can stop reading after
   [Building the project for the first time](#building-the-project-for-the-first-time).

## Table of Contents

- [System Requirements](#system-requirements)
- [Cloning the project](#cloning-the-project)
  - [Troubleshooting cloning issues](#troubleshooting-cloning-issues)
- [Installing dependencies](#installing-dependencies)
  - [macOS](#macos)
  - [Linux](#linux)
- [Building the project for the first time](#building-the-project-for-the-first-time)
  - [Spot check dependencies](#spot-check-dependencies)
  - [The roles of different tools](#the-roles-of-different-tools)
  - [The actual build](#the-actual-build)
  - [Troubleshooting build issues](#troubleshooting-build-issues)
- [Editing code](#editing-code)
  - [Setting up your fork](#setting-up-your-fork)
  - [Using Ninja with Xcode](#using-ninja-with-xcode)
  - [Other IDEs setup](#other-ides-setup)
  - [Editing](#editing)
  - [Incremental builds with Ninja](#incremental-builds-with-ninja)
  - [Spot checking an incremental build](#spot-checking-an-incremental-build)
- [Reproducing an issue](#reproducing-an-issue)
- [Running tests](#running-tests)
- [Debugging issues](#debugging-issues)
  - [Print debugging](#print-debugging)
  - [Debugging using LLDB](#debugging-using-lldb)
- [Next steps](#next-steps)

## System Requirements

1. Operating system:
   The supported operating systems for developing the Swift toolchain are:
   macOS, Ubuntu Linux LTS, and the latest Ubuntu Linux release.
   At the moment, Windows is not supported as a host development operating
   system. Experimental instructions for Windows are available under
   [Windows.md](/docs/Windows.md).
2. Python 3: Several utility scripts are written in Python.
3. Git 2.x to check out the sources. We find that older versions of Git
   can't successfully check out all of the required repositories or
   fail during a rebase when switching between checkout schemes.
4. Disk space:
   Make sure that you have enough available disk space before starting.
   The source code, including full git history, requires about 3.5 GB.
   Build artifacts take anywhere between 5 GB to 70 GB, depending on the
   build settings.
5. Time:
   Depending on your machine and build settings,
   a from-scratch build can take a few minutes to several hours,
   so you might want to grab a beverage while you follow the instructions.
   Incremental builds are much faster.

## Cloning the project

1. Create a directory for the whole project:
   ```sh
   mkdir swift-project
   cd swift-project
   ```
   
    > **Warning**  
    > Make sure the absolute path to your `swift-project` directory **does not** contain spaces, 
        since that might cause issues during the build step.
    
2. Clone the sources:
   - Via SSH (recommended):
     If you plan on contributing regularly, cloning over SSH provides a better
     experience. After you've [uploaded your SSH keys to GitHub][]:
     ```sh
     git clone git@github.com:apple/swift.git swift
     cd swift
     utils/update-checkout --clone-with-ssh
     ```
   - Via HTTPS:
     If you want to check out the sources as read-only,
     or are not familiar with setting up SSH,
     you can use HTTPS instead:
     ```sh
     git clone https://github.com/apple/swift.git swift
     cd swift
     utils/update-checkout --clone
     ```
   > **Note**  
   > If you've already forked the project on GitHub at this stage, **do not
   > clone your fork** to start off. We describe [how to setup your fork](#setting-up-your-fork)
   > in a subsection below.
   <!-- Recommending against cloning the fork due to https://github.com/apple/swift/issues/55918 and https://github.com/apple/swift/issues/55947. -->
3. Double-check that `swift`'s sibling directories are present.
   ```sh
   ls ..
   ```
   This should list directories like `llvm-project`, `swiftpm` and so on.
4. Checkout the right branch/tag:
   If you are building the toolchain for local development, you can skip this
   step, as Step 2 will checkout `swift`'s `main` branch and matching
   branches for other projects.
   If you are building the toolchain as a one-off, it is more likely that you
   want a specific branch or a tag, often corresponding to a specific release
   or a specific snapshot. You can update the branch/tag for all repositories
   as follows:
   ```sh
   utils/update-checkout --scheme mybranchname
   # OR
   utils/update-checkout --tag mytagname
   ```
   Detailed branching information, including names for release branches, can
   be found in [Branches.md](/docs/Branches.md).

> **Note**  
> The commands used in the rest of this guide assumes that the absolute path
> to your working directory is something like `/path/to/swift-project/swift`.
> Double-check that running `pwd` prints a path ending with `swift`.

[uploaded your SSH keys to GitHub]: https://help.github.com/articles/adding-a-new-ssh-key-to-your-github-account/

### Troubleshooting cloning issues

- If `update-checkout` failed, double-check that the absolute path to your
  working directory does not have non-ASCII characters.
- Before running `update-checkout`, double-check that `swift` is the only
  repository inside the `swift-project` directory. Otherwise,
  `update-checkout` may not clone the necessary dependencies.

## Installing dependencies

### macOS

1. Install [Xcode 13 beta 4][Xcode] or newer:
   The required version of Xcode changes frequently and is often a beta release.
   Check this document or the host information on <https://ci.swift.org> for the
   current required version.
2. Install [CMake][], [Ninja][] and [Sccache][]:
   - Via [Homebrew][] (recommended):
     ```sh
     brew install cmake ninja sccache
     ```
   - Via [Homebrew Bundle][]:
     ```sh
     brew bundle
     ```

[Xcode]: https://developer.apple.com/xcode/resources/
[CMake]: https://cmake.org
[Ninja]: https://ninja-build.org
[Homebrew]: https://brew.sh/
[Homebrew Bundle]: https://github.com/Homebrew/homebrew-bundle

### Linux

1. The latest Linux dependencies are listed in the respective Dockerfiles:
   * [Ubuntu 18.04](https://github.com/apple/swift-docker/blob/main/swift-ci/master/ubuntu/18.04/Dockerfile)
   * [Ubuntu 20.04](https://github.com/apple/swift-docker/blob/main/swift-ci/master/ubuntu/20.04/Dockerfile)
   * [Ubuntu 22.04](https://github.com/apple/swift-docker/blob/main/swift-ci/master/ubuntu/22.04/Dockerfile)
   * [CentOS 7](https://github.com/apple/swift-docker/blob/main/swift-ci/master/centos/7/Dockerfile)
   * [Amazon Linux 2](https://github.com/apple/swift-docker/blob/main/swift-ci/master/amazon-linux/2/Dockerfile)

2. To install [Sccache][] (optional):
   * If you're not building within a Docker container:
     ```sh
     sudo snap install sccache --candidate --classic
     ```
   * If you're building within a Docker container, you'll have to install
     `sccache` manually, since [`snap` is not available in environments
     without `systemd`](https://unix.stackexchange.com/questions/541230/do-snaps-require-systemd):

     ```sh
     SCCACHE_VERSION=v0.3.0
     curl -L "https://github.com/mozilla/sccache/releases/download/${SCCACHE_VERSION}/sccache-${SCCACHE_VERSION}-$(uname -m)-unknown-linux-musl.tar.gz" -o sccache.tar.gz
     tar xzpvf sccache.tar.gz
     sudo cp "sccache-${SCCACHE_VERSION}-$(uname -m)-unknown-linux-musl/sccache" /usr/local/bin
     sudo chmod +x /usr/local/bin/sccache
     ```

> **Note**  
> LLDB currently requires at least `swig-1.3.40` but will successfully build
> with version 2 shipped with Ubuntu.

[Sccache]: https://github.com/mozilla/sccache

## Building the project for the first time

### Spot check dependencies

* Run `cmake --version`; this should be at least 3.19.6 (3.24.2 if you want to use Xcode for editing on macOS).
* Run `python3 --version`; check that this succeeds.
* Run `ninja --version`; check that this succeeds.
* If you installed and want to use Sccache: Run `sccache --version`; check
  that this succeeds.

### The roles of different tools

At this point, it is worthwhile to pause for a moment
to understand what the different tools do:

1. On macOS and Windows, IDEs (Xcode and Visual Studio resp.) serve as an
   easy way to install development dependencies such as a C++ compiler,
   a linker, header files, etc. The IDE's build system need not be used to
   build Swift. On Linux, these dependencies are installed by the
   distribution's package manager.
2. CMake is a cross-platform build system for C and C++.
   It forms the core infrastructure used to _configure_ builds of
   Swift and its companion projects.
3. Ninja is a low-level build system that can be used to _build_ the project,
   as an alternative to Xcode's build system. Ninja is somewhat faster,
   especially for incremental builds, and supports more build environments.
4. Sccache is a caching tool:
   If you ever delete your build directory and rebuild from scratch
   (i.e. do a "clean build"), Sccache can accelerate the new build
   significantly. There are few things more satisfying than seeing Sccache
   cut through build times.

   > **Note**
   > Sccache defaults to a cache size of 10GB, which is relatively small
   > compared to build artifacts. You can bump it up, say, by setting
   > `export SCCACHE_CACHE_SIZE="50G"` in your dotfile(s).
5. `utils/update-checkout` is a script to help you work with all the individual
   git repositories together, instead of manually cloning/updating each one.
6. `utils/build-script` (we will introduce this shortly)
   is a high-level automation script that handles configuration (via CMake),
   building (via Ninja), caching (via Sccache), running tests and more.

> **Pro Tip**: Most tools support `--help` flags describing the options they
> support. Additionally, both Clang and the Swift compiler have hidden flags
> (`clang --help-hidden`/`swiftc --help-hidden`) and frontend flags
> (`clang -cc1 --help`/`swiftc -frontend --help`) and the Swift compiler
> even has hidden frontend flags (`swiftc -frontend --help-hidden`). Sneaky!

Phew, that's a lot to digest! Now let's proceed to the actual build itself!

### The actual build

1. Build the toolchain with optimizations, debuginfo, and assertions, using
   Ninja.
   - macOS:
     ```sh
     utils/build-script --skip-build-benchmarks \
       --skip-ios --skip-watchos --skip-tvos --swift-darwin-supported-archs "$(uname -m)" \
       --sccache --release-debuginfo --swift-disable-dead-stripping
     ```
   - Linux:
     ```sh
     utils/build-script --release-debuginfo --skip-early-swift-driver \
       --skip-early-swiftsyntax
     ```
     If you installed and want to use Sccache, include the `--sccache` option in
     the invocation as well.
   <!-- FIXME: Without this "hard" line break, the note doesn’t get properly spaced from the bullet -->
   <br />

   > **Note**  
   > If you are planning to work on the compiler, but not the parts that are
   > written in Swift, pass `--bootstrapping=hosttools` to speed up local
   > development. Note that on Linux — unlike macOS, where the toolchain already
   > comes with Xcode — this option additionally requires
   > [a recent Swift toolchain](https://www.swift.org/download/) to be
   > installed.

   This will create a directory `swift-project/build/Ninja-RelWithDebInfoAssert`
   containing the Swift compiler and standard library and clang/LLVM build artifacts.
   If the build fails, see [Troubleshooting build issues](#troubleshooting-build-issues).

   > **Note**  
   > `--release-debuginfo` means that although debug information will be produced, all targets will
   > be compiled in release mode, meaning optimized code, which can affect your debugging experience.
   > Consider [`--debug-swift` to build a debug variant of the compiler](#debugging-issues) and have 
   > the swift targets (including `swift-frontend`) built in debug mode.

   If you would like to additionally build the Swift corelibs,
   ie swift-corelibs-libdispatch, swift-corelibs-foundation, and swift-corelibs-xctest,
   on Linux, add the `--xctest` flag to `build-script`.

In the following sections, for simplicity, we will assume that you are using a
`Ninja-RelWithDebInfoAssert` build on macOS, unless explicitly mentioned otherwise.
You will need to slightly tweak the paths for other build configurations.

### Troubleshooting build issues

- Double-check that all projects are checked out at the right branches.
  A common failure mode is using `git checkout` to change the branch only
  for `swift` (often to a release branch), leading to an unsupported
  configuration. See Step 4 of [Cloning the Project](#cloning-the-project)
  on how to fix this.
- Double-check that all your dependencies
  [meet the minimum required versions](#spot-check-dependencies).
- Check if there are spaces in the paths being used by `build-script` in
  the log. While `build-script` should work with paths containing spaces,
  sometimes bugs do slip through, such as [#55883](https://github.com/apple/swift/issues/55883).
  If this is the case, please [file a bug report][Swift Issues] and change the path
  to work around it.
- Check that your `build-script` invocation doesn't have typos. You can compare
  the flags you passed against the supported flags listed by
  `utils/build-script --help`.
- Check the error logs and see if there is something you can fix.
  In many situations, there are several errors, so scrolling further back
  and looking at the first error may be more helpful than simply looking
  at the last error.
- Check if others have encountered the same issue on the [Swift Forums](https://forums.swift.org/c/development/compiler) or on [Swift repository 'Issues' tab][Swift Issues]. Here is a list of threads that describe common issues:
  * [Problems with `build-script` building compiler with `–xcode`](https://forums.swift.org/t/problems-with-build-script-building-compiler-with-xcode/53477)
  * [Error building the compiler (even with ninja)](https://forums.swift.org/t/error-building-the-compiler-even-with-ninja/54834)
  * [Build failure on Apple MacBook Pro with Apple M1 Chip](https://forums.swift.org/t/build-failure-on-apple-silicon-m1-mac-mini/45011)
  * [CMake cannot compile a test program](https://forums.swift.org/t/build-failure-locally/55695)
  * [Building Swift compiler from source fails when not using Ninja](https://forums.swift.org/t/building-swift-compiler-from-source-fails-when-not-using-ninja/54656)
  * [ALL_BUILD Target failing at validation](https://forums.swift.org/t/help-building-swift-in-xcode-error/49728)
  * [“gtest/gtest.h” not found while compiling the compiler](https://forums.swift.org/t/gtest-gtest-h-not-found-in-typeref-cpp-while-compiling-the-compiler/44399)
- If you still could not find a solution to your issue, feel free to create a new Swift Forums thread in the [Development/Compiler](https://forums.swift.org/c/development/compiler) category: 
  - Include information about your configuration and the errors you are seeing.
  - You can [create a gist](https://gist.github.com) with the entire build
    output and link it, while highlighting the most important part of the
    build log in the post.
  - Include the output of `utils/update-checkout --dump-hashes`.

[Swift Issues]: https://github.com/apple/swift/issues
[Swift Forums]: https://forums.swift.org

## Editing code

### Setting up your fork

If you are building the toolchain for development and submitting patches,
you will need to setup a GitHub fork.

First fork the `apple/swift` [repository](https://github.com/apple/swift.git),
using the "Fork" button in the web UI, near the top-right. This will create a
repository `username/swift` for your GitHub username. Next, add it as a remote:
```sh
# Using 'my-remote' as a placeholder name.

# If you set up SSH in step 2
git remote add my-remote git@github.com:username/swift.git

# If you used HTTPS in step 2
git remote add my-remote https://github.com/username/swift.git
```
Finally, create a new branch.
```sh
# Using 'my-branch' as a placeholder name
git checkout -b my-branch
git push --set-upstream my-remote my-branch
```

<!-- TODO: Insert paragraph about the main Ninja targets. -->


<!--
Note: utils/build-script contains a link to this heading that needs an update
whenever the heading is modified.
-->
### Using Ninja with Xcode

This workflow enables you to navigate, edit, build, run, and debug in Xcode
while retaining the option of building with Ninja on the command line.

Assuming that you have already [built the toolchain via Ninja](#the-actual-build),
several more steps are necessary to set up this environment:
* Generate Xcode projects with `utils/build-script --swift-darwin-supported-archs "$(uname -m)" --xcode --clean`.
  This will first build a few LLVM files that are needed to configure the
  projects.
* Create a new Xcode workspace.
* Add the generated Xcode projects or Swift packages that are relevant to your
  tasks to your workspace. All the Xcode projects can be found among the
  build artifacts under `build/Xcode-*/`. For example:
  * If you are aiming for the compiler, add `build/Xcode-*/swift-macosx-*/Swift.xcodeproj`.
    This project also includes the standard library and runtime sources. If you
    need the parts of the compiler that are implemented in Swift itself, add the
    `swift/SwiftCompilerSources/Package.swift` package as well.
  * If you are aiming for just the standard library or runtime, add
    `build/Xcode-*/swift-macosx-*/stdlib/Swift-stdlib.xcodeproj`.
  <!-- FIXME: Without this "hard" line break, the note doesn’t get properly spaced from the bullet -->
  <br />

  > **Warning**  
  > Adding both `Swift.xcodeproj` and `LLVM.xcodeproj` *might* slow down the IDE
    and is not recommended unless you know what you're doing.

  In general, we encourage you to add only what you need. Keep in mind that none
  of the generated Xcode projects are required to build or run with this setup
  because we are using Ninja—an *external* build system; rather, they should be
  viewed as a means of leveraging the navigation, editing and debugging features
  of the IDE in relation to the source code they wrap.

* Create an empty Xcode project in the workspace, using the
  _External Build System_ template.
* Add a target to the empty project, using the _External Build System_ template,
  and name it after the Ninja target that you want to build (e.g. `swift-frontend`
  is the compiler).
* In the _Info_ pane of the target settings, set
  * _Build Tool_ to the absolute path of the `ninja` executable (the output of
    `which ninja` on the command line)
  * _Arguments_ to the Ninja target (e.g. `bin/swift-frontend`)
  * _Directory_ to the absolute path of the build directory where the Ninja
    target lives. For Swift targets such as the compiler or standard library,
    this is the `build/Ninja-*/swift-macosx-*` directory.
* Add a scheme for the target. In the drop-down menu, be careful not to mistake
  your target for a similar one that belongs to a generated Xcode project.
* > **Note**  
  > Ignore this step if the target associates to a non-executable Ninja target
    like `swift-stdlib`.

  Adjust the _Run_ action settings of the scheme:
  * In the _Info_ pane, select the _Executable_ built by the Ninja target from
    the appropriate `bin` directory (e.g. `build/Ninja-*/swift-macosx-*/bin/swift-frontend`).
  * In the _Arguments_ pane, add the command line arguments that you want to
    pass to the executable on launch (e.g. `path/to/file.swift -typecheck` for
    `bin/swift-frontend`).
  * You can optionally set the working directory for debugging in the
    _Options_ pane.
* Configure as many more target-scheme pairs as you need.

Now you are all set! You can build, run and debug as with a native Xcode
project. If an `update-checkout` routine or a structural change—such as when
source files are added or deleted—happens to impact your editing experience,
simply regenerate the Xcode projects.

> **Note**  
> * For debugging to *fully* work for a given component—say, the compiler—the
    `build-script` invocation for the Ninja build must be arranged to
    [build a debug variant of that component](#debugging-issues).
> * Xcode's indexing can occasionally start slipping after switching to and back
    from a distant branch, resulting in a noticeable slowdown. To sort things
    out, close the workspace and delete the _Index_ directory from the
    workspace's derived data before reopening.

### Other IDEs setup

You can also use other editors and IDEs to work on Swift.

#### IntelliJ CLion

CLion supports CMake and Ninja. In order to configure it properly, build the swift project first using the `build-script`, then open the `swift` directory with CLion and proceed to project settings (`cmd + ,`).

In project settings, locate `Build, Execution, Deployment > CMake`. You will need to create a new profile named `RelWithDebInfoAssert` (or `Debug` if going to point it at the debug build). Enter the following information:

- Name: mirror the name of the build configuration here, e.g. `RelWithDebInfoAssert` or `Debug`
- Build type: This corresponds to `CMAKE_BUILD_TYPE` so should be e.g. `RelWithDebInfoAssert` or `Debug`
    - latest versions of the IDE suggest valid values here. Generally `RelWithDebInfoAssert` is a good one to work with
- Toolchain: Default should be fine
- Generator: Ninja
- CMake options: You want to duplicate the essential CMake flags that `build-script` had used here, so CLion understands the build configuration. You can get the full list of CMake arguments from `build-script` by providing the `-n` dry-run flag; look for the last `cmake` command with a `-G Ninja`. Here is a minimal list of what you should provide to CLion here for this setting:
    - `-D SWIFT_PATH_TO_CMARK_BUILD=SOME_PATH/swift-project/build/Ninja-RelWithDebInfoAssert/cmark-macosx-arm64 -D LLVM_DIR=SOME_PATH/swift-project/build/Ninja-RelWithDebInfoAssert/llvm-macosx-arm64/lib/cmake/llvm -D Clang_DIR=SOME_PATH/swift-project/build/Ninja-RelWithDebInfoAssert/llvm-macosx-arm64/lib/cmake/clang -D CMAKE_BUILD_TYPE=RelWithDebInfoAssert -D
SWIFT_PATH_TO_SWIFT_SYNTAX_SOURCE=SOME_PATH/swift-project/swift-syntax -G Ninja -S .`
    - replace the `SOME_PATH` to the path where your `swift-project` directory is
    - the CMAKE_BUILD_TYPE should match the build configuration name, so if you named this profile `RelWithDebInfo` the CMAKE_BUILD_TYPE should also be `RelWithDebInfo`
    - **Note**: If you're using an Intel machine to build swift, you'll need to replace the architecture in the options. (ex: `arm64` with `x86_64`)
- Build Directory: change this to the Swift build directory corresponding to the `build-script` run you did earlier, for example, `SOME_PATH/swift-project/build/Ninja-RelWithDebInfoAssert/swift-macosx-arm64`.

With this done, CLion should be able to successfully import the project and have full autocomplete and code navigation powers.

### Editing

Make changes to the code as appropriate. Implement a shiny new feature!
Or fix a nasty bug! Update the documentation as you go! <!-- please 🙏 -->
The codebase is your oyster!

:construction::construction_worker::building_construction:

Now that you have made some changes, you will need to rebuild...

### Incremental builds with Ninja

Subsequent steps in this and the next subsections are specific to the platform you're building on, so we'll try to detect it first and reuse as a shell variable:

```sh
platform=$([[ $(uname) == Darwin ]] && echo macosx || echo linux)
```

After setting that variable you can rebuild the compiler incrementally with this command:
```sh
ninja -C ../build/Ninja-RelWithDebInfoAssert/swift-${platform}-$(uname -m) bin/swift-frontend
```

To rebuild everything that has its sources located in the `swift` repository, including the standard library:
```sh
ninja -C ../build/Ninja-RelWithDebInfoAssert/swift-${platform}-$(uname -m)
```

Similarly, you can rebuild other projects like Foundation or Dispatch by substituting their respective subdirectories in the commands above.

### Spot checking an incremental build

As a quick test, go to `lib/Basic/Version.cpp` and tweak the version
printing code slightly. Next, do an incremental build as above. This incremental
build should be much faster than the from-scratch build at the beginning.
Now check if the version string has been updated (assumes you have `platform` shell variable
defined as specified in the previous subsection:

```sh
../build/Ninja-RelWithDebInfoAssert/swift-$(platform)-$(uname -m)/bin/swift-frontend --version
```

This should print your updated version string.

## Reproducing an issue

[Good first issues](https://github.com/apple/swift/contribute) typically have
small code examples that fit within a single file. You can reproduce such an
issue in various ways, such as compiling it from the command line using
`/path/to/swiftc MyFile.swift`, pasting the code into [Compiler Explorer](https://godbolt.org)
(aka godbolt) or using an Xcode Playground.

For files using frameworks from an SDK bundled with Xcode, you need the pass
the SDK explicitly. Here are a couple of examples:
```sh
# Compile a file to an executable for your local machine.
xcrun -sdk macosx /path/to/swiftc MyFile.swift

# Say you are trying to compile a file importing an iOS-only framework.
xcrun -sdk iphoneos /path/to/swiftc -target arm64-apple-ios13.0 MyFile.swift
```
You can see the full list of `-sdk` options using `xcodebuild -showsdks`,
and check some potential `-target` options for different operating systems by
skimming the compiler's test suite under `test/`.

Sometimes bug reports come with SwiftPM packages or Xcode projects as minimal
reproducers. While we do not add packages or projects to the compiler's test
suite, it is generally helpful to first reproduce the issue in context before
trying to create a minimal self-contained test case. If that's the case with
the bug you're working on, check out our
[instructions on building packages and Xcode projects with a locally built compiler](/docs/HowToGuides/FAQ.md#how-do-i-use-a-locally-built-compiler-to-build-x).

## Running tests

There are two main ways to run tests:

1. `utils/run-test`: By default, `run-test` builds the tests' dependencies
   before running them.
   ```sh
   # Rebuild all test dependencies and run all tests under test/.
   utils/run-test --lit ../llvm-project/llvm/utils/lit/lit.py \
     ../build/Ninja-RelWithDebInfoAssert/swift-macosx-$(uname -m)/test-macosx-$(uname -m)

   # Rebuild all test dependencies and run tests containing "MyTest".
   utils/run-test --lit ../llvm-project/llvm/utils/lit/lit.py \
     ../build/Ninja-RelWithDebInfoAssert/swift-macosx-$(uname -m)/test-macosx-$(uname -m) \
     --filter="MyTest"
   ```
2. `lit.py`: lit doesn't know anything about dependencies. It just runs tests.
   ```sh
   # Run all tests under test/.
   ../llvm-project/llvm/utils/lit/lit.py -s -vv \
     ../build/Ninja-RelWithDebInfoAssert/swift-macosx-$(uname -m)/test-macosx-$(uname -m)

   # Run tests containing "MyTest"
   ../llvm-project/llvm/utils/lit/lit.py -s -vv \
     ../build/Ninja-RelWithDebInfoAssert/swift-macosx-$(uname -m)/test-macosx-$(uname -m) \
     --filter="MyTest"
   ```
   The `-s` and `-vv` flags print a progress bar and the executed commands
   respectively.

If you are making small changes to the compiler or some other component, you'll
likely want to [incrementally rebuild](#editing-code) only the relevant
target and use `lit.py` with `--filter`. One potential failure mode with this
approach is accidental use of stale binaries. For example, say that you want to
rerun a SourceKit test but you only incrementally rebuilt the compiler. Then
your changes will not be reflected when the test runs because the `sourcekitd`
binary was not rebuilt. Using `run-test` instead is the safer option, but it
will lead to a longer feedback loop due to more things getting rebuilt.

In the rare event that a local test failure happens to be unrelated to your
changes (is not due to stale binaries and reproduces without your changes),
there is a good chance that it has already been caught by our continuous
integration infrastructure, and it may be ignored.

If you want to rerun all the tests, you can either rebuild the whole project
and use `lit.py` without `--filter` or use `run-test` to handle both aspects.

For more details on running tests and understanding the various Swift-specific
lit customizations, see [Testing.md](/docs/Testing.md). Also check out the
[lit documentation](https://llvm.org/docs/CommandGuide/lit.html) to understand
how the different lit commands work.

## Debugging issues

In this section, we briefly describe two common ways of debugging: print
debugging and using LLDB.

Depending on the code you're interested in, LLDB may be significantly more
effective when using a debug build. Depending on what components you are
working on, you could turn off optimizations for only a few things.
Here are some example invocations:

```sh
# optimized Stdlib + debug Swiftc + optimized Clang/LLVM
utils/build-script --release-debuginfo --debug-swift # other flags...

# debug Stdlib + optimized Swiftc + optimized Clang/LLVM
utils/build-script --release-debuginfo --debug-swift-stdlib # other flags...

# optimized Stdlib + debug Swiftc (except typechecker) + optimized Clang/LLVM
utils/build-script --release-debuginfo --debug-swift --force-optimized-typechecker

# Last resort option, it is highly unlikely that you will need this
# debug Stdlib + debug Swiftc + debug Clang/LLVM
utils/build-script --debug # other flags...
```

Debug builds have two major drawbacks:
- A debug compiler is much slower, leading to longer feedback loops in case you
  need to repeatedly compile the Swift standard library and/or run a large
  number of tests.
- The build artifacts consume a lot more disk space.

[DebuggingTheCompiler.md](/docs/DebuggingTheCompiler.md) goes into a LOT
more detail on how you can level up your debugging skills! Make sure you check
it out in case you're trying to debug a tricky issue and aren't sure how to
go about it.

### Print debugging

A large number of types have `dump(..)`/`print(..)` methods which can be used
along with `llvm::errs()` or other LLVM streams. For example, if you have a
variable `std::vector<CanType> canTypes` that you want to print, you could do:
```cpp
auto &e = llvm::errs();
e << "canTypes = [";
llvm::interleaveComma(canTypes, e, [&](auto ty) { ty.dump(e); });
e << "]\n";
```
You can also crash the compiler using `assert`/`llvm_unreachable`/
`llvm::report_fatal_error`, after accumulating the result in a stream:
```cpp
std::string msg; llvm::raw_string_ostream os(msg);
os << "unexpected canTypes = [";
llvm::interleaveComma(canTypes, os, [&](auto ty) { ty.dump(os); });
os << "] !!!\n";
llvm::report_fatal_error(os.str());
```

### Debugging using LLDB

When the compiler crashes, the commandline arguments passed to it will be
printed to stderr. It will likely look something like:

```
/path/to/swift-frontend <args>
```

- Using LLDB on the commandline: Copy the entire invocation and pass it to LLDB.
  ```sh
  lldb -- /path/to/swift-frontend <args>
  ```
  Now you can use the usual LLDB commands like `run`, `breakpoint set` and so
  on. If you are new to LLDB, check out the [official LLDB documentation][] and
  [nesono's LLDB cheat sheet][].
- Using LLDB within Xcode:
  Select the current scheme 'swift-frontend' → Edit Scheme → Run phase →
  Arguments tab. Under "Arguments Passed on Launch", copy-paste the `<args>`
  and make sure that "Expand Variables Based On" is set to swift-frontend.
  Close the scheme editor. If you now run the compiler
  (<kbd>⌘</kbd>+<kbd>R</kbd> or Product → Run), you will be able to use the
  Xcode debugger.

  Xcode also has the ability to attach to and debug Swift processes launched
  elsewhere. Under Debug → Attach to Process by PID or name..., you can enter
  a compiler process's PID or name (`swift-frontend`) to debug a compiler
  instance invoked elsewhere. This can be helpful if you have a single compiler
  process being invoked by another tool, such as SwiftPM or another open Xcode
  project.

  > **Pro Tip**: Xcode 12's terminal does not support colors, so you may see
  > explicit color codes printed by `dump()` methods on various types. To avoid
  > color codes in dumped output, run `expr llvm::errs().enable_color(false)`.

[official LLDB documentation]: https://lldb.llvm.org
[nesono's LLDB cheat sheet]: https://www.nesono.com/sites/default/files/lldb%20cheat%20sheet.pdf

## Next steps

Make sure you check out the following resources:

* [LLVM Coding Standards](https://llvm.org/docs/CodingStandards.html): A style
  guide followed by both LLVM and Swift. If there is a mismatch between the LLVM
  Coding Standards and the surrounding code that you are editing, please match
  the style of existing code.
* [LLVM Programmer's Manual](https://llvm.org/docs/ProgrammersManual.html):
  A guide describing common programming idioms and data types used by LLVM and
  Swift.
* [docs/README.md](/docs/README.md): Provides a bird's eye view of the available
  documentation.
* [Lexicon.md](/docs/Lexicon.md): Provides definitions for jargon. If you run
  into a term frequently that you don't recognize, it's likely that this file
  has a definition for it.
* [Testing.md](/docs/Testing.md) and
  [DebuggingTheCompiler.md](/docs/DebuggingTheCompiler.md): These cover more
  ground on testing and debugging respectively.
* [Development Tips](/docs/DevelopmentTips.md): Tips for being more productive.
<!-- Link to Compiler Architecture.md once that is ready -->

If you see mistakes in the documentation (including typos, not just major
errors) or identify gaps that you could potentially improve the contributing
experience, please start a discussion on the forums, submit a pull request
or file a bug report on [Swift repository 'Issues' tab][Swift Issues]. Thanks!
