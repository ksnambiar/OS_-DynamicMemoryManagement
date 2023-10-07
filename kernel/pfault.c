/* This file contains code for a generic page fault handler for processes. */
#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "defs.h"
#include "elf.h"

#include "sleeplock.h"
#include "fs.h"
#include "buf.h"
// h
int loadseg(pagetable_t pagetable, uint64 va, struct inode *ip, uint offset, uint sz);
int flags2perm(int flags);

/* CSE 536: (2.4) read current time. */
uint64 read_current_timestamp() {
  uint64 curticks = 0;
  acquire(&tickslock);
  curticks = ticks;
  wakeup(&ticks);
  release(&tickslock);
  return curticks;
}

bool psa_tracker[PSASIZE];

/* All blocks are free during initialization. */
void init_psa_regions(void)
{
    for (int i = 0; i < PSASIZE; i++) 
        psa_tracker[i] = false;
}

/* Evict heap page to disk when resident pages exceed limit */
void evict_page_to_disk(struct proc* p) {
    /* Find free block */
    int blockno = 0;
    for (int i = 0; i <= PSASIZE; ++i){
      if(psa_tracker[i] == false) {
        blockno = i;
        break;
      }
    }

    /* Find victim page using FIFO. */
    uint64 victim_time = p->heap_tracker[0].last_load_time;
    int page_index = 0;
    for (int i = 0; i < MAXHEAP; i++)
    { 
      if(p->heap_tracker[i].last_load_time < victim_time){
        victim_time = p->heap_tracker[i].last_load_time;
        page_index = i;
      }
    }
    
    /* Print statement. */
    print_evict_page(p->heap_tracker[page_index].addr, blockno);
    /* Read memory from the user to kernel memory first. */
    char *kpage;
    kpage = kalloc();
    int result = copyin(p->pagetable, kpage, p->heap_tracker[page_index].addr, PGSIZE);
    if (result == -1) {
      return;
    }
    /* Write to the disk blocks. Below is a template as to how this works. There is
     * definitely a better way but this works for now. :p */
    struct buf* b;

    for(int i = blockno; i < blockno+4; ++i) {
    
    b = bread(1, PSASTART + i);

    memmove(b->data, ((uint64)kpage) +((i-blockno)*BSIZE), (BSIZE));
    bwrite(b);
    brelse(b);
    psa_tracker[i] = true;
    }
    
    /* Unmap swapped out page */

    uvmunmap(p->pagetable, p->heap_tracker[page_index].addr, 1, 0);
    kfree(kpage);
    /* Update the resident heap tracker. */
    p->resident_heap_pages-=1;
    p->heap_tracker[page_index].last_load_time = 0xFFFFFFFFFFFFFFFF;
}

/* Retrieve faulted page from disk. */
void retrieve_page_from_disk(struct proc* p, uint64 uvaddr) {
    /* Find where the page is located in disk */
    int page_position = -1;
    for (int i = 0; i < MAXHEAP; i++)
    { 
      if(uvaddr != 0 && p->heap_tracker[i].addr == uvaddr) {
        page_position = i;
        break;
      }
      /* code */
    }
    /* Print statement. */
    int blockno = p->heap_tracker[page_position].startblock;
    print_retrieve_page(uvaddr, p->heap_tracker[page_position].startblock);

    /* Create a kernel page to read memory temporarily into first. */
    char *kpage;
    kpage = kalloc();
    
    /* Read the disk block into temp kernel page. */
  for(int i = blockno; i < blockno+4; ++i) {  
    struct buf* b;
    b = bread(1, PSASTART+(i));
    memmove(((uint64)kpage) +((i-blockno)*(BSIZE)), b->data, (BSIZE));
    brelse(b);
    }
    copyout(p->pagetable,uvaddr, kpage, PGSIZE);
}

void page_fault_handler(void) 
{
    /* Current process struct */
    struct proc *p = myproc();

    /* Track whether the heap page should be brought back from disk or not. */
    bool load_from_disk = false;
    

    /* Find faulting address. */
    uint64 stval = r_stval();
    uint64 faulting_addr = PGROUNDDOWN(r_stval());
    print_page_fault(p->name, faulting_addr);


    if(p->cow_enabled && r_scause() == 15) {
      copy_on_write();
      goto out;
    }
    
    /* Check if the fault address is a heap page. Use p->heap_tracker */
    bool isPageHeap = false;
    int position = -1;
    if (stval == -1) {
      setkilled(p);
      return;
    }
    
    for (int i = 0; i < MAXHEAP; i++)
    {
      if(faulting_addr != 0 && p->heap_tracker[i].addr == faulting_addr) {
        isPageHeap = true;
        position = i;
        break;
      }
      /* code */
    }
    if(isPageHeap && p->heap_tracker[position].loaded == true) {
      load_from_disk = true;
    }
    
    
    if (isPageHeap) {
        goto heap_handle;
    }
    /* If it came here, it is a page from the program binary that we must load. */

/* tryout area start*/
  // if(true){  
    struct elfhdr elf;
    struct inode *ip;
    struct proghdr ph;
    int i, off;

    uint64 sz = 0;
    //trying something new here
    pagetable_t pagetable = 0x0, oldpagetable;
    begin_op();
    if((ip = namei(p->name)) == 0){
    end_op();
    return -1;
    }
    ilock(ip);

    if(readi(ip, 0, (uint64)&elf, 0, sizeof(elf)) != sizeof(elf))
    goto out;

    init_psa_regions();
    cow_init();
    for(i=0, off=elf.phoff; i<elf.phnum; i++, off+=sizeof(ph)){
    if(readi(ip, 0, (uint64)&ph, off, sizeof(ph)) != sizeof(ph))
      goto out;
    
    if(ph.type != ELF_PROG_LOAD){
      // printf("\nelf prog not good\n");
      continue;
    }
    if (ph.memsz < ph.filesz){
      // printf("\n memsz less than filesz\n");
      goto out;
    }
    if (ph.vaddr + ph.memsz < ph.vaddr){
      //  printf("\n vaddr + memsz < vaddr\n"); 
      goto out;}
    if (ph.vaddr % PGSIZE != 0){
      // printf("\n vaddr pgsize issue \n");
      goto out;
      }

    if (faulting_addr >= ph.vaddr && faulting_addr < (ph.vaddr + ph.memsz)) {
        uint64 sz1;
        if ((sz1 = uvmalloc(p->pagetable, ph.vaddr, ph.vaddr + ph.memsz, flags2perm(ph.flags))) == 0){
        printf("uv malloc failed");
        goto out;
        }

        sz = sz1;
        // printf("loop intermediate: vaddr = %d, sz = %d, ph.memsz= %d\n\n", ph.vaddr, sz,ph.memsz);
        // printf("loop2 %d: phvaddr = %d, phoff = %d,ph.off = %d, ph.memsz = %d, sz = %d  \n", i,ph.vaddr, elf.phoff, ph.off, ph.memsz, sz);
        
        if (loadseg(p->pagetable, ph.vaddr, ip, ph.off, ph.memsz) < 0)
        goto out;

        if(ph.filesz > 0) {
        print_load_seg(faulting_addr, ph.off, ph.filesz);
        }
        }

  }
  iunlockput(ip);
  end_op();

    /* Go to out, since the remainder of this code is for the heap. */
  goto out;

heap_handle:
    /* 2.4: Check if resident pages are more than heap pages. If yes, evict. */
    bool isHeapFull = false;
    int heapCount = 0;
    for (int i = 0; i < MAXHEAP; i++)
    { 
      if(p->heap_tracker[i].addr != 0xFFFFFFFFFFFFFFFF){
        ++heapCount;
      }
    }
    if(heapCount == MAXHEAP) {
          // printf("\n%d %d\n", heapCount, MAXHEAP);
          isHeapFull = true;
    }
    if (p->resident_heap_pages == MAXRESHEAP && !isHeapFull) {
        evict_page_to_disk(p);
    }

    /* 2.3: Map a heap page into the process' address space. (Hint: check growproc) */
    
    if(sz = uvmalloc(p->pagetable, faulting_addr, faulting_addr + PGSIZE, PTE_W) == 0) {
      return -1;
    }

    /* 2.4: Update the last load time for the loaded heap page in p->heap_tracker. */
    if (position == -1) {
      printf("EXEC: illegal position");
      panic("illegal position within array");
    }

    p->heap_tracker[position].last_load_time = read_current_timestamp();
    p->heap_tracker[position].loaded = true;
    
    /* 2.4: Heap page was swapped to disk previously. We must load it from disk. */
    if (load_from_disk && !isHeapFull) {
        retrieve_page_from_disk(p, faulting_addr);
    }

    /* Track that another heap page has been brought into memory. */
    p->resident_heap_pages++;

out:
    /* Flush stale page table entries. This is important to always do. */
    sfence_vma();
    return;
}