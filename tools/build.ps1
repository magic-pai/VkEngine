param(
    [ValidateSet('Debug', 'Release')][string]$Configuration = 'Release',
    [switch]$Test,
    [switch]$Package
)
$ErrorActionPreference = 'Stop'
$sourceRoot = Split-Path -Parent $PSScriptRoot
$cmakeCommand = Get-Command cmake -ErrorAction SilentlyContinue
if ($cmakeCommand) {
    $cmakePath = $cmakeCommand.Source
} else {
    $vswherePath = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (-not (Test-Path -LiteralPath $vswherePath)) { throw 'Install Visual Studio 2022 C++ build tools and CMake.' }
    $vsPath = & $vswherePath -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    $cmakePath = Join-Path $vsPath 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
    if (-not (Test-Path -LiteralPath $cmakePath)) { throw 'Install the C++ CMake tools component in Visual Studio.' }
}
Push-Location $sourceRoot
try {
    & $cmakePath --preset windows
    if ($LASTEXITCODE) { throw 'CMake configuration failed.' }
    & $cmakePath --build --preset $Configuration.ToLowerInvariant() --parallel 12
    if ($LASTEXITCODE) { throw 'Build failed.' }
    if ($Test) {
        $ctestPath = Join-Path (Split-Path -Parent $cmakePath) 'ctest.exe'
        & $ctestPath --preset $Configuration.ToLowerInvariant()
        if ($LASTEXITCODE) { throw 'Tests failed.' }
    }
    if ($Package) {
        & $cmakePath --install build --config $Configuration --prefix out/VkEngine
        if ($LASTEXITCODE) { throw 'Package installation failed.' }
    }
} finally {
    Pop-Location
}
