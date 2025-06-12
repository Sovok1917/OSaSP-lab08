# Compiler and flags
CC = gcc
STD_C11 = -std=c11
PEDANTIC = -pedantic
WARNINGS_COMMON = -W -Wall -Wextra
CFLAGS_DEBUG_MODE = -g -ggdb $(STD_C11) $(PEDANTIC) $(WARNINGS_COMMON)
CFLAGS_RELEASE_MODE = $(STD_C11) $(PEDANTIC) $(WARNINGS_COMMON) -Werror -O2

# Default mode
MODE ?= debug

# Determine CFLAGS based on MODE
ifeq ($(MODE),debug)
    CURRENT_CFLAGS = $(CFLAGS_DEBUG_MODE)
else ifeq ($(MODE),release)
    CURRENT_CFLAGS = $(CFLAGS_RELEASE_MODE)
else
    $(error Invalid MODE: $(MODE). Use 'debug' or 'release'.)
endif

LDFLAGS = -pthread # For pthread_create, etc.
LDLIBS = # No special libs needed beyond standard and pthreads for now

# Directories
SRC_DIR = src
BUILD_DIR = build
INC_DIR = -I$(SRC_DIR)

# Mode flag file to detect mode changes
MODE_FLAG_FILE = $(BUILD_DIR)/.mode_$(MODE)

# Source files and object files
COMMON_SRCS = $(SRC_DIR)/common.c
COMMON_OBJS = $(patsubst $(SRC_DIR)/%.c,$(BUILD_DIR)/%.o,$(COMMON_SRCS))

SERVER_SRCS = $(SRC_DIR)/server.c $(COMMON_SRCS)
SERVER_OBJS = $(patsubst $(SRC_DIR)/%.c,$(BUILD_DIR)/%.o,$(SERVER_SRCS))
SERVER_EXEC = myserver

CLIENT_SRCS = $(SRC_DIR)/client.c $(COMMON_SRCS)
CLIENT_OBJS = $(patsubst $(SRC_DIR)/%.c,$(BUILD_DIR)/%.o,$(CLIENT_SRCS))
CLIENT_EXEC = myclient

# Targets
.PHONY: all clean server client force_clean

# The main 'all' target
all: $(MODE_FLAG_FILE) $(BUILD_DIR)/$(SERVER_EXEC) $(BUILD_DIR)/$(CLIENT_EXEC)

# Rule to handle mode changes: if the mode flag file for the *current* mode
# doesn't exist, it means either it's a fresh build or the mode changed.
# In case of mode change, other .mode_* files might exist.
$(MODE_FLAG_FILE):
	@# Check if any other mode flag exists, indicating a mode switch
	@if ls $(BUILD_DIR)/.mode_* 1> /dev/null 2>&1 && [ ! -f $@ ]; then \
		echo "Build mode changed to $(MODE). Cleaning previous build artifacts..."; \
		$(MAKE) force_clean; \
	fi
	@mkdir -p $(BUILD_DIR)
	@touch $@ # Create/update the mode flag file

$(BUILD_DIR)/$(SERVER_EXEC): $(SERVER_OBJS)
	@mkdir -p $(@D)
	$(CC) $(CURRENT_CFLAGS) $^ -o $@ $(LDFLAGS) $(LDLIBS)
	@echo "Built Server ($@) in $(MODE) mode"

$(BUILD_DIR)/$(CLIENT_EXEC): $(CLIENT_OBJS)
	@mkdir -p $(@D)
	$(CC) $(CURRENT_CFLAGS) $^ -o $@ $(LDFLAGS) $(LDLIBS)
	@echo "Built Client ($@) in $(MODE) mode"

$(BUILD_DIR)/%.o: $(SRC_DIR)/%.c
	@mkdir -p $(@D)
	$(CC) $(CURRENT_CFLAGS) $(INC_DIR) -c $< -o $@

server: $(MODE_FLAG_FILE) $(BUILD_DIR)/$(SERVER_EXEC)
client: $(MODE_FLAG_FILE) $(BUILD_DIR)/$(CLIENT_EXEC)

# 'clean' target removes all build artifacts including mode flags
clean:
	@echo "Cleaning all build artifacts..."
	rm -rf $(BUILD_DIR)
	@echo "Clean complete."

# 'force_clean' is used internally by the mode change detection
force_clean:
	@echo "Forcing clean of build directory..."
	rm -rf $(BUILD_DIR)/*
	@# We don't remove the BUILD_DIR itself here, just its contents
	@# because mkdir -p $(BUILD_DIR) will be called again.
	@echo "Forced clean complete."
