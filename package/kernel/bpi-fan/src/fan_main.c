// SPDX-License-Identifier: GPL-2.0
/*
 * bpi_fan - quiet fan controller for the BPI-Wifi6-mini (TR6560).
 *
 * Replaces the prebuilt bpi_thermal. The fan (CN5) is a 5 V fan with a PWM
 * input, fed from GPIO19 through a non-inverting MOSFET level shifter
 * (Q4, 10k pull-ups on both sides, so a floating pin means full speed).
 *
 * bpi_thermal drove GPIO19 with the SoC's LED blink engine, whose period is
 * so long that the fan crackles. GPIO19 has no real PWM/LED function
 * (schematic: RGMII_RXDV / UART0_RTS only), so the pin is toggled in
 * software at a few kHz (inaudible above ~5 kHz), by one of two backends:
 *
 *  - FIQ (default): the SP804 timer1 - the kernel's unused broadcast
 *    clockevent - runs periodic and is routed to CPU1 as a FIQ. A 20
 *    instruction handler copied over the kernel's vector_fiq stub toggles
 *    the pin; edge timing comes from the timer's hardware reload. Cost is
 *    below measurement noise.
 *  - hrtimer: two hrtimer callbacks per period, ~3% of one CPU at 6 kHz.
 *    Used if fiq=0 or if any FIQ precondition check fails at load time.
 *
 * FIQ details. Linux runs in the secure world here, so the GIC can be split:
 * every interrupt except the timer's (ID 79 = SPI 47; the vendor DT wrongly
 * says PPI 47 -> ID 63) moves to group 1, still delivered as IRQ, with
 * GICD_CTLR group 1 forwarding and GICC AckCtl so Linux can take them; the
 * timer stays group 0 and FIQEn is set on CPU1 only. SGIs move to group 1
 * too, which requires NSATT=1 when sending them, so the GIC's ipi_send_mask
 * is wrapped. Linux's own secure GICC_IAR read can still acknowledge the
 * timer interrupt before the FIQ does; a normal Linux handler for it does
 * the same step and EOIs (without it CPU1 wedges at priority 0). A FIQ must
 * never fault, so the two registers it touches get their page-directory
 * entries copied into every mm and its state lives in kmalloc memory.
 *
 * Pin: 0x14900100 bit19 = 1 GPIO mode, 0x14900240 bit19 = 0 blink overlay
 * off, 0x10106004 bit19 = 1 output, 0x10106000 bit19 = level.
 *
 * Temperature comes from the vendor HAL (hcall_tri_kernel_hal_sensor_temp_get,
 * whole degrees C). A PI loop keeps the SoC at target_temp_c; the fan never
 * runs below min_duty (it stalls around 5-10%), switches off below
 * target_temp_c - off_hyst_c, and the duty may only change by slew per poll
 * so the speed doesn't audibly hunt.
 *
 * hwmon: temp1_input, pwm1 (0-255), pwm1_enable (1 = manual, 2 = auto).
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/platform_device.h>
#include <linux/hwmon.h>
#include <linux/hrtimer.h>
#include <linux/workqueue.h>
#include <linux/kallsyms.h>
#include <linux/io.h>
#include <linux/spinlock.h>
#include <linux/mutex.h>
#include <linux/mm.h>
#include <linux/sched/signal.h>
#include <linux/slab.h>
#include <linux/smp.h>
#include <linux/irq.h>
#include <linux/irqdesc.h>
#include <linux/irqdomain.h>
#include <linux/interrupt.h>
#include <linux/delay.h>
#include <linux/cpuhotplug.h>
#include <linux/sched/mm.h>
#include <linux/of_address.h>
#include <linux/irqchip/arm-gic.h>
#include <asm/cacheflush.h>
#include <asm/pgtable.h>

#define FAN_BIT		BIT(19)
#define GPIO_DATA	0x10106000
#define GPIO_DIR	0x10106004
#define PIN_GPIO_MODE	0x14900100
#define PIN_PWM_OVERLAY	0x14900240
#define DUTY_MAX	1000

/*
 * FIQ backend hardware. The SP804 is hardcoded rather than taken from the
 * device tree: the vendor DT describes its interrupt as PPI 47 (Linux maps
 * that to ID 63), while the real line is SPI 47 = GIC ID 79.
 */
#define SP804_BASE	0x10104000	/* timer1: broadcast clockevent, unused */
#define TIMER_HZ	100000000
#define FIQ_ID		79		/* SPI 47 */
#define FIQ_CPU		1
#define GICD_GROUP_REG(id)	(GIC_DIST_IGROUP + 4 * ((id) / 32))
#define GICD_CTRL_GRP1		BIT(1)	/* secure view: forward group 1 */
#define T_LOAD		0x00
#define T_CTRL		0x08
#define T_INTCLR	0x0c
#define T_RIS		0x10
#define T_BGLOAD	0x18
#define T_CTRL_RUN	0xe2		/* enable | periodic | int enable | 32-bit */

/* Tunables (duties in per mille of full speed). */
static int target_temp_c = 60;
module_param(target_temp_c, int, 0644);
MODULE_PARM_DESC(target_temp_c, "SoC temperature to hold, deg C (default 60)");
static int off_hyst_c = 5;
module_param(off_hyst_c, int, 0644);
MODULE_PARM_DESC(off_hyst_c, "fan stops below target - this, deg C (default 5; <0 = never stop)");
static int crit_temp_c = 80;
module_param(crit_temp_c, int, 0644);
MODULE_PARM_DESC(crit_temp_c, "full speed at or above this, deg C (default 80)");
static int kp = 60;
module_param(kp, int, 0644);
MODULE_PARM_DESC(kp, "proportional gain, per mille per deg C (default 60)");
static int ki = 4;
module_param(ki, int, 0644);
MODULE_PARM_DESC(ki, "integral gain, per mille per deg C per poll (default 4)");
static int min_duty = 120;
module_param(min_duty, int, 0644);
MODULE_PARM_DESC(min_duty, "lowest running duty, per mille (default 120)");
static int start_duty = 250;
module_param(start_duty, int, 0644);
MODULE_PARM_DESC(start_duty, "duty for the first poll after starting, per mille (default 250)");
static int slew = 30;
module_param(slew, int, 0644);
MODULE_PARM_DESC(slew, "max duty change per poll, per mille (default 30)");
static int poll_ms = 2000;
module_param(poll_ms, int, 0644);
MODULE_PARM_DESC(poll_ms, "control loop period, ms (default 2000)");
static unsigned int freq = 6000;
module_param(freq, uint, 0444);
MODULE_PARM_DESC(freq, "PWM frequency, Hz (load-time only, default 6000)");
static bool fiq = true;
module_param(fiq, bool, 0444);
MODULE_PARM_DESC(fiq, "use the FIQ PWM backend if possible (load-time only, default 1)");

/* int hcall_tri_kernel_hal_sensor_temp_get(void *out, int len, void *hdr); */
static int (*hcall_temp_get)(void *out, int len, void *hdr);

static void __iomem *gpio_data;
static bool use_fiq;

static DEFINE_MUTEX(ctl_lock);
static struct delayed_work ctl_work;
static struct device *hwmon_dev;
static struct platform_device *pdev;
static bool manual;
static int duty = -1;		/* current output, per mille; -1 = not set yet */
static int integral;		/* PI integrator, per mille */
static int temp_mc;		/* filtered temperature, milli-deg C */
static bool temp_valid;

/*
 * GPIO0-31 share one data register with no set/clear aliases, so the pin is
 * set by read-modify-write - here, in the FIQ handler and in its fallback -
 * without the vendor GPIO driver's lock (a FIQ can't take it). A write to
 * another pin of the bank racing ours can be lost; on this board the other
 * pins of the bank (LEDs, power enables) don't change at runtime.
 */
static void pin_set(bool high)
{
	u32 v = readl(gpio_data);

	writel(high ? v | FAN_BIT : v & ~FAN_BIT, gpio_data);
}

/* ---- hrtimer backend -------------------------------------------------- */

static struct hrtimer pwm_timer;
static DEFINE_SPINLOCK(pwm_lock);
static u64 on_ns, off_ns;
static bool pin_high;

static enum hrtimer_restart hrt_tick(struct hrtimer *t)
{
	u64 next;

	spin_lock(&pwm_lock);		/* HRTIMER_MODE_REL_HARD: hard-IRQ context */
	if (!on_ns || !off_ns) {
		pin_high = on_ns != 0;
		pin_set(pin_high);
		spin_unlock(&pwm_lock);
		return HRTIMER_NORESTART;
	}
	pin_high = !pin_high;
	pin_set(pin_high);
	next = pin_high ? on_ns : off_ns;
	spin_unlock(&pwm_lock);

	hrtimer_forward_now(t, ns_to_ktime(next));
	return HRTIMER_RESTART;
}

static void hrt_output(int d)
{
	u64 period = div_u64(NSEC_PER_SEC, freq);
	bool was_running, run;
	unsigned long flags;

	spin_lock_irqsave(&pwm_lock, flags);
	was_running = on_ns && off_ns;
	on_ns = div_u64(period * d, DUTY_MAX);
	off_ns = period - on_ns;
	run = on_ns && off_ns;
	if (!run) {
		pin_high = on_ns != 0;
		pin_set(pin_high);
	}
	spin_unlock_irqrestore(&pwm_lock, flags);

	/* A running timer picks up new on/off times on its next edge. */
	if (run && !was_running)
		hrtimer_start(&pwm_timer, ns_to_ktime(off_ns), HRTIMER_MODE_REL_HARD);
}

/* ---- FIQ backend ------------------------------------------------------ */

extern const u8 bpi_fiq_start[], bpi_fiq_end[];

static void __iomem *timer, *gicd, *gicc;
static u32 *params;	/* [0] high ticks [1] low ticks [2] level [3] FIQ count */
static u32 fiq_period;	/* timer ticks per PWM period */
static bool fiq_running;
static unsigned int fallback_irq;
static unsigned long fallback_count;

static void *stub_alias;
static u8 stub_saved[128];
static struct irq_chip *gic_chip;
static void (*orig_send)(struct irq_data *d, const struct cpumask *mask);
static u32 saved_ctlr[NR_CPUS], saved_group0[NR_CPUS];
static u32 saved_group[8];
static u32 saved_dist_ctlr;
static u8 saved_prio, saved_target;
static int nr_group_regs;

static void fiq_ipi_send(struct irq_data *d, const struct cpumask *mask)
{
	unsigned long map = 0;
	int cpu;

	orig_send(d, mask);		/* NSATT=0: reaches group 0 targets */
	for_each_cpu(cpu, mask)
		map |= BIT(cpu);
	writel_relaxed(map << 16 | BIT(15) | d->hwirq, gicd + GIC_DIST_SOFTINT);
}

static struct irq_data *find_gic_irq(void)
{
	unsigned int i;

	for (i = 1; i < nr_irqs; i++) {
		struct irq_data *d = irq_get_irq_data(i);

		if (d && d->chip && d->chip->ipi_send_mask && d->domain)
			return d;
	}
	return NULL;
}

static irqreturn_t fiq_fallback(int irq, void *dev)
{
	u32 lvl;

	if (!(readl(timer + T_RIS) & 1))
		return IRQ_NONE;
	writel(1, timer + T_INTCLR);
	lvl = params[2] ^ FAN_BIT;
	params[2] = lvl;
	pin_set(lvl);
	writel(lvl ? params[1] : params[0], timer + T_BGLOAD);
	fallback_count++;
	return IRQ_HANDLED;
}

static void cpu_if_grp1_on(void *unused)
{
	int cpu = smp_processor_id();

	saved_ctlr[cpu] = readl(gicc + GIC_CPU_CTRL);
	saved_group0[cpu] = readl(gicd + GIC_DIST_IGROUP);
	writel(saved_ctlr[cpu] | GIC_CPU_CTRL_EnableGrp1 | GIC_CPU_CTRL_AckCtl,
	       gicc + GIC_CPU_CTRL);
	writel(0xffffffff, gicd + GIC_DIST_IGROUP);	/* banked: SGIs + PPIs -> group 1 */
}

static void cpu_if_restore(void *unused)
{
	int cpu = smp_processor_id();

	writel(saved_group0[cpu], gicd + GIC_DIST_IGROUP);
	writel(saved_ctlr[cpu], gicc + GIC_CPU_CTRL);
}

static void cpu_fiq_enable(void *on)
{
	u32 v = readl(gicc + GIC_CPU_CTRL);

	writel(on ? v | GIC_CPU_CTRL_FIQEn : v & ~GIC_CPU_CTRL_FIQEn, gicc + GIC_CPU_CTRL);
}

struct fiq_regs { u32 r8, r9, r10; };

static void set_fiq_regs(void *info)
{
	register struct fiq_regs *r asm("r1") = info;

	asm volatile(
		"mrs	r2, cpsr\n\t"
		"msr	cpsr_c, #0xd1\n\t"	/* FIQ mode, I+F masked */
		"ldmia	r1, {r8 - r10}\n\t"
		"msr	cpsr_c, r2\n\t"
		: : "r"(r) : "r2", "memory");
}

/*
 * A FIQ must never take a translation fault, so the page-directory entries
 * covering the registers it touches are copied into every existing mm (mms
 * created later inherit them from init_mm). get_task_mm() pins each mm, so a
 * process exiting meanwhile can't free the page directory under us.
 */
#define SYNC_MAX_MMS	1024

static int sync_pmd(void __iomem *va)
{
	unsigned long addr = (unsigned long)va;
	pmd_t *src = pmd_off(current->mm, addr);
	struct mm_struct **mms;
	struct task_struct *p;
	int i, n = 0, ret = 0;

	readl(va);			/* faults the entry into our own mm */
	mms = kmalloc_array(SYNC_MAX_MMS, sizeof(*mms), GFP_KERNEL);
	if (!mms)
		return -ENOMEM;
	rcu_read_lock();
	for_each_process(p) {
		struct mm_struct *mm;

		if (n == SYNC_MAX_MMS) {
			ret = -E2BIG;
			break;
		}
		mm = get_task_mm(p);
		if (mm)
			mms[n++] = mm;
	}
	rcu_read_unlock();
	for (i = 0; i < n; i++) {
		pmd_t *dst = pmd_off(mms[i], addr);

		if (mms[i] != current->mm && pmd_none(dst[(addr >> SECTION_SHIFT) & 1]))
			copy_pmd(dst, src);
		mmput(mms[i]);
	}
	kfree(mms);
	return ret;
}

static int install_stub(void)
{
	unsigned long vfiq = 0xffff001c, target;
	size_t len = bpi_fiq_end - bpi_fiq_start;
	void *alias;
	u32 insn, par;
	s32 off;

	insn = *(volatile u32 *)vfiq;
	if ((insn & 0xff000000) != 0xea000000)
		return -ENODEV;			/* expected "b vector_fiq" */
	off = (s32)(insn << 8) >> 6;
	target = vfiq + 8 + off;
	if (target < 0xffff1000 || target >= 0xffff2000)
		return -ENODEV;

	if (len > sizeof(stub_saved) ||
	    (target & ~PAGE_MASK) + sizeof(stub_saved) > PAGE_SIZE)
		return -ENOSPC;
	asm volatile("mcr p15, 0, %1, c7, c8, 0\n\tisb\n\t"
		     "mrc p15, 0, %0, c7, c4, 0" : "=r"(par) : "r"(target));
	if (par & 1)
		return -EFAULT;
	alias = phys_to_virt((par & PAGE_MASK) | (target & ~PAGE_MASK));
	if (!virt_addr_valid(alias))
		return -EFAULT;
	memcpy(stub_saved, alias, sizeof(stub_saved));
	memcpy(alias, bpi_fiq_start, len);
	flush_icache_range((unsigned long)alias, (unsigned long)alias + len);
	flush_icache_range(target, target + len);
	stub_alias = alias;
	return 0;
}

static void remove_stub(void)
{
	unsigned long target;

	if (!stub_alias)
		return;
	target = 0xffff1000 | ((unsigned long)stub_alias & ~PAGE_MASK);
	memcpy(stub_alias, stub_saved, sizeof(stub_saved));
	flush_icache_range((unsigned long)stub_alias,
			   (unsigned long)stub_alias + sizeof(stub_saved));
	flush_icache_range(target, target + sizeof(stub_saved));
	stub_alias = NULL;
}

/*
 * Bringing a CPU back online resets its GIC CPU interface (gic_cpu_if_up()
 * rewrites GICC_CTLR without the group 1 / AckCtl / FIQEn bits), which would
 * leave it deaf to every interrupt moved to group 1 above. So CPUs may not go
 * offline while the FIQ PWM is active. (CPU PM power-down would do the same,
 * but this kernel has no cpuidle driver.)
 */
static int fiq_cpuhp_state;

static int fiq_cpu_offline(unsigned int cpu)
{
	pr_warn("bpi_fan: CPU%u stays online while the FIQ PWM is active\n", cpu);
	return -EBUSY;
}

static void fiq_stop(bool high)
{
	writel(0, timer + T_CTRL);
	writel(1, timer + T_INTCLR);
	udelay(5);			/* let an in-flight FIQ finish */
	pin_set(high);
	fiq_running = false;
}

static void fiq_output(int d)
{
	u32 hi, lo;

	if (d == 0 || d == DUTY_MAX) {
		fiq_stop(d == DUTY_MAX);
		return;
	}
	hi = div_u64((u64)fiq_period * d, DUTY_MAX);
	lo = fiq_period - hi;
	WRITE_ONCE(params[0], hi);
	WRITE_ONCE(params[1], lo);
	if (!fiq_running) {
		pin_set(false);		/* start in the low half */
		WRITE_ONCE(params[2], 0);
		writel(lo, timer + T_LOAD);
		writel(hi, timer + T_BGLOAD);
		writel(1, timer + T_INTCLR);
		writel(T_CTRL_RUN, timer + T_CTRL);
		fiq_running = true;
	}
}

static void gic_restore(void)
{
	int i;

	on_each_cpu(cpu_fiq_enable, NULL, 1);	/* clear FIQEn everywhere */
	if (fallback_irq) {
		irq_set_affinity_hint(fallback_irq, NULL);
		free_irq(fallback_irq, &fallback_count);
		irq_dispose_mapping(fallback_irq);
		fallback_irq = 0;
	}
	writeb(saved_target, gicd + GIC_DIST_TARGET + FIQ_ID);
	writeb(saved_prio, gicd + GIC_DIST_PRI + FIQ_ID);
	for (i = 1; i < nr_group_regs; i++)
		writel(saved_group[i], gicd + GIC_DIST_IGROUP + 4 * i);
	on_each_cpu(cpu_if_restore, NULL, 1);
	writel(saved_dist_ctlr, gicd + GIC_DIST_CTRL);
	gic_chip->ipi_send_mask = orig_send;
}

static void fiq_unmap(void)
{
	kfree(params);
	params = NULL;
	if (gicc)
		iounmap(gicc);
	if (gicd)
		iounmap(gicd);
	if (timer)
		iounmap(timer);
	gicc = gicd = timer = NULL;
}

/* Called with the pin already a GPIO output. Returns 0 if FIQ PWM is live. */
static int fiq_setup(void)
{
	struct irq_fwspec fwspec = {
		.param_count = 3,
		.param = { 0, FIQ_ID - 32, IRQ_TYPE_LEVEL_HIGH },
	};
	struct device_node *gic_np;
	struct fiq_regs regs;
	struct irq_data *gic_d;
	int i, ret;
	u32 g;

	if (num_online_cpus() < 2 || !cpu_online(FIQ_CPU))
		return -ENODEV;
	fiq_period = TIMER_HZ / freq;

	gic_d = find_gic_irq();
	if (!gic_d)
		return -ENODEV;
	gic_chip = gic_d->chip;
	gic_np = to_of_node(gic_d->domain->fwnode);
	gicd = of_iomap(gic_np, 0);	/* GIC node reg: distributor, CPU interface */
	gicc = of_iomap(gic_np, 1);
	timer = ioremap(SP804_BASE, 0x20);
	params = kzalloc(4 * sizeof(u32), GFP_KERNEL);
	ret = -ENOMEM;
	if (!timer || !gicd || !gicc || !params)
		goto err_unmap;

	ret = -EBUSY;			/* timer1 must be idle */
	if (readl(timer + T_CTRL) & 0x80)
		goto err_unmap;

	/* Secure GIC access: toggle FIQ_ID's group bit and read it back. */
	g = readl(gicd + GICD_GROUP_REG(FIQ_ID));
	writel(g | BIT(FIQ_ID % 32), gicd + GICD_GROUP_REG(FIQ_ID));
	ret = (readl(gicd + GICD_GROUP_REG(FIQ_ID)) & BIT(FIQ_ID % 32)) ? 0 : -EPERM;
	writel(g, gicd + GICD_GROUP_REG(FIQ_ID));
	if (ret)
		goto err_unmap;

	ret = cpuhp_setup_state_nocalls(CPUHP_AP_ONLINE_DYN, "bpi_fan:fiq",
					NULL, fiq_cpu_offline);
	if (ret < 0)
		goto err_unmap;
	fiq_cpuhp_state = ret;

	ret = sync_pmd(gpio_data) ?: sync_pmd(timer);
	if (ret)
		goto err_cpuhp;
	ret = install_stub();
	if (ret)
		goto err_cpuhp;

	/* Group 1 must be forwarded by the distributor before anything moves. */
	saved_dist_ctlr = readl(gicd + GIC_DIST_CTRL);
	writel(saved_dist_ctlr | GICD_CTRL_GRP1, gicd + GIC_DIST_CTRL);
	ret = -EPERM;
	if (!(readl(gicd + GIC_DIST_CTRL) & GICD_CTRL_GRP1))
		goto err_stub;

	/* IPIs must get through whichever group the SGIs are in. */
	orig_send = gic_chip->ipi_send_mask;
	gic_chip->ipi_send_mask = fiq_ipi_send;

	/* Group 1 enabled and acknowledgeable on every CPU interface; then
	 * SGIs/PPIs and all SPIs but ours move to it. */
	on_each_cpu(cpu_if_grp1_on, NULL, 1);
	nr_group_regs = min_t(int, (readl(gicd + GIC_DIST_CTR) & 0x1f) + 1,
			      ARRAY_SIZE(saved_group));
	for (i = 1; i < nr_group_regs; i++) {
		saved_group[i] = readl(gicd + GIC_DIST_IGROUP + 4 * i);
		writel(i == FIQ_ID / 32 ? ~BIT(FIQ_ID % 32) : 0xffffffff,
		       gicd + GIC_DIST_IGROUP + 4 * i);
	}

	/* FIQ_ID: banked registers and Linux fallback on CPU1, then highest
	 * priority, CPU1 only, FIQ there. */
	saved_prio = readb(gicd + GIC_DIST_PRI + FIQ_ID);
	saved_target = readb(gicd + GIC_DIST_TARGET + FIQ_ID);
	regs.r8 = (u32)gpio_data;
	regs.r9 = (u32)timer;
	regs.r10 = (u32)params;
	smp_call_function_single(FIQ_CPU, set_fiq_regs, &regs, 1);

	fwspec.fwnode = gic_d->domain->fwnode;
	ret = irq_create_fwspec_mapping(&fwspec);
	if (ret <= 0) {
		ret = ret ?: -ENODEV;
		goto err_gic;
	}
	fallback_irq = ret;
	ret = request_irq(fallback_irq, fiq_fallback, IRQF_NO_THREAD | IRQF_TIMER,
			  "bpi_fan", &fallback_count);
	if (ret) {
		irq_dispose_mapping(fallback_irq);
		fallback_irq = 0;
		goto err_gic;
	}
	irq_set_affinity_hint(fallback_irq, cpumask_of(FIQ_CPU));
	writeb(0x00, gicd + GIC_DIST_PRI + FIQ_ID);
	writeb(BIT(FIQ_CPU), gicd + GIC_DIST_TARGET + FIQ_ID);
	writel(BIT(FIQ_ID % 32), gicd + GIC_DIST_ENABLE_SET + 4 * (FIQ_ID / 32));
	smp_call_function_single(FIQ_CPU, cpu_fiq_enable, (void *)1, 1);
	return 0;

err_gic:
	gic_restore();
	goto err_remove_stub;
err_stub:
	writel(saved_dist_ctlr, gicd + GIC_DIST_CTRL);
err_remove_stub:
	remove_stub();
err_cpuhp:
	cpuhp_remove_state_nocalls(fiq_cpuhp_state);
err_unmap:
	fiq_unmap();
	return ret;
}

static void fiq_teardown(void)
{
	fiq_stop(true);
	gic_restore();
	remove_stub();
	cpuhp_remove_state_nocalls(fiq_cpuhp_state);
	fiq_unmap();
}

/* ---- output ----------------------------------------------------------- */

static void pwm_output(int d)
{
	d = clamp(d, 0, DUTY_MAX);
	if (use_fiq)
		fiq_output(d);
	else
		hrt_output(d);
	duty = d;
}

/* ---- control loop ------------------------------------------------------ */

static int read_temp_c(int *t)
{
	u32 buf[4] = { 0, 0, 0, 0 };

	if (hcall_temp_get(&buf[1], 12, &buf[0]))
		return -EIO;
	*t = (int)buf[3];
	return (*t > -40 && *t < 150) ? 0 : -ERANGE;
}

static int control_step(int t_mc)
{
	int err_mc = t_mc - target_temp_c * 1000;
	int want;

	if (t_mc >= crit_temp_c * 1000) {
		integral = DUTY_MAX;
		return DUTY_MAX;
	}

	/* Anti-windup: only integrate while the output isn't pinned. */
	if (!((duty >= DUTY_MAX && err_mc > 0) || (duty <= min_duty && err_mc < 0)))
		integral = clamp(integral + ki * err_mc / 1000, 0, DUTY_MAX);
	want = clamp(integral + kp * err_mc / 1000, 0, DUTY_MAX);

	if (duty == 0)		/* stopped: restart once back at target, with a kick */
		return err_mc < 0 ? 0 : max(want, start_duty);
	if (want < min_duty) {
		if (off_hyst_c >= 0 && err_mc <= -off_hyst_c * 1000) {
			integral = 0;
			return 0;
		}
		want = min_duty;
	}
	if (duty < 0)		/* first reading: straight to the PI output */
		return want;
	return clamp(want, max(duty - slew, min_duty), duty + slew);
}

static void ctl_fn(struct work_struct *w)
{
	int t;

	mutex_lock(&ctl_lock);
	if (read_temp_c(&t)) {
		temp_valid = false;
		if (!manual)
			pwm_output(DUTY_MAX);
	} else {
		/* EMA, alpha 1/2: the sensor only resolves whole degrees. */
		temp_mc = temp_valid ? (temp_mc + t * 1000) / 2 : t * 1000;
		temp_valid = true;
		if (!manual)
			pwm_output(control_step(temp_mc));
	}
	mutex_unlock(&ctl_lock);

	schedule_delayed_work(&ctl_work, msecs_to_jiffies(clamp(poll_ms, 200, 60000)));
}

/* ---- hwmon -------------------------------------------------------------- */

static umode_t bpi_fan_visible(const void *data, enum hwmon_sensor_types type,
			       u32 attr, int channel)
{
	if (type == hwmon_temp && attr == hwmon_temp_input)
		return 0444;
	if (type == hwmon_pwm && (attr == hwmon_pwm_input || attr == hwmon_pwm_enable))
		return 0644;
	return 0;
}

static int bpi_fan_read(struct device *dev, enum hwmon_sensor_types type,
			u32 attr, int channel, long *val)
{
	mutex_lock(&ctl_lock);
	if (type == hwmon_temp)
		*val = temp_mc;
	else if (attr == hwmon_pwm_input)
		*val = duty < 0 ? 255 : DIV_ROUND_CLOSEST(duty * 255, DUTY_MAX);
	else
		*val = manual ? 1 : 2;
	mutex_unlock(&ctl_lock);

	if (type == hwmon_temp && !temp_valid)
		return -EIO;
	return 0;
}

static int bpi_fan_write(struct device *dev, enum hwmon_sensor_types type,
			 u32 attr, int channel, long val)
{
	int ret = 0;

	mutex_lock(&ctl_lock);
	if (attr == hwmon_pwm_input) {
		if (val < 0 || val > 255)
			ret = -EINVAL;
		else if (!manual)
			ret = -EBUSY;
		else
			pwm_output(DIV_ROUND_CLOSEST(val * DUTY_MAX, 255));
	} else if (attr == hwmon_pwm_enable) {
		if (val == 1) {
			manual = true;
		} else if (val == 2) {
			manual = false;
			integral = max(duty, 0);	/* bumpless transfer */
		} else {
			ret = -EINVAL;
		}
	} else {
		ret = -EOPNOTSUPP;
	}
	mutex_unlock(&ctl_lock);
	return ret;
}

static const struct hwmon_channel_info *bpi_fan_info[] = {
	HWMON_CHANNEL_INFO(temp, HWMON_T_INPUT),
	HWMON_CHANNEL_INFO(pwm, HWMON_PWM_INPUT | HWMON_PWM_ENABLE),
	NULL
};

static const struct hwmon_ops bpi_fan_ops = {
	.is_visible = bpi_fan_visible,
	.read = bpi_fan_read,
	.write = bpi_fan_write,
};

static const struct hwmon_chip_info bpi_fan_chip = {
	.ops = &bpi_fan_ops,
	.info = bpi_fan_info,
};

/* Diagnostics: edges handled by the FIQ and by its Linux fallback. */
static int edges_get(char *buf, const struct kernel_param *kp)
{
	if (!use_fiq)
		return sprintf(buf, "hrtimer\n");
	return sprintf(buf, "fiq %u fallback %lu\n", READ_ONCE(params[3]),
		       fallback_count);
}

static const struct kernel_param_ops edges_ops = { .get = edges_get };
module_param_cb(edges, &edges_ops, NULL, 0444);
MODULE_PARM_DESC(edges, "PWM backend and edge counters (read-only)");

/* ---- init / exit ------------------------------------------------------ */

static int reg_update(phys_addr_t a, u32 clear, u32 set)
{
	void __iomem *p = ioremap(a, 4);

	if (!p)
		return -ENOMEM;
	writel((readl(p) & ~clear) | set, p);
	iounmap(p);
	return 0;
}

static int __init bpi_fan_init(void)
{
	int ret;

	if (freq < 100 || freq > 50000)
		return -EINVAL;

	hcall_temp_get = (void *)kallsyms_lookup_name(
		"hcall_tri_kernel_hal_sensor_temp_get");
	if (!hcall_temp_get) {
		pr_err("bpi_fan: hcall_tri_kernel_hal_sensor_temp_get not found\n");
		return -ENODEV;
	}

	gpio_data = ioremap(GPIO_DATA, 4);
	if (!gpio_data)
		return -ENOMEM;

	hrtimer_init(&pwm_timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL_HARD);
	pwm_timer.function = hrt_tick;

	/* Full speed until the first control step, then hand the pin over. */
	pin_set(true);
	ret = reg_update(PIN_GPIO_MODE, 0, FAN_BIT) ?:
	      reg_update(GPIO_DIR, 0, FAN_BIT) ?:
	      reg_update(PIN_PWM_OVERLAY, FAN_BIT, 0);
	if (ret)
		goto err_unmap;

	if (fiq) {
		ret = fiq_setup();
		use_fiq = !ret;
		if (ret)
			pr_warn("bpi_fan: FIQ PWM unavailable (%d), using hrtimer\n", ret);
	}

	pdev = platform_device_register_simple("bpi_fan", -1, NULL, 0);
	if (IS_ERR(pdev)) {
		ret = PTR_ERR(pdev);
		goto err_backend;
	}
	hwmon_dev = hwmon_device_register_with_info(&pdev->dev, "bpi_fan", NULL,
						    &bpi_fan_chip, NULL);
	if (IS_ERR(hwmon_dev)) {
		ret = PTR_ERR(hwmon_dev);
		goto err_pdev;
	}

	INIT_DELAYED_WORK(&ctl_work, ctl_fn);
	schedule_delayed_work(&ctl_work, 0);
	pr_info("bpi_fan: loaded, target %d C, %u Hz %s PWM\n", target_temp_c,
		freq, use_fiq ? "FIQ" : "hrtimer");
	return 0;

err_pdev:
	platform_device_unregister(pdev);
err_backend:
	if (use_fiq)
		fiq_teardown();
err_unmap:
	pin_set(true);
	iounmap(gpio_data);
	return ret;
}

static void __exit bpi_fan_exit(void)
{
	cancel_delayed_work_sync(&ctl_work);
	hwmon_device_unregister(hwmon_dev);
	platform_device_unregister(pdev);
	if (use_fiq)
		fiq_teardown();
	hrtimer_cancel(&pwm_timer);
	/* Leave the fan at full speed. */
	pin_set(true);
	iounmap(gpio_data);
	pr_info("bpi_fan: unloaded (fan left at full speed)\n");
}

module_init(bpi_fan_init);
module_exit(bpi_fan_exit);

MODULE_LICENSE("GPL");
MODULE_VERSION("2.0");
MODULE_DESCRIPTION("BPI-Wifi6-mini quiet fan controller (FIQ or hrtimer PWM on GPIO19)");
