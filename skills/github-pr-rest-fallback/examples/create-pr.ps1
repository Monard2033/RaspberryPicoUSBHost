# One-shot PowerShell: read GCM token into a variable and open a PR
$proto = "protocol=https`nhost=github.com`n`n"
$out = $proto | git credential fill 2>$null | Out-String
if ($out -notmatch "password=(\S+)") { throw "No stored GitHub credential (check cmdkey /list)" }
$tok = $Matches[1]
$headers = @{
    Accept        = "application/vnd.github+json"
    Authorization = "Bearer $tok"
}
$body = @{
    title = "<PR title>"
    head  = "<source-branch>"
    base  = "<target-branch>"
    body  = "<markdown body>"
} | ConvertTo-Json
$resp = Invoke-RestMethod -Uri "https://api.github.com/repos/<owner>/<repo>/pulls" `
    -Method Post -Headers $headers -ContentType "application/json" -Body $body
"PR #$($resp.number): $($resp.html_url)"
