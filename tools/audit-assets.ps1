param(
    [Parameter(Mandatory = $true)][string]$Directory,
    [Parameter(Mandatory = $true)][string]$OutputDirectory
)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$output = [System.IO.Path]::GetFullPath($OutputDirectory)
New-Item -ItemType Directory -Path $output -Force | Out-Null
$modelExtensions = @('.glb', '.gltf', '.fbx', '.obj', '.dae', '.3ds', '.ply', '.stl', '.blend', '.x', '.3mf', '.off')
$models = @(Get-ChildItem -LiteralPath $Directory -Recurse -File | Where-Object { $_.Extension.ToLowerInvariant() -in $modelExtensions } | Sort-Object FullName)
$results = @()
$index = 0
foreach ($model in $models) {
    $index++
    $prefix = '{0:D2}-{1}' -f $index, $model.BaseName
    $cpuLog = Join-Path $output "$prefix.cpu.log"
    $cpuErrorLog = Join-Path $output "$prefix.cpu.stderr.log"
    $cpuProcess = Start-Process -FilePath (Join-Path $root 'build\Release\vke_tests.exe') -ArgumentList "`"$($model.FullName)`"" `
        -WindowStyle Hidden -PassThru -RedirectStandardOutput $cpuLog -RedirectStandardError $cpuErrorLog
    if (-not $cpuProcess.WaitForExit(210000)) { $cpuProcess.Kill(); $cpuProcess.WaitForExit() }
    $cpuProcess.Refresh()
    $cpuExit = $cpuProcess.ExitCode
    $stdout = Join-Path $output "$prefix.stdout.log"
    $stderr = Join-Path $output "$prefix.stderr.log"
    $screenshot = Join-Path $output "$prefix.png"
    $report = Join-Path $output "$prefix.json"
    $arguments = "--model `"$($model.FullName)`" --smoke --validate --screenshot `"$screenshot`" --report `"$report`""
    $started = Get-Date
    $process = Start-Process -FilePath (Join-Path $root 'build\Release\VkEngine.exe') -ArgumentList $arguments `
        -WindowStyle Hidden -PassThru -RedirectStandardOutput $stdout -RedirectStandardError $stderr
    $timedOut = -not $process.WaitForExit(210000)
    if ($timedOut) { $process.Kill(); $process.WaitForExit() }
    $process.Refresh()
    $data = if (Test-Path -LiteralPath $report) { Get-Content -LiteralPath $report -Raw | ConvertFrom-Json } else { $null }
    $row = [pscustomobject]@{
        name = $model.Name; path = $model.FullName; bytes = $model.Length
        cpuExitCode = $cpuExit; renderExitCode = $process.ExitCode; timedOut = $timedOut
        screenshotExists = Test-Path -LiteralPath $screenshot
        report = $data; prefix = $prefix
        elapsedSeconds = [math]::Round(((Get-Date) - $started).TotalSeconds, 2)
        cpuLog = (Get-Content -LiteralPath $cpuLog -Raw) + (Get-Content -LiteralPath $cpuErrorLog -Raw)
        stdout = Get-Content -LiteralPath $stdout -Raw
        stderr = Get-Content -LiteralPath $stderr -Raw
    }
    $results += $row
    $results | ConvertTo-Json -Depth 10 | Set-Content -LiteralPath (Join-Path $output 'results.json') -Encoding utf8
    Write-Output "$($model.Name): CPU=$cpuExit render=$($process.ExitCode) objects=$($data.objects) validation=$($data.validationErrors) screenshot=$($row.screenshotExists)"
}
