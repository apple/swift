# Copyright 2020 Saleem Abdulrasool <compnerd@compnerd.org>
# Copyright 2023 Tristan Labelle <tristan@thebrowser.company>

<#
.SYNOPSIS
Builds the Swift toolchain, installers, and optionally runs tests.

.DESCRIPTION
This script performs various steps associated with building the Swift toolchain:

- Builds the redistributable, SDK, devtools and toolchain binaries and files
- Builds the msi's and installer executable
- Creates a mock installation under S:\Program Files and S:\Library for local toolchain use
- Optionally runs tests for supported projects
- Optionally stages build artifacts for CI

.PARAMETER SourceCache
The path to a directory where projects contributing to the Swift.
toolchain have been cloned.

.PARAMETER BinaryCache
The path to a directory where to write build system files and outputs.

.PARAMETER ImageRoot
The path to a directory that mimics a file system image root,
under which "Library" and "Program Files" subdirectories will be created
with the files installed by CMake.

.PARAMETER CDebugFormat
The debug information format for C/C++ code: dwarf or codeview.

.PARAMETER SwiftDebugFormat
The debug information format for Swift code: dwarf or codeview.

.PARAMETER AndroidAPILevel
The API Level to target when building the Android SDKs

.PARAMETER Android
When set, build android SDKs.

.PARAMETER AndroidSDKs
An array of architectures for which the Android Swift SDK should be built.

.PARAMETER WindowsSDKs
An array of architectures for which the Windows Swift SDK should be built.

.PARAMETER ProductVersion
The product version to be used when building the installer.
Supports semantic version strings.

.PARAMETER PinnedBuild
The toolchain snapshot to build the early components with.

.PARAMETER PinnedSHA256
The SHA256 for the pinned toolchain.

.PARAMETER AndroidNDKVersion
The version number of the Android NDK to be used.

.PARAMETER WinSDKVersion
The version number of the Windows SDK to be used.
Overrides the value resolved by the Visual Studio command prompt.
If no such Windows SDK is installed, it will be downloaded from nuget.

.PARAMETER SkipBuild
If set, does not run the build phase.

.PARAMETER SkipPackaging
If set, skips building the msi's and installer

.PARAMETER DebugInfo
If set, debug information will be generated for the builds.

.PARAMETER EnableCaching
If true, use `sccache` to cache the build rules.

.PARAMETER Clean
If true, clean non-compiler builds while building.

.PARAMETER Test
An array of names of projects to run tests for.
'*' runs all tests

.PARAMETER Stage
The path to a directory where built msi's and the installer executable should be staged (for CI).

.PARAMETER BuildTo
The name of a build step after which the script should terminate.
For example: -BuildTo ToolsSupportCore

.PARAMETER ToBatch
When set, runs the script in a special mode which outputs a listing of command invocations
in batch file format instead of executing them.

.PARAMETER HostArchName
The architecture where the toolchain will execute.

.EXAMPLE
PS> .\Build.ps1

.EXAMPLE
PS> .\Build.ps1 -WindowsSDKs x64 -ProductVersion 1.2.3 -Test foundation,xctest
#>
[CmdletBinding(PositionalBinding = $false)]
param(
  [string] $SourceCache = "S:\SourceCache",
  [string] $BinaryCache = "S:\b",
  [string] $ImageRoot = "S:",
  [string] $CDebugFormat = "dwarf",
  [string] $SwiftDebugFormat = "dwarf",
  [string] $AndroidAPILevel = 28,
  [string[]] $AndroidSDKs = @(),
  [string[]] $WindowsSDKs = @("X64","X86","Arm64"),
  [string] $ProductVersion = "0.0.0",
  [string] $PinnedBuild = "",
  [string] $PinnedSHA256 = "",
  [string] $PythonVersion = "3.9.10",
  [string] $AndroidNDKVersion = "r26b",
  [string] $WinSDKVersion = "",
  [switch] $Android = $false,
  [switch] $SkipBuild = $false,
  [switch] $SkipRedistInstall = $false,
  [switch] $SkipPackaging = $false,
  [string[]] $Test = @(),
  [string] $Stage = "",
  [string] $BuildTo = "",
  [string] $HostArchName = $(if ($env:PROCESSOR_ARCHITEW6432 -ne $null) { "$env:PROCESSOR_ARCHITEW6432" } else { "$env:PROCESSOR_ARCHITECTURE" }),
  [switch] $Clean,
  [switch] $DebugInfo,
  [switch] $EnableCaching,
  [switch] $Summary,
  [switch] $ToBatch
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version 3.0

# Avoid being run in a "Developer" shell since this script launches its own sub-shells targeting
# different architectures, and these variables cause confusion.
if ($null -ne $env:VSCMD_ARG_HOST_ARCH -or $null -ne $env:VSCMD_ARG_TGT_ARCH) {
  throw "At least one of VSCMD_ARG_HOST_ARCH and VSCMD_ARG_TGT_ARCH is set, which is incompatible with this script. Likely need to run outside of a Developer shell."
}

# Prevent elsewhere-installed swift modules from confusing our builds.
$env:SDKROOT = ""

$BuildArchName = $env:PROCESSOR_ARCHITEW6432
if ($null -eq $BuildArchName) { $BuildArchName = $env:PROCESSOR_ARCHITECTURE }

if ($PinnedBuild -eq "") {
  switch ($BuildArchName) {
    "AMD64" {
      $PinnedBuild = "https://download.swift.org/swift-5.10.1-release/windows10/swift-5.10.1-RELEASE/swift-5.10.1-RELEASE-windows10.exe"
      $PinnedSHA256 = "3027762138ACFA1BBE3050FF6613BBE754332E84C9EFA5C23984646009297286"
      $PinnedVersion = "5.10.1"
    }
    "ARM64" {
      # TODO(hjyamauchi) once we have an arm64 release, fill in PinnedBuild and PinnedSHA256.
      throw "Missing pinned toolchain for ARM64"
    }
    default { throw "Unsupported processor architecture" }
  }
}

# Store the revision zero variant of the Windows SDK version (no-op if unspecified)
$WindowsSDKMajorMinorBuildMatch = [Regex]::Match($WinSDKVersion, "^\d+\.\d+\.\d+")
$WinSDKVersionRevisionZero = if ($WindowsSDKMajorMinorBuildMatch.Success) { $WindowsSDKMajorMinorBuildMatch.Value + ".0" } else { "" }
$CustomWinSDKRoot = $null # Overwritten if we download a Windows SDK from nuget

$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$VSInstallRoot = & $vswhere -nologo -latest -products "*" -all -prerelease -property installationPath
$msbuild = "$VSInstallRoot\MSBuild\Current\Bin\$BuildArchName\MSBuild.exe"

# Avoid $env:ProgramFiles in case this script is running as x86
$UnixToolsBinDir = "$env:SystemDrive\Program Files\Git\usr\bin"

$python = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Shared\Python39_64\python.exe"
if (-not (Test-Path $python)) {
  $python = (where.exe python) | Select-Object -First 1
  if (-not (Test-Path $python)) {
    throw "Python.exe not found"
  }
}

if ($Android -and ($AndroidSDKs.Length -eq 0)) {
  # Enable all android SDKs by default.
  $AndroidSDKs = @("aarch64","armv7","i686","x86_64")
}
# Work around limitations of cmd passing in array arguments via powershell.exe -File
if ($AndroidSDKs.Length -eq 1) { $AndroidSDKs = $AndroidSDKs[0].Split(",") }
if ($WindowsSDKs.Length -eq 1) { $WindowsSDKs = $WindowsSDKs[0].Split(",") }
if ($Test.Length -eq 1) { $Test = $Test[0].Split(",") }

if ($AndroidSDKs.Length -gt 0) {
  # Always enable android when one of the SDKs is specified.
  $Android = $true
}

if ($Test -contains "*") {
  # Explicitly don't include llbuild yet since tests are known to fail on Windows
  $Test = @("swift", "dispatch", "foundation", "xctest")
}

# Architecture definitions
$ArchX64 = @{
  VSName = "amd64";
  ShortName = "x64";
  LLVMName = "x86_64";
  LLVMTarget = "x86_64-unknown-windows-msvc";
  CMakeName = "AMD64";
  BinaryDir = "bin64";
  BuildID = 100;
  BinaryCache = "$BinaryCache\x64";
  PlatformInstallRoot = "$BinaryCache\x64\Windows.platform";
  SDKInstallRoot = "$BinaryCache\x64\Windows.platform\Developer\SDKs\Windows.sdk";
  XCTestInstallRoot = "$BinaryCache\x64\Windows.platform\Developer\Library\XCTest-development";
  ToolchainInstallRoot = "$BinaryCache\x64\toolchains\$ProductVersion+Asserts";
}

$ArchX86 = @{
  VSName = "x86";
  ShortName = "x86";
  LLVMName = "i686";
  LLVMTarget = "i686-unknown-windows-msvc";
  CMakeName = "i686";
  BinaryDir = "bin32";
  BuildID = 200;
  BinaryCache = "$BinaryCache\x86";
  PlatformInstallRoot = "$BinaryCache\x86\Windows.platform";
  SDKInstallRoot = "$BinaryCache\x86\Windows.platform\Developer\SDKs\Windows.sdk";
  XCTestInstallRoot = "$BinaryCache\x86\Windows.platform\Developer\Library\XCTest-development";
}

$ArchARM64 = @{
  VSName = "arm64";
  ShortName = "arm64";
  LLVMName = "aarch64";
  LLVMTarget = "aarch64-unknown-windows-msvc";
  CMakeName = "ARM64";
  BinaryDir = "bin64a";
  BuildID = 300;
  BinaryCache = "$BinaryCache\arm64";
  PlatformInstallRoot = "$BinaryCache\arm64\Windows.platform";
  SDKInstallRoot = "$BinaryCache\arm64\Windows.platform\Developer\SDKs\Windows.sdk";
  XCTestInstallRoot = "$BinaryCache\arm64\Windows.platform\Developer\Library\XCTest-development";
  ToolchainInstallRoot = "$BinaryCache\arm64\toolchains\$ProductVersion+Asserts";
}

$AndroidARM64 = @{
  AndroidArchABI = "arm64-v8a";
  BinaryDir = "bin64a";
  CMakeName = "aarch64";
  LLVMName = "aarch64";
  LLVMTarget = "aarch64-unknown-linux-android$AndroidAPILevel";
  BuildID = 400;
  BinaryCache = "$BinaryCache\aarch64";
  PlatformInstallRoot = "$BinaryCache\arm64\Android.platform";
  SDKInstallRoot = "$BinaryCache\arm64\Android.platform\Developer\SDKs\Android.sdk";
  XCTestInstallRoot = "$BinaryCache\arm64\Android.platform\Developer\Library\XCTest-development";
}

$AndroidARMv7 = @{
  AndroidArchABI = "armeabi-v7a";
  BinaryDir = "bina";
  CMakeName = "armv7-a";
  LLVMName = "armv7";
  LLVMTarget = "armv7-unknown-linux-androideabi$AndroidAPILevel";
  BuildID = 500;
  BinaryCache = "$BinaryCache\armv7";
  PlatformInstallRoot = "$BinaryCache\armv7\Android.platform";
  SDKInstallRoot = "$BinaryCache\armv7\Android.platform\Developer\SDKs\Android.sdk";
  XCTestInstallRoot = "$BinaryCache\armv7\Android.platform\Developer\Library\XCTest-development";
}

$AndroidX86 = @{
  AndroidArchABI = "x86";
  BinaryDir = "bin";
  CMakeName = "i686";
  LLVMName = "i686";
  LLVMTarget = "i686-unknown-linux-android$AndroidAPILevel";
  BuildID = 600;
  BinaryCache = "$BinaryCache\i686";
  PlatformInstallRoot = "$BinaryCache\x86\Android.platform";
  SDKInstallRoot = "$BinaryCache\x86\Android.platform\Developer\SDKs\Android.sdk";
  XCTestInstallRoot = "$BinaryCache\x86\Android.platform\Developer\Library\XCTest-development";
}

$AndroidX64 = @{
  AndroidArchABI = "x86_64";
  BinaryDir = "bin64";
  CMakeName = "x86_64";
  LLVMName = "x86_64";
  LLVMTarget = "x86_64-unknown-linux-android$AndroidAPILevel";
  BuildID = 700;
  BinaryCache = "$BinaryCache\x86_64";
  PlatformInstallRoot = "$BinaryCache\x64\Android.platform";
  SDKInstallRoot = "$BinaryCache\x64\Android.platform\Developer\SDKs\Android.sdk";
  XCTestInstallRoot = "$BinaryCache\x64\Android.platform\Developer\Library\XCTest-development";
}

$HostArch = switch ($HostArchName) {
  "AMD64" { $ArchX64 }
  "ARM64" { $ArchARM64 }
  default { throw "Unsupported processor architecture" }
}

$BuildArch = switch ($BuildArchName) {
  "AMD64" { $ArchX64 }
  "ARM64" { $ArchARM64 }
  default { throw "Unsupported processor architecture" }
}

$IsCrossCompiling = $HostArchName -ne $BuildArchName

$TimingData = New-Object System.Collections.Generic.List[System.Object]

function Get-AndroidNDKPath {
  $androidNDKPath = Join-Path -Path $BinaryCache -ChildPath "android-ndk-$AndroidNDKVersion"
  return $androidNDKPath
}

function Get-InstallDir($Arch) {
  if ($Arch -eq $HostArch) {
    $ProgramFilesName = "Program Files"
  } elseif ($Arch -eq $ArchX86) {
    $ProgramFilesName = "Program Files (x86)"
  } elseif (($HostArch -eq $ArchArm64) -and ($Arch -eq $ArchX64)) {
    # x64 programs actually install under "Program Files" on arm64,
    # but this would conflict with the native installation.
    $ProgramFilesName = "Program Files (Amd64)"
  } else {
    # arm64 cannot be installed on x64
    return $null
  }
  return "$ImageRoot\$ProgramFilesName\Swift"
}

function Get-HostSwiftSDK() {
  return ([IO.Path]::Combine((Get-InstallDir $HostArch), "Platforms", "Windows.platform", "Developer", "SDKs", "Windows.sdk"))
}

$NugetRoot = "$BinaryCache\nuget"
$PinnedToolchain = [IO.Path]::GetFileNameWithoutExtension($PinnedBuild)

$LibraryRoot = "$ImageRoot\Library"

# For dev productivity, install the host toolchain directly using CMake.
# This allows iterating on the toolchain using ninja builds.
$HostArch.ToolchainInstallRoot = "$(Get-InstallDir $HostArch)\Toolchains\$ProductVersion+Asserts"

# Resolve the architectures received as argument
$AndroidSDKArchs = @($AndroidSDKs | ForEach-Object {
  switch ($_) {
    "aarch64" { $AndroidARM64 }
    "armv7" { $AndroidARMv7 }
    "i686" { $AndroidX86 }
    "x86_64" { $AndroidX64 }
    default { throw "Unknown architecture $_" }
  }
})
if ($Android) {
  if ($HostArch -ne $ArchX64) {
    throw "Unsupported host architecture for building android SDKs"
  }
}
$WindowsSDKArchs = @($WindowsSDKs | ForEach-Object {
  switch ($_) {
    "X64" { $ArchX64 }
    "X86" { $ArchX86 }
    "Arm64" { $ArchArm64 }
    default { throw "Unknown architecture $_" }
  }
})

# Build functions
function Invoke-BuildStep([string]$Name) {
  & $Name @Args
  if ($Name.Replace("Build-", "") -eq $BuildTo) {
    exit 0
  }
}

enum TargetComponent {
  LLVM
  Runtime
  Dispatch
  Foundation
  XCTest
}

function Get-TargetProjectBinaryCache($Arch, [TargetComponent]$Project) {
  return "$BinaryCache\" + ($Arch.BuildID + $Project.value__)
}

enum HostComponent {
  Compilers = 5
  System = 10
  ToolsSupportCore
  LLBuild
  Yams
  ArgumentParser
  Driver
  Crypto
  Collections
  ASN1
  Certificates
  PackageManager
  Markdown
  Format
  IndexStoreDB
  SourceKitLSP
  LMDB
  SymbolKit
  DocC
}

function Get-HostProjectBinaryCache([HostComponent]$Project) {
  return "$BinaryCache\$($Project.value__)"
}

function Get-HostProjectCMakeModules([HostComponent]$Project) {
  return "$BinaryCache\$($Project.value__)\cmake\modules"
}

enum BuildComponent {
  BuildTools
  Compilers
}

function Get-BuildProjectBinaryCache([BuildComponent]$Project) {
  return "$BinaryCache\$($Project.value__)"
}

function Get-BuildProjectCMakeModules([BuildComponent]$Project) {
  return "$BinaryCache\$($Project.value__)\cmake\modules"
}

function Copy-File($Src, $Dst) {
  # Create the directory tree first so Copy-Item succeeds
  # If $Dst is the target directory, make sure it ends with "\"
  $DstDir = [IO.Path]::GetDirectoryName($Dst)
  if ($ToBatch) {
    Write-Output "md `"$DstDir`""
    Write-Output "copy /Y `"$Src`" `"$Dst`""
  } else {
    New-Item -ItemType Directory -ErrorAction Ignore $DstDir | Out-Null
    Copy-Item -Force $Src $Dst
  }
}

function Copy-Directory($Src, $Dst) {
  if ($Tobatch) {
    Write-Output "md `"$Dst`""
    Write-Output "copy /Y `"$Src`" `"$Dst`""
  } else {
    New-Item -ItemType Directory -ErrorAction Ignore $Dst | Out-Null
    Copy-Item -Force -Recurse $Src $Dst
  }
}

function Invoke-Program() {
  [CmdletBinding(PositionalBinding = $false)]
  param(
    [Parameter(Position = 0, Mandatory = $true)]
    [string] $Executable,
    [switch] $OutNull = $false,
    [string] $OutFile = "",
    [Parameter(Position = 1, ValueFromRemainingArguments)]
    [string[]] $Args
  )

  if ($ToBatch) {
    # Print the invocation in batch file-compatible format
    $OutputLine = "`"$Executable`""
    $ShouldBreakLine = $false
    for ($i = 0; $i -lt $Args.Length; $i++) {
      if ($ShouldBreakLine -or $OutputLine.Length -ge 40) {
        $OutputLine += " ^"
        Write-Output $OutputLine
        $OutputLine = "  "
      }

      $Arg = $Args[$i]
      if ($Arg.Contains(" ")) {
        $OutputLine += " `"$Arg`""
      } else {
        $OutputLine += " $Arg"
      }

      # Break lines after non-switch arguments
      $ShouldBreakLine = -not $Arg.StartsWith("-")
    }

    if ($OutNull) {
      $OutputLine += " > nul"
    } elseif ("" -ne $OutFile) {
      $OutputLine += " > `"$OutFile`""
    }

    Write-Output $OutputLine
  } else {
    if ($OutNull) {
      & $Executable @Args | Out-Null
    } elseif ("" -ne $OutFile) {
      & $Executable @Args | Out-File -Encoding UTF8 $OutFile
    } else {
      & $Executable @Args
    }

    if ($LastExitCode -ne 0) {
      $ErrorMessage = "Error: $([IO.Path]::GetFileName($Executable)) exited with code $($LastExitCode).`n"

      $ErrorMessage += "Invocation:`n"
      $ErrorMessage += "  $Executable $Args`n"

      $ErrorMessage += "Call stack:`n"
      foreach ($Frame in @(Get-PSCallStack)) {
        $ErrorMessage += "  $Frame`n"
      }

      throw $ErrorMessage
    }
  }
}

function Isolate-EnvVars([scriptblock]$Block) {
  if ($ToBatch) {
    Write-Output "setlocal enableextensions enabledelayedexpansion"
  }

  $OldVars = @{}
  foreach ($Var in (Get-ChildItem env:*).GetEnumerator()) {
    $OldVars.Add($Var.Key, $Var.Value)
  }

  & $Block

  Remove-Item env:*
  foreach ($Var in $OldVars.GetEnumerator()) {
    New-Item -Path "env:\$($Var.Key)" -Value $Var.Value -ErrorAction Ignore | Out-Null
  }

  if ($ToBatch) {
    Write-Output "endlocal"
  }
}

function Invoke-VsDevShell($Arch) {
  $DevCmdArguments = "-no_logo -host_arch=$($BuildArch.VSName) -arch=$($Arch.VSName)"
  if ($CustomWinSDKRoot) {
    $DevCmdArguments += " -winsdk=none"
  } elseif ($WinSDKVersion) {
    $DevCmdArguments += " -winsdk=$WinSDKVersionRevisionZero"
  }

  if ($ToBatch) {
    Write-Output "call `"$VSInstallRoot\Common7\Tools\VsDevCmd.bat`" $DevCmdArguments"
  } else {
    # This dll path is valid for VS2019 and VS2022, but it was under a vsdevcmd subfolder in VS2017 
    Import-Module "$VSInstallRoot\Common7\Tools\Microsoft.VisualStudio.DevShell.dll"
    Enter-VsDevShell -VsInstallPath $VSInstallRoot -SkipAutomaticLocation -DevCmdArguments $DevCmdArguments

    if ($CustomWinSDKRoot) {
      # Using a non-installed Windows SDK. Setup environment variables manually.
      $WinSDKVerIncludeRoot = "$CustomWinSDKRoot\include\$WinSDKVersionRevisionZero"
      $WinSDKIncludePath = "$WinSDKVerIncludeRoot\ucrt;$WinSDKVerIncludeRoot\um;$WinSDKVerIncludeRoot\shared;$WinSDKVerIncludeRoot\winrt;$WinSDKVerIncludeRoot\cppwinrt"
      $WinSDKVerLibRoot = "$CustomWinSDKRoot\lib\$WinSDKVersionRevisionZero"

      $env:WindowsLibPath = "$CustomWinSDKRoot\UnionMetadata\$WinSDKVersionRevisionZero;$CustomWinSDKRoot\References\$WinSDKVersionRevisionZero"
      $env:WindowsSdkBinPath = "$CustomWinSDKRoot\bin"
      $env:WindowsSDKLibVersion = "$WinSDKVersionRevisionZero\"
      $env:WindowsSdkVerBinPath = "$CustomWinSDKRoot\bin\$WinSDKVersionRevisionZero"
      $env:WindowsSDKVersion = "$WinSDKVersionRevisionZero\"

      $env:EXTERNAL_INCLUDE += ";$WinSDKIncludePath"
      $env:INCLUDE += ";$WinSDKIncludePath"
      $env:LIB += ";$WinSDKVerLibRoot\ucrt\$($Arch.ShortName);$WinSDKVerLibRoot\um\$($Arch.ShortName)"
      $env:LIBPATH += ";$env:WindowsLibPath"
      $env:PATH += ";$env:WindowsSdkVerBinPath\$($Arch.ShortName);$env:WindowsSdkBinPath\$($Arch.ShortName)"
      $env:UCRTVersion = $WinSDKVersionRevisionZero
      $env:UniversalCRTSdkDir = $CustomWinSDKRoot
    }
  }
}

function Fetch-Dependencies {
  $ProgressPreference = "SilentlyContinue"

  $WebClient = New-Object Net.WebClient

  function DownloadAndVerify($URL, $Destination, $Hash) {
    if (Test-Path $Destination) {
      return
    }

    Write-Output "$Destination not found. Downloading ..."
    if ($ToBatch) {
      Write-Output "md `"$(Split-Path -Path $Destination -Parent)`""
      Write-Output "curl.exe -sL $URL -o $Destination"
      Write-Output "(certutil -HashFile $Destination SHA256) == $Hash || (exit /b)"
    } else {
      New-Item -ItemType Directory (Split-Path -Path $Destination -Parent) -ErrorAction Ignore | Out-Null
      $WebClient.DownloadFile($URL, $Destination)
      $SHA256 = Get-FileHash -Path $Destination -Algorithm SHA256
      if ($SHA256.Hash -ne $Hash) {
        throw "SHA256 mismatch ($($SHA256.Hash) vs $Hash)"
      }
    }
  }

  function Extract-ZipFile {
    param (
        [string]$ZipFileName,
        [string]$BinaryCache,
        [string]$ExtractPath
    )

    $source = Join-Path -Path $BinaryCache -ChildPath $ZipFileName
    $destination = Join-Path -Path $BinaryCache -ChildPath $ExtractPath

    # Check if the extracted directory already exists and is up to date.
    if (Test-Path $destination) {
        $zipLastWriteTime = (Get-Item $source).LastWriteTime
        $extractedLastWriteTime = (Get-Item $destination).LastWriteTime
        # Compare the last write times
        if ($zipLastWriteTime -le $extractedLastWriteTime) {
            Write-Output "'$ZipFileName' is already extracted and up to date."
            return
        }
    }

    Write-Output "Extracting '$ZipFileName' ..."
    New-Item -ItemType Directory -ErrorAction Ignore -Path $BinaryCache | Out-Null
    Expand-Archive -Path $source -DestinationPath $BinaryCache -Force
  }

  $WiXVersion = "4.0.4"
  $WiXURL = "https://www.nuget.org/api/v2/package/wix/$WiXVersion"
  $WiXHash = "A9CA12214E61BB49430A8C6E5E48AC5AE6F27DC82573B5306955C4D35F2D34E2"
  DownloadAndVerify $WixURL "$BinaryCache\WiX-$WiXVersion.zip" $WiXHash

  # TODO(compnerd) stamp/validate that we need to re-extract
  New-Item -ItemType Directory -ErrorAction Ignore $BinaryCache\WiX-$WiXVersion | Out-Null
  Write-Output "Extracting WiX ..."
  Expand-Archive -Path $BinaryCache\WiX-$WiXVersion.zip -Destination $BinaryCache\WiX-$WiXVersion -Force

  DownloadAndVerify $PinnedBuild "$BinaryCache\$PinnedToolchain.exe" $PinnedSHA256

  # TODO(compnerd) stamp/validate that we need to re-extract
  Write-Output "Extracting $PinnedToolchain ..."
  New-Item -ItemType Directory -ErrorAction Ignore $BinaryCache\toolchains | Out-Null
  # The new runtime MSI is built to expand files into the immediate directory. So, setup the installation location.
  New-Item -ItemType Directory -ErrorAction Ignore $BinaryCache\toolchains\$PinnedToolchain\LocalApp\Programs\Swift\Runtimes\0.0.0\usr\bin | Out-Null
  Invoke-Program $BinaryCache\WiX-$WiXVersion\tools\net6.0\any\wix.exe -- burn extract $BinaryCache\$PinnedToolchain.exe -out $BinaryCache\toolchains\ -outba $BinaryCache\toolchains\
  Get-ChildItem "$BinaryCache\toolchains\WixAttachedContainer" -Filter "*.msi" | % {
    $LogFile = [System.IO.Path]::ChangeExtension($_.Name, "log")
    $TARGETDIR = if ($_.Name -eq "rtl.msi") { "$BinaryCache\toolchains\$PinnedToolchain\LocalApp\Programs\Swift\Runtimes\5.10.1\usr\bin" } else { "$BinaryCache\toolchains\$PinnedToolchain" }
    Invoke-Program -OutNull msiexec.exe /lvx! $BinaryCache\toolchains\$LogFile /qn /a $BinaryCache\toolchains\WixAttachedContainer\$_ ALLUSERS=0 TARGETDIR=$TARGETDIR
  }

  function Download-Python($ArchName) {
    $PythonAMD64URL = "https://www.nuget.org/api/v2/package/python/$PythonVersion"
    $PythonAMD64Hash = "ac43b491e9488ac926ed31c5594f0c9409a21ecbaf99dc7a93f8c7b24cf85867"

    $PythonARM64URL = "https://www.nuget.org/api/v2/package/pythonarm64/$PythonVersion"
    $PythonARM64Hash = "429ada77e7f30e4bd8ff22953a1f35f98b2728e84c9b1d006712561785641f69"

    DownloadAndVerify (Get-Variable -Name "Python${ArchName}URL").Value $BinaryCache\Python$ArchName-$PythonVersion.zip (Get-Variable -Name "Python${ArchName}Hash").Value

    if (-not $ToBatch) {
      # TODO(compnerd) stamp/validate that we need to re-extract
      New-Item -ItemType Directory -ErrorAction Ignore $BinaryCache\Python$ArchName-$PythonVersion | Out-Null
      Write-Output "Extracting Python ($ArchName) ..."
      Expand-Archive -Path $BinaryCache\Python$ArchName-$PythonVersion.zip -Destination $BinaryCache\Python$ArchName-$PythonVersion -Force
    }
  }

  Download-Python $HostArchName
  if ($IsCrossCompiling) {
    Download-Python $BuildArchName
  }

  if ($Android) {
    # Only a specific NDK version is supported right now.
    if ($AndroidNDKVersion -ne "r26b") {
      throw "Unsupported Android NDK version"
    }
    $NDKURL = "https://dl.google.com/android/repository/android-ndk-r26b-windows.zip"
    $NDKHash = "A478D43D4A45D0D345CDA6BE50D79642B92FB175868D9DC0DFC86181D80F691E"
    DownloadAndVerify $NDKURL "$BinaryCache\android-ndk-$AndroidNDKVersion-windows.zip" $NDKHash

    Extract-ZipFile -ZipFileName "android-ndk-$AndroidNDKVersion-windows.zip" -BinaryCache $BinaryCache -ExtractPath "android-ndk-$AndroidNDKVersion"
  }

  if ($WinSDKVersion) {
    try {
      # Check whether VsDevShell can already resolve the requested Windows SDK Version
      Isolate-EnvVars { Invoke-VsDevShell $HostArch }
    } catch {
      $Package = Microsoft.Windows.SDK.CPP

      Write-Output "Windows SDK $WinSDKVersion not found. Downloading from nuget.org ..."
      Invoke-Program nuget install $Package -Version $WinSDKVersion -OutputDirectory $NugetRoot

      # Set to script scope so Invoke-VsDevShell can read it.
      $script:CustomWinSDKRoot = "$NugetRoot\$Package.$WinSDKVersion\c"

      # Install each required architecture package and move files under the base /lib directory.
      $WinSDKArchs = $WindowsSDKArchs.Clone()
      if (-not ($HostArch -in $WinSDKArchs)) {
        $WinSDKArch += $HostArch
      }

      foreach ($Arch in $WinSDKArchs) {
        Invoke-Program nuget install $Package.$($Arch.ShortName) -Version $WinSDKVersion -OutputDirectory $NugetRoot
        Copy-Directory "$NugetRoot\$Package.$($Arch.ShortName).$WinSDKVersion\c\*" "$CustomWinSDKRoot\lib\$WinSDKVersionRevisionZero"
      }
    }
  }
}

function Get-PinnedToolchainTool() {
  if (Test-Path "$BinaryCache\toolchains\${PinnedToolchain}\LocalApp\Programs\Swift\Toolchains\5.10.1+Asserts\usr\bin") {
    return "$BinaryCache\toolchains\${PinnedToolchain}\LocalApp\Programs\Swift\Toolchains\5.10.1+Asserts\usr\bin"
  }
  return "$BinaryCache\toolchains\${PinnedToolchain}\Library\Developer\Toolchains\unknown-Asserts-development.xctoolchain\usr\bin"
}

function Get-PinnedToolchainSDK() {
  if (Test-Path "$BinaryCache\toolchains\${PinnedToolchain}\LocalApp\Programs\Swift\Platforms\5.10.1\Windows.platform\Developer\SDKs\Windows.sdk") {
    return "$BinaryCache\toolchains\${PinnedToolchain}\LocalApp\Programs\Swift\Platforms\5.10.1\Windows.platform\Developer\SDKs\Windows.sdk"
  }
  return "$BinaryCache\toolchains\${PinnedToolchain}\Library\Developer\Platforms\Windows.platform\Developer\SDKs\Windows.sdk"
}

function Get-PinnedToolchainRuntime() {
  if (Test-Path "$BinaryCache\toolchains\${PinnedToolchain}\LocalApp\Programs\Swift\Runtimes\5.10.1\usr\bin\swiftCore.dll") {
    return "$BinaryCache\toolchains\${PinnedToolchain}\LocalApp\Programs\Swift\Runtimes\5.10.1\usr\bin"
  }
  return "$BinaryCache\toolchains\${PinnedToolchain}\PFiles64\Swift\runtime-development\usr\bin"
}

function Get-PinnedToolchainVersion() {
  if (Test-Path variable:PinnedVersion) {
    return $PinnedVersion
  }
  return "5.10.1"
}

function TryAdd-KeyValue([hashtable]$Hashtable, [string]$Key, [string]$Value) {
  if (-not $Hashtable.Contains($Key)) {
    $Hashtable.Add($Key, $Value)
  }
}

function Append-FlagsDefine([hashtable]$Defines, [string]$Name, [string[]]$Value) {
  if ($Defines.Contains($Name)) {
    $Defines[$name] = @($Defines[$name]) + $Value
  } else {
    $Defines.Add($Name, $Value)
  }
}

function Test-CMakeAtLeast([int]$Major, [int]$Minor, [int]$Patch = 0) {
  if ($ToBatch) { return $false }

  $CMakeVersionString = @(& cmake.exe --version)[0]
  if (-not ($CMakeVersionString -match "^cmake version (\d+)\.(\d+)(?:\.(\d+))?")) {
    throw "Unexpected CMake version string format"
  }

  if ([int]$Matches.1 -ne $Major) { return [int]$Matches.1 -gt $Major }
  if ([int]$Matches.2 -ne $Minor) { return [int]$Matches.2 -gt $Minor }
  if ($null -eq $Matches.3) { return 0 -gt $Patch }
  return [int]$Matches.3 -ge $Patch
}

enum Platform {
  Windows
  Android
}

function Build-CMakeProject {
  [CmdletBinding(PositionalBinding = $false)]
  param(
    [string] $Src,
    [string] $Bin,
    [string] $InstallTo = "",
    [Platform] $Platform = "Windows",
    [hashtable] $Arch,
    [string] $Generator = "Ninja",
    [string] $CacheScript = "",
    [string[]] $UseMSVCCompilers = @(), # C,CXX
    [string[]] $UseBuiltCompilers = @(), # ASM,C,CXX,Swift
    [string[]] $UsePinnedCompilers = @(), # ASM,C,CXX,Swift
    [switch] $UseSwiftSwiftDriver = $false,
    [string] $SwiftSDK = "",
    [hashtable] $Defines = @{}, # Values are either single strings or arrays of flags
    [string[]] $BuildTargets = @()
  )

  if ($ToBatch) {
    Write-Output ""
    Write-Output "echo Building '$Src' to '$Bin' for arch '$($Arch.LLVMName)'..."
  } else {
    Write-Host -ForegroundColor Cyan "[$([DateTime]::Now.ToString("yyyy-MM-dd HH:mm:ss"))] Building '$Src' to '$Bin' for arch '$($Arch.LLVMName)'..."
  }

  $Stopwatch = [Diagnostics.Stopwatch]::StartNew()

  # Enter the developer command shell early so we can resolve cmake.exe
  # for version checks.
  Isolate-EnvVars {
    if ($Platform -eq "Windows") {
      Invoke-VsDevShell $Arch
    }

    $CompilersBinaryCache = if ($IsCrossCompiling) {
      Get-BuildProjectBinaryCache Compilers
    } else {
      Get-HostProjectBinaryCache Compilers
    }
    $DriverBinaryCache = Get-HostProjectBinaryCache Driver

    if ($EnableCaching) {
      $env:SCCACHE_DIRECT = "true"
      $env:SCCACHE_DIR = "$BinaryCache\sccache"
    }
    if ($UseSwiftSwiftDriver) {
      $env:SWIFT_DRIVER_SWIFT_FRONTEND_EXEC = ([IO.Path]::Combine($CompilersBinaryCache, "bin", "swift-frontend.exe"))
    }

    # TODO(compnerd) workaround swiftc.exe symlink not existing.
    if ($UseSwiftSwiftDriver) {
      Copy-Item -Force ([IO.Path]::Combine($DriverBinaryCache, "bin", "swift-driver.exe")) ([IO.Path]::Combine($DriverBinaryCache, "bin", "swiftc.exe"))
    }

    # Add additional defines (unless already present)
    $Defines = $Defines.Clone()

    if (($Platform -ne "Windows") -or ($Arch.CMakeName -ne $BuildArch.CMakeName)) {
      TryAdd-KeyValue $Defines CMAKE_SYSTEM_NAME $Platform
      TryAdd-KeyValue $Defines CMAKE_SYSTEM_PROCESSOR $Arch.CMakeName
    }

    if ($Platform -eq "Android") {
      $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
      $VSInstallPath = & $vswhere -nologo -latest -products * -property installationPath
      if (Test-Path "${VSInstallPath}\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin") {
        $env:Path = "${VSInstallPath}\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin;${VSInstallPath}\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja;${env:Path}"
        TryAdd-KeyValue $Defines CMAKE_MAKE_PROGRAM "${VSInstallPath}\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe"
      } else {
        throw "Missing CMake and Ninja in the visual studio installation that are needed to build Android"
      }
      $androidNDKPath = Get-AndroidNDKPath
      TryAdd-KeyValue $Defines CMAKE_C_COMPILER (Join-Path -Path $androidNDKPath -ChildPath "toolchains\llvm\prebuilt\windows-x86_64\bin\clang.exe")
      TryAdd-KeyValue $Defines CMAKE_CXX_COMPILER (Join-Path -Path $androidNDKPath -ChildPath "toolchains\llvm\prebuilt\windows-x86_64\bin\clang++.exe")
      TryAdd-KeyValue $Defines CMAKE_ANDROID_API "$AndroidAPILevel"
      TryAdd-KeyValue $Defines CMAKE_ANDROID_ARCH_ABI $Arch.AndroidArchABI
      TryAdd-KeyValue $Defines CMAKE_ANDROID_NDK "$androidNDKPath"
      TryAdd-KeyValue $Defines SWIFT_ANDROID_NDK_PATH "$androidNDKPath"
      TryAdd-KeyValue $Defines CMAKE_C_COMPILER_WORKS YES
      TryAdd-KeyValue $Defines CMAKE_CXX_COMPILER_WORKS YES
      # The current Android NDK ships with Clang 17,
      # which doesn't provide the _Builtin_float module.
      TryAdd-KeyValue $Defines SWIFT_BUILD_CLANG_OVERLAYS_SKIP_BUILTIN_FLOAT YES
    }

    TryAdd-KeyValue $Defines CMAKE_BUILD_TYPE Release
    TryAdd-KeyValue $Defines CMAKE_MT "mt"

    $CFlags = @()
    switch ($Platform) {
      Windows {
        $CFlags = @("/GS-", "/Gw", "/Gy", "/Oi", "/Oy", "/Zc:inline")
      }
      Android {
        $CFlags = @("--sysroot=$(Get-AndroidNDKPath)\toolchains\llvm\prebuilt\windows-x86_64\sysroot")
      }
    }

    $CXXFlags = @()
    if ($Platform -eq "Windows") {
      $CXXFlags += $CFlags.Clone() + @("/Zc:__cplusplus")
    }

    if ($UseMSVCCompilers.Contains("C") -Or $UseMSVCCompilers.Contains("CXX") -Or
        $UseBuiltCompilers.Contains("C") -Or $UseBuiltCompilers.Contains("CXX") -Or
        $UsePinnedCompilers.Contains("C") -Or $UsePinnedCompilers.Contains("CXX")) {
      if ($DebugInfo -and $Platform -eq "Windows") {
        Append-FlagsDefine $Defines CMAKE_MSVC_DEBUG_INFORMATION_FORMAT Embedded
        Append-FlagsDefine $Defines CMAKE_POLICY_CMP0141 NEW
        # Add additional linker flags for generating the debug info.
        Append-FlagsDefine $Defines CMAKE_SHARED_LINKER_FLAGS "/debug"
        Append-FlagsDefine $Defines CMAKE_EXE_LINKER_FLAGS "/debug"
      }
    }

    if ($UseMSVCCompilers.Contains("C")) {
      TryAdd-KeyValue $Defines CMAKE_C_COMPILER cl
      if ($EnableCaching) {
        TryAdd-KeyValue $Defines CMAKE_C_COMPILER_LAUNCHER sccache
      }
      Append-FlagsDefine $Defines CMAKE_C_FLAGS $CFlags
    }
    if ($UseMSVCCompilers.Contains("CXX")) {
      TryAdd-KeyValue $Defines CMAKE_CXX_COMPILER cl
      if ($EnableCaching) {
        TryAdd-KeyValue $Defines CMAKE_CXX_COMPILER_LAUNCHER sccache
      }
      Append-FlagsDefine $Defines CMAKE_CXX_FLAGS $CXXFlags
    }
    if ($UsePinnedCompilers.Contains("ASM") -Or $UseBuiltCompilers.Contains("ASM")) {
      $Driver = if ($Platform -eq "Windows") { "clang-cl.exe" } else { "clang.exe" }
      if ($UseBuiltCompilers.Contains("ASM")) {
        TryAdd-KeyValue $Defines CMAKE_ASM_COMPILER ([IO.Path]::Combine($CompilersBinaryCache, "bin", $Driver))
      } else {
        TryAdd-KeyValue $Defines CMAKE_ASM_COMPILER (Join-Path -Path (Get-PinnedToolchainTool) -ChildPath $Driver)
      }
      Append-FlagsDefine $Defines CMAKE_ASM_FLAGS "--target=$($Arch.LLVMTarget)"
      if ($Platform -eq "Windows") {
        TryAdd-KeyValue $Defines CMAKE_ASM_COMPILE_OPTIONS_MSVC_RUNTIME_LIBRARY_MultiThreadedDLL "/MD"
      }
    }
    if ($UsePinnedCompilers.Contains("C") -Or $UseBuiltCompilers.Contains("C")) {
      $Driver = if ($Platform -eq "Windows") { "clang-cl.exe" } else { "clang.exe" }
      if ($UseBuiltCompilers.Contains("C")) {
        TryAdd-KeyValue $Defines CMAKE_C_COMPILER ([IO.Path]::Combine($CompilersBinaryCache, "bin", $Driver))
      } else {
        TryAdd-KeyValue $Defines CMAKE_C_COMPILER (Join-Path -Path (Get-PinnedToolchainTool) -ChildPath $Driver)
      }
      TryAdd-KeyValue $Defines CMAKE_C_COMPILER_TARGET $Arch.LLVMTarget

      if (-not (Test-CMakeAtLeast -Major 3 -Minor 26 -Patch 3) -and $Platform -eq "Windows") {
        # Workaround for https://github.com/ninja-build/ninja/issues/2280
        TryAdd-KeyValue $Defines CMAKE_CL_SHOWINCLUDES_PREFIX "Note: including file: "
      }

      if ($DebugInfo -and $CDebugFormat -eq "dwarf") {
        Append-FlagsDefine $Defines CMAKE_C_FLAGS "-gdwarf"
      }
      Append-FlagsDefine $Defines CMAKE_C_FLAGS $CFlags
    }
    if ($UsePinnedCompilers.Contains("CXX") -Or $UseBuiltCompilers.Contains("CXX")) {
      $Driver = if ($Platform -eq "Windows") { "clang-cl.exe" } else { "clang++.exe" }
      if ($UseBuiltCompilers.Contains("CXX")) {
        TryAdd-KeyValue $Defines CMAKE_CXX_COMPILER ([IO.Path]::Combine($CompilersBinaryCache, "bin", $Driver))
      } else {
        TryAdd-KeyValue $Defines CMAKE_CXX_COMPILER (Join-Path -Path (Get-PinnedToolchainTool) -ChildPath $Driver)
      }
      TryAdd-KeyValue $Defines CMAKE_CXX_COMPILER_TARGET $Arch.LLVMTarget

      if (-not (Test-CMakeAtLeast -Major 3 -Minor 26 -Patch 3) -and $Platform -eq "Windows") {
        # Workaround for https://github.com/ninja-build/ninja/issues/2280
        TryAdd-KeyValue $Defines CMAKE_CL_SHOWINCLUDES_PREFIX "Note: including file: "
      }

      if ($DebugInfo -and $CDebugFormat -eq "dwarf") {
        Append-FlagsDefine $Defines CMAKE_CXX_FLAGS "-gdwarf"
      }
      Append-FlagsDefine $Defines CMAKE_CXX_FLAGS $CXXFlags
    }
    if ($UsePinnedCompilers.Contains("Swift") -Or $UseBuiltCompilers.Contains("Swift")) {
      $SwiftArgs = @()

      if ($UseSwiftSwiftDriver) {
        TryAdd-KeyValue $Defines CMAKE_Swift_COMPILER ([IO.Path]::Combine($DriverBinaryCache, "bin", "swiftc.exe"))
      } elseif ($UseBuiltCompilers.Contains("Swift")) {
        TryAdd-KeyValue $Defines CMAKE_Swift_COMPILER ([IO.Path]::Combine($CompilersBinaryCache, "bin", "swiftc.exe"))
      } else {
        TryAdd-KeyValue $Defines CMAKE_Swift_COMPILER (Join-Path -Path (Get-PinnedToolchainTool) -ChildPath  "swiftc.exe")
      }
      if (-not ($Platform -eq "Windows")) {
        TryAdd-KeyValue $Defines CMAKE_Swift_COMPILER_WORKS = "YES"
      }
      TryAdd-KeyValue $Defines CMAKE_Swift_COMPILER_TARGET $Arch.LLVMTarget.Replace("$AndroidAPILevel", "")
      if ($UseBuiltCompilers.Contains("Swift")) {
        if ($SwiftSDK -ne "") {
          $SwiftArgs += @("-sdk", $SwiftSDK)
        } else {
          $RuntimeBinaryCache = Get-TargetProjectBinaryCache $Arch Runtime
          $SwiftResourceDir = "${RuntimeBinaryCache}\lib\swift"

          switch ($Platform) {
            Windows {
              $SwiftArgs += @(
                "-vfsoverlay", "$RuntimeBinaryCache\stdlib\windows-vfs-overlay.yaml",
                "-strict-implicit-module-context",
                "-Xcc", "-Xclang", "-Xcc", "-fbuiltin-headers-in-system-modules"
              )
            }
            Android {
              $androidNDKPath = Get-AndroidNDKPath
              $SwiftArgs += @("-sdk", "$androidNDKPath\toolchains\llvm\prebuilt\windows-x86_64\sysroot")
              $SwiftArgs += @(
                "-Xclang-linker", "-target",
                "-Xclang-linker", $Arch.LLVMTarget,
                "-Xclang-linker", "--sysroot",
                "-Xclang-linker", "$androidNDKPath\toolchains\llvm\prebuilt\windows-x86_64\sysroot",
                "-Xclang-linker", "-resource-dir",
                "-Xclang-linker", "$androidNDKPath\toolchains\llvm\prebuilt\windows-x86_64\lib\clang\17"
              )
            }
          }
          $SwiftArgs += @("-resource-dir", "$SwiftResourceDir")
          $SwiftArgs += @("-L", "$SwiftResourceDir\$($Platform.ToString().ToLowerInvariant())")
        }
      } else {
        $SwiftArgs += @("-sdk", (Get-PinnedToolchainSDK))
      }

      # Debug Information
      if ($DebugInfo) {
        if ($Platform -eq "Windows") {
          if ($SwiftDebugFormat -eq "dwarf") {
            $SwiftArgs += @("-g", "-Xlinker", "/DEBUG:DWARF", "-use-ld=lld-link")
          } else {
            $SwiftArgs += @("-g", "-debug-info-format=codeview", "-Xlinker", "-debug")
          }
        } else {
          $SwiftArgs += @("-g")
        }
      } else {
        $SwiftArgs += "-gnone"
      }

      if ($Platform -eq "Windows") {
        $SwiftArgs += @("-Xlinker", "/INCREMENTAL:NO")
        # Swift Requries COMDAT folding and de-duplication
        $SwiftArgs += @("-Xlinker", "/OPT:REF")
        $SwiftArgs += @("-Xlinker", "/OPT:ICF")
      }

      Append-FlagsDefine $Defines CMAKE_Swift_FLAGS $SwiftArgs

      # Workaround CMake 3.26+ enabling `-wmo` by default on release builds
      Append-FlagsDefine $Defines CMAKE_Swift_FLAGS_RELEASE "-O"
      Append-FlagsDefine $Defines CMAKE_Swift_FLAGS_RELWITHDEBINFO "-O"
    }
    if ("" -ne $InstallTo) {
      TryAdd-KeyValue $Defines CMAKE_INSTALL_PREFIX $InstallTo
    }

    # Generate the project
    $cmakeGenerateArgs = @("-B", $Bin, "-S", $Src, "-G", $Generator)
    if ("" -ne $CacheScript) {
      $cmakeGenerateArgs += @("-C", $CacheScript)
    }
    foreach ($Define in ($Defines.GetEnumerator() | Sort-Object Name)) {
      # The quoting gets tricky to support defines containing compiler flags args,
      # some of which can contain spaces, for example `-D` `Flags=-flag "C:/Program Files"`
      # Avoid backslashes since they are going into CMakeCache.txt,
      # where they are interpreted as escapes.
      if ($Define.Value -is [string]) {
        # Single token value, no need to quote spaces, the splat operator does the right thing.
        $Value = $Define.Value.Replace("\", "/")
      } else {
        # Flags array, multiple tokens, quoting needed for tokens containing spaces
        $Value = ""
        foreach ($Arg in $Define.Value) {
          if ($Value.Length -gt 0) {
            $Value += " "
          }

          $ArgWithForwardSlashes = $Arg.Replace("\", "/")
          if ($ArgWithForwardSlashes.Contains(" ")) {
            # Quote and escape the quote so it makes it through
            $Value += "\""$ArgWithForwardSlashes\"""
          } else {
            $Value += $ArgWithForwardSlashes
          }
        }
      }

      $cmakeGenerateArgs += @("-D", "$($Define.Key)=$Value")
    }

    if ($UseBuiltCompilers.Contains("Swift")) {
      $env:Path = "$($BuildArch.SDKInstallRoot)\usr\bin;$($BuildArch.BinaryCache)\cmark-gfm-0.29.0.gfm.13\src;$($BuildArch.ToolchainInstallRoot)\usr\bin;${env:Path}"
    } elseif ($UsePinnedCompilers.Contains("Swift")) {
      $env:Path = "$(Get-PinnedToolchainRuntime);${env:Path}"
    }
    Invoke-Program cmake.exe @cmakeGenerateArgs

    # Build all requested targets
    foreach ($Target in $BuildTargets) {
      if ($Target -eq "default") {
        Invoke-Program cmake.exe --build $Bin
      } else {
        Invoke-Program cmake.exe --build $Bin --target $Target
      }
    }

    if ("" -ne $InstallTo) {
      Invoke-Program cmake.exe --build $Bin --target install
    }
  }

  if (-not $ToBatch) {
    Write-Host -ForegroundColor Cyan "[$([DateTime]::Now.ToString("yyyy-MM-dd HH:mm:ss"))] Finished building '$Src' to '$Bin' for arch '$($Arch.LLVMName)' in $($Stopwatch.Elapsed)"
    Write-Host ""
  }

  if ($Summary) {
    $TimingData.Add([PSCustomObject]@{
      Arch = $Arch.LLVMName
      Platform = $Platform
      Checkout = $Src.Replace($SourceCache, '')
      "Elapsed Time" = $Stopwatch.Elapsed.ToString()
    })
  }
}

function Build-SPMProject {
  [CmdletBinding(PositionalBinding = $false)]
  param(
    [string] $Src,
    [string] $Bin,
    [hashtable] $Arch,
    [switch] $Test = $false,
    [Parameter(ValueFromRemainingArguments)]
    [string[]] $AdditionalArguments
  )

  if ($ToBatch) {
    Write-Output ""
    Write-Output "echo Building '$Src' to '$Bin' for arch '$($Arch.LLVMName)'..."
  } else {
    Write-Host -ForegroundColor Cyan "[$([DateTime]::Now.ToString("yyyy-MM-dd HH:mm:ss"))] Building '$Src' to '$Bin' for arch '$($Arch.LLVMName)'..."
  }

  $Stopwatch = [Diagnostics.Stopwatch]::StartNew()

  Isolate-EnvVars {
    $SDKInstallRoot = [IO.Path]::Combine((Get-InstallDir $HostArch), "Platforms", "Windows.platform", "Developer", "SDKs", "Windows.sdk")
    $RuntimeInstallRoot = [IO.Path]::Combine((Get-InstallDir $HostArch), "Runtimes", $ProductVersion)

    $env:Path = "$RuntimeInstallRoot\usr\bin;$($HostArch.ToolchainInstallRoot)\usr\bin;${env:Path}"
    $env:SDKROOT = $SDKInstallRoot

    $Arguments = @(
        "--scratch-path", $Bin,
        "--package-path", $Src,
        "-c", "release",
        "-Xbuild-tools-swiftc", "-I$SDKInstallRoot\usr\lib\swift",
        "-Xbuild-tools-swiftc", "-L$SDKInstallRoot\usr\lib\swift\windows",
        "-Xcc", "-I$SDKInstallRoot\usr\lib\swift",
        "-Xlinker", "-L$SDKInstallRoot\usr\lib\swift\windows"
    )
    if ($DebugInfo) {
      if ($SwiftDebugFormat -eq "dwarf") {
        $Arguments += @("-debug-info-format", "dwarf")
      } else {
        $Arguments += @("-debug-info-format", "codeview")
      }
    } else {
      $Arguments += @("-debug-info-format", "none")
    }

    $Action = if ($Test) { "test" } else { "build" }
    Invoke-Program "$($HostArch.ToolchainInstallRoot)\usr\bin\swift.exe" $Action @Arguments @AdditionalArguments
  }

  if (-not $ToBatch) {
    Write-Host -ForegroundColor Cyan "[$([DateTime]::Now.ToString("yyyy-MM-dd HH:mm:ss"))] Finished building '$Src' to '$Bin' for arch '$($Arch.LLVMName)' in $($Stopwatch.Elapsed)"
    Write-Host ""
  }

  if ($Summary) {
    $TimingData.Add([PSCustomObject]@{
      Arch = $Arch.LLVMName
      Checkout = $Src.Replace($SourceCache, '')
      Platform = "Windows"
      "Elapsed Time" = $Stopwatch.Elapsed.ToString()
    })
  }
}

function Build-WiXProject() {
  [CmdletBinding(PositionalBinding = $false)]
  param(
    [Parameter(Position = 0, Mandatory = $true)]
    [string]$FileName,
    [Parameter(Mandatory = $true)]
    [hashtable]$Arch,
    [switch]$Bundle,
    [hashtable]$Properties = @{}
  )

  $ArchName = $Arch.VSName

  $ProductVersionArg = $ProductVersion
  if (-not $Bundle) {
    # WiX v4 will accept a semantic version string for Bundles,
    # but Packages still require a purely numerical version number, 
    # so trim any semantic versionning suffixes
    $ProductVersionArg = [regex]::Replace($ProductVersion, "[-+].*", "")
  }

  $Properties = $Properties.Clone()
  TryAdd-KeyValue $Properties Configuration Release
  TryAdd-KeyValue $Properties BaseOutputPath "$($Arch.BinaryCache)\installer\"
  TryAdd-KeyValue $Properties ProductArchitecture $ArchName
  TryAdd-KeyValue $Properties ProductVersion $ProductVersionArg

  $MSBuildArgs = @("$SourceCache\swift-installer-scripts\platforms\Windows\$FileName")
  $MSBuildArgs += "-noLogo"
  $MSBuildArgs += "-restore"
  $MSBuildArgs += "-maxCpuCount"
  foreach ($Property in $Properties.GetEnumerator()) {
    if ($Property.Value.Contains(" ")) {
      $MSBuildArgs += "-p:$($Property.Key)=$($Property.Value.Replace('\', '\\'))"
    } else {
      $MSBuildArgs += "-p:$($Property.Key)=$($Property.Value)"
    }
  }
  $MSBuildArgs += "-binaryLogger:$($Arch.BinaryCache)\msi\$ArchName-$([System.IO.Path]::GetFileNameWithoutExtension($FileName)).binlog"
  $MSBuildArgs += "-detailedSummary:False"

  Invoke-Program $msbuild @MSBuildArgs
}

function Build-CMark($Arch) {
  $ArchName = $Arch.LLVMName

  Build-CMakeProject `
    -Src $SourceCache\cmark `
    -Bin "$($Arch.BinaryCache)\cmark-gfm-0.29.0.gfm.13" `
    -InstallTo "$($Arch.ToolchainInstallRoot)\usr" `
    -Arch $Arch `
    -BuildTargets default `
    -Defines @{
      BUILD_SHARED_LIBS = "YES";
      BUILD_TESTING = "NO";
      CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS_SKIP = "YES";
    }
}

function Build-BuildTools($Arch) {
  Build-CMakeProject `
    -Src $SourceCache\llvm-project\llvm `
    -Bin (Get-BuildProjectBinaryCache BuildTools) `
    -Arch $Arch `
    -UseMSVCCompilers C,CXX `
    -BuildTargets llvm-tblgen,clang-tblgen,clang-pseudo-gen,clang-tidy-confusable-chars-gen,lldb-tblgen,llvm-config,swift-def-to-strings-converter,swift-serialize-diagnostics,swift-compatibility-symbols `
    -Defines @{
      CMAKE_CROSSCOMPILING = "NO";
      LLDB_ENABLE_PYTHON = "NO";
      LLDB_INCLUDE_TESTS = "NO";
      LLDB_ENABLE_SWIFT_SUPPORT = "NO";
      LLVM_ENABLE_ASSERTIONS = "NO";
      LLVM_ENABLE_LIBEDIT = "NO";
      LLVM_ENABLE_LIBXML2 = "NO";
      LLVM_ENABLE_PROJECTS = "clang;clang-tools-extra;lldb";
      LLVM_EXTERNAL_PROJECTS = "swift";
      LLVM_EXTERNAL_SWIFT_SOURCE_DIR = "$SourceCache\swift";
      SWIFT_BUILD_DYNAMIC_SDK_OVERLAY = "NO";
      SWIFT_BUILD_DYNAMIC_STDLIB = "NO";
      SWIFT_BUILD_HOST_DISPATCH = "NO";
      SWIFT_BUILD_LIBEXEC = "NO";
      SWIFT_BUILD_REGEX_PARSER_IN_COMPILER = "NO";
      SWIFT_BUILD_REMOTE_MIRROR = "NO";
      SWIFT_BUILD_SOURCEKIT = "NO";
      SWIFT_BUILD_STATIC_SDK_OVERLAY = "NO";
      SWIFT_BUILD_STATIC_STDLIB = "NO";
      SWIFT_BUILD_SWIFT_SYNTAX = "NO";
      SWIFT_ENABLE_DISPATCH = "NO";
      SWIFT_INCLUDE_APINOTES = "NO";
      SWIFT_INCLUDE_DOCS = "NO";
      SWIFT_INCLUDE_TESTS = "NO";
      "cmark-gfm_DIR" = "$($Arch.ToolchainInstallRoot)\usr\lib\cmake";
    }
}

function Build-Compilers() {
  [CmdletBinding(PositionalBinding = $false)]
  param
  (
    [Parameter(Position = 0, Mandatory = $true)]
    [hashtable]$Arch,
    [switch]$TestClang = $false,
    [switch]$TestLLD = $false,
    [switch]$TestLLDB = $false,
    [switch]$TestLLVM = $false,
    [switch]$TestSwift = $false,
    [switch]$Build = $false
  )

  Isolate-EnvVars {
    $CompilersBinaryCache = if ($Build) {
      Get-BuildProjectBinaryCache Compilers
    } else {
      Get-HostProjectBinaryCache Compilers
    }
    $BuildTools = Join-Path -Path (Get-BuildProjectBinaryCache BuildTools) -ChildPath bin

    if ($TestClang -or $TestLLD -or $TestLLDB -or $TestLLVM -or $TestSwift) {
      $env:Path = "$($HostArch.BinaryCache)\cmark-gfm-0.29.0.gfm.13\src;$CompilersBinaryCache\tools\swift\libdispatch-windows-$($Arch.LLVMName)-prefix\bin;$CompilersBinaryCache\bin;$env:Path;$VSInstallRoot\DIA SDK\bin\$($HostArch.VSName);$UnixToolsBinDir"
      $Targets = @()
      $TestingDefines = @{
        SWIFT_BUILD_DYNAMIC_SDK_OVERLAY = "YES";
        SWIFT_BUILD_DYNAMIC_STDLIB = "YES";
        SWIFT_BUILD_REMOTE_MIRROR = "YES";
        SWIFT_NATIVE_SWIFT_TOOLS_PATH = "";
      }

      if ($TestClang) { $Targets += @("check-clang") }
      if ($TestLLD) { $Targets += @("check-lld") }
      if ($TestLLDB) { $Targets += @("check-lldb") }
      if ($TestLLVM) { $Targets += @("check-llvm") }
      if ($TestSwift) { $Targets += @("check-swift", "SwiftCompilerPlugin") }
    } else {
      $Targets = @("distribution", "install-distribution")
      $TestingDefines = @{
        SWIFT_BUILD_DYNAMIC_SDK_OVERLAY = "NO";
        SWIFT_BUILD_DYNAMIC_STDLIB = "NO";
        SWIFT_BUILD_REMOTE_MIRROR = "NO";
        SWIFT_NATIVE_SWIFT_TOOLS_PATH = $BuildTools;
      }
    }

    # The STL in VS 17.10 requires Clang 17 or higher, but Swift toolchains prior to version 6 include older versions
    # of Clang. If bootstrapping with an older toolchain, we need to relax to relax this requirement with
    # ALLOW_COMPILER_AND_STL_VERSION_MISMATCH.
    $SwiftFlags = @();
    if ([System.Version](Get-PinnedToolchainVersion) -lt [System.Version]"6.0") {
      $SwiftFlags += @("-Xcc", "-D_ALLOW_COMPILER_AND_STL_VERSION_MISMATCH");
    }

    Build-CMakeProject `
      -Src $SourceCache\llvm-project\llvm `
      -Bin $CompilersBinaryCache `
      -InstallTo "$($Arch.ToolchainInstallRoot)\usr" `
      -Arch $Arch `
      -UseMSVCCompilers C,CXX `
      -UsePinnedCompilers Swift `
      -BuildTargets $Targets `
      -CacheScript $SourceCache\swift\cmake\caches\Windows-$($Arch.LLVMName).cmake `
      -Defines ($TestingDefines + @{
        CLANG_TABLEGEN = (Join-Path -Path $BuildTools -ChildPath "clang-tblgen.exe");
        CLANG_TIDY_CONFUSABLE_CHARS_GEN = (Join-Path -Path $BuildTools -ChildPath "clang-tidy-confusable-chars-gen.exe");
        CMAKE_Swift_FLAGS = $SwiftFlags;
        LLDB_PYTHON_EXE_RELATIVE_PATH = "python.exe";
        LLDB_PYTHON_EXT_SUFFIX = ".pyd";
        LLDB_PYTHON_RELATIVE_PATH = "lib/site-packages";
        LLDB_TABLEGEN = (Join-Path -Path $BuildTools -ChildPath "lldb-tblgen.exe");
        LLVM_CONFIG_PATH = (Join-Path -Path $BuildTools -ChildPath "llvm-config.exe");
        LLVM_EXTERNAL_SWIFT_SOURCE_DIR = "$SourceCache\swift";
        LLVM_NATIVE_TOOL_DIR = $BuildTools;
        LLVM_TABLEGEN = (Join-Path $BuildTools -ChildPath "llvm-tblgen.exe");
        LLVM_USE_HOST_TOOLS = "NO";
        Python3_EXECUTABLE = "$python";
        Python3_INCLUDE_DIR = "$BinaryCache\Python$($Arch.CMakeName)-$PythonVersion\tools\include";
        Python3_LIBRARY = "$BinaryCache\Python$($Arch.CMakeName)-$PythonVersion\tools\libs\python39.lib";
        Python3_ROOT_DIR = "$BinaryCache\Python$($Arch.CMakeName)-$PythonVersion\tools";
        SWIFT_BUILD_SWIFT_SYNTAX = "YES";
        SWIFT_CLANG_LOCATION = (Get-PinnedToolchainTool);
        SWIFT_ENABLE_EXPERIMENTAL_CONCURRENCY = "YES";
        SWIFT_ENABLE_EXPERIMENTAL_CXX_INTEROP = "YES";
        SWIFT_ENABLE_EXPERIMENTAL_DIFFERENTIABLE_PROGRAMMING = "YES";
        SWIFT_ENABLE_EXPERIMENTAL_DISTRIBUTED = "YES";
        SWIFT_ENABLE_EXPERIMENTAL_OBSERVATION = "YES";
        SWIFT_ENABLE_EXPERIMENTAL_STRING_PROCESSING = "YES";
        SWIFT_ENABLE_SYNCHRONIZATION = "YES";
        SWIFT_ENABLE_VOLATILE = "YES";
        SWIFT_PATH_TO_LIBDISPATCH_SOURCE = "$SourceCache\swift-corelibs-libdispatch";
        SWIFT_PATH_TO_SWIFT_SYNTAX_SOURCE = "$SourceCache\swift-syntax";
        SWIFT_PATH_TO_STRING_PROCESSING_SOURCE = "$SourceCache\swift-experimental-string-processing";
        SWIFT_PATH_TO_SWIFT_SDK = (Get-PinnedToolchainSDK);
        "cmark-gfm_DIR" = "$($Arch.ToolchainInstallRoot)\usr\lib\cmake";
      })
  }
}

function Build-LLVM([Platform]$Platform, $Arch) {
  Build-CMakeProject `
    -Src $SourceCache\llvm-project\llvm `
    -Bin (Get-TargetProjectBinaryCache $Arch LLVM) `
    -Arch $Arch `
    -Platform $Platform `
    -UseMSVCCompilers C,CXX `
    -Defines @{
      CMAKE_SYSTEM_NAME = $Platform.ToString();
      LLVM_HOST_TRIPLE = $Arch.LLVMTarget;
    }
}

function Build-ZLib([Platform]$Platform, $Arch) {
  $ArchName = $Arch.LLVMName

  Build-CMakeProject `
    -Src $SourceCache\zlib `
    -Bin "$($Arch.BinaryCache)\$Platform\zlib-1.3.1" `
    -InstallTo $LibraryRoot\zlib-1.3.1\usr `
    -Arch $Arch `
    -Platform $Platform `
    -UseMSVCCompilers C `
    -BuildTargets default `
    -Defines @{
      BUILD_SHARED_LIBS = "NO";
      CMAKE_POSITION_INDEPENDENT_CODE = "YES";
      CMAKE_SYSTEM_NAME = $Platform.ToString();
      INSTALL_BIN_DIR = "$LibraryRoot\zlib-1.3.1\usr\bin\$Platform\$ArchName";
      INSTALL_LIB_DIR = "$LibraryRoot\zlib-1.3.1\usr\lib\$Platform\$ArchName";
    }
}

function Build-XML2([Platform]$Platform, $Arch) {
  $ArchName = $Arch.LLVMName

  Build-CMakeProject `
    -Src $SourceCache\libxml2 `
    -Bin "$($Arch.BinaryCache)\$Platform\libxml2-2.11.5" `
    -InstallTo "$LibraryRoot\libxml2-2.11.5\usr" `
    -Arch $Arch `
    -Platform $Platform `
    -UseMSVCCompilers C,CXX `
    -BuildTargets default `
    -Defines @{
      BUILD_SHARED_LIBS = "NO";
      CMAKE_INSTALL_BINDIR = "bin/$Platform/$ArchName";
      CMAKE_INSTALL_LIBDIR = "lib/$Platform/$ArchName";
      CMAKE_SYSTEM_NAME = $Platform.ToString();
      LIBXML2_WITH_ICONV = "NO";
      LIBXML2_WITH_ICU = "NO";
      LIBXML2_WITH_LZMA = "NO";
      LIBXML2_WITH_PYTHON = "NO";
      LIBXML2_WITH_TESTS = "NO";
      LIBXML2_WITH_THREADS = "YES";
      LIBXML2_WITH_ZLIB = "NO";
    }
}

function Build-CURL([Platform]$Platform, $Arch) {
  $ArchName = $Arch.LLVMName

  $PlatformDefines = @{}
  if ($Platform -eq "Android") {
    $PlatformDefines += @{
      HAVE_FSEEKO = "0";
    }
  }

  Build-CMakeProject `
    -Src $SourceCache\curl `
    -Bin "$($Arch.BinaryCache)\$Platform\curl-8.5.0" `
    -InstallTo "$LibraryRoot\curl-8.5.0\usr" `
    -Arch $Arch `
    -Platform $Platform `
    -UseMSVCCompilers C `
    -BuildTargets default `
    -Defines ($PlatformDefines + @{
      BUILD_SHARED_LIBS = "NO";
      BUILD_TESTING = "NO";
      CMAKE_INSTALL_LIBDIR = "lib/$Platform/$ArchName";
      CMAKE_SYSTEM_NAME = $Platform.ToString();
      BUILD_CURL_EXE = "NO";
      CURL_CA_BUNDLE = "none";
      CURL_CA_FALLBACK = "NO";
      CURL_CA_PATH = "none";
      CURL_BROTLI = "NO";
      CURL_DISABLE_ALTSVC = "NO";
      CURL_DISABLE_AWS = "YES";
      CURL_DISABLE_BASIC_AUTH = "NO";
      CURL_DISABLE_BEARER_AUTH = "NO";
      CURL_DISABLE_COOKIES = "NO";
      CURL_DISABLE_DICT = "YES";
      CURL_DISABLE_DIGEST_AUTH = "NO";
      CURL_DISABLE_DOH = "NO";
      CURL_DISABLE_FILE = "YES";
      CURL_DISABLE_FORM_API = "NO";
      CURL_DISABLE_FTP = "YES";
      CURL_DISABLE_GETOPTIONS = "NO";
      CURL_DISABLE_GOPHER = "YES";
      CURL_DISABLE_HSTS = "NO";
      CURL_DISABLE_HTTP = "NO";
      CURL_DISABLE_HTTP_AUTH = "NO";
      CURL_DISABLE_IMAP = "YES";
      CURL_DISABLE_KERBEROS_AUTH = "NO";
      CURL_DISABLE_LDAP = "YES";
      CURL_DISABLE_LDAPS = "YES";
      CURL_DISABLE_MIME = "NO";
      CURL_DISABLE_MQTT = "YES";
      CURL_DISABLE_NEGOTIATE_AUTH = "NO";
      CURL_DISABLE_NETRC = "NO";
      CURL_DISABLE_NTLM = "NO";
      CURL_DISABLE_PARSEDATE = "NO";
      CURL_DISABLE_POP3 = "YES";
      CURL_DISABLE_PROGRESS_METER = "YES";
      CURL_DISABLE_PROXY = "NO";
      CURL_DISABLE_RTSP = "YES";
      CURL_DISABLE_SHUFFLE_DNS = "YES";
      CURL_DISABLE_SMB = "YES";
      CURL_DISABLE_SMTP = "YES";
      CURL_DISABLE_SOCKETPAIR = "YES";
      CURL_DISABLE_SRP = "NO";
      CURL_DISABLE_TELNET = "YES";
      CURL_DISABLE_TFTP = "YES";
      CURL_DISABLE_VERBOSE_STRINGS = "NO";
      CURL_LTO = "NO";
      CURL_USE_BEARSSL = "NO";
      CURL_USE_GNUTLS = "NO";
      CURL_USE_GSSAPI = "NO";
      CURL_USE_LIBPSL = "NO";
      CURL_USE_LIBSSH = "NO";
      CURL_USE_LIBSSH2 = "NO";
      CURL_USE_MBEDTLS = "NO";
      CURL_USE_OPENSSL = "NO";
      CURL_USE_SCHANNEL = if ($Platform -eq "Windows") { "YES" } else { "NO" };
      CURL_USE_WOLFSSL = "NO";
      CURL_WINDOWS_SSPI = if ($Platform -eq "Windows") { "YES" } else { "NO" };
      CURL_ZLIB = "YES";
      CURL_ZSTD = "NO";
      ENABLE_ARES = "NO";
      ENABLE_CURLDEBUG = "NO";
      ENABLE_DEBUG = "NO";
      ENABLE_IPV6 = "YES";
      ENABLE_MANUAL = "NO";
      ENABLE_THREADED_RESOLVER = "NO";
      ENABLE_UNICODE = "YES";
      ENABLE_UNIX_SOCKETS = "NO";
      ENABLE_WEBSOCKETS = "YES";
      HAVE_POLL_FINE = "NO";
      USE_IDN2 = "NO";
      USE_MSH3 = "NO";
      USE_NGHTTP2 = "NO";
      USE_NGTCP2 = "NO";
      USE_QUICHE = "NO";
      USE_WIN32_IDN = if ($Platform -eq "Windows") { "YES" } else { "NO" };
      USE_WIN32_LARGE_FILES = if ($Platform -eq "Windows") { "YES" } else { "NO" };
      USE_WIN32_LDAP = "NO";
      ZLIB_ROOT = "$LibraryRoot\zlib-1.3.1\usr";
      ZLIB_LIBRARY = "$LibraryRoot\zlib-1.3.1\usr\lib\$Platform\$ArchName\zlibstatic.lib";
    })
}

function Build-ICU([Platform]$Platform, $Arch) {
  $ArchName = $Arch.LLVMName

  if (-not $ToBatch) {
    if (-not (Test-Path -Path "$SourceCache\icu\icu4c\CMakeLists.txt")) {
      Copy-Item $SourceCache\swift-installer-scripts\shared\ICU\CMakeLists.txt $SourceCache\icu\icu4c\
      Copy-Item $SourceCache\swift-installer-scripts\shared\ICU\icupkg.inc.cmake $SourceCache\icu\icu4c\
    }
  }

  if ($Platform -eq "Windows" -and (($Arch.CMakeName -eq $BuildArch.CMakeName) -or ($Arch.CMakeName -ne "ARM64"))) {
    $BuildToolsDefines = @{BUILD_TOOLS = "YES"}
  } else {
    $BuildToolsDefines = @{
      BUILD_TOOLS = "NO";
      BUILD_DATA = if ($Platform -eq "Android") { "NO" } else { "YES" };
      ICU_TOOLS_DIR = "$($BuildArch.BinaryCache)\windows\icu-69.1"
    }
  }

  Build-CMakeProject `
    -Src $SourceCache\icu\icu4c `
    -Bin "$($Arch.BinaryCache)\$Platform\icu-69.1" `
    -InstallTo "$LibraryRoot\icu-69.1\usr" `
    -Arch $Arch `
    -Platform $Platform `
    -UseMSVCCompilers C,CXX `
    -BuildTargets default `
    -Defines ($BuildToolsDefines + @{
      BUILD_SHARED_LIBS = "NO";
      CMAKE_SYSTEM_NAME = $Platform.ToString();
      CMAKE_POSITION_INDEPENDENT_CODE = "YES";
      CMAKE_INSTALL_BINDIR = "bin/$Platform/$ArchName";
      CMAKE_INSTALL_LIBDIR = "lib/$Platform/$ArchName";
    })
}

function Build-Runtime([Platform]$Platform, $Arch) {
  $PlatformDefines = @{}
  if ($Platform -eq "Android") {
    $PlatformDefines += @{
      LLVM_ENABLE_LIBCXX = "YES";
      SWIFT_USE_LINKER = "lld";
    }
  }


  Isolate-EnvVars {
    $env:Path = "$($BuildArch.BinaryCache)\cmark-gfm-0.29.0.gfm.13\src;$(Get-PinnedToolchainRuntime);${env:Path}"

    $CompilersBinaryCache = if ($IsCrossCompiling) {
      Get-BuildProjectBinaryCache Compilers
    } else {
      Get-HostProjectBinaryCache Compilers
    }

    Build-CMakeProject `
      -Src $SourceCache\swift `
      -Bin (Get-TargetProjectBinaryCache $Arch Runtime) `
      -InstallTo "$($Arch.SDKInstallRoot)\usr" `
      -Arch $Arch `
      -Platform $Platform `
      -CacheScript $SourceCache\swift\cmake\caches\Runtime-$Platform-$($Arch.LLVMName).cmake `
      -UseBuiltCompilers C,CXX,Swift `
      -BuildTargets default `
      -Defines ($PlatformDefines + @{
        CMAKE_Swift_COMPILER_TARGET = $Arch.LLVMTarget.Replace("$AndroidAPILevel", "");
        CMAKE_Swift_COMPILER_WORKS = "YES";
        CMAKE_SYSTEM_NAME = $Platform.ToString();
        LLVM_DIR = "$(Get-TargetProjectBinaryCache $Arch LLVM)\lib\cmake\llvm";
        SWIFT_ENABLE_EXPERIMENTAL_CONCURRENCY = "YES";
        SWIFT_ENABLE_EXPERIMENTAL_CXX_INTEROP = "YES";
        SWIFT_ENABLE_EXPERIMENTAL_DIFFERENTIABLE_PROGRAMMING = "YES";
        SWIFT_ENABLE_EXPERIMENTAL_DISTRIBUTED = "YES";
        SWIFT_ENABLE_EXPERIMENTAL_OBSERVATION = "YES";
        SWIFT_ENABLE_EXPERIMENTAL_STRING_PROCESSING = "YES";
        # FIXME: re-enable after https://github.com/apple/swift/issues/74186 is fixed.
        SWIFT_ENABLE_SYNCHRONIZATION = if (($Platform -eq "Android") -and ($Arch -eq $AndroidARMv7)) { "NO" } else { "YES" };
        SWIFT_ENABLE_VOLATILE = "YES";
        SWIFT_NATIVE_SWIFT_TOOLS_PATH = (Join-Path -Path $CompilersBinaryCache -ChildPath "bin");
        SWIFT_PATH_TO_LIBDISPATCH_SOURCE = "$SourceCache\swift-corelibs-libdispatch";
        SWIFT_PATH_TO_STRING_PROCESSING_SOURCE = "$SourceCache\swift-experimental-string-processing";
        CMAKE_SHARED_LINKER_FLAGS = if ($Platform -eq "Windows") { @("/INCREMENTAL:NO", "/OPT:REF", "/OPT:ICF") } else { @() };
      })
  }

  Invoke-Program $python -c "import plistlib; print(str(plistlib.dumps({ 'DefaultProperties': { 'DEFAULT_USE_RUNTIME': 'MD' } }), encoding='utf-8'))" `
    -OutFile "$($Arch.SDKInstallRoot)\SDKSettings.plist"
}

function Build-Dispatch([Platform]$Platform, $Arch, [switch]$Test = $false) {
  if ($Test) {
    $Targets = @("default", "ExperimentalTest")
    $InstallPath = ""
  } else {
    $Targets = @("default", "install")
    $InstallPath = "$($Arch.SDKInstallRoot)\usr"
  }

  Build-CMakeProject `
    -Src $SourceCache\swift-corelibs-libdispatch `
    -Bin (Get-TargetProjectBinaryCache $Arch Dispatch) `
    -InstallTo $InstallPath `
    -Arch $Arch `
    -Platform $Platform `
    -UseBuiltCompilers C,CXX,Swift `
    -BuildTargets $Targets `
    -Defines @{
      ENABLE_SWIFT = "YES";
    }
}

function Build-Foundation([Platform]$Platform, $Arch, [switch]$Test = $false) {
  $DispatchBinaryCache = Get-TargetProjectBinaryCache $Arch Dispatch
  $SwiftSyntaxDir = Get-HostProjectCMakeModules Compilers
  $FoundationBinaryCache = Get-TargetProjectBinaryCache $Arch Foundation
  $ShortArch = $Arch.LLVMName

  Isolate-EnvVars {
    if ($Test) {
      $XCTestBinaryCache = Get-TargetProjectBinaryCache $Arch XCTest
      $TestingDefines = @{
        ENABLE_TESTING = "YES";
        XCTest_DIR = "$XCTestBinaryCache\cmake\modules";
      }
      $Targets = @("default", "test")
      $env:Path = "$XCTestBinaryCache;$FoundationBinaryCache\bin;$DispatchBinaryCache;$(Get-TargetProjectBinaryCache $Arch Runtime)\bin;$env:Path"
      $InstallPath = ""
    } else {
      $TestingDefines = @{ ENABLE_TESTING = "NO" }
      $Targets = @("default", "install")
      $InstallPath = "$($Arch.SDKInstallRoot)\usr"
    }

    $env:CTEST_OUTPUT_ON_FAILURE = 1
    Build-CMakeProject `
      -Src $SourceCache\swift-corelibs-foundation `
      -Bin $FoundationBinaryCache `
      -InstallTo $InstallPath `
      -Arch $Arch `
      -Platform $Platform `
      -UseBuiltCompilers ASM,C,CXX,Swift `
      -BuildTargets $Targets `
      -Defines (@{
        FOUNDATION_BUILD_TOOLS = if ($Platform -eq "Windows") { "YES" } else { "NO" };
        # Turn off safeseh for lld as it has safeseh enabled by default
        # and fails with an ICU data object file icudt69l_dat.obj. This
        # matters to X86 only.
        CMAKE_Swift_FLAGS = if ($Arch -eq $ArchX86) { @("-Xlinker", "/SAFESEH:NO") } else { "" };
        CURL_DIR = "$LibraryRoot\curl-8.5.0\usr\lib\$Platform\$ShortArch\cmake\CURL";
        ICU_DATA_LIBRARY_RELEASE = if ($Platform -eq "Windows") {
          "$LibraryRoot\icu-69.1\usr\lib\$Platform\$ShortArch\sicudt69.lib"
        } else {
          "$LibraryRoot\icu-69.1\usr\lib\$Platform\$ShortArch\libicudt69.a"
        };
        ICU_I18N_LIBRARY_RELEASE = if ($Platform -eq "Windows") {
          "$LibraryRoot\icu-69.1\usr\lib\$Platform\$ShortArch\sicuin69.lib"
        } else {
          "$LibraryRoot\icu-69.1\usr\lib\$Platform\$ShortArch\libicuin69.a"
        };
        ICU_ROOT = "$LibraryRoot\icu-69.1\usr";
        ICU_INCLUDE_DIR = "$LibraryRoot\icu-69.1\usr\include";
        ICU_UC_LIBRARY_RELEASE = if ($Platform -eq "Windows") {
          "$LibraryRoot\icu-69.1\usr\lib\$Platform\$ShortArch\sicuuc69.lib";
        } else {
          "$LibraryRoot\icu-69.1\usr\lib\$Platform\$ShortArch\libicuuc69.a"
        };
        LIBXML2_LIBRARY = if ($Platform -eq "Windows") {
          "$LibraryRoot\libxml2-2.11.5\usr\lib\$Platform\$ShortArch\libxml2s.lib";
        } else {
          "$LibraryRoot\libxml2-2.11.5\usr\lib\$Platform\$ShortArch\libxml2.a";
        };
        LIBXML2_INCLUDE_DIR = "$LibraryRoot\libxml2-2.11.5\usr\include\libxml2";
        LIBXML2_DEFINITIONS = "-DLIBXML_STATIC";
        ZLIB_LIBRARY = if ($Platform -eq "Windows") {
          "$LibraryRoot\zlib-1.3.1\usr\lib\$Platform\$ShortArch\zlibstatic.lib"
        } else {
          "$LibraryRoot\zlib-1.3.1\usr\lib\$Platform\$ShortArch\libz.a"
        };
        ZLIB_INCLUDE_DIR = "$LibraryRoot\zlib-1.3.1\usr\include";
        dispatch_DIR = "$DispatchBinaryCache\cmake\modules";
        SwiftSyntax_DIR = "$SwiftSyntaxDir";
        _SwiftFoundation_SourceDIR = "$SourceCache\swift-foundation";
        _SwiftFoundationICU_SourceDIR = "$SourceCache\swift-foundation-icu";
        _SwiftCollections_SourceDIR = "$SourceCache\swift-collections"
      } + $TestingDefines)
  }
}

function Build-XCTest([Platform]$Platform, $Arch, [switch]$Test = $false) {
  $DispatchBinaryCache = Get-TargetProjectBinaryCache $Arch Dispatch
  $FoundationBinaryCache = Get-TargetProjectBinaryCache $Arch Foundation
  $XCTestBinaryCache = Get-TargetProjectBinaryCache $Arch XCTest

  Isolate-EnvVars {
    if ($Test) {
      $TestingDefines = @{
        ENABLE_TESTING = "YES";
        LLVM_DIR = "$(Get-TargetProjectBinaryCache $Arch LLVM)/lib/cmake/llvm";
        XCTEST_PATH_TO_LIBDISPATCH_BUILD = $DispatchBinaryCache;
        XCTEST_PATH_TO_LIBDISPATCH_SOURCE = "$SourceCache\swift-corelibs-libdispatch";
        XCTEST_PATH_TO_FOUNDATION_BUILD = $FoundationBinaryCache;
      }
      $Targets = @("default", "check-xctest")
      $InstallPath = ""
      $env:Path = "$XCTestBinaryCache;$FoundationBinaryCache\bin;$DispatchBinaryCache;$(Get-TargetProjectBinaryCache $Arch Runtime)\bin;$env:Path;$UnixToolsBinDir"
    } else {
      $TestingDefines = @{ ENABLE_TESTING = "NO" }
      $Targets = @("default", "install")
      $InstallPath = "$($Arch.XCTestInstallRoot)\usr"
    }

    Build-CMakeProject `
      -Src $SourceCache\swift-corelibs-xctest `
      -Bin $XCTestBinaryCache `
      -InstallTo $InstallPath `
      -Arch $Arch `
      -Platform $Platform `
      -UseBuiltCompilers Swift `
      -BuildTargets $Targets `
      -Defines (@{
        CMAKE_BUILD_WITH_INSTALL_RPATH = "YES";
        dispatch_DIR = "$DispatchBinaryCache\cmake\modules";
        Foundation_DIR = "$FoundationBinaryCache\cmake\modules";
      } + $TestingDefines)

    $PList = Join-Path -Path $Arch.PlatformInstallRoot -ChildPath "Info.plist"
    Invoke-Program $python -c "import plistlib; print(str(plistlib.dumps({ 'DefaultProperties': { 'XCTEST_VERSION': 'development', 'SWIFTC_FLAGS': ['-use-ld=lld'] } }), encoding='utf-8'))" `
      -OutFile "$PList"
  }
}

# Copies files installed by CMake from the arch-specific platform root,
# where they follow the layout expected by the installer,
# to the final platform root, following the installer layout.
function Install-Platform([Platform]$Platform, $Arch) {
  if ($ToBatch) { return }

  $SDKInstallRoot = [IO.Path]::Combine((Get-InstallDir $HostArch), "Platforms", "$Platform.platform", "Developer", "SDKs", "$Platform.sdk")

  New-Item -ItemType Directory -ErrorAction Ignore $SDKInstallRoot\usr | Out-Null

  # Copy SDK header files
  Copy-Directory "$($Arch.SDKInstallRoot)\usr\include\swift\SwiftRemoteMirror" $SDKInstallRoot\usr\include\swift
  Copy-Directory "$($Arch.SDKInstallRoot)\usr\lib\swift\shims" $SDKInstallRoot\usr\lib\swift
  foreach ($Module in ("Block", "dispatch", "os")) {
    Copy-Directory "$($Arch.SDKInstallRoot)\usr\lib\swift\$Module" $SDKInstallRoot\usr\include
  }

  # Copy SDK share folder
  Copy-File "$($Arch.SDKInstallRoot)\usr\share\*.*" $SDKInstallRoot\usr\share\

  # Copy SDK libs, placing them in an arch-specific directory
  $PlatformLibSrc = "$($Arch.SDKInstallRoot)\usr\lib\swift\$($Platform.ToString().ToLowerInvariant())"
  $PlatformLibDst = "$SDKInstallRoot\usr\lib\swift\$($Platform.ToString().ToLowerInvariant())"

  switch ($Platform) {
    Windows {
      Copy-File "$PlatformLibSrc\*.lib" "$PlatformLibDst\$($Arch.LLVMName)\"
      Copy-File "$PlatformLibSrc\$($Arch.LLVMName)\*.lib" "$PlatformLibDst\$($Arch.LLVMName)\"
    }
    Android {
      Copy-File "$PlatformLibSrc\*.a" "$PlatformLibDst\$($Arch.LLVMName)\"
      Copy-File "$PlatformLibSrc\*.so" "$PlatformLibDst\$($Arch.LLVMName)\"
    }
  }

  # Copy well-structured SDK modules
  Copy-Directory "$PlatformLibSrc\*.swiftmodule" "$PlatformLibDst\"

  # Copy files from the arch subdirectory, including "*.swiftmodule" which need restructuring
  Get-ChildItem -Recurse "$PlatformLibSrc\$($Arch.LLVMName)" | ForEach-Object {
    if (".swiftmodule", ".swiftdoc", ".swiftinterface" -contains $_.Extension) {
      $DstDir = "$PlatformLibDst\$($_.BaseName).swiftmodule"
      Copy-File $_.FullName "$DstDir\$($Arch.LLVMTarget)$($_.Extension)"
    } else {
      Copy-File $_.FullName "$PlatformLibDst\$($Arch.LLVMName)\"
    }
  }

  # Copy the CxxShim module
  foreach ($Source in ("libcxxshim.h", "libcxxshim.modulemap", "libcxxstdlibshim.h")) {
    Copy-File "$PlatformLibSrc\$Source" "$PlatformLibDst"
  }

  # Copy plist files (same across architectures)
  Copy-File "$($Arch.PlatformInstallRoot)\Info.plist" ([IO.Path]::Combine((Get-InstallDir $HostArch), "Platforms", "${Platform}.platform"))
  Copy-File "$($Arch.SDKInstallRoot)\SDKSettings.plist" ([IO.Path]::Combine((Get-InstallDir $HostArch), "Platforms", "${Platform}.platform", "Developer", "SDKs", "${Platform}.sdk"))

  # Copy XCTest
  $XCTestInstallRoot = [IO.Path]::Combine((Get-InstallDir $HostArch), "Platforms", "${Platform}.platform", "Developer", "Library", "XCTest-development")
  switch ($Platform) {
    Windows {
      Copy-File "$($Arch.XCTestInstallRoot)\usr\bin\XCTest.dll" "$XCTestInstallRoot\usr\$($Arch.BinaryDir)\"
      Copy-File "$($Arch.XCTestInstallRoot)\usr\lib\swift\windows\XCTest.lib" "$XCTestInstallRoot\usr\lib\swift\windows\$($Arch.LLVMName)\"
    }
    default {
      Copy-File "$($Arch.XCTestInstallRoot)\usr\lib\swift\$($Platform.ToString().ToLowerInvariant())\libXCTest.so" "$XCTestInstallRoot\usr\lib\$($Arch.BinaryDir)\"
    }
  }
  Copy-File "$($Arch.XCTestInstallRoot)\usr\lib\swift\$($Platform.ToString().ToLowerInvariant())\$($Arch.LLVMName)\XCTest.swiftmodule" "$XCTestInstallRoot\usr\lib\swift\$($Platform.ToString().ToLowerInvariant())\XCTest.swiftmodule\$($Arch.LLVMTarget).swiftmodule"
  Copy-File "$($Arch.XCTestInstallRoot)\usr\lib\swift\$($Platform.ToString().ToLowerInvariant())\$($Arch.LLVMName)\XCTest.swiftdoc" "$XCTestInstallRoot\usr\lib\swift\$($Platform.ToString().ToLowerInvariant())\XCTest.swiftmodule\$($Arch.LLVMTarget).swiftdoc"
}

function Build-SQLite($Arch) {
  $SrcPath = "$SourceCache\sqlite-3.43.2"

  # Download the sources
  if (-not (Test-Path $SrcPath)) {
    $ZipPath = "$env:TEMP\sqlite-amalgamation-3430200.zip"
    if (-not $ToBatch) { Remove-item $ZipPath -ErrorAction Ignore | Out-Null }
    Invoke-Program curl.exe -- -sL https://sqlite.org/2023/sqlite-amalgamation-3430200.zip -o $ZipPath

    if (-not $ToBatch) { New-Item -Type Directory -Path $SrcPath -ErrorAction Ignore | Out-Null }
    Invoke-Program "$UnixToolsBinDir\unzip.exe" -- -j -o $ZipPath -d $SrcPath
    if (-not $ToBatch) { Remove-item $ZipPath | Out-Null }

    if (-not $ToBatch) {
      # Inject a CMakeLists.txt so we can build sqlite
@"
cmake_minimum_required(VERSION 3.12.3)
project(SQLite LANGUAGES C)

set(CMAKE_POSITION_INDEPENDENT_CODE YES)
add_library(SQLite3 sqlite3.c)

if(CMAKE_SYSTEM_NAME STREQUAL Windows AND BUILD_SHARED_LIBS)
  target_compile_definitions(SQLite3 PRIVATE "SQLITE_API=__declspec(dllexport)")
endif()

install(TARGETS SQLite3
  ARCHIVE DESTINATION lib
  LIBRARY DESTINATION lib
  RUNTIME DESTINATION bin)
install(FILES sqlite3.h sqlite3ext.h DESTINATION include)
"@ | Out-File -Encoding UTF8 $SrcPath\CMakeLists.txt
    }
  }

  Build-CMakeProject `
    -Src $SrcPath `
    -Bin "$($Arch.BinaryCache)\sqlite-3.43.2" `
    -InstallTo $LibraryRoot\sqlite-3.43.2\usr `
    -Arch $Arch `
    -UseMSVCCompilers C `
    -BuildTargets default `
    -Defines @{
      BUILD_SHARED_LIBS = "NO";
    }
}

function Build-System($Arch) {
  Build-CMakeProject `
    -Src $SourceCache\swift-system `
    -Bin (Get-HostProjectBinaryCache System) `
    -InstallTo "$($Arch.ToolchainInstallRoot)\usr" `
    -Arch $Arch `
    -Platform Windows `
    -UseBuiltCompilers C,Swift `
    -SwiftSDK (Get-HostSwiftSDK) `
    -BuildTargets default `
    -Defines @{
      BUILD_SHARED_LIBS = "YES";
    }

  if (-not $ToBatch) {
    # Remove unnecessary "S:\Program Files\swift\Toolchains\0.0.0+Asserts\usr\include\CSystem"
    Remove-Item -Force -Recurse "$($Arch.ToolchainInstallRoot)\usr\include\CSystem" -ErrorAction Ignore | Out-Null
  }
}

function Build-ToolsSupportCore($Arch) {
  Build-CMakeProject `
    -Src $SourceCache\swift-tools-support-core `
    -Bin (Get-HostProjectBinaryCache ToolsSupportCore) `
    -InstallTo "$($Arch.ToolchainInstallRoot)\usr" `
    -Arch $Arch `
    -Platform Windows `
    -UseBuiltCompilers C,Swift `
    -SwiftSDK (Get-HostSwiftSDK) `
    -BuildTargets default `
    -Defines @{
      BUILD_SHARED_LIBS = "YES";
      SwiftSystem_DIR = (Get-HostProjectCMakeModules System);
    }
}

function Build-LLBuild($Arch, [switch]$Test = $false) {
  Isolate-EnvVars {
    if ($Test) {
      # Build additional llvm executables needed by tests
      Isolate-EnvVars {
        Invoke-VsDevShell $HostArch
        Invoke-Program ninja.exe -C (Get-BuildProjectBinaryCache BuildTools) FileCheck not
      }

      $Targets = @("default", "test-llbuild")
      $TestingDefines = @{
        FILECHECK_EXECUTABLE = ([IO.Path]::Combine((Get-BuildProjectBinaryCache BuildTools), "bin", "FileCheck.exe"));
        LIT_EXECUTABLE = "$SourceCache\llvm-project\llvm\utils\lit\lit.py";
      }
      $env:Path = "$env:Path;$UnixToolsBinDir"
      $env:AR = ([IO.Path]::Combine((Get-HostProjectBinaryCache Compilers), "bin", "llvm-ar.exe"))
      $env:CLANG = ([IO.Path]::Combine((Get-HostProjectBinaryCache Compilers), "bin", "clang.exe"))
    } else {
      $Targets = @("default", "install")
      $TestingDefines = @{}
    }

    Build-CMakeProject `
      -Src $SourceCache\llbuild `
      -Bin (Get-HostProjectBinaryCache LLBuild) `
      -InstallTo "$($Arch.ToolchainInstallRoot)\usr" `
      -Arch $Arch `
      -Platform Windows `
      -UseMSVCCompilers CXX `
      -UseBuiltCompilers Swift `
      -SwiftSDK (Get-HostSwiftSDK) `
      -BuildTargets $Targets `
      -Defines ($TestingDefines + @{
        BUILD_SHARED_LIBS = "YES";
        LLBUILD_SUPPORT_BINDINGS = "Swift";
        SQLite3_INCLUDE_DIR = "$LibraryRoot\sqlite-3.43.2\usr\include";
        SQLite3_LIBRARY = "$LibraryRoot\sqlite-3.43.2\usr\lib\SQLite3.lib";
      })
  }
}

function Build-Yams($Arch) {
  Build-CMakeProject `
    -Src $SourceCache\Yams `
    -Bin (Get-HostProjectBinaryCache Yams) `
    -Arch $Arch `
    -Platform Windows `
    -UseBuiltCompilers C,Swift `
    -SwiftSDK (Get-HostSwiftSDK) `
    -BuildTargets default `
    -Defines @{
      BUILD_SHARED_LIBS = "NO";
      BUILD_TESTING = "NO";
    }
}

function Build-ArgumentParser($Arch) {
  Build-CMakeProject `
    -Src $SourceCache\swift-argument-parser `
    -Bin (Get-HostProjectBinaryCache ArgumentParser) `
    -InstallTo "$($Arch.ToolchainInstallRoot)\usr" `
    -Arch $Arch `
    -Platform Windows `
    -UseBuiltCompilers Swift `
    -SwiftSDK (Get-HostSwiftSDK) `
    -BuildTargets default `
    -Defines @{
      BUILD_SHARED_LIBS = "YES";
      BUILD_TESTING = "NO";
    }
}

function Build-Driver($Arch) {
  Build-CMakeProject `
    -Src $SourceCache\swift-driver `
    -Bin (Get-HostProjectBinaryCache Driver) `
    -InstallTo "$($Arch.ToolchainInstallRoot)\usr" `
    -Arch $Arch `
    -Platform Windows `
    -UseBuiltCompilers C,CXX,Swift `
    -SwiftSDK (Get-HostSwiftSDK) `
    -BuildTargets default `
    -Defines @{
      BUILD_SHARED_LIBS = "YES";
      SwiftSystem_DIR = (Get-HostProjectCMakeModules System);
      TSC_DIR = (Get-HostProjectCMakeModules ToolsSupportCore);
      LLBuild_DIR = (Get-HostProjectCMakeModules LLBuild);
      Yams_DIR = (Get-HostProjectCMakeModules Yams);
      ArgumentParser_DIR = (Get-HostProjectCMakeModules ArgumentParser);
      SQLite3_INCLUDE_DIR = "$LibraryRoot\sqlite-3.43.2\usr\include";
      SQLite3_LIBRARY = "$LibraryRoot\sqlite-3.43.2\usr\lib\SQLite3.lib";
      SWIFT_DRIVER_BUILD_TOOLS = "YES";
      LLVM_DIR = "$(Get-HostProjectBinaryCache Compilers)\lib\cmake\llvm";
      Clang_DIR = "$(Get-HostProjectBinaryCache Compilers)\lib\cmake\clang";
      Swift_DIR = "$(Get-HostProjectBinaryCache Compilers)\tools\swift\lib\cmake\swift";
      CMAKE_CXX_FLAGS = "-Xclang -fno-split-cold-code";
    }
}

function Build-Crypto($Arch) {
  Build-CMakeProject `
    -Src $SourceCache\swift-crypto `
    -Bin (Get-HostProjectBinaryCache Crypto) `
    -Arch $Arch `
    -Platform Windows `
    -UseBuiltCompilers Swift `
    -SwiftSDK (Get-HostSwiftSDK) `
    -BuildTargets default `
    -Defines @{
      BUILD_SHARED_LIBS = "NO";
    }
}

function Build-Collections($Arch) {
  Build-CMakeProject `
    -Src $SourceCache\swift-collections `
    -Bin (Get-HostProjectBinaryCache Collections) `
    -InstallTo "$($Arch.ToolchainInstallRoot)\usr" `
    -Arch $Arch `
    -Platform Windows `
    -UseBuiltCompilers C,Swift `
    -SwiftSDK (Get-HostSwiftSDK) `
    -BuildTargets default `
    -Defines @{
      BUILD_SHARED_LIBS = "YES";
    }
}

function Build-ASN1($Arch) {
  Build-CMakeProject `
    -Src $SourceCache\swift-asn1 `
    -Bin (Get-HostProjectBinaryCache ASN1) `
    -Arch $Arch `
    -UseBuiltCompilers C,Swift `
    -SwiftSDK (Get-HostSwiftSDK) `
    -BuildTargets default `
    -Defines @{
      BUILD_SHARED_LIBS = "NO";
    }
}

function Build-Certificates($Arch) {
  Build-CMakeProject `
    -Src $SourceCache\swift-certificates `
    -Bin (Get-HostProjectBinaryCache Certificates) `
    -Arch $Arch `
    -Platform Windows `
    -UseBuiltCompilers Swift `
    -SwiftSDK (Get-HostSwiftSDK) `
    -BuildTargets default `
    -Defines @{
      BUILD_SHARED_LIBS = "NO";
      SwiftCrypto_DIR = (Get-HostProjectCMakeModules Crypto);
      SwiftASN1_DIR = (Get-HostProjectCMakeModules ASN1);
    }
}

function Build-PackageManager($Arch) {
  $SrcDir = if (Test-Path -Path "$SourceCache\swift-package-manager" -PathType Container) {
    "$SourceCache\swift-package-manager"
  } else {
    "$SourceCache\swiftpm"
  }

  Build-CMakeProject `
    -Src $SrcDir `
    -Bin (Get-HostProjectBinaryCache PackageManager) `
    -InstallTo "$($Arch.ToolchainInstallRoot)\usr" `
    -Arch $Arch `
    -Platform Windows `
    -UseBuiltCompilers C,Swift `
    -SwiftSDK (Get-HostSwiftSDK) `
    -BuildTargets default `
    -Defines @{
      BUILD_SHARED_LIBS = "YES";
      CMAKE_Swift_FLAGS = @("-DCRYPTO_v2");
      SwiftSystem_DIR = (Get-HostProjectCMakeModules System);
      TSC_DIR = (Get-HostProjectCMakeModules ToolsSupportCore);
      LLBuild_DIR = (Get-HostProjectCMakeModules LLBuild);
      ArgumentParser_DIR = (Get-HostProjectCMakeModules ArgumentParser);
      SwiftDriver_DIR = (Get-HostProjectCMakeModules Driver);
      SwiftCrypto_DIR = (Get-HostProjectCMakeModules Crypto);
      SwiftCollections_DIR = (Get-HostProjectCMakeModules Collections);
      SwiftASN1_DIR = (Get-HostProjectCMakeModules ASN1);
      SwiftCertificates_DIR = (Get-HostProjectCMakeModules Certificates);
      SwiftSyntax_DIR = (Get-HostProjectCMakeModules Compilers);
      SQLite3_INCLUDE_DIR = "$LibraryRoot\sqlite-3.43.2\usr\include";
      SQLite3_LIBRARY = "$LibraryRoot\sqlite-3.43.2\usr\lib\SQLite3.lib";
    }
}

function Build-Markdown($Arch) {
  Build-CMakeProject `
    -Src $SourceCache\swift-markdown `
    -Bin (Get-HostProjectBinaryCache Markdown) `
    -InstallTo "$($Arch.ToolchainInstallRoot)\usr" `
    -Arch $Arch `
    -Platform Windows `
    -UseBuiltCompilers C,Swift `
    -SwiftSDK (Get-HostSwiftSDK) `
    -BuildTargets default `
    -Defines @{
      BUILD_SHARED_LIBS = "NO";
      ArgumentParser_DIR = (Get-HostProjectCMakeModules ArgumentParser);
      "cmark-gfm_DIR" = "$($Arch.ToolchainInstallRoot)\usr\lib\cmake";
    }
}

function Build-Format($Arch) {
  Build-CMakeProject `
    -Src $SourceCache\swift-format `
    -Bin (Get-HostProjectBinaryCache Format) `
    -InstallTo "$($Arch.ToolchainInstallRoot)\usr" `
    -Arch $Arch `
    -Platform Windows `
    -UseMSVCCompilers C `
    -UseBuiltCompilers Swift `
    -SwiftSDK (Get-HostSwiftSDK) `
    -BuildTargets default `
    -Defines @{
      BUILD_SHARED_LIBS = "YES";
      ArgumentParser_DIR = (Get-HostProjectCMakeModules ArgumentParser);
      SwiftSyntax_DIR = (Get-HostProjectCMakeModules Compilers);
      SwiftMarkdown_DIR = (Get-HostProjectCMakeModules Markdown);
      "cmark-gfm_DIR" = "$($Arch.ToolchainInstallRoot)\usr\lib\cmake";
    }
}

function Build-IndexStoreDB($Arch) {
  $SDKInstallRoot = (Get-HostSwiftSDK);

  Build-CMakeProject `
    -Src $SourceCache\indexstore-db `
    -Bin (Get-HostProjectBinaryCache IndexStoreDB) `
    -Arch $Arch `
    -Platform Windows `
    -UseBuiltCompilers C,CXX,Swift `
    -SwiftSDK $SDKInstallRoot `
    -BuildTargets default `
    -Defines @{
      BUILD_SHARED_LIBS = "NO";
      CMAKE_C_FLAGS = @("-Xclang", "-fno-split-cold-code", "-I$SDKInstallRoot\usr\include", "-I$SDKInstallRoot\usr\include\Block");
      CMAKE_CXX_FLAGS = @("-Xclang", "-fno-split-cold-code", "-I$SDKInstallRoot\usr\include", "-I$SDKInstallRoot\usr\include\Block");
    }
}

function Build-SourceKitLSP($Arch) {
  Build-CMakeProject `
    -Src $SourceCache\sourcekit-lsp `
    -Bin (Get-HostProjectBinaryCache SourceKitLSP) `
    -InstallTo "$($Arch.ToolchainInstallRoot)\usr" `
    -Arch $Arch `
    -Platform Windows `
    -UseBuiltCompilers C,Swift `
    -SwiftSDK (Get-HostSwiftSDK) `
    -BuildTargets default `
    -Defines @{
      SwiftSyntax_DIR = (Get-HostProjectCMakeModules Compilers);
      SwiftSystem_DIR = (Get-HostProjectCMakeModules System);
      TSC_DIR = (Get-HostProjectCMakeModules ToolsSupportCore);
      LLBuild_DIR = (Get-HostProjectCMakeModules LLBuild);
      ArgumentParser_DIR = (Get-HostProjectCMakeModules ArgumentParser);
      SwiftCrypto_DIR = (Get-HostProjectCMakeModules Crypto);
      SwiftCollections_DIR = (Get-HostProjectCMakeModules Collections);
      SwiftPM_DIR = (Get-HostProjectCMakeModules PackageManager);
      IndexStoreDB_DIR = (Get-HostProjectCMakeModules IndexStoreDB);
    }
}

function Install-HostToolchain() {
  if ($ToBatch) { return }

  # We've already special-cased $HostArch.ToolchainInstallRoot to point to $ToolchainInstallRoot.
  # There are only a few extra restructuring steps we need to take care of.

  # Restructure _InternalSwiftScan (keep the original one for the installer)
  Copy-Item -Force `
    "$($HostArch.ToolchainInstallRoot)\usr\lib\swift\_InternalSwiftScan" `
    "$($HostArch.ToolchainInstallRoot)\usr\include"
  Copy-Item -Force `
    "$($HostArch.ToolchainInstallRoot)\usr\lib\swift\windows\_InternalSwiftScan.lib" `
    "$($HostArch.ToolchainInstallRoot)\usr\lib"

  # Switch to swift-driver
  $SwiftDriver = ([IO.Path]::Combine((Get-HostProjectBinaryCache Driver), "bin", "swift-driver.exe"))
  Copy-Item -Force $SwiftDriver "$($HostArch.ToolchainInstallRoot)\usr\bin\swift.exe"
  Copy-Item -Force $SwiftDriver "$($HostArch.ToolchainInstallRoot)\usr\bin\swiftc.exe"
}

function Build-Inspect() {
  $OutDir = Join-Path -Path $HostArch.BinaryCache -ChildPath swift-inspect
  $SDKInstallRoot = (Get-HostSwiftSDK) `

  Isolate-EnvVars {
    $env:SWIFTCI_USE_LOCAL_DEPS=1
    Build-SPMProject `
      -Src $SourceCache\swift\tools\swift-inspect `
      -Bin $OutDir `
      -Arch $HostArch `
      -Xcc "-I$SDKInstallRoot\usr\include\swift\SwiftRemoteMirror" -Xlinker "$SDKInstallRoot\usr\lib\swift\windows\$($HostArch.LLVMName)\swiftRemoteMirror.lib"
  }
}

function Build-DocC() {
  $OutDir = Join-Path -Path $HostArch.BinaryCache -ChildPath swift-docc

  Isolate-EnvVars {
    $env:SWIFTCI_USE_LOCAL_DEPS=1
    Build-SPMProject `
      -Src $SourceCache\swift-docc `
      -Bin $OutDir `
      -Arch $HostArch `
      --product docc
  }
}

function Test-PackageManager() {
  $OutDir = Join-Path -Path $HostArch.BinaryCache -ChildPath swift-package-manager
  $SrcDir = if (Test-Path -Path "$SourceCache\swift-package-manager" -PathType Container) {
    "$SourceCache\swift-package-manager"
  } else {
    "$SourceCache\swiftpm"
  }

  Isolate-EnvVars {
    $env:SWIFTCI_USE_LOCAL_DEPS=1
    Build-SPMProject `
      -Test `
      -Src $SrcDir `
      -Bin $OutDir `
      -Arch $HostArch `
      -Xcc -Xclang -Xcc -fno-split-cold-code -Xcc "-I$LibraryRoot\sqlite-3.43.2\usr\include" -Xlinker "-L$LibraryRoot\sqlite-3.43.2\usr\lib"
  }
}

function Build-Installer($Arch) {
  # TODO(hjyamauchi) Re-enable the swift-inspect and swift-docc builds
  # when cross-compiling https://github.com/apple/swift/issues/71655
  $INCLUDE_SWIFT_INSPECT = if ($IsCrossCompiling) { "false" } else { "true" }
  $INCLUDE_SWIFT_DOCC = if ($IsCrossCompiling) { "false" } else { "true" }

  $Properties = @{
    BundleFlavor = "offline";
    DEVTOOLS_ROOT = "$($Arch.ToolchainInstallRoot)\";
    TOOLCHAIN_ROOT = "$($Arch.ToolchainInstallRoot)\";
    INCLUDE_SWIFT_INSPECT = $INCLUDE_SWIFT_INSPECT;
    SWIFT_INSPECT_BUILD = "$($Arch.BinaryCache)\swift-inspect\release";
    INCLUDE_SWIFT_DOCC = $INCLUDE_SWIFT_DOCC;
    SWIFT_DOCC_BUILD = "$($Arch.BinaryCache)\swift-docc\release";
    SWIFT_DOCC_RENDER_ARTIFACT_ROOT = "${SourceCache}\swift-docc-render-artifact";
  }

  Isolate-EnvVars {
    Invoke-VsDevShell $Arch
    # Avoid hard-coding the VC tools version number
    $VCRedistDir = (Get-ChildItem "${env:VCToolsRedistDir}\$($HostArch.ShortName)" -Filter "Microsoft.VC*.CRT").FullName
    if ($VCRedistDir) {
      $Properties["VCRedistDir"] = "$VCRedistDir\"
    }
  }

  foreach ($SDK in $WindowsSDKArchs) {
    $Properties["INCLUDE_$($SDK.VSName.ToUpperInvariant())_SDK"] = "true"
    $Properties["PLATFORM_ROOT_$($SDK.VSName.ToUpperInvariant())"] = "$($SDK.PlatformInstallRoot)\"
    $Properties["SDK_ROOT_$($SDK.VSName.ToUpperInvariant())"] = "$($SDK.SDKInstallRoot)\"
  }

  Build-WiXProject bundle\installer.wixproj -Arch $Arch -Bundle -Properties $Properties
}

function Stage-BuildArtifacts($Arch) {
  Copy-File "$($Arch.BinaryCache)\installer\Release\$($Arch.VSName)\*.cab" "$Stage\"
  Copy-File "$($Arch.BinaryCache)\installer\Release\$($Arch.VSName)\*.msi" "$Stage\"
  Copy-File "$($Arch.BinaryCache)\installer\Release\$($Arch.VSName)\rtl.cab" "$Stage\"
  Copy-File "$($Arch.BinaryCache)\installer\Release\$($Arch.VSName)\rtl.msi" "$Stage\"
  foreach ($SDK in $WindowsSDKArchs) {
    Copy-File "$($Arch.BinaryCache)\installer\Release\$($SDK.VSName)\sdk.$($SDK.VSName).cab" "$Stage\"
    Copy-File "$($Arch.BinaryCache)\installer\Release\$($SDK.VSName)\sdk.$($SDK.VSName).msi" "$Stage\"
    Copy-File "$($Arch.BinaryCache)\installer\Release\$($SDK.VSName)\rtl.$($SDK.VSName).msm" "$Stage\"
  }
  Copy-File "$($Arch.BinaryCache)\installer\Release\$($Arch.VSName)\installer.exe" "$Stage\"
  # Extract installer engine to ease code-signing on swift.org CI
  if ($ToBatch) {
    Write-Output "md `"$($Arch.BinaryCache)\installer\$($Arch.VSName)\`""
  } else {
    New-Item -Type Directory -Path "$($Arch.BinaryCache)\installer\$($Arch.VSName)\" -ErrorAction Ignore | Out-Null
  }
  Invoke-Program "$BinaryCache\wix-4.0.4\tools\net6.0\any\wix.exe" -- burn detach "$($Arch.BinaryCache)\installer\Release\$($Arch.VSName)\installer.exe" -engine "$Stage\installer-engine.exe" -intermediateFolder "$($Arch.BinaryCache)\installer\$($Arch.VSName)\"
}

#-------------------------------------------------------------------
try {

if (-not $SkipBuild) {
  Fetch-Dependencies
}

if (-not $SkipBuild) {
  Invoke-BuildStep Build-CMark $BuildArch
  Invoke-BuildStep Build-BuildTools $BuildArch
  if ($IsCrossCompiling) {
    Invoke-BuildStep Build-Compilers -Build $BuildArch
  }

  Invoke-BuildStep Build-CMark $HostArch
  Invoke-BuildStep Build-Compilers $HostArch
}

if ($Clean) {
  10..27 | % { Remove-Item -Force -Recurse "$BinaryCache\$_" -ErrorAction Ignore }
  foreach ($Arch in $WindowsSDKArchs) {
    0..3 | % { Remove-Item -Force -Recurse "$BinaryCache\$($Arch.BuildID + $_)" -ErrorAction Ignore }
  }
}

if (-not $SkipBuild) {
  foreach ($Arch in $WindowsSDKArchs) {
    Invoke-BuildStep Build-ZLib Windows $Arch
    Invoke-BuildStep Build-XML2 Windows $Arch
    Invoke-BuildStep Build-CURL Windows $Arch
    Invoke-BuildStep Build-ICU Windows $Arch
    Invoke-BuildStep Build-LLVM Windows $Arch

    # Build platform: SDK, Redist and XCTest
    Invoke-BuildStep Build-Runtime Windows $Arch
    Invoke-BuildStep Build-Dispatch Windows $Arch
    Invoke-BuildStep Build-Foundation Windows $Arch
    Invoke-BuildStep Build-XCTest Windows $Arch
  }

   foreach ($Arch in $AndroidSDKArchs) {
     Invoke-BuildStep Build-ZLib Android $Arch
     Invoke-BuildStep Build-XML2 Android $Arch
     Invoke-BuildStep Build-CURL Android $Arch
     Invoke-BuildStep Build-ICU Android $Arch
     Invoke-BuildStep Build-LLVM Android $Arch

     # Build platform: SDK, Redist and XCTest
     Invoke-BuildStep Build-Runtime Android $Arch
     Invoke-BuildStep Build-Dispatch Android $Arch
     Invoke-BuildStep Build-Foundation Android $Arch
     Invoke-BuildStep Build-XCTest Android $Arch
   }
}

if (-not $ToBatch) {
  if ($HostArch -in $WindowsSDKArchs) {
    $RuntimeInstallRoot = [IO.Path]::Combine((Get-InstallDir $HostArch), "Runtimes", $ProductVersion)

    Remove-Item -Force -Recurse $RuntimeInstallRoot -ErrorAction Ignore
    Copy-Directory "$($HostArch.SDKInstallRoot)\usr\bin" "$RuntimeInstallRoot\usr"
  }

  Remove-Item -Force -Recurse ([IO.Path]::Combine((Get-InstallDir $HostArch), "Platforms")) -ErrorAction Ignore
  foreach ($Arch in $WindowsSDKArchs) {
    Install-Platform Windows $Arch
  }

  foreach ($Arch in $AndroidSDKArchs) {
    Install-Platform Android $Arch
  }
}

if (-not $SkipBuild) {
  Invoke-BuildStep Build-SQLite $HostArch
  Invoke-BuildStep Build-System $HostArch
  Invoke-BuildStep Build-ToolsSupportCore $HostArch
  Invoke-BuildStep Build-LLBuild $HostArch
  Invoke-BuildStep Build-Yams $HostArch
  Invoke-BuildStep Build-ArgumentParser $HostArch
  Invoke-BuildStep Build-Driver $HostArch
  Invoke-BuildStep Build-Crypto $HostArch
  Invoke-BuildStep Build-Collections $HostArch
  Invoke-BuildStep Build-ASN1 $HostArch
  Invoke-BuildStep Build-Certificates $HostArch
  Invoke-BuildStep Build-PackageManager $HostArch
  Invoke-BuildStep Build-Markdown $HostArch
  Invoke-BuildStep Build-Format $HostArch
  Invoke-BuildStep Build-IndexStoreDB $HostArch
  Invoke-BuildStep Build-SourceKitLSP $HostArch
}

Install-HostToolchain

if (-not $SkipBuild -and -not $IsCrossCompiling) {
  Invoke-BuildStep Build-Inspect $HostArch
  Invoke-BuildStep Build-DocC $HostArch
}

if (-not $SkipPackaging) {
  Invoke-BuildStep Build-Installer $HostArch
}

if ($Stage) {
  Stage-BuildArtifacts $HostArch
}

if (-not $IsCrossCompiling) {
  if ($Test -ne $null -and (Compare-Object $Test @("clang", "lld", "lldb", "llvm", "swift") -PassThru -IncludeEqual -ExcludeDifferent) -ne $null) {
    $Tests = @{
      "-TestClang" = $Test -contains "clang";
      "-TestLLD" = $Test -contains "lld";
      "-TestLLDB" = $Test -contains "lldb";
      "-TestLLVM" = $Test -contains "llvm";
      "-TestSwift" = $Test -contains "swift";
    }
    Build-Compilers $HostArch @Tests
  }

  if ($Test -contains "dispatch") {
    Build-Dispatch Windows $HostArch -Test
  }
  if ($Test -contains "foundation") {
    Build-Foundation Windows $HostArch -Test
  }
  if ($Test -contains "xctest") {
    Build-XCTest Windows $HostArch -Test
  }
  if ($Test -contains "llbuild") { Build-LLBuild $HostArch -Test }
  if ($Test -contains "swiftpm") { Test-PackageManager $HostArch }
}

# Custom exception printing for more detailed exception information
} catch {
  function Write-ErrorLines($Text, $Indent = 0) {
    $IndentString = " " * $Indent
    $Text.Replace("`r", "") -split "`n" | ForEach-Object {
      Write-Host "$IndentString$_" -ForegroundColor Red
    }
  }

  Write-ErrorLines "Error: $_"
  Write-ErrorLines $_.ScriptStackTrace -Indent 4

  # Walk the .NET inner exception chain to print all messages and stack traces
  $Exception = $_.Exception
  $Indent = 2
  while ($Exception -is [Exception]) {
      Write-ErrorLines "From $($Exception.GetType().FullName): $($Exception.Message)" -Indent $Indent
      if ($null -ne $Exception.StackTrace) {
          # .NET exceptions stack traces are already indented by 3 spaces
          Write-ErrorLines $Exception.StackTrace -Indent ($Indent + 1)
      }
      $Exception = $Exception.InnerException
      $Indent += 2
  }

  exit 1
} finally {
  if ($Summary) {
    $TimingData | Select Platform,Arch,Checkout,"Elapsed Time" | Sort -Descending -Property "Elapsed Time" | Format-Table -AutoSize
  }
}
