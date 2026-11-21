#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "defs.h"

// Print the valid PTEs of one page-table page and descend into
// lower-level tables. depth is 1 for the root, 3 for leaf level.
static void vmprint_level(pagetable_t pagetable, int depth){
  for (int i=0;i<512;i++){
    pte_t pte=pagetable[i];

    // Skip invalid PTEs.
    if((pte & PTE_V) == 0)
      continue;

    for(int j=0;j<depth;j++){
      printf(" ..");
    }
    printf("%d: pte %p pa %p\n", i, (void *)pte, (void *)PTE2PA(pte));

    // A valid PTE with R, W and X all clear points to a lower-level table.
    if((pte & (PTE_R|PTE_W|PTE_X)) == 0)
      vmprint_level((pagetable_t)PTE2PA(pte), depth + 1);
  }
}

// Print a page table: its address, then every valid PTE.
void vmprint(pagetable_t pagetable){
  printf("page table %p\n", pagetable);
  vmprint_level(pagetable, 1);
}
