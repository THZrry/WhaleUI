# fetch-3rdparty.ps1
# Downloads official prebuilt third-party packages into 3rdparty/ so xmake can
# link them directly (mode A). Run from the repo root. Requires internet access.
#
# Current packages:
#   sdl3_ttf 3.2.2 - mingw-w64 x64 devel package (include + import lib + dll)
#                    https://github.com/libsdl-org/SDL_ttf/releases

param(
    [switch]$Force
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
Set-Location $root

function Fetch-Zip {
    param($Url, $DestDir, $InnerArch)
    $zip = Join-Path $env:TEMP ("whaleui-" + [IO.Path]::GetFileName($Url))
    Write-Host "==> downloading $Url"
    curl.exe -s -L -o $zip --max-time 300 -w "    %{http_code} %{size_download} bytes`n" $Url
    $staging = Join-Path $env:TEMP ("whaleui-extract-" + [guid]::NewGuid().ToString("N"))
    Expand-Archive -Path $zip -DestinationPath $staging -Force
    # find the arch dir inside the devel package, copy its contents up
    $archdir = Get-ChildItem $staging -Recurse -Directory -Filter $InnerArch | Select-Object -First 1
    if (-not $archdir) { throw "arch dir '$InnerArch' not found in $Url" }
    New-Item -ItemType Directory -Force -Path $DestDir | Out-Null
    Copy-Item (Join-Path $archdir.FullName "*") $DestDir -Recurse -Force
    # also copy top-level docs (README/LICENSE/INSTALL) if present
    foreach ($f in @("README.md", "LICENSE.txt", "INSTALL.md")) {
        $top = Join-Path $staging $f
        if (Test-Path $top) { Copy-Item $top $DestDir -Force }
    }
    Remove-Item $staging -Recurse -Force
    Remove-Item $zip -Force
    Write-Host "    done -> $DestDir"
}

$sdl3ttf = Join-Path $root "3rdparty\sdl3_ttf"
if (-not $Force -and (Test-Path (Join-Path $sdl3ttf "lib\libSDL3_ttf.dll.a"))) {
    Write-Host "sdl3_ttf already present. Use -Force to re-fetch."
} else {
    Fetch-Zip `
        -Url "https://github.com/libsdl-org/SDL_ttf/releases/download/release-3.2.2/SDL3_ttf-devel-3.2.2-mingw.zip" `
        -DestDir $sdl3ttf `
        -InnerArch "x86_64-w64-mingw32"
}

# --- SDL3 (shared runtime: SDL3.dll, imported by SDL3_ttf.dll) ---
$sdl3 = Join-Path $root "3rdparty\sdl3"
if (-not $Force -and (Test-Path (Join-Path $sdl3 "lib\libSDL3.dll.a"))) {
    Write-Host "sdl3 already present. Use -Force to re-fetch."
} else {
    $tar = Join-Path $env:TEMP "SDL3-devel-mingw.tar.gz"
    Write-Host "==> downloading SDL3 mingw devel"
    curl.exe -s -L -o $tar --max-time 300 `
        "https://github.com/libsdl-org/SDL/releases/download/release-3.4.4/SDL3-devel-3.4.4-mingw.tar.gz"
    New-Item -ItemType Directory -Force -Path $sdl3 | Out-Null
    tar.exe -xzf $tar -C $sdl3 --strip-components=2 "SDL3-3.4.4/x86_64-w64-mingw32"
    Remove-Item $tar -Force
    Write-Host "    done -> $sdl3"
}

Write-Host ""
Write-Host "Done. Now run:  xmake f -p mingw -y  &&  xmake"
