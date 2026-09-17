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
#   make install    install for the current user and select it as the screen saver

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
# -MMD -MP makes the compiler emit a .d file listing every header a translation unit read, which
# is then included below. Without it nothing rebuilds when a header changes, and the result is not
# a build error but a silent ODR violation: two objects compiled against different layouts of the
# same struct, linked together, corrupting memory at runtime. That cost an access violation that
# looked seed-dependent and vanished whenever the affected file was touched for any other reason.
DEPFLAGS = -MMD -MP
WARN     = -Wall -Wextra
CXXFLAGS = -std=c++17 $(WARN) $(DEPFLAGS) -O2 -municode
CFLAGS   = -std=c11 $(WARN) $(DEPFLAGS) -O2

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
# Shared GLSL pulled in by #include. Not compiled on its own, but every stage depends on it:
# editing scene.glsl without rebuilding the shaders that read it is a mismatch nothing reports.
SHADER_INCS  := $(sort $(wildcard shaders/*.glsl))
SPVS         := $(patsubst shaders/%,$(SPVDIR)/%.spv,$(SHADER_SRCS))
SHADER_C     := $(GENDIR)/shaders_generated.c

# The preview thumbnail (spec 9.3). A baked still rather than something generated at build time:
# making it needs a GPU and a run, which a build does not have.
THUMB_BIN    := assets/preview_thumb.bin
THUMB_C      := $(GENDIR)/preview_generated.c

APP_OBJS     := $(APP_CXX_SRCS:%.cpp=$(OBJDIR)/%.o) \
                $(C_SRCS:%.c=$(OBJDIR)/%.o) \
                $(SHADER_C:$(GENDIR)/%.c=$(OBJDIR)/gen/%.o) \
                $(THUMB_C:$(GENDIR)/%.c=$(OBJDIR)/gen/%.o)
SELFTEST_OBJS := $(filter-out $(OBJDIR)/src/main.o,$(APP_OBJS))                  $(SELFTEST_SRCS:%.cpp=$(OBJDIR)/%.o)

SELFTEST_EXE := $(BUILD)/nuke-saver-selftest.exe

# One .d beside every .o, covering both targets' objects.
DEPS := $(APP_OBJS:.o=.d) $(SELFTEST_SRCS:%.cpp=$(OBJDIR)/%.d)

.PHONY: all clean install selftest
.SUFFIXES:

all: $(TARGET)

# ---- shaders ---------------------------------------------------------------------------
# A shader that fails to compile fails the build (spec 13.3). -Werror makes that true for
# warnings too, so a pipeline never ships with a diagnostic nobody read.

$(SPVDIR)/%.spv: shaders/% $(SHADER_INCS) | $(SPVDIR)
	$(GLSLC) -Werror -O -Ishaders -o $@ $<

# One generated TU holding every blob plus the lookup table.
$(SHADER_C): $(SPVS) tools/embed_shaders.sh | $(GENDIR)
	sh tools/embed_shaders.sh $@ $(SPVS)

$(THUMB_C): $(THUMB_BIN) tools/embed_blob.sh | $(GENDIR)
	sh tools/embed_blob.sh $@ app/preview_image.h g_preview_thumb $(THUMB_BIN)

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

# Last, so a missing .d on the first build is not an error - the object does not exist yet either,
# and it will be built with its dependencies recorded.
-include $(DEPS)

# Installed under the user's AppData rather than into System32, for two reasons. System32 needs
# Administrator, and Windows is happy to run a screen saver from anywhere as long as the registry
# names a full path. And it must not be the build output: Windows holds the running .scr open, so
# a screen saver that kicks in while the machine is idle makes the next link fail with a permission
# error that says nothing about what is holding the file.
#
# The destination is read out of the registry rather than out of the environment. Make runs its
# recipes through the MSYS shell, which is started here with almost nothing in it: LOCALAPPDATA,
# USERPROFILE and APPDATA are all empty, and HOME is the MSYS home rather than the Windows profile.
# Shell Folders is where that answer actually lives, and reg.exe is already needed below.
SHELLFOLDERS = HKCU\\Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Shell Folders
DESKTOPKEY   = HKCU\\Control Panel\\Desktop

install: $(TARGET)
	@local=$$(reg.exe query "$(SHELLFOLDERS)" //v "Local AppData" \
		| sed -n 's/.*REG_[A-Z_]*[[:space:]]*//p' | tr -d '\r'); \
	test -n "$$local" || { echo "could not find Local AppData in the registry" >&2; exit 1; }; \
	dir=$$(cygpath -u "$$local")/Nuke-Saver; \
	mkdir -p "$$dir"; \
	cp $(TARGET) "$$dir/$(TARGET)" || { \
		echo "could not write $$dir/$(TARGET); it is probably running" >&2; exit 1; }; \
	win="$$(cygpath -w "$$dir")\\$(TARGET)"; \
	reg.exe add "$(DESKTOPKEY)" //v SCRNSAVE.EXE //t REG_SZ //d "$$win" //f > /dev/null; \
	reg.exe add "$(DESKTOPKEY)" //v ScreenSaveActive //t REG_SZ //d 1 //f > /dev/null; \
	echo "installed $$win and selected it as the screen saver"; \
	echo "the timeout and the lock-on-resume setting are left alone; change them in Settings"
