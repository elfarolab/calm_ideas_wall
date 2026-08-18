CMAKE      ?= cmake
BUILD_DIR  := build
TARGET     := calm_ideas_wall
BUILD_TYPE ?= Release

.PHONY: configure build run clean cleanall distclean

configure:
	$(CMAKE) -S . -B $(BUILD_DIR) -G Ninja -DCMAKE_BUILD_TYPE=$(BUILD_TYPE)

build: configure
	$(CMAKE) --build $(BUILD_DIR) --parallel

run: build
	$(BUILD_DIR)/$(TARGET)

clean:
	$(CMAKE) --build $(BUILD_DIR) --target clean

cleanall:
	rm -rf $(BUILD_DIR)

