BUILD_DIR = build
include $(N64_INST)/include/n64.mk

all: usb-echo.z64

$(BUILD_DIR)/usb-echo.elf: $(BUILD_DIR)/usb-echo.o

usb-echo.z64: N64_ROM_TITLE = "USB echo"

clean:
	rm -rf $(BUILD_DIR) usb-echo.z64

-include $(wildcard $(BUILD_DIR)/*.d)

.PHONY: all clean
