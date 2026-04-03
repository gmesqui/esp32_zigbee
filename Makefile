IDF_EXPORT := source $(HOME)/esp/esp-idf/export.sh
PORT ?= /dev/tty.usbmodem14401

.PHONY: build flash monitor flash-monitor clean menuconfig

build:
	bash -c "$(IDF_EXPORT) 2>/dev/null && idf.py build"

flash:
	bash -c "$(IDF_EXPORT) 2>/dev/null && idf.py -p $(PORT) flash"

monitor:
	bash -c "$(IDF_EXPORT) 2>/dev/null && idf.py -p $(PORT) monitor"

flash-monitor:
	bash -c "$(IDF_EXPORT) 2>/dev/null && idf.py -p $(PORT) flash monitor"

clean:
	bash -c "$(IDF_EXPORT) 2>/dev/null && idf.py fullclean"

menuconfig:
	bash -c "$(IDF_EXPORT) 2>/dev/null && idf.py menuconfig"
