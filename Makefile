CXX      := g++
CXXFLAGS := -std=c++17 -O3 -Wall -Wextra -Wpedantic -march=native -ffast-math
LDFLAGS  := -pthread

SRC_DIR   := src
BUILD_DIR := build
BIN_DIR   := bin

TARGET := $(BIN_DIR)/capacity_estimator
SRCS   := $(wildcard $(SRC_DIR)/*.cpp)
OBJS   := $(patsubst $(SRC_DIR)/%.cpp, $(BUILD_DIR)/%.o, $(SRCS))

.PHONY: all clean dirs

all: dirs $(TARGET)

dirs:
	@mkdir -p $(BUILD_DIR)
	@mkdir -p $(BIN_DIR)

$(TARGET): $(OBJS)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDFLAGS)
	@echo "Build successful: $(TARGET)"

$(BUILD_DIR)/%.o: $(SRC_DIR)/%.cpp
	$(CXX) $(CXXFLAGS) -c $< -o $@

clean:
	@rm -rf $(BUILD_DIR) $(BIN_DIR)
	@echo "Cleaned build artifacts."
