<#
.SYNOPSIS
Packs the desktop app (apps/portablekit) for Windows x64 as a portable folder
and a zip archive, with the trimmed llvm-mingw toolchain it compiles games with.

.DESCRIPTION
  package_desktop_windows.ps1 -BuildDir <build> -Toolchain <llvm-mingw>
      [-Output <folder>] [-Version <version>] [-Packaging <folder>]
      [-License NAME=FILE,...] [-Notices FILE,...] [-Extra PATH,...]
      [-Sdl3License FILE] [-TopLicense FILE] [-NoZip]

BuildDir is a build of apps/portablekit made with that llvm-mingw toolchain,
configured with -DPORTABLEKIT_RELEASE=ON (see docs/RELEASING.md, "The desktop
app"). Nothing in it is changed. The result is

  <Output>\PortableKit\                  the folder a player unpacks
    portablekit.exe  psp_recomp.exe      the program and the recompiler
    *.dll                                SDL3, FFmpeg, libc++ and libunwind
    share\portablekit\include\           the headers the generated code includes
    toolchain\                           the part of llvm-mingw the app compiles with
    data\                                empty until the player adds a game
    README.txt  LICENSE.txt  licenses\
  <Output>\dist\portablekit-<version>-windows-x64.zip, SHA256SUMS, BUILDINFO.txt

Packaging (default apps\portablekit\packaging) holds THIRD_PARTY_NOTICES.md
and windows\README.txt; a downstream build may point it at its own copy.
-License, -Notices and -Extra add what such a build brings: a licence as
licenses\NAME-LICENSE.txt, a notices file in licenses\, and files or folders
at the top of the package. -TopLicense replaces the LICENSE.txt at the top of
the package (PortableKit's by default), for a build whose combined licence is
another; PortableKit's own is in licenses\ either way.

Before packing it checks that the package holds no game data and no path of
this machine in the program's own files, and that the packed program starts
and reports the expected version.
#>
param(
    [Parameter(Mandatory = $true)][string]$BuildDir,
    [Parameter(Mandatory = $true)][string]$Toolchain,
    [string]$Output = "",
    [string]$Version = "",
    [string]$Packaging = "",
    [string[]]$License = @(),
    [string[]]$Notices = @(),
    [string[]]$Extra = @(),
    [string]$Sdl3License = "",
    [string]$TopLicense = "",
    [switch]$NoZip
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 3

function Fail([string]$message) { Write-Error "error: $message"; exit 1 }
# Git's answer, or null outside a checkout: Windows PowerShell turns a native
# command's error output into an error, which Stop would make fatal.
function Get-GitOutput([string[]]$arguments) {
    $saved = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        $output = & git @arguments 2>$null
        if ($LASTEXITCODE -eq 0 -and $output) { return ($output | Select-Object -First 1) }
    } catch {
    } finally { $ErrorActionPreference = $saved }
    return $null
}
function Step([string]$message) { Write-Host ""; Write-Host "=== $message" }

$kitDir = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$BuildDir = (Resolve-Path $BuildDir).Path
$Toolchain = (Resolve-Path $Toolchain).Path
if ($Packaging -eq "") { $Packaging = Join-Path $kitDir 'apps\portablekit\packaging' }
$Packaging = (Resolve-Path $Packaging).Path
if ($Output -eq "") { $Output = Join-Path $kitDir 'out\package-windows' }
$bin = Join-Path $BuildDir 'bin'
$name = 'PortableKit'
$stage = Join-Path $Output $name
$dist = Join-Path $Output 'dist'

# --- What the build must have made ------------------------------------------
Step "Checking $BuildDir"
$programFiles = @('portablekit.exe', 'psp_recomp.exe', 'SDL3.dll', 'avcodec-61.dll', 'avutil-59.dll',
                  'swresample-5.dll', 'FFmpeg-LICENSE.txt', 'FFmpeg-SOURCE.txt')
foreach ($file in $programFiles) {
    if (-not (Test-Path (Join-Path $bin $file))) { Fail "$bin\$file is missing; build the portablekit target" }
}
if (-not (Test-Path (Join-Path $bin 'share\portablekit\include\psprecomp\corpus_abi.hpp'))) {
    Fail "$bin\share\portablekit\include is missing; build the portablekit target"
}
foreach ($file in @('bin\clang++.exe', 'bin\ld.lld.exe', 'bin\libc++.dll', 'bin\libunwind.dll', 'LICENSE.TXT',
                    'x86_64-w64-mingw32\lib\libc++.a')) {
    if (-not (Test-Path (Join-Path $Toolchain $file))) { Fail "$Toolchain is not an llvm-mingw toolchain (no $file)" }
}
$notices = Join-Path $Packaging 'THIRD_PARTY_NOTICES.md'
$readme = Join-Path $Packaging 'windows\README.txt'
foreach ($file in @($notices, $readme)) { if (-not (Test-Path $file)) { Fail "$file is missing" } }

# A developer build falls back to its checkout's game folder; a release must
# not depend on the machine it was built on.
function Test-BinaryContains([string]$path, [string]$text) {
    $bytes = [IO.File]::ReadAllBytes($path)
    $haystack = [Text.Encoding]::GetEncoding(28591).GetString($bytes)
    return $haystack.IndexOf($text, [StringComparison]::OrdinalIgnoreCase) -ge 0
}
$cache = Join-Path $BuildDir 'CMakeCache.txt'
if (-not (Test-Path $cache)) { Fail "$BuildDir has no CMakeCache.txt" }
$cacheText = Get-Content $cache
if (-not ($cacheText -match '^PORTABLEKIT_RELEASE:BOOL=ON$')) {
    Fail "$BuildDir is a developer build; configure it with -DPORTABLEKIT_RELEASE=ON and build again"
}
$sourceLine = $cacheText | Where-Object { $_ -like 'CMAKE_HOME_DIRECTORY:*' } | Select-Object -First 1
$sourceDir = if ($sourceLine) { $sourceLine.Split('=', 2)[1] } else { $kitDir }

# SDL3's licence comes with the development package the build found.
if ($Sdl3License -eq "") {
    $sdlLine = $cacheText | Where-Object { $_ -like 'SDL3_DIR:*' } | Select-Object -First 1
    if ($sdlLine) {
        $dir = $sdlLine.Split('=', 2)[1]
        for ($i = 0; $i -lt 6 -and $dir; $i++) {
            if (Test-Path (Join-Path $dir 'LICENSE.txt')) { $Sdl3License = Join-Path $dir 'LICENSE.txt'; break }
            $dir = Split-Path $dir -Parent
        }
    }
}
if ($Sdl3License -eq "" -or -not (Test-Path $Sdl3License)) { Fail "cannot find SDL3's LICENSE.txt; give it with -Sdl3License" }

foreach ($entry in $License) {
    $parts = $entry.Split('=', 2)
    if ($parts.Count -ne 2 -or -not (Test-Path $parts[1])) { Fail "-License $entry`: expected NAME=FILE with an existing file" }
}
foreach ($file in $Notices) { if (-not (Test-Path $file)) { Fail "-Notices $file`: no such file" } }
foreach ($path in $Extra) { if (-not (Test-Path $path)) { Fail "-Extra $path`: no such file or folder" } }
if ($TopLicense -eq "") { $TopLicense = Join-Path $kitDir 'LICENSE' }
if (-not (Test-Path -PathType Leaf $TopLicense)) { Fail "-TopLicense $TopLicense`: no such file" }

# The notices must name what is bundled.
$ffmpegCmake = Get-Content (Join-Path $kitDir 'cmake\FFmpeg.cmake') -Raw
$ffmpegUrl = [regex]::Match($ffmpegCmake, '"(https://[^"]*win64-lgpl-shared[^"]*)"').Groups[1].Value
$sources = Get-Content (Join-Path $kitDir 'packaging\sources.sh') -Raw
$sdlVersion = [regex]::Match($sources, '(?m)^SDL3_VERSION=(\S+)').Groups[1].Value
$llvmMingw = (& (Join-Path $Toolchain 'bin\clang++.exe') --version | Select-Object -First 1)
$noticeText = Get-Content $notices -Raw
foreach ($pinned in @("SDL3 $sdlVersion", $ffmpegUrl, 'llvm-mingw')) {
    if (-not $noticeText.Contains($pinned)) { Fail "THIRD_PARTY_NOTICES.md does not mention: $pinned" }
}

# --- The folder ---------------------------------------------------------------
Step "Assembling $stage"
foreach ($old in @($stage, $dist)) { if (Test-Path $old) { Remove-Item -Recurse -Force $old } }
New-Item -ItemType Directory -Force $stage, $dist | Out-Null
foreach ($file in $programFiles) { Copy-Item (Join-Path $bin $file) $stage }
Copy-Item (Join-Path $Toolchain 'bin\libc++.dll'), (Join-Path $Toolchain 'bin\libunwind.dll') $stage
New-Item -ItemType Directory (Join-Path $stage 'share') | Out-Null
Copy-Item -Recurse (Join-Path $bin 'share\portablekit') (Join-Path $stage 'share\portablekit')
New-Item -ItemType Directory (Join-Path $stage 'data') | Out-Null

# The toolchain, cut to what compiling a game for x86_64 needs: the compiler
# driver and its libraries, the linker, the headers, clang's runtime and the
# x86_64 sysroot. Other targets, tools and documentation stay out.
$tool = Join-Path $stage 'toolchain'
New-Item -ItemType Directory (Join-Path $tool 'bin'), (Join-Path $tool 'lib') | Out-Null
$toolBin = Join-Path $Toolchain 'bin'
$wanted = @('clang.exe', 'clang++.exe', 'clang-target-wrapper.exe', 'ld.lld.exe', 'x86_64-w64-mingw32-clang.exe',
            'x86_64-w64-mingw32-clang++.exe', 'libc++.dll', 'libunwind.dll', 'libwinpthread-1.dll',
            'libclang-cpp.dll', 'mingw32-common.cfg', 'x86_64-w64-windows-gnu.cfg')
foreach ($file in $wanted) {
    $path = Join-Path $toolBin $file
    if (-not (Test-Path $path)) { Fail "$path is missing from the toolchain" }
    Copy-Item $path (Join-Path $tool 'bin')
}
# The versioned compiler (clang-23.exe) and LLVM library (libLLVM-23.dll).
$versioned = @(Get-ChildItem $toolBin | Where-Object { $_.Name -match '^(clang-\d+\.exe|libLLVM-\d+\.dll)$' })
if ($versioned.Count -lt 2) { Fail "no clang-<version>.exe and libLLVM-<version>.dll in $toolBin" }
$versioned | ForEach-Object { Copy-Item $_.FullName (Join-Path $tool 'bin') }
Copy-Item (Join-Path $Toolchain 'LICENSE.TXT') $tool
Copy-Item -Recurse (Join-Path $Toolchain 'include') (Join-Path $tool 'include')
Copy-Item -Recurse (Join-Path $Toolchain 'lib\clang') (Join-Path $tool 'lib\clang')
# Of clang's runtime libraries, only Windows x86_64's: lib\x86_64-w64-windows-gnu
# (which the linker reads), and the x86_64 files of lib\windows.
# (A wildcard in the path would list the matching folders themselves, not
# their contents, so each version's lib\ is named in full.)
$runtimeDirs = @(Get-ChildItem -Directory (Join-Path $tool 'lib\clang') |
    ForEach-Object { Join-Path $_.FullName 'lib' } | Where-Object { Test-Path $_ } |
    ForEach-Object { Get-ChildItem -Directory -LiteralPath $_ })
if ($runtimeDirs.Count -eq 0) { Fail "no clang runtime libraries in $Toolchain\lib\clang" }
foreach ($runtime in $runtimeDirs) {
    if ($runtime.Name -like 'x86_64-*windows*') { continue }
    if ($runtime.Name -ne 'windows') { Remove-Item -Recurse -Force $runtime.FullName; continue }
    Get-ChildItem -File -LiteralPath $runtime.FullName | Where-Object { $_.Name -notmatch 'x86_64' } | Remove-Item -Force
}
if (-not (Get-ChildItem -Recurse -File (Join-Path $tool 'lib\clang') -Filter 'libclang_rt.builtins*x86_64*')) {
    Fail "the trimmed toolchain lost clang's x86_64 runtime"
}
Copy-Item -Recurse (Join-Path $Toolchain 'x86_64-w64-mingw32') (Join-Path $tool 'x86_64-w64-mingw32')

Copy-Item $readme (Join-Path $stage 'README.txt')
Copy-Item $TopLicense (Join-Path $stage 'LICENSE.txt')
$licenses = Join-Path $stage 'licenses'
New-Item -ItemType Directory $licenses | Out-Null
Copy-Item $notices $licenses
Copy-Item (Join-Path $kitDir 'LICENSE') (Join-Path $licenses 'PortableKit-LICENSE.txt')
Copy-Item $Sdl3License (Join-Path $licenses 'SDL3-LICENSE.txt')
Copy-Item (Join-Path $bin 'FFmpeg-LICENSE.txt'), (Join-Path $bin 'FFmpeg-SOURCE.txt') $licenses
Copy-Item (Join-Path $kitDir 'third_party\imgui\LICENSE.txt') (Join-Path $licenses 'DearImGui-LICENSE.txt')
Copy-Item (Join-Path $kitDir 'third_party\tiny_aes\UNLICENSE') (Join-Path $licenses 'tiny-AES-c-UNLICENSE.txt')
Copy-Item (Join-Path $kitDir 'third_party\xxhash\LICENSE') (Join-Path $licenses 'xxHash-LICENSE.txt')
Copy-Item (Join-Path $Toolchain 'LICENSE.TXT') (Join-Path $licenses 'llvm-mingw-LICENSE.txt')
$mingwNotices = Join-Path $Toolchain 'x86_64-w64-mingw32\share\mingw32'
if (Test-Path $mingwNotices) {
    Get-ChildItem $mingwNotices -Filter 'COPYING*' | ForEach-Object {
        Copy-Item $_.FullName (Join-Path $licenses ("mingw-w64-" + $_.Name + $(if ($_.Extension -eq '') { '.txt' } else { '' })))
    }
}
foreach ($entry in $License) {
    $parts = $entry.Split('=', 2)
    Copy-Item $parts[1] (Join-Path $licenses ($parts[0] + '-LICENSE.txt'))
}
foreach ($file in $Notices) { Copy-Item $file $licenses }
foreach ($path in $Extra) { Copy-Item -Recurse $path $stage }

# --- Checks -------------------------------------------------------------------
Step "Checking the package"
$bad = @()
foreach ($item in Get-ChildItem -Recurse -File $stage) {
    $lower = $item.Name.ToLowerInvariant()
    if ($lower -match '^(eboot.*|data\.bin|param\.sfo|umd_data.*)$' -or
        $lower -match '\.(iso|cso|pbp|prx|elf|ovl)$') { $bad += "$($item.FullName) (name)"; continue }
    if ($item.Length -lt 20) { continue }
    $stream = [IO.File]::OpenRead($item.FullName)
    try {
        $head = New-Object byte[] 20
        [void]$stream.Read($head, 0, 20)
        if ($head[0] -eq 0x7F -and $head[1] -eq 0x45 -and $head[2] -eq 0x4C -and $head[3] -eq 0x46 -and
            $head[18] -eq 8 -and $head[19] -eq 0) { $bad += "$($item.FullName) (PSP executable)" }
        elseif ($head[0] -eq 0 -and $head[1] -eq 0x50 -and $head[2] -eq 0x42 -and $head[3] -eq 0x50) { $bad += "$($item.FullName) (PBP)" }
        elseif ($head[0] -eq 0x7E -and $head[1] -eq 0x50 -and $head[2] -eq 0x53 -and $head[3] -eq 0x50) { $bad += "$($item.FullName) (encrypted PSP module)" }
        elseif ($head[0] -eq 0 -and $head[1] -eq 0x50 -and $head[2] -eq 0x53 -and $head[3] -eq 0x46) { $bad += "$($item.FullName) (PARAM.SFO)" }
        elseif ($item.Length -gt 32774) {
            $iso = New-Object byte[] 5
            [void]$stream.Seek(32769, 'Begin')
            [void]$stream.Read($iso, 0, 5)
            if ([Text.Encoding]::ASCII.GetString($iso) -eq 'CD001') { $bad += "$($item.FullName) (disc image)" }
        }
    } finally { $stream.Dispose() }
}
if ($bad.Count -gt 0) { $bad | ForEach-Object { Write-Host "  $_" }; Fail "the package contains game data" }
Write-Host "no game data in the package"

# The program's own files name no path of this machine. The toolchain is
# llvm-mingw's release as it is, so it is not searched.
$leakChecks = @($env:USERPROFILE, $BuildDir, $sourceDir, $kitDir) | Where-Object { $_ } |
    ForEach-Object { $_; $_.Replace('\', '/') } | Select-Object -Unique
$ownFiles = @(Get-ChildItem $stage -File | Where-Object { $_.Extension -in '.exe' }) +
            @(Get-ChildItem -Recurse -File (Join-Path $stage 'share'))
foreach ($file in $ownFiles) {
    foreach ($text in $leakChecks) {
        if (Test-BinaryContains $file.FullName $text) { Fail "$($file.FullName) names a path of this machine: $text" }
    }
}
Write-Host "no paths of this machine in the program's files"

# The packed program starts, with only what the package holds.
$savedPath = $env:PATH
try {
    $env:PATH = "$stage;$env:SystemRoot\System32;$env:SystemRoot"
    $report = & (Join-Path $stage 'portablekit.exe') version --json | ConvertFrom-Json
} finally { $env:PATH = $savedPath }
if (-not $report.ok) { Fail "the packed portablekit.exe did not start" }
$toolchainReport = & (Join-Path $stage 'portablekit.exe') --data-dir (Join-Path $Output 'check-data') toolchain --json | ConvertFrom-Json
Remove-Item -Recurse -Force (Join-Path $Output 'check-data') -ErrorAction SilentlyContinue
if (-not $toolchainReport.ok -or $toolchainReport.kind -ne 'llvm-mingw') { Fail "the packed program does not find its toolchain" }
# The packed toolchain compiles and links a library the way the app links a
# game's code (-shared -static), with nothing from outside the package.
$probe = Join-Path $Output 'check-toolchain'
if (Test-Path $probe) { Remove-Item -Recurse -Force $probe }
New-Item -ItemType Directory $probe | Out-Null
Set-Content -Encoding ascii (Join-Path $probe 'probe.cpp') @'
#include <string>
#include <vector>
extern "C" __declspec(dllexport) int probe(int n) { std::vector<std::string> v(n, "x"); return static_cast<int>(v.size()); }
'@
$savedPath = $env:PATH
try {
    $env:PATH = "$env:SystemRoot\System32;$env:SystemRoot"
    & (Join-Path $tool 'bin\clang++.exe') -std=c++20 -O1 -shared -static -static-libgcc -static-libstdc++ `
        -o (Join-Path $probe 'probe.dll') (Join-Path $probe 'probe.cpp')
    $probeStatus = $LASTEXITCODE
} finally { $env:PATH = $savedPath }
if ($probeStatus -ne 0 -or -not (Test-Path (Join-Path $probe 'probe.dll'))) {
    Fail "the packed toolchain cannot compile and link a library"
}
Remove-Item -Recurse -Force $probe
Write-Host "the packed toolchain compiles and links a library"
Write-Host "portablekit $($report.version)$(if ($report.label) { " ($($report.label))" }), toolchain: $($toolchainReport.version)"
foreach ($module in $report.hle_extensions) { Write-Host "  HLE extension: $($module.title) $($module.version) ($($module.license))" }

if ($Version -eq "") {
    $Version = $report.version
    if ($Version -eq 'unknown') {
        $described = Get-GitOutput @('-C', $sourceDir, 'describe', '--tags', '--always', '--dirty')
        if ($described) { $Version = $described }
    }
    $Version = $Version.TrimStart('v')
}
$size = (Get-ChildItem -Recurse -File $stage | Measure-Object Length -Sum).Sum
Write-Host ("{0}: {1:N0} MB" -f $stage, ($size / 1MB))

# --- Archive --------------------------------------------------------------------
$archive = "portablekit-$Version-windows-x64"
if (-not $NoZip) {
    Step "Zip"
    $zip = Join-Path $dist "$archive.zip"
    if (Test-Path $zip) { Remove-Item -Force $zip }
    # Windows' own tar writes zip archives much faster than Compress-Archive.
    & tar.exe -a -c -f $zip -C $Output $name
    if ($LASTEXITCODE -ne 0) { Fail "tar could not write $zip" }
    $hash = (Get-FileHash -Algorithm SHA256 $zip).Hash.ToLowerInvariant()
    Set-Content -Encoding ascii (Join-Path $dist 'SHA256SUMS') "$hash  $archive.zip"
    Write-Host "$hash  $archive.zip"
}
$commit = Get-GitOutput @('-C', $sourceDir, 'rev-parse', 'HEAD')
@(
    "PortableKit $Version for Windows x64$(if ($report.label) { " ($($report.label))" })",
    "Built from: $(if ($commit) { $commit } else { 'a source tree without Git history' })",
    "Toolchain shipped and used to build: $llvmMingw",
    "Corpus ABI: $($report.abi)",
    "HLE extension modules: $(if ($report.hle_extensions.Count -gt 0) { ($report.hle_extensions | ForEach-Object { "$($_.title) $($_.version) ($($_.license))" }) -join ', ' } else { 'none' })"
) | Set-Content -Encoding utf8 (Join-Path $dist 'BUILDINFO.txt')
Get-ChildItem $dist | Format-Table Name, Length -AutoSize
