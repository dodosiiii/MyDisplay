#Requires -Version 5.1

[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'

$sampleMonitor = Get-PnpDevice -Class Monitor -PresentOnly -ErrorAction SilentlyContinue |
    Where-Object { $_.InstanceId -like '*DELD0E6*' } |
    Select-Object -First 1

if (-not $sampleMonitor) {
    throw 'The virtual S2719DGF monitor is not active. Start Test-Phase1.ps1 in another window and leave IddSampleApp open.'
}

$programFilesX86 = [Environment]::GetEnvironmentVariable('ProgramFiles(x86)')
if ([string]::IsNullOrWhiteSpace($programFilesX86)) {
    $programFilesX86 = $env:ProgramFiles
}

$vswherePath = Join-Path $programFilesX86 'Microsoft Visual Studio\Installer\vswhere.exe'
if (-not (Test-Path $vswherePath)) {
    throw 'Visual Studio Installer (vswhere.exe) was not found.'
}

$installationPath = & $vswherePath -latest -prerelease -products '*' -requires Microsoft.Component.MSBuild -property installationPath
if ([string]::IsNullOrWhiteSpace($installationPath)) {
    throw 'No Visual Studio instance with MSBuild was found.'
}

$msbuildPath = Join-Path $installationPath 'MSBuild\Current\Bin\amd64\MSBuild.exe'
if (-not (Test-Path $msbuildPath)) {
    $msbuildPath = Join-Path $installationPath 'MSBuild\Current\Bin\MSBuild.exe'
}
if (-not (Test-Path $msbuildPath)) {
    throw 'MSBuild was not found in the Visual Studio installation.'
}

$projectRoot = Split-Path -Parent $PSScriptRoot
$projectPath = Join-Path $projectRoot 'Server\Capture\DisplayCaptureProbe.vcxproj'
$probePath = Join-Path $projectRoot 'Server\Capture\x64\Debug\DisplayCaptureProbe.exe'

Write-Host "Virtual monitor detected: $($sampleMonitor.FriendlyName)"
Write-Host 'Building the x64 DXGI capture probe...'
& $msbuildPath $projectPath '/m' '/p:Configuration=Debug' '/p:Platform=x64' '/v:minimal'
if ($LASTEXITCODE -ne 0) {
    throw "MSBuild failed with exit code $LASTEXITCODE."
}

Write-Host 'Capturing one frame from the verified virtual monitor only...'
& $probePath
if ($LASTEXITCODE -ne 0) {
    throw "DisplayCaptureProbe failed with exit code $LASTEXITCODE."
}