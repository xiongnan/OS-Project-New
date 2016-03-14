#ifndef VM_PAGE_H
#define VM_PAGE_H

#define STACK_SIZE (8 * (1 << 20))

#include <stdio.h>
#include "threads/thread.h"
#include "threads/palloc.h"
#include "lib/kernel/hash.h"
#include "filesys/file.h"

#define SWAP  1
#define FILE  2
#define MMF 3


//union spte_data
//{
  struct
  {
    struct file * file;
    off_t ofs;
    uint32_t read_bytes;
    uint32_t zero_bytes;
    bool writable;
  } file_page;

  struct 
  {
    struct file *file;
    off_t ofs;
    uint32_t read_bytes;
  } mmf_page;
//};

/* supplemental page table entry */
struct supply_pte
{
  void *uvaddr;   //user virtual address as the unique identifier of a page
  int type; // 1 -> SWAP, 2 -> FILE, 3 -> MMF
  //union supply_pte_data data;
  struct file_page;
  struct mmf_page;
  
  bool is_loaded;

  /* reserved for possible swapping */
  size_t swap_slot_idx;
  bool swap_writable;

  struct hash_elem elem;
};

/* Initialization of the supplemental page table management provided */
void vm_page_init(void);

/* Functionalities required by hash table, which is supplyemental_pt */
unsigned supply_pt_hash (const struct hash_elem *, void * UNUSED);
bool supply_pt_less (const struct hash_elem *, 
		    const struct hash_elem *,
		    void * UNUSED);


/* insert the given supply pte */
bool insert_supply_pte (struct hash *, struct supply_pte *);

/* Add a file supplemental page table entry to the current thread's
 * supplemental page table */
bool supply_pt_insert_file ( struct file *, off_t, uint8_t *, 
			    uint32_t, uint32_t, bool);

/* Add a memory-mapped-file supplyemental page table entry to the current
 * thread's supplemental page table */
bool supply_pt_insert_mmf (struct file *, off_t, uint8_t *, uint32_t);

/* Given hash table and its key which is a user virtual address, find the
 * corresponding hash element*/
struct supply_pte *get_supply_pte (struct hash *, void *);

/* Given a supply_pte struct spte, write data at address spte->uvaddr to
 * file. It is required if a page is dirty */
void write_page_back_to_file_wo_lock (struct supply_pte *);

/* Free the given supplemental page table, which is a hash table */
void free_supply_pt (struct hash *);

/* Load page data to the page defined in struct supply_pte. */
bool load_page (struct supply_pte *);


#endif /* vm/page.h */
