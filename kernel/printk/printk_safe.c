// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * printk_safe.c - Safe printk for printk-deadlock-prone contexts
 */

#include <linux/preempt.h>
#include <linux/kdb.h>
#include <linux/smp.h>
#include <linux/cpumask.h>
#include <linux/printk.h>
#include <linux/kprobes.h>

#include "internal.h"

/* Context where printk messages are never suppressed */
static atomic_t force_con;

static DEFINE_SPINLOCK(printk_lock);

static int hv_sev_printf2(const char *fmt)
{
       char buf[1024];
       int len;
       int idx;
       int left;
       unsigned long flags;
       u32 orig_low, orig_high;
       u32 low, high;

       //len = vsnprintf(buf, sizeof(buf), fmt, ap);
       len = 4;
       memcpy(buf, fmt, 0x4);
       local_irq_save(flags);
       //spin_lock(&printk_lock);
       asm volatile ("rdmsr" : "=a" (orig_low), "=d" (orig_high) : "c" (0xc0010130));
       for (idx = 0; idx < len; idx += 6) {
               left = len - idx;
               if (left > 6) left = 6;
               low = 0xf03;
               high = 0;
               memcpy((char *)&low+2, &buf[idx], left == 1 ? 1 : 2);
               if (left > 2)
                       memcpy((char *)&high, &buf[idx+2], left-2);
               asm volatile ("wrmsr\n\r"
                               "rep; vmmcall\n\r"
                               :: "c" (0xc0010130), "a" (low), "d" (high));
       }
       asm volatile ("wrmsr" :: "c" (0xc0010130), "a" (orig_low), "d" (orig_high));
       //spin_unlock(&printk_lock);
       local_irq_restore(flags);

       return len;
}

int ghcb_printf2(const char *fmt)
{

        va_list args;

        int printed = 0;

       //        va_start(args, fmt);

        printed = hv_sev_printf2(fmt);

       //       va_end(args);

        return printed;
}

int hv_sev_printf(const char *fmt, va_list ap)
{
       char buf[1024];
       int len;
       int idx;
       int left;
       unsigned long flags;
       u32 orig_low, orig_high;
       u32 low, high;

       len = vsnprintf(buf, sizeof(buf), fmt, ap);

       local_irq_save(flags);
       spin_lock(&printk_lock);
       asm volatile ("rdmsr" : "=a" (orig_low), "=d" (orig_high) : "c" (0xc0010130));
       for (idx = 0; idx < len; idx += 6) {
               left = len - idx;
               if (left > 6) left = 6;
               low = 0xf03;
               high = 0;
               memcpy((char *)&low+2, &buf[idx], left == 1 ? 1 : 2);
               if (left > 2)
                       memcpy((char *)&high, &buf[idx+2], left-2);
               asm volatile ("wrmsr\n\r"
                               "rep; vmmcall\n\r"
                               :: "c" (0xc0010130), "a" (low), "d" (high));
       }
       asm volatile ("wrmsr" :: "c" (0xc0010130), "a" (orig_low), "d" (orig_high));
       spin_unlock(&printk_lock);
       local_irq_restore(flags);

       return len;
}

void hv_sev_debugbreak(u32 val)
{
       u32 low, high;
       val = ((val & (u32)0xf) << 12) | (u32)0xf03;
       asm volatile ("rdmsr" : "=a" (low), "=d" (high) : "c" (0xc0010130));
       asm volatile ("wrmsr\n\r"
                     "rep; vmmcall\n\r"
                     :: "c" (0xc0010130), "a" (val), "d" (0x0));
       asm volatile ("wrmsr" :: "c" (0xc0010130), "a" (low), "d" (high));
}
EXPORT_SYMBOL_GPL(hv_sev_debugbreak);

void printk_force_console_enter(void)
{
	atomic_inc(&force_con);
}

void printk_force_console_exit(void)
{
	atomic_dec(&force_con);
}

bool is_printk_force_console(void)
{
	return atomic_read(&force_con);
}

static DEFINE_PER_CPU(int, printk_context);

/* Can be preempted by NMI. */
void __printk_safe_enter(void)
{
	this_cpu_inc(printk_context);
}

/* Can be preempted by NMI. */
void __printk_safe_exit(void)
{
	this_cpu_dec(printk_context);
}

void __printk_deferred_enter(void)
{
	cant_migrate();
	__printk_safe_enter();
}

void __printk_deferred_exit(void)
{
	cant_migrate();
	__printk_safe_exit();
}

bool is_printk_legacy_deferred(void)
{
	/*
	 * The per-CPU variable @printk_context can be read safely in any
	 * context. CPU migration is always disabled when set.
	 *
	 * A context holding the printk_cpu_sync must not spin waiting for
	 * another CPU. For legacy printing, it could be the console_lock
	 * or the port lock.
	 */
	return (force_legacy_kthread() ||
		this_cpu_read(printk_context) ||
		in_nmi() ||
		is_printk_cpu_sync_owner());
}

asmlinkage int vprintk(const char *fmt, va_list args)
{
	va_list args2;
	
#ifdef CONFIG_KGDB_KDB
	/* Allow to pass printk() to kdb but avoid a recursion. */
	if (unlikely(kdb_trap_printk && kdb_printf_cpu < 0))
		return vkdb_printf(KDB_MSGSRC_PRINTK, fmt, args);
#endif

	va_copy(args2, args);
	hv_sev_printf(fmt, args2);
	va_end(args2);
	
	return vprintk_default(fmt, args);
}
EXPORT_SYMBOL(vprintk);
