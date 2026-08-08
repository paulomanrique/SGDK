# Build the one-time local SDL2 static dependency used by build-pc.ps1.
param([switch]$Clean)

$ErrorActionPreference = "Stop"
$repo = Split-Path -Parent $PSScriptRoot
$root = Join-Path $repo "third_party\sdl2"
$version = "2.32.10"
$archiveName = "SDL2-$version.zip"
$archive = Join-Path $root $archiveName
$source = Join-Path $root "src\SDL2-$version"
$build = Join-Path $root "build"
$install = Join-Path $root "install"
$url = "https://github.com/libsdl-org/SDL/releases/download/release-$version/$archiveName"
$expectedSha256 = "12b2dc2eb8f2836100a7916b5d394a0c82f1f7e32693f95f98305403af242f08"

$bundledCmake = "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
$cmakeCommand = Get-Command cmake -ErrorAction SilentlyContinue
if ($cmakeCommand) {
    $cmake = $cmakeCommand.Source
} elseif (Test-Path -LiteralPath $bundledCmake) {
    $cmake = $bundledCmake
} else {
    throw "CMake was not found. Install the Visual Studio C++ CMake tools or put cmake on PATH."
}

if ($Clean) {
    foreach ($path in @($build, $install)) {
        if (Test-Path -LiteralPath $path) {
            $resolved = (Resolve-Path -LiteralPath $path).Path
            if (-not $resolved.StartsWith((Join-Path $repo "third_party"), [StringComparison]::OrdinalIgnoreCase)) {
                throw "Refusing to clean path outside third_party: $resolved"
            }
            Remove-Item -LiteralPath $resolved -Recurse -Force
        }
    }
}

New-Item -ItemType Directory -Path $root -Force | Out-Null
if (-not (Test-Path -LiteralPath $archive)) {
    $scoopCache = Join-Path $env:USERPROFILE "scoop\cache"
    $cached = Get-ChildItem $scoopCache -Filter "sdl2#$version#*.zip" -File -ErrorAction SilentlyContinue |
        Where-Object { (Get-FileHash $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant() -eq $expectedSha256 } |
        Select-Object -First 1
    if ($cached) {
        Copy-Item -LiteralPath $cached.FullName -Destination $archive
    } else {
        Write-Host "Downloading SDL2 $version source from the official release..."
        Invoke-WebRequest -UseBasicParsing -Uri $url -OutFile $archive
    }
}

$actualSha256 = (Get-FileHash $archive -Algorithm SHA256).Hash.ToLowerInvariant()
if ($actualSha256 -ne $expectedSha256) {
    throw "SDL2 source SHA-256 mismatch: expected $expectedSha256, got $actualSha256"
}

if (-not (Test-Path -LiteralPath (Join-Path $source "CMakeLists.txt"))) {
    $sourceParent = Split-Path -Parent $source
    New-Item -ItemType Directory -Path $sourceParent -Force | Out-Null
    Expand-Archive -LiteralPath $archive -DestinationPath $sourceParent -Force
}

& $cmake -S $source -B $build -A x64 `
    -DSDL_SHARED=OFF -DSDL_STATIC=ON -DSDL_TESTS=OFF `
    -DSDL_FORCE_STATIC_VCRT=ON -DCMAKE_INSTALL_PREFIX=$install
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
& $cmake --build $build --config Release --target install
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

$library = Join-Path $install "lib\SDL2-static.lib"
if (-not (Test-Path -LiteralPath $library)) {
    throw "SDL2 static build completed but $library was not produced."
}
Write-Host "OK -> third_party/sdl2/install/lib/SDL2-static.lib"
