# vim: set ft=ps1 fenc=utf-8 ff=unix :
# ==================================================
# WinFocus タスクスケジューラ登録スクリプト
#
# ログオン時から 6 分おきに winfocus.exe --save を
# バックグラウンドで実行するタスクを登録する。
#
# 動作要件:
#   - PowerShell 7 以降 (pwsh.exe)
#   - %USERPROFILE%\scoop\shims\winfocus.exe が存在すること
# ==================================================

# 管理者権限チェック：なければ自動昇格して再実行
$isAdmin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole(
    [Security.Principal.WindowsBuiltInRole]::Administrator)
if (-not $isAdmin) {
    Start-Process pwsh -Verb RunAs -ArgumentList "-ExecutionPolicy Bypass -File `"$PSCommandPath`""
    exit
}

$taskName    = "WinFocus_AutoSave"
$taskPath    = "\"
$winfocusExe = "$env:USERPROFILE\scoop\shims\winfocus.exe"

# アクション：pwsh 経由で隠し実行
$action = New-ScheduledTaskAction `
    -Execute  "pwsh.exe" `
    -Argument "-WindowStyle Hidden -NonInteractive -Command `"& '$winfocusExe' --save`""

# トリガー：ログオン時に開始（RepetitionInterval は後で XML パッチ）
$trigger = New-ScheduledTaskTrigger -AtLogOn

# 設定
$settings = New-ScheduledTaskSettingsSet `
    -ExecutionTimeLimit (New-TimeSpan -Minutes 2) `
    -MultipleInstances  IgnoreNew `
    -StartWhenAvailable

# 実行ユーザー（対話型・昇格なし）
$principal = New-ScheduledTaskPrincipal `
    -UserId    ([System.Security.Principal.WindowsIdentity]::GetCurrent().Name) `
    -LogonType Interactive `
    -RunLevel  Limited

# 初回登録
Register-ScheduledTask `
    -TaskName  $taskName `
    -TaskPath  $taskPath `
    -Action    $action `
    -Trigger   $trigger `
    -Settings  $settings `
    -Principal $principal `
    -Force | Out-Null

# RepetitionInterval を XML パッチで設定（PT6M = 6 分、Duration 省略 = 無期限）
$taskXml = [xml](Get-ScheduledTask -TaskName $taskName -TaskPath $taskPath | Export-ScheduledTask)
$ns      = "http://schemas.microsoft.com/windows/2004/02/mit/task"
$logon   = $taskXml.Task.Triggers.LogonTrigger

$rep = $logon.SelectSingleNode("*[local-name()='Repetition']")
if ($null -eq $rep) {
    $rep = $taskXml.CreateElement("Repetition", $ns)
    $logon.AppendChild($rep) | Out-Null
}

$intNode = $taskXml.CreateElement("Interval", $ns)
$intNode.InnerText = "PT6M"
$rep.AppendChild($intNode) | Out-Null

Register-ScheduledTask `
    -TaskName $taskName `
    -TaskPath $taskPath `
    -Xml      $taskXml.OuterXml `
    -Force | Out-Null

Write-Host "登録完了: $taskName（6 分おき、ログオン時から）"
