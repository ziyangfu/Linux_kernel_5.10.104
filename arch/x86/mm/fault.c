// SPDX-License-Identifier: GPL-2.0
/*
 *  Copyright (C) 1995  Linus Torvalds
 *  Copyright (C) 2001, 2002 Andi Kleen, SuSE Labs.
 *  Copyright (C) 2008-2009, Red Hat Inc., Ingo Molnar
 */
#include <linux/sched.h>		/* test_thread_flag(), ...	*/
#include <linux/sched/task_stack.h>	/* task_stack_*(), ...		*/
#include <linux/kdebug.h>		/* oops_begin/end, ...		*/
#include <linux/extable.h>		/* search_exception_tables	*/
#include <linux/memblock.h>		/* max_low_pfn			*/
#include <linux/kprobes.h>		/* NOKPROBE_SYMBOL, ...		*/
#include <linux/mmiotrace.h>		/* kmmio_handler, ...		*/
#include <linux/perf_event.h>		/* perf_sw_event		*/
#include <linux/hugetlb.h>		/* hstate_index_to_shift	*/
#include <linux/prefetch.h>		/* prefetchw			*/
#include <linux/context_tracking.h>	/* exception_enter(), ...	*/
#include <linux/uaccess.h>		/* faulthandler_disabled()	*/
#include <linux/efi.h>			/* efi_recover_from_page_fault()*/
#include <linux/mm_types.h>

#include <asm/cpufeature.h>		/* boot_cpu_has, ...		*/
#include <asm/traps.h>			/* dotraplinkage, ...		*/
#include <asm/fixmap.h>			/* VSYSCALL_ADDR		*/
#include <asm/vsyscall.h>		/* emulate_vsyscall		*/
#include <asm/vm86.h>			/* struct vm86			*/
#include <asm/mmu_context.h>		/* vma_pkey()			*/
#include <asm/efi.h>			/* efi_recover_from_page_fault()*/
#include <asm/desc.h>			/* store_idt(), ...		*/
#include <asm/cpu_entry_area.h>		/* exception stack		*/
#include <asm/pgtable_areas.h>		/* VMALLOC_START, ...		*/
#include <asm/kvm_para.h>		/* kvm_handle_async_pf		*/

#define CREATE_TRACE_POINTS
#include <asm/trace/exceptions.h>

/*
 * Returns 0 if mmiotrace is disabled, or if the fault is not
 * handled by mmiotrace:
 */
static nokprobe_inline int
kmmio_fault(struct pt_regs *regs, unsigned long addr)
{
	if (unlikely(is_kmmio_active()))
		if (kmmio_handler(regs, addr) == 1)
			return -1;
	return 0;
}

/*
 * Prefetch quirks:
 *
 * 32-bit mode:
 *
 *   Sometimes AMD Athlon/Opteron CPUs report invalid exceptions on prefetch.
 *   Check that here and ignore it.  This is AMD erratum #91.
 *
 * 64-bit mode:
 *
 *   Sometimes the CPU reports invalid exceptions on prefetch.
 *   Check that here and ignore it.
 *
 * Opcode checker based on code by Richard Brunner.
 */
static inline int
check_prefetch_opcode(struct pt_regs *regs, unsigned char *instr,
		      unsigned char opcode, int *prefetch)
{
	unsigned char instr_hi = opcode & 0xf0;
	unsigned char instr_lo = opcode & 0x0f;

	switch (instr_hi) {
	case 0x20:
	case 0x30:
		/*
		 * Values 0x26,0x2E,0x36,0x3E are valid x86 prefixes.
		 * In X86_64 long mode, the CPU will signal invalid
		 * opcode if some of these prefixes are present so
		 * X86_64 will never get here anyway
		 */
		return ((instr_lo & 7) == 0x6);
#ifdef CONFIG_X86_64
	case 0x40:
		/*
		 * In 64-bit mode 0x40..0x4F are valid REX prefixes
		 */
		return (!user_mode(regs) || user_64bit_mode(regs));
#endif
	case 0x60:
		/* 0x64 thru 0x67 are valid prefixes in all modes. */
		return (instr_lo & 0xC) == 0x4;
	case 0xF0:
		/* 0xF0, 0xF2, 0xF3 are valid prefixes in all modes. */
		return !instr_lo || (instr_lo>>1) == 1;
	case 0x00:
		/* Prefetch instruction is 0x0F0D or 0x0F18 */
		if (get_kernel_nofault(opcode, instr))
			return 0;

		*prefetch = (instr_lo == 0xF) &&
			(opcode == 0x0D || opcode == 0x18);
		return 0;
	default:
		return 0;
	}
}

static int
is_prefetch(struct pt_regs *regs, unsigned long error_code, unsigned long addr)
{
	unsigned char *max_instr;
	unsigned char *instr;
	int prefetch = 0;

	/*
	 * If it was a exec (instruction fetch) fault on NX page, then
	 * do not ignore the fault:
	 */
	if (error_code & X86_PF_INSTR)
		return 0;

	instr = (void *)convert_ip_to_linear(current, regs);
	max_instr = instr + 15;

	/*
	 * This code has historically always bailed out if IP points to a
	 * not-present page (e.g. due to a race).  No one has ever
	 * complained about this.
	 */
	pagefault_disable();

	while (instr < max_instr) {
		unsigned char opcode;

		if (user_mode(regs)) {
			if (get_user(opcode, instr))
				break;
		} else {
			if (get_kernel_nofault(opcode, instr))
				break;
		}

		instr++;

		if (!check_prefetch_opcode(regs, instr, opcode, &prefetch))
			break;
	}

	pagefault_enable();
	return prefetch;
}

DEFINE_SPINLOCK(pgd_lock);
LIST_HEAD(pgd_list);

#ifdef CONFIG_X86_32
static inline pmd_t *vmalloc_sync_one(pgd_t *pgd, unsigned long address)
{
	unsigned index = pgd_index(address);
	pgd_t *pgd_k;
	p4d_t *p4d, *p4d_k;
	pud_t *pud, *pud_k;
	pmd_t *pmd, *pmd_k;

	pgd += index;
	pgd_k = init_mm.pgd + index;

	if (!pgd_present(*pgd_k))
		return NULL;

	/*
	 * set_pgd(pgd, *pgd_k); here would be useless on PAE
	 * and redundant with the set_pmd() on non-PAE. As would
	 * set_p4d/set_pud.
	 */
	p4d = p4d_offset(pgd, address);
	p4d_k = p4d_offset(pgd_k, address);
	if (!p4d_present(*p4d_k))
		return NULL;

	pud = pud_offset(p4d, address);
	pud_k = pud_offset(p4d_k, address);
	if (!pud_present(*pud_k))
		return NULL;

	pmd = pmd_offset(pud, address);
	pmd_k = pmd_offset(pud_k, address);

	if (pmd_present(*pmd) != pmd_present(*pmd_k))
		set_pmd(pmd, *pmd_k);

	if (!pmd_present(*pmd_k))
		return NULL;
	else
		BUG_ON(pmd_pfn(*pmd) != pmd_pfn(*pmd_k));

	return pmd_k;
}

/*
 *   Handle a fault on the vmalloc or module mapping area
 *
 *   This is needed because there is a race condition between the time
 *   when the vmalloc mapping code updates the PMD to the point in time
 *   where it synchronizes this update with the other page-tables in the
 *   system.
 *
 *   In this race window another thread/CPU can map an area on the same
 *   PMD, finds it already present and does not synchronize it with the
 *   rest of the system yet. As a result v[mz]alloc might return areas
 *   which are not mapped in every page-table in the system, causing an
 *   unhandled page-fault when they are accessed.
 */
/**
 * \details
 * 内核通过 vmalloc 内存分配接⼝在 vmalloc 映射区申请内存的时候，⾸先也会在 32T
 * ⼤⼩的 vmalloc 映射区中划分出⼀段未被使⽤的虚拟内存区域出来，我们暂且叫这段虚拟
 * 内存区域为 vmalloc 区，这⼀点和前⾯⽂章介绍的 mmap ⾮常相似，只不过 mmap ⼯作
 * 在⽤户空间的⽂件与匿名映射区，vmalloc ⼯作在内核空间的 vmalloc 映射区
 * 
 * 内核空间中的 vmalloc 映射区就是由这样⼀段⼀段的 vmalloc 区组成的，每调⽤⼀次
 * vmalloc 内存分配接⼝，就会在 vmalloc 映射区中映射出⼀段 vmalloc 虚拟内存区域，
 * ⽽且每个 vmalloc 区之间隔着⼀个 4K ⼤⼩的 guard page（虚拟内存），⽤于防⽌内存
 * 越界，将这些⾮连续的物理内存区域隔离起来
 * 
 * 和 mmap 不同的是，vmalloc 在分配完虚拟内存之后，会⻢上为这段虚拟内存分配物理内存
*/
static noinline int vmalloc_fault(unsigned long address)
{
	unsigned long pgd_paddr;
	pmd_t *pmd_k;
	pte_t *pte_k;

	/* Make sure we are in vmalloc area: */
	// 确保缺⻚发⽣在内核 vmalloc 动态映射区
	if (!(address >= VMALLOC_START && address < VMALLOC_END))
		return -1;

	/*
	 * Synchronize this task's top level page-table
	 * with the 'reference' page table.
	 *
	 * Do _not_ use "current" here. We might be inside
	 * an interrupt in the middle of a task switch..
	 */
	// 从CR3寄存器中获取页全局目录地址
	pgd_paddr = read_cr3_pa();
	// 获取页中间目录
	pmd_k = vmalloc_sync_one(__va(pgd_paddr), address);
	if (!pmd_k)
		return -1;

	if (pmd_large(*pmd_k))
		return 0;
	// 获取页表
	pte_k = pte_offset_kernel(pmd_k, address);
	if (!pte_present(*pte_k))
		return -1;

	return 0;
}
NOKPROBE_SYMBOL(vmalloc_fault);

void arch_sync_kernel_mappings(unsigned long start, unsigned long end)
{
	unsigned long addr;

	for (addr = start & PMD_MASK;
	     addr >= TASK_SIZE_MAX && addr < VMALLOC_END;
	     addr += PMD_SIZE) {
		struct page *page;

		spin_lock(&pgd_lock);
		list_for_each_entry(page, &pgd_list, lru) {
			spinlock_t *pgt_lock;

			/* the pgt_lock only for Xen */
			pgt_lock = &pgd_page_get_mm(page)->page_table_lock;

			spin_lock(pgt_lock);
			vmalloc_sync_one(page_address(page), addr);
			spin_unlock(pgt_lock);
		}
		spin_unlock(&pgd_lock);
	}
}

/*
 * Did it hit the DOS screen memory VA from vm86 mode?
 */
static inline void
check_v8086_mode(struct pt_regs *regs, unsigned long address,
		 struct task_struct *tsk)
{
#ifdef CONFIG_VM86
	unsigned long bit;

	if (!v8086_mode(regs) || !tsk->thread.vm86)
		return;

	bit = (address - 0xA0000) >> PAGE_SHIFT;
	if (bit < 32)
		tsk->thread.vm86->screen_bitmap |= 1 << bit;
#endif
}

static bool low_pfn(unsigned long pfn)
{
	return pfn < max_low_pfn;
}

static void dump_pagetable(unsigned long address)
{
	pgd_t *base = __va(read_cr3_pa());
	pgd_t *pgd = &base[pgd_index(address)];
	p4d_t *p4d;
	pud_t *pud;
	pmd_t *pmd;
	pte_t *pte;

#ifdef CONFIG_X86_PAE
	pr_info("*pdpt = %016Lx ", pgd_val(*pgd));
	if (!low_pfn(pgd_val(*pgd) >> PAGE_SHIFT) || !pgd_present(*pgd))
		goto out;
#define pr_pde pr_cont
#else
#define pr_pde pr_info
#endif
	p4d = p4d_offset(pgd, address);
	pud = pud_offset(p4d, address);
	pmd = pmd_offset(pud, address);
	pr_pde("*pde = %0*Lx ", sizeof(*pmd) * 2, (u64)pmd_val(*pmd));
#undef pr_pde

	/*
	 * We must not directly access the pte in the highpte
	 * case if the page table is located in highmem.
	 * And let's rather not kmap-atomic the pte, just in case
	 * it's allocated already:
	 */
	if (!low_pfn(pmd_pfn(*pmd)) || !pmd_present(*pmd) || pmd_large(*pmd))
		goto out;

	pte = pte_offset_kernel(pmd, address);
	pr_cont("*pte = %0*Lx ", sizeof(*pte) * 2, (u64)pte_val(*pte));
out:
	pr_cont("\n");
}

#else /* CONFIG_X86_64: */

#ifdef CONFIG_CPU_SUP_AMD
static const char errata93_warning[] =
KERN_ERR 
"******* Your BIOS seems to not contain a fix for K8 errata #93\n"
"******* Working around it, but it may cause SEGVs or burn power.\n"
"******* Please consider a BIOS update.\n"
"******* Disabling USB legacy in the BIOS may also help.\n";
#endif

/*
 * No vm86 mode in 64-bit mode:
 */
static inline void
check_v8086_mode(struct pt_regs *regs, unsigned long address,
		 struct task_struct *tsk)
{
}

static int bad_address(void *p)
{
	unsigned long dummy;

	return get_kernel_nofault(dummy, (unsigned long *)p);
}

static void dump_pagetable(unsigned long address)
{
	pgd_t *base = __va(read_cr3_pa());
	pgd_t *pgd = base + pgd_index(address);
	p4d_t *p4d;
	pud_t *pud;
	pmd_t *pmd;
	pte_t *pte;

	if (bad_address(pgd))
		goto bad;

	pr_info("PGD %lx ", pgd_val(*pgd));

	if (!pgd_present(*pgd))
		goto out;

	p4d = p4d_offset(pgd, address);
	if (bad_address(p4d))
		goto bad;

	pr_cont("P4D %lx ", p4d_val(*p4d));
	if (!p4d_present(*p4d) || p4d_large(*p4d))
		goto out;

	pud = pud_offset(p4d, address);
	if (bad_address(pud))
		goto bad;

	pr_cont("PUD %lx ", pud_val(*pud));
	if (!pud_present(*pud) || pud_large(*pud))
		goto out;

	pmd = pmd_offset(pud, address);
	if (bad_address(pmd))
		goto bad;

	pr_cont("PMD %lx ", pmd_val(*pmd));
	if (!pmd_present(*pmd) || pmd_large(*pmd))
		goto out;

	pte = pte_offset_kernel(pmd, address);
	if (bad_address(pte))
		goto bad;

	pr_cont("PTE %lx", pte_val(*pte));
out:
	pr_cont("\n");
	return;
bad:
	pr_info("BAD\n");
}

#endif /* CONFIG_X86_64 */

/*
 * Workaround for K8 erratum #93 & buggy BIOS.
 *
 * BIOS SMM functions are required to use a specific workaround
 * to avoid corruption of the 64bit RIP register on C stepping K8.
 *
 * A lot of BIOS that didn't get tested properly miss this.
 *
 * The OS sees this as a page fault with the upper 32bits of RIP cleared.
 * Try to work around it here.
 *
 * Note we only handle faults in kernel here.
 * Does nothing on 32-bit.
 */
static int is_errata93(struct pt_regs *regs, unsigned long address)
{
#if defined(CONFIG_X86_64) && defined(CONFIG_CPU_SUP_AMD)
	if (boot_cpu_data.x86_vendor != X86_VENDOR_AMD
	    || boot_cpu_data.x86 != 0xf)
		return 0;

	if (address != regs->ip)
		return 0;

	if ((address >> 32) != 0)
		return 0;

	address |= 0xffffffffUL << 32;
	if ((address >= (u64)_stext && address <= (u64)_etext) ||
	    (address >= MODULES_VADDR && address <= MODULES_END)) {
		printk_once(errata93_warning);
		regs->ip = address;
		return 1;
	}
#endif
	return 0;
}

/*
 * Work around K8 erratum #100 K8 in compat mode occasionally jumps
 * to illegal addresses >4GB.
 *
 * We catch this in the page fault handler because these addresses
 * are not reachable. Just detect this case and return.  Any code
 * segment in LDT is compatibility mode.
 */
static int is_errata100(struct pt_regs *regs, unsigned long address)
{
#ifdef CONFIG_X86_64
	if ((regs->cs == __USER32_CS || (regs->cs & (1<<2))) && (address >> 32))
		return 1;
#endif
	return 0;
}

/* Pentium F0 0F C7 C8 bug workaround: */
static int is_f00f_bug(struct pt_regs *regs, unsigned long address)
{
#ifdef CONFIG_X86_F00F_BUG
	if (boot_cpu_has_bug(X86_BUG_F00F) && idt_is_f00f_address(address)) {
		handle_invalid_op(regs);
		return 1;
	}
#endif
	return 0;
}

static void show_ldttss(const struct desc_ptr *gdt, const char *name, u16 index)
{
	u32 offset = (index >> 3) * sizeof(struct desc_struct);
	unsigned long addr;
	struct ldttss_desc desc;

	if (index == 0) {
		pr_alert("%s: NULL\n", name);
		return;
	}

	if (offset + sizeof(struct ldttss_desc) >= gdt->size) {
		pr_alert("%s: 0x%hx -- out of bounds\n", name, index);
		return;
	}

	if (copy_from_kernel_nofault(&desc, (void *)(gdt->address + offset),
			      sizeof(struct ldttss_desc))) {
		pr_alert("%s: 0x%hx -- GDT entry is not readable\n",
			 name, index);
		return;
	}

	addr = desc.base0 | (desc.base1 << 16) | ((unsigned long)desc.base2 << 24);
#ifdef CONFIG_X86_64
	addr |= ((u64)desc.base3 << 32);
#endif
	pr_alert("%s: 0x%hx -- base=0x%lx limit=0x%x\n",
		 name, index, addr, (desc.limit0 | (desc.limit1 << 16)));
}

static void
show_fault_oops(struct pt_regs *regs, unsigned long error_code, unsigned long address)
{
	if (!oops_may_print())
		return;

	if (error_code & X86_PF_INSTR) {
		unsigned int level;
		pgd_t *pgd;
		pte_t *pte;

		pgd = __va(read_cr3_pa());
		pgd += pgd_index(address);

		pte = lookup_address_in_pgd(pgd, address, &level);

		if (pte && pte_present(*pte) && !pte_exec(*pte))
			pr_crit("kernel tried to execute NX-protected page - exploit attempt? (uid: %d)\n",
				from_kuid(&init_user_ns, current_uid()));
		if (pte && pte_present(*pte) && pte_exec(*pte) &&
				(pgd_flags(*pgd) & _PAGE_USER) &&
				(__read_cr4() & X86_CR4_SMEP))
			pr_crit("unable to execute userspace code (SMEP?) (uid: %d)\n",
				from_kuid(&init_user_ns, current_uid()));
	}

	if (address < PAGE_SIZE && !user_mode(regs))
		pr_alert("BUG: kernel NULL pointer dereference, address: %px\n",
			(void *)address);
	else
		pr_alert("BUG: unable to handle page fault for address: %px\n",
			(void *)address);

	pr_alert("#PF: %s %s in %s mode\n",
		 (error_code & X86_PF_USER)  ? "user" : "supervisor",
		 (error_code & X86_PF_INSTR) ? "instruction fetch" :
		 (error_code & X86_PF_WRITE) ? "write access" :
					       "read access",
			     user_mode(regs) ? "user" : "kernel");
	pr_alert("#PF: error_code(0x%04lx) - %s\n", error_code,
		 !(error_code & X86_PF_PROT) ? "not-present page" :
		 (error_code & X86_PF_RSVD)  ? "reserved bit violation" :
		 (error_code & X86_PF_PK)    ? "protection keys violation" :
					       "permissions violation");

	if (!(error_code & X86_PF_USER) && user_mode(regs)) {
		struct desc_ptr idt, gdt;
		u16 ldtr, tr;

		/*
		 * This can happen for quite a few reasons.  The more obvious
		 * ones are faults accessing the GDT, or LDT.  Perhaps
		 * surprisingly, if the CPU tries to deliver a benign or
		 * contributory exception from user code and gets a page fault
		 * during delivery, the page fault can be delivered as though
		 * it originated directly from user code.  This could happen
		 * due to wrong permissions on the IDT, GDT, LDT, TSS, or
		 * kernel or IST stack.
		 */
		store_idt(&idt);

		/* Usable even on Xen PV -- it's just slow. */
		native_store_gdt(&gdt);

		pr_alert("IDT: 0x%lx (limit=0x%hx) GDT: 0x%lx (limit=0x%hx)\n",
			 idt.address, idt.size, gdt.address, gdt.size);

		store_ldt(ldtr);
		show_ldttss(&gdt, "LDTR", ldtr);

		store_tr(tr);
		show_ldttss(&gdt, "TR", tr);
	}

	dump_pagetable(address);
}

static noinline void
pgtable_bad(struct pt_regs *regs, unsigned long error_code,
	    unsigned long address)
{
	struct task_struct *tsk;
	unsigned long flags;
	int sig;

	flags = oops_begin();
	tsk = current;
	sig = SIGKILL;

	printk(KERN_ALERT "%s: Corrupted page table at address %lx\n",
	       tsk->comm, address);
	dump_pagetable(address);

	if (__die("Bad pagetable", regs, error_code))
		sig = 0;

	oops_end(flags, regs, sig);
}

static void set_signal_archinfo(unsigned long address,
				unsigned long error_code)
{
	struct task_struct *tsk = current;

	/*
	 * To avoid leaking information about the kernel page
	 * table layout, pretend that user-mode accesses to
	 * kernel addresses are always protection faults.
	 *
	 * NB: This means that failed vsyscalls with vsyscall=none
	 * will have the PROT bit.  This doesn't leak any
	 * information and does not appear to cause any problems.
	 */
	if (address >= TASK_SIZE_MAX)
		error_code |= X86_PF_PROT;

	tsk->thread.trap_nr = X86_TRAP_PF;
	tsk->thread.error_code = error_code | X86_PF_USER;
	tsk->thread.cr2 = address;
}

static noinline void
no_context(struct pt_regs *regs, unsigned long error_code,
	   unsigned long address, int signal, int si_code)
{
	struct task_struct *tsk = current;
	unsigned long flags;
	int sig;

	if (user_mode(regs)) {
		/*
		 * This is an implicit supervisor-mode access from user
		 * mode.  Bypass all the kernel-mode recovery code and just
		 * OOPS.
		 */
		goto oops;
	}

	/* Are we prepared to handle this kernel fault? */
	if (fixup_exception(regs, X86_TRAP_PF, error_code, address)) {
		/*
		 * Any interrupt that takes a fault gets the fixup. This makes
		 * the below recursive fault logic only apply to a faults from
		 * task context.
		 */
		if (in_interrupt())
			return;

		/*
		 * Per the above we're !in_interrupt(), aka. task context.
		 *
		 * In this case we need to make sure we're not recursively
		 * faulting through the emulate_vsyscall() logic.
		 */
		if (current->thread.sig_on_uaccess_err && signal) {
			set_signal_archinfo(address, error_code);

			/* XXX: hwpoison faults will set the wrong code. */
			force_sig_fault(signal, si_code, (void __user *)address);
		}

		/*
		 * Barring that, we can do the fixup and be happy.
		 */
		return;
	}

#ifdef CONFIG_VMAP_STACK
	/*
	 * Stack overflow?  During boot, we can fault near the initial
	 * stack in the direct map, but that's not an overflow -- check
	 * that we're in vmalloc space to avoid this.
	 */
	if (is_vmalloc_addr((void *)address) &&
	    (((unsigned long)tsk->stack - 1 - address < PAGE_SIZE) ||
	     address - ((unsigned long)tsk->stack + THREAD_SIZE) < PAGE_SIZE)) {
		unsigned long stack = __this_cpu_ist_top_va(DF) - sizeof(void *);
		/*
		 * We're likely to be running with very little stack space
		 * left.  It's plausible that we'd hit this condition but
		 * double-fault even before we get this far, in which case
		 * we're fine: the double-fault handler will deal with it.
		 *
		 * We don't want to make it all the way into the oops code
		 * and then double-fault, though, because we're likely to
		 * break the console driver and lose most of the stack dump.
		 */
		asm volatile ("movq %[stack], %%rsp\n\t"
			      "call handle_stack_overflow\n\t"
			      "1: jmp 1b"
			      : ASM_CALL_CONSTRAINT
			      : "D" ("kernel stack overflow (page fault)"),
				"S" (regs), "d" (address),
				[stack] "rm" (stack));
		unreachable();
	}
#endif

	/*
	 * 32-bit:
	 *
	 *   Valid to do another page fault here, because if this fault
	 *   had been triggered by is_prefetch fixup_exception would have
	 *   handled it.
	 *
	 * 64-bit:
	 *
	 *   Hall of shame of CPU/BIOS bugs.
	 */
	if (is_prefetch(regs, error_code, address))
		return;

	if (is_errata93(regs, address))
		return;

	/*
	 * Buggy firmware could access regions which might page fault, try to
	 * recover from such faults.
	 */
	if (IS_ENABLED(CONFIG_EFI))
		efi_recover_from_page_fault(address);

oops:
	/*
	 * Oops. The kernel tried to access some bad page. We'll have to
	 * terminate things with extreme prejudice:
	 */
	flags = oops_begin();

	show_fault_oops(regs, error_code, address);

	if (task_stack_end_corrupted(tsk))
		printk(KERN_EMERG "Thread overran stack, or stack corrupted\n");

	sig = SIGKILL;
	if (__die("Oops", regs, error_code))
		sig = 0;

	/* Executive summary in case the body of the oops scrolled away */
	printk(KERN_DEFAULT "CR2: %016lx\n", address);

	oops_end(flags, regs, sig);
}

/*
 * Print out info about fatal segfaults, if the show_unhandled_signals
 * sysctl is set:
 */
static inline void
show_signal_msg(struct pt_regs *regs, unsigned long error_code,
		unsigned long address, struct task_struct *tsk)
{
	const char *loglvl = task_pid_nr(tsk) > 1 ? KERN_INFO : KERN_EMERG;

	if (!unhandled_signal(tsk, SIGSEGV))
		return;

	if (!printk_ratelimit())
		return;

	printk("%s%s[%d]: segfault at %lx ip %px sp %px error %lx",
		loglvl, tsk->comm, task_pid_nr(tsk), address,
		(void *)regs->ip, (void *)regs->sp, error_code);

	print_vma_addr(KERN_CONT " in ", regs->ip);

	printk(KERN_CONT "\n");

	show_opcodes(regs, loglvl);
}

/*
 * The (legacy) vsyscall page is the long page in the kernel portion
 * of the address space that has user-accessible permissions.
 */
static bool is_vsyscall_vaddr(unsigned long vaddr)
{
	return unlikely((vaddr & PAGE_MASK) == VSYSCALL_ADDR);
}

static void
__bad_area_nosemaphore(struct pt_regs *regs, unsigned long error_code,
		       unsigned long address, u32 pkey, int si_code)
{
	struct task_struct *tsk = current;

	/* User mode accesses just cause a SIGSEGV */
	if (user_mode(regs) && (error_code & X86_PF_USER)) {
		/*
		 * It's possible to have interrupts off here:
		 */
		local_irq_enable();

		/*
		 * Valid to do another page fault here because this one came
		 * from user space:
		 */
		if (is_prefetch(regs, error_code, address))
			return;

		if (is_errata100(regs, address))
			return;

		/*
		 * To avoid leaking information about the kernel page table
		 * layout, pretend that user-mode accesses to kernel addresses
		 * are always protection faults.
		 */
		if (address >= TASK_SIZE_MAX)
			error_code |= X86_PF_PROT;

		if (likely(show_unhandled_signals))
			show_signal_msg(regs, error_code, address, tsk);

		set_signal_archinfo(address, error_code);

		if (si_code == SEGV_PKUERR)
			force_sig_pkuerr((void __user *)address, pkey);

		force_sig_fault(SIGSEGV, si_code, (void __user *)address);

		local_irq_disable();

		return;
	}

	if (is_f00f_bug(regs, address))
		return;

	no_context(regs, error_code, address, SIGSEGV, si_code);
}

static noinline void
bad_area_nosemaphore(struct pt_regs *regs, unsigned long error_code,
		     unsigned long address)
{
	__bad_area_nosemaphore(regs, error_code, address, 0, SEGV_MAPERR);
}

static void
__bad_area(struct pt_regs *regs, unsigned long error_code,
	   unsigned long address, u32 pkey, int si_code)
{
	struct mm_struct *mm = current->mm;
	/*
	 * Something tried to access memory that isn't in our memory map..
	 * Fix it, but check if it's kernel or user first..
	 */
	mmap_read_unlock(mm);

	__bad_area_nosemaphore(regs, error_code, address, pkey, si_code);
}

static noinline void
bad_area(struct pt_regs *regs, unsigned long error_code, unsigned long address)
{
	__bad_area(regs, error_code, address, 0, SEGV_MAPERR);
}

static inline bool bad_area_access_from_pkeys(unsigned long error_code,
		struct vm_area_struct *vma)
{
	/* This code is always called on the current mm */
	bool foreign = false;

	if (!boot_cpu_has(X86_FEATURE_OSPKE))
		return false;
	if (error_code & X86_PF_PK)
		return true;
	/* this checks permission keys on the VMA: */
	if (!arch_vma_access_permitted(vma, (error_code & X86_PF_WRITE),
				       (error_code & X86_PF_INSTR), foreign))
		return true;
	return false;
}

static noinline void
bad_area_access_error(struct pt_regs *regs, unsigned long error_code,
		      unsigned long address, struct vm_area_struct *vma)
{
	/*
	 * This OSPKE check is not strictly necessary at runtime.
	 * But, doing it this way allows compiler optimizations
	 * if pkeys are compiled out.
	 */
	if (bad_area_access_from_pkeys(error_code, vma)) {
		/*
		 * A protection key fault means that the PKRU value did not allow
		 * access to some PTE.  Userspace can figure out what PKRU was
		 * from the XSAVE state.  This function captures the pkey from
		 * the vma and passes it to userspace so userspace can discover
		 * which protection key was set on the PTE.
		 *
		 * If we get here, we know that the hardware signaled a X86_PF_PK
		 * fault and that there was a VMA once we got in the fault
		 * handler.  It does *not* guarantee that the VMA we find here
		 * was the one that we faulted on.
		 *
		 * 1. T1   : mprotect_key(foo, PAGE_SIZE, pkey=4);
		 * 2. T1   : set PKRU to deny access to pkey=4, touches page
		 * 3. T1   : faults...
		 * 4.    T2: mprotect_key(foo, PAGE_SIZE, pkey=5);
		 * 5. T1   : enters fault handler, takes mmap_lock, etc...
		 * 6. T1   : reaches here, sees vma_pkey(vma)=5, when we really
		 *	     faulted on a pte with its pkey=4.
		 */
		u32 pkey = vma_pkey(vma);

		__bad_area(regs, error_code, address, pkey, SEGV_PKUERR);
	} else {
		__bad_area(regs, error_code, address, 0, SEGV_ACCERR);
	}
}

static void
do_sigbus(struct pt_regs *regs, unsigned long error_code, unsigned long address,
	  vm_fault_t fault)
{
	/* Kernel mode? Handle exceptions or die: */
	if (!(error_code & X86_PF_USER)) {
		no_context(regs, error_code, address, SIGBUS, BUS_ADRERR);
		return;
	}

	/* User-space => ok to do another page fault: */
	if (is_prefetch(regs, error_code, address))
		return;

	set_signal_archinfo(address, error_code);

#ifdef CONFIG_MEMORY_FAILURE
	if (fault & (VM_FAULT_HWPOISON|VM_FAULT_HWPOISON_LARGE)) {
		struct task_struct *tsk = current;
		unsigned lsb = 0;

		pr_err(
	"MCE: Killing %s:%d due to hardware memory corruption fault at %lx\n",
			tsk->comm, tsk->pid, address);
		if (fault & VM_FAULT_HWPOISON_LARGE)
			lsb = hstate_index_to_shift(VM_FAULT_GET_HINDEX(fault));
		if (fault & VM_FAULT_HWPOISON)
			lsb = PAGE_SHIFT;
		force_sig_mceerr(BUS_MCEERR_AR, (void __user *)address, lsb);
		return;
	}
#endif
	force_sig_fault(SIGBUS, BUS_ADRERR, (void __user *)address);
}

static noinline void
mm_fault_error(struct pt_regs *regs, unsigned long error_code,
	       unsigned long address, vm_fault_t fault)
{
	if (fatal_signal_pending(current) && !(error_code & X86_PF_USER)) {
		no_context(regs, error_code, address, 0, 0);
		return;
	}

	if (fault & VM_FAULT_OOM) {
		/* Kernel mode? Handle exceptions or die: */
		if (!(error_code & X86_PF_USER)) {
			no_context(regs, error_code, address,
				   SIGSEGV, SEGV_MAPERR);
			return;
		}

		/*
		 * We ran out of memory, call the OOM killer, and return the
		 * userspace (which will retry the fault, or kill us if we got
		 * oom-killed):
		 */
		pagefault_out_of_memory();
	} else {
		if (fault & (VM_FAULT_SIGBUS|VM_FAULT_HWPOISON|
			     VM_FAULT_HWPOISON_LARGE))
			do_sigbus(regs, error_code, address, fault);
		else if (fault & VM_FAULT_SIGSEGV)
			bad_area_nosemaphore(regs, error_code, address);
		else
			BUG();
	}
}

static int spurious_kernel_fault_check(unsigned long error_code, pte_t *pte)
{
	if ((error_code & X86_PF_WRITE) && !pte_write(*pte))
		return 0;

	if ((error_code & X86_PF_INSTR) && !pte_exec(*pte))
		return 0;

	return 1;
}

/*
 * Handle a spurious fault caused by a stale TLB entry.
 *
 * This allows us to lazily refresh the TLB when increasing the
 * permissions of a kernel page (RO -> RW or NX -> X).  Doing it
 * eagerly is very expensive since that implies doing a full
 * cross-processor TLB flush, even if no stale TLB entries exist
 * on other processors.
 *
 * Spurious faults may only occur if the TLB contains an entry with
 * fewer permission than the page table entry.  Non-present (P = 0)
 * and reserved bit (R = 1) faults are never spurious.
 *
 * There are no security implications to leaving a stale TLB when
 * increasing the permissions on a page.
 *
 * Returns non-zero if a spurious fault was handled, zero otherwise.
 *
 * See Intel Developer's Manual Vol 3 Section 4.10.4.3, bullet 3
 * (Optional Invalidation).
 */
static noinline int
spurious_kernel_fault(unsigned long error_code, unsigned long address)
{
	pgd_t *pgd;
	p4d_t *p4d;
	pud_t *pud;
	pmd_t *pmd;
	pte_t *pte;
	int ret;

	/*
	 * Only writes to RO or instruction fetches from NX may cause
	 * spurious faults.
	 *
	 * These could be from user or supervisor accesses but the TLB
	 * is only lazily flushed after a kernel mapping protection
	 * change, so user accesses are not expected to cause spurious
	 * faults.
	 */
	if (error_code != (X86_PF_WRITE | X86_PF_PROT) &&
	    error_code != (X86_PF_INSTR | X86_PF_PROT))
		return 0;

	pgd = init_mm.pgd + pgd_index(address);
	if (!pgd_present(*pgd))
		return 0;

	p4d = p4d_offset(pgd, address);
	if (!p4d_present(*p4d))
		return 0;

	if (p4d_large(*p4d))
		return spurious_kernel_fault_check(error_code, (pte_t *) p4d);

	pud = pud_offset(p4d, address);
	if (!pud_present(*pud))
		return 0;

	if (pud_large(*pud))
		return spurious_kernel_fault_check(error_code, (pte_t *) pud);

	pmd = pmd_offset(pud, address);
	if (!pmd_present(*pmd))
		return 0;

	if (pmd_large(*pmd))
		return spurious_kernel_fault_check(error_code, (pte_t *) pmd);

	pte = pte_offset_kernel(pmd, address);
	if (!pte_present(*pte))
		return 0;

	ret = spurious_kernel_fault_check(error_code, pte);
	if (!ret)
		return 0;

	/*
	 * Make sure we have permissions in PMD.
	 * If not, then there's a bug in the page tables:
	 */
	ret = spurious_kernel_fault_check(error_code, (pte_t *) pmd);
	WARN_ONCE(!ret, "PMD has incorrect permission bits\n");

	return ret;
}
NOKPROBE_SYMBOL(spurious_kernel_fault);

int show_unhandled_signals = 1;

static inline int
access_error(unsigned long error_code, struct vm_area_struct *vma)
{
	/* This is only called for the current mm, so: */
	bool foreign = false;

	/*
	 * Read or write was blocked by protection keys.  This is
	 * always an unconditional error and can never result in
	 * a follow-up action to resolve the fault, like a COW.
	 */
	if (error_code & X86_PF_PK)
		return 1;

	/*
	 * Make sure to check the VMA so that we do not perform
	 * faults just to hit a X86_PF_PK as soon as we fill in a
	 * page.
	 */
	if (!arch_vma_access_permitted(vma, (error_code & X86_PF_WRITE),
				       (error_code & X86_PF_INSTR), foreign))
		return 1;

	if (error_code & X86_PF_WRITE) {
		/* write, present and write, not present: */
		if (unlikely(!(vma->vm_flags & VM_WRITE)))
			return 1;
		return 0;
	}

	/* read, present: */
	if (unlikely(error_code & X86_PF_PROT))
		return 1;

	/* read, not present: */
	if (unlikely(!vma_is_accessible(vma)))
		return 1;

	return 0;
}

bool fault_in_kernel_space(unsigned long address)
{
	/*
	 * On 64-bit systems, the vsyscall page is at an address above
	 * TASK_SIZE_MAX, but is not considered part of the kernel
	 * address space.
	 */
	if (IS_ENABLED(CONFIG_X86_64) && is_vsyscall_vaddr(address))
		return false;
	// 在进程虚拟内存空间中，TASK_SIZE_MAX 以上的虚拟地址均属于内核空间
	return address >= TASK_SIZE_MAX;
}

/*
 * Called for all faults where 'address' is part of the kernel address
 * space.  Might get called for faults that originate from *code* that
 * ran in userspace or the kernel.
 */
static void
do_kern_addr_fault(struct pt_regs *regs, unsigned long hw_error_code,
		   unsigned long address)
{
	/*
	 * Protection keys exceptions only happen on user pages.  We
	 * have no user pages in the kernel portion of the address
	 * space, so do not expect them here.
	 */
	WARN_ON_ONCE(hw_error_code & X86_PF_PK);

#ifdef CONFIG_X86_32
	/*
	 * We can fault-in kernel-space virtual memory on-demand. The
	 * 'reference' page table is init_mm.pgd.
	 *
	 * NOTE! We MUST NOT take any locks for this case. We may
	 * be in an interrupt or a critical region, and should
	 * only copy the information from the master page table,
	 * nothing more.
	 *
	 * Before doing this on-demand faulting, ensure that the
	 * fault is not any of the following:
	 * 1. A fault on a PTE with a reserved bit set.
	 * 2. A fault caused by a user-mode access.  (Do not demand-
	 *    fault kernel memory due to user-mode accesses).
	 * 3. A fault caused by a page-level protection violation.
	 *    (A demand fault would be on a non-present page which
	 *     would have X86_PF_PROT==0).
	 *
	 * This is only needed to close a race condition on x86-32 in
	 * the vmalloc mapping/unmapping code. See the comment above
	 * vmalloc_fault() for details. On x86-64 the race does not
	 * exist as the vmalloc mappings don't need to be synchronized
	 * there.
	 */
	// 该缺页的内核地址 address 在内核页表中对应的 pte 不能使用保留位(X86_PF_RSVD = 0)
    // 不能是用户态的缺页中断(X86_PF_USER = 0)
    // 且不能是保护类型的缺页中断 (X86_PF_PROT = 0)
	if (!(hw_error_code & (X86_PF_RSVD | X86_PF_USER | X86_PF_PROT))) {
		// 处理 vmalloc 映射区里的缺页异常
		if (vmalloc_fault(address) >= 0)
			return;
	}
#endif

	/* Was the fault spurious, caused by lazy TLB invalidation? */
	if (spurious_kernel_fault(hw_error_code, address))
		return;

	/* kprobes don't want to hook the spurious faults: */
	if (kprobe_page_fault(regs, X86_TRAP_PF))
		return;

	/*
	 * Note, despite being a "bad area", there are quite a few
	 * acceptable reasons to get here, such as erratum fixups
	 * and handling kernel code that can fault, like get_user().
	 *
	 * Don't take the mm semaphore here. If we fixup a prefetch
	 * fault we could otherwise deadlock:
	 */
	bad_area_nosemaphore(regs, hw_error_code, address);
}
NOKPROBE_SYMBOL(do_kern_addr_fault);

/* Handle faults in the user portion of the address space */
// 用户态缺页异常处理
static inline
void do_user_addr_fault(struct pt_regs *regs,
			unsigned long hw_error_code,
			unsigned long address)
{
	struct vm_area_struct *vma;
	struct task_struct *tsk;
	struct mm_struct *mm;
	vm_fault_t fault;
	unsigned int flags = FAULT_FLAG_DEFAULT;

	tsk = current;
	mm = tsk->mm;

	/* kprobes don't want to hook the spurious【虚假的】 faults: */
	if (unlikely(kprobe_page_fault(regs, X86_TRAP_PF)))
		return;

	/*
	 * Reserved bits are never expected to be set on
	 * entries in the user portion of the page tables.
	 */
	if (unlikely(hw_error_code & X86_PF_RSVD))
		pgtable_bad(regs, hw_error_code, address);

	/*
	 * If SMAP is on, check for invalid kernel (supervisor) access to user
	 * pages in the user address space.  The odd case here is WRUSS,
	 * which, according to the preliminary documentation, does not respect
	 * SMAP and will have the USER bit set so, in all cases, SMAP
	 * enforcement appears to be consistent with the USER bit.
	 */
	if (unlikely(cpu_feature_enabled(X86_FEATURE_SMAP) &&
		     !(hw_error_code & X86_PF_USER) &&
		     !(regs->flags & X86_EFLAGS_AC)))
	{
		bad_area_nosemaphore(regs, hw_error_code, address);
		return;
	}

	/*
	 * If we're in an interrupt, have no user context or are running
	 * in a region with pagefaults disabled then we must not take the fault
	 */
	if (unlikely(faulthandler_disabled() || !mm)) {
		bad_area_nosemaphore(regs, hw_error_code, address);
		return;
	}

	/*
	 * It's safe to allow irq's after cr2 has been saved and the
	 * vmalloc fault has been handled.
	 *
	 * User-mode registers count as a user access even for any
	 * potential system fault or CPU buglet:
	 */
	if (user_mode(regs)) {
		local_irq_enable();
		flags |= FAULT_FLAG_USER;
	} else {
		if (regs->flags & X86_EFLAGS_IF)
			local_irq_enable();
	}

	perf_sw_event(PERF_COUNT_SW_PAGE_FAULTS, 1, regs, address);

	if (hw_error_code & X86_PF_WRITE)
		flags |= FAULT_FLAG_WRITE;
	if (hw_error_code & X86_PF_INSTR)
		flags |= FAULT_FLAG_INSTRUCTION;

#ifdef CONFIG_X86_64
	/*
	 * Faults in the vsyscall page might need emulation.  The
	 * vsyscall page is at a high address (>PAGE_OFFSET), but is
	 * considered to be part of the user address space.
	 *
	 * The vsyscall page does not have a "real" VMA, so do this
	 * emulation before we go searching for VMAs.
	 *
	 * PKRU never rejects instruction fetches, so we don't need
	 * to consider the PF_PK bit.
	 */
	if (is_vsyscall_vaddr(address)) {
		if (emulate_vsyscall(hw_error_code, regs, address))
			return;
	}
#endif

	/*
	 * Kernel-mode access to the user address space should only occur
	 * on well-defined single instructions listed in the exception
	 * tables.  But, an erroneous kernel fault occurring outside one of
	 * those areas which also holds mmap_lock might deadlock attempting
	 * to validate the fault against the address space.
	 *
	 * Only do the expensive exception table search when we might be at
	 * risk of a deadlock.  This happens if we
	 * 1. Failed to acquire mmap_lock, and
	 * 2. The access did not originate in userspace.
	 */
	if (unlikely(!mmap_read_trylock(mm))) {
		if (!user_mode(regs) && !search_exception_tables(regs->ip)) {
			/*
			 * Fault from code in kernel from
			 * which we do not expect faults.
			 */
			bad_area_nosemaphore(regs, hw_error_code, address);
			return;
		}
retry:
		mmap_read_lock(mm);
	} else {
		/*
		 * The above down_read_trylock() might have succeeded in
		 * which case we'll have missed the might_sleep() from
		 * down_read():
		 */
		might_sleep();
	}
	// 在进程虚拟地址空间查找第⼀个符合条件：address < vma->vm_end 的虚拟内存区域 vma
	vma = find_vma(mm, address);
	// 如果该缺⻚地址 address 后⾯没有 vma 跳转到 bad_area 处理异常
	if (unlikely(!vma)) {
		bad_area(regs, hw_error_code, address);
		return;
	}
	// 缺⻚地址 address 恰好落在⼀个 vma 中，跳转到 good_area 处理 vma 中的缺⻚
	if (likely(vma->vm_start <= address))
		goto good_area;
	// 上⾯第三种情况，vma 不是栈区，跳转到 bad_area
	// VM_GROWSDOWN表示该 vma 中的地址增长方向是由高到底了，说明这个 vma 可能是栈区域
	if (unlikely(!(vma->vm_flags & VM_GROWSDOWN))) {
		bad_area(regs, hw_error_code, address);
		return;
	}
	// vma 是栈区，尝试扩展栈区到 address 地址处
	if (unlikely(expand_stack(vma, address))) {
		bad_area(regs, hw_error_code, address);
		return;
	}

	/*
	 * Ok, we have a good vm_area for this memory access, so
	 * we can handle it..
	 */
good_area:
	if (unlikely(access_error(hw_error_code, vma))) {
		bad_area_access_error(regs, hw_error_code, address, vma);
		return;
	}

	/*
	 * If for any reason at all we couldn't handle the fault,
	 * make sure we exit gracefully【优雅的】 rather than endlessly redo
	 * the fault.  Since we never set FAULT_FLAG_RETRY_NOWAIT, if
	 * we get VM_FAULT_RETRY back, the mmap_lock has been unlocked.
	 *
	 * Note that handle_userfault() may also release and reacquire mmap_lock
	 * (and not return with VM_FAULT_RETRY), when returning to userland to
	 * repeat the page fault later with a VM_FAULT_NOPAGE retval
	 * (potentially after handling any pending signal during the return to
	 * userland). The return to userland is identified whenever
	 * FAULT_FLAG_USER|FAULT_FLAG_KILLABLE are both set in flags.
	 */
	// 处理 vma 区域的缺⻚异常，返回值 fault 是⼀个位图,通过这个位图可以简要描述⼀下在
	// 整个缺⻚异常处理的过程中究竟发⽣了哪些状况，⽅便内核对各种状况进⾏针对性处理
	fault = handle_mm_fault(vma, address, flags, regs);

	/* Quick path to respond to signals */
	if (fault_signal_pending(fault, regs)) {
		if (!user_mode(regs))
			no_context(regs, hw_error_code, address, SIGBUS,
				   BUS_ADRERR);
		return;
	}

	/*
	 * If we need to retry the mmap_lock has already been released,
	 * and if there is a fatal signal pending there is no guarantee
	 * that we made any progress. Handle this case first.
	 */
	if (unlikely((fault & VM_FAULT_RETRY) &&
		     (flags & FAULT_FLAG_ALLOW_RETRY))) {
		flags |= FAULT_FLAG_TRIED;
		goto retry;
	}

	mmap_read_unlock(mm);
	if (unlikely(fault & VM_FAULT_ERROR)) {
		mm_fault_error(regs, hw_error_code, address, fault);
		return;
	}

	check_v8086_mode(regs, address, tsk);
}
NOKPROBE_SYMBOL(do_user_addr_fault);

static __always_inline void
trace_page_fault_entries(struct pt_regs *regs, unsigned long error_code,
			 unsigned long address)
{
	if (!trace_pagefault_enabled())
		return;

	if (user_mode(regs))
		trace_page_fault_user(address, regs, error_code);
	else
		trace_page_fault_kernel(address, regs, error_code);
}
// arm64是do_page_fault，x86是handle_page_fault
// struct pt_regs 结构中存放的是缺页异常发生时，正在使用中的寄存器值的集合
// address 表示触发缺页异常的虚拟内存地址
// error_code 是对缺页异常的一个描述，目前内核只使用了 error_code 的前六个比特位来描述
// 		引起缺页异常的具体原因
static __always_inline void
handle_page_fault(struct pt_regs *regs, unsigned long error_code,
			      unsigned long address)
{
	// 一个tracepoint，根据user_mode选择user还是kernel
	trace_page_fault_entries(regs, error_code, address);  // 这里可以跟踪缺页异常入口

	if (unlikely(kmmio_fault(regs, address)))
		return;

	/* Was the fault on kernel-controlled part of the address space? */
	// 这⾥判断引起缺⻚异常的虚拟内存地址 address 是属于内核空间的还是⽤户空间的
	if (unlikely(fault_in_kernel_space(address))) {
		// 如果缺页异常发生在内核空间，则由 vmalloc_fault 进行处理
        // 这里使用 unlikely 的原因是，内核对内存的使用通常是高优先级的而且使用比较频繁，
		// 内核空间在分配虚拟内存后，都会立即分配物理内存，所以内核空间一般很少发生缺页异常。
		do_kern_addr_fault(regs, error_code, address);
	} else {
		// 缺页异常发生在用户态
		do_user_addr_fault(regs, error_code, address);
		/*
		 * User address page fault handling might have reenabled
		 * interrupts. Fixing up all potential exit points of
		 * do_user_addr_fault() and its leaf functions is just not
		 * doable w/o creating an unholy mess or turning the code
		 * upside down.
		 */
		local_irq_disable();
	}
}
/**
 * CPU 会将发生缺页异常时，进程正在使用的相关寄存器中的值压入内核栈中。
 * 比如，引起进程缺页异常的虚拟内存地址会被存放在 CR2 寄存器中。
 * 同时 CPU 还会将缺页异常的错误码 error\_code 压入内核栈中
 * 
 * 缺页异常的表现形式有三种:
 * 1. 虚拟内存没有映射物理内存，页表项都是空的
 * 2. 虚拟内存映射了物理内存，但物理内存swap到了交换空间，此时页表项是有的
 * 3. 虚拟内存映射了物理内存，页表项也有，但对物理内存的访问权限不够，例如写一个只读物理内存页
*/
DEFINE_IDTENTRY_RAW_ERRORCODE(exc_page_fault)
{
	// address 表示触发缺页异常的虚拟内存地址
	unsigned long address = read_cr2();
	irqentry_state_t state;
	// 内核这⾥将 mmap_lock 预取到 cacheline 中，并标记为独占状态（ MESI 协议中的 X 状态
	prefetchw(&current->mm->mmap_lock);

	/*
	 * KVM uses #PF vector to deliver 'page not present' events to guests
	 * (asynchronous page fault mechanism). The event happens when a
	 * userspace task is trying to access some valid (from guest's point of
	 * view) memory which is not currently mapped by the host (e.g. the
	 * memory is swapped out). Note, the corresponding "page ready" event
	 * which is injected when the memory becomes available, is delived via
	 * an interrupt mechanism and not a #PF exception
	 * (see arch/x86/kernel/kvm.c: sysvec_kvm_asyncpf_interrupt()).
	 *
	 * We are relying on the interrupted context being sane (valid RSP,
	 * relevant locks not held, etc.), which is fine as long as the
	 * interrupted context had IF=1.  We are also relying on the KVM
	 * async pf type field and CR2 being read consistently instead of
	 * getting values from real and async page faults mixed up.
	 *
	 * Fingers crossed.
	 *
	 * The async #PF handling code takes care of idtentry handling
	 * itself.
	 */
	/*
	上述注释的解释：
	注释解释了 KVM（基于内核的虚拟机）如何利用 x86 的 #PF（页错误异常） 
	向客户机（guest）传递“页面不在内存”的事件（即异步页错误机制）。

	简要说明如下：

	1. 当用户任务访问合法但当前未映射（如被交换到磁盘）的内存时，触发异步页错误；
	2. KVM 使用该机制通知客户机内存不可用；
	3. 当内存恢复可用时，通过中断而非异常通知（见 sysvec_kvm_asyncpf_interrupt()）；
	4. 要求中断上下文是安全的，并确保读取的数据一致；
	5. 异步页错误处理自行管理 IDT（中断描述符表）入口
	*/
	if (kvm_handle_async_pf(regs, (u32)address))
		return;

	/*
	 * Entry handling for valid #PF from kernel mode is slightly
	 * different: RCU is already watching and rcu_irq_enter() must not
	 * be invoked because a kernel fault on a user space address might
	 * sleep.
	 *
	 * In case the fault hit a RCU idle region the conditional entry
	 * code reenabled RCU to avoid subsequent wreckage which helps
	 * debugability.
	 */
	state = irqentry_enter(regs);

	instrumentation_begin();
	handle_page_fault(regs, error_code, address);
	instrumentation_end();

	irqentry_exit(regs, state);
}
