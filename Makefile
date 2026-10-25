# ruru — local speech-to-text service
#
#   make            build bin/ruru and bin/ruru-cli
#   make check      build, load the model, print its identity
#   make install    copy into ~/.local/share/ruru and link ~/.local/bin
#   make clean
#
# The engine is transcribe.cpp. Point TRANSCRIBE_DIR at a release tree that
# contains libtranscribe.so and its backend modules, and TRANSCRIBE_INC at the
# matching transcribe.h.

PREFIX      ?= $(HOME)/.local/share/ruru
BINDIR      ?= $(HOME)/.local/bin
TRANSCRIBE_DIR ?= $(HOME)/.local/share/ruru/lib
TRANSCRIBE_INC ?= include

CC      ?= cc
CFLAGS  ?= -O2 -Wall -Wextra -std=c11
LDFLAGS ?= -Wl,-rpath,$(TRANSCRIBE_DIR)
LDLIBS  ?= -L$(TRANSCRIBE_DIR) -ltranscribe -lpthread -lm

BIN := bin/ruru bin/ruru-cli

all: $(BIN)

bin:
	mkdir -p bin

bin/ruru: src/ruru.c | bin
	$(CC) $(CFLAGS) -I$(TRANSCRIBE_INC) -o $@ $< $(LDFLAGS) $(LDLIBS)

bin/ruru-cli: src/ruru-cli.c | bin
	$(CC) $(CFLAGS) -o $@ $< -lm

check: bin/ruru
	RURU_ARTIFACT_DIR=$(TRANSCRIBE_DIR) ./bin/ruru --check

install: all
	mkdir -p $(PREFIX) $(PREFIX)/models $(BINDIR)
	install -m 755 bin/ruru $(PREFIX)/rurud
	install -m 755 bin/ruru-cli $(PREFIX)/ruru-cli
	install -m 755 src/ruru-ctl.sh $(PREFIX)/ruru-ctl
	install -m 755 src/ruru-open.sh $(PREFIX)/ruru-open
	ln -sf $(PREFIX)/ruru-ctl $(BINDIR)/ruru
	@echo "installed rurud to $(PREFIX)"
	@echo "model goes in $(PREFIX)/models/"

install-unit:
	install -Dm644 packaging/ruru.service $(HOME)/.config/systemd/user/ruru.service
	systemctl --user daemon-reload
	@echo "installed systemd user unit ruru.service (not yet enabled)"

install-desktop:
	install -Dm644 assets/ruru.svg $(HOME)/.local/share/icons/hicolor/scalable/apps/ruru.svg
	install -Dm644 packaging/ruru.desktop $(HOME)/.local/share/applications/ruru.desktop
	gtk-update-icon-cache -f -t $(HOME)/.local/share/icons/hicolor 2>/dev/null || true
	@echo "installed Super-key launcher Ruru"

clean:
	rm -rf bin

.PHONY: all check install install-unit install-desktop clean
