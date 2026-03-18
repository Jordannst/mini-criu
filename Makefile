CC ?= gcc
CFLAGS ?= -Wall -Wextra -Wpedantic -std=c11 -g
CPPFLAGS ?= -D_POSIX_C_SOURCE=200809L -Iinclude

BUILD_DIR := build
APP := $(BUILD_DIR)/mini-criu

APP_SOURCES := \
	src/main.c \
	src/cli.c \
	src/freeze.c \
	src/restore.c \
	src/memory_dump.c \
	src/utils.c

APP_OBJECTS := $(patsubst %.c,$(BUILD_DIR)/%.o,$(APP_SOURCES))

TARGET_SOURCES := \
	targets/cpu_bound_target.c \
	targets/memory_bound_target.c

TARGET_BINS := $(patsubst targets/%.c,$(BUILD_DIR)/targets/%,$(TARGET_SOURCES))

.PHONY: all clean run

all: $(APP) $(TARGET_BINS)

$(APP): $(APP_OBJECTS)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $^ -o $@

$(BUILD_DIR)/targets/%: targets/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $< -o $@

$(BUILD_DIR)/%.o: %.c include/mini_criu.h
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

run: all
	./$(APP)

clean:
	rm -rf $(BUILD_DIR)
