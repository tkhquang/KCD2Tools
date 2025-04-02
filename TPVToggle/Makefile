# Makefile for building KCD2_TPVToggle.asi using g++ (MinGW)

# Compiler
CXX := g++

# Output DLL
TARGET := build/KCD2_TPVToggle.asi

# Source directory
SRC_DIR := src

# Source files (sorted and line-separated, with path)
SRCS := \
    $(SRC_DIR)/aob_scanner.cpp \
    $(SRC_DIR)/config.cpp \
    $(SRC_DIR)/dllmain.cpp \
    $(SRC_DIR)/exception_handler.cpp \
    $(SRC_DIR)/logger.cpp \
    $(SRC_DIR)/overlay_detection.cpp \
    $(SRC_DIR)/toggle_thread.cpp \
    $(SRC_DIR)/version.cpp

# Compiler flags
CXXFLAGS := -std=c++20 -m64 -O2 -Wall -Wextra \
            -static -static-libgcc -static-libstdc++ \
            -DWIN32_LEAN_AND_MEAN -DNOMINMAX \
            -Isrc

# Linker flags
LDFLAGS := -shared -Wl,--add-stdcall-alias

# Windows libraries
LIBS := -ldinput8 -luser32 -lkernel32 -lpsapi

# Build rule
all: $(TARGET)

$(TARGET): $(SRCS)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(LDFLAGS) $^ -o $@ $(LIBS)

# Clean rule
clean:
	@echo Cleaning...
	@rm -f $(TARGET)

.PHONY: all clean
