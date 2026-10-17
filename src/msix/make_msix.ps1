# make_msix.ps1: Create the MSIX package of the whole Vim.
#
# Used by "nmake -f Make_mvc.mak msix", run from the src directory after the
# build.  Copies the executables, JvimExt11 and the runtime files to
# $StagingDir, adds the translated messages (gettext and iconv are downloaded,
# see below), fills msix\AppxManifest.xml.in, creates resources.pri with
# makepri.exe and packs it with makeappx.exe (both from the Windows SDK, e.g.
# in a Visual Studio Developer Command Prompt).
#
# The package is not signed.  To test it without signing, register the
# staging directory, with the Developer Mode enabled in Settings:
#
#   Add-AppxPackage -Register <StagingDir>\AppxManifest.xml
#
# The defaults of -Name, -Publisher and -PublisherName are the JoliVIM
# identity reserved in Partner Center, so the package is ready to upload
# unsigned to the Microsoft Store (the Store signs it).  To sideload a
# self-signed build instead, override -Publisher to match the test
# certificate's subject (e.g. PUBLISHER="CN=JoliVIM Test").

param(
  # Executables and DLLs built in the src directory, copied to the root of
  # the package, separated by spaces.  Must include jvim.exe.
  [Parameter(Mandatory = $true)] [string] $Files,
  # Vim version: major.minor.patchlevel.0
  [Parameter(Mandatory = $true)] [string] $Version,
  [string] $Cpu = "AMD64",
  [string] $Name = "SylvainCresto.JoliVIM",
  [string] $Publisher = "CN=4D7AE54D-4897-4AAF-AD4A-96A83C99020F",
  [string] $PublisherName = "Sylvain Cresto",
  # Directory of WindowsAppSDK-VersionInfo.h, when built with WINUI=yes:
  # the package then depends on the Windows App Runtime.
  [string] $WinUIInclude = "",
  [string] $StagingDir = "msix\staging",
  [string] $OutFile = "",
  # Languages of the messages, from po\<language>.po.  English is built in.
  [string] $Languages = "fr",
  # Where gettext and iconv are downloaded, see below.
  [string] $CacheDir = "msix\cache"
)

$ErrorActionPreference = "Stop"

switch ($Cpu) {
  "AMD64" { $arch = "x64" }
  "ARM64" { $arch = "arm64" }
  default { throw "Unsupported CPU for the package: $Cpu (Windows 11 is x64 or arm64)" }
}
if (-not $OutFile) { $OutFile = "JoliVIM_${Version}_$arch.msix" }

# Find a Windows SDK tool, from PATH (Developer Command Prompt) or the SDK.
function Find-SdkTool([string] $tool) {
  $cmd = Get-Command $tool -ErrorAction SilentlyContinue
  if ($cmd) { return $cmd.Source }
  $hostArch = if ($env:PROCESSOR_ARCHITECTURE -eq "ARM64") { "arm64" } else { "x64" }
  $found = Get-ChildItem "${env:ProgramFiles(x86)}\Windows Kits\10\bin\*\$hostArch\$tool" `
             -ErrorAction SilentlyContinue | Sort-Object FullName | Select-Object -Last 1
  if (-not $found) { throw "$tool not found" }
  return $found.FullName
}

$fileList = $Files -split '\s+' | Where-Object { $_ }
if ((Split-Path -Leaf $fileList) -notcontains "jvim.exe") {
  throw "jvim.exe is not built, build with GUI=yes"
}

# Stage the package content.
if (Test-Path $StagingDir) { Remove-Item -Recurse -Force $StagingDir }
New-Item -ItemType Directory -Force $StagingDir | Out-Null

# The runtime files at the root, jvim.exe uses them as $VIMRUNTIME.
Copy-Item -Recurse ..\runtime\* $StagingDir
# Drop runtime files that the recursive copy pulls in but that are not needed
# at run time on Windows: the test suites and the syntax generator
# (syntax\testdir alone is over 10 MB), build tooling (Makefiles, the doc awk
# scripts), the C headers of the logos and the Amiga / Linux desktop files.
foreach ($d in "syntax\testdir", "indent\testdir", "syntax\generator") {
  if (Test-Path "$StagingDir\$d") { Remove-Item -Recurse -Force "$StagingDir\$d" }
}
$dropFiles = "Makefile", "makehtml.awk", "maketags.awk",
             "vim16x16_png.h", "vim32x32_png.h", "vim48x48_png.h"
Get-ChildItem -Recurse -File $StagingDir | Where-Object {
  $_.Name -in $dropFiles -or $_.Extension -in ".info", ".desktop"
} | Remove-Item -Force
Copy-Item ..\LICENSE $StagingDir

foreach ($f in $fileList) {
  if (-not (Test-Path $f)) { throw "$f not found" }
  Copy-Item $f $StagingDir
}

# Translated messages: libintl-8.dll and libiconv-2.dll, loaded by Vim when
# present, and lang\<language>\LC_MESSAGES\vim.mo.  gettext 1.0 and iconv
# 1.19, from:
# - x64: https://github.com/mlocati/gettext-iconv-windows (MinGW build, only
#   depends on msvcrt.dll), which also provides the msgfmt.exe used to
#   compile the .mo files for both architectures (x64 is emulated on arm64);
# - arm64: the MSYS2 clangarm64 packages (only depend on the UCRT).
$gettextZip = @{
  Url = "https://github.com/mlocati/gettext-iconv-windows/releases/download/v1.0-v1.19/gettext1.0-iconv1.19-shared-64.zip"
  Sha256 = "C2F195FC4ED3DF4070FB08FF88C2F724AFD1EED4E28F859EFAE1000BB7BA1152"
}
$msysPackages = @(
  @{
    Url = "https://repo.msys2.org/mingw/clangarm64/mingw-w64-clang-aarch64-gettext-runtime-1.0-1-any.pkg.tar.zst"
    Sha256 = "B5364A7C78CC4B73273A4A28E07E3C6D9CC8EC0CE269527409D944AB1C8F5A70"
  },
  @{
    Url = "https://repo.msys2.org/mingw/clangarm64/mingw-w64-clang-aarch64-libiconv-1.19-1-any.pkg.tar.zst"
    Sha256 = "955499BC5CB73D86EA2850ECE9BBDBBFD66B213E272E032F28147ECD42897E21"
  }
)

# Download "$d.Url" to $CacheDir, unless already there, check its SHA-256 and
# return its path.
function Get-Download($d) {
  $file = Join-Path $CacheDir (Split-Path -Leaf $d.Url)
  if (-not (Test-Path $file)) {
    Write-Host "Downloading $($d.Url)"
    Invoke-WebRequest -UseBasicParsing -Uri $d.Url -OutFile "$file.tmp"
    Move-Item -Force "$file.tmp" $file
  }
  $hash = (Get-FileHash -Algorithm SHA256 $file).Hash
  if ($hash -ne $d.Sha256) {
    Remove-Item $file
    throw "Bad SHA-256 for $file`: $hash, expected $($d.Sha256)"
  }
  return $file
}

New-Item -ItemType Directory -Force $CacheDir | Out-Null
$zip = Get-Download $gettextZip
$gettextDir = $zip -replace '\.zip$', ''
if (-not (Test-Path "$gettextDir\bin\msgfmt.exe")) {
  if (Test-Path $gettextDir) { Remove-Item -Recurse -Force $gettextDir }
  Expand-Archive -Path $zip -DestinationPath $gettextDir
}

$licDir = "$StagingDir\licenses\gettext-iconv"
New-Item -ItemType Directory -Force $licDir | Out-Null
if ($arch -eq "x64") {
  Copy-Item "$gettextDir\bin\libintl-8.dll", "$gettextDir\bin\libiconv-2.dll" $StagingDir
  # LGPL for gettext and iconv, GCC runtime exception, MIT for the build.
  Copy-Item "$gettextDir\license.txt", "$gettextDir\licenses\gettext.txt",
            "$gettextDir\licenses\iconv.txt", "$gettextDir\licenses\gcc.txt" $licDir
} else {
  $msysDir = Join-Path $CacheDir "msys2-clangarm64"
  if (-not (Test-Path "$msysDir\clangarm64\bin\libiconv-2.dll")) {
    if (Test-Path $msysDir) { Remove-Item -Recurse -Force $msysDir }
    New-Item -ItemType Directory -Force $msysDir | Out-Null
    foreach ($p in $msysPackages) {
      # The tar.exe of Windows 11 reads zstd archives.
      & "$env:SystemRoot\System32\tar.exe" -x -f (Get-Download $p) -C $msysDir
      if ($LASTEXITCODE -ne 0) { throw "Cannot extract $($p.Url)" }
    }
  }
  Copy-Item "$msysDir\clangarm64\bin\libintl-8.dll", "$msysDir\clangarm64\bin\libiconv-2.dll" $StagingDir
  # LGPL for gettext and iconv.
  Copy-Item "$msysDir\clangarm64\share\licenses\gettext-runtime\intl\COPYING.LIB" "$licDir\gettext.txt"
  Copy-Item "$msysDir\clangarm64\share\licenses\libiconv\COPYING.LIB" "$licDir\iconv.txt"
}

foreach ($lang in $Languages -split '\s+' | Where-Object { $_ }) {
  $moDir = "$StagingDir\lang\$lang\LC_MESSAGES"
  New-Item -ItemType Directory -Force $moDir | Out-Null
  & "$gettextDir\bin\msgfmt.exe" -c -o "$moDir\vim.mo" "po\$lang.po"
  if ($LASTEXITCODE -ne 0) { throw "msgfmt failed for po\$lang.po" }
}

New-Item -ItemType Directory -Force "$StagingDir\JvimExt11" | Out-Null
foreach ($f in "JvimExt11\jvimext11.dll", "JvimExt11\jvimext11_cfg.exe") {
  if (-not (Test-Path $f)) { throw "$f not found" }
  Copy-Item $f "$StagingDir\JvimExt11"
}

New-Item -ItemType Directory -Force "$StagingDir\msix" | Out-Null
Copy-Item msix\Assets\*.png "$StagingDir\msix"

# The manifest.
$manifest = Get-Content -Raw -Encoding UTF8 msix\AppxManifest.xml.in
$manifest = $manifest.Replace("@NAME@", $Name)
$manifest = $manifest.Replace("@PUBLISHER@", [Security.SecurityElement]::Escape($Publisher))
$manifest = $manifest.Replace("@PUBLISHERNAME@", [Security.SecurityElement]::Escape($PublisherName))
$manifest = $manifest.Replace("@VERSION@", $Version)
$manifest = $manifest.Replace("@ARCH@", $arch)

$deps = ""
if ($WinUIInclude) {
  # The framework package of the Windows App Runtime used for building, e.g.
  # Microsoft.WindowsAppRuntime.2 version 2.5.1.0.  The Store installs it
  # with the package; when sideloading it must already be installed.
  $info = Get-Content -Raw (Join-Path $WinUIInclude "WindowsAppSDK-VersionInfo.h")
  $family = [regex]::Match($info, 'PACKAGE_FRAMEWORK_PACKAGEFAMILYNAME\s+"([^"_]+)_[^"]*"').Groups[1].Value
  $minVer = [regex]::Match($info, 'RUNTIME_VERSION_DOTQUADSTRING\s+"([^"]+)"').Groups[1].Value
  $pub = [regex]::Match($info, 'RUNTIME_IDENTITY_PUBLISHER\s+"([^"]+)"').Groups[1].Value
  if (-not $family -or -not $minVer -or -not $pub) {
    throw "Windows App Runtime version not found in $WinUIInclude\WindowsAppSDK-VersionInfo.h"
  }
  $deps = "`r`n    <PackageDependency Name=`"$family`" MinVersion=`"$minVer`"`r`n" +
          "                       Publisher=`"$([Security.SecurityElement]::Escape($pub))`" />"
}
$manifest = $manifest.Replace("@DEPENDENCIES@", $deps)

if ((Split-Path -Leaf $fileList) -notcontains "vim.exe") {
  Write-Warning "vim.exe is not built (use VIMDLL=yes), no ""vim"" command"
  $manifest = [regex]::Replace($manifest, '(?s)\s*<!-- BEGIN vim -->.*?<!-- END vim -->', '')
}
Set-Content -Encoding UTF8 -Path "$StagingDir\AppxManifest.xml" -Value $manifest

# resources.pri: the translated texts of the manifest (msix\Strings) and the
# variants of the logos (msix\Assets).  Only these are indexed, from a
# separate directory: the runtime files are not resources, and folders like
# "lang\fr" would be taken as language qualifiers.
$priRoot = "$StagingDir.pri"
if (Test-Path $priRoot) { Remove-Item -Recurse -Force $priRoot }
New-Item -ItemType Directory -Force "$priRoot\msix" | Out-Null
Copy-Item msix\Assets\*.png "$priRoot\msix"
Copy-Item -Recurse msix\Strings "$priRoot\Strings"
$makepri = Find-SdkTool makepri.exe
& $makepri createconfig /cf "$priRoot.xml" /dq en-US /pv 10.0.0 /o
if ($LASTEXITCODE -ne 0) { throw "makepri createconfig failed" }
& $makepri new /pr $priRoot /cf "$priRoot.xml" `
    /mn "$StagingDir\AppxManifest.xml" /of "$StagingDir\resources.pri" /o
if ($LASTEXITCODE -ne 0) { throw "makepri failed" }

& (Find-SdkTool makeappx.exe) pack /o /d $StagingDir /p $OutFile
if ($LASTEXITCODE -ne 0) { throw "makeappx failed" }
Write-Host "Created $OutFile (not signed)"
