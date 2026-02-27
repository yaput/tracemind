# TraceMind - AI Root Cause Assistant
# Build System — auto-detects optional dependencies

CC := gcc
CFLAGS := -std=c11 -Wall -Wextra -Werror -pedantic -D_POSIX_C_SOURCE=200809L
CFLAGS += -Iinclude -Isrc

# Platform detection
UNAME_S := $(shell uname -s)
ifeq ($(UNAME_S),Darwin)
    BREW_PREFIX := $(shell brew --prefix 2>/dev/null)
    ifneq ($(BREW_PREFIX),)
        CFLAGS += -I$(BREW_PREFIX)/include
        LDFLAGS := -L$(BREW_PREFIX)/lib
    endif
endif

LDFLAGS += -lcurl -ljansson

# Auto-detect optional dependencies (override with HAVE_TREE_SITTER=0 / HAVE_LIBGIT2=0 to disable)
ifndef HAVE_TREE_SITTER
    HAVE_TREE_SITTER := $(shell pkg-config --exists tree-sitter 2>/dev/null && echo 1 || \
        ([ -f "$(BREW_PREFIX)/lib/libtree-sitter.a" ] 2>/dev/null && echo 1 || echo 0))
endif
ifndef HAVE_LIBGIT2
    HAVE_LIBGIT2 := $(shell pkg-config --exists libgit2 2>/dev/null && echo 1 || \
        ([ -f "$(BREW_PREFIX)/lib/libgit2.dylib" ] 2>/dev/null && echo 1 || echo 0))
endif

ifeq ($(HAVE_TREE_SITTER),1)
CFLAGS += -DHAVE_TREE_SITTER
LDFLAGS += -ltree-sitter -ltree-sitter-python -ltree-sitter-go -ltree-sitter-javascript
endif

ifeq ($(HAVE_LIBGIT2),1)
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

.PHONY: all debug release clean test install format check help info deps-mac deps-linux

all: release

help:  ## Show this help
	@echo "TraceMind Build System"
	@echo "====================="
	@echo ""
	@echo "Targets:"
	@echo "  make              Build release binary"
	@echo "  make debug        Build with sanitizers & debug info"
	@echo "  make test         Build and run tests"
	@echo "  make install      Install to $(PREFIX)/bin"
	@echo "  make clean        Remove build artifacts"
	@echo "  make info         Show detected features"
	@echo "  make deps-mac     Install dependencies (macOS)"
	@echo "  make deps-linux   Install dependencies (Linux)"
	@echo "  make format       Run clang-format on sources"
	@echo "  make check        Run cppcheck static analysis"
	@echo ""
	@echo "Options:"
	@echo "  HAVE_TREE_SITTER=0  Disable tree-sitter (auto-detected)"
	@echo "  HAVE_LIBGIT2=0      Disable libgit2 (auto-detected)"
	@echo "  PREFIX=/usr/local   Install prefix"

info:  ## Show detected build features
	@echo "Platform:      $(UNAME_S)"
	@echo "Compiler:      $(CC)"
	@echo "tree-sitter:   $(if $(filter 1,$(HAVE_TREE_SITTER)),YES,NO)"
	@echo "libgit2:       $(if $(filter 1,$(HAVE_LIBGIT2)),YES,NO)"
	@echo "CFLAGS:        $(CFLAGS)"
	@echo "LDFLAGS:       $(LDFLAGS)"

debug: CFLAGS += $(DEBUG_FLAGS)
debug: $(TARGET)

release: CFLAGS += $(RELEASE_FLAGS)
release: $(TARGET)

$(TARGET): $(OBJS) | $(BIN_DIR)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)
	@echo "✓ Built: $@"
	@echo "  tree-sitter: $(if $(filter 1,$(HAVE_TREE_SITTER)),enabled,disabled)"
	@echo "  libgit2:     $(if $(filter 1,$(HAVE_LIBGIT2)),enabled,disabled)"

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

uninstall:
	rm -f $(PREFIX)/bin/tracemind

clean:
	rm -rf build

# Dependency installation
deps-mac:
	brew install curl jansson libgit2 tree-sitter cppcheck

deps-linux:
	sudo apt-get install -y libcurl4-openssl-dev libjansson-dev libgit2-dev libtree-sitter-dev cppcheck
