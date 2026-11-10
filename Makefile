# Simple front end for the CMake project.  `make` does the obvious thing.
BUILD_DIR ?= build
PREFIX    ?= $(HOME)/.local
JOBS      ?= $(shell nproc)

.PHONY: all configure build test install uninstall udev clean distclean help

all: build

help:
	@echo "make            configure and build"
	@echo "make test       build and run the unit tests"
	@echo "make install    install into $(PREFIX) and enable the user service"
	@echo "make udev       install udev rules (sudo, once)"
	@echo "make uninstall  remove the installed files"
	@echo "make clean      remove build artefacts"

configure:
	cmake -S . -B $(BUILD_DIR) -DCMAKE_BUILD_TYPE=Release

build: configure
	cmake --build $(BUILD_DIR) -j$(JOBS)

test: build
	ctest --test-dir $(BUILD_DIR) --output-on-failure

install: build
	./scripts/install.sh

udev:
	sudo -S -p '' ./scripts/install-udev.sh

uninstall:
	./scripts/uninstall.sh

clean:
	rm -rf $(BUILD_DIR)

distclean: clean
	rm -rf .cache
