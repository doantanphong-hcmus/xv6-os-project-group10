#include "kernel/types.h"
#include "user/user.h"

// Exercise invalid execvm arguments; the Linux runner checks the return values.
int
main(void)
{
  char *missing[] = { "nosuchprog", 0 };
  char *notelf[] = { "README", 0 };

  printf("missing file: %d\n", execvm("nosuchprog", missing));
  printf("non-ELF file: %d\n", execvm("README", notelf));
  printf("invalid argv: %d\n", execvm("echo", (char **)0xffffffffffL));
  exit(0);
}
