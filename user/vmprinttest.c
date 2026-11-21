#include "kernel/types.h"
#include "user/user.h"

// Run path in a child with exec (print == 0) or execvm (print == 1).
// Returns the child's exit status.
static int
run(int print, char *path, char **argv)
{
  int pid = fork();
  if(pid < 0){
    printf("fork failed\n");
    exit(1);
  }
  if(pid == 0){
    if(print)
      execvm(path, argv);
    else
      exec(path, argv);
    exit(2);  // exec failed
  }
  int status;
  wait(&status);
  return status;
}

static int failures = 0;

static void
check(char *name, int ok)
{
  printf("%s: %s\n", name, ok ? "OK" : "FAIL");
  if(!ok)
    failures++;
}

int
main(void)
{
  char *echo_normal[] = { "echo", "normal-exec", 0 };
  char *echo_print[] = { "echo", "execvm-exec", 0 };
  char *missing[] = { "nosuchprog", 0 };
  char *notelf[] = { "README", 0 };

  // With LAB=syscall, uvmalloc does not zero new pages, so .bss is not
  // guaranteed to start at 0.
  failures = 0;

  printf("[1] exec: no page table printed\n");
  check("test1", run(0, "echo", echo_normal) == 0);

  printf("[2] execvm: page table of echo printed\n");
  check("test2", run(1, "echo", echo_print) == 0);

  printf("[3] execvm on missing file\n");
  check("test3", execvm("nosuchprog", missing) == -1);

  printf("[4] execvm on non-ELF file\n");
  check("test4", execvm("README", notelf) == -1);

  printf("[5] execvm with invalid argv\n");
  check("test5", execvm("echo", (char **)0xffffffffffL) == -1);

  if(failures == 0)
    printf("vmprinttest: ALL OK\n");
  else
    printf("vmprinttest: %d FAIL\n", failures);
  exit(failures != 0);
}
