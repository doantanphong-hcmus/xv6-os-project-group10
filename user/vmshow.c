#include "kernel/types.h"
#include "user/user.h"

// vmshow prog [args...]: run prog and print its page table on exec.
int
main(int argc, char *argv[])
{
  if(argc < 2){
    fprintf(2, "usage: vmshow prog [args...]\n");
    exit(1);
  }
  execvm(argv[1], argv + 1);
  fprintf(2, "vmshow: exec %s failed\n", argv[1]);
  exit(1);
}
