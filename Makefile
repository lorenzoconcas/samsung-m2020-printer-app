#
# Makefile for samsung-m2020-printer-app
#
# Usage:
#   make                       # uses pkg-config to find PAPPL
#   make install               # installs into $(prefix) (default /usr/local)
#   make PREFIX=/opt/m2k DEPS=/opt/m2k   # use deps built by scripts/build-deps.sh
#

NAME		= samsung-m2020-printer-app
VERSION		= 0.1.0
prefix		?= /usr/local
DEPS		?= $(prefix)
bindir		= $(prefix)/bin
libexecdir	= $(prefix)/libexec/$(NAME)
datadir		= $(prefix)/share/$(NAME)
SPLIX_FILTER	?= $(DEPS)/libexec/splix/rastertoqpdl
SERVERROOT	?= /var/lib/$(NAME)/cups

PKGCONFIG	= PKG_CONFIG_PATH="$(DEPS)/lib/pkgconfig:$$PKG_CONFIG_PATH" pkg-config

CC		?= cc
CFLAGS		?= -O2 -g
CFLAGS		+= -Wall -Wextra -Wno-unused-parameter $(shell $(PKGCONFIG) --cflags pappl)
CPPFLAGS	+= -DVERSION=\"$(VERSION)\" \
		   -DM2K_FILTER=\"$(libexecdir)/rastertoqpdl\" \
		   -DM2K_PPD=\"$(datadir)/m2020.ppd\"
LIBS		= $(shell $(PKGCONFIG) --libs pappl)

UNAME		:= $(shell uname)
ifeq ($(UNAME),Darwin)
LIBS		+= -framework CoreFoundation -framework Security -framework SystemConfiguration -framework IOKit -liconv -lresolv
endif

all: $(NAME)

$(NAME): $(NAME).c
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ $< $(LDFLAGS) $(LIBS)

install: $(NAME)
	install -d $(DESTDIR)$(bindir) $(DESTDIR)$(libexecdir) $(DESTDIR)$(datadir)
	install -m 755 $(NAME) $(DESTDIR)$(bindir)/$(NAME)
	install -m 755 $(SPLIX_FILTER) $(DESTDIR)$(libexecdir)/rastertoqpdl
	install -m 644 ppd/m2020.ppd $(DESTDIR)$(datadir)/m2020.ppd
ifeq ($(UNAME),Linux)
	# TLS certificate store used by libcups when the service runs as root
	install -d -m 0700 $(DESTDIR)$(SERVERROOT) $(DESTDIR)$(SERVERROOT)/ssl
endif

clean:
	rm -f $(NAME)

.PHONY: all install clean
