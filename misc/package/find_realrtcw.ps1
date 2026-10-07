# Prints the RealRTCW folder (the one containing "Main") from the Steam libraries.
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
    $dir = Join-Path $lib 'steamapps\common\RealRTCW'
    if (Test-Path (Join-Path $dir 'Main')) { Write-Output $dir; exit 0 }
}
exit 1
