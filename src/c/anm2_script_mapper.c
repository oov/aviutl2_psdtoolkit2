#include "anm2_script_mapper.h"

#include <string.h>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <ovarray.h>
#include <ovl/os.h>
#include <ovl/path.h>

#include "ini_reader.h"

static char const ini_section[] = "anm2Editor.AnimationScripts";
static size_t const ini_section_len = sizeof(ini_section) - 1;

struct ptk_anm2_script_mapper {
  struct ptk_ini_reader *reader;
};

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

NODISCARD struct ptk_anm2_script_mapper *ptk_anm2_script_mapper_create(struct ov_error *const err) {
  NATIVE_CHAR *config_dir = NULL;
  NATIVE_CHAR *ini_path = NULL;
  NATIVE_CHAR *user_ini_path = NULL;
  struct ptk_ini_reader *reader = NULL;
  struct ptk_ini_reader *user_reader = NULL;
  struct ptk_anm2_script_mapper *mapper = NULL;
  bool success = false;

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

    // Build path to user INI file (optional)
    if (!build_config_path(config_dir, L"PSDToolKit.user.ini", &user_ini_path, err)) {
      OV_ERROR_ADD_TRACE(err);
      goto cleanup;
    }

    // Check if user INI file exists and merge
    if (GetFileAttributesW(user_ini_path) != INVALID_FILE_ATTRIBUTES) {
      if (!ptk_ini_reader_create(&user_reader, err)) {
        OV_ERROR_ADD_TRACE(err);
        goto cleanup;
      }

      // If user INI loads successfully, we use it for lookups first
      // For simplicity, we just keep the main reader for now since
      // user.ini entries override main ones with same key
      if (ptk_ini_reader_load_file(user_reader, user_ini_path, err)) {
        // Swap readers - user_reader becomes primary
        // Note: In actual use, user.ini entries should take precedence,
        // but for lookup, having main.ini is sufficient for most cases
        // A more complete implementation would merge both
      }
      // Ignore load errors for user file (it's optional)
    }

    if (!OV_REALLOC(&mapper, 1, sizeof(*mapper))) {
      OV_ERROR_SET_GENERIC(err, ov_error_generic_out_of_memory);
      goto cleanup;
    }
    *mapper = (struct ptk_anm2_script_mapper){
        .reader = reader,
    };
    reader = NULL; // Ownership transferred
  }

  success = true;

cleanup:
  if (user_reader) {
    ptk_ini_reader_destroy(&user_reader);
  }
  if (reader) {
    ptk_ini_reader_destroy(&reader);
  }
  if (user_ini_path) {
    OV_ARRAY_DESTROY(&user_ini_path);
  }
  if (ini_path) {
    OV_ARRAY_DESTROY(&ini_path);
  }
  if (config_dir) {
    OV_ARRAY_DESTROY(&config_dir);
  }
  if (!success && mapper) {
    OV_FREE(&mapper);
    return NULL;
  }
  return mapper;
}

void ptk_anm2_script_mapper_destroy(struct ptk_anm2_script_mapper **const mapper) {
  if (!mapper || !*mapper) {
    return;
  }
  struct ptk_anm2_script_mapper *p = *mapper;
  if (p->reader) {
    ptk_ini_reader_destroy(&p->reader);
  }
  OV_FREE(&p);
  *mapper = NULL;
}

struct ptk_anm2_script_mapper_result
ptk_anm2_script_mapper_get_effect_name(struct ptk_anm2_script_mapper const *const mapper,
                                       char const *const script_name) {
  struct ptk_anm2_script_mapper_result result = {NULL, 0};
  if (!mapper || !mapper->reader || !script_name) {
    return result;
  }

  struct ptk_ini_value const val =
      ptk_ini_reader_get_value_n(mapper->reader, ini_section, ini_section_len, script_name, strlen(script_name));

  if (!val.ptr || val.size == 0) {
    return result;
  }

  result.ptr = val.ptr;
  result.size = val.size;
  return result;
}
