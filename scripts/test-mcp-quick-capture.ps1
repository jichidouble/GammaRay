param(
    [string]$QtRoot = 'D:\Qt',
    [string]$BuildDirectory = 'build-qt6.11.1-mingw',
    [string]$QuickTarget = 'E:\Dev\qml-rhi\build\qml_rhi.exe'
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
$QuickTarget = [System.IO.Path]::GetFullPath($QuickTarget)
$QuickTargetProcessName = [System.IO.Path]::GetFileNameWithoutExtension($QuickTarget)
$ExistingQuickTargetPids = @(
    Get-Process -Name $QuickTargetProcessName -ErrorAction SilentlyContinue |
        Where-Object { $_.Path -eq $QuickTarget } |
        ForEach-Object { $_.Id }
)

foreach ($path in @($Server, $QuickTarget)) {
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
        throw "Required executable was not built: $path"
    }
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
$startInfo.Environment['QT_QPA_PLATFORM'] = 'offscreen'
$startInfo.Environment['QSG_RHI_BACKEND'] = 'opengl'
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
        $exitCode = if ($serverProcess.HasExited) { $serverProcess.ExitCode } else { 'unknown' }
        throw "MCP process closed while waiting for $Method (exit $exitCode). stderr: $($serverProcess.StandardError.ReadToEnd())"
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

    $launch = Invoke-Mcp 'tools/call' ([ordered]@{
        name = 'gammaray_launch'
        arguments = [ordered]@{
            executable = $QuickTarget
            environment = @{ QT_QPA_PLATFORM = 'offscreen'; QSG_RHI_BACKEND = 'opengl' }
        }
    })
    if ($launch.isError) {
        throw "Could not launch Qt Quick target: $($launch.structuredContent | ConvertTo-Json -Compress)"
    }

    $quickItems = Invoke-Mcp 'tools/call' ([ordered]@{
        name = 'gammaray_list_quick_items'
        arguments = @{ pattern = 'QQuickItem|TriangleRhiItem'; maxDepth = 12; limit = 100; timeoutMs = 15000 }
    })
    if ($quickItems.isError) {
        throw "gammaray_list_quick_items returned an error: $($quickItems.structuredContent | ConvertTo-Json -Compress)"
    }
    foreach ($quickItem in @($quickItems.structuredContent.items)) {
        if ([string]::IsNullOrWhiteSpace($quickItem.objectPath)) {
            throw "gammaray_list_quick_items returned an item without a mapped objectPath: $($quickItem | ConvertTo-Json -Compress)"
        }
    }
    $item = @($quickItems.structuredContent.items | Where-Object {
        $_.type -match 'TriangleRhiItem' -and -not [string]::IsNullOrWhiteSpace($_.objectPath)
    }) | Select-Object -First 1
    if ($null -eq $item) {
        throw "The Qt Quick target did not expose TriangleRhiItem with an objectPath: $($quickItems.structuredContent | ConvertTo-Json -Compress -Depth 16)"
    }

    $captures = @{}
    foreach ($toolName in @('gammaray_grab_quick_window', 'gammaray_grab_quick_item')) {
        $capture = Invoke-Mcp 'tools/call' ([ordered]@{
            name = $toolName
            arguments = @{ objectPath = $item.objectPath; maxWidth = 720; maxHeight = 420; timeoutMs = 15000 }
        })
        if ($capture.isError) {
            throw "$toolName returned an error: $($capture.structuredContent | ConvertTo-Json -Compress -Depth 16)"
        }
        $images = @($capture.content | Where-Object { $_.type -eq 'image' })
        if ($images.Count -ne 1 -or $images[0].mimeType -ne 'image/png' -or [string]::IsNullOrWhiteSpace($images[0].data)) {
            throw "$toolName did not return exactly one PNG image content item."
        }
        $bytes = [Convert]::FromBase64String($images[0].data)
        if ($bytes.Length -lt 100 -or $bytes[0..7] -join ',' -ne '137,80,78,71,13,10,26,10') {
            throw "$toolName returned invalid PNG bytes."
        }
        $expectedKind = if ($toolName -eq 'gammaray_grab_quick_window') { 'quickWindow' } else { 'quickItem' }
        if ($capture.structuredContent.captureKind -ne $expectedKind) {
            throw "$toolName returned an unexpected capture metadata kind."
        }
        if ([int]$capture.structuredContent.image.width -le 0 -or [int]$capture.structuredContent.image.height -le 0) {
            throw "$toolName returned non-positive image dimensions."
        }
        if ($toolName -eq 'gammaray_grab_quick_item' -and
            ([int]$capture.structuredContent.cropRect.width -le 0 -or [int]$capture.structuredContent.cropRect.height -le 0)) {
            throw "$toolName returned a non-positive crop rectangle."
        }
        $captures[$toolName] = $capture
    }

    Write-Output 'GammaRay MCP Qt Quick capture integration test passed.'
}
finally {
    if (-not $serverProcess.HasExited) {
        $serverProcess.StandardInput.Close()
        if (-not $serverProcess.WaitForExit(5000)) {
            $serverProcess.Kill()
        }
    }
    $serverProcess.Dispose()

    Get-Process -Name $QuickTargetProcessName -ErrorAction SilentlyContinue |
        Where-Object { $_.Path -eq $QuickTarget -and $ExistingQuickTargetPids -notcontains $_.Id } |
        Stop-Process -Force
}
