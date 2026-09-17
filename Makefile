# nuke-saver Makefile
# Builds a Windows screensaver (.scr) that renders a simulated nuclear detonation with Vulkan.
#
# Requires MinGW-w64 (MSYS2 UCRT64 is what this is developed against) plus glslc from the
# shaderc package and xxd from MSYS2's base. See docs/Nuke-Saver-Spec.md section 13.
#
# Targets:
#   make            build nuke-saver.scr
#   make selftest   build and run the console toolchain probe (build/nuke-saver-selftest.exe)
#   make clean      remove everything generated
#   make install    copy the .scr into System32 (requires Administrator)

CXX      = g++
CC       = gcc
WINDRES  = windres
GLSLC    = glslc
XXD      = xxd

TARGET   = nuke-saver.scr
RES      = nuke-saver.res

BUILD    = build
OBJDIR   = $(BUILD)/obj
SPVDIR   = $(BUILD)/spv
GENDIR   = $(BUILD)/gen
BUILDTMP = $(BUILD)/tmp

# Keep compiler/linker temp files inside the project; don't depend on TMP/TEMP.
export TMP    := $(abspath $(BUILDTMP))
export TEMP   := $(TMP)
export TMPDIR := $(TMP)

# VK_USE_PLATFORM_WIN32_KHR exposes the Win32 surface entry points; without it volk declares
# neither vkCreateWin32SurfaceKHR nor the presentation-support query.
# VK_NO_PROTOTYPES is not optional: spec section 12 requires that nothing static-imports
# vulkan-1.dll, so every entry point comes from volk at runtime.
CPPFLAGS = -Isrc -Ithird_party/volk -Ithird_party/vma -DVK_NO_PROTOTYPES -DVK_USE_PLATFORM_WIN32_KHR \
           -DUNICODE -D_UNICODE
WARN     = -Wall -Wextra
CXXFLAGS = -std=c++17 $(WARN) -O2 -municode
CFLAGS   = -std=c11 $(WARN) -O2

LDFLAGS  = -mwindows -municode -static
LDLIBS   = -lgdi32 -lshell32 -ladvapi32

# ---- sources ---------------------------------------------------------------------------

# Every .cpp under src/ belongs to the screensaver except the console selftest harness, which
# is any src/selftest_*.cpp. Matching on the prefix rather than naming files means a new suite
# is picked up by both targets without touching this.
ALL_CXX_SRCS := $(shell find src -name '*.cpp' | sort)
SELFTEST_SRCS := $(filter src/selftest_%,$(ALL_CXX_SRCS))
APP_CXX_SRCS  := $(filter-out $(SELFTEST_SRCS),$(ALL_CXX_SRCS))

C_SRCS       := third_party/volk/volk.c

SHADER_SRCS  := $(sort $(wildcard shaders/*.vert shaders/*.frag shaders/*.comp))
SPVS         := $(patsubst shaders/%,$(SPVDIR)/%.spv,$(SHADER_SRCS))
SHADER_C     := $(GENDIR)/shaders_generated.c

APP_OBJS     := $(APP_CXX_SRCS:%.cpp=$(OBJDIR)/%.o) \
                $(C_SRCS:%.c=$(OBJDIR)/%.o) \
                $(SHADER_C:$(GENDIR)/%.c=$(OBJDIR)/gen/%.o)
SELFTEST_OBJS := $(filter-out $(OBJDIR)/src/main.o,$(APP_OBJS))                  $(SELFTEST_SRCS:%.cpp=$(OBJDIR)/%.o)

SELFTEST_EXE := $(BUILD)/nuke-saver-selftest.exe

.PHONY: all clean install selftest
.SUFFIXES:

all: $(TARGET)

# ---- shaders ---------------------------------------------------------------------------
# A shader that fails to compile fails the build (spec 13.3). -Werror makes that true for
# warnings too, so a pipeline never ships with a diagnostic nobody read.

$(SPVDIR)/%.spv: shaders/% | $(SPVDIR)
	$(GLSLC) -Werror -O -o $@ $<

# One generated TU holding every blob plus the lookup table.
$(SHADER_C): $(SPVS) tools/embed_shaders.sh | $(GENDIR)
	sh tools/embed_shaders.sh $@ $(SPVS)

# ---- objects ---------------------------------------------------------------------------
# Every object depends on the Makefile: a change to CPPFLAGS is a change to what the code
# means, and a half-rebuilt tree links with the wrong entry points rather than failing loudly.

$(OBJDIR)/%.o: %.cpp $(SHADER_C) Makefile | $(BUILDTMP)
	@mkdir -p $(dir $@)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -c -o $@ $<

$(OBJDIR)/%.o: %.c Makefile | $(BUILDTMP)
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

$(OBJDIR)/gen/%.o: $(GENDIR)/%.c Makefile | $(BUILDTMP)
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

# ---- resources -------------------------------------------------------------------------

$(RES): nuke-saver.rc nuke-saver.manifest | $(BUILDTMP)
	$(WINDRES) nuke-saver.rc -O coff -o $@

# ---- link ------------------------------------------------------------------------------

$(TARGET): $(APP_OBJS) $(RES) | $(BUILDTMP)
	$(CXX) $(CXXFLAGS) -o $@ $(APP_OBJS) $(RES) $(LDFLAGS) $(LDLIBS)

# The selftest is a console binary: it needs stdout, so it drops -mwindows — and -municode
# with it, since that selects wWinMain as the entry point and the harness has a plain main().
$(SELFTEST_EXE): $(SELFTEST_OBJS) | $(BUILD) $(BUILDTMP)
	$(CXX) -std=c++17 $(WARN) -O2 -o $@ $(SELFTEST_OBJS) -static $(LDLIBS)

selftest: $(SELFTEST_EXE)
	@echo
	@./$(SELFTEST_EXE)

# ---- housekeeping ----------------------------------------------------------------------

$(BUILD) $(SPVDIR) $(GENDIR) $(BUILDTMP):
	@mkdir -p $@

clean:
	rm -rf $(TARGET) $(RES) $(BUILD)

install: $(TARGET)
	cp $(TARGET) "$(SYSTEMROOT)/System32/"
