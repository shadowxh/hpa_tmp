#ifndef _LINUX_HPA_H
#define _LINUX_HPA_H

#include <linux/types.h>
#include <linux/list.h>
#include <linux/spinlock.h>
#include <linux/mm.h>
#include <linux/topology.h>
#include <linux/wait.h>
#include <linux/uaccess.h>
#include <linux/mmzone.h>
#include <linux/kernel.h>
#include <linux/sched.h>

struct hugepage
{
    /* First double word block */
    unsigned long flags;
    struct address_space *mapping;
    /* Seconf double world block*/ 
    pgoff_t index;
    struct {
        atomic_t _mapcount;
        atomic_t _refcount;
    };
    /* Third double word block */
    struct list_head lru;
    /* Remainder is not double word aligned */
    unsigned long private;
#if defined(WANT_PAGE_VIRTUAL)
    void *virtual;
#endif /* WANT_PAGE_VIRTUAL */
//#ifdef CONFIG_WANT_PAGE_DEBUG_FLAGS
	unsigned long pfn_offset;
//#endif
};


struct hpa_node
{
    /* below shouln't change after init */
    
    /* for struct hpa_section */
    /*
    struct list_head section_list; 
    */
    
    unsigned long node_start_pfn;
    unsigned long node_present_pages;
    unsigned long node_spanned_pages;
    unsigned long node_max_sections;

    int node_id;

    unsigned long next_nr_section;
    
    wait_queue_head_t waitq;
    

    int nid;
    int all_unreclaimable;
    spinlock_t lru_lock;
    unsigned long pages_scanned;
    struct lruvec lruvec;
    atomic_long_t vm_stat[NR_VM_ZONE_STAT_ITEMS];
    unsigned long  watermark;
    struct task_struct *hp_kswapd;    

};

struct scan_control {
	/* Incremented by the number of inactive pages that were scanned */
	unsigned long nr_scanned;

	/* Number of pages freed so far during a call to shrink_zones() */
	unsigned long nr_reclaimed;

	/* How many pages shrink_list() should reclaim */
	unsigned long nr_to_reclaim;

	unsigned long hibernation_mode;

	/* This context's GFP mask */
	gfp_t gfp_mask;

	int may_writepage;
	int may_unmap;
	int may_swap;
	int order;
	int priority;
	struct mem_cgroup *target_mem_cgroup;
	nodemask_t      *nodemask;
};
 
enum page_references{
	PAGEREF_RECLAIM,
	PAGEREF_RECLAIM_CLEAN,
	PAGEREF_KEEP,
	PAGEREF_ACTIVATE,
};

struct hpa_section
{
    struct list_head free_list;
    struct list_head section_node;
};

extern struct hugepage *huge_mem_map;
extern struct hpa_node *hpa_node_data[MAX_NUMNODES];
extern struct hpa_section *hpa_section_array[MAX_NUMNODES];
extern unsigned long total_page;
extern unsigned long free_page;
extern unsigned long hpa_start_pfn;
extern unsigned long hpa_nr_pages;
extern unsigned long hpa_end_pfn;
extern unsigned long hpnode_mask;

/*TODO should be get dynamically*/
#define HPNODE_MASK  hpnode_mask
//#define HUGEPAGE_SHIFT      21
#define HUGEPAGE_SIZE       (1 << 21)
#define HPA_NODE_DATA(nid)  (hpa_node_data[(nid)])

#define for_each_huge_node(node,mask) for_each_node_mask(node, node_possible_map) if((1UL<<node)& HPNODE_MASK)
/* We need to alloc one page to hold section, number of seciton
 * should be PAGE_SIZE / sizeof( struct hpa_section )
 * */
#define MAX_NUMSECTION  (PAGE_SIZE/ sizeof(struct hpa_section))
#define hpa_pfn_to_page(pfn)    (huge_mem_map + ((pfn - hpa_start_pfn) >> 9))
#define hpa_page_to_pfn(page)   (hpa_start_pfn + ((page - huge_mem_map) << 9))

#define SECTION_SHIFT   11
#define SECTION_SIZE    (1 << SECTION_SHIFT)
#define HPA_PFN_PHYS(x)    ((phys_addr_t)(x) << 12)

bool is_hpa_pfn(unsigned long pfn);
bool is_hpa_page(struct page* page);
int hpa_init(void);
void hpa_free_page(struct hugepage *page);
void hpa_free_page_list(struct list_head *list);
void __hpa_free_page(struct hugepage *page);
struct hugepage *hpa_alloc_page_node(int nid);
struct hugepage *hpa_alloc_page(void);
int hpa_set_page_dirty(struct hugepage *page);
void hpa_put_page(struct hugepage *page);
void hpa_node_start_end_init(int nid, u64 start, u64 end);
void hpa_start_nr_set(u64 start_at, u64 mem_size);

static inline void hpa_set_page_node(struct hugepage *page,unsigned long node)
{
    page->flags &= ~(NODES_MASK << NODES_PGSHIFT);
    page->flags |= (node & NODES_MASK) << NODES_PGSHIFT;
}

static inline void hpa_set_page_section(struct hugepage *page,unsigned long section)
{
    page->flags &= ~(SECTIONS_MASK << SECTIONS_PGSHIFT);
    page->flags |= (section & SECTIONS_MASK) << SECTIONS_PGSHIFT; 
}
static inline int hpa_page_to_nid(const struct hugepage *page)
{
    return (page->flags >> NODES_PGSHIFT) & NODES_MASK;
}

static inline struct hpa_section *hpa_page_section(const struct hugepage *page)
{
    unsigned long section = (page->flags >> SECTIONS_PGSHIFT) & SECTIONS_MASK;
    return hpa_section_array[hpa_page_to_nid(page)] + section;
}

static inline void *hpa_page_address(const struct hugepage *page)
{
    return __va(HPA_PFN_PHYS(hpa_page_to_pfn(page)));
}

static inline void *hpa_kmap_atomic(struct hugepage *page)
{
    pagefault_disable();
    return hpa_page_address(page);
}

static inline void hpa_kunmap_atomic(void *addr)
{
    pagefault_enable();
}


void __hpa_lock_page(struct hugepage* page);
void hpa_unlock_page(struct hugepage* page);
static inline int hpa_trylock_page(struct hugepage *page)
{
	return (likely(!test_and_set_bit_lock(PG_locked, &page->flags)));
}

static inline void hpa_lock_page(struct hugepage *page)
{	
	might_sleep();
	if(!hpa_trylock_page(page))
		__hpa_lock_page(page);
}


struct hugepage *hpa_find_lock_page(struct address_space *mapping, pgoff_t offset);


int hpa_add_to_page_cache(struct hugepage *page, struct address_space *mapping, pgoff_t idx);

void __hpa_delete_from_page_cache(struct hugepage *page);


void hpa_delete_from_page_cache(struct hugepage *page);


void hpa_clear_huge_page(struct hugepage *page, unsigned long address);

static inline struct hpa_node *lruvec_node(struct lruvec *lruvec){
    return container_of(lruvec, struct hpa_node, lruvec);	/* container_of is in "include/linux/kernel.h" */
}

static inline void node_page_state_add(long x, struct hpa_node *node,enum zone_stat_item item)
{
    atomic_long_add(x, &node->vm_stat[item]);
}

void add_hpage_to_lruvec(struct hugepage *hpage,enum lru_list lru);

void hp_del_page_from_lru_list(struct hugepage *hpage,
                                struct lruvec *lruvec, enum lru_list lru);
void hp_add_page_to_lru_list(struct hugepage *hpage,struct lruvec *lruvec, enum lru_list lru);
#endif /*_LINUX_HPA_H */
