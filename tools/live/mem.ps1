Get-CimInstance Win32_Process -Filter "Name='kh2recomp.exe'" | ForEach-Object { "{0} {1} MB {2}" -f $_.ProcessId, [int]($_.WorkingSetSize/1MB), ($_.CommandLine -replace ".*--name=","") }
