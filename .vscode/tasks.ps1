# IDE tasks for Bedrock. cmake is taken from PATH, else from Visual Studio,
# the same way build.ps1 finds it. vcpkg is $env:VCPKG_ROOT, else %USERPROFILE%\vcpkg.
param(
    [Parameter(Mandatory = $true)]
    [ValidateSet("build", "test", "run")]
    [string]$Task
)

$ErrorActionPreference = "Stop"
$projectPath = Split-Path $PSScriptRoot -Parent
$vcpkgPath = if ($env:VCPKG_ROOT) { $env:VCPKG_ROOT } else { Join-Path $env:USERPROFILE "vcpkg" }
$buildPath = Join-Path $projectPath "build"
$exe = Join-Path $buildPath "Release\Bedrock.exe"

function Get-Cmake {
    $found = (Get-Command cmake -ErrorAction SilentlyContinue).Source
    if (-not $found) {
        $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
        if (Test-Path $vswhere) {
            $vsPath = & $vswhere -latest -products * -property installationPath
            $candidate = Join-Path $vsPath "Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
            if (Test-Path $candidate) { $found = $candidate }
        }
    }
    if (-not $found) {
        Write-Host "cmake not found (neither on PATH nor in Visual Studio)." -ForegroundColor Red
        exit 1
    }
    return $found
}

switch ($Task) {
    "build" {
        $cmake = Get-Cmake
        $toolchain = Join-Path $vcpkgPath "scripts\buildsystems\vcpkg.cmake"
        if (-not (Test-Path $toolchain)) {
            Write-Host "vcpkg toolchain not found at $toolchain" -ForegroundColor Red
            exit 1
        }
        & $cmake -S $projectPath -B $buildPath -A x64 "-DCMAKE_TOOLCHAIN_FILE=$toolchain"
        if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
        & $cmake --build $buildPath --config Release --parallel
        exit $LASTEXITCODE
    }
    "test" {
        if (-not (Test-Path $exe)) {
            Write-Host "Bedrock.exe not found. Build first." -ForegroundColor Red
            exit 1
        }
        & $exe --selftest
        exit $LASTEXITCODE
    }
    "run" {
        if (-not (Test-Path $exe)) {
            Write-Host "Bedrock.exe not found. Build first." -ForegroundColor Red
            exit 1
        }
        & $exe
        exit $LASTEXITCODE
    }
}
