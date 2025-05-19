# Makefile for building KCD2_TPVToggle.asi using g++ (MinGW)

CXX := g++
WINDRES := windres

# Base Directories
SRC_DIR := src
ASM_DIR := $(SRC_DIR)/asm
EXTERNAL_DIR := external
BUILD_DIR := build
DEV_BUILD_DIR := build

# MinHook and SimpleIni Directories
MINHOOK_DIR := $(EXTERNAL_DIR)/minhook
MINHOOK_INCLUDE_DIR := $(MINHOOK_DIR)/include
MINHOOK_SRC_DIR := $(MINHOOK_DIR)/src
MINHOOK_HDE_SRC_DIR := $(MINHOOK_SRC_DIR)/hde

SIMPLEINI_DIR := $(EXTERNAL_DIR)/simpleini

DIRECTXMATH_DIR := $(EXTERNAL_DIR)/DirectXMath
DIRECTXMATH_INCLUDE_DIR := $(DIRECTXMATH_DIR)/Inc

JSON_DIR := $(EXTERNAL_DIR)/json
JSON_INCLUDE_DIR := $(JSON_DIR)/include

# Output Directories
OBJ_DIR := $(BUILD_DIR)/obj
DEV_OBJ_DIR := $(DEV_BUILD_DIR)/obj
MINHOOK_OBJ_OUT_DIR := $(OBJ_DIR)/minhook
MINHOOK_DEV_OBJ_OUT_DIR := $(DEV_OBJ_DIR)/minhook

# Final Targets
TARGET := $(BUILD_DIR)/KCD2_TPVToggle.asi
DEV_TARGET := $(DEV_BUILD_DIR)/KCD2_TPVToggle.asi

# --- Sanity Check for Dependencies ---
ifeq ($(wildcard $(MINHOOK_INCLUDE_DIR)/MinHook.h),)
$(error MinHook submodule not initialized. Please run 'git submodule update --init --recursive')
endif
ifeq ($(wildcard $(SIMPLEINI_DIR)/SimpleIni.h),)
$(error SimpleIni submodule not initialized. Please run 'git submodule update --init --recursive')
endif
ifeq ($(wildcard $(DIRECTXMATH_INCLUDE_DIR)/DirectXMath.h),)
$(error DirectXMath submodule not initialized. Please run 'git submodule update --init --recursive')
endif
ifeq ($(wildcard $(JSON_INCLUDE_DIR)/nlohmann/json.hpp),)
$(error json submodule not initialized. Please run 'git submodule update --init --recursive')
endif

# --- Common Compiler Flags ---
INCLUDE_PATHS := -I$(MINHOOK_INCLUDE_DIR) -I$(SIMPLEINI_DIR) -I$(DIRECTXMATH_INCLUDE_DIR) -I$(JSON_INCLUDE_DIR) -I$(SRC_DIR)
COMMON_FLAGS := -std=c++17 -m64 -Wall -Wextra $(INCLUDE_PATHS) -fexceptions \
                -DWINVER=0x0601 -D_WIN32_WINNT=0x0601

# --- Production Build Flags ---
# Optimize for size, remove unused sections, no RTTI
PROD_CXXFLAGS := $(COMMON_FLAGS) -Os -fdata-sections -ffunction-sections -fno-rtti -fno-threadsafe-statics
PROD_CFLAGS := $(PROD_CXXFLAGS)
PROD_LDFLAGS := -static -static-libgcc -static-libstdc++ -shared -s -Wl,--gc-sections

# --- Development Build Flags ---
# Optimize for speed, include debug symbols, keep RTTI
DEV_CXXFLAGS := $(COMMON_FLAGS) -O0 -g -fno-omit-frame-pointer -D_DEBUG
DEV_CFLAGS := $(DEV_CXXFLAGS)
DEV_LDFLAGS := -static -static-libgcc -static-libstdc++ -shared -g

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

# --- Production Object Files ---
CPP_OBJS := $(patsubst $(SRC_DIR)/%.cpp,$(OBJ_DIR)/%.o,$(CPP_SRCS))
ASM_OBJS := $(patsubst $(ASM_DIR)/%.S,$(OBJ_DIR)/%.o,$(ASM_SRCS))
MINHOOK_C_OBJS := $(patsubst $(MINHOOK_SRC_DIR)/%.c,$(MINHOOK_OBJ_OUT_DIR)/%.o,$(MINHOOK_C_SRCS))
MINHOOK_HDE_OBJS := $(patsubst $(MINHOOK_HDE_SRC_DIR)/%.c,$(MINHOOK_OBJ_OUT_DIR)/%.o,$(MINHOOK_HDE_SRCS))
ALL_OBJS := $(sort $(CPP_OBJS) $(ASM_OBJS) $(MINHOOK_C_OBJS) $(MINHOOK_HDE_OBJS))

# --- Development Object Files ---
DEV_CPP_OBJS := $(patsubst $(SRC_DIR)/%.cpp,$(DEV_OBJ_DIR)/%.o,$(CPP_SRCS))
DEV_ASM_OBJS := $(patsubst $(ASM_DIR)/%.S,$(DEV_OBJ_DIR)/%.o,$(ASM_SRCS))
DEV_MINHOOK_C_OBJS := $(patsubst $(MINHOOK_SRC_DIR)/%.c,$(MINHOOK_DEV_OBJ_OUT_DIR)/%.o,$(MINHOOK_C_SRCS))
DEV_MINHOOK_HDE_OBJS := $(patsubst $(MINHOOK_HDE_SRC_DIR)/%.c,$(MINHOOK_DEV_OBJ_OUT_DIR)/%.o,$(MINHOOK_HDE_SRCS))
ALL_DEV_OBJS := $(sort $(DEV_CPP_OBJS) $(DEV_ASM_OBJS) $(DEV_MINHOOK_C_OBJS) $(DEV_MINHOOK_HDE_OBJS))

# --- Make Rules ---

.PHONY: all clean distclean install dev help prepare prepare_dev

# Default target: ensure build directories exist, then build the target
all: prepare $(TARGET)

# Development target: faster builds with debug symbols
dev: prepare_dev $(DEV_TARGET)

# Rule to create necessary build directories
prepare:
	@echo "Creating build directories..."
	@mkdir -p $(BUILD_DIR) $(OBJ_DIR) $(OBJ_DIR)/hooks $(MINHOOK_OBJ_OUT_DIR)

# Rule to create necessary development build directories
prepare_dev:
	@echo "Creating development build directories..."
	@mkdir -p $(DEV_BUILD_DIR) $(DEV_OBJ_DIR) $(DEV_OBJ_DIR)/hooks $(MINHOOK_DEV_OBJ_OUT_DIR)

# Rule to link the final production DLL/ASI target
$(TARGET): $(ALL_OBJS)
	@echo "Linking production target $@..."
	$(CXX) $(PROD_LDFLAGS) -o $@ $^ $(LDLIBS)
	@echo "Production build complete: $@"
	@du -b $(TARGET) | cut -f1 | xargs -I {} echo "Size: {} bytes"

# Rule to link the development DLL/ASI target
$(DEV_TARGET): $(ALL_DEV_OBJS)
	@echo "Linking development target $@..."
	$(CXX) $(DEV_LDFLAGS) -o $@ $^ $(LDLIBS)
	@echo "Development build complete: $@"
	@du -b $(DEV_TARGET) | cut -f1 | xargs -I {} echo "Size: {} bytes"

# Rules to compile production C++ source files
$(OBJ_DIR)/%.o: $(SRC_DIR)/%.cpp
	@echo "Compiling production C++ $<..."
	@mkdir -p $(dir $@)
	$(CXX) $(PROD_CXXFLAGS) -c $< -o $@

# Rules to compile development C++ source files
$(DEV_OBJ_DIR)/%.o: $(SRC_DIR)/%.cpp
	@echo "Compiling development C++ $<..."
	@mkdir -p $(dir $@)
	$(CXX) $(DEV_CXXFLAGS) -c $< -o $@

# Rule to assemble production Assembly source files
$(OBJ_DIR)/%.o: $(ASM_DIR)/%.S
	@echo "Assembling production $<..."
	@mkdir -p $(dir $@)
	$(CXX) $(PROD_CFLAGS) -c $< -o $@

# Rule to assemble development Assembly source files
$(DEV_OBJ_DIR)/%.o: $(ASM_DIR)/%.S
	@echo "Assembling development $<..."
	@mkdir -p $(dir $@)
	$(CXX) $(DEV_CFLAGS) -c $< -o $@

# Rule to compile production MinHook Core C source files
$(MINHOOK_OBJ_OUT_DIR)/%.o: $(MINHOOK_SRC_DIR)/%.c
	@echo "Compiling production MinHook $<..."
	@mkdir -p $(dir $@)
	$(CXX) $(PROD_CFLAGS) -c $< -o $@

# Rule to compile development MinHook Core C source files
$(MINHOOK_DEV_OBJ_OUT_DIR)/%.o: $(MINHOOK_SRC_DIR)/%.c
	@echo "Compiling development MinHook $<..."
	@mkdir -p $(dir $@)
	$(CXX) $(DEV_CFLAGS) -c $< -o $@

# Rule to compile production MinHook HDE C source files
$(MINHOOK_OBJ_OUT_DIR)/%.o: $(MINHOOK_HDE_SRC_DIR)/%.c
	@echo "Compiling production MinHook HDE $<..."
	@mkdir -p $(dir $@)
	$(CXX) $(PROD_CFLAGS) -c $< -o $@

# Rule to compile development MinHook HDE C source files
$(MINHOOK_DEV_OBJ_OUT_DIR)/%.o: $(MINHOOK_HDE_SRC_DIR)/%.c
	@echo "Compiling development MinHook HDE $<..."
	@mkdir -p $(dir $@)
	$(CXX) $(DEV_CFLAGS) -c $< -o $@

# --- Utility Targets ---

# Clean target: remove object files and final target
clean:
	@echo "Cleaning build files..."
	rm -rf $(OBJ_DIR) $(DEV_OBJ_DIR) $(TARGET) $(DEV_TARGET)

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
	@echo "  make          - Build the production mod ($(TARGET)) optimized for size"
	@echo "  make dev      - Build the development mod ($(DEV_TARGET)) with debug symbols"
	@echo "  make clean    - Remove generated object files and the mod files"
	@echo "  make distclean - Remove the entire build directory"
	@echo "  make install  - Build and copy config/docs to build directory"
	@echo "  make help     - Display this help information"
	@echo ""
	@echo "Production: Building with -Os flag for minimum size"
	@echo "Development: Building with -O0 -g flags for debug information"
