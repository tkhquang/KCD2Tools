# Makefile for building KCD2_TPVToggle.asi using g++ (MinGW)

CXX := g++
WINDRES := windres

# Base Directories
SRC_DIR := src
ASM_DIR := $(SRC_DIR)/asm
EXTERNAL_DIR := external
BUILD_DIR := build

# MinHook and SimpleIni Directories
MINHOOK_DIR := $(EXTERNAL_DIR)/minhook
MINHOOK_INCLUDE_DIR := $(MINHOOK_DIR)/include
MINHOOK_SRC_DIR := $(MINHOOK_DIR)/src
MINHOOK_HDE_SRC_DIR := $(MINHOOK_SRC_DIR)/hde

SIMPLEINI_DIR := $(EXTERNAL_DIR)/simpleini

# Output Directories
OBJ_DIR := $(BUILD_DIR)/obj
MINHOOK_OBJ_OUT_DIR := $(OBJ_DIR)/minhook

# Final Target
TARGET := $(BUILD_DIR)/KCD2_TPVToggle.asi

# --- Sanity Check for Dependencies ---
ifeq ($(wildcard $(MINHOOK_INCLUDE_DIR)/MinHook.h),)
$(error MinHook submodule not initialized. Please run 'git submodule update --init --recursive')
endif
ifeq ($(wildcard $(SIMPLEINI_DIR)/SimpleIni.h),)
$(error SimpleIni submodule not initialized. Please run 'git submodule update --init --recursive')
endif

# --- Compiler and Linker Flags ---
INCLUDE_PATHS := -I$(MINHOOK_INCLUDE_DIR) -I$(SIMPLEINI_DIR) -I$(SRC_DIR)
# Use C++17 for std::filesystem, enable exceptions, common warnings, 64-bit, optimize for size
CXXFLAGS := -std=c++17 -m64 -Os -Wall -Wextra $(INCLUDE_PATHS) -fexceptions \
            -DWINVER=0x0601 -D_WIN32_WINNT=0x0601 -fdata-sections -ffunction-sections \
            -fno-rtti -fno-threadsafe-statics
CFLAGS := $(CXXFLAGS) # Use same flags for MinHook C code
# Link static libraries, create shared library (DLL/ASI), strip symbols, remove unused sections
LDFLAGS := -static -static-libgcc -static-libstdc++ -shared -s -Wl,--gc-sections
# Required Windows system libraries (minimized)
LDLIBS := -lpsapi -luser32 -lkernel32 -lshell32

# --- Source Files ---
# Find C++ source files
CPP_SRCS := $(wildcard $(SRC_DIR)/*.cpp) \
            $(wildcard $(SRC_DIR)/hooks/*.cpp)
# List Assembly source files
ASM_SRCS := $(wildcard $(ASM_DIR)/*.S)

# Find MinHook C source files
MINHOOK_C_SRCS := $(wildcard $(MINHOOK_SRC_DIR)/*.c)
MINHOOK_HDE_SRCS := $(wildcard $(MINHOOK_HDE_SRC_DIR)/*.c)

# --- Object Files ---
# Generate object file paths from source paths
CPP_OBJS := $(patsubst $(SRC_DIR)/%.cpp,$(OBJ_DIR)/%.o,$(CPP_SRCS))
ASM_OBJS := $(patsubst $(ASM_DIR)/%.S,$(OBJ_DIR)/%.o,$(ASM_SRCS))
MINHOOK_C_OBJS := $(patsubst $(MINHOOK_SRC_DIR)/%.c,$(MINHOOK_OBJ_OUT_DIR)/%.o,$(MINHOOK_C_SRCS))
MINHOOK_HDE_OBJS := $(patsubst $(MINHOOK_HDE_SRC_DIR)/%.c,$(MINHOOK_OBJ_OUT_DIR)/%.o,$(MINHOOK_HDE_SRCS))

# Combine all required object files for linking
ALL_OBJS := $(sort $(CPP_OBJS) $(ASM_OBJS) $(MINHOOK_C_OBJS) $(MINHOOK_HDE_OBJS))

# --- Make Rules ---

.PHONY: all clean distclean install help prepare

# Default target: ensure build directories exist, then build the target
all: prepare $(TARGET)

# Rule to create necessary build directories
prepare:
	@echo "Creating build directories..."
	@mkdir -p $(BUILD_DIR) $(OBJ_DIR) $(OBJ_DIR)/hooks $(MINHOOK_OBJ_OUT_DIR)

# Rule to link the final DLL/ASI target
$(TARGET): $(ALL_OBJS)
	@echo "Linking $@..."
	$(CXX) $(LDFLAGS) -o $@ $^ $(LDLIBS)
	@echo "Build complete: $@"
	@du -b $(TARGET) | cut -f1 | xargs -I {} echo "Size: {} bytes"

# Rule to compile C++ source files
$(OBJ_DIR)/%.o: $(SRC_DIR)/%.cpp
	@echo "Compiling C++ $<..."
	$(CXX) $(CXXFLAGS) -c $< -o $@

# Rule to assemble Assembly source files (GAS syntax)
$(OBJ_DIR)/%.o: $(ASM_DIR)/%.S
	@echo "Assembling $<..."
	$(CXX) $(CFLAGS) -c $< -o $@

# Rule to compile MinHook Core C source files
$(MINHOOK_OBJ_OUT_DIR)/%.o: $(MINHOOK_SRC_DIR)/%.c
	@echo "Compiling MinHook $<..."
	$(CXX) $(CFLAGS) -c $< -o $@

# Rule to compile MinHook HDE C source files
$(MINHOOK_OBJ_OUT_DIR)/%.o: $(MINHOOK_HDE_SRC_DIR)/%.c
	@echo "Compiling MinHook HDE $<..."
	$(CXX) $(CFLAGS) -c $< -o $@

# --- Utility Targets ---

# Clean target: remove object files and final target
clean:
	@echo "Cleaning build files..."
	rm -f $(ALL_OBJS) $(TARGET)

# Distclean target: remove the entire build directory
distclean: clean
	@echo "Cleaning entire build directory..."
	rm -rf $(BUILD_DIR)

# Install target: Copy ASI and essential config/docs to build dir
install: all
	@echo "Copying distributable files to $(BUILD_DIR)..."
	@cp -fv KCD2_TPVToggle.ini $(BUILD_DIR)/ || copy KCD2_TPVToggle.ini $(subst /,\,$(BUILD_DIR))\
	@cp -fv README.md $(BUILD_DIR)/README_MOD.md || copy README.md $(subst /,\,$(BUILD_DIR))\README_MOD.md
	@cp -fv KCD2_TPVToggle_Acknowledgements.txt $(BUILD_DIR)/ || copy KCD2_TPVToggle_Acknowledgements.txt $(subst /,\,$(BUILD_DIR))\
	@echo "Install complete: $@"
	@du -b $(TARGET) | cut -f1 | xargs -I {} echo "Size: {} bytes"

# Help target: display available commands
help:
	@echo "Available commands:"
	@echo "  make          - Build the mod ($(TARGET))"
	@echo "  make clean    - Remove generated object files and the mod file"
	@echo "  make distclean - Remove the entire build directory"
	@echo "  make install  - Build and copy config/docs to build directory"
	@echo "  make help     - Display this help information"
	@echo ""
	@echo "Optimization: Building with -Os flag for minimum size"
