# nuke-saver Makefile
# Builds a Windows screensaver (.scr file).

CXX      = g++
WINDRES  = windres
CXXFLAGS = -std=c++11 -Wall -Wextra -O2 -municode -DUNICODE -D_UNICODE
LDFLAGS  = -mwindows -municode -static
LDLIBS   = -lgdi32 -lshell32
TARGET   = nuke-saver.scr
RES      = nuke-saver.res
BUILDTMP = build/tmp

# Keep compiler/linker temp files inside the project; don't depend on TMP/TEMP.
export TMP    := $(abspath $(BUILDTMP))
export TEMP   := $(TMP)
export TMPDIR := $(TMP)

.PHONY: all clean install

all: $(TARGET)

$(BUILDTMP):
	mkdir -p $@

$(RES): nuke-saver.rc nuke-saver.manifest | $(BUILDTMP)
	$(WINDRES) nuke-saver.rc -O coff -o $@

$(TARGET): main.cpp $(RES) | $(BUILDTMP)
	$(CXX) $(CXXFLAGS) -o $@ main.cpp $(RES) $(LDFLAGS) $(LDLIBS)

clean:
	rm -rf $(TARGET) $(RES) build

install: $(TARGET)
	cp $(TARGET) "$(SYSTEMROOT)/System32/"
