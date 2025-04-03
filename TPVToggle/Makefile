# Makefile for building KCD2_TPVToggle.asi using g++ (MinGW)

# Compiler and linker
CXX = g++
WINDRES = windres

# Directories
SRC_DIR = src
EXTERNAL_DIR = external
BUILD_DIR = build
OBJ_DIR = $(BUILD_DIR)/obj
MINHOOK_DIR = $(EXTERNAL_DIR)/minhook
MINHOOK_OBJ_DIR = $(OBJ_DIR)/minhook

# Check if MinHook submodule is initialized
ifeq ($(wildcard $(MINHOOK_DIR)/include/MinHook.h),)
$(error MinHook submodule not initialized. Please run 'git submodule update --init --recursive')
endif

# Determine C++ standard support
ifeq ($(shell $(CXX) -std=c++20 -E - 2>/dev/null >/dev/null || echo fail),fail)
    CXX_STANDARD := -std=c++2a
else
    CXX_STANDARD := -std=c++20
endif

# Compiler flags
CXXFLAGS = -std=c++20 -m64 -O2 -Wall -Wextra -I$(MINHOOK_DIR)/include -I$(SRC_DIR)

# Linker flags
LDFLAGS = -static -static-libgcc -static-libstdc++ -shared
LDLIBS = -ldinput8 -luser32 -lkernel32 -lpsapi -Wl,--add-stdcall-alias

# Source files - standard approach assuming exception_handler files are removed
SRCS = $(wildcard $(SRC_DIR)/*.cpp)
OBJS = $(patsubst $(SRC_DIR)/%.cpp,$(OBJ_DIR)/%.o,$(SRCS))

# Add MinHook object files
MINHOOK_SRCS = $(wildcard $(MINHOOK_DIR)/src/*.c) $(wildcard $(MINHOOK_DIR)/src/hde/*.c)
MINHOOK_OBJS = $(patsubst $(MINHOOK_DIR)/%.c,$(MINHOOK_OBJ_DIR)/%.o,$(MINHOOK_SRCS))

# Target
TARGET = $(BUILD_DIR)/KCD2_TPVToggle.asi

# Default target
all: prepare $(TARGET)

# Create directories
prepare:
	@mkdir -p $(OBJ_DIR)
	@mkdir -p $(MINHOOK_OBJ_DIR)/src/hde

# Link everything together
$(TARGET): $(OBJS) $(MINHOOK_OBJS)
	$(CXX) $(LDFLAGS) -o $@ $^ $(LDLIBS)
	@echo Build complete: $@

# Compile source files
$(OBJ_DIR)/%.o: $(SRC_DIR)/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c $< -o $@

# Compile MinHook source files
$(MINHOOK_OBJ_DIR)/%.o: $(MINHOOK_DIR)/%.c
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c $< -o $@

# Clean target removes compiled objects and executables
clean:
	rm -rf $(OBJ_DIR)/* $(TARGET)

# Clean everything including build directory
distclean:
	rm -rf $(BUILD_DIR)

# Copy config files and documentation to build directory
install: all
	@cp -f $(SRC_DIR)/../KCD2_TPVToggle.ini $(BUILD_DIR)/
	@cp -f $(SRC_DIR)/../README.txt $(BUILD_DIR)/
	@cp -f $(SRC_DIR)/../THIRD-PARTY-LICENSES.txt $(BUILD_DIR)/
	@echo Files copied to $(BUILD_DIR)

# Help target to display available commands
help:
	@echo "Available commands:"
	@echo "  make          - Build the mod"
	@echo "  make clean    - Remove object files and executable"
	@echo "  make distclean - Remove entire build directory"
	@echo "  make install  - Build and copy configuration files to build directory"
	@echo "  make help     - Display this help information"

.PHONY: all prepare clean distclean install help
