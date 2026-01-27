#include "layer.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <ovarray.h>
#include <ovmo.h>
#include <ovprintf.h>
#include <ovprintf_ex.h>
#include <ovsort.h>
#include <ovutf.h>

#include <aviutl2_plugin2.h>

#include "anm2_edit.h"
#include "anm2editor.h"
#include "error.h"
#include "i18n.h"
#include "ini_reader.h"
#include "logf.h"
#include "win32.h"

struct layer_item {
  char *name;
  char *value;
};

struct layer_info {
  struct layer_item *items;
  char *slider_name; // NULL for layer names, non-NULL for FAView
  char *file_path;
};

static void layer_info_free(struct layer_info *const li) {
  if (!li) {
    return;
  }
  size_t const len = OV_ARRAY_LENGTH(li->items);
  for (size_t i = 0; i < len; i++) {
    if (li->items[i].name) {
      OV_FREE(&li->items[i].name);
    }
    if (li->items[i].value) {
      OV_FREE(&li->items[i].value);
    }
  }
  if (li->items) {
    OV_ARRAY_DESTROY(&li->items);
  }
  if (li->slider_name) {
    OV_FREE(&li->slider_name);
  }
  if (li->file_path) {
    OV_FREE(&li->file_path);
  }
}

static inline bool is_faview(struct layer_info const *const li) { return li && li->slider_name; }

struct ptkl_target_item {
  char *effect_name;  // points to entry in effects array (do not free)
  char *item_name;    // item name with suffix, e.g., "開き~ptkl"
  size_t line_number; // line number in INI for sorting
};

struct ptkl_targets {
  char **effects;
  struct ptkl_target_item *items;
};

static void ptkl_targets_free(struct ptkl_targets *const t) {
  if (!t) {
    return;
  }
  size_t const effects_count = OV_ARRAY_LENGTH(t->effects);
  for (size_t i = 0; i < effects_count; i++) {
    if (t->effects[i]) {
      OV_FREE(&t->effects[i]);
    }
  }
  if (t->effects) {
    OV_ARRAY_DESTROY(&t->effects);
  }
  size_t const items_count = OV_ARRAY_LENGTH(t->items);
  for (size_t i = 0; i < items_count; i++) {
    t->items[i].effect_name = NULL;
    if (t->items[i].item_name) {
      OV_FREE(&t->items[i].item_name);
    }
  }
  if (t->items) {
    OV_ARRAY_DESTROY(&t->items);
  }
}

static bool strdup_n(char **const dest, char const *const src, size_t const len, struct ov_error *const err) {
  if (!OV_REALLOC(dest, len + 1, sizeof(char))) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_out_of_memory);
    return false;
  }
  memcpy(*dest, src, len);
  (*dest)[len] = '\0';
  return true;
}

static bool utf8_to_wstr(wchar_t **const dest, char const *const src, struct ov_error *const err) {
  size_t const src_len = strlen(src);
  size_t const wlen = ov_utf8_to_wchar_len(src, src_len);
  if (!OV_ARRAY_GROW(dest, wlen + 1)) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_out_of_memory);
    return false;
  }
  ov_utf8_to_wchar(src, src_len, *dest, wlen + 1, NULL);
  OV_ARRAY_SET_LENGTH(*dest, wlen + 1);
  return true;
}

/**
 * @brief Parse layer info from concatenated null-terminated strings
 *
 * names_utf8 and values_utf8 are sequences of null-terminated strings concatenated together.
 * For example: "name1\0name2\0name3\0" with total length including all null terminators.
 *
 * @param li Output layer_info structure (must be zero-initialized)
 * @param names_utf8 Concatenated null-terminated layer names
 * @param names_len Total byte length of names_utf8
 * @param values_utf8 Concatenated null-terminated layer values
 * @param values_len Total byte length of values_utf8
 * @param slider_name_utf8 FAView slider name (NULL for layer names mode)
 * @param file_path_utf8 Source file path (can be NULL)
 * @param err Error information
 * @return true on success, false on failure
 */
static bool layer_info_parse(struct layer_info *const li,
                             char const *const names_utf8,
                             size_t const names_len,
                             char const *const values_utf8,
                             size_t const values_len,
                             char const *const slider_name_utf8,
                             char const *const file_path_utf8,
                             struct ov_error *const err) {
  char *name = NULL;
  char *value = NULL;
  bool success = false;

  size_t npos = 0, vpos = 0;
  while (npos < names_len && vpos < values_len) {
    char const *const name_u8 = names_utf8 + npos;
    char const *const value_u8 = values_utf8 + vpos;
    size_t const name_len = strlen(name_u8);
    size_t const value_len = strlen(value_u8);

    if (!strdup_n(&name, name_u8, name_len, err)) {
      OV_ERROR_ADD_TRACE(err);
      goto cleanup;
    }
    if (!strdup_n(&value, value_u8, value_len, err)) {
      OV_ERROR_ADD_TRACE(err);
      goto cleanup;
    }

    size_t const items_len = OV_ARRAY_LENGTH(li->items);
    if (!OV_ARRAY_GROW(&li->items, items_len + 1)) {
      OV_ERROR_SET_GENERIC(err, ov_error_generic_out_of_memory);
      goto cleanup;
    }
    li->items[items_len] = (struct layer_item){.name = name, .value = value};
    OV_ARRAY_SET_LENGTH(li->items, items_len + 1);
    name = NULL;
    value = NULL;

    npos += name_len + 1;
    vpos += value_len + 1;
  }

  if (slider_name_utf8) {
    if (!strdup_n(&li->slider_name, slider_name_utf8, strlen(slider_name_utf8), err)) {
      OV_ERROR_ADD_TRACE(err);
      goto cleanup;
    }
  }
  if (file_path_utf8) {
    if (!strdup_n(&li->file_path, file_path_utf8, strlen(file_path_utf8), err)) {
      OV_ERROR_ADD_TRACE(err);
      goto cleanup;
    }
  }

  success = true;

cleanup:
  if (name) {
    OV_FREE(&name);
  }
  if (value) {
    OV_FREE(&value);
  }
  if (!success) {
    layer_info_free(li);
  }
  return success;
}

/**
 * @brief Get readable item name (last component for layers, full name for FAView)
 */
static bool layer_info_get_item_name(struct layer_info const *const li,
                                     size_t const idx,
                                     char **const dest,
                                     struct ov_error *const err) {
  if (!li || idx >= OV_ARRAY_LENGTH(li->items)) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_invalid_argument);
    return false;
  }

  char const *const full_name = li->items[idx].name;
  if (is_faview(li)) {
    if (!strdup_n(dest, full_name, strlen(full_name), err)) {
      OV_ERROR_ADD_TRACE(err);
      return false;
    }
    return true;
  }

  // For layer names, extract last component after '/'
  char const *base = strrchr(full_name, '/');
  char const *const result = base ? base + 1 : full_name;
  if (!strdup_n(dest, result, strlen(result), err)) {
    OV_ERROR_ADD_TRACE(err);
    return false;
  }
  return true;
}

/**
 * @brief Get group name for export filename generation
 */
static bool layer_info_get_group_name(struct layer_info const *const li,
                                      size_t const idx,
                                      char **const dest,
                                      struct ov_error *const err) {
  if (!li || idx >= OV_ARRAY_LENGTH(li->items)) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_invalid_argument);
    return false;
  }

  if (is_faview(li)) {
    // For FAView, use last component of slider_name
    char const *base = strrchr(li->slider_name, '\\');
    if (!base) {
      base = strrchr(li->slider_name, '/');
    }
    char const *const result = base ? base + 1 : li->slider_name;
    if (!strdup_n(dest, result, strlen(result), err)) {
      OV_ERROR_ADD_TRACE(err);
      return false;
    }
    return true;
  }

  // For layer names, get parent folder name
  char const *const full_name = li->items[idx].name;
  char const *const last_sep = strrchr(full_name, '/');
  if (!last_sep) {
    if (!strdup_n(dest, full_name, strlen(full_name), err)) {
      OV_ERROR_ADD_TRACE(err);
      return false;
    }
    return true;
  }

  // Find the previous separator to get parent folder
  size_t const parent_len = (size_t)(last_sep - full_name);
  char const *parent_start = full_name;
  for (size_t i = 0; i < parent_len; i++) {
    if (full_name[i] == '/') {
      parent_start = full_name + i + 1;
    }
  }
  size_t const name_len = (size_t)(last_sep - parent_start);
  if (!strdup_n(dest, parent_start, name_len, err)) {
    OV_ERROR_ADD_TRACE(err);
    return false;
  }
  return true;
}

/**
 * @brief Check if the selected item has children (is a folder)
 *
 * An item has children if there exists another item whose name starts with
 * the selected item's name followed by '/'.
 *
 * For FAView mode, always returns false since there is no folder hierarchy.
 *
 * @param li Layer info
 * @param idx Index of the selected item
 * @return true if the item has children, false otherwise
 */
static bool has_children(struct layer_info const *const li, size_t const idx) {
  if (!li || idx >= OV_ARRAY_LENGTH(li->items)) {
    return false;
  }

  if (is_faview(li)) {
    return false;
  }

  char const *const selected_name = li->items[idx].name;
  size_t const selected_len = strlen(selected_name);
  size_t const items_len = OV_ARRAY_LENGTH(li->items);

  for (size_t i = 0; i < items_len; i++) {
    if (i == idx) {
      continue;
    }
    char const *const name = li->items[i].name;
    size_t const name_len = strlen(name);
    if (name_len > selected_len + 1 && strncmp(name, selected_name, selected_len) == 0 && name[selected_len] == '/') {
      return true;
    }
  }
  return false;
}

/**
 * @brief Enumerate child items of the selected folder
 *
 * Finds items whose name starts with the selected item's name followed by '/'.
 * Only direct children are included (not grandchildren).
 *
 * For FAView mode, all items are considered children (same as siblings).
 *
 * @param li Layer info
 * @param idx Index of the selected item (folder)
 * @param indices Output array of child indices (caller must destroy with OV_ARRAY_DESTROY)
 * @param err Error information
 * @return true on success, false on failure
 */
static bool enumerate_children(struct layer_info const *const li,
                               size_t const idx,
                               size_t **const indices,
                               struct ov_error *const err) {
  if (!li || idx >= OV_ARRAY_LENGTH(li->items)) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_invalid_argument);
    return false;
  }

  size_t const items_len = OV_ARRAY_LENGTH(li->items);
  bool success = false;

  if (is_faview(li)) {
    // For FAView, all items are children
    if (!OV_ARRAY_GROW(indices, items_len)) {
      OV_ERROR_SET_GENERIC(err, ov_error_generic_out_of_memory);
      goto cleanup;
    }
    for (size_t i = 0; i < items_len; i++) {
      (*indices)[i] = i;
    }
    OV_ARRAY_SET_LENGTH(*indices, items_len);
    success = true;
    goto cleanup;
  }

  // For layer names, find direct children
  {
    char const *const selected_name = li->items[idx].name;
    size_t const selected_len = strlen(selected_name);

    for (size_t i = 0; i < items_len; i++) {
      if (i == idx) {
        continue;
      }
      char const *const name = li->items[i].name;
      size_t const name_len = strlen(name);

      // Check if this item is under the selected folder
      if (name_len <= selected_len + 1) {
        continue;
      }
      if (strncmp(name, selected_name, selected_len) != 0) {
        continue;
      }
      if (name[selected_len] != '/') {
        continue;
      }

      // Check if this is a direct child (no more '/' after the prefix)
      char const *const after_prefix = name + selected_len + 1;
      if (strchr(after_prefix, '/') != NULL) {
        continue;
      }

      size_t const cur_len = OV_ARRAY_LENGTH(*indices);
      if (!OV_ARRAY_GROW(indices, cur_len + 1)) {
        OV_ERROR_SET_GENERIC(err, ov_error_generic_out_of_memory);
        goto cleanup;
      }
      (*indices)[cur_len] = i;
      OV_ARRAY_SET_LENGTH(*indices, cur_len + 1);
    }
  }

  success = true;

cleanup:
  if (!success && *indices) {
    OV_ARRAY_DESTROY(indices);
  }
  return success;
}

/**
 * @brief Remove invalid characters from a string in-place
 *
 * Removes ',' and '=' characters which are not allowed in selector names.
 *
 * @param s String to sanitize (modified in-place)
 */
static void sanitize_selector_name(char *const s) {
  char *read = s;
  char *write = s;
  while (*read) {
    if (*read != ',' && *read != '=') {
      *write++ = *read;
    }
    read++;
  }
  *write = '\0';
}

/**
 * @brief Add layers to the PSDToolKit anm2 Editor as a new selector
 *
 * Creates a selector from the specified layer indices and adds it to the
 * PSDToolKit anm2 Editor instance.
 *
 * @param anm2editor PSDToolKit anm2 Editor instance
 * @param li Layer info containing layer names and values
 * @param indices Array of layer indices to include
 * @param indices_len Number of indices
 * @param err Error information
 * @return true on success, false on failure
 */
static bool add_to_anm2editor(struct ptk_anm2editor *const anm2editor,
                              struct layer_info const *const li,
                              size_t const *const indices,
                              size_t const indices_len,
                              struct ov_error *const err) {
  char *group_name = NULL;
  char **names = NULL;
  char **values = NULL;
  bool success = false;

  if (indices_len == 0) {
    success = true;
    goto cleanup;
  }

  // Check if PSD file path is available
  if (!li->file_path || li->file_path[0] == '\0') {
    OV_ERROR_SET(err,
                 ov_error_type_generic,
                 ov_error_generic_invalid_argument,
                 gettext("PSD file path is required to add to PSDToolKit anm2 Editor."));
    goto cleanup;
  }

  if (!anm2editor || !ptk_anm2editor_is_open(anm2editor)) {
    OV_ERROR_SET(
        err, ov_error_type_generic, ov_error_generic_invalid_argument, gettext("PSDToolKit anm2 Editor is not open."));
    goto cleanup;
  }

  // Get group name
  if (!layer_info_get_group_name(li, indices[0], &group_name, err)) {
    OV_ERROR_ADD_TRACE(err);
    goto cleanup;
  }

  // Sanitize group_name
  sanitize_selector_name(group_name);

  // Build names and values arrays
  if (!OV_ARRAY_GROW(&names, indices_len)) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_out_of_memory);
    goto cleanup;
  }
  if (!OV_ARRAY_GROW(&values, indices_len)) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_out_of_memory);
    goto cleanup;
  }

  for (size_t i = 0; i < indices_len; i++) {
    char *item_name = NULL;

    // Get display name
    if (!layer_info_get_item_name(li, indices[i], &item_name, err)) {
      OV_ERROR_ADD_TRACE(err);
      goto cleanup;
    }
    names[i] = item_name;

    // Duplicate value
    char const *const src_value = li->items[indices[i]].value;
    size_t const value_len = strlen(src_value);
    char *dup_value = NULL;
    if (!OV_ARRAY_GROW(&dup_value, value_len + 1)) {
      OV_FREE(&item_name);
      names[i] = NULL;
      OV_ERROR_SET_GENERIC(err, ov_error_generic_out_of_memory);
      goto cleanup;
    }
    memcpy(dup_value, src_value, value_len + 1);
    values[i] = dup_value;

    OV_ARRAY_SET_LENGTH(names, i + 1);
    OV_ARRAY_SET_LENGTH(values, i + 1);
  }

  // Add to editor using new API (psd_path is set inside the transaction if editor has no path)
  if (!ptk_anm2editor_add_value_items(anm2editor,
                                      li->file_path,
                                      group_name,
                                      (char const *const *)names,
                                      (char const *const *)values,
                                      indices_len,
                                      err)) {
    OV_ERROR_ADD_TRACE(err);
    goto cleanup;
  }

  success = true;

cleanup:
  if (names) {
    size_t const len = OV_ARRAY_LENGTH(names);
    for (size_t i = 0; i < len; i++) {
      if (names[i]) {
        OV_FREE(&names[i]);
      }
    }
    OV_ARRAY_DESTROY(&names);
  }
  if (values) {
    size_t const len = OV_ARRAY_LENGTH(values);
    for (size_t i = 0; i < len; i++) {
      if (values[i]) {
        OV_ARRAY_DESTROY(&values[i]);
      }
    }
    OV_ARRAY_DESTROY(&values);
  }
  if (group_name) {
    OV_FREE(&group_name);
  }
  return success;
}

/**
 * @brief Add a single layer item to the selected selector or create a new one
 *
 * If a selector is currently selected in the PSDToolKit anm2 Editor, adds the
 * item to that selector. Otherwise creates a new selector with the group name.
 *
 * @param anm2editor PSDToolKit anm2 Editor instance
 * @param li Layer info containing layer names and values
 * @param item_idx Index of the layer item to add
 * @param err Error information
 * @return true on success, false on failure
 */
static bool add_single_to_anm2editor(struct ptk_anm2editor *const anm2editor,
                                     struct layer_info const *const li,
                                     size_t const item_idx,
                                     struct ov_error *const err) {
  char *group_name = NULL;
  char *item_name = NULL;
  bool success = false;

  // Check if PSD file path is available
  if (!li->file_path || li->file_path[0] == '\0') {
    OV_ERROR_SET(err,
                 ov_error_type_generic,
                 ov_error_generic_invalid_argument,
                 gettext("PSD file path is required to add to PSDToolKit anm2 Editor."));
    goto cleanup;
  }

  if (!anm2editor || !ptk_anm2editor_is_open(anm2editor)) {
    OV_ERROR_SET(
        err, ov_error_type_generic, ov_error_generic_invalid_argument, gettext("PSDToolKit anm2 Editor is not open."));
    goto cleanup;
  }

  // Get group name (used only when creating new selector)
  if (!layer_info_get_group_name(li, item_idx, &group_name, err)) {
    OV_ERROR_ADD_TRACE(err);
    goto cleanup;
  }

  // Sanitize group_name
  sanitize_selector_name(group_name);

  // Get item name
  if (!layer_info_get_item_name(li, item_idx, &item_name, err)) {
    OV_ERROR_ADD_TRACE(err);
    goto cleanup;
  }

  // Add to editor using new API (adds to selected selector if any, otherwise creates new)
  // psd_path is set inside the transaction if editor has no path
  if (!ptk_anm2editor_add_value_item_to_selected(
          anm2editor, li->file_path, group_name, item_name, li->items[item_idx].value, err)) {
    OV_ERROR_ADD_TRACE(err);
    goto cleanup;
  }

  success = true;

cleanup:
  if (item_name) {
    OV_FREE(&item_name);
  }
  if (group_name) {
    OV_FREE(&group_name);
  }
  return success;
}

static bool ends_with_ptkl_suffix(char const *const s, size_t const len) {
  static char const suffix[] = "~ptkl";
  static size_t const suffix_len = sizeof(suffix) - 1;
  return len >= suffix_len && memcmp(s + len - suffix_len, suffix, suffix_len) == 0;
}

static bool is_digits(char const *const s, size_t const len) {
  for (size_t i = 0; i < len; i++) {
    if (s[i] < '0' || s[i] > '9') {
      return false;
    }
  }
  return len > 0;
}

static int ptkl_target_item_compare_by_line(void const *const a, void const *const b, void *const userdata) {
  (void)userdata;
  struct ptkl_target_item const *const ia = (struct ptkl_target_item const *)a;
  struct ptkl_target_item const *const ib = (struct ptkl_target_item const *)b;
  if (ia->line_number < ib->line_number) {
    return -1;
  }
  if (ia->line_number > ib->line_number) {
    return 1;
  }
  return 0;
}

/**
 * @brief Collect ptkl assignment targets from object alias INI
 *
 * Parses the alias INI content to find parameters ending with "~ptkl" suffix.
 * These are layer assignment targets that can receive layer values.
 *
 * @param alias INI format alias content
 * @param alias_len Length of alias content
 * @param targets Output structure (caller must free with ptkl_targets_free)
 * @param err Error information
 * @return true on success, false on failure
 */
static bool collect_ptkl_targets_from_alias(char const *const alias,
                                            size_t const alias_len,
                                            struct ptkl_targets *const targets,
                                            struct ov_error *const err) {
  if (!alias || alias_len == 0) {
    return true;
  }

  struct ptk_ini_reader *reader = NULL;
  struct ptk_ini_iter section_iter = {0};
  struct ptk_ini_iter entry_iter = {0};
  bool success = false;

  if (!ptk_ini_reader_create(&reader, err)) {
    OV_ERROR_ADD_TRACE(err);
    goto cleanup;
  }

  if (!ptk_ini_reader_load_memory(reader, alias, alias_len, err)) {
    OV_ERROR_ADD_TRACE(err);
    goto cleanup;
  }

  while (ptk_ini_reader_iter_sections(reader, &section_iter)) {
    if (!section_iter.name) {
      continue;
    }

    static char const object_prefix[] = "Object.";
    static size_t const prefix_len = sizeof(object_prefix) - 1;
    if (section_iter.name_len <= prefix_len || strncmp(section_iter.name, object_prefix, prefix_len) != 0) {
      continue;
    }
    if (!is_digits(section_iter.name + prefix_len, section_iter.name_len - prefix_len)) {
      continue;
    }

    size_t current_effect_index = SIZE_MAX;
    entry_iter.index = 0;
    entry_iter.state = NULL;

    while (ptk_ini_reader_iter_entries_n(reader, section_iter.name, section_iter.name_len, &entry_iter)) {
      if (!ends_with_ptkl_suffix(entry_iter.name, entry_iter.name_len)) {
        continue;
      }

      if (current_effect_index == SIZE_MAX) {
        static char const key[] = "effect.name";
        static size_t const key_len = sizeof(key) - 1;
        struct ptk_ini_value val =
            ptk_ini_reader_get_value_n(reader, section_iter.name, section_iter.name_len, key, key_len);
        if (!val.ptr || val.size == 0) {
          break;
        }

        char *effect_name = NULL;
        if (!strdup_n(&effect_name, val.ptr, val.size, err)) {
          OV_ERROR_ADD_TRACE(err);
          goto cleanup;
        }

        size_t const effects_len = OV_ARRAY_LENGTH(targets->effects);
        if (!OV_ARRAY_GROW(&targets->effects, effects_len + 1)) {
          OV_FREE(&effect_name);
          OV_ERROR_SET_GENERIC(err, ov_error_generic_out_of_memory);
          goto cleanup;
        }
        targets->effects[effects_len] = effect_name;
        OV_ARRAY_SET_LENGTH(targets->effects, effects_len + 1);
        current_effect_index = effects_len;
      }

      size_t const items_len = OV_ARRAY_LENGTH(targets->items);
      if (!OV_ARRAY_GROW(&targets->items, items_len + 1)) {
        OV_ERROR_SET_GENERIC(err, ov_error_generic_out_of_memory);
        goto cleanup;
      }

      char *item_name = NULL;
      if (!strdup_n(&item_name, entry_iter.name, entry_iter.name_len, err)) {
        OV_ERROR_ADD_TRACE(err);
        goto cleanup;
      }

      targets->items[items_len] = (struct ptkl_target_item){
          .effect_name = targets->effects[current_effect_index],
          .item_name = item_name,
          .line_number = entry_iter.line_number,
      };
      OV_ARRAY_SET_LENGTH(targets->items, items_len + 1);
    }
  }

  // Sort items by line number to maintain definition order
  {
    size_t const items_len = OV_ARRAY_LENGTH(targets->items);
    if (items_len > 1) {
      ov_qsort(targets->items, items_len, sizeof(struct ptkl_target_item), ptkl_target_item_compare_by_line, NULL);
    }
  }

  success = true;

cleanup:
  if (reader) {
    ptk_ini_reader_destroy(&reader);
  }
  if (!success) {
    ptkl_targets_free(targets);
  }
  return success;
}

struct collect_targets_context {
  struct ptkl_targets targets;
  struct ov_error *err;
  bool success;
};

/**
 * @brief Callback for collecting ptkl targets from the focused object
 *
 * Called via edit->call_edit_section_param to access the edit section safely.
 * Retrieves the focused object's alias and extracts ptkl targets.
 *
 * @param param Pointer to collect_targets_context
 * @param edit Edit section interface
 */
static void collect_targets_proc(void *const param, struct aviutl2_edit_section *const edit) {
  struct collect_targets_context *const ctx = (struct collect_targets_context *)param;
  char const *alias = NULL;

  aviutl2_object_handle obj = edit->get_focus_object();
  if (!obj) {
    OV_ERROR_SET(ctx->err, ov_error_type_generic, ov_error_generic_fail, gettext("no object is selected in AviUtl."));
    goto cleanup;
  }

  alias = edit->get_object_alias(obj);
  if (!alias) {
    OV_ERROR_SET(ctx->err,
                 ov_error_type_generic,
                 ov_error_generic_fail,
                 gettext("no assignable parameters found in selected object in AviUtl."));
    goto cleanup;
  }

  if (!collect_ptkl_targets_from_alias(alias, strlen(alias), &ctx->targets, ctx->err)) {
    OV_ERROR_ADD_TRACE(ctx->err);
    goto cleanup;
  }

  if (OV_ARRAY_LENGTH(ctx->targets.items) == 0) {
    OV_ERROR_SET(ctx->err,
                 ov_error_type_generic,
                 ov_error_generic_fail,
                 gettext("no assignable parameters found in selected object in AviUtl."));
    goto cleanup;
  }

  ctx->success = true;

cleanup:;
}

struct set_value_context {
  char const *effect_name;
  char const *item_name;
  char const *value_utf8;
  struct ov_error *err;
  bool success;
};

/**
 * @brief Callback for setting a layer value to an object parameter
 *
 * Called via edit->call_edit_section_param to access the edit section safely.
 * Sets the specified value to the target effect's parameter.
 *
 * @param param Pointer to set_value_context
 * @param edit Edit section interface
 */
static void set_value_proc(void *const param, struct aviutl2_edit_section *const edit) {
  struct set_value_context *const ctx = (struct set_value_context *)param;
  wchar_t *effect_name_w = NULL;
  wchar_t *item_name_w = NULL;

  aviutl2_object_handle obj = edit->get_focus_object();
  if (!obj) {
    OV_ERROR_SET_GENERIC(ctx->err, ov_error_generic_unexpected);
    goto cleanup;
  }

  // Convert to wchar_t for API call
  if (!utf8_to_wstr(&effect_name_w, ctx->effect_name, ctx->err)) {
    OV_ERROR_ADD_TRACE(ctx->err);
    goto cleanup;
  }
  if (!utf8_to_wstr(&item_name_w, ctx->item_name, ctx->err)) {
    OV_ERROR_ADD_TRACE(ctx->err);
    goto cleanup;
  }

  if (!edit->set_object_item_value(obj, effect_name_w, item_name_w, ctx->value_utf8)) {
    OV_ERROR_SET_GENERIC(ctx->err, ov_error_generic_fail);
    goto cleanup;
  }

  ctx->success = true;

cleanup:
  if (item_name_w) {
    OV_ARRAY_DESTROY(&item_name_w);
  }
  if (effect_name_w) {
    OV_ARRAY_DESTROY(&effect_name_w);
  }
}

enum menu_cmd {
  menu_cmd_copy_single = 1,
  menu_cmd_copy_siblings,
  menu_cmd_add_single_to_anm2editor,
  menu_cmd_add_to_anm2editor,
  menu_cmd_assign_base = 100,
  menu_cmd_anm2editor_selected_assign_base = 200,
};

/**
 * @brief Build popup menu for layer export operations
 *
 * Creates a context menu with options:
 * - Copy selected item to clipboard
 * - Assign to ptkl targets (grouped by effect name as submenus)
 *   If targets_error_message is provided, show grayed out error instead
 * - Assign to anm2editor selected selector ptkl targets
 * - Add to PSDToolKit anm2 Editor (if editor is open)
 * - Copy/export all siblings
 *
 * @param menu Output menu handle (caller must destroy with DestroyMenu on failure)
 * @param li Layer info
 * @param selected_idx Index of the selected item
 * @param targets Available ptkl assignment targets (can be NULL if error occurred)
 * @param targets_error_message Error message to show instead of targets (can be NULL)
 * @param anm2edit_selected_targets ptkl targets from currently selected selector in anm2_edit (can be NULL)
 * @param anm2editor_open Whether anm2editor is open
 * @param err Error information
 * @return true on success, false on failure
 */
static bool build_popup_menu(HMENU *const menu,
                             struct layer_info const *const li,
                             size_t const selected_idx,
                             struct ptkl_targets const *const targets,
                             wchar_t const *const targets_error_message,
                             struct ptk_anm2_edit_ptkl_targets const *const anm2edit_selected_targets,
                             bool const anm2editor_open,
                             struct ov_error *const err) {
  char *item_name = NULL;
  char *group_name = NULL;
  bool success = false;

  *menu = CreatePopupMenu();
  if (!*menu) {
    OV_ERROR_SET_HRESULT(err, HRESULT_FROM_WIN32(GetLastError()));
    goto cleanup;
  }

  if (!layer_info_get_item_name(li, selected_idx, &item_name, err)) {
    OV_ERROR_ADD_TRACE(err);
    goto cleanup;
  }
  if (!layer_info_get_group_name(li, selected_idx, &group_name, err)) {
    OV_ERROR_ADD_TRACE(err);
    goto cleanup;
  }

  // Copy single item
  {
    wchar_t text[256];
    ov_snprintf_wchar(text, sizeof(text) / sizeof(text[0]), L"%1$hs", L"%1$hs", pgettext("layer", "Copy to clipboard"));
    AppendMenuW(*menu, MF_STRING, menu_cmd_copy_single, text);

    // Copy all child layers to clipboard
    // Only enabled if the selected item is a folder (has children)
    {
      UINT child_flags = MF_STRING;
      if (!is_faview(li) && !has_children(li, selected_idx)) {
        child_flags |= MF_GRAYED;
      }
      if (is_faview(li)) {
        ov_snprintf_wchar(text,
                          sizeof(text) / sizeof(text[0]),
                          L"%1$hs",
                          L"%1$hs",
                          pgettext("layer", "Copy entire slider to clipboard"));
      } else {
        ov_snprintf_wchar(text,
                          sizeof(text) / sizeof(text[0]),
                          L"%1$hs",
                          L"%1$hs",
                          pgettext("layer", "Copy all child layers of to clipboard"));
      }
      AppendMenuW(*menu, child_flags, menu_cmd_copy_siblings, text);
    }
  }

  AppendMenuW(*menu, MF_SEPARATOR, 0, NULL);

  // Assign submenu(s) grouped by effect_name, or show error message if failed
  if (targets_error_message) {
    // Show grayed error message when targets collection failed
    AppendMenuW(*menu, MF_STRING | MF_GRAYED, 0, targets_error_message);
  } else if (targets && OV_ARRAY_LENGTH(targets->items) > 0) {
    size_t const targets_len = OV_ARRAY_LENGTH(targets->items);
    struct {
      char const *name;
      wchar_t const *translated_name; // NULL if no translation, points to SDK-managed memory
      HMENU menu;
    } submenus[64];
    size_t submenu_count = 0;

    for (size_t i = 0; i < targets_len && i < 64; i++) {
      struct ptkl_target_item const *const t = &targets->items[i];

      HMENU submenu = NULL;
      wchar_t const *translated_effect = NULL;
      for (size_t j = 0; j < submenu_count; j++) {
        if (strcmp(submenus[j].name, t->effect_name) == 0) {
          submenu = submenus[j].menu;
          translated_effect = submenus[j].translated_name;
          break;
        }
      }

      if (!submenu) {
        submenu = CreatePopupMenu();
        if (!submenu) {
          OV_ERROR_SET_HRESULT(err, HRESULT_FROM_WIN32(GetLastError()));
          goto cleanup;
        }
        // Get translated effect name (use original if translation not available)
        translated_effect = ptk_i18n_get_translated_text(t->effect_name, t->effect_name);
        wchar_t effect_name_w[256];
        if (translated_effect) {
          // Convert translated_effect to UTF-8 for formatting
          char translated_effect_utf8[256];
          ov_wchar_to_utf8(translated_effect, wcslen(translated_effect), translated_effect_utf8, 256, NULL);
          ov_snprintf_char2wchar(
              effect_name_w, 256, "%1$hs", pgettext("layer", "%1$hs (Selected in AviUtl)"), translated_effect_utf8);
        } else {
          ov_snprintf_char2wchar(
              effect_name_w, 256, "%1$hs", pgettext("layer", "%1$hs (Selected in AviUtl)"), t->effect_name);
        }
        AppendMenuW(*menu, MF_POPUP, (UINT_PTR)submenu, effect_name_w);
        submenus[submenu_count].name = t->effect_name;
        submenus[submenu_count].translated_name = translated_effect;
        submenus[submenu_count].menu = submenu;
        submenu_count++;
      }

      // Get translated item name (use original if translation not available)
      wchar_t const *const translated_item = ptk_i18n_get_translated_text(t->effect_name, t->item_name);

      wchar_t text[256];
      if (translated_item) {
        // Convert translated_item to UTF-8 for formatting
        char translated_item_utf8[256];
        ov_wchar_to_utf8(translated_item, wcslen(translated_item), translated_item_utf8, 256, NULL);
        ov_snprintf_char2wchar(
            text, 256, "%1$hs%2$hs", gettext("Assign \"%1$hs\" to \"%2$hs\""), item_name, translated_item_utf8);
      } else {
        ov_snprintf_char2wchar(
            text, 256, "%1$hs%2$hs", gettext("Assign \"%1$hs\" to \"%2$hs\""), item_name, t->item_name);
      }
      AppendMenuW(submenu, MF_STRING, (UINT)(menu_cmd_assign_base + i), text);
    }
  }

  // anm2editor selected selector ptkl targets
  // Structure: effect_name -> param_key
  if (anm2editor_open) {
    AppendMenuW(*menu, MF_SEPARATOR, 0, NULL);
    if (anm2edit_selected_targets && OV_ARRAY_LENGTH(anm2edit_selected_targets->items) > 0) {
      size_t const targets_len = OV_ARRAY_LENGTH(anm2edit_selected_targets->items);
      struct {
        char const *name;
        wchar_t const *translated_name; // NULL if no translation, points to SDK-managed memory
        HMENU menu;
      } submenus[64];
      size_t submenu_count = 0;

      for (size_t i = 0; i < targets_len && i < 64; i++) {
        struct ptk_anm2_edit_ptkl_target const *const t = &anm2edit_selected_targets->items[i];

        HMENU submenu = NULL;
        wchar_t const *translated_display = NULL;
        for (size_t j = 0; j < submenu_count; j++) {
          if (strcmp(submenus[j].name, t->display_name) == 0) {
            submenu = submenus[j].menu;
            translated_display = submenus[j].translated_name;
            break;
          }
        }

        if (!submenu) {
          submenu = CreatePopupMenu();
          if (!submenu) {
            OV_ERROR_SET_HRESULT(err, HRESULT_FROM_WIN32(GetLastError()));
            goto cleanup;
          }
          // Get translated display name (use effect_name as section for translation)
          if (t->effect_name) {
            translated_display = ptk_i18n_get_translated_text(t->effect_name, t->effect_name);
          }
          wchar_t display_name_w[256];
          if (translated_display) {
            // Convert translated_display to UTF-8 for formatting
            char translated_display_utf8[256];
            ov_wchar_to_utf8(translated_display, wcslen(translated_display), translated_display_utf8, 256, NULL);
            ov_snprintf_char2wchar(display_name_w,
                                   256,
                                   "%1$hs",
                                   pgettext("layer", "%1$hs (Selected in anm2 Editor)"),
                                   translated_display_utf8);
          } else {
            ov_snprintf_char2wchar(
                display_name_w, 256, "%1$hs", pgettext("layer", "%1$hs (Selected in anm2 Editor)"), t->display_name);
          }
          AppendMenuW(*menu, MF_POPUP, (UINT_PTR)submenu, display_name_w);
          submenus[submenu_count].name = t->display_name;
          submenus[submenu_count].translated_name = translated_display;
          submenus[submenu_count].menu = submenu;
          submenu_count++;
        }

        // Get translated param_key (use effect_name as section)
        wchar_t const *translated_param = NULL;
        if (t->effect_name) {
          translated_param = ptk_i18n_get_translated_text(t->effect_name, t->param_key);
        }

        wchar_t text[256];
        if (translated_param) {
          // Convert translated_param to UTF-8 for formatting
          char translated_param_utf8[256];
          ov_wchar_to_utf8(translated_param, wcslen(translated_param), translated_param_utf8, 256, NULL);
          ov_snprintf_char2wchar(
              text, 256, "%1$hs%2$hs", gettext("Assign \"%1$hs\" to \"%2$hs\""), item_name, translated_param_utf8);
        } else {
          ov_snprintf_char2wchar(
              text, 256, "%1$hs%2$hs", gettext("Assign \"%1$hs\" to \"%2$hs\""), item_name, t->param_key);
        }
        AppendMenuW(submenu, MF_STRING, (UINT)(menu_cmd_anm2editor_selected_assign_base + i), text);
      }
    } else {
      // No ptkl targets in selected selector
      wchar_t text[256];
      ov_snprintf_wchar(
          text, 256, L"%hs", L"%hs", gettext("No assignable parameters found in selected selector in anm2 Editor."));
      AppendMenuW(*menu, MF_STRING | MF_GRAYED, 0, text);
    }
  }

  // PSDToolKit anm2 Editor related items (only shown when editor is open)
  if (anm2editor_open) {
    AppendMenuW(*menu, MF_SEPARATOR, 0, NULL);

    // Add single item to PSDToolKit anm2 Editor
    wchar_t text[256];
    ov_snprintf_wchar(text, 256, L"%1$hs", L"%1$hs", pgettext("layer", "Add to anm2 Editor"));
    AppendMenuW(*menu, MF_STRING, menu_cmd_add_single_to_anm2editor, text);

    // Add all child layers to PSDToolKit anm2 Editor
    // Only enabled if the selected item is a folder (has children)
    {
      UINT child_flags = MF_STRING;
      if (!is_faview(li) && !has_children(li, selected_idx)) {
        child_flags |= MF_GRAYED;
      }
      if (is_faview(li)) {
        ov_snprintf_wchar(text, 256, L"%1$hs", L"%1$hs", pgettext("layer", "Add entire slider to anm2 Editor"));
      } else {
        ov_snprintf_wchar(text, 256, L"%1$hs", L"%1$hs", pgettext("layer", "Add all child layers of to anm2 Editor"));
      }
      AppendMenuW(*menu, child_flags, menu_cmd_add_to_anm2editor, text);
    }
  }

  success = true;

cleanup:
  if (group_name) {
    OV_FREE(&group_name);
  }
  if (item_name) {
    OV_FREE(&item_name);
  }
  if (!success && *menu) {
    DestroyMenu(*menu);
    *menu = NULL;
  }
  return success;
}

/**
 * @brief Show popup menu with proper focus handling for cross-process windows
 *
 * When a foreign process window is embedded as a child, we need to create a
 * dummy message-only window and use AttachThreadInput to ensure proper focus
 * behavior. The dummy window is used as the menu owner to avoid issues with
 * the parent-child relationship of embedded cross-process windows.
 *
 * @param hwnd Owner window handle (used for HINSTANCE, must be in this process)
 * @param hwnd_foreign Foreign process window (can be NULL)
 * @param hmenu Menu to display
 * @param flags TrackPopupMenu flags
 * @return Selected menu command, or 0 if cancelled
 */
static UINT
show_popup_menu_cross_process(HWND const hwnd, HWND const hwnd_foreign, HMENU const hmenu, UINT const flags) {
  (void)hwnd;

  static wchar_t const class_name[] = L"PSDToolKit_PopupMenu_Dummy";
  static ATOM s_atom = 0;

  HINSTANCE const hinst = GetModuleHandleW(NULL);
  HWND dummy_window = NULL;
  DWORD foreign_thread_id = 0;
  DWORD const current_thread_id = GetCurrentThreadId();
  bool attached = false;
  UINT cmd = 0;

  // Register window class (once)
  if (!s_atom) {
    WNDCLASSW wc = {
        .hInstance = hinst,
        .lpszClassName = class_name,
        .lpfnWndProc = DefWindowProcW,
    };
    s_atom = RegisterClassW(&wc);
    if (!s_atom) {
      goto cleanup;
    }
  }

  // Create dummy message-only window
  dummy_window = CreateWindowExW(0,
                                 class_name,
                                 NULL,
                                 WS_OVERLAPPEDWINDOW,
                                 CW_USEDEFAULT,
                                 CW_USEDEFAULT,
                                 CW_USEDEFAULT,
                                 CW_USEDEFAULT,
                                 HWND_MESSAGE,
                                 NULL,
                                 hinst,
                                 NULL);
  if (!dummy_window) {
    goto cleanup;
  }

  // Attach thread input if foreign window exists
  if (hwnd_foreign) {
    foreign_thread_id = GetWindowThreadProcessId(hwnd_foreign, NULL);
    if (foreign_thread_id != 0 && foreign_thread_id != current_thread_id) {
      attached = AttachThreadInput(current_thread_id, foreign_thread_id, TRUE) != 0;
    }
  }

  SetForegroundWindow(dummy_window);

  if (attached) {
    AttachThreadInput(current_thread_id, foreign_thread_id, FALSE);
  }

  {
    POINT pt;
    GetCursorPos(&pt);
    cmd = (UINT)TrackPopupMenu(hmenu, flags, pt.x, pt.y, 0, dummy_window, NULL);
  }

cleanup:
  if (dummy_window) {
    DestroyWindow(dummy_window);
  }
  // Note: We don't unregister the class since it can be reused
  return cmd;
}

/**
 * @brief Common implementation for layer/FAView export processing
 *
 * Shows a popup menu with layer operations and handles the selected action:
 * copying to clipboard, assigning to ptkl targets, or adding to PSDToolKit anm2 Editor.
 *
 * @param hwnd Owner window handle (must be in this process)
 * @param hwnd_foreign Foreign process window embedded as child (can be NULL)
 * @param edit AviUtl edit handle for accessing the edit section
 * @param anm2editor PSDToolKit anm2 Editor instance (can be NULL)
 * @param li Layer info
 * @param selected_idx Index of the selected item
 */
static void process_export_common(HWND const hwnd,
                                  HWND const hwnd_foreign,
                                  struct aviutl2_edit_handle *const edit,
                                  struct ptk_anm2editor *const anm2editor,
                                  struct layer_info *const li,
                                  size_t const selected_idx) {
  enum {
    op_none,
    op_copy_clipboard,
    op_assign_value,
    op_add_to_anm2editor,
    op_anm2editor_assign,
  } current_op = op_none;

  struct ov_error err = {0};
  struct collect_targets_context target_ctx = {.err = &err};
  struct ptk_anm2_edit_ptkl_targets anm2edit_selected_targets = {0};
  HMENU hmenu = NULL;
  size_t *sibling_indices = NULL;
  char *clipboard_text = NULL;
  wchar_t *targets_error_message = NULL;
  bool success = false;

  // Collect ptkl targets (error is handled by showing grayed menu item)
  edit->call_edit_section_param(&target_ctx, collect_targets_proc);
  if (!target_ctx.success) {
    // Get error message for display, but continue to build menu
    ptk_error_get_main_message(&err, &targets_error_message);
    OV_ERROR_DESTROY(&err);
  }

  // Collect anm2_edit ptkl targets from selected selector
  bool const anm2editor_open = anm2editor && ptk_anm2editor_is_open(anm2editor);
  struct ptk_anm2_edit *anm2edit_core = anm2editor ? ptk_anm2editor_get_edit(anm2editor) : NULL;
  if (anm2editor_open && anm2edit_core) {
    if (!ptk_anm2_edit_collect_ptkl_targets(anm2edit_core, &anm2edit_selected_targets, &err)) {
      OV_ERROR_DESTROY(&err);
    }
  }

  if (!build_popup_menu(&hmenu,
                        li,
                        selected_idx,
                        target_ctx.success ? &target_ctx.targets : NULL,
                        targets_error_message,
                        &anm2edit_selected_targets,
                        anm2editor_open,
                        &err)) {
    OV_ERROR_ADD_TRACE(&err);
    goto cleanup;
  }

  {
    // Show menu
    UINT const cmd =
        show_popup_menu_cross_process(hwnd, hwnd_foreign, hmenu, TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTBUTTON);

    if (cmd == 0) {
      success = true;
      goto cleanup;
    }

    if (cmd == menu_cmd_copy_single) {
      current_op = op_copy_clipboard;
      char const *const value = li->items[selected_idx].value;
      if (!ptk_win32_copy_to_clipboard(hwnd, value, &err)) {
        OV_ERROR_ADD_TRACE(&err);
        goto cleanup;
      }
    } else if (cmd == menu_cmd_copy_siblings) {
      current_op = op_copy_clipboard;
      if (!enumerate_children(li, selected_idx, &sibling_indices, &err)) {
        OV_ERROR_ADD_TRACE(&err);
        goto cleanup;
      }
      size_t const sib_len = OV_ARRAY_LENGTH(sibling_indices);
      for (size_t i = 0; i < sib_len; i++) {
        char const *const value = li->items[sibling_indices[i]].value;
        char const *const sep = i > 0 ? "\n" : "";
        if (!ov_sprintf_append_char(&clipboard_text, &err, "%1$s%2$s", "%1$s%2$s", sep, value)) {
          OV_ERROR_ADD_TRACE(&err);
          goto cleanup;
        }
      }
      if (!ptk_win32_copy_to_clipboard(hwnd, clipboard_text ? clipboard_text : "", &err)) {
        OV_ERROR_ADD_TRACE(&err);
        goto cleanup;
      }
    } else if (cmd == menu_cmd_add_single_to_anm2editor) {
      current_op = op_add_to_anm2editor;
      if (!add_single_to_anm2editor(anm2editor, li, selected_idx, &err)) {
        OV_ERROR_ADD_TRACE(&err);
        goto cleanup;
      }
    } else if (cmd == menu_cmd_add_to_anm2editor) {
      current_op = op_add_to_anm2editor;
      if (!enumerate_children(li, selected_idx, &sibling_indices, &err)) {
        OV_ERROR_ADD_TRACE(&err);
        goto cleanup;
      }
      if (!add_to_anm2editor(anm2editor, li, sibling_indices, OV_ARRAY_LENGTH(sibling_indices), &err)) {
        OV_ERROR_ADD_TRACE(&err);
        goto cleanup;
      }
    } else if (cmd >= menu_cmd_anm2editor_selected_assign_base &&
               cmd < menu_cmd_anm2editor_selected_assign_base + 100) {
      current_op = op_anm2editor_assign;
      size_t const target_idx = cmd - menu_cmd_anm2editor_selected_assign_base;
      size_t const targets_len = OV_ARRAY_LENGTH(anm2edit_selected_targets.items);
      if (target_idx < targets_len && anm2edit_core) {
        struct ptk_anm2_edit_ptkl_target const *const t = &anm2edit_selected_targets.items[target_idx];
        if (!ptk_anm2_edit_set_param_value_by_id(anm2edit_core, t->param_id, li->items[selected_idx].value, &err)) {
          OV_ERROR_ADD_TRACE(&err);
          goto cleanup;
        }
      }
    } else if (cmd >= menu_cmd_assign_base) {
      current_op = op_assign_value;
      size_t const target_idx = cmd - menu_cmd_assign_base;
      size_t const targets_len = OV_ARRAY_LENGTH(target_ctx.targets.items);
      if (target_idx < targets_len) {
        struct ptkl_target_item const *const t = &target_ctx.targets.items[target_idx];

        struct set_value_context set_ctx = {
            .effect_name = t->effect_name,
            .item_name = t->item_name,
            .value_utf8 = li->items[selected_idx].value,
            .err = &err,
        };
        edit->call_edit_section_param(&set_ctx, set_value_proc);
        if (!set_ctx.success) {
          OV_ERROR_ADD_TRACE(&err);
          goto cleanup;
        }
      }
    }

    success = true;
  }

cleanup:
  ptk_anm2_edit_ptkl_targets_free(&anm2edit_selected_targets);
  if (targets_error_message) {
    OV_ARRAY_DESTROY(&targets_error_message);
  }
  if (clipboard_text) {
    OV_ARRAY_DESTROY(&clipboard_text);
  }
  if (sibling_indices) {
    OV_ARRAY_DESTROY(&sibling_indices);
  }
  if (hmenu) {
    DestroyMenu(hmenu);
  }
  ptkl_targets_free(&target_ctx.targets);
  if (!success) {
    wchar_t main_instruction[256];
    char const *msg = NULL;
    switch (current_op) {
    case op_copy_clipboard:
      msg = gettext("Failed to copy to clipboard.");
      break;
    case op_assign_value:
      msg = gettext("Failed to assign value to effect.");
      break;
    case op_add_to_anm2editor:
      msg = gettext("Failed to add to PSDToolKit anm2 Editor.");
      break;
    case op_anm2editor_assign:
      msg = gettext("Failed to assign value to PSDToolKit anm2 Editor.");
      break;
    case op_none:
      msg = gettext("Operation failed.");
      break;
    }
    ov_snprintf_wchar(main_instruction, sizeof(main_instruction) / sizeof(main_instruction[0]), L"%hs", L"%hs", msg);
    ptk_logf_error(&err, NULL, NULL);
    ptk_error_dialog(hwnd, &err, L"PSDToolKit", main_instruction, NULL, TD_ERROR_ICON, TDCBF_OK_BUTTON);
    OV_ERROR_DESTROY(&err);
  }
}

void ptk_layer_export(void *const hwnd,
                      void *const hwnd_foreign,
                      struct aviutl2_edit_handle *const edit,
                      struct ptk_anm2editor *const anm2editor,
                      struct ptk_layer_export_params const *const params) {
  if (!hwnd || !edit || !params) {
    return;
  }

  struct ov_error err = {0};
  struct layer_info li = {0};

  if (!layer_info_parse(&li,
                        params->names_utf8,
                        params->names_len,
                        params->values_utf8,
                        params->values_len,
                        NULL,
                        params->file_path_utf8,
                        &err)) {
    ptk_logf_error(&err, NULL, NULL);
    OV_ERROR_DESTROY(&err);
    return;
  }

  if (params->selected_index < 0 || (size_t)params->selected_index >= OV_ARRAY_LENGTH(li.items)) {
    layer_info_free(&li);
    return;
  }

  process_export_common((HWND)hwnd, (HWND)hwnd_foreign, edit, anm2editor, &li, (size_t)params->selected_index);
  layer_info_free(&li);
}

void ptk_faview_slider_export(void *const hwnd,
                              void *const hwnd_foreign,
                              struct aviutl2_edit_handle *const edit,
                              struct ptk_anm2editor *const anm2editor,
                              struct ptk_faview_slider_export_params const *const params) {
  if (!hwnd || !edit || !params) {
    return;
  }

  struct ov_error err = {0};
  struct layer_info li = {0};

  if (!layer_info_parse(&li,
                        params->names_utf8,
                        params->names_len,
                        params->values_utf8,
                        params->values_len,
                        params->slider_name_utf8,
                        params->file_path_utf8,
                        &err)) {
    ptk_logf_error(&err, NULL, NULL);
    OV_ERROR_DESTROY(&err);
    return;
  }

  if (params->selected_index < 0 || (size_t)params->selected_index >= OV_ARRAY_LENGTH(li.items)) {
    layer_info_free(&li);
    return;
  }

  process_export_common((HWND)hwnd, (HWND)hwnd_foreign, edit, anm2editor, &li, (size_t)params->selected_index);
  layer_info_free(&li);
}
