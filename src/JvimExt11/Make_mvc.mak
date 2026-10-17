# Makefile for JvimExt11, using MSVC
# Options:
#   DEBUG=yes		Build debug version
#   CPUARG=		/arch:IA32/AVX/etc, call from main makefile to set
#   			automatically from CPUNR
#   PATCHLEVEL=		Vim patch level shown in the settings window
#
# "nmake -f Make_mvc.mak msix" creates jvimext11.msix, the unsigned package
# that adds the entry to the Windows 11 context menu, see
# AppxManifest.xml.in.  PUBLISHER is the subject of the certificate used to
# sign it, default "CN=Vim Test".  This needs makeappx.exe from the Windows
# SDK in the PATH.  The main makefile does this with JVIMEXT11=yes.
#

# included common tools
!INCLUDE ..\auto\nmake\tools.mak

TARGETOS = WINNT

!IFNDEF APPVER
APPVER = 6.01
!ENDIF
# Set the default $(WINVER) to make it work with Windows 7.
!IFNDEF WINVER
WINVER = 0x0601
!ENDIF

!IF "$(DEBUG)" != "yes"
NODEBUG = 1
!ENDIF

!IFNDEF CPU
CPU = i386
! IFNDEF PLATFORM
!  IFDEF TARGET_CPU
PLATFORM = $(TARGET_CPU)
!  ELSEIF defined(VSCMD_ARG_TGT_ARCH)
PLATFORM = $(VSCMD_ARG_TGT_ARCH)
!  ENDIF
! ENDIF
! IFDEF PLATFORM
!  IF ("$(PLATFORM)" == "x64") || ("$(PLATFORM)" == "X64")
CPU = AMD64
!  ELSEIF ("$(PLATFORM)" == "arm64") || ("$(PLATFORM)" == "ARM64")
CPU = ARM64
!  ELSEIF ("$(PLATFORM)" != "x86") && ("$(PLATFORM)" != "X86")
!   ERROR *** ERROR Unknown target platform "$(PLATFORM)". Make aborted.
!  ENDIF
! ENDIF
!ENDIF

# Architecture of the package, the same as the DLL.  Windows 11 is only x64
# or arm64, there is no 32-bit package.
!IF "$(CPU)" == "AMD64"
APPX_ARCH = x64
!ELSEIF "$(CPU)" == "ARM64"
APPX_ARCH = arm64
!ENDIF

cc = cl
link = link
rc = rc
cflags = -nologo -c -GF -EHsc -DUNICODE -D_UNICODE
lflags = -incremental:no -nologo
rcflags = /r

!IF "$(DEBUG)" == "yes"
cflags = $(cflags) -Zi -Od
lflags = $(lflags) -debug
!ELSE
cflags = $(cflags) -O2
!ENDIF

# include CPUARG
cflags = $(cflags) $(CPUARG)

# set WINVER and _WIN32_WINNT
cflags = $(cflags) -DWINVER=$(WINVER) -D_WIN32_WINNT=$(WINVER)

!IFDEF PATCHLEVEL
cflags = $(cflags) -DVIM_VERSION_PATCHLEVEL=$(PATCHLEVEL)
rcflags = $(rcflags) -DVIM_VERSION_PATCHLEVEL=$(PATCHLEVEL)
!ENDIF

!IF "$(CL)" == "/D_USING_V110_SDK71_"
rcflags = $(rcflags) /D_USING_V110_SDK71_
!ENDIF

SUBSYSTEM = console
SUBSYSTEM_GUI = windows
!IF "$(SUBSYSTEM_VER)" != ""
SUBSYSTEM = $(SUBSYSTEM),$(SUBSYSTEM_VER)
SUBSYSTEM_GUI = $(SUBSYSTEM_GUI),$(SUBSYSTEM_VER)
!ENDIF

LANG_FILES = resource.h version.rc ..\version.h lang\en.rc lang\fr.rc

all: jvimext11.dll jvimext11_cfg.exe

jvimext11.dll: jvimext11.obj jvimext11.res jvimext11.def
	$(link) $(lflags) -dll -def:jvimext11.def -out:$@ \
		jvimext11.obj jvimext11.res \
		kernel32.lib user32.lib advapi32.lib ole32.lib shlwapi.lib \
		-subsystem:$(SUBSYSTEM)

jvimext11_cfg.exe: jvimext11_cfg.obj jvimext11_cfg.res
	$(link) $(lflags) -out:$@ jvimext11_cfg.obj jvimext11_cfg.res \
		kernel32.lib user32.lib advapi32.lib gdi32.lib comdlg32.lib \
		comctl32.lib shell32.lib -subsystem:$(SUBSYSTEM_GUI)

jvimext11.obj: jvimext11.cpp jvimext11.h resource.h

jvimext11_cfg.obj: jvimext11_cfg.cpp jvimext11.h resource.h ../version.h

.cpp.obj:
	$(cc) $(cflags) $*.cpp

jvimext11.res: jvimext11.rc $(LANG_FILES)
	$(rc) /nologo $(rcflags) jvimext11.rc

jvimext11_cfg.res: jvimext11_cfg.rc jvimext11.ico jvimext11_cfg.manifest \
		$(LANG_FILES)
	$(rc) /nologo $(rcflags) jvimext11_cfg.rc

!IFNDEF PUBLISHER
PUBLISHER = CN=Vim Test
!ENDIF

# Always rebuilt: the result depends on PUBLISHER.
msix:
!IFNDEF APPX_ARCH
	@echo The package is only for x64 and arm64, not for $(CPU).
	@exit 1
!ENDIF
	- if exist appx\nul $(RD) appx
	$(MKD) appx
	$(PS) $(PSFLAGS) "(Get-Content -Raw -Encoding UTF8 AppxManifest.xml.in).Replace('@PUBLISHER@', [Security.SecurityElement]::Escape('$(PUBLISHER)')).Replace('@ARCH@', '$(APPX_ARCH)') | Set-Content -Encoding UTF8 appx\AppxManifest.xml"
	makeappx pack /o /nv /d appx /p jvimext11.msix

clean:
	- if exist jvimext11.dll $(RM) jvimext11.dll
	- if exist jvimext11.lib $(RM) jvimext11.lib
	- if exist jvimext11.exp $(RM) jvimext11.exp
	- if exist jvimext11.obj $(RM) jvimext11.obj
	- if exist jvimext11.res $(RM) jvimext11.res
	- if exist jvimext11.pdb $(RM) jvimext11.pdb
	- if exist jvimext11_cfg.exe $(RM) jvimext11_cfg.exe
	- if exist jvimext11_cfg.obj $(RM) jvimext11_cfg.obj
	- if exist jvimext11_cfg.res $(RM) jvimext11_cfg.res
	- if exist jvimext11_cfg.pdb $(RM) jvimext11_cfg.pdb
	- if exist jvimext11.msix $(RM) jvimext11.msix
	- if exist appx\nul $(RD) appx
	- if exist vc*.pdb $(RM) vc*.pdb

# vim: set noet sw=8 ts=8 sts=0 wm=0 tw=79 ft=make:
