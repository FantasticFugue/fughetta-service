param(
    [Parameter(Mandatory = $true)][string]$Prefix,
    [Parameter(Mandatory = $true)][string]$Workspace
)
$ErrorActionPreference = 'Stop'
$Prefix = [System.IO.Path]::GetFullPath($Prefix)

# Config and data are created outside MSI's file table and survive uninstall.
[System.IO.Directory]::CreateDirectory($Workspace) | Out-Null
$acl = New-Object System.Security.AccessControl.DirectorySecurity
$acl.SetAccessRuleProtection($true, $false)
foreach ($sid in @('S-1-5-18', 'S-1-5-32-544', 'S-1-5-19')) {
    $identity = New-Object System.Security.Principal.SecurityIdentifier($sid)
    $rule = New-Object System.Security.AccessControl.FileSystemAccessRule(
        $identity, 'FullControl', 'ContainerInherit,ObjectInherit', 'None', 'Allow')
    $acl.AddAccessRule($rule)
}
Set-Acl -LiteralPath $Workspace -AclObject $acl
foreach ($name in @('state', 'logs')) {
    [System.IO.Directory]::CreateDirectory((Join-Path $Workspace $name)) | Out-Null
}
$config = Join-Path $Workspace 'config.json'
if (-not (Test-Path -LiteralPath $config)) {
    $template = Get-Content -Raw -LiteralPath (Join-Path $Prefix 'share/fughetta-service/config.json.in')
    # JSON escapes path characters before inserting them into quoted strings.
    $installPath = ($Prefix.TrimEnd('\').Replace('\', '/') | ConvertTo-Json -Compress).Trim('"')
    $statePath = ((Join-Path $Workspace 'state').Replace('\', '/') | ConvertTo-Json -Compress).Trim('"')
    $text = $template.Replace('@PREFIX@', $installPath).Replace('@STATE@', $statePath).Replace('@EXE@', '.exe')
    [System.IO.File]::WriteAllText($config, $text, (New-Object System.Text.UTF8Encoding($false)))
}
