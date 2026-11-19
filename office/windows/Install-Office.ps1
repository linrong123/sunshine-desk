<#
.SYNOPSIS
在全新 Windows 测试机上复用 Sunshine MSI 安装器，不替换现有服务。
.DESCRIPTION
必须提供经独立核对的安装包 SHA256。已有 Sunshine 安装或进程时拒绝继续。不会安装驱动、导入凭据、跳过配对或重启系统。
#>
[CmdletBinding(SupportsShouldProcess = $true)]
param(
    [Parameter(Mandatory = $true)][string]$InstallerPath,
    [Parameter(Mandatory = $true)][ValidatePattern('^[a-fA-F0-9]{64}$')][string]$ExpectedSha256
)
$ErrorActionPreference = 'Stop'
if ($env:OS -ne 'Windows_NT') { throw '此安装流程仅支持 Windows。' }
if ((Get-Service -Name SunshineService, SunshineSvc -ErrorAction SilentlyContinue) -or
    (Get-Process -Name sunshine, sunshinesvc -ErrorAction SilentlyContinue)) {
    throw '检测到现有 Sunshine 服务或进程，请使用全新测试机；不会替换。'
}
$installer = (Resolve-Path -LiteralPath $InstallerPath).Path
if ([IO.Path]::GetExtension($installer) -ne '.msi') { throw '需要 MSI 安装包。' }
if ((Get-FileHash -LiteralPath $installer -Algorithm SHA256).Hash -ne $ExpectedSha256) { throw '安装包 SHA256 不匹配。' }
if ($PSCmdlet.ShouldProcess('本机', '安装办公版 Sunshine MSI（不重启）')) {
    $result = Start-Process -FilePath msiexec.exe -ArgumentList @('/i', ('"' + $installer + '"'), '/passive', '/norestart') -Wait -PassThru
    if ($result.ExitCode -notin @(0, 3010)) { throw "MSI 安装失败，退出码：$($result.ExitCode)" }
    if ($result.ExitCode -eq 3010) { Write-Output '安装器报告需要重启；脚本未重启。' }
    Write-Output 'MSI 已执行；仍需首次设置账户、配对和串流验收。'
}
