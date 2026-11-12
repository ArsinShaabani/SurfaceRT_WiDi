# SurfaceRT-WiDi - portable build for the Linux test harness and dev tools.
# The Windows RT binaries are built with MSBuild (see docs/BUILD-RT.md).
CXX      ?= g++
CXXFLAGS ?= -std=c++17 -O2 -g -Wall -Wextra -Wpedantic -Iinclude
OUT      := out

CORE_SRC := \
    src/core/protocol.cpp \
    src/core/session.cpp \
    src/core/rate_control.cpp \
    src/core/beacon.cpp \
    src/core/sink_engine.cpp \
    src/core/log.cpp \
    src/core/net_posix.cpp

TEST_BIN   := $(OUT)/widi_tests
SINK_BIN   := $(OUT)/widi-sink-linux
SENDER_BIN := $(OUT)/widi-sender-linux

.PHONY: all test clean check

all: $(TEST_BIN) $(SINK_BIN) $(SENDER_BIN)

$(OUT):
	@mkdir -p $(OUT)

$(TEST_BIN): tests/test_main.cpp $(CORE_SRC) | $(OUT)
	$(CXX) $(CXXFLAGS) -pthread tests/test_main.cpp $(CORE_SRC) -o $@

$(SINK_BIN): src/apps/console/sink_linux.cpp $(CORE_SRC) | $(OUT)
	$(CXX) $(CXXFLAGS) -pthread src/apps/console/sink_linux.cpp $(CORE_SRC) -o $@

$(SENDER_BIN): src/apps/console/sender_linux.cpp $(CORE_SRC) | $(OUT)
	$(CXX) $(CXXFLAGS) -pthread src/apps/console/sender_linux.cpp $(CORE_SRC) -o $@

test: $(TEST_BIN)
	./$(TEST_BIN)

check: test

clean:
	rm -rf $(OUT)
