:: build-windows.bat
::
:: This source file is part of the Swift.org open source project
::
:: Copyright (c) 2014 - 2019 Apple Inc. and the Swift project authors
:: Licensed under Apache License v2.0 with Runtime Library Exception
::
:: See https://swift.org/LICENSE.txt for license information
:: See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors

:: REQUIRED ENVIRONMENT VARIABLES
:: This script requires to be executed inside one Visual Studio command line,
:: in order for many of the tools and environment variables to be available.
:: Additionally, it needs the following variables:
:: - CMAKE_BUILD_TYPE: Kind of build: Release, RelWithDebInfo, Debug.
:: - PYTHON_HOME: The Python installation directory.
:: - REPO_SCHEME: Optional. The scheme name to checkout.

:: REQUIRED PERMISSIONS
:: Practically, it is easier to be in the Adminstrators group to run the
:: script, but it should be possible to execute as a normal user.
:: The user will need permission to write files into the Windows SDK and the
:: VisualC++ folder.

:: @echo off

setlocal enableextensions enabledelayedexpansion

PATH=%PATH%;%PYTHON_HOME%

set icu_version_major=64
set icu_version_minor=2
set icu_version=%icu_version_major%_%icu_version_minor%
set icu_version_dashed=%icu_version_major%-%icu_version_minor%

set "exitOnError=|| (exit /b)"
set current_directory=%~dp0
set current_directory=%current_directory:~0,-1%
set source_root=%current_directory%\..\..

:: Resetting source_root with %CD% removes the ..\.. from the paths, and makes
:: the output easier to read.
cd %source_root%
set source_root=%CD%

set full_build_root=%source_root%\build
mkdir %full_build_root%

:: Use the shortest path we can for the build directory, to avoid Windows
:: path problems as much as we can.
subst T: /d
subst T: %full_build_root% %exitOnError%
set build_root=T:
set install_directory=%build_root%\Library\Developer\Toolchains\unknown-Asserts-development.xctoolchain\usr

md %build_root%\tmp
set TMPDIR=%build_root%\tmp
set TMP=%build_root%\tmp
set TEMP=%build_root%\tmp

md %build_root%\tmp\org.llvm.clang.9999
set CUSTOM_CLANG_MODULE_CACHE=%build_root%\tmp\org.llvm.clang.9999

md %build_root%\tmp\org.swift.package-manager
set SWIFTPM_MODULECACHE_OVERRIDE=%build_root%\tmp\org.swift.package-manager

call :clone_repositories %exitOnError%
call :download_icu %exitOnError%
:: TODO: Disabled until we need LLBuild/SwiftPM in this build script.
:: call :download_sqlite3

call :build_llvm %exitOnError%
path %PATH%;%install_directory%\bin

call :build_cmark %exitOnError%

call :prepare_platform_modules %exitOnError%
call :build_swift %exitOnError%

call :build_lldb %exitOnError%

call :build_libdispatch %exitOnError%

path %source_root%\icu-%icu_version%\bin64;%install_directory%\bin;%build_root%\swift\bin;%build_root%\swift\libdispatch-prefix\bin;%PATH%;C:\Program Files\Git\usr\bin
call :test_swift %exitOnError%
call :test_libdispatch %exitOnError%

goto :end
endlocal

:clone_repositories
:: Clones the repositories used by the Windows build.
:: It supposes that the swift repository is already cloned by CI.
:: It supposes the %CD% is the source root.
setlocal enableextensions enabledelayedexpansion

if defined REPO_SCHEME SET "scheme_arg=--scheme %REPO_SCHEME%"

git -C "%source_root%\swift" config --local core.autocrlf input
git -C "%source_root%\swift" config --local core.symlink true
git -C "%source_root%\swift" checkout-index --force --all

:: Always skip Swift, since it is checked out by Jenkins
@set "skip_repositories_arg=--skip-repository swift"
@set "skip_repositories_arg=%skip_repositories_arg% --skip-repository llbuild"
@set "skip_repositories_arg=%skip_repositories_arg% --skip-repository indexstore-db"
@set "skip_repositories_arg=%skip_repositories_arg% --skip-repository ninja"
@set "skip_repositories_arg=%skip_repositories_arg% --skip-repository sourcekit-lsp"
@set "skip_repositories_arg=%skip_repositories_arg% --skip-repository swift-argument-parser"
@set "skip_repositories_arg=%skip_repositories_arg% --skip-repository swift-corelibs-foundation"
@set "skip_repositories_arg=%skip_repositories_arg% --skip-repository swift-corelibs-xctest"
@set "skip_repositories_arg=%skip_repositories_arg% --skip-repository swift-driver"
@set "skip_repositories_arg=%skip_repositories_arg% --skip-repository swift-format"
@set "skip_repositories_arg=%skip_repositories_arg% --skip-repository swift-integration-tests"
@set "skip_repositories_arg=%skip_repositories_arg% --skip-repository swiftpm"
@set "skip_repositories_arg=%skip_repositories_arg% --skip-repository swift-stress-tester"
@set "skip_repositories_arg=%skip_repositories_arg% --skip-repository swift-syntax"
@set "skip_repositories_arg=%skip_repositories_arg% --skip-repository swift-tools-support-core"
@set "skip_repositories_arg=%skip_repositories_arg% --skip-repository swift-xcode-playground-support"
@set "skip_repositories_arg=%skip_repositories_arg% --skip-repository tensorflow-swift-apis"
@set "skip_repositories_arg=%skip_repositories_arg% --skip-repository yams"

call "%source_root%\swift\utils\update-checkout.cmd" %scheme_arg% %skip_repositories_arg% --clone --skip-history --github-comment "%ghprbCommentBody%" >NUL 2>NUL

goto :eof
endlocal


:download_icu
:: Downloads ICU, which will be used as a dependency for the Swift Standard
:: Library and Foundation.
setlocal enableextensions enabledelayedexpansion

set file_name=icu4c-%icu_version%-Win64-MSVC2017.zip
curl -L -O "https://github.com/unicode-org/icu/releases/download/release-%icu_version_dashed%/%file_name%" %exitOnError%
:: unzip warns about the paths in the zip using slashes, which raises the
:: errorLevel to 1. We cannot use exitOnError, and have to ignore errors.
"C:\Program Files\Git\usr\bin\unzip.exe" -o %file_name% -d "%source_root%\icu-%icu_version%"
exit /b 0

goto :eof
endlocal


:download_sqlite3
:: Downloads SQLite3, which will be used as a dependency for llbuild and
:: Swift Package Manager.
setlocal enableextensions enabledelayedexpansion

set file_name=sqlite-amalgamation-3270200.zip
curl -L -O "https://www.sqlite.org/2019/%file_name%" %exitOnError%
"C:\Program Files\Git\usr\bin\unzip.exe" -o %file_name% %exitOnError%

goto :eof
endlocal


:prepare_platform_modules
:: Create files into the right places of the Windows SDK to the files in the
:: swift repository, in order to consider the headers of the Windows SDK a
:: module to compile Swift code against them.
setlocal enableextensions enabledelayedexpansion

copy /y "%source_root%\swift\stdlib\public\Platform\ucrt.modulemap" "%UniversalCRTSdkDir%\Include\%UCRTVersion%\ucrt\module.modulemap" %exitOnError%
copy /y "%source_root%\swift\stdlib\public\Platform\winsdk.modulemap" "%UniversalCRTSdkDir%\Include\%UCRTVersion%\um\module.modulemap" %exitOnError%

goto :eof
endlocal


:build_llvm
:: Configures, builds, and installs LLVM
setlocal enableextensions enabledelayedexpansion

cmake^
    -B "%build_root%\llvm"^
    -G Ninja^
    -DCMAKE_BUILD_TYPE=%CMAKE_BUILD_TYPE%^
    -DCMAKE_C_COMPILER=cl^
    -DCMAKE_CXX_COMPILER=cl^
    -DCMAKE_MT=mt^
    -DCMAKE_INSTALL_PREFIX:PATH=%install_directory%^
    -DLLVM_DEFAULT_TARGET_TRIPLE=x86_64-unknown-windows-msvc^
    -DLLVM_ENABLE_PDB:BOOL=YES^
    -DLLVM_ENABLE_ASSERTIONS:BOOL=YES^
    -DLLVM_ENABLE_PROJECTS:STRING=lld;clang^
    -DLLVM_TARGETS_TO_BUILD:STRING="AArch64;ARM;X86"^
    -DLLVM_INCLUDE_BENCHMARKS:BOOL=NO^
    -DLLVM_INCLUDE_DOCS:BOOL=NO^
    -DLLVM_INCLUDE_EXAMPLES:BOOL=NO^
    -DLLVM_INCLUDE_GO_TESTS:BOOL=NO^
    -DLLVM_TOOL_GOLD_BUILD:BOOL=NO^
    -DLLVM_ENABLE_OCAMLDOC:BOOL=NO^
    -DLLVM_ENABLE_LIBXML2:BOOL=NO^
    -DLLVM_ENABLE_ZLIB:BOOL=NO^
    -DLLVM_TEMPORARILY_ALLOW_OLD_TOOLCHAIN=ON^
    -DENABLE_X86_RELAX_RELOCATIONS:BOOL=YES^
    -DLLVM_INSTALL_BINUTILS_SYMLINKS:BOOL=YES^
    -DLLVM_INSTALL_TOOLCHAIN_ONLY:BOOL=YES^
    -DLLVM_TOOLCHAIN_TOOLS:STRING="addr2line;ar;c++filt;dsymutil;dwp;llvm-ar;llvm-cov;llvm-cvtres;llvm-cxxfilt;llvm-dlltool;llvm-dwp;llvm-ranlib;llvm-lib;llvm-mt;llvm-nm;llvm-objdump;llvm-pdbutil;llvm-profdata;llvm-rc;llvm-readelf;llvm-readobj;llvm-size;llvm-strip;llvm-symbolizer;llvm-undname;nm;objcopy;objdump;ranlib;readelf;size;strings"^
    -DCLANG_TOOLS="clang;clang-format;clang-headers;clang-tidy"^
    -DCMAKE_CXX_FLAGS:STRING="/GS- /Oy"^
    -DCMAKE_EXE_LINKER_FLAGS:STRING=/INCREMENTAL:NO^
    -DCMAKE_SHARED_LINKER_FLAGS:STRING=/INCREMENTAL:NO^
    -S "%source_root%\llvm-project\llvm" %exitOnError%

cmake --build "%build_root%\llvm" %exitOnError%
cmake --build "%build_root%\llvm" --target install %exitOnError%

goto :eof
endlocal


:build_cmark
:: Configures and builds CMark
setlocal enableextensions enabledelayedexpansion

cmake^
    -B "%build_root%\cmark"^
    -G Ninja^
    -DCMAKE_BUILD_TYPE=%CMAKE_BUILD_TYPE%^
    -DCMAKE_C_COMPILER=cl^
    -DCMAKE_CXX_COMPILER=cl^
    -DCMAKE_MT=mt^
    -DCMAKE_CXX_FLAGS:STRING="/GS- /Oy"^
    -DCMAKE_EXE_LINKER_FLAGS:STRING=/INCREMENTAL:NO^
    -DCMAKE_SHARED_LINKER_FLAGS:STRING=/INCREMENTAL:NO^
    -S "%source_root%\cmark" %exitOnError%

cmake --build "%build_root%\cmark" %exitOnError%

goto :eof
endlocal


:build_swift
:: Configures, builds, and installs Swift and the Swift Standard Library
setlocal enableextensions enabledelayedexpansion

:: SWIFT_PARALLEL_LINK_JOBS=8 allows the build machine to use as many CPU as
:: possible, while not exhausting the RAM.
cmake^
    -B "%build_root%\swift"^
    -G Ninja^
    -DCMAKE_BUILD_TYPE=%CMAKE_BUILD_TYPE%^
    -DCMAKE_C_COMPILER=cl^
    -DCMAKE_CXX_COMPILER=cl^
    -DCMAKE_MT=mt^
    -DCMAKE_INSTALL_PREFIX:PATH=%install_directory%^
    -DClang_DIR:PATH=%build_root%\llvm\lib\cmake\clang^
    -DSWIFT_PATH_TO_CMARK_BUILD:PATH=%build_root%\cmark^
    -DSWIFT_PATH_TO_CMARK_SOURCE:PATH=%source_root%\cmark^
    -DSWIFT_PATH_TO_LIBDISPATCH_SOURCE:PATH=%source_root%\swift-corelibs-libdispatch^
    -DLLVM_DIR:PATH=%build_root%\llvm\lib\cmake\llvm^
    -DLLVM_TEMPORARILY_ALLOW_OLD_TOOLCHAIN=ON^
    -DSWIFT_INCLUDE_DOCS:BOOL=NO^
    -DSWIFT_WINDOWS_x86_64_ICU_UC_INCLUDE:PATH=%source_root%\icu-%icu_version%\include\unicode^
    -DSWIFT_WINDOWS_x86_64_ICU_UC:PATH=%source_root%\icu-%icu_version%\lib64\icuuc.lib^
    -DSWIFT_WINDOWS_x86_64_ICU_I18N_INCLUDE:PATH=%source_root%\icu-%icu_version%\include^
    -DSWIFT_WINDOWS_x86_64_ICU_I18N:PATH=%source_root%\icu-%icu_version%\lib64\icuin.lib^
    -DSWIFT_BUILD_DYNAMIC_STDLIB:BOOL=YES^
    -DSWIFT_BUILD_DYNAMIC_SDK_OVERLAY:BOOL=YES^
    -DSWIFT_BUILD_STATIC_STDLIB:BOOL=NO^
    -DSWIFT_BUILD_STATIC_SDK_OVERLAY:BOOL=NO^
    -DLLVM_INSTALL_TOOLCHAIN_ONLY:BOOL=YES^
    -DSWIFT_BUILD_SOURCEKIT:BOOL=YES^
    -DSWIFT_ENABLE_SOURCEKIT_TESTS:BOOL=YES^
    -DSWIFT_ENABLE_EXPERIMENTAL_CONCURRENCY=YES^
    -DSWIFT_ENABLE_EXPERIMENTAL_DISTRIBUTED=YES^
    -DSWIFT_ENABLE_EXPERIMENTAL_DIFFERENTIABLE_PROGRAMMING=YES^
    -DSWIFT_INSTALL_COMPONENTS="autolink-driver;compiler;clang-resource-dir-symlink;stdlib;sdk-overlay;editor-integration;tools;sourcekit-inproc;swift-remote-mirror;swift-remote-mirror-headers"^
    -DSWIFT_PARALLEL_LINK_JOBS=8^
    -DPYTHON_EXECUTABLE:PATH=%PYTHON_HOME%\python.exe^
    -DCMAKE_CXX_FLAGS:STRING="/GS- /Oy"^
    -DCMAKE_EXE_LINKER_FLAGS:STRING=/INCREMENTAL:NO^
    -DCMAKE_SHARED_LINKER_FLAGS:STRING=/INCREMENTAL:NO^
    -DSWIFT_LIT_ARGS="--time-tests"^
    -S "%source_root%\swift" %exitOnError%

cmake --build "%build_root%\swift" %exitOnError%
cmake --build "%build_root%\swift" --target install %exitOnError%

goto :eof
endlocal


:test_swift
:: Tests the Swift compiler and the Swift Standard Library
setlocal enableextensions enabledelayedexpansion

cmake --build "%build_root%\swift" --target check-swift %exitOnError%

goto :eof
endlocal


:build_lldb
:: Configures, builds, and installs LLDB
setlocal enableextensions enabledelayedexpansion

cmake^
    -B "%build_root%\lldb"^
    -G Ninja^
    -DCMAKE_BUILD_TYPE=%CMAKE_BUILD_TYPE%^
    -DCMAKE_C_COMPILER=cl^
    -DCMAKE_CXX_COMPILER=cl^
    -DCMAKE_MT=mt^
    -DCMAKE_INSTALL_PREFIX:PATH=%install_directory%^
    -DLLVM_DIR:PATH=%build_root%\llvm\lib\cmake\llvm^
    -DClang_DIR:PATH=%build_root%\llvm\lib\cmake\clang^
    -DSwift_DIR:PATH=%build_root%\swift\lib\cmake\swift^
    -DLLVM_ENABLE_ASSERTIONS:BOOL=YES^
    -DLLDB_USE_STATIC_BINDINGS:BOOL=YES^
    -DPYTHON_HOME:PATH=%PYTHON_HOME%^
    -DCMAKE_CXX_FLAGS:STRING="/GS- /Oy"^
    -DCMAKE_EXE_LINKER_FLAGS:STRING=/INCREMENTAL:NO^
    -DCMAKE_SHARED_LINKER_FLAGS:STRING=/INCREMENTAL:NO^
    -DLLDB_DISABLE_PYTHON=YES^
    -DLLDB_INCLUDE_TESTS:BOOL=NO^
    -DLLVM_TEMPORARILY_ALLOW_OLD_TOOLCHAIN=ON^
    -S "%source_root%\llvm-project\lldb" %exitOnError%

cmake --build "%build_root%\lldb" %exitOnError%
cmake --build "%build_root%\lldb" --target install %exitOnError%

goto :eof
endlocal


:build_libdispatch
:: Configures, builds, and installs Dispatch
setlocal enableextensions enabledelayedexpansion

cmake^
    -B "%build_root%\swift-corelibs-libdispatch"^
    -G Ninja^
    -DCMAKE_BUILD_TYPE=%CMAKE_BUILD_TYPE%^
    -DCMAKE_C_COMPILER=clang-cl^
    -DCMAKE_CXX_COMPILER=clang-cl^
    -DCMAKE_MT=mt^
    -DCMAKE_Swift_COMPILER=swiftc^
    -DSwift_DIR:PATH=%build_root%\swift\lib\cmake\swift^
    -DCMAKE_INSTALL_PREFIX:PATH=%install_directory%^
    -DCMAKE_C_COMPILER_TARGET=x86_64-unknown-windows-msvc^
    -DCMAKE_CXX_COMPILER_TARGET=x86_64-unknown-windows-msvc^
    -DENABLE_SWIFT:BOOL=YES^
    -DENABLE_TESTING:BOOL=YES^
    -DCMAKE_C_FLAGS:STRING="${CMAKE_C_FLAGS} --target=x86_64-unknown-windows-msvc /GS- /Oy /Gw /Gy"^
    -DCMAKE_CXX_FLAGS:STRING="${CMAKE_CXX_FLAGS} --target=x86_64-unknown-windows-msvc /GS- /Oy /Gw /Gy"^
    -DCMAKE_EXE_LINKER_FLAGS:STRING="/INCREMENTAL:NO"^
    -DCMAKE_SHARED_LINKER_FLAGS:STRING="/INCREMENTAL:NO"^
    -DCMAKE_Swift_COMPILER_TARGET:STRING=x86_64-unknown-windows-msvc^
    -DCMAKE_Swift_FLAGS:STRING="-resource-dir \"%install_directory%\lib\swift\""^
    -DCMAKE_Swift_LINK_FLAGS:STRING="-resource-dir \"%install_directory%\lib\swift\""^
    -S "%source_root%\swift-corelibs-libdispatch" %exitOnError%

cmake --build "%build_root%\swift-corelibs-libdispatch" %exitOnError%
cmake --build "%build_root%\swift-corelibs-libdispatch" --target install %exitOnError%

goto :eof
endlocal


:test_libdispatch
:: Tests libdispatch C interface
setlocal enableextensions enabledelayedexpansion

cmake --build "%build_root%\swift-corelibs-libdispatch" --target ExperimentalTest %exitOnError%

goto :eof
endlocal


:end
