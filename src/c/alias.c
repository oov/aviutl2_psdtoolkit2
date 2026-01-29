#include "alias.h"

#include <string.h>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <ovarray.h>
#include <ovl/os.h>
#include <ovl/path.h>
#include <ovmo.h>
#include <ovsort.h>

#include "i18n.h"
#include "ini_reader.h"

/**
 * @brief Copy a string with length
 *
 * @param dst [out] Destination pointer (caller must free with OV_FREE)
 * @param src Source string
 * @param len Length of source string
 * @param err [out] Error information on failure
 * @return true on success, false on failure
 */
static bool strdup_n(char **const dst, char const *const src, size_t const len, struct ov_error *const err) {
  if (!dst || !src) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_invalid_argument);
    return false;
  }
  if (!OV_REALLOC(dst, len + 1, sizeof(char))) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_out_of_memory);
    return false;
  }
  memcpy(*dst, src, len);
  (*dst)[len] = '\0';
  return true;
}

/**
 * @brief Check if a string contains only digits
 */
static bool is_digits(char const *const s, size_t const len) {
  for (size_t i = 0; i < len; i++) {
    if (s[i] < '0' || s[i] > '9') {
      return false;
    }
  }
  return len > 0;
}

void ptk_alias_script_definitions_free(struct ptk_alias_script_definitions *const defs) {
  if (!defs || !defs->items) {
    return;
  }
  size_t const n = OV_ARRAY_LENGTH(defs->items);
  for (size_t i = 0; i < n; ++i) {
    if (defs->items[i].script_name) {
      OV_FREE(&defs->items[i].script_name);
    }
    if (defs->items[i].effect_name) {
      OV_FREE(&defs->items[i].effect_name);
    }
  }
  OV_ARRAY_DESTROY(&defs->items);
}

/**
 * @brief Get the base directory for PSDToolKit configuration files
 *
 * Returns the full path to the PSDToolKit config directory.
 *
 * @param dir [out] Output directory path (caller must free with OV_ARRAY_DESTROY)
 * @param err [out] Error information on failure
 * @return true on success, false on failure
 */
static bool get_config_dir(NATIVE_CHAR **const dir, struct ov_error *const err) {
  if (!dir) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_invalid_argument);
    return false;
  }

  NATIVE_CHAR *module_path = NULL;
  void *hinstance = NULL;
  bool success = false;

  {
    if (!ovl_os_get_hinstance_from_fnptr((void *)get_config_dir, &hinstance, err)) {
      OV_ERROR_ADD_TRACE(err);
      goto cleanup;
    }
    if (!ovl_path_get_module_name(&module_path, hinstance, err)) {
      OV_ERROR_ADD_TRACE(err);
      goto cleanup;
    }

    NATIVE_CHAR const *last_slash = ovl_path_find_last_path_sep(module_path);
    if (!last_slash) {
      OV_ERROR_SET(err, ov_error_type_generic, ov_error_generic_fail, "No directory separator found in module path");
      goto cleanup;
    }

    static wchar_t const suffix[] = L"\\";
    static size_t const suffix_len = (sizeof(suffix) / sizeof(wchar_t)) - 1;

    size_t const dir_len = (size_t)(last_slash - module_path);
    size_t const path_len = dir_len + suffix_len;

    if (!OV_ARRAY_GROW(dir, path_len + 1)) {
      OV_ERROR_SET_GENERIC(err, ov_error_generic_out_of_memory);
      goto cleanup;
    }

    memcpy(*dir, module_path, dir_len * sizeof(NATIVE_CHAR));
    memcpy(*dir + dir_len, suffix, (suffix_len + 1) * sizeof(wchar_t));
  }

  success = true;

cleanup:
  if (module_path) {
    OV_ARRAY_DESTROY(&module_path);
  }
  return success;
}

/**
 * @brief Build a config file path from base directory and filename
 *
 * @param dir Base directory path
 * @param filename Filename to append
 * @param path [out] Output path (caller must free with OV_ARRAY_DESTROY)
 * @param err [out] Error information on failure
 * @return true on success, false on failure
 */
static bool build_config_path(NATIVE_CHAR const *const dir,
                              wchar_t const *const filename,
                              NATIVE_CHAR **const path,
                              struct ov_error *const err) {
  if (!dir || !filename || !path) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_invalid_argument);
    return false;
  }

  size_t const dir_len = wcslen(dir);
  size_t const filename_len = wcslen(filename);
  size_t const path_len = dir_len + filename_len;

  if (!OV_ARRAY_GROW(path, path_len + 1)) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_out_of_memory);
    return false;
  }

  memcpy(*path, dir, dir_len * sizeof(NATIVE_CHAR));
  memcpy(*path + dir_len, filename, (filename_len + 1) * sizeof(wchar_t));
  return true;
}

/**
 * @brief Load script definitions from an INI reader into defs array
 *
 * Reads the [anm2Editor.AnimationScripts] section and adds entries to defs.
 * Entries with duplicate script_name will be skipped.
 *
 * @param reader INI reader instance (must already be loaded)
 * @param defs [in/out] Script definitions to append to
 * @param script_name_buf [in/out] Reusable buffer for script name
 * @param effect_name_buf [in/out] Reusable buffer for effect name
 * @param err [out] Error information on failure
 * @return true on success, false on failure
 */
static bool load_script_definitions_from_reader(struct ptk_ini_reader const *const reader,
                                                struct ptk_alias_script_definitions *const defs,
                                                char **const script_name_buf,
                                                char **const effect_name_buf,
                                                struct ov_error *const err) {
  static char const ini_section[] = "anm2Editor.AnimationScripts";
  static size_t const ini_section_len = sizeof(ini_section) - 1;
  struct ptk_ini_iter iter = {0};
  bool success = false;

  while (ptk_ini_reader_iter_entries(reader, ini_section, &iter)) {
    struct ptk_ini_value val =
        ptk_ini_reader_get_value_n(reader, ini_section, ini_section_len, iter.name, iter.name_len);
    if (!val.ptr || val.size == 0) {
      continue;
    }

    // Check for duplicate script_name
    bool duplicate = false;
    size_t const current_len = OV_ARRAY_LENGTH(defs->items);
    for (size_t i = 0; i < current_len; i++) {
      if (strlen(defs->items[i].script_name) == iter.name_len &&
          strncmp(defs->items[i].script_name, iter.name, iter.name_len) == 0) {
        duplicate = true;
        break;
      }
    }
    if (duplicate) {
      continue;
    }

    // Reuse buffers - strdup_n will realloc as needed
    if (!strdup_n(script_name_buf, iter.name, iter.name_len, err)) {
      OV_ERROR_ADD_TRACE(err);
      goto cleanup;
    }
    if (!strdup_n(effect_name_buf, val.ptr, val.size, err)) {
      OV_ERROR_ADD_TRACE(err);
      goto cleanup;
    }

    if (!OV_ARRAY_GROW(&defs->items, current_len + 1)) {
      OV_ERROR_SET_GENERIC(err, ov_error_generic_out_of_memory);
      goto cleanup;
    }
    defs->items[current_len].script_name = *script_name_buf;
    defs->items[current_len].effect_name = *effect_name_buf;
    OV_ARRAY_SET_LENGTH(defs->items, current_len + 1);

    // Ownership transferred to array
    *script_name_buf = NULL;
    *effect_name_buf = NULL;
  }

  success = true;

cleanup:
  return success;
}

bool ptk_alias_load_script_definitions(struct ptk_alias_script_definitions *const defs, struct ov_error *const err) {
  if (!defs) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_invalid_argument);
    return false;
  }

  NATIVE_CHAR *config_dir = NULL;
  NATIVE_CHAR *ini_path = NULL;
  NATIVE_CHAR *user_ini_path = NULL;
  struct ptk_ini_reader *reader = NULL;
  char *script_name = NULL;
  char *effect_name = NULL;
  bool success = false;

  memset(defs, 0, sizeof(*defs));

  {
    if (!get_config_dir(&config_dir, err)) {
      OV_ERROR_ADD_TRACE(err);
      goto cleanup;
    }

    // Build path to PSDToolKit.ini
    if (!build_config_path(config_dir, L"PSDToolKit.ini", &ini_path, err)) {
      OV_ERROR_ADD_TRACE(err);
      goto cleanup;
    }

    if (!ptk_ini_reader_create(&reader, err)) {
      OV_ERROR_ADD_TRACE(err);
      goto cleanup;
    }

    // Load main INI file
    if (!ptk_ini_reader_load_file(reader, ini_path, err)) {
      OV_ERROR_ADD_TRACE(err);
      goto cleanup;
    }

    if (!load_script_definitions_from_reader(reader, defs, &script_name, &effect_name, err)) {
      OV_ERROR_ADD_TRACE(err);
      goto cleanup;
    }

    // Try to load user INI file (optional - ignore errors if file doesn't exist)
    if (!build_config_path(config_dir, L"PSDToolKit.user.ini", &user_ini_path, err)) {
      OV_ERROR_ADD_TRACE(err);
      goto cleanup;
    }

    // Check if user INI file exists
    if (GetFileAttributesW(user_ini_path) != INVALID_FILE_ATTRIBUTES) {
      // Destroy and recreate reader for user file
      ptk_ini_reader_destroy(&reader);
      reader = NULL;

      if (!ptk_ini_reader_create(&reader, err)) {
        OV_ERROR_ADD_TRACE(err);
        goto cleanup;
      }

      if (ptk_ini_reader_load_file(reader, user_ini_path, err)) {
        // Merge user definitions (duplicates are skipped)
        if (!load_script_definitions_from_reader(reader, defs, &script_name, &effect_name, err)) {
          OV_ERROR_ADD_TRACE(err);
          goto cleanup;
        }
      }
      // If user file fails to load, we silently ignore and continue with main file's data
    }
  }

  success = true;

cleanup:
  if (reader) {
    ptk_ini_reader_destroy(&reader);
  }
  if (config_dir) {
    OV_ARRAY_DESTROY(&config_dir);
  }
  if (ini_path) {
    OV_ARRAY_DESTROY(&ini_path);
  }
  if (user_ini_path) {
    OV_ARRAY_DESTROY(&user_ini_path);
  }
  if (script_name) {
    OV_FREE(&script_name);
  }
  if (effect_name) {
    OV_FREE(&effect_name);
  }
  if (!success) {
    ptk_alias_script_definitions_free(defs);
  }
  return success;
}

void ptk_alias_available_scripts_free(struct ptk_alias_available_scripts *const scripts) {
  if (!scripts) {
    return;
  }
  if (scripts->psd_path) {
    OV_FREE(&scripts->psd_path);
  }
  if (!scripts->items) {
    return;
  }
  size_t const n = OV_ARRAY_LENGTH(scripts->items);
  for (size_t i = 0; i < n; ++i) {
    if (scripts->items[i].script_name) {
      OV_FREE(&scripts->items[i].script_name);
    }
    if (scripts->items[i].effect_name) {
      OV_FREE(&scripts->items[i].effect_name);
    }
    // translated_name points to SDK-managed memory, do not free
    scripts->items[i].translated_name = NULL;
  }
  OV_ARRAY_DESTROY(&scripts->items);
}

/**
 * @brief Scan alias sections to find available scripts and PSD path
 *
 * This function performs a single scan of all [Object.N] sections to:
 * 1. Find which effects from the script definitions exist in the alias
 * 2. Extract the PSD file path from "PSDファイル@PSDToolKit" effect
 *
 * @param reader INI reader with loaded alias data
 * @param defs Script definitions to check against
 * @param scripts [out] Available scripts with selected flag set to true
 * @param err [out] Error information on failure
 * @return true on success, false on failure
 */
static bool scan_alias_sections(struct ptk_ini_reader const *const reader,
                                struct ptk_alias_script_definitions const *const defs,
                                struct ptk_alias_available_scripts *const scripts,
                                struct ov_error *const err) {
  static char const psd_effect_name[] = "PSD\xe3\x83\x95\xe3\x82\xa1\xe3\x82\xa4\xe3\x83\xab@PSDToolKit"; // PSDファイル
  static size_t const psd_effect_name_len = sizeof(psd_effect_name) - 1;
  static char const psd_key[] = "PSD\xe3\x83\x95\xe3\x82\xa1\xe3\x82\xa4\xe3\x83\xab"; // PSDファイル

  char *script_name = NULL;
  char *effect_name = NULL;
  bool success = false;

  size_t const defs_count = OV_ARRAY_LENGTH(defs->items);

  {
    struct ptk_ini_iter section_iter = {0};
    while (ptk_ini_reader_iter_sections(reader, &section_iter)) {
      if (!section_iter.name) {
        continue;
      }

      // Check for Object.N section
      static char const object_prefix[] = "Object.";
      static size_t const prefix_len = sizeof(object_prefix) - 1;
      if (section_iter.name_len <= prefix_len || strncmp(section_iter.name, object_prefix, prefix_len) != 0) {
        continue;
      }
      if (!is_digits(section_iter.name + prefix_len, section_iter.name_len - prefix_len)) {
        continue;
      }

      // Check effect.name key
      static char const effect_key[] = "effect.name";
      struct ptk_ini_value effect_val = ptk_ini_reader_get_value_n(
          reader, section_iter.name, section_iter.name_len, effect_key, sizeof(effect_key) - 1);
      if (!effect_val.ptr || effect_val.size == 0) {
        continue;
      }

      // Check if this is the PSD file effect
      if (!scripts->psd_path && effect_val.size == psd_effect_name_len &&
          strncmp(effect_val.ptr, psd_effect_name, effect_val.size) == 0) {
        // Extract PSD path value
        struct ptk_ini_value psd_val =
            ptk_ini_reader_get_value_n(reader, section_iter.name, section_iter.name_len, psd_key, sizeof(psd_key) - 1);
        if (psd_val.ptr && psd_val.size > 0) {
          // Keep full path including PFV info (format: "path|pfv_file")
          if (!strdup_n(&scripts->psd_path, psd_val.ptr, psd_val.size, err)) {
            OV_ERROR_ADD_TRACE(err);
            goto cleanup;
          }
        }
      }

      // Check against script definitions
      for (size_t i = 0; i < defs_count; i++) {
        struct ptk_alias_script_definition const *def = &defs->items[i];
        size_t const def_effect_len = strlen(def->effect_name);
        if (effect_val.size != def_effect_len || strncmp(effect_val.ptr, def->effect_name, effect_val.size) != 0) {
          continue;
        }

        // Found matching effect - add to scripts
        if (!strdup_n(&script_name, def->script_name, strlen(def->script_name), err)) {
          OV_ERROR_ADD_TRACE(err);
          goto cleanup;
        }
        if (!strdup_n(&effect_name, def->effect_name, def_effect_len, err)) {
          OV_ERROR_ADD_TRACE(err);
          goto cleanup;
        }

        size_t const len = OV_ARRAY_LENGTH(scripts->items);
        if (!OV_ARRAY_GROW(&scripts->items, len + 1)) {
          OV_ERROR_SET_GENERIC(err, ov_error_generic_out_of_memory);
          goto cleanup;
        }
        scripts->items[len].script_name = script_name;
        scripts->items[len].effect_name = effect_name;
        scripts->items[len].translated_name = NULL;
        scripts->items[len].selected = true;
        OV_ARRAY_SET_LENGTH(scripts->items, len + 1);

        // Ownership transferred
        script_name = NULL;
        effect_name = NULL;
        break;
      }
    }
  }

  success = true;

cleanup:
  if (script_name) {
    OV_FREE(&script_name);
  }
  if (effect_name) {
    OV_FREE(&effect_name);
  }
  return success;
}

bool ptk_alias_enumerate_available_scripts(char const *const alias,
                                           size_t const alias_len,
                                           struct ptk_alias_script_definitions const *const defs,
                                           struct ptk_alias_available_scripts *const scripts,
                                           struct ov_error *const err) {
  if (!alias || !defs || !scripts) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_invalid_argument);
    return false;
  }

  struct ptk_ini_reader *reader = NULL;
  bool success = false;

  memset(scripts, 0, sizeof(*scripts));

  {
    if (!ptk_ini_reader_create(&reader, err)) {
      OV_ERROR_ADD_TRACE(err);
      goto cleanup;
    }

    if (!ptk_ini_reader_load_memory(reader, alias, alias_len, err)) {
      OV_ERROR_ADD_TRACE(err);
      goto cleanup;
    }

    // Scan all sections once to find scripts and PSD path
    if (!scan_alias_sections(reader, defs, scripts, err)) {
      OV_ERROR_ADD_TRACE(err);
      goto cleanup;
    }

    // Check if PSD path was found
    if (!scripts->psd_path) {
      OV_ERROR_SET(err,
                   ov_error_type_generic,
                   ptk_alias_error_psd_not_found,
                   gettext("PSD file effect not found in the selected object."));
      goto cleanup;
    }

    // Check if any scripts were found
    if (!scripts->items || OV_ARRAY_LENGTH(scripts->items) == 0) {
      OV_ERROR_SET(err,
                   ov_error_type_generic,
                   ptk_alias_error_no_scripts,
                   gettext("No importable scripts found in the selected object."));
      goto cleanup;
    }
  }

  success = true;

cleanup:
  if (reader) {
    ptk_ini_reader_destroy(&reader);
  }
  if (!success) {
    ptk_alias_available_scripts_free(scripts);
  }
  return success;
}

void ptk_alias_extracted_animation_free(struct ptk_alias_extracted_animation *const anim) {
  if (!anim) {
    return;
  }
  if (anim->script_name) {
    OV_FREE(&anim->script_name);
  }
  if (anim->effect_name) {
    OV_FREE(&anim->effect_name);
  }
  if (anim->params) {
    size_t const n = OV_ARRAY_LENGTH(anim->params);
    for (size_t i = 0; i < n; i++) {
      if (anim->params[i].key) {
        OV_FREE(&anim->params[i].key);
      }
      if (anim->params[i].value) {
        OV_FREE(&anim->params[i].value);
      }
    }
    OV_ARRAY_DESTROY(&anim->params);
  }
}

/**
 * @brief Find the section containing the specified effect
 *
 * @param reader INI reader with loaded alias data
 * @param effect_name Effect name to search for
 * @param section_name [out] Buffer for section name
 * @param section_name_size Size of section_name buffer
 * @param section_name_len [out] Length of section name
 * @return true if found, false otherwise
 */
static bool find_effect_section(struct ptk_ini_reader *const reader,
                                char const *const effect_name,
                                char *const section_name,
                                size_t const section_name_size,
                                size_t *const section_name_len) {
  struct ptk_ini_iter section_iter = {0};
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

    static char const key[] = "effect.name";
    struct ptk_ini_value val =
        ptk_ini_reader_get_value_n(reader, section_iter.name, section_iter.name_len, key, sizeof(key) - 1);
    if (!val.ptr || val.size == 0) {
      continue;
    }

    if (val.size == strlen(effect_name) && strncmp(val.ptr, effect_name, val.size) == 0) {
      if (section_iter.name_len >= section_name_size) {
        return false;
      }
      memcpy(section_name, section_iter.name, section_iter.name_len);
      section_name[section_iter.name_len] = '\0';
      *section_name_len = section_iter.name_len;
      return true;
    }
  }
  return false;
}

/**
 * @brief Intermediate structure to hold entry info with line number for sorting
 */
struct ini_entry_info {
  char const *name;
  size_t name_len;
  char const *value;
  size_t value_len;
  size_t line_number;
};

/**
 * @brief Comparison function for sorting ini_entry_info by line number
 */
static int compare_ini_entry_info_by_line(void const *const a, void const *const b, void *const userdata) {
  (void)userdata;
  struct ini_entry_info const *ea = (struct ini_entry_info const *)a;
  struct ini_entry_info const *eb = (struct ini_entry_info const *)b;
  if (ea->line_number < eb->line_number) {
    return -1;
  }
  if (ea->line_number > eb->line_number) {
    return 1;
  }
  return 0;
}

/**
 * @brief Add a parameter entry to the params array
 *
 * @param params Pointer to params ovarray
 * @param key Parameter key
 * @param key_len Key length
 * @param value Parameter value (can be NULL)
 * @param value_len Value length
 * @param err Error information
 * @return true on success, false on failure
 */
static bool add_param_entry(struct ptk_alias_extracted_param **const params,
                            char const *const key,
                            size_t const key_len,
                            char const *const value,
                            size_t const value_len,
                            struct ov_error *const err) {
  char *key_buf = NULL;
  char *value_buf = NULL;
  bool success = false;

  {
    if (!strdup_n(&key_buf, key, key_len, err)) {
      OV_ERROR_ADD_TRACE(err);
      goto cleanup;
    }

    if (value && value_len > 0) {
      if (!strdup_n(&value_buf, value, value_len, err)) {
        OV_ERROR_ADD_TRACE(err);
        goto cleanup;
      }
    } else {
      if (!strdup_n(&value_buf, "", 0, err)) {
        OV_ERROR_ADD_TRACE(err);
        goto cleanup;
      }
    }

    size_t const params_len = OV_ARRAY_LENGTH(*params);
    if (!OV_ARRAY_GROW(params, params_len + 1)) {
      OV_ERROR_SET_GENERIC(err, ov_error_generic_out_of_memory);
      goto cleanup;
    }

    (*params)[params_len].key = key_buf;
    (*params)[params_len].value = value_buf;
    OV_ARRAY_SET_LENGTH(*params, params_len + 1);
    key_buf = NULL;
    value_buf = NULL;
  }

  success = true;

cleanup:
  if (key_buf) {
    OV_FREE(&key_buf);
  }
  if (value_buf) {
    OV_FREE(&value_buf);
  }
  return success;
}

/**
 * @brief Collect all parameters from a section as key-value pairs
 *
 * This function iterates through all entries in the given section and adds them
 * as key-value pairs to the params array, excluding "effect.name".
 * Entries are sorted by line number to preserve the original order from the INI file.
 *
 * @param reader INI reader instance
 * @param section Section name
 * @param section_len Section name length
 * @param params Pointer to params ovarray
 * @param err Error information
 * @return true on success, false on failure
 */
static bool collect_all_params_from_section(struct ptk_ini_reader *const reader,
                                            char const *const section,
                                            size_t const section_len,
                                            struct ptk_alias_extracted_param **const params,
                                            struct ov_error *const err) {
  static char const excluded_key[] = "effect.name";
  struct ptk_ini_iter iter = {0};
  struct ini_entry_info *entries = NULL;
  bool success = false;

  {
    // First pass: collect all entries with line numbers
    while (ptk_ini_reader_iter_entries_n(reader, section, section_len, &iter)) {
      if (iter.name_len == sizeof(excluded_key) - 1 && strncmp(iter.name, excluded_key, iter.name_len) == 0) {
        continue;
      }

      struct ptk_ini_value val = ptk_ini_reader_get_value_n(reader, section, section_len, iter.name, iter.name_len);

      size_t const entries_len = OV_ARRAY_LENGTH(entries);
      if (!OV_ARRAY_GROW(&entries, entries_len + 1)) {
        OV_ERROR_SET_GENERIC(err, ov_error_generic_out_of_memory);
        goto cleanup;
      }
      entries[entries_len] = (struct ini_entry_info){
          .name = iter.name,
          .name_len = iter.name_len,
          .value = val.ptr,
          .value_len = val.size,
          .line_number = iter.line_number,
      };
      OV_ARRAY_SET_LENGTH(entries, entries_len + 1);
    }

    // Sort entries by line number
    {
      size_t const entries_len = OV_ARRAY_LENGTH(entries);
      if (entries_len > 1) {
        ov_qsort(entries, entries_len, sizeof(entries[0]), compare_ini_entry_info_by_line, NULL);
      }
    }

    // Second pass: add entries to params array in sorted order
    {
      size_t const entries_len = OV_ARRAY_LENGTH(entries);
      for (size_t i = 0; i < entries_len; i++) {
        struct ini_entry_info const *e = &entries[i];
        if (!add_param_entry(params, e->name, e->name_len, e->value, e->value_len, err)) {
          OV_ERROR_ADD_TRACE(err);
          goto cleanup;
        }
      }
    }
  }

  success = true;

cleanup:
  if (entries) {
    OV_ARRAY_DESTROY(&entries);
  }
  return success;
}

bool ptk_alias_extract_animation(char const *const alias,
                                 size_t const alias_len,
                                 char const *const script_name,
                                 char const *const effect_name,
                                 struct ptk_alias_extracted_animation *const anim,
                                 struct ov_error *const err) {
  if (!alias || !script_name || !effect_name || !anim) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_invalid_argument);
    return false;
  }

  struct ptk_ini_reader *reader = NULL;
  char section[64];
  size_t section_len = 0;
  bool success = false;

  memset(anim, 0, sizeof(*anim));

  {
    if (!ptk_ini_reader_create(&reader, err)) {
      OV_ERROR_ADD_TRACE(err);
      goto cleanup;
    }

    if (!ptk_ini_reader_load_memory(reader, alias, alias_len, err)) {
      OV_ERROR_ADD_TRACE(err);
      goto cleanup;
    }

    if (!find_effect_section(reader, effect_name, section, sizeof(section), &section_len)) {
      OV_ERROR_SETF(err,
                    ov_error_type_generic,
                    ov_error_generic_fail,
                    "%1$hs",
                    gettext("Effect \"%1$hs\" not found."),
                    effect_name);
      goto cleanup;
    }

    if (!strdup_n(&anim->script_name, script_name, strlen(script_name), err)) {
      OV_ERROR_ADD_TRACE(err);
      goto cleanup;
    }

    if (!strdup_n(&anim->effect_name, effect_name, strlen(effect_name), err)) {
      OV_ERROR_ADD_TRACE(err);
      goto cleanup;
    }

    if (!collect_all_params_from_section(reader, section, section_len, &anim->params, err)) {
      OV_ERROR_ADD_TRACE(err);
      goto cleanup;
    }
  }

  success = true;

cleanup:
  if (reader) {
    ptk_ini_reader_destroy(&reader);
  }
  if (!success) {
    ptk_alias_extracted_animation_free(anim);
    memset(anim, 0, sizeof(*anim));
  }
  return success;
}

void ptk_alias_populate_translated_names(struct ptk_alias_available_scripts *const scripts) {
  if (!scripts || !scripts->items) {
    return;
  }

  size_t const n = OV_ARRAY_LENGTH(scripts->items);
  for (size_t i = 0; i < n; ++i) {
    struct ptk_alias_available_script *const item = &scripts->items[i];
    if (!item->effect_name) {
      continue;
    }

    // Use effect_name as both section and text
    item->translated_name = ptk_i18n_get_translated_text(item->effect_name, item->effect_name);
  }
}
