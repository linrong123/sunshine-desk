<#
.SYNOPSIS
生成办公配置候选文件，不修改现有配置或服务。
.DESCRIPTION
保留原配置的其他选项。仅设置内置自动分辨率和温和的显示器激活策略；不修改配对、凭据、应用目录、会话迁移或防火墙。
#>
[CmdletBinding()]
param(
    [string]$ConfigPath,
    [Parameter(Mandatory = $true)][string]$OutputPath,
    [string]$DisplayId
)
$ErrorActionPreference = 'Stop'
if (Test-Path -LiteralPath $OutputPath) { throw '输出文件已存在，请另选路径；不会覆盖。' }
$content = ''
if ($ConfigPath) { $content = [IO.File]::ReadAllText((Resolve-Path -LiteralPath $ConfigPath)) }
$settings = [ordered]@{
    dd_configuration_option = 'ensure_active'
    dd_resolution_option = 'auto'
    dd_refresh_rate_option = 'auto'
}
if ($DisplayId) {
    if ($DisplayId -match '[\r\n]') { throw '显示器标识不能包含换行。' }
    $settings['output_name'] = $DisplayId
}
foreach ($key in $settings.Keys) {
    $pattern = '(?m)^\s*' + [regex]::Escape($key) + '\s*=.*\r?\n?'
    $content = [regex]::Replace($content, $pattern, '')
}
$content = $content.TrimEnd() + "`r`n"
foreach ($key in $settings.Keys) { $content += "$key = $($settings[$key])`r`n" }
# CreateNew prevents races from overwriting an existing output file.
$stream = [IO.File]::Open($OutputPath, [IO.FileMode]::CreateNew, [IO.FileAccess]::Write)
try {
    $bytes = [Text.UTF8Encoding]::new($false).GetBytes($content)
    $stream.Write($bytes, 0, $bytes.Length)
} finally { $stream.Dispose() }
Write-Output '办公配置候选已生成；尚未应用。'
