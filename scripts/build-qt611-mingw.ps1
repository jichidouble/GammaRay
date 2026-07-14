param(
    [string]$QtRoot = 'D:\Qt',
    [string]$BuildDirectory = 'build-qt6.11.1-mingw',
    [ValidateSet('Debug', 'Release', 'RelWithDebInfo')]
    [string]$BuildType = 'RelWithDebInfo',
    [int]$Parallel = 8,
    [switch]$Install
)

$ErrorActionPreference = 'Stop'
$RepositoryRoot = Split-Path -Parent $PSScriptRoot
if (-not [System.IO.Path]::IsPathRooted($BuildDirectory)) {
    $BuildDirectory = Join-Path $RepositoryRoot $BuildDirectory
}
$BuildDirectory = [System.IO.Path]::GetFullPath($BuildDirectory)

$QtPrefix = Join-Path $QtRoot '6.11.1\mingw_64'
$MingwBin = Join-Path $QtRoot 'Tools\mingw1310_64\bin'
$CMake = Join-Path $QtRoot 'Tools\CMake_64\bin\cmake.exe'
$Ninja = Join-Path $QtRoot 'Tools\Ninja\ninja.exe'
$InstallPrefix = Join-Path $RepositoryRoot 'dist\qt6.11.1-mingw'

$RequiredPaths = @(
    (Join-Path $QtPrefix 'bin\qmake.exe'),
    (Join-Path $MingwBin 'gcc.exe'),
    (Join-Path $MingwBin 'g++.exe'),
    $CMake,
    $Ninja
)
foreach ($Path in $RequiredPaths) {
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        throw "Required Qt development tool was not found: $Path"
    }
}

$env:PATH = "$($QtPrefix)\bin;$MingwBin;$([System.IO.Path]::GetDirectoryName($CMake));$([System.IO.Path]::GetDirectoryName($Ninja));$env:PATH"

& $CMake -S $RepositoryRoot -B $BuildDirectory -G Ninja `
    "-DCMAKE_PREFIX_PATH=$($QtPrefix -replace '\\','/')" `
    "-DCMAKE_C_COMPILER=$((Join-Path $MingwBin 'gcc.exe') -replace '\\','/')" `
    "-DCMAKE_CXX_COMPILER=$((Join-Path $MingwBin 'g++.exe') -replace '\\','/')" `
    "-DCMAKE_MAKE_PROGRAM=$($Ninja -replace '\\','/')" `
    "-DCMAKE_BUILD_TYPE=$BuildType" `
    "-DCMAKE_INSTALL_PREFIX=$($InstallPrefix -replace '\\','/')" `
    -DGAMMARAY_BUILD_DOCS=OFF `
    -DBUILD_TESTING=OFF `
    -DGAMMARAY_BUILD_UI=ON `
    -DGAMMARAY_BUILD_MCP=ON
if ($LASTEXITCODE -ne 0) {
    throw "CMake configuration failed with exit code $LASTEXITCODE"
}

& $CMake --build $BuildDirectory --parallel $Parallel
if ($LASTEXITCODE -ne 0) {
    throw "Build failed with exit code $LASTEXITCODE"
}

if ($Install) {
    & $CMake --install $BuildDirectory
    if ($LASTEXITCODE -ne 0) {
        throw "Install failed with exit code $LASTEXITCODE"
    }
}
