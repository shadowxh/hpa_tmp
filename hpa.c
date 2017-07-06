/*
 * Yet another hugepage allocator
 */

#include <linux/hpa.h>
#include <linux/mm.h>
#include <linux/atomic.h>
#include <linux/bootmem.h>
#include <linux/memblock.h>
#include "internal.h"

struct hugepage *huge_mem_map;
struct hpa_node *hpa_node_data[MAX_NUMNODES];
struct hpa_section *hpa_section_array[MAX_NUMNODES];

EXPORT_SYMBOL(huge_mem_map);
EXPORT_SYMBOL(hpa_node_data);
EXPORT_SYMBOL(hpa_section_array);
/*start pfn and number of whole hugepages area*/
unsigned long hpa_start_pfn;
unsigned long hpa_nr_pages;
unsigned long hpa_end_pfn;
unsigned long hpa_node_start[MAX_NUMNODES];
unsigned long hpa_node_end[MAX_NUMNODES];
unsigned long hpnode_mask = 0UL;
unsigned long total_page;
unsigned long free_page;
EXPORT_SYMBOL(hpa_start_pfn);
EXPORT_SYMBOL(hpa_end_pfn);
EXPORT_SYMBOL(hpa_nr_pages);
EXPORT_SYMBOL(hpnode_mask);
EXPORT_SYMBOL(total_page);
EXPORT_SYMBOL(free_page);

bool is_hpa_pfn(unsigned long pfn)
{
	return (pfn>=hpa_start_pfn && pfn<hpa_end_pfn);
}
EXPORT_SYMBOL(is_hpa_pfn);
bool is_hpa_page(struct page* page)
{
	struct hugepage* p=(struct hugepage*)page;
	return ((p>=huge_mem_map) && (p<huge_mem_map+hpa_nr_pages));
}
EXPORT_SYMBOL(is_hpa_page);
void __hpa_free_page(struct hugepage *page)
{

	unsigned long flags;
	struct hpa_section *section;
	int nid;
	struct hpa_node *node;
	local_irq_save(flags);


	/*from struct hugepage to nid and section*/
	/*TODO for SMP add spin lock*/
	section = hpa_page_section(page);

	nid = hpa_page_to_nid(page);
	node = HPA_NODE_DATA(nid);
	if (!section) {
		return;
	}
	if (PageLRU((struct page*)page)) {
		__ClearPageLRU((struct page*)page);

		list_move(&page->lru,&section->free_list);
		if (PageActive((struct page *)page)) {
			node_page_state_add(-1, node, NR_ACTIVE_FILE);
		} else {
			node_page_state_add(-1, node, NR_INACTIVE_FILE);
		}
	} else {
		list_add(&page->lru,&section->free_list);
	}
	node_page_state_add(1, node, NR_FREE_PAGES);

	local_irq_restore(flags);
	free_page++;
}
EXPORT_SYMBOL(__hpa_free_page);


void hpa_free_page(struct hugepage *page)
{
    /* TODO some free page prepare */
    /*page refcount should be 1*/
    if (put_page_testzero((struct page*)page)) {
        /* turn off irq, but page fault will still happen*/
		 __hpa_free_page(page);
    }
}
EXPORT_SYMBOL(hpa_free_page);

/*make sure page count is zero*/
void hpa_free_page_list(struct list_head *list)
{
    struct hugepage *page, *next;

    list_for_each_entry_safe(page, next, list, lru) {
        __hpa_free_page(page);
    }
}
EXPORT_SYMBOL(hpa_free_page_list);


static struct list_head *get_next_section_list(int nid)
{
    unsigned long nr_section = HPA_NODE_DATA(nid)->next_nr_section;
    unsigned long max_nr_section = HPA_NODE_DATA(nid)->node_max_sections;
    struct list_head *list = &hpa_section_array[nid][nr_section].free_list;

    if ( nr_section + 2 >  max_nr_section )
        HPA_NODE_DATA(nid)->next_nr_section = 0;
    else
        HPA_NODE_DATA(nid)->next_nr_section++;

    return list;
}

struct hugepage *hpa_alloc_page_node(int nid)
{
    unsigned long flags;
    struct hugepage *page;
    struct list_head *list;
    unsigned long max_num, pnum;

    local_irq_save(flags);

    max_num = HPA_NODE_DATA(nid)->node_max_sections; 

    for (pnum = 0; pnum < max_num; pnum++) {

        list = get_next_section_list(nid);

    
        if (list_empty(list))
            continue;
        else {
            page = list_first_entry(list, struct hugepage, lru);
            list_del(&page->lru);

            set_page_refcounted((struct page*)page);
			//add to lru[LRU_INACTIVE_FILE] list
			add_hpage_to_lruvec(page, LRU_INACTIVE_FILE);
            local_irq_restore(flags);
            free_page--;
		return page;
        }
    }

    /*failed*/
    local_irq_restore(flags);
    return NULL;
}
EXPORT_SYMBOL(hpa_alloc_page_node);

struct hugepage *hpa_alloc_page(void)
{
	struct hugepage *page;
	int nid;
	for_each_node_mask(nid, node_possible_map) { 
		if((1UL<<nid)&hpnode_mask) {		
			page=hpa_alloc_page_node(nid);
			if(page!=NULL) return page;		
		}
	}
	return NULL;
}
EXPORT_SYMBOL(hpa_alloc_page);


/* get_XXX function currently is fix-returned
 * we will calculate accordingly in the future
 * */
static unsigned long get_section_num(unsigned long nr_pages)
{
    unsigned long ret = (nr_pages + ((1 << 11) -1) )>> 11;
    //BUG_ON (ret != 1);
    return ret; 
}

static void __init hpa_alloc_section_node(int nid)
{

    unsigned long num_section;
    unsigned long pnum;
    struct hpa_section *section;
    /*allocation of section*/
    num_section = HPA_NODE_DATA(nid)->node_max_sections;
    if (num_section != 0) {
        section = alloc_bootmem(PAGE_SIZE);
        if (!section) {
            pr_err("Cannot find %zu bytes in node %d\n",
                    PAGE_SIZE, nid);
            return;
        }
        hpa_section_array[nid] = section;
        for (pnum=0; pnum < num_section; pnum++) {
            INIT_LIST_HEAD(&section[pnum].free_list);
        }
    }
    else
        hpa_section_array[nid] = 0;
}

static void hpa_init_mem_mapping(void)
{
    unsigned long start,end;

    start = hpa_start_pfn << 12;
    end = (hpa_start_pfn << 12)+ (hpa_nr_pages << 21);

    init_memory_mapping(start,end);

}

static void hpa_memmap_init(unsigned long size, int nid,
        unsigned long start_pfn, unsigned long sectionid)
{
    unsigned long end_pfn = start_pfn + (size << 9);
    unsigned long pfn;


    for (pfn = start_pfn; pfn < end_pfn; pfn+=512)
    {
        struct hugepage *page = hpa_pfn_to_page(pfn);
        hpa_set_page_node(page,nid);
        hpa_set_page_section(page,sectionid);
    }


}

/* according to function setup_node_data from arch/x86/mm/numa.c */
static void __init hpa_alloc_node_data(int nid)
{
	/*what is different between u64 and void star */
	const size_t nd_size = roundup(sizeof(struct hpa_node), PAGE_SIZE);
	//u64 nd_pa;
	void * nd;
	unsigned long size, start_pfn;
	unsigned long num_section;
	struct hpa_node *node = NULL;
	struct lruvec *lruvec;

	struct list_head *lru_active,*lru_inactive;

	nd = alloc_bootmem(nd_size);
	//if (!nd_pa) {
	//    pr_err("Cannot find %zu bytes in node %d\n",
	//            nd_size, nid);
	//    return;
	//}
	//nd = __va(nd_pa);

	BUG_ON( nd == NULL);

	hpa_node_data[nid] = nd;
	node = HPA_NODE_DATA(nid);
	lruvec = &node->lruvec;
	start_pfn = hpa_node_start[nid];
	size = (hpa_node_end[nid]-hpa_node_start[nid]) >> 9;
	/*one section is 4G, num is got by being divided by nr_pages * 2M*/
	num_section = get_section_num(size);

	/*We need a macro HPA_NODE_DATA*/
	memset(node,0,sizeof(struct hpa_node));
	node->node_id = nid;
	node->node_start_pfn = start_pfn;
	node->node_spanned_pages = size;
	node->node_present_pages = size;
	node->node_max_sections = num_section;
	node->next_nr_section = 0;
	node->nid = nid;

	lruvec->lists[LRU_INACTIVE_FILE].prev = &lruvec->lists[LRU_INACTIVE_FILE];
	lruvec->lists[LRU_INACTIVE_FILE].next = &lruvec->lists[LRU_INACTIVE_FILE];
	lruvec->lists[LRU_ACTIVE_FILE].prev = &lruvec->lists[LRU_ACTIVE_FILE];
	lruvec->lists[LRU_ACTIVE_FILE].next = &lruvec->lists[LRU_ACTIVE_FILE];

	INIT_LIST_HEAD(&(lruvec->lists[LRU_ACTIVE_FILE]));
	INIT_LIST_HEAD(&(lruvec->lists[LRU_INACTIVE_FILE]));

            lru_active=&lruvec->lists[LRU_ACTIVE_FILE];
            lru_inactive=&lruvec->lists[LRU_INACTIVE_FILE];

	node->pages_scanned = 0;
	node->watermark = 500;
	atomic_long_set(&node->vm_stat[NR_FREE_PAGES], 0);
	/*
	   INIT_LIST_HEAD(&node->section_list); 
	 */
	hpa_alloc_section_node(nid);


	return;
}

static void __init hpa_nodes_init(void)
{
    int nid;
    /*TODO instead of possible map we should setup our own map*/
    for_each_node_mask(nid, node_possible_map) {
        if((1UL<<nid)&hpnode_mask) {
	unsigned long pnum, num_section;
        /*default section is 4G which is 1^11 hugepages*/
        unsigned long start_pfn = hpa_node_start[nid], size = 1 << 20;
        hpa_alloc_node_data(nid);
        num_section = HPA_NODE_DATA(nid)->node_max_sections;
	for( pnum = 0; pnum < num_section; pnum++, start_pfn+=size) {
            if ( pnum == num_section - 1 ) 
                size = (hpa_node_end[nid]-start_pfn) >> 9;
            hpa_memmap_init(size,nid,start_pfn,pnum);
        }
        init_waitqueue_head(&HPA_NODE_DATA(nid)->waitq);
	}
    }
    hpa_init_mem_mapping();
    return;
}

static unsigned long hpa_free_all_boot_hugepages(void)
{
    unsigned long start_pfn = hpa_start_pfn;
    unsigned long end_pfn = start_pfn + (hpa_nr_pages << 9);
    unsigned long pfn;
    struct hugepage * page;
    for (pfn = start_pfn; pfn < end_pfn; pfn+=512) {
        page = hpa_pfn_to_page(pfn);
        atomic_set(&page->_mapcount, -1);
        set_page_refcounted((struct page*)page);
        hpa_free_page(page);
    }
    return 0;
}

void hpa_node_start_end_init(int nid, u64 start, u64 end)
{
	unsigned long addr1 = start >> 12, addr2 = end >> 12;
	hpa_node_start[nid]=hpa_start_pfn>addr1?hpa_start_pfn:addr1;
	hpa_node_end[nid]=hpa_start_pfn+(hpa_nr_pages<<9)<addr2?hpa_start_pfn+(hpa_nr_pages<<9):addr2;
	if(hpa_node_start[nid]<hpa_node_end[nid]) {
		hpnode_mask|=1UL<<nid;
	}
}

void hpa_start_nr_set(u64 start_at, u64 mem_size)
{
	hpa_start_pfn = start_at >> 12;
	hpa_nr_pages = mem_size >> 21;
	hpa_end_pfn = hpa_start_pfn + (hpa_nr_pages << 9);
	total_page += hpa_nr_pages;
}

int __init hpa_init(void)
{
	int ret = 0;
	unsigned long size;
	total_page = 0;
	free_page = 0;
	/*start_pfn and size should be get dynamically */
	size = PAGE_ALIGN(sizeof(struct hugepage)* hpa_nr_pages);

	huge_mem_map = alloc_bootmem(size);

	hpa_nodes_init();

	hpa_free_all_boot_hugepages();

	return ret;
}

void hp_del_page_from_lru_list(struct hugepage *hpage,
                                struct lruvec *lruvec, enum lru_list lru)
{   
    list_del(&hpage->lru);
    node_page_state_add(-1, lruvec_node(lruvec), NR_LRU_BASE + lru);
}
EXPORT_SYMBOL(hp_del_page_from_lru_list);
void hp_add_page_to_lru_list(struct hugepage *hpage,struct lruvec *lruvec, enum lru_list lru)
{
    SetPageLRU((struct page *)hpage);
    list_add(&hpage->lru, &lruvec->lists[lru]);
    node_page_state_add(1, lruvec_node(lruvec), NR_LRU_BASE + lru);
}
EXPORT_SYMBOL(hp_add_page_to_lru_list);

void page_cache_release(struct page* page)
{
    (is_hpa_page(page)?hpa_put_page((struct hugepage*)page):put_page(page));
}
EXPORT_SYMBOL(page_cache_release);
