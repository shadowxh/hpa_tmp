#include <linux/uaccess.h>
#include <linux/kernel_stat.h>
#include <linux/wait.h>
#include <linux/pagemap.h>
#include <linux/hpa.h>
#include <linux/hugetlb.h>


static void __hpa_put_page(struct hugepage *page)
{
    __hpa_free_page(page);
}


void hpa_put_page(struct hugepage *page)
{

    if (put_page_testzero((struct page*)page))
	{
        __hpa_put_page(page);
	}
}
EXPORT_SYMBOL(hpa_put_page);
static int sleep_on_page(void *word)
{
    io_schedule();
    return 0;
}

static wait_queue_head_t *hpa_node_waitqueue(struct hugepage *page)
{
        struct hpa_node *node = HPA_NODE_DATA(hpa_page_to_nid(page));

        return &node->waitq;
}

static inline void hpa_wake_up_page(struct hugepage *page, int bit)
{
        __wake_up_bit(hpa_node_waitqueue(page), &page->flags, bit);
}

void hpa_unlock_page(struct hugepage *page)
{
	//VM_BUG_ON(!PageLocked(hpa));
	clear_bit_unlock(PG_locked, &page->flags);
	smp_mb__after_clear_bit();
	hpa_wake_up_page(page, PG_locked);
}
EXPORT_SYMBOL(hpa_unlock_page);

void __hpa_lock_page(struct hugepage *page)
{
	DEFINE_WAIT_BIT(wait, &page->flags, PG_locked);

	__wait_on_bit_lock(hpa_node_waitqueue(page), &wait, sleep_on_page,
							TASK_UNINTERRUPTIBLE);
}
EXPORT_SYMBOL(__hpa_lock_page);

struct hugepage *hpa_find_lock_page(struct address_space *mapping, pgoff_t offset)
{
    struct hugepage *page;
repeat:
    page = (struct hugepage*)find_get_page(mapping, offset);
    if (page && !radix_tree_exception(page)) {
        hpa_lock_page(page);

        if (unlikely(page->mapping != mapping)) {
            hpa_unlock_page(page);
            hpa_put_page(page);
            goto repeat;
        }
        VM_BUG_ON(page->index != offset);
    }
    return page;
}


static int hpa_add_page_cache_locked(struct hugepage *page,
        struct address_space *mapping, pgoff_t offset, gfp_t gfp_mask)
{
    int error;

    error = radix_tree_preload(gfp_mask & ~__GFP_HIGHMEM);
    if (error)
        return error;

    get_page((struct page*)page);
    page->mapping = mapping;
    page->index = offset;

    spin_lock_irq(&mapping->tree_lock);

    error = radix_tree_insert(&mapping->page_tree, offset, page);
    radix_tree_preload_end();

    if (unlikely(error))
        goto err_insert;

    mapping->nrpages++;

    spin_unlock_irq(&mapping->tree_lock);

    return 0;

err_insert:
    page->mapping = NULL;
    spin_unlock_irq(&mapping->tree_lock);
    hpa_put_page(page);
    return error;
}


static inline int __hpa_to_page_cache(struct hugepage *page, struct address_space *mapping,
        pgoff_t offset, gfp_t gfp_mask)
{
    int error;

    __set_page_locked((struct page*)page);

    error = hpa_add_page_cache_locked(page, mapping, offset, gfp_mask);

    if (unlikely(error))
        __clear_page_locked((struct page*)page);

    return error;
}


int hpa_add_to_page_cache(struct hugepage *page, struct address_space *mapping,
        pgoff_t idx)
{
    struct inode *inode = mapping->host;
    struct hstate *h = hstate_inode(inode);
    int err = __hpa_to_page_cache(page, mapping, idx, GFP_KERNEL);
    if (err)
        return err;
    ClearPagePrivate((struct page*)page);
    spin_lock(&inode->i_lock);
    inode->i_blocks += blocks_per_huge_page(h);
    spin_unlock(&inode->i_lock);
    return 0;
}

void __hpa_delete_from_page_cache(struct hugepage *page)
{
    struct address_space *mapping = page->mapping;

    radix_tree_delete(&mapping->page_tree, page->index);
    page->mapping = NULL;

    mapping->nrpages--;

    BUG_ON(page_mapped((struct page*)page));
}
EXPORT_SYMBOL(__hpa_delete_from_page_cache);

void hpa_delete_from_page_cache(struct hugepage *page)
{
    struct address_space *mapping = page->mapping;
    void (*freepage)(struct page *);

    freepage = mapping->a_ops->freepage;
    spin_lock_irq(&mapping->tree_lock);
    __hpa_delete_from_page_cache(page);
    spin_unlock_irq(&mapping->tree_lock);


    if (freepage)
        freepage((struct page*)page);

    hpa_put_page(page);
 //   page_cache_release((struct page*)page);
}
EXPORT_SYMBOL(hpa_delete_from_page_cache);

void hpa_clear_huge_page(struct hugepage *page,
		     unsigned long address)
{
    void *addr;
	might_sleep();
	cond_resched();
	addr = hpa_kmap_atomic(page);
    memset(addr,0,HUGEPAGE_SIZE);
    hpa_kunmap_atomic(addr);
}

void add_hpage_to_lruvec(struct hugepage *hpage,enum lru_list lru)
{
    int nid, active;
    struct hpa_node * node;
    unsigned long flags;
    struct lruvec * lruvec;
    
    preempt_disable();
    
    nid = hpa_page_to_nid(hpage);
    node = hpa_node_data[nid];
    flags = 0;
    lruvec = &node->lruvec;
    
    spin_lock_irqsave(&node->lru_lock,flags);
    
    active = (lru == LRU_ACTIVE_FILE ? 1 : 0);
    SetPageLRU((struct page*)hpage);
    if(active) SetPageActive((struct page*)hpage);
    
    list_add(&hpage->lru,&lruvec->lists[lru]);
    node_page_state_add(-1,lruvec_node(lruvec),NR_FREE_PAGES);//to tell xi ge to add this in his free_page
    node_page_state_add(1,lruvec_node(lruvec),NR_LRU_BASE+lru);
    
    spin_unlock_irqrestore(&node->lru_lock, flags);
    
    preempt_enable();
}
EXPORT_SYMBOL(add_hpage_to_lruvec);
