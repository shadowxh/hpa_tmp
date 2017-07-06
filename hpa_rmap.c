
#include <linux/hpa_rmap.h>

int hpa_page_mapcount(struct hugepage* page)
{
    return page_mapcount((struct page*)page);
}
EXPORT_SYMBOL(hpa_page_mapcount);

int hpa_page_mapcount_is_zero(struct hugepage* page)
{
    return !hpa_page_mapcount(page);
}
EXPORT_SYMBOL(hpa_page_mapcount_is_zero);

unsigned long hpa_pte_to_pfn(pte_t pte)
{
    return (pte_val(pte) & HPA_PTE_PFN_MASK) >> 12;
}
EXPORT_SYMBOL(hpa_pte_to_pfn);

static pte_t *__hpa_page_check_address(struct hugepage *page, struct mm_struct *mm,
                                       unsigned long address, spinlock_t **ptlp, int sync)
{
    pgd_t *pgd;
    pud_t *pud;
    pte_t *pte = NULL;
    spinlock_t *ptl;

    pgd = pgd_offset(mm, address);

    if(pgd_present(*pgd))
    {
        pud = pud_offset(pgd, address);
        if(pud_present(*pud))
        {
            pte = (pte_t *) pmd_offset(pud, address);
        }
    }

    if(!pte) return NULL;

    ptl = &mm->page_table_lock;

    spin_lock(ptl);

    if(pte_present(*pte) && hpa_page_to_pfn(page) == hpa_pte_to_pfn(*pte))
    {
        *ptlp = ptl;
        return pte;
    }

    pte_unmap_unlock(pte, ptl);
    return NULL;
}

pte_t *hpa_page_check_address(struct hugepage *page, struct mm_struct *mm,
                                            unsigned long address,
                                            spinlock_t **ptlp, int sync)
{
    pte_t *ptep;
    __cond_lock(*ptlp, ptep = __hpa_page_check_address(page, mm, address, ptlp, sync));
    return ptep;
}
EXPORT_SYMBOL(hpa_page_check_address);

static int hpa_try_to_unmap_one(struct hugepage* page, struct vm_area_struct *vma,
                                unsigned long address, enum ttu_flags flags)
{
    struct mm_struct *mm = vma->vm_mm;
    pte_t *pte;
    pte_t pteval;
    spinlock_t *ptl;
    int ret = SWAP_AGAIN;

    if((flags & TTU_MUNLOCK) && !(vma->vm_flags & VM_LOCKED))
        goto out;

    //检查page有没有映射到此mm地址空间中
    //对ptl上锁
    pte = hpa_page_check_address(page, mm, address, &ptl, 0);

    if(!pte)
       goto out;

#ifdef NEED_VM_LOCKED
     //flags没有要求忽略mlock 的vam
    if(!(flags & TTU_IGNORE_MLOCK))
    {
        //此vma要求里面的页都锁在内存中
        if(vma->vm_flags & VM_LOCKED)
            goto out_mlock;

        //flags标记了对vma进行mlock释放模式
        if(flags & TTU_MUNLOCK)
            goto out_unmap;
    }
#endif
    //忽略页表项中的accessed标记
    if(!(flags & TTU_IGNORE_ACCESS))
    {
        //清除页表项的accessed标志
        if(ptep_clear_flush_young_notify(vma, address, pte))
        {
            ret = SWAP_FAIL;
            goto out_unmap;
        }
    }

     flush_cache_page(vma, address, hpa_page_to_pfn(page));

    //获取pte中的内容，并对pte清空
     pteval = ptep_clear_flush(vma, address, pte);

    //如果pte标记了此页为脏页，则设置page的PG_dirty标志位
     if(pte_dirty(pteval))
        set_page_dirty((struct page*)page);

     //更新进程所拥有的最大页框数
     update_hiwater_rss(mm);

     //对mm的文件页计数--
     //dec_mm_counter(mm,MM_FILEPAGES);
     //对此页的_mapcount--
     hpa_page_remove_rmap(page);

    //对此页的引用_count--
     hpa_put_page(page);

out_unmap:
     pte_unmap_unlock(pte, ptl);
     if(ret != SWAP_FAIL)
        mmu_notifier_invalidate_page(mm, address);
out:
    return ret;

#ifdef NEED_VM_LOCKED
out_mlock:
     pte_unmap_unlock(pte,ptl);

     if(down_read_trylock(&vma->vm_mm->mmap_sem))
     {
         if(vma->vm_flags & VM_LOCKED)
         {
             mlock_vma_page((struct page*)page);
             ret = SWAP_MLOCK;
         }
         up_read(&vma->vm_mm->mmap_sem);
     }
     return ret;
#endif
}

struct address_space *hpa_page_mapping(struct hugepage *page)
{
    return page_mapping((struct page*) page);
}
EXPORT_SYMBOL(hpa_page_mapping);

unsigned long hpa_vma_address(struct hugepage* page, struct vm_area_struct *vma)
{
    unsigned long address = vma->vm_start + (((page->index << 9)- vma->vm_pgoff)<<12);
    VM_BUG_ON( address < vma->vm_start || address >= vma->vm_end);
    return address;
}
EXPORT_SYMBOL(hpa_vma_address);

static int hpa_try_to_unmap_file(struct hugepage* page, enum ttu_flags flags)
{
    struct address_space *mapping = hpa_page_mapping(page);
    pgoff_t  pgoff = page->index;
    struct vm_area_struct *vma;
    int ret = SWAP_AGAIN;

    VM_BUG_ON(!PageLocked((struct page*)page));

    if(!mapping)
         return ret;

    mutex_lock(&mapping->i_mmap_mutex);

    vma_interval_tree_foreach(vma, &mapping->i_mmap, pgoff, pgoff)
    {
        unsigned long address = hpa_vma_address(page, vma);

        cond_resched();

        ret = hpa_try_to_unmap_one(page, vma, address, flags);

        if(ret != SWAP_AGAIN || hpa_page_mapcount_is_zero(page))
            goto done;
    }
    //关于非线性映射的部分暂时没做，后期如果有这种场景的话，就可以加入
    // if(list_empty(&mapping->i_mmap_nonlinear))
    //     goto done;

done:
    mutex_unlock(&mapping->i_mmap_mutex);
    return ret;
}

int hpa_try_to_unmap(struct hugepage* page, enum ttu_flags flags){

     int ret;

     ret = hpa_try_to_unmap_file(page, flags);

     if(ret != SWAP_MLOCK && !hpa_page_mapcount(page))
            ret = SWAP_SUCCESS;

     return ret;
}
EXPORT_SYMBOL(hpa_try_to_unmap);


static inline int hpa_page_mapped(struct hugepage *page)
{
        return atomic_read(&(page)->_mapcount) >= 0;
}


static inline void *hpa_page_rmapping(struct hugepage *page)
{
        return (void *)((unsigned long)page->mapping & ~PAGE_MAPPING_FLAGS);
}


static int hpa_page_referenced_one(struct hugepage *page, struct vm_area_struct *vma,
                        unsigned long address, unsigned int *mapcount,
                        unsigned long *vm_flags)
{
        struct mm_struct *mm = vma->vm_mm;
        int referenced = 0;
        pte_t *pte;
        spinlock_t *ptl;

        pte = hpa_page_check_address(page, mm, address, &ptl, 0);

        if (!pte)
            goto out;

        if (vma->vm_flags & VM_LOCKED) {
        	pte_unmap_unlock(pte, ptl);
            *mapcount = 0;  /* break early from loop */
            *vm_flags |= VM_LOCKED;
            goto out;
        }

        if (ptep_clear_flush_young_notify(vma, address, pte)) {
            if (likely(!VM_SequentialReadHint(vma)))
                referenced++;
        }

        pte_unmap_unlock(pte, ptl);

        (*mapcount)--;

        if (referenced)
            *vm_flags |= vma->vm_flags;
out:
        return referenced;
}


static int hpa_page_referenced_file(struct hugepage *page,
                                struct mem_cgroup *memcg,
                                unsigned long *vm_flags)
{
        unsigned int mapcount;
        struct address_space *mapping = page->mapping;
        pgoff_t pgoff = page->index;
        struct vm_area_struct *vma;
        int referenced=0;

        BUG_ON(!PageLocked((struct page*)page));

        mutex_lock(&mapping->i_mmap_mutex);

        mapcount = hpa_page_mapcount(page);

        vma_interval_tree_foreach(vma, &mapping->i_mmap, pgoff, pgoff) {
            unsigned long address = hpa_vma_address(page, vma);

//            if (memcg && !mm_match_cgroup(vma->vm_mm, memcg))
//                continue;

            referenced += hpa_page_referenced_one(page, vma, address, &mapcount, vm_flags);

            if (!mapcount) break;
        }

        mutex_unlock(&mapping->i_mmap_mutex);
        return referenced;
}


int hpa_page_referenced(struct hugepage *page,
                    int is_locked,
                    struct mem_cgroup *memcg,
                    unsigned long *vm_flags)
{
        int referenced = 0;
        int we_locked = 0;

        *vm_flags = 0;
        if (hpa_page_mapped(page) && hpa_page_rmapping(page)) {
            if (!is_locked) {
                we_locked = hpa_trylock_page(page);
                if (!we_locked) {
                    referenced++;
                    goto out;
                }
            }
            if (page->mapping)
                referenced += hpa_page_referenced_file(page, memcg, vm_flags);

            if (we_locked)
                hpa_unlock_page(page);

            if (page_test_and_clear_young(hpa_page_to_pfn(page)))
                referenced++;
        }
out:
        return referenced;
}
EXPORT_SYMBOL(hpa_page_referenced);


void hpa_page_remove_rmap(struct hugepage *page)
{
        //bool locked;
        //unsigned long flags;

        //mem_cgroup_begin_update_page_stat(page, &locked, &flags);
	if (!atomic_add_negative(-1, &page->_mapcount))
                goto out;
        //__dec_zone_page_state(page, NR_FILE_MAPPED);
        //mem_cgroup_dec_page_stat(page, MEMCG_NR_FILE_MAPPED);
        //mem_cgroup_end_update_page_stat(page, &locked, &flags);
//	if (unlikely(PageMlocked((struct page*)page)))
//                clear_page_mlock((struct page*)page);
//        return;
out:
        //mem_cgroup_end_update_page_stat(page, &locked, &flags);
        return;
}
EXPORT_SYMBOL(hpa_page_remove_rmap);
