#include "kernel/types.h"
#include "kernel/param.h"
#include "kernel/riscv.h"
#include "kernel/memlayout.h"
#include "user/user.h"

static volatile char *pages;
static uint mask;

static void
check(int ok, char *message)
{
  if(!ok){
    fprintf(2, "pgaccesstest: FAIL: %s\n", message);
    exit(1);
  }
}

static uint
sample(int n)
{
  mask = ~0U;
  check(pgaccess((void *)pages, n, &mask) == 0, "sample mapped pages");
  return mask;
}

static void
reject(void *start, int n, uint *out, char *message)
{
  sample(32);
  pages[31 * PGSIZE] = 1;
  check(pgaccess(start, n, out) == -1, message);
  check(sample(32) == (1U << 31), "failed call preserves access bits");
}

int
main(void)
{
  char *mem = sbrk(34 * PGSIZE);
  check(mem != (char *)-1, "allocate test pages");
  check((uint64)mem % PGSIZE == 0, "test allocation is page aligned");
  pages = mem;
  for(int i = 0; i < 32; i++)
    pages[i * PGSIZE] = i;
  sample(32);

  printf("[1/6] No accesses, reads, writes and repeated samples\n");
  check(sample(32) == 0, "untouched pages have zero mask");
  check(pages[PGSIZE] == 1, "read test page");
  pages[3 * PGSIZE] = 9;
  check(sample(32) == ((1U << 1) | (1U << 3)), "read and write bits");
  check(sample(32) == 0, "sample clears accessed bits");
  check(pages[3 * PGSIZE] == 9, "read page again");
  check(sample(32) == (1U << 3), "access is detected again after clearing");

  printf("[2/6] All 32 bits and result buffer size\n");
  for(int i = 0; i < 32; i++)
    pages[i * PGSIZE] = i;
  check(sample(32) == ~0U, "all 32 pages accessed");
  pages[31 * PGSIZE] = 1;
  struct {
    uint before, result, after;
  } bounded = {0x12345678, 0, 0x87654321};
  check(pgaccess(mem, 32, &bounded.result) == 0, "32-page sample");
  check(bounded.result == (1U << 31), "highest mask bit");
  check(bounded.before == 0x12345678 && bounded.after == 0x87654321,
        "result writes exactly four bytes");

  printf("[3/6] Zero count, unaligned start and read-only input\n");
  pages[PGSIZE] = 1;
  check(sample(0) == 0, "zero pages returns zero mask");
  check(sample(32) == (1U << 1), "zero count does not clear other pages");
  pages[2 * PGSIZE] = 1;
  check(pgaccess(mem + 2 * PGSIZE + 7, 1, &mask) == 0 && mask == 1,
        "unaligned start selects its containing page");
  check(pgaccess(0, 1, &mask) == 0, "read-only user text can be sampled");

  printf("[4/6] Invalid counts, addresses and protected pages\n");
  reject(mem, -1, &mask, "negative page count");
  reject(mem, 33, &mask, "page count above 32");
  reject((void *)MAXVA, 1, &mask, "address at MAXVA");
  reject((void *)~(uint64)0, 1, &mask, "address overflow");
  reject((void *)(MAXVA - PGSIZE), 2, &mask, "range exceeds MAXVA");
  reject(mem + 34 * PGSIZE, 1, &mask, "unmapped page");
  reject(mem + 31 * PGSIZE, 4, &mask, "partially mapped range");
  uint64 guard = PGROUNDDOWN(r_sp()) - USERSTACK * PGSIZE;
  reject((void *)guard, 1, &mask, "stack guard page");
  reject((void *)TRAPFRAME, 1, &mask, "supervisor trapframe");
  reject((void *)TRAMPOLINE, 1, &mask, "supervisor trampoline");

  printf("[5/6] Invalid output buffers preserve accessed bits\n");
  reject(mem, 32, 0, "read-only output buffer");
  reject(mem, 32, (uint *)MAXVA, "output at MAXVA");
  reject(mem, 32, (uint *)~(uint64)0, "invalid output address");
  reject(mem, 32, (uint *)TRAPFRAME, "supervisor output buffer");
  reject(mem, 32, (uint *)(mem + 34 * PGSIZE - 2), "partially writable output");

  printf("[6/6] Result buffer crossing two writable pages\n");
  pages[7 * PGSIZE] = 1;
  char *cross = mem + 33 * PGSIZE - 2;
  check(pgaccess(mem, 32, (uint *)cross) == 0, "cross-page output");
  memmove(&mask, cross, sizeof(mask));
  check(mask == (1U << 7), "cross-page result matches accesses");
  check(sample(32) == 0, "cross-page output still clears access bits");
  check(sbrk(-34 * PGSIZE) != (char *)-1, "release test pages");
  printf("pgaccesstest: ALL TESTS PASSED (6/6 groups)\n");
  exit(0);
}
