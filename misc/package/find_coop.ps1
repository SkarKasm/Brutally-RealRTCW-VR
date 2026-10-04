# Prints the RealRTCW co-op folder (the one containing "coopmain") from the Steam libraries.
$steam = (Get-ItemProperty 'HKCU:\Software\Valve\Steam' -ErrorAction SilentlyContinue).SteamPath
if (-not $steam) { exit 1 }
$libraries = @($steam)
$vdf = Join-Path $steam 'steamapps\libraryfolders.vdf'
if (Test-Path $vdf) {
    foreach ($line in Get-Content $vdf) {
        if ($line -match '"path"\s+"(.+)"') { $libraries += ($matches[1] -replace '\\\\', '\') }
    }
}
foreach ($lib in $libraries) {
    $coop = Join-Path $lib 'steamapps\common\RealRTCW\coop'
    if (Test-Path (Join-Path $coop 'coopmain')) { Write-Output $coop; exit 0 }
}
exit 1
