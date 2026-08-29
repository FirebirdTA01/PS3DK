param(
    [string]$LockPath = "C:\ps3boot\.rpcs3-owner",
    [string]$Owner = "$env:USERNAME@$env:COMPUTERNAME"
)

$ErrorActionPreference = "Stop"

$parent = Split-Path -Parent $LockPath
if ($parent) {
    New-Item -ItemType Directory -Force -Path $parent | Out-Null
}

$payload = [ordered]@{
    owner = $Owner
    pid = $PID
    host = $env:COMPUTERNAME
    timestamp = (Get-Date).ToString("o")
} | ConvertTo-Json -Compress

try {
    $stream = [System.IO.File]::Open($LockPath,
        [System.IO.FileMode]::CreateNew,
        [System.IO.FileAccess]::Write,
        [System.IO.FileShare]::None)
    try {
        $writer = New-Object System.IO.StreamWriter($stream)
        try {
            $writer.WriteLine($payload)
        } finally {
            $writer.Dispose()
        }
    } finally {
        $stream.Dispose()
    }
} catch [System.IO.IOException] {
    Write-Error "RPCS3 lock already exists at $LockPath. Current owner: $(Get-Content -Raw -Path $LockPath -ErrorAction SilentlyContinue)"
    exit 1
}

Write-Host "claimed $LockPath for $Owner"
