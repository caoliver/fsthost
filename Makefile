### Generated by Winemaker ... and improved by Xj ;-)
SRCDIR             := .
SUBDIRS            :=
PLAT               := 32
GTK                := 3
VUMETER            := 0
LBITS              := $(shell getconf LONG_BIT)
LASH               := $(shell pkg-config --exists lash-1.0 && echo 1 || echo 0 )

# Modules
PKG_CONFIG_MODULES := jack
PKG_CONFIG_MODULES += libxml-2.0

ifneq ($(GTK),0)
PKG_CONFIG_MODULES += gtk+-$(GTK).0
endif

ifeq ($(GTK),2)
override VUMETER = 0
endif

ifeq ($(LASH),1)
PKG_CONFIG_MODULES += lash-1.0
endif

# Shared GCC flags
CEXTRA             := $(shell pkg-config --cflags $(PKG_CONFIG_MODULES))
CEXTRA             += -g -O2 -Wall -fPIC -Wno-deprecated-declarations -Wno-multichar -march=native -mfpmath=sse

ifeq ($(VUMETER),1)
CEXTRA             += -DVUMETER
endif

ifeq ($(LASH),1)
CEXTRA             += -DHAVE_LASH
endif

ifeq ($(MWW),1)
CEXTRA             += -DMOVING_WINDOWS_WORKAROUND
endif

ifeq ($(EE),1)
CEXTRA             += -DEMBEDDED_EDITOR
endif

ifeq ($(GTK),0)
CEXTRA             += -DNO_GTK
endif

# Shared LDFlags
LDFLAGS            := -mwindows
LIBRARIES          := -lpthread -lX11 $(shell pkg-config --libs $(PKG_CONFIG_MODULES))

# Shared include / install paths
INCLUDE_PATH        = -I. -I/usr/include -I/usr/include/wine -I/usr/include/wine/windows -I/usr/include/x86_64-linux-gnu
DESTDIR             =
PREFIX              = /usr
MANDIR              = $(PREFIX)/man/man1
ICONDIR             = $(PREFIX)/share/icons/hicolor/32x32/apps
LIB32_INST_PATH     = $(PREFIX)/lib/i386-linux-gnu/wine
LIB64_INST_PATH     = $(PREFIX)/lib/x86_64-linux-gnu/wine
BIN_INST_PATH       = $(PREFIX)/bin

# Platform specific GCC flags
CEXTRA32           := -m32 $(CEXTRA)
CEXTRA64           := -m64 $(CEXTRA)

# Platform specific LDFLAGS
LDFLAGS32          := -m32 $(LDFLAGS) -L/usr/lib/i386-linux-gnu/wine
LDFLAGS64          := -m64 $(LDFLAGS)

### Global source lists
C_SRCS             := fsthost.c cpuusage.c proto.c

# JFST
C_SRCS             += $(wildcard jfst/*.c)

# MIDIFILTER
C_SRCS             += $(wildcard midifilter/*.c)

# FST
C_SRCS             += $(wildcard fst/*.c)

# JFST proto stuff
C_SRCS             += $(wildcard serv/*.c)

# XML DB
C_SRCS             += $(wildcard xmldb/*.c)

# LOG
C_SRCS             += $(wildcard log/*.c)

# LASH
ifeq ($(LASH),1)
C_SRCS             += $(wildcard lash/*.c)
endif

# GTK
ifneq ($(GTK),0)
C_SRCS             += $(wildcard gtk/*.c)
endif

# On 64 bit platform build also fsthost64
ifeq ($(LBITS), 64)
PLAT               += 64
endif

EXES               := $(PLAT:%=fsthost%)

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
ALL_OBJS = $(C_SRCS:.c=_32.o) $(C_SRCS:.c=_64.o) fsthost_list
CLEAN_FILES = fsthost_menu.1 *.dbg.c y.tab.c y.tab.h lex.yy.c core *.orig *.rej fsthost.exe* \\\#*\\\# *~ *% .\\\#*
clean:: $(SUBDIRS:%=%/__clean__) $(EXTRASUBDIRS:%=%/__clean__)
	$(RM) $(CLEAN_FILES) $(ALL_OBJS) $(EXES:%=%.dbg.o) $(EXES:%=%.so) $(EXES:%.exe=%)

# Do not remove intermediate files
.SECONDARY: $(ALL_OBJS)

# Prepare manual
man:
	pod2man perl/fsthost_menu.pl fsthost_menu.1

install-man: man
	install -Dm 0644 fsthost.1 $(DESTDIR)$(MANDIR)/fsthost.1
	install -Dm 0644 fsthost_menu.1 $(DESTDIR)$(MANDIR)/fsthost_menu.1

install-icon:
	install -Dm 0644 gtk/fsthost.xpm $(DESTDIR)$(ICONDIR)/fsthost.xpm

# Rules for install
install: $(EXES) install-man install-icon
	install -Dm 0644 fsthost32.so $(DESTDIR)$(LIB32_INST_PATH)/fsthost32.so
	install -Dm 0755 fsthost32 $(DESTDIR)$(BIN_INST_PATH)/fsthost32
ifeq ($(LBITS), 64)
	install -Dm 0644 fsthost64.so $(DESTDIR)$(LIB64_INST_PATH)/fsthost64.so
	install -Dm 0755 fsthost64 $(DESTDIR)$(BIN_INST_PATH)/fsthost64
endif
	install -Dm 0755 fsthost_list $(DESTDIR)$(BIN_INST_PATH)/fsthost_list
	install -Dm 0755 perl/fsthost_menu.pl $(DESTDIR)$(BIN_INST_PATH)/fsthost_menu
	install -Dm 0755 perl/fsthost_ctrl.pl $(DESTDIR)$(BIN_INST_PATH)/fsthost_ctrl
	ln -fs fsthost32 $(DESTDIR)$(BIN_INST_PATH)/fsthost

$(SUBDIRS:%=%/__clean__): dummy
	cd `dirname $@` && $(MAKE) clean

$(EXTRASUBDIRS:%=%/__clean__): dummy
	cd `dirname $@` && $(RM) $(CLEAN_FILES)

fsthost_list: xmldb/list_$(LBITS).o
	gcc flist.c $< $(shell pkg-config --cflags --libs libxml-2.0) -O3 -g -I. -o $@

### Target specific build rules
define compile
	$(LINK) -o $@ $(C_SRCS:.c=_$(1).o) $(LDFLAGS$(1)) $(LIBRARIES)
	mv $@.exe $@		# Fix script name
	mv $@.exe.so $@.so	# Fix library name

	# Script postprocessing
	sed -i -e 's|-n "$$appdir"|-r "$$appdir/$$appname"|' \
		-e 's|.exe.so|.so|' \
		-e '3i export WINEPATH="$(LIB$(1)_INST_PATH)"' \
		-e '3i export WINE_SRV_RT=$${WINE_SRV_RT:-15}' \
		-e '3i export WINE_RT=$${WINE_RT:-10}' \
		-e '3i export STAGING_RT_PRIORITY_SERVER=$${STAGING_RT_PRIORITY_SERVER:-15}' \
		-e '3i export STAGING_RT_PRIORITY_BASE=$${STAGING_RT_PRIORITY_BASE:-0}' \
		-e '3i export STAGING_SHARED_MEMORY=$${STAGING_SHARED_MEMORY:-1}' \
		-e '3i export L_ENABLE_PIPE_SYNC_FOR_APP="$@"' \
		-e '3i export L_RT_POLICY="$${L_RT_POLICY:-FF}"' \
		-e '3i export L_RT_PRIO=$${L_RT_PRIO:-10}' $@
endef

fsthost%: $(C_SRCS:.c=_%.o) fsthost_list
	$(call compile,$(@:fsthost%=%))
