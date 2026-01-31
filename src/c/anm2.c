#include "anm2.h"

#include <ovarray.h>
#include <ovcyrb64.h>
#include <ovmo.h>
#include <ovprintf.h>
#include <ovprintf_ex.h>

#include <ovl/file.h>

#include "json.h"

// JSON metadata prefix/suffix
static char const json_prefix[] = "--[==[PTK:";
static char const json_suffix[] = "]==]";

static size_t const json_prefix_len = sizeof(json_prefix) - 1;

struct param {
  uint32_t id;
  uintptr_t userdata;
  char *key;
  char *value;
};

struct item {
  uint32_t id;
  uintptr_t userdata;
  char *script_name; // NULL for value items
  char *name;
  char *value;          // For value items
  struct param *params; // ovarray, for animation items
};

struct selector {
  uint32_t id;
  uintptr_t userdata;
  char *name;
  struct item *items; // ovarray
};

// enum ptk_anm2_op_type is now defined in anm2.h

struct ptk_anm2_op {
  uint32_t id;        // ID of affected element
  uint32_t parent_id; // Parent ID (selector for item, item for param)
  uint32_t before_id; // For insert/move: ID of element before which to insert (0=end)
  enum ptk_anm2_op_type type;
  char *str_data;
  void *removed_data;
};

struct ptk_anm2 {
  uint32_t next_id;
  int version;
  char *label;
  char *psd_path;
  char *information;              // NULL = auto-generate from psd_path
  char *default_character_id;     // Default character ID for multi-script format
  bool exclusive_support_default; // Default value for exclusive support control checkbox
  struct selector *selectors;     // ovarray
  struct ptk_anm2_op *undo_stack; // ovarray
  struct ptk_anm2_op *redo_stack; // ovarray
  int transaction_depth;
  uint64_t stored_checksum;     // checksum from JSON metadata (set by load)
  uint64_t calculated_checksum; // checksum calculated from script body (set by load)
  ptk_anm2_change_callback change_callback;
  void *change_callback_userdata;
  ptk_anm2_state_callback state_callback;
  void *state_callback_userdata;
  bool modified; // true if document has unsaved changes
};

static uint32_t generate_id(struct ptk_anm2 *const doc) { return doc->next_id++; }

static void param_free(struct param *p) {
  if (!p) {
    return;
  }
  if (p->key) {
    OV_ARRAY_DESTROY(&p->key);
  }
  if (p->value) {
    OV_ARRAY_DESTROY(&p->value);
  }
}

static void item_free(struct item *it) {
  if (!it) {
    return;
  }
  if (it->script_name) {
    OV_ARRAY_DESTROY(&it->script_name);
  }
  if (it->name) {
    OV_ARRAY_DESTROY(&it->name);
  }
  if (it->value) {
    OV_ARRAY_DESTROY(&it->value);
  }
  if (it->params) {
    size_t const n = OV_ARRAY_LENGTH(it->params);
    for (size_t i = 0; i < n; i++) {
      param_free(&it->params[i]);
    }
    OV_ARRAY_DESTROY(&it->params);
  }
}

static void selector_free(struct selector *sel) {
  if (!sel) {
    return;
  }
  if (sel->name) {
    OV_ARRAY_DESTROY(&sel->name);
  }
  if (sel->items) {
    size_t const n = OV_ARRAY_LENGTH(sel->items);
    for (size_t i = 0; i < n; i++) {
      item_free(&sel->items[i]);
    }
    OV_ARRAY_DESTROY(&sel->items);
  }
}

static void op_free(struct ptk_anm2_op *op) {
  if (!op) {
    return;
  }
  if (op->str_data) {
    OV_ARRAY_DESTROY(&op->str_data);
  }
  if (op->removed_data) {
    switch (op->type) {
    case ptk_anm2_op_selector_remove:
      selector_free((struct selector *)op->removed_data);
      OV_FREE(&op->removed_data);
      break;
    case ptk_anm2_op_selector_insert:
      // INSERT ops also store selector data in removed_data
      selector_free((struct selector *)op->removed_data);
      OV_FREE(&op->removed_data);
      break;
    case ptk_anm2_op_item_remove:
      item_free((struct item *)op->removed_data);
      OV_FREE(&op->removed_data);
      break;
    case ptk_anm2_op_item_insert:
      // INSERT ops also store item data in removed_data
      item_free((struct item *)op->removed_data);
      OV_FREE(&op->removed_data);
      break;
    case ptk_anm2_op_param_remove:
      param_free((struct param *)op->removed_data);
      OV_FREE(&op->removed_data);
      break;
    case ptk_anm2_op_param_insert:
      // INSERT ops also store param data in removed_data
      param_free((struct param *)op->removed_data);
      OV_FREE(&op->removed_data);
      break;
    case ptk_anm2_op_reset:
    case ptk_anm2_op_transaction_begin:
    case ptk_anm2_op_transaction_end:
    case ptk_anm2_op_set_label:
    case ptk_anm2_op_set_psd_path:
    case ptk_anm2_op_set_exclusive_support_default:
    case ptk_anm2_op_set_information:
    case ptk_anm2_op_set_default_character_id:
    case ptk_anm2_op_selector_set_name:
    case ptk_anm2_op_selector_move:
    case ptk_anm2_op_item_set_name:
    case ptk_anm2_op_item_set_value:
    case ptk_anm2_op_item_set_script_name:
    case ptk_anm2_op_item_move:
    case ptk_anm2_op_param_set_key:
    case ptk_anm2_op_param_set_value:
      // These operations don't have removed_data that needs special handling
      OV_FREE(&op->removed_data);
      break;
    }
  }
}

static void op_stack_clear(struct ptk_anm2_op **stack) {
  if (!stack || !*stack) {
    return;
  }
  size_t const n = OV_ARRAY_LENGTH(*stack);
  for (size_t i = 0; i < n; i++) {
    op_free(&(*stack)[i]);
  }
  OV_ARRAY_DESTROY(stack);
}

static bool strdup_to_array(char **dest, char const *src, struct ov_error *const err) {
  if (!dest) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_invalid_argument);
    return false;
  }
  if (*dest) {
    OV_ARRAY_DESTROY(dest);
  }
  if (!src || src[0] == '\0') {
    *dest = NULL;
    return true;
  }
  size_t const len = strlen(src);
  if (!OV_ARRAY_GROW(dest, len + 1)) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_out_of_memory);
    return false;
  }
  memcpy(*dest, src, len + 1);
  OV_ARRAY_SET_LENGTH(*dest, len + 1);
  return true;
}

// Get before_id for selector at position idx (0 if at end)
static uint32_t get_selector_before_id(struct ptk_anm2 *doc, size_t idx) {
  size_t const n = OV_ARRAY_LENGTH(doc->selectors);
  if (idx + 1 >= n) {
    return 0;
  }
  return doc->selectors[idx + 1].id;
}

// Get before_id for item at position idx within selector (0 if at end)
static uint32_t get_item_before_id(struct selector *sel, size_t idx) {
  size_t const n = OV_ARRAY_LENGTH(sel->items);
  if (idx + 1 >= n) {
    return 0;
  }
  return sel->items[idx + 1].id;
}

// Get before_id for param at position idx within item (0 if at end)
static uint32_t get_param_before_id(struct item *it, size_t idx) {
  size_t const n = OV_ARRAY_LENGTH(it->params);
  if (idx + 1 >= n) {
    return 0;
  }
  return it->params[idx + 1].id;
}

static void notify_change(
    struct ptk_anm2 *doc, enum ptk_anm2_op_type op_type, uint32_t id, uint32_t parent_id, uint32_t before_id) {
  if (!doc) {
    return;
  }
  if (op_type != ptk_anm2_op_reset) {
    doc->modified = true;
  }
  if (doc->change_callback) {
    doc->change_callback(doc->change_callback_userdata, op_type, id, parent_id, before_id);
  }
}

void ptk_anm2_set_change_callback(struct ptk_anm2 *doc, ptk_anm2_change_callback callback, void *userdata) {
  if (!doc) {
    return;
  }
  doc->change_callback = callback;
  doc->change_callback_userdata = userdata;
}

void ptk_anm2_set_state_callback(struct ptk_anm2 *doc, ptk_anm2_state_callback callback, void *userdata) {
  if (!doc) {
    return;
  }
  doc->state_callback = callback;
  doc->state_callback_userdata = userdata;
}

static void notify_state(struct ptk_anm2 const *doc) {
  if (doc && doc->state_callback) {
    doc->state_callback(doc->state_callback_userdata);
  }
}

static uint64_t calculate_checksum(char const *const script_body, size_t const body_len) {
  struct ov_cyrb64 ctx;
  size_t word_len = 0;
  uint32_t *aligned_buf = NULL;
  uint64_t result = 0;
  bool success = false;

  if (!script_body || body_len == 0) {
    goto cleanup;
  }

  // Allocate aligned buffer for cyrb64
  word_len = (body_len + 3) / 4;
  if (!OV_REALLOC(&aligned_buf, word_len, sizeof(uint32_t))) {
    goto cleanup;
  }
  memset(aligned_buf, 0, word_len * sizeof(uint32_t));
  memcpy(aligned_buf, script_body, body_len);

  ov_cyrb64_init(&ctx, 0);
  ov_cyrb64_update(&ctx, aligned_buf, word_len);
  result = ov_cyrb64_final(&ctx);

  success = true;

cleanup:
  if (aligned_buf) {
    OV_FREE(&aligned_buf);
  }
  if (!success) {
    return 0;
  }
  return result;
}

static bool escape_lua_string(char **const dest, char const *const src, struct ov_error *const err) {
  size_t src_len = 0;
  size_t j = 0;

  src_len = strlen(src);
  if (!OV_ARRAY_GROW(dest, src_len * 2 + 3)) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_out_of_memory);
    return false;
  }

  j = 0;
  (*dest)[j++] = '"';
  for (size_t i = 0; i < src_len; i++) {
    char const c = src[i];
    char escaped = 0;
    switch (c) {
    case '\a':
      escaped = 'a';
      break;
    case '\b':
      escaped = 'b';
      break;
    case '\t':
      escaped = 't';
      break;
    case '\n':
      escaped = 'n';
      break;
    case '\v':
      escaped = 'v';
      break;
    case '\f':
      escaped = 'f';
      break;
    case '\r':
      escaped = 'r';
      break;
    case '"':
      escaped = '"';
      break;
    case '\'':
      escaped = '\'';
      break;
    case '\\':
      escaped = '\\';
      break;
    default:
      (*dest)[j++] = c;
      continue;
    }
    (*dest)[j++] = '\\';
    (*dest)[j++] = escaped;
  }
  (*dest)[j++] = '"';
  (*dest)[j] = '\0';
  OV_ARRAY_SET_LENGTH(*dest, j + 1);
  return true;
}

// Sanitize display name for --select@ format
// Replaces '=' with '＝' (U+FF1D) and ',' with '，' (U+FF0C)
// to avoid breaking the selector syntax.
// The dest buffer is reused; caller must destroy it after all calls.
// OV_ARRAY_LENGTH returns the string length (not including NUL terminator).
static bool sanitize_selector_name(char **const dest, char const *const src, struct ov_error *const err) {
  OV_ARRAY_SET_LENGTH(*dest, 0);
  if (!src || src[0] == '\0') {
    return true;
  }

  size_t const src_len = strlen(src);
  size_t j = 0;
  for (size_t i = 0; i < src_len; i++) {
    unsigned char const c = (unsigned char)src[i];
    size_t const need = (c == '=' || c == ',') ? 3 : 1;
    if (j + need + 1 > OV_ARRAY_CAPACITY(*dest)) {
      size_t const new_cap = (OV_ARRAY_CAPACITY(*dest) + need + 1) * 2;
      if (!OV_ARRAY_GROW(dest, new_cap)) {
        OV_ERROR_SET_GENERIC(err, ov_error_generic_out_of_memory);
        return false;
      }
    }
    if (c == '=') {
      // '=' -> '＝' (U+FF1D: 0xEF 0xBC 0x9D)
      (*dest)[j++] = (char)0xEF;
      (*dest)[j++] = (char)0xBC;
      (*dest)[j++] = (char)0x9D;
    } else if (c == ',') {
      // ',' -> '，' (U+FF0C: 0xEF 0xBC 0x8C)
      (*dest)[j++] = (char)0xEF;
      (*dest)[j++] = (char)0xBC;
      (*dest)[j++] = (char)0x8C;
    } else {
      (*dest)[j++] = (char)c;
    }
  }
  (*dest)[j] = '\0';
  OV_ARRAY_SET_LENGTH(*dest, j);
  return true;
}

// Helper function for generating a single param line
// escaped is reused buffer, caller must destroy it after all calls
static bool generate_param_line(char **const content,
                                struct param const *const param,
                                char **const escaped,
                                struct ov_error *const err) {
  // Escape key for Lua (use ["key"] syntax for safety with special chars)
  if (!escape_lua_string(escaped, param->key ? param->key : "", err)) {
    OV_ERROR_ADD_TRACE(err);
    return false;
  }
  if (!ov_sprintf_append_char(content, err, "%1$s", "    [%1$s] = ", *escaped)) {
    OV_ERROR_ADD_TRACE(err);
    return false;
  }

  // Escape value for Lua (reuse buffer)
  if (!escape_lua_string(escaped, param->value ? param->value : "", err)) {
    OV_ERROR_ADD_TRACE(err);
    return false;
  }
  if (!ov_sprintf_append_char(content, err, "%1$s", "%1$s,\n", *escaped)) {
    OV_ERROR_ADD_TRACE(err);
    return false;
  }

  return true;
}

// Generate animation code from script_name and params
// Output format: require("script_name").new({ ["key"] = "value", ... }),
static bool generate_animation_code(char **const content, struct item const *const item, struct ov_error *const err) {
  char *escaped = NULL;
  bool success = false;

  // require("script_name").new({
  if (!ov_sprintf_append_char(content, err, "%1$s", "  require(\"%1$s\").new({\n", item->script_name)) {
    OV_ERROR_ADD_TRACE(err);
    goto cleanup;
  }

  // Output params as key-value pairs
  {
    size_t const params_len = OV_ARRAY_LENGTH(item->params);
    for (size_t i = 0; i < params_len; i++) {
      if (!generate_param_line(content, &item->params[i], &escaped, err)) {
        OV_ERROR_ADD_TRACE(err);
        goto cleanup;
      }
    }
  }

  // Close the table and function call
  if (!ov_sprintf_append_char(content, err, NULL, "  }),\n")) {
    OV_ERROR_ADD_TRACE(err);
    goto cleanup;
  }

  success = true;

cleanup:
  if (escaped) {
    OV_ARRAY_DESTROY(&escaped);
  }
  return success;
}

static bool generate_json_line(char **const content,
                               struct ptk_anm2 const *const doc,
                               uint64_t const checksum,
                               struct ov_error *const err) {
  yyjson_mut_doc *jdoc = NULL;
  yyjson_mut_val *root = NULL;
  char *json_str = NULL;
  bool success = false;

  jdoc = yyjson_mut_doc_new(ptk_json_get_alc());
  if (!jdoc) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_out_of_memory);
    goto cleanup;
  }

  root = yyjson_mut_obj(jdoc);
  yyjson_mut_doc_set_root(jdoc, root);

  // version
  yyjson_mut_obj_add_int(jdoc, root, "version", doc->version);

  // checksum (as hex string)
  {
    char checksum_str[17];
    ov_snprintf_char(checksum_str, sizeof(checksum_str), "%016llx", "%016llx", (unsigned long long)checksum);
    yyjson_mut_obj_add_strcpy(jdoc, root, "checksum", checksum_str);
  }

  // selectors
  {
    yyjson_mut_val *selectors = yyjson_mut_arr(jdoc);
    size_t const selectors_len = OV_ARRAY_LENGTH(doc->selectors);
    for (size_t i = 0; i < selectors_len; i++) {
      struct selector const *const sel = &doc->selectors[i];
      yyjson_mut_val *sel_obj = yyjson_mut_obj(jdoc);
      yyjson_mut_obj_add_strcpy(jdoc, sel_obj, "group", sel->name);

      yyjson_mut_val *items = yyjson_mut_arr(jdoc);
      size_t const items_len = OV_ARRAY_LENGTH(sel->items);
      for (size_t j = 0; j < items_len; j++) {
        struct item const *const item = &sel->items[j];

        if (item->script_name) {
          // Animation item: {script: "name", n: "display name", params: [[key, value], ...]}
          yyjson_mut_val *item_obj = yyjson_mut_obj(jdoc);
          yyjson_mut_obj_add_strcpy(jdoc, item_obj, "script", item->script_name);
          if (item->name) {
            yyjson_mut_obj_add_strcpy(jdoc, item_obj, "n", item->name);
          }

          yyjson_mut_val *params_arr = yyjson_mut_arr(jdoc);
          size_t const params_len = OV_ARRAY_LENGTH(item->params);
          for (size_t k = 0; k < params_len; k++) {
            struct param const *const p = &item->params[k];
            yyjson_mut_val *param_tuple = yyjson_mut_arr(jdoc);
            yyjson_mut_arr_add_strcpy(jdoc, param_tuple, p->key ? p->key : "");
            yyjson_mut_arr_add_strcpy(jdoc, param_tuple, p->value ? p->value : "");
            yyjson_mut_arr_add_val(params_arr, param_tuple);
          }
          yyjson_mut_obj_add_val(jdoc, item_obj, "params", params_arr);
          yyjson_mut_arr_add_val(items, item_obj);
        } else {
          // Value item: [name, value]
          yyjson_mut_val *item_arr = yyjson_mut_arr(jdoc);
          yyjson_mut_arr_add_strcpy(jdoc, item_arr, item->name);
          yyjson_mut_arr_add_strcpy(jdoc, item_arr, item->value);
          yyjson_mut_arr_add_val(items, item_arr);
        }
      }
      yyjson_mut_obj_add_val(jdoc, sel_obj, "items", items);
      yyjson_mut_arr_append(selectors, sel_obj);
    }
    yyjson_mut_obj_add_val(jdoc, root, "selectors", selectors);
  }

  // psd path (required)
  if (doc->psd_path) {
    yyjson_mut_obj_add_strcpy(jdoc, root, "psd", doc->psd_path);
  }

  // label
  if (doc->label && doc->label[0] != '\0') {
    yyjson_mut_obj_add_strcpy(jdoc, root, "label", doc->label);
  }

  // exclusive_support_default (only store if false, true is the default)
  if (!doc->exclusive_support_default) {
    yyjson_mut_obj_add_bool(jdoc, root, "exclusive_support_default", doc->exclusive_support_default);
  }

  // information (only store if custom, NULL means auto-generate)
  if (doc->information && doc->information[0] != '\0') {
    yyjson_mut_obj_add_strcpy(jdoc, root, "information", doc->information);
  }

  // default_character_id (only store if set)
  if (doc->default_character_id && doc->default_character_id[0] != '\0') {
    yyjson_mut_obj_add_strcpy(jdoc, root, "defaultCharacterId", doc->default_character_id);
  }

  // Write JSON to string using custom allocator
  json_str = yyjson_mut_write_opts(jdoc, 0, ptk_json_get_alc(), NULL, NULL);
  if (!json_str) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_fail);
    goto cleanup;
  }

  // Check for dangerous sequence that would break Lua comment syntax
  // The JSON is wrapped in --[==[PTK:...]==], so ]==] in the content would break parsing
  if (strstr(json_str, json_suffix) != NULL) {
    OV_ERROR_SET(err,
                 ov_error_type_generic,
                 ov_error_generic_fail,
                 gettext("Layer name or value contains forbidden character sequence \"]==]\"."));
    goto cleanup;
  }

  // Format: --[==[PTK:{json}]==]\n
  if (!ov_sprintf_append_char(content, err, "%1$s%2$s%3$s", "%1$s%2$s%3$s\n", json_prefix, json_str, json_suffix)) {
    OV_ERROR_ADD_TRACE(err);
    goto cleanup;
  }

  success = true;

cleanup:
  if (json_str) {
    ptk_json_get_alc()->free(NULL, json_str);
  }
  if (jdoc) {
    yyjson_mut_doc_free(jdoc);
  }
  return success;
}

static bool
generate_script_content(struct ptk_anm2 const *const doc, char **const content, struct ov_error *const err) {
  char *escaped = NULL;
  char *sanitized = NULL;
  char *body = NULL;
  bool success = false;

  // --label: line (only if label is set and not empty)
  if (doc->label && doc->label[0] != '\0') {
    if (!ov_sprintf_append_char(&body, err, "%1$hs", "--label:%1$hs\n", doc->label)) {
      OV_ERROR_ADD_TRACE(err);
      goto cleanup;
    }
  }

  // --information: line
  {
    if (doc->information && doc->information[0] != '\0') {
      // Use custom information text
      if (!ov_sprintf_append_char(&body, err, "%1$s", "--information:%1$s\n", doc->information)) {
        OV_ERROR_ADD_TRACE(err);
        goto cleanup;
      }
    } else {
      // Auto-generate from PSD filename
      // Extract filename from path (find last / or \)
      char const *filename = doc->psd_path ? doc->psd_path : "";
      if (doc->psd_path) {
        for (char const *p = doc->psd_path; *p; p++) {
          if (*p == '/' || *p == '\\') {
            filename = p + 1;
          }
        }
      }
      if (filename && filename[0] != '\0') {
        char info[256];
        ov_snprintf_char(info, sizeof(info), "%1$hs", pgettext(".ptk.anm2", "PSD Layer Selector for %1$hs"), filename);
        if (!ov_sprintf_append_char(&body, err, "%1$hs", "--information:%1$hs\n", info)) {
          OV_ERROR_ADD_TRACE(err);
          goto cleanup;
        }
      }
    }
  }

  // --check@exclusive: line for exclusive support
  {
    if (!ov_sprintf_append_char(&body,
                                err,
                                "%1$hs%2$d",
                                "--check@exclusive:%1$hs,%2$d\n",
                                pgettext(".ptk.anm2", "Exclusive Support"),
                                doc->exclusive_support_default ? 1 : 0)) {
      OV_ERROR_ADD_TRACE(err);
      goto cleanup;
    }
  }

  // --select@ lines for each selector (all items have names)
  // Skip selectors with no items to avoid AviUtl crash
  {
    size_t const selectors_len = OV_ARRAY_LENGTH(doc->selectors);
    for (size_t i = 0; i < selectors_len; i++) {
      struct selector const *const sel = &doc->selectors[i];
      size_t const items_len = OV_ARRAY_LENGTH(sel->items);

      // Skip empty selectors - AviUtl crashes on --select@ with no items
      if (items_len == 0) {
        continue;
      }

      // Variable name is auto-generated as sel1, sel2, etc.
      // Use fallback name if group is NULL (should not happen, but safety measure)
      char const *const group_name =
          sel->name ? sel->name : pgettext(".ptk.anm2 default name for unnamed selector", "Selector");
      if (!ov_sprintf_append_char(&body, err, "%1$zu%2$hs", "--select@sel%1$zu:%2$hs", i + 1, group_name)) {
        OV_ERROR_ADD_TRACE(err);
        goto cleanup;
      }

      // Insert a "(None)" option as the first item for selectors
      if (!ov_sprintf_append_char(
              &body, err, "%1$hs", ",%1$hs=0", pgettext(".ptk.anm2 Unselected item name for selector", "(None)"))) {
        OV_ERROR_ADD_TRACE(err);
        goto cleanup;
      }

      for (size_t j = 0; j < items_len; j++) {
        struct item const *const item = &sel->items[j];
        // Use name for all items; for animation items, use script_name if name is not set
        char const *display_name = item->name;
        if (!display_name || display_name[0] == '\0') {
          display_name = item->script_name;
        }
        if (display_name && display_name[0] != '\0') {
          if (!sanitize_selector_name(&sanitized, display_name, err)) {
            OV_ERROR_ADD_TRACE(err);
            goto cleanup;
          }
          if (!ov_sprintf_append_char(&body, err, "%1$s%2$zu", ",%1$s=%2$zu", sanitized, j + 1)) {
            OV_ERROR_ADD_TRACE(err);
            goto cleanup;
          }
        }
      }

      if (!ov_sprintf_append_char(&body, err, NULL, "\n")) {
        OV_ERROR_ADD_TRACE(err);
        goto cleanup;
      }
    }
  }

  // Generate cached add_layer_selector for each selector wrapped in psdcall
  {
    size_t cache_index = 0;
    size_t const selectors_len = OV_ARRAY_LENGTH(doc->selectors);

    // Check if there are any non-empty selectors to determine if we need psdcall wrapper
    bool has_selectors = false;
    for (size_t i = 0; i < selectors_len; i++) {
      if (OV_ARRAY_LENGTH(doc->selectors[i].items) > 0) {
        has_selectors = true;
        break;
      }
    }

    // Only wrap with psdcall if there are selectors to generate
    if (has_selectors) {
      if (!ov_sprintf_append_char(&body, err, NULL, "require(\"PSDToolKit\").psdcall(function()\n")) {
        OV_ERROR_ADD_TRACE(err);
        goto cleanup;
      }
    }

    for (size_t i = 0; i < selectors_len; i++) {
      struct selector const *const sel = &doc->selectors[i];
      size_t const items_len = OV_ARRAY_LENGTH(sel->items);

      // Skip empty selectors
      if (items_len == 0) {
        continue;
      }

      cache_index++;

      // require("PSDToolKit").add_layer_selector(N, function() return {
      if (!ov_sprintf_append_char(&body,
                                  err,
                                  "%1$zu",
                                  "require(\"PSDToolKit\").add_layer_selector(%1$zu, function() return {\n",
                                  cache_index)) {
        OV_ERROR_ADD_TRACE(err);
        goto cleanup;
      }

      // Generate items for this selector
      for (size_t j = 0; j < items_len; j++) {
        struct item const *const item = &sel->items[j];
        if (item->script_name) {
          // Animation item
          if (!generate_animation_code(&body, item, err)) {
            OV_ERROR_ADD_TRACE(err);
            goto cleanup;
          }
        } else {
          // Value item
          if (!escape_lua_string(&escaped, item->value ? item->value : "", err)) {
            OV_ERROR_ADD_TRACE(err);
            goto cleanup;
          }
          if (!ov_sprintf_append_char(&body, err, "%1$s", "  %1$s,\n", escaped)) {
            OV_ERROR_ADD_TRACE(err);
            goto cleanup;
          }
          OV_ARRAY_DESTROY(&escaped);
        }
      }

      // } end, selN, {exclusive = exclusive ~= 0})
      if (!ov_sprintf_append_char(&body, err, "%1$zu", "} end, sel%1$zu, {exclusive = exclusive ~= 0})\n", i + 1)) {
        OV_ERROR_ADD_TRACE(err);
        goto cleanup;
      }
    }

    // Close psdcall wrapper
    if (has_selectors) {
      if (!ov_sprintf_append_char(&body, err, NULL, "end)\n")) {
        OV_ERROR_ADD_TRACE(err);
        goto cleanup;
      }
    }
  }

  // Now calculate checksum and create JSON header with checksum
  {
    size_t body_len = OV_ARRAY_LENGTH(body);
    uint64_t checksum = calculate_checksum(body, body_len);

    // Generate JSON line with checksum
    if (!generate_json_line(content, doc, checksum, err)) {
      OV_ERROR_ADD_TRACE(err);
      goto cleanup;
    }
  }

  // Append body to content
  if (body) {
    size_t body_len = OV_ARRAY_LENGTH(body);
    size_t content_len = OV_ARRAY_LENGTH(*content);
    if (!OV_ARRAY_GROW(content, content_len + body_len + 1)) {
      OV_ERROR_SET_GENERIC(err, ov_error_generic_out_of_memory);
      goto cleanup;
    }
    memcpy(*content + content_len, body, body_len + 1);
    OV_ARRAY_SET_LENGTH(*content, content_len + body_len);
  }

  success = true;

cleanup:
  if (escaped) {
    OV_ARRAY_DESTROY(&escaped);
  }
  if (sanitized) {
    OV_ARRAY_DESTROY(&sanitized);
  }
  if (body) {
    OV_ARRAY_DESTROY(&body);
  }
  if (!success) {
    if (*content) {
      OV_ARRAY_DESTROY(content);
    }
  }
  return success;
}

static bool
generate_parts_override_script(struct ptk_anm2 const *const doc, char **const content, struct ov_error *const err) {
  char *sanitized = NULL;
  bool success = false;

  // Note: Header (@OverwriteSelector) is added by caller (generate_obj2_content)

  // --label: line (only if label is set and not empty)
  if (doc->label && doc->label[0] != '\0') {
    if (!ov_sprintf_append_char(content, err, "%1$hs", "--label:%1$hs\n", doc->label)) {
      OV_ERROR_ADD_TRACE(err);
      goto cleanup;
    }
  }

  // --information: line
  {
    if (doc->information && doc->information[0] != '\0') {
      // Use custom information text
      if (!ov_sprintf_append_char(content, err, "%1$s", "--information:%1$s\n", doc->information)) {
        OV_ERROR_ADD_TRACE(err);
        goto cleanup;
      }
    } else {
      // Auto-generate from PSD filename
      // Extract filename from path (find last / or \)
      char const *filename = doc->psd_path ? doc->psd_path : "";
      if (doc->psd_path) {
        for (char const *p = doc->psd_path; *p; p++) {
          if (*p == '/' || *p == '\\') {
            filename = p + 1;
          }
        }
      }
      if (filename && filename[0] != '\0') {
        char info[256];
        ov_snprintf_char(info,
                         sizeof(info),
                         "%1$hs",
                         pgettext(".ptk.anm2 OverwriteSelector", "PSD Layer Selector for %1$hs"),
                         filename);
        if (!ov_sprintf_append_char(content, err, "%1$hs", "--information:%1$hs\n", info)) {
          OV_ERROR_ADD_TRACE(err);
          goto cleanup;
        }
      }
    }
  }

  // --value@id: line with default character ID
  {
    char const *const char_id = doc->default_character_id ? doc->default_character_id : "";
    if (!ov_sprintf_append_char(content,
                                err,
                                "%1$hs%2$s",
                                "--value@id:%1$hs,\"%2$s\"\n",
                                pgettext(".ptk.anm2 OverwriteSelector", "Character ID"),
                                char_id)) {
      OV_ERROR_ADD_TRACE(err);
      goto cleanup;
    }
  }

  // --select@pN: lines for each non-empty selector (max 16)
  {
    size_t const selectors_len = OV_ARRAY_LENGTH(doc->selectors);
    size_t part_num = 0;
    for (size_t i = 0; i < selectors_len && part_num < 16; i++) {
      struct selector const *const sel = &doc->selectors[i];
      size_t const items_len = OV_ARRAY_LENGTH(sel->items);

      if (items_len == 0) {
        continue;
      }

      part_num++;

      // --select@pN:SelectorName,(None)=0,Item1=1,Item2=2,...
      char const *const sel_name =
          sel->name ? sel->name : pgettext(".ptk.anm2 default name for unnamed selector", "Selector");
      if (!ov_sprintf_append_char(content, err, "%1$zu%2$hs", "--select@p%1$zu:%2$hs", part_num, sel_name)) {
        OV_ERROR_ADD_TRACE(err);
        goto cleanup;
      }

      // (None)=0 as first option
      if (!ov_sprintf_append_char(
              content, err, "%1$hs", ",%1$hs=0", pgettext(".ptk.anm2 Unselected item name for selector", "(None)"))) {
        OV_ERROR_ADD_TRACE(err);
        goto cleanup;
      }

      // Item options
      for (size_t j = 0; j < items_len; j++) {
        struct item const *const item = &sel->items[j];
        char const *display_name = item->name;
        if (!display_name || display_name[0] == '\0') {
          display_name = item->script_name;
        }
        if (display_name && display_name[0] != '\0') {
          if (!sanitize_selector_name(&sanitized, display_name, err)) {
            OV_ERROR_ADD_TRACE(err);
            goto cleanup;
          }
          if (!ov_sprintf_append_char(content, err, "%1$s%2$zu", ",%1$s=%2$zu", sanitized, j + 1)) {
            OV_ERROR_ADD_TRACE(err);
            goto cleanup;
          }
        }
      }

      if (!ov_sprintf_append_char(content, err, NULL, "\n")) {
        OV_ERROR_ADD_TRACE(err);
        goto cleanup;
      }
    }
  }

  // Generate Lua code for set_layer_selector_overwriter
  {
    if (!ov_sprintf_append_char(
            content,
            err,
            NULL,
            "require(\"PSDToolKit\").psdcall(function()\n"
            "  require(\"PSDToolKit\").set_layer_selector_overwriter(id ~= \"\" and id or nil, {\n")) {
      OV_ERROR_ADD_TRACE(err);
      goto cleanup;
    }

    // p1 = p1 ~= 0 and p1 or nil, ...
    size_t const selectors_len = OV_ARRAY_LENGTH(doc->selectors);
    size_t part_num = 0;
    for (size_t i = 0; i < selectors_len && part_num < 16; i++) {
      if (OV_ARRAY_LENGTH(doc->selectors[i].items) == 0) {
        continue;
      }
      part_num++;
      if (!ov_sprintf_append_char(
              content, err, "%1$zu%2$zu", "    p%1$zu = p%2$zu ~= 0 and p%2$zu or nil,\n", part_num, part_num)) {
        OV_ERROR_ADD_TRACE(err);
        goto cleanup;
      }
    }

    if (!ov_sprintf_append_char(content, err, NULL, "  }, obj)\nend)\n")) {
      OV_ERROR_ADD_TRACE(err);
      goto cleanup;
    }
  }

  success = true;

cleanup:
  if (sanitized) {
    OV_ARRAY_DESTROY(&sanitized);
  }
  return success;
}

static bool
generate_multiscript_content(struct ptk_anm2 const *const doc, char **const content, struct ov_error *const err) {
  char *single_content = NULL;
  bool success = false;

  // Generate single-script content first
  if (!generate_script_content(doc, &single_content, err)) {
    OV_ERROR_ADD_TRACE(err);
    goto cleanup;
  }

  // Add @Selector header before the single script content
  if (!ov_sprintf_append_char(
          content, err, "%1$hs", "@%1$hs\n", pgettext(".ptk.anm2 multi-script section name", "Selector"))) {
    OV_ERROR_ADD_TRACE(err);
    goto cleanup;
  }

  // Append single script content
  {
    size_t const single_len = OV_ARRAY_LENGTH(single_content);
    size_t const content_len = OV_ARRAY_LENGTH(*content);
    if (!OV_ARRAY_GROW(content, content_len + single_len + 1)) {
      OV_ERROR_SET_GENERIC(err, ov_error_generic_out_of_memory);
      goto cleanup;
    }
    memcpy(*content + content_len, single_content, single_len + 1);
    OV_ARRAY_SET_LENGTH(*content, content_len + single_len);
  }

  // Note: Parts override script is now generated in a separate .obj2 file

  success = true;

cleanup:
  if (single_content) {
    OV_ARRAY_DESTROY(&single_content);
  }
  return success;
}

static bool generate_obj2_content(struct ptk_anm2 const *const doc, char **const content, struct ov_error *const err) {
  bool success = false;

  // Add @OverwriteSelector header
  if (!ov_sprintf_append_char(
          content, err, "%1$hs", "@%1$hs\n", pgettext(".ptk.anm2 multi-script section name", "OverwriteSelector"))) {
    OV_ERROR_ADD_TRACE(err);
    goto cleanup;
  }

  // Generate JSON metadata line (same as in main script)
  // For obj2, we still need the JSON metadata so the editor can load it
  {
    // Calculate a dummy checksum (obj2 files don't need checksum verification)
    // We use 0 as the checksum since the obj2 content is auto-generated
    if (!generate_json_line(content, doc, 0, err)) {
      OV_ERROR_ADD_TRACE(err);
      goto cleanup;
    }
  }

  // Generate parts override script content
  if (!generate_parts_override_script(doc, content, err)) {
    OV_ERROR_ADD_TRACE(err);
    goto cleanup;
  }

  success = true;

cleanup:
  return success;
}
// Cleanup document contents (does not free the struct itself)
static void doc_cleanup(struct ptk_anm2 *doc) {
  if (!doc) {
    return;
  }
  if (doc->label) {
    OV_ARRAY_DESTROY(&doc->label);
  }
  if (doc->psd_path) {
    OV_ARRAY_DESTROY(&doc->psd_path);
  }
  if (doc->information) {
    OV_ARRAY_DESTROY(&doc->information);
  }
  if (doc->default_character_id) {
    OV_ARRAY_DESTROY(&doc->default_character_id);
  }
  if (doc->selectors) {
    size_t const n = OV_ARRAY_LENGTH(doc->selectors);
    for (size_t i = 0; i < n; i++) {
      selector_free(&doc->selectors[i]);
    }
    OV_ARRAY_DESTROY(&doc->selectors);
  }
  op_stack_clear(&doc->undo_stack);
  op_stack_clear(&doc->redo_stack);
}

bool ptk_anm2_reset(struct ptk_anm2 *doc, struct ov_error *const err) {
  if (!doc) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_invalid_argument);
    return false;
  }

  bool success = false;

  // Save callbacks before cleanup
  ptk_anm2_change_callback const cb = doc->change_callback;
  void *const cb_userdata = doc->change_callback_userdata;
  ptk_anm2_state_callback const state_cb = doc->state_callback;
  void *const state_cb_userdata = doc->state_callback_userdata;

  // Clean up document contents
  doc_cleanup(doc);

  // Initialize as empty document
  *doc = (struct ptk_anm2){
      .version = 1,
      .next_id = 1,
      .exclusive_support_default = true,
      .change_callback = cb,
      .change_callback_userdata = cb_userdata,
      .state_callback = state_cb,
      .state_callback_userdata = state_cb_userdata,
  };
  if (!strdup_to_array(&doc->label, pgettext(".ptk.anm2 label", "PSD"), err)) {
    OV_ERROR_ADD_TRACE(err);
    goto cleanup;
  }

  // Notify change
  notify_change(doc, ptk_anm2_op_reset, 0, 0, 0);
  notify_state(doc);

  success = true;

cleanup:
  return success;
}

struct ptk_anm2 *ptk_anm2_create(struct ov_error *const err) {
  struct ptk_anm2 *doc = NULL;
  bool success = false;

  if (!OV_REALLOC(&doc, 1, sizeof(struct ptk_anm2))) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_out_of_memory);
    goto cleanup;
  }
  *doc = (struct ptk_anm2){0};

  if (!ptk_anm2_reset(doc, err)) {
    OV_ERROR_ADD_TRACE(err);
    goto cleanup;
  }

  success = true;

cleanup:
  if (!success) {
    if (doc) {
      OV_FREE(&doc);
    }
    return NULL;
  }
  return doc;
}

void ptk_anm2_destroy(struct ptk_anm2 **doc) {
  if (!doc || !*doc) {
    return;
  }
  doc_cleanup(*doc);
  OV_FREE(doc);
}

char const *ptk_anm2_get_label(struct ptk_anm2 const *doc) {
  if (!doc) {
    return NULL;
  }
  return doc->label;
}

static bool push_undo_op(struct ptk_anm2 *doc, struct ptk_anm2_op const *op, struct ov_error *const err) {
  bool success = false;

  size_t const len = OV_ARRAY_LENGTH(doc->undo_stack);
  if (!OV_ARRAY_GROW(&doc->undo_stack, len + 1)) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_out_of_memory);
    goto cleanup;
  }
  doc->undo_stack[len] = *op;
  OV_ARRAY_SET_LENGTH(doc->undo_stack, len + 1);

  success = true;

cleanup:
  return success;
}

static void clear_redo_stack(struct ptk_anm2 *doc) { op_stack_clear(&doc->redo_stack); }

// Apply a single operation (used for redo)
// Returns the reverse operation via reverse_op (caller takes ownership of allocated fields)
// NOTE: This function may consume op->removed_data (sets it to NULL after use)
static bool
apply_op(struct ptk_anm2 *doc, struct ptk_anm2_op *op, struct ptk_anm2_op *reverse_op, struct ov_error *const err) {
  bool success = false;

  *reverse_op = (struct ptk_anm2_op){.type = op->type};

  switch (op->type) {
  case ptk_anm2_op_set_label:
    // Save current value as reverse
    if (!strdup_to_array(&reverse_op->str_data, doc->label, err)) {
      OV_ERROR_ADD_TRACE(err);
      goto cleanup;
    }
    // Apply new value (op->str_data is the value to apply)
    if (!strdup_to_array(&doc->label, op->str_data, err)) {
      OV_ERROR_ADD_TRACE(err);
      goto cleanup;
    }
    break;

  case ptk_anm2_op_set_psd_path:
    // Save current value as reverse
    if (!strdup_to_array(&reverse_op->str_data, doc->psd_path, err)) {
      OV_ERROR_ADD_TRACE(err);
      goto cleanup;
    }
    // Apply new value
    if (!strdup_to_array(&doc->psd_path, op->str_data, err)) {
      OV_ERROR_ADD_TRACE(err);
      goto cleanup;
    }
    break;

  case ptk_anm2_op_set_exclusive_support_default:
    // Save current value as reverse (as string "1" or "0")
    if (!strdup_to_array(&reverse_op->str_data, doc->exclusive_support_default ? "1" : "0", err)) {
      OV_ERROR_ADD_TRACE(err);
      goto cleanup;
    }
    // Apply new value (op->str_data contains "1" or "0")
    doc->exclusive_support_default = op->str_data && op->str_data[0] == '1';
    break;

  case ptk_anm2_op_set_information:
    // Save current value as reverse
    if (doc->information) {
      if (!strdup_to_array(&reverse_op->str_data, doc->information, err)) {
        OV_ERROR_ADD_TRACE(err);
        goto cleanup;
      }
    } else {
      reverse_op->str_data = NULL;
    }
    // Apply new value (op->str_data may be NULL for auto-generate mode)
    if (doc->information) {
      OV_ARRAY_DESTROY(&doc->information);
    }
    if (op->str_data) {
      if (!strdup_to_array(&doc->information, op->str_data, err)) {
        OV_ERROR_ADD_TRACE(err);
        goto cleanup;
      }
    } else {
      doc->information = NULL;
    }
    break;

  case ptk_anm2_op_set_default_character_id:
    // Save current value as reverse
    if (doc->default_character_id) {
      if (!strdup_to_array(&reverse_op->str_data, doc->default_character_id, err)) {
        OV_ERROR_ADD_TRACE(err);
        goto cleanup;
      }
    } else {
      reverse_op->str_data = NULL;
    }
    // Apply new value (op->str_data may be NULL to clear)
    if (doc->default_character_id) {
      OV_ARRAY_DESTROY(&doc->default_character_id);
    }
    if (op->str_data) {
      if (!strdup_to_array(&doc->default_character_id, op->str_data, err)) {
        OV_ERROR_ADD_TRACE(err);
        goto cleanup;
      }
    } else {
      doc->default_character_id = NULL;
    }
    break;

  case ptk_anm2_op_transaction_begin:
    // TRANSACTION_BEGIN's reverse is TRANSACTION_END
    reverse_op->type = ptk_anm2_op_transaction_end;
    break;

  case ptk_anm2_op_transaction_end:
    // TRANSACTION_END's reverse is TRANSACTION_BEGIN
    reverse_op->type = ptk_anm2_op_transaction_begin;
    break;

  case ptk_anm2_op_selector_insert:
    // INSERT: insert selector using op->before_id for position
    // op->removed_data contains the selector to insert
    // before_id = 0 means insert at end, otherwise insert before the element with that ID
    {
      struct selector *sel = (struct selector *)op->removed_data;
      if (!sel) {
        OV_ERROR_SET_GENERIC(err, ov_error_generic_invalid_argument);
        goto cleanup;
      }

      size_t const len = OV_ARRAY_LENGTH(doc->selectors);

      // Calculate insertion index from before_id
      size_t idx = len; // Default: insert at end
      if (op->before_id != 0) {
        size_t before_idx = 0;
        if (ptk_anm2_find_selector(doc, op->before_id, &before_idx)) {
          idx = before_idx;
        }
        // If before_id not found, insert at end
      }

      // Grow array
      if (!OV_ARRAY_GROW(&doc->selectors, len + 1)) {
        OV_ERROR_SET_GENERIC(err, ov_error_generic_out_of_memory);
        goto cleanup;
      }

      // Shift elements to make room
      for (size_t i = len; i > idx; i--) {
        doc->selectors[i] = doc->selectors[i - 1];
      }

      // Insert the selector
      doc->selectors[idx] = *sel;
      OV_ARRAY_SET_LENGTH(doc->selectors, len + 1);

      // Free the container (content is now owned by doc)
      OV_FREE(&sel);
      op->removed_data = NULL; // Mark as consumed

      // Reverse operation: REMOVE
      reverse_op->type = ptk_anm2_op_selector_remove;
      reverse_op->id = op->id;
      // before_id for reverse: element after the inserted one (for re-insertion position)
      reverse_op->before_id = get_selector_before_id(doc, idx);
      reverse_op->removed_data = NULL;
    }
    break;

  case ptk_anm2_op_selector_remove:
    // REMOVE: remove selector by op->id
    // Save the removed selector to reverse_op->removed_data
    {
      // Find selector by ID
      size_t idx = 0;
      if (!ptk_anm2_find_selector(doc, op->id, &idx)) {
        OV_ERROR_SET_GENERIC(err, ov_error_generic_invalid_argument);
        goto cleanup;
      }

      size_t const len = OV_ARRAY_LENGTH(doc->selectors);

      // Allocate storage for the removed selector
      struct selector *removed_sel = NULL;
      if (!OV_REALLOC(&removed_sel, 1, sizeof(struct selector))) {
        OV_ERROR_SET_GENERIC(err, ov_error_generic_out_of_memory);
        goto cleanup;
      }
      *removed_sel = doc->selectors[idx];

      // Calculate before_id for reverse operation (element that will be after the removed one)
      uint32_t next_id = (idx + 1 < len) ? doc->selectors[idx + 1].id : 0;

      // Shift remaining selectors
      for (size_t i = idx; i < len - 1; i++) {
        doc->selectors[i] = doc->selectors[i + 1];
      }
      OV_ARRAY_SET_LENGTH(doc->selectors, len - 1);

      // Reverse operation: INSERT with the saved selector
      reverse_op->type = ptk_anm2_op_selector_insert;
      reverse_op->before_id = next_id;
      reverse_op->removed_data = removed_sel;
      reverse_op->id = removed_sel->id;
    }
    break;

  case ptk_anm2_op_item_insert:
    // INSERT: insert item using op->parent_id (selector) and op->before_id for position
    {
      struct item *it = (struct item *)op->removed_data;
      if (!it) {
        OV_ERROR_SET_GENERIC(err, ov_error_generic_invalid_argument);
        goto cleanup;
      }

      size_t sidx = 0;
      if (!ptk_anm2_find_selector(doc, op->parent_id, &sidx)) {
        OV_ERROR_SET_GENERIC(err, ov_error_generic_invalid_argument);
        goto cleanup;
      }

      struct selector *sel = &doc->selectors[sidx];
      size_t const len = OV_ARRAY_LENGTH(sel->items);

      size_t iidx = len;
      if (op->before_id != 0) {
        for (size_t i = 0; i < len; i++) {
          if (sel->items[i].id == op->before_id) {
            iidx = i;
            break;
          }
        }
      }

      if (!OV_ARRAY_GROW(&sel->items, len + 1)) {
        OV_ERROR_SET_GENERIC(err, ov_error_generic_out_of_memory);
        goto cleanup;
      }

      for (size_t i = len; i > iidx; i--) {
        sel->items[i] = sel->items[i - 1];
      }

      sel->items[iidx] = *it;
      OV_ARRAY_SET_LENGTH(sel->items, len + 1);

      OV_FREE(&it);
      op->removed_data = NULL;

      reverse_op->type = ptk_anm2_op_item_remove;
      reverse_op->id = op->id;
      reverse_op->parent_id = op->parent_id;
      reverse_op->before_id = get_item_before_id(sel, iidx);
      reverse_op->removed_data = NULL;
    }
    break;

  case ptk_anm2_op_item_remove:
    // REMOVE: remove item by op->id
    {
      size_t sidx = 0, iidx = 0;
      if (!ptk_anm2_find_item(doc, op->id, &sidx, &iidx)) {
        OV_ERROR_SET_GENERIC(err, ov_error_generic_invalid_argument);
        goto cleanup;
      }

      struct selector *sel = &doc->selectors[sidx];
      size_t const len = OV_ARRAY_LENGTH(sel->items);

      struct item *removed_item = NULL;
      if (!OV_REALLOC(&removed_item, 1, sizeof(struct item))) {
        OV_ERROR_SET_GENERIC(err, ov_error_generic_out_of_memory);
        goto cleanup;
      }
      *removed_item = sel->items[iidx];

      uint32_t next_id = (iidx + 1 < len) ? sel->items[iidx + 1].id : 0;

      for (size_t i = iidx; i < len - 1; i++) {
        sel->items[i] = sel->items[i + 1];
      }
      OV_ARRAY_SET_LENGTH(sel->items, len - 1);

      reverse_op->type = ptk_anm2_op_item_insert;
      reverse_op->before_id = next_id;
      reverse_op->removed_data = removed_item;
      reverse_op->id = removed_item->id;
      reverse_op->parent_id = sel->id;
    }
    break;

  case ptk_anm2_op_param_insert:
    // INSERT: insert param before element with op->before_id (0=end)
    // op->parent_id contains the item ID, op->removed_data contains the param to insert
    {
      struct param *p = (struct param *)op->removed_data;
      if (!p) {
        OV_ERROR_SET_GENERIC(err, ov_error_generic_invalid_argument);
        goto cleanup;
      }

      size_t sidx = 0;
      size_t iidx = 0;
      if (!ptk_anm2_find_item(doc, op->parent_id, &sidx, &iidx)) {
        OV_ERROR_SET_GENERIC(err, ov_error_generic_invalid_argument);
        goto cleanup;
      }

      struct item *it = &doc->selectors[sidx].items[iidx];
      size_t const len = OV_ARRAY_LENGTH(it->params);

      size_t pidx = len;
      if (op->before_id != 0) {
        for (size_t i = 0; i < len; i++) {
          if (it->params[i].id == op->before_id) {
            pidx = i;
            break;
          }
        }
      }

      if (!OV_ARRAY_GROW(&it->params, len + 1)) {
        OV_ERROR_SET_GENERIC(err, ov_error_generic_out_of_memory);
        goto cleanup;
      }

      for (size_t i = len; i > pidx; i--) {
        it->params[i] = it->params[i - 1];
      }

      it->params[pidx] = *p;
      OV_ARRAY_SET_LENGTH(it->params, len + 1);

      uint32_t next_id = (pidx + 1 < len + 1) ? it->params[pidx + 1].id : 0;

      reverse_op->type = ptk_anm2_op_param_remove;
      reverse_op->removed_data = NULL;
      reverse_op->id = op->id;
      reverse_op->parent_id = op->parent_id;
      reverse_op->before_id = next_id;

      OV_FREE(&p);
      op->removed_data = NULL;
    }
    break;

  case ptk_anm2_op_param_remove:
    // REMOVE: remove param with op->id from item op->parent_id
    {
      size_t sidx = 0;
      size_t iidx = 0;
      if (!ptk_anm2_find_item(doc, op->parent_id, &sidx, &iidx)) {
        OV_ERROR_SET_GENERIC(err, ov_error_generic_invalid_argument);
        goto cleanup;
      }

      struct item *it = &doc->selectors[sidx].items[iidx];
      size_t const len = OV_ARRAY_LENGTH(it->params);
      size_t pidx = len;
      for (size_t i = 0; i < len; i++) {
        if (it->params[i].id == op->id) {
          pidx = i;
          break;
        }
      }
      if (pidx >= len) {
        OV_ERROR_SET_GENERIC(err, ov_error_generic_invalid_argument);
        goto cleanup;
      }

      uint32_t next_id = (pidx + 1 < len) ? it->params[pidx + 1].id : 0;

      struct param *removed_param = NULL;
      if (!OV_REALLOC(&removed_param, 1, sizeof(struct param))) {
        OV_ERROR_SET_GENERIC(err, ov_error_generic_out_of_memory);
        goto cleanup;
      }
      *removed_param = it->params[pidx];

      for (size_t i = pidx; i < len - 1; i++) {
        it->params[i] = it->params[i + 1];
      }
      OV_ARRAY_SET_LENGTH(it->params, len - 1);

      reverse_op->type = ptk_anm2_op_param_insert;
      reverse_op->removed_data = removed_param;
      reverse_op->id = removed_param->id;
      reverse_op->parent_id = it->id;
      reverse_op->before_id = next_id;
    }
    break;

  case ptk_anm2_op_selector_set_name:
    // Save current value as reverse, apply op->str_data
    {
      size_t sidx = 0;
      if (!ptk_anm2_find_selector(doc, op->id, &sidx)) {
        OV_ERROR_SET_GENERIC(err, ov_error_generic_invalid_argument);
        goto cleanup;
      }
      struct selector *sel = &doc->selectors[sidx];
      if (!strdup_to_array(&reverse_op->str_data, sel->name, err)) {
        OV_ERROR_ADD_TRACE(err);
        goto cleanup;
      }
      if (!strdup_to_array(&sel->name, op->str_data, err)) {
        OV_ERROR_ADD_TRACE(err);
        goto cleanup;
      }
      reverse_op->id = sel->id;
    }
    break;

  case ptk_anm2_op_selector_move:
    // MOVE: move selector (op->id) to position before op->before_id (0=end)
    {
      // Find current position of the selector to move
      size_t from = 0;
      if (!ptk_anm2_find_selector(doc, op->id, &from)) {
        OV_ERROR_SET_GENERIC(err, ov_error_generic_invalid_argument);
        goto cleanup;
      }

      size_t const len = OV_ARRAY_LENGTH(doc->selectors);

      // Calculate target position from before_id
      size_t to = len; // Default: move to end
      if (op->before_id != 0) {
        size_t before_idx = 0;
        if (ptk_anm2_find_selector(doc, op->before_id, &before_idx)) {
          to = before_idx;
        }
      }

      // Adjust: if moving forward and to > from, we need to account for removal
      if (from < to && to > 0) {
        to--;
      }

      // Save before_id for reverse operation (element after current position)
      uint32_t reverse_before_id = get_selector_before_id(doc, from);

      if (from != to && to < len) {
        struct selector tmp = doc->selectors[from];
        if (from < to) {
          for (size_t i = from; i < to; i++) {
            doc->selectors[i] = doc->selectors[i + 1];
          }
        } else {
          for (size_t i = from; i > to; i--) {
            doc->selectors[i] = doc->selectors[i - 1];
          }
        }
        doc->selectors[to] = tmp;
      }

      // Reverse operation: move back to original position
      reverse_op->id = op->id;
      reverse_op->before_id = reverse_before_id;
    }
    break;

  case ptk_anm2_op_item_set_name:
    // Save current value as reverse, apply op->str_data
    {
      size_t sidx = 0;
      size_t iidx = 0;
      if (!ptk_anm2_find_item(doc, op->id, &sidx, &iidx)) {
        OV_ERROR_SET_GENERIC(err, ov_error_generic_invalid_argument);
        goto cleanup;
      }
      struct item *it = &doc->selectors[sidx].items[iidx];
      if (!strdup_to_array(&reverse_op->str_data, it->name, err)) {
        OV_ERROR_ADD_TRACE(err);
        goto cleanup;
      }
      if (!strdup_to_array(&it->name, op->str_data, err)) {
        OV_ERROR_ADD_TRACE(err);
        goto cleanup;
      }
      reverse_op->id = it->id;
    }
    break;

  case ptk_anm2_op_item_set_value:
    // Save current value as reverse, apply op->str_data
    {
      size_t sidx = 0;
      size_t iidx = 0;
      if (!ptk_anm2_find_item(doc, op->id, &sidx, &iidx)) {
        OV_ERROR_SET_GENERIC(err, ov_error_generic_invalid_argument);
        goto cleanup;
      }
      struct item *it = &doc->selectors[sidx].items[iidx];
      if (!strdup_to_array(&reverse_op->str_data, it->value, err)) {
        OV_ERROR_ADD_TRACE(err);
        goto cleanup;
      }
      if (!strdup_to_array(&it->value, op->str_data, err)) {
        OV_ERROR_ADD_TRACE(err);
        goto cleanup;
      }
      reverse_op->id = it->id;
    }
    break;

  case ptk_anm2_op_item_set_script_name:
    // Save current value as reverse, apply op->str_data
    {
      size_t sidx = 0;
      size_t iidx = 0;
      if (!ptk_anm2_find_item(doc, op->id, &sidx, &iidx)) {
        OV_ERROR_SET_GENERIC(err, ov_error_generic_invalid_argument);
        goto cleanup;
      }
      struct item *it = &doc->selectors[sidx].items[iidx];
      if (!strdup_to_array(&reverse_op->str_data, it->script_name, err)) {
        OV_ERROR_ADD_TRACE(err);
        goto cleanup;
      }
      if (!strdup_to_array(&it->script_name, op->str_data, err)) {
        OV_ERROR_ADD_TRACE(err);
        goto cleanup;
      }
      reverse_op->id = it->id;
    }
    break;

  case ptk_anm2_op_item_move:
    // MOVE: move item (op->id) to selector (op->parent_id), before (op->before_id)
    {
      size_t from_sidx = 0, from_iidx = 0;
      if (!ptk_anm2_find_item(doc, op->id, &from_sidx, &from_iidx)) {
        OV_ERROR_SET_GENERIC(err, ov_error_generic_invalid_argument);
        goto cleanup;
      }

      size_t to_sidx = 0;
      if (!ptk_anm2_find_selector(doc, op->parent_id, &to_sidx)) {
        OV_ERROR_SET_GENERIC(err, ov_error_generic_invalid_argument);
        goto cleanup;
      }

      struct selector *from_sel = &doc->selectors[from_sidx];
      struct selector *to_sel = &doc->selectors[to_sidx];

      size_t const from_len = OV_ARRAY_LENGTH(from_sel->items);
      size_t const to_len = OV_ARRAY_LENGTH(to_sel->items);

      size_t to_iidx = to_len;
      if (op->before_id != 0) {
        for (size_t i = 0; i < to_len; i++) {
          if (to_sel->items[i].id == op->before_id) {
            to_iidx = i;
            break;
          }
        }
      }

      uint32_t reverse_before_id = get_item_before_id(from_sel, from_iidx);
      uint32_t reverse_parent_id = from_sel->id;

      if (from_sidx == to_sidx) {
        if (from_iidx < to_iidx && to_iidx > 0) {
          to_iidx--;
        }
        if (from_iidx != to_iidx) {
          struct item tmp = from_sel->items[from_iidx];
          if (from_iidx < to_iidx) {
            for (size_t i = from_iidx; i < to_iidx; i++) {
              from_sel->items[i] = from_sel->items[i + 1];
            }
          } else {
            for (size_t i = from_iidx; i > to_iidx; i--) {
              from_sel->items[i] = from_sel->items[i - 1];
            }
          }
          from_sel->items[to_iidx] = tmp;
        }
      } else {
        struct item tmp = from_sel->items[from_iidx];

        for (size_t i = from_iidx; i < from_len - 1; i++) {
          from_sel->items[i] = from_sel->items[i + 1];
        }
        OV_ARRAY_SET_LENGTH(from_sel->items, from_len - 1);

        if (!OV_ARRAY_GROW(&to_sel->items, to_len + 1)) {
          OV_ARRAY_SET_LENGTH(from_sel->items, from_len);
          for (size_t i = from_len - 1; i > from_iidx; i--) {
            from_sel->items[i] = from_sel->items[i - 1];
          }
          from_sel->items[from_iidx] = tmp;
          OV_ERROR_SET_GENERIC(err, ov_error_generic_out_of_memory);
          goto cleanup;
        }
        OV_ARRAY_SET_LENGTH(to_sel->items, to_len + 1);

        for (size_t i = to_len; i > to_iidx; i--) {
          to_sel->items[i] = to_sel->items[i - 1];
        }
        to_sel->items[to_iidx] = tmp;
      }

      reverse_op->id = op->id;
      reverse_op->parent_id = reverse_parent_id;
      reverse_op->before_id = reverse_before_id;
    }
    break;

  case ptk_anm2_op_param_set_key:
    // Save current value as reverse, apply op->str_data
    {
      size_t sidx = 0;
      size_t iidx = 0;
      size_t pidx = 0;
      if (!ptk_anm2_find_param(doc, op->id, &sidx, &iidx, &pidx)) {
        OV_ERROR_SET_GENERIC(err, ov_error_generic_invalid_argument);
        goto cleanup;
      }
      struct item *item = &doc->selectors[sidx].items[iidx];
      struct param *p = &item->params[pidx];
      if (!strdup_to_array(&reverse_op->str_data, p->key, err)) {
        OV_ERROR_ADD_TRACE(err);
        goto cleanup;
      }
      if (!strdup_to_array(&p->key, op->str_data, err)) {
        OV_ERROR_ADD_TRACE(err);
        goto cleanup;
      }
      // Set parent_id (item_id) for change callback - same for both forward and reverse
      op->parent_id = item->id;
      reverse_op->id = op->id;
      reverse_op->parent_id = item->id;
    }
    break;

  case ptk_anm2_op_param_set_value:
    // Save current value as reverse, apply op->str_data
    {
      size_t sidx = 0;
      size_t iidx = 0;
      size_t pidx = 0;
      if (!ptk_anm2_find_param(doc, op->id, &sidx, &iidx, &pidx)) {
        OV_ERROR_SET_GENERIC(err, ov_error_generic_invalid_argument);
        goto cleanup;
      }
      struct item *item = &doc->selectors[sidx].items[iidx];
      struct param *p = &item->params[pidx];
      if (!strdup_to_array(&reverse_op->str_data, p->value, err)) {
        OV_ERROR_ADD_TRACE(err);
        goto cleanup;
      }
      if (!strdup_to_array(&p->value, op->str_data, err)) {
        OV_ERROR_ADD_TRACE(err);
        goto cleanup;
      }
      // Set parent_id (item_id) for change callback - same for both forward and reverse
      op->parent_id = item->id;
      reverse_op->id = op->id;
      reverse_op->parent_id = item->id;
    }
    break;

  case ptk_anm2_op_reset:
    // RESET is not used as an operation, only for notification
    OV_ERROR_SET_GENERIC(err, ov_error_generic_invalid_argument);
    goto cleanup;
  }

  // Notify change callback
  switch (op->type) {
  case ptk_anm2_op_set_label:
  case ptk_anm2_op_set_psd_path:
  case ptk_anm2_op_set_exclusive_support_default:
  case ptk_anm2_op_set_information:
  case ptk_anm2_op_set_default_character_id:
    notify_change(doc, op->type, 0, 0, 0);
    break;
  case ptk_anm2_op_selector_insert: {
    size_t idx = 0;
    uint32_t before_id = 0;
    if (ptk_anm2_find_selector(doc, op->id, &idx)) {
      before_id = get_selector_before_id(doc, idx);
    }
    notify_change(doc, op->type, op->id, 0, before_id);
  } break;
  case ptk_anm2_op_selector_remove:
  case ptk_anm2_op_selector_set_name:
    notify_change(doc, op->type, op->id, 0, 0);
    break;
  case ptk_anm2_op_selector_move: {
    size_t idx = 0;
    uint32_t before_id = 0;
    if (ptk_anm2_find_selector(doc, op->id, &idx)) {
      before_id = get_selector_before_id(doc, idx);
    }
    notify_change(doc, op->type, op->id, 0, before_id);
  } break;
  case ptk_anm2_op_item_insert: {
    size_t sel_idx = 0;
    size_t item_idx = 0;
    uint32_t before_id = 0;
    if (ptk_anm2_find_item(doc, op->id, &sel_idx, &item_idx)) {
      before_id = get_item_before_id(&doc->selectors[sel_idx], item_idx);
    }
    notify_change(doc, op->type, op->id, op->parent_id, before_id);
  } break;
  case ptk_anm2_op_item_remove:
    notify_change(doc, op->type, op->id, op->parent_id, 0);
    break;
  case ptk_anm2_op_item_set_name:
  case ptk_anm2_op_item_set_value:
  case ptk_anm2_op_item_set_script_name:
    notify_change(doc, op->type, op->id, 0, 0);
    break;
  case ptk_anm2_op_item_move: {
    size_t sel_idx = 0;
    size_t item_idx = 0;
    uint32_t before_id = 0;
    if (ptk_anm2_find_item(doc, op->id, &sel_idx, &item_idx)) {
      before_id = get_item_before_id(&doc->selectors[sel_idx], item_idx);
    }
    notify_change(doc, op->type, op->id, op->parent_id, before_id);
  } break;
  case ptk_anm2_op_param_insert: {
    size_t sel_idx = 0, item_idx = 0, param_idx = 0;
    uint32_t before_id = 0;
    if (ptk_anm2_find_param(doc, op->id, &sel_idx, &item_idx, &param_idx)) {
      before_id = get_param_before_id(&doc->selectors[sel_idx].items[item_idx], param_idx);
    }
    notify_change(doc, op->type, op->id, op->parent_id, before_id);
  } break;
  case ptk_anm2_op_param_remove:
    notify_change(doc, op->type, op->id, op->parent_id, 0);
    break;
  case ptk_anm2_op_param_set_key:
  case ptk_anm2_op_param_set_value:
    // parent_id is set in apply_op when looking up the param
    notify_change(doc, op->type, op->id, op->parent_id, 0);
    break;
  case ptk_anm2_op_transaction_begin:
  case ptk_anm2_op_transaction_end:
    notify_change(doc, op->type, 0, 0, 0);
    break;
  case ptk_anm2_op_reset:
    break;
  }

  success = true;

cleanup:
  if (!success) {
    if (reverse_op->str_data) {
      OV_ARRAY_DESTROY(&reverse_op->str_data);
    }
  }
  return success;
}

bool ptk_anm2_set_label(struct ptk_anm2 *doc, char const *label, struct ov_error *const err) {
  if (!doc) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_invalid_argument);
    return false;
  }

  bool success = false;
  struct ptk_anm2_op op = {0};
  struct ptk_anm2_op reverse_op = {0};

  // Build SET_LABEL operation with new value
  op.type = ptk_anm2_op_set_label;
  if (!strdup_to_array(&op.str_data, label, err)) {
    OV_ERROR_ADD_TRACE(err);
    goto cleanup;
  }

  // Apply the operation
  if (!apply_op(doc, &op, &reverse_op, err)) {
    OV_ERROR_ADD_TRACE(err);
    goto cleanup;
  }

  // Push reverse operation to undo stack
  if (!push_undo_op(doc, &reverse_op, err)) {
    OV_ERROR_ADD_TRACE(err);
    goto cleanup;
  }
  memset(&reverse_op, 0, sizeof(reverse_op)); // ownership transferred

  // Clear redo stack
  clear_redo_stack(doc);
  notify_state(doc);

  success = true;

cleanup:
  op_free(&op);
  op_free(&reverse_op);
  return success;
}

char const *ptk_anm2_get_psd_path(struct ptk_anm2 const *doc) {
  if (!doc) {
    return NULL;
  }
  return doc->psd_path;
}

bool ptk_anm2_set_psd_path(struct ptk_anm2 *doc, char const *path, struct ov_error *const err) {
  if (!doc) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_invalid_argument);
    return false;
  }

  bool success = false;
  struct ptk_anm2_op op = {0};
  struct ptk_anm2_op reverse_op = {0};

  // Build SET_PSD_PATH operation with new value
  op.type = ptk_anm2_op_set_psd_path;
  if (!strdup_to_array(&op.str_data, path, err)) {
    OV_ERROR_ADD_TRACE(err);
    goto cleanup;
  }

  // Apply the operation
  if (!apply_op(doc, &op, &reverse_op, err)) {
    OV_ERROR_ADD_TRACE(err);
    goto cleanup;
  }

  // Push reverse operation to undo stack
  if (!push_undo_op(doc, &reverse_op, err)) {
    OV_ERROR_ADD_TRACE(err);
    goto cleanup;
  }
  memset(&reverse_op, 0, sizeof(reverse_op)); // ownership transferred

  // Clear redo stack
  clear_redo_stack(doc);
  notify_state(doc);

  success = true;

cleanup:
  op_free(&op);
  op_free(&reverse_op);
  return success;
}

bool ptk_anm2_get_exclusive_support_default(struct ptk_anm2 const *doc) {
  if (!doc) {
    return true; // Default value when doc is NULL
  }
  return doc->exclusive_support_default;
}

bool ptk_anm2_set_exclusive_support_default(struct ptk_anm2 *doc,
                                            bool exclusive_support_default,
                                            struct ov_error *const err) {
  if (!doc) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_invalid_argument);
    return false;
  }

  bool success = false;
  struct ptk_anm2_op op = {0};
  struct ptk_anm2_op reverse_op = {0};

  // Build SET_EXCLUSIVE_DEFAULT operation with new value as string
  op.type = ptk_anm2_op_set_exclusive_support_default;
  if (!strdup_to_array(&op.str_data, exclusive_support_default ? "1" : "0", err)) {
    OV_ERROR_ADD_TRACE(err);
    goto cleanup;
  }

  // Apply the operation
  if (!apply_op(doc, &op, &reverse_op, err)) {
    OV_ERROR_ADD_TRACE(err);
    goto cleanup;
  }

  // Push reverse operation to undo stack
  if (!push_undo_op(doc, &reverse_op, err)) {
    OV_ERROR_ADD_TRACE(err);
    goto cleanup;
  }
  memset(&reverse_op, 0, sizeof(reverse_op)); // ownership transferred

  // Clear redo stack
  clear_redo_stack(doc);
  notify_state(doc);

  success = true;

cleanup:
  op_free(&op);
  op_free(&reverse_op);
  return success;
}

char const *ptk_anm2_get_information(struct ptk_anm2 const *doc) {
  if (!doc) {
    return NULL;
  }
  return doc->information;
}

bool ptk_anm2_set_information(struct ptk_anm2 *doc, char const *information, struct ov_error *const err) {
  if (!doc) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_invalid_argument);
    return false;
  }

  bool success = false;
  struct ptk_anm2_op op = {0};
  struct ptk_anm2_op reverse_op = {0};

  // Build SET_INFORMATION operation with new value
  op.type = ptk_anm2_op_set_information;
  if (information) {
    if (!strdup_to_array(&op.str_data, information, err)) {
      OV_ERROR_ADD_TRACE(err);
      goto cleanup;
    }
  } else {
    op.str_data = NULL;
  }

  // Apply the operation
  if (!apply_op(doc, &op, &reverse_op, err)) {
    OV_ERROR_ADD_TRACE(err);
    goto cleanup;
  }

  // Push reverse operation to undo stack
  if (!push_undo_op(doc, &reverse_op, err)) {
    OV_ERROR_ADD_TRACE(err);
    goto cleanup;
  }
  memset(&reverse_op, 0, sizeof(reverse_op)); // ownership transferred

  // Clear redo stack
  clear_redo_stack(doc);
  notify_state(doc);

  success = true;

cleanup:
  op_free(&op);
  op_free(&reverse_op);
  return success;
}

char const *ptk_anm2_get_default_character_id(struct ptk_anm2 const *doc) {
  if (!doc) {
    return NULL;
  }
  return doc->default_character_id;
}

bool ptk_anm2_set_default_character_id(struct ptk_anm2 *doc, char const *character_id, struct ov_error *const err) {
  if (!doc) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_invalid_argument);
    return false;
  }

  bool success = false;
  struct ptk_anm2_op op = {0};
  struct ptk_anm2_op reverse_op = {0};

  // Build SET_DEFAULT_CHARACTER_ID operation with new value
  op.type = ptk_anm2_op_set_default_character_id;
  if (character_id && character_id[0] != '\0') {
    if (!strdup_to_array(&op.str_data, character_id, err)) {
      OV_ERROR_ADD_TRACE(err);
      goto cleanup;
    }
  } else {
    op.str_data = NULL;
  }

  // Apply the operation
  if (!apply_op(doc, &op, &reverse_op, err)) {
    OV_ERROR_ADD_TRACE(err);
    goto cleanup;
  }

  // Push reverse operation to undo stack
  if (!push_undo_op(doc, &reverse_op, err)) {
    OV_ERROR_ADD_TRACE(err);
    goto cleanup;
  }
  memset(&reverse_op, 0, sizeof(reverse_op)); // ownership transferred

  // Clear redo stack
  clear_redo_stack(doc);
  notify_state(doc);

  success = true;

cleanup:
  op_free(&op);
  op_free(&reverse_op);
  return success;
}

int ptk_anm2_get_version(struct ptk_anm2 const *doc) {
  if (!doc) {
    return 0;
  }
  return doc->version;
}

size_t ptk_anm2_selector_count(struct ptk_anm2 const *doc) {
  if (!doc || !doc->selectors) {
    return 0;
  }
  return OV_ARRAY_LENGTH(doc->selectors);
}

uint32_t ptk_anm2_selector_insert(struct ptk_anm2 *doc,
                                  uint32_t before_id,
                                  char const *name,
                                  uintptr_t initial_userdata,
                                  struct ov_error *const err) {
  if (!doc) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_invalid_argument);
    return 0;
  }

  uint32_t result_id = 0;
  uint32_t new_id = 0;
  struct selector *new_sel = NULL;
  struct ptk_anm2_op op = {0};
  struct ptk_anm2_op reverse_op = {0};

  // Use default name if name is empty or NULL
  char const *effective_name =
      (name && name[0] != '\0') ? name : pgettext(".ptk.anm2 default selector name", "Unnamed Selector");

  // Allocate and initialize new selector
  if (!OV_REALLOC(&new_sel, 1, sizeof(struct selector))) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_out_of_memory);
    goto cleanup;
  }
  memset(new_sel, 0, sizeof(struct selector));
  new_sel->userdata = initial_userdata;
  new_sel->id = generate_id(doc);
  new_id = new_sel->id;

  // Copy name
  if (!strdup_to_array(&new_sel->name, effective_name, err)) {
    OV_ERROR_ADD_TRACE(err);
    goto cleanup;
  }

  // Build INSERT operation
  {
    op.type = ptk_anm2_op_selector_insert;
    op.before_id = before_id; // Use before_id for position
    op.removed_data = new_sel;
    op.id = new_id;
    new_sel = NULL; // ownership transferred to op
  }

  // Apply the operation
  if (!apply_op(doc, &op, &reverse_op, err)) {
    OV_ERROR_ADD_TRACE(err);
    goto cleanup;
  }

  // Push reverse operation to undo stack
  if (!push_undo_op(doc, &reverse_op, err)) {
    OV_ERROR_ADD_TRACE(err);
    goto cleanup;
  }
  memset(&reverse_op, 0, sizeof(reverse_op)); // ownership transferred

  // Clear redo stack
  clear_redo_stack(doc);
  notify_state(doc);

  result_id = new_id;

cleanup:
  if (new_sel) {
    selector_free(new_sel);
    OV_FREE(&new_sel);
  }
  op_free(&op);
  op_free(&reverse_op);
  return result_id;
}

bool ptk_anm2_selector_remove(struct ptk_anm2 *doc, uint32_t id, struct ov_error *const err) {
  if (!doc) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_invalid_argument);
    return false;
  }
  size_t idx = 0;
  if (!ptk_anm2_find_selector(doc, id, &idx)) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_invalid_argument);
    return false;
  }

  bool success = false;
  struct ptk_anm2_op op = {0};
  struct ptk_anm2_op reverse_op = {0};

  op.type = ptk_anm2_op_selector_remove;
  op.id = id;

  if (!apply_op(doc, &op, &reverse_op, err)) {
    OV_ERROR_ADD_TRACE(err);
    goto cleanup;
  }

  if (!push_undo_op(doc, &reverse_op, err)) {
    OV_ERROR_ADD_TRACE(err);
    goto cleanup;
  }
  memset(&reverse_op, 0, sizeof(reverse_op));

  clear_redo_stack(doc);
  notify_state(doc);

  success = true;

cleanup:
  op_free(&op);
  op_free(&reverse_op);
  return success;
}

char const *ptk_anm2_selector_get_name(struct ptk_anm2 const *doc, uint32_t id) {
  size_t idx = 0;
  if (!ptk_anm2_find_selector(doc, id, &idx)) {
    return NULL;
  }
  return doc->selectors[idx].name;
}

bool ptk_anm2_selector_set_name(struct ptk_anm2 *doc, uint32_t id, char const *name, struct ov_error *const err) {
  if (!doc) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_invalid_argument);
    return false;
  }
  size_t idx = 0;
  if (!ptk_anm2_find_selector(doc, id, &idx)) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_invalid_argument);
    return false;
  }

  bool success = false;
  struct ptk_anm2_op op = {0};
  struct ptk_anm2_op reverse_op = {0};

  if (!strdup_to_array(&op.str_data, name, err)) {
    OV_ERROR_ADD_TRACE(err);
    goto cleanup;
  }
  op.type = ptk_anm2_op_selector_set_name;
  op.id = id;

  if (!apply_op(doc, &op, &reverse_op, err)) {
    OV_ERROR_ADD_TRACE(err);
    goto cleanup;
  }

  if (!push_undo_op(doc, &reverse_op, err)) {
    OV_ERROR_ADD_TRACE(err);
    goto cleanup;
  }
  memset(&reverse_op, 0, sizeof(reverse_op));

  clear_redo_stack(doc);
  notify_state(doc);

  success = true;

cleanup:
  op_free(&op);
  op_free(&reverse_op);
  return success;
}

bool ptk_anm2_selector_move(struct ptk_anm2 *doc, uint32_t id, uint32_t before_id, struct ov_error *const err) {
  if (!doc) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_invalid_argument);
    return false;
  }
  if (!doc->selectors) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_invalid_argument);
    return false;
  }

  size_t from_idx = 0;
  if (!ptk_anm2_find_selector(doc, id, &from_idx)) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_invalid_argument);
    return false;
  }

  size_t const len = OV_ARRAY_LENGTH(doc->selectors);
  size_t to_idx = len;
  if (before_id != 0) {
    size_t before_idx = 0;
    if (ptk_anm2_find_selector(doc, before_id, &before_idx)) {
      to_idx = before_idx;
    }
  }

  if (from_idx < to_idx) {
    to_idx--;
  }

  if (to_idx >= len) {
    to_idx = len - 1;
  }

  if (from_idx == to_idx) {
    return true;
  }

  bool success = false;
  struct ptk_anm2_op op = {0};
  struct ptk_anm2_op reverse_op = {0};

  op.type = ptk_anm2_op_selector_move;
  op.id = id;
  op.before_id = before_id;

  if (!apply_op(doc, &op, &reverse_op, err)) {
    OV_ERROR_ADD_TRACE(err);
    goto cleanup;
  }

  if (!push_undo_op(doc, &reverse_op, err)) {
    OV_ERROR_ADD_TRACE(err);
    goto cleanup;
  }
  memset(&reverse_op, 0, sizeof(reverse_op));

  clear_redo_stack(doc);
  notify_state(doc);

  success = true;

cleanup:
  op_free(&op);
  op_free(&reverse_op);
  return success;
}

bool ptk_anm2_selector_would_move(struct ptk_anm2 const *doc, uint32_t id, uint32_t before_id) {
  if (!doc || !doc->selectors) {
    return false;
  }

  // Find the source selector
  size_t from_idx = 0;
  if (!ptk_anm2_find_selector(doc, id, &from_idx)) {
    return false;
  }

  // Determine target index (same logic as ptk_anm2_selector_move)
  size_t const len = OV_ARRAY_LENGTH(doc->selectors);
  size_t to_idx = len; // Default: end
  if (before_id != 0) {
    size_t before_idx = 0;
    if (ptk_anm2_find_selector(doc, before_id, &before_idx)) {
      to_idx = before_idx;
    }
  }

  // Adjust target if source is before target (moving forward)
  if (from_idx < to_idx) {
    to_idx--;
  }

  if (to_idx >= len) {
    to_idx = len - 1;
  }

  if (from_idx == to_idx) {
    return false; // No-op
  }

  return true; // Would actually move
}

static size_t item_count(struct ptk_anm2 const *doc, size_t sel_idx) {
  if (!doc || !doc->selectors) {
    return 0;
  }
  if (sel_idx >= OV_ARRAY_LENGTH(doc->selectors)) {
    return 0;
  }
  struct selector const *sel = &doc->selectors[sel_idx];
  if (!sel->items) {
    return 0;
  }
  return OV_ARRAY_LENGTH(sel->items);
}

size_t ptk_anm2_item_count(struct ptk_anm2 const *doc, uint32_t selector_id) {
  if (!doc || !doc->selectors) {
    return 0;
  }
  size_t sel_idx = 0;
  if (!ptk_anm2_find_selector(doc, selector_id, &sel_idx)) {
    return 0;
  }
  return item_count(doc, sel_idx);
}

static bool item_is_animation(struct ptk_anm2 const *doc, size_t sel_idx, size_t item_idx) {
  if (!doc || !doc->selectors) {
    return false;
  }
  if (sel_idx >= OV_ARRAY_LENGTH(doc->selectors)) {
    return false;
  }
  struct selector const *sel = &doc->selectors[sel_idx];
  if (!sel->items || item_idx >= OV_ARRAY_LENGTH(sel->items)) {
    return false;
  }
  return sel->items[item_idx].script_name != NULL;
}

bool ptk_anm2_item_is_animation(struct ptk_anm2 const *doc, uint32_t id) {
  size_t sel_idx = 0;
  size_t item_idx = 0;
  if (!ptk_anm2_find_item(doc, id, &sel_idx, &item_idx)) {
    return false;
  }
  return item_is_animation(doc, sel_idx, item_idx);
}

uint32_t ptk_anm2_item_insert_value(
    struct ptk_anm2 *doc, uint32_t before_id, char const *name, char const *value, struct ov_error *const err) {
  if (!doc || before_id == 0) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_invalid_argument);
    return 0;
  }

  size_t sel_idx = 0;
  uint32_t selector_id = 0;
  uint32_t item_before_id = 0;

  if (ptk_anm2_find_selector(doc, before_id, &sel_idx)) {
    selector_id = before_id;
    item_before_id = 0;
  } else {
    size_t item_idx = 0;
    if (!ptk_anm2_find_item(doc, before_id, &sel_idx, &item_idx)) {
      OV_ERROR_SET_GENERIC(err, ov_error_generic_invalid_argument);
      return 0;
    }
    selector_id = doc->selectors[sel_idx].id;
    item_before_id = before_id;
  }

  uint32_t result_id = 0;
  uint32_t new_id = 0;
  struct item *new_item = NULL;
  struct ptk_anm2_op op = {0};
  struct ptk_anm2_op reverse_op = {0};

  if (!OV_REALLOC(&new_item, 1, sizeof(struct item))) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_out_of_memory);
    goto cleanup;
  }
  memset(new_item, 0, sizeof(struct item));
  new_item->id = generate_id(doc);
  new_id = new_item->id;

  if (!strdup_to_array(&new_item->name, name, err)) {
    OV_ERROR_ADD_TRACE(err);
    goto cleanup;
  }

  if (!strdup_to_array(&new_item->value, value, err)) {
    OV_ERROR_ADD_TRACE(err);
    goto cleanup;
  }

  op.type = ptk_anm2_op_item_insert;
  op.before_id = item_before_id;
  op.removed_data = new_item;
  op.id = new_id;
  op.parent_id = selector_id;
  new_item = NULL;

  if (!apply_op(doc, &op, &reverse_op, err)) {
    OV_ERROR_ADD_TRACE(err);
    goto cleanup;
  }

  if (!push_undo_op(doc, &reverse_op, err)) {
    OV_ERROR_ADD_TRACE(err);
    goto cleanup;
  }
  memset(&reverse_op, 0, sizeof(reverse_op));

  clear_redo_stack(doc);
  notify_state(doc);

  result_id = new_id;

cleanup:
  if (new_item) {
    item_free(new_item);
    OV_FREE(&new_item);
  }
  op_free(&op);
  op_free(&reverse_op);
  return result_id;
}

uint32_t ptk_anm2_item_insert_animation(
    struct ptk_anm2 *doc, uint32_t before_id, char const *script_name, char const *name, struct ov_error *const err) {
  if (!doc || before_id == 0) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_invalid_argument);
    return 0;
  }

  size_t sel_idx = 0;
  uint32_t selector_id = 0;
  uint32_t item_before_id = 0;

  if (ptk_anm2_find_selector(doc, before_id, &sel_idx)) {
    selector_id = before_id;
    item_before_id = 0;
  } else {
    size_t item_idx = 0;
    if (!ptk_anm2_find_item(doc, before_id, &sel_idx, &item_idx)) {
      OV_ERROR_SET_GENERIC(err, ov_error_generic_invalid_argument);
      return 0;
    }
    selector_id = doc->selectors[sel_idx].id;
    item_before_id = before_id;
  }

  uint32_t result_id = 0;
  uint32_t new_id = 0;
  struct item *new_item = NULL;
  struct ptk_anm2_op op = {0};
  struct ptk_anm2_op reverse_op = {0};

  if (!OV_REALLOC(&new_item, 1, sizeof(struct item))) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_out_of_memory);
    goto cleanup;
  }
  memset(new_item, 0, sizeof(struct item));
  new_item->id = generate_id(doc);
  new_id = new_item->id;

  if (!strdup_to_array(&new_item->script_name, script_name, err)) {
    OV_ERROR_ADD_TRACE(err);
    goto cleanup;
  }

  if (!strdup_to_array(&new_item->name, name, err)) {
    OV_ERROR_ADD_TRACE(err);
    goto cleanup;
  }

  op.type = ptk_anm2_op_item_insert;
  op.before_id = item_before_id;
  op.removed_data = new_item;
  op.id = new_id;
  op.parent_id = selector_id;
  new_item = NULL;

  if (!apply_op(doc, &op, &reverse_op, err)) {
    OV_ERROR_ADD_TRACE(err);
    goto cleanup;
  }

  if (!push_undo_op(doc, &reverse_op, err)) {
    OV_ERROR_ADD_TRACE(err);
    goto cleanup;
  }
  memset(&reverse_op, 0, sizeof(reverse_op));

  clear_redo_stack(doc);
  notify_state(doc);

  result_id = new_id;

cleanup:
  if (new_item) {
    item_free(new_item);
    OV_FREE(&new_item);
  }
  op_free(&op);
  op_free(&reverse_op);
  return result_id;
}

bool ptk_anm2_item_remove(struct ptk_anm2 *doc, uint32_t item_id, struct ov_error *const err) {
  if (!doc) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_invalid_argument);
    return false;
  }
  size_t sel_idx = 0;
  size_t item_idx = 0;
  if (!ptk_anm2_find_item(doc, item_id, &sel_idx, &item_idx)) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_invalid_argument);
    return false;
  }

  bool success = false;
  struct ptk_anm2_op op = {0};
  struct ptk_anm2_op reverse_op = {0};

  op.type = ptk_anm2_op_item_remove;
  op.id = item_id;
  op.parent_id = doc->selectors[sel_idx].id;

  if (!apply_op(doc, &op, &reverse_op, err)) {
    OV_ERROR_ADD_TRACE(err);
    goto cleanup;
  }

  if (!push_undo_op(doc, &reverse_op, err)) {
    OV_ERROR_ADD_TRACE(err);
    goto cleanup;
  }
  memset(&reverse_op, 0, sizeof(reverse_op));

  clear_redo_stack(doc);
  notify_state(doc);

  success = true;

cleanup:
  op_free(&op);
  op_free(&reverse_op);
  return success;
}

bool ptk_anm2_item_move(struct ptk_anm2 *doc, uint32_t id, uint32_t before_id, struct ov_error *const err) {
  if (!doc || before_id == 0) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_invalid_argument);
    return false;
  }

  size_t from_sel_idx = 0;
  size_t from_idx = 0;
  if (!ptk_anm2_find_item(doc, id, &from_sel_idx, &from_idx)) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_invalid_argument);
    return false;
  }

  uint32_t dest_selector_id = 0;
  uint32_t item_before_id = 0;

  size_t before_sel_idx = 0;
  if (ptk_anm2_find_selector(doc, before_id, &before_sel_idx)) {
    dest_selector_id = before_id;
    item_before_id = 0;
  } else {
    size_t before_item_sel_idx = 0;
    size_t before_item_idx = 0;
    if (!ptk_anm2_find_item(doc, before_id, &before_item_sel_idx, &before_item_idx)) {
      OV_ERROR_SET_GENERIC(err, ov_error_generic_invalid_argument);
      return false;
    }
    dest_selector_id = doc->selectors[before_item_sel_idx].id;
    item_before_id = before_id;
  }

  size_t to_sel_idx = 0;
  if (!ptk_anm2_find_selector(doc, dest_selector_id, &to_sel_idx)) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_invalid_argument);
    return false;
  }

  size_t const from_len = OV_ARRAY_LENGTH(doc->selectors[from_sel_idx].items);
  size_t const to_len = OV_ARRAY_LENGTH(doc->selectors[to_sel_idx].items);

  size_t to_idx = to_len;
  if (item_before_id != 0) {
    size_t tmp_sel_idx = 0;
    size_t tmp_idx = 0;
    if (!ptk_anm2_find_item(doc, item_before_id, &tmp_sel_idx, &tmp_idx) || tmp_sel_idx != to_sel_idx) {
      OV_ERROR_SET_GENERIC(err, ov_error_generic_invalid_argument);
      return false;
    }
    to_idx = tmp_idx;
  }

  if (from_sel_idx == to_sel_idx) {
    if (from_idx == to_idx || (item_before_id == 0 && from_idx == from_len - 1)) {
      return true;
    }
  }

  bool success = false;
  struct ptk_anm2_op op = {0};
  struct ptk_anm2_op reverse_op = {0};

  op.type = ptk_anm2_op_item_move;
  op.id = id;
  op.parent_id = dest_selector_id;
  op.before_id = item_before_id;

  if (!apply_op(doc, &op, &reverse_op, err)) {
    OV_ERROR_ADD_TRACE(err);
    goto cleanup;
  }

  if (!push_undo_op(doc, &reverse_op, err)) {
    OV_ERROR_ADD_TRACE(err);
    goto cleanup;
  }
  memset(&reverse_op, 0, sizeof(reverse_op));

  clear_redo_stack(doc);
  notify_state(doc);

  success = true;

cleanup:
  op_free(&op);
  op_free(&reverse_op);
  return success;
}

bool ptk_anm2_item_would_move(struct ptk_anm2 const *doc, uint32_t id, uint32_t before_id) {
  if (!doc || before_id == 0) {
    return false;
  }

  // Find the item to move
  size_t from_sel_idx = 0;
  size_t from_idx = 0;
  if (!ptk_anm2_find_item(doc, id, &from_sel_idx, &from_idx)) {
    return false;
  }

  // Determine destination based on before_id (same logic as ptk_anm2_item_move)
  size_t to_sel_idx = 0;
  size_t to_idx = 0;

  // Check if before_id is a selector ID (move to end)
  size_t before_sel_idx = 0;
  if (ptk_anm2_find_selector(doc, before_id, &before_sel_idx)) {
    to_sel_idx = before_sel_idx;
    to_idx = OV_ARRAY_LENGTH(doc->selectors[before_sel_idx].items);
    // Adjust if moving within same selector
    if (from_sel_idx == before_sel_idx) {
      to_idx--;
    }
  } else {
    // before_id must be an item ID - insert before it
    size_t before_item_idx = 0;
    if (!ptk_anm2_find_item(doc, before_id, &to_sel_idx, &before_item_idx)) {
      return false;
    }
    to_idx = before_item_idx;
    // If moving within same selector and source is before destination,
    // the destination index will be reduced by 1 after source removal
    if (from_sel_idx == to_sel_idx && from_idx < before_item_idx) {
      to_idx--;
    }
  }

  // Check if move would be a no-op
  if (from_sel_idx == to_sel_idx && from_idx == to_idx) {
    return false; // No-op
  }

  return true; // Would actually move
}

char const *ptk_anm2_item_get_name(struct ptk_anm2 const *doc, uint32_t item_id) {
  size_t sel_idx = 0;
  size_t item_idx = 0;
  if (!ptk_anm2_find_item(doc, item_id, &sel_idx, &item_idx)) {
    return NULL;
  }
  return doc->selectors[sel_idx].items[item_idx].name;
}

bool ptk_anm2_item_set_name(struct ptk_anm2 *doc, uint32_t item_id, char const *name, struct ov_error *const err) {
  if (!doc) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_invalid_argument);
    return false;
  }
  size_t sel_idx = 0;
  size_t item_idx = 0;
  if (!ptk_anm2_find_item(doc, item_id, &sel_idx, &item_idx)) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_invalid_argument);
    return false;
  }

  bool success = false;
  struct ptk_anm2_op op = {0};
  struct ptk_anm2_op reverse_op = {0};

  if (!strdup_to_array(&op.str_data, name, err)) {
    OV_ERROR_ADD_TRACE(err);
    goto cleanup;
  }
  op.type = ptk_anm2_op_item_set_name;
  op.id = item_id;

  if (!apply_op(doc, &op, &reverse_op, err)) {
    OV_ERROR_ADD_TRACE(err);
    goto cleanup;
  }

  if (!push_undo_op(doc, &reverse_op, err)) {
    OV_ERROR_ADD_TRACE(err);
    goto cleanup;
  }
  memset(&reverse_op, 0, sizeof(reverse_op));

  clear_redo_stack(doc);
  notify_state(doc);

  success = true;

cleanup:
  op_free(&op);
  op_free(&reverse_op);
  return success;
}

char const *ptk_anm2_item_get_value(struct ptk_anm2 const *doc, uint32_t item_id) {
  size_t sel_idx = 0;
  size_t item_idx = 0;
  if (!ptk_anm2_find_item(doc, item_id, &sel_idx, &item_idx)) {
    return NULL;
  }
  struct selector const *sel = &doc->selectors[sel_idx];
  // Return NULL for animation items
  if (sel->items[item_idx].script_name != NULL) {
    return NULL;
  }
  return sel->items[item_idx].value;
}

bool ptk_anm2_item_set_value(struct ptk_anm2 *doc, uint32_t item_id, char const *value, struct ov_error *const err) {
  if (!doc) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_invalid_argument);
    return false;
  }
  size_t sel_idx = 0;
  size_t item_idx = 0;
  if (!ptk_anm2_find_item(doc, item_id, &sel_idx, &item_idx)) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_invalid_argument);
    return false;
  }
  if (doc->selectors[sel_idx].items[item_idx].script_name != NULL) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_invalid_argument);
    return false;
  }

  bool success = false;
  struct ptk_anm2_op op = {0};
  struct ptk_anm2_op reverse_op = {0};

  if (!strdup_to_array(&op.str_data, value, err)) {
    OV_ERROR_ADD_TRACE(err);
    goto cleanup;
  }
  op.type = ptk_anm2_op_item_set_value;
  op.id = item_id;

  if (!apply_op(doc, &op, &reverse_op, err)) {
    OV_ERROR_ADD_TRACE(err);
    goto cleanup;
  }

  if (!push_undo_op(doc, &reverse_op, err)) {
    OV_ERROR_ADD_TRACE(err);
    goto cleanup;
  }
  memset(&reverse_op, 0, sizeof(reverse_op));

  clear_redo_stack(doc);
  notify_state(doc);

  success = true;

cleanup:
  op_free(&op);
  op_free(&reverse_op);
  return success;
}

char const *ptk_anm2_item_get_script_name(struct ptk_anm2 const *doc, uint32_t item_id) {
  size_t sel_idx = 0;
  size_t item_idx = 0;
  if (!ptk_anm2_find_item(doc, item_id, &sel_idx, &item_idx)) {
    return NULL;
  }
  return doc->selectors[sel_idx].items[item_idx].script_name;
}

bool ptk_anm2_item_set_script_name(struct ptk_anm2 *doc,
                                   uint32_t item_id,
                                   char const *script_name,
                                   struct ov_error *const err) {
  if (!doc) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_invalid_argument);
    return false;
  }
  size_t sel_idx = 0;
  size_t item_idx = 0;
  if (!ptk_anm2_find_item(doc, item_id, &sel_idx, &item_idx)) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_invalid_argument);
    return false;
  }
  if (doc->selectors[sel_idx].items[item_idx].script_name == NULL) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_invalid_argument);
    return false;
  }

  bool success = false;
  struct ptk_anm2_op op = {0};
  struct ptk_anm2_op reverse_op = {0};

  if (!strdup_to_array(&op.str_data, script_name, err)) {
    OV_ERROR_ADD_TRACE(err);
    goto cleanup;
  }
  op.type = ptk_anm2_op_item_set_script_name;
  op.id = item_id;

  if (!apply_op(doc, &op, &reverse_op, err)) {
    OV_ERROR_ADD_TRACE(err);
    goto cleanup;
  }

  if (!push_undo_op(doc, &reverse_op, err)) {
    OV_ERROR_ADD_TRACE(err);
    goto cleanup;
  }
  memset(&reverse_op, 0, sizeof(reverse_op));

  clear_redo_stack(doc);
  notify_state(doc);

  success = true;

cleanup:
  op_free(&op);
  op_free(&reverse_op);
  return success;
}

size_t ptk_anm2_param_count(struct ptk_anm2 const *doc, uint32_t item_id) {
  size_t sel_idx = 0;
  size_t item_idx = 0;
  if (!ptk_anm2_find_item(doc, item_id, &sel_idx, &item_idx)) {
    return 0;
  }
  struct item const *it = &doc->selectors[sel_idx].items[item_idx];
  // Value items have no params
  if (it->script_name == NULL || !it->params) {
    return 0;
  }
  return OV_ARRAY_LENGTH(it->params);
}

uint32_t ptk_anm2_param_insert(struct ptk_anm2 *doc,
                               uint32_t item_id,
                               uint32_t before_param_id,
                               char const *key,
                               char const *value,
                               struct ov_error *const err) {
  if (!doc) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_invalid_argument);
    return 0;
  }
  size_t sel_idx = 0;
  size_t item_idx = 0;
  if (!ptk_anm2_find_item(doc, item_id, &sel_idx, &item_idx)) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_invalid_argument);
    return 0;
  }
  struct item *it = &doc->selectors[sel_idx].items[item_idx];
  if (it->script_name == NULL) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_invalid_argument);
    return 0;
  }

  if (before_param_id != 0) {
    size_t found_sel_idx = 0;
    size_t found_item_idx = 0;
    size_t found_param_idx = 0;
    if (!ptk_anm2_find_param(doc, before_param_id, &found_sel_idx, &found_item_idx, &found_param_idx) ||
        found_sel_idx != sel_idx || found_item_idx != item_idx) {
      OV_ERROR_SET_GENERIC(err, ov_error_generic_invalid_argument);
      return 0;
    }
  }

  uint32_t result_id = 0;
  uint32_t new_id = 0;
  struct param *new_param = NULL;
  struct ptk_anm2_op op = {0};
  struct ptk_anm2_op reverse_op = {0};

  if (!OV_REALLOC(&new_param, 1, sizeof(struct param))) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_out_of_memory);
    goto cleanup;
  }
  memset(new_param, 0, sizeof(struct param));
  new_param->id = generate_id(doc);
  new_id = new_param->id;

  if (!strdup_to_array(&new_param->key, key, err)) {
    OV_ERROR_ADD_TRACE(err);
    goto cleanup;
  }
  if (!strdup_to_array(&new_param->value, value, err)) {
    OV_ERROR_ADD_TRACE(err);
    goto cleanup;
  }

  op.type = ptk_anm2_op_param_insert;
  op.before_id = before_param_id;
  op.removed_data = new_param;
  op.id = new_id;
  op.parent_id = item_id;
  new_param = NULL;

  if (!apply_op(doc, &op, &reverse_op, err)) {
    OV_ERROR_ADD_TRACE(err);
    goto cleanup;
  }

  if (!push_undo_op(doc, &reverse_op, err)) {
    OV_ERROR_ADD_TRACE(err);
    goto cleanup;
  }
  memset(&reverse_op, 0, sizeof(reverse_op));

  clear_redo_stack(doc);
  notify_state(doc);

  result_id = new_id;

cleanup:
  if (new_param) {
    if (new_param->key) {
      OV_ARRAY_DESTROY(&new_param->key);
    }
    if (new_param->value) {
      OV_ARRAY_DESTROY(&new_param->value);
    }
    OV_FREE(&new_param);
  }
  op_free(&op);
  op_free(&reverse_op);
  return result_id;
}

bool ptk_anm2_param_remove(struct ptk_anm2 *doc, uint32_t param_id, struct ov_error *const err) {
  if (!doc) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_invalid_argument);
    return false;
  }
  size_t sel_idx = 0;
  size_t item_idx = 0;
  size_t param_idx = 0;
  if (!ptk_anm2_find_param(doc, param_id, &sel_idx, &item_idx, &param_idx)) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_invalid_argument);
    return false;
  }
  struct item *it = &doc->selectors[sel_idx].items[item_idx];

  bool success = false;
  struct ptk_anm2_op op = {0};
  struct ptk_anm2_op reverse_op = {0};

  op.type = ptk_anm2_op_param_remove;
  op.id = param_id;
  op.parent_id = it->id;

  if (!apply_op(doc, &op, &reverse_op, err)) {
    OV_ERROR_ADD_TRACE(err);
    goto cleanup;
  }

  if (!push_undo_op(doc, &reverse_op, err)) {
    OV_ERROR_ADD_TRACE(err);
    goto cleanup;
  }
  memset(&reverse_op, 0, sizeof(reverse_op));

  clear_redo_stack(doc);
  notify_state(doc);

  success = true;

cleanup:
  op_free(&op);
  op_free(&reverse_op);
  return success;
}

char const *ptk_anm2_param_get_key(struct ptk_anm2 const *doc, uint32_t param_id) {
  size_t sel_idx = 0;
  size_t item_idx = 0;
  size_t param_idx = 0;
  if (!ptk_anm2_find_param(doc, param_id, &sel_idx, &item_idx, &param_idx)) {
    return NULL;
  }
  return doc->selectors[sel_idx].items[item_idx].params[param_idx].key;
}

bool ptk_anm2_param_set_key(struct ptk_anm2 *doc, uint32_t param_id, char const *key, struct ov_error *const err) {
  if (!doc) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_invalid_argument);
    return false;
  }
  size_t sel_idx = 0;
  size_t item_idx = 0;
  size_t param_idx = 0;
  if (!ptk_anm2_find_param(doc, param_id, &sel_idx, &item_idx, &param_idx)) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_invalid_argument);
    return false;
  }

  bool success = false;
  struct ptk_anm2_op op = {0};
  struct ptk_anm2_op reverse_op = {0};

  if (!strdup_to_array(&op.str_data, key, err)) {
    OV_ERROR_ADD_TRACE(err);
    goto cleanup;
  }
  op.type = ptk_anm2_op_param_set_key;
  op.id = param_id;

  if (!apply_op(doc, &op, &reverse_op, err)) {
    OV_ERROR_ADD_TRACE(err);
    goto cleanup;
  }

  if (!push_undo_op(doc, &reverse_op, err)) {
    OV_ERROR_ADD_TRACE(err);
    goto cleanup;
  }
  memset(&reverse_op, 0, sizeof(reverse_op));

  clear_redo_stack(doc);
  notify_state(doc);

  success = true;

cleanup:
  op_free(&op);
  op_free(&reverse_op);
  return success;
}

char const *ptk_anm2_param_get_value(struct ptk_anm2 const *doc, uint32_t param_id) {
  size_t sel_idx = 0;
  size_t item_idx = 0;
  size_t param_idx = 0;
  if (!ptk_anm2_find_param(doc, param_id, &sel_idx, &item_idx, &param_idx)) {
    return NULL;
  }
  return doc->selectors[sel_idx].items[item_idx].params[param_idx].value;
}

bool ptk_anm2_param_set_value(struct ptk_anm2 *doc, uint32_t param_id, char const *value, struct ov_error *const err) {
  if (!doc) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_invalid_argument);
    return false;
  }
  size_t sel_idx = 0;
  size_t item_idx = 0;
  size_t param_idx = 0;
  if (!ptk_anm2_find_param(doc, param_id, &sel_idx, &item_idx, &param_idx)) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_invalid_argument);
    return false;
  }

  bool success = false;
  struct ptk_anm2_op op = {0};
  struct ptk_anm2_op reverse_op = {0};

  if (!strdup_to_array(&op.str_data, value, err)) {
    OV_ERROR_ADD_TRACE(err);
    goto cleanup;
  }
  op.type = ptk_anm2_op_param_set_value;
  op.id = param_id;

  if (!apply_op(doc, &op, &reverse_op, err)) {
    OV_ERROR_ADD_TRACE(err);
    goto cleanup;
  }

  if (!push_undo_op(doc, &reverse_op, err)) {
    OV_ERROR_ADD_TRACE(err);
    goto cleanup;
  }
  memset(&reverse_op, 0, sizeof(reverse_op));

  clear_redo_stack(doc);
  notify_state(doc);

  success = true;

cleanup:
  op_free(&op);
  op_free(&reverse_op);
  return success;
}

bool ptk_anm2_can_undo(struct ptk_anm2 const *doc) {
  if (!doc || !doc->undo_stack) {
    return false;
  }
  return OV_ARRAY_LENGTH(doc->undo_stack) > 0;
}

bool ptk_anm2_can_redo(struct ptk_anm2 const *doc) {
  if (!doc || !doc->redo_stack) {
    return false;
  }
  return OV_ARRAY_LENGTH(doc->redo_stack) > 0;
}

bool ptk_anm2_undo(struct ptk_anm2 *doc, struct ov_error *const err) {
  if (!doc) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_invalid_argument);
    return false;
  }
  if (!ptk_anm2_can_undo(doc)) {
    return false;
  }

  bool success = false;
  struct ptk_anm2_op op = {.type = ptk_anm2_op_transaction_begin};
  struct ptk_anm2_op reverse_op = {.type = ptk_anm2_op_transaction_begin};

  // Pop from undo stack
  size_t len = OV_ARRAY_LENGTH(doc->undo_stack);
  op = doc->undo_stack[len - 1];
  OV_ARRAY_SET_LENGTH(doc->undo_stack, len - 1);

  // Check if this is a TRANSACTION_END - if so, we need to undo until TRANSACTION_BEGIN
  bool const is_transaction = (op.type == ptk_anm2_op_transaction_end);

  for (;;) {
    enum ptk_anm2_op_type const op_type = op.type;

    // Apply the reverse operation (the op contains the old value)
    if (!apply_op(doc, &op, &reverse_op, err)) {
      OV_ERROR_ADD_TRACE(err);
      goto cleanup;
    }

    // Push reverse operation to redo stack (transfers ownership)
    {
      size_t const rlen = OV_ARRAY_LENGTH(doc->redo_stack);
      if (!OV_ARRAY_GROW(&doc->redo_stack, rlen + 1)) {
        OV_ERROR_SET_GENERIC(err, ov_error_generic_out_of_memory);
        goto cleanup;
      }
      doc->redo_stack[rlen] = reverse_op;
      OV_ARRAY_SET_LENGTH(doc->redo_stack, rlen + 1);
    }
    memset(&reverse_op, 0, sizeof(reverse_op)); // ownership transferred

    // Free original op's resources
    op_free(&op);
    memset(&op, 0, sizeof(op));

    // If we just processed TRANSACTION_BEGIN and we're in a transaction, we're done
    if (is_transaction && op_type == ptk_anm2_op_transaction_begin) {
      break;
    }

    // If we're not in a transaction, we're done after one operation
    if (!is_transaction) {
      break;
    }

    // Continue processing the group
    len = OV_ARRAY_LENGTH(doc->undo_stack);
    if (len == 0) {
      // Shouldn't happen in well-formed groups, but handle gracefully
      break;
    }
    op = doc->undo_stack[len - 1];
    OV_ARRAY_SET_LENGTH(doc->undo_stack, len - 1);
  }
  notify_state(doc);

  success = true;

cleanup:
  if (!success) {
    op_free(&op);
    op_free(&reverse_op);
  }
  return success;
}

bool ptk_anm2_redo(struct ptk_anm2 *doc, struct ov_error *const err) {
  if (!doc) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_invalid_argument);
    return false;
  }
  if (!ptk_anm2_can_redo(doc)) {
    return false;
  }

  bool success = false;
  struct ptk_anm2_op op = {.type = ptk_anm2_op_transaction_begin};
  struct ptk_anm2_op reverse_op = {.type = ptk_anm2_op_transaction_begin};

  // Pop from redo stack
  size_t len = OV_ARRAY_LENGTH(doc->redo_stack);
  op = doc->redo_stack[len - 1];
  OV_ARRAY_SET_LENGTH(doc->redo_stack, len - 1);

  // Check if this is a TRANSACTION_END - if so, we need to redo until TRANSACTION_BEGIN
  // (After undo, redo stack has operations in reverse order:
  //  TRANSACTION_END is at top, TRANSACTION_BEGIN is at bottom)
  bool const is_transaction = (op.type == ptk_anm2_op_transaction_end);

  for (;;) {
    enum ptk_anm2_op_type const op_type = op.type;

    // Apply the operation
    if (!apply_op(doc, &op, &reverse_op, err)) {
      OV_ERROR_ADD_TRACE(err);
      goto cleanup;
    }

    // Push reverse operation to undo stack (transfers ownership)
    if (!push_undo_op(doc, &reverse_op, err)) {
      OV_ERROR_ADD_TRACE(err);
      goto cleanup;
    }
    memset(&reverse_op, 0, sizeof(reverse_op)); // ownership transferred

    // Free original op's resources
    op_free(&op);
    memset(&op, 0, sizeof(op));

    // If we just processed TRANSACTION_BEGIN and we're in a transaction, we're done
    if (is_transaction && op_type == ptk_anm2_op_transaction_begin) {
      break;
    }

    // If we're not in a transaction, we're done after one operation
    if (!is_transaction) {
      break;
    }

    // Continue processing the group
    len = OV_ARRAY_LENGTH(doc->redo_stack);
    if (len == 0) {
      // Shouldn't happen in well-formed groups, but handle gracefully
      break;
    }
    op = doc->redo_stack[len - 1];
    OV_ARRAY_SET_LENGTH(doc->redo_stack, len - 1);
  }
  notify_state(doc);

  success = true;

cleanup:
  if (!success) {
    op_free(&op);
    op_free(&reverse_op);
  }
  return success;
}

void ptk_anm2_clear_undo_history(struct ptk_anm2 *doc) {
  if (!doc) {
    return;
  }
  op_stack_clear(&doc->undo_stack);
  op_stack_clear(&doc->redo_stack);
}

bool ptk_anm2_begin_transaction(struct ptk_anm2 *doc, struct ov_error *const err) {
  if (!doc) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_invalid_argument);
    return false;
  }
  if (doc->transaction_depth == 0) {
    // Push TRANSACTION_BEGIN marker when entering outermost transaction
    clear_redo_stack(doc);
    struct ptk_anm2_op op = {.type = ptk_anm2_op_transaction_begin};
    if (!push_undo_op(doc, &op, err)) {
      OV_ERROR_ADD_TRACE(err);
      return false;
    }
    notify_change(doc, ptk_anm2_op_transaction_begin, 0, 0, 0);
    notify_state(doc);
  }
  doc->transaction_depth++;
  return true;
}

bool ptk_anm2_end_transaction(struct ptk_anm2 *doc, struct ov_error *const err) {
  if (!doc) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_invalid_argument);
    return false;
  }
  if (doc->transaction_depth <= 0) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_invalid_argument);
    return false;
  }
  doc->transaction_depth--;
  if (doc->transaction_depth == 0) {
    // Check if the transaction was empty (only TRANSACTION_BEGIN on stack)
    size_t const undo_len = OV_ARRAY_LENGTH(doc->undo_stack);
    if (undo_len > 0 && doc->undo_stack[undo_len - 1].type == ptk_anm2_op_transaction_begin) {
      // Empty transaction - remove TRANSACTION_BEGIN and don't push TRANSACTION_END
      op_free(&doc->undo_stack[undo_len - 1]);
      OV_ARRAY_SET_LENGTH(doc->undo_stack, undo_len - 1);
      // Notify state change to update toolbar (undo was enabled during begin_transaction)
      notify_state(doc);
      return true;
    }

    // Push TRANSACTION_END marker when exiting outermost transaction
    struct ptk_anm2_op op = {.type = ptk_anm2_op_transaction_end};
    if (!push_undo_op(doc, &op, err)) {
      OV_ERROR_ADD_TRACE(err);
      return false;
    }
    notify_change(doc, ptk_anm2_op_transaction_end, 0, 0, 0);
    notify_state(doc);
  }
  return true;
}

// Parse animation item: {script: "name", n: "display name", params: [[key, value], ...]}
static bool parse_item_animation(yyjson_val *item_val, struct item *it, struct ov_error *const err) {
  bool success = false;
  yyjson_val *script_val = yyjson_obj_get(item_val, "script");
  yyjson_val *n_val = NULL;
  yyjson_val *params_val = NULL;

  if (!script_val || !yyjson_is_str(script_val)) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_fail);
    goto cleanup;
  }

  // Copy script_name
  {
    char const *script_str = yyjson_get_str(script_val);
    if (!strdup_to_array(&it->script_name, script_str, err)) {
      OV_ERROR_ADD_TRACE(err);
      goto cleanup;
    }
  }

  // Copy name (display name) if present
  n_val = yyjson_obj_get(item_val, "n");
  if (n_val && yyjson_is_str(n_val)) {
    char const *n_str = yyjson_get_str(n_val);
    if (!strdup_to_array(&it->name, n_str, err)) {
      OV_ERROR_ADD_TRACE(err);
      goto cleanup;
    }
  }

  // Parse params array [[key, value], ...]
  params_val = yyjson_obj_get(item_val, "params");
  if (params_val && yyjson_is_arr(params_val)) {
    size_t idx, max;
    yyjson_val *param_tuple;
    yyjson_arr_foreach(params_val, idx, max, param_tuple) {
      if (yyjson_is_arr(param_tuple) && yyjson_arr_size(param_tuple) == 2) {
        yyjson_val *key_val = yyjson_arr_get(param_tuple, 0);
        yyjson_val *val_val = yyjson_arr_get(param_tuple, 1);
        if (yyjson_is_str(key_val) && yyjson_is_str(val_val)) {
          struct param p = {0};
          size_t params_len;

          char const *key_str = yyjson_get_str(key_val);
          char const *val_str = yyjson_get_str(val_val);

          if (!strdup_to_array(&p.key, key_str, err)) {
            OV_ERROR_ADD_TRACE(err);
            goto cleanup;
          }
          if (!strdup_to_array(&p.value, val_str, err)) {
            OV_ARRAY_DESTROY(&p.key);
            OV_ERROR_ADD_TRACE(err);
            goto cleanup;
          }

          params_len = OV_ARRAY_LENGTH(it->params);
          if (!OV_ARRAY_GROW(&it->params, params_len + 1)) {
            param_free(&p);
            OV_ERROR_SET_GENERIC(err, ov_error_generic_out_of_memory);
            goto cleanup;
          }
          it->params[params_len] = p;
          OV_ARRAY_SET_LENGTH(it->params, params_len + 1);
        }
      }
    }
  }

  success = true;

cleanup:
  return success;
}

// Parse selector: {group: "name", items: [...]}
static bool parse_selector_json(yyjson_val *sel_val, struct selector *sel, struct ov_error *const err) {
  bool success = false;
  yyjson_val *group_val = yyjson_obj_get(sel_val, "group");
  yyjson_val *items_val = yyjson_obj_get(sel_val, "items");

  if (!yyjson_is_obj(sel_val)) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_fail);
    goto cleanup;
  }

  if (!group_val || !yyjson_is_str(group_val) || !items_val || !yyjson_is_arr(items_val)) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_fail);
    goto cleanup;
  }

  {
    char const *group_str = yyjson_get_str(group_val);
    if (!strdup_to_array(&sel->name, group_str, err)) {
      OV_ERROR_ADD_TRACE(err);
      goto cleanup;
    }
  }

  {
    size_t idx, max;
    yyjson_val *item_val;
    yyjson_arr_foreach(items_val, idx, max, item_val) {
      struct item it = {0};
      size_t items_len;

      if (yyjson_is_obj(item_val)) {
        // Animation item {script, n, params}
        yyjson_val *script_val = yyjson_obj_get(item_val, "script");
        if (script_val && yyjson_is_str(script_val)) {
          if (!parse_item_animation(item_val, &it, err)) {
            OV_ERROR_ADD_TRACE(err);
            goto cleanup;
          }
        } else {
          // Unknown object format, skip
          continue;
        }
      } else if (yyjson_is_arr(item_val) && yyjson_arr_size(item_val) == 2) {
        // Value item [name, value]
        yyjson_val *n_val = yyjson_arr_get(item_val, 0);
        yyjson_val *v_val = yyjson_arr_get(item_val, 1);
        if (!yyjson_is_str(n_val) || !yyjson_is_str(v_val)) {
          continue;
        }
        char const *n_str = yyjson_get_str(n_val);
        char const *v_str = yyjson_get_str(v_val);
        if (!strdup_to_array(&it.name, n_str, err)) {
          OV_ERROR_ADD_TRACE(err);
          goto cleanup;
        }
        if (!strdup_to_array(&it.value, v_str, err)) {
          item_free(&it);
          OV_ERROR_ADD_TRACE(err);
          goto cleanup;
        }
      } else {
        continue;
      }

      items_len = OV_ARRAY_LENGTH(sel->items);
      if (!OV_ARRAY_GROW(&sel->items, items_len + 1)) {
        item_free(&it);
        OV_ERROR_SET_GENERIC(err, ov_error_generic_out_of_memory);
        goto cleanup;
      }
      sel->items[items_len] = it;
      OV_ARRAY_SET_LENGTH(sel->items, items_len + 1);
    }
  }

  success = true;

cleanup:
  if (!success) {
    selector_free(sel);
    memset(sel, 0, sizeof(*sel));
  }
  return success;
}

// Parse JSON metadata and populate doc
static bool
parse_metadata_json(char const *json_str, size_t json_len, struct ptk_anm2 *doc, struct ov_error *const err) {
  yyjson_doc *jdoc = NULL;
  yyjson_val *root = NULL;
  bool success = false;
  char *json_buf = NULL;

  if (!OV_ARRAY_GROW(&json_buf, json_len + 1)) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_out_of_memory);
    goto cleanup;
  }
  memcpy(json_buf, json_str, json_len);
  json_buf[json_len] = '\0';
  OV_ARRAY_SET_LENGTH(json_buf, json_len + 1);

  jdoc = yyjson_read_opts(json_buf, json_len, 0, ptk_json_get_alc(), NULL);
  if (!jdoc) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_fail);
    goto cleanup;
  }

  root = yyjson_doc_get_root(jdoc);
  if (!yyjson_is_obj(root)) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_fail);
    goto cleanup;
  }

  // version
  {
    yyjson_val *v = yyjson_obj_get(root, "version");
    doc->version = v && yyjson_is_int(v) ? (int)yyjson_get_int(v) : 1;
  }

  // psd path
  {
    yyjson_val *v = yyjson_obj_get(root, "psd");
    if (v && yyjson_is_str(v)) {
      if (!strdup_to_array(&doc->psd_path, yyjson_get_str(v), err)) {
        OV_ERROR_ADD_TRACE(err);
        goto cleanup;
      }
    }
  }

  // label (may not be in JSON, often in script comments)
  {
    yyjson_val *v = yyjson_obj_get(root, "label");
    if (v && yyjson_is_str(v)) {
      if (!strdup_to_array(&doc->label, yyjson_get_str(v), err)) {
        OV_ERROR_ADD_TRACE(err);
        goto cleanup;
      }
    }
  }

  // checksum (stored in JSON metadata)
  {
    yyjson_val *v = yyjson_obj_get(root, "checksum");
    if (v && yyjson_is_str(v)) {
      char const *checksum_str = yyjson_get_str(v);
      if (checksum_str) {
        doc->stored_checksum = strtoull(checksum_str, NULL, 16);
      }
    }
  }

  // exclusive_support_default (default is true if not present)
  {
    yyjson_val *v = yyjson_obj_get(root, "exclusive_support_default");
    if (v && yyjson_is_bool(v)) {
      doc->exclusive_support_default = yyjson_get_bool(v);
    } else {
      doc->exclusive_support_default = true; // Default value
    }
  }

  // information (NULL means auto-generate)
  {
    yyjson_val *v = yyjson_obj_get(root, "information");
    if (v && yyjson_is_str(v)) {
      if (!strdup_to_array(&doc->information, yyjson_get_str(v), err)) {
        OV_ERROR_ADD_TRACE(err);
        goto cleanup;
      }
    }
  }

  // default_character_id
  {
    yyjson_val *v = yyjson_obj_get(root, "defaultCharacterId");
    if (v && yyjson_is_str(v)) {
      if (!strdup_to_array(&doc->default_character_id, yyjson_get_str(v), err)) {
        OV_ERROR_ADD_TRACE(err);
        goto cleanup;
      }
    }
  }

  // selectors
  {
    yyjson_val *selectors = yyjson_obj_get(root, "selectors");
    if (selectors && yyjson_is_arr(selectors)) {
      size_t idx, max;
      yyjson_val *sel_val;
      yyjson_arr_foreach(selectors, idx, max, sel_val) {
        struct selector sel = {0};
        size_t selectors_len;

        if (!parse_selector_json(sel_val, &sel, err)) {
          OV_ERROR_ADD_TRACE(err);
          goto cleanup;
        }

        // Assign IDs to loaded selector, items, and params
        sel.id = generate_id(doc);
        if (sel.items) {
          size_t items_count = OV_ARRAY_LENGTH(sel.items);
          for (size_t i = 0; i < items_count; i++) {
            sel.items[i].id = generate_id(doc);
            if (sel.items[i].params) {
              size_t params_count = OV_ARRAY_LENGTH(sel.items[i].params);
              for (size_t j = 0; j < params_count; j++) {
                sel.items[i].params[j].id = generate_id(doc);
              }
            }
          }
        }

        selectors_len = OV_ARRAY_LENGTH(doc->selectors);
        if (!OV_ARRAY_GROW(&doc->selectors, selectors_len + 1)) {
          selector_free(&sel);
          OV_ERROR_SET_GENERIC(err, ov_error_generic_out_of_memory);
          goto cleanup;
        }
        doc->selectors[selectors_len] = sel;
        OV_ARRAY_SET_LENGTH(doc->selectors, selectors_len + 1);
      }
    }
  }

  success = true;

cleanup:
  if (jdoc) {
    yyjson_doc_free(jdoc);
  }
  if (json_buf) {
    OV_ARRAY_DESTROY(&json_buf);
  }
  return success;
}

bool ptk_anm2_load(struct ptk_anm2 *doc, wchar_t const *path, struct ov_error *const err) {
  if (!doc || !path) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_invalid_argument);
    return false;
  }

  bool success = false;
  char *content = NULL;
  struct ovl_file *file = NULL;
  // Initialize temp with doc's callbacks so they survive through reset and swap
  struct ptk_anm2 temp = {
      .change_callback = doc->change_callback,
      .change_callback_userdata = doc->change_callback_userdata,
      .state_callback = doc->state_callback,
      .state_callback_userdata = doc->state_callback_userdata,
  };

  // Initialize temporary document (ptk_anm2_reset preserves callbacks)
  if (!ptk_anm2_reset(&temp, err)) {
    OV_ERROR_ADD_TRACE(err);
    goto cleanup;
  }

  // Read file content
  {
    size_t file_size = 0;
    size_t bytes_read = 0;

    if (!ovl_file_open(path, &file, err)) {
      OV_ERROR_ADD_TRACE(err);
      goto cleanup;
    }

    if (!ovl_file_size(file, &file_size, err)) {
      OV_ERROR_ADD_TRACE(err);
      goto cleanup;
    }

    if (!OV_ARRAY_GROW(&content, file_size + 1)) {
      OV_ERROR_SET_GENERIC(err, ov_error_generic_out_of_memory);
      goto cleanup;
    }

    if (!ovl_file_read(file, content, file_size, &bytes_read, err)) {
      OV_ERROR_ADD_TRACE(err);
      goto cleanup;
    }
    content[bytes_read] = '\0';
    OV_ARRAY_SET_LENGTH(content, bytes_read + 1);

    ovl_file_close(file);
    file = NULL;
  }

  // Find and parse JSON metadata line into temp
  {
    char const *prefix_pos = NULL;
    char const *suffix_pos = NULL;
    char const *json_start = NULL;
    size_t json_len;

    // Search for json_prefix at the beginning of any line
    char const *search_start = content;
    while ((prefix_pos = strstr(search_start, json_prefix)) != NULL) {
      // Check if prefix is at the start of the content or at the start of a line
      if (prefix_pos == content || prefix_pos[-1] == '\n') {
        break; // Found valid prefix at line beginning
      }
      // Continue searching after this occurrence
      search_start = prefix_pos + 1;
    }
    if (prefix_pos) {
      suffix_pos = strstr(prefix_pos + json_prefix_len, json_suffix);
    }
    if (!prefix_pos || !suffix_pos) {
      OV_ERROR_SET(err,
                   ov_error_type_generic,
                   ptk_anm2_error_invalid_format,
                   gettext("The file does not appear to be a valid PSDToolKit anm2 script."));
      goto cleanup;
    }

    json_start = prefix_pos + json_prefix_len;
    json_len = (size_t)(suffix_pos - json_start);

    // Parse into temp (not doc) - doc_init already set default label, clear it first
    if (temp.label) {
      OV_ARRAY_DESTROY(&temp.label);
    }

    if (!parse_metadata_json(json_start, json_len, &temp, err)) {
      OV_ERROR_ADD_TRACE(err);
      goto cleanup;
    }

    // Calculate checksum from script body (everything after the JSON metadata line)
    char const *newline = strchr(suffix_pos, '\n');
    if (newline) {
      char const *script_body = newline + 1;
      size_t body_len = strlen(script_body);
      temp.calculated_checksum = calculate_checksum(script_body, body_len);
    } else {
      temp.calculated_checksum = 0;
    }
  }

  // Swap contents
  doc_cleanup(doc);
  *doc = temp;
  temp = (struct ptk_anm2){0};

  // Clear undo/redo stacks after load
  ptk_anm2_clear_undo_history(doc);

  // Clear modified flag after successful load
  doc->modified = false;

  // Notify that document was reset (loaded)
  notify_change(doc, ptk_anm2_op_reset, 0, 0, 0);
  notify_state(doc);

  success = true;

cleanup:
  // Clean up temp if it still has resources (failure case)
  doc_cleanup(&temp);
  if (file) {
    ovl_file_close(file);
  }
  if (content) {
    OV_ARRAY_DESTROY(&content);
  }
  return success;
}

bool ptk_anm2_can_save(struct ptk_anm2 const *doc) {
  if (!doc) {
    return false;
  }

  // Must have at least one selector with items
  size_t const selectors_len = OV_ARRAY_LENGTH(doc->selectors);
  for (size_t i = 0; i < selectors_len; i++) {
    if (OV_ARRAY_LENGTH(doc->selectors[i].items) > 0) {
      return true;
    }
  }

  return false;
}

bool ptk_anm2_save(struct ptk_anm2 *doc, wchar_t const *path, struct ov_error *const err) {
  if (!doc || !path) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_invalid_argument);
    return false;
  }

  char *content = NULL;
  char *obj2_content = NULL;
  wchar_t *obj2_path = NULL;
  struct ovl_file *file = NULL;
  bool success = false;

  // Check if filename starts with @ to determine multi-script mode
  // Find the filename part (after last path separator)
  wchar_t const *filename = path;
  for (wchar_t const *p = path; *p; p++) {
    if (*p == L'/' || *p == L'\\') {
      filename = p + 1;
    }
  }
  bool const is_multiscript = (filename[0] == L'@');

  // Generate script content
  if (is_multiscript) {
    if (!generate_multiscript_content(doc, &content, err)) {
      OV_ERROR_ADD_TRACE(err);
      goto cleanup;
    }
  } else {
    if (!generate_script_content(doc, &content, err)) {
      OV_ERROR_ADD_TRACE(err);
      goto cleanup;
    }
  }

  // Write main .anm2 file
  if (!ovl_file_create(path, &file, err)) {
    OV_ERROR_ADD_TRACE(err);
    goto cleanup;
  }

  {
    size_t const content_len = OV_ARRAY_LENGTH(content);
    size_t bytes_written = 0;
    if (!ovl_file_write(file, content, content_len, &bytes_written, err)) {
      OV_ERROR_ADD_TRACE(err);
      goto cleanup;
    }
  }

  ovl_file_close(file);
  file = NULL;

  // For multiscript mode, also generate and save .obj2 file
  if (is_multiscript) {
    // Create .obj2 path by changing extension
    // @foo.ptk.anm2 -> @foo.ptk.obj2
    {
      size_t const path_len = wcslen(path);
      // Check for .anm2 extension
      if (path_len > 5 && wcscmp(path + path_len - 5, L".anm2") == 0) {
        if (!OV_ARRAY_GROW(&obj2_path, path_len + 1)) {
          OV_ERROR_SET_GENERIC(err, ov_error_generic_out_of_memory);
          goto cleanup;
        }
        wcscpy(obj2_path, path);
        // Replace .anm2 with .obj2
        wcscpy(obj2_path + path_len - 5, L".obj2");
        OV_ARRAY_SET_LENGTH(obj2_path, path_len + 1);
      } else {
        // If not .anm2, just append .obj2
        if (!OV_ARRAY_GROW(&obj2_path, path_len + 6)) {
          OV_ERROR_SET_GENERIC(err, ov_error_generic_out_of_memory);
          goto cleanup;
        }
        wcscpy(obj2_path, path);
        wcscat(obj2_path, L".obj2");
        OV_ARRAY_SET_LENGTH(obj2_path, path_len + 6);
      }
    }

    // Generate .obj2 content
    if (!generate_obj2_content(doc, &obj2_content, err)) {
      OV_ERROR_ADD_TRACE(err);
      goto cleanup;
    }

    // Write .obj2 file
    if (!ovl_file_create(obj2_path, &file, err)) {
      OV_ERROR_ADD_TRACE(err);
      goto cleanup;
    }

    {
      size_t const obj2_len = OV_ARRAY_LENGTH(obj2_content);
      size_t bytes_written = 0;
      if (!ovl_file_write(file, obj2_content, obj2_len, &bytes_written, err)) {
        OV_ERROR_ADD_TRACE(err);
        goto cleanup;
      }
    }

    ovl_file_close(file);
    file = NULL;
  }

  // Clear modified flag on successful save
  doc->modified = false;
  notify_state(doc);

  success = true;

cleanup:
  if (file) {
    ovl_file_close(file);
  }
  if (content) {
    OV_ARRAY_DESTROY(&content);
  }
  if (obj2_content) {
    OV_ARRAY_DESTROY(&obj2_content);
  }
  if (obj2_path) {
    OV_ARRAY_DESTROY(&obj2_path);
  }
  return success;
}

bool ptk_anm2_verify_checksum(struct ptk_anm2 const *doc) {
  if (!doc) {
    return false;
  }
  return doc->stored_checksum == doc->calculated_checksum;
}

bool ptk_anm2_is_modified(struct ptk_anm2 const *doc) {
  if (!doc) {
    return false;
  }
  return doc->modified;
}

uint32_t ptk_anm2_selector_get_id(struct ptk_anm2 const *doc, size_t idx) {
  if (!doc || !doc->selectors) {
    return 0;
  }
  if (idx >= OV_ARRAY_LENGTH(doc->selectors)) {
    return 0;
  }
  return doc->selectors[idx].id;
}

static uintptr_t selector_get_userdata(struct ptk_anm2 const *doc, size_t idx) {
  if (!doc || !doc->selectors) {
    return 0;
  }
  if (idx >= OV_ARRAY_LENGTH(doc->selectors)) {
    return 0;
  }
  return doc->selectors[idx].userdata;
}

static void selector_set_userdata(struct ptk_anm2 *doc, size_t idx, uintptr_t userdata) {
  if (!doc || !doc->selectors) {
    return;
  }
  if (idx >= OV_ARRAY_LENGTH(doc->selectors)) {
    return;
  }
  doc->selectors[idx].userdata = userdata;
}

uint32_t ptk_anm2_item_get_id(struct ptk_anm2 const *doc, size_t sel_idx, size_t item_idx) {
  if (!doc || !doc->selectors) {
    return 0;
  }
  if (sel_idx >= OV_ARRAY_LENGTH(doc->selectors)) {
    return 0;
  }
  struct selector const *sel = &doc->selectors[sel_idx];
  if (!sel->items || item_idx >= OV_ARRAY_LENGTH(sel->items)) {
    return 0;
  }
  return sel->items[item_idx].id;
}

static uintptr_t item_get_userdata(struct ptk_anm2 const *doc, size_t sel_idx, size_t item_idx) {
  if (!doc || !doc->selectors) {
    return 0;
  }
  if (sel_idx >= OV_ARRAY_LENGTH(doc->selectors)) {
    return 0;
  }
  struct selector const *sel = &doc->selectors[sel_idx];
  if (!sel->items || item_idx >= OV_ARRAY_LENGTH(sel->items)) {
    return 0;
  }
  return sel->items[item_idx].userdata;
}

static void item_set_userdata(struct ptk_anm2 *doc, size_t sel_idx, size_t item_idx, uintptr_t userdata) {
  if (!doc || !doc->selectors) {
    return;
  }
  if (sel_idx >= OV_ARRAY_LENGTH(doc->selectors)) {
    return;
  }
  struct selector *sel = &doc->selectors[sel_idx];
  if (!sel->items || item_idx >= OV_ARRAY_LENGTH(sel->items)) {
    return;
  }
  sel->items[item_idx].userdata = userdata;
}

uint32_t ptk_anm2_param_get_id(struct ptk_anm2 const *doc, size_t sel_idx, size_t item_idx, size_t param_idx) {
  if (!doc || !doc->selectors) {
    return 0;
  }
  if (sel_idx >= OV_ARRAY_LENGTH(doc->selectors)) {
    return 0;
  }
  struct selector const *sel = &doc->selectors[sel_idx];
  if (!sel->items || item_idx >= OV_ARRAY_LENGTH(sel->items)) {
    return 0;
  }
  struct item const *it = &sel->items[item_idx];
  if (!it->params || param_idx >= OV_ARRAY_LENGTH(it->params)) {
    return 0;
  }
  return it->params[param_idx].id;
}

uint32_t *ptk_anm2_get_item_ids(struct ptk_anm2 const *doc, uint32_t selector_id, struct ov_error *const err) {
  if (!doc || selector_id == 0) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_invalid_argument);
    return NULL;
  }

  uint32_t *ids = NULL;
  size_t sel_idx = 0;

  if (!ptk_anm2_find_selector(doc, selector_id, &sel_idx)) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_invalid_argument);
    return NULL;
  }

  struct selector const *sel = &doc->selectors[sel_idx];
  size_t const n = sel->items ? OV_ARRAY_LENGTH(sel->items) : 0;
  if (!OV_ARRAY_GROW(&ids, n > 0 ? n : 1)) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_out_of_memory);
    return NULL;
  }
  for (size_t i = 0; i < n; i++) {
    ids[i] = sel->items[i].id;
  }
  OV_ARRAY_SET_LENGTH(ids, n);

  return ids;
}

uint32_t *ptk_anm2_get_param_ids(struct ptk_anm2 const *doc, uint32_t item_id, struct ov_error *const err) {
  if (!doc || item_id == 0) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_invalid_argument);
    return NULL;
  }

  uint32_t *ids = NULL;
  size_t sel_idx = 0;
  size_t item_idx = 0;

  if (!ptk_anm2_find_item(doc, item_id, &sel_idx, &item_idx)) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_invalid_argument);
    return NULL;
  }

  struct item const *it = &doc->selectors[sel_idx].items[item_idx];
  size_t const n = it->params ? OV_ARRAY_LENGTH(it->params) : 0;
  if (!OV_ARRAY_GROW(&ids, n > 0 ? n : 1)) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_out_of_memory);
    return NULL;
  }
  for (size_t i = 0; i < n; i++) {
    ids[i] = it->params[i].id;
  }
  OV_ARRAY_SET_LENGTH(ids, n);

  return ids;
}

uint32_t ptk_anm2_param_get_item_id(struct ptk_anm2 const *doc, uint32_t param_id) {
  if (!doc || !doc->selectors || param_id == 0) {
    return 0;
  }
  size_t const sel_count = OV_ARRAY_LENGTH(doc->selectors);
  for (size_t i = 0; i < sel_count; i++) {
    struct selector const *sel = &doc->selectors[i];
    if (!sel->items) {
      continue;
    }
    size_t const item_count = OV_ARRAY_LENGTH(sel->items);
    for (size_t j = 0; j < item_count; j++) {
      struct item const *it = &sel->items[j];
      if (!it->params) {
        continue;
      }
      size_t const param_count = OV_ARRAY_LENGTH(it->params);
      for (size_t k = 0; k < param_count; k++) {
        if (it->params[k].id == param_id) {
          return it->id;
        }
      }
    }
  }
  return 0;
}

static uintptr_t param_get_userdata(struct ptk_anm2 const *doc, size_t sel_idx, size_t item_idx, size_t param_idx) {
  if (!doc || !doc->selectors) {
    return 0;
  }
  if (sel_idx >= OV_ARRAY_LENGTH(doc->selectors)) {
    return 0;
  }
  struct selector const *sel = &doc->selectors[sel_idx];
  if (!sel->items || item_idx >= OV_ARRAY_LENGTH(sel->items)) {
    return 0;
  }
  struct item const *it = &sel->items[item_idx];
  if (!it->params || param_idx >= OV_ARRAY_LENGTH(it->params)) {
    return 0;
  }
  return it->params[param_idx].userdata;
}

static void
param_set_userdata(struct ptk_anm2 *doc, size_t sel_idx, size_t item_idx, size_t param_idx, uintptr_t userdata) {
  if (!doc || !doc->selectors) {
    return;
  }
  if (sel_idx >= OV_ARRAY_LENGTH(doc->selectors)) {
    return;
  }
  struct selector *sel = &doc->selectors[sel_idx];
  if (!sel->items || item_idx >= OV_ARRAY_LENGTH(sel->items)) {
    return;
  }
  struct item *it = &sel->items[item_idx];
  if (!it->params || param_idx >= OV_ARRAY_LENGTH(it->params)) {
    return;
  }
  it->params[param_idx].userdata = userdata;
}

uintptr_t ptk_anm2_selector_get_userdata(struct ptk_anm2 const *doc, uint32_t id) {
  size_t sel_idx;
  if (!ptk_anm2_find_selector(doc, id, &sel_idx)) {
    return 0;
  }
  return selector_get_userdata(doc, sel_idx);
}

void ptk_anm2_selector_set_userdata(struct ptk_anm2 *doc, uint32_t id, uintptr_t userdata) {
  size_t sel_idx;
  if (!ptk_anm2_find_selector(doc, id, &sel_idx)) {
    return;
  }
  selector_set_userdata(doc, sel_idx, userdata);
}

uintptr_t ptk_anm2_item_get_userdata(struct ptk_anm2 const *doc, uint32_t id) {
  size_t sel_idx, item_idx;
  if (!ptk_anm2_find_item(doc, id, &sel_idx, &item_idx)) {
    return 0;
  }
  return item_get_userdata(doc, sel_idx, item_idx);
}

void ptk_anm2_item_set_userdata(struct ptk_anm2 *doc, uint32_t id, uintptr_t userdata) {
  size_t sel_idx, item_idx;
  if (!ptk_anm2_find_item(doc, id, &sel_idx, &item_idx)) {
    return;
  }
  item_set_userdata(doc, sel_idx, item_idx, userdata);
}

uintptr_t ptk_anm2_param_get_userdata(struct ptk_anm2 const *doc, uint32_t id) {
  size_t sel_idx, item_idx, param_idx;
  if (!ptk_anm2_find_param(doc, id, &sel_idx, &item_idx, &param_idx)) {
    return 0;
  }
  return param_get_userdata(doc, sel_idx, item_idx, param_idx);
}

void ptk_anm2_param_set_userdata(struct ptk_anm2 *doc, uint32_t id, uintptr_t userdata) {
  size_t sel_idx, item_idx, param_idx;
  if (!ptk_anm2_find_param(doc, id, &sel_idx, &item_idx, &param_idx)) {
    return;
  }
  param_set_userdata(doc, sel_idx, item_idx, param_idx, userdata);
}

bool ptk_anm2_find_selector(struct ptk_anm2 const *doc, uint32_t id, size_t *out_sel_idx) {
  if (!doc || !doc->selectors || id == 0) {
    return false;
  }
  size_t const n = OV_ARRAY_LENGTH(doc->selectors);
  for (size_t i = 0; i < n; i++) {
    if (doc->selectors[i].id == id) {
      if (out_sel_idx) {
        *out_sel_idx = i;
      }
      return true;
    }
  }
  return false;
}

bool ptk_anm2_find_item(struct ptk_anm2 const *doc, uint32_t id, size_t *out_sel_idx, size_t *out_item_idx) {
  if (!doc || !doc->selectors || id == 0) {
    return false;
  }
  size_t const sel_count = OV_ARRAY_LENGTH(doc->selectors);
  for (size_t sel_idx = 0; sel_idx < sel_count; sel_idx++) {
    struct selector const *sel = &doc->selectors[sel_idx];
    if (!sel->items) {
      continue;
    }
    size_t const item_count = OV_ARRAY_LENGTH(sel->items);
    for (size_t item_idx = 0; item_idx < item_count; item_idx++) {
      if (sel->items[item_idx].id == id) {
        if (out_sel_idx) {
          *out_sel_idx = sel_idx;
        }
        if (out_item_idx) {
          *out_item_idx = item_idx;
        }
        return true;
      }
    }
  }
  return false;
}

bool ptk_anm2_find_param(
    struct ptk_anm2 const *doc, uint32_t id, size_t *out_sel_idx, size_t *out_item_idx, size_t *out_param_idx) {
  if (!doc || !doc->selectors || id == 0) {
    return false;
  }
  size_t const sel_count = OV_ARRAY_LENGTH(doc->selectors);
  for (size_t sel_idx = 0; sel_idx < sel_count; sel_idx++) {
    struct selector const *sel = &doc->selectors[sel_idx];
    if (!sel->items) {
      continue;
    }
    size_t const item_count = OV_ARRAY_LENGTH(sel->items);
    for (size_t item_idx = 0; item_idx < item_count; item_idx++) {
      struct item const *it = &sel->items[item_idx];
      if (!it->params) {
        continue;
      }
      size_t const param_count = OV_ARRAY_LENGTH(it->params);
      for (size_t param_idx = 0; param_idx < param_count; param_idx++) {
        if (it->params[param_idx].id == id) {
          if (out_sel_idx) {
            *out_sel_idx = sel_idx;
          }
          if (out_item_idx) {
            *out_item_idx = item_idx;
          }
          if (out_param_idx) {
            *out_param_idx = param_idx;
          }
          return true;
        }
      }
    }
  }
  return false;
}
