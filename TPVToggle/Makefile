# Makefile for building KCD2_TPVToggle.asi using g++ (MinGW)

# Compiler
CXX := g++

# Output DLL
TARGET := build/KCD2_TPVToggle.asi

# Source files (sorted and line-separated)
SRCS := \
    aob_scanner.cpp \
    config.cpp \
    dllmain.cpp \
    exception_handler.cpp \
    logger.cpp \
    toggle_thread.cpp \
    version.cpp

# Compiler flags
CXXFLAGS := -std=c++20 -m64 -O2 -Wall -Wextra \
            -static -static-libgcc -static-libstdc++ \
            -DWIN32_LEAN_AND_MEAN -DNOMINMAX

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
