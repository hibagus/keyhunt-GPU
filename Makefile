# Compatibility entry points. Prefer the documented CMake presets for development.
CMAKE ?= cmake
BUILD_DIR ?= build/cpu-release
CMAKE_ARGS ?=
JOBS ?= 2

.PHONY: all default configure keyhunt legacy bsgsd test clean
all: keyhunt
default: keyhunt
configure:
	$(CMAKE) -S . -B "$(BUILD_DIR)" -DCMAKE_BUILD_TYPE=Release $(CMAKE_ARGS)
keyhunt: configure
	$(CMAKE) --build "$(BUILD_DIR)" --parallel $(JOBS) --target keyhunt
	cp "$(BUILD_DIR)/keyhunt" ./keyhunt
legacy:
	$(CMAKE) -S . -B "$(BUILD_DIR)-legacy" -DCMAKE_BUILD_TYPE=Release -DKEYHUNT_BUILD_LEGACY=ON $(CMAKE_ARGS)
	$(CMAKE) --build "$(BUILD_DIR)-legacy" --parallel $(JOBS) --target keyhunt_legacy
	cp "$(BUILD_DIR)-legacy/keyhunt-legacy" ./keyhunt
bsgsd:
	$(CMAKE) -S . -B "$(BUILD_DIR)-bsgsd" -DCMAKE_BUILD_TYPE=Release -DKEYHUNT_BUILD_BSGSD=ON $(CMAKE_ARGS)
	$(CMAKE) --build "$(BUILD_DIR)-bsgsd" --parallel $(JOBS) --target bsgsd
	cp "$(BUILD_DIR)-bsgsd/bsgsd" ./bsgsd
test: keyhunt
	ctest --test-dir "$(BUILD_DIR)" --output-on-failure
clean:
	@if [ -f "$(BUILD_DIR)/CMakeCache.txt" ]; then $(CMAKE) --build "$(BUILD_DIR)" --target clean; fi
	@if [ -f "$(BUILD_DIR)-legacy/CMakeCache.txt" ]; then $(CMAKE) --build "$(BUILD_DIR)-legacy" --target clean; fi
	@if [ -f "$(BUILD_DIR)-bsgsd/CMakeCache.txt" ]; then $(CMAKE) --build "$(BUILD_DIR)-bsgsd" --target clean; fi
	rm -f ./keyhunt ./bsgsd
