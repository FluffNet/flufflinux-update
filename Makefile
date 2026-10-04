CMAKE ?= cmake
BUILD_DIR ?= build
JOBS ?= 2

.PHONY: all configure build fakeroot
.DEFAULT_GOAL := all

all: build

configure:
	"$(CMAKE)" "-DFLU_SOURCE_DIR=$(CURDIR)" "-DFLU_BUILD_DIR=$(BUILD_DIR)" \
		-DFLU_VALIDATE_ONLY=ON -P cmake/StagePackage.cmake
	"$(CMAKE)" -S . -B "$(BUILD_DIR)" \
		-DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr \
		-DKDE_INSTALL_SYSCONFDIR=/etc -DBUILD_TESTING=OFF

build: configure
	"$(CMAKE)" --build "$(BUILD_DIR)" --parallel "$(JOBS)"

fakeroot: build
	"$(CMAKE)" "-DFLU_SOURCE_DIR=$(CURDIR)" "-DFLU_BUILD_DIR=$(BUILD_DIR)" \
		-P cmake/StagePackage.cmake
