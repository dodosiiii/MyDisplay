#Requires -Version 5.1

[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
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
    throw '64-bit MSBuild was not found.'
}

$projectRoot = Split-Path -Parent $PSScriptRoot
$clientProjects = @(
    (Join-Path $projectRoot 'Client\UI\DisplayClient.vcxproj'),
    (Join-Path $projectRoot 'Client\UI\DisplayClientSetup.vcxproj'),
    (Join-Path $projectRoot 'Client\Network\DisplayClientTlsBridge.vcxproj')
)
foreach ($project in $clientProjects) {
    & $msbuildPath $project '/m' '/p:Configuration=Release' '/p:Platform=x64' '/v:minimal'
    if ($LASTEXITCODE -ne 0) {
        throw "Client build failed with exit code ${LASTEXITCODE}: $project"
    }
}

$certificatePath = Join-Path $env:LOCALAPPDATA 'MyDisplay\ServerCertificate.cer'
if (-not (Test-Path $certificatePath)) {
    throw 'The public server certificate was not found. Run New-ServerCertificate.ps1 on the host first.'
}

$packagePath = Join-Path $projectRoot 'Client\Package'
New-Item -ItemType Directory -Path $packagePath -Force | Out-Null
Copy-Item (Join-Path $projectRoot 'Client\UI\x64\Release\DisplayClient.exe') $packagePath -Force
Copy-Item (Join-Path $projectRoot 'Client\UI\x64\Release\DisplayClientSetup.exe') $packagePath -Force
Copy-Item (Join-Path $projectRoot 'Client\Network\x64\Release\DisplayClientTlsBridge.exe') $packagePath -Force
Copy-Item $certificatePath (Join-Path $packagePath 'ServerCertificate.cer') -Force

Write-Host "Client package ready: $packagePath"
Write-Host 'Launch DisplayClientSetup.exe on the client PC. The package is portable and needs no separate install.'
Write-Host 'Copy the .cer file to the client PC through a trusted offline channel. It contains no private key.'