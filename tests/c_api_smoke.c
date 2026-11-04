#include <syrnck.h>
#include <stdio.h>
#include <string.h>
int main(void) {
  SyrnckStore *store = NULL;
  size_t required = 0;
  char message[128];
  if (syrnck_abi_version() != SYRNCK_ABI_VERSION || sizeof(SyrnckChunking) != 16)
    return 1;
  if (syrnck_store_open(NULL, false, NULL, 0, &store) != 1 || store != NULL ||
      strlen(syrnck_last_error()) == 0)
    return 1;
  if (syrnck_error_copy(NULL, 0, &required) != 0 || required < 2 || required > sizeof(message))
    return 1;
  memset(message, 'X', sizeof(message));
  if (syrnck_error_copy(message, 1, &required) != 1 || message[0] != 'X' ||
      syrnck_error_copy(message, sizeof(message), &required) != 0 ||
      strcmp(message, syrnck_last_error()) != 0)
    return 1;
  if (syrnck_object_read(NULL, "chunk", "bad", NULL, 0, &required) != 1 || required != 0 ||
      syrnck_object_import(NULL, "chunk", "bad", NULL, 0, NULL) != 1 ||
      syrnck_closure_json(NULL, "manifest", "bad", NULL, 0, NULL) != 1)
    return 1;
  if (syrnck_store_close(NULL) != 0 || syrnck_string_free(NULL) != 0)
    return 1;
  required = 123;
  if (syrnck_file_read_range(NULL, "bad", 0, 1, NULL, 0, &required) != 1 || required != 0 ||
      syrnck_file_compare_json(NULL, "bad", "bad", NULL, 0, &required) != 1 || required != 0 ||
      syrnck_restore_file_range(NULL, "bad", "destination", 0, 1) != 1)
    return 1;
  puts("C ABI declarations, linking and null/error handling verified");
  return 0;
}
