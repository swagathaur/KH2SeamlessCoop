# Build the Android APK (arm64): the game as libkh2recomp.so (NDK, CMake,
# Ninja), packaged with android\AndroidManifest.xml, signed with a local key.
# Output: build\android\kh2coop.apk. Needs the Android SDK/NDK under C:\Android
# (or ANDROID_SDK_ROOT) and a JDK (keytool, apksigner).
# The APK holds the translated game: it is for the person who built it, not for sharing.
param([switch]$NoNative, [string]$Sdk = $(if ($env:ANDROID_SDK_ROOT) { $env:ANDROID_SDK_ROOT } else { 'C:\Android\sdk' }))
$ErrorActionPreference = 'Continue'
$root = Split-Path $PSScriptRoot -Parent | Split-Path -Parent
Set-Location $root
New-Item -ItemType Directory -Force logs, build\android | Out-Null
. "$root\tools\win\vsenv.ps1" # CMake and Ninja
$ndk = Get-ChildItem "$Sdk\ndk" -Directory -ErrorAction SilentlyContinue | Sort-Object Name | Select-Object -Last 1
if (-not $ndk) { Write-Host "No NDK under $Sdk\ndk" -ForegroundColor Red; exit 1 }
$jdk = Get-ChildItem 'C:\Android' -Directory -Filter 'jdk-17*' -ErrorAction SilentlyContinue | Select-Object -First 1
if ($jdk) { $env:JAVA_HOME = $jdk.FullName; $env:PATH = "$($jdk.FullName)\bin;$env:PATH" }
$bt = Get-ChildItem "$Sdk\build-tools" -Directory | Sort-Object Name | Select-Object -Last 1
$platform = Get-ChildItem "$Sdk\platforms" -Directory | Sort-Object Name | Select-Object -Last 1
"NDK $($ndk.Name), build-tools $($bt.Name), $($platform.Name)"

$out = 'build\android'
if (-not $NoNative) {
    cmake -S . -B $out -G Ninja `
        "-DCMAKE_TOOLCHAIN_FILE=$($ndk.FullName)\build\cmake\android.toolchain.cmake" `
        -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-30 -DANDROID_STL=c++_static `
        -DCMAKE_BUILD_TYPE=Release -DKH2_SDL3=OFF -DKH2_BUILD_TESTS=OFF `
        "-Dgame-activity_DIR=$root\tools\android\cmake-stubs" "-Dgames-controller_DIR=$root\tools\android\cmake-stubs" `
        "-Dgames-frame-pacing_DIR=$root\tools\android\cmake-stubs" 2>&1 |
        Tee-Object -FilePath logs\android-configure.log | Select-String -Pattern 'kh2:|Error|error|Generating done' | Out-Host
    if ($LASTEXITCODE) { Write-Host 'Configure failed (logs\android-configure.log).' -ForegroundColor Red; exit 1 }
    Write-Host 'Compiling (full output: logs\android-build.log) ...'
    $writer = New-Object System.IO.StreamWriter((Join-Path $root 'logs\android-build.log'), $false, [Text.UTF8Encoding]::new($false))
    $lastShown = -5; $started = Get-Date
    cmake --build $out --target kh2recomp --parallel -- -k 0 2>&1 | ForEach-Object {
        $line = "$_"; $writer.WriteLine($line)
        if ($line -match '^\[(\d+)/(\d+)\]') {
            $pct = [int](100 * [int]$Matches[1] / [math]::Max(1, [int]$Matches[2]))
            if ($pct -ge $lastShown + 5 -and [int]$Matches[2] -gt 50) {
                $lastShown = $pct - ($pct % 5)
                Write-Host ("  {0,3}%  ({1} of {2} steps, {3:N0} min so far)" -f $pct, $Matches[1], $Matches[2], ((Get-Date) - $started).TotalMinutes)
            }
        }
    }
    $code = $LASTEXITCODE
    $writer.Close()
    if ($code) { Get-Content logs\android-build.log | Select-String -Pattern 'error' | Select-Object -First 40 | ForEach-Object { $_.Line }; exit 1 }
}
$so = Join-Path $out 'libkh2recomp.so'
if (-not (Test-Path $so)) { Write-Host "No $so" -ForegroundColor Red; exit 1 }

# ---- package ------------------------------------------------------------------
$stage = Join-Path $out 'apk'
Remove-Item -Recurse -Force $stage -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force "$stage\lib\arm64-v8a", "$stage\compiled" | Out-Null
$strip = Join-Path $ndk.FullName 'toolchains\llvm\prebuilt\windows-x86_64\bin\llvm-strip.exe'
# --strip-debug keeps function names (for simpleperf and crash backtraces).
& $strip --strip-debug -o "$stage\lib\arm64-v8a\libkh2recomp.so" $so
# GPU driver loader hooks (libadrenotools)
$hooks = Get-ChildItem (Join-Path $out 'android-hooks') -Filter '*.so'
foreach ($h in $hooks) { & $strip --strip-unneeded -o "$stage\lib\arm64-v8a\$($h.Name)" $h.FullName }
$aapt2 = Join-Path $bt.FullName 'aapt2.exe'
& $aapt2 compile --dir android\res -o "$stage\compiled\res.zip"
if ($LASTEXITCODE) { exit 1 }
& $aapt2 link -o "$stage\base.apk" --manifest android\AndroidManifest.xml -I "$($platform.FullName)\android.jar" `
    --min-sdk-version 30 --target-sdk-version 34 "$stage\compiled\res.zip"
if ($LASTEXITCODE) { exit 1 }
Add-Type -AssemblyName System.IO.Compression, System.IO.Compression.FileSystem
$zip = [System.IO.Compression.ZipFile]::Open((Resolve-Path "$stage\base.apk"), 'Update')
foreach ($lib in Get-ChildItem "$stage\lib\arm64-v8a" -Filter '*.so') {
    [System.IO.Compression.ZipFileExtensions]::CreateEntryFromFile($zip, $lib.FullName, "lib/arm64-v8a/$($lib.Name)", 'Optimal') | Out-Null
}
$zip.Dispose()
& (Join-Path $bt.FullName 'zipalign.exe') -p -f 4 "$stage\base.apk" "$stage\aligned.apk"
if ($LASTEXITCODE) { exit 1 }
$ks = Join-Path $out 'kh2coop.keystore'
if (-not (Test-Path $ks)) {
    & keytool -genkeypair -keystore $ks -storepass kh2coop -keypass kh2coop -alias kh2coop -keyalg RSA -keysize 2048 `
        -validity 10000 -dname 'CN=KH2 Co-op' 2>&1 | Out-Null
}
$apk = Join-Path $out 'kh2coop.apk'
& (Join-Path $bt.FullName 'apksigner.bat') sign --ks $ks --ks-pass pass:kh2coop --key-pass pass:kh2coop --out $apk "$stage\aligned.apk"
if ($LASTEXITCODE) { exit 1 }
& (Join-Path $bt.FullName 'apksigner.bat') verify $apk
"APK: $apk ({0:N0} MB)" -f ((Get-Item $apk).Length / 1MB)
