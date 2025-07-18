// SPDX-License-Identifier: GPL-2.0
#include <linux/mm_types.h>
#include <linux/rbtree.h>
#include <linux/rwsem.h>
#include <linux/spinlock.h>
#include <linux/list.h>
#include <linux/cpumask.h>
#include <linux/mman.h>
#include <linux/pgtable.h>

#include <linux/atomic.h>
#include <linux/user_namespace.h>
#include <asm/mmu.h>

#ifndef INIT_MM_CONTEXT
#define INIT_MM_CONTEXT(name)
#endif

/*
 * For dynamically allocated mm_structs, there is a dynamically sized cpumask
 * at the end of the structure, the size of which depends on the maximum CPU
 * number the system can see. That way we allocate only as much memory for
 * mm_cpumask() as needed for the hundreds, or thousands of processes that
 * a system typically runs.
 *
 * Since there is only one init_mm in the entire system, keep it simple
 * and size this cpu_bitmask to NR_CPUS.
 */
// 内核主⻚表在系统初始化的时候被⼀段汇编代码 arch\x86\kernel\head_64.S 所创建。
// 后续在系统启动函数 start_kernel 中调⽤ setup_arch 进⾏初始化

// 注意，普通进程在内核态亦或是内核线程都是无法直接访问内核主页表的，
// 它们只能访问内核主页表的 copy 副本，于是进程页表体系就分为了两个部分，
// 一个是进程用户态页表（用户态缺页处理的就是这部分），
// 另一个就是内核页表的 copy 部分（内核态缺页处理的是这部分）。

// 在 fork 系统调用创建进程的时候，进程的用户态页表拷贝自他的父进程，
// 而进程的内核态页表则从内核主页表中拷贝，后续进程陷入内核态之后，
// 访问的就是内核主页表中拷贝的这部分。
struct mm_struct init_mm = {
	.mm_rb		= RB_ROOT,
	.pgd		= swapper_pg_dir,   // 内核主⻚表
	.mm_users	= ATOMIC_INIT(2),
	.mm_count	= ATOMIC_INIT(1),
	.write_protect_seq = SEQCNT_ZERO(init_mm.write_protect_seq),
	MMAP_LOCK_INITIALIZER(init_mm)
	.page_table_lock =  __SPIN_LOCK_UNLOCKED(init_mm.page_table_lock),
	.arg_lock	=  __SPIN_LOCK_UNLOCKED(init_mm.arg_lock),
	.mmlist		= LIST_HEAD_INIT(init_mm.mmlist),
	.user_ns	= &init_user_ns,
	.cpu_bitmap	= CPU_BITS_NONE,
	INIT_MM_CONTEXT(init_mm)
};
