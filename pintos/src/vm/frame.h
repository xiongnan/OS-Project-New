#ifndef VM_FRAME_H
#define VM_FRAME_H

#include "threads/thread.h"

struct vm_frame {
  void *frame;
  tid_t tid;
  uint32_t *pte;
  void *uva;
  struct list_elem elem;
};

struct list vm_frames;

/* frame allocation functionalitie */
void vm_frames_init (void);
void *vm_allocate_frame (enum palloc_flags flags);
void vm_free_frame (void *);

/* frame table management functionalities */
void vm_frame_set_usr (void*, uint32_t *, void *);

/* evict a frame to be freed and write the content to swap slot or file*/
void *evict_frame (void);


#define SWAP_ERROR SIZE_MAX

/* Swap initialization */
void vm_swap_init (void);

/* Swap a frame into a swap slot */
size_t vm_swap_out (const void *);

/* Swap a frame out of a swap slot to mem page */
void vm_swap_in (size_t, void *);

void vm_clear_swap_slot (size_t);



#endif /* vm/frame.h */
