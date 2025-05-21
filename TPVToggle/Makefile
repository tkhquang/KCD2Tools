# Makefile for building KCD2_TPVToggle.asi using g++ (MinGW)

# --- Toolchain ---
CXX := g++
CC := gcc
PROJECT_AS := $(CXX)
WINDRES := windres
MINGW_MAKE := mingw32-make


# --- Base Project Directories ---
SRC_DIR := src
ASM_DIR := $(SRC_DIR)/asm
EXTERNAL_DIR := external
BUILD_DIR := build
DEV_BUILD_DIR := build
STATIC_DIR := static

# --- Dependency Source and Include Directories ---
SAFETYHOOK_DIR := $(EXTERNAL_DIR)/safetyhook
SAFETYHOOK_BUILD_DIR := $(SAFETYHOOK_DIR)/build
SAFETYHOOK_INCLUDE_DIR := $(SAFETYHOOK_DIR)/include

SIMPLEINI_DIR := $(EXTERNAL_DIR)/simpleini
DIRECTXMATH_DIR := $(EXTERNAL_DIR)/DirectXMath
DIRECTXMATH_INCLUDE_DIR := $(DIRECTXMATH_DIR)/Inc
JSON_DIR := $(EXTERNAL_DIR)/json
JSON_INCLUDE_DIR := $(JSON_DIR)/include

# --- Object File Output Directories ---
OBJ_DIR := $(BUILD_DIR)/obj
DEV_OBJ_DIR := $(DEV_BUILD_DIR)/obj

# --- Final Target Executables ---
TARGET_BASENAME := KCD2_TPVToggle
TARGET := $(BUILD_DIR)/$(TARGET_BASENAME).asi
DEV_TARGET := $(DEV_BUILD_DIR)/$(TARGET_BASENAME).asi

# --- Paths to Statically Built Libraries from SafetyHook's CMake process ---
BUILT_SAFETYHOOK_LIB := $(SAFETYHOOK_BUILD_DIR)/libsafetyhook.a
BUILT_ZYDIS_LIB := $(SAFETYHOOK_BUILD_DIR)/_deps/zydis-build/libZydis.a
BUILT_ZYCORE_LIB := $(SAFETYHOOK_BUILD_DIR)/_deps/zydis-build/zycore/libZycore.a

# --- Dependency list for convenient checking ---
DEPS := $(SAFETYHOOK_BUILD_DIR) $(BUILT_ZYDIS_LIB) $(BUILT_ZYCORE_LIB)

# --- Sanity Checks for Dependencies ---
ifeq ($(wildcard $(SAFETYHOOK_DIR)/CMakeLists.txt),)
$(error SafetyHook directory or its CMakeLists.txt not found at $(SAFETYHOOK_DIR). Please ensure the submodule is initialized.)
endif
ifeq ($(wildcard $(SAFETYHOOK_INCLUDE_DIR)/safetyhook.hpp),)
$(error SafetyHook project header not found. Expected: $(SAFETYHOOK_INCLUDE_DIR)/safetyhook.hpp)
endif

# --- Common Compiler Flags ---
INCLUDE_PATHS := \
    -I$(SAFETYHOOK_INCLUDE_DIR) \
    -I$(SIMPLEINI_DIR) \
    -I$(DIRECTXMATH_INCLUDE_DIR) \
    -I$(JSON_INCLUDE_DIR) \
    -I$(SRC_DIR)

COMMON_DEFINES := \
    -DWINVER=0x0601 \
    -D_WIN32_WINNT=0x0601 \
    -DSAFETYHOOK_NO_DLL \
    -DZYDIS_STATIC_BUILD \
    -DZYCORE_STATIC_BUILD

SUPPRESS_WARNINGS := -Wno-unused-but-set-variable -Wno-reorder

COMMON_COMPILE_FLAGS := -m64 -Wall -Wextra $(INCLUDE_PATHS) -fexceptions $(COMMON_DEFINES) $(SUPPRESS_WARNINGS)
CXX_STANDARD_FLAG := -std=c++23
PROJECT_ASFLAGS := -m64

# --- Production Build Specific Flags ---
PROD_OPTIMIZATION_FLAGS := -Os
PROD_EXTRA_CXXFLAGS := -fdata-sections -ffunction-sections -fno-rtti -fno-threadsafe-statics
PROD_EXTRA_PROJECT_ASFLAGS := $(PROD_OPTIMIZATION_FLAGS)
PROD_CXXFLAGS := $(COMMON_COMPILE_FLAGS) $(CXX_STANDARD_FLAG) $(PROD_OPTIMIZATION_FLAGS) $(PROD_EXTRA_CXXFLAGS)
PROD_PROJECT_ASFLAGS  := $(PROJECT_ASFLAGS) $(PROD_EXTRA_PROJECT_ASFLAGS)
PROD_LDFLAGS  := -static -static-libgcc -static-libstdc++ -shared -s -Wl,--gc-sections

# --- Development Build Specific Flags ---
DEV_DEBUG_FLAGS := -g -fno-omit-frame-pointer
DEV_EXTRA_CXXFLAGS := -D_DEBUG
DEV_EXTRA_PROJECT_ASFLAGS := $(DEV_DEBUG_FLAGS)
DEV_CXXFLAGS := $(COMMON_COMPILE_FLAGS) $(CXX_STANDARD_FLAG) $(DEV_DEBUG_FLAGS) $(DEV_EXTRA_CXXFLAGS) -O0
DEV_PROJECT_ASFLAGS  := $(PROJECT_ASFLAGS) $(DEV_EXTRA_PROJECT_ASFLAGS)
DEV_LDFLAGS  := -static -static-libgcc -static-libstdc++ -shared -g

LDLIBS := -lpsapi -luser32 -lkernel32 -lshell32

PROJECT_CPP_SRCS := $(wildcard $(SRC_DIR)/*.cpp) $(wildcard $(SRC_DIR)/hooks/*.cpp)
PROJECT_ASM_SRCS := $(wildcard $(ASM_DIR)/*.S)

PROJECT_CPP_OBJS := $(patsubst $(SRC_DIR)/%.cpp,$(OBJ_DIR)/%.o,$(PROJECT_CPP_SRCS))
PROJECT_ASM_OBJS := $(patsubst $(ASM_DIR)/%.S,$(OBJ_DIR)/%.o,$(PROJECT_ASM_SRCS))
ALL_OBJS := $(sort $(PROJECT_CPP_OBJS) $(PROJECT_ASM_OBJS))

DEV_PROJECT_CPP_OBJS := $(patsubst $(SRC_DIR)/%.cpp,$(DEV_OBJ_DIR)/%.o,$(PROJECT_CPP_SRCS))
DEV_PROJECT_ASM_OBJS := $(patsubst $(ASM_DIR)/%.S,$(DEV_OBJ_DIR)/%.o,$(PROJECT_ASM_SRCS))
ALL_DEV_OBJS := $(sort $(DEV_PROJECT_CPP_OBJS) $(DEV_PROJECT_ASM_OBJS))

# --- Main Target Rules ---
all: prepare $(TARGET)
dev: prepare_dev $(DEV_TARGET)

# --- Dependency Building Logic ---
$(BUILT_SAFETYHOOK_LIB): $(SAFETYHOOK_DIR)/CMakeLists.txt
	@echo "---- Building SafetyHook & dependencies via CMake ----"
	@cd $(SAFETYHOOK_DIR) && \
	  rm -rf build && \
	  cmake -B build  && \
	  cmake --build build -j4
	@echo "---- SafetyHook & dependencies build attempt complete. ----"

$(BUILT_ZYDIS_LIB): $(BUILT_SAFETYHOOK_LIB)
	@# Zydis is built as part of SafetyHook, nothing to do

$(BUILT_ZYCORE_LIB): $(BUILT_SAFETYHOOK_LIB)
	@# Zycore is built as part of SafetyHook, nothing to do


$(TARGET): $(DEPS) $(ALL_OBJS)
	@echo "Linking production target $(TARGET_BASENAME).asi..."
	$(CXX) $(PROD_LDFLAGS) -o $@ $(ALL_OBJS) $(BUILT_SAFETYHOOK_LIB) $(BUILT_ZYDIS_LIB) $(BUILT_ZYCORE_LIB) $(LDLIBS)
	@echo "Production build complete: $@"
	@du -b $(TARGET) | cut -f1 | xargs -I {} echo "Size: {} bytes"

$(DEV_TARGET): $(DEPS) $(ALL_DEV_OBJS)
	@echo "Linking development target $(TARGET_BASENAME)_dev.asi..."
	$(CXX) $(DEV_LDFLAGS) -o $@ $(ALL_DEV_OBJS) $(BUILT_SAFETYHOOK_LIB) $(BUILT_ZYDIS_LIB) $(BUILT_ZYCORE_LIB) $(LDLIBS)
	@echo "Development build complete: $@"
	@du -b $(DEV_TARGET) | cut -f1 | xargs -I {} echo "Size: {} bytes"

# Convenience: 'make deps' to build dependencies ONLY, not required for all/dev
.PHONY: deps
deps: $(DEPS)
	@echo "All required static libs are built."

prepare:
	@echo "Creating production build directories for the project..."
	@mkdir -p $(BUILD_DIR) $(OBJ_DIR) $(OBJ_DIR)/hooks

prepare_dev:
	@echo "Creating development build directories for the project..."
	@mkdir -p $(DEV_BUILD_DIR) $(DEV_OBJ_DIR) $(DEV_OBJ_DIR)/hooks

$(OBJ_DIR)/%.o: $(SRC_DIR)/%.cpp
	@echo "Compiling (Prod) C++ $<"
	@mkdir -p $(dir $@)
	$(CXX) $(PROD_CXXFLAGS) -c $< -o $@
$(DEV_OBJ_DIR)/%.o: $(SRC_DIR)/%.cpp
	@echo "Compiling (Dev) C++ $<"
	@mkdir -p $(dir $@)
	$(CXX) $(DEV_CXXFLAGS) -c $< -o $@

$(OBJ_DIR)/%.o: $(ASM_DIR)/%.S
	@echo "Assembling (Prod) Project ASM $<"
	@mkdir -p $(dir $@)
	$(PROJECT_AS) $(PROD_PROJECT_ASFLAGS) -c $< -o $@
$(DEV_OBJ_DIR)/%.o: $(ASM_DIR)/%.S
	@echo "Assembling (Dev) Project ASM $<"
	@mkdir -p $(dir $@)
	$(PROJECT_AS) $(DEV_PROJECT_ASFLAGS) -c $< -o $@

clean:
	@echo "Cleaning build files for KCD2_TPVToggle..."
	rm -rf $(OBJ_DIR) $(TARGET)
	rm -rf $(DEV_OBJ_DIR) $(DEV_TARGET)

clean_safetyhook_build:
	@echo "Cleaning SafetyHook build directory $(SAFETYHOOK_BUILD_DIR)..."
	rm -rf $(SAFETYHOOK_BUILD_DIR)

distclean: clean clean_safetyhook_build
	@echo "Cleaning KCD2_TPVToggle build output ($(BUILD_DIR)) and SafetyHook build output..."
	rm -rf $(BUILD_DIR)

install: all
	@echo "Copying all files from static/ to $(BUILD_DIR)..."
	@cp -fv $(STATIC_DIR)/* $(BUILD_DIR)/ || copy $(STATIC_DIR)\* $(subst /,\,$(BUILD_DIR))\
	@echo "Install complete. Main ASI is: $(TARGET)"
	@du -b $(TARGET) | cut -f1 | xargs -I {} echo "Size: {} bytes"

help:
	@echo "Available commands:"
	@echo "  make deps             - Ensures SafetyHook & its dependencies are built if missing."
	@echo "  make                  - Build the production mod ($(TARGET))"
	@echo "  make dev              - Build the development mod ($(DEV_TARGET))"
	@echo "  make clean            - Remove KCD2_TPVToggle generated object files and mod ASIs"
	@echo "  make clean_safetyhook_build - Removes SafetyHook's own build directory"
	@echo "  make distclean        - Clean KCD2_TPVToggle output and SafetyHook build dir"
	@echo "  make install          - Build production mod and copy config/docs to $(BUILD_DIR)"
	@echo "  make help             - Display this help information"
	@echo ""
