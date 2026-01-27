#include "anm2editor_import.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <string.h>

#include <ovarray.h>
#include <ovmo.h>
#include <ovprintf.h>
#include <ovutf.h>

#include <ovl/path.h>

#include <aviutl2_plugin2.h>

#include "alias.h"
#include "anm2_script_picker.h"
#include "dialog.h"
#include "error.h"
#include "win32.h"

struct get_alias_context {
  char *alias;
  struct ov_error *err;
  bool success;
};

static void get_alias_callback(void *param, struct aviutl2_edit_section *edit) {
  struct get_alias_context *ctx = (struct get_alias_context *)param;
  struct ov_error *err = ctx->err;
  char const *alias = NULL;

  aviutl2_object_handle obj = edit->get_focus_object();
  if (!obj) {
    OV_ERROR_SET(err,
                 ov_error_type_generic,
                 ptk_alias_error_no_object_selected,
                 gettext("no object is selected in AviUtl ExEdit2."));
    goto cleanup;
  }

  alias = edit->get_object_alias(obj);
  if (!alias || alias[0] == '\0') {
    OV_ERROR_SET(err,
                 ov_error_type_generic,
                 ptk_alias_error_failed_to_get_alias,
                 gettext("failed to get alias data from the selected object."));
    goto cleanup;
  }

  {
    size_t const len = strlen(alias);
    if (!OV_ARRAY_GROW(&ctx->alias, len + 1)) {
      OV_ERROR_SET_GENERIC(err, ov_error_generic_out_of_memory);
      goto cleanup;
    }
    memcpy(ctx->alias, alias, len + 1);
    OV_ARRAY_SET_LENGTH(ctx->alias, len + 1);
  }

  ctx->success = true;

cleanup:;
}

static void build_selector_name_from_psd_path(char const *const psd_path,
                                              char *const selector_name,
                                              size_t const selector_name_size) {
  if (!selector_name || selector_name_size == 0) {
    return;
  }

  bool success = false;
  selector_name[0] = '\0';

  if (!psd_path || psd_path[0] == '\0') {
    goto cleanup;
  }

  {
    char const *pipe = strchr(psd_path, '|');
    size_t path_len = pipe ? (size_t)(pipe - psd_path) : strlen(psd_path);
    if (path_len >= MAX_PATH) {
      goto cleanup;
    }

    char path_buf[MAX_PATH];
    memcpy(path_buf, psd_path, path_len);
    path_buf[path_len] = '\0';

    char const *base_name = ovl_path_extract_file_name(path_buf);
    char const *ext = ovl_path_find_ext(base_name);
    size_t base_len = ext ? (size_t)(ext - base_name) : strlen(base_name);
    if (base_len == 0 || base_len >= selector_name_size - 32 || base_len >= 256) {
      goto cleanup;
    }

    char base_name_buf[256];
    memcpy(base_name_buf, base_name, base_len);
    base_name_buf[base_len] = '\0';

    ov_snprintf_char(
        selector_name, selector_name_size, "%1$hs", pgettext("anm2editor", "%1$hs Selector"), base_name_buf);
    success = true;
  }

cleanup:
  if (!success) {
    ov_snprintf_char(selector_name, selector_name_size, "%1$hs", "%1$hs", pgettext("anm2editor", "Unnamed Selector"));
  }
}

static bool compare_psd_paths(char const *path1, char const *path2) {
  if (!path1 || path1[0] == '\0') {
    return !path2 || path2[0] == '\0';
  }
  if (!path2 || path2[0] == '\0') {
    return false;
  }
  return strcmp(path1, path2) == 0;
}

static wchar_t const *get_window_title(void) {
  static wchar_t buf[64];
  if (!buf[0]) {
    ov_snprintf_wchar(buf, sizeof(buf) / sizeof(buf[0]), L"%1$hs", L"%1$hs", gettext("PSDToolKit anm2 Editor"));
  }
  return buf;
}

static void show_hint_dialog(void *const parent_window, struct ov_error *const err) {
  wchar_t msg[256];
  wchar_t hint_text[768];
  ov_snprintf_wchar(
      msg, sizeof(msg) / sizeof(msg[0]), L"%hs", L"%hs", pgettext("anm2editor", "Welcome to Script Importer!"));
  ov_snprintf_wchar(hint_text,
                    768,
                    L"%hs",
                    L"%hs",
                    pgettext("anm2editor",
                             "1. Select a PSD File object in AviUtl ExEdit2\n"
                             "2. Add effects like \"Blinker@PSDToolKit\" and configure them\n"
                             "3. Press this button\n\n"
                             "This feature imports animation settings from the selected PSD File object."));
  ptk_error_dialog((HWND)parent_window, err, get_window_title(), msg, hint_text, TD_INFORMATION_ICON, TDCBF_OK_BUTTON);
}

bool anm2editor_import_execute(void *const parent_window,
                               struct aviutl2_edit_handle *const edit_handle,
                               char const *const current_psd_path,
                               bool const has_selected_selector,
                               anm2editor_import_callback const callback,
                               void *const callback_context,
                               struct ov_error *const err) {
  struct get_alias_context alias_ctx = {
      .err = err,
      .success = false,
  };
  struct ptk_alias_script_definitions defs = {0};
  struct ptk_alias_available_scripts *scripts = NULL;
  char *selector_name = NULL;
  bool success = false;

  if (!OV_REALLOC(&scripts, 1, sizeof(struct ptk_alias_available_scripts))) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_out_of_memory);
    goto cleanup;
  }
  *scripts = (struct ptk_alias_available_scripts){0};

  if (!edit_handle->call_edit_section_param(&alias_ctx, get_alias_callback)) {
    OV_ERROR_SET(err, ov_error_type_generic, ov_error_generic_fail, gettext("edit section is not available."));
    goto cleanup;
  }
  if (!alias_ctx.success) {
    OV_ERROR_ADD_TRACE(err);
    goto cleanup;
  }

  if (!ptk_alias_load_script_definitions(&defs, err)) {
    OV_ERROR_ADD_TRACE(err);
    goto cleanup;
  }

  if (!ptk_alias_enumerate_available_scripts(alias_ctx.alias, strlen(alias_ctx.alias), &defs, scripts, err)) {
    OV_ERROR_ADD_TRACE(err);
    goto cleanup;
  }

  // Populate translated names for dialog display
  ptk_alias_populate_translated_names(scripts);

  if (OV_ARRAY_LENGTH(scripts->items) == 0) {
    OV_ERROR_SET(err,
                 ov_error_type_generic,
                 ptk_alias_error_no_scripts,
                 gettext("no importable scripts found in the selected object."));
    goto cleanup;
  }

  // Check if we can skip the dialog
  {
    bool const psd_paths_match = compare_psd_paths(current_psd_path, scripts->psd_path);
    bool const can_skip_dialog = (OV_ARRAY_LENGTH(scripts->items) == 1) && psd_paths_match;

    if (can_skip_dialog) {
      scripts->items[0].selected = true;

      // Build selector name if needed
      if (!has_selected_selector) {
        char selector_name_buf[256];
        build_selector_name_from_psd_path(scripts->psd_path, selector_name_buf, sizeof(selector_name_buf));
        size_t const len = strlen(selector_name_buf);
        if (!OV_ARRAY_GROW(&selector_name, len + 1)) {
          OV_ERROR_SET_GENERIC(err, ov_error_generic_out_of_memory);
          goto cleanup;
        }
        memcpy(selector_name, selector_name_buf, len + 1);
        OV_ARRAY_SET_LENGTH(selector_name, len + 1);
      }

      // Call the callback to execute import
      if (!callback(callback_context, alias_ctx.alias, scripts, selector_name, false, err)) {
        OV_ERROR_ADD_TRACE(err);
        goto cleanup;
      }

      success = true;
      goto cleanup;
    }
  }

  // Show script picker dialog
  {
    struct ptk_script_picker_params picker_params = {
        .items = scripts->items,
        .item_count = OV_ARRAY_LENGTH(scripts->items),
        .current_psd_path = current_psd_path,
        .source_psd_path = scripts->psd_path,
        .update_psd_path = false,
    };

    HWND *disabled_windows = ptk_win32_disable_family_windows((HWND)parent_window);
    ov_tribool const picker_result = ptk_script_picker_show((HWND)parent_window, &picker_params, err);
    ptk_win32_restore_disabled_family_windows(disabled_windows);

    if (picker_result == ov_false) {
      // User cancelled
      success = true;
      goto cleanup;
    }
    if (picker_result == ov_indeterminate) {
      OV_ERROR_ADD_TRACE(err);
      goto cleanup;
    }

    bool has_selected = false;
    size_t const n = OV_ARRAY_LENGTH(scripts->items);
    for (size_t i = 0; i < n; i++) {
      if (scripts->items[i].selected) {
        has_selected = true;
        break;
      }
    }

    if (!has_selected && !picker_params.update_psd_path) {
      // Nothing to do
      success = true;
      goto cleanup;
    }

    // Build selector name if needed
    if (has_selected && !has_selected_selector) {
      char selector_name_buf[256];
      build_selector_name_from_psd_path(scripts->psd_path, selector_name_buf, sizeof(selector_name_buf));
      size_t const len = strlen(selector_name_buf);
      if (!OV_ARRAY_GROW(&selector_name, len + 1)) {
        OV_ERROR_SET_GENERIC(err, ov_error_generic_out_of_memory);
        goto cleanup;
      }
      memcpy(selector_name, selector_name_buf, len + 1);
      OV_ARRAY_SET_LENGTH(selector_name, len + 1);
    }

    // Call the callback to execute import
    if (!callback(callback_context, alias_ctx.alias, scripts, selector_name, picker_params.update_psd_path, err)) {
      OV_ERROR_ADD_TRACE(err);
      goto cleanup;
    }
  }

  success = true;

cleanup:
  if (selector_name) {
    OV_ARRAY_DESTROY(&selector_name);
  }
  if (scripts) {
    ptk_alias_available_scripts_free(scripts);
    OV_FREE(&scripts);
  }
  ptk_alias_script_definitions_free(&defs);
  if (alias_ctx.alias) {
    OV_ARRAY_DESTROY(&alias_ctx.alias);
  }

  // Post-cleanup UI handling: Show hint dialog for common "how to use" errors
  // This is placed after resource cleanup to ensure no leaks, but before return
  // to provide user feedback. The hint dialog consumes the error and we return true
  // to indicate the operation was handled (user saw the hint).
  if (!success) {
    if (ov_error_is(err, ov_error_type_generic, ptk_alias_error_psd_not_found) ||
        ov_error_is(err, ov_error_type_generic, ptk_alias_error_no_scripts) ||
        ov_error_is(err, ov_error_type_generic, ptk_alias_error_no_object_selected) ||
        ov_error_is(err, ov_error_type_generic, ptk_alias_error_failed_to_get_alias)) {
      show_hint_dialog(parent_window, err);
      OV_ERROR_DESTROY(err);
      success = true;
    }
  }
  return success;
}
