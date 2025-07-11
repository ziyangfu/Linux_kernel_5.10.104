/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __LINUX_NET_AFUNIX_H
#define __LINUX_NET_AFUNIX_H

#include <linux/socket.h>
#include <linux/un.h>
#include <linux/mutex.h>
#include <linux/refcount.h>
#include <net/sock.h>

void unix_inflight(struct user_struct *user, struct file *fp);
void unix_notinflight(struct user_struct *user, struct file *fp);
void unix_destruct_scm(struct sk_buff *skb);
void unix_gc(void);
void wait_for_unix_gc(void);
struct sock *unix_get_socket(struct file *filp);
struct sock *unix_peer_get(struct sock *sk);

#define UNIX_HASH_SIZE	256
#define UNIX_HASH_BITS	8

extern unsigned int unix_tot_inflight;
extern spinlock_t unix_table_lock;
extern struct hlist_head unix_socket_table[2 * UNIX_HASH_SIZE];

struct unix_address {
	refcount_t	refcnt;
	int		len;
	unsigned int	hash;
	struct sockaddr_un name[];
};

struct unix_skb_parms {
	struct pid		*pid;		/* Skb credentials	*/
	kuid_t			uid;
	kgid_t			gid;
	struct scm_fp_list	*fp;		/* Passed files		*/
#ifdef CONFIG_SECURITY_NETWORK
	u32			secid;		/* Security ID		*/
#endif
	u32			consumed;
} __randomize_layout;

struct scm_stat {
	atomic_t nr_fds;
};

#define UNIXCB(skb)	(*(struct unix_skb_parms *)&((skb)->cb))

#define unix_state_lock(s)	spin_lock(&unix_sk(s)->lock)
#define unix_state_unlock(s)	spin_unlock(&unix_sk(s)->lock)
#define unix_state_lock_nested(s) \
				spin_lock_nested(&unix_sk(s)->lock, \
				SINGLE_DEPTH_NESTING)

/* The AF_UNIX socket */
struct unix_sock {
	/* WARNING: sk has to be the first member */
    // 这是unix_sock结构体中最重要的成员，它代表一个网络套接字。
	// 注意：sk必须是结构体的第一个成员，因为继承机制和强制类型转换。
	struct sock		sk;   
	struct unix_address	*addr;      // 存储Unix域套接字的地址信息。
	struct path		path;  // 保存文件系统的路径信息，用于Unix域套接字的绑定。
		// iolock用于同步I/O操作，bindlock用于同步绑定操作。
	struct mutex		iolock, bindlock;
	struct sock		*peer;  // 指向与当前套接字配对的另一个Unix域套接字
	struct list_head	link;  // 用于将Unix域套接字链接到某个列表中
	atomic_long_t		inflight;
	spinlock_t		lock;
	unsigned long		gc_flags;  	// 用于标记垃圾收集的标志位。
#define UNIX_GC_CANDIDATE	0
#define UNIX_GC_MAYBE_CYCLE	1
	struct socket_wq	peer_wq; // 用于等待队列，当关联的peer变为可读或可写时唤醒等待的进程
	wait_queue_entry_t	peer_wake;
	struct scm_stat		scm_stat;  // 用于统计辅助控制消息（SCM）的相关信息
};

static inline struct unix_sock *unix_sk(const struct sock *sk)
{
	return (struct unix_sock *)sk;
}

#define peer_wait peer_wq.wait

long unix_inq_len(struct sock *sk);
long unix_outq_len(struct sock *sk);

#ifdef CONFIG_SYSCTL
int unix_sysctl_register(struct net *net);
void unix_sysctl_unregister(struct net *net);
#else
static inline int unix_sysctl_register(struct net *net) { return 0; }
static inline void unix_sysctl_unregister(struct net *net) {}
#endif
#endif
