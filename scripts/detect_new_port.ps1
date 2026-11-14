param([string]$Before,[string]$OutFile)
$old=@($Before -split ',' | Where-Object {$_})
$end=(Get-Date).AddSeconds(30)
do {
  Start-Sleep -Milliseconds 500
  $now=@([IO.Ports.SerialPort]::GetPortNames())
  $new=@($now | Where-Object {$_ -notin $old})
  if($new.Count -eq 1){$new[0] | Set-Content -Encoding ASCII $OutFile; exit}
} while((Get-Date)-lt $end)
if($new.Count -gt 0){$new[0] | Set-Content -Encoding ASCII $OutFile}
