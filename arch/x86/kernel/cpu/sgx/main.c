// SPDX-License-Identifier: (GPL-2.0 OR BSD-3-Clause)
// Copyright(c) 2016-17 Intel Corporation.

#include <linux/file.h>
#include <linux/freezer.h>
#include <linux/highmem.h>
#include <linux/kthread.h>
#include <linux/miscdevice.h>
#include <linux/pagemap.h>
#include <linux/ratelimit.h>
#include <linux/sched/mm.h>
#include <linux/sched/signal.h>
#include <linux/slab.h>

#include <asm/sgx_arch.h>

#include "driver.h"
#include "encl.h"
#include "encls.h"
#include "epc_cgroup.h"
#include "virt.h"

/* A per-cpu cache for the last known values of IA32_SGXLEPUBKEYHASHx MSRs. */
static DEFINE_PER_CPU(u64 [4], sgx_lepubkeyhash_cache);

void sgx_update_lepubkeyhash_msrs(u64 *lepubkeyhash, bool enforce)
{
	u64 *cache;
	int i;

	cache = per_cpu(sgx_lepubkeyhash_cache, smp_processor_id());
	for (i = 0; i < 4; i++) {
		if (enforce || (lepubkeyhash[i] != cache[i])) {
			wrmsrl(MSR_IA32_SGXLEPUBKEYHASH0 + i, lepubkeyhash[i]);
			cache[i] = lepubkeyhash[i];
		}
	}
}

#define SGX_MAX_NR_TO_RECLAIM	32

struct sgx_epc_section sgx_epc_sections[SGX_MAX_EPC_SECTIONS];
static int sgx_nr_epc_sections;
static struct task_struct *ksgxswapd_tsk;
static DECLARE_WAIT_QUEUE_HEAD(ksgxswapd_waitq);
static struct sgx_epc_lru sgx_global_lru;

static inline struct sgx_epc_lru *sgx_lru(struct sgx_epc_page *epc_page)
{
#ifdef CONFIG_CGROUP_SGX_EPC
	if (epc_page->epc_cg)
		return &epc_page->epc_cg->lru;
#endif

	return &sgx_global_lru;
}

/**
 * sgx_record_epc_page() - Add a page to the LRU tracking
 * @page:	EPC page
 * @flags:	Reclaim flags for the page.
 *
 * Mark a page with the specified flags and add it to the appropriate
 * (un)reclaimable list.
 */
void sgx_record_epc_page(struct sgx_epc_page *page, unsigned long flags)
{
	struct sgx_epc_lru *lru = sgx_lru(page);

	spin_lock(&lru->lock);
	WARN_ON(page->desc & SGX_EPC_PAGE_RECLAIM_FLAGS);
	page->desc |= flags;
	if (flags & SGX_EPC_PAGE_RECLAIMABLE)
		list_add_tail(&page->list, &lru->reclaimable);
	else
		list_add_tail(&page->list, &lru->unreclaimable);
	spin_unlock(&lru->lock);
}

/**
 * sgx_drop_epc_page() - Remove a page from a LRU list
 * @page:	EPC page
 *
 * Clear the reclaimable flag if set and remove the page from its LRU.
 *
 * Return:
 *   0 on success,
 *   -EBUSY if the page is in the process of being reclaimed
 */
int sgx_drop_epc_page(struct sgx_epc_page *page)
{
	struct sgx_epc_lru *lru = sgx_lru(page);

	/*
	 * Remove the page from its LRU list.  If the page is actively being
	 * reclaimed, return -EBUSY as we can't free the page at this time
	 * since it is "owned" by the reclaimer.
	 */
	spin_lock(&lru->lock);
	if ((page->desc & SGX_EPC_PAGE_RECLAIMABLE) &&
	    (page->desc & SGX_EPC_PAGE_RECLAIM_IN_PROGRESS)) {
		spin_unlock(&lru->lock);
		return -EBUSY;
	}
	list_del(&page->list);
	spin_unlock(&lru->lock);

	page->desc &= ~SGX_EPC_PAGE_RECLAIM_FLAGS;

	return 0;
}

static bool sgx_reclaimer_age(struct sgx_epc_page *epc_page)
{
	struct sgx_encl_page *page = epc_page->owner;
	struct sgx_encl *encl = page->encl;
	struct sgx_encl_mm *encl_mm;
	bool ret = true;
	int idx;

	idx = srcu_read_lock(&encl->srcu);

	list_for_each_entry_rcu(encl_mm, &encl->mm_list, list) {
		if (!mmget_not_zero(encl_mm->mm))
			continue;

		mmap_read_lock(encl_mm->mm);
		ret = !sgx_encl_test_and_clear_young(encl_mm->mm, page);
		mmap_read_unlock(encl_mm->mm);

		mmput_async(encl_mm->mm);

		if (!ret || (atomic_read(&encl->flags) & SGX_ENCL_DEAD_OR_OOM))
			break;
	}

	srcu_read_unlock(&encl->srcu, idx);

	if (!ret && !(atomic_read(&encl->flags) & SGX_ENCL_DEAD_OR_OOM))
		return false;

	return true;
}

static void sgx_reclaimer_block(struct sgx_epc_page *epc_page)
{
	struct sgx_encl_page *page = epc_page->owner;
	unsigned long addr = SGX_ENCL_PAGE_ADDR(page);
	struct sgx_encl *encl = page->encl;
	unsigned long mm_list_version;
	struct sgx_encl_mm *encl_mm;
	struct vm_area_struct *vma;
	int idx, ret;

	do {
		mm_list_version = encl->mm_list_version;

		/* Pairs with smp_rmb() in sgx_encl_mm_add(). */
		smp_rmb();

		idx = srcu_read_lock(&encl->srcu);

		list_for_each_entry_rcu(encl_mm, &encl->mm_list, list) {
			if (!mmget_not_zero(encl_mm->mm))
				continue;

			mmap_read_lock(encl_mm->mm);

			ret = sgx_encl_find(encl_mm->mm, addr, &vma);
			if (!ret && encl == vma->vm_private_data)
				zap_vma_ptes(vma, addr, PAGE_SIZE);

			mmap_read_unlock(encl_mm->mm);

			mmput_async(encl_mm->mm);
		}

		srcu_read_unlock(&encl->srcu, idx);
	} while (unlikely(encl->mm_list_version != mm_list_version));

	mutex_lock(&encl->lock);

	/* Note, EBLOCK can only be skipped if the enclave if fully dead. */
	if (!(atomic_read(&encl->flags) & SGX_ENCL_DEAD)) {
		ret = __eblock(sgx_get_epc_addr(epc_page));
		if (encls_failed(ret))
			ENCLS_WARN(ret, "EBLOCK");
	}

	mutex_unlock(&encl->lock);
}

static int __sgx_encl_ewb(struct sgx_epc_page *epc_page, void *va_slot,
			  struct sgx_backing *backing)
{
	struct sgx_pageinfo pginfo;
	int ret;

	pginfo.addr = 0;
	pginfo.secs = 0;

	pginfo.contents = (unsigned long)kmap_atomic(backing->contents);
	pginfo.metadata = (unsigned long)kmap_atomic(backing->pcmd) +
			  backing->pcmd_offset;

	ret = __ewb(&pginfo, sgx_get_epc_addr(epc_page), va_slot);

	kunmap_atomic((void *)(unsigned long)(pginfo.metadata -
					      backing->pcmd_offset));
	kunmap_atomic((void *)(unsigned long)pginfo.contents);

	return ret;
}

static void sgx_ipi_cb(void *info)
{
}

static const cpumask_t *sgx_encl_ewb_cpumask(struct sgx_encl *encl)
{
	cpumask_t *cpumask = &encl->cpumask;
	struct sgx_encl_mm *encl_mm;
	int idx;

	/*
	 * Can race with sgx_encl_mm_add(), but ETRACK has already been
	 * executed, which means that the CPUs running in the new mm will enter
	 * into the enclave with a fresh epoch.
	 */
	cpumask_clear(cpumask);

	idx = srcu_read_lock(&encl->srcu);

	list_for_each_entry_rcu(encl_mm, &encl->mm_list, list) {
		if (!mmget_not_zero(encl_mm->mm))
			continue;

		cpumask_or(cpumask, cpumask, mm_cpumask(encl_mm->mm));

		mmput_async(encl_mm->mm);
	}

	srcu_read_unlock(&encl->srcu, idx);

	return cpumask;
}

static void sgx_encl_ewb(struct sgx_epc_page *epc_page,
			 struct sgx_backing *backing)
{
	struct sgx_encl_page *encl_page = epc_page->owner;
	struct sgx_encl *encl = encl_page->encl;
	struct sgx_va_page *va_page;
	unsigned int va_offset;
	void *va_slot;
	int ret;

	encl_page->desc &= ~SGX_ENCL_PAGE_RECLAIMED;

	va_page = list_first_entry(&encl->va_pages, struct sgx_va_page,
				   list);
	va_offset = sgx_alloc_va_slot(va_page);
	va_slot = sgx_get_epc_addr(va_page->epc_page) + va_offset;
	if (sgx_va_page_full(va_page))
		list_move_tail(&va_page->list, &encl->va_pages);

	ret = __sgx_encl_ewb(epc_page, va_slot, backing);
	if (ret == SGX_NOT_TRACKED) {
		ret = __etrack(sgx_get_epc_addr(encl->secs.epc_page));
		if (ret) {
			if (encls_failed(ret))
				ENCLS_WARN(ret, "ETRACK");
		}

		ret = __sgx_encl_ewb(epc_page, va_slot, backing);
		if (ret == SGX_NOT_TRACKED) {
			/*
			 * Slow path, send IPIs to kick cpus out of the
			 * enclave.  Note, it's imperative that the cpu
			 * mask is generated *after* ETRACK, else we'll
			 * miss cpus that entered the enclave between
			 * generating the mask and incrementing epoch.
			 */
			on_each_cpu_mask(sgx_encl_ewb_cpumask(encl),
					 sgx_ipi_cb, NULL, 1);
			ret = __sgx_encl_ewb(epc_page, va_slot, backing);
		}
	}

	if (ret) {
		if (encls_failed(ret))
			ENCLS_WARN(ret, "EWB");

		sgx_free_va_slot(va_page, va_offset);
	} else {
		encl_page->desc |= va_offset;
		encl_page->va_page = va_page;
	}
}

static void sgx_reclaimer_write(struct sgx_epc_page *epc_page,
				struct sgx_backing *backing)
{
	struct sgx_encl_page *encl_page = epc_page->owner;
	struct sgx_encl *encl = encl_page->encl;
	struct sgx_backing secs_backing;
	int ret;

	mutex_lock(&encl->lock);

	/* Note, EWB can only be skipped if the enclave if fully dead. */
	if (atomic_read(&encl->flags) & SGX_ENCL_DEAD) {
		ret = __eremove(sgx_get_epc_addr(epc_page));
		ENCLS_WARN(ret, "EREMOVE");
	} else {
		sgx_encl_ewb(epc_page, backing);
	}

	encl_page->epc_page = NULL;
	encl->secs_child_cnt--;

	if (!encl->secs_child_cnt) {
		if (atomic_read(&encl->flags) & SGX_ENCL_DEAD) {
			sgx_drop_epc_page(encl->secs.epc_page);
			sgx_free_epc_page(encl->secs.epc_page);
			encl->secs.epc_page = NULL;
		} else if (atomic_read(&encl->flags) & SGX_ENCL_INITIALIZED) {
			ret = sgx_encl_get_backing(encl, PFN_DOWN(encl->size),
						   &secs_backing);
			if (ret)
				goto out;

			sgx_encl_ewb(encl->secs.epc_page, &secs_backing);

			sgx_drop_epc_page(encl->secs.epc_page);
			sgx_free_epc_page(encl->secs.epc_page);
			encl->secs.epc_page = NULL;

			sgx_encl_put_backing(&secs_backing, true);
		}
	}

out:
	mutex_unlock(&encl->lock);
}

/**
 * sgx_isolate_epc_pages - Isolate pages from an LRU for reclaim
 * @lru		LRU from which to reclaim
 * @nr_to_scan	Number of pages to scan for reclaim
 * @dst		Destination list to hold the isolated pages
 */
void sgx_isolate_epc_pages(struct sgx_epc_lru *lru, int *nr_to_scan,
			   struct list_head *dst)
{
	struct sgx_encl_page *encl_page;
	struct sgx_epc_page *epc_page;

	spin_lock(&lru->lock);
	for (; *nr_to_scan > 0; --(*nr_to_scan)) {
		if (list_empty(&lru->reclaimable))
			break;

		epc_page = list_first_entry(&lru->reclaimable,
					    struct sgx_epc_page, list);

		encl_page = epc_page->owner;
		if (WARN_ON_ONCE(!(epc_page->desc & SGX_EPC_PAGE_ENCLAVE)))
			continue;

		if (kref_get_unless_zero(&encl_page->encl->refcount)) {
			epc_page->desc |= SGX_EPC_PAGE_RECLAIM_IN_PROGRESS;
			list_move_tail(&epc_page->list, dst);
		} else {
			/*
			 * The owner is freeing the page, remove it from the
			 * LRU list.
			 */
			epc_page->desc &= ~SGX_EPC_PAGE_RECLAIMABLE;
			list_del_init(&epc_page->list);
		}
	}
	spin_unlock(&lru->lock);
}

/**
 * sgx_reclaim_epc_pages() - Reclaim EPC pages from the consumers
 * @nr_to_scan:		Number of EPC pages to scan for reclaim
 * @ignore_age:		Reclaim a page even if it is young
 * @epc_cg:		EPC cgroup from which to reclaim
 *
 * Take a fixed number of pages from the head of the active page pool and
 * reclaim them to the enclave's private shmem files. Skip the pages, which have
 * been accessed since the last scan. Move those pages to the tail of active
 * page pool so that the pages get scanned in LRU like fashion.
 *
 * Return: number of EPC pages reclaimed
 */
int sgx_reclaim_epc_pages(int nr_to_scan, bool ignore_age,
			  struct sgx_epc_cgroup *epc_cg)
{
	struct sgx_backing backing[SGX_MAX_NR_TO_RECLAIM];
	struct sgx_epc_page *epc_page, *tmp;
	struct sgx_epc_section *section;
	struct sgx_encl_page *encl_page;
	struct sgx_epc_lru *lru;
	LIST_HEAD(iso);
	int i = 0;
	int ret;

        /*
         * If we're not targeting a specific cgroup, take from the global
         * list first, even when cgroups are enabled.  If we somehow have
         * pages on the global LRU then they should get reclaimed asap.
         */
        if (!IS_ENABLED(CONFIG_CGROUP_SGX_EPC) || !epc_cg)
                sgx_isolate_epc_pages(&sgx_global_lru, &nr_to_scan, &iso);

#ifdef CONFIG_CGROUP_SGX_EPC
	sgx_epc_cgroup_isolate_pages(epc_cg, &nr_to_scan, &iso);
#endif

	if (list_empty(&iso))
		goto out;

	list_for_each_entry_safe(epc_page, tmp, &iso, list) {
		encl_page = epc_page->owner;

		if (i == SGX_MAX_NR_TO_RECLAIM ||
		    (!ignore_age && !sgx_reclaimer_age(epc_page)))
			goto skip;

		ret = sgx_encl_get_backing(encl_page->encl,
					   SGX_ENCL_PAGE_INDEX(encl_page),
					   &backing[i]);
		if (ret)
			goto skip;

		i++;
		mutex_lock(&encl_page->encl->lock);
		encl_page->desc |= SGX_ENCL_PAGE_RECLAIMED;
		mutex_unlock(&encl_page->encl->lock);
		continue;

skip:
		lru = sgx_lru(epc_page);
		spin_lock(&lru->lock);
		epc_page->desc &= ~SGX_EPC_PAGE_RECLAIM_IN_PROGRESS;
		list_move_tail(&epc_page->list, &lru->reclaimable);
		spin_unlock(&lru->lock);

		kref_put(&encl_page->encl->refcount, sgx_encl_release);
	}

	list_for_each_entry(epc_page, &iso, list)
		sgx_reclaimer_block(epc_page);

	i = 0;
	list_for_each_entry_safe(epc_page, tmp, &iso, list) {
		encl_page = epc_page->owner;
		sgx_reclaimer_write(epc_page, &backing[i]);
		sgx_encl_put_backing(&backing[i++], true);

		kref_put(&encl_page->encl->refcount, sgx_encl_release);
		epc_page->desc &= ~SGX_EPC_PAGE_RECLAIM_FLAGS;

#ifdef CONFIG_CGROUP_SGX_EPC
		if (epc_page->epc_cg) {
			sgx_epc_cgroup_uncharge(epc_page->epc_cg, true);
			epc_page->epc_cg = NULL;
		}
#endif

		section = sgx_get_epc_section(epc_page);
		spin_lock(&section->lock);
		list_move_tail(&epc_page->list, &section->page_list);
		section->free_cnt++;
		spin_unlock(&section->lock);
	}

out:
	cond_resched();
	return i;
}

static bool sgx_oom_get_ref(struct sgx_epc_page *epc_page)
{
	struct sgx_encl *encl;

	if (epc_page->desc & SGX_EPC_PAGE_ENCLAVE)
		encl = ((struct sgx_encl_page *)epc_page->owner)->encl;
	else if (epc_page->desc & SGX_EPC_PAGE_VERSION_ARRAY)
		encl = epc_page->owner;
	else
		return sgx_virt_epc_get_ref(epc_page);

	return kref_get_unless_zero(&encl->refcount);
}

static struct sgx_epc_page *sgx_oom_get_victim(struct sgx_epc_lru *lru)
{
	struct sgx_epc_page *epc_page, *tmp;

	if (list_empty(&lru->unreclaimable))
		return NULL;

	list_for_each_entry_safe(epc_page, tmp, &lru->unreclaimable, list) {
		list_del_init(&epc_page->list);

		if (sgx_oom_get_ref(epc_page))
			return epc_page;
	}
	return NULL;
}

void sgx_epc_oom_zap(void *owner, struct mm_struct *mm, unsigned long start,
		     unsigned long end, const struct vm_operations_struct *ops)
{
	struct vm_area_struct *vma, *tmp;
	unsigned long vm_end;

	vma = find_vma(mm, start);
	if (!vma || vma->vm_ops != ops || vma->vm_private_data != owner ||
	    vma->vm_start >= end)
		return;

	for (tmp = vma; tmp->vm_start < end; tmp = tmp->vm_next) {
		do {
			vm_end = tmp->vm_end;
			tmp = tmp->vm_next;
		} while (tmp && tmp->vm_ops == ops &&
			 vma->vm_private_data == owner && tmp->vm_start < end);

		zap_page_range(vma, vma->vm_start, vm_end - vma->vm_start);

		if (!tmp)
			break;
	}
}

static void sgx_oom_encl(struct sgx_encl *encl)
{
	unsigned long mm_list_version;
	struct sgx_encl_mm *encl_mm;
	int encl_flags;
	int idx;

	/* Set OOM under encl->lock to ensure faults won't insert new PTEs. */
	mutex_lock(&encl->lock);
	encl_flags = atomic_fetch_or(SGX_ENCL_OOM, &encl->flags);
	mutex_unlock(&encl->lock);

	if ((encl_flags & SGX_ENCL_DEAD_OR_OOM) ||
	    !(encl_flags & SGX_ENCL_CREATED))
		goto out;

	do {
		mm_list_version = encl->mm_list_version;

		/* Pairs with smp_rmb() in sgx_encl_mm_add(). */
		smp_rmb();

		idx = srcu_read_lock(&encl->srcu);

		list_for_each_entry_rcu(encl_mm, &encl->mm_list, list) {
			if (!mmget_not_zero(encl_mm->mm))
				continue;

			mmap_read_lock(encl_mm->mm);

			sgx_epc_oom_zap(encl, encl_mm->mm, encl->base,
					encl->base + encl->size, &sgx_vm_ops);

			mmap_read_unlock(encl_mm->mm);

			mmput_async(encl_mm->mm);
		}

		srcu_read_unlock(&encl->srcu, idx);
	} while (WARN_ON_ONCE(encl->mm_list_version != mm_list_version));

	mutex_lock(&encl->lock);
	sgx_encl_destroy(encl);
	mutex_unlock(&encl->lock);

out:
	kref_put(&encl->refcount, sgx_encl_release);
}

static inline void sgx_oom_encl_page(struct sgx_encl_page *encl_page)
{
	return sgx_oom_encl(encl_page->encl);
}

/**
 * sgx_epc_oom - invoke EPC out-of-memory handling on target LRU
 * @lru		LRU that is OOM
 *
 * Return: %true if a victim was found and kicked.
 */
bool sgx_epc_oom(struct sgx_epc_lru *lru)
{
	struct sgx_epc_page *victim;

	spin_lock(&lru->lock);
	victim = sgx_oom_get_victim(lru);
	spin_unlock(&lru->lock);

	if (!victim)
		return false;

	if (victim->desc & SGX_EPC_PAGE_ENCLAVE)
		sgx_oom_encl_page(victim->owner);
	else if (victim->desc & SGX_EPC_PAGE_VERSION_ARRAY)
		sgx_oom_encl(victim->owner);
	else
		sgx_virt_epc_oom(victim);

	return true;
}

static void sgx_sanitize_section(struct sgx_epc_section *section)
{
	struct sgx_epc_page *page;
	LIST_HEAD(secs_list);
	int ret;

	while (!list_empty(&section->unsanitized_page_list)) {
		if (kthread_should_stop())
			return;

		spin_lock(&section->lock);

		page = list_first_entry(&section->unsanitized_page_list,
					struct sgx_epc_page, list);

		ret = __eremove(sgx_get_epc_addr(page));
		if (!ret)
			list_move(&page->list, &section->page_list);
		else
			list_move_tail(&page->list, &secs_list);

		spin_unlock(&section->lock);

		cond_resched();
	}
}

static unsigned long sgx_nr_free_pages(void)
{
	unsigned long cnt = 0;
	int i;

	for (i = 0; i < sgx_nr_epc_sections; i++)
		cnt += sgx_epc_sections[i].free_cnt;

	return cnt;
}

static bool sgx_can_reclaim(void)
{
#ifdef CONFIG_CGROUP_SGX_EPC
	return !sgx_epc_cgroup_lru_empty(NULL);
#else
	return !list_empty(&sgx_global_lru.reclaimable);
#endif
}

static bool sgx_should_reclaim(unsigned long watermark)
{
	return sgx_nr_free_pages() < watermark && sgx_can_reclaim();
}

static int ksgxswapd(void *p)
{
	int i;

	set_freezable();

	/*
	 * Reset all pages to uninitialized state. Pages could be in initialized
	 * on kmemexec.
	 */
	for (i = 0; i < sgx_nr_epc_sections; i++)
		sgx_sanitize_section(&sgx_epc_sections[i]);

	/*
	 * 2nd round for the SECS pages as they cannot be removed when they
	 * still hold child pages.
	 */
	for (i = 0; i < sgx_nr_epc_sections; i++) {
		sgx_sanitize_section(&sgx_epc_sections[i]);

		/* Should never happen. */
		if (!list_empty(&sgx_epc_sections[i].unsanitized_page_list))
			WARN(1, "EPC section %d has unsanitized pages.\n", i);
	}

	while (!kthread_should_stop()) {
		if (try_to_freeze())
			continue;

		wait_event_freezable(ksgxswapd_waitq,
				     kthread_should_stop() ||
				     sgx_should_reclaim(SGX_NR_HIGH_PAGES));

		if (sgx_should_reclaim(SGX_NR_HIGH_PAGES))
			sgx_reclaim_epc_pages(SGX_NR_TO_SCAN, false, NULL);
	}

	return 0;
}

static bool __init sgx_page_reclaimer_init(void)
{
	struct task_struct *tsk;

	tsk = kthread_run(ksgxswapd, NULL, "ksgxswapd");
	if (IS_ERR(tsk))
		return false;

	ksgxswapd_tsk = tsk;

	sgx_lru_init(&sgx_global_lru);

	return true;
}

static struct sgx_epc_page *__sgx_alloc_epc_page_from_section(struct sgx_epc_section *section)
{
	struct sgx_epc_page *page;

	if (list_empty(&section->page_list))
		return NULL;

	page = list_first_entry(&section->page_list, struct sgx_epc_page, list);
	list_del_init(&page->list);
	section->free_cnt--;

	return page;
}

/**
 * __sgx_alloc_epc_page() - Allocate an EPC page
 *
 * Iterate through EPC sections and borrow a free EPC page to the caller. When a
 * page is no longer needed it must be released with sgx_free_epc_page().
 *
 * Return:
 *   an EPC page,
 *   -errno on error
 */
struct sgx_epc_page *__sgx_alloc_epc_page(void)
{
	struct sgx_epc_section *section;
	struct sgx_epc_page *page;
	int i;

	for (i = 0; i < sgx_nr_epc_sections; i++) {
		section = &sgx_epc_sections[i];
		spin_lock(&section->lock);
		page = __sgx_alloc_epc_page_from_section(section);
		spin_unlock(&section->lock);

		if (page)
			return page;
	}

	return ERR_PTR(-ENOMEM);
}

/**
 * sgx_alloc_epc_page() - Allocate an EPC page
 * @owner:	the owner of the EPC page
 * @reclaim:	reclaim pages if necessary
 *
 * Iterate through EPC sections and borrow a free EPC page to the caller. When a
 * page is no longer needed it must be released with sgx_free_epc_page(). If
 * @reclaim is set to true, directly reclaim pages when we are out of pages. No
 * mm's can be locked when @reclaim is set to true.
 *
 * Finally, wake up ksgxswapd when the number of pages goes below the watermark
 * before returning back to the caller.
 *
 * Return:
 *   an EPC page,
 *   -errno on error
 */
struct sgx_epc_page *sgx_alloc_epc_page(void *owner, bool reclaim)
{
	struct sgx_epc_page *entry;
#ifdef CONFIG_CGROUP_SGX_EPC
	struct sgx_epc_cgroup *epc_cg;

	epc_cg = sgx_epc_cgroup_try_charge(current->mm, reclaim);
	if (IS_ERR(epc_cg))
		return ERR_CAST(epc_cg);
#endif

	for ( ; ; ) {
		entry = __sgx_alloc_epc_page();
		if (!IS_ERR(entry)) {
			entry->owner = owner;
			break;
		}

		if (!sgx_can_reclaim()) {
			entry = ERR_PTR(-ENOMEM);
			break;
		}

		if (!reclaim) {
			entry = ERR_PTR(-EBUSY);
			break;
		}

		if (signal_pending(current)) {
			entry = ERR_PTR(-ERESTARTSYS);
			break;
		}

		sgx_reclaim_epc_pages(SGX_NR_TO_SCAN, false, NULL);
	}

#ifdef CONFIG_CGROUP_SGX_EPC
	if (!IS_ERR(entry)) {
		WARN_ON(entry->epc_cg);
		entry->epc_cg = epc_cg;
	} else {
		sgx_epc_cgroup_uncharge(epc_cg, false);
	}
#endif

	if (sgx_should_reclaim(SGX_NR_LOW_PAGES))
		wake_up(&ksgxswapd_waitq);

	return entry;
}

/**
 * __sgx_free_epc_page() - Free an EPC page
 * @page:	pointer to a previously allocated EPC page
 *
 * Insert an EPC page back to the list of free pages.
 */
void __sgx_free_epc_page(struct sgx_epc_page *page)
{
	struct sgx_epc_section *section = sgx_get_epc_section(page);

#ifdef CONFIG_CGROUP_SGX_EPC
	if (page->epc_cg) {
		sgx_epc_cgroup_uncharge(page->epc_cg, false);
		page->epc_cg = NULL;
	}
#endif

	spin_lock(&section->lock);
	list_add_tail(&page->list, &section->page_list);
	section->free_cnt++;
	spin_unlock(&section->lock);
}

/**
 * sgx_free_epc_page() - Free an EPC page
 * @page:	pointer to a previously allocated EPC page
 *
 * Call EREMOVE for an EPC page and insert it back to the list of free pages.
 */
void sgx_free_epc_page(struct sgx_epc_page *page)
{
	unsigned long flags = page->desc & SGX_EPC_PAGE_RECLAIM_FLAGS;
	int ret;

	/*
	 * Don't take sgx_active_page_list_lock when asserting the page isn't
	 * tagged with reclaim flags, missing a WARN in the very rare case is
	 * preferable to unnecessarily taking a global lock in the common case.
	 */
	WARN_ONCE(flags, "sgx: reclaim flags set during free: %lx", flags);

	ret = __eremove(sgx_get_epc_addr(page));
	if (WARN_ONCE(ret, "EREMOVE returned %d (0x%x)", ret, ret))
		return;

	__sgx_free_epc_page(page);
}

static void __init sgx_free_epc_section(struct sgx_epc_section *section)
{
	struct sgx_epc_page *page;

	while (!list_empty(&section->page_list)) {
		page = list_first_entry(&section->page_list,
					struct sgx_epc_page, list);
		list_del(&page->list);
		kfree(page);
	}

	while (!list_empty(&section->unsanitized_page_list)) {
		page = list_first_entry(&section->unsanitized_page_list,
					struct sgx_epc_page, list);
		list_del(&page->list);
		kfree(page);
	}

	memunmap(section->va);
}

static bool __init sgx_setup_epc_section(u64 addr, u64 size,
					 unsigned long index,
					 struct sgx_epc_section *section)
{
	unsigned long nr_pages = size >> PAGE_SHIFT;
	struct sgx_epc_page *page;
	unsigned long i;

	section->va = memremap(addr, size, MEMREMAP_WB);
	if (!section->va)
		return false;

	section->pa = addr;
	spin_lock_init(&section->lock);
	INIT_LIST_HEAD(&section->page_list);
	INIT_LIST_HEAD(&section->unsanitized_page_list);

	for (i = 0; i < nr_pages; i++) {
		page = kzalloc(sizeof(*page), GFP_KERNEL);
		if (!page)
			goto err_out;

		page->desc = (addr + (i << PAGE_SHIFT)) | index;
		list_add_tail(&page->list, &section->unsanitized_page_list);
	}

	section->free_cnt = nr_pages;
	return true;

err_out:
	sgx_free_epc_section(section);
	return false;
}

static void __init sgx_page_cache_teardown(void)
{
	int i;

	for (i = 0; i < sgx_nr_epc_sections; i++)
		sgx_free_epc_section(&sgx_epc_sections[i]);
}

/**
 * A section metric is concatenated in a way that @low bits 12-31 define the
 * bits 12-31 of the metric and @high bits 0-19 define the bits 32-51 of the
 * metric.
 */
static inline u64 __init sgx_calc_section_metric(u64 low, u64 high)
{
	return (low & GENMASK_ULL(31, 12)) +
	       ((high & GENMASK_ULL(19, 0)) << 32);
}

static bool __init sgx_page_cache_init(void)
{
	u32 eax, ebx, ecx, edx, type;
	u64 pa, size;
	int i;

	for (i = 0; i < ARRAY_SIZE(sgx_epc_sections); i++) {
		cpuid_count(SGX_CPUID, i + SGX_CPUID_FIRST_VARIABLE_SUB_LEAF,
			    &eax, &ebx, &ecx, &edx);

		type = eax & SGX_CPUID_SUB_LEAF_TYPE_MASK;
		if (type == SGX_CPUID_SUB_LEAF_INVALID)
			break;

		if (type != SGX_CPUID_SUB_LEAF_EPC_SECTION) {
			pr_err_once("Unknown EPC section type: %u\n", type);
			break;
		}

		pa = sgx_calc_section_metric(eax, ebx);
		size = sgx_calc_section_metric(ecx, edx);

		pr_info("EPC section 0x%llx-0x%llx\n", pa, pa + size - 1);

		if (!sgx_setup_epc_section(pa, size, i, &sgx_epc_sections[i])) {
			pr_err("No free memory for an EPC section\n");
			break;
		}

		sgx_nr_epc_sections++;
	}

	if (!sgx_nr_epc_sections) {
		pr_err("There are zero EPC sections.\n");
		return false;
	}

	return true;
}

const struct file_operations sgx_provision_fops = {
	.owner			= THIS_MODULE,
};

static struct miscdevice sgx_dev_provision = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = "provision",
	.nodename = "sgx/provision",
	.fops = &sgx_provision_fops,
};

int sgx_set_attribute(u64 *allowed_attributes, unsigned int attribute_fd)
{
	struct file *attribute_file;

	attribute_file = fget(attribute_fd);
	if (!attribute_file)
		return -EINVAL;

	if (attribute_file->f_op != &sgx_provision_fops) {
		fput(attribute_file);
		return -EINVAL;
	}
	fput(attribute_file);

	*allowed_attributes |= SGX_ATTR_PROVISIONKEY;
	return 0;
}
EXPORT_SYMBOL_GPL(sgx_set_attribute);

static void __init sgx_init(void)
{
	int ret;

	if (!boot_cpu_has(X86_FEATURE_SGX))
		return;

	if (!sgx_page_cache_init())
		return;

	if (!sgx_page_reclaimer_init())
		goto err_page_cache;

	ret = misc_register(&sgx_dev_provision);
	if (ret) {
		pr_err("Creating /dev/sgx/provision failed with %d.\n", ret);
		goto err_kthread;
	}

	/* Success if the native *or* virtual driver initialized cleanly. */
	ret = sgx_drv_init();
	ret = sgx_virt_epc_init() ? ret : 0;
	if (ret)
		goto err_provision;

	return;

err_provision:
	misc_deregister(&sgx_dev_provision);

err_kthread:
	kthread_stop(ksgxswapd_tsk);

err_page_cache:
	sgx_page_cache_teardown();
}

device_initcall(sgx_init);
