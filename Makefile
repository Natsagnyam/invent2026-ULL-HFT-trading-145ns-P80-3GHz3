# INVENT 2027 — GNU Make fallback
# Preferred: CMake (see CMakeLists.txt)

CXX := g++
AS := as
CXXFLAGS := -std=c++20 -O3 -march=native -mtune=native \
            -fno-exceptions -fno-rtti -fno-stack-protector \
            -fno-asynchronous-unwind-tables -ffunction-sections -fdata-sections \
            -DNDEBUG -DNATSKA_ENGINE_BUILD \
            -Iinclude

LDFLAGS := -Wl,--gc-sections -Wl,-O1 -Wl,--as-needed -pthread -lnuma

# FIX: Assembler does not accept -march=native — remove ASMFLAGS entirely
# as src/natska_asm.S -o build/natska_asm.o

SRC_DIR := src
INC_DIR := include/natska
BUILD_DIR := build

SOURCES := $(SRC_DIR)/main.cpp $(SRC_DIR)/natska_asm.S
OBJECTS := $(BUILD_DIR)/main.o $(BUILD_DIR)/natska_asm.o

TARGET := natska_engine

.PHONY: all clean dirs

all: dirs $(TARGET)

dirs:
	@mkdir -p $(BUILD_DIR)

$(TARGET): $(OBJECTS)
	$(CXX) $(CXXFLAGS) $^ -o $@ $(LDFLAGS)

$(BUILD_DIR)/main.o: $(SRC_DIR)/main.cpp $(INC_DIR)/ring_buffer.hpp $(INC_DIR)/asm.hpp $(INC_DIR)/engine.hpp
	$(CXX) $(CXXFLAGS) -c $< -o $@

# FIX: No -march flag for assembler
$(BUILD_DIR)/natska_asm.o: $(SRC_DIR)/natska_asm.S
	$(AS) $< -o $@

clean:
	rm -rf $(BUILD_DIR) $(TARGET)

# Run with root privileges required
test: all
	@echo "Run: sudo ./$(TARGET)"
