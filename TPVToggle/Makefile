# Makefile for building KCD2_TPVToggle.asi using g++ (MinGW)
# Uses local DirectXMath headers. Reverts to simpler rules for MinHook objects.

# Compiler and linker
CXX := g++
# Define RM for clean commands
RM := rm -f

# --- Base Directories ---
SRC_DIR := src
ASM_DIR := $(SRC_DIR)/asm
EXTERNAL_DIR := external
BUILD_DIR := build

# MinHook Source Directories
MINHOOK_DIR := $(EXTERNAL_DIR)/minhook
MINHOOK_INCLUDE_DIR := $(MINHOOK_DIR)/include
MINHOOK_SRC_DIR := $(MINHOOK_DIR)/src
MINHOOK_HDE_SRC_DIR := $(MINHOOK_SRC_DIR)/hde

# --- LOCAL DirectXMath Include Path ---
DXMATH_INCLUDE_DIR := $(EXTERNAL_DIR)/DirectXMath/Inc

# --- Output Directories ---
OBJ_DIR := $(BUILD_DIR)/obj
MINHOOK_OBJ_OUT_DIR := $(OBJ_DIR)/minhook

# --- Final Target ---
TARGET := $(BUILD_DIR)/KCD2_TPVToggle.asi

# --- Sanity Checks ---
ifeq ($(wildcard $(MINHOOK_INCLUDE_DIR)/MinHook.h),)
$(error MinHook header missing. Run 'git submodule update --init --recursive')
endif
ifeq ($(wildcard $(DXMATH_INCLUDE_DIR)/DirectXMath.h),)
$(error Local DirectXMath header missing. Check external/DirectXMath/Inc)
endif

# --- Compiler and Linker Flags ---
# Includes MinHook, Project Source, Local DirectXMath
INCLUDE_PATHS := -I$(MINHOOK_INCLUDE_DIR) -I$(SRC_DIR) -I$(DXMATH_INCLUDE_DIR)
CXXFLAGS := -std=c++20 -m64 -O2 -Wall -Wextra $(INCLUDE_PATHS)
CFLAGS := $(CXXFLAGS) # Use same flags for MinHook's C code
LDFLAGS := -static -static-libgcc -static-libstdc++ -shared
LDLIBS := -lpsapi -luser32 -lkernel32

# --- Source File Lists ---
CPP_SRCS := $(wildcard $(SRC_DIR)/*.cpp)
ASM_SRCS := $(wildcard $(ASM_DIR)/*.S)
MINHOOK_CORE_SRCS := $(wildcard $(MINHOOK_SRC_DIR)/*.c)
MINHOOK_HDE_SRCS := $(wildcard $(MINHOOK_HDE_SRC_DIR)/*.c)

# --- Object File Generation (Reverted to Simple .o) ---
CPP_OBJS := $(patsubst $(SRC_DIR)/%.cpp,$(OBJ_DIR)/%.o,$(CPP_SRCS))
# ** Reverted ASM Objects to simple .o - Check for Name Collisions! **
# If you have e.g. hook.S and hook.c (minhook), this WILL collide.
# Consider renaming hook.S or use unique suffix if collision occurs.
ASM_OBJS := $(patsubst $(ASM_DIR)/%.S,$(OBJ_DIR)/%.o,$(ASM_SRCS))
# MinHook Core source -> minhook object directory (simple .o)
MINHOOK_CORE_OBJS := $(patsubst $(MINHOOK_SRC_DIR)/%.c,$(MINHOOK_OBJ_OUT_DIR)/%.o,$(MINHOOK_CORE_SRCS))
# MinHook HDE source -> minhook object directory (simple .o)
MINHOOK_HDE_OBJS := $(patsubst $(MINHOOK_HDE_SRC_DIR)/%.c,$(MINHOOK_OBJ_OUT_DIR)/%.o,$(MINHOOK_HDE_SRCS))

# Combine ALL object files (Check output of 'make -n' or 'echo $(ALL_OBJS)' if clean still fails)
ALL_OBJS := $(sort $(CPP_OBJS) $(ASM_OBJS) $(MINHOOK_CORE_OBJS) $(MINHOOK_HDE_OBJS))


# --- Make Rules ---
.PHONY: all prepare clean distclean install help

all: prepare $(TARGET)

prepare:
	@echo "Creating build directories..."
	@mkdir -p "$(MINHOOK_OBJ_OUT_DIR)" # Will also create build/obj

$(TARGET): $(ALL_OBJS)
	@echo "Linking $(TARGET)..."
	$(CXX) $(LDFLAGS) -o $@ $^ $(LDLIBS)
	@echo "Build complete: $(TARGET)"


# --- Build Rules ---
# Use simple patterns similar to the original working version.
# No explicit dependency on MinHook internal headers here.

# Rule for project .cpp files -> build/obj/%.o
# Depends on source, main MinHook header, local DXMath, local headers
$(OBJ_DIR)/%.o: $(SRC_DIR)/%.cpp $(MINHOOK_INCLUDE_DIR)/MinHook.h $(DXMATH_INCLUDE_DIR)/DirectXMath.h $(SRC_DIR)/*.h | prepare
	@echo "Compiling C++ $< -> $@"
	$(CXX) $(CXXFLAGS) -c $< -o $@

# Rule for project .S files -> build/obj/%.o
# WARNING: Might conflict if C/CPP file has same base name!
$(OBJ_DIR)/%.o: $(ASM_DIR)/%.S | prepare
	@echo "Assembling $< -> $@"
	$(CXX) $(CFLAGS) -c $< -o $@

# Rule for MinHook core .c files -> build/obj/minhook/%.o
$(MINHOOK_OBJ_OUT_DIR)/%.o: $(MINHOOK_SRC_DIR)/%.c $(MINHOOK_INCLUDE_DIR)/MinHook.h | prepare
	@echo "Compiling MinHook Core $< -> $@"
	$(CXX) $(CFLAGS) -c $< -o $@

# Rule for MinHook HDE .c files -> build/obj/minhook/%.o
$(MINHOOK_OBJ_OUT_DIR)/%.o: $(MINHOOK_HDE_SRC_DIR)/%.c $(MINHOOK_INCLUDE_DIR)/MinHook.h | prepare
	@echo "Compiling MinHook HDE $< -> $@"
	$(CXX) $(CFLAGS) -c $< -o $@


# --- Utility Targets ---
.PHONY: clean
clean:
	@echo "Cleaning build artifacts..."
	# Ensure ALL_OBJS expands correctly now; if errors persist, check this variable's content.
	# Use: @echo Debug objects: $(ALL_OBJS)
	$(RM) $(ALL_OBJS) $(TARGET)

.PHONY: distclean
distclean: clean
	@echo "Cleaning entire build directory..."
	$(RM) -r "$(BUILD_DIR)"

.PHONY: install
install: all
	@echo "Copying distribution files to $(BUILD_DIR)..."
	@cp -fv KCD2_TPVToggle.ini "$(BUILD_DIR)/"
	@cp -fv README.txt "$(BUILD_DIR)/"
	@cp -fv THIRD-PARTY-LICENSES.txt "$(BUILD_DIR)/"
	@echo "Install step complete. (Check ASI Loader)"

.PHONY: help
help:
	@echo "Available make commands:"
	@echo "  make / make all     - Build the mod ($(TARGET))"
	@echo "  make clean          - Remove object files and target"
	@echo "  make distclean      - Remove the entire build directory"
	@echo "  make install        - Build and copy distributables"
	@echo "  make help           - Display this help message"
