[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string[]]$Files,
    [string]$TimestampUrl = 'http://timestamp.digicert.com',
    [switch]$VerifyOnly,
    [switch]$NoTimestamp
)

$ErrorActionPreference = 'Stop'

$sdkRoot = Join-Path ${env:ProgramFiles(x86)} 'Windows Kits\10\bin'
if (-not (Test-Path -LiteralPath $sdkRoot -PathType Container)) {
	throw "Windows SDK bin directory not found: $sdkRoot"
}

$tools = @(Get-ChildItem -LiteralPath $sdkRoot -Recurse -File -Filter 'signtool.exe' -ErrorAction Stop |
    Where-Object { $_.FullName -match '[\\/](x64|arm64)[\\/]signtool\.exe$' })
$preferredArchitecture = if ([System.Runtime.InteropServices.RuntimeInformation]::OSArchitecture.ToString() -eq 'Arm64') {
    'arm64'
} else {
    'x64'
}
$signTool = $tools |
    Where-Object { $_.FullName -match "[\\/]$preferredArchitecture[\\/]signtool\.exe$" } |
    Sort-Object -Property { [version]$_.Directory.Parent.Name } -Descending |
    Select-Object -First 1 -ExpandProperty FullName
if ([string]::IsNullOrWhiteSpace($signTool) -and $preferredArchitecture -eq 'arm64') {
    $signTool = $tools |
        Where-Object { $_.FullName -match '[\\/]x64[\\/]signtool\.exe$' } |
        Sort-Object -Property { [version]$_.Directory.Parent.Name } -Descending |
        Select-Object -First 1 -ExpandProperty FullName
}
if ([string]::IsNullOrWhiteSpace($signTool)) {
    throw "No SignTool.exe for $preferredArchitecture was found under $sdkRoot"
}

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
        if (-not $NoTimestamp) {
            $signArguments += @('/tr', $TimestampUrl, '/td', 'SHA256')
        }
        $signArguments += $resolvedFile

        & $signTool @signArguments
        if ($LASTEXITCODE -ne 0) {
            throw "SignTool failed to sign $resolvedFile with exit code $LASTEXITCODE"
        }
    }

    & $signTool verify /pa /v $resolvedFile
    if ($LASTEXITCODE -ne 0) {
        throw "Authenticode verification failed for $resolvedFile with exit code $LASTEXITCODE"
    }
}
