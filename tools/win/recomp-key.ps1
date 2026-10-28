# The recompiler patches (patches\ps2recomp\*.patch touching ps2xRecomp/) and function maps the
# translated game in generated\ was made with: build.ps1 translates again when
# they change. Prints a key; with -Save, writes it to generated\.recompiler-patches.
param([switch]$Save)
$root = Split-Path $PSScriptRoot -Parent | Split-Path -Parent
$patches = Get-ChildItem (Join-Path $root 'patches\ps2recomp') -Filter *.patch | Sort-Object Name |
    Where-Object { Select-String -Path $_.FullName -Pattern '^diff --git a/ps2xRecomp/' -Quiet }
$key = ($patches | ForEach-Object { (Get-FileHash $_.FullName -Algorithm SHA256).Hash }) -join ','
# ... and the function maps it translated from (config\functions*.csv).
$maps = Get-ChildItem (Join-Path $root 'config') -Filter 'functions*.csv' | Sort-Object Name
if ($maps) { $key += ';' + (($maps | ForEach-Object { (Get-FileHash $_.FullName -Algorithm SHA256).Hash }) -join ',') }
if ($Save) { Set-Content -Path (Join-Path $root 'generated\.recompiler-patches') -Value $key -Encoding ascii }
else { $key }
