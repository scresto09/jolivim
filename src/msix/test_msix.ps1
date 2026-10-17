# test_msix.ps1: Tests for the MSIX package of the whole Vim.
#
# Used by "nmake -f Make_mvc.mak testmsix", run from the src directory.
#
# Without arguments, only checks the sources of the package (no build and no
# Windows SDK needed): make_msix.ps1, msix\AppxManifest.xml.in, msix\Strings
# and msix\Assets.
#
# With -StagingDir (the directory filled by make_msix.ps1) or -Package (a
# .msix file, unpacked with makeappx.exe), also checks the content of the
# built package and runs the packaged vim.exe, when present, to check that it
# finds its runtime files and its translated messages.
#
# Exits with 1 when a check fails.

param(
  [string] $StagingDir = "",
  [string] $Package = ""
)

$ErrorActionPreference = "Stop"

$script:failed = 0
$script:passed = 0

function Check([bool] $ok, [string] $what) {
  if ($ok) {
    $script:passed++
  } else {
    $script:failed++
    Write-Host "FAIL: $what"
  }
}

function Skip([string] $what) {
  Write-Host "SKIP: $what"
}

# Fill the manifest template like make_msix.ps1 does, with dummy values.
function Fill-Manifest([string] $tmpl, [bool] $withDeps, [bool] $withVim) {
  $m = $tmpl.Replace("@NAME@", "JoliVIM")
  $m = $m.Replace("@PUBLISHER@", "CN=Test &amp; Co")
  $m = $m.Replace("@PUBLISHERNAME@", "Test &lt;Co&gt;")
  $m = $m.Replace("@VERSION@", "9.2.1.0")
  $m = $m.Replace("@ARCH@", "x64")
  $deps = ""
  if ($withDeps) {
    $deps = "`r`n    <PackageDependency Name=`"Microsoft.WindowsAppRuntime.2`" MinVersion=`"2.0.0.0`"`r`n" +
            "                       Publisher=`"CN=Microsoft Corporation, O=Microsoft Corporation, L=Redmond, S=Washington, C=US`" />"
  }
  $m = $m.Replace("@DEPENDENCIES@", $deps)
  if (-not $withVim) {
    $m = [regex]::Replace($m, '(?s)\s*<!-- BEGIN vim -->.*?<!-- END vim -->', '')
  }
  return $m
}

function Parse-Xml([string] $text, [string] $what) {
  try {
    return [xml] $text
  } catch {
    Check $false "$what is not valid XML: $($_.Exception.Message)"
    return $null
  }
}

function Get-Ns([xml] $doc) {
  $ns = New-Object Xml.XmlNamespaceManager $doc.NameTable
  $ns.AddNamespace("f", "http://schemas.microsoft.com/appx/manifest/foundation/windows10")
  $ns.AddNamespace("uap", "http://schemas.microsoft.com/appx/manifest/uap/windows10")
  $ns.AddNamespace("com", "http://schemas.microsoft.com/appx/manifest/com/windows10")
  $ns.AddNamespace("desktop5", "http://schemas.microsoft.com/appx/manifest/desktop/windows10/5")
  # The manager is enumerable: keep PowerShell from unrolling it.
  return ,$ns
}

# The files of the package referenced by the manifest: the executables and
# the COM server.
function Get-ManifestFiles([xml] $doc) {
  $ns = Get-Ns $doc
  $files = @()
  foreach ($n in $doc.SelectNodes("//f:Application/@Executable", $ns)) { $files += $n.Value }
  foreach ($n in $doc.SelectNodes("//com:Class/@Path", $ns)) { $files += $n.Value }
  return $files
}

function Get-ManifestLogos([xml] $doc) {
  $ns = Get-Ns $doc
  $logos = @()
  foreach ($n in $doc.SelectNodes("//f:Properties/f:Logo", $ns)) { $logos += $n.InnerText }
  foreach ($n in $doc.SelectNodes("//uap:VisualElements/@*[contains(local-name(), 'Logo')]", $ns)) {
    $logos += $n.Value
  }
  return $logos | Sort-Object -Unique
}

# The machine type of a PE file (exe or dll).
function Get-PeMachine([string] $file) {
  $fs = [IO.File]::OpenRead($file)
  try {
    $br = New-Object IO.BinaryReader $fs
    $fs.Seek(0x3C, "Begin") | Out-Null
    $peOffset = $br.ReadInt32()
    $fs.Seek($peOffset + 4, "Begin") | Out-Null
    return $br.ReadUInt16()
  } finally {
    $fs.Close()
  }
}

# Find a Windows SDK tool, like make_msix.ps1.
function Find-SdkTool([string] $tool) {
  $cmd = Get-Command $tool -ErrorAction SilentlyContinue
  if ($cmd) { return $cmd.Source }
  $hostArch = if ($env:PROCESSOR_ARCHITECTURE -eq "ARM64") { "arm64" } else { "x64" }
  $found = Get-ChildItem "${env:ProgramFiles(x86)}\Windows Kits\10\bin\*\$hostArch\$tool" `
             -ErrorAction SilentlyContinue | Sort-Object FullName | Select-Object -Last 1
  if (-not $found) { throw "$tool not found" }
  return $found.FullName
}

# The languages built by default by make_msix.ps1.
function Get-DefaultLanguages([string] $script) {
  $m = [regex]::Match($script, '\[string\]\s*\$Languages\s*=\s*"([^"]*)"')
  Check $m.Success "default -Languages not found in make_msix.ps1"
  return $m.Groups[1].Value -split '\s+' | Where-Object { $_ }
}

# ---------------------------------------------------------------------------
# Checks of the sources of the package.

$script = Get-Content -Raw msix\make_msix.ps1
$tmpl = Get-Content -Raw -Encoding UTF8 msix\AppxManifest.xml.in

# make_msix.ps1 has no syntax error.
$tokens = $null
$errors = $null
[Management.Automation.Language.Parser]::ParseFile(
    (Resolve-Path msix\make_msix.ps1).Path, [ref] $tokens, [ref] $errors) | Out-Null
Check ($errors.Count -eq 0) "make_msix.ps1 has syntax errors: $($errors -join '; ')"

# Each placeholder of the template is filled by make_msix.ps1, and the other
# way around.
$inTmpl = @([regex]::Matches($tmpl, '@[A-Z]+@') | ForEach-Object { $_.Value } | Sort-Object -Unique)
$inScript = @([regex]::Matches($script, '\.Replace\("(@[A-Z]+@)"') |
                ForEach-Object { $_.Groups[1].Value } | Sort-Object -Unique)
Check (-not (Compare-Object $inTmpl $inScript)) `
      "placeholders differ: template has $($inTmpl -join ' '), make_msix.ps1 fills $($inScript -join ' ')"

# The pattern that removes vim.exe from the manifest is the one used here.
Check ($script.Contains("'(?s)\s*<!-- BEGIN vim -->.*?<!-- END vim -->'")) `
      "make_msix.ps1 does not remove the vim block with the expected pattern"
Check (([regex]::Matches($tmpl, '<!-- BEGIN vim -->')).Count -eq 1 -and
       ([regex]::Matches($tmpl, '<!-- END vim -->')).Count -eq 1) `
      "the template must have exactly one BEGIN vim / END vim block"

# The filled manifest is valid XML, with and without the Windows App Runtime
# and vim.exe, and has the expected applications.
foreach ($withDeps in $true, $false) {
  foreach ($withVim in $true, $false) {
    $what = "manifest (WinUI=$withDeps, vim.exe=$withVim)"
    $doc = Parse-Xml (Fill-Manifest $tmpl $withDeps $withVim) $what
    if (-not $doc) { continue }
    $ns = Get-Ns $doc
    $ids = @($doc.SelectNodes("//f:Application/@Id", $ns) | ForEach-Object { $_.Value })
    $expected = if ($withVim) { @("jvim", "vim", "settings") } else { @("jvim", "settings") }
    Check (($ids -join ' ') -eq ($expected -join ' ')) `
          "$what has applications $($ids -join ' '), expected $($expected -join ' ')"
    $deps = $doc.SelectNodes("//f:Dependencies/f:PackageDependency", $ns).Count
    Check ($deps -eq [int] $withDeps) "$what has $deps package dependencies"
    Check ($doc.Package.Identity.Publisher -eq "CN=Test & Co") "$what does not keep the escaped publisher"
  }
}

$doc = [xml] (Fill-Manifest $tmpl $true $true)
$ns = Get-Ns $doc

# The tested Windows version is not older than the minimal one.
$tdf = $doc.SelectSingleNode("//f:TargetDeviceFamily", $ns)
Check ([version] $tdf.MaxVersionTested -ge [version] $tdf.MinVersion) `
      "MaxVersionTested $($tdf.MaxVersionTested) is older than MinVersion $($tdf.MinVersion)"

# Each "ms-resource:" text is translated in each language, and the languages
# of msix\Strings are the ones declared in the manifest.
$keys = @([regex]::Matches($tmpl, 'ms-resource:(\w+)') | ForEach-Object { $_.Groups[1].Value } |
            Sort-Object -Unique)
Check ($keys.Count -gt 0) "no ms-resource: text in the manifest"
$manifestLangs = @($doc.SelectNodes("//f:Resources/f:Resource/@Language", $ns) |
                     ForEach-Object { $_.Value.ToLower() } | Sort-Object)
$stringLangs = @(Get-ChildItem -Directory msix\Strings | ForEach-Object { $_.Name.ToLower() } |
                   Sort-Object)
Check (($manifestLangs -join ' ') -eq ($stringLangs -join ' ')) `
      "manifest languages ($($manifestLangs -join ' ')) differ from msix\Strings ($($stringLangs -join ' '))"
foreach ($dir in Get-ChildItem -Directory msix\Strings) {
  $resw = Join-Path $dir.FullName "Resources.resw"
  if (-not (Test-Path $resw)) {
    Check $false "$resw is missing"
    continue
  }
  $res = Parse-Xml (Get-Content -Raw -Encoding UTF8 $resw) $resw
  if (-not $res) { continue }
  foreach ($k in $keys) {
    $node = $res.root.data | Where-Object { $_.name -eq $k }
    Check ($node -and $node.value.Trim()) "ms-resource:$k is not translated in $resw"
  }
}

# The manifest languages are also the ones of the JvimExt11 menu entry.
foreach ($lang in $manifestLangs) {
  $primary = ($lang -split '-')[0]
  Check (Test-Path "JvimExt11\lang\$primary.rc") "no JvimExt11\lang\$primary.rc for language $lang"
}

# The messages of each default language can be compiled.
foreach ($lang in Get-DefaultLanguages $script) {
  Check (Test-Path "po\$lang.po") "po\$lang.po not found for the default language $lang"
}

# Each logo has its image in msix\Assets: the plain name, or variants for
# scales or target sizes, indexed by makepri.
foreach ($logo in Get-ManifestLogos $doc) {
  Check ($logo -match '^msix\\[^\\]+\.png$') "logo $logo is not msix\<name>.png"
  $base = [IO.Path]::GetFileNameWithoutExtension($logo)
  $re = "^$([regex]::Escape($base))(\.(scale|targetsize)-[^.]+)?\.png$"
  $variants = @(Get-ChildItem msix\Assets -Filter "$base*.png" | Where-Object { $_.Name -match $re })
  Check ($variants.Count -gt 0) "no image in msix\Assets for logo $logo"
  if ($variants.Count -gt 0 -and -not (Test-Path "msix\Assets\$base.png")) {
    Check (Test-Path "msix\Assets\$base.scale-100.png") `
          "logo $logo has no msix\Assets\$base.png nor $base.scale-100.png"
  }
}

# The context menu entry uses the class of JvimExt11, declared once as a COM
# server.
$src = Get-Content -Raw JvimExt11\jvimext11.cpp
$m = [regex]::Match($src, '//\s*\{([0-9A-Fa-f-]{36})\}')
Check $m.Success "CLSID not found in JvimExt11\jvimext11.cpp"
$clsid = $m.Groups[1].Value.ToUpper()
$verbs = @($doc.SelectNodes("//desktop5:Verb/@Clsid", $ns) | ForEach-Object { $_.Value.ToUpper() })
$classes = @($doc.SelectNodes("//com:Class/@Id", $ns) | ForEach-Object { $_.Value.ToUpper() })
Check ($verbs.Count -gt 0) "no context menu verb in the manifest"
foreach ($v in $verbs) { Check ($v -eq $clsid) "verb CLSID $v differs from JvimExt11 $clsid" }
Check ($classes.Count -eq 1 -and $classes[0] -eq $clsid) `
      "COM server classes ($($classes -join ' ')) differ from JvimExt11 $clsid"

# Each file referenced by the manifest is copied by make_msix.ps1: jvim.exe
# and vim.exe from -Files, the others explicitly.
foreach ($f in Get-ManifestFiles $doc) {
  if ($f -eq "jvim.exe" -or $f -eq "vim.exe") { continue }
  Check ($script.Contains("`"$f`"")) "$f is in the manifest but not copied by make_msix.ps1"
}

# ---------------------------------------------------------------------------
# Checks of a built package.

$unpacked = ""
if ($Package) {
  if (-not (Test-Path $Package)) { throw "$Package not found" }
  $unpacked = Join-Path ([IO.Path]::GetTempPath()) ("test_msix_" + [guid]::NewGuid())
  & (Find-SdkTool makeappx.exe) unpack /o /p $Package /d $unpacked | Out-Null
  if ($LASTEXITCODE -ne 0) { throw "makeappx unpack failed for $Package" }
  $StagingDir = $unpacked
}

if (-not $StagingDir) {
  Skip "no -StagingDir or -Package, the built package is not checked"
} elseif (-not (Test-Path "$StagingDir\AppxManifest.xml")) {
  throw "$StagingDir\AppxManifest.xml not found, build the package first"
} else {
  try {
    $text = Get-Content -Raw -Encoding UTF8 "$StagingDir\AppxManifest.xml"
    Check (-not ($text -match '@[A-Z]+@')) "placeholder $($Matches[0]) left in the manifest"
    $built = Parse-Xml $text "built manifest"
    if ($built) {
      $bns = Get-Ns $built
      $hasVim = Test-Path "$StagingDir\vim.exe"

      # The files referenced by the manifest are in the package.
      foreach ($f in Get-ManifestFiles $built) {
        Check (Test-Path (Join-Path $StagingDir $f)) "$f is in the manifest but not in the package"
      }
      $ids = @($built.SelectNodes("//f:Application/@Id", $bns) | ForEach-Object { $_.Value })
      Check (($ids -contains "vim") -eq $hasVim) `
            "application ""vim"" in the manifest: $($ids -contains 'vim'), vim.exe in the package: $hasVim"

      # The runtime files, the translations and the licenses.
      foreach ($f in "LICENSE", "resources.pri", "defaults.vim", "vimrc_example.vim",
                     "doc\help.txt", "doc\tags", "syntax\syntax.vim", "filetype.vim",
                     "pack\dist\opt\netrw\autoload\netrw.vim", "libintl-8.dll", "libiconv-2.dll",
                     "licenses\gettext-iconv\gettext.txt", "licenses\gettext-iconv\iconv.txt") {
        Check (Test-Path (Join-Path $StagingDir $f)) "$f is not in the package"
      }
      foreach ($lang in Get-DefaultLanguages $script) {
        Check (Test-Path "$StagingDir\lang\$lang\LC_MESSAGES\vim.mo") `
              "lang\$lang\LC_MESSAGES\vim.mo is not in the package"
      }

      # All the binaries are built for the architecture of the package.
      $arch = $built.Package.Identity.ProcessorArchitecture
      $machine = @{ "x64" = 0x8664; "arm64" = 0xAA64 }[$arch]
      Check ($machine -ne $null) "unexpected ProcessorArchitecture $arch"
      $bins = @(Get-ChildItem $StagingDir -File | Where-Object { $_.Extension -in ".exe", ".dll" })
      $bins += @(Get-ChildItem "$StagingDir\JvimExt11" -File -ErrorAction SilentlyContinue |
                   Where-Object { $_.Extension -in ".exe", ".dll" })
      foreach ($b in $bins) {
        $pm = Get-PeMachine $b.FullName
        Check ($pm -eq $machine) ("{0} is for machine 0x{1:X4}, the package is {2}" -f $b.Name, $pm, $arch)
      }

      # The version of the package is the one of jvim.exe, not the
      # PATCHLEVEL = 0 fallback of Make_mvc.mak.
      $ver = [version] $built.Package.Identity.Version
      $fv = (Get-Item "$StagingDir\jvim.exe").VersionInfo
      Check ($ver.Major -eq $fv.FileMajorPart -and $ver.Minor -eq $fv.FileMinorPart -and
             $ver.Build -eq $fv.FileBuildPart) `
            "package version $ver differs from jvim.exe $($fv.FileVersion)"
      Check ($ver.Build -gt 0) "package version $ver has no patch level"

      # The packaged vim.exe finds its runtime files at the root of the
      # package, without $VIM and $VIMRUNTIME, and its French messages.
      $hostMachine = if ($env:PROCESSOR_ARCHITECTURE -eq "ARM64") { 0xAA64 } else { 0x8664 }
      if (-not $hasVim) {
        Skip "vim.exe is not in the package (build with VIMDLL=yes), not run"
      } elseif ($machine -ne $hostMachine) {
        Skip "vim.exe is for $arch, not run on this machine"
      } else {
        $out = Join-Path ([IO.Path]::GetTempPath()) ("test_msix_" + [guid]::NewGuid() + ".txt")
        $saved = @{}
        foreach ($v in "VIM", "VIMRUNTIME", "LANG", "LC_ALL", "LC_MESSAGES") {
          $saved[$v] = [Environment]::GetEnvironmentVariable($v)
          [Environment]::SetEnvironmentVariable($v, $null)
        }
        $env:LANG = "fr"
        try {
          $outVim = $out.Replace("\", "/")
          & "$StagingDir\vim.exe" --clean -es -N `
              -c "try | NotACommand | catch | let g:err = v:exception | endtry" `
              -c "call writefile([`$VIMRUNTIME, filereadable(`$VIMRUNTIME . '/defaults.vim'), g:err], '$outVim')" `
              -c "qa!" | Out-Null
          Check ($LASTEXITCODE -eq 0) "packaged vim.exe exited with $LASTEXITCODE"
          if (Test-Path $out) {
            $lines = @(Get-Content -Encoding UTF8 $out)
            $root = (Resolve-Path $StagingDir).Path.TrimEnd('\')
            Check ($lines[0].Replace('/', '\').TrimEnd('\') -eq $root) `
                  "packaged vim.exe uses `$VIMRUNTIME $($lines[0]), expected $root"
            Check ($lines[1] -eq "1") "packaged vim.exe cannot read `$VIMRUNTIME/defaults.vim"
            Check ($lines[2] -match '^Vim:E492:' -and $lines[2] -notmatch 'Not an editor command') `
                  "packaged vim.exe does not show French messages: $($lines[2])"
          } else {
            Check $false "packaged vim.exe did not write $out"
          }
        } finally {
          foreach ($v in $saved.Keys) { [Environment]::SetEnvironmentVariable($v, $saved[$v]) }
          if (Test-Path $out) { Remove-Item $out }
        }
      }
    }
  } finally {
    if ($unpacked -and (Test-Path $unpacked)) { Remove-Item -Recurse -Force $unpacked }
  }
}

Write-Host "$($script:passed) checks passed, $($script:failed) failed"
if ($script:failed -gt 0) { exit 1 }
