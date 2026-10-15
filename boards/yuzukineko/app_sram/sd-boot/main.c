/* SPDX-License-Identifier: GPL-2.0+ */

/*
 * SD card boot path for Yuzuki Neko (sun252iw2 / F101 C907).
 *
 * This SPL is loaded by the BROM (FEL or an eGON image) into SRAM. It brings up
 * the PSRAM, opens the FAT32 boot partition of the SD card on SMHC0 and copies
 * the boot payloads into PSRAM, then hands off to the SBI firmware (fw_jump),
 * which in turn starts the Linux kernel.
 *
 * SD card layout:
 *	partition 1 (FAT32)  fw_jump.bin   SBI firmware (<= 512 KiB)
 *	                     sunxi.dtb     device tree (<= 256 KiB)
 *	                     Image         Linux kernel (<= 6 MiB)
 *	                     bootargs.txt  kernel command line (optional, <= 1 KiB;
 *	                                   without it the DTB bootargs stay as they are)
 *	partition 2 (ext4)   the root filesystem, mounted by Linux as /dev/mmcblk0p2
 *
 * FatFS follows the first partition of the drive, so the FAT32 volume has to
 * come first in the partition table.
 */

#include <stdint.h>

#include <cache.h>
#include <cli/cli.h>
#include <cli/cli_shell.h>
#include <common.h>
#include <driver.h>
#include <log.h>
#include <malloc.h>
#include <string.h>
#include <timer.h>

#include <drivers/clk/clk.h>
#include <drivers/mmc/sdcard.h>
#include <drivers/mmc/sdhci.h>
#include <drivers/psram/psram.h>
#include <drivers/serial/serial.h>
#include <dt-bindings/soc/sun252iw2.h>
#include <dt-compatible/mmc-dt.h>
#include <dt-compatible/psram-dt.h>
#include <fdt_wrapper.h>
#include <lib/fatfs/ff.h>
#include <lib/fatfs/diskio.h>
#include <lib/fdt/libfdt.h>

/* PSRAM is the load target for every payload below. */
#define F101_RAM_BASE SUNXI_PSRAM_BASE
#define F101_RAM_SIZE 0x01000000U

/*
 * PSRAM reservations, bottom up: Linux Image, then DTB, then SBI firmware
 * pinned at the top of PSRAM. The SMHC0 IDMA descriptors sit between the heap
 * and the DTB reservation, and the heap for FatFS lives in the gap between the
 * kernel region and the descriptors.
 */
#define F101_KERNEL_SIZE 0x00600000U /* Linux Image up to 6 MiB */
#define F101_DTB_SIZE	 0x00040000U /* Device tree up to 256 KiB */
#define F101_SBI_SIZE	 0x00080000U /* SBI firmware fw_jump 512 KiB */
#define F101_DMA_SIZE	 0x00001000U /* SMHC0 IDMA descriptors */

#define F101_LINUX_ADDR (F101_RAM_BASE)
#define F101_SBI_ADDR	(F101_RAM_BASE + F101_RAM_SIZE - F101_SBI_SIZE)
#define F101_DTB_ADDR	(F101_SBI_ADDR - F101_DTB_SIZE)
#define F101_DMA_ADDR	(F101_DTB_ADDR - F101_DMA_SIZE)

#define F101_HEAP_BASE (F101_LINUX_ADDR + F101_KERNEL_SIZE)
#define F101_HEAP_SIZE (F101_DMA_ADDR - F101_HEAP_BASE)

/* files of the FAT32 boot partition */
#define F101_DTB_FILE	   "sunxi.dtb"
#define F101_SBI_FILE	   "fw_jump.bin"
#define F101_LINUX_FILE	   "Image"
#define F101_BOOTARGS_FILE "bootargs.txt"

/* the bootargs file is read into this SRAM buffer and NUL terminated there */
#define F101_BOOTARGS_MAX 0x100U
static char f101_bootargs[F101_BOOTARGS_MAX];

/* FatFS read granularity; every chunk lands in the destination directly */
#define F101_READ_CHUNK 0x20000U

typedef void (*sbi_entry_t)(unsigned long hartid, uintptr_t fdt_addr);

static int f101_psram_init(void)
{
	sunxi_psram_t psram = { 0 };

	if (sunxi_psram_dt_read_alias(&psram, "psram0") != DRIVER_OK) {
		pr_err("PSRAM: invalid devicetree configuration\n");
		return -1;
	}

	if (sunxi_psram_init(&psram) == 0U) {
		pr_err("PSRAM: initialization failed\n");
		return -1;
	}

	pr_info("PSRAM: %u MiB initialized\n", sunxi_get_psram_size(&psram));
	return 0;
}

static int f101_sd_up(sunxi_sdhci_t *sdhci, sdmmc_pdata_t *card)
{
	if (sunxi_sdhci_dt_read_alias(sdhci, "mmc0") != DRIVER_OK) {
		pr_err("SMHC: invalid devicetree configuration\n");
		return -1;
	}

	if (sunxi_sdhci_init(sdhci) != 0) {
		pr_err("SMHC: %s controller init failed\n", sdhci->name);
		return -1;
	}
	pr_info("SMHC: %s controller initialized\n", sdhci->name);

	if (sdmmc_init(card, sdhci) != 0) {
		pr_warn("SD: init failed, retry...\n");
		if (sdmmc_init(card, sdhci) != 0) {
			pr_err("SD: no card or init failed\n");
			return -1;
		}
	}
	disk_set_device(0, card);
	return 0;
}

/*
 * Read one file from the FAT volume into a fixed destination, capped at
 * max_size. Returns the number of bytes read or -1 on failure.
 */
static int f101_read_from_sd(const char *name, uintptr_t dest, uint32_t max_size)
{
	FATFS fs;
	FIL file;
	UINT byte_read;
	uint32_t total = 0;
	uint32_t start;

	if (f_mount(&fs, "", 1) != FR_OK) {
		pr_err("FATFS: no FAT volume on the first partition\n");
		return -1;
	}

	if (f_open(&file, name, FA_OPEN_EXISTING | FA_READ) != FR_OK) {
		pr_err("FATFS: open [%s] failed\n", name);
		f_mount(0, "", 0);
		return -1;
	}

	if (f_size(&file) > max_size) {
		pr_err("FATFS: [%s] exceeds reserved %u bytes\n", name, max_size);
		f_close(&file);
		f_mount(0, "", 0);
		return -1;
	}

	start = time_ms();
	while (total < max_size) {
		UINT want = max_size - total;

		if (want > F101_READ_CHUNK)
			want = F101_READ_CHUNK;
		if (f_read(&file, (void *)(dest + total), want, &byte_read) != FR_OK)
			break;
		total += byte_read;
		if (byte_read < want)
			break; /* end of file */
	}
	f_close(&file);
	f_mount(0, "", 0);

	if (total == 0U) {
		pr_err("FATFS: [%s] is empty\n", name);
		return -1;
	}

	pr_info("FATFS: %-11s -> 0x%08x (%u KiB in %u ms)\n", name, (unsigned int)dest, total >> 10,
		(unsigned int)(time_ms() - start));
	return (int)total;
}

/*
 * Load the boot payloads from the FAT32 boot partition into PSRAM.
 */
static int f101_load_images(void)
{
	if (f101_read_from_sd(F101_DTB_FILE, F101_DTB_ADDR, F101_DTB_SIZE) < 0)
		return -1;

	if (f101_read_from_sd(F101_SBI_FILE, F101_SBI_ADDR, F101_SBI_SIZE) < 0)
		return -1;

	if (f101_read_from_sd(F101_LINUX_FILE, F101_LINUX_ADDR, F101_KERNEL_SIZE) < 0)
		return -1;

	return 0;
}

/*
 * Read bootargs.txt (optional) and patch it into /chosen of the loaded DTB;
 * without the file the bootargs of the DTB stay as they are.
 */
static int f101_update_dtb(void)
{
	void *fdt = (void *)(uintptr_t)F101_DTB_ADDR;
	int len;
	int node;
	int rc;

	rc = fdt_check_header(fdt);
	if (rc != 0) {
		pr_err("DTB: invalid blob at 0x%08x: %s\n", F101_DTB_ADDR, fdt_strerror(rc));
		return -1;
	}

	if (fdt_totalsize(fdt) > F101_DTB_SIZE) {
		pr_err("DTB: %u bytes exceeds reserved %u bytes\n", fdt_totalsize(fdt), F101_DTB_SIZE);
		return -1;
	}

	len = f101_read_from_sd(F101_BOOTARGS_FILE, (uintptr_t)f101_bootargs, F101_BOOTARGS_MAX - 1);
	if (len < 0) {
		pr_info("DTB: no " F101_BOOTARGS_FILE ", keeping DTB bootargs\n");
		return 0;
	}

	/* trim trailing whitespace and terminate */
	while (len > 0 && (f101_bootargs[len - 1] == '\n' || f101_bootargs[len - 1] == '\r' ||
			   f101_bootargs[len - 1] == ' '))
		len--;
	f101_bootargs[len] = '\0';
	if (len == 0) {
		pr_info("DTB: empty " F101_BOOTARGS_FILE ", keeping DTB bootargs\n");
		return 0;
	}

	if (fdt_increase_size(fdt, 256) != 0) {
		pr_err("DTB: cannot grow for bootargs\n");
		return -1;
	}
	node = fdt_path_offset(fdt, "/chosen");
	if (node < 0)
		node = fdt_add_subnode(fdt, 0, "chosen");
	if (node < 0 || fdt_setprop_string(fdt, node, "bootargs", f101_bootargs) != 0) {
		pr_err("DTB: cannot set bootargs\n");
		return -1;
	}

	pr_info("DTB: bootargs: %s\n", f101_bootargs);
	return 0;
}

static __attribute__((noreturn)) void f101_boot_sbi_firmware(void)
{
	sbi_entry_t entry = (sbi_entry_t)(uintptr_t)F101_SBI_ADDR;

	/* Make images loaded from the SD card visible to SBI firmware and Linux. */
	flush_dcache_all();
	/* Keep D-cache off through PSRAM initialization and image loading. */
	if (!(csr_read(mhcr) & MHCR_DE)) {
		invalidate_dcache_all();
		dcache_enable();
		data_sync_barrier();
	}
	asm volatile("fence rw, rw\n\tfence.i" ::: "memory");
	pr_info("SBI handoff: MHCR=0x%lx\n", csr_read(mhcr));

	entry(0UL, F101_DTB_ADDR);
	__builtin_unreachable();
}

msh_declare_command(boot);
msh_define_help(boot, "boot images already loaded in memory", "Usage: boot");

int cmd_boot(int argc, const char **argv)
{
	(void)argc;
	(void)argv;

	pr_info("Booting memory images: Linux=0x%08x DTB=0x%08x SBI firmware=0x%08x\n", F101_LINUX_ADDR,
		F101_DTB_ADDR, F101_SBI_ADDR);
	f101_boot_sbi_firmware();
}

static const msh_command_entry commands[] = {
	msh_define_command(boot),
	msh_command_end,
};

int main(void)
{
	sunxi_sdhci_t sdhci = { 0 };
	sdmmc_pdata_t card = { 0 };

	if (sunxi_serial_init_stdout() != DRIVER_OK)
		return -1;

	show_banner();
	sunxi_clk_init();

	if (f101_psram_init())
		return -1;

	/* FatFS long-name buffers and the SD stack allocate from the heap. */
	if (malloc_init(F101_HEAP_BASE, F101_HEAP_SIZE) != 0)
		pr_warn("SD: heap init failed\n");

	if (f101_sd_up(&sdhci, &card) != 0)
		goto fallback;

	if (f101_load_images() != 0)
		goto fallback;

	if (f101_update_dtb() != 0)
		goto fallback;

	return cmd_boot(0, NULL);

fallback:
	pr_warn("SD boot failed; insert a card, load images and run 'boot'\n");
	syterkit_shell_attach(commands);

	return cmd_boot(0, NULL);
}
