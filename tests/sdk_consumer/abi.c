#include <syrnck.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv) {
  SyrnckStore *store = NULL;
  SyrnckStore *wire = NULL;
  SyrnckSealedKey *key = NULL;
  SyrnckSystemVault *vault = NULL;
  unsigned char envelope[SYRNCK_PROTECTED_KEY_SIZE];
  const unsigned char password[] = "Installed SDK fixture password";
  size_t length = 0;
  char *result = NULL, *wire_path = NULL;
  char id[65], roots[160];
  int status = 3;
  const SyrnckSealedLimits limits = {0};
  if (argc != 2 || syrnck_abi_version() != SYRNCK_ABI_VERSION) return 1;
  if (syrnck_store_open(argv[1], true, NULL, 0, &store) != 0) {
    fprintf(stderr, "%s\n", syrnck_last_error());
    return 2;
  }
  if (sizeof(SyrnckSealedLimits) != 40) goto done;
  if (syrnck_system_vault_open(NULL, &vault) != 0) goto done;
  if (syrnck_system_vault_provider_json(vault, &result, &length) != 0 || !result || !strstr(result, "\"provider\":")) goto done;
  syrnck_string_free(result); result = NULL;
  if (syrnck_system_vault_close(vault) != 0) goto done;
  vault = NULL;
  if (syrnck_sealed_key_protect(NULL, 0, password, sizeof(password)-1, NULL,
        envelope, sizeof(envelope), &length) != 0 || length != sizeof(envelope)) goto done;
  if (syrnck_sealed_key_open(envelope, length, password, sizeof(password)-1, &key) != 0) goto done;
  if (syrnck_sealed_key_public_json(key, &result, &length) != 0 || !result || !strstr(result, "\"realm\":")) goto done;
  syrnck_string_free(result); result = NULL;
  if (syrnck_object_put(store, "chunk", (const unsigned char *)"SDK sealed payload", 18, id) != 0) goto done;
  if (snprintf(roots, sizeof(roots), "{\"sdk/object\":{\"kind\":\"chunk\",\"id\":\"%s\"}}", id) < 0) goto done;
  wire_path = (char *)malloc(strlen(argv[1])+6);
  if (!wire_path) goto done;
  strcpy(wire_path, argv[1]); strcat(wire_path, "-wire");
  if (syrnck_store_open(wire_path, true, NULL, 0, &wire) != 0) goto done;
  if (syrnck_sealed_snapshot_json_ex(store, wire, key, "sdk", (const unsigned char *)roots,
        strlen(roots), NULL, &limits, NULL, &result, &length) != 0 || !result ||
        !strstr(result, "\"state\":\"prepared\"") || !strstr(result, id)) goto done;
  status = 0;
done:
  if (status) fprintf(stderr, "%s\n", syrnck_last_error());
  syrnck_system_vault_close(vault);
  if (result) syrnck_string_free(result);
  syrnck_sealed_key_close(key);
  syrnck_store_close(wire);
  syrnck_store_close(store);
  free(wire_path);
  return status;
}
