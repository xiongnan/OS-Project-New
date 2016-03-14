#include "vm/page.h"
#include "threads/pte.h"
#include "vm/frame.h"
#include "threads/vaddr.h"
#include "userprog/pagedir.h"
#include "threads/malloc.h"
#include "filesys/file.h"
#include "string.h"
#include "userprog/syscall.h"
//#include "vm/swap.h"

static bool load_page_file (struct supply_pte *);
static bool load_page_swap (struct supply_pte *);
static bool load_page_mmf (struct supply_pte *);
static void free_supply_pte (struct hash_elem *, void * UNUSED);

/* init the supplyemental page table and neccessary data structure */
void 
vm_page_init (void)
{
  return;
}

/* Functionality required by hash table*/
unsigned
supply_pt_hash (const struct hash_elem *he, void *aux UNUSED)
{
  const struct supply_pte *vspte;
  vspte = hash_entry (he, struct supply_pte, elem);
  return hash_bytes (&vspte->uvaddr, sizeof vspte->uvaddr);
}

/* Functionality required by hash table*/
bool
supply_pt_less (const struct hash_elem *hea,
               const struct hash_elem *heb,
	       void *aux UNUSED)
{
  const struct supply_pte *vsptea;
  const struct supply_pte *vspteb;
 
  vsptea = hash_entry (hea, struct supply_pte, elem);
  vspteb = hash_entry (heb, struct supply_pte, elem);

  return (vsptea->uvaddr - vspteb->uvaddr) < 0;
}

/* Given hash table and its key which is a user virtual address, find the
 * corresponding hash element*/
struct supply_pte *
get_supply_pte (struct hash *ht, void *uvaddr)
{
  struct supply_pte spte;
  struct hash_elem *e;

  spte.uvaddr = uvaddr;
  e = hash_find (ht, &spte.elem);
  return e != NULL ? hash_entry (e, struct supply_pte, elem) : NULL;
}

/* Load page data to the page defined in struct supply_pte. */
bool
load_page (struct supply_pte *spte)
{
  bool success = false;
  int type = spte->type;
  if (type == FILE) {
      success = load_page_file (spte);
  } else if (type == MMF || type == MMF | SWAP) {
      success = load_page_mmf (spte);
  } else if (type == SWAP || type == FILE | SWAP) {
      success = load_page_swap (spte);
  }
  return success;
}

/* Load page data to the page defined in struct supply_pte from the given file
   in struct supply_pte */
static bool
load_page_file (struct supply_pte *spte)
{
  
  file_seek (spte->file.file, spte->file.ofs);

  /* Get a page of memory. */
  uint8_t *kpage = vm_allocate_frame (PAL_USER);
  if (kpage == NULL)
    return false;
  
  /* Load this page. */
  if (file_read (spte->file.file, kpage,
		 spte->file.read_bytes)
      
      != (int) spte->file.read_bytes)
    {
      vm_free_frame (kpage);
      return false; 
    }
  memset (kpage + spte->file.read_bytes, 0,
	  spte->file.zero_bytes);
  
  /* Add the page to the process's address space. */
  if (!pagedir_set_page (thread_current ()->pagedir, spte->uvaddr, kpage,
			 spte->file.writable))
    {
      vm_free_frame (kpage);
      return false; 
    }
  
  spte->is_loaded = true;
  return true;
}


/* Load a mmf page whose details are defined in struct supply_pte */
static bool
load_page_mmf (struct supply_pte *spte)
{

  file_seek (spte->mmf.file, spte->mmf.ofs);

  /* Get a page of memory. */
  uint8_t *kpage = vm_allocate_frame (PAL_USER);
  if (kpage == NULL)
    return false;

  /* Load this page. */
  if (file_read (spte->mmf.file, kpage,
		 spte->mmf.read_bytes)
      != (int) spte->mmf.read_bytes)
    {
      vm_free_frame (kpage);
      return false; 
    }
  memset (kpage + spte->mmf.read_bytes, 0,
	  PGSIZE - spte->mmf.read_bytes);

  /* Add the page to the process's address space. */
  if (!pagedir_set_page (thread_current ()->pagedir, spte->uvaddr, kpage, true))
    {
      vm_free_frame (kpage);
      return false; 
    }

  spte->is_loaded = true;
  if (spte->type & SWAP)
    spte->type = MMF;

  return true;
}

/* Load a zero page whose details are defined in struct supply_pte */
static bool
load_page_swap (struct supply_pte *spte)
{
  
  /* Get a page of memory. */
  uint8_t *kpage = vm_allocate_frame (PAL_USER);
  if (kpage == NULL)
    return false;
 
  /* Map the user page to given frame */
  if (!pagedir_set_page (thread_current ()->pagedir, spte->uvaddr, kpage, 
			 spte->swap_writable))
    {
      vm_free_frame (kpage);
      return false;
    }
 
  /* Swap data from disk into memory page */
  vm_swap_in (spte->swap_slot_idx, spte->uvaddr);

  if (spte->type == SWAP)
    {
      /* After swap in, remove the corresponding entry in supply page table */
      hash_delete (&thread_current ()->supply_page_table, &spte->elem);
    }
  if (spte->type == (FILE | SWAP))
    {
      spte->type = FILE;
      spte->is_loaded = true;
    }

  return true;
}

/* Free the given supplyimental page table, which is a hash table */
void free_supply_pt (struct hash *supply_pt) 
{
  hash_destroy (supply_pt, free_supply_pte);
}

/* Free supplyemental page entry represented by the given hash element in
   hash table */
static void
free_supply_pte (struct hash_elem *e, void *aux UNUSED)
{
  struct supply_pte *spte;
  spte = hash_entry (e, struct supply_pte, elem);
  if (spte->type & SWAP)
    vm_clear_swap_slot (spte->swap_slot_idx);

  free (spte);
}

/* insert the given supply pte */
bool 
insert_supply_pte (struct hash *spt, struct supply_pte *spte)
{
  struct hash_elem *result;

  if (spte == NULL)
    return false;
  
  result = hash_insert (spt, &spte->elem);
  if (result != NULL)
    return false;
  
  return true;
}


/* Add an file suplemental page entry to supplyemental page table */
bool
supply_pt_insert_file (struct file *file, off_t ofs, uint8_t *upage, 
		      uint32_t read_bytes, uint32_t zero_bytes, bool writable)
{
  struct supply_pte *spte; 
  struct hash_elem *result;
  struct thread *cur = thread_current ();

  spte = calloc (1, sizeof *spte);
  
  if (spte == NULL)
    return false;
  
  spte->uvaddr = upage;
  spte->type = FILE;
  spte->file.file = file;
  spte->file.ofs = ofs;
  spte->file.read_bytes = read_bytes;
  spte->file.zero_bytes = zero_bytes;
  spte->file.writable = writable;
  spte->is_loaded = false;
      
  result = hash_insert (&cur->supply_page_table, &spte->elem);
  if (result != NULL)
    return false;

  return true;
}

/* Add an file suplemental page entry to supplyemental page table */
bool
supply_pt_insert_mmf (struct file *file, off_t ofs, uint8_t *upage, 
		      uint32_t read_bytes)
{
  struct supply_pte *spte; 
  struct hash_elem *result;
  struct thread *cur = thread_current ();

  spte = calloc (1, sizeof *spte);
      
  if (spte == NULL)
    return false;
  
  spte->uvaddr = upage;
  spte->type = MMF;
  spte->mmf.file = file;
  spte->mmf.ofs = ofs;
  spte->mmf.read_bytes = read_bytes;
  spte->is_loaded = false;
      
  result = hash_insert (&cur->supply_page_table, &spte->elem);
  if (result != NULL)
    return false;

  return true;
}

/* Given a supply_pte struct spte, write data at address spte->uvaddr to
 * file. It is required if a page is dirty */
void write_page_back_to_file_wo_lock (struct supply_pte *spte)
{
  if (spte->type == MMF)
    {
      file_seek (spte->mmf.file, spte->mmf.ofs);
      file_write (spte->mmf.file,
                  spte->uvaddr,
                  spte->mmf.read_bytes);
    }
}









