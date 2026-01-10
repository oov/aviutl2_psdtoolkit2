#include "config.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <ovarray.h>

#include <ovl/file.h>
#include <ovl/os.h>
#include <ovl/path.h>
#include <ovl/source.h>
#include <ovl/source/file.h>

#include "json.h"

struct ptk_config {
  // Manual drop triggers
  bool manual_shift_wav;
  bool manual_shift_psd;
  bool manual_wav_txt_pair;
  bool manual_object_audio_text;
  // External API drop triggers
  bool external_wav_txt_pair;
  bool external_object_audio_text;
  // Debug mode
  bool debug_mode;
};

static bool get_dll_directory(NATIVE_CHAR **const dir, struct ov_error *const err) {
  if (!dir) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_invalid_argument);
    return false;
  }

  NATIVE_CHAR *module_path = NULL;
  void *hinstance = NULL;
  bool result = false;

  {
    if (!ovl_os_get_hinstance_from_fnptr((void *)get_dll_directory, &hinstance, err)) {
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

    size_t const dir_len = (size_t)(last_slash - module_path);
    if (!OV_ARRAY_GROW(dir, dir_len + 1)) {
      OV_ERROR_SET_GENERIC(err, ov_error_generic_out_of_memory);
      goto cleanup;
    }

    memcpy(*dir, module_path, dir_len * sizeof(NATIVE_CHAR));
    (*dir)[dir_len] = NSTR('\0');
  }

  result = true;

cleanup:
  if (module_path) {
    OV_ARRAY_DESTROY(&module_path);
  }
  return result;
}

struct ptk_config *ptk_config_create(struct ov_error *const err) {
  struct ptk_config *cfg = NULL;
  struct ptk_config *result = NULL;

  if (!OV_REALLOC(&cfg, 1, sizeof(struct ptk_config))) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_out_of_memory);
    goto cleanup;
  }
  *cfg = (struct ptk_config){
      .manual_shift_wav = false,
      .manual_shift_psd = false,
      .manual_wav_txt_pair = false,
      .manual_object_audio_text = false,
      .external_wav_txt_pair = false,
      .external_object_audio_text = false,
      .debug_mode = false,
  };

  result = cfg;
  cfg = NULL;

cleanup:
  if (cfg) {
    ptk_config_destroy(&cfg);
  }
  return result;
}

void ptk_config_destroy(struct ptk_config **const config) {
  if (!config || !*config) {
    return;
  }
  OV_FREE(config);
}

static bool get_config_file_path(NATIVE_CHAR **const config_path, struct ov_error *const err) {
  if (!config_path) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_invalid_argument);
    return false;
  }

  NATIVE_CHAR const last_part[] = NSTR("\\PSDToolKit\\PSDToolKit.json");
  NATIVE_CHAR *dll_dir = NULL;
  bool result = false;

  if (!get_dll_directory(&dll_dir, err)) {
    OV_ERROR_ADD_TRACE(err);
    goto cleanup;
  }
  {
    size_t const dll_dir_len = wcslen(dll_dir);
    size_t const last_part_len = wcslen(last_part);
    if (!OV_ARRAY_GROW(config_path, dll_dir_len + last_part_len + 1)) {
      OV_ERROR_SET_GENERIC(err, ov_error_generic_out_of_memory);
      goto cleanup;
    }
    memcpy(*config_path, dll_dir, dll_dir_len * sizeof(NATIVE_CHAR));
    memcpy((*config_path) + dll_dir_len, last_part, last_part_len * sizeof(NATIVE_CHAR));
    (*config_path)[dll_dir_len + last_part_len] = NSTR('\0');
  }

  result = true;

cleanup:
  if (dll_dir) {
    OV_ARRAY_DESTROY(&dll_dir);
  }
  return result;
}

static char const g_json_key_version[] = "version";
static char const g_json_key_manual_shift_wav[] = "manual_shift_wav";
static char const g_json_key_manual_shift_psd[] = "manual_shift_psd";
static char const g_json_key_manual_wav_txt_pair[] = "manual_wav_txt_pair";
static char const g_json_key_manual_object_audio_text[] = "manual_object_audio_text";
static char const g_json_key_external_wav_txt_pair[] = "external_wav_txt_pair";
static char const g_json_key_external_object_audio_text[] = "external_object_audio_text";
static char const g_json_key_debug_mode[] = "debug_mode";

bool ptk_config_load(struct ptk_config *const config, struct ov_error *const err) {
  if (!config) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_invalid_argument);
    return false;
  }

  NATIVE_CHAR *config_path = NULL;
  struct ovl_source *source = NULL;
  char *json_str = NULL;
  yyjson_doc *doc = NULL;
  bool result = false;

  {
    if (!get_config_file_path(&config_path, err)) {
      OV_ERROR_ADD_TRACE(err);
      goto cleanup;
    }

    if (!ovl_source_file_create(config_path, &source, err)) {
      if (ov_error_is(err, ov_error_type_hresult, HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND))) {
        OV_ERROR_DESTROY(err);
        result = true; // Use default settings
        goto cleanup;
      }
      OV_ERROR_ADD_TRACE(err);
      goto cleanup;
    }

    uint64_t const file_size = ovl_source_size(source);
    if (file_size == UINT64_MAX) {
      OV_ERROR_SET_GENERIC(err, ov_error_generic_fail);
      goto cleanup;
    }
    if (file_size > SIZE_MAX) {
      OV_ERROR_SET_GENERIC(err, ov_error_generic_fail);
      goto cleanup;
    }

    size_t const size = (size_t)file_size;
    if (!OV_ARRAY_GROW(&json_str, size + 1)) {
      OV_ERROR_SET_GENERIC(err, ov_error_generic_out_of_memory);
      goto cleanup;
    }

    size_t const bytes_read = ovl_source_read(source, json_str, 0, size);
    if (bytes_read != size) {
      OV_ERROR_SET_GENERIC(err, ov_error_generic_fail);
      goto cleanup;
    }

    json_str[size] = '\0';

    doc = yyjson_read(json_str, strlen(json_str), 0);
    if (!doc) {
      OV_ERROR_SET_GENERIC(err, ov_error_generic_fail);
      goto cleanup;
    }

    yyjson_val *root = yyjson_doc_get_root(doc);
    if (!yyjson_is_obj(root)) {
      OV_ERROR_SET_GENERIC(err, ov_error_generic_fail);
      goto cleanup;
    }

    yyjson_val *val;

    val = yyjson_obj_get(root, g_json_key_manual_shift_wav);
    if (val && yyjson_is_bool(val)) {
      config->manual_shift_wav = yyjson_get_bool(val);
    }

    val = yyjson_obj_get(root, g_json_key_manual_shift_psd);
    if (val && yyjson_is_bool(val)) {
      config->manual_shift_psd = yyjson_get_bool(val);
    }

    val = yyjson_obj_get(root, g_json_key_manual_wav_txt_pair);
    if (val && yyjson_is_bool(val)) {
      config->manual_wav_txt_pair = yyjson_get_bool(val);
    }

    val = yyjson_obj_get(root, g_json_key_manual_object_audio_text);
    if (val && yyjson_is_bool(val)) {
      config->manual_object_audio_text = yyjson_get_bool(val);
    }

    val = yyjson_obj_get(root, g_json_key_external_wav_txt_pair);
    if (val && yyjson_is_bool(val)) {
      config->external_wav_txt_pair = yyjson_get_bool(val);
    }

    val = yyjson_obj_get(root, g_json_key_external_object_audio_text);
    if (val && yyjson_is_bool(val)) {
      config->external_object_audio_text = yyjson_get_bool(val);
    }

    val = yyjson_obj_get(root, g_json_key_debug_mode);
    if (val && yyjson_is_bool(val)) {
      config->debug_mode = yyjson_get_bool(val);
    }
  }

  result = true;

cleanup:
  if (source) {
    ovl_source_destroy(&source);
  }
  if (doc) {
    yyjson_doc_free(doc);
  }
  if (json_str) {
    OV_ARRAY_DESTROY(&json_str);
  }
  if (config_path) {
    OV_ARRAY_DESTROY(&config_path);
  }
  return result;
}

bool ptk_config_save(struct ptk_config const *const config, struct ov_error *const err) {
  if (!config) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_invalid_argument);
    return false;
  }

  NATIVE_CHAR *config_path = NULL;
  yyjson_mut_doc *doc = NULL;
  char *json_str = NULL;
  struct ovl_file *file = NULL;
  bool result = false;

  {
    if (!get_config_file_path(&config_path, err)) {
      OV_ERROR_ADD_TRACE(err);
      goto cleanup;
    }

    doc = yyjson_mut_doc_new(NULL);
    if (!doc) {
      OV_ERROR_SET(err, ov_error_type_generic, ov_error_generic_fail, "Failed to create JSON document");
      goto cleanup;
    }

    yyjson_mut_val *root = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root);

    yyjson_mut_obj_add_str(doc, root, g_json_key_version, "1.0");
    yyjson_mut_obj_add_bool(doc, root, g_json_key_manual_shift_wav, config->manual_shift_wav);
    yyjson_mut_obj_add_bool(doc, root, g_json_key_manual_shift_psd, config->manual_shift_psd);
    yyjson_mut_obj_add_bool(doc, root, g_json_key_manual_wav_txt_pair, config->manual_wav_txt_pair);
    yyjson_mut_obj_add_bool(doc, root, g_json_key_manual_object_audio_text, config->manual_object_audio_text);
    yyjson_mut_obj_add_bool(doc, root, g_json_key_external_wav_txt_pair, config->external_wav_txt_pair);
    yyjson_mut_obj_add_bool(doc, root, g_json_key_external_object_audio_text, config->external_object_audio_text);
    yyjson_mut_obj_add_bool(doc, root, g_json_key_debug_mode, config->debug_mode);

    json_str = yyjson_mut_write_opts(doc, YYJSON_WRITE_PRETTY, ptk_json_get_alc(), NULL, NULL);
    if (!json_str) {
      OV_ERROR_SET_GENERIC(err, ov_error_generic_fail);
      goto cleanup;
    }

    if (!ovl_file_create(config_path, &file, err)) {
      OV_ERROR_ADD_TRACE(err);
      goto cleanup;
    }

    size_t const json_len = strlen(json_str);
    size_t written = 0;
    if (!ovl_file_write(file, json_str, json_len, &written, err)) {
      OV_ERROR_ADD_TRACE(err);
      goto cleanup;
    }
    if (written != json_len) {
      OV_ERROR_SET_GENERIC(err, ov_error_generic_fail);
      goto cleanup;
    }
  }

  result = true;

cleanup:
  if (file) {
    ovl_file_close(file);
  }
  if (doc) {
    yyjson_mut_doc_free(doc);
  }
  if (json_str) {
    OV_FREE(&json_str);
  }
  if (config_path) {
    OV_ARRAY_DESTROY(&config_path);
  }
  return result;
}

// Getter/Setter implementations

bool ptk_config_get_manual_shift_wav(struct ptk_config const *const config,
                                     bool *const value,
                                     struct ov_error *const err) {
  if (!config || !value) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_invalid_argument);
    return false;
  }
  *value = config->manual_shift_wav;
  return true;
}

bool ptk_config_set_manual_shift_wav(struct ptk_config *const config, bool const value, struct ov_error *const err) {
  if (!config) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_invalid_argument);
    return false;
  }
  config->manual_shift_wav = value;
  return true;
}

bool ptk_config_get_manual_shift_psd(struct ptk_config const *const config,
                                     bool *const value,
                                     struct ov_error *const err) {
  if (!config || !value) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_invalid_argument);
    return false;
  }
  *value = config->manual_shift_psd;
  return true;
}

bool ptk_config_set_manual_shift_psd(struct ptk_config *const config, bool const value, struct ov_error *const err) {
  if (!config) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_invalid_argument);
    return false;
  }
  config->manual_shift_psd = value;
  return true;
}

bool ptk_config_get_manual_wav_txt_pair(struct ptk_config const *const config,
                                        bool *const value,
                                        struct ov_error *const err) {
  if (!config || !value) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_invalid_argument);
    return false;
  }
  *value = config->manual_wav_txt_pair;
  return true;
}

bool ptk_config_set_manual_wav_txt_pair(struct ptk_config *const config, bool const value, struct ov_error *const err) {
  if (!config) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_invalid_argument);
    return false;
  }
  config->manual_wav_txt_pair = value;
  return true;
}

bool ptk_config_get_manual_object_audio_text(struct ptk_config const *const config,
                                             bool *const value,
                                             struct ov_error *const err) {
  if (!config || !value) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_invalid_argument);
    return false;
  }
  *value = config->manual_object_audio_text;
  return true;
}

bool ptk_config_set_manual_object_audio_text(struct ptk_config *const config,
                                             bool const value,
                                             struct ov_error *const err) {
  if (!config) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_invalid_argument);
    return false;
  }
  config->manual_object_audio_text = value;
  return true;
}

bool ptk_config_get_external_wav_txt_pair(struct ptk_config const *const config,
                                          bool *const value,
                                          struct ov_error *const err) {
  if (!config || !value) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_invalid_argument);
    return false;
  }
  *value = config->external_wav_txt_pair;
  return true;
}

bool ptk_config_set_external_wav_txt_pair(struct ptk_config *const config,
                                          bool const value,
                                          struct ov_error *const err) {
  if (!config) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_invalid_argument);
    return false;
  }
  config->external_wav_txt_pair = value;
  return true;
}

bool ptk_config_get_external_object_audio_text(struct ptk_config const *const config,
                                               bool *const value,
                                               struct ov_error *const err) {
  if (!config || !value) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_invalid_argument);
    return false;
  }
  *value = config->external_object_audio_text;
  return true;
}

bool ptk_config_set_external_object_audio_text(struct ptk_config *const config,
                                               bool const value,
                                               struct ov_error *const err) {
  if (!config) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_invalid_argument);
    return false;
  }
  config->external_object_audio_text = value;
  return true;
}

bool ptk_config_get_debug_mode(struct ptk_config const *const config, bool *const value, struct ov_error *const err) {
  if (!config || !value) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_invalid_argument);
    return false;
  }
  *value = config->debug_mode;
  return true;
}

bool ptk_config_set_debug_mode(struct ptk_config *const config, bool const value, struct ov_error *const err) {
  if (!config) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_invalid_argument);
    return false;
  }
  config->debug_mode = value;
  return true;
}
