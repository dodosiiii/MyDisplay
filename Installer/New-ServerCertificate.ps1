#Requires -Version 5.1

[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
$subject = "CN=MyDisplayServer-$env:COMPUTERNAME"
$certificate = Get-ChildItem Cert:\CurrentUser\My |
    Where-Object { $_.Subject -eq $subject -and $_.HasPrivateKey -and $_.NotAfter -gt (Get-Date) } |
    Sort-Object NotAfter -Descending |
    Select-Object -First 1

if (-not $certificate) {
    $certificate = New-SelfSignedCertificate `
        -Subject $subject `
        -DnsName $env:COMPUTERNAME `
        -FriendlyName 'MyDisplay TLS Server' `
        -Type SSLServerAuthentication `
        -KeyAlgorithm RSA `
        -KeyLength 2048 `
        -KeyExportPolicy Exportable `
        -HashAlgorithm SHA256 `
        -CertStoreLocation Cert:\CurrentUser\My `
        -NotAfter (Get-Date).AddYears(2)
}

$outputDirectory = Join-Path $env:LOCALAPPDATA 'MyDisplay'
New-Item -ItemType Directory -Path $outputDirectory -Force | Out-Null
$publicCertificatePath = Join-Path $outputDirectory 'ServerCertificate.cer'
Export-Certificate -Cert $certificate -FilePath $publicCertificatePath -Force | Out-Null

Write-Host "Server certificate subject: $($certificate.Subject)"
$sha256 = [System.Security.Cryptography.SHA256]::Create()
try {
    $sha256Fingerprint = ([BitConverter]::ToString($sha256.ComputeHash($certificate.RawData))).Replace('-', '')
}
finally {
    $sha256.Dispose()
}
Write-Host "Windows thumbprint (SHA-1): $($certificate.Thumbprint)"
Write-Host "TLS pin (SHA-256):          $sha256Fingerprint"
Write-Host "Public certificate file:    $publicCertificatePath"
Write-Host 'The .cer file contains no private key. Keep the private key in the current-user certificate store.'