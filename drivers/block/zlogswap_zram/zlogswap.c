#include <linux/blkdev.h>
#include <linux/bitmap.h>
#include <linux/lockdep.h>
#include <linux/completion.h>
#include <linux/workqueue.h>
#include <linux/bio.h>
#include <linux/delay.h>
#include <linux/vmalloc.h>
#include <linux/zsmalloc.h>
#include <trace/hooks/mm.h>

#include "zlogswap.h"
#include "zram_drv.h"

static atomic64_t zlogswap_generation = ATOMIC64_INIT(1);

// low-level linked-list API

int obj_idx(struct zlogswap *zlogswap, int idx)
{
	if (!zlogswap) {
		log_err("NULL zlogswap\n");
		return -EINVAL;
	}
	if (idx < 0 || idx >= zlogswap->nr_objs) {
		log_err("idx = %d invalid\n", idx);
		return -EINVAL;
	}

	return idx;
}

int ext_idx(struct zlogswap *zlogswap, int idx)
{
	if (!zlogswap) {
		log_err("NULL zlogswap\n");
		return -EINVAL;
	}
	if (idx < 0 || idx >= zlogswap->nr_exts) {
		log_err("idx = %d invalid\n", idx);
		return -EINVAL;
	}

	return idx + zlogswap->nr_objs;
}

int mcg_idx(struct zlogswap *zlogswap, int idx)
{
	if (!zlogswap) {
		log_err("NULL zlogswap\n");
		return -EINVAL;
	}
	if (idx <= 0 || idx >= zlogswap->nr_mcgs) {
		log_err("idx = %d invalid, nr_mcgs %d\n", idx,
			zlogswap->nr_mcgs);
		return -EINVAL;
	}

	return idx + zlogswap->nr_objs + zlogswap->nr_exts;
}

static struct zlogswap_list_head *get_node_default(int idx, void *private)
{
	struct zlogswap_list_head *table = private;

	return &table[idx];
}

struct zlogswap_list_table *alloc_table(struct zlogswap_list_head *(*get_node)(int, void *),
				  void *private, gfp_t gfp)
{
	struct zlogswap_list_table *table =
		kmalloc(sizeof(struct zlogswap_list_table), gfp);

	if (!table)
		return NULL;
	table->get_node = get_node ? get_node : get_node_default;
	table->private = private;

	return table;
}

void zlogswap_lock_list(int idx, struct zlogswap_list_table *table)
{
	struct zlogswap_list_head *node = idx_node(idx, table);

	if (!node) {
		log_err("idx = %d, table = %pK\n", idx, table);
		return;
	}
	bit_spin_lock(zlogswap_LIST_LOCK_BIT, (unsigned long *)node);
}

void zlogswap_unlock_list(int idx, struct zlogswap_list_table *table)
{
	struct zlogswap_list_head *node = idx_node(idx, table);

	if (!node) {
		log_err("idx = %d, table = %pK\n", idx, table);
		return;
	}
	bit_spin_unlock(zlogswap_LIST_LOCK_BIT, (unsigned long *)node);
}

bool zlogswap_list_empty(int hidx, struct zlogswap_list_table *table)
{
	bool ret = false;

	zlogswap_lock_list(hidx, table);
	ret = (prev_idx(hidx, table) == hidx) && (next_idx(hidx, table) == hidx);
	zlogswap_unlock_list(hidx, table);

	return ret;
}

void zlogswap_list_init(int idx, struct zlogswap_list_table *table)
{
	struct zlogswap_list_head *node = idx_node(idx, table);

	if (!node) {
		log_err("idx = %d, table = %pS func %pS\n",
			idx, table, table->get_node);
		return;
	}
	memset(node, 0, sizeof(struct zlogswap_list_head));
	node->prev = idx;
	node->next = idx;
}

void zlogswap_list_add_nolock(int idx, int hidx, struct zlogswap_list_table *table)
{
	struct zlogswap_list_head *node = NULL;
	struct zlogswap_list_head *head = NULL;
	struct zlogswap_list_head *next = NULL;
	int nidx;

	node = idx_node(idx, table);
	if (!node) {
		log_err("NULL node, idx = %d, hidx = %d, table = %pK\n", idx, hidx, table);
		return;
	}
	head = idx_node(hidx, table);
	if (!head) {
        log_err("NULL head, idx = %d, hidx = %d, table = %pK\n", idx, hidx, table);
		return;
	}
	next = idx_node(head->next, table);
	if (!next) {
        log_err("NULL next, idx = %d, hidx = %d, table = %pK\n", idx, hidx, table);
		return;
	}

	nidx = head->next;
	if (idx != hidx)
		zlogswap_lock_list(idx, table);
	node->prev = hidx;
	node->next = nidx;
	if (idx != hidx)
		zlogswap_unlock_list(idx, table);
	head->next = idx;
	if (nidx != hidx)
		zlogswap_lock_list(nidx, table);
	next->prev = idx;
	if (nidx != hidx)
		zlogswap_unlock_list(nidx, table);
}

void zlogswap_list_add_tail_nolock(int idx, int hidx, struct zlogswap_list_table *table)
{
	struct zlogswap_list_head *node = NULL;
	struct zlogswap_list_head *head = NULL;
	struct zlogswap_list_head *tail = NULL;
	int tidx;

	node = idx_node(idx, table);
	if (!node) {
        log_err("NULL node, idx = %d, hidx = %d, table = %pK\n", idx, hidx, table);
		return;
	}
	head = idx_node(hidx, table);
	if (!head) {
        log_err("NULL head, idx = %d, hidx = %d, table = %pK\n", idx, hidx, table);
		return;
	}
	tail = idx_node(head->prev, table);
	if (!tail) {
        log_err("NULL tail, idx = %d, hidx = %d, table = %pK\n", idx, hidx, table);
		return;
	}

	tidx = head->prev;
	if (idx != hidx)
		zlogswap_lock_list(idx, table);
	node->prev = tidx;
	node->next = hidx;
	if (idx != hidx)
		zlogswap_unlock_list(idx, table);
	head->prev = idx;
	if (tidx != hidx)
		zlogswap_lock_list(tidx, table);
	tail->next = idx;
	if (tidx != hidx)
		zlogswap_unlock_list(tidx, table);
}

void zlogswap_list_del_nolock(int idx, int hidx, struct zlogswap_list_table *table)
{
	struct zlogswap_list_head *node = NULL;
	struct zlogswap_list_head *prev = NULL;
	struct zlogswap_list_head *next = NULL;
	int pidx, nidx;

	node = idx_node(idx, table);
	if (!node) {
        log_err("NULL node, idx = %d, hidx = %d, table = %pK\n", idx, hidx, table);
		return;
	}
	prev = idx_node(node->prev, table);
	if (!prev) {
        log_err("NULL prev, idx = %d, hidx = %d, table = %pK\n", idx, hidx, table);
		return;
	}
	next = idx_node(node->next, table);
	if (!next) {
        log_err("NULL next, idx = %d, hidx = %d, table = %pK\n", idx, hidx, table);
		return;
	}

	if (idx != hidx)
		zlogswap_lock_list(idx, table);
	pidx = node->prev;
	nidx = node->next;
	node->prev = idx;
	node->next = idx;
	if (idx != hidx)
		zlogswap_unlock_list(idx, table);
	if (pidx != hidx)
		zlogswap_lock_list(pidx, table);
	prev->next = nidx;
	if (pidx != hidx)
		zlogswap_unlock_list(pidx, table);
	if (nidx != hidx)
		zlogswap_lock_list(nidx, table);
	next->prev = pidx;
	if (nidx != hidx)
		zlogswap_unlock_list(nidx, table);
}

void zlogswap_list_add(int idx, int hidx, struct zlogswap_list_table *table)
{
	zlogswap_lock_list(hidx, table);
	zlogswap_list_add_nolock(idx, hidx, table);
	zlogswap_unlock_list(hidx, table);
}

void zlogswap_list_add_tail(int idx, int hidx, struct zlogswap_list_table *table)
{
	zlogswap_lock_list(hidx, table);
	zlogswap_list_add_tail_nolock(idx, hidx, table);
	zlogswap_unlock_list(hidx, table);
}

void zlogswap_list_del(int idx, int hidx, struct zlogswap_list_table *table)
{
	zlogswap_lock_list(hidx, table);
	zlogswap_list_del_nolock(idx, hidx, table);
	zlogswap_unlock_list(hidx, table);
}

unsigned short zlogswap_list_get_mcgid(int idx, struct zlogswap_list_table *table)
{
	struct zlogswap_list_head *node = idx_node(idx, table);
	int mcg_id;

	if (!node) {
		log_err("idx = %d, table = %pK\n", idx, table);
		return 0;
	}

	zlogswap_lock_list(idx, table);
	mcg_id = (node->mcg_hi << zlogswap_LIST_MCG_SHIFT_HALF) | node->mcg_lo;
	zlogswap_unlock_list(idx, table);

	return mcg_id;
}

void zlogswap_list_set_mcgid(int idx, struct zlogswap_list_table *table, int mcg_id)
{
	struct zlogswap_list_head *node = idx_node(idx, table);

	if (!node) {
		log_err("idx = %d, table = %pK, mcg = %d\n",
			idx, table, mcg_id);
		return;
	}

	zlogswap_lock_list(idx, table);
	node->mcg_hi = (u32)mcg_id >> zlogswap_LIST_MCG_SHIFT_HALF;
	node->mcg_lo = (u32)mcg_id & ((1 << zlogswap_LIST_MCG_SHIFT_HALF) - 1);
	zlogswap_unlock_list(idx, table);
}

bool zlogswap_list_set_priv(int idx, struct zlogswap_list_table *table)
{
	struct zlogswap_list_head *node = idx_node(idx, table);
	bool ret = false;

	if (!node) {
		log_err("idx = %d, table = %pK\n", idx, table);
		return false;
	}
	zlogswap_lock_list(idx, table);
	ret = !test_and_set_bit(zlogswap_LIST_PRIV_BIT, (unsigned long *)node);
	zlogswap_unlock_list(idx, table);

	return ret;
}

bool zlogswap_list_test_priv(int idx, struct zlogswap_list_table *table)
{
	struct zlogswap_list_head *node = idx_node(idx, table);
	bool ret = false;

	if (!node) {
		log_err("idx = %d, table = %pK\n", idx, table);
		return false;
	}
	zlogswap_lock_list(idx, table);
	ret = test_bit(zlogswap_LIST_PRIV_BIT, (unsigned long *)node);
	zlogswap_unlock_list(idx, table);

	return ret;
}

bool zlogswap_list_clear_priv(int idx, struct zlogswap_list_table *table)
{
	struct zlogswap_list_head *node = idx_node(idx, table);
	bool ret = false;

	if (!node) {
		log_err("idx = %d, table = %pK\n", idx, table);
		return false;
	}

	zlogswap_lock_list(idx, table);
	ret = test_and_clear_bit(zlogswap_LIST_PRIV_BIT, (unsigned long *)node);
	zlogswap_unlock_list(idx, table);

	return ret;
}

struct mem_cgroup *get_mem_cgroup(unsigned short mcg_id)
{
	struct mem_cgroup *mcg = NULL;

	rcu_read_lock();
	mcg = mem_cgroup_from_id(mcg_id);
	rcu_read_unlock();

	return mcg;
}

void zram_set_memcg(struct zram *zram, u32 index, int mcg_id)
{
	zlogswap_list_set_mcgid(obj_idx(zram->zlogswap, index),
			  zram->zlogswap->obj_table, mcg_id);
}

struct mem_cgroup *zram_get_memcg(struct zram *zram, u32 index)
{
	unsigned short mcg_id;

	mcg_id = zlogswap_list_get_mcgid(obj_idx(zram->zlogswap, index),
				   zram->zlogswap->obj_table);

	return get_mem_cgroup(mcg_id);
}

// High-level metadata API

void zram_lru_add(struct zram *zram, u32 index, struct mem_cgroup *memcg)
{
	unsigned long size;
    if (!zram || !zram->zlogswap) {
        log_err("invalid arg(zram || zram->zlogswap) for zlogswap_zram_lru_add!\n");
        return;
    }
    if (index >= (u32)zram->zlogswap->nr_objs) {
        log_err("invalid arg(index) for zlogswap_zram_lru_add!\n");
        return;
    }
    if (zram_test_flag(zram, index, ZRAM_WB)) {
        log_err("WB object! index: %d\n", index);
        return;
    }
    if (zram_test_flag(zram, index, ZRAM_SAME)) {
        return;
    }

    zram_set_memcg(zram, index, memcg->id.id);
    zlogswap_list_add(obj_idx(zram->zlogswap, index),
                    mcg_idx(zram->zlogswap, memcg->id.id),
                    zram->zlogswap->obj_table);

	size = zram_get_obj_size(zram, index);
	atomic64_add(size, &MEMCGRP_ITEM(memcg, zram_stored_size));
	atomic64_inc(&MEMCGRP_ITEM(memcg, zram_page_cnt));
}

void zlogswap_zram_lru_add(struct zram *zram, u32 index, struct mem_cgroup *memcg)
{
	zram_lru_add(zram, index, memcg);
}

/* Extent ownership: bitmap=allocated, priv=available, count=live objects. */
static int extent_args(struct zram *zram, struct mem_cgroup *memcg,
		       struct zlogswap **out)
{
	struct mem_cgroup_zlogswap *owner;
	struct zlogswap *zs;

	if (!zram || !memcg)
		return -EINVAL;
	zs = zram->zlogswap;
	if (!zs || !zs->bitmap || !zs->ext_table || !zs->obj_table ||
	    !zs->ext_stored_pages)
		return -ENODEV;
	if (!memcg->id.id || memcg->id.id >= zs->nr_mcgs)
		return -EINVAL;
	if (zs->nr_objs <= 0 || zs->nr_exts <= 0 ||
	    (u64)zs->nr_objs + zs->nr_exts + zs->nr_mcgs - 1 >=
	    (1ULL << zlogswap_LIST_PTR_SHIFT))
		return -EOVERFLOW;
	owner = (struct mem_cgroup_zlogswap *)
		READ_ONCE(memcg->android_oem_data1[0]);
	if (!owner || !smp_load_acquire(&owner->initialized))
		return -ENOENT;
	if (owner->zram != zram)
		return -EXDEV;
	*out = zs;
	return 0;
}

/* extent_lock is held; caller protects owner and its ID mapping lifetime. */
static int extent_check_locked(struct zlogswap *zs, struct mem_cgroup *memcg,
			       int ext_id)
{
	lockdep_assert_held(&zs->extent_lock);
	if (ext_id < 0 || ext_id >= zs->nr_exts)
		return -EINVAL;
	if (!test_bit(ext_id, zs->bitmap))
		return -ENOENT;
	if (zlogswap_list_get_mcgid(ext_idx(zs, ext_id), zs->ext_table) !=
	    memcg->id.id)
		return -EXDEV;
	return 0;
}

/* Owned, empty and detached. Free never steals another owner's extent. */
static int extent_free_locked(struct zlogswap *zs, int ext_id)
{
	int idx = ext_idx(zs, ext_id);

	lockdep_assert_held(&zs->extent_lock);
	if (zlogswap_list_test_priv(idx, zs->ext_table) ||
	    atomic_read(&zs->ext_stored_pages[ext_id]))
		return -EBUSY;
	if (!zlogswap_list_empty(idx, zs->ext_table) ||
	    !zlogswap_list_empty(idx, zs->obj_table))
		return -EBUSY;
	zlogswap_list_set_mcgid(idx, zs->ext_table, 0);
	atomic_dec(&zs->stored_exts);
	clear_bit(ext_id, zs->bitmap);
	return 0;
}

int zlogswap_alloc_extent(struct zram *zram, struct mem_cgroup *memcg)
{
	struct zlogswap *zs;
	unsigned long flags, bit;
	int ret = extent_args(zram, memcg, &zs);
	int start;

	if (ret)
		return ret;
	spin_lock_irqsave(&zs->extent_lock, flags);
	start = atomic_read(&zs->last_alloc_bit);
	bit = find_next_zero_bit(zs->bitmap, zs->nr_exts, start);
	if (bit >= zs->nr_exts)
		bit = find_first_zero_bit(zs->bitmap, zs->nr_exts);
	if (bit >= zs->nr_exts) {
		ret = -ENOSPC;
		goto out;
	}
	/* All bitmap updates are serialized by extent_lock. */
	set_bit(bit, zs->bitmap);
	zlogswap_list_init(ext_idx(zs, bit), zs->ext_table);
	zlogswap_list_init(ext_idx(zs, bit), zs->obj_table);
	zlogswap_list_set_mcgid(ext_idx(zs, bit), zs->ext_table,
				memcg->id.id);
	atomic_set(&zs->ext_stored_pages[bit], 0);
	atomic_inc(&zs->stored_exts);
	atomic_set(&zs->last_alloc_bit, bit + 1 == zs->nr_exts ? 0 : bit + 1);
	ret = bit;
out:
	spin_unlock_irqrestore(&zs->extent_lock, flags);
	return ret;
}

int zlogswap_get_extent(struct zram *zram, struct mem_cgroup *memcg, int ext_id)
{
	struct zlogswap *zs;
	unsigned long flags;
	int ret = extent_args(zram, memcg, &zs);

	if (ret)
		return ret;
	spin_lock_irqsave(&zs->extent_lock, flags);
	ret = extent_check_locked(zs, memcg, ext_id);
	if (ret)
		goto out;
	if (!zlogswap_list_clear_priv(ext_idx(zs, ext_id), zs->ext_table)) {
		ret = -EBUSY;
		goto out;
	}
	zlogswap_list_del(ext_idx(zs, ext_id), mcg_idx(zs, memcg->id.id),
			  zs->ext_table);
	ret = ext_id;
out:
	spin_unlock_irqrestore(&zs->extent_lock, flags);
	return ret;
}

int zlogswap_put_extent(struct zram *zram, struct mem_cgroup *memcg, int ext_id)
{
	struct zlogswap *zs;
	unsigned long flags;
	int ret = extent_args(zram, memcg, &zs);

	if (ret)
		return ret;
	spin_lock_irqsave(&zs->extent_lock, flags);
	ret = extent_check_locked(zs, memcg, ext_id);
	if (ret)
		goto out;
	if (zlogswap_list_test_priv(ext_idx(zs, ext_id), zs->ext_table)) {
		ret = -EALREADY;
		goto out;
	}
	if (!atomic_read(&zs->ext_stored_pages[ext_id])) {
		ret = extent_free_locked(zs, ext_id);
		goto out;
	}
	zlogswap_list_add(ext_idx(zs, ext_id), mcg_idx(zs, memcg->id.id),
			  zs->ext_table);
	zlogswap_list_set_priv(ext_idx(zs, ext_id), zs->ext_table);
	ret = 0;
out:
	spin_unlock_irqrestore(&zs->extent_lock, flags);
	return ret;
}

int zlogswap_free_extent(struct zram *zram, struct mem_cgroup *memcg, int ext_id)
{
	struct zlogswap *zs;
	unsigned long flags;
	int ret = extent_args(zram, memcg, &zs);

	if (ret)
		return ret;
	spin_lock_irqsave(&zs->extent_lock, flags);
	ret = extent_check_locked(zs, memcg, ext_id);
	if (!ret)
		ret = extent_free_locked(zs, ext_id);
	spin_unlock_irqrestore(&zs->extent_lock, flags);
	return ret;
}

int zlogswap_extent_add_obj(struct zram *zram, struct mem_cgroup *memcg, int ext_id)
{
	struct zlogswap *zs;
	unsigned long flags;
	int ret = extent_args(zram, memcg, &zs);

	if (ret)
		return ret;
	spin_lock_irqsave(&zs->extent_lock, flags);
	ret = extent_check_locked(zs, memcg, ext_id);
	if (!ret) {
		if (zlogswap_list_test_priv(ext_idx(zs, ext_id), zs->ext_table))
			ret = -EBUSY;
		else
			atomic_inc(&zs->ext_stored_pages[ext_id]);
	}
	spin_unlock_irqrestore(&zs->extent_lock, flags);
	return ret;
}

int zlogswap_extent_del_obj(struct zram *zram, struct mem_cgroup *memcg, int ext_id)
{
	struct zlogswap *zs;
	unsigned long flags;
	int ret = extent_args(zram, memcg, &zs);
	int idx;

	if (ret)
		return ret;
	spin_lock_irqsave(&zs->extent_lock, flags);
	ret = extent_check_locked(zs, memcg, ext_id);
	if (ret)
		goto out;
	if (atomic_read(&zs->ext_stored_pages[ext_id]) <= 0) {
		ret = -EINVAL;
		goto out;
	}
	if (!atomic_dec_and_test(&zs->ext_stored_pages[ext_id]))
		goto out;
	idx = ext_idx(zs, ext_id);
	/* Busy owner will see zero in put, under this same lock. */
	if (!zlogswap_list_test_priv(idx, zs->ext_table))
		goto out;
	if (!zlogswap_list_empty(idx, zs->obj_table)) {
		/* Last rmap member must be removed before decrementing. */
		atomic_inc(&zs->ext_stored_pages[ext_id]);
		ret = -EBUSY;
		goto out;
	}
	zlogswap_list_clear_priv(idx, zs->ext_table);
	zlogswap_list_del(idx, mcg_idx(zs, memcg->id.id), zs->ext_table);
	ret = extent_free_locked(zs, ext_id);
out:
	spin_unlock_irqrestore(&zs->extent_lock, flags);
	return ret;
}

// init zlogswap

#define DEVICE_NAME_LEN 64
static char loop_device[DEVICE_NAME_LEN];

struct zlogswap_config {
	struct workqueue_struct *flushing_complete_wq;
	atomic_t zlogswap_initialized;
};

struct zlogswap_config global_settings;

/* Module lifetime: created before publishing any devices, destroyed after them. */
int zlogswap_init_completion_wq(void)
{
	global_settings.flushing_complete_wq = alloc_workqueue(
		"zlogswap_flush_complete", WQ_UNBOUND | WQ_MEM_RECLAIM, 0);
	return global_settings.flushing_complete_wq ? 0 : -ENOMEM;
}

void zlogswap_destroy_completion_wq(void)
{
	if (global_settings.flushing_complete_wq) {
		destroy_workqueue(global_settings.flushing_complete_wq);
		global_settings.flushing_complete_wq = NULL;
	}
}


static inline struct zram *dev_to_zram(struct device *dev)
{
	return (struct zram *)dev_to_disk(dev)->private_data;
}

static struct zlogswap_list_head *get_obj_table_node(int idx, void *private)
{
	struct zlogswap *zlogswap = private;

	if (!zlogswap) {
		log_err("NULL zlogswap\n");
		return NULL;
	}
	if (idx < 0) {
		log_err("idx = %d invalid\n", idx);
		return NULL;
	}
	if (idx < zlogswap->nr_objs)
		return &zlogswap->lru[idx];
	idx -= zlogswap->nr_objs;
	if (idx < zlogswap->nr_exts)
		return &zlogswap->rmap[idx];
	idx -= zlogswap->nr_exts;
	if (idx > 0 && idx < zlogswap->nr_mcgs) {
		struct mem_cgroup *mcg = get_mem_cgroup(idx);

		if (!mcg)
			goto err_out;
		return (struct zlogswap_list_head *)(&MEMCGRP_ITEM(mcg, zram_lru));
	}
err_out:
	log_err("idx = %d invalid, mcg is NULL\n", idx);

	return NULL;
}

static void free_obj_list_table(struct zlogswap *zlogswap)
{
	if (!zlogswap) {
		log_err("NULL zlogswap\n");
		return;
	}

	if (zlogswap->lru) {
		vfree(zlogswap->lru);
		zlogswap->lru = NULL;
	}
	if (zlogswap->rmap) {
		vfree(zlogswap->rmap);
		zlogswap->rmap = NULL;
	}

	kfree(zlogswap->obj_table);
	zlogswap->obj_table = NULL;
}

static int init_obj_list_table(struct zlogswap *zlogswap)
{
	int i;

	if (!zlogswap) {
		log_err("NULL zlogswap\n");
		return -EINVAL;
	}

	zlogswap->lru = vzalloc(sizeof(struct zlogswap_list_head) * zlogswap->nr_objs);
	if (!zlogswap->lru) {
		log_err("zlogswap->lru alloc failed\n");
		goto err_out;
	}
	zlogswap->rmap = vzalloc(sizeof(struct zlogswap_list_head) * zlogswap->nr_exts);
	if (!zlogswap->rmap) {
		log_err("zlogswap->rmap alloc failed\n");
		goto err_out;
	}
	zlogswap->obj_table = alloc_table(get_obj_table_node, zlogswap, GFP_KERNEL);
	if (!zlogswap->obj_table) {
		log_err("zlogswap->obj_table alloc failed\n");
		goto err_out;
	}
	for (i = 0; i < zlogswap->nr_objs; i++)
		zlogswap_list_init(obj_idx(zlogswap, i), zlogswap->obj_table);
	for (i = 0; i < zlogswap->nr_exts; i++)
		zlogswap_list_init(ext_idx(zlogswap, i), zlogswap->obj_table);

	log_info("zlogswap obj list table init OK.\n");
	return 0;
err_out:
	free_obj_list_table(zlogswap);
	log_err("zlogswap obj list table init failed.\n");

	return -ENOMEM;
}

static struct zlogswap_list_head *get_ext_table_node(int idx, void *private)
{
	struct zlogswap *zlogswap = private;

	if (!zlogswap) {
		log_err("NULL zlogswap\n");
		return NULL;
	}

	if (idx < zlogswap->nr_objs)
		goto err_out;
	idx -= zlogswap->nr_objs;
	if (idx < zlogswap->nr_exts)
		return &zlogswap->ext[idx];
	idx -= zlogswap->nr_exts;
	if (idx > 0 && idx < zlogswap->nr_mcgs) {
		struct mem_cgroup *mcg = get_mem_cgroup(idx);

		if (!mcg)
			return NULL;
		return (struct zlogswap_list_head *)(&MEMCGRP_ITEM(mcg, ext_lru));
	}
err_out:
	log_err("idx = %d invalid\n", idx);

	return NULL;
}

static void free_ext_list_table(struct zlogswap *zlogswap)
{
	if (!zlogswap) {
		log_err("NULL zlogswap\n");
		return;
	}

	if (zlogswap->ext)
		vfree(zlogswap->ext);

	kfree(zlogswap->ext_table);
}

static int init_ext_list_table(struct zlogswap *zlogswap)
{
	int i;

	if (!zlogswap) {
		log_err("NULL zlogswap\n");
		return -EINVAL;
	}
	zlogswap->ext = vzalloc(sizeof(struct zlogswap_list_head) * zlogswap->nr_exts);
	if (!zlogswap->ext)
		goto err_out;
	zlogswap->ext_table = alloc_table(get_ext_table_node, zlogswap, GFP_KERNEL);
	if (!zlogswap->ext_table)
		goto err_out;
	for (i = 0; i < zlogswap->nr_exts; i++)
		zlogswap_list_init(ext_idx(zlogswap, i), zlogswap->ext_table);
	log_info("zlogswap ext list table init OK.\n");
	return 0;
err_out:
	free_ext_list_table(zlogswap);
	log_err("zlogswap ext list table init failed.\n");

	return -ENOMEM;
}

static void free_zlogswap(struct zram *zram)
{
    struct zlogswap *zlogswap = zram->zlogswap;
    if (!zlogswap) {
        log_warn("zram->zlogswap is null, no need to free\n");
        return;
    }
    vfree(zlogswap->bitmap);
    vfree(zlogswap->ext_stored_pages);
    free_obj_list_table(zlogswap);
    free_ext_list_table(zlogswap);
    vfree(zlogswap->zlogswap_memcg_init_lock);
    vfree(zlogswap);
    zram->zlogswap = NULL;
}

static int alloc_zlogswap(struct zram *zram)
{
    // The caller guarantees that zram->zlogswap_backing_dev/zlogswap_bdev is not NULL
    int err;
    int i;
    struct zlogswap *zlogswap = NULL;

    if (!zram->disksize) {
        log_err("zram->disksize is not set before initialize zlogswap!\n");
        err = -EINVAL;
        goto err_out;
    }

    zlogswap = (struct zlogswap*)vzalloc(sizeof(struct zlogswap));
    if (!zlogswap) {
        log_err("failed to alloc zram->zlogswap!\n");
        err = -ENOMEM;
        goto err_out;
    }

    zram->zlogswap = zlogswap;
    spin_lock_init(&zlogswap->extent_lock);

    zlogswap->size = (unsigned long)(i_size_read(zram->zlogswap_backing_dev->f_mapping->host) >> PAGE_SHIFT << PAGE_SHIFT);
    zlogswap->nr_objs = zram->disksize >> PAGE_SHIFT;
    zlogswap->nr_exts = zlogswap->size >> EXTENT_SHIFT;
    /* Exclusive ID limit: valid memcg IDs are 1..MEM_CGROUP_ID_MAX. */
    zlogswap->nr_mcgs = MEM_CGROUP_ID_MAX + 1;
    zlogswap->gen = atomic64_inc_return(&zlogswap_generation);
    zlogswap->bitmap = vzalloc(BITS_TO_LONGS(zlogswap->nr_exts) * sizeof(long));
    if (!zlogswap->bitmap) {
        log_err("failed to alloc zlogswap->bitmap!\n");
        err = -ENOMEM;
        goto err_out;
    }
    zlogswap->ext_stored_pages = vzalloc(sizeof(atomic_t) * zlogswap->nr_exts);
    if (!zlogswap->ext_stored_pages) {
        log_err("failed to alloc zlogswap->ext_stored_pages!\n");
        err = -ENOMEM;
        goto err_out;
    }
    if (init_obj_list_table(zlogswap)) {
        log_err("failed to init zlogswap->obj_table!\n");
        err = -ENOMEM;
        goto err_out;
    }
    if (init_ext_list_table(zlogswap)) {
        log_err("failed to init zlogswap->ext_table!\n");
        err = -ENOMEM;
        goto err_out;
    }
    zlogswap->zlogswap_memcg_init_lock = vzalloc(sizeof(spinlock_t) * (zlogswap->nr_mcgs));
    if (!zlogswap->zlogswap_memcg_init_lock) {
        log_err("failed to alloc zlogswap->zlogswap_memcg_init_lock!\n");
        err = -ENOMEM;
        goto err_out;
    }
    for (i = 0; i < zlogswap->nr_mcgs; i++) {
        spin_lock_init(&zlogswap->zlogswap_memcg_init_lock[i]);
    }

	atomic_set(&global_settings.zlogswap_initialized, 1);
    log_info("success to initialize zlogswap!\n");
    log_info("zlogswap size: %lu(Bytes), nr_obs: %d, nr_exts: %d, nr_mcgs: %d",
                zlogswap->size, zlogswap->nr_objs, zlogswap->nr_exts, zlogswap->nr_mcgs);

    return 0;

err_out:
    free_zlogswap(zram);
    return err;
}

/*
 * Caller must hold init_lock for writing, prevent new backend users, and wait
 * for all existing backend I/O/work to finish. This does not migrate data.
 */
void zlogswap_unbind(struct zram *zram)
{
	struct file *backing_dev;
	struct zlogswap *zlogswap;
	int err;

	if (WARN_ON_ONCE(!zram))
		return;

	lockdep_assert_held_write(&zram->init_lock);
	zlogswap = zram->zlogswap;

	/* Do not discard data or an extent reserved by an unfinished operation. */
	if (zlogswap &&
	    (atomic_read(&zlogswap->stored_exts) ||
	     (zlogswap->bitmap &&
	      !bitmap_empty(zlogswap->bitmap, zlogswap->nr_exts)))) {
		log_err("cannot unbind: backend extents are still allocated\n");
		return;
	}

	backing_dev = zram->zlogswap_backing_dev;
	zram->zlogswap_backing_dev = NULL;
	zram->zlogswap_bdev = NULL;
	zram->zlogswap_bdev_nr_pages = 0;

	if (zlogswap)
		free_zlogswap(zram);

	/* I_BDEV() borrowed this pointer; there is no extra blkdev_put() here. */
	if (!IS_ERR_OR_NULL(backing_dev)) {
		err = filp_close(backing_dev, NULL);
		if (err)
			log_warn("closing backend returned: %d\n", err);
	}

    memset(loop_device, 0, DEVICE_NAME_LEN);

	log_info("backend unbound\n");
}

static int zlogswap_bind(struct zram *zram, char *file_name)
{
    struct file *zlogswap_backing_dev = NULL;
    int err = 0;

    if (zram->zlogswap_backing_dev || zram->zlogswap) {
        log_err("zlogswap_backing_dev has been binded!\n");
        err = -EBUSY;
        return err;
    }

    zlogswap_backing_dev = filp_open_block(file_name, O_RDWR | O_LARGEFILE, 0);
    if (IS_ERR(zlogswap_backing_dev)) {
        log_err("failed to get backing_dev by path: %s", file_name);
        err = PTR_ERR(zlogswap_backing_dev);
        goto out;
    }

    zram->zlogswap_backing_dev = zlogswap_backing_dev;
    zram->zlogswap_bdev = I_BDEV(zlogswap_backing_dev->f_mapping->host);

    err = alloc_zlogswap(zram);
    if (err) {
        log_err("failed to alloc & initialize zram->zlogswap!\n");
        goto out;
    }

    return 0;

out:
    if (!IS_ERR(zlogswap_backing_dev)) {
        filp_close(zlogswap_backing_dev, NULL);
        zram->zlogswap_backing_dev = NULL;
        zram->zlogswap_bdev = NULL;
    }
    return err;
}

ssize_t zlogswap_loop_device_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t len)
{
    struct zram *zram;
    int err;

    if (len > DEVICE_NAME_LEN - 1) {
        log_err("buf %s len %lu is too long!\n", buf, len);
        return -EINVAL;
    }

    memcpy(loop_device, buf, len);
    if (len && loop_device[len-1] == '\n')
        loop_device[len-1] = '\0';

    zram = dev_to_zram(dev);
    down_write(&zram->init_lock);
    if (zram->disksize == 0) {
        log_err("disksize is 0!\n");
        err = -EINVAL;
        goto out;
    }

    err = zlogswap_bind(zram, loop_device);

out:
    up_write(&zram->init_lock);
    return err ? err : len;
}

ssize_t zlogswap_loop_device_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
    int len;

    len = sprintf(buf, "%s\n", loop_device);

    return len;
}

/* One synchronous request owns at most one 32 KiB staging buffer. */
#define ZLOGSWAP_BATCH_OBJS 128
struct zlogswap_flush_obj {
	u32 index;
	u32 offset;
	u32 size;
};

static struct mem_cgroup *slot_owner(struct zram *zram, u32 index)
{
	struct zlogswap *zs = zram->zlogswap;
	u16 id = zlogswap_list_get_mcgid(index, zs->obj_table);

	return id && id < zs->nr_mcgs ? get_mem_cgroup(id) : NULL;
}

/* slot lock held. Cancel pending snapshots without waiting in an atomic path. */
void zlogswap_untrack(struct zram *zram, u32 index)
{
	struct zlogswap *zs = zram->zlogswap;
	struct mem_cgroup *mcg;
	struct mem_cgroup_zlogswap *owner;
	unsigned long size, entry;
	int ext_id;

	if (!zs || index >= zs->nr_objs)
		return;
	mcg = slot_owner(zram, index);
	if (!mcg)
		return;
	owner = MEMCGRP_ITEM_DATA(mcg);
	size = zram_get_obj_size(zram, index);
	if (zram_test_flag(zram, index, ZRAM_ZLOGSWAP_FLUSHED)) {
		entry = zram_get_handle(zram, index);
		ext_id = entry >> EXTENT_SHIFT;
		zlogswap_list_del(index, ext_idx(zs, ext_id), zs->obj_table);
		atomic64_sub(size, &owner->zlogswap_stored_size);
		atomic64_dec(&owner->zlogswap_page_cnt);
		WARN_ON_ONCE(zlogswap_extent_del_obj(zram, mcg, ext_id));
	} else {
		zlogswap_list_del(index, mcg_idx(zs, mcg->id.id), zs->obj_table);
		atomic64_sub(size, &owner->zram_stored_size);
		atomic64_dec(&owner->zram_page_cnt);
	}
	zram_set_memcg(zram, index, 0);
}

/* Contiguous zeroed buffer; bio pages may be split by the block layer. */
static int zlogswap_extent_io(struct zram *zram, int ext_id,
			      void *buffer, bool write)
{
	struct bio *bio;
	unsigned int i;
	int ret;

	BUILD_BUG_ON(EXTENT_SIZE < PAGE_SIZE);
	bio = bio_alloc(zram->zlogswap_bdev, EXTENT_PG_CNT,
			(write ? REQ_OP_WRITE : REQ_OP_READ) | REQ_SYNC, GFP_NOIO);
	if (!bio)
		return -ENOMEM;
	bio->bi_iter.bi_sector = (sector_t)ext_id << (EXTENT_SHIFT - 9);
	for (i = 0; i < EXTENT_PG_CNT; i++) {
		if (bio_add_page(bio, virt_to_page(buffer + i * PAGE_SIZE),
				 PAGE_SIZE, 0) != PAGE_SIZE) {
			bio_put(bio);
			return -EIO;
		}
	}
	ret = submit_bio_wait(bio);
	bio_put(bio);
	return ret;
}

/*
 * The reclaim caller waits for done and flush_work before reusing its borrowed
 * buffer/obj array or dropping init_lock/swap_lock. No callback dereferences
 * caller memory after that join. The extra css reference covers endio/work.
 */
struct zlogswap_flush_io {
	struct work_struct work;
	struct completion done;
	struct zram *zram;
	struct mem_cgroup *memcg;
	struct zlogswap_flush_obj *objs;
	struct bio *bio;
	u32 nr;
	int ext_id;
	int error;
	unsigned long written;
	bool memcg_ref;
};

static void zlogswap_flush_complete_work(struct work_struct *work)
{
	struct zlogswap_flush_io *io = container_of(work, struct zlogswap_flush_io, work);
	struct zram *zram = io->zram;
	struct mem_cgroup *memcg = io->memcg;
	struct zlogswap *zs = zram->zlogswap;
	struct mem_cgroup_zlogswap *owner = MEMCGRP_ITEM_DATA(memcg);
	struct zlogswap_flush_obj *objs = io->objs;
	unsigned long *written = &io->written;
	u32 i, nr = io->nr;
	int ext_id = io->ext_id, ret = io->error;

	for (i = 0; i < nr; i++) {
		u32 index = objs[i].index;

		zram_slot_lock(zram, index);
		if (!ret && zram_test_flag(zram, index, ZRAM_ZLOGSWAP_FLUSH_VALID)) {
			unsigned long handle = zram_get_handle(zram, index);

			/* Cancellation bit guarantees this is the copied slot incarnation. */
			zlogswap_list_del(index, mcg_idx(zs, memcg->id.id), zs->obj_table);
			zs_free(zram->mem_pool, handle);
			zram_set_handle(zram, index, ((unsigned long)ext_id << EXTENT_SHIFT) |
					objs[i].offset);
			zram_set_flag(zram, index, ZRAM_ZLOGSWAP_FLUSHED);
			zlogswap_list_add(index, ext_idx(zs, ext_id), zs->obj_table);
			WARN_ON_ONCE(zlogswap_extent_add_obj(zram, memcg, ext_id));
			atomic64_sub(objs[i].size, &owner->zram_stored_size);
			atomic64_dec(&owner->zram_page_cnt);
			atomic64_add(objs[i].size, &owner->zlogswap_stored_size);
			atomic64_inc(&owner->zlogswap_page_cnt);
			__this_cpu_sub(zram->pcp_stats->items[COMPRESSED_SIZE], objs[i].size);
			*written += objs[i].size;
		}
		zram_clear_flag(zram, index, ZRAM_ZLOGSWAP_FLUSH_VALID);
		zram_clear_flag(zram, index, ZRAM_ZLOGSWAP_UNDER_FLUSH);
		zram_slot_unlock(zram, index);
	}
	if (ext_id >= 0) {
		int finish = zlogswap_put_extent(zram, memcg, ext_id);

		if (WARN_ON_ONCE(finish) && !ret)
			ret = finish;
	}
	io->error = ret;
	if (io->bio)
		bio_put(io->bio);
	if (io->memcg_ref)
		mem_cgroup_put(memcg);
	complete(&io->done);
}

/* May run in atomic context: no slot processing, allocations or sleeping. */
static void zlogswap_flush_end_io(struct bio *bio)
{
	struct zlogswap_flush_io *io = bio->bi_private;

	io->error = blk_status_to_errno(bio->bi_status);
	queue_work(global_settings.flushing_complete_wq, &io->work);
}

static int zlogswap_flush_batch(struct zram *zram, struct mem_cgroup *memcg,
		struct zlogswap_flush_obj *objs, u32 nr, void *buffer,
		unsigned long *written)
{
	struct zlogswap_flush_io io = {
		.zram = zram, .memcg = memcg, .objs = objs, .nr = nr,
		.ext_id = -1,
	};
	struct bio *bio;
	u32 i;

	*written = 0;
	INIT_WORK_ONSTACK(&io.work, zlogswap_flush_complete_work);
	init_completion(&io.done);
	if (!mem_cgroup_tryget(memcg)) {
		io.error = -EAGAIN;
		goto queue_failure;
	}
	io.memcg_ref = true;
	io.ext_id = zlogswap_alloc_extent(zram, memcg);
	if (io.ext_id < 0) {
		io.error = io.ext_id;
		goto queue_failure;
	}
	bio = bio_alloc(zram->zlogswap_bdev, EXTENT_PG_CNT, REQ_OP_WRITE, GFP_NOIO);
	if (!bio) {
		io.error = -ENOMEM;
		goto queue_failure;
	}
	io.bio = bio;
	bio->bi_private = &io;
	bio->bi_end_io = zlogswap_flush_end_io;
	bio->bi_iter.bi_sector = (sector_t)io.ext_id << (EXTENT_SHIFT - 9);
	for (i = 0; i < EXTENT_PG_CNT; i++) {
		if (bio_add_page(bio, virt_to_page(buffer + i * PAGE_SIZE),
				 PAGE_SIZE, 0) != PAGE_SIZE) {
			io.error = -EIO;
			goto queue_failure;
		}
	}
	submit_bio(bio);
	goto wait;
queue_failure:
	/* Pre-submission failures use the same worker rollback path. */
	queue_work(global_settings.flushing_complete_wq, &io.work);
wait:
	/* Keep reclaim's final-success-byte API; the BIO itself is asynchronous. */
	wait_for_completion(&io.done);
	flush_work(&io.work);
	destroy_work_on_stack(&io.work);
	*written = io.written;
	return io.error;
}

/*
 * Snapshot the memcg's RAM LRU, oldest first. Never carry list cursors across
 * an unlock: overwrites, frees and readback can change membership concurrently.
 * Growth retries are bounded by nr_objs; allocations and slot locks stay outside
 * the list-head critical section. New objects after the snapshot are not chased.
 */
static int zlogswap_snapshot_lru(struct zram *zram,
		struct mem_cgroup_zlogswap *owner, u32 **indices, u32 *count)
{
	struct zlogswap *zs = zram->zlogswap;
	u32 capacity, nr, index, *snapshot;
	int head = mcg_idx(zs, owner->mcg_id);
	bool overflow;

	*indices = NULL;
	*count = 0;
	capacity = min_t(u32, ZLOGSWAP_BATCH_OBJS, zs->nr_objs);
	for (;;) {
		snapshot = kvmalloc_array(capacity, sizeof(*snapshot), GFP_NOIO);
		if (!snapshot)
			return -ENOMEM;
		nr = 0;
		overflow = false;
		zlogswap_lock_list(head, zs->obj_table);
		zlogswap_list_for_each_entry_reverse(index, head, zs->obj_table) {
			if (index >= zs->nr_objs) {
				zlogswap_unlock_list(head, zs->obj_table);
				kvfree(snapshot);
				return -EIO;
			}
			if (nr == capacity) {
				overflow = true;
				break;
			}
			snapshot[nr++] = index;
		}
		zlogswap_unlock_list(head, zs->obj_table);
		if (!overflow) {
			*indices = snapshot;
			*count = nr;
			return 0;
		}
		kvfree(snapshot);
		if (capacity == zs->nr_objs)
			return -EIO;
		capacity = min_t(u32, capacity * 2, zs->nr_objs);
		cond_resched();
	}
}

/*
 * Caller holds init_lock(read), owner reference and swap_lock.
 * Whole objects only: final object may exceed the requested remaining bytes
 * by less than PAGE_SIZE. Successful bytes exclude overwritten snapshots.
 */
static int zlogswap_permemcg_reclaim(struct mem_cgroup *memcg,
		unsigned long require_size, unsigned long *memcg_reclaimed_size)
{
	struct mem_cgroup_zlogswap *owner;
	struct zlogswap *zs;
	struct zram *zram;
	struct zlogswap_flush_obj *objs;
	unsigned long written, limit;
	u32 index, pos, nr_indices, *indices, nr = 0, used = 0;
	void *buffer;
	int ret = 0;

	if (!memcg_reclaimed_size)
		return -EINVAL;
	*memcg_reclaimed_size = 0;
	if (!memcg)
		return -EINVAL;
	if (!require_size)
		return 0;
	owner = MEMCGRP_ITEM_DATA(memcg);
	if (!owner || !smp_load_acquire(&owner->initialized))
		return -ENOENT;
	zram = owner->zram;
	ret = extent_args(zram, memcg, &zs);
	if (ret)
		return ret;
	lockdep_assert_held(&owner->swap_lock);
	buffer = (void *)__get_free_pages(GFP_NOIO | __GFP_ZERO, get_order(EXTENT_SIZE));
	if (!buffer)
		return -ENOMEM;
	objs = kcalloc(ZLOGSWAP_BATCH_OBJS, sizeof(*objs), GFP_NOIO);
	if (!objs) {
		free_pages((unsigned long)buffer, get_order(EXTENT_SIZE));
		return -ENOMEM;
	}
	ret = zlogswap_snapshot_lru(zram, owner, &indices, &nr_indices);
	if (ret)
		goto free_buffers;
	/* Only indices captured from this memcg's zram_lru are candidates. */
	for (pos = 0; pos < nr_indices; pos++) {
		unsigned long size;
		void *src;

		index = indices[pos];
retry_slot:
		zram_slot_lock(zram, index);
		if (!zram_get_handle(zram, index) ||
		    zram_test_flag(zram, index, ZRAM_SAME) ||
		    zram_test_flag(zram, index, ZRAM_WB) ||
		    zram_test_flag(zram, index, ZRAM_UNDER_WB) ||
		    zram_test_flag(zram, index, ZRAM_ZLOGSWAP_FLUSHED) ||
		    zram_test_flag(zram, index, ZRAM_ZLOGSWAP_UNDER_FLUSH) ||
		    slot_owner(zram, index) != memcg) {
			zram_slot_unlock(zram, index);
			goto next;
		}
		size = zram_get_obj_size(zram, index);
		if (!size || size > PAGE_SIZE) {
			zram_slot_unlock(zram, index);
			goto next;
		}
		if (size > EXTENT_SIZE - used) {
			zram_slot_unlock(zram, index);
			ret = zlogswap_flush_batch(zram, memcg, objs, nr, buffer, &written);
			*memcg_reclaimed_size += written;
			nr = used = 0;
			memset(buffer, 0, EXTENT_SIZE);
			if (ret || *memcg_reclaimed_size >= require_size)
				break;
			/* The candidate may have changed while the previous extent wrote. */
			goto retry_slot;
		}
		src = zs_map_object(zram->mem_pool, zram_get_handle(zram, index), ZS_MM_RO);
		memcpy(buffer + used, src, size);
		zs_unmap_object(zram->mem_pool, zram_get_handle(zram, index));
		objs[nr++] = (struct zlogswap_flush_obj){ index, used, size };
		used += size;
		zram_set_flag(zram, index, ZRAM_ZLOGSWAP_UNDER_FLUSH);
		zram_set_flag(zram, index, ZRAM_ZLOGSWAP_FLUSH_VALID);
		zram_slot_unlock(zram, index);
		limit = require_size - *memcg_reclaimed_size;
		if (used >= limit || used == EXTENT_SIZE || nr == ZLOGSWAP_BATCH_OBJS) {
			ret = zlogswap_flush_batch(zram, memcg, objs, nr, buffer, &written);
			*memcg_reclaimed_size += written;
			nr = used = 0;
			memset(buffer, 0, EXTENT_SIZE);
			if (ret || *memcg_reclaimed_size >= require_size)
				break;
		}
next:
		cond_resched();
	}
	if (nr) {
		ret = zlogswap_flush_batch(zram, memcg, objs, nr, buffer, &written);
		*memcg_reclaimed_size += written;
	}
	kvfree(indices);
free_buffers:
	kfree(objs);
	free_pages((unsigned long)buffer, get_order(EXTENT_SIZE));
	return ret;
}

/* Caller owns ext_id; always releases ownership, including partial failures. */
static int zlogswap_restore_owned_extent(struct zram *zram,
		struct mem_cgroup *memcg, int ext_id, unsigned long *promoted)
{
	struct zlogswap *zs = zram->zlogswap;
	struct mem_cgroup_zlogswap *owner;
	unsigned long entry, size, handle;
	u32 indices[ZLOGSWAP_BATCH_OBJS];
	unsigned int pos, nr = 0;
	u32 i, node;
	int ret, finish;
	void *buffer, *dst;

	owner = MEMCGRP_ITEM_DATA(memcg);
	buffer = (void *)__get_free_pages(GFP_NOIO, get_order(EXTENT_SIZE));
	if (!buffer) {
		ret = -ENOMEM;
		goto finish;
	}
	ret = zlogswap_extent_io(zram, ext_id, buffer, false);
	if (ret)
		goto release;
	/* No new members can be added while we own this extent. */
	zlogswap_lock_list(ext_idx(zs, ext_id), zs->obj_table);
	zlogswap_list_for_each_entry(node, ext_idx(zs, ext_id), zs->obj_table) {
		if (nr == ARRAY_SIZE(indices) || node >= zs->nr_objs) {
			ret = -EIO;
			break;
		}
		indices[nr++] = node;
	}
	zlogswap_unlock_list(ext_idx(zs, ext_id), zs->obj_table);
	if (ret)
		goto release;
	for (pos = 0; pos < nr; pos++) {
		i = indices[pos];
		zram_slot_lock(zram, i);
		entry = zram_get_handle(zram, i);
		if (!zram_test_flag(zram, i, ZRAM_ZLOGSWAP_FLUSHED) ||
		    entry >> EXTENT_SHIFT != ext_id) {
			zram_slot_unlock(zram, i);
			continue;
		}
		size = zram_get_obj_size(zram, i);
		zram_slot_unlock(zram, i);
		if (!size || size > PAGE_SIZE ||
		    (entry & (EXTENT_SIZE - 1)) + size > EXTENT_SIZE) {
			ret = -EIO;
			break;
		}
		handle = zs_malloc(zram->mem_pool, size, GFP_NOIO);
		if (IS_ERR((void *)handle)) {
			ret = PTR_ERR((void *)handle);
			break;
		}
		dst = zs_map_object(zram->mem_pool, handle, ZS_MM_WO);
		memcpy(dst, buffer + (entry & (EXTENT_SIZE - 1)), size);
		zs_unmap_object(zram->mem_pool, handle);
		zram_slot_lock(zram, i);
		if (!zram_test_flag(zram, i, ZRAM_ZLOGSWAP_FLUSHED) ||
		    zram_get_handle(zram, i) != entry) {
			zram_slot_unlock(zram, i);
			zs_free(zram->mem_pool, handle);
			continue;
		}
		zlogswap_list_del(i, ext_idx(zs, ext_id), zs->obj_table);
		WARN_ON_ONCE(zlogswap_extent_del_obj(zram, memcg, ext_id));
		zram_set_handle(zram, i, handle);
		zram_clear_flag(zram, i, ZRAM_ZLOGSWAP_FLUSHED);
		zlogswap_list_add(i, mcg_idx(zs, memcg->id.id), zs->obj_table);
		atomic64_sub(size, &owner->zlogswap_stored_size);
		atomic64_dec(&owner->zlogswap_page_cnt);
		atomic64_add(size, &owner->zram_stored_size);
		atomic64_inc(&owner->zram_page_cnt);
		__this_cpu_add(zram->pcp_stats->items[COMPRESSED_SIZE], size);
		if (promoted)
			*promoted += size;
		zram_slot_unlock(zram, i);
		cond_resched();
	}
release:
	free_pages((unsigned long)buffer, get_order(EXTENT_SIZE));
finish:
	finish = zlogswap_put_extent(zram, memcg, ext_id);
	return ret ? ret : finish;
}

/* Fault path: resolve the current slot, then restore its complete extent. */
int zlogswap_read_extent(struct zram *zram, u32 index)
{
	struct mem_cgroup *memcg;
	int ext_id, ret;

retry:
	zram_slot_lock(zram, index);
	if (!zram_test_flag(zram, index, ZRAM_ZLOGSWAP_FLUSHED)) {
		zram_slot_unlock(zram, index);
		return 0;
	}
	memcg = slot_owner(zram, index);
	if (!memcg) {
		zram_slot_unlock(zram, index);
		return -ENOENT;
	}
	ext_id = zram_get_handle(zram, index) >> EXTENT_SHIFT;
	ret = zlogswap_get_extent(zram, memcg, ext_id);
	zram_slot_unlock(zram, index);
	if (ret == -EBUSY) {
		usleep_range(100, 200);
		goto retry;
	}
	if (ret < 0)
		return ret;
	ret = zlogswap_restore_owned_extent(zram, memcg, ext_id, NULL);
	/* Unrelated objects may fail after the requested slot was restored. */
	zram_slot_lock(zram, index);
	if (ret && !zram_test_flag(zram, index, ZRAM_ZLOGSWAP_FLUSHED))
		ret = 0;
	zram_slot_unlock(zram, index);
	return ret;
}

/* Claim one available extent atomically with respect to get/put/delete. */
static int zlogswap_get_promote_extent(struct zram *zram,
		struct mem_cgroup *memcg)
{
	struct zlogswap *zs = zram->zlogswap;
	int head = mcg_idx(zs, memcg->id.id);
	unsigned long flags;
	u32 node;
	int ret = -ENOENT;

	spin_lock_irqsave(&zs->extent_lock, flags);
	zlogswap_lock_list(head, zs->ext_table);
	zlogswap_list_for_each_entry(node, head, zs->ext_table) {
		if (node < zs->nr_objs || node - zs->nr_objs >= zs->nr_exts) {
			ret = -EIO;
			break;
		}
		if (!zlogswap_list_clear_priv(node, zs->ext_table))
			continue;
		zlogswap_list_del_nolock(node, head, zs->ext_table);
		ret = node - zs->nr_objs;
		break;
	}
	zlogswap_unlock_list(head, zs->ext_table);
	spin_unlock_irqrestore(&zs->extent_lock, flags);
	return ret;
}

static int zlogswap_promote_memcg(struct zram *zram, struct mem_cgroup *memcg)
{
	struct mem_cgroup_zlogswap *owner;
	struct zlogswap *zs;
	unsigned long promoted = 0;
	int ret, ext_id;

	down_read(&zram->init_lock);
	if (!zram->zlogswap_bdev) {
		ret = -ENODEV;
		goto out;
	}
	ret = extent_args(zram, memcg, &zs);
	if (ret)
		goto out;
	owner = MEMCGRP_ITEM_DATA(memcg);
	mutex_lock(&owner->swap_lock);
	for (;;) {
		ext_id = zlogswap_get_promote_extent(zram, memcg);
		if (ext_id == -ENOENT) {
			/* Fault readers temporarily remove their owned extents. */
			if (atomic64_read(&owner->zlogswap_page_cnt)) {
				usleep_range(100, 200);
				continue;
			}
			ret = 0;
			break;
		}
		if (ext_id < 0) {
			ret = ext_id;
			break;
		}
		ret = zlogswap_restore_owned_extent(zram, memcg, ext_id, &promoted);
		if (ret)
			break;
		cond_resched();
	}
	mutex_unlock(&owner->swap_lock);
	log_info("memcg %u promote restored=%lu ret=%d", memcg->id.id, promoted, ret);
out:
	up_read(&zram->init_lock);
	return ret;
}

int zlogswap_force_reclaim(struct zram *zram, struct mem_cgroup *memcg)
{
	struct mem_cgroup_zlogswap *owner;
	unsigned long require_size, reclaimed = 0;
	int ret;

	if (!zram || !memcg)
		return -EINVAL;
	down_read(&zram->init_lock);
	if (!zram->zlogswap || !zram->zlogswap_bdev) {
		ret = -ENODEV;
		goto out;
	}
	owner = MEMCGRP_ITEM_DATA(memcg);
	if (!owner || !smp_load_acquire(&owner->initialized) || owner->zram != zram) {
		ret = -ENOENT;
		goto out;
	}
	mutex_lock(&owner->swap_lock);
	require_size = atomic64_read(&owner->zram_stored_size);
	ret = zlogswap_permemcg_reclaim(memcg, require_size, &reclaimed);
	mutex_unlock(&owner->swap_lock);
	log_info("memcg %u reclaim target=%lu committed=%lu ret=%d\n",
		memcg->id.id, require_size, reclaimed, ret);
out:
	up_read(&zram->init_lock);
	return ret;
}

/* Returns a referenced memcg, or ERR_PTR() on failure. */
struct mem_cgroup *get_memcg_by_pid(int pid_nr)
{
	struct pid *pid;
	struct task_struct *task;
	struct mem_cgroup *memcg;

	if (pid_nr <= 0)
		return ERR_PTR(-EINVAL);

	pid = find_get_pid(pid_nr);
	if (!pid) {
		log_err("invalid pid: %d!\n", pid_nr);
		return ERR_PTR(-ESRCH);
	}

	task = get_pid_task(pid, PIDTYPE_PID);
	put_pid(pid);

	if (!task) {
		log_err("failed to get task by pid: %d!\n", pid_nr);
		return ERR_PTR(-ESRCH);
	}

	rcu_read_lock();
	/* Equivalent to mem_cgroup_from_task(), without an external symbol. */
	memcg = mem_cgroup_from_css(task_css(task, memory_cgrp_id));
	if (!memcg) {
		log_err("failed to get memcg by task! pid: %d\n", pid_nr);
		memcg = ERR_PTR(-ENOENT);
	} else if (!mem_cgroup_tryget(memcg)) {
		log_err("!mem_cgroup_tryget(memcg), pid: %d!\n", pid_nr);
		memcg = ERR_PTR(-EAGAIN);
	}
	rcu_read_unlock();
	put_task_struct(task);

	return memcg;
}

ssize_t zlogswap_flush_pid_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t len)
{
	struct mem_cgroup *memcg;
	int pid_nr;
	int err;
	struct zram *zram;

	zram = dev_to_zram(dev);
	err = kstrtoint(buf, 10, &pid_nr);
	if (err) {
		log_err("failed to parse pid_str: %s", buf);
		return err;
	}
	if (pid_nr <= 0)
		return -EINVAL;

	if (mem_cgroup_disabled())
		return -EOPNOTSUPP;

	memcg = get_memcg_by_pid(pid_nr);
	if (IS_ERR(memcg)) {
		log_err("failed to get memcg, pid: %d!\n", pid_nr);
		return PTR_ERR(memcg);
	}

	log_info("flush pid %d, memcg id %u\n", pid_nr, (unsigned int)memcg->id.id);

	/* Flush objects from the selected memcg. */
	err = zlogswap_force_reclaim(zram, memcg);

	mem_cgroup_put(memcg);

	if (err)
		return err;
	return len;
}

/* Restore this PID's entire current memcg, not just its own swap slots. */
ssize_t zlogswap_promote_pid_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t len)
{
	struct mem_cgroup *memcg;
	int pid_nr, ret;

	ret = kstrtoint(buf, 10, &pid_nr);
	if (ret)
		return ret;
	if (pid_nr <= 0)
		return -EINVAL;
	if (mem_cgroup_disabled())
		return -EOPNOTSUPP;
	memcg = get_memcg_by_pid(pid_nr);
	if (IS_ERR(memcg))
		return PTR_ERR(memcg);
	log_info("promote pid %d, memcg id %u", pid_nr, memcg->id.id);
	ret = zlogswap_promote_memcg(dev_to_zram(dev), memcg);
	mem_cgroup_put(memcg);
	return ret ? ret : len;
}

static int init_mem_cgroup_zlogswap(struct zram *zram, struct mem_cgroup *memcg)
{
	struct mem_cgroup_zlogswap *owner = MEMCGRP_ITEM_DATA(memcg);
	unsigned short id;

	if (!zram || !zram->zlogswap || !owner)
		return -EINVAL;

	id = memcg->id.id;
	if (!id || id >= zram->zlogswap->nr_mcgs)
		return -EINVAL;

	owner->zram = zram;
	owner->memcg = memcg;
	owner->mcg_id = id;
	owner->gen = zram->zlogswap->gen;
	mutex_init(&owner->swap_lock);
	atomic64_set(&owner->zram_stored_size, 0);
	atomic64_set(&owner->zram_page_cnt, 0);
	atomic64_set(&owner->zlogswap_stored_size, 0);
	atomic64_set(&owner->zlogswap_page_cnt, 0);
	zlogswap_list_init(mcg_idx(zram->zlogswap, id), zram->zlogswap->obj_table);
	zlogswap_list_init(mcg_idx(zram->zlogswap, id), zram->zlogswap->ext_table);
	smp_store_release(&owner->initialized, true);

	log_dbg("create a new memcg_zlogswap, memcg id: %d\n", id);
	return 0;
}

struct mem_cgroup_zlogswap *get_memcg_zlogswap(struct zram *zram, struct mem_cgroup *memcg)
{
	struct mem_cgroup_zlogswap *owner;
	struct zlogswap *zlogswap;
	unsigned short id;
	int ret;

	if (!zram || !memcg)
		return NULL;

	id = memcg->id.id;
	zlogswap = zram->zlogswap;
	if (!zlogswap || !id || id >= zlogswap->nr_mcgs)
		return NULL;

	owner = (struct mem_cgroup_zlogswap *)READ_ONCE(memcg->android_oem_data1[0]);
	if (owner && smp_load_acquire(&owner->initialized) &&
	    owner->zram == zram && owner->gen == zlogswap->gen)
		return owner;

	spin_lock(&zlogswap->zlogswap_memcg_init_lock[id]);
	owner = (struct mem_cgroup_zlogswap *)READ_ONCE(memcg->android_oem_data1[0]);
	if (owner && smp_load_acquire(&owner->initialized) &&
	    owner->zram == zram && owner->gen == zlogswap->gen) {
		spin_unlock(&zlogswap->zlogswap_memcg_init_lock[id]);
		return owner;
	}
	if (!owner) {
		owner = kzalloc(sizeof(*owner), GFP_NOWAIT);
		if (!owner) {
			log_err("failed to alloc memcg_zlogswap!\n");
			spin_unlock(&zlogswap->zlogswap_memcg_init_lock[id]);
			return NULL;
		}
		owner->memcg = memcg;
		smp_store_release(&memcg->android_oem_data1[0], (u64)owner);
	}

	ret = init_mem_cgroup_zlogswap(zram, memcg);
	if (ret) {
		log_err("failed to init zlogswap_memcg! ret=%d id=%u\n", ret, id);
		spin_unlock(&zlogswap->zlogswap_memcg_init_lock[id]);
		return NULL;
	}
	spin_unlock(&zlogswap->zlogswap_memcg_init_lock[id]);
	return owner;
}

void zlogswap_track(struct zram *zram, u32 index, struct mem_cgroup *memcg)
{
	struct zlogswap *zlogswap = zram->zlogswap;

	if (!zlogswap) {
		log_dbg("zlogswap has not been initialized.\n");
		return;
	}

	if (!atomic_read(&global_settings.zlogswap_initialized))
		return;

	/* hybridswap: 没有 memcg 或 id 已被回收，就直接跳过。 */
	if (!memcg || !memcg->id.id)
		return;

	if (!get_memcg_zlogswap(zram, memcg)) {
		log_dbg("failed to get memcg_zlogswap!\n");
		return;
	}

	zlogswap_zram_lru_add(zram, index, memcg);
}

/* Called from android_vh_mem_cgroup_id_remove(), before the ID is removed. */
static void zlogswap_memcg_deinit(struct mem_cgroup *memcg)
{
	struct mem_cgroup_zlogswap *owner = MEMCGRP_ITEM_DATA(memcg);
	struct zram *zram;
	struct zlogswap *zs;
	u32 head, index, *batch;
	int nr, i;

	if (!owner || !owner->zram || !memcg->id.id)
		return;

	zram = owner->zram;
	zs = zram->zlogswap;
	if (!zs || memcg->id.id >= zs->nr_mcgs)
		return;

	head = mcg_idx(zs, memcg->id.id);
	batch = kvmalloc_array(ZLOGSWAP_BATCH_OBJS, sizeof(*batch), GFP_NOIO);
	if (!batch) {
		log_err("deinit: failed to alloc batch\n");
		return;
	}

	for (;;) {
		nr = 0;
		zlogswap_lock_list(head, zs->obj_table);
		zlogswap_list_for_each_entry(index, head, zs->obj_table) {
			if (nr >= ZLOGSWAP_BATCH_OBJS)
				break;
			batch[nr++] = index;
		}
		zlogswap_unlock_list(head, zs->obj_table);
		if (!nr)
			break;
		for (i = 0; i < nr; i++) {
			index = batch[i];
			if (index >= zs->nr_objs)
				continue;
			zram_slot_lock(zram, index);
			if (zram_get_memcg(zram, index) == memcg) {
				zlogswap_list_del(index, head, zs->obj_table);
				zram_set_memcg(zram, index, 0);
			}
			zram_slot_unlock(zram, index);
		}
	}
	kvfree(batch);

	/*
	 * Flushed objects live in extents.  Their slots still carry this
	 * memcg's id, so clear it and detach the extents from the memcg's
	 * list; otherwise a recycled id would later resolve to another
	 * memcg.  The extent itself is left allocated (hybridswap frees it
	 * via put_extent(); that path needs the extent rmap to be empty).
	 */
	for (;;) {
		u32 eidx;
		bool found = false;

		zlogswap_lock_list(head, zs->ext_table);
		zlogswap_list_for_each_entry(eidx, head, zs->ext_table) {
			found = true;
			break;
		}
		zlogswap_unlock_list(head, zs->ext_table);
		if (!found)
			break;

		for (;;) {
			u32 oidx;
			bool more = false;

			zlogswap_lock_list(eidx, zs->obj_table);
			zlogswap_list_for_each_entry(oidx, eidx, zs->obj_table) {
				more = true;
				break;
			}
			zlogswap_unlock_list(eidx, zs->obj_table);
			if (!more)
				break;

			zram_slot_lock(zram, oidx);
			zram_set_memcg(zram, oidx, 0);
			zlogswap_list_del(oidx, eidx, zs->obj_table);
			zram_slot_unlock(zram, oidx);
		}

		zlogswap_list_del(eidx, head, zs->ext_table);
		zlogswap_list_set_mcgid(eidx, zs->ext_table, 0);
	}

	smp_store_release(&owner->initialized, false);
	owner->zram = NULL;
	log_dbg("deinit memcg %d done\n", memcg->id.id);
}

static void zlogswap_mem_cgroup_alloc_hook(void *data, struct mem_cgroup *memcg)
{
	struct mem_cgroup_zlogswap *owner;
	u64 ret;

	if (mem_cgroup_disabled() || !memcg)
		return;
	if (READ_ONCE(memcg->android_oem_data1[0]))
		return;

	owner = kzalloc(sizeof(*owner), GFP_NOWAIT);
	if (!owner) {
		log_err("alloc hook: failed to alloc memcg_zlogswap\n");
		return;
	}
	owner->memcg = memcg;
	ret = atomic64_cmpxchg((atomic64_t *)&memcg->android_oem_data1[0],
			       0, (u64)owner);
	if (ret)
		kfree(owner);
}

static void zlogswap_mem_cgroup_free_hook(void *data, struct mem_cgroup *memcg)
{
	struct mem_cgroup_zlogswap *owner =
		(struct mem_cgroup_zlogswap *)READ_ONCE(memcg->android_oem_data1[0]);

	if (!owner)
		return;
	WRITE_ONCE(memcg->android_oem_data1[0], 0ULL);
	WARN_ON_ONCE(owner->zram);
	kfree(owner);
}

static void zlogswap_mem_cgroup_id_remove_hook(void *data, struct mem_cgroup *memcg)
{
	zlogswap_memcg_deinit(memcg);
}

int zlogswap_register_hooks(void)
{
	int rc;

	rc = register_trace_android_vh_mem_cgroup_alloc(zlogswap_mem_cgroup_alloc_hook, NULL);
	if (rc)
		return rc;
	rc = register_trace_android_vh_mem_cgroup_free(zlogswap_mem_cgroup_free_hook, NULL);
	if (rc) {
		unregister_trace_android_vh_mem_cgroup_alloc(zlogswap_mem_cgroup_alloc_hook, NULL);
		return rc;
	}
	rc = register_trace_android_vh_mem_cgroup_id_remove(zlogswap_mem_cgroup_id_remove_hook, NULL);
	if (rc) {
		unregister_trace_android_vh_mem_cgroup_free(zlogswap_mem_cgroup_free_hook, NULL);
		unregister_trace_android_vh_mem_cgroup_alloc(zlogswap_mem_cgroup_alloc_hook, NULL);
		return rc;
	}
	return 0;
}

void zlogswap_unregister_hooks(void)
{
	unregister_trace_android_vh_mem_cgroup_id_remove(zlogswap_mem_cgroup_id_remove_hook, NULL);
	unregister_trace_android_vh_mem_cgroup_free(zlogswap_mem_cgroup_free_hook, NULL);
	unregister_trace_android_vh_mem_cgroup_alloc(zlogswap_mem_cgroup_alloc_hook, NULL);
}
