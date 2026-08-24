PROJECT := pcm-mixer
BUILD_DIR ?= build
PREFIX ?= /usr/local
DESTDIR ?=

CC ?= cc
AR ?= ar
INSTALL ?= install

CPPFLAGS += -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE -Iinclude
WARNINGS := \
	-Wall -Wextra -Wpedantic -Wconversion -Wshadow \
	-Wstrict-prototypes -Wmissing-prototypes -Wformat=2 -Werror
CFLAGS ?= -O2 -g
override CFLAGS += -std=c11 -fPIC -pthread $(WARNINGS)
DEPFLAGS := -MMD -MP
LDLIBS := -lm -pthread

LIB_OBJS := \
	$(BUILD_DIR)/pcm_mixer.o \
	$(BUILD_DIR)/pcm_wav.o \
	$(BUILD_DIR)/pcmmix_bank.o
DEPS := $(LIB_OBJS:.o=.d)
STATIC_LIB := $(BUILD_DIR)/lib$(PROJECT).a
SHARED_LIB := $(BUILD_DIR)/lib$(PROJECT).so
TEST_BIN := $(BUILD_DIR)/test-mixer
BANK_TEST_BIN := $(BUILD_DIR)/test-bank
EXAMPLE_BIN := $(BUILD_DIR)/beep
MIXER_BENCHMARK_BIN := $(BUILD_DIR)/benchmark-mixer
WAV_BENCHMARK_BIN := $(BUILD_DIR)/benchmark-wav
SANITIZE_BUILD_DIR ?= $(BUILD_DIR)/sanitize

.PHONY: all benchmark clean install sanitize test tsanitize

all: $(STATIC_LIB) $(SHARED_LIB) $(EXAMPLE_BIN)

$(BUILD_DIR):
	mkdir -p $@

$(BUILD_DIR)/pcm_mixer.o: src/pcm_mixer.c include/pcm_mixer.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(DEPFLAGS) -c $< -o $@

$(BUILD_DIR)/pcm_wav.o: src/pcm_wav.c include/pcm_mixer.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(DEPFLAGS) -c $< -o $@

$(BUILD_DIR)/pcmmix_bank.o: src/pcmmix_bank.c include/pcmmix_bank.h include/pcm_mixer.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(DEPFLAGS) -c $< -o $@

$(STATIC_LIB): $(LIB_OBJS)
	$(AR) rcs $@ $^

$(SHARED_LIB): $(LIB_OBJS)
	$(CC) -shared $(LDFLAGS) $^ $(LDLIBS) -o $@

$(TEST_BIN): tests/test_mixer.c $(STATIC_LIB) | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) $< $(STATIC_LIB) $(LDFLAGS) $(LDLIBS) -o $@

$(BANK_TEST_BIN): tests/test_bank.c $(STATIC_LIB) | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) $< $(STATIC_LIB) $(LDFLAGS) $(LDLIBS) -o $@

$(EXAMPLE_BIN): examples/beep.c $(STATIC_LIB) | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) $< $(STATIC_LIB) $(LDFLAGS) $(LDLIBS) -o $@

$(MIXER_BENCHMARK_BIN): benchmarks/benchmark_mixer.c $(STATIC_LIB) | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) $< $(STATIC_LIB) $(LDFLAGS) $(LDLIBS) -o $@

$(WAV_BENCHMARK_BIN): benchmarks/benchmark_wav.c $(STATIC_LIB) | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) $< $(STATIC_LIB) $(LDFLAGS) $(LDLIBS) -o $@

test: $(TEST_BIN) $(BANK_TEST_BIN)
	$(TEST_BIN)
	$(BANK_TEST_BIN)

benchmark: $(MIXER_BENCHMARK_BIN) $(WAV_BENCHMARK_BIN)
	$(MIXER_BENCHMARK_BIN)
	$(WAV_BENCHMARK_BIN)

sanitize:
	ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 \
	$(MAKE) BUILD_DIR=$(SANITIZE_BUILD_DIR) \
		CFLAGS='-O1 -g3 -fsanitize=address,undefined \
		-fno-sanitize-recover=undefined -fno-omit-frame-pointer' test

tsanitize:
	$(MAKE) BUILD_DIR=$(BUILD_DIR)/tsanitize \
		CFLAGS='-O1 -g3 -fsanitize=thread -fno-omit-frame-pointer' \
		LDFLAGS='-fsanitize=thread' test

install: all
	$(INSTALL) -d $(DESTDIR)$(PREFIX)/include $(DESTDIR)$(PREFIX)/lib
	$(INSTALL) -m 0644 include/pcm_mixer.h include/pcmmix_bank.h \
		$(DESTDIR)$(PREFIX)/include/
	$(INSTALL) -m 0644 $(STATIC_LIB) $(SHARED_LIB) $(DESTDIR)$(PREFIX)/lib/

clean:
	rm -rf $(BUILD_DIR)

-include $(DEPS)
