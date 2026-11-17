#include <linux/blkdev.h>
#include <linux/bitmap.h>
#include <linux/lockdep.h>

#include "zlogswap.h"
#include "zram_drv.h"

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
}

void zlogswap_zram_lru_add(struct zram *zram, u32 index, struct mem_cgroup *memcg)
{
	zram_lru_add(zram, index, memcg);
}

// init zlogswap

#define DEVICE_NAME_LEN 64
static char loop_device[DEVICE_NAME_LEN];

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

    zlogswap->size = (unsigned long)(i_size_read(zram->zlogswap_backing_dev->f_mapping->host) >> PAGE_SHIFT << PAGE_SHIFT);
    zlogswap->nr_objs = zram->disksize >> PAGE_SHIFT;
    zlogswap->nr_exts = zlogswap->size >> EXTENT_SHIFT;
    /* Exclusive ID limit: valid memcg IDs are 1..MEM_CGROUP_ID_MAX. */
    zlogswap->nr_mcgs = MEM_CGROUP_ID_MAX + 1;
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

    atomic_set(&zlogswap->initialized, 1);
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

int init_mem_cgroup_zlogswap(struct zram *zram, struct mem_cgroup *memcg)
{
    struct mem_cgroup_zlogswap *memcg_zlogswap = MEMCGRP_ITEM_DATA(memcg);

    if (!zram || !zram->zlogswap) {
        log_err("invalid args for init_mem_cgroup_zlogswap!\n");
        return -EINVAL;
    }

    memcg_zlogswap->zram = zram;
    memcg_zlogswap->memcg = memcg;
    zlogswap_list_init(mcg_idx(zram->zlogswap, memcg->id.id), zram->zlogswap->obj_table);
    zlogswap_list_init(mcg_idx(zram->zlogswap, memcg->id.id), zram->zlogswap->ext_table);
	smp_store_release(&memcg_zlogswap->initialized, true);

    log_dbg("create a new memcg_zlogswap, memcg id: %d\n", memcg->id.id);
    
    return 0;
}

// TODO: hook mem_cgroup_alloc/free to alloc/free struct mem_cgroup_zlogswap
struct mem_cgroup_zlogswap *get_zlogswap_memcg(struct zram *zram, struct mem_cgroup *memcg)
{
    struct mem_cgroup_zlogswap *memcg_zlogswap;
    struct zlogswap *zlogswap;
    int ret;

    if (!zram || !memcg) {
        return NULL;
    }

    zlogswap = zram->zlogswap;
    if (!zlogswap || !memcg->id.id ||
        memcg->id.id >= zlogswap->nr_mcgs) {
        return NULL;
    }

    // memcg_zlogswap = MEMCGRP_ITEM_DATA(memcg);
	memcg_zlogswap = (struct mem_cgroup_zlogswap*)READ_ONCE(memcg->android_oem_data1[0]);
    if (memcg_zlogswap && smp_load_acquire(&memcg_zlogswap->initialized))
        return memcg_zlogswap;
    
    spin_lock(&zlogswap->zlogswap_memcg_init_lock[memcg->id.id]);
    // memcg_zlogswap = MEMCGRP_ITEM_DATA(memcg);
	memcg_zlogswap = (struct mem_cgroup_zlogswap*)READ_ONCE(memcg->android_oem_data1[0]);

	if (memcg_zlogswap) {
		if (memcg_zlogswap->initialized) {
			spin_unlock(&zlogswap->zlogswap_memcg_init_lock[memcg->id.id]);
			return memcg_zlogswap;
		}
	}
	else {
		memcg_zlogswap = kzalloc(sizeof(struct mem_cgroup_zlogswap), GFP_NOWAIT);
		if (!memcg_zlogswap) {
			log_err("failed to alloc memcg_zlogswap!\n");
			goto err_out;
		}
		smp_store_release(&memcg->android_oem_data1[0], (unsigned long long)memcg_zlogswap);
	}

    ret = init_mem_cgroup_zlogswap(zram, memcg);
    if (ret) {
        log_err("failed to init zlogswap_memcg!\n");
        goto err_out;
    }
    spin_unlock(&zlogswap->zlogswap_memcg_init_lock[memcg->id.id]);
    return memcg_zlogswap;

err_out:
    // kfree(memcg_zlogswap);
	// memcg->android_oem_data1[0] = (unsigned long long)NULL;
    spin_unlock(&zlogswap->zlogswap_memcg_init_lock[memcg->id.id]);
    return NULL;
}

void zlogswap_track(struct zram *zram, u32 index, struct mem_cgroup *memcg)
{
    struct mem_cgroup_zlogswap *memcg_zlogswap;
    struct zlogswap *zlogswap = zram->zlogswap;

    if (!zlogswap) {
        log_dbg("zlogswap has not been initialized.\n");
        return;
    }

    if (!atomic_read(&zlogswap->initialized))
        return;

    if (!memcg || !memcg->id.id) {
        log_warn("zlogswap_track received a obj without memcg id!\n");
        return;
    }

    memcg_zlogswap = get_zlogswap_memcg(zram, memcg);
	if (!memcg_zlogswap) {
		log_err("failed to get memcg_zlogswap!\n");
		return;
	}

    zlogswap_zram_lru_add(zram, index, memcg);
}