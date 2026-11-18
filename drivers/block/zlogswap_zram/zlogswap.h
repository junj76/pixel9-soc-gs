#ifndef _ZLOGSWAP_H
#define _ZLOGSWAP_H

#include <linux/device.h>
#include <linux/printk.h>
#include <linux/memcontrol.h>
#include <linux/pid.h>

#define ZLOGSWAP
#define EXTENT_SHIFT 15
#define EXTENT_SIZE         (1UL << EXTENT_SHIFT)
#define EXTENT_PG_CNT		(EXTENT_SIZE >> PAGE_SHIFT)

/* Use printk directly so pr_fmt in other source files cannot alter the prefix. */
#define log_err(fmt, ...) \
	printk(KERN_ERR "[zlogswap]: " fmt, ##__VA_ARGS__)
#define log_warn(fmt, ...) \
	printk(KERN_WARNING "[zlogswap]: " fmt, ##__VA_ARGS__)
#define log_info(fmt, ...) \
	printk(KERN_INFO "[zlogswap]: " fmt, ##__VA_ARGS__)
/* Keep debug messages enabled independently of DEBUG/dynamic_debug settings. */
#define log_dbg(fmt, ...) \
	printk(KERN_DEBUG "[zlogswap]: " fmt, ##__VA_ARGS__)

#define REGISTER_HOOK(name) do {\
	rc = register_trace_android_vh_##name(name##_hook, NULL);\
	if (rc) {\
		log_err("%s:%d register hook %s failed", __FILE__, __LINE__, #name);\
		goto err_out_##name;\
	}\
} while (0)

#define UNREGISTER_HOOK(name) do {\
	unregister_trace_android_vh_##name(name##_hook, NULL);\
} while (0)

struct zlogswap;

#define zlogswap_LIST_PTR_SHIFT 23
#define zlogswap_LIST_MCG_SHIFT_HALF 8
#define zlogswap_LIST_LOCK_BIT zlogswap_LIST_MCG_SHIFT_HALF
#define zlogswap_LIST_PRIV_BIT (zlogswap_LIST_PTR_SHIFT + zlogswap_LIST_MCG_SHIFT_HALF + \
			  zlogswap_LIST_MCG_SHIFT_HALF + 1)

struct zlogswap_list_head {
	unsigned int mcg_hi : 8;
	unsigned int lock : 1;
	unsigned int prev : 23;
	unsigned int mcg_lo : 8;
	unsigned int priv : 1;
	unsigned int next : 23;
};

struct zlogswap_list_table {
	struct zlogswap_list_head *(*get_node)(int, void*);
	void *private;
};

#define idx_node(idx, tab) ((tab)->get_node((idx), (tab)->private))
#define next_idx(idx, tab) (idx_node((idx), (tab))->next)
#define prev_idx(idx, tab) (idx_node((idx), (tab))->prev)

#define is_last_idx(idx, hidx, tab) (next_idx(idx, tab) == (hidx))
#define is_first_idx(idx, hidx, tab) (prev_idx(idx, tab) == (hidx))

int obj_idx(struct zlogswap *zlogswap_swap, int idx);
int ext_idx(struct zlogswap *zlogswap_swap, int idx);
int mcg_idx(struct zlogswap *zlogswap_swap, int idx);

struct zlogswap_list_table *alloc_table(struct zlogswap_list_head *(*get_node)(int, void *),
				  void *private, gfp_t gfp);
void zlogswap_lock_list(int idx, struct zlogswap_list_table *table);
void zlogswap_unlock_list(int idx, struct zlogswap_list_table *table);

void zlogswap_list_init(int idx, struct zlogswap_list_table *table);
void zlogswap_list_add_nolock(int idx, int hidx, struct zlogswap_list_table *table);
void zlogswap_list_add_tail_nolock(int idx, int hidx, struct zlogswap_list_table *table);
void zlogswap_list_del_nolock(int idx, int hidx, struct zlogswap_list_table *table);
void zlogswap_list_add(int idx, int hidx, struct zlogswap_list_table *table);
void zlogswap_list_add_tail(int idx, int hidx, struct zlogswap_list_table *table);
void zlogswap_list_del(int idx, int hidx, struct zlogswap_list_table *table);

unsigned short zlogswap_list_get_mcgid(int idx, struct zlogswap_list_table *table);
void zlogswap_list_set_mcgid(int idx, struct zlogswap_list_table *table, int mcg_id);
bool zlogswap_list_set_priv(int idx, struct zlogswap_list_table *table);
bool zlogswap_list_clear_priv(int idx, struct zlogswap_list_table *table);
bool zlogswap_list_test_priv(int idx, struct zlogswap_list_table *table);

bool zlogswap_list_empty(int hidx, struct zlogswap_list_table *table);

#define zlogswap_list_for_each_entry(idx, hidx, tab) \
	for ((idx) = next_idx((hidx), (tab)); \
	     (idx) != (hidx); (idx) = next_idx((idx), (tab)))
#define zlogswap_list_for_each_entry_safe(idx, tmp, hidx, tab) \
	for ((idx) = next_idx((hidx), (tab)), (tmp) = next_idx((idx), (tab)); \
	     (idx) != (hidx); (idx) = (tmp), (tmp) = next_idx((idx), (tab)))
#define zlogswap_list_for_each_entry_reverse(idx, hidx, tab) \
	for ((idx) = prev_idx((hidx), (tab)); \
	     (idx) != (hidx); (idx) = prev_idx((idx), (tab)))
#define zlogswap_list_for_each_entry_reverse_safe(idx, tmp, hidx, tab) \
	for ((idx) = prev_idx((hidx), (tab)), (tmp) = prev_idx((idx), (tab)); \
	     (idx) != (hidx); (idx) = (tmp), (tmp) = prev_idx((idx), (tab)))

struct zlogswap {
	unsigned long size; // zlogswap bacing device size in Bytes
	int nr_objs;
	int nr_exts;
	int nr_mcgs;
	u64 gen; /* bumped on every bind; invalidates stale per-memcg state */

	unsigned long *bitmap;
	atomic_t last_alloc_bit;

	struct zlogswap_list_table *ext_table; // link the extent into its owner mem_cgroup list
	struct zlogswap_list_head *ext; // ext_table node for extent

	struct zlogswap_list_table *obj_table; // link the obj into its owner extent list
	struct zlogswap_list_head *lru; // obj_table node for obj
	struct zlogswap_list_head *rmap; // obj_table node for extent

	atomic_t stored_exts;
	atomic_t *ext_stored_pages;
	/* Order: slot (if held) -> extent_lock -> list head -> node. */
	spinlock_t extent_lock;

	spinlock_t *zlogswap_memcg_init_lock; // lock for zlgoswap_memcg alloc and init
};

struct zram;

/*
 * Extent lifecycle only: no I/O, slot updates or rmap movement.
 * Caller keeps the backend, owner memcg, ID mapping and extension alive.
 * Use these APIs for ALL extent bitmap/priv/count changes.
 *
 * alloc/get return ext_id with exclusive ownership (priv=0).
 * get returns -EBUSY when already owned; unallocated IDs return -ENOENT.
 * put publishes a nonempty extent, or frees an empty one.
 * free accepts only an owned, empty extent with empty rmap.
 *
 * add_obj is owner-only, after installing an rmap member; del_obj is called
 * after removing an rmap member. A busy extent's last deletion is finalized
 * by its owner's put. Slot locks protect the mapping/count pairing.
 * These APIs do not verify the caller's identity: exclusive ownership is
 * a caller contract, not a reference count. Do not pass stale extent IDs.
 * Do not call with list/node locks held; never take a slot lock from here.
 */
int zlogswap_alloc_extent(struct zram *zram, struct mem_cgroup *memcg);
int zlogswap_get_extent(struct zram *zram, struct mem_cgroup *memcg, int ext_id);
int zlogswap_put_extent(struct zram *zram, struct mem_cgroup *memcg, int ext_id);
int zlogswap_free_extent(struct zram *zram, struct mem_cgroup *memcg, int ext_id);
int zlogswap_extent_add_obj(struct zram *zram, struct mem_cgroup *memcg, int ext_id);
int zlogswap_extent_del_obj(struct zram *zram, struct mem_cgroup *memcg, int ext_id);

extern void zlogswap_unbind(struct zram *zram);
int zlogswap_init_completion_wq(void);
void zlogswap_destroy_completion_wq(void);
int zlogswap_register_hooks(void);
void zlogswap_unregister_hooks(void);
void zlogswap_untrack(struct zram *zram, u32 index);
int zlogswap_read_extent(struct zram *zram, u32 index);
extern void zlogswap_track(struct zram *zram, u32 index, struct mem_cgroup *memcg);

extern ssize_t zlogswap_loop_device_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t len);
extern ssize_t zlogswap_loop_device_show(struct device *dev,
		struct device_attribute *attr, char *buf);
extern ssize_t zlogswap_promote_pid_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t len);
extern ssize_t zlogswap_flush_pid_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t len);

struct mem_cgroup_zlogswap {
	u16 mcg_id;
	u64 gen; /* zlogswap->gen this owner was initialized for */
	struct zram *zram;
	struct mem_cgroup *memcg;
	unsigned long zram_lru;
	unsigned long ext_lru;

	atomic64_t zram_stored_size; // in Bytes
	atomic64_t zram_page_cnt; // nr_pages
	atomic64_t zlogswap_stored_size;
	atomic64_t zlogswap_page_cnt;

	struct mutex swap_lock;
	bool initialized;
};

#define MEMCGRP_ITEM_DATA(memcg) ((struct mem_cgroup_zlogswap *)(memcg)->android_oem_data1[0])
#define MEMCGRP_ITEM(memcg, item) (MEMCGRP_ITEM_DATA(memcg)->item)

#endif