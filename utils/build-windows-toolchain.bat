:: build-windows-toolchain.bat
::
:: This source file is part of the Swift.org open source project
::
:: Copyright (c) 2014 - 2021 Apple Inc. and the Swift project authors
:: Licensed under Apache License v2.0 with Runtime Library Exception
::
:: See https://swift.org/LICENSE.txt for license information
:: See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors

setlocal enableextensions enabledelayedexpansion
path %PATH%;%PYTHON_HOME%

:: Identify the SourceRoot
:: Normalize the SourceRoot to make it easier to read the output.
cd %~dp0\..\..
set SourceRoot=%CD%

:: Identify the BuildRoot
set BuildRoot=%SourceRoot%\build

md %BuildRoot%
subst T: /d
subst T: %BuildRoot% || (exit /b)
set BuildRoot=T:

:: Identify the PackageRoot
set PackageRoot=%BuildRoot%\package

md %PackageRoot%

:: Identify the InstallRoot
set InstallRoot=%BuildRoot%\Library\Developer\Toolchains\unknown-Asserts-development.xctoolchain\usr
set PlatformRoot=%BuildRoot%\Library\Developer\Platforms\Windows.platform
set SDKInstallRoot=%PlatformRoot%\Developer\SDKs\Windows.sdk

:: Setup temporary directories
md %BuildRoot%\tmp
set TEMP=%BuildRoot%\tmp
set TMP=%BuildRoot%\tmp
set TMPDIR=%BuildRoot%\tmp

set NINJA_STATUS=[%%f/%%t][%%p][%%es] 

:: Always enable symbolic links
git config --global core.symlinks true

call :CloneRepositories || (exit /b)
call :CloneICU || (exit /b)

md "%BuildRoot%\Library"

:: Build ICU
copy %SourceRoot%\swift-installer-scripts\shared\ICU\CMakeLists.txt %SourceRoot%\icu\icu4c\ || (exit /b)
cmake ^
  -B %BuildRoot%\icu ^

  -D BUILD_SHARED_LIBS=NO ^
  -D CMAKE_BUILD_TYPE=%CMAKE_BUILD_TYPE% ^
  -D CMAKE_C_COMPILER=cl ^
  -D CMAKE_C_FLAGS="/GS- /Oy /Gw /Gy" ^
  -D CMAKE_CXX_COMPILER=cl ^
  -D CMAKE_CXX_FLAGS="/GS- /Oy /Gw /Gy" ^
  -D CMAKE_MT=mt ^
  -D CMAKE_EXE_LINKER_FLAGS="/INCREMENTAL:NO" ^
  -D CMAKE_SHARED_LINKER_FLAGS="/INCREMENTAL:NO" ^

  -D CMAKE_INSTALL_PREFIX=%BuildRoot%\Library\icu-69.1\usr ^

  -D BUILD_TOOLS=YES ^

  -G Ninja ^
  -S %SourceRoot%\icu\icu4c || (exit /b)
cmake --build "%BuildRoot%\icu" || (exit /b)
cmake --build "%BuildRoot%\icu" --target install || (exit /b)

:: Prepare system modules
copy /y "%SourceRoot%\swift\stdlib\public\Platform\ucrt.modulemap" "%UniversalCRTSdkDir%\Include\%UCRTVersion%\ucrt\module.modulemap" || (exit /b)
copy /y "%SourceRoot%\swift\stdlib\public\Platform\winsdk.modulemap" "%UniversalCRTSdkDir%\Include\%UCRTVersion%\um\module.modulemap" || (exit /b)
copy /y "%SourceRoot%\swift\stdlib\public\Platform\vcruntime.modulemap" "%VCToolsInstallDir%\include\module.modulemap" || (exit /b)
copy /y "%SourceRoot%\swift\stdlib\public\Platform\vcruntime.apinotes" "%VCToolsInstallDir%\include\vcruntime.apinotes" || (exit /b)

:: Unset SDKROOT
set SDKROOT=

:: Build Toolchain
cmake ^
  -B "%BuildRoot%\1" ^

  -C %SourceRoot%\swift\cmake\caches\Windows-x86_64.cmake ^

  -D CMAKE_BUILD_TYPE=%CMAKE_BUILD_TYPE% ^
  -D CMAKE_C_COMPILER=cl ^
  -D CMAKE_C_FLAGS="/GS- /Oy /Gw /Gy /utf-8" ^
  -D CMAKE_CXX_COMPILER=cl ^
  -D CMAKE_CXX_FLAGS="/GS- /Oy /Gw /Gy /utf-8" ^
  -D CMAKE_MT=mt ^
  -D CMAKE_EXE_LINKER_FLAGS="/INCREMENTAL:NO" ^
  -D CMAKE_SHARED_LINKER_FLAGS="/INCREMENTAL:NO" ^

  -D CMAKE_INSTALL_PREFIX="%InstallRoot%" ^

  -D LLVM_DEFAULT_TARGET_TRIPLE=x86_64-unknown-windows-msvc ^

  -D PACKAGE_VENDOR="swift.org" ^
  -D CLANG_VENDOR="swift.org" ^
  -D CLANG_VENDOR_UTI="org.swift" ^
  -D SWIFT_VENDOR="swift.org" ^
  -D LLVM_APPEND_VC_REV=NO ^
  -D LLVM_VERSION_SUFFIX="" ^

  -D SWIFT_ENABLE_EXPERIMENTAL_CONCURRENCY=YES ^
  -D SWIFT_ENABLE_EXPERIMENTAL_DISTRIBUTED=YES ^
  -D SWIFT_ENABLE_EXPERIMENTAL_DIFFERENTIABLE_PROGRAMMING=YES ^
  -D SWIFT_ENABLE_EXPERIMENTAL_STRING_PROCESSING=YES ^
  -D SWIFT_ENABLE_EXPERIMENTAL_REFLECTION=YES ^
  -D SWIFT_ENABLE_EXPERIMENTAL_OBSERVATION=YES ^

  -D LLVM_EXTERNAL_SWIFT_SOURCE_DIR="%SourceRoot%\swift" ^
  -D LLVM_EXTERNAL_CMARK_SOURCE_DIR="%SourceRoot%\cmark" ^
  -D PYTHON_HOME=%PYTHON_HOME% ^
  -D PYTHON_EXECUTABLE=%PYTHON_HOME%\python.exe ^
  -D SWIFT_PATH_TO_LIBDISPATCH_SOURCE="%SourceRoot%\swift-corelibs-libdispatch" ^
  -D SWIFT_PATH_TO_SWIFT_SYNTAX_SOURCE="%SourceRoot%\swift-syntax" ^
  -D SWIFT_PATH_TO_STRING_PROCESSING_SOURCE=%SourceRoot%\swift-experimental-string-processing ^

  -G Ninja ^
  -S llvm-project\llvm || (exit /b)
cmake --build "%BuildRoot%\1" || (exit /b)
cmake --build "%BuildRoot%\1" --target install || (exit /b)

:: Build Swift Standard Library
cmake ^
  -B %BuildRoot%\2 ^

  -C %SourceRoot%\swift\cmake\caches\Runtime-Windows-x86_64.cmake ^

  -D CMAKE_BUILD_TYPE=%CMAKE_BUILD_TYPE% ^
  -D CMAKE_C_COMPILER=%BuildRoot%/1/bin/clang-cl.exe ^
  -D CMAKE_C_FLAGS="/GS- /Oy /Gw /Gy" ^
  -D CMAKE_CXX_COMPILER=%BuildRoot%/1/bin/clang-cl.exe ^
  -D CMAKE_CXX_FLAGS="/GS- /Oy /Gw /Gy" ^
  -D CMAKE_MT=mt ^
  -D CMAKE_EXE_LINKER_FLAGS="/INCREMENTAL:NO" ^
  -D CMAKE_SHARED_LINKER_FLAGS="/INCREMENTAL:NO" ^

  -D CMAKE_INSTALL_PREFIX=%SDKInstallRoot%\usr ^

  -D LLVM_DIR=%BuildRoot%\1\lib\cmake\llvm ^
  -D SWIFT_NATIVE_SWIFT_TOOLS_PATH=%BuildRoot%\1\bin ^
  -D SWIFT_PATH_TO_LIBDISPATCH_SOURCE=%SourceRoot%\swift-corelibs-libdispatch ^
  -D SWIFT_PATH_TO_SWIFT_SYNTAX_SOURCE="%SourceRoot%\swift-syntax" ^
  -D SWIFT_PATH_TO_STRING_PROCESSING_SOURCE=%SourceRoot%\swift-experimental-string-processing ^

  -D SWIFT_ENABLE_EXPERIMENTAL_CONCURRENCY=YES ^
  -D SWIFT_ENABLE_EXPERIMENTAL_DISTRIBUTED=YES ^
  -D SWIFT_ENABLE_EXPERIMENTAL_DIFFERENTIABLE_PROGRAMMING=YES ^
  -D SWIFT_ENABLE_EXPERIMENTAL_STRING_PROCESSING=YES ^
  -D SWIFT_ENABLE_EXPERIMENTAL_REFLECTION=YES ^
  -D SWIFT_ENABLE_EXPERIMENTAL_OBSERVATION=YES ^

  -G Ninja ^
  -S %SourceRoot%\swift || (exit /b)
cmake --build %BuildRoot%\2 || (exit /b)
cmake --build %BuildRoot%\2 --target install || (exit /b)

:: Build libdispatch
cmake ^
  -B %BuildRoot%\3 ^

  -D CMAKE_BUILD_TYPE=%CMAKE_BUILD_TYPE% ^
  -D CMAKE_C_COMPILER=%BuildRoot%/1/bin/clang-cl.exe ^
  -D CMAKE_C_FLAGS="/GS- /Oy /Gw /Gy" ^
  -D CMAKE_CXX_COMPILER=%BuildRoot%/1/bin/clang-cl.exe ^
  -D CMAKE_CXX_FLAGS="/GS- /Oy /Gw /Gy" ^
  -D CMAKE_MT=mt ^
  -D CMAKE_Swift_COMPILER=%BuildRoot%/1/bin/swiftc.exe ^
  -D CMAKE_EXE_LINKER_FLAGS="/INCREMENTAL:NO" ^
  -D CMAKE_SHARED_LINKER_FLAGS="/INCREMENTAL:NO" ^

  -D CMAKE_INSTALL_PREFIX=%SDKInstallRoot%\usr ^

  -D ENABLE_SWIFT=YES ^

  -G Ninja ^
  -S %SourceRoot%\swift-corelibs-libdispatch || (exit /b)
cmake --build %BuildRoot%\3 || (exit /b)
cmake --build %BuildRoot%\3 --target install || (exit /b)

:: Build Foundation
cmake ^
  -B %BuildRoot%\4 ^

  -D CMAKE_BUILD_TYPE=%CMAKE_BUILD_TYPE% ^
  -D CMAKE_C_COMPILER=%BuildRoot%/1/bin/clang-cl.exe ^
  -D CMAKE_C_FLAGS="/GS- /Oy /Gw /Gy" ^
  -D CMAKE_CXX_COMPILER=%BuildRoot%/1/bin/clang-cl.exe ^
  -D CMAKE_CXX_FLAGS="/GS- /Oy /Gw /Gy" ^
  -D CMAKE_MT=mt ^
  -D CMAKE_Swift_COMPILER=%BuildRoot%/1/bin/swiftc.exe ^
  -D CMAKE_EXE_LINKER_FLAGS="/INCREMENTAL:NO" ^
  -D CMAKE_SHARED_LINKER_FLAGS="/INCREMENTAL:NO" ^

  -D CMAKE_INSTALL_PREFIX=%SDKInstallRoot%\usr ^

  -D CMAKE_TOOLCHAIN_FILE=%SourceRoot%\vcpkg\scripts\buildsystems\vcpkg.cmake ^
  -D VCPKG_TARGET_TRIPLET=x64-windows-static-md ^
  -D VCPKG_MANIFEST_DIR=%SourceRoot%\swift-installer-scripts\shared\Foundation ^
  -D ICU_ROOT=%BuildRoot%\Library\icu-69.1\usr ^
  -D ICU_DATA_LIBRARY_RELEASE=%BuildRoot%\Library\icu-69.1\usr\lib\sicudt69.lib ^
  -D ICU_UC_LIBRARY_RELEASE=%BuildRoot%\Library\icu-69.1\usr\lib\sicuuc69.lib ^
  -D ICU_I18N_LIBRARY_RELEASE=%BuildRoot%\Library\icu-69.1\usr\lib\sicuin69.lib ^
  -D dispatch_DIR=%BuildRoot%\3\cmake\modules ^

  -D ENABLE_TESTING=NO ^

  -G Ninja ^
  -S %SourceRoot%\swift-corelibs-foundation || (exit /b)
cmake --build %BuildRoot%\4 || (exit /b)
cmake --build %BuildRoot%\4 --target install || (exit /b)

:: Build XCTest
cmake ^
  -B %BuildRoot%\5 ^

  -D CMAKE_BUILD_TYPE=%CMAKE_BUILD_TYPE% ^
  -D CMAKE_C_COMPILER=%BuildRoot%/1/bin/clang-cl.exe ^
  -D CMAKE_C_FLAGS="/GS- /Oy /Gw /Gy" ^
  -D CMAKE_CXX_COMPILER=%BuildRoot%/1/bin/clang-cl.exe ^
  -D CMAKE_CXX_FLAGS="/GS- /Oy /Gw /Gy" ^
  -D CMAKE_MT=mt ^
  -D CMAKE_Swift_COMPILER=%BuildRoot%/1/bin/swiftc.exe ^
  -D CMAKE_EXE_LINKER_FLAGS="/INCREMENTAL:NO" ^
  -D CMAKE_SHARED_LINKER_FLAGS="/INCREMENTAL:NO" ^

  -D CMAKE_INSTALL_PREFIX=%PlatformRoot%\Developer\Library\XCTest-development\usr ^

  -D dispatch_DIR=%BuildRoot%\3\cmake\modules ^
  -D Foundation_DIR=%BuildRoot%\4\cmake\modules ^

  -D ENABLE_TESTING=NO ^

  -G Ninja ^
  -S %SourceRoot%\swift-corelibs-xctest || (exit /b)
cmake --build %BuildRoot%\5 || (exit /b)
cmake --build %BuildRoot%\5 --target install || (exit /b)

:: Build swift-system
cmake ^
  -B %BuildRoot%\6 ^

  -D BUILD_SHARED_LIBS=YES ^
  -D CMAKE_BUILD_TYPE=%CMAKE_BUILD_TYPE% ^
  -D CMAKE_C_COMPILER=%BuildRoot%/1/bin/clang-cl.exe ^
  -D CMAKE_C_FLAGS="/GS- /Oy /Gw /Gy" ^
  -D CMAKE_CXX_COMPILER=%BuildRoot%/1/bin/clang-cl.exe ^
  -D CMAKE_CXX_FLAGS="/GS- /Oy /Gw /Gy" ^
  -D CMAKE_MT=mt ^
  -D CMAKE_Swift_COMPILER=%BuildRoot%/1/bin/swiftc.exe ^
  -D CMAKE_EXE_LINKER_FLAGS="/INCREMENTAL:NO" ^
  -D CMAKE_SHARED_LINKER_FLAGS="/INCREMENTAL:NO" ^

  -D CMAKE_INSTALL_PREFIX=%InstallRoot% ^

  -G Ninja ^
  -S %SourceRoot%\swift-system || (exit /b)
cmake --build %BuildRoot%\6 || (exit /b)
cmake --build %BuildRoot%\6 --target install || (exit /b)

:: Build swift-tools-support-core
cmake ^
  -B %BuildRoot%\7 ^

  -D CMAKE_BUILD_TYPE=%CMAKE_BUILD_TYPE% ^
  -D CMAKE_C_COMPILER=%BuildRoot%/1/bin/clang-cl.exe ^
  -D CMAKE_C_FLAGS="/GS- /Oy /Gw /Gy" ^
  -D CMAKE_CXX_COMPILER=%BuildRoot%/1/bin/clang-cl.exe ^
  -D CMAKE_CXX_FLAGS="/GS- /Oy /Gw /Gy" ^
  -D CMAKE_MT=mt ^
  -D CMAKE_Swift_COMPILER=%BuildRoot%/1/bin/swiftc.exe ^
  -D CMAKE_EXE_LINKER_FLAGS="/INCREMENTAL:NO" ^
  -D CMAKE_SHARED_LINKER_FLAGS="/INCREMENTAL:NO" ^

  -D CMAKE_INSTALL_PREFIX=%InstallRoot% ^

  -D CMAKE_TOOLCHAIN_FILE=%SourceRoot%\vcpkg\scripts\buildsystems\vcpkg.cmake ^
  -D VCPKG_TARGET_TRIPLET=x64-windows-static-md ^
  -D VCPKG_MANIFEST_DIR=%SourceRoot%\swift-installer-scripts\shared\TSC ^
  -D dispatch_DIR=%BuildRoot%\3\cmake\modules ^
  -D Foundation_DIR=%BuildRoot%\4\cmake\modules ^
  -D SwiftSystem_DIR=%BuildRoot%\6\cmake\modules ^

  -G Ninja ^
  -S %SourceRoot%\swift-tools-support-core || (exit /b)
cmake --build %BuildRoot%\7 || (exit /b)
cmake --build %BuildRoot%\7 --target install || (exit /b)

:: Build llbuild
cmake ^
  -B %BuildRoot%\8 ^

  -D CMAKE_BUILD_TYPE=%CMAKE_BUILD_TYPE% ^
  -D CMAKE_C_COMPILER=%BuildRoot%/1/bin/clang-cl.exe ^
  -D CMAKE_C_FLAGS="/GS- /Oy /Gw /Gy" ^
  -D CMAKE_CXX_COMPILER=%BuildRoot%/1/bin/clang-cl.exe ^
  -D CMAKE_CXX_FLAGS="/GS- /Oy /Gw /Gy -Xclang -fno-split-cold-code" ^
  -D CMAKE_MT=mt ^
  -D CMAKE_Swift_COMPILER=%BuildRoot%/1/bin/swiftc.exe ^
  -D CMAKE_EXE_LINKER_FLAGS="/INCREMENTAL:NO" ^
  -D CMAKE_SHARED_LINKER_FLAGS="/INCREMENTAL:NO" ^

  -D CMAKE_INSTALL_PREFIX=%InstallRoot% ^

  -D LLBUILD_SUPPORT_BINDINGS=Swift ^

  -D CMAKE_TOOLCHAIN_FILE=%SourceRoot%\vcpkg\scripts\buildsystems\vcpkg.cmake ^
  -D VCPKG_TARGET_TRIPLET=x64-windows-static-md ^
  -D VCPKG_MANIFEST_DIR=%SourceRoot%\swift-installer-scripts\shared\LLBuild ^
  -D dispatch_DIR=%BuildRoot%\3\cmake\modules ^
  -D Foundation_DIR=%BuildRoot%\4\cmake\modules ^

  -G Ninja ^
  -S %SourceRoot%\llbuild || (exit /b)
cmake --build %BuildRoot%\8 || (exit /b)
cmake --build %BuildRoot%\8 --target install || (exit /b)

:: Build swift-argument-parser
cmake ^
  -B %BuildRoot%\9 ^

  -D CMAKE_BUILD_TYPE=%CMAKE_BUILD_TYPE% ^
  -D CMAKE_C_COMPILER=%BuildRoot%/1/bin/clang-cl.exe ^
  -D CMAKE_C_FLAGS="/GS- /Oy /Gw /Gy" ^
  -D CMAKE_CXX_COMPILER=%BuildRoot%/1/bin/clang-cl.exe ^
  -D CMAKE_CXX_FLAGS="/GS- /Oy /Gw /Gy" ^
  -D CMAKE_MT=mt ^
  -D CMAKE_Swift_COMPILER=%BuildRoot%/1/bin/swiftc.exe ^
  -D CMAKE_EXE_LINKER_FLAGS="/INCREMENTAL:NO" ^
  -D CMAKE_SHARED_LINKER_FLAGS="/INCREMENTAL:NO" ^

  -D CMAKE_INSTALL_PREFIX=%InstallRoot% ^

  -D dispatch_DIR=%BuildRoot%\3\cmake\modules ^
  -D Foundation_DIR=%BuildRoot%\4\cmake\modules ^
  -D XCTest_DIR=%BuildRoot%\5\cmake\modules ^

  -G Ninja ^
  -S %SourceRoot%\swift-argument-parser || (exit /b)
cmake --build %BuildRoot%\9 || (exit /b)
cmake --build %BuildRoot%\9 --target install || (exit /b)

:: Build Yams
cmake ^
  -B %BuildRoot%\10 ^

  -D BUILD_SHARED_LIBS=NO ^
  -D CMAKE_BUILD_TYPE=%CMAKE_BUILD_TYPE% ^
  -D CMAKE_C_COMPILER=%BuildRoot%/1/bin/clang-cl.exe ^
  -D CMAKE_C_FLAGS="/GS- /Oy /Gw /Gy /DYAML_DECLARE_EXPORT /DWIN32" ^
  -D CMAKE_CXX_COMPILER=%BuildRoot%/1/bin/clang-cl.exe ^
  -D CMAKE_CXX_FLAGS="/GS- /Oy /Gw /Gy" ^
  -D CMAKE_MT=mt ^
  -D CMAKE_Swift_COMPILER=%BuildRoot%/1/bin/swiftc.exe ^
  -D CMAKE_Swift_FLAGS="-Xcc -DYAML_DECLARE_EXPORT -Xcc -DWIN32" ^
  -D CMAKE_EXE_LINKER_FLAGS="/INCREMENTAL:NO" ^
  -D CMAKE_SHARED_LINKER_FLAGS="/INCREMENTAL:NO" ^

  -D CMAKE_INSTALL_PREFIX=%InstallRoot% ^

  -D dispatch_DIR=%BuildRoot%\3\cmake\modules ^
  -D Foundation_DIR=%BuildRoot%\4\cmake\modules ^
  -D XCTest_DIR=%BuildRoot%\5\cmake\modules ^

  -G Ninja ^
  -S %SourceRoot%\Yams || (exit /b)
cmake --build %BuildRoot%\10 || (exit /b)

:: Build swift-driver
cmake ^
  -B %BuildRoot%\11 ^

  -D CMAKE_BUILD_TYPE=%CMAKE_BUILD_TYPE% ^
  -D CMAKE_C_COMPILER=%BuildRoot%/1/bin/clang-cl.exe ^
  -D CMAKE_C_FLAGS="/GS- /Oy /Gw /Gy" ^
  -D CMAKE_CXX_COMPILER=%BuildRoot%/1/bin/clang-cl.exe ^
  -D CMAKE_CXX_FLAGS="/GS- /Oy /Gw /Gy" ^
  -D CMAKE_MT=mt ^
  -D CMAKE_Swift_COMPILER=%BuildRoot%/1/bin/swiftc.exe ^
  -D CMAKE_EXE_LINKER_FLAGS="/INCREMENTAL:NO" ^
  -D CMAKE_SHARED_LINKER_FLAGS="/INCREMENTAL:NO" ^

  -D CMAKE_INSTALL_PREFIX=%InstallRoot% ^

  -D dispatch_DIR=%BuildRoot%\3\cmake\modules ^
  -D Foundation_DIR=%BuildRoot%\4\cmake\modules ^
  -D XCTest_DIR=%BuildRoot%\5\cmake\modules ^
  -D SwiftSystem_DIR=%BuildRoot%\6\cmake\modules ^
  -D TSC_DIR=%BuildRoot%\7\cmake\modules ^
  -D LLBuild_DIR=%BuildRoot%\8\cmake\modules ^
  -D ArgumentParser_DIR=%BuildRoot%\9\cmake\modules ^
  -D Yams_DIR=%BuildRoot%\10\cmake\modules ^

  -G Ninja ^
  -S %SourceRoot%\swift-driver || (exit /b)
cmake --build %BuildRoot%\11 || (exit /b)
cmake --build %BuildRoot%\11 --target install || (exit /b)

:: Build swift-crypto
cmake ^
  -B %BuildRoot%\12 ^

  -D BUILD_SHARED_LIBS=NO ^
  -D CMAKE_BUILD_TYPE=%CMAKE_BUILD_TYPE% ^
  -D CMAKE_C_COMPILER=%BuildRoot%/1/bin/clang-cl.exe ^
  -D CMAKE_C_FLAGS="/GS- /Oy /Gw /Gy" ^
  -D CMAKE_CXX_COMPILER=%BuildRoot%/1/bin/clang-cl.exe ^
  -D CMAKE_CXX_FLAGS="/GS- /Oy /Gw /Gy" ^
  -D CMAKE_MT=mt ^
  -D CMAKE_Swift_COMPILER=%BuildRoot%/1/bin/swiftc.exe ^
  -D CMAKE_EXE_LINKER_FLAGS="/INCREMENTAL:NO" ^
  -D CMAKE_SHARED_LINKER_FLAGS="/INCREMENTAL:NO" ^

  -D CMAKE_INSTALL_PREFIX=%InstallRoot% ^

  -D dispatch_DIR=%BuildRoot%\3\cmake\modules ^
  -D Foundation_DIR=%BuildRoot%\4\cmake\modules ^

  -G Ninja ^
  -S %SourceRoot%\swift-crypto || (exit /b)
cmake --build %BuildRoot%\12 || (exit /b)

:: Build swift-collections
cmake ^
  -B %BuildRoot%\13 ^

  -D CMAKE_BUILD_TYPE=%CMAKE_BUILD_TYPE% ^
  -D CMAKE_C_COMPILER=%BuildRoot%/1/bin/clang-cl.exe ^
  -D CMAKE_C_FLAGS="/GS- /Oy /Gw /Gy" ^
  -D CMAKE_CXX_COMPILER=%BuildRoot%/1/bin/clang-cl.exe ^
  -D CMAKE_CXX_FLAGS="/GS- /Oy /Gw /Gy" ^
  -D CMAKE_MT=mt ^
  -D CMAKE_Swift_COMPILER=%BuildRoot%/1/bin/swiftc.exe ^
  -D CMAKE_EXE_LINKER_FLAGS="/INCREMENTAL:NO" ^
  -D CMAKE_SHARED_LINKER_FLAGS="/INCREMENTAL:NO" ^

  -D CMAKE_INSTALL_PREFIX=%InstallRoot% ^

  -G Ninja ^
  -S %SourceRoot%\swift-collections || (exit /b)
cmake --build %BuildRoot%\13 || (exit /b)
cmake --build %BuildRoot%\13 --target install || (exit /b)

:: Build swift-asn1
 cmake ^
   -B %BuildRoot%\14 ^

   -D BUILD_SHARED_LIBS=NO ^
   -D CMAKE_BUILD_TYPE=%CMAKE_BUILD_TYPE% ^
   -D CMAKE_C_COMPILER=%BuildRoot%/1/bin/clang-cl.exe ^
   -D CMAKE_C_FLAGS="/GS- /Oy /Gw /Gy" ^
   -D CMAKE_CXX_COMPILER=%BuildRoot%/1/bin/clang-cl.exe ^
   -D CMAKE_CXX_FLAGS="/GS- /Oy /Gw /Gy" ^
   -D CMAKE_MT=mt ^
   -D CMAKE_Swift_COMPILER=%BuildRoot%/1/bin/swiftc.exe ^
   -D CMAKE_EXE_LINKER_FLAGS="/INCREMENTAL:NO" ^
   -D CMAKE_SHARED_LINKER_FLAGS="/INCREMENTAL:NO" ^

   -D CMAKE_INSTALL_PREFIX=%InstallRoot% ^

   -D dispatch_DIR=%BuildRoot%\3\cmake\modules ^
   -D Foundation_DIR=%BuildRoot%\4\cmake\modules ^

   -G Ninja ^
   -S %SourceRoot%\swift-asn1 || (exit /b)
 cmake --build %BuildRoot%\14 || (exit /b)
 cmake --build %BuildRoot%\14 --target install || (exit /b)

:: Build swift-certificates
 cmake ^
   -B %BuildRoot%\15 ^

   -D BUILD_SHARED_LIBS=NO ^
   -D CMAKE_BUILD_TYPE=%CMAKE_BUILD_TYPE% ^
   -D CMAKE_C_COMPILER=%BuildRoot%/1/bin/clang-cl.exe ^
   -D CMAKE_C_FLAGS="/GS- /Oy /Gw /Gy" ^
   -D CMAKE_CXX_COMPILER=%BuildRoot%/1/bin/clang-cl.exe ^
   -D CMAKE_CXX_FLAGS="/GS- /Oy /Gw /Gy" ^
   -D CMAKE_MT=mt ^
   -D CMAKE_Swift_COMPILER=%BuildRoot%/1/bin/swiftc.exe ^
   -D CMAKE_EXE_LINKER_FLAGS="/INCREMENTAL:NO" ^
   -D CMAKE_SHARED_LINKER_FLAGS="/INCREMENTAL:NO" ^

   -D CMAKE_INSTALL_PREFIX=%InstallRoot% ^

   -D dispatch_DIR=%BuildRoot%\3\cmake\modules ^
   -D Foundation_DIR=%BuildRoot%\4\cmake\modules ^
   -D SwiftCrypto_DIR=%BuildRoot%\12\cmake\modules ^
   -D SwiftASN1_DIR=%BuildRoot%\14\cmake\modules ^

   -G Ninja ^
   -S %SourceRoot%\swift-certificates || (exit /b)
 cmake --build %BuildRoot%\15 || (exit /b)
 cmake --build %BuildRoot%\15 --target install || (exit /b)

:: Build swift-package-manager
cmake ^
  -B %BuildRoot%\16 ^

  -D CMAKE_BUILD_TYPE=%CMAKE_BUILD_TYPE% ^
  -D CMAKE_C_COMPILER=%BuildRoot%/1/bin/clang-cl.exe ^
  -D CMAKE_C_FLAGS="/GS- /Oy /Gw /Gy" ^
  -D CMAKE_CXX_COMPILER=%BuildRoot%/1/bin/clang-cl.exe ^
  -D CMAKE_CXX_FLAGS="/GS- /Oy /Gw /Gy" ^
  -D CMAKE_MT=mt ^
  -D CMAKE_Swift_COMPILER=%BuildRoot%/1/bin/swiftc.exe ^
  -D CMAKE_EXE_LINKER_FLAGS="/INCREMENTAL:NO" ^
  -D CMAKE_SHARED_LINKER_FLAGS="/INCREMENTAL:NO" ^

  -D CMAKE_INSTALL_PREFIX=%InstallRoot% ^

  -D dispatch_DIR=%BuildRoot%\3\cmake\modules ^
  -D Foundation_DIR=%BuildRoot%\4\cmake\modules ^
  -D SwiftSystem_DIR=%BuildRoot%\6\cmake\modules ^
  -D TSC_DIR=%BuildRoot%\7\cmake\modules ^
  -D LLBuild_DIR=%BuildRoot%\8\cmake\modules ^
  -D ArgumentParser_DIR=%BuildRoot%\9\cmake\modules ^
  -D SwiftDriver_DIR=%BuildRoot%\11\cmake\modules ^
  -D SwiftCrypto_DIR=%BuildRoot%\12\cmake\modules ^
  -D SwiftCollections_DIR=%BuildRoot%\13\cmake\modules ^
  -D SwiftASN1_DIR=%BuildRoot%\14\cmake\modules ^
  -D SwiftCertificates_DIR=%BuildRoot%\15\cmake\modules ^
  -D SQLite3_INCLUDE_DIR=%BuildRoot%\Library\sqlite-3.36.0\usr\include ^
  -D SQLite3_LIBRARY=%BuildRoot%\Library\sqlite-3.36.0\usr\lib\SQLite3.lib ^

  -G Ninja ^
  -S %SourceRoot%\swiftpm || (exit /b)
cmake --build %BuildRoot%\16 || (exit /b)
cmake --build %BuildRoot%\16 --target install || (exit /b)

:: Build IndexStoreDB
cmake ^
  -B %BuildRoot%\17 ^

  -D BUILD_SHARED_LIBS=NO ^
  -D CMAKE_BUILD_TYPE=%CMAKE_BUILD_TYPE% ^
  -D CMAKE_C_COMPILER=%BuildRoot%/1/bin/clang-cl.exe ^
  -D CMAKE_C_FLAGS="/GS- /Oy /Gw /Gy" ^
  -D CMAKE_CXX_COMPILER=%BuildRoot%/1/bin/clang-cl.exe ^
  -D CMAKE_CXX_FLAGS="/GS- /Oy /Gw /Gy -Xclang -fno-split-cold-code" ^
  -D CMAKE_MT=mt ^
  -D CMAKE_Swift_COMPILER=%BuildRoot%/1/bin/swiftc.exe ^
  -D CMAKE_EXE_LINKER_FLAGS="/INCREMENTAL:NO" ^
  -D CMAKE_SHARED_LINKER_FLAGS="/INCREMENTAL:NO" ^

  -D CMAKE_INSTALL_PREFIX=%InstallRoot% ^

  -D dispatch_DIR=%BuildRoot%\3\cmake\modules ^
  -D Foundation_DIR=%BuildRoot%\4\cmake\modules ^

  -G Ninja ^
  -S %SourceRoot%\indexstore-db || (exit /b)
cmake --build %BuildRoot%\17 || (exit /b)

:: Build swift-syntax
cmake ^
  -B %BuildRoot%\18 ^

  -D CMAKE_BUILD_TYPE=%CMAKE_BUILD_TYPE% ^
  -D CMAKE_C_COMPILER=%BuildRoot%/1/bin/clang-cl.exe ^
  -D CMAKE_C_FLAGS="/GS- /Oy /Gw /Gy" ^
  -D CMAKE_MT=mt ^
  -D CMAKE_Swift_COMPILER=%BuildRoot%/1/bin/swiftc.exe ^
  -D CMAKE_EXE_LINKER_FLAGS="/INCREMENTAL:NO" ^
  -D CMAKE_SHARED_LINKER_FLAGS="/INCREMENTAL:NO" ^

  -D CMAKE_INSTALL_PREFIX=%InstallRoot% ^

  -G Ninja ^
  -S %SourceRoot%\swift-syntax || (exit /b)
cmake --build %BuildRoot%\18 || (exit /b)
cmake --build %BuildRoot%\18 --target install || (exit /b)

:: Build SourceKit-LSP
cmake ^
  -B %BuildRoot%\19 ^

  -D BUILD_SHARED_LIBS=YES ^
  -D CMAKE_BUILD_TYPE=%CMAKE_BUILD_TYPE% ^
  -D CMAKE_C_COMPILER=%BuildRoot%/1/bin/clang-cl.exe ^
  -D CMAKE_C_FLAGS="/GS- /Oy /Gw /Gy" ^
  -D CMAKE_CXX_COMPILER=%BuildRoot%/1/bin/clang-cl.exe ^
  -D CMAKE_CXX_FLAGS="/GS- /Oy /Gw /Gy -Xclang -fno-split-cold-code" ^
  -D CMAKE_MT=mt ^
  -D CMAKE_Swift_COMPILER=%BuildRoot%/1/bin/swiftc.exe ^
  -D CMAKE_EXE_LINKER_FLAGS="/INCREMENTAL:NO" ^
  -D CMAKE_SHARED_LINKER_FLAGS="/INCREMENTAL:NO" ^

  -D CMAKE_INSTALL_PREFIX=%InstallRoot% ^

  -D dispatch_DIR=%BuildRoot%\3\cmake\modules ^
  -D Foundation_DIR=%BuildRoot%\4\cmake\modules ^
  -D SwiftSystem_DIR=%BuildRoot%\6\cmake\modules ^
  -D TSC_DIR=%BuildRoot%\7\cmake\modules ^
  -D LLBuild_DIR=%BuildRoot%\8\cmake\modules ^
  -D ArgumentParser_DIR=%BuildRoot%\9\cmake\modules ^
  -D Yams_DIR=%BuildRoot%\10\cmake\modules ^
  -D SwiftPM_DIR=%BuildRoot%\16\cmake\modules ^
  -D IndexStoreDB_DIR=%BuildRoot%\17\cmake\modules ^
  -D SwiftCollections_DIR=%BuildRoot%\13\cmake\modules ^
  -D SwiftSyntax_DIR=%BuildRoot%\18\cmake\modules ^

  -G Ninja ^
  -S %SourceRoot%\sourcekit-lsp || (exit /b)
cmake --build %BuildRoot%\19 || (exit /b)
cmake --build %BuildRoot%\19 --target install || (exit /b)

:: Create Configuration Files
python -c "import plistlib; print(str(plistlib.dumps({ 'DefaultProperties': { 'DEFAULT_USE_RUNTIME': 'MD' } }), encoding='utf-8'))" > %SDKInstallRoot%\SDKSettings.plist
:: TODO(compnerd) match the XCTest installation name
python -c "import plistlib; print(str(plistlib.dumps({ 'DefaultProperties': { 'XCTEST_VERSION': 'development' } }), encoding='utf-8'))" > %PlatformRoot%\Info.plist

IF NOT "%SKIP_PACKAGING%"=="1" call :PackageToolchain

:: TODO(compnerd) test LLVM

SET SKIP_TEST=0
FOR %%T IN (%SKIP_TESTS%) DO (IF /I %%T==swift SET SKIP_TEST=1)
IF "%SKIP_TEST%"=="0" call :TestSwift

SET SKIP_TEST=0
FOR %%T IN (%SKIP_TESTS%) DO (IF /I %%T==dispatch SET SKIP_TEST=1)
IF "%SKIP_TEST%"=="0" call :TestDispatch

SET SKIP_TEST=0
FOR %%T IN (%SKIP_TESTS%) DO (IF /I %%T==foundation SET SKIP_TEST=1)
IF "%SKIP_TEST%"=="0" call :TestFoundation

SET SKIP_TEST=0
FOR %%T IN (%SKIP_TESTS%) DO (IF /I %%T==xctest SET SKIP_TEST=1)
IF "%SKIP_TEST%"=="0" call :TestXCTest

:: Clean up the module cache
rd /s /q %LocalAppData%\clang\ModuleCache

goto :end
endlocal

:CloneRepositories
setlocal enableextensions enabledelayedexpansion

if defined SKIP_UPDATE_CHECKOUT goto :eof

if defined REPO_SCHEME set "args=--scheme %REPO_SCHEME%"

:: Ensure that we have the files in the original line endings, the swift tests
:: depend on this being the case.
git -C "%SourceRoot%\swift" config --local core.autocrlf input
git -C "%SourceRoot%\swift" checkout-index --force --all

set "args=%args% --skip-repository swift"
set "args=%args% --skip-repository ninja"
set "args=%args% --skip-repository swift-integration-tests"
set "args=%args% --skip-repository swift-stress-tester"
set "args=%args% --skip-repository swift-xcode-playground-support"

call "%SourceRoot%\swift\utils\update-checkout.cmd" %args% --clone --skip-history --github-comment "%ghprbCommentBody%"

goto :eof
endlocal

:CloneICU
:: TODO(stevapple) move ICU to update-checkout
setlocal enableextensions enabledelayedexpansion

:: FIXME(compnerd) avoid the fresh clone
rd /s /q icu
git clone --quiet --no-tags --depth 1 --branch maint/maint-69 https://github.com/unicode-org/icu

goto :eof
endlocal

:TestSwift
setlocal enableextensions enabledelayedexpansion

:: Test Swift
:: TODO(compnerd) make lit adjust the path properly
path %BuildRoot%\3;%BuildRoot%\1\bin;%PATH%;%SystemDrive%\Program Files\Git\usr\bin
cmake --build %BuildRoot%\1 --target check-swift || (exit /b)

goto :eof
endlocal

:TestDispatch
setlocal enableextensions enabledelayedexpansion

:: Test dispatch
cmake --build %BuildRoot%\3 --target ExperimentalTest || (exit /b)

goto :eof
endlocal

:TestFoundation
setlocal enableextensions enabledelayedexpansion

:: NOTE(compnerd) update the path *before* the build because the tests are
:: executed to shard the test suite.
path %BuildRoot%\5;%BuildRoot%\4\bin;%BuildRoot%\3;%BuildRoot%\1\bin;%PATH%;%SystemDrive%\Program Files\Git\usr\bin

:: Rebuild Foundation (w/ testing)
cmake ^
  -B %BuildRoot%\4 ^

  -D CMAKE_BUILD_TYPE=%CMAKE_BUILD_TYPE% ^
  -D CMAKE_C_COMPILER=%BuildRoot%/1/bin/clang-cl.exe ^
  -D CMAKE_C_FLAGS="/GS- /Oy /Gw /Gy" ^
  -D CMAKE_CXX_COMPILER=%BuildRoot%/1/bin/clang-cl.exe ^
  -D CMAKE_CXX_FLAGS="/GS- /Oy /Gw /Gy" ^
  -D CMAKE_MT=mt ^
  -D CMAKE_Swift_COMPILER=%BuildRoot%/1/bin/swiftc.exe ^
  -D CMAKE_EXE_LINKER_FLAGS="/INCREMENTAL:NO" ^
  -D CMAKE_SHARED_LINKER_FLAGS="/INCREMENTAL:NO" ^

  -D CMAKE_INSTALL_PREFIX=%SDKInstallRoot%\usr ^

  -D CMAKE_TOOLCHAIN_FILE=%SourceRoot%\vcpkg\scripts\buildsystems\vcpkg.cmake ^
  -D VCPKG_TARGET_TRIPLET=x64-windows-static-md ^
  -D VCPKG_MANIFEST_DIR=%SourceRoot%\swift-installer-scripts\shared\Foundation ^
  -D ICU_ROOT=%BuildRoot%\Library\icu-69.1\usr ^
  -D ICU_DATA_LIBRARY_RELEASE=%BuildRoot%\Library\icu-69.1\usr\lib\sicudt69.lib ^
  -D ICU_I18N_LIBRARY_RELEASE=%BuildRoot%\Library\icu-69.1\usr\lib\sicuin69.lib ^
  -D ICU_UC_LIBRARY_RELEASE=%BuildRoot%\Library\icu-69.1\usr\lib\sicuuc69.lib ^
  -D dispatch_DIR=%BuildRoot%\3\cmake\modules ^
  -D XCTest_DIR=%BuildRoot%\5\cmake\modules ^

  -D ENABLE_TESTING=YES ^

  -G Ninja ^
  -S %SourceRoot%\swift-corelibs-foundation || (exit /b)
cmake --build %BuildRoot%\4 || (exit /b)

:: Test Foundation
set CTEST_OUTPUT_ON_FAILURE=1
cmake --build %BuildRoot%\4 --target test || (exit /b)

goto :eof
endlocal

:TestXCTest
setlocal enableextensions enabledelayedexpansion

:: NOTE(compnerd) update the path *before* the build because the tests are
:: executed to shard the test suite.
path %BuildRoot%\5;%BuildRoot%\4\bin;%BuildRoot%\3;%BuildRoot%\1\bin;%PATH%;%SystemDrive%\Program Files\Git\usr\bin

:: Rebuild XCTest (w/ testing)
cmake ^
  -B %BuildRoot%\5 ^

  -D CMAKE_BUILD_TYPE=%CMAKE_BUILD_TYPE% ^
  -D CMAKE_C_COMPILER=%BuildRoot%/1/bin/clang-cl.exe ^
  -D CMAKE_C_FLAGS="/GS- /Oy /Gw /Gy" ^
  -D CMAKE_CXX_COMPILER=%BuildRoot%/1/bin/clang-cl.exe ^
  -D CMAKE_CXX_FLAGS="/GS- /Oy /Gw /Gy" ^
  -D CMAKE_MT=mt ^
  -D CMAKE_Swift_COMPILER=%BuildRoot%/1/bin/swiftc.exe ^
  -D CMAKE_EXE_LINKER_FLAGS="/INCREMENTAL:NO" ^
  -D CMAKE_SHARED_LINKER_FLAGS="/INCREMENTAL:NO" ^

  -D CMAKE_INSTALL_PREFIX=%PlatformRoot%\Developer\Library\XCTest-development\usr ^

  -D dispatch_DIR=%BuildRoot%\3\cmake\modules ^
  -D Foundation_DIR=%BuildRoot%\4\cmake\modules ^

  -D ENABLE_TESTING=YES ^
  -D XCTEST_PATH_TO_LIBDISPATCH_BUILD=%BuildRoot%\3 ^
  -D XCTEST_PATH_TO_LIBDISPATCH_SOURCE=%SourceRoot%\swift-corelibs-libdispatch ^
  -D XCTEST_PATH_TO_FOUNDATION_BUILD=%BuildRoot%\4 ^

  -G Ninja ^
  -S %SourceRoot%\swift-corelibs-xctest || (exit /b)
cmake --build %BuildRoot%\5 || (exit /b)

:: Test XCTest
cmake --build %BuildRoot%\5 --target check-xctest || (exit /b)

goto :eof
endlocal

:PackageToolchain
setlocal enableextensions enabledelayedexpansion

:: Package toolchain.msi
msbuild %SourceRoot%\swift-installer-scripts\platforms\Windows\toolchain.wixproj ^
  -p:RunWixToolsOutOfProc=true ^
  -p:OutputPath=%PackageRoot%\toolchain\ ^
  -p:IntermediateOutputPath=%PackageRoot%\toolchain\ ^
  -p:DEVTOOLS_ROOT=%BuildRoot%\Library\Developer\Toolchains\unknown-Asserts-development.xctoolchain ^
  -p:TOOLCHAIN_ROOT=%BuildRoot%\Library\Developer\Toolchains\unknown-Asserts-development.xctoolchain
:: TODO(compnerd) actually perform the code-signing
:: signtool sign /f Apple_CodeSign.pfx /p Apple_CodeSign_Password /tr http://timestamp.digicert.com /fd sha256 %PackageRoot%\toolchain\toolchain.msi

:: Package sdk.msi
msbuild %SourceRoot%\swift-installer-scripts\platforms\Windows\CustomActions\SwiftInstaller\SwiftInstaller.vcxproj -t:restore
msbuild %SourceRoot%\swift-installer-scripts\platforms\Windows\sdk.wixproj ^
  -p:RunWixToolsOutOfProc=true ^
  -p:OutputPath=%PackageRoot%\sdk\ ^
  -p:IntermediateOutputPath=%PackageRoot%\sdk\ ^
  -p:PLATFORM_ROOT=%PlatformRoot%\ ^
  -p:SDK_ROOT=%SDKInstallRoot%\ ^
  -p:SWIFT_SOURCE_DIR=%SourceRoot%\swift\ ^
  -p:PlatformToolset=v142
:: TODO(compnerd) actually perform the code-signing
:: signtool sign /f Apple_CodeSign.pfx /p Apple_CodeSign_Password /tr http://timestamp.digicert.com /fd sha256 %PackageRoot%\sdk\sdk.msi

:: Package runtime.msi
msbuild %SourceRoot%\swift-installer-scripts\platforms\Windows\runtime.wixproj ^
  -p:RunWixToolsOutOfProc=true ^
  -p:OutputPath=%PackageRoot%\runtime\ ^
  -p:IntermediateOutputPath=%PackageRoot%\runtime\ ^
  -p:SDK_ROOT=%SDKInstallRoot%\
:: TODO(compnerd) actually perform the code-signing
:: signtool sign /f Apple_CodeSign.pfx /p Apple_CodeSign_Password /tr http://timestamp.digicert.com /fd sha256 %PackageRoot%\runtime\runtime.msi

:: Package devtools.msi
msbuild %SourceRoot%\swift-installer-scripts\platforms\Windows\devtools.wixproj ^
  -p:RunWixToolsOutOfProc=true ^
  -p:OutputPath=%PackageRoot%\devtools\ ^
  -p:IntermediateOutputPath=%PackageRoot%\devtools\ ^
  -p:DEVTOOLS_ROOT=%BuildRoot%\Library\Developer\Toolchains\unknown-Asserts-development.xctoolchain
:: TODO(compnerd) actually perform the code-signing
:: signtool sign /f Apple_CodeSign.pfx /p Apple_CodeSign_Password /tr http://timestamp.digicert.com /fd sha256 %PackageRoot%\devtools\devtools.msi

:: Collate MSIs
move %PackageRoot%\toolchain\toolchain.msi %PackageRoot% || (exit /b)
move %PackageRoot%\sdk\sdk.msi %PackageRoot% || (exit /b)
move %PackageRoot%\runtime\runtime.msi %PackageRoot% || (exit /b)
move %PackageRoot%\devtools\devtools.msi %PackageRoot% || (exit /b)

:: Build Installer
msbuild %SourceRoot%\swift-installer-scripts\platforms\Windows\installer.wixproj ^
  -p:RunWixToolsOutOfProc=true ^
  -p:OutputPath=%PackageRoot%\installer\ ^
  -p:IntermediateOutputPath=%PackageRoot%\installer\ ^
  -p:MSI_LOCATION=%PackageRoot%\
:: TODO(compnerd) actually perform the code-signing
:: signtool sign /f Apple_CodeSign.pfx /p Apple_CodeSign_Password /tr http://timestamp.digicert.com /fd sha256 %PackageRoot%\installer\installer.exe

:: Stage Artifacts
md %BuildRoot%\artifacts

:: Redistributable libraries for developers
move %PackageRoot%\runtime.msi %BuildRoot%\artifacts || (exit /b)
:: Toolchain
move %PackageRoot%\toolchain.msi %BuildRoot%\artifacts || (exit /b)
:: SDK
move %PackageRoot%\sdk.msi %BuildRoot%\artifacts || (exit /b)
:: Installer
move %PackageRoot%\installer\installer.exe %BuildRoot%\artifacts || (exit /b)

goto :eof
endlocal

:end
