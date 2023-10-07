#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "defs.h"
#include "elf.h"
#include <stdbool.h>

struct spinlock cow_lock;

// Max number of pages a CoW group of processes can share
#define SHMEM_MAX 100

struct cow_group {
    int group; // group id
    uint64 shmem[SHMEM_MAX]; // list of pages a CoW group share
    int count; // Number of active processes
};

struct cow_group cow_group[NPROC];

struct cow_group* get_cow_group(int group) {
    if(group == -1)
        return 0;

    for(int i = 0; i < NPROC; i++) {
        if(cow_group[i].group == group)
            return &cow_group[i];
    }
    return 0;
}

void cow_group_init(int groupno) {
    for(int i = 0; i < NPROC; i++) {
        if(cow_group[i].group == -1) {
            cow_group[i].group = groupno;
            return;
        }
    }
} 

int get_cow_group_count(int group) {
    return get_cow_group(group)->count;
}
void incr_cow_group_count(int group) {
    get_cow_group(group)->count = get_cow_group_count(group)+1;
}
void decr_cow_group_count(int group) {
    get_cow_group(group)->count = get_cow_group_count(group)-1;
}

void add_shmem(int group, uint64 pa) {
    if(group == -1)
        return;

    uint64 *shmem = get_cow_group(group)->shmem;
    int index;
    for(index = 0; index < SHMEM_MAX; index++) {
        // duplicate address
        if(shmem[index] == pa)
            return;
        if(shmem[index] == 0)
            break;
    }
    shmem[index] = pa;
}

int is_shmem(int group, uint64 pa) {
    if(group == -1)
        return 0;

    uint64 *shmem = get_cow_group(group)->shmem;
    for(int i = 0; i < SHMEM_MAX; i++) {
        if(shmem[i] == 0)
            return 0;
        if(shmem[i] == pa)
            return 1;
    }
    return 0;
}

void cow_init() {
    for(int i = 0; i < NPROC; i++) {
        cow_group[i].count = 0;
        cow_group[i].group = -1;
        for(int j = 0; j < SHMEM_MAX; j++)
            cow_group[i].shmem[j] = 0;
    }
    initlock(&cow_lock, "cow_lock");
}

int uvmcopy_cow(pagetable_t old, pagetable_t new, uint64 sz) {
    /* CSE 536: (2.6.1) Handling Copy-on-write fork() */
    struct proc* p = myproc();
    // Copy user vitual memory from old(parent) to new(child) process
    
    pte_t *pte;
    uint64 pa, i;
    uint flags;
  if(get_cow_group_count(p->cow_group) == 0) {
    incr_cow_group_count(p->cow_group);
  }

  for(i = 0; i < sz; i += PGSIZE){
    if((pte = walk(old, i, 0)) == 0)
      panic("uvmcopy: pte should exist");
    if((*pte & PTE_V) == 0)
      panic("uvmcopy: page not present");
    pa = PTE2PA(*pte);
    flags = PTE_FLAGS(*pte);
    // clear PTE_W in the PTEs of both child and parent*
    flags &= (~PTE_W);

    // map the parent’s physical pages into the child
    if(mappages(new, i, PGSIZE, (uint64)pa, flags) != 0){
      goto err;
    }

    // Remove parent page table mapping.
    add_shmem(p->cow_group, (uint64)pa);
    uvmunmap(old, i, 1, 0);
    if (mappages(old, i, PGSIZE, pa, flags) != 0) {
      goto err;
    }
  }
    incr_cow_group_count(p->cow_group);

    // printf("\nCREATED PROCESS: cow_group: %d, pid = %d\n", p->cow_group, get_cow_group_count(p->cow_group));
    return 0;

err:
    printf("\nERROR ERROR got called\n");
    uvmunmap(new, 0, i/PGSIZE, 1);
    return -1;
}

void copy_on_write() {
    /* CSE 536: (2.6.2) Handling Copy-on-write */
    struct proc* p = myproc();
    uint64 required_address = PGROUNDDOWN(r_stval());
    pte_t *pte;
    pte = walk(p->pagetable, required_address, 0);
    if (pte == 0) {
        printf("page not found\n");
        // goto end;
        return;
    }
    // Allocate a new page 
    
    uint flags = PTE_FLAGS(*pte);
    flags = flags | PTE_W;

    char *mem = kalloc();
    uint64 pa = PTE2PA(*pte);
    // Copy contents from the shared page to the new page
    memmove(mem, (char*)pa, PGSIZE);
 
    uvmunmap(p->pagetable, required_address, 1, get_cow_group(p->cow_group)->count == 1 ? 1 : 0);
    //remove from shmem

    // Map the new page in the faulting process's page table with write permissions
    if (mappages(p->pagetable, required_address, PGSIZE, mem, flags) != 0) {
      panic("COPYOW: issue while writing");
    }
    print_copy_on_write(p, required_address);
}
