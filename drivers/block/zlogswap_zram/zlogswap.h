#ifndef _ZLOGSWAP_H
#define _ZLOGSWAP_H

#include <linux/device.h>
#include <linux/printk.h>
#include <linux/memcontrol.h>

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

	unsigned long *bitmap;
	atomic_t last_alloc_bit;

	struct zlogswap_list_table *ext_table; // link the extent into its owner mem_cgroup list
	struct zlogswap_list_head *ext; // ext_table node for extent

	struct zlogswap_list_table *obj_table; // link the obj into its owner extent list
	struct zlogswap_list_head *lru; // obj_table node for obj
	struct zlogswap_list_head *rmap; // obj_table node for extent

	atomic_t stored_exts;
	atomic_t *ext_stored_pages;

	spinlock_t *zlogswap_memcg_init_lock; // lock for zlgoswap_memcg alloc and init

	atomic_t initialized;
};

struct zram;

extern void zlogswap_unbind(struct zram *zram);
extern void zlogswap_track(struct zram *zram, u32 index, struct mem_cgroup *memcg);

extern ssize_t zlogswap_loop_device_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t len);
extern ssize_t zlogswap_loop_device_show(struct device *dev,
		struct device_attribute *attr, char *buf);

struct mem_cgroup_zlogswap {
	struct zram *zram;
	struct mem_cgroup *memcg;
	unsigned long zram_lru;
	unsigned long ext_lru;

	bool initialized;
};

#define MEMCGRP_ITEM_DATA(memcg) ((struct mem_cgroup_zlogswap *)(memcg)->android_oem_data1[0])
#define MEMCGRP_ITEM(memcg, item) (MEMCGRP_ITEM_DATA(memcg)->item)

#endif