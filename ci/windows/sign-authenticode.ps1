[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string[]]$Files,
    [string]$TimestampUrl = 'http://timestamp.digicert.com',
    [switch]$VerifyOnly
)

$ErrorActionPreference = 'Stop'

$sdkRoot = Join-Path ${env:ProgramFiles(x86)} 'Windows Kits\10\bin'
if (-not (Test-Path -LiteralPath $sdkRoot -PathType Container)) {
	throw "Windows SDK bin directory not found: $sdkRoot"
}

$preferredArchitecture = if ([System.Runtime.InteropServices.RuntimeInformation]::OSArchitecture.ToString() -eq 'Arm64') {
    'arm64'
} else {
    'x64'
}
$architectures = @($preferredArchitecture)
if ($preferredArchitecture -eq 'arm64') {
    $architectures += 'x64'
}
$sdkVersions = @(Get-ChildItem -LiteralPath $sdkRoot -Directory -ErrorAction Stop |
    Where-Object { $_.Name -match '^\d+\.\d+\.\d+\.\d+$' } |
    Sort-Object -Property { [version]$_.Name } -Descending)
$signTool = $null
foreach ($architecture in $architectures) {
    foreach ($sdkVersion in $sdkVersions) {
        $candidate = Join-Path $sdkVersion.FullName (Join-Path $architecture 'signtool.exe')
        if (Test-Path -LiteralPath $candidate -PathType Leaf) {
            $signTool = $candidate
            break
        }
    }
    if (-not [string]::IsNullOrWhiteSpace($signTool)) {
        break
    }
}
if ([string]::IsNullOrWhiteSpace($signTool)) {
    throw "No SignTool.exe for $preferredArchitecture was found under $sdkRoot"
}
Write-Host "Using Windows SDK SignTool at $signTool"

if (-not $VerifyOnly) {
    if ([string]::IsNullOrWhiteSpace($env:WINDOWS_SIGNING_CERTIFICATE_PATH) -or
        -not (Test-Path -LiteralPath $env:WINDOWS_SIGNING_CERTIFICATE_PATH -PathType Leaf)) {
        throw 'Windows signing certificate file is unavailable.'
    }
    if ([string]::IsNullOrWhiteSpace($env:WINDOWS_SIGNING_CERTIFICATE_PASSWORD)) {
        throw 'Windows signing certificate password is unavailable.'
    }
}

foreach ($file in $Files) {
    if (-not (Test-Path -LiteralPath $file -PathType Leaf)) {
        throw "File to sign does not exist: $file"
    }
    $resolvedFile = (Resolve-Path -LiteralPath $file).Path

    if (-not $VerifyOnly) {
        $signArguments = @(
            'sign', '/fd', 'SHA256', '/f', $env:WINDOWS_SIGNING_CERTIFICATE_PATH,
            '/p', $env:WINDOWS_SIGNING_CERTIFICATE_PASSWORD
        )
        $signArguments += @('/tr', $TimestampUrl, '/td', 'SHA256')
        $signArguments += $resolvedFile

        Write-Host "Signing $resolvedFile"
        & $signTool @signArguments
        if ($LASTEXITCODE -ne 0) {
            throw "SignTool failed to sign $resolvedFile with exit code $LASTEXITCODE"
        }
    }

    Write-Host "Verifying Authenticode signature for $resolvedFile"
    & $signTool verify /pa /v $resolvedFile
    if ($LASTEXITCODE -ne 0) {
        throw "Authenticode verification failed for $resolvedFile with exit code $LASTEXITCODE"
    }
}
