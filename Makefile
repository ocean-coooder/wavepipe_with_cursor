CC ?= gcc
CFLAGS ?= -O2 -Wall -Wextra -std=c11
LDFLAGS ?=
LDLIBS ?= -lasound

SRC := src/alsa_tool.c
DEPS_CHECK_SRC := src/deps_check.c
DEPS_CHECK_BIN := bin/deps_check
BIN_DIR := bin
TARGET := $(BIN_DIR)/alsa_tool

.PHONY: all clean

all: $(TARGET)

$(TARGET): $(SRC) $(DEPS_CHECK_SRC)
	mkdir -p $(BIN_DIR)
	$(CC) $(CFLAGS) $(DEPS_CHECK_SRC) -o $(DEPS_CHECK_BIN)
	@$(DEPS_CHECK_BIN); \
	status=$$?; \
	if [ $$status -eq 0 ]; then \
	  echo "Deps check: ALSA header OK."; \
	else \
	  echo "Deps check: ALSA header missing. Build halted."; \
	  echo "You can still build on Raspberry Pi after: sudo apt install -y libasound2-dev"; \
	  exit $$status; \
	fi
	$(CC) $(CFLAGS) $(SRC) -o $(TARGET) $(LDFLAGS) $(LDLIBS)

clean:
	rm -rf $(BIN_DIR)
