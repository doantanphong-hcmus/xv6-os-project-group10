#include "kernel/types.h"
#include "user/user.h"

// Cross a 2 MiB boundary so exec needs more than one lowest-level table.
static volatile char data[2 * 1024 * 1024 + 4096];

int
main(void)
{
  data[0] = 17;
  data[sizeof(data) - 1] = 23;
  if(data[0] != 17 || data[sizeof(data) - 1] != 23)
    exit(1);
  printf("VMWIDE OK\n");
  exit(0);
}
