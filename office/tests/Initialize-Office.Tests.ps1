$ErrorActionPreference = 'Stop'
$root = Join-Path ([IO.Path]::GetTempPath()) ([guid]::NewGuid().ToString())
New-Item -ItemType Directory -Path $root | Out-Null
try {
    $original = Join-Path $root 'original.conf'
    $output = Join-Path $root 'candidate.conf'
    $second = Join-Path $root 'second.conf'
    $inputText = "# existing`nport = 49999`ndd_resolution_option = manual`n"
    [IO.File]::WriteAllText($original, $inputText)
    $script = Join-Path $PSScriptRoot '../windows/Initialize-Office.ps1'
    & $script -ConfigPath $original -OutputPath $output -DisplayId 'test-display'
    if ([IO.File]::ReadAllText($original) -cne $inputText) { throw '原配置被修改。' }
    $result = [IO.File]::ReadAllText($output)
    if ($result -notmatch 'port = 49999' -or $result -match '= manual' -or
        $result -notmatch 'dd_resolution_option = auto' -or $result -notmatch 'output_name = test-display') { throw '配置生成错误。' }
    & $script -ConfigPath $output -OutputPath $second -DisplayId 'test-display'
    if ([IO.File]::ReadAllText($second) -cne $result) { throw '配置生成不幂等。' }
    $rejected = $false
    try { & $script -OutputPath $output } catch { $rejected = $true }
    if (!$rejected) { throw '已有文件应拒绝覆盖。' }
    $rejected = $false
    try { & $script -OutputPath (Join-Path $root 'bad.conf') -DisplayId "bad`nport=1" } catch { $rejected = $true }
    if (!$rejected) { throw '不应接受多行显示器标识。' }
    Write-Output '初始化脚本检查通过。'
} finally { Remove-Item -LiteralPath $root -Recurse -Force }
