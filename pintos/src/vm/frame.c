#include <debug.h>
#include <stdio.h>
#include <string.h>
#include "threads/malloc.h"
#include "threads/synch.h"
#include "threads/palloc.h"
#include "lib/kernel/list.h"
#include "userprog/pagedir.h"
#include "vm/page.h"
#include "threads/pte.h"
//#include "vm/swap.h"

#include "vm/frame.h"

/* lock to synchronize between processes on frame table */
static struct lock vm_lock;

/* lock to ensure the eviction is atomic */
static struct lock eviction_lock;

/* functionality to operate vm_frames */
static bool add_vm_frame (void *);
static void remove_vm_frame (void *);
/* Get the vm_frame struct, whose frame contribute equals the given frame, from
 * the frame list vm_frames. */
static struct vm_frame *get_vm_frame (void *);

/* functionalities needed by eviction*/
static struct vm_frame *frame_to_evict (void); // select a frame to evict
// save evicted frame's content for later swap in
static bool save_evicted_frame (struct vm_frame *);

/* Block device that contains the swap */
struct block *swap_device;

/* Bitmap of swap slot availablities and corresponding lock */
static struct bitmap *swap_map;

/* Represents how many sectors are needed to store a page */
static size_t SECTORS_PER_PAGE = PGSIZE / BLOCK_SECTOR_SIZE;
static size_t swap_size_in_page (void);




/* init the frame table and necessary data structure */
void
vm_frames_init ()
{
  list_init (&vm_frames);
  lock_init (&vm_lock);
  lock_init (&eviction_lock);
  
  /* init the swap device */
  swap_device = block_get_role (BLOCK_SWAP);
  if (swap_device == NULL)
    PANIC ("no swap device found, can't initialize swap");
  
  /* init the swap bitmap */
  swap_map = bitmap_create (swap_size_in_page ());
  if (swap_map == NULL)
    PANIC ("swap bitmap creation failed");
  
  /* initialize all bits to be true */
  bitmap_set_all (swap_map, true);

}

/* allocate a page from USER_POOL, and add an entry to frame table */
void *
vm_allocate_frame (enum palloc_flags flags)
{
  void *frame = NULL;

  /* trying to allocate a page from user pool */
  if (flags & PAL_USER)
    {
      if (flags & PAL_ZERO)
        frame = palloc_get_page (PAL_USER | PAL_ZERO);
      else
        frame = palloc_get_page (PAL_USER);
    }

  /* if succeed, add to frame list
     otherwise, should evict one page to swap, but fail the allocator
     for now */
  if (frame != NULL)
    add_vm_frame (frame);
  else
    if ((frame = evict_frame ()) == NULL)
      PANIC ("Evicting frame failed");

  return frame;
}

void
vm_free_frame (void *frame)
{
  /* remove frame table entry */
  remove_vm_frame (frame);
  /* free the frame */
  palloc_free_page (frame);
}

/* set the pte attribute to PTE in corresponding entry of FRAME */
void
vm_frame_set_usr (void *frame, uint32_t *pte, void *upage)
{
  struct vm_frame *vf;
  vf = get_vm_frame (frame);
  if (vf != NULL)
    {
      vf->pte = pte;
      vf->uva = upage;
    }
}

/* evict a frame and save its content for later swap in */
void *
evict_frame ()
{
  bool result;
  struct vm_frame *vf;
  struct thread *t = thread_current ();

  lock_acquire (&eviction_lock);

  vf = frame_to_evict ();
  if (vf == NULL)
    PANIC ("No frame to evict.");

  result = save_evicted_frame (vf);
  if (!result)
    PANIC ("can't save evicted frame");
  
  vf->tid = t->tid;
  vf->pte = NULL;
  vf->uva = NULL;

  lock_release (&eviction_lock);

  return vf->frame;
}

/* select a frame to evict */
static struct vm_frame *
frame_to_evict ()
{
  struct vm_frame *vf;
  struct thread *t;
  struct list_elem *e;

  struct vm_frame *vf_class0 = NULL;

  int round_count = 1;
  bool found = false;
  /* iterate each entry in frame table */
  while (!found)
    {
      /* go through the vm frame list, try to locate the first encounter
         of each class of the four. The goal is to find a (0,0) class,
         if found, the break eviction selecting is ended,
         if not, set the reference/accessed bit to 0 of each page.
         The maxium round is 2, which is one scan after all the reference
         bit are set to 0, if we still cannot find (0,0), we have to live
         with the first encounter of the lowest nonempty class*/
      e = list_head (&vm_frames);
      while ((e = list_next (e)) != list_tail (&vm_frames))
        {
          vf = list_entry (e, struct vm_frame, elem);
          t = thread_get_by_id (vf->tid);
          bool accessed  = pagedir_is_accessed (t->pagedir, vf->uva);
          if (!accessed)
            {
              vf_class0 = vf;
              list_remove (e);
              list_push_back (&vm_frames, e);
              break;
            }
          else
            {
              pagedir_set_accessed (t->pagedir, vf->uva, false);
            }
        }

      if (vf_class0 != NULL)
        found = true;
      else if (round_count++ == 2)
        found = true;
    }

  return vf_class0;
}
 
/* save evicted frame's content for later swap in */
static bool
save_evicted_frame (struct vm_frame *vf)
{
  struct thread *t;
  struct suppl_pte *spte;

  /* Get corresponding thread vm_frame->tid's suppl page table */
  t = thread_get_by_id (vf->tid);

  /* Get suppl page table entry corresponding to vm_frame->uva */
  spte = get_suppl_pte (&t->suppl_page_table, vf->uva);

  /* if no suppl page table entry is found, create one and insert it
     into suppl page table */
  if (spte == NULL)
    {
      spte = calloc(1, sizeof *spte);
      spte->uvaddr = vf->uva;
      spte->type = SWAP;
      if (!insert_suppl_pte (&t->suppl_page_table, spte))
        return false;
    }

  size_t swap_slot_idx;
  /* if the page is dirty and mmf_page write it back to file
   * else if the page is dirty, put into swap
   * if a page is not dirty and is not a file, then it is a stack,
   * it needs to put into swap
   * Here, for a file, it is not dirty, we do nothing, since we can
   * always re-load that file when needed.*/
  if (pagedir_is_dirty (t->pagedir, spte->uvaddr) && (spte->type == MMF))
  {
      write_page_back_to_file_wo_lock (spte);
  }
  else if (pagedir_is_dirty (t->pagedir, spte->uvaddr) || (spte->type != FILE))
  {
      swap_slot_idx = vm_swap_out (spte->uvaddr);
      if (swap_slot_idx == SWAP_ERROR)
        return false;

      spte->type = spte->type | SWAP;
  }
  /* else if the page clean or read-only, do nothing */

  memset (vf->frame, 0, PGSIZE);
  /* update the swap attributes, including swap_slot_idx,
     and swap_writable */
  spte->swap_slot_idx = swap_slot_idx;
  spte->swap_writable = *(vf->pte) & PTE_W;

  spte->is_loaded = false;

  /* unmap it from user's pagedir, free vm page/frame */
  pagedir_clear_page (t->pagedir, spte->uvaddr);

  return true;
}

/* Add an entry to frame table */
static bool
add_vm_frame (void *frame)
{
  struct vm_frame *vf;
  vf = calloc (1, sizeof *vf);
 
  if (vf == NULL)
    return false;

  vf->tid = thread_current ()->tid;
  vf->frame = frame;
  
  lock_acquire (&vm_lock);
  list_push_back (&vm_frames, &vf->elem);
  lock_release (&vm_lock);

  return true;
  
}

/* Remove the entry from frame table and free the memory space */
static void
remove_vm_frame (void *frame)
{
  struct vm_frame *vf;
  struct list_elem *e;
  
  lock_acquire (&vm_lock);
  e = list_head (&vm_frames);
  while ((e = list_next (e)) != list_tail (&vm_frames))
    {
      vf = list_entry (e, struct vm_frame, elem);
      if (vf->frame == frame)
        {
          list_remove (e);
          free (vf);
          break;
        }
    }
  lock_release (&vm_lock);
}

/* Get the vm_frame struct, whose frame contribute equals the given frame, from
 * the frame list vm_frames. */
static struct vm_frame *
get_vm_frame (void *frame)
{
  struct vm_frame *vf;
  struct list_elem *e;
  
  lock_acquire (&vm_lock);
  e = list_head (&vm_frames);
  while ((e = list_next (e)) != list_tail (&vm_frames))
    {
      vf = list_entry (e, struct vm_frame, elem);
      if (vf->frame == frame)
        break;
      vf = NULL;
    }
  lock_release (&vm_lock);

  return vf;
}

/* Find an available swap slot and dump in the given page represented by UVA
 If failed, return SWAP_ERROR
 Otherwise, return the swap slot index */
/* No inner synchronization, should be used with a sync machanism */
size_t vm_swap_out (const void *uva)
{
  /* find a swap slot and mark it in use */
  size_t swap_idx = bitmap_scan_and_flip (swap_map, 0, 1, true);
  
  if (swap_idx == BITMAP_ERROR)
    return SWAP_ERROR;
  
  /* write the page of data to the swap slot */
  size_t counter = 0;
  while (counter < SECTORS_PER_PAGE)
  {
    block_write (swap_device, swap_idx * SECTORS_PER_PAGE + counter,
                 uva + counter * BLOCK_SECTOR_SIZE);
    counter++;
  }
  return swap_idx;
}

/* Swap a page of data in swap slot SWAP_IDX to a page starting at UVA */
void
vm_swap_in (size_t swap_idx, void *uva)
{
  /* swap out the data from swap slot to mem page */
  size_t counter = 0;
  while (counter < SECTORS_PER_PAGE)
  {
    block_read (swap_device, swap_idx * SECTORS_PER_PAGE + counter,
                uva + counter * BLOCK_SECTOR_SIZE);
    counter++;
  }
  /* free the corresponding swap slot bit in bitmap */
  bitmap_flip (swap_map, swap_idx);
}

void vm_clear_swap_slot (size_t swap_idx)
{
  /* free the corresponding swap slot bit in bitmap */
  bitmap_flip (swap_map, swap_idx);
}

/* Returns how many pages the swap device can contain, which is rounded down */
static size_t
swap_size_in_page ()
{
  return block_size (swap_device) / SECTORS_PER_PAGE;
}



