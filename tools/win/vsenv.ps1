# Dot-source this to get an MSVC x64 developer environment plus VS-bundled CMake and Ninja.
$vsRoot = & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * `
    -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vsRoot) { throw "Visual Studio with C++ tools not found" }
Import-Module (Join-Path $vsRoot 'Common7\Tools\Microsoft.VisualStudio.DevShell.dll')
Enter-VsDevShell -VsInstallPath $vsRoot -SkipAutomaticLocation -DevCmdArguments '-arch=x64 -host_arch=x64' | Out-Null
$cm = Join-Path $vsRoot 'Common7\IDE\CommonExtensions\Microsoft\CMake'
$env:PATH = "$cm\CMake\bin;$cm\Ninja;$env:PATH"
