#include "script_module.h"

#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <ovarray.h>
#include <ovmo.h>
#include <ovrand.h>
#include <ovutf.h>

#include <aviutl2_module2.h>

#include "error.h"
#include "logf.h"

// Convert uint64 cache key to 16-character hex string
static void ckey_to_hex(uint64_t ckey, char hex[17]) {
  static char const hexchars[] = "0123456789abcdef";
  for (int i = 15; i >= 0; --i) {
    hex[i] = hexchars[ckey & 0xf];
    ckey >>= 4;
  }
  hex[16] = '\0';
}

// Convert 16-character hex string to uint64 cache key
static bool hex_to_ckey(char const *hex, uint64_t *ckey) {
  if (!hex || !ckey) {
    return false;
  }
  uint64_t result = 0;
  for (int i = 0; i < 16; ++i) {
    char c = hex[i];
    uint64_t digit;
    if (c >= '0' && c <= '9') {
      digit = (uint64_t)(c - '0');
    } else if (c >= 'a' && c <= 'f') {
      digit = (uint64_t)(c - 'a' + 10);
    } else if (c >= 'A' && c <= 'F') {
      digit = (uint64_t)(c - 'A' + 10);
    } else {
      return false;
    }
    result = (result << 4) | digit;
  }
  *ckey = result;
  return true;
}

struct ptk_script_module {
  struct ptk_script_module_callbacks callbacks;
  struct ov_rand_xoshiro256pp rng;
};

struct ptk_script_module *ptk_script_module_create(struct ptk_script_module_callbacks const *const callbacks,
                                                   struct ov_error *const err) {
  if (!callbacks) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_invalid_argument);
    return NULL;
  }

  struct ptk_script_module *sm = NULL;

  if (!OV_REALLOC(&sm, 1, sizeof(struct ptk_script_module))) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_out_of_memory);
    return NULL;
  }

  sm->callbacks = *callbacks;
  ov_rand_xoshiro256pp_init(&sm->rng, ov_rand_get_global_hint());
  return sm;
}

void ptk_script_module_destroy(struct ptk_script_module **const sm) {
  if (!sm || !*sm) {
    return;
  }
  OV_FREE(sm);
}

void ptk_script_module_get_debug_mode(struct ptk_script_module *const sm,
                                      struct aviutl2_script_module_param *const param,
                                      int const cache_index) {
  struct ov_error err = {0};
  bool debug_mode = false;
  bool success = false;

  if (!sm || !param) {
    OV_ERROR_SET_GENERIC(&err, ov_error_generic_invalid_argument);
    goto cleanup;
  }

  if (!sm->callbacks.get_debug_mode) {
    OV_ERROR_SET_GENERIC(&err, ov_error_generic_not_implemented_yet);
    goto cleanup;
  }

  if (!sm->callbacks.get_debug_mode(sm->callbacks.userdata, &debug_mode, &err)) {
    OV_ERROR_ADD_TRACE(&err);
    goto cleanup;
  }

  param->push_result_boolean(debug_mode);
  param->push_result_int(cache_index);
  success = true;

cleanup:
  if (!success) {
    param->push_result_boolean(false);
    param->push_result_int(cache_index);
    ptk_logf_error(&err, "%1$hs", "%1$hs", gettext("failed to get debug mode."));
    OV_ERROR_DESTROY(&err);
  }
}

void ptk_script_module_generate_tag(struct ptk_script_module *const sm,
                                    struct aviutl2_script_module_param *const param) {
  if (!sm || !param) {
    if (param) {
      param->push_result_int(0);
    }
    return;
  }
  uint64_t const val = ov_rand_xoshiro256pp_next(&sm->rng);
  int const tag = (int)(val & 0x7FFFFFFF);
  param->push_result_int(tag);
}

void ptk_script_module_add_psd_file(struct ptk_script_module *const sm,
                                    struct aviutl2_script_module_param *const param) {
  struct ov_error err = {0};
  bool result = false;

  if (!sm->callbacks.add_file) {
    OV_ERROR_SET_GENERIC(&err, ov_error_generic_not_implemented_yet);
    goto cleanup;
  }

  {
    char const *const path_utf8 = param->get_param_string(0);
    if (!path_utf8) {
      OV_ERROR_SET_GENERIC(&err, ov_error_generic_invalid_argument);
      goto cleanup;
    }

    int const tag_value = param->get_param_int(1);
    if (!sm->callbacks.add_file(sm->callbacks.userdata, path_utf8, (uint32_t)tag_value, &err)) {
      OV_ERROR_ADD_TRACE(&err);
      goto cleanup;
    }
  }

  result = true;

cleanup:
  param->push_result_boolean(result);
  if (!result) {
    ptk_logf_error(&err, "%1$hs", "%1$hs", gettext("failed to add PSD file."));
    OV_ERROR_DESTROY(&err);
  }
}

void ptk_script_module_set_props(struct ptk_script_module *const sm, struct aviutl2_script_module_param *const param) {
  struct ov_error err = {0};
  bool success = false;

  if (!sm->callbacks.set_props) {
    OV_ERROR_SET_GENERIC(&err, ov_error_generic_not_implemented_yet);
    goto cleanup;
  }

  {
    char const *const path_utf8 = param->get_param_string(1);
    if (!path_utf8) {
      OV_ERROR_SET_GENERIC(&err, ov_error_generic_invalid_argument);
      goto cleanup;
    }

    struct ptk_script_module_set_props_params params = {
        .id = param->get_param_int(0),
        .path_utf8 = path_utf8,
        // NULL if key not present, "" for empty string (both are valid and have different meanings)
        .layer = param->get_param_table_string(2, "layer"),
        .scale = param->get_param_table_double(2, "scale"),
        .offset_x = param->get_param_table_int(2, "offsetx"),
        .offset_y = param->get_param_table_int(2, "offsety"),
        .tag = param->get_param_table_int(2, "tag"),
    };

    struct ptk_script_module_set_props_result result = {0};
    if (!sm->callbacks.set_props(sm->callbacks.userdata, &params, &result, &err)) {
      OV_ERROR_ADD_TRACE(&err);
      goto cleanup;
    }

    // Convert ckey to hex string for Lua
    char ckey_hex[17];
    ckey_to_hex(result.ckey, ckey_hex);

    // Return 4 values: modified, cachekey_hex, width, height
    param->push_result_boolean(result.modified);
    param->push_result_string(ckey_hex);
    param->push_result_int(result.width);
    param->push_result_int(result.height);
  }

  success = true;

cleanup:
  if (!success) {
    param->push_result_boolean(false);
    param->push_result_string("");
    param->push_result_int(0);
    param->push_result_int(0);
    ptk_logf_error(&err, "%1$hs", "%1$hs", gettext("failed to set PSD properties."));
    OV_ERROR_DESTROY(&err);
  }
}

void ptk_script_module_get_drop_config(struct ptk_script_module *const sm,
                                       struct aviutl2_script_module_param *const param) {
  struct ov_error err = {0};
  bool success = false;

  if (!sm || !param) {
    OV_ERROR_SET_GENERIC(&err, ov_error_generic_invalid_argument);
    goto cleanup;
  }

  if (!sm->callbacks.get_drop_config) {
    OV_ERROR_SET_GENERIC(&err, ov_error_generic_not_implemented_yet);
    goto cleanup;
  }

  {
    struct ptk_script_module_drop_config config = {0};
    if (!sm->callbacks.get_drop_config(sm->callbacks.userdata, &config, &err)) {
      OV_ERROR_ADD_TRACE(&err);
      goto cleanup;
    }

    char const *keys[] = {
        "manual_shift_wav",
        "manual_shift_psd",
        "manual_wav_txt_pair",
        "manual_object_audio_text",
        "external_wav_txt_pair",
        "external_object_audio_text",
    };
    int values[] = {
        config.manual_shift_wav ? 1 : 0,
        config.manual_shift_psd ? 1 : 0,
        config.manual_wav_txt_pair ? 1 : 0,
        config.manual_object_audio_text ? 1 : 0,
        config.external_wav_txt_pair ? 1 : 0,
        config.external_object_audio_text ? 1 : 0,
    };
    static_assert(sizeof(keys) / sizeof(keys[0]) == sizeof(values) / sizeof(values[0]),
                  "keys and values array size mismatch");
    param->push_result_table_int(keys, values, sizeof(keys) / sizeof(keys[0]));
  }

  success = true;

cleanup:
  if (!success) {
    param->push_result_boolean(false);
    ptk_logf_error(&err, "%1$hs", "%1$hs", gettext("failed to get drop configuration."));
    OV_ERROR_DESTROY(&err);
  }
}

void ptk_script_module_draw(struct ptk_script_module *const sm, struct aviutl2_script_module_param *const param) {
  struct ov_error err = {0};
  bool success = false;

  if (!sm || !param) {
    OV_ERROR_SET_GENERIC(&err, ov_error_generic_invalid_argument);
    goto cleanup;
  }

  if (!sm->callbacks.draw) {
    OV_ERROR_SET_GENERIC(&err, ov_error_generic_not_implemented_yet);
    goto cleanup;
  }

  {
    int const id = param->get_param_int(0);
    char const *const path_utf8 = param->get_param_string(1);
    int const width = param->get_param_int(2);
    int const height = param->get_param_int(3);
    char const *const cachekey_hex = param->get_param_string(4);

    if (!path_utf8 || !cachekey_hex || width <= 0 || height <= 0) {
      OV_ERROR_SET_GENERIC(&err, ov_error_generic_invalid_argument);
      goto cleanup;
    }

    // Parse cachekey hex string to uint64
    uint64_t ckey = 0;
    if (!hex_to_ckey(cachekey_hex, &ckey)) {
      OV_ERROR_SET_GENERIC(&err, ov_error_generic_invalid_argument);
      goto cleanup;
    }

    if (!sm->callbacks.draw(sm->callbacks.userdata, id, path_utf8, width, height, ckey, &err)) {
      OV_ERROR_ADD_TRACE(&err);
      goto cleanup;
    }
  }

  param->push_result_boolean(true);
  success = true;

cleanup:
  if (!success) {
    param->push_result_boolean(false);
    ptk_logf_error(&err, "%1$hs", "%1$hs", gettext("failed to draw PSD image."));
    OV_ERROR_DESTROY(&err);
  }
}

void ptk_script_module_get_preferred_languages(struct ptk_script_module *const sm,
                                               struct aviutl2_script_module_param *const param) {
  (void)sm;
  struct ov_error err = {0};
  wchar_t *preferred_w = NULL;
  char **langs = NULL;
  char const **langs_const = NULL;
  bool success = false;

  {
    if (!mo_get_preferred_ui_languages(&preferred_w, &err)) {
      OV_ERROR_ADD_TRACE(&err);
      goto cleanup;
    }

    // Count and convert languages
    for (wchar_t const *l = preferred_w; *l != L'\0'; l += wcslen(l) + 1) {
      size_t const wlen = wcslen(l);
      size_t const utf8_len = ov_wchar_to_utf8_len(l, wlen);
      if (utf8_len == 0) {
        continue;
      }

      char *lang = NULL;
      if (!OV_ARRAY_GROW(&lang, utf8_len + 1)) {
        OV_ERROR_SET_GENERIC(&err, ov_error_generic_out_of_memory);
        goto cleanup;
      }
      ov_wchar_to_utf8(l, wlen, lang, utf8_len + 1, NULL);

      // Convert hyphen to underscore for Lua compatibility (ja-JP -> ja_JP)
      for (size_t i = 0; i < utf8_len; ++i) {
        if (lang[i] == '-') {
          lang[i] = '_';
        }
      }

      if (!OV_ARRAY_PUSH(&langs, lang)) {
        OV_ARRAY_DESTROY(&lang);
        OV_ERROR_SET_GENERIC(&err, ov_error_generic_out_of_memory);
        goto cleanup;
      }
    }

    size_t const num_langs = OV_ARRAY_LENGTH(langs);
    if (num_langs == 0) {
      // No languages found, return empty array
      param->push_result_array_string(NULL, 0);
      success = true;
      goto cleanup;
    }

    // Create const pointer array for push_result_array_string
    if (!OV_ARRAY_GROW(&langs_const, num_langs)) {
      OV_ERROR_SET_GENERIC(&err, ov_error_generic_out_of_memory);
      goto cleanup;
    }
    for (size_t i = 0; i < num_langs; ++i) {
      langs_const[i] = langs[i];
    }

    param->push_result_array_string((LPCSTR *)langs_const, (int)num_langs);
  }

  success = true;

cleanup:
  if (langs_const) {
    OV_ARRAY_DESTROY(&langs_const);
  }
  if (langs) {
    size_t const n = OV_ARRAY_LENGTH(langs);
    for (size_t i = 0; i < n; ++i) {
      if (langs[i]) {
        OV_ARRAY_DESTROY(&langs[i]);
      }
    }
    OV_ARRAY_DESTROY(&langs);
  }
  if (preferred_w) {
    OV_ARRAY_DESTROY(&preferred_w);
  }
  if (!success) {
    param->push_result_array_string(NULL, 0);
    ptk_logf_error(&err, "%1$hs", "%1$hs", gettext("failed to get preferred languages."));
    OV_ERROR_DESTROY(&err);
  }
}

void ptk_script_module_read_text_file(struct ptk_script_module *const sm,
                                      struct aviutl2_script_module_param *const param) {
  (void)sm;
  struct ov_error err = {0};
  wchar_t *path_w = NULL;
  char *content = NULL;
  char *error_msg = NULL;
  HANDLE h = INVALID_HANDLE_VALUE;
  bool success = false;

  {
    char const *const path_utf8 = param->get_param_string(0);
    if (!path_utf8) {
      OV_ERROR_SET_GENERIC(&err, ov_error_generic_invalid_argument);
      goto cleanup;
    }

    // Convert UTF-8 path to wide string for Windows API
    size_t const path_len = strlen(path_utf8);
    size_t const wide_len = ov_utf8_to_wchar_len(path_utf8, path_len);
    if (wide_len == 0) {
      OV_ERROR_SET_GENERIC(&err, ov_error_generic_invalid_argument);
      goto cleanup;
    }

    if (!OV_ARRAY_GROW(&path_w, wide_len + 1)) {
      OV_ERROR_SET_GENERIC(&err, ov_error_generic_out_of_memory);
      goto cleanup;
    }
    ov_utf8_to_wchar(path_utf8, path_len, path_w, wide_len + 1, NULL);

    // Open file using Windows API with wide string path
    h = CreateFileW(path_w, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) {
      OV_ERROR_SET_HRESULT(&err, HRESULT_FROM_WIN32(GetLastError()));
      goto cleanup;
    }

    // Get file size
    LARGE_INTEGER file_size;
    if (!GetFileSizeEx(h, &file_size)) {
      OV_ERROR_SET_HRESULT(&err, HRESULT_FROM_WIN32(GetLastError()));
      goto cleanup;
    }

    // Limit file size to reasonable value (16MB)
    if (file_size.QuadPart > 16 * 1024 * 1024) {
      OV_ERROR_SETF(&err,
                    ov_error_type_generic,
                    ov_error_generic_fail,
                    "%1$lld",
                    gettext("file size %1$lld bytes exceeds limit."),
                    (long long)file_size.QuadPart);
      goto cleanup;
    }

    size_t const size = (size_t)file_size.QuadPart;

    // Allocate buffer for content (+1 for null terminator)
    if (!OV_ARRAY_GROW(&content, size + 1)) {
      OV_ERROR_SET_GENERIC(&err, ov_error_generic_out_of_memory);
      goto cleanup;
    }

    // Read file content
    DWORD bytes_read = 0;
    if (!ReadFile(h, content, (DWORD)size, &bytes_read, NULL)) {
      OV_ERROR_SET_HRESULT(&err, HRESULT_FROM_WIN32(GetLastError()));
      goto cleanup;
    }

    // Null terminate
    content[bytes_read] = '\0';

    // Return content as string (single return value on success)
    param->push_result_string(content);
  }

  success = true;

cleanup:
  if (h != INVALID_HANDLE_VALUE) {
    CloseHandle(h);
    h = INVALID_HANDLE_VALUE;
  }
  if (content) {
    OV_ARRAY_DESTROY(&content);
  }
  if (path_w) {
    OV_ARRAY_DESTROY(&path_w);
  }
  if (!success) {
    // Return nil, error_message (like io.open)
    param->push_result_string(NULL);
    // Get main error message for Lua
    wchar_t *error_msg_w = NULL;
    if (ptk_error_get_main_message(&err, &error_msg_w)) {
      size_t const wlen = wcslen(error_msg_w);
      size_t const utf8_len = ov_wchar_to_utf8_len(error_msg_w, wlen);
      if (utf8_len > 0 && OV_ARRAY_GROW(&error_msg, utf8_len + 1)) {
        ov_wchar_to_utf8(error_msg_w, wlen, error_msg, utf8_len + 1, NULL);
        param->push_result_string(error_msg);
      } else {
        param->push_result_string(gettext("failed to read text file."));
      }
      OV_ARRAY_DESTROY(&error_msg_w);
    } else {
      param->push_result_string(gettext("failed to read text file."));
    }
    ptk_logf_error(&err, "%1$hs", "%1$hs", gettext("failed to read text file."));
    OV_ERROR_DESTROY(&err);
  }
  if (error_msg) {
    OV_ARRAY_DESTROY(&error_msg);
  }
}
