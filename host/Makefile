CXX ?= g++
CXXFLAGS ?= -std=gnu++17 -O2 -Wall -Wextra -Wno-unused-parameter
SRC = main.cpp sim.cpp web.cpp ../src/js.cpp ../src/js_builtins.cpp

# Use TFT_eSPI's 5x7 font for simulated text if PlatformIO has downloaded the library.
FONT = ../.pio/libdeps/cyd/TFT_eSPI/Fonts/glcdfont.c
ifneq ($(wildcard $(FONT)),)
  CXXFLAGS += -DSIM_FONT='"$(abspath $(FONT))"'
endif

jsh: $(SRC) ../src/js.h sim.h web.h
	$(CXX) $(CXXFLAGS) -o $@ $(SRC)

test_native: test_native.cpp ../src/js.cpp ../src/js_builtins.cpp ../src/js.h
	$(CXX) $(CXXFLAGS) -o $@ test_native.cpp ../src/js.cpp ../src/js_builtins.cpp

test: jsh test_native
	./jsh tests.js
	./test_native

clean:
	rm -f jsh test_native
