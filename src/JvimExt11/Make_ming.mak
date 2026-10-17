# Project: jvimext11
# Generates jvimext11.dll and jvimext11_cfg.exe with gcc.
# To be used with MinGW-w64 and Cygwin.
#
# The package for the Windows 11 context menu (AppxManifest.xml.in) needs
# makeappx.exe from the Windows SDK, see Make_mvc.mak.

# If cross-compiling set this to yes, else set it to no
CROSS = no
#CROSS = yes

# Link against the shared versions of libgcc/libstdc++ by default.  Set
# STATIC_STDCPLUS to "yes" to link against static versions instead.
STATIC_STDCPLUS=no
#STATIC_STDCPLUS=yes

# If you use TDM-GCC(-64), change HAS_GCC_EH to "no".
# This is used when STATIC_STDCPLUS=yes.
HAS_GCC_EH=yes

STATIC_LIBS=

ifeq (yes, $(STATIC_STDCPLUS))
LDFLAGS += -static-libgcc -static-libstdc++
# Order important: gcc_eh must be placed before winpthread
STATIC_LIBS += -lstdc++ -lgcc
 ifeq (yes, $(HAS_GCC_EH))
STATIC_LIBS += -lgcc_eh
 endif
STATIC_LIBS += -lwinpthread
endif

CXXFLAGS := -O2
ifeq ($(CROSS),yes)
DEL = rm -f
else
ifneq (sh.exe, $(SHELL))
DEL = rm -f
else
DEL = del
endif
endif
# Set the default $(WINVER) to make it work with Windows 7.
ifndef WINVER
WINVER = 0x0601
endif
DEFINES = -DUNICODE -D_UNICODE -DWINVER=$(WINVER) -D_WIN32_WINNT=$(WINVER)
ifdef PATCHLEVEL
DEFINES += -DVIM_VERSION_PATCHLEVEL=$(PATCHLEVEL)
endif
CXX := $(CROSS_COMPILE)g++
WINDRES := $(CROSS_COMPILE)windres
WINDRES_FLAGS =

LANG_FILES = resource.h version.rc ../version.h lang/en.rc lang/fr.rc

DLL := jvimext11.dll
EXE := jvimext11_cfg.exe
DLL_OBJ := jvimext11.o jvimext11.res
EXE_OBJ := jvimext11_cfg.o jvimext11_cfg.res
ifeq ($(findstring clang++,$(CXX)),)
DEFFILE = jvimext11_ming.def
else
DEFFILE = jvimext11.def
endif

.PHONY: all clean

all: $(DLL) $(EXE)

$(DLL): $(DLL_OBJ) $(DEFFILE)
	$(CXX) -shared $(LDFLAGS) $(CXXFLAGS) -s -o $@ $^ \
		-lole32 -lshlwapi -luuid \
		-Wl,-Bstatic $(STATIC_LIBS) -Wl,-Bdynamic

$(EXE): $(EXE_OBJ)
	$(CXX) -mwindows -municode $(LDFLAGS) $(CXXFLAGS) -s -o $@ $^ \
		-lcomctl32 -lcomdlg32 -lshell32 -lgdi32 \
		-Wl,-Bstatic $(STATIC_LIBS) -Wl,-Bdynamic

jvimext11.o: jvimext11.cpp jvimext11.h resource.h
	$(CXX) $(CXXFLAGS) $(DEFINES) -c $< -o $@

jvimext11_cfg.o: jvimext11_cfg.cpp jvimext11.h resource.h ../version.h
	$(CXX) $(CXXFLAGS) $(DEFINES) -c $< -o $@

jvimext11.res: jvimext11.rc $(LANG_FILES)
	$(WINDRES) $(WINDRES_FLAGS) --input-format=rc --output-format=coff $(DEFINES) $< -o $@

jvimext11_cfg.res: jvimext11_cfg.rc jvimext11.ico jvimext11_cfg.manifest \
		$(LANG_FILES)
	$(WINDRES) $(WINDRES_FLAGS) --input-format=rc --output-format=coff $(DEFINES) $< -o $@

clean:
	-$(DEL) jvimext11.o jvimext11.res $(DLL)
	-$(DEL) jvimext11_cfg.o jvimext11_cfg.res $(EXE)
