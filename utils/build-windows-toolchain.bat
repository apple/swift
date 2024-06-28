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

:: Work around CI invocation in vsdevcmd
:: The build relies on build.ps1, which should not be called in a vs dev cmd
if "%VSCMD_ARG_HOST_ARCH%"=="" goto Start
echo This script should not be called in a vs developer command prompt
echo Reeinvoking script in the default environment
set TEMP=%~dp0..\..\tmp
mkdir %TEMP% 2>&1 1>nul
echo set PYTHON_HOME=%PYTHON_HOME%> %TEMP%\call-build.cmd
echo set SKIP_TESTS=%SKIP_TESTS%>> %TEMP%\call-build.cmd
echo set SKIP_PACKAGING=%SKIP_PACKAGING%>> %TEMP%\call-build.cmd
echo set SKIP_UPDATE_CHECKOUT=%SKIP_UPDATE_CHECKOUT%>> %TEMP%\call-build.cmd
echo set REPO_SCHEME=%REPO_SCHEME%>> %TEMP%\call-build.cmd
echo "%~f0">> %TEMP%\call-build.cmd
start /i /b /wait cmd.exe /env=default /c "%TEMP%\call-build.cmd"
set ec=%errorlevel%
del %TEMP%\call-build.cmd
exit /b %ec%

:Start

:: Work around CI invocation with PYTHON_HOME containing double quotes
if defined PYTHON_HOME path !Path!;!PYTHON_HOME:"=!

:: Identify the SourceRoot
:: Normalize the SourceRoot to make it easier to read the output.
cd %~dp0\..\..
set SourceRoot=%CD%

:: Identify the BuildRoot
set BuildRoot=%SourceRoot%\build

md %BuildRoot%
subst T: /d
subst T: %BuildRoot% || (exit /b 1)
set BuildRoot=T:

:: Identify the PackageRoot
set PackageRoot=%BuildRoot%\artifacts

md %PackageRoot%

:: Setup temporary directories
md %BuildRoot%\tmp
set TEMP=%BuildRoot%\tmp
set TMP=%BuildRoot%\tmp
set TMPDIR=%BuildRoot%\tmp

set NINJA_STATUS=[%%f/%%t][%%p][%%es] 

:: Build the -Test argument, if any, by subtracting skipped tests
set TestArg=-Test swift,dispatch,xctest,
for %%I in (%SKIP_TESTS%) do (call set TestArg=%%TestArg:%%I,=%%)
if "%TestArg:~-1%"=="," (set TestArg=%TestArg:~0,-1%) else (set TestArg= )

:: Build the -SkipPackaging argument, if any
set SkipPackagingArg=-SkipPackaging
if not "%SKIP_PACKAGING%"=="1" set "SkipPackagingArg= "

call :CloneRepositories || (exit /b 1)

:: We only have write access to BuildRoot, so use that as the image root.
powershell.exe -ExecutionPolicy RemoteSigned -File %~dp0build.ps1 ^
  -SourceCache %SourceRoot% ^
  -BinaryCache %BuildRoot% ^
  -ImageRoot %BuildRoot% ^
  %SkipPackagingArg% ^
  %TestArg% ^
  -Stage %PackageRoot% ^
  -Summary || (exit /b 1)

:: Clean up the module cache
rd /s /q %LocalAppData%\clang\ModuleCache

goto :end
endlocal

:CloneRepositories
setlocal enableextensions enabledelayedexpansion

if defined SKIP_UPDATE_CHECKOUT goto :eof

if defined REPO_SCHEME set "args=--scheme %REPO_SCHEME%"

:: Always enable symbolic links
git config --global core.symlink true

:: Ensure that we have the files in the original line endings, the swift tests
:: depend on this being the case.
rem git -C "%SourceRoot%\swift" config --local core.autocrlf input
rem git -C "%SourceRoot%\swift" checkout-index --force --all

set "args=%args% --skip-repository swift"
set "args=%args% --skip-repository ninja"
set "args=%args% --skip-repository swift-integration-tests"
set "args=%args% --skip-repository swift-stress-tester"
set "args=%args% --skip-repository swift-xcode-playground-support"

call "%SourceRoot%\swift\utils\update-checkout.cmd" %args% --clone --skip-history --reset-to-remote --github-comment "%ghprbCommentBody%"

goto :eof
endlocal

:end
