# winui_gen.ps1: Prepare building the WinUI 3 popup menu (gui_winui.cpp).
#
# Used by Make_mvc.mak when WINUI=yes.  Finds the Windows App SDK NuGet
# package installed with "nuget install Microsoft.WindowsAppSDK", generates
# the C++/WinRT headers in "$OutDir\include" and copies the bootstrap DLL to
# "$DllDir".
#
# The package is searched for in $SdkDir when given, otherwise in the current
# directory, its parents, the user profile and the global NuGet cache.

param(
  [Parameter(Mandatory = $true)] [string] $OutDir,
  [Parameter(Mandatory = $true)] [string] $DllDir,
  [string] $Cpu = "AMD64",
  [string] $SdkDir = ""
)

$ErrorActionPreference = "Stop"

# Return the version of a package directory, for sorting.
function Get-PkgVersion([string] $s) {
  $v = $null
  if ([version]::TryParse(($s -replace '-.*$', ''), [ref] $v)) { return $v }
  return [version] "0.0"
}

# Return the directories of the newest version of each package whose name
# matches $pattern under $root.  Handles both the "nuget install" layout
# (Name.Version) and the global cache layout (name\version).
function Find-Packages([string] $root, [string] $pattern) {
  $result = @{}
  foreach ($d in Get-ChildItem $root -Directory -ErrorAction SilentlyContinue) {
    if ($d.Name -notmatch "^(?<name>$pattern)(\.(?<ver>\d+\.\d+.*))?$") {
      continue
    }
    $name = $Matches['name'].ToLower()
    if ($Matches['ver']) {
      $cands = @(@{ Dir = $d.FullName; Ver = Get-PkgVersion $Matches['ver'] })
    } else {
      $cands = Get-ChildItem $d.FullName -Directory |
        ForEach-Object { @{ Dir = $_.FullName; Ver = Get-PkgVersion $_.Name } }
    }
    foreach ($c in $cands) {
      if (-not $result[$name] -or $c.Ver -gt $result[$name].Ver) {
        $result[$name] = $c
      }
    }
  }
  return $result.Values
}

$sdkPattern = 'Microsoft\.WindowsAppSDK(\.[A-Za-z]+)*'

# Find the root directory holding the Windows App SDK package.
$roots = @()
if ($SdkDir) {
  $roots += $SdkDir
} else {
  $d = (Get-Location).Path
  while ($d) {
    $roots += $d
    $roots += Join-Path $d "packages"
    $d = Split-Path $d -Parent
  }
  $roots += $env:USERPROFILE
  $roots += Join-Path $env:USERPROFILE "packages"
  if ($env:NUGET_PACKAGES) { $roots += $env:NUGET_PACKAGES }
  $roots += Join-Path $env:USERPROFILE ".nuget\packages"
}

$root = $null
foreach ($r in $roots) {
  if (Find-Packages $r 'Microsoft\.WindowsAppSDK' |
        Where-Object { Test-Path $_.Dir }) {
    $root = $r
    break
  }
}
if (-not $root) {
  throw "Windows App SDK not found, run 'nuget install Microsoft.WindowsAppSDK' or set WINUI_SDK. Searched: $($roots -join ', ')"
}
Write-Host "Windows App SDK found in $root"

$sdkPkgs = Find-Packages $root $sdkPattern
# WinUI refers to WebView2 types, its metadata is needed as well.
$allPkgs = @($sdkPkgs) + @(Find-Packages $root 'Microsoft\.Web\.WebView2')
$allPkgs | ForEach-Object { Write-Host "  using $($_.Dir)" }

# Use cppwinrt from a NuGet package when present, otherwise from the Windows
# SDK.
$cppwinrt = Find-Packages $root 'Microsoft\.Windows\.CppWinRT' |
  ForEach-Object { Get-ChildItem $_.Dir -Recurse -Filter cppwinrt.exe } |
  Select-Object -First 1 -ExpandProperty FullName
if (-not $cppwinrt -and $env:WindowsSdkVerBinPath) {
  $p = Join-Path $env:WindowsSdkVerBinPath "x64\cppwinrt.exe"
  if (Test-Path $p) { $cppwinrt = $p }
}
if (-not $cppwinrt) {
  $cppwinrt = Get-ChildItem "${env:ProgramFiles(x86)}\Windows Kits\10\bin\*\x64\cppwinrt.exe" -ErrorAction SilentlyContinue |
    Sort-Object { Get-PkgVersion $_.Directory.Parent.Name } -Descending |
    Select-Object -First 1 -ExpandProperty FullName
}
if (-not $cppwinrt) {
  throw "cppwinrt.exe not found, install the Windows SDK or run 'nuget install Microsoft.Windows.CppWinRT'"
}
Write-Host "Using $cppwinrt"

$include = Join-Path $OutDir "include"
New-Item -ItemType Directory -Force $include | Out-Null

# Generate the projection, using each .winmd file only once.
$winmds = $allPkgs |
  ForEach-Object { Get-ChildItem $_.Dir -Recurse -Filter *.winmd } |
  Group-Object Name | ForEach-Object { $_.Group[0].FullName }
if (-not $winmds) { throw "No .winmd files found in the Windows App SDK" }
$cmd = @("-in", "sdk")
foreach ($w in $winmds) { $cmd += @("-in", $w) }
$cmd += @("-out", $include)
& $cppwinrt @cmd
if ($LASTEXITCODE -ne 0) { throw "cppwinrt failed" }

# Copy the C headers of the Windows App SDK (MddBootstrap.h, ...).
foreach ($p in $sdkPkgs) {
  $inc = Join-Path $p.Dir "include"
  if (Test-Path $inc) { Copy-Item "$inc\*" $include -Recurse -Force }
}
foreach ($f in "MddBootstrap.h", "WindowsAppSDK-VersionInfo.h") {
  if (-not (Test-Path (Join-Path $include $f))) {
    throw "$f not found in the Windows App SDK"
  }
}

# Copy the bootstrap DLL next to jvim.exe.
switch ($Cpu) {
  "i386"  { $arch = "win-x86" }
  "ARM64" { $arch = "win-arm64" }
  default { $arch = "win-x64" }
}
$dll = $sdkPkgs |
  ForEach-Object { Get-ChildItem $_.Dir -Recurse -Filter Microsoft.WindowsAppRuntime.Bootstrap.dll } |
  Where-Object { $_.FullName -match "\\$arch\\" } |
  Select-Object -First 1
if (-not $dll) { throw "Microsoft.WindowsAppRuntime.Bootstrap.dll for $arch not found" }
Copy-Item $dll.FullName $DllDir -Force

Set-Content (Join-Path $OutDir "winui.stamp") $root
