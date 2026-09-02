#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/uaccess.h>
#include <linux/io.h>

#define PWM_CTRL_PHYS   0x14900800
#define PWM_CTRL2_PHYS  0x1490010c
#define MAP_SIZE         4
#define OFFSET_MAX       60

static void __iomem *pwm_ctrl;
static void __iomem *pwm_ctrl2;

static int duty_offset = 0;
module_param(duty_offset, int, 0644);
MODULE_PARM_DESC(duty_offset, "Duty cycle offset: +N = undervolt, -N = overvolt (default 0)");

/*
 * We track the base duty (what cpufreq sets) separately from what we wrote.
 * On write to /proc/bpi_voltage:
 *   - If offset is changing from 0, current register value IS the base.
 *   - If offset is changing from non-zero, we compute base = current - old_offset.
 * The timer detects cpufreq changes by checking if the register value
 * differs from what we last wrote (meaning cpufreq overwrote us).
 */
static u32 base_duty = 0;
static u32 last_written_duty = 0;

static DEFINE_SPINLOCK(voltage_lock);

static void write_duty(u32 new_duty)
{
	u32 val;

	val = readl(pwm_ctrl);
	val &= ~(0xFFu << 16);
	val |= (new_duty & 0xFF) << 16;
	val |= (1 << 25); /* load */
	writel(val, pwm_ctrl);

	/* Step 2: read back and set bit 26 (latch/apply trigger) */
	val = readl(pwm_ctrl);
	val |= (1 << 26);
	writel(val, pwm_ctrl);

	/* Step 3: set bit 0 of ctrl2 (PWM output enable) */
	val = readl(pwm_ctrl2);
	val |= 1;
	writel(val, pwm_ctrl2);

	last_written_duty = new_duty;
}

static void apply_offset(void)
{
	u32 cur_duty, new_duty;
	int nd;
	unsigned long flags;

	if (!pwm_ctrl)
		return;

	spin_lock_irqsave(&voltage_lock, flags);

	cur_duty = (readl(pwm_ctrl) >> 16) & 0xFF;

	/* Detect if cpufreq overwrote us since last write */
	if (cur_duty != last_written_duty)
		base_duty = cur_duty;

	nd = (int)base_duty + duty_offset;
	if (nd < 1) nd = 1;
	if (nd > 89) nd = 89; /* must stay below period (90) */
	new_duty = (u32)nd;

	if (new_duty != cur_duty) {
		write_duty(new_duty);
	} else {
		last_written_duty = cur_duty;
	}

	spin_unlock_irqrestore(&voltage_lock, flags);
}

/* Restore register to base_duty (undo our offset) */
static void restore_stock(void)
{
	unsigned long flags;

	if (!pwm_ctrl)
		return;

	spin_lock_irqsave(&voltage_lock, flags);
	write_duty(base_duty);
	/* Mark last_written as base so timer won't re-detect as cpufreq change */
	last_written_duty = base_duty;
	spin_unlock_irqrestore(&voltage_lock, flags);
}

static struct timer_list reapply_timer;
static int timer_active;

static void reapply_timer_fn(struct timer_list *t)
{
	if (duty_offset != 0) {
		apply_offset();
		mod_timer(&reapply_timer, jiffies + HZ / 4);
	} else {
		timer_active = 0;
	}
}

static int voltage_show(struct seq_file *m, void *v)
{
	u32 val, duty, period;

	if (!pwm_ctrl) {
		seq_puts(m, "Error: registers not mapped\n");
		return 0;
	}

	val = readl(pwm_ctrl);
	duty = (val >> 16) & 0xFF;
	period = (val >> 8) & 0xFF;

	seq_printf(m, "pwm_ctrl:    0x%08x\n", val);
	seq_printf(m, "period:      %u\n", period);
	seq_printf(m, "duty:        %u\n", duty);
	seq_printf(m, "base_duty:   %u\n", base_duty);
	seq_printf(m, "duty_offset: %d\n", duty_offset);
	seq_printf(m, "vcode_approx: %u\n", 960 - (duty * 5 / 2));

	return 0;
}

static ssize_t voltage_write(struct file *file, const char __user *buf,
			     size_t count, loff_t *ppos)
{
	char kbuf[32];
	int new_offset;
	u32 cur_duty;

	if (count >= sizeof(kbuf))
		return -EINVAL;
	if (copy_from_user(kbuf, buf, count))
		return -EFAULT;
	kbuf[count] = '\0';

	if (sscanf(kbuf, "%d", &new_offset) != 1)
		return -EINVAL;
	if (new_offset < -OFFSET_MAX || new_offset > OFFSET_MAX)
		return -EINVAL;

	if (!pwm_ctrl)
		return -ENODEV;

	/*
	 * Derive the true base from current register state:
	 * If we have an active offset, base = current_duty - old_offset.
	 * If offset was 0, current register IS the base.
	 */
	cur_duty = (readl(pwm_ctrl) >> 16) & 0xFF;
	if (duty_offset != 0 && cur_duty == last_written_duty)
		base_duty = cur_duty - duty_offset;
	else
		base_duty = cur_duty;

	duty_offset = new_offset;
	pr_info("offset=%d base=%u\n", duty_offset, base_duty);

	if (duty_offset != 0) {
		apply_offset();
		if (!timer_active) {
			timer_active = 1;
			mod_timer(&reapply_timer, jiffies + HZ / 4);
		}
	} else {
		restore_stock();
	}

	return count;
}

static int voltage_open(struct inode *inode, struct file *file)
{
	return single_open(file, voltage_show, NULL);
}

static const struct proc_ops voltage_fops = {
	.proc_open    = voltage_open,
	.proc_read    = seq_read,
	.proc_write   = voltage_write,
	.proc_lseek   = seq_lseek,
	.proc_release = single_release,
};

static int __init bpi_voltage_init(void)
{
	pwm_ctrl = ioremap(PWM_CTRL_PHYS, MAP_SIZE);
	if (!pwm_ctrl) {
		pr_err("Failed to map 0x%x\n", PWM_CTRL_PHYS);
		return -ENOMEM;
	}

	pwm_ctrl2 = ioremap(PWM_CTRL2_PHYS, MAP_SIZE);
	if (!pwm_ctrl2) {
		iounmap(pwm_ctrl);
		return -ENOMEM;
	}

	if (!proc_create("bpi_voltage", 0644, NULL, &voltage_fops)) {
		iounmap(pwm_ctrl2);
		iounmap(pwm_ctrl);
		return -ENOMEM;
	}

	timer_setup(&reapply_timer, reapply_timer_fn, 0);

	base_duty = (readl(pwm_ctrl) >> 16) & 0xFF;
	last_written_duty = base_duty;

	pr_info("Loaded (base_duty=%u)\n", base_duty);
	return 0;
}

static void __exit bpi_voltage_exit(void)
{
	int old_offset = duty_offset;
	duty_offset = 0;
	del_timer_sync(&reapply_timer);
	if (old_offset != 0)
		restore_stock();
	remove_proc_entry("bpi_voltage", NULL);
	if (pwm_ctrl2) iounmap(pwm_ctrl2);
	if (pwm_ctrl) iounmap(pwm_ctrl);
	pr_info("Unloaded (restored base_duty=%u)\n", base_duty);
}

module_init(bpi_voltage_init);
module_exit(bpi_voltage_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("TR6560 CPU voltage offset control via PWM");
MODULE_VERSION("1.2");
