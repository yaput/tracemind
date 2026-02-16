# TraceMind - AI Root Cause Assistant
# Build System
# Optional: tree-sitter, libgit2 (for enhanced AST and git analysis)

CC := gcc
CFLAGS := -std=c11 -Wall -Wextra -Werror -pedantic -D_POSIX_C_SOURCE=200809L
CFLAGS += -Iinclude -Isrc

# macOS homebrew paths
UNAME_S := $(shell uname -s)
ifeq ($(UNAME_S),Darwin)
    BREW_PREFIX := $(shell brew --prefix 2>/dev/null)
    ifneq ($(BREW_PREFIX),)
        CFLAGS += -I$(BREW_PREFIX)/include
        LDFLAGS := -L$(BREW_PREFIX)/lib
    endif
endif

LDFLAGS += -lcurl -ljansson

# Optional dependencies - define HAVE_TREE_SITTER and/or HAVE_LIBGIT2
# to enable these features if libraries are installed
ifdef HAVE_TREE_SITTER
CFLAGS += -DHAVE_TREE_SITTER
LDFLAGS += -ltree-sitter -ltree-sitter-python -ltree-sitter-go -ltree-sitter-javascript
endif

ifdef HAVE_LIBGIT2
CFLAGS += -DHAVE_LIBGIT2
LDFLAGS += -lgit2
endif

# Debug/Release configurations
DEBUG_FLAGS := -g -O0 -DDEBUG -fsanitize=address,undefined
RELEASE_FLAGS := -O3 -DNDEBUG -march=native -flto

# Directories
SRC_DIR := src
INC_DIR := include
OBJ_DIR := build/obj
BIN_DIR := build/bin
TEST_DIR := tests

# Source files
SRCS := $(wildcard $(SRC_DIR)/*.c) $(wildcard $(SRC_DIR)/**/*.c)
OBJS := $(SRCS:$(SRC_DIR)/%.c=$(OBJ_DIR)/%.o)
DEPS := $(OBJS:.o=.d)

# Main target
TARGET := $(BIN_DIR)/tracemind

# Test sources
TEST_SRCS := $(wildcard $(TEST_DIR)/*.c)
TEST_BINS := $(TEST_SRCS:$(TEST_DIR)/%.c=$(BIN_DIR)/test_%)

.PHONY: all debug release clean test install format check

all: release

debug: CFLAGS += $(DEBUG_FLAGS)
debug: $(TARGET)

release: CFLAGS += $(RELEASE_FLAGS)
release: $(TARGET)

$(TARGET): $(OBJS) | $(BIN_DIR)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)
	@echo "Built: $@"

$(OBJ_DIR)/%.o: $(SRC_DIR)/%.c | $(OBJ_DIR)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BIN_DIR) $(OBJ_DIR):
	@mkdir -p $@

# Include dependencies
-include $(DEPS)

# Tests
test: debug $(TEST_BINS)
	@for t in $(TEST_BINS); do echo "Running $$t..."; $$t || exit 1; done
	@echo "All tests passed!"

$(BIN_DIR)/test_%: $(TEST_DIR)/%.c $(filter-out $(OBJ_DIR)/main.o,$(OBJS)) | $(BIN_DIR)
	$(CC) $(CFLAGS) $(DEBUG_FLAGS) -o $@ $^ $(LDFLAGS)

# Code quality
format:
	@find $(SRC_DIR) $(INC_DIR) $(TEST_DIR) -name "*.c" -o -name "*.h" | xargs clang-format -i

check:
	@cppcheck --enable=all --inconclusive --std=c11 -I$(INC_DIR) $(SRC_DIR)
	@echo "Static analysis complete."

# Install
PREFIX ?= /usr/local
install: release
	install -d $(PREFIX)/bin
	install -m 755 $(TARGET) $(PREFIX)/bin/tracemind
	@echo "Installed to $(PREFIX)/bin/tracemind"

clean:
	rm -rf build

# Development helpers
.PHONY: deps-mac deps-linux

deps-mac:
	brew install tree-sitter libgit2 curl jansson cppcheck

deps-linux:
	sudo apt-get install -y libtree-sitter-dev libgit2-dev libcurl4-openssl-dev libjansson-dev cppcheck
