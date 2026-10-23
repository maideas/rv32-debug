# rv32-debug — reference C++ implementation of docs/debug-sequences.md.
# Builds the library sources plus the mock-target demo into build/.

CXX ?= g++
CXXFLAGS ?= -std=c++17 -Wall -Wextra -O2 -g
BUILD := build
TARGET := $(BUILD)/rvdebug-demo

SRCS := src/dtm.cpp src/sequences.cpp src/mock/mock_target.cpp src/demo.cpp
HDRS := src/jtag_transport.hpp src/dtm.hpp src/sequences.hpp \
        src/mock/mock_target.hpp

all: $(TARGET)

$(TARGET): $(SRCS) $(HDRS)
	$(CXX) $(CXXFLAGS) -Isrc -o $@ $(SRCS)

test: $(TARGET)
	./$(TARGET)

clean:
	rm -f $(TARGET)

.PHONY: all test clean
