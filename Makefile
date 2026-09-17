CXX ?= c++
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra -Wpedantic

BUILD_DIR := build
SERVER := $(BUILD_DIR)/tcp_file_server
CLIENT := $(BUILD_DIR)/tcp_file_client

.PHONY: all test clean

all: $(SERVER) $(CLIENT)

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

$(SERVER): src/server.cpp | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) $< -o $@

$(CLIENT): src/client.cpp | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) $< -o $@

test: all
	bash tests/integration_test.sh $(SERVER) $(CLIENT)

clean:
	rm -rf $(BUILD_DIR)
