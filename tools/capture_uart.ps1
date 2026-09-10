param([string]$PortName = "COM25", [int]$Seconds = 240)
$ErrorActionPreference = 'Continue'
[System.IO.Ports.SerialPort]::GetPortNames() | ForEach-Object { Write-Output "PORT_FOUND: $_" }
try {
    $port = New-Object System.IO.Ports.SerialPort($PortName, 115200, 'None', 8, 'One')
    $port.ReadTimeout = 500
    $port.Open()
    Write-Output "CAPTURING $PortName for $Seconds s"
    $deadline = (Get-Date).AddSeconds($Seconds)
    while ((Get-Date) -lt $deadline) {
        try { $line = $port.ReadLine(); Write-Output $line } catch { }
    }
    $port.Close()
    Write-Output "CAPTURE_DONE"
} catch {
    Write-Output "CAPTURE_FAILED: $($_.Exception.Message)"
}
