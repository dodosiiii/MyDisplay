#Requires -Version 5.1
#Requires -RunAsAdministrator

[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'

$projectRoot = Split-Path -Parent $PSScriptRoot
$sampleRoot = Join-Path $projectRoot 'Server\DisplayDriver\IddSample'
$solutionPath = Join-Path $sampleRoot 'IddSampleDriver.sln'

if (-not (Test-Path $solutionPath)) {
    throw "IDD sample solution not found: $solutionPath"
}

$msbuild = Get-Command msbuild.exe -ErrorAction SilentlyContinue
if ($msbuild) {
    $msbuildPath = $msbuild.Source
}
else {
    $vswherePath = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (-not (Test-Path $vswherePath)) {
        throw 'MSBuild is missing. Install Visual Studio with Desktop C++ and the WDK integration.'
    }

    $installationPath = & $vswherePath -latest -prerelease -products '*' -requires Microsoft.Component.MSBuild -property installationPath
    if ([string]::IsNullOrWhiteSpace($installationPath)) {
        throw 'No Visual Studio instance with MSBuild was found. Include prerelease/Insiders instances or install the MSBuild component.'
    }

    $msbuildPath = Join-Path $installationPath 'MSBuild\Current\Bin\MSBuild.exe'
    if (-not (Test-Path $msbuildPath)) {
        throw 'MSBuild was not found in the latest Visual Studio installation.'
    }
}

$msbuildDirectory = Split-Path -Parent $msbuildPath
if ((Split-Path -Leaf $msbuildDirectory) -ne 'amd64') {
    $amd64MsbuildPath = Join-Path $msbuildDirectory 'amd64\MSBuild.exe'
    if (Test-Path $amd64MsbuildPath) {
        $msbuildPath = $amd64MsbuildPath
    }
}

$kitsRoot = Join-Path ${env:ProgramFiles(x86)} 'Windows Kits\10'
$iddcxHeader = Get-ChildItem -Path (Join-Path $kitsRoot 'Include') -Filter 'iddcx.h' -File -Recurse -ErrorAction SilentlyContinue |
    Select-Object -First 1
$wdkBuildProps = Get-ChildItem -Path (Join-Path $kitsRoot 'build') -Filter 'WindowsDriver.Common.props' -File -Recurse -ErrorAction SilentlyContinue |
    Select-Object -First 1

if (-not $iddcxHeader -or -not $wdkBuildProps) {
    throw 'The WDK is incomplete or missing. Install the Windows Driver Kit and its Visual Studio integration, plus an SDK with a matching build number.'
}

Write-Host 'Building the official IDD sample for x64 Debug...'
& $msbuildPath $solutionPath '/m' '/p:Configuration=Debug' '/p:Platform=x64' '/v:minimal'
if ($LASTEXITCODE -ne 0) {
    throw "MSBuild failed with exit code $LASTEXITCODE. Check that the WDK and its Visual Studio integration are installed."
}

$packageCatalog = Get-ChildItem -Path $sampleRoot -Filter 'IddSampleDriver.cat' -File -Recurse |
    Where-Object {
        (Test-Path (Join-Path $_.DirectoryName 'IddSampleDriver.inf')) -and
        (Test-Path (Join-Path $_.DirectoryName 'IddSampleDriver.dll'))
    } |
    Sort-Object LastWriteTime -Descending |
    Select-Object -First 1

if (-not $packageCatalog) {
    throw 'The build completed, but no complete signed driver package (INF/CAT/DLL) was found.'
}

$testCertificatePath = Join-Path (Split-Path -Parent $packageCatalog.DirectoryName) 'IddSampleDriver.cer'
if (-not (Test-Path $testCertificatePath)) {
    throw "The package test certificate was not found: $testCertificatePath"
}

$packageSignature = Get-AuthenticodeSignature -FilePath $packageCatalog.FullName
$testCertificate = [System.Security.Cryptography.X509Certificates.X509Certificate2]::new($testCertificatePath)
if (-not $packageSignature.SignerCertificate -or $packageSignature.SignerCertificate.Thumbprint -ne $testCertificate.Thumbprint) {
    throw 'The package signer does not match the sample test certificate; refusing to trust an unexpected certificate.'
}

$certificateStores = @('Cert:\LocalMachine\Root', 'Cert:\LocalMachine\TrustedPublisher')
$missingCertificateStores = @(
    $certificateStores | Where-Object {
        -not (Get-ChildItem $_ | Where-Object Thumbprint -eq $testCertificate.Thumbprint)
    }
)

if ($missingCertificateStores.Count -gt 0) {
    Write-Warning 'Trusting this self-signed development certificate changes machine-wide certificate stores. It is only for this local test driver.'
    Write-Host "Certificate: $($testCertificate.Subject)"
    Write-Host "Thumbprint:  $($testCertificate.Thumbprint)"
    $confirmation = Read-Host 'Type TRUST to add this certificate to LocalMachine Root and TrustedPublisher'
    if ($confirmation -cne 'TRUST') {
        throw 'Certificate trust was not approved; the driver package was not installed.'
    }

    foreach ($store in $missingCertificateStores) {
        Import-Certificate -FilePath $testCertificatePath -CertStoreLocation $store | Out-Null
    }
}

$packageInf = Join-Path $packageCatalog.DirectoryName 'IddSampleDriver.inf'
Write-Host "Installing driver package from $($packageCatalog.DirectoryName)..."
& pnputil.exe /add-driver $packageInf /install
if ($LASTEXITCODE -ne 0) {
    throw "PnPUtil failed with exit code $LASTEXITCODE. Check driver-signing policy and Code Integrity logs."
}

$testApp = Get-ChildItem -Path $sampleRoot -Filter 'IddSampleApp.exe' -File -Recurse |
    Sort-Object LastWriteTime -Descending |
    Select-Object -First 1

if (-not $testApp) {
    throw 'The build completed, but IddSampleApp.exe was not found.'
}

Write-Host 'Starting IddSampleApp. Keep this console open, verify the virtual monitor in Display Settings, then press X here to remove it.'
& $testApp.FullName
if ($LASTEXITCODE -ne 0) {
    throw "IddSampleApp failed with exit code $LASTEXITCODE. The device may not have enumerated successfully."
}

Write-Host 'The sample app exited. Confirm that the virtual monitor disappeared from Display Settings.'