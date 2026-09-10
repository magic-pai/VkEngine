param(
    [Parameter(Mandatory = $true)][string]$Model,
    [switch]$Benchmark
)
$ErrorActionPreference = 'Stop'
$rootPath = Split-Path -Parent $PSScriptRoot
$capturePath = Join-Path $rootPath 'captures'
New-Item -ItemType Directory -Path $capturePath -Force | Out-Null
$modelPath = (Resolve-Path -LiteralPath $Model).Path
function Invoke-Viewer([string]$Name, [string]$Configuration, [string]$Arguments) {
    $exePath = Join-Path $rootPath "build\$Configuration\VkEngine.exe"
    $result = Start-Process -FilePath $exePath -ArgumentList $Arguments -WindowStyle Hidden -Wait -PassThru `
        -RedirectStandardOutput (Join-Path $capturePath "$Name.stdout.log") `
        -RedirectStandardError (Join-Path $capturePath "$Name.stderr.log")
    if ($result.ExitCode -ne 0) { throw "$Name failed with exit code $($result.ExitCode); see captures/$Name.stderr.log" }
    $stderrText = Get-Content -LiteralPath (Join-Path $capturePath "$Name.stderr.log") -Raw
    if ($stderrText -match '\[Vulkan\]|Fatal:') { throw "$Name reported Vulkan diagnostics; inspect its log." }
    Write-Host "$Name passed"
}
Invoke-Viewer 'integration-final' 'Debug' "--model `"$modelPath`" --self-test --validate --screenshot `"$capturePath\integration-final.png`" --report `"$capturePath\integration-final.json`""
$fixture = Join-Path $rootPath 'tests\fixtures\materials.vkscene'
Invoke-Viewer 'materials-final' 'Release' "--scene `"$fixture`" --smoke --validate --screenshot `"$capturePath\materials-final.png`" --report `"$capturePath\materials-final.json`""
if ($Benchmark) {
    foreach ($instanceCount in @(1, 10)) {
        Invoke-Viewer "benchmark-$instanceCount" 'Release' "--model `"$modelPath`" --instances $instanceCount --width 2560 --height 1440 --no-ui --no-vsync --no-validation --benchmark 30 --report `"$capturePath\benchmark-$instanceCount.json`" --screenshot `"$capturePath\benchmark-$instanceCount.png`""
    }
}
