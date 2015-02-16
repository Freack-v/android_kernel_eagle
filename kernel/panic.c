/*
 *  linux/kernel/panic.c
 *
 *  Copyright (C) 1991, 1992  Linus Torvalds
 * Copyright (C) 2012 Sony Mobile Communications AB.
 */

/*
 * This function is used through-out the kernel (including mm and fs)
 * to indicate a major problem.
 */
#include <linux/debug_locks.h>
#include <linux/interrupt.h>
#include <linux/kmsg_dump.h>
#include <linux/kallsyms.h>
#include <linux/notifier.h>
#include <linux/module.h>
#include <linux/random.h>
#include <linux/reboot.h>
#include <linux/delay.h>
#include <linux/kexec.h>
#include <linux/sched.h>
#include <linux/sysrq.h>
#include <linux/init.h>
#include <linux/nmi.h>
#include <linux/dmi.h>
#include <linux/coresight.h>
#include <linux/io.h>
#include <mach/subsystem_restart.h>
#include <mach/msm_iomap.h>
#ifdef CONFIG_CCI_KLOG
extern long* powerpt;
extern long* unknowflag;
extern long* backupcrashflag;
#endif
//[VY5x] ==> CCI KLog, added by Jimmy@CCI
#ifdef CONFIG_CCI_KLOG
#include <linux/mm.h>
#include <linux/irq.h>
#include <linux/cciklog.h>
#endif // #ifdef CONFIG_CCI_KLOG
//[VY5x] <== CCI KLog, added by Jimmy@CCI

/**/
#ifdef CCI_TRACE_INIT_SERVICE
#include <linux/proc_fs.h>
#include <asm/uaccess.h>
extern int get_svc_name(char *pName, int nameSize);
#endif
/**/

#define PANIC_TIMER_STEP 100
#define PANIC_BLINK_SPD 18

/* Machine specific panic information string */
char *mach_panic_string;

int panic_on_oops;
static unsigned long tainted_mask;
static int pause_on_oops;
static int pause_on_oops_flag;
static DEFINE_SPINLOCK(pause_on_oops_lock);
extern void set_warmboot(void);
extern void *restart_reason;
#define ABNORAML_NONE		0x0
#define ABNORAML_REBOOT		0x1
#define ABNORAML_CRASH		0x2
#define ABNORAML_POWEROFF	0x3
extern long abnormalflag;
#define CONFIG_WARMBOOT_CRASH       0xC0DEDEAD
#define CONFIG_WARMBOOT_NONE        0x00000000
#define CONFIG_WARMBOOT_NORMAL      0x77665501
#ifndef CONFIG_PANIC_TIMEOUT
#define CONFIG_PANIC_TIMEOUT 0
#endif
int panic_timeout = CONFIG_PANIC_TIMEOUT;

/**/
#ifdef CCI_TRACE_INIT_SERVICE
#define INIT_CRASH_STRING "kill init"
#define SERVICE_NAME_LENGTH 64
static char svc_name[SERVICE_NAME_LENGTH] = {0};
static struct proc_dir_entry *svc_proc_entry = NULL;
#endif
/**/

EXPORT_SYMBOL_GPL(panic_timeout);

ATOMIC_NOTIFIER_HEAD(panic_notifier_list);

EXPORT_SYMBOL(panic_notifier_list);

static long no_blink(int state)
{
	return 0;
}

/* Returns how long it waited in ms */
long (*panic_blink)(int state);
EXPORT_SYMBOL(panic_blink);

/*
 * Stop ourself in panic -- architecture code may override this
 */
void __weak panic_smp_self_stop(void)
{
	while (1)
		cpu_relax();
}

/**/
#ifdef CCI_TRACE_INIT_SERVICE
static int svc_startrun_write(struct file *filp, const char __user *buff, unsigned long len, void *data)
{
	int ret = 0, length = 0;

	memset((void*)svc_name, 0, sizeof(svc_name));
	length = (len > sizeof(svc_name) ? sizeof(svc_name) : len);
	ret = copy_from_user(&svc_name[0], buff, length);
	if (ret) {
		pr_err("%s: error: copy_from_user failed.\n", __func__);
		ret = -EFAULT;
	}
	svc_name[length+1] = '\0';
	pr_info("%s: svc_name[%s] length[%d]\n", __func__, &svc_name[0],length);
	return length;
}

static int svc_startrun_read(char *page, char **start, off_t off, int count, int *eof, void *data)
{
	int len;
	len = sprintf(page, "%s\n", &svc_name[0]);
	pr_info("%s: len[%d] svc_name[%s]\n", __func__, len, &svc_name[0]);
	return len;
}
#endif
/**/

//quiet reboot
extern void set_quiet_reboot_flag(void);

/**
 *	panic - halt the system
 *	@fmt: The text string to print
 *
 *	Display a message, then perform cleanups.
 *
 *	This function never returns.
 */
void panic(const char *fmt, ...)
{
	static DEFINE_SPINLOCK(panic_lock);
	static char buf[1024];
	va_list args;
	long i, i_next = 0;
	int state = 0;

	if(abnormalflag == ABNORAML_CRASH)
		goto end;

#ifdef CONFIG_CCI_KLOG		
	*unknowflag = 0;
	*backupcrashflag = 0;
#endif	
	set_warmboot();
#ifdef CCI_KLOG_ALLOW_FORCE_PANIC			
	__raw_writel(CONFIG_WARMBOOT_CRASH, restart_reason);
#else
	__raw_writel(CONFIG_WARMBOOT_NORMAL, restart_reason);
#ifdef CONFIG_CCI_KLOG
	*backupcrashflag = CONFIG_WARMBOOT_CRASH;
#endif
	//quiet reboot
	set_quiet_reboot_flag();
#endif	
	abnormalflag = ABNORAML_CRASH;
	mb();
	
	coresight_abort();
	/*
	 * Disable local interrupts. This will prevent panic_smp_self_stop
	 * from deadlocking the first cpu that invokes the panic, since
	 * there is nothing to prevent an interrupt handler (that runs
	 * after the panic_lock is acquired) from invoking panic again.
	 */
	local_irq_disable();

	/*
	 * It's possible to come here directly from a panic-assertion and
	 * not have preempt disabled. Some functions called from here want
	 * preempt to be disabled. No point enabling it later though...
	 *
	 * Only one CPU is allowed to execute the panic code from here. For
	 * multiple parallel invocations of panic, all other CPUs either
	 * stop themself or will wait until they are stopped by the 1st CPU
	 * with smp_send_stop().
	 */
	if (!spin_trylock(&panic_lock))
		panic_smp_self_stop();

//[VY5x] ==> CCI KLog, modified by Jimmy@CCI
#ifndef CONFIG_CCI_KLOG
	console_verbose();
#endif // #ifndef CONFIG_CCI_KLOG
//[VY5x] <== CCI KLog, modified by Jimmy@CCI
	bust_spinlocks(1);
	va_start(args, fmt);
	vsnprintf(buf, sizeof(buf), fmt, args);
	va_end(args);
//[VY5x] ==> CCI KLog, added by Jimmy@CCI
#ifdef CCI_KLOG_CRASH_SIZE
#if CCI_KLOG_CRASH_SIZE
	set_fault_state(FAULT_LEVEL_PANIC, FAULT_TYPE_NONE, buf);
#endif // #if CCI_KLOG_CRASH_SIZE
#endif // #ifdef CCI_KLOG_CRASH_SIZE

/**/
#ifdef CCI_TRACE_INIT_SERVICE
	if (NULL != strstr(buf, INIT_CRASH_STRING))
	{
		printk(KERN_EMERG "Kernel panic - not syncing: %s ,service[%s] died!\n",buf,svc_name);
	}
	else
#endif
/**/
	{
//[VY5x] <== CCI KLog, added by Jimmy@CCI
	printk(KERN_EMERG "Kernel panic - not syncing: %s\n",buf);
//[VY5x] ==> CCI KLog, added by Jimmy@CCI
	}

#ifdef CONFIG_CCI_KLOG
	cklc_save_magic(KLOG_MAGIC_AARM_PANIC, KLOG_STATE_AARM_PANIC);
	local_irq_disable();
	if(!oops_in_progress)//not called from die
	{
		struct pt_regs *regs = get_irq_regs();//only available when in intrrupt
//modules info
		print_modules();
//register info
		if(regs)
		{
			show_regs(regs);
		}
//memory info
		show_mem(SHOW_MEM_FILTER_NODES);
/*
#ifndef CONFIG_DEBUG_BUGVERBOSE
//call-stack
		dump_stack();
#endif // #ifndef CONFIG_DEBUG_BUGVERBOSE
*/
//call-stacks of all threads/processes, it will output huge amount of logs
//		show_state();
	}
#endif // #ifdef CONFIG_CCI_KLOG
//[VY5x] <== CCI KLog, added by Jimmy@CCI
#ifdef CONFIG_DEBUG_BUGVERBOSE
	/*
	 * Avoid nested stack-dumping if a panic occurs during oops processing
	 */
	if (!test_taint(TAINT_DIE) && oops_in_progress <= 1)
		dump_stack();
#endif

	/*
	 * If we have crashed and we have a crash kernel loaded let it handle
	 * everything else.
	 * Do we want to call this before we try to display a message?
	 */
	crash_kexec(NULL);

	/*
	 * Note smp_send_stop is the usual smp shutdown function, which
	 * unfortunately means it may not be hardened to work in a panic
	 * situation.
	 */
	smp_send_stop();

	kmsg_dump(KMSG_DUMP_PANIC);

	atomic_notifier_call_chain(&panic_notifier_list, 0, buf);

	bust_spinlocks(0);

	if (!panic_blink)
		panic_blink = no_blink;

	if (panic_timeout > 0) {
		/*
		 * Delay timeout seconds before rebooting the machine.
		 * We can't use the "normal" timers since we just panicked.
		 */
		printk(KERN_EMERG "Rebooting in %d seconds..", panic_timeout);

		for (i = 0; i < panic_timeout * 1000; i += PANIC_TIMER_STEP) {
			touch_nmi_watchdog();
			if (i >= i_next) {
				i += panic_blink(state ^= 1);
				i_next = i + 3600 / PANIC_BLINK_SPD;
			}
			mdelay(PANIC_TIMER_STEP);
		}
	}
	if (panic_timeout != 0) {
		/*
		 * This will not be a clean reboot, with everything
		 * shutting down.  But if there is a chance of
		 * rebooting the system it will be rebooted.
		 */
//[VY5x] ==> CCI KLog, modified by Jimmy@CCI
#ifndef CONFIG_CCI_KLOG
		emergency_restart();
#endif // #ifdef CONFIG_CCI_KLOG
//[VY5x] <== CCI KLog, modified by Jimmy@CCI
	}
#ifdef __sparc__
	{
		extern int stop_a_enabled;
		/* Make sure the user can actually press Stop-A (L1-A) */
		stop_a_enabled = 1;
		printk(KERN_EMERG "Press Stop-A (L1-A) to return to the boot prom\n");
	}
#endif
#if defined(CONFIG_S390)
	{
		unsigned long caller;

		caller = (unsigned long)__builtin_return_address(0);
		disabled_wait(caller);
	}
#endif
//[VY5x] ==> CCI KLog, modified by Jimmy@CCI
#ifdef CONFIG_CCI_KLOG
#ifdef CCI_KLOG_CRASH_SIZE
#if CCI_KLOG_CRASH_SIZE
	set_fault_state(FAULT_LEVEL_FINISH, FAULT_TYPE_NONE, "");//crash info finished, record important log only
#endif // #if CCI_KLOG_CRASH_SIZE
#endif // #ifdef CCI_KLOG_CRASH_SIZE
	kernel_restart(NULL);
#else // #ifdef CONFIG_CCI_KLOG
	local_irq_enable();
#endif // #ifdef CONFIG_CCI_KLOG
//[VY5x] <== CCI KLog, modified by Jimmy@CCI
end:
	for (i = 0; ; i += PANIC_TIMER_STEP) {
		touch_softlockup_watchdog();
		if (i >= i_next) {
			i += panic_blink(state ^= 1);
			i_next = i + 3600 / PANIC_BLINK_SPD;
		}
		mdelay(PANIC_TIMER_STEP);
	}
}

EXPORT_SYMBOL(panic);


struct tnt {
	u8	bit;
	char	true;
	char	false;
};

static const struct tnt tnts[] = {
	{ TAINT_PROPRIETARY_MODULE,	'P', 'G' },
	{ TAINT_FORCED_MODULE,		'F', ' ' },
	{ TAINT_UNSAFE_SMP,		'S', ' ' },
	{ TAINT_FORCED_RMMOD,		'R', ' ' },
	{ TAINT_MACHINE_CHECK,		'M', ' ' },
	{ TAINT_BAD_PAGE,		'B', ' ' },
	{ TAINT_USER,			'U', ' ' },
	{ TAINT_DIE,			'D', ' ' },
	{ TAINT_OVERRIDDEN_ACPI_TABLE,	'A', ' ' },
	{ TAINT_WARN,			'W', ' ' },
	{ TAINT_CRAP,			'C', ' ' },
	{ TAINT_FIRMWARE_WORKAROUND,	'I', ' ' },
	{ TAINT_OOT_MODULE,		'O', ' ' },
};

/**
 *	print_tainted - return a string to represent the kernel taint state.
 *
 *  'P' - Proprietary module has been loaded.
 *  'F' - Module has been forcibly loaded.
 *  'S' - SMP with CPUs not designed for SMP.
 *  'R' - User forced a module unload.
 *  'M' - System experienced a machine check exception.
 *  'B' - System has hit bad_page.
 *  'U' - Userspace-defined naughtiness.
 *  'D' - Kernel has oopsed before
 *  'A' - ACPI table overridden.
 *  'W' - Taint on warning.
 *  'C' - modules from drivers/staging are loaded.
 *  'I' - Working around severe firmware bug.
 *  'O' - Out-of-tree module has been loaded.
 *
 *	The string is overwritten by the next call to print_tainted().
 */
const char *print_tainted(void)
{
	static char buf[ARRAY_SIZE(tnts) + sizeof("Tainted: ") + 1];

	if (tainted_mask) {
		char *s;
		int i;

		s = buf + sprintf(buf, "Tainted: ");
		for (i = 0; i < ARRAY_SIZE(tnts); i++) {
			const struct tnt *t = &tnts[i];
			*s++ = test_bit(t->bit, &tainted_mask) ?
					t->true : t->false;
		}
		*s = 0;
	} else
		snprintf(buf, sizeof(buf), "Not tainted");

	return buf;
}

int test_taint(unsigned flag)
{
	return test_bit(flag, &tainted_mask);
}
EXPORT_SYMBOL(test_taint);

unsigned long get_taint(void)
{
	return tainted_mask;
}

void add_taint(unsigned flag)
{
	/*
	 * Can't trust the integrity of the kernel anymore.
	 * We don't call directly debug_locks_off() because the issue
	 * is not necessarily serious enough to set oops_in_progress to 1
	 * Also we want to keep up lockdep for staging/out-of-tree
	 * development and post-warning case.
	 */
	switch (flag) {
	case TAINT_CRAP:
	case TAINT_OOT_MODULE:
	case TAINT_WARN:
	case TAINT_FIRMWARE_WORKAROUND:
		break;

	default:
		if (__debug_locks_off())
			printk(KERN_WARNING "Disabling lock debugging due to kernel taint\n");
	}

	set_bit(flag, &tainted_mask);
}
EXPORT_SYMBOL(add_taint);

static void spin_msec(int msecs)
{
	int i;

	for (i = 0; i < msecs; i++) {
		touch_nmi_watchdog();
		mdelay(1);
	}
}

/*
 * It just happens that oops_enter() and oops_exit() are identically
 * implemented...
 */
static void do_oops_enter_exit(void)
{
	unsigned long flags;
	static int spin_counter;

	if (!pause_on_oops)
		return;

	spin_lock_irqsave(&pause_on_oops_lock, flags);
	if (pause_on_oops_flag == 0) {
		/* This CPU may now print the oops message */
		pause_on_oops_flag = 1;
	} else {
		/* We need to stall this CPU */
		if (!spin_counter) {
			/* This CPU gets to do the counting */
			spin_counter = pause_on_oops;
			do {
				spin_unlock(&pause_on_oops_lock);
				spin_msec(MSEC_PER_SEC);
				spin_lock(&pause_on_oops_lock);
			} while (--spin_counter);
			pause_on_oops_flag = 0;
		} else {
			/* This CPU waits for a different one */
			while (spin_counter) {
				spin_unlock(&pause_on_oops_lock);
				spin_msec(1);
				spin_lock(&pause_on_oops_lock);
			}
		}
	}
	spin_unlock_irqrestore(&pause_on_oops_lock, flags);
}

/*
 * Return true if the calling CPU is allowed to print oops-related info.
 * This is a bit racy..
 */
int oops_may_print(void)
{
	return pause_on_oops_flag == 0;
}

/*
 * Called when the architecture enters its oops handler, before it prints
 * anything.  If this is the first CPU to oops, and it's oopsing the first
 * time then let it proceed.
 *
 * This is all enabled by the pause_on_oops kernel boot option.  We do all
 * this to ensure that oopses don't scroll off the screen.  It has the
 * side-effect of preventing later-oopsing CPUs from mucking up the display,
 * too.
 *
 * It turns out that the CPU which is allowed to print ends up pausing for
 * the right duration, whereas all the other CPUs pause for twice as long:
 * once in oops_enter(), once in oops_exit().
 */
void oops_enter(void)
{
	tracing_off();
	/* can't trust the integrity of the kernel anymore: */
	debug_locks_off();
	do_oops_enter_exit();
}

/*
 * 64-bit random ID for oopses:
 */
static u64 oops_id;

static int init_oops_id(void)
{
	if (!oops_id)
		get_random_bytes(&oops_id, sizeof(oops_id));
	else
		oops_id++;

	return 0;
}
late_initcall(init_oops_id);

/**/
#ifdef CCI_TRACE_INIT_SERVICE
static int init_trace_service_name(void)
{
	memset((void*)svc_name, 0, sizeof(svc_name));
	svc_proc_entry = create_proc_entry("svc_startrun", 0777, NULL);
	if (svc_proc_entry == NULL) 
	{
		pr_err("Couldn't create svc_startrun proc entry!");
	} 
	else 
	{
		pr_info("Create svc_startrun proc entry success!");
		svc_proc_entry->write_proc = svc_startrun_write;
		svc_proc_entry->read_proc = svc_startrun_read;
	}

	return 0;
}
late_initcall(init_trace_service_name);
#endif
/**/

void print_oops_end_marker(void)
{
//[VY5x] ==> CCI KLog, added by Jimmy@CCI
#ifdef CCI_KLOG_CRASH_SIZE
#if CCI_KLOG_CRASH_SIZE
	int fault_state = get_fault_state();
#endif // #if CCI_KLOG_CRASH_SIZE
#endif // #ifdef CCI_KLOG_CRASH_SIZE
//[VY5x] <== CCI KLog, added by Jimmy@CCI

	init_oops_id();

	if (mach_panic_string)
		printk(KERN_WARNING "Board Information: %s\n",
		       mach_panic_string);

//[VY5x] ==> CCI KLog, added by Jimmy@CCI
#ifdef CCI_KLOG_CRASH_SIZE
#if CCI_KLOG_CRASH_SIZE
	if(fault_state > 0 && (fault_state & 0x10) == 0)
	{
		printk(KERN_ALERT "---[ end trace %016llx ]---\n", (unsigned long long)oops_id);
	}
	else
#endif // #if CCI_KLOG_CRASH_SIZE
#endif // #ifdef CCI_KLOG_CRASH_SIZE
//[VY5x] <== CCI KLog, added by Jimmy@CCI
	printk(KERN_WARNING "---[ end trace %016llx ]---\n",
		(unsigned long long)oops_id);
}

/*
 * Called when the architecture exits its oops handler, after printing
 * everything.
 */
void oops_exit(void)
{
	do_oops_enter_exit();
	print_oops_end_marker();
	kmsg_dump(KMSG_DUMP_OOPS);
}

#ifdef WANT_WARN_ON_SLOWPATH
struct slowpath_args {
	const char *fmt;
	va_list args;
};

static void warn_slowpath_common(const char *file, int line, void *caller,
				 unsigned taint, struct slowpath_args *args)
{
	const char *board;

	printk(KERN_WARNING "------------[ cut here ]------------\n");
	printk(KERN_WARNING "WARNING: at %s:%d %pS()\n", file, line, caller);
	board = dmi_get_system_info(DMI_PRODUCT_NAME);
	if (board)
		printk(KERN_WARNING "Hardware name: %s\n", board);

	if (args)
		vprintk(args->fmt, args->args);

	print_modules();
	dump_stack();
	print_oops_end_marker();
	add_taint(taint);
}

void warn_slowpath_fmt(const char *file, int line, const char *fmt, ...)
{
	struct slowpath_args args;

	args.fmt = fmt;
	va_start(args.args, fmt);
	warn_slowpath_common(file, line, __builtin_return_address(0),
			     TAINT_WARN, &args);
	va_end(args.args);
}
EXPORT_SYMBOL(warn_slowpath_fmt);

void warn_slowpath_fmt_taint(const char *file, int line,
			     unsigned taint, const char *fmt, ...)
{
	struct slowpath_args args;

	args.fmt = fmt;
	va_start(args.args, fmt);
	warn_slowpath_common(file, line, __builtin_return_address(0),
			     taint, &args);
	va_end(args.args);
}
EXPORT_SYMBOL(warn_slowpath_fmt_taint);

void warn_slowpath_null(const char *file, int line)
{
	warn_slowpath_common(file, line, __builtin_return_address(0),
			     TAINT_WARN, NULL);
}
EXPORT_SYMBOL(warn_slowpath_null);
#endif

#ifdef CONFIG_CC_STACKPROTECTOR

/*
 * Called when gcc's -fstack-protector feature is used, and
 * gcc detects corruption of the on-stack canary value
 */
void __stack_chk_fail(void)
{
	panic("stack-protector: Kernel stack is corrupted in: %p\n",
		__builtin_return_address(0));
}
EXPORT_SYMBOL(__stack_chk_fail);

#endif

core_param(panic, panic_timeout, int, 0644);
core_param(pause_on_oops, pause_on_oops, int, 0644);

static int __init oops_setup(char *s)
{
	if (!s)
		return -EINVAL;
	if (!strcmp(s, "panic"))
		panic_on_oops = 1;
	return 0;
}
early_param("oops", oops_setup);
