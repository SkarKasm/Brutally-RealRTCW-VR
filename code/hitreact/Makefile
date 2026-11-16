# HitReact on its own: a static library, a shared library (DLL), and the demo.
#
#   make                         libhitreact.a, the demo (Linux, or MSYS2/MinGW on Windows)
#   make shared                  libhitreact.so / hitreact.dll (exports the C API only)
#   make CROSS=x86_64-w64-mingw32-   Windows build from Linux
#   make JOLT=/path/to/JoltPhysics   another copy of Jolt (default: RealRTCW-VR's)
#   make demo-run                both host spaces, timelines in out/
#   make test                    the library's own checks
#
# Jolt is built from source into out/jolt (once; takes a few minutes). The flags match
# RealRTCW-VR's so the results are the same as in the game: no FMA contraction, SSE4.2.

CROSS    ?=
CC       := $(CROSS)gcc
CXX      := $(CROSS)g++
AR       := $(CROSS)ar
JOLT     ?= ../JoltPhysics
OUT      ?= out$(if $(CROSS),-$(patsubst %-,%,$(CROSS)))

FPFLAGS  := -O2 -ffp-contract=off -fno-strict-aliasing
SIMD     := -DJPH_USE_SSE4_1 -DJPH_USE_SSE4_2 -msse4.2 -mpopcnt
CFLAGS   ?= $(FPFLAGS) -Wall
CXXFLAGS ?= $(FPFLAGS) -std=c++17 -DNDEBUG $(SIMD) -I$(JOLT) -w
ifeq ($(CROSS),)
  PIC    := -fPIC
  SO     := libhitreact.so
  EXE    :=
else
  PIC    :=
  SO     := hitreact.dll
  EXE    := .exe
endif

JOLT_SRC := $(shell cd $(JOLT) 2>/dev/null && find Jolt -name '*.cpp' | grep -v 'Compute/DX12\|Compute/VK\|Compute/MTL')
JOLT_OBJ := $(patsubst %.cpp,$(OUT)/jolt/%.o,$(JOLT_SRC))
HR_OBJ   := $(OUT)/hr_physics.o $(OUT)/hr_humanoid.o

all: $(OUT)/libhitreact.a $(OUT)/hr_demo$(EXE)
shared: $(OUT)/$(SO)

$(OUT)/jolt/%.o: $(JOLT)/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(PIC) -fvisibility=hidden -c $< -o $@

$(OUT)/hr_physics.o: hr_physics.cpp hitreact.h
	@mkdir -p $(OUT)
	$(CXX) $(CXXFLAGS) $(PIC) -c $< -o $@

$(OUT)/hr_humanoid.o: hr_humanoid.c hitreact.h
	@mkdir -p $(OUT)
	$(CC) $(CFLAGS) $(PIC) -c $< -o $@

$(OUT)/libhitreact.a: $(HR_OBJ) $(JOLT_OBJ)
	rm -f $@
	$(AR) rcs $@ $^

# (only the C API is exported: hitreact.def / hitreact.map, from hitreact.h)
$(OUT)/$(SO): $(HR_OBJ) $(JOLT_OBJ) hitreact.def hitreact.map
	$(CXX) -shared -o $@ $(HR_OBJ) $(JOLT_OBJ) $(if $(CROSS),hitreact.def -static -Wl$(comma)--out-implib$(comma)$(OUT)/libhitreact.dll.a,-Wl$(comma)--version-script=hitreact.map)

$(OUT)/hr_demo$(EXE): test/hr_demo.c $(OUT)/libhitreact.a
	$(CC) $(CFLAGS) -o $@ $< $(OUT)/libhitreact.a $(if $(CROSS),-static) -lstdc++ -lm -lpthread

comma := ,

demo-run: $(OUT)/hr_demo$(EXE)
	./$(OUT)/hr_demo$(EXE) inches $(OUT)/timeline-inches.txt $(OUT)/frames-inches.csv
	./$(OUT)/hr_demo$(EXE) metres $(OUT)/timeline-metres.txt $(OUT)/frames-metres.csv
	python3 tools/hr_compare.py $(OUT)/frames-inches.csv $(OUT)/frames-metres.csv

# the library's own checks (each prints PASS or FAIL)
test: $(OUT)/libhitreact.a
	$(CC) $(CFLAGS) -I. -o $(OUT)/hr_test_movers$(EXE) test/hr_test_movers.c $(OUT)/libhitreact.a $(if $(CROSS),-static) -lstdc++ -lm -lpthread
	$(CC) $(CFLAGS) -I. -o $(OUT)/hr_test_overlap$(EXE) test/hr_test_overlap.c $(OUT)/libhitreact.a $(if $(CROSS),-static) -lstdc++ -lm -lpthread
	./$(OUT)/hr_test_movers$(EXE)
	./$(OUT)/hr_test_overlap$(EXE)

clean:
	rm -rf $(OUT)

.PHONY: all shared demo-run test clean
