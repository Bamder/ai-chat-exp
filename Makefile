CXX      ?= g++
CXXFLAGS ?= -std=gnu++17 -O2 -Wall -Wextra
CXXFLAGS += -pthread
CPPFLAGS += -Iinclude -I.

BIN      := ai-chat
BUILD    := build
SRC      := $(shell find src -name '*.cpp' | sort)
TOOL_SRC := $(wildcard ai-tools/*/*.cpp)
OBJ      := $(patsubst src/%.cpp,$(BUILD)/%.o,$(SRC)) \
            $(patsubst ai-tools/%.cpp,$(BUILD)/ai-tools/%.o,$(TOOL_SRC))
DEP      := $(OBJ:.o=.d)

all: $(BIN)

$(BIN): $(OBJ)
	$(CXX) $(CXXFLAGS) -o $@ $^

$(BUILD)/%.o: src/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -MMD -MP -c -o $@ $<

$(BUILD)/ai-tools/%.o: ai-tools/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -MMD -MP -c -o $@ $<

-include $(DEP)

test: $(BIN)
	./$(BIN) --selftest

clean:
	rm -rf $(BUILD) $(BIN)

.PHONY: all test clean
