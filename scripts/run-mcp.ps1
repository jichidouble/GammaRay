param(
    [string]$QtRoot = 'D:\Qt',
    [string]$BuildDirectory = 'build-qt6.11.1-mingw'
)

$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
$RepositoryRoot = Split-Path -Parent $PSScriptRoot
if (-not [System.IO.Path]::IsPathRooted($BuildDirectory)) {
    $BuildDirectory = Join-Path $RepositoryRoot $BuildDirectory
}
$BuildDirectory = [System.IO.Path]::GetFullPath($BuildDirectory)

$BinDirectory = Join-Path $BuildDirectory 'bin'
$Server = Join-Path $BinDirectory 'gammaray-mcp.exe'
$QtBin = Join-Path $QtRoot '6.11.1\mingw_64\bin'
$MingwBin = Join-Path $QtRoot 'Tools\mingw1310_64\bin'
if (-not (Test-Path -LiteralPath $Server -PathType Leaf)) {
    throw "GammaRay MCP server was not built: $Server"
}

$env:PATH = "$BinDirectory;$QtBin;$MingwBin;$env:PATH"
$env:QT_QPA_PLATFORM = 'offscreen'
& $Server
exit $LASTEXITCODE
