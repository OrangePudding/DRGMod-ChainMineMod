$ErrorActionPreference = 'Stop'
$modDir = 'E:\SteamLibrary\steamapps\common\Deep Rock Galactic\FSD\Binaries\Win64\ue4ss\Mods\Sakura_CPP_ChainMine'
$new = 'F:\stuff\DRG\ue4ss-research\ChainMineMod\main.dll'
if (-not (Test-Path $new)) { throw 'new main.dll not found - run build.bat first' }
New-Item -ItemType Directory -Path "$modDir\dlls" -Force | Out-Null
Copy-Item -LiteralPath $new -Destination "$modDir\dlls\main.dll" -Force

# ship config.txt only if missing (user may have edited the deployed one)
$srcDir = Split-Path -Parent $PSCommandPath
$dst = Join-Path $modDir 'config.txt'
if (-not (Test-Path -LiteralPath $dst)) {
    Copy-Item -LiteralPath (Join-Path $srcDir 'config.txt') -Destination $dst
    Write-Host 'created config.txt'
}

# ensure mods.txt entry
$modsTxt = 'E:\SteamLibrary\steamapps\common\Deep Rock Galactic\FSD\Binaries\Win64\ue4ss\Mods\mods.txt'
$content = Get-Content -LiteralPath $modsTxt -Raw
if ($content -notmatch 'Sakura_CPP_ChainMine') {
    $content = $content -replace '(?m)^Keybinds : 0', "Sakura_CPP_ChainMine : 1`r`nKeybinds : 0"
    [System.IO.File]::WriteAllText($modsTxt, $content, [System.Text.UTF8Encoding]::new($false))
    Write-Host 'added Sakura_CPP_ChainMine : 1 to mods.txt'
}
Get-FileHash "$modDir\dlls\main.dll" | Select-Object Path, Hash
Write-Host 'deployed (log: <moddir>\chainmine.log, auto-clears on game start)'