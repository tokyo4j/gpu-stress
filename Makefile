CC ?= cc
PKG_CONFIG ?= pkg-config
GLSLANG ?= glslangValidator
WAYLAND_SCANNER ?= wayland-scanner
PROTOCOLS_DIR := $(shell $(PKG_CONFIG) --variable=pkgdatadir wayland-protocols)
XDG_SHELL := $(PROTOCOLS_DIR)/stable/xdg-shell/xdg-shell.xml
CPPFLAGS += -Ibuild $(shell $(PKG_CONFIG) --cflags wayland-client vulkan)
CFLAGS ?= -O2 -g
CFLAGS += -std=c11 -Wall -Wextra -Wpedantic
LDLIBS += $(shell $(PKG_CONFIG) --libs wayland-client vulkan) -lm

.PHONY: all clean
all: gpu-stress

gpu-stress: main.c build/xdg-shell-protocol.c build/xdg-shell-client-protocol.h build/stress.vert.h build/stress.frag.h
	$(CC) $(CPPFLAGS) $(CFLAGS) main.c build/xdg-shell-protocol.c $(LDFLAGS) $(LDLIBS) -o $@

build:
	mkdir -p $@

build/xdg-shell-client-protocol.h: $(XDG_SHELL) | build
	$(WAYLAND_SCANNER) client-header $< $@

build/xdg-shell-protocol.c: $(XDG_SHELL) | build
	$(WAYLAND_SCANNER) private-code $< $@

build/stress.vert.h: stress.vert | build
	$(GLSLANG) -V --vn stress_vert -o $@ $<

build/stress.frag.h: stress.frag | build
	$(GLSLANG) -V --vn stress_frag -o $@ $<

clean:
	rm -f gpu-stress build/xdg-shell-client-protocol.h build/xdg-shell-protocol.c build/stress.vert.h build/stress.frag.h
