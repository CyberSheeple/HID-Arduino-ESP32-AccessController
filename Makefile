# Headless build and flash for the Nano and ESP32 door-controller sketches.
#   make help        list targets and the variables they take
#   make bootstrap   install arduino-cli (if needed), cores, and libraries
#   make compile     build both sketches
#   make flash       upload the Nano, the ESP32 app, and the LittleFS image

CONFIG := arduino-cli.yaml
PYTHON ?= python3

NANO_CPU ?= atmega328old
NANO_FQBN := arduino:avr:nano:cpu=$(NANO_CPU)
ESP_FQBN := esp32:esp32:esp32:UploadSpeed=921600,FlashSize=4M,PartitionScheme=default
ESP_BAUD ?= 921600

NANO_SKETCH := Arduino nano Code/HIDR10-RP15-Door-Simulator
ESP_SKETCH := ESP32_Code
ESP_DATA := ESP32_Code/data
BUILD_NANO := build/nano
BUILD_ESP := build/esp32
SCRIPTS := scripts

PROXMARK_REPO := https://github.com/RfidResearchGroup/proxmark3.git
PM3_SRC := $(CURDIR)/.tools/proxmark3
PM3 ?= $(PM3_SRC)/pm3
TAG ?= lf
SIM_SECONDS ?= 8
PROXMARK_PACKAGES := git ca-certificates build-essential pkg-config \
	libreadline-dev gcc-arm-none-eabi libnewlib-dev \
	libbz2-dev liblz4-dev libpython3-dev libssl-dev

ARDUINO_CLI_ON_PATH := $(shell command -v arduino-cli 2>/dev/null)
ifeq ($(ARDUINO_CLI_ON_PATH),)
  ARDUINO_CLI := $(CURDIR)/.tools/arduino-cli
else
  ARDUINO_CLI := $(ARDUINO_CLI_ON_PATH)
endif

ifeq ($(SKIP_FS),1)
FLASH_FS_AFTER := true
else
FLASH_FS_AFTER := $(PYTHON) $(SCRIPTS)/flash_littlefs.py --config $(CONFIG) --cli "$(ARDUINO_CLI)" --fqbn "$(ESP_FQBN)" --build-dir $(BUILD_ESP) --data "$(ESP_DATA)" --baud $(ESP_BAUD) --port "$$port"
endif

.DEFAULT_GOAL := compile

.PHONY: help bootstrap compile compile-nano compile-esp32 list \
	flash flash-nano flash-esp32 flash-fs \
	proxmark-install proxmark-flash proxmark-provision-admin proxmark-test-reader \
	proxmark-write-admin proxmark-write-cards proxmark-write-modes

help:
	@echo 'Build and flash'
	@echo '  make help                      list targets'
	@echo '  make bootstrap                 install arduino-cli, cores, and libraries'
	@echo '  make compile                   build the Nano and ESP32 sketches'
	@echo '  make list                      show serial ports'
	@echo '  make flash                     upload the Nano, the ESP32 app, and LittleFS'
	@echo '  make flash-nano                upload the Nano only'
	@echo '  make flash-esp32               upload the ESP32 sketch and LittleFS'
	@echo '  make flash-fs                  rewrite LittleFS from ESP32_Code/data'
	@echo ''
	@echo 'Proxmark'
	@echo '  make proxmark-install          build the Iceman client into .tools/proxmark3'
	@echo '  make proxmark-flash            flash Iceman firmware to the Proxmark'
	@echo '  make proxmark-provision-admin  simulate the admin card at the HID reader'
	@echo '  make proxmark-test-reader      simulate a door card and check ESP32 serial'
	@echo '  make proxmark-write-admin      write the admin id onto one tag'
	@echo '  make proxmark-write-cards      write each cards.txt id onto its own tag'
	@echo '  make proxmark-write-modes      write the four mode cards onto tags'
	@echo ''
	@echo 'Variables'
	@echo '  NANO_CPU=$(NANO_CPU)   Nano bootloader (atmega328 for an official board)'
	@echo '  NANO_PORT              serial port for the Nano, when detection is ambiguous'
	@echo '  ESP_PORT               serial port for the ESP32, when detection is ambiguous'
	@echo '  SKIP_FS=1              skip LittleFS on flash and flash-esp32'
	@echo '  PM3=$(PM3)'
	@echo '  TAG=$(TAG)             lf (T5577) or hf (magic 1K) for the write targets'
	@echo '  SIM_SECONDS=$(SIM_SECONDS)         seconds to simulate a card'

# Download the official CLI into .tools/ only when it is not already on PATH.
$(CURDIR)/.tools/arduino-cli:
	mkdir -p .tools
	curl -fsSL https://raw.githubusercontent.com/arduino/arduino-cli/master/install.sh | BINDIR="$(CURDIR)/.tools" sh
	test -x "$@"

bootstrap: $(if $(ARDUINO_CLI_ON_PATH),,$(CURDIR)/.tools/arduino-cli)
	"$(ARDUINO_CLI)" --config-file $(CONFIG) core update-index
	"$(ARDUINO_CLI)" --config-file $(CONFIG) core install arduino:avr
	"$(ARDUINO_CLI)" --config-file $(CONFIG) core install esp32:esp32
	"$(ARDUINO_CLI)" --config-file $(CONFIG) lib install "Adafruit SSD1306" "Adafruit GFX Library"
	"$(ARDUINO_CLI)" --config-file $(CONFIG) lib install --git-url https://github.com/monkeyboard/Wiegand-Protocol-Library-for-Arduino
	mkdir -p .arduino
	touch .arduino/.bootstrapped

.arduino/.bootstrapped: arduino-cli.yaml
	$(MAKE) bootstrap

compile: compile-nano compile-esp32

compile-nano: .arduino/.bootstrapped
	"$(ARDUINO_CLI)" --config-file $(CONFIG) compile --fqbn "$(NANO_FQBN)" --build-path $(BUILD_NANO) "$(NANO_SKETCH)"

compile-esp32: .arduino/.bootstrapped
	"$(ARDUINO_CLI)" --config-file $(CONFIG) compile --fqbn "$(ESP_FQBN)" --build-path $(BUILD_ESP) "$(ESP_SKETCH)"

list: .arduino/.bootstrapped
	"$(ARDUINO_CLI)" --config-file $(CONFIG) board list

flash: flash-nano flash-esp32

flash-nano: compile-nano
	port=$$($(PYTHON) $(SCRIPTS)/resolve_ports.py --config $(CONFIG) --cli "$(ARDUINO_CLI)" --need nano --print); \
	test -n "$$port" || exit 1; \
	"$(ARDUINO_CLI)" --config-file $(CONFIG) upload -p "$$port" --fqbn "$(NANO_FQBN)" --input-dir $(BUILD_NANO)

flash-esp32: compile-esp32
	port=$$($(PYTHON) $(SCRIPTS)/resolve_ports.py --config $(CONFIG) --cli "$(ARDUINO_CLI)" --need esp --print); \
	test -n "$$port" || exit 1; \
	"$(ARDUINO_CLI)" --config-file $(CONFIG) upload -p "$$port" --fqbn "$(ESP_FQBN)" --input-dir $(BUILD_ESP); \
	$(FLASH_FS_AFTER)

flash-fs: compile-esp32
	port=$$($(PYTHON) $(SCRIPTS)/resolve_ports.py --config $(CONFIG) --cli "$(ARDUINO_CLI)" --need esp --print); \
	test -n "$$port" || exit 1; \
	$(PYTHON) $(SCRIPTS)/flash_littlefs.py --config $(CONFIG) --cli "$(ARDUINO_CLI)" --fqbn "$(ESP_FQBN)" --build-dir $(BUILD_ESP) --data "$(ESP_DATA)" --baud $(ESP_BAUD) --port "$$port"

# Iceman client lives in .tools/proxmark3. PM3 may point at another pm3 binary.
define REQUIRE_PM3
	@if [ ! -e "$(PM3)" ] && ! command -v "$(PM3)" >/dev/null 2>&1; then \
		echo "Proxmark client not found at $(PM3). Run make proxmark-install or set PM3." >&2; \
		exit 1; \
	fi
endef

proxmark-install:
	sudo apt-get update
	sudo apt-get install --no-install-recommends -y $(PROXMARK_PACKAGES)
	@if command -v systemctl >/dev/null 2>&1 && systemctl is-active --quiet ModemManager; then \
		echo "ModemManager is running. It can grab the Proxmark serial port during flash. Stop it before make proxmark-flash if flashing fails."; \
	fi
	if [ ! -d "$(PM3_SRC)/.git" ]; then \
		rm -rf "$(PM3_SRC)"; \
		mkdir -p .tools; \
		git clone --depth 1 "$(PROXMARK_REPO)" "$(PM3_SRC)"; \
	fi
	$(MAKE) -C "$(PM3_SRC)" -j"$(shell nproc)"
	$(MAKE) -C "$(PM3_SRC)" accessrights
	@echo "If the Proxmark device node is not writable, open a new shell so the group from 'make accessrights' applies."
	@if ls /dev/ttyACM* >/dev/null 2>&1; then \
		$(MAKE) proxmark-flash; \
	else \
		echo "No /dev/ttyACM* device. Plug in the Proxmark and run make proxmark-flash."; \
	fi

proxmark-flash:
	@test -x "$(PM3_SRC)/pm3-flash-all" || { echo "Run make proxmark-install first." >&2; exit 1; }
	"$(PM3_SRC)/pm3-flash-all"

proxmark-provision-admin:
	$(REQUIRE_PM3)
	$(PYTHON) $(SCRIPTS)/proxmark_reader.py provision-admin --pm3 "$(PM3)" --admin "$(ESP_DATA)/admin.txt" --seconds "$(SIM_SECONDS)"

proxmark-test-reader:
	$(REQUIRE_PM3)
	$(PYTHON) $(SCRIPTS)/proxmark_reader.py test-reader --pm3 "$(PM3)" --admin "$(ESP_DATA)/admin.txt" --cards "$(ESP_DATA)/cards.txt" --seconds "$(SIM_SECONDS)" --config "$(CONFIG)" --cli "$(ARDUINO_CLI)"

proxmark-write-admin:
	$(REQUIRE_PM3)
	$(PYTHON) $(SCRIPTS)/proxmark_reader.py write-admin --pm3 "$(PM3)" --admin "$(ESP_DATA)/admin.txt" --tag "$(TAG)"

proxmark-write-cards:
	$(REQUIRE_PM3)
	$(PYTHON) $(SCRIPTS)/proxmark_reader.py write-cards --pm3 "$(PM3)" --cards "$(ESP_DATA)/cards.txt" --tag "$(TAG)"

proxmark-write-modes:
	$(REQUIRE_PM3)
	$(PYTHON) $(SCRIPTS)/proxmark_reader.py write-modes --pm3 "$(PM3)" --tag "$(TAG)"
