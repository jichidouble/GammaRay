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
    throw "Required executable was not built: $Server"
}

$startInfo = [System.Diagnostics.ProcessStartInfo]::new()
$startInfo.FileName = $Server
$startInfo.WorkingDirectory = $RepositoryRoot
$startInfo.UseShellExecute = $false
$startInfo.RedirectStandardInput = $true
$startInfo.RedirectStandardOutput = $true
$startInfo.RedirectStandardError = $true
$startInfo.CreateNoWindow = $true
$startInfo.Environment['PATH'] = "$BinDirectory;$QtBin;$MingwBin;$($startInfo.Environment['PATH'])"
$serverProcess = [System.Diagnostics.Process]::new()
$serverProcess.StartInfo = $startInfo
if (-not $serverProcess.Start()) {
    throw 'Could not start gammaray-mcp.'
}

$nextId = 0
function Invoke-Mcp {
    param([string]$Method, [object]$Params)
    $script:nextId++
    $request = [ordered]@{ jsonrpc = '2.0'; id = $script:nextId; method = $Method }
    if ($null -ne $Params) {
        $request.params = $Params
    }
    $serverProcess.StandardInput.WriteLine(($request | ConvertTo-Json -Compress -Depth 16))
    $serverProcess.StandardInput.Flush()
    $lineTask = $serverProcess.StandardOutput.ReadLineAsync()
    if (-not $lineTask.Wait(70000)) {
        throw "Timed out waiting for MCP response to $Method."
    }
    $line = $lineTask.Result
    if ([string]::IsNullOrWhiteSpace($line)) {
        throw "MCP process closed while waiting for $Method. stderr: $($serverProcess.StandardError.ReadToEnd())"
    }
    $response = $line | ConvertFrom-Json
    if ($response.error) {
        throw "MCP protocol error: $($response.error.message)"
    }
    return $response.result
}

try {
    $null = Invoke-Mcp 'initialize' ([ordered]@{
        protocolVersion = '2025-11-25'
        capabilities = @{}
        clientInfo = @{ name = 'gammaray-mcp-quick-capture-test'; version = '1.0' }
    })
    $serverProcess.StandardInput.WriteLine('{"jsonrpc":"2.0","method":"notifications/initialized"}')
    $serverProcess.StandardInput.Flush()

    $tools = Invoke-Mcp 'tools/list' @{}
    $toolNames = @($tools.tools | ForEach-Object { $_.name })
    foreach ($requiredTool in @('gammaray_list_quick_items', 'gammaray_grab_quick_window', 'gammaray_grab_quick_item')) {
        if ($toolNames -notcontains $requiredTool) {
            throw "Qt Quick MCP tool is not advertised: $requiredTool"
        }
    }

    Write-Output 'GammaRay MCP Qt Quick tool advertisement test passed.'
}
finally {
    if (-not $serverProcess.HasExited) {
        $serverProcess.StandardInput.Close()
        if (-not $serverProcess.WaitForExit(5000)) {
            $serverProcess.Kill()
        }
    }
    $serverProcess.Dispose()
}
