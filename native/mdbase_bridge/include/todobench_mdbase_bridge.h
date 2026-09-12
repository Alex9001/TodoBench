// SPDX-License-Identifier: MIT
#pragma once
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct MdbaseCollection;

const char *todobench_mdbase_version(void);
void todobench_mdbase_free(char *ptr, size_t len);

// All out_json buffers are length-delimited UTF-8 (not NUL-terminated).
// Caller frees with todobench_mdbase_free. Return 0 on valid, 1 on
// diagnostics-error, 2 on panic/unwind.

int todobench_mdbase_collection_open(const char *root, size_t root_len,
                                     struct MdbaseCollection **out_handle,
                                     char **out_json, size_t *out_len);
void todobench_mdbase_collection_close(struct MdbaseCollection *handle);

int todobench_mdbase_inspect(struct MdbaseCollection *handle,
                             char **out_json, size_t *out_len);
int todobench_mdbase_validate(struct MdbaseCollection *handle,
                              const char *input, size_t input_len,
                              char **out_json, size_t *out_len);
int todobench_mdbase_read(struct MdbaseCollection *handle,
                          const char *input, size_t input_len,
                          char **out_json, size_t *out_len);
int todobench_mdbase_query(struct MdbaseCollection *handle,
                           const char *input, size_t input_len,
                           char **out_json, size_t *out_len);
int todobench_mdbase_get_types(struct MdbaseCollection *handle,
                               const char *input, size_t input_len,
                               char **out_json, size_t *out_len);
int todobench_mdbase_list_types(struct MdbaseCollection *handle,
                                char **out_json, size_t *out_len);
int todobench_mdbase_resolve_link(struct MdbaseCollection *handle,
                                  const char *input, size_t input_len,
                                  char **out_json, size_t *out_len);

#ifdef __cplusplus
}
#endif
