#include <syrnck.h>
#include <stdio.h>

int main(int argc, char **argv) {
  SyrnckStore *store = NULL;
  if (argc != 2 || syrnck_abi_version() != SYRNCK_ABI_VERSION) return 1;
  if (syrnck_store_open(argv[1], true, NULL, 0, &store) != 0) {
    fprintf(stderr, "%s\n", syrnck_last_error());
    return 2;
  }
  syrnck_store_close(store);
  return 0;
}
