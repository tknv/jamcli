BUILD_DIR ?= build
BUILD_TYPE ?= Release

.PHONY: all configure build clean install uninstall \
        deb pkg arch package version

all: build

# Quick version check that doesn't require a build (CMakeLists.txt reads
# the same VERSION file + `git describe` when actually compiling jamcli).
version:
	@echo "$$(cat VERSION 2>/dev/null || echo unknown) ($$(git describe --tags --always --dirty 2>/dev/null || echo unknown))"

configure:
	cmake -S . -B $(BUILD_DIR) \
		-DCMAKE_BUILD_TYPE=$(BUILD_TYPE) \
		-DCMAKE_INSTALL_PREFIX=/usr

build: configure
	cmake --build $(BUILD_DIR) -j$$(nproc)

clean:
	rm -rf $(BUILD_DIR)

install: build
	sudo cmake --install $(BUILD_DIR)

uninstall:
	sudo xargs rm -vf < $(BUILD_DIR)/install_manifest.txt

deb: build
	cpack --config $(BUILD_DIR)/CPackConfig.cmake -G DEB

pkg arch:
	makepkg -sf

package: deb
	@echo "Packages created."

distclean:
	rm -rf $(BUILD_DIR)
	rm -f *.deb *.pkg.tar.zst
