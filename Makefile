### Generated by Winemaker ... and improved by Xj ;-)
SRCDIR                = .
SUBDIRS               =
EXES                  = fsthost32

LBITS = $(shell getconf LONG_BIT)
LASH_EXISTS := $(shell if pkg-config --exists lash-1.0; then echo yes; else echo no; fi)
#LAST_EXISTS := 'no'

### Common settings
PKG_CONFIG_MODULES := glib-2.0
PKG_CONFIG_MODULES += gtk+-2.0
PKG_CONFIG_MODULES += jack
PKG_CONFIG_MODULES += libxml-2.0
ifeq ($(LASH_EXISTS),yes)
PKG_CONFIG_MODULES += lash-1.0
endif

# Shared GCC flags
CEXTRA             := $(shell pkg-config --cflags $(PKG_CONFIG_MODULES))
CEXTRA             += -g -O2 -Wall -Wno-multichar -frounding-math -fsignaling-nans -mfpmath=sse -msse2
ifeq ($(LASH_EXISTS),yes)
CEXTRA             += -DHAVE_LASH
endif

# Shared LDFlags
LDFLAGS            := -mwindows
LIBRARIES          := -lpthread -lX11 $(shell pkg-config --libs $(PKG_CONFIG_MODULES))

# Shared include / install paths
INCLUDE_PATH       = -I. -I/usr/include -I/usr/include -I/usr/include/wine -I/usr/include/wine/windows
DESTDIR            =
PREFIX             = /usr
LIB32_INST_PATH    = $(PREFIX)/lib/i386-linux-gnu/wine
LIB64_INST_PATH    = $(PREFIX)/lib/x86_64-linux-gnu/wine
BIN_INST_PATH      = $(PREFIX)/bin

# Platform specific GCC flags
CEXTRA32           := -m32 $(CEXTRA) -fno-pic -fno-PIC
CEXTRA64           := -m64 $(CEXTRA) -fPIC

# Platform specific LDFLAGS
LDFLAGS32          := -m32 $(LDFLAGS) -L/usr/lib/i386-linux-gnu/wine
LDFLAGS64          := -m64 $(LDFLAGS)

### Global source lists
C_SRCS             = audiomaster.c fst.c gtk.c jackvst.c jfst.c fxb.c fps.c vstwin.c cpuusage.c info.c midifilter.c
ifeq ($(LASH_EXISTS),yes)
C_SRCS             += lash.c
endif

### fsthost.exe sources and settings
fsthost32_OBJS     = $(C_SRCS:.c=_32.o)
fsthost64_OBJS     = $(C_SRCS:.c=_64.o)
ALL_OBJS           := $(fsthost32_OBJS) $(fsthost64_OBJS)
BUILD_OBJS         := $(fsthost32_OBJS)

# On 64 bit platform build also fsthost64
ifeq ($(LBITS), 64)
EXES               += fsthost64
BUILD_OBJS         += $(fsthost64_OBJS)
endif

### Tools
CC = gcc
LINK = winegcc

### Generic targets
all: $(SUBDIRS) $(DLLS:%=%.so) $(EXES:%=%)

### Build rules
.PHONY: all clean dummy install
$(SUBDIRS): dummy
	@cd $@ && $(MAKE)

# Implicit rules
.SUFFIXES: _64.o _32.o
DEFINCL = $(INCLUDE_PATH) $(DEFINES) $(OPTIONS)

.c_32.o:
	$(CC) -c $(CFLAGS) $(CEXTRA32) $(DEFINCL) -o $@ $<

.c_64.o:
	$(CC) -c $(CFLAGS) $(CEXTRA64) $(DEFINCL) -o $@ $<

# Rules for cleaning
CLEAN_FILES = *.dbg.c y.tab.c y.tab.h lex.yy.c core *.orig *.rej fsthost.exe* \\\#*\\\# *~ *% .\\\#*
clean:: $(SUBDIRS:%=%/__clean__) $(EXTRASUBDIRS:%=%/__clean__)
	$(RM) $(CLEAN_FILES) $(ALL_OBJS) $(EXES:%=%.dbg.o) $(EXES:%=%.so) $(EXES:%.exe=%)

# Rules for install
install: $(EXES)
	install -Dm 0644 fsthost32.so $(DESTDIR)$(LIB32_INST_PATH)/fsthost32.so
	install -Dm 0755 fsthost32 $(DESTDIR)$(BIN_INST_PATH)/fsthost32
ifeq ($(LBITS), 64)
	install -Dm 0644 fsthost64.so $(DESTDIR)$(LIB64_INST_PATH)/fsthost64.so
	install -Dm 0755 fsthost64 $(DESTDIR)$(BIN_INST_PATH)/fsthost64
endif
	install -Dm 0755 fsthost_menu $(DESTDIR)$(BIN_INST_PATH)/fsthost_menu
	ln -fs fsthost32 $(DESTDIR)$(BIN_INST_PATH)/fsthost

$(SUBDIRS:%=%/__clean__): dummy
	cd `dirname $@` && $(MAKE) clean

$(EXTRASUBDIRS:%=%/__clean__): dummy
	cd `dirname $@` && $(RM) $(CLEAN_FILES)

### Target specific build rules
define compile
	$(LINK) -m$1 -o $@ $($(@)_OBJS) $(LDFLAGS$(1)) $(LIBRARIES)
	mv $@.exe $@		# Fix script name
	mv $@.exe.so $@.so	# Fix library name

	# Script postprocessing
	sed -i -e 's|-n "$$appdir"|-r "$$appdir/$$appname"|' \
		-e 's|.exe.so|.so|' \
		-e '3i export WINEPATH="$(LIB$(1)_INST_PATH)"' \
		-e '3i export WINE_RT=$${WINE_RT:-10}' \
		-e '3i export L_ENABLE_PIPE_SYNC_FOR_APP="$@"' \
		-e '3i export L_RT_POLICY="$${L_RT_POLICY:-FF}"' \
		-e '3i export L_RT_PRIO=$${L_RT_PRIO:-10}' \
		-e '3i export WINE_SRV_RT=$${WINE_SRV_RT:-15}' $@
endef

$(EXES): $(BUILD_OBJS)
	$(call compile,$(@:fsthost%=%))
