/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_PID_NS_H
#define _LINUX_PID_NS_H

#include <linux/sched.h>
#include <linux/bug.h>
#include <linux/mm.h>
#include <linux/workqueue.h>
#include <linux/threads.h>
#include <linux/nsproxy.h>
#include <linux/kref.h>
#include <linux/ns_common.h>
#include <linux/idr.h>

/* MAX_PID_NS_LEVEL is needed for limiting size of 'struct pid' */
#define MAX_PID_NS_LEVEL 32

struct fs_pin;

struct pid_namespace {
	struct kref kref;   // 引用计数
	struct idr idr;		// 用于管理 PID 到 struct pid 的映射
	struct rcu_head rcu;
	unsigned int pid_allocated;	 	// 已经分配的 PID 数量
	struct task_struct *child_reaper;	// 子进程回收者（类似 init 进程）
	struct kmem_cache *pid_cachep;
	unsigned int level;				// 当前命名空间的层级（0 表示初始命名空间）
	struct pid_namespace *parent;
#ifdef CONFIG_BSD_PROCESS_ACCT
	struct fs_pin *bacct;
#endif
	struct user_namespace *user_ns;		// 关联的用户命名空间
	struct ucounts *ucounts;
	int reboot;	/* group exit code if this pidns was rebooted */
	struct ns_common ns;  // 命名空间通用结构
} __randomize_layout;

extern struct pid_namespace init_pid_ns;

#define PIDNS_ADDING (1U << 31)

#ifdef CONFIG_PID_NS
static inline struct pid_namespace *get_pid_ns(struct pid_namespace *ns)
{
	if (ns != &init_pid_ns)
		kref_get(&ns->kref);
	return ns;
}

extern struct pid_namespace *copy_pid_ns(unsigned long flags,
	struct user_namespace *user_ns, struct pid_namespace *ns);
extern void zap_pid_ns_processes(struct pid_namespace *pid_ns);
extern int reboot_pid_ns(struct pid_namespace *pid_ns, int cmd);
extern void put_pid_ns(struct pid_namespace *ns);

#else /* !CONFIG_PID_NS */
#include <linux/err.h>

static inline struct pid_namespace *get_pid_ns(struct pid_namespace *ns)
{
	return ns;
}

static inline struct pid_namespace *copy_pid_ns(unsigned long flags,
	struct user_namespace *user_ns, struct pid_namespace *ns)
{
	if (flags & CLONE_NEWPID)
		ns = ERR_PTR(-EINVAL);
	return ns;
}

static inline void put_pid_ns(struct pid_namespace *ns)
{
}

static inline void zap_pid_ns_processes(struct pid_namespace *ns)
{
	BUG();
}

static inline int reboot_pid_ns(struct pid_namespace *pid_ns, int cmd)
{
	return 0;
}
#endif /* CONFIG_PID_NS */

extern struct pid_namespace *task_active_pid_ns(struct task_struct *tsk);
void pidhash_init(void);
void pid_idr_init(void);

#endif /* _LINUX_PID_NS_H */
