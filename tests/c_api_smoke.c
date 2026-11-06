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
      syrnck_restore_file_range(NULL, "bad", "destination", 0, 1) != 1 ||
      syrnck_file_verify(NULL, "bad") != 1)
    return 1;
  {
    SyrnckWorkspace *workspace = NULL;
    char *json = NULL;
    if (sizeof(SyrnckWorkspaceOptions) != 12 ||
        syrnck_workspace_open(NULL, NULL, NULL, &workspace) != 1 || workspace != NULL ||
        syrnck_workspace_invalidate(NULL) != 1 || syrnck_workspace_close(NULL) != 0)
      return 1;
    required = 123;
    if (syrnck_workspace_wait_json(NULL, 0, &json, &required) != 1 || json != NULL || required != 0 ||
        syrnck_workspace_status_json(NULL, NULL, NULL, 0, &json, &required) != 1 ||
        json != NULL || required != 0 || syrnck_workspace_wait_json(NULL, 0, NULL, &required) != 1)
      return 1;
  }
  {
    SyrnckCancellation *cancel = NULL;
    if (SYRNCK_CANCELLED != 3 || syrnck_cancellation_create(NULL) != 1 ||
        syrnck_cancellation_request(NULL) != 1 || syrnck_cancellation_close(NULL) != 0 ||
        syrnck_cancellation_create(&cancel) != 0 || !cancel ||
        syrnck_cancellation_request(cancel) != 0 || syrnck_cancellation_request(cancel) != 0 ||
        syrnck_cancellation_close(cancel) != 0)
      return 1;
  }
  puts("C ABI declarations, linking and null/error handling verified");
  return 0;
}
