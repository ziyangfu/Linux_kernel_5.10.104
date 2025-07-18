/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_SLUB_DEF_H
#define _LINUX_SLUB_DEF_H

/*
 * SLUB : A Slab allocator without object queues.
 *
 * (C) 2007 SGI, Christoph Lameter
 */
#include <linux/kobject.h>
#include <linux/reciprocal_div.h>

enum stat_item {
	ALLOC_FASTPATH,		/* Allocation from cpu slab */
	ALLOC_SLOWPATH,		/* Allocation by getting a new cpu slab */
	FREE_FASTPATH,		/* Free to cpu slab */
	FREE_SLOWPATH,		/* Freeing not to cpu slab */
	FREE_FROZEN,		/* Freeing to frozen slab */
	FREE_ADD_PARTIAL,	/* Freeing moves slab to partial list */
	FREE_REMOVE_PARTIAL,	/* Freeing removes last object */
	ALLOC_FROM_PARTIAL,	/* Cpu slab acquired from node partial list */
	ALLOC_SLAB,		/* Cpu slab acquired from page allocator */
	ALLOC_REFILL,		/* Refill cpu slab from slab freelist */
	ALLOC_NODE_MISMATCH,	/* Switching cpu slab */
	FREE_SLAB,		/* Slab freed to the page allocator */
	CPUSLAB_FLUSH,		/* Abandoning of the cpu slab */
	DEACTIVATE_FULL,	/* Cpu slab was full when deactivated */
	DEACTIVATE_EMPTY,	/* Cpu slab was empty when deactivated */
	DEACTIVATE_TO_HEAD,	/* Cpu slab was moved to the head of partials */
	DEACTIVATE_TO_TAIL,	/* Cpu slab was moved to the tail of partials */
	DEACTIVATE_REMOTE_FREES,/* Slab contained remotely freed objects */
	DEACTIVATE_BYPASS,	/* Implicit deactivation */
	ORDER_FALLBACK,		/* Number of times fallback was necessary */
	CMPXCHG_DOUBLE_CPU_FAIL,/* Failure of this_cpu_cmpxchg_double */
	CMPXCHG_DOUBLE_FAIL,	/* Number of times that cmpxchg double did not match */
	CPU_PARTIAL_ALLOC,	/* Used cpu partial on alloc */
	CPU_PARTIAL_FREE,	/* Refill cpu partial on free */
	CPU_PARTIAL_NODE,	/* Refill cpu partial from node partial */
	CPU_PARTIAL_DRAIN,	/* Drain cpu partial to node partial */
	NR_SLUB_STAT_ITEMS };
/**
 * slab cache的本地CPU缓存结构
 * 这里考虑到了多线程并发访问时带来的同步性能开销
 * 所以这里的设计是多线程无锁化设计
*/
struct kmem_cache_cpu {
	// 指向被 CPU 本地缓存的 slab 中第⼀个空闲的对象
	// 注意是空闲对象，当一个空闲对象分配出去后，freelist指针会移动到freelist的下一个空闲对象位置
	void **freelist;	/* Pointer to next available object */
	// 保证进程在 slab cache 中获取到的 cpu 本地缓存 kmem_cache_cpu 与当前执⾏进程的 cpu 是⼀致的
	// 进程可能会被更高优先级的进程抢占，随后进程可能会被内核重新调度到别的核上
	unsigned long tid;	/* Globally unique transaction id */
	// slab cache 中 CPU 本地所缓存的 slab，由于 slab 底层的存储结构是内存⻚ page
	// 所以这⾥直接⽤内存⻚ page 表⽰ slab
	// slab分配对象的快速路径，将从这里分配
	struct page *page;	/* The slab from which we are allocating */
#ifdef CONFIG_SLUB_CPU_PARTIAL
	// cpu cache 缓存的备⽤ slab 列表，同样也是⽤ page 表⽰
	// 当被本地 cpu 缓存的 slab 中没有空闲对象时，内核会从 partial 列表中的 slab 中查找空闲对象
	// 这里的slab会有很多个
	struct page *partial;	/* Partially allocated frozen slabs */
#endif
#ifdef CONFIG_SLUB_STATS
	// 记录 slab 分配对象的⼀些状态信息
	unsigned stat[NR_SLUB_STAT_ITEMS];
#endif
};

#ifdef CONFIG_SLUB_CPU_PARTIAL
#define slub_percpu_partial(c)		((c)->partial)

#define slub_set_percpu_partial(c, p)		\
({						\
	slub_percpu_partial(c) = (p)->next;	\
})

#define slub_percpu_partial_read_once(c)     READ_ONCE(slub_percpu_partial(c))
#else
#define slub_percpu_partial(c)			NULL

#define slub_set_percpu_partial(c, p)

#define slub_percpu_partial_read_once(c)	NULL
#endif // CONFIG_SLUB_CPU_PARTIAL

/*
 * Word size structure that can be atomically updated or read and that
 * contains both the order and the number of objects that a slab of the
 * given order would contain.
 */
struct kmem_cache_order_objects {
	unsigned int x; // 高 16 为存储 slab 所需的内存页个数,低 16 为存储 slab 所能包含的对象总数
};

/*
 * Slab cache management.
 */
// slub小内存分配器
// slab cache在内核中的数据结构，即slab对象池
// 在内存中有很多个slab cache，每个slab cache都对应一个slab对象池，下辖多个slab
// 然后在slab中多个object对象
// 多个slab cache通过链表 struct list_head list 连起来
struct kmem_cache { 
	/** 每个 cpu 拥有⼀个本地缓存，⽤于⽆锁化快速分配释放对象
	 * 这样⼀来，当进程需要向 slab cache 申请对应的内存块（object）时，⾸先会直接来到
	 * kmem_cache_cpu 中查看 cpu 本地缓存的 slab，如果本地缓存的 slab 中有空闲对象，那
	 * 么就直接返回了，整个过程完全没有加锁。⽽且访问路径特别短，防⽌了对 CPU 硬件⾼速缓存
	 *  L1Cache 中的 Instruction Cache（指令⾼速缓存）污染
	*/
	struct kmem_cache_cpu __percpu *cpu_slab;
	/* Used for retrieving partial slabs, etc. */
	// slab cache 的管理标志位，用于设置 slab 的一些特性
    // 比如：slab 中的对象按照什么方式对齐，对象是否需要 POISON  毒化，是否插入
	//  red zone 在对象内存周围，是否追踪对象的分配和释放信息 等等
	slab_flags_t flags;
	// slab cache 在 numa node 中缓存的 slab 个数上限，slab 个数超过该值，
	// 空闲的 empty slab 则会被回收到伙伴系统
	unsigned long min_partial;
	// slab 对象在内存中的真实占用，包括为了内存对齐填充的字节数，red zone 等等
	unsigned int size;	/* The size of an object including metadata */
	// slab 中对象的实际大小，不包含填充的字节数
	unsigned int object_size;/* The size of an object without metadata */
	struct reciprocal_value reciprocal_size;
	// slab 对象池中的对象在没有被分配之前，我们是不关心对象里边存储的内容的。
    // 内核巧妙的利用对象占用的内存空间存储下一个空闲对象的地址。
    // offset 表示用于存储下一个空闲对象指针的位置距离对象首地址的偏移，标识 freepointer的偏移位置
	unsigned int offset;	/* Free pointer offset */
#ifdef CONFIG_SLUB_CPU_PARTIAL
	/* Number of per cpu partial objects to keep around */
	// 限定 slab cache 在每个 cpu 本地缓存 partial 链表中所有 slab 中空闲对象的总数
    // cpu 本地缓存 partial 链表中空闲对象的数量超过该值，则会将 cpu 本地缓存 partial
	//  链表中的所有 slab 转移到 numa node 缓存中。
	unsigned int cpu_partial;
#endif
	// 表示 cache 中的 slab 大小，包括 slab 所需要申请的页面个数，以及所包含的对象个数
    // 其中低 16 位表示一个 slab 中所包含的对象总数，高 16 位表示一个 slab 所占有的内存页个数。
	struct kmem_cache_order_objects oo;

	/* Allocation and freeing of slabs */
	// slab 中所能包含对象以及内存页个数的最大值
	struct kmem_cache_order_objects max;
	// 当按照 oo 的尺寸为 slab 申请内存时，如果内存紧张，
	// 会采用 min 的尺寸为 slab 申请内存，可以容纳一个对象即可。
	struct kmem_cache_order_objects min;
	// 向伙伴系统申请内存时使用的内存分配标识
	gfp_t allocflags;	/* gfp flags to use on each alloc */
	// slab cache 的引用计数，为 0 时就可以销毁并释放内存回伙伴系统中
	int refcount;		/* Refcount for slab cache destroy */
	// 池化对象的构造函数，用于创建 slab 对象池中的对象
	void (*ctor)(void *);
	// 对象的 object_size 按照 word 字长对齐之后的大小，如果我们设置了SLAB_RED_ZONE，
	// inuse 也会包括对象右侧 red zone 区域的大小
	unsigned int inuse;		/* Offset to metadata */
	// 对象按照指定的 align 进行对齐
	unsigned int align;		/* Alignment */
	unsigned int red_left_pad;	/* Left redzone padding size */
	// slab cache 的名称， 也就是在 slabinfo 命令中 name 那一列
	const char *name;	/* Name (only for display!) */
	// 用于组织串联系统中所有类型的 slab cache
	struct list_head list;	/* List of slab caches */
#ifdef CONFIG_SYSFS
	struct kobject kobj;	/* For sysfs */
#endif
#ifdef CONFIG_SLAB_FREELIST_HARDENED
	unsigned long random;
#endif

#ifdef CONFIG_NUMA
	/*
	 * Defragmentation by allocating from a remote node.
	 */
	unsigned int remote_node_defrag_ratio;
#endif

#ifdef CONFIG_SLAB_FREELIST_RANDOM
	unsigned int *random_seq;
#endif

#ifdef CONFIG_KASAN
	struct kasan_cache kasan_info;
#endif

	unsigned int useroffset;	/* Usercopy region offset */
	unsigned int usersize;		/* Usercopy region size */
	// slab cache 中 numa node 中的缓存，每个 node ⼀个
	// 对于普通家用的单处理器系统来说，NUMA节点只有一个
	struct kmem_cache_node *node[MAX_NUMNODES];
};

#ifdef CONFIG_SLUB_CPU_PARTIAL
#define slub_cpu_partial(s)		((s)->cpu_partial)
#define slub_set_cpu_partial(s, n)		\
({						\
	slub_cpu_partial(s) = (n);		\
})
#else
#define slub_cpu_partial(s)		(0)
#define slub_set_cpu_partial(s, n)
#endif /* CONFIG_SLUB_CPU_PARTIAL */

#ifdef CONFIG_SYSFS
#define SLAB_SUPPORTS_SYSFS
void sysfs_slab_unlink(struct kmem_cache *);
void sysfs_slab_release(struct kmem_cache *);
#else
static inline void sysfs_slab_unlink(struct kmem_cache *s)
{
}
static inline void sysfs_slab_release(struct kmem_cache *s)
{
}
#endif

void object_err(struct kmem_cache *s, struct page *page,
		u8 *object, char *reason);

void *fixup_red_left(struct kmem_cache *s, void *p);

static inline void *nearest_obj(struct kmem_cache *cache, struct page *page,
				void *x) {
	void *object = x - (x - page_address(page)) % cache->size;
	void *last_object = page_address(page) +
		(page->objects - 1) * cache->size;
	void *result = (unlikely(object > last_object)) ? last_object : object;

	result = fixup_red_left(cache, result);
	return result;
}

/* Determine object index from a given position */
static inline unsigned int __obj_to_index(const struct kmem_cache *cache,
					  void *addr, void *obj)
{
	return reciprocal_divide(kasan_reset_tag(obj) - addr,
				 cache->reciprocal_size);
}

static inline unsigned int obj_to_index(const struct kmem_cache *cache,
					const struct page *page, void *obj)
{
	return __obj_to_index(cache, page_address(page), obj);
}

static inline int objs_per_slab_page(const struct kmem_cache *cache,
				     const struct page *page)
{
	return page->objects;
}
#endif /* _LINUX_SLUB_DEF_H */
