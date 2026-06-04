# Makefile for the Waveshare ESP32-S3-Touch-AMOLED-1.8 firmware.
#
# Quick start:
#   make build        # compile the AMOLED firmware
#   make flash        # build + upload to the board (verifies the write)
#   make monitor      # open the serial monitor
#   make flash-monitor# upload then immediately monitor
#
# Override anything on the command line, e.g.:
#   make flash PORT=/dev/cu.usbmodem1101

# PlatformIO was installed into an isolated env (not on PATH); fall back to a
# PATH `pio` if that location ever changes.
PIO  ?= $(firstword $(wildcard $(HOME)/.platformio/penv/bin/pio) pio)
ENV  ?= ws-amoled-18

# Serial device default depends on OS:
#   macOS → /dev/cu.usbmodem*   (the *call-up* node, what pio expects)
#   Linux → /dev/ttyACM0        (ESP32-S3 USB-CDC enumerates as ACM)
# Override with `make flash PORT=...` for a non-default device.
UNAME_S := $(shell uname -s)
ifeq ($(UNAME_S),Darwin)
  PORT ?= /dev/cu.usbmodem11401
else
  PORT ?= /dev/ttyACM0
endif
BAUD ?= 115200

# Where `make log` mirrors the serial stream. Override with `make log LOG=...`.
LOG  ?= $(HOME)/buddy-serial.log

.DEFAULT_GOAL := build

.PHONY: build flash upload monitor flash-monitor log clean fs-flash erase ports help char-usb

# Default pack for `make char-usb`; override: make char-usb CHAR=characters/bufo
CHAR ?= characters/claude-svg

## build: compile the firmware for $(ENV)
build:
	$(PIO) run -e $(ENV)

## flash: build + upload, and fail loudly if the write wasn't verified
flash:
	$(PIO) run -e $(ENV) -t upload --upload-port $(PORT)

# alias
upload: flash

## monitor: open the serial monitor on $(PORT)
monitor:
	$(PIO) device monitor -p $(PORT) -b $(BAUD)

## flash-monitor: upload then jump straight into the monitor
flash-monitor: flash monitor

## log: monitor AND mirror the serial stream to $(LOG) for later grepping
log:
	$(PIO) device monitor -p $(PORT) -b $(BAUD) | tee $(LOG)

## char-usb: push a character pack over USB-CDC (no BLE needed): make char-usb [CHAR=characters/...]
char-usb:
	tools/usb_xfer_send.sh $(CHAR) --port $(PORT)

## fs-flash: build + upload the LittleFS image (GIF character packs in data/)
fs-flash:
	$(PIO) run -e $(ENV) -t uploadfs --upload-port $(PORT)

## erase: wipe the whole flash (use before a clean reflash if things are weird)
erase:
	$(PIO) run -e $(ENV) -t erase --upload-port $(PORT)

## clean: remove build artifacts for $(ENV)
clean:
	$(PIO) run -e $(ENV) -t clean

## ports: list serial ports PlatformIO can see
ports:
	$(PIO) device list

## help: list targets
help:
	@grep -E '^## ' $(MAKEFILE_LIST) | sed 's/^## /  /'
	@echo
	@echo "Vars: ENV=$(ENV)  PORT=$(PORT)  BAUD=$(BAUD)"
	@echo "      PIO=$(PIO)"
