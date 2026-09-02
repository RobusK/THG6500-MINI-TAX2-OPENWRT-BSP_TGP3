// SPDX-License-Identifier: GPL-2.0
/*
 * bpi_temp - expose the TR6560 SoC sensor temperature at /proc/bpi_temp.
 *
 * The Triductor BSP has no public API for the on-die temperature sensor; the
 * reading is only available through the vendor HAL function
 * hcall_tri_kernel_hal_sensor_temp_get(), which is exported by one of the
 * tri_* kernel modules but is NOT in any header. We resolve it at load time
 * with kallsyms_lookup_name() and call it on each read of /proc/bpi_temp.
 *
 * This source was reconstructed from the prebuilt bpi_temp.ko (v1.0) shipped
 * in the original THG6500 image after the package sources were lost. The HAL
 * calling convention mirrors the binary exactly: the temperature is returned
 * in the 4th word of a zero-initialised 4-word scratch buffer, with the HAL
 * writing 12 bytes starting at word 1 and word 0 used as the request header.
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/kallsyms.h>

/* int hcall_tri_kernel_hal_sensor_temp_get(void *out, int len, void *hdr); */
static int (*hcall_temp_get)(void *out, int len, void *hdr);

static int temp_show(struct seq_file *m, void *v)
{
	u32 buf[4] = { 0, 0, 0, 0 };
	int ret;

	if (!hcall_temp_get) {
		seq_puts(m, "Error: HAL symbol not resolved\n");
		return 0;
	}

	ret = hcall_temp_get(&buf[1], 12, &buf[0]);
	if (ret == 0)
		seq_printf(m, "%d\n", buf[3]);
	else
		seq_printf(m, "Error: HAL returned %u\n", ret);

	return 0;
}

static int temp_open(struct inode *inode, struct file *file)
{
	return single_open(file, temp_show, NULL);
}

static const struct proc_ops temp_fops = {
	.proc_open    = temp_open,
	.proc_read    = seq_read,
	.proc_lseek   = seq_lseek,
	.proc_release = single_release,
};

static int __init bpi_temp_init(void)
{
	hcall_temp_get = (void *)kallsyms_lookup_name(
		"hcall_tri_kernel_hal_sensor_temp_get");
	if (!hcall_temp_get) {
		pr_err("bpi_temp: Failed to resolve hcall_tri_kernel_hal_sensor_temp_get\n");
		return -ENODEV;
	}

	if (!proc_create("bpi_temp", 0444, NULL, &temp_fops)) {
		pr_err("bpi_temp: Failed to create /proc/bpi_temp\n");
		return -ENOMEM;
	}

	pr_info("bpi_temp: Module loaded (HAL at %p)\n", hcall_temp_get);
	return 0;
}

static void __exit bpi_temp_exit(void)
{
	remove_proc_entry("bpi_temp", NULL);
	pr_info("bpi_temp: Module unloaded\n");
}

module_init(bpi_temp_init);
module_exit(bpi_temp_exit);

MODULE_LICENSE("GPL");
MODULE_VERSION("1.0");
MODULE_DESCRIPTION("TR6560 SoC temperature sensor readout via /proc/bpi_temp");
