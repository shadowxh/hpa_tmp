#ifndef _HPA_RMAP_H_
#define _HPA_RMAP_H_

#include <linux/list.h>
#include <linux/slab.h>
#include <linux/rwsem.h>
#include <linux/memcontrol.h>
#include <linux/mm_types.h>
#include <linux/rmap.h>
#include <linux/hpa.h>
#include <linux/mmu_notifier.h>
#include <linux/pagemap.h>
#include <linux/mm.h>
#include <linux/init.h>

#define HPA_PTE_PFN_MASK   PTE_PFN_MASK
//如果需要考虑 VM_LOCKED就把注释去掉
//#define NEED_VM_LOCKED

int hpa_page_mapcount(struct hugepage* page);

int hpa_page_mapcount_is_zero(struct hugepage* page);

unsigned long hpa_pte_to_pfn(pte_t pte);

pte_t *hpa_page_check_address(struct hugepage *page, struct mm_struct *mm,
                              unsigned long address, spinlock_t **ptlp, int sync);

struct address_space *hpa_page_mapping(struct hugepage *page);

unsigned long hpa_vma_address(struct hugepage* page, struct vm_area_struct *vma);


int hpa_try_to_unmap(struct hugepage* page, enum ttu_flags flags);

int hpa_page_referenced(struct hugepage *page, int is_locked, struct mem_cgroup *memcg, unsigned long *vm_flags);

void hpa_page_remove_rmap(struct hugepage* page);


#endif

