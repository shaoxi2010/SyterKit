# SPDX-License-Identifier: GPL-2.0+
include $(srctree)/boards/Makefile.common

# the demo application drives carrier peripherals over I2C and PWM
ifeq ($(CONFIG_DRIVER_I2C)$(CONFIG_DRIVER_PWM),yy)
apps-sram-y += app
endif

ifeq ($(CONFIG_ARCH_RISCV32)$(CONFIG_DRIVER_PSRAM),yy)
# the flash/SPI NOR boot loaders need the SPIF controller
apps-sram-$(CONFIG_DRIVER_MTD_SPIF_NOR) += spinor-boot
apps-sram-$(CONFIG_DRIVER_MTD_SPIF_NOR) += spinor-boot-rv64i
apps-sram-$(CONFIG_DRIVER_MTD_SPIF_NOR) += flash-boot
apps-sram-$(CONFIG_DRIVER_MTD_SPIF_NOR) += xip-boot
apps-sram-y += usb-boot

# SD card boot is the MMC-only alternative to the SPI NOR boot loaders
ifneq ($(CONFIG_DRIVER_MTD_SPIF_NOR),y)
apps-sram-$(CONFIG_DRIVER_MMC) += sd-boot
endif
endif

rust-apps-sram-$(CONFIG_RUST_FFI) += app-rs
apps-sram-$(CONFIG_DRIVER_PLIC) += intc_test
apps-sram-$(CONFIG_DRIVER_USB) += usb_test
# the display test keeps its frame buffer and stack in the PSRAM
ifeq ($(CONFIG_DRIVER_DISPLAY)$(CONFIG_DRIVER_PSRAM),yy)
apps-sram-y += disp_test
endif
apps-dram-$(CONFIG_DRIVER_DISPLAY) += disp_test

obj-$(CONFIG_ARCH_BOOT) += arch_boot/arch_boot.o
