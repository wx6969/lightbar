param([string]$Before)
$before = @($Before -split ',' | Where-Object { $_ })
$deadline = (Get-Date).AddSeconds(20)
do {
    Start-Sleep -Milliseconds 500
    $now = @(Get-CimInstance Win32_SerialPort -ErrorAction SilentlyContinue | ForEach-Object { $_.DeviceID })
    $new = @($now | Where-Object { $_ -notin $before })
    if ($new.Count -eq 1) { $new[0]; exit 0 }
} while ((Get-Date) -lt $deadline)
if ($new.Count -gt 1) {
    Write-Host "检测到多个新增端口：$($new -join ', ')" -ForegroundColor Yellow
    $new[0]
}
