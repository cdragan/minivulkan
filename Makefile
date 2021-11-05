
CXXFLAGS += -Wall -Wextra -pedantic -fno-rtti -fno-exceptions

ifdef debug
CXXFLAGS += -Og
else
CXXFLAGS += -DNDEBUG -Os
endif

all: minivulkan minivulkan.S

run: minivulkan
	./minivulkan

minivulkan: minivulkan.cpp Makefile
	$(CXX) -o $@ $< $(CXXFLAGS) -lxcb -ldl -Wl,-s

minivulkan.S: minivulkan.cpp Makefile
	$(CXX) -S -masm=intel -o $@ $< $(CXXFLAGS)

clean:
	rm minivulkan minivulkan.S
