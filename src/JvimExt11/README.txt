README.txt for the jvimext11 DLL.

Written by Sylvain Cresto.


jvimext11.dll adds an entry at the root of the Windows 11 File Explorer
context menu: "Edit in JoliVIM" on files and "Open with JoliVIM" on
folders.  The files are opened in the running gVim if there is one, each in
a new tab page by default.  jvimext11_cfg.exe is its settings window: the
jvim.exe to use (found automatically by default) and whether to open files
in new tab pages.

On Windows 11, gvimext.dll is only shown under "Show more options".  The root
of the menu needs a package identity, given by a "sparse package": an MSIX
package with only AppxManifest.xml, pointing to the Vim directory.


BUILDING

jvimext11.dll, jvimext11_cfg.exe and the package are not built by default.
With MSVC, build them with Vim by adding JVIMEXT11=yes, in the src directory:

    nmake -f Make_mvc.mak GUI=yes JVIMEXT11=yes

To build them alone, in this directory:

    nmake -f Make_mvc.mak all msix
    make -f Make_ming.mak

The package needs makeappx.exe from the Windows SDK (e.g. in a Visual Studio
Developer Command Prompt), so MinGW only builds the DLL and the settings
window.

This creates jvimext11.msix, unsigned.  PUBLISHER (default "CN=Vim Test")
must be exactly the subject of the certificate used to sign it, e.g. with
PUBLISHER="CN=..." and:

    signtool sign /fd SHA256 /f certificate.pfx /p password jvimext11.msix

The package doesn't depend on the version of Vim, only on AppxManifest.xml.in
and the architecture: a signed package can be used for several versions of
Vim.  It is also independent of the compiler used for the DLL.


INSTALLATION

The installer has to:

1. Copy jvimext11.dll, jvimext11_cfg.exe and the Assets directory to the
   JvimExt11 directory of the Vim directory, the one with jvim.exe.
   When building with MinGW, use STATIC_STDCPLUS=yes: jvimext11.dll is loaded
   by dllhost.exe, which might not find the libstdc++ and libgcc DLLs in the
   JvimExt11 directory.
   The architecture of jvimext11.dll must match the package and Windows, x64
   or arm64 (there is no 32-bit Windows 11).

2. On Windows 11 (build 22000 or later) only, register the signed package
   with the Vim directory as external location.  For the current user:

    Add-AppxPackage -Path jvimext11.msix -ExternalLocation <Vim directory>

   For all users (as administrator):

    Add-AppxPackage -Stage jvimext11.msix -ExternalLocation <Vim directory>
    Add-AppxProvisionedPackage -Online -SkipLicense -PackagePath jvimext11.msix

3. Optionally create a shortcut to JvimExt11\jvimext11_cfg.exe: the package
   doesn't add it to the Start menu.

jvimext11.dll finds jvim.exe in the parent directory of the JvimExt11
directory, otherwise it is set in the settings window (jvimext11_cfg.exe).
JoliVIM never reads Vim's own Software\Vim registry keys, so it can be
installed next to a regular Vim without interfering with it.


UNINSTALLATION

Before deleting the JvimExt11 directory, unregister the package; Explorer
may keep jvimext11.dll loaded (in dllhost.exe) until then:

    Get-AppxPackage Vim.JvimExt11 | Remove-AppxPackage

For all users (as administrator):

    Get-AppxProvisionedPackage -Online |
        Where-Object DisplayName -eq Vim.JvimExt11 |
        Remove-AppxProvisionedPackage -Online
    Get-AppxPackage -AllUsers Vim.JvimExt11 | Remove-AppxPackage -AllUsers

The settings are in HKEY_CURRENT_USER\Software\JoliVIM\JvimExt11.


TESTING

To test a package signed with a self-signed certificate, its public part must
first be trusted, as administrator:

    Import-Certificate -FilePath test.cer -CertStoreLocation Cert:\LocalMachine\TrustedPeople

A package with the same name and version can't be registered twice: remove
the previous one first.
