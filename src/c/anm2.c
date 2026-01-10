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

// ============================================================================
// Internal data structures
// ============================================================================

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
  char *group;
  struct item *items; // ovarray
};

// enum ptk_anm2_op_type is now defined in anm2.h

struct ptk_anm2_op {
  size_t sel_idx;
  size_t item_idx;
  size_t param_idx;
  size_t move_to_sel_idx;
  size_t move_to_idx;
  enum ptk_anm2_op_type type;
  char *str_data;     // Previous string value
  void *removed_data; // Removed struct (selector/item/param)
};

struct ptk_anm2 {
  uint32_t next_id;
  int version;
  char *label;
  char *psd_path;
  char *information;              // NULL = auto-generate from psd_path
  bool exclusive_support_default; // Default value for exclusive support control checkbox
  struct selector *selectors;     // ovarray
  struct ptk_anm2_op *undo_stack; // ovarray
  struct ptk_anm2_op *redo_stack; // ovarray
  int transaction_depth;
  uint64_t stored_checksum;     // checksum from JSON metadata (set by load)
  uint64_t calculated_checksum; // checksum calculated from script body (set by load)
  ptk_anm2_change_callback change_callback;
  void *change_callback_userdata;
};

// ============================================================================
// Internal helper implementations
// ============================================================================

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
  if (sel->group) {
    OV_ARRAY_DESTROY(&sel->group);
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
    case ptk_anm2_op_group_begin:
    case ptk_anm2_op_group_end:
    case ptk_anm2_op_set_label:
    case ptk_anm2_op_set_psd_path:
    case ptk_anm2_op_set_exclusive_support_default:
    case ptk_anm2_op_set_information:
    case ptk_anm2_op_selector_set_group:
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

static void notify_change(struct ptk_anm2 const *doc,
                          enum ptk_anm2_op_type op_type,
                          size_t sel_idx,
                          size_t item_idx,
                          size_t param_idx,
                          size_t to_sel_idx,
                          size_t to_idx) {
  if (doc && doc->change_callback) {
    doc->change_callback(doc->change_callback_userdata, op_type, sel_idx, item_idx, param_idx, to_sel_idx, to_idx);
  }
}

void ptk_anm2_set_change_callback(struct ptk_anm2 *doc, ptk_anm2_change_callback callback, void *userdata) {
  if (!doc) {
    return;
  }
  doc->change_callback = callback;
  doc->change_callback_userdata = userdata;
}

// ============================================================================
// Save helpers (code generation)
// ============================================================================

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
      yyjson_mut_obj_add_strcpy(jdoc, sel_obj, "group", sel->group);

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
          sel->group ? sel->group : pgettext(".ptk.anm2 default name for unnamed selector", "Selector");
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
          if (!ov_sprintf_append_char(&body, err, "%1$s%2$zu", ",%1$s=%2$zu", display_name, j + 1)) {
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

// ============================================================================
// Document lifecycle
// ============================================================================

// Initialize document structure (does not allocate the struct itself)
static bool doc_init(struct ptk_anm2 *doc, struct ov_error *const err) {
  *doc = (struct ptk_anm2){
      .version = 1,
      .next_id = 1,
      .exclusive_support_default = true,
  };
  if (!strdup_to_array(&doc->label, pgettext(".ptk.anm2 label", "PSD"), err)) {
    OV_ERROR_ADD_TRACE(err);
    return false;
  }
  return true;
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

struct ptk_anm2 *ptk_anm2_new(struct ov_error *const err) {
  struct ptk_anm2 *doc = NULL;
  bool success = false;

  if (!OV_REALLOC(&doc, 1, sizeof(struct ptk_anm2))) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_out_of_memory);
    goto cleanup;
  }

  if (!doc_init(doc, err)) {
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

// ============================================================================
// Metadata operations (stub implementations)
// ============================================================================

char const *ptk_anm2_get_label(struct ptk_anm2 const *doc) {
  if (!doc) {
    return NULL;
  }
  return doc->label;
}

static bool push_undo_op(struct ptk_anm2 *doc, struct ptk_anm2_op const *op, struct ov_error *const err) {
  size_t const len = OV_ARRAY_LENGTH(doc->undo_stack);
  if (!OV_ARRAY_GROW(&doc->undo_stack, len + 1)) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_out_of_memory);
    return false;
  }
  doc->undo_stack[len] = *op;
  OV_ARRAY_SET_LENGTH(doc->undo_stack, len + 1);
  return true;
}

static void clear_redo_stack(struct ptk_anm2 *doc) { op_stack_clear(&doc->redo_stack); }

static bool push_redo_op(struct ptk_anm2 *doc, struct ptk_anm2_op const *op, struct ov_error *const err) {
  size_t const len = OV_ARRAY_LENGTH(doc->redo_stack);
  if (!OV_ARRAY_GROW(&doc->redo_stack, len + 1)) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_out_of_memory);
    return false;
  }
  doc->redo_stack[len] = *op;
  OV_ARRAY_SET_LENGTH(doc->redo_stack, len + 1);
  return true;
}

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

  case ptk_anm2_op_group_begin:
    // GROUP_BEGIN's reverse is GROUP_END
    reverse_op->type = ptk_anm2_op_group_end;
    break;

  case ptk_anm2_op_group_end:
    // GROUP_END's reverse is GROUP_BEGIN
    reverse_op->type = ptk_anm2_op_group_begin;
    break;

  case ptk_anm2_op_selector_insert:
    // INSERT: insert selector at op->sel_idx using op->removed_data
    // op->removed_data contains the selector to insert
    {
      size_t const idx = op->sel_idx;
      struct selector *sel = (struct selector *)op->removed_data;
      if (!sel) {
        OV_ERROR_SET_GENERIC(err, ov_error_generic_invalid_argument);
        goto cleanup;
      }

      size_t const len = OV_ARRAY_LENGTH(doc->selectors);

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

      // Reverse operation: REMOVE at that index
      reverse_op->type = ptk_anm2_op_selector_remove;
      reverse_op->sel_idx = idx;
      reverse_op->removed_data = NULL;
    }
    break;

  case ptk_anm2_op_selector_remove:
    // REMOVE: remove selector at op->sel_idx
    // Save the removed selector to reverse_op->removed_data
    {
      size_t const idx = op->sel_idx;
      size_t const len = OV_ARRAY_LENGTH(doc->selectors);
      if (idx >= len) {
        OV_ERROR_SET_GENERIC(err, ov_error_generic_invalid_argument);
        goto cleanup;
      }

      // Allocate storage for the removed selector
      struct selector *removed_sel = NULL;
      if (!OV_REALLOC(&removed_sel, 1, sizeof(struct selector))) {
        OV_ERROR_SET_GENERIC(err, ov_error_generic_out_of_memory);
        goto cleanup;
      }
      *removed_sel = doc->selectors[idx];

      // Shift remaining selectors
      for (size_t i = idx; i < len - 1; i++) {
        doc->selectors[i] = doc->selectors[i + 1];
      }
      OV_ARRAY_SET_LENGTH(doc->selectors, len - 1);

      // Reverse operation: INSERT with the saved selector
      reverse_op->type = ptk_anm2_op_selector_insert;
      reverse_op->sel_idx = idx;
      reverse_op->removed_data = removed_sel;
    }
    break;

  case ptk_anm2_op_item_insert:
    // INSERT: insert item at (op->sel_idx, op->item_idx) using op->removed_data
    // op->removed_data contains the item to insert
    {
      size_t const sidx = op->sel_idx;
      size_t const iidx = op->item_idx;
      struct item *it = (struct item *)op->removed_data;
      if (!it) {
        OV_ERROR_SET_GENERIC(err, ov_error_generic_invalid_argument);
        goto cleanup;
      }
      if (!doc->selectors || sidx >= OV_ARRAY_LENGTH(doc->selectors)) {
        OV_ERROR_SET_GENERIC(err, ov_error_generic_invalid_argument);
        goto cleanup;
      }

      struct selector *sel = &doc->selectors[sidx];
      size_t const len = OV_ARRAY_LENGTH(sel->items);

      // Grow array
      if (!OV_ARRAY_GROW(&sel->items, len + 1)) {
        OV_ERROR_SET_GENERIC(err, ov_error_generic_out_of_memory);
        goto cleanup;
      }

      // Shift elements to make room
      for (size_t i = len; i > iidx; i--) {
        sel->items[i] = sel->items[i - 1];
      }

      // Insert the item
      sel->items[iidx] = *it;
      OV_ARRAY_SET_LENGTH(sel->items, len + 1);

      // Free the container (content is now owned by doc)
      OV_FREE(&it);
      op->removed_data = NULL; // Mark as consumed

      // Reverse operation: REMOVE at that index
      reverse_op->type = ptk_anm2_op_item_remove;
      reverse_op->sel_idx = sidx;
      reverse_op->item_idx = iidx;
      reverse_op->removed_data = NULL;
    }
    break;

  case ptk_anm2_op_item_remove:
    // REMOVE: remove item at (op->sel_idx, op->item_idx)
    // Save the removed item to reverse_op->removed_data
    {
      size_t const sidx = op->sel_idx;
      size_t const iidx = op->item_idx;
      if (!doc->selectors || sidx >= OV_ARRAY_LENGTH(doc->selectors)) {
        OV_ERROR_SET_GENERIC(err, ov_error_generic_invalid_argument);
        goto cleanup;
      }
      struct selector *sel = &doc->selectors[sidx];
      size_t const len = OV_ARRAY_LENGTH(sel->items);
      if (iidx >= len) {
        OV_ERROR_SET_GENERIC(err, ov_error_generic_invalid_argument);
        goto cleanup;
      }

      // Allocate storage for the removed item
      struct item *removed_item = NULL;
      if (!OV_REALLOC(&removed_item, 1, sizeof(struct item))) {
        OV_ERROR_SET_GENERIC(err, ov_error_generic_out_of_memory);
        goto cleanup;
      }
      *removed_item = sel->items[iidx];

      // Shift remaining items
      for (size_t i = iidx; i < len - 1; i++) {
        sel->items[i] = sel->items[i + 1];
      }
      OV_ARRAY_SET_LENGTH(sel->items, len - 1);

      // Reverse operation: INSERT with the saved item
      reverse_op->type = ptk_anm2_op_item_insert;
      reverse_op->sel_idx = sidx;
      reverse_op->item_idx = iidx;
      reverse_op->removed_data = removed_item;
    }
    break;

  case ptk_anm2_op_param_insert:
    // INSERT: insert param at (op->sel_idx, op->item_idx, op->param_idx) using op->removed_data
    // op->removed_data contains the param to insert
    {
      size_t const sidx = op->sel_idx;
      size_t const iidx = op->item_idx;
      size_t const pidx = op->param_idx;
      struct param *p = (struct param *)op->removed_data;
      if (!p) {
        OV_ERROR_SET_GENERIC(err, ov_error_generic_invalid_argument);
        goto cleanup;
      }
      if (!doc->selectors || sidx >= OV_ARRAY_LENGTH(doc->selectors)) {
        OV_ERROR_SET_GENERIC(err, ov_error_generic_invalid_argument);
        goto cleanup;
      }
      struct selector *sel = &doc->selectors[sidx];
      if (!sel->items || iidx >= OV_ARRAY_LENGTH(sel->items)) {
        OV_ERROR_SET_GENERIC(err, ov_error_generic_invalid_argument);
        goto cleanup;
      }

      struct item *it = &sel->items[iidx];
      size_t const len = OV_ARRAY_LENGTH(it->params);

      // Grow array
      if (!OV_ARRAY_GROW(&it->params, len + 1)) {
        OV_ERROR_SET_GENERIC(err, ov_error_generic_out_of_memory);
        goto cleanup;
      }

      // Shift elements to make room
      for (size_t i = len; i > pidx; i--) {
        it->params[i] = it->params[i - 1];
      }

      // Insert the param
      it->params[pidx] = *p;
      OV_ARRAY_SET_LENGTH(it->params, len + 1);

      // Free the container (content is now owned by doc)
      OV_FREE(&p);
      op->removed_data = NULL; // Mark as consumed

      // Reverse operation: REMOVE at that index
      reverse_op->type = ptk_anm2_op_param_remove;
      reverse_op->sel_idx = sidx;
      reverse_op->item_idx = iidx;
      reverse_op->param_idx = pidx;
      reverse_op->removed_data = NULL;
    }
    break;

  case ptk_anm2_op_param_remove:
    // REMOVE: remove param at (op->sel_idx, op->item_idx, op->param_idx)
    // Save the removed param to reverse_op->removed_data
    {
      size_t const sidx = op->sel_idx;
      size_t const iidx = op->item_idx;
      size_t const pidx = op->param_idx;
      if (!doc->selectors || sidx >= OV_ARRAY_LENGTH(doc->selectors)) {
        OV_ERROR_SET_GENERIC(err, ov_error_generic_invalid_argument);
        goto cleanup;
      }
      struct selector *sel = &doc->selectors[sidx];
      if (!sel->items || iidx >= OV_ARRAY_LENGTH(sel->items)) {
        OV_ERROR_SET_GENERIC(err, ov_error_generic_invalid_argument);
        goto cleanup;
      }
      struct item *it = &sel->items[iidx];
      size_t const len = OV_ARRAY_LENGTH(it->params);
      if (pidx >= len) {
        OV_ERROR_SET_GENERIC(err, ov_error_generic_invalid_argument);
        goto cleanup;
      }

      // Allocate storage for the removed param
      struct param *removed_param = NULL;
      if (!OV_REALLOC(&removed_param, 1, sizeof(struct param))) {
        OV_ERROR_SET_GENERIC(err, ov_error_generic_out_of_memory);
        goto cleanup;
      }
      *removed_param = it->params[pidx];

      // Shift remaining params
      for (size_t i = pidx; i < len - 1; i++) {
        it->params[i] = it->params[i + 1];
      }
      OV_ARRAY_SET_LENGTH(it->params, len - 1);

      // Reverse operation: INSERT with the saved param
      reverse_op->type = ptk_anm2_op_param_insert;
      reverse_op->sel_idx = sidx;
      reverse_op->item_idx = iidx;
      reverse_op->param_idx = pidx;
      reverse_op->removed_data = removed_param;
    }
    break;

  case ptk_anm2_op_selector_set_group:
    // Save current value as reverse, apply op->str_data
    {
      size_t const sidx = op->sel_idx;
      if (!doc->selectors || sidx >= OV_ARRAY_LENGTH(doc->selectors)) {
        OV_ERROR_SET_GENERIC(err, ov_error_generic_invalid_argument);
        goto cleanup;
      }
      struct selector *sel = &doc->selectors[sidx];
      if (!strdup_to_array(&reverse_op->str_data, sel->group, err)) {
        OV_ERROR_ADD_TRACE(err);
        goto cleanup;
      }
      if (!strdup_to_array(&sel->group, op->str_data, err)) {
        OV_ERROR_ADD_TRACE(err);
        goto cleanup;
      }
      reverse_op->sel_idx = sidx;
    }
    break;

  case ptk_anm2_op_selector_move:
    // MOVE: move selector from op->sel_idx to op->move_to_idx
    {
      size_t const from = op->sel_idx;
      size_t const to = op->move_to_idx;
      size_t const len = OV_ARRAY_LENGTH(doc->selectors);
      if (from >= len || to >= len) {
        OV_ERROR_SET_GENERIC(err, ov_error_generic_invalid_argument);
        goto cleanup;
      }
      if (from != to) {
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
      // Reverse operation: move from 'to' back to 'from'
      reverse_op->sel_idx = to;
      reverse_op->move_to_idx = from;
    }
    break;

  case ptk_anm2_op_item_set_name:
    // Save current value as reverse, apply op->str_data
    {
      size_t const sidx = op->sel_idx;
      size_t const iidx = op->item_idx;
      if (!doc->selectors || sidx >= OV_ARRAY_LENGTH(doc->selectors)) {
        OV_ERROR_SET_GENERIC(err, ov_error_generic_invalid_argument);
        goto cleanup;
      }
      struct selector *sel = &doc->selectors[sidx];
      if (!sel->items || iidx >= OV_ARRAY_LENGTH(sel->items)) {
        OV_ERROR_SET_GENERIC(err, ov_error_generic_invalid_argument);
        goto cleanup;
      }
      struct item *it = &sel->items[iidx];
      if (!strdup_to_array(&reverse_op->str_data, it->name, err)) {
        OV_ERROR_ADD_TRACE(err);
        goto cleanup;
      }
      if (!strdup_to_array(&it->name, op->str_data, err)) {
        OV_ERROR_ADD_TRACE(err);
        goto cleanup;
      }
      reverse_op->sel_idx = sidx;
      reverse_op->item_idx = iidx;
    }
    break;

  case ptk_anm2_op_item_set_value:
    // Save current value as reverse, apply op->str_data
    {
      size_t const sidx = op->sel_idx;
      size_t const iidx = op->item_idx;
      if (!doc->selectors || sidx >= OV_ARRAY_LENGTH(doc->selectors)) {
        OV_ERROR_SET_GENERIC(err, ov_error_generic_invalid_argument);
        goto cleanup;
      }
      struct selector *sel = &doc->selectors[sidx];
      if (!sel->items || iidx >= OV_ARRAY_LENGTH(sel->items)) {
        OV_ERROR_SET_GENERIC(err, ov_error_generic_invalid_argument);
        goto cleanup;
      }
      struct item *it = &sel->items[iidx];
      if (!strdup_to_array(&reverse_op->str_data, it->value, err)) {
        OV_ERROR_ADD_TRACE(err);
        goto cleanup;
      }
      if (!strdup_to_array(&it->value, op->str_data, err)) {
        OV_ERROR_ADD_TRACE(err);
        goto cleanup;
      }
      reverse_op->sel_idx = sidx;
      reverse_op->item_idx = iidx;
    }
    break;

  case ptk_anm2_op_item_set_script_name:
    // Save current value as reverse, apply op->str_data
    {
      size_t const sidx = op->sel_idx;
      size_t const iidx = op->item_idx;
      if (!doc->selectors || sidx >= OV_ARRAY_LENGTH(doc->selectors)) {
        OV_ERROR_SET_GENERIC(err, ov_error_generic_invalid_argument);
        goto cleanup;
      }
      struct selector *sel = &doc->selectors[sidx];
      if (!sel->items || iidx >= OV_ARRAY_LENGTH(sel->items)) {
        OV_ERROR_SET_GENERIC(err, ov_error_generic_invalid_argument);
        goto cleanup;
      }
      struct item *it = &sel->items[iidx];
      if (!strdup_to_array(&reverse_op->str_data, it->script_name, err)) {
        OV_ERROR_ADD_TRACE(err);
        goto cleanup;
      }
      if (!strdup_to_array(&it->script_name, op->str_data, err)) {
        OV_ERROR_ADD_TRACE(err);
        goto cleanup;
      }
      reverse_op->sel_idx = sidx;
      reverse_op->item_idx = iidx;
    }
    break;

  case ptk_anm2_op_item_move:
    // MOVE: move item from (op->sel_idx, op->item_idx) to (op->move_to_sel_idx, op->move_to_idx)
    {
      size_t const from_sidx = op->sel_idx;
      size_t const from_iidx = op->item_idx;
      size_t const to_sidx = op->move_to_sel_idx;
      size_t const to_iidx = op->move_to_idx;

      if (!doc->selectors || from_sidx >= OV_ARRAY_LENGTH(doc->selectors) ||
          to_sidx >= OV_ARRAY_LENGTH(doc->selectors)) {
        OV_ERROR_SET_GENERIC(err, ov_error_generic_invalid_argument);
        goto cleanup;
      }

      struct selector *from_sel = &doc->selectors[from_sidx];
      struct selector *to_sel = &doc->selectors[to_sidx];

      size_t const from_len = OV_ARRAY_LENGTH(from_sel->items);
      if (from_iidx >= from_len) {
        OV_ERROR_SET_GENERIC(err, ov_error_generic_invalid_argument);
        goto cleanup;
      }

      if (from_sidx == to_sidx) {
        // Same selector move
        size_t const len = from_len;
        if (to_iidx >= len) {
          OV_ERROR_SET_GENERIC(err, ov_error_generic_invalid_argument);
          goto cleanup;
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
        // Cross-selector move
        size_t const to_len = OV_ARRAY_LENGTH(to_sel->items);
        if (to_iidx > to_len) {
          OV_ERROR_SET_GENERIC(err, ov_error_generic_invalid_argument);
          goto cleanup;
        }

        struct item tmp = from_sel->items[from_iidx];

        // Remove from source
        for (size_t i = from_iidx; i < from_len - 1; i++) {
          from_sel->items[i] = from_sel->items[i + 1];
        }
        OV_ARRAY_SET_LENGTH(from_sel->items, from_len - 1);

        // Insert into destination
        if (!OV_ARRAY_GROW(&to_sel->items, to_len + 1)) {
          // Rollback
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

      // Reverse operation: move from (to_sidx, to_iidx) back to (from_sidx, from_iidx)
      reverse_op->sel_idx = to_sidx;
      reverse_op->item_idx = to_iidx;
      reverse_op->move_to_sel_idx = from_sidx;
      reverse_op->move_to_idx = from_iidx;
    }
    break;

  case ptk_anm2_op_param_set_key:
    // Save current value as reverse, apply op->str_data
    {
      size_t const sidx = op->sel_idx;
      size_t const iidx = op->item_idx;
      size_t const pidx = op->param_idx;
      if (!doc->selectors || sidx >= OV_ARRAY_LENGTH(doc->selectors)) {
        OV_ERROR_SET_GENERIC(err, ov_error_generic_invalid_argument);
        goto cleanup;
      }
      struct selector *sel = &doc->selectors[sidx];
      if (!sel->items || iidx >= OV_ARRAY_LENGTH(sel->items)) {
        OV_ERROR_SET_GENERIC(err, ov_error_generic_invalid_argument);
        goto cleanup;
      }
      struct item *it = &sel->items[iidx];
      if (!it->params || pidx >= OV_ARRAY_LENGTH(it->params)) {
        OV_ERROR_SET_GENERIC(err, ov_error_generic_invalid_argument);
        goto cleanup;
      }
      struct param *p = &it->params[pidx];
      if (!strdup_to_array(&reverse_op->str_data, p->key, err)) {
        OV_ERROR_ADD_TRACE(err);
        goto cleanup;
      }
      if (!strdup_to_array(&p->key, op->str_data, err)) {
        OV_ERROR_ADD_TRACE(err);
        goto cleanup;
      }
      reverse_op->sel_idx = sidx;
      reverse_op->item_idx = iidx;
      reverse_op->param_idx = pidx;
    }
    break;

  case ptk_anm2_op_param_set_value:
    // Save current value as reverse, apply op->str_data
    {
      size_t const sidx = op->sel_idx;
      size_t const iidx = op->item_idx;
      size_t const pidx = op->param_idx;
      if (!doc->selectors || sidx >= OV_ARRAY_LENGTH(doc->selectors)) {
        OV_ERROR_SET_GENERIC(err, ov_error_generic_invalid_argument);
        goto cleanup;
      }
      struct selector *sel = &doc->selectors[sidx];
      if (!sel->items || iidx >= OV_ARRAY_LENGTH(sel->items)) {
        OV_ERROR_SET_GENERIC(err, ov_error_generic_invalid_argument);
        goto cleanup;
      }
      struct item *it = &sel->items[iidx];
      if (!it->params || pidx >= OV_ARRAY_LENGTH(it->params)) {
        OV_ERROR_SET_GENERIC(err, ov_error_generic_invalid_argument);
        goto cleanup;
      }
      struct param *p = &it->params[pidx];
      if (!strdup_to_array(&reverse_op->str_data, p->value, err)) {
        OV_ERROR_ADD_TRACE(err);
        goto cleanup;
      }
      if (!strdup_to_array(&p->value, op->str_data, err)) {
        OV_ERROR_ADD_TRACE(err);
        goto cleanup;
      }
      reverse_op->sel_idx = sidx;
      reverse_op->item_idx = iidx;
      reverse_op->param_idx = pidx;
    }
    break;

  case ptk_anm2_op_reset:
    // RESET is not used as an operation, only for notification
    OV_ERROR_SET_GENERIC(err, ov_error_generic_invalid_argument);
    goto cleanup;
  }

  // Notify change callback
  // Now that apply_op executes the operation as named, notify with the operation type directly.
  switch (op->type) {
  case ptk_anm2_op_set_label:
  case ptk_anm2_op_set_psd_path:
  case ptk_anm2_op_set_exclusive_support_default:
  case ptk_anm2_op_set_information:
    notify_change(doc, op->type, 0, 0, 0, 0, 0);
    break;
  case ptk_anm2_op_selector_insert:
  case ptk_anm2_op_selector_remove:
    notify_change(doc, op->type, op->sel_idx, 0, 0, 0, 0);
    break;
  case ptk_anm2_op_selector_set_group:
    notify_change(doc, op->type, op->sel_idx, 0, 0, 0, 0);
    break;
  case ptk_anm2_op_selector_move:
    // Moved from op->sel_idx to op->move_to_idx
    notify_change(doc, op->type, op->sel_idx, 0, 0, op->move_to_idx, 0);
    break;
  case ptk_anm2_op_item_insert:
  case ptk_anm2_op_item_remove:
    notify_change(doc, op->type, op->sel_idx, op->item_idx, 0, 0, 0);
    break;
  case ptk_anm2_op_item_set_name:
  case ptk_anm2_op_item_set_value:
  case ptk_anm2_op_item_set_script_name:
    notify_change(doc, op->type, op->sel_idx, op->item_idx, 0, 0, 0);
    break;
  case ptk_anm2_op_item_move:
    // Moved from (op->sel_idx, op->item_idx) to (op->move_to_sel_idx, op->move_to_idx)
    notify_change(doc, op->type, op->sel_idx, op->item_idx, 0, op->move_to_sel_idx, op->move_to_idx);
    break;
  case ptk_anm2_op_param_insert:
  case ptk_anm2_op_param_remove:
    notify_change(doc, op->type, op->sel_idx, op->item_idx, op->param_idx, 0, 0);
    break;
  case ptk_anm2_op_param_set_key:
  case ptk_anm2_op_param_set_value:
    notify_change(doc, op->type, op->sel_idx, op->item_idx, op->param_idx, 0, 0);
    break;
  case ptk_anm2_op_group_begin:
  case ptk_anm2_op_group_end:
    notify_change(doc, op->type, 0, 0, 0, 0, 0);
    break;
  case ptk_anm2_op_reset:
    // RESET is not used in apply_op, only for direct notification
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

// ============================================================================
// Selector operations (stub implementations)
// ============================================================================

size_t ptk_anm2_selector_count(struct ptk_anm2 const *doc) {
  if (!doc || !doc->selectors) {
    return 0;
  }
  return OV_ARRAY_LENGTH(doc->selectors);
}

uint32_t ptk_anm2_selector_add(struct ptk_anm2 *doc, char const *group, struct ov_error *const err) {
  if (!doc) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_invalid_argument);
    return 0;
  }

  uint32_t result_id = 0;
  uint32_t new_id = 0;
  struct selector *new_sel = NULL;
  struct ptk_anm2_op op = {0};
  struct ptk_anm2_op reverse_op = {0};

  // Use default name if group is empty or NULL
  char const *effective_group =
      (group && group[0] != '\0') ? group : pgettext(".ptk.anm2 default selector name", "Unnamed Selector");

  // Allocate and initialize new selector
  if (!OV_REALLOC(&new_sel, 1, sizeof(struct selector))) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_out_of_memory);
    goto cleanup;
  }
  memset(new_sel, 0, sizeof(struct selector));
  new_sel->id = generate_id(doc);
  new_id = new_sel->id;

  // Copy group name
  if (!strdup_to_array(&new_sel->group, effective_group, err)) {
    OV_ERROR_ADD_TRACE(err);
    goto cleanup;
  }

  // Build INSERT operation
  {
    size_t const idx = OV_ARRAY_LENGTH(doc->selectors);
    op.type = ptk_anm2_op_selector_insert;
    op.sel_idx = idx;
    op.removed_data = new_sel;
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

bool ptk_anm2_selector_remove(struct ptk_anm2 *doc, size_t idx, struct ov_error *const err) {
  if (!doc) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_invalid_argument);
    return false;
  }
  if (!doc->selectors || idx >= OV_ARRAY_LENGTH(doc->selectors)) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_invalid_argument);
    return false;
  }

  bool success = false;
  struct ptk_anm2_op op = {0};
  struct ptk_anm2_op reverse_op = {0};

  // Build REMOVE operation
  op.type = ptk_anm2_op_selector_remove;
  op.sel_idx = idx;

  // Apply the operation (removes the selector, reverse_op will contain the data)
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

  success = true;

cleanup:
  op_free(&op);
  op_free(&reverse_op);
  return success;
}

char const *ptk_anm2_selector_get_group(struct ptk_anm2 const *doc, size_t idx) {
  if (!doc || !doc->selectors) {
    return NULL;
  }
  if (idx >= OV_ARRAY_LENGTH(doc->selectors)) {
    return NULL;
  }
  return doc->selectors[idx].group;
}

bool ptk_anm2_selector_set_group(struct ptk_anm2 *doc, size_t idx, char const *group, struct ov_error *const err) {
  if (!doc) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_invalid_argument);
    return false;
  }
  if (!doc->selectors || idx >= OV_ARRAY_LENGTH(doc->selectors)) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_invalid_argument);
    return false;
  }

  bool success = false;
  struct ptk_anm2_op op = {0};
  struct ptk_anm2_op reverse_op = {0};

  // Build SET_GROUP operation
  if (!strdup_to_array(&op.str_data, group, err)) {
    OV_ERROR_ADD_TRACE(err);
    goto cleanup;
  }
  op.type = ptk_anm2_op_selector_set_group;
  op.sel_idx = idx;

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

  success = true;

cleanup:
  op_free(&op);
  op_free(&reverse_op);
  return success;
}

bool ptk_anm2_selector_move_to(struct ptk_anm2 *doc, size_t from_idx, size_t to_idx, struct ov_error *const err) {
  if (!doc) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_invalid_argument);
    return false;
  }
  if (!doc->selectors) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_invalid_argument);
    return false;
  }
  size_t const len = OV_ARRAY_LENGTH(doc->selectors);
  if (from_idx >= len || to_idx >= len) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_invalid_argument);
    return false;
  }

  bool success = false;
  struct ptk_anm2_op op = {0};
  struct ptk_anm2_op reverse_op = {0};

  // Build MOVE operation
  op.type = ptk_anm2_op_selector_move;
  op.sel_idx = from_idx;
  op.move_to_idx = to_idx;

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

  success = true;

cleanup:
  op_free(&op);
  op_free(&reverse_op);
  return success;
}

// ============================================================================
// Item operations (stub implementations)
// ============================================================================

size_t ptk_anm2_item_count(struct ptk_anm2 const *doc, size_t sel_idx) {
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

bool ptk_anm2_item_is_animation(struct ptk_anm2 const *doc, size_t sel_idx, size_t item_idx) {
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

uint32_t ptk_anm2_item_add_value(
    struct ptk_anm2 *doc, size_t sel_idx, char const *name, char const *value, struct ov_error *const err) {
  if (!doc) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_invalid_argument);
    return 0;
  }
  if (!doc->selectors || sel_idx >= OV_ARRAY_LENGTH(doc->selectors)) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_invalid_argument);
    return 0;
  }

  uint32_t result_id = 0;
  uint32_t new_id = 0;
  struct item *new_item = NULL;
  struct ptk_anm2_op op = {0};
  struct ptk_anm2_op reverse_op = {0};
  struct selector *sel = &doc->selectors[sel_idx];

  // Allocate and initialize new item
  if (!OV_REALLOC(&new_item, 1, sizeof(struct item))) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_out_of_memory);
    goto cleanup;
  }
  memset(new_item, 0, sizeof(struct item));
  new_item->id = generate_id(doc);
  new_id = new_item->id;

  // Copy name
  if (!strdup_to_array(&new_item->name, name, err)) {
    OV_ERROR_ADD_TRACE(err);
    goto cleanup;
  }

  // Copy value
  if (!strdup_to_array(&new_item->value, value, err)) {
    OV_ERROR_ADD_TRACE(err);
    goto cleanup;
  }

  // Build INSERT operation
  {
    size_t const idx = OV_ARRAY_LENGTH(sel->items);
    op.type = ptk_anm2_op_item_insert;
    op.sel_idx = sel_idx;
    op.item_idx = idx;
    op.removed_data = new_item;
    new_item = NULL; // ownership transferred to op
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

uint32_t ptk_anm2_item_insert_value(struct ptk_anm2 *doc,
                                    size_t sel_idx,
                                    size_t item_idx,
                                    char const *name,
                                    char const *value,
                                    struct ov_error *const err) {
  if (!doc) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_invalid_argument);
    return 0;
  }
  if (!doc->selectors || sel_idx >= OV_ARRAY_LENGTH(doc->selectors)) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_invalid_argument);
    return 0;
  }

  uint32_t result_id = 0;
  uint32_t new_id = 0;
  struct item *new_item = NULL;
  struct ptk_anm2_op op = {0};
  struct ptk_anm2_op reverse_op = {0};
  struct selector *sel = &doc->selectors[sel_idx];
  size_t const len = OV_ARRAY_LENGTH(sel->items);

  if (item_idx > len) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_invalid_argument);
    return 0;
  }

  // Allocate and initialize new item
  if (!OV_REALLOC(&new_item, 1, sizeof(struct item))) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_out_of_memory);
    goto cleanup;
  }
  memset(new_item, 0, sizeof(struct item));
  new_item->id = generate_id(doc);
  new_id = new_item->id;

  // Copy name
  if (!strdup_to_array(&new_item->name, name, err)) {
    OV_ERROR_ADD_TRACE(err);
    goto cleanup;
  }

  // Copy value
  if (!strdup_to_array(&new_item->value, value, err)) {
    OV_ERROR_ADD_TRACE(err);
    goto cleanup;
  }

  // Build INSERT operation
  op.type = ptk_anm2_op_item_insert;
  op.sel_idx = sel_idx;
  op.item_idx = item_idx;
  op.removed_data = new_item;
  new_item = NULL; // ownership transferred to op

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

uint32_t ptk_anm2_item_add_animation(
    struct ptk_anm2 *doc, size_t sel_idx, char const *script_name, char const *name, struct ov_error *const err) {
  if (!doc) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_invalid_argument);
    return 0;
  }
  if (!doc->selectors || sel_idx >= OV_ARRAY_LENGTH(doc->selectors)) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_invalid_argument);
    return 0;
  }

  uint32_t result_id = 0;
  uint32_t new_id = 0;
  struct item *new_item = NULL;
  struct ptk_anm2_op op = {0};
  struct ptk_anm2_op reverse_op = {0};
  struct selector *sel = &doc->selectors[sel_idx];

  // Allocate and initialize new item
  if (!OV_REALLOC(&new_item, 1, sizeof(struct item))) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_out_of_memory);
    goto cleanup;
  }
  memset(new_item, 0, sizeof(struct item));
  new_item->id = generate_id(doc);
  new_id = new_item->id;

  // Copy script_name
  if (!strdup_to_array(&new_item->script_name, script_name, err)) {
    OV_ERROR_ADD_TRACE(err);
    goto cleanup;
  }

  // Copy name
  if (!strdup_to_array(&new_item->name, name, err)) {
    OV_ERROR_ADD_TRACE(err);
    goto cleanup;
  }

  // Build INSERT operation
  {
    size_t const idx = OV_ARRAY_LENGTH(sel->items);
    op.type = ptk_anm2_op_item_insert;
    op.sel_idx = sel_idx;
    op.item_idx = idx;
    op.removed_data = new_item;
    new_item = NULL; // ownership transferred to op
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

uint32_t ptk_anm2_item_insert_animation(struct ptk_anm2 *doc,
                                        size_t sel_idx,
                                        size_t item_idx,
                                        char const *script_name,
                                        char const *name,
                                        struct ov_error *const err) {
  if (!doc) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_invalid_argument);
    return 0;
  }
  if (!doc->selectors || sel_idx >= OV_ARRAY_LENGTH(doc->selectors)) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_invalid_argument);
    return 0;
  }

  uint32_t result_id = 0;
  uint32_t new_id = 0;
  struct item *new_item = NULL;
  struct ptk_anm2_op op = {0};
  struct ptk_anm2_op reverse_op = {0};
  struct selector *sel = &doc->selectors[sel_idx];
  size_t const len = OV_ARRAY_LENGTH(sel->items);

  // item_idx must be <= len (insert at end is allowed)
  if (item_idx > len) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_invalid_argument);
    return 0;
  }

  // Allocate and initialize new item
  if (!OV_REALLOC(&new_item, 1, sizeof(struct item))) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_out_of_memory);
    goto cleanup;
  }
  memset(new_item, 0, sizeof(struct item));
  new_item->id = generate_id(doc);
  new_id = new_item->id;

  // Copy script_name
  if (!strdup_to_array(&new_item->script_name, script_name, err)) {
    OV_ERROR_ADD_TRACE(err);
    goto cleanup;
  }

  // Copy name
  if (!strdup_to_array(&new_item->name, name, err)) {
    OV_ERROR_ADD_TRACE(err);
    goto cleanup;
  }

  // Build INSERT operation
  op.type = ptk_anm2_op_item_insert;
  op.sel_idx = sel_idx;
  op.item_idx = item_idx;
  op.removed_data = new_item;
  new_item = NULL; // ownership transferred to op

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

bool ptk_anm2_item_remove(struct ptk_anm2 *doc, size_t sel_idx, size_t item_idx, struct ov_error *const err) {
  if (!doc) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_invalid_argument);
    return false;
  }
  if (!doc->selectors || sel_idx >= OV_ARRAY_LENGTH(doc->selectors)) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_invalid_argument);
    return false;
  }

  struct selector *sel = &doc->selectors[sel_idx];
  if (!sel->items || item_idx >= OV_ARRAY_LENGTH(sel->items)) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_invalid_argument);
    return false;
  }

  bool success = false;
  struct ptk_anm2_op op = {0};
  struct ptk_anm2_op reverse_op = {0};

  // Build REMOVE operation
  op.type = ptk_anm2_op_item_remove;
  op.sel_idx = sel_idx;
  op.item_idx = item_idx;

  // Apply the operation (removes the item, reverse_op will contain the data)
  if (!apply_op(doc, &op, &reverse_op, err)) {
    OV_ERROR_ADD_TRACE(err);
    goto cleanup;
  }

  // Push reverse operation (INSERT) to undo stack
  if (!push_undo_op(doc, &reverse_op, err)) {
    OV_ERROR_ADD_TRACE(err);
    goto cleanup;
  }
  memset(&reverse_op, 0, sizeof(reverse_op)); // ownership transferred

  // Clear redo stack
  clear_redo_stack(doc);

  success = true;

cleanup:
  op_free(&op);
  op_free(&reverse_op);
  return success;
}

bool ptk_anm2_item_move_to(struct ptk_anm2 *doc,
                           size_t from_sel_idx,
                           size_t from_idx,
                           size_t to_sel_idx,
                           size_t to_idx,
                           struct ov_error *const err) {
  if (!doc) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_invalid_argument);
    return false;
  }
  if (!doc->selectors || from_sel_idx >= OV_ARRAY_LENGTH(doc->selectors) ||
      to_sel_idx >= OV_ARRAY_LENGTH(doc->selectors)) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_invalid_argument);
    return false;
  }

  struct selector *from_sel = &doc->selectors[from_sel_idx];
  if (!from_sel->items) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_invalid_argument);
    return false;
  }

  size_t const from_len = OV_ARRAY_LENGTH(from_sel->items);
  if (from_idx >= from_len) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_invalid_argument);
    return false;
  }

  // Same selector move: to_idx must be < len
  if (from_sel_idx == to_sel_idx) {
    if (to_idx >= from_len) {
      OV_ERROR_SET_GENERIC(err, ov_error_generic_invalid_argument);
      return false;
    }
    if (from_idx == to_idx) {
      return true; // No-op
    }
  } else {
    // Cross-selector move: to_idx can be <= to_len (insert at end allowed)
    struct selector *to_sel = &doc->selectors[to_sel_idx];
    size_t const to_len = OV_ARRAY_LENGTH(to_sel->items);
    if (to_idx > to_len) {
      OV_ERROR_SET_GENERIC(err, ov_error_generic_invalid_argument);
      return false;
    }
  }

  bool success = false;
  struct ptk_anm2_op op = {0};
  struct ptk_anm2_op reverse_op = {0};

  // Build MOVE operation
  op.type = ptk_anm2_op_item_move;
  op.sel_idx = from_sel_idx;
  op.item_idx = from_idx;
  op.move_to_sel_idx = to_sel_idx;
  op.move_to_idx = to_idx;

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

  success = true;

cleanup:
  op_free(&op);
  op_free(&reverse_op);
  return success;
}

char const *ptk_anm2_item_get_name(struct ptk_anm2 const *doc, size_t sel_idx, size_t item_idx) {
  if (!doc || !doc->selectors) {
    return NULL;
  }
  if (sel_idx >= OV_ARRAY_LENGTH(doc->selectors)) {
    return NULL;
  }
  struct selector const *sel = &doc->selectors[sel_idx];
  if (!sel->items || item_idx >= OV_ARRAY_LENGTH(sel->items)) {
    return NULL;
  }
  return sel->items[item_idx].name;
}

bool ptk_anm2_item_set_name(
    struct ptk_anm2 *doc, size_t sel_idx, size_t item_idx, char const *name, struct ov_error *const err) {
  if (!doc) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_invalid_argument);
    return false;
  }
  if (!doc->selectors || sel_idx >= OV_ARRAY_LENGTH(doc->selectors)) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_invalid_argument);
    return false;
  }
  struct selector *sel = &doc->selectors[sel_idx];
  if (!sel->items || item_idx >= OV_ARRAY_LENGTH(sel->items)) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_invalid_argument);
    return false;
  }

  bool success = false;
  struct ptk_anm2_op op = {0};
  struct ptk_anm2_op reverse_op = {0};

  // Build SET_NAME operation
  if (!strdup_to_array(&op.str_data, name, err)) {
    OV_ERROR_ADD_TRACE(err);
    goto cleanup;
  }
  op.type = ptk_anm2_op_item_set_name;
  op.sel_idx = sel_idx;
  op.item_idx = item_idx;

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

  success = true;

cleanup:
  op_free(&op);
  op_free(&reverse_op);
  return success;
}

char const *ptk_anm2_item_get_value(struct ptk_anm2 const *doc, size_t sel_idx, size_t item_idx) {
  if (!doc || !doc->selectors) {
    return NULL;
  }
  if (sel_idx >= OV_ARRAY_LENGTH(doc->selectors)) {
    return NULL;
  }
  struct selector const *sel = &doc->selectors[sel_idx];
  if (!sel->items || item_idx >= OV_ARRAY_LENGTH(sel->items)) {
    return NULL;
  }
  // Return NULL for animation items
  if (sel->items[item_idx].script_name != NULL) {
    return NULL;
  }
  return sel->items[item_idx].value;
}

bool ptk_anm2_item_set_value(
    struct ptk_anm2 *doc, size_t sel_idx, size_t item_idx, char const *value, struct ov_error *const err) {
  if (!doc) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_invalid_argument);
    return false;
  }
  if (!doc->selectors || sel_idx >= OV_ARRAY_LENGTH(doc->selectors)) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_invalid_argument);
    return false;
  }
  struct selector *sel = &doc->selectors[sel_idx];
  if (!sel->items || item_idx >= OV_ARRAY_LENGTH(sel->items)) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_invalid_argument);
    return false;
  }
  // Cannot set value on animation items
  if (sel->items[item_idx].script_name != NULL) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_invalid_argument);
    return false;
  }

  bool success = false;
  struct ptk_anm2_op op = {0};
  struct ptk_anm2_op reverse_op = {0};

  // Build SET_VALUE operation
  if (!strdup_to_array(&op.str_data, value, err)) {
    OV_ERROR_ADD_TRACE(err);
    goto cleanup;
  }
  op.type = ptk_anm2_op_item_set_value;
  op.sel_idx = sel_idx;
  op.item_idx = item_idx;

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

  success = true;

cleanup:
  op_free(&op);
  op_free(&reverse_op);
  return success;
}

char const *ptk_anm2_item_get_script_name(struct ptk_anm2 const *doc, size_t sel_idx, size_t item_idx) {
  if (!doc || !doc->selectors) {
    return NULL;
  }
  if (sel_idx >= OV_ARRAY_LENGTH(doc->selectors)) {
    return NULL;
  }
  struct selector const *sel = &doc->selectors[sel_idx];
  if (!sel->items || item_idx >= OV_ARRAY_LENGTH(sel->items)) {
    return NULL;
  }
  return sel->items[item_idx].script_name;
}

bool ptk_anm2_item_set_script_name(
    struct ptk_anm2 *doc, size_t sel_idx, size_t item_idx, char const *script_name, struct ov_error *const err) {
  if (!doc) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_invalid_argument);
    return false;
  }
  if (!doc->selectors || sel_idx >= OV_ARRAY_LENGTH(doc->selectors)) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_invalid_argument);
    return false;
  }
  struct selector *sel = &doc->selectors[sel_idx];
  if (!sel->items || item_idx >= OV_ARRAY_LENGTH(sel->items)) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_invalid_argument);
    return false;
  }
  // Cannot set script_name on value items (script_name is NULL for value items)
  if (sel->items[item_idx].script_name == NULL) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_invalid_argument);
    return false;
  }

  bool success = false;
  struct ptk_anm2_op op = {0};
  struct ptk_anm2_op reverse_op = {0};

  // Build SET_SCRIPT_NAME operation
  if (!strdup_to_array(&op.str_data, script_name, err)) {
    OV_ERROR_ADD_TRACE(err);
    goto cleanup;
  }
  op.type = ptk_anm2_op_item_set_script_name;
  op.sel_idx = sel_idx;
  op.item_idx = item_idx;

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

  success = true;

cleanup:
  op_free(&op);
  op_free(&reverse_op);
  return success;
}

// ============================================================================
// Parameter operations (stub implementations)
// ============================================================================

size_t ptk_anm2_param_count(struct ptk_anm2 const *doc, size_t sel_idx, size_t item_idx) {
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
  // Value items have no params
  if (it->script_name == NULL || !it->params) {
    return 0;
  }
  return OV_ARRAY_LENGTH(it->params);
}

uint32_t ptk_anm2_param_add(struct ptk_anm2 *doc,
                            size_t sel_idx,
                            size_t item_idx,
                            char const *key,
                            char const *value,
                            struct ov_error *const err) {
  if (!doc) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_invalid_argument);
    return 0;
  }
  if (!doc->selectors || sel_idx >= OV_ARRAY_LENGTH(doc->selectors)) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_invalid_argument);
    return 0;
  }
  struct selector *sel = &doc->selectors[sel_idx];
  if (!sel->items || item_idx >= OV_ARRAY_LENGTH(sel->items)) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_invalid_argument);
    return 0;
  }
  struct item *it = &sel->items[item_idx];
  // Cannot add params to value items
  if (it->script_name == NULL) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_invalid_argument);
    return 0;
  }

  uint32_t result_id = 0;
  uint32_t new_id = 0;
  struct param *new_param = NULL;
  struct ptk_anm2_op op = {0};
  struct ptk_anm2_op reverse_op = {0};

  // Allocate and initialize new param
  if (!OV_REALLOC(&new_param, 1, sizeof(struct param))) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_out_of_memory);
    goto cleanup;
  }
  memset(new_param, 0, sizeof(struct param));
  new_param->id = generate_id(doc);
  new_id = new_param->id;

  // Create the param
  if (!strdup_to_array(&new_param->key, key, err)) {
    OV_ERROR_ADD_TRACE(err);
    goto cleanup;
  }
  if (!strdup_to_array(&new_param->value, value, err)) {
    OV_ERROR_ADD_TRACE(err);
    goto cleanup;
  }

  // Build INSERT operation
  {
    size_t const len = OV_ARRAY_LENGTH(it->params);
    op.type = ptk_anm2_op_param_insert;
    op.sel_idx = sel_idx;
    op.item_idx = item_idx;
    op.param_idx = len;
    op.removed_data = new_param;
    new_param = NULL; // ownership transferred to op
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

uint32_t ptk_anm2_param_insert(struct ptk_anm2 *doc,
                               size_t sel_idx,
                               size_t item_idx,
                               size_t param_idx,
                               char const *key,
                               char const *value,
                               struct ov_error *const err) {
  if (!doc) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_invalid_argument);
    return 0;
  }
  if (!doc->selectors || sel_idx >= OV_ARRAY_LENGTH(doc->selectors)) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_invalid_argument);
    return 0;
  }
  struct selector *sel = &doc->selectors[sel_idx];
  if (!sel->items || item_idx >= OV_ARRAY_LENGTH(sel->items)) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_invalid_argument);
    return 0;
  }
  struct item *it = &sel->items[item_idx];
  // Cannot add params to value items
  if (it->script_name == NULL) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_invalid_argument);
    return 0;
  }

  size_t const len = OV_ARRAY_LENGTH(it->params);
  // Allow inserting at end (param_idx == len)
  if (param_idx > len) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_invalid_argument);
    return 0;
  }

  uint32_t result_id = 0;
  uint32_t new_id = 0;
  struct param *new_param = NULL;
  struct ptk_anm2_op op = {0};
  struct ptk_anm2_op reverse_op = {0};

  // Allocate and initialize new param
  if (!OV_REALLOC(&new_param, 1, sizeof(struct param))) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_out_of_memory);
    goto cleanup;
  }
  memset(new_param, 0, sizeof(struct param));
  new_param->id = generate_id(doc);
  new_id = new_param->id;

  // Create the param
  if (!strdup_to_array(&new_param->key, key, err)) {
    OV_ERROR_ADD_TRACE(err);
    goto cleanup;
  }
  if (!strdup_to_array(&new_param->value, value, err)) {
    OV_ERROR_ADD_TRACE(err);
    goto cleanup;
  }

  // Build INSERT operation
  op.type = ptk_anm2_op_param_insert;
  op.sel_idx = sel_idx;
  op.item_idx = item_idx;
  op.param_idx = param_idx;
  op.removed_data = new_param;
  new_param = NULL; // ownership transferred to op

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

bool ptk_anm2_param_remove(
    struct ptk_anm2 *doc, size_t sel_idx, size_t item_idx, size_t param_idx, struct ov_error *const err) {
  if (!doc) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_invalid_argument);
    return false;
  }
  if (!doc->selectors || sel_idx >= OV_ARRAY_LENGTH(doc->selectors)) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_invalid_argument);
    return false;
  }
  struct selector *sel = &doc->selectors[sel_idx];
  if (!sel->items || item_idx >= OV_ARRAY_LENGTH(sel->items)) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_invalid_argument);
    return false;
  }
  struct item *it = &sel->items[item_idx];
  // Cannot remove params from value items
  if (it->script_name == NULL) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_invalid_argument);
    return false;
  }
  if (!it->params || param_idx >= OV_ARRAY_LENGTH(it->params)) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_invalid_argument);
    return false;
  }

  bool success = false;
  struct ptk_anm2_op op = {0};
  struct ptk_anm2_op reverse_op = {0};

  // Build REMOVE operation
  op.type = ptk_anm2_op_param_remove;
  op.sel_idx = sel_idx;
  op.item_idx = item_idx;
  op.param_idx = param_idx;

  // Apply the operation (removes the param, reverse_op will contain the data)
  if (!apply_op(doc, &op, &reverse_op, err)) {
    OV_ERROR_ADD_TRACE(err);
    goto cleanup;
  }

  // Push reverse operation (INSERT) to undo stack
  if (!push_undo_op(doc, &reverse_op, err)) {
    OV_ERROR_ADD_TRACE(err);
    goto cleanup;
  }
  memset(&reverse_op, 0, sizeof(reverse_op)); // ownership transferred

  // Clear redo stack
  clear_redo_stack(doc);

  success = true;

cleanup:
  op_free(&op);
  op_free(&reverse_op);
  return success;
}

char const *ptk_anm2_param_get_key(struct ptk_anm2 const *doc, size_t sel_idx, size_t item_idx, size_t param_idx) {
  if (!doc || !doc->selectors) {
    return NULL;
  }
  if (sel_idx >= OV_ARRAY_LENGTH(doc->selectors)) {
    return NULL;
  }
  struct selector const *sel = &doc->selectors[sel_idx];
  if (!sel->items || item_idx >= OV_ARRAY_LENGTH(sel->items)) {
    return NULL;
  }
  struct item const *it = &sel->items[item_idx];
  if (!it->params || param_idx >= OV_ARRAY_LENGTH(it->params)) {
    return NULL;
  }
  return it->params[param_idx].key;
}

bool ptk_anm2_param_set_key(struct ptk_anm2 *doc,
                            size_t sel_idx,
                            size_t item_idx,
                            size_t param_idx,
                            char const *key,
                            struct ov_error *const err) {
  if (!doc) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_invalid_argument);
    return false;
  }
  if (!doc->selectors || sel_idx >= OV_ARRAY_LENGTH(doc->selectors)) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_invalid_argument);
    return false;
  }
  struct selector *sel = &doc->selectors[sel_idx];
  if (!sel->items || item_idx >= OV_ARRAY_LENGTH(sel->items)) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_invalid_argument);
    return false;
  }
  struct item *it = &sel->items[item_idx];
  if (!it->params || param_idx >= OV_ARRAY_LENGTH(it->params)) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_invalid_argument);
    return false;
  }

  bool success = false;
  struct ptk_anm2_op op = {0};
  struct ptk_anm2_op reverse_op = {0};

  // Build SET_KEY operation
  if (!strdup_to_array(&op.str_data, key, err)) {
    OV_ERROR_ADD_TRACE(err);
    goto cleanup;
  }
  op.type = ptk_anm2_op_param_set_key;
  op.sel_idx = sel_idx;
  op.item_idx = item_idx;
  op.param_idx = param_idx;

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

  success = true;

cleanup:
  op_free(&op);
  op_free(&reverse_op);
  return success;
}

char const *ptk_anm2_param_get_value(struct ptk_anm2 const *doc, size_t sel_idx, size_t item_idx, size_t param_idx) {
  if (!doc || !doc->selectors) {
    return NULL;
  }
  if (sel_idx >= OV_ARRAY_LENGTH(doc->selectors)) {
    return NULL;
  }
  struct selector const *sel = &doc->selectors[sel_idx];
  if (!sel->items || item_idx >= OV_ARRAY_LENGTH(sel->items)) {
    return NULL;
  }
  struct item const *it = &sel->items[item_idx];
  if (!it->params || param_idx >= OV_ARRAY_LENGTH(it->params)) {
    return NULL;
  }
  return it->params[param_idx].value;
}

bool ptk_anm2_param_set_value(struct ptk_anm2 *doc,
                              size_t sel_idx,
                              size_t item_idx,
                              size_t param_idx,
                              char const *value,
                              struct ov_error *const err) {
  if (!doc) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_invalid_argument);
    return false;
  }
  if (!doc->selectors || sel_idx >= OV_ARRAY_LENGTH(doc->selectors)) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_invalid_argument);
    return false;
  }
  struct selector *sel = &doc->selectors[sel_idx];
  if (!sel->items || item_idx >= OV_ARRAY_LENGTH(sel->items)) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_invalid_argument);
    return false;
  }
  struct item *it = &sel->items[item_idx];
  if (!it->params || param_idx >= OV_ARRAY_LENGTH(it->params)) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_invalid_argument);
    return false;
  }

  bool success = false;
  struct ptk_anm2_op op = {0};
  struct ptk_anm2_op reverse_op = {0};

  // Build SET_VALUE operation
  if (!strdup_to_array(&op.str_data, value, err)) {
    OV_ERROR_ADD_TRACE(err);
    goto cleanup;
  }
  op.type = ptk_anm2_op_param_set_value;
  op.sel_idx = sel_idx;
  op.item_idx = item_idx;
  op.param_idx = param_idx;

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

  success = true;

cleanup:
  op_free(&op);
  op_free(&reverse_op);
  return success;
}

// ============================================================================
// UNDO/REDO operations (stub implementations)
// ============================================================================

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
  struct ptk_anm2_op op = {.type = ptk_anm2_op_group_begin};
  struct ptk_anm2_op reverse_op = {.type = ptk_anm2_op_group_begin};

  // Pop from undo stack
  size_t len = OV_ARRAY_LENGTH(doc->undo_stack);
  op = doc->undo_stack[len - 1];
  OV_ARRAY_SET_LENGTH(doc->undo_stack, len - 1);

  // Check if this is a GROUP_END - if so, we need to undo until GROUP_BEGIN
  bool const is_group = (op.type == ptk_anm2_op_group_end);

  for (;;) {
    enum ptk_anm2_op_type const op_type = op.type;

    // Apply the reverse operation (the op contains the old value)
    if (!apply_op(doc, &op, &reverse_op, err)) {
      OV_ERROR_ADD_TRACE(err);
      goto cleanup;
    }

    // Push reverse operation to redo stack (transfers ownership)
    if (!push_redo_op(doc, &reverse_op, err)) {
      OV_ERROR_ADD_TRACE(err);
      goto cleanup;
    }
    memset(&reverse_op, 0, sizeof(reverse_op)); // ownership transferred

    // Free original op's resources
    op_free(&op);
    memset(&op, 0, sizeof(op));

    // If we just processed GROUP_BEGIN and we're in a group, we're done
    if (is_group && op_type == ptk_anm2_op_group_begin) {
      break;
    }

    // If we're not in a group, we're done after one operation
    if (!is_group) {
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
  struct ptk_anm2_op op = {.type = ptk_anm2_op_group_begin};
  struct ptk_anm2_op reverse_op = {.type = ptk_anm2_op_group_begin};

  // Pop from redo stack
  size_t len = OV_ARRAY_LENGTH(doc->redo_stack);
  op = doc->redo_stack[len - 1];
  OV_ARRAY_SET_LENGTH(doc->redo_stack, len - 1);

  // Check if this is a GROUP_END - if so, we need to redo until GROUP_BEGIN
  // (After undo, redo stack has operations in reverse order:
  //  GROUP_END is at top, GROUP_BEGIN is at bottom)
  bool const is_group = (op.type == ptk_anm2_op_group_end);

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

    // If we just processed GROUP_BEGIN and we're in a group, we're done
    if (is_group && op_type == ptk_anm2_op_group_begin) {
      break;
    }

    // If we're not in a group, we're done after one operation
    if (!is_group) {
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
    // Push GROUP_BEGIN marker when entering outermost transaction
    clear_redo_stack(doc);
    struct ptk_anm2_op op = {.type = ptk_anm2_op_group_begin};
    if (!push_undo_op(doc, &op, err)) {
      OV_ERROR_ADD_TRACE(err);
      return false;
    }
    notify_change(doc, ptk_anm2_op_group_begin, 0, 0, 0, 0, 0);
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
    // Push GROUP_END marker when exiting outermost transaction
    struct ptk_anm2_op op = {.type = ptk_anm2_op_group_end};
    if (!push_undo_op(doc, &op, err)) {
      OV_ERROR_ADD_TRACE(err);
      return false;
    }
    notify_change(doc, ptk_anm2_op_group_end, 0, 0, 0, 0, 0);
  }
  return true;
}

// ============================================================================
// File operations
// ============================================================================

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
    if (!strdup_to_array(&sel->group, group_str, err)) {
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
  struct ptk_anm2 temp = {0};

  // Initialize temporary document
  if (!doc_init(&temp, err)) {
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

  // Success - swap contents
  {
    // Save callback info from original doc
    ptk_anm2_change_callback cb = doc->change_callback;
    void *cb_userdata = doc->change_callback_userdata;

    // Clean up original doc contents
    doc_cleanup(doc);

    // Copy temp contents to doc (struct copy)
    *doc = temp;

    // Restore callback
    doc->change_callback = cb;
    doc->change_callback_userdata = cb_userdata;

    // Clear temp so cleanup doesn't free the now-doc-owned resources
    temp = (struct ptk_anm2){0};
  }

  // Clear undo/redo stacks after load
  ptk_anm2_clear_undo_history(doc);

  // Notify that document was reset (loaded)
  notify_change(doc, ptk_anm2_op_reset, 0, 0, 0, 0, 0);

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
  struct ovl_file *file = NULL;
  bool success = false;

  // Generate script content
  if (!generate_script_content(doc, &content, err)) {
    OV_ERROR_ADD_TRACE(err);
    goto cleanup;
  }

  // Write to file
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

  success = true;

cleanup:
  if (file) {
    ovl_file_close(file);
  }
  if (content) {
    OV_ARRAY_DESTROY(&content);
  }
  return success;
}

bool ptk_anm2_verify_checksum(struct ptk_anm2 const *doc) {
  if (!doc) {
    return false;
  }
  return doc->stored_checksum == doc->calculated_checksum;
}

// ============================================================================
// ID and userdata operations
// ============================================================================

uint32_t ptk_anm2_selector_get_id(struct ptk_anm2 const *doc, size_t idx) {
  if (!doc || !doc->selectors) {
    return 0;
  }
  if (idx >= OV_ARRAY_LENGTH(doc->selectors)) {
    return 0;
  }
  return doc->selectors[idx].id;
}

uintptr_t ptk_anm2_selector_get_userdata(struct ptk_anm2 const *doc, size_t idx) {
  if (!doc || !doc->selectors) {
    return 0;
  }
  if (idx >= OV_ARRAY_LENGTH(doc->selectors)) {
    return 0;
  }
  return doc->selectors[idx].userdata;
}

void ptk_anm2_selector_set_userdata(struct ptk_anm2 *doc, size_t idx, uintptr_t userdata) {
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

uintptr_t ptk_anm2_item_get_userdata(struct ptk_anm2 const *doc, size_t sel_idx, size_t item_idx) {
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

void ptk_anm2_item_set_userdata(struct ptk_anm2 *doc, size_t sel_idx, size_t item_idx, uintptr_t userdata) {
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

uintptr_t ptk_anm2_param_get_userdata(struct ptk_anm2 const *doc, size_t sel_idx, size_t item_idx, size_t param_idx) {
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

void ptk_anm2_param_set_userdata(
    struct ptk_anm2 *doc, size_t sel_idx, size_t item_idx, size_t param_idx, uintptr_t userdata) {
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

// ============================================================================
// ID reverse lookup operations
// ============================================================================

bool ptk_anm2_find_selector_by_id(struct ptk_anm2 const *doc, uint32_t id, size_t *out_sel_idx) {
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

bool ptk_anm2_find_item_by_id(struct ptk_anm2 const *doc, uint32_t id, size_t *out_sel_idx, size_t *out_item_idx) {
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

bool ptk_anm2_find_param_by_id(
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
