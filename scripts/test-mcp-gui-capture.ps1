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
$Target = Join-Path $BinDirectory 'gammaray-mcp-gui-test-target.exe'
$QtBin = Join-Path $QtRoot '6.11.1\mingw_64\bin'
$MingwBin = Join-Path $QtRoot 'Tools\mingw1310_64\bin'

foreach ($path in @($Server, $Target)) {
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
    # Windows PowerShell 5.1 lacks ConvertFrom-Json -Depth.  MCP responses in
    # this test do not require the newer parser option.
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
        clientInfo = @{ name = 'gammaray-mcp-gui-capture-test'; version = '1.0' }
    })
    $serverProcess.StandardInput.WriteLine('{"jsonrpc":"2.0","method":"notifications/initialized"}')
    $serverProcess.StandardInput.Flush()

    $tools = Invoke-Mcp 'tools/list' @{}
    $toolNames = @($tools.tools | ForEach-Object { $_.name })
    foreach ($requiredTool in @('gammaray_grab_widget', 'gammaray_grab_window')) {
        if ($toolNames -notcontains $requiredTool) {
            throw "Screenshot tool is not advertised: $requiredTool"
        }
    }

    $launch = Invoke-Mcp 'tools/call' ([ordered]@{
        name = 'gammaray_launch'
        arguments = [ordered]@{
            executable = $Target
            arguments = @('120000')
            environment = @{ QT_QPA_PLATFORM = 'windows' }
        }
    })
    if ($launch.isError) {
        throw "Could not launch GUI target: $($launch.structuredContent | ConvertTo-Json -Compress)"
    }

    $objects = Invoke-Mcp 'tools/call' ([ordered]@{
        name = 'gammaray_list_objects'
        arguments = @{ pattern = 'screenshotButton'; maxDepth = 12; limit = 10; timeoutMs = 15000 }
    })
    $button = @($objects.structuredContent.objects | Where-Object { $_.name -eq 'screenshotButton' }) | Select-Object -First 1
    if ($null -eq $button) {
        throw 'The GUI test target did not expose screenshotButton in the QObject tree.'
    }

    $captures = @{}
    foreach ($toolName in @('gammaray_grab_widget', 'gammaray_grab_window')) {
        $capture = Invoke-Mcp 'tools/call' ([ordered]@{
            name = $toolName
            arguments = @{ objectPath = $button.path; maxWidth = 720; maxHeight = 420; timeoutMs = 15000 }
        })
        if ($capture.isError) {
            throw "$toolName returned an error: $($capture.structuredContent | ConvertTo-Json -Compress)"
        }
        $image = @($capture.content | Where-Object { $_.type -eq 'image' }) | Select-Object -First 1
        if ($null -eq $image -or $image.mimeType -ne 'image/png' -or [string]::IsNullOrWhiteSpace($image.data)) {
            throw "$toolName did not return a PNG image content item."
        }
        $bytes = [Convert]::FromBase64String($image.data)
        if ($bytes.Length -lt 100 -or $bytes[0..7] -join ',' -ne '137,80,78,71,13,10,26,10') {
            throw "$toolName returned invalid PNG bytes."
        }
        if ($capture.structuredContent.captureKind -ne $toolName.Substring('gammaray_grab_'.Length)) {
            throw "$toolName returned an unexpected capture metadata kind."
        }
        $captures[$toolName] = $capture
    }

    $widgetHeight = [int]$captures['gammaray_grab_widget'].structuredContent.sourceImage.height
    $windowHeight = [int]$captures['gammaray_grab_window'].structuredContent.sourceImage.height
    if ($widgetHeight -ge $windowHeight) {
        throw "Widget capture height ($widgetHeight) is not smaller than its containing window ($windowHeight)."
    }

    Write-Output 'GammaRay MCP GUI capture integration test passed.'
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
