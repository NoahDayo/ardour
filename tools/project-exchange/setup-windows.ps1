[CmdletBinding()]
param(
    [string]$BuildRoot = (Join-Path (Split-Path (Resolve-Path "$PSScriptRoot/../..") -Parent) 'Ardour_Build')
)

$ErrorActionPreference = 'Stop'
$BuildRoot = [IO.Path]::GetFullPath($BuildRoot)
if ($BuildRoot -match '\s') {
    throw 'MSYS2 requires a build-tool directory without spaces.'
}

# Pin the bootstrap archive. Pacman validates signed packages separately.
$release = '2026-09-27'
$archiveName = 'msys2-base-x86_64-20260927.sfx.exe'
$expectedHash = 'ad336cccfda47758b5e15cda993fbba421115cb0b126697daef1ee4dfe37209f'
$msysRoot = Join-Path $BuildRoot 'msys64'
$bash = Join-Path $msysRoot 'usr/bin/bash.exe'

New-Item -ItemType Directory -Path $BuildRoot -Force | Out-Null
if (-not (Test-Path -LiteralPath $bash)) {
    # Avoid unpacking over an incomplete or unrelated installation.
    if (Test-Path -LiteralPath $msysRoot) {
        throw "An incomplete MSYS2 directory exists at $msysRoot. Inspect it before retrying."
    }
    $archive = Join-Path $BuildRoot $archiveName
    if (-not (Test-Path -LiteralPath $archive)) {
        Invoke-WebRequest -Uri "https://github.com/msys2/msys2-installer/releases/download/$release/$archiveName" -OutFile $archive
    }
    if ((Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash -ne $expectedHash) {
        throw 'MSYS2 bootstrap checksum mismatch. The archive was not executed.'
    }
    $extract = Start-Process -FilePath $archive -ArgumentList '-y', "-o$BuildRoot" -WindowStyle Hidden -Wait -PassThru
    if ($extract.ExitCode -ne 0 -or -not (Test-Path -LiteralPath $bash)) {
        throw 'MSYS2 extraction failed.'
    }
}

$previousMsystem = $env:MSYSTEM
$previousChere = $env:CHERE_INVOKING
$previousPackageList = $env:ARDOUR_PACKAGE_LIST
try {
    $env:MSYSTEM = 'UCRT64'
    $env:CHERE_INVOKING = '1'
    $env:ARDOUR_PACKAGE_LIST = Join-Path $PSScriptRoot 'msys2-packages.txt'
    # A runtime upgrade closes the first shell. Always use a fresh shell for
    # the second update, then require success before installing dependencies.
    & $bash -lc 'pacman -Syu --noconfirm'
    & $bash -lc 'pacman -Syu --noconfirm'
    if ($LASTEXITCODE -ne 0) { throw 'MSYS2 update failed.' }
    & $bash -lc 'mapfile -t packages < "$(cygpath -u -- "$ARDOUR_PACKAGE_LIST")"; pacman -S --needed --noconfirm "${packages[@]}"'
    if ($LASTEXITCODE -ne 0) { throw 'Ardour dependency installation failed.' }
    Write-Output "Toolchain ready: $msysRoot"
    Write-Output 'In a UCRT64 shell, run bash tools/project-exchange/build-windows.sh from the repository.'
} finally {
    $env:MSYSTEM = $previousMsystem
    $env:CHERE_INVOKING = $previousChere
    $env:ARDOUR_PACKAGE_LIST = $previousPackageList
}
