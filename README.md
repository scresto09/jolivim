# JoliVIM

**JoliVIM** is a playful fork of [Vim](https://www.vim.org), made for fun by
**Sylvain Cresto**. It specializes Vim's Windows GUI (the `jvim` binary) for **Windows 11**,
giving it a modern **WinUI 3** look while staying 100% Vim underneath.

It is **open source and freely distributable**, making Vim look as good as it
edits, just my personal opinion, of course :-)

JoliVIM is an **independent project and is not affiliated with the Vim
project**.

Repository: <https://github.com/scresto09/jolivim>

For issues, please visit <https://github.com/scresto09/jolivim/issues>

---

## What's different from Vim

The original Vim README.md has been renamed to [README.VIM.md](README.VIM.md).

All of this is Windows-only and enabled through the `FEAT_WINUI` build flag:

- **Tabs in the title bar** (like Notepad), with a per-tab right-click menu
  (New tab / Close tab / Close other tabs / Close tabs to the right / Copy file
  name).
- **Modern menu bar, toolbar icons, context menus and dialogs** (WinUI 3).
- **Two toolbar icon sets**, chosen with the `'toolbariconstyle'` (`tbst`)
  option: the modern **Windows 11** glyphs (Segoe Fluent Icons) or the
  **classic** Vim bitmaps. Activate with `:set tbst=win11`, `:set tbst=classic`
  or `:set tbst=auto` (the default: the Windows 11 glyphs when the icon font is
  available, the classic bitmaps otherwise). In a vimrc shared with a regular
  Vim, guard it so Vim doesn't error: `if has('winui') | set tbst=win11 | endif`.
- **Find/replace bar** floating over the text, with search history.
- **Light / dark theme that follows the Windows setting** (title bar, tabs,
  menu, toolbar, scroll bars, dialogs), switching live.
- **"Copy file name"** from the status-line right-click menu.
- **The GUI binary is renamed `jvim.exe`** (`jvimd.exe` for a debug build),
  instead of `gvim.exe`.

Everything gracefully **falls back to the classic Win32 UI** when the WinUI
runtime is not available.

---

## Prerequisites

- **Windows 11** (Windows 10 may work but is not the target).
- **Visual Studio** with the MSVC C++ toolchain (the WinUI code is C++/WinRT and
  only builds with MSVC — MinGW cannot build it).
- The **Windows App SDK** package, installed with:

  ```
  nuget install Microsoft.WindowsAppSDK
  ```

  It is searched for in the current directory, its parents and the NuGet cache;
  otherwise set `WINUI_SDK=<directory where nuget installed it>`.
- At **runtime**, the **Windows App Runtime** must be present on the machine
  (otherwise JoliVIM runs as classic Win32 Vim). These DLLs ship next to the
  executable:
  - `Microsoft.WindowsAppRuntime.Bootstrap.dll`
  - `libintl-8.dll`, `libiconv-2.dll` (for translated dialog text via gettext,
    downloaded and added by the MSIX package build, see below)

See `src/INSTALLpc.txt` for the general Windows build setup, which must be
adapted for this version.

---

## Building

From a **Visual Studio x64 Native Tools** command prompt, in `src/`:

```
nmake -f Make_mvc.mak GUI=yes WINUI=yes
```

For a debug build (`jvimd.exe`):

```
nmake -f Make_mvc.mak GUI=yes WINUI=yes DEBUG=yes
```

`WINUI=yes` requires `GUI=yes`. Omitting `WINUI=yes` builds a plain Win32 Vim GUI.

The runtime files (`joli.png`, the modified
`menu.vim`, …) must be reachable through `'runtimepath'` (e.g. `$VIMRUNTIME`).

### MSIX package

The whole JoliVIM (jvim, vim, the runtime files and the "Edit in JoliVIM" entry
of the Windows 11 context menu) can be packaged as a single MSIX, without
installer, e.g. for the Microsoft Store:

```
nmake -f Make_mvc.mak GUI=yes WINUI=yes VIMDLL=yes msix
```

This needs `makeappx.exe` and `makepri.exe` from the Windows SDK and creates
the unsigned
`JoliVIM_<version>_<arch>.msix`. The Store signs it. The package identity
defaults to the one reserved in Partner Center
(`SylvainCresto.JoliVIM`, publisher `CN=4D7AE54D-4897-4AAF-AD4A-96A83C99020F`),
so the package is ready to upload as is; override it with `MSIX_NAME=`,
`PUBLISHER=` and `MSIX_PUBLISHERNAME=` if needed (e.g. for a self-signed test
build, set `PUBLISHER=` to the test certificate's subject).
To test it without signing, enable the Developer Mode and register the staging
directory (in the object directory, here `ObjDXWAMD64`):

```
Add-AppxPackage -Register ObjDXWAMD64\msix\AppxManifest.xml
```

`jvim` and `vim` are then available in a command prompt.  The package depends
on the Windows App Runtime used for building: the Store installs it, otherwise
it must already be installed.  See `src/msix/make_msix.ps1`.

The package supports localization: the build downloads gettext and iconv
(checked against their SHA-256, cached in `src/msix/cache`), adds
`libintl-8.dll`, `libiconv-2.dll` and their licenses, and compiles the
translations of `src/po` to `lang/<language>/LC_MESSAGES/vim.mo`. The DLLs come from
[gettext-iconv-windows](https://github.com/mlocati/gettext-iconv-windows) for
x64 and from the MSYS2 `clangarm64` packages for arm64. Only x64 and arm64 are
supported, there is no 32-bit package.

---

## Status

This is an **experimental, for-fun fork**. Use at your own risk, and enjoy! :-)
It tracks upstream Vim over time, so no local patch numbers are added to
`version.c`.

---

## Credits & license

JoliVIM is built on the wonderful **Vim** by Bram Moolenaar et al. The Vim license
applies to the original Vim code (`:help license`); the JoliVIM-specific additions
are distributed under the same terms.
Please support the original Vim project and consider helping children in Uganda.
See `:help uganda`.
