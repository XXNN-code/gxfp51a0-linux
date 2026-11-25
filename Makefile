CC ?= cc
CFLAGS ?= -O2 -g
CFLAGS += -std=c11 -Wall -Wextra -Wpedantic -Werror -Iinclude
LDFLAGS ?=

BUILD := build
COMMON := src/transport.c src/protocol.c src/target.c src/config_base.c \
	  src/pmk.c src/tls.c src/image.c src/sha256.c src/aes128.c
# Capture sequence shared with the libfprint driver (see src/session.c).
SESSION := src/session.c $(COMMON)
CRYPTO := src/sha256.c src/aes128.c src/pmk.c

.PHONY: all test clean

all: $(BUILD)/gx51-probe

$(BUILD):
	mkdir -p $(BUILD)

$(BUILD)/gx51-probe: src/probe.c $(SESSION) include/gx51.h include/crypto.h include/tls.h include/image.h include/session.h | $(BUILD)
	$(CC) $(CFLAGS) -o $@ src/probe.c $(SESSION) $(LDFLAGS) -lssl -lcrypto

$(BUILD)/test-protocol: tests/test_protocol.c src/protocol.c include/gx51.h | $(BUILD)
	$(CC) $(CFLAGS) -o $@ tests/test_protocol.c src/protocol.c $(LDFLAGS)

$(BUILD)/test-cal: tests/test_cal.c src/target.c src/config_base.c src/protocol.c include/gx51.h | $(BUILD)
	$(CC) $(CFLAGS) -o $@ tests/test_cal.c src/target.c src/config_base.c src/protocol.c $(LDFLAGS)

$(BUILD)/test-pmk: tests/test_pmk.c $(CRYPTO) src/transport.c src/protocol.c include/gx51.h include/crypto.h | $(BUILD)
	$(CC) $(CFLAGS) -o $@ tests/test_pmk.c $(CRYPTO) src/transport.c src/protocol.c $(LDFLAGS)

$(BUILD)/test-session: tests/test_session.c $(SESSION) include/gx51.h include/session.h | $(BUILD)
	$(CC) $(CFLAGS) -o $@ tests/test_session.c $(SESSION) $(LDFLAGS) -lssl -lcrypto

# Offline matcher experiments; they read captured PGMs only, no hardware.
$(BUILD)/match-corr: tools/match-corr.c | $(BUILD)
	$(CC) $(CFLAGS) -o $@ tools/match-corr.c $(LDFLAGS) -lm

$(BUILD)/test-image: tests/test_image.c src/image.c include/image.h | $(BUILD)
	$(CC) $(CFLAGS) -o $@ tests/test_image.c src/image.c $(LDFLAGS)

test: $(BUILD)/test-protocol $(BUILD)/test-cal $(BUILD)/test-pmk $(BUILD)/test-image $(BUILD)/test-session
	./$(BUILD)/test-protocol
	./$(BUILD)/test-cal
	./$(BUILD)/test-pmk
	./$(BUILD)/test-image
	./$(BUILD)/test-session

clean:
	rm -rf $(BUILD)
