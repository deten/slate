<#
.SYNOPSIS
    Configure, build and test Slate.

.DESCRIPTION
    Uses the portable CMake and Ninja under .tools/ and the MSVC toolchain from
    the local Visual Studio installation, so no machine-wide build tooling or
    elevated install is required. This matches the portable-application goal
    and keeps the build reproducible on a fresh machine.

.PARAMETER Config
    CMake build type. Debug, Release or RelWithDebInfo (default).

.PARAMETER Target
    Build a specific target instead of all.

.PARAMETER Clean
    Delete the build directory before configuring.

.PARAMETER NoTest
    Build without running the test suite.

.PARAMETER Package
    Stage a self-contained folder under build\package that runs on a machine
    with no Qt, no Visual Studio and no build tools. Copying Slate.exe on its
    own does not work: it needs the Qt runtime, the Qt plugin folders and the
    bundled DWG converter beside it.
#>
[CmdletBinding()]
param(
    [ValidateSet('Debug', 'Release', 'RelWithDebInfo')]
    [string]$Config = 'RelWithDebInfo',
    [string]$Target = '',
    [switch]$Clean,
    [switch]$NoTest,
    [switch]$Package
)

$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot
$tools = Join-Path $root '.tools'
$cmake = Join-Path $tools 'cmake-4.4.2-windows-x86_64\bin\cmake.exe'
$ctest = Join-Path $tools 'cmake-4.4.2-windows-x86_64\bin\ctest.exe'
$ninjaDir = Join-Path $tools 'ninja'
$buildDir = Join-Path $root "build\$Config"

if (-not (Test-Path $cmake)) {
    throw "CMake not found at $cmake. See docs/BUILDING.md."
}

# Locate MSVC and import its environment. The compiler cannot be invoked
# without the INCLUDE/LIB variables vcvars sets up.
$vswhere = 'C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe'
if (-not (Test-Path $vswhere)) {
    throw 'Visual Studio not found. Install the "Desktop development with C++" workload.'
}
$vsRoot = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vsRoot) {
    # Fall back to any VS install; the VC tools check can miss unusual layouts.
    $vsRoot = & $vswhere -latest -products * -property installationPath
}
$vcvars = Join-Path $vsRoot 'VC\Auxiliary\Build\vcvars64.bat'
if (-not (Test-Path $vcvars)) {
    throw "vcvars64.bat not found under $vsRoot. Install the C++ workload."
}

# Import the MSVC environment into this PowerShell session by running vcvars in
# a child cmd and reading back the resulting variables.
Write-Host "Importing MSVC environment..." -ForegroundColor DarkGray
$envDump = & cmd /c "`"$vcvars`" >nul 2>&1 && set"
foreach ($line in $envDump) {
    if ($line -match '^([^=]+)=(.*)$') {
        Set-Item -Path "Env:$($Matches[1])" -Value $Matches[2] -ErrorAction SilentlyContinue
    }
}

# Put the portable Ninja on PATH so CMake's generator finds it.
$env:PATH = "$ninjaDir;$env:PATH"

if ($Clean -and (Test-Path $buildDir)) {
    Write-Host "Cleaning $buildDir" -ForegroundColor DarkGray
    Remove-Item -Recurse -Force $buildDir
}

# A locked output binary makes the linker fail with LNK1104, which explains
# nothing about the cause. Two things hold such a lock: a copy still running,
# and an on-access virus scanner that has not finished with the file the
# previous build wrote. Clear the first and wait out the second.
$binDir = Join-Path $buildDir 'bin'
foreach ($name in 'cad_tests', 'Slate', 'ApertureCAD') {
    $running = Get-Process -Name $name -ErrorAction SilentlyContinue
    if ($running) {
        Write-Host "Stopping a running $name process" -ForegroundColor DarkGray
        $running | Stop-Process -Force
        $running | Wait-Process -Timeout 5 -ErrorAction SilentlyContinue
    }

    # Deleting the file is both the fix and the test for whether the lock has
    # cleared: the linker is about to replace it anyway.
    $exe = Join-Path $binDir "$name.exe"
    if (Test-Path $exe) {
        for ($attempt = 1; $attempt -le 10; $attempt++) {
            try {
                Remove-Item $exe -Force -ErrorAction Stop
                break
            } catch {
                if ($attempt -eq 10) {
                    Write-Host "Warning: $exe stayed locked; the link may fail." -ForegroundColor Yellow
                } else {
                    Start-Sleep -Milliseconds 300
                }
            }
        }
    }
}

Write-Host "Configuring ($Config)..." -ForegroundColor Cyan
# Locate the portable Qt, if present. The engine and its tests do not need it;
# only the shell does, so a missing Qt degrades to a core-only build rather
# than an error.
$qtRoot = Join-Path $tools 'Qt\6.8.3\msvc2022_64'
$cmakeArgs = @(
    '-S', $root, '-B', $buildDir, '-G', 'Ninja',
    "-DCMAKE_BUILD_TYPE=$Config",
    # Only CXX is enabled in the project, so passing CMAKE_C_COMPILER would
    # raise an unused-variable warning.
    '-DCMAKE_CXX_COMPILER=cl'
)

if (Test-Path (Join-Path $qtRoot 'lib\cmake\Qt6')) {
    $cmakeArgs += "-DCMAKE_PREFIX_PATH=$qtRoot"
    $cmakeArgs += "-DWINDEPLOYQT_EXECUTABLE=$(Join-Path $qtRoot 'bin\windeployqt.exe')"
} else {
    Write-Host "Qt not found at $qtRoot; building without the application shell." -ForegroundColor Yellow
    $cmakeArgs += '-DCAD_BUILD_APP=OFF'
}

& $cmake @cmakeArgs
if ($LASTEXITCODE -ne 0) { throw "Configure failed with exit code $LASTEXITCODE" }

Write-Host "Building..." -ForegroundColor Cyan
$buildArgs = @('--build', $buildDir)
if ($Target) { $buildArgs += @('--target', $Target) }
& $cmake @buildArgs
if ($LASTEXITCODE -ne 0) { throw "Build failed with exit code $LASTEXITCODE" }

# Expose the portable ODA File Converter to the built application.
#
# The app looks in `bin/converters/ODAFileConverter` (see OdaFileConverter.cpp),
# which keeps the build output a directory that can be copied and run
# elsewhere with DWG support intact. A junction rather than a copy: the payload
# is ~90MB and there is no reason to duplicate it per configuration.
$odaSource = Join-Path $tools 'ODA'
if (Test-Path (Join-Path $odaSource 'ODAFileConverter.exe')) {
    $convertersDir = Join-Path $binDir 'converters'
    $odaLink = Join-Path $convertersDir 'ODAFileConverter'
    if (-not (Test-Path $odaLink)) {
        New-Item -ItemType Directory -Force -Path $convertersDir | Out-Null
        try {
            New-Item -ItemType Junction -Path $odaLink -Target $odaSource -ErrorAction Stop | Out-Null
            Write-Host "Linked DWG converter into $odaLink" -ForegroundColor DarkGray
        } catch {
            Write-Host "Could not link the DWG converter: $($_.Exception.Message)" -ForegroundColor Yellow
        }
    }
}

if (-not $NoTest) {
    Write-Host "Testing..." -ForegroundColor Cyan
    Push-Location $buildDir
    try {
        & $ctest --output-on-failure
        if ($LASTEXITCODE -ne 0) { throw "Tests failed with exit code $LASTEXITCODE" }
    } finally {
        Pop-Location
    }
}

if (-not $NoTest) {
    # The unit tests cover the libraries; these cover the application, by
    # driving it and reading back what it drew. Skipped when Python is not on
    # the machine rather than failing the build: they are a second suite, and
    # the first one has already run by this point.
    $python = Get-Command python -ErrorAction SilentlyContinue
    if ($null -eq $python) {
        Write-Host "Skipping interaction tests: python not found" -ForegroundColor Yellow
    } else {
        Write-Host "Testing interactions..." -ForegroundColor Cyan
        & $python.Source (Join-Path $root 'tools\interaction_tests.py')
        if ($LASTEXITCODE -ne 0) { throw "Interaction tests failed" }
    }
}

if ($Package) {
    Write-Host "Packaging..." -ForegroundColor Cyan
    $bin = Join-Path $buildDir 'bin'
    $stage = Join-Path $root 'build\package\Slate'
    Remove-Item $stage -Recurse -Force -ErrorAction SilentlyContinue
    New-Item -ItemType Directory -Force -Path $stage | Out-Null

    # The application only. The other executables in bin -- the test runner and
    # the benchmark and inspection tools -- are development scaffolding, and
    # shipping them invites somebody to run one and file the result as a bug.
    Copy-Item (Join-Path $bin 'Slate.exe') -Destination $stage
    # The licence and the third-party notices travel with every copy; the
    # licence's terms require it, and so does Qt's.
    Copy-Item (Join-Path $root 'LICENSE') -Destination $stage
    Copy-Item (Join-Path $root 'NOTICE.md') -Destination $stage

    # Every DLL beside it. windeployqt put the Qt runtime here; picking the
    # ones to copy by name would go stale the first time a Qt module is added.
    Get-ChildItem -Path $bin -Filter *.dll -File | Copy-Item -Destination $stage

    # Qt looks for its plugins in folders beside the executable, and refuses to
    # start at all without the platform one: "could not find or load the Qt
    # platform plugin windows" is what a missing `platforms` folder looks like.
    foreach ($folder in @('platforms', 'styles', 'imageformats', 'iconengines',
                          'networkinformation', 'generic', 'tls', 'plugins')) {
        $source = Join-Path $bin $folder
        if (Test-Path $source) {
            Copy-Item -Path $source -Destination $stage -Recurse -Force
        }
    }

    # The DWG converter is a *junction* into an external install, so a plain
    # copy would reproduce the link and the package would depend on a path that
    # does not exist on the other machine. robocopy walks through it.
    $converters = Join-Path $bin 'converters'
    if (Test-Path $converters) {
        $target = Join-Path $stage 'converters'
        & robocopy $converters $target /E /NFL /NDL /NJH /NJS /NP | Out-Null
        # robocopy reports what it did in its exit code; under 8 is success, and
        # leaving it set would make the next check think the build failed.
        if ($LASTEXITCODE -lt 8) { $global:LASTEXITCODE = 0 }
    }

    $size = (Get-ChildItem $stage -Recurse -File | Measure-Object -Property Length -Sum).Sum
    Write-Host ("Staged {0:N0} MB to {1}" -f ($size / 1MB), $stage) -ForegroundColor Green
    Write-Host "Copy that whole folder to the other machine and run Slate.exe inside it." -ForegroundColor DarkGray
}

Write-Host "OK ($Config)" -ForegroundColor Green
