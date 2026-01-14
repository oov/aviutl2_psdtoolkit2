#include "psdtoolkit.h"

#include <ovarray.h>
#include <ovbase.h>
#include <ovmo.h>
#include <ovprintf.h>
#include <ovprintf_ex.h>
#include <ovrand.h>
#include <ovthreads.h>
#include <ovutf.h>

#include <string.h>

#include <ovl/dialog.h>
#include <ovl/file.h>
#include <ovl/os.h>
#include <ovl/path.h>

#include <aviutl2_module2.h>
#include <aviutl2_plugin2.h>

#include "cache.h"
#include "config.h"
#include "config_dialog.h"
#include "dialog.h"
#include "error.h"
#include "ipc.h"
#include "layer.h"
#include "logf.h"
#include "script_module.h"
#include "version.h"

#include <commctrl.h>

struct psdtoolkit {
  struct aviutl2_edit_handle *edit;
  struct ptk_anm2editor *anm2editor;
  struct ptk_config *config;
  struct ptk_cache *cache;
  struct ipc *ipc;
  struct ptk_script_module *script_module;
  HWND hwnd_psdtoolkit;
  HWND plugin_window;
  ATOM plugin_window_class;
};

static bool sm_get_debug_mode(void *const userdata, bool *const debug_mode, struct ov_error *const err) {
  struct psdtoolkit *const ptk = (struct psdtoolkit *)userdata;
  if (!ptk || !ptk->config) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_unexpected);
    return false;
  }
  if (!ptk_config_get_debug_mode(ptk->config, debug_mode, err)) {
    OV_ERROR_ADD_TRACE(err);
    return false;
  }
  return true;
}

static bool
sm_add_file(void *const userdata, char const *const path_utf8, uint32_t const tag, struct ov_error *const err) {
  struct psdtoolkit *const ptk = (struct psdtoolkit *)userdata;
  if (!ptk || !ptk->ipc) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_unexpected);
    return false;
  }
  if (!ipc_add_file(ptk->ipc, path_utf8, tag, err)) {
    OV_ERROR_ADD_TRACE(err);
    return false;
  }
  return true;
}

static bool sm_set_props(void *const userdata,
                         struct ptk_script_module_set_props_params const *const params,
                         struct ptk_script_module_set_props_result *const result,
                         struct ov_error *const err) {
  struct psdtoolkit *const ptk = (struct psdtoolkit *)userdata;
  if (!ptk || !ptk->ipc || !params || !result) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_unexpected);
    return false;
  }

  // Convert ptk_script_module_set_props_params to ipc_prop_params
  float scale_val = (float)params->scale;
  int32_t offset_x_val = params->offset_x;
  int32_t offset_y_val = params->offset_y;
  uint32_t tag_val = (uint32_t)params->tag;

  struct ipc_prop_params ipc_params = {
      .layer = params->layer,
      .scale = &scale_val,
      .offset_x = &offset_x_val,
      .offset_y = &offset_y_val,
      .tag = &tag_val,
  };

  struct ipc_prop_result ipc_result = {0};
  if (!ipc_set_props(ptk->ipc, params->id, params->path_utf8, &ipc_params, &ipc_result, err)) {
    OV_ERROR_ADD_TRACE(err);
    return false;
  }

  result->modified = ipc_result.modified;
  result->ckey = ipc_result.ckey;
  result->width = ipc_result.width;
  result->height = ipc_result.height;
  return true;
}

static bool sm_get_drop_config(void *const userdata,
                               struct ptk_script_module_drop_config *const config,
                               struct ov_error *const err) {
  struct psdtoolkit *const ptk = (struct psdtoolkit *)userdata;
  if (!ptk || !ptk->config || !config) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_invalid_argument);
    return false;
  }

  bool b = false;

#define GET_CONFIG(name)                                                                                               \
  if (!ptk_config_get_##name(ptk->config, &b, err)) {                                                                  \
    OV_ERROR_ADD_TRACE(err);                                                                                           \
    return false;                                                                                                      \
  }                                                                                                                    \
  config->name = b

  GET_CONFIG(manual_shift_wav);
  GET_CONFIG(manual_shift_psd);
  GET_CONFIG(manual_wav_txt_pair);
  GET_CONFIG(manual_object_audio_text);
  GET_CONFIG(external_wav_txt_pair);
  GET_CONFIG(external_object_audio_text);
#undef GET_CONFIG

  return true;
}

static bool sm_draw(void *const userdata,
                    int const id,
                    char const *const path_utf8,
                    int32_t const width,
                    int32_t const height,
                    uint64_t const ckey,
                    struct ov_error *const err) {
  struct psdtoolkit *const ptk = (struct psdtoolkit *)userdata;
  if (!ptk || !ptk->ipc || !ptk->cache) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_invalid_argument);
    return false;
  }

  uint8_t *pixels = NULL;
  bool success = false;

  {
    // Allocate buffer for pixel data
    size_t const data_size = (size_t)width * (size_t)height * 4;
    if (!OV_REALLOC(&pixels, data_size, 1)) {
      OV_ERROR_SET_GENERIC(err, ov_error_generic_out_of_memory);
      goto cleanup;
    }

    // Call IPC to render
    if (!ipc_draw(ptk->ipc, id, path_utf8, pixels, width, height, err)) {
      OV_ERROR_ADD_TRACE(err);
      goto cleanup;
    }

    // Flip vertically (top-down to bottom-up for BITMAP)
    size_t const stride = (size_t)width * 4;
    for (int y = 0; y < height / 2; y++) {
      uint8_t *top = pixels + (size_t)y * stride;
      uint8_t *bottom = pixels + (size_t)(height - 1 - y) * stride;
      for (size_t x = 0; x < stride; x++) {
        uint8_t tmp = top[x];
        top[x] = bottom[x];
        bottom[x] = tmp;
      }
    }

    // Store in cache
    if (!ptk_cache_put(ptk->cache, ckey, pixels, width, height, err)) {
      OV_ERROR_ADD_TRACE(err);
      goto cleanup;
    }
  }

  success = true;

cleanup:
  if (pixels) {
    OV_FREE(&pixels);
  }
  return success;
}

struct ptk_script_module *psdtoolkit_get_script_module(struct psdtoolkit *const ptk) {
  return ptk ? ptk->script_module : NULL;
}

static bool update_ipc_project_path(struct psdtoolkit *const ptk,
                                    struct aviutl2_project_file *project,
                                    struct ov_error *const err) {
  char buf[MAX_PATH];
  char *p = NULL;
  bool success = false;
  wchar_t const *const project_path = project->get_project_file_path();
  if (project_path && project_path[0] != L'\0') {
    size_t const wlen = wcslen(project_path);
    size_t const u8len = ov_wchar_to_utf8_len(project_path, wlen);
    if (u8len < MAX_PATH) {
      ov_wchar_to_utf8(project_path, wlen, buf, sizeof(buf), NULL);
      p = buf;
    } else {
      if (!OV_ARRAY_GROW(&p, u8len + 1)) {
        OV_ERROR_SET_GENERIC(err, ov_error_generic_out_of_memory);
        goto cleanup;
      }
      ov_wchar_to_utf8(project_path, wlen, p, u8len + 1, NULL);
    }
  } else {
    buf[0] = '\0';
    p = buf;
  }
  if (!ipc_update_current_project_path(ptk->ipc, p, err)) {
    OV_ERROR_ADD_TRACE(err);
    goto cleanup;
  }
  success = true;

cleanup:
  if (p && p != buf) {
    OV_ARRAY_DESTROY(&p);
  }
  return success;
}

static bool
get_ptk_project_path(struct aviutl2_project_file *const project, wchar_t **const path, struct ov_error *const err) {
  static wchar_t const sidecar_ext[] = L".psdtoolkit";
  static size_t const sidecar_ext_len = sizeof(sidecar_ext) / sizeof(sidecar_ext[0]) - 1;

  wchar_t const *project_path = NULL;
  size_t project_path_len = 0;
  wchar_t *ext = NULL;
  bool success = false;

  project_path = project->get_project_file_path();
  if (!project_path || project_path[0] == L'\0') {
    *path = NULL;
    success = true;
    goto cleanup;
  }

  project_path_len = wcslen(project_path);
  if (!OV_ARRAY_GROW(path, project_path_len + sidecar_ext_len + 1)) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_out_of_memory);
    goto cleanup;
  }
  wcscpy(*path, project_path);
  ext = ovl_path_find_ext(*path);
  if (!ext) {
    ext = *path + project_path_len;
  }
  wcscpy(ext, sidecar_ext);
  OV_ARRAY_SET_LENGTH(*path, (size_t)(ext - *path) + sidecar_ext_len);
  success = true;

cleanup:
  return success;
}

void psdtoolkit_project_load_handler(struct psdtoolkit *const ptk, struct aviutl2_project_file *const project) {
  struct ov_error err = {0};
  wchar_t *path = NULL;
  struct ovl_file *file = NULL;
  char *data = NULL;
  uint64_t file_size = 0;
  size_t read_bytes = 0;
  bool success = false;

  if (!ptk || !ptk->ipc) {
    OV_ERROR_SET_GENERIC(&err, ov_error_generic_unexpected);
    goto cleanup;
  }

  if (!update_ipc_project_path(ptk, project, &err)) {
    OV_ERROR_ADD_TRACE(&err);
    goto cleanup;
  }

  if (!get_ptk_project_path(project, &path, &err)) {
    OV_ERROR_ADD_TRACE(&err);
    goto cleanup;
  }

  if (!path) {
    success = true;
    goto cleanup;
  }

  if (!ovl_file_open(path, &file, &err)) {
    if (ov_error_is(&err, ov_error_type_hresult, HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND)) ||
        ov_error_is(&err, ov_error_type_hresult, HRESULT_FROM_WIN32(ERROR_PATH_NOT_FOUND))) {
      OV_ERROR_DESTROY(&err);
      success = true;
      goto cleanup;
    }
    OV_ERROR_ADD_TRACE(&err);
    goto cleanup;
  }

  if (!ovl_file_size(file, &file_size, &err)) {
    OV_ERROR_ADD_TRACE(&err);
    goto cleanup;
  }

  if (file_size > 0) {
    if (!OV_ARRAY_GROW(&data, file_size + 1)) {
      OV_ERROR_SET_GENERIC(&err, ov_error_generic_out_of_memory);
      goto cleanup;
    }
    if (!ovl_file_read(file, data, (size_t)file_size, &read_bytes, &err)) {
      OV_ERROR_ADD_TRACE(&err);
      goto cleanup;
    }
    data[read_bytes] = '\0';
  }
  success = true;

cleanup:
  if (file) {
    ovl_file_close(file);
  }
  if (path) {
    OV_ARRAY_DESTROY(&path);
  }
  if (success) {
    if (!ipc_deserialize(ptk->ipc, data ? data : "", &err)) {
      OV_ERROR_ADD_TRACE(&err);
      success = false;
    }
  }
  if (data) {
    OV_ARRAY_DESTROY(&data);
  }
  if (!success) {
    ptk_logf_error(&err, "%1$hs", "%1$hs", gettext("An error occurred while loading project data."));
  }
  OV_ERROR_REPORT(&err, NULL);
}

void psdtoolkit_project_save_handler(struct psdtoolkit *const ptk, struct aviutl2_project_file *const project) {
  struct ov_error err = {0};
  wchar_t *path = NULL;
  struct ovl_file *file = NULL;
  char *data = NULL;
  size_t data_len = 0;
  size_t written = 0;
  bool success = false;

  if (!ptk || !ptk->ipc) {
    OV_ERROR_SET_GENERIC(&err, ov_error_generic_unexpected);
    goto cleanup;
  }

  if (!update_ipc_project_path(ptk, project, &err)) {
    OV_ERROR_ADD_TRACE(&err);
    goto cleanup;
  }

  if (!get_ptk_project_path(project, &path, &err)) {
    OV_ERROR_ADD_TRACE(&err);
    goto cleanup;
  }

  if (!path) {
    // Project not yet saved to disk, skip saving sidecar file
    success = true;
    goto cleanup;
  }

  if (!ipc_serialize(ptk->ipc, &data, &err)) {
    OV_ERROR_ADD_TRACE(&err);
    goto cleanup;
  }

  if (!ovl_file_create(path, &file, &err)) {
    OV_ERROR_ADD_TRACE(&err);
    goto cleanup;
  }

  data_len = strlen(data);
  if (!ovl_file_write(file, data, data_len, &written, &err)) {
    OV_ERROR_ADD_TRACE(&err);
    goto cleanup;
  }
  success = true;

cleanup:
  if (file) {
    ovl_file_close(file);
    file = NULL;
  }
  if (path) {
    OV_ARRAY_DESTROY(&path);
  }
  if (data) {
    OV_ARRAY_DESTROY(&data);
  }
  if (!success) {
    ptk_logf_error(&err, "%1$hs", "%1$hs", gettext("An error occurred while saving project data."));
  }
  OV_ERROR_REPORT(&err, NULL);
}

void psdtoolkit_show_config_dialog(struct psdtoolkit *const ptk, void *const hwnd) {
  if (!ptk) {
    return;
  }
  struct ov_error err = {0};
  bool success = false;
  if (!ptk_config_dialog_show(ptk->config, (HWND)hwnd, &err)) {
    OV_ERROR_ADD_TRACE(&err);
    goto cleanup;
  }
  success = true;
cleanup:
  if (!success) {
    ptk_logf_error(&err, "%1$hs", "%1$hs", gettext("failed to show configuration dialog."));
    OV_ERROR_DESTROY(&err);
  }
}

struct update_editing_image_state_context {
  char const *file_path_utf8;
  char const *state_utf8;
  // Output: set by procs
  struct ov_error *err;
  bool success;
  bool safeguard_enabled;
  char *current_file_path;
};

// Custom window messages for deferred processing
#define WM_PTK_UPDATE_EDITING_IMAGE_STATE (WM_APP + 1)
#define WM_PTK_EXPORT_LAYER_NAMES (WM_APP + 2)
#define WM_PTK_EXPORT_FAVIEW_SLIDER (WM_APP + 3)

// Parameters for WM_PTK_UPDATE_EDITING_IMAGE_STATE message
struct update_editing_image_state_msg_params {
  struct psdtoolkit *ptk;
  char *file_path_utf8; // owned, must be freed
  char *state_utf8;     // owned, must be freed
};

// Parameters for WM_PTK_EXPORT_LAYER_NAMES message
struct export_layer_names_msg_params {
  char *file_path_utf8; // owned, must be freed
  char *names_utf8;     // owned, must be freed (NUL-separated list)
  char *values_utf8;    // owned, must be freed (NUL-separated list)
  size_t names_len;     // total length including NULs
  size_t values_len;    // total length including NULs
  int32_t selected_index;
};

// Parameters for WM_PTK_EXPORT_FAVIEW_SLIDER message
struct export_faview_slider_msg_params {
  char *file_path_utf8;   // owned, must be freed
  char *slider_name_utf8; // owned, must be freed
  char *names_utf8;       // owned, must be freed (NUL-separated list)
  char *values_utf8;      // owned, must be freed (NUL-separated list)
  size_t names_len;       // total length including NULs
  size_t values_len;      // total length including NULs
  int32_t selected_index;
};

static wchar_t const psd_effect_name[] = L"PSDファイル@PSDToolKit";
static wchar_t const psd_file_item[] = L"PSDファイル";
static wchar_t const layer_item[] = L"レイヤー";
static wchar_t const safeguard_item[] = L"セーフガード";

// Get current file path and safeguard setting from the selected object
static void get_editing_image_state_proc(void *param, struct aviutl2_edit_section *edit) {
  struct update_editing_image_state_context *ctx = (struct update_editing_image_state_context *)param;
  bool success = false;

  aviutl2_object_handle obj = edit->get_focus_object();
  if (!obj) {
    OV_ERROR_SETF(ctx->err,
                  ov_error_type_generic,
                  ov_error_generic_not_found,
                  "%1$hs",
                  "%1$hs",
                  gettext("No object is selected. Please select a PSD object in the timeline."));
    goto cleanup;
  }

  if (edit->count_object_effect(obj, psd_effect_name) == 0) {
    OV_ERROR_SETF(ctx->err,
                  ov_error_type_generic,
                  ov_error_generic_not_found,
                  "%1$hs",
                  gettext("The selected object does not have a %1$hs."),
                  psd_effect_name);
    goto cleanup;
  }

  {
    char const *const current_file_path = edit->get_object_item_value(obj, psd_effect_name, psd_file_item);
    if (!current_file_path) {
      OV_ERROR_SETF(ctx->err,
                    ov_error_type_generic,
                    ov_error_generic_fail,
                    "%1$hs",
                    gettext("failed to get %1$hs on %2$hs."),
                    psd_file_item,
                    psd_effect_name);
      goto cleanup;
    }
    size_t const len = strlen(current_file_path);
    if (!OV_ARRAY_GROW(&ctx->current_file_path, len + 1)) {
      OV_ERROR_SET_GENERIC(ctx->err, ov_error_generic_out_of_memory);
      goto cleanup;
    }
    memcpy(ctx->current_file_path, current_file_path, len + 1);

    char const *const safeguard_value = edit->get_object_item_value(obj, psd_effect_name, safeguard_item);
    ctx->safeguard_enabled = !safeguard_value || strcmp(safeguard_value, "0") != 0;
  }

  success = true;

cleanup:
  ctx->success = success;
}

// Actually update the object values
static void do_update_editing_image_state_proc(void *param, struct aviutl2_edit_section *edit) {
  struct update_editing_image_state_context *ctx = (struct update_editing_image_state_context *)param;

  bool success = false;

  aviutl2_object_handle obj = edit->get_focus_object();
  if (!obj) {
    OV_ERROR_SETF(ctx->err,
                  ov_error_type_generic,
                  ov_error_generic_not_found,
                  "%1$hs",
                  "%1$hs",
                  gettext("No object is selected. Please select a PSD object in the timeline."));
    goto cleanup;
  }

  if (edit->count_object_effect(obj, psd_effect_name) == 0) {
    OV_ERROR_SETF(ctx->err,
                  ov_error_type_generic,
                  ov_error_generic_not_found,
                  "%1$hs",
                  gettext("The selected object does not have a %1$hs."),
                  psd_effect_name);
    goto cleanup;
  }

  if (!edit->set_object_item_value(obj, psd_effect_name, psd_file_item, ctx->file_path_utf8)) {
    OV_ERROR_SETF(ctx->err,
                  ov_error_type_generic,
                  ov_error_generic_fail,
                  "%1$hs",
                  gettext("failed to set %1$hs on %2$hs."),
                  psd_file_item,
                  psd_effect_name);
    goto cleanup;
  }

  if (!edit->set_object_item_value(obj, psd_effect_name, layer_item, ctx->state_utf8)) {
    OV_ERROR_SETF(ctx->err,
                  ov_error_type_generic,
                  ov_error_generic_fail,
                  "%1$hs",
                  gettext("failed to set %1$hs on %2$hs."),
                  layer_item,
                  psd_effect_name);
    goto cleanup;
  }

  {
    char const *const filename_utf8 = ovl_path_extract_file_name(ctx->file_path_utf8);
    wchar_t filename_wchar[MAX_PATH];
    if (ov_utf8_to_wchar(filename_utf8, strlen(filename_utf8), filename_wchar, MAX_PATH, NULL) > 0) {
      edit->set_object_name(obj, filename_wchar);
    }
  }

  success = true;

cleanup:
  ctx->success = success;
}

// Process the deferred update_editing_image_state request
static void
process_update_editing_image_state(struct psdtoolkit *ptk, char const *file_path_utf8, char const *state_utf8) {
  struct ov_error err = {0};
  bool success = false;
  struct update_editing_image_state_context ctx = {0};

  if (!file_path_utf8 || !state_utf8 || !ptk->edit) {
    OV_ERROR_SET_GENERIC(&err, ov_error_generic_unexpected);
    goto cleanup;
  }

  ctx = (struct update_editing_image_state_context){
      .file_path_utf8 = file_path_utf8,
      .state_utf8 = state_utf8,
      .err = &err,
  };

  ptk->edit->call_edit_section_param(&ctx, get_editing_image_state_proc);
  if (!ctx.success) {
    OV_ERROR_ADD_TRACE(&err);
    goto cleanup;
  }
  ctx.success = false;

  // Check if safeguard dialog is needed
  if (ctx.safeguard_enabled && strcmp(ctx.current_file_path, file_path_utf8) != 0) {
    wchar_t main_instruction[256];
    wchar_t content[256];
    wchar_t detail[1024];
    ov_snprintf_char2wchar(main_instruction,
                           256,
                           NULL,
                           gettext("A different PSD file is assigned to the destination. Do you want to continue?"),
                           NULL);
    ov_snprintf_char2wchar(
        content,
        256,
        NULL,
        gettext("Note: Uncheck the safeguard checkbox in the PSD object settings to disable this dialog."),
        NULL);
    ov_snprintf_char2wchar(detail,
                           1024,
                           NULL,
                           gettext("Current PSD file object:\n"
                                   "%1$s\n\n"
                                   "PSD file to be assigned:\n"
                                   "%2$s"),
                           ctx.current_file_path,
                           file_path_utf8);
    int const button_id = ptk_dialog_show(&(struct ptk_dialog_params){
        .owner = ptk->plugin_window,
        .icon = TD_WARNING_ICON,
        .buttons = TDCBF_OK_BUTTON | TDCBF_CANCEL_BUTTON,
        .default_button = IDCANCEL,
        .window_title = L"PSDToolKit",
        .main_instruction = main_instruction,
        .content = content,
        .expanded_info = detail,
    });
    if (button_id != IDOK) {
      success = true; // User cancelled, not an error
      goto cleanup;
    }
  }

  ptk->edit->call_edit_section_param(&ctx, do_update_editing_image_state_proc);
  if (!ctx.success) {
    OV_ERROR_ADD_TRACE(&err);
    goto cleanup;
  }

  success = true;

cleanup:
  if (ctx.current_file_path) {
    OV_ARRAY_DESTROY(&ctx.current_file_path);
  }
  if (!success) {
    wchar_t main_instruction[256];
    ov_snprintf_wchar(main_instruction,
                      sizeof(main_instruction) / sizeof(main_instruction[0]),
                      L"%1$hs",
                      L"%1$hs",
                      gettext("Failed to update editing image state."));
    ptk_error_dialog(ptk->plugin_window, &err, L"PSDToolKit", main_instruction, NULL, TD_ERROR_ICON, TDCBF_OK_BUTTON);
    OV_ERROR_DESTROY(&err);
  }
}

static void ipc_on_update_editing_image_state(void *const userdata,
                                              struct ipc_update_editing_image_state_params *const params) {
  struct psdtoolkit *ptk = (struct psdtoolkit *)userdata;
  if (!ptk || !ptk->plugin_window) {
    return;
  }

  if (!params->file_path_utf8 || !params->state_utf8) {
    return;
  }

  struct ov_error err = {0};
  struct update_editing_image_state_msg_params *msg_params = NULL;
  bool success = false;

  if (!OV_REALLOC(&msg_params, 1, sizeof(struct update_editing_image_state_msg_params))) {
    OV_ERROR_SET_GENERIC(&err, ov_error_generic_out_of_memory);
    goto cleanup;
  }

  {
    size_t const file_path_len = strlen(params->file_path_utf8) + 1;
    size_t const state_len = strlen(params->state_utf8) + 1;

    msg_params->ptk = ptk;
    msg_params->file_path_utf8 = NULL;
    msg_params->state_utf8 = NULL;
    if (!OV_REALLOC(&msg_params->file_path_utf8, file_path_len, sizeof(char))) {
      OV_ERROR_SET_GENERIC(&err, ov_error_generic_out_of_memory);
      goto cleanup;
    }
    if (!OV_REALLOC(&msg_params->state_utf8, state_len, sizeof(char))) {
      OV_ERROR_SET_GENERIC(&err, ov_error_generic_out_of_memory);
      goto cleanup;
    }

    memcpy(msg_params->file_path_utf8, params->file_path_utf8, file_path_len);
    memcpy(msg_params->state_utf8, params->state_utf8, state_len);
  }

  // Use PostMessageW to defer processing to the main thread's message loop.
  // This avoids deadlock: the IPC callback runs on the Go IPC thread, and if we show
  // a dialog here, it blocks the IPC thread. Since the plugin window is a child of
  // AviUtl's window hierarchy (via SetParent), the dialog's message pump can cause
  // cross-thread message dispatching issues that lead to deadlock.
  if (!PostMessageW(ptk->plugin_window, WM_PTK_UPDATE_EDITING_IMAGE_STATE, 0, (LPARAM)msg_params)) {
    OV_ERROR_SET_HRESULT(&err, HRESULT_FROM_WIN32(GetLastError()));
    goto cleanup;
  }

  success = true;

cleanup:
  if (!success) {
    if (msg_params) {
      OV_FREE(&msg_params->file_path_utf8);
      OV_FREE(&msg_params->state_utf8);
      OV_FREE(&msg_params);
    }
    ptk_logf_error(&err, NULL, NULL);
    OV_ERROR_DESTROY(&err);
  }
}

static LRESULT CALLBACK plugin_wnd_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
  static wchar_t const plugin_window_prop_name[] = L"psdtoolkit";
  (void)wparam;
  struct psdtoolkit *ptk = (struct psdtoolkit *)GetPropW(hwnd, plugin_window_prop_name);
  switch (message) {
  case WM_CREATE: {
    CREATESTRUCTW const *cs = (CREATESTRUCTW const *)lparam;
    SetPropW(hwnd, plugin_window_prop_name, cs->lpCreateParams);
    return 0;
  }
  case WM_CLOSE:
    DestroyWindow(hwnd);
    return 0;
  case WM_DESTROY:
    RemovePropW(hwnd, plugin_window_prop_name);
    // Detach Go window and restore its style before destroying plugin window
    if (ptk && ptk->hwnd_psdtoolkit) {
      ShowWindow(ptk->hwnd_psdtoolkit, SW_HIDE);
      SetWindowLongPtrW(
          ptk->hwnd_psdtoolkit, GWL_STYLE, (GetWindowLongPtrW(ptk->hwnd_psdtoolkit, GWL_STYLE) | WS_POPUP) & ~WS_CHILD);
      SetParent(ptk->hwnd_psdtoolkit, NULL);
      ptk->hwnd_psdtoolkit = NULL;
    }
    return 0;
  case WM_ERASEBKGND:
    // Suppress background erasure to prevent flickering
    return 0;
  case WM_SIZE:
    // Resize child PSDToolKit window to fill the plugin window
    if (ptk && ptk->hwnd_psdtoolkit) {
      RECT rc;
      GetClientRect(hwnd, &rc);
      SetWindowPos(ptk->hwnd_psdtoolkit,
                   NULL,
                   0,
                   0,
                   rc.right - rc.left,
                   rc.bottom - rc.top,
                   SWP_NOZORDER | SWP_NOACTIVATE | SWP_ASYNCWINDOWPOS);
    }
    return 0;
  case WM_PTK_UPDATE_EDITING_IMAGE_STATE: {
    struct update_editing_image_state_msg_params *msg_params = (struct update_editing_image_state_msg_params *)lparam;
    if (!msg_params) {
      return 0;
    }
    process_update_editing_image_state(msg_params->ptk, msg_params->file_path_utf8, msg_params->state_utf8);
    OV_FREE(&msg_params->file_path_utf8);
    OV_FREE(&msg_params->state_utf8);
    OV_FREE(&msg_params);
    return 0;
  }
  case WM_PTK_EXPORT_LAYER_NAMES: {
    struct export_layer_names_msg_params *msg_params = (struct export_layer_names_msg_params *)lparam;
    if (!msg_params) {
      return 0;
    }
    if (ptk->edit) {
      ptk_layer_export(ptk->plugin_window,
                       ptk->hwnd_psdtoolkit,
                       ptk->edit,
                       ptk->anm2editor,
                       &(struct ptk_layer_export_params){
                           .file_path_utf8 = msg_params->file_path_utf8,
                           .names_utf8 = msg_params->names_utf8,
                           .values_utf8 = msg_params->values_utf8,
                           .names_len = msg_params->names_len,
                           .values_len = msg_params->values_len,
                           .selected_index = msg_params->selected_index,
                       });
    }
    OV_FREE(&msg_params->file_path_utf8);
    OV_FREE(&msg_params->names_utf8);
    OV_FREE(&msg_params->values_utf8);
    OV_FREE(&msg_params);
    return 0;
  }
  case WM_PTK_EXPORT_FAVIEW_SLIDER: {
    struct export_faview_slider_msg_params *msg_params = (struct export_faview_slider_msg_params *)lparam;
    if (!msg_params) {
      return 0;
    }
    if (ptk->edit) {
      ptk_faview_slider_export(ptk->plugin_window,
                               ptk->hwnd_psdtoolkit,
                               ptk->edit,
                               ptk->anm2editor,
                               &(struct ptk_faview_slider_export_params){
                                   .file_path_utf8 = msg_params->file_path_utf8,
                                   .slider_name_utf8 = msg_params->slider_name_utf8,
                                   .names_utf8 = msg_params->names_utf8,
                                   .values_utf8 = msg_params->values_utf8,
                                   .names_len = msg_params->names_len,
                                   .values_len = msg_params->values_len,
                                   .selected_index = msg_params->selected_index,
                               });
    }
    OV_FREE(&msg_params->file_path_utf8);
    OV_FREE(&msg_params->slider_name_utf8);
    OV_FREE(&msg_params->names_utf8);
    OV_FREE(&msg_params->values_utf8);
    OV_FREE(&msg_params);
    return 0;
  }
  }
  return DefWindowProcW(hwnd, message, wparam, lparam);
}

static void ipc_on_export_faview_slider(void *const userdata, struct ipc_export_faview_slider_params *const params) {
  struct psdtoolkit *ptk = (struct psdtoolkit *)userdata;
  if (!ptk || !ptk->plugin_window) {
    return;
  }

  if (!params->file_path_utf8 || !params->names_utf8 || !params->values_utf8 || !params->slider_name_utf8) {
    return;
  }

  struct ov_error err = {0};
  struct export_faview_slider_msg_params *msg_params = NULL;
  bool success = false;

  if (!OV_REALLOC(&msg_params, 1, sizeof(struct export_faview_slider_msg_params))) {
    OV_ERROR_SET_GENERIC(&err, ov_error_generic_out_of_memory);
    goto cleanup;
  }

  {
    size_t const file_path_len = strlen(params->file_path_utf8) + 1;
    size_t const slider_name_len = strlen(params->slider_name_utf8) + 1;
    // Use OV_ARRAY_LENGTH to get the full length including embedded NULs
    size_t const names_len = OV_ARRAY_LENGTH(params->names_utf8);
    size_t const values_len = OV_ARRAY_LENGTH(params->values_utf8);

    msg_params->file_path_utf8 = NULL;
    msg_params->slider_name_utf8 = NULL;
    msg_params->names_utf8 = NULL;
    msg_params->values_utf8 = NULL;
    msg_params->names_len = names_len;
    msg_params->values_len = values_len;
    msg_params->selected_index = params->selected_index;

    if (!OV_REALLOC(&msg_params->file_path_utf8, file_path_len, sizeof(char))) {
      OV_ERROR_SET_GENERIC(&err, ov_error_generic_out_of_memory);
      goto cleanup;
    }
    if (!OV_REALLOC(&msg_params->slider_name_utf8, slider_name_len, sizeof(char))) {
      OV_ERROR_SET_GENERIC(&err, ov_error_generic_out_of_memory);
      goto cleanup;
    }
    if (!OV_REALLOC(&msg_params->names_utf8, names_len, sizeof(char))) {
      OV_ERROR_SET_GENERIC(&err, ov_error_generic_out_of_memory);
      goto cleanup;
    }
    if (!OV_REALLOC(&msg_params->values_utf8, values_len, sizeof(char))) {
      OV_ERROR_SET_GENERIC(&err, ov_error_generic_out_of_memory);
      goto cleanup;
    }

    memcpy(msg_params->file_path_utf8, params->file_path_utf8, file_path_len);
    memcpy(msg_params->slider_name_utf8, params->slider_name_utf8, slider_name_len);
    memcpy(msg_params->names_utf8, params->names_utf8, names_len);
    memcpy(msg_params->values_utf8, params->values_utf8, values_len);
  }

  if (!PostMessageW(ptk->plugin_window, WM_PTK_EXPORT_FAVIEW_SLIDER, 0, (LPARAM)msg_params)) {
    OV_ERROR_SET_HRESULT(&err, HRESULT_FROM_WIN32(GetLastError()));
    goto cleanup;
  }

  success = true;

cleanup:
  if (!success) {
    if (msg_params) {
      OV_FREE(&msg_params->file_path_utf8);
      OV_FREE(&msg_params->slider_name_utf8);
      OV_FREE(&msg_params->names_utf8);
      OV_FREE(&msg_params->values_utf8);
      OV_FREE(&msg_params);
    }
    ptk_logf_error(&err, NULL, NULL);
    OV_ERROR_DESTROY(&err);
  }
}

static void ipc_on_export_layer_names(void *const userdata, struct ipc_export_layer_names_params *const params) {
  struct psdtoolkit *ptk = (struct psdtoolkit *)userdata;
  if (!ptk || !ptk->plugin_window) {
    return;
  }

  if (!params->file_path_utf8 || !params->names_utf8 || !params->values_utf8) {
    return;
  }

  struct ov_error err = {0};
  struct export_layer_names_msg_params *msg_params = NULL;
  bool success = false;

  if (!OV_REALLOC(&msg_params, 1, sizeof(struct export_layer_names_msg_params))) {
    OV_ERROR_SET_GENERIC(&err, ov_error_generic_out_of_memory);
    goto cleanup;
  }

  {
    size_t const file_path_len = strlen(params->file_path_utf8) + 1;
    // Use OV_ARRAY_LENGTH to get the full length including embedded NULs
    size_t const names_len = OV_ARRAY_LENGTH(params->names_utf8);
    size_t const values_len = OV_ARRAY_LENGTH(params->values_utf8);

    msg_params->file_path_utf8 = NULL;
    msg_params->names_utf8 = NULL;
    msg_params->values_utf8 = NULL;
    msg_params->names_len = names_len;
    msg_params->values_len = values_len;
    msg_params->selected_index = params->selected_index;

    if (!OV_REALLOC(&msg_params->file_path_utf8, file_path_len, sizeof(char))) {
      OV_ERROR_SET_GENERIC(&err, ov_error_generic_out_of_memory);
      goto cleanup;
    }
    if (!OV_REALLOC(&msg_params->names_utf8, names_len, sizeof(char))) {
      OV_ERROR_SET_GENERIC(&err, ov_error_generic_out_of_memory);
      goto cleanup;
    }
    if (!OV_REALLOC(&msg_params->values_utf8, values_len, sizeof(char))) {
      OV_ERROR_SET_GENERIC(&err, ov_error_generic_out_of_memory);
      goto cleanup;
    }

    memcpy(msg_params->file_path_utf8, params->file_path_utf8, file_path_len);
    memcpy(msg_params->names_utf8, params->names_utf8, names_len);
    memcpy(msg_params->values_utf8, params->values_utf8, values_len);
  }

  if (!PostMessageW(ptk->plugin_window, WM_PTK_EXPORT_LAYER_NAMES, 0, (LPARAM)msg_params)) {
    OV_ERROR_SET_HRESULT(&err, HRESULT_FROM_WIN32(GetLastError()));
    goto cleanup;
  }

  success = true;

cleanup:
  if (!success) {
    if (msg_params) {
      OV_FREE(&msg_params->file_path_utf8);
      OV_FREE(&msg_params->names_utf8);
      OV_FREE(&msg_params->values_utf8);
      OV_FREE(&msg_params);
    }
    ptk_logf_error(&err, NULL, NULL);
    OV_ERROR_DESTROY(&err);
  }
}

static bool initialize_ipc(HINSTANCE const hinst, struct psdtoolkit *const ptk, struct ov_error *const err) {
  wchar_t *exe_path = NULL;
  wchar_t *working_dir = NULL;
  bool result = false;

  if (!ovl_path_get_module_name(&exe_path, hinst, err)) {
    OV_ERROR_ADD_TRACE(err);
    goto cleanup;
  }
  {
    wchar_t *sep = ovl_path_find_last_path_sep(exe_path);
    if (sep) {
      *(sep + 1) = L'\0';
      OV_ARRAY_SET_LENGTH(exe_path, (size_t)(sep - exe_path + 1));
    } else {
      exe_path[0] = L'\0';
      OV_ARRAY_SET_LENGTH(exe_path, 0);
    }

    static wchar_t const name[] = L"PSDToolKit\\PSDToolKit.exe";
    static size_t const name_len = sizeof(name) / sizeof(name[0]) - 1;
    size_t const current_len = OV_ARRAY_LENGTH(exe_path);
    if (!OV_ARRAY_GROW(&exe_path, current_len + name_len + 1)) {
      OV_ERROR_SET_GENERIC(err, ov_error_generic_out_of_memory);
      goto cleanup;
    }
    memcpy(exe_path + current_len, name, (name_len + 1) * sizeof(wchar_t));
    OV_ARRAY_SET_LENGTH(exe_path, current_len + name_len);
  }

  {
    wchar_t *sep = ovl_path_find_last_path_sep(exe_path);
    if (!sep) {
      OV_ERROR_SET_GENERIC(err, ov_error_generic_unexpected);
      goto cleanup;
    }
    size_t const dir_len = (size_t)(sep - exe_path);
    if (!OV_ARRAY_GROW(&working_dir, dir_len + 1)) {
      OV_ERROR_SET_GENERIC(err, ov_error_generic_out_of_memory);
      goto cleanup;
    }
    memcpy(working_dir, exe_path, dir_len * sizeof(wchar_t));
    working_dir[dir_len] = L'\0';
    OV_ARRAY_SET_LENGTH(working_dir, dir_len);
  }

  if (!ipc_init(&ptk->ipc,
                &(struct ipc_options){
                    .exe_path = exe_path,
                    .working_dir = working_dir,
                    .userdata = ptk,
                    .on_update_editing_image_state = ipc_on_update_editing_image_state,
                    .on_export_faview_slider = ipc_on_export_faview_slider,
                    .on_export_layer_names = ipc_on_export_layer_names,
                },
                err)) {
    OV_ERROR_ADD_TRACE(err);
    goto cleanup;
  }
  result = true;

cleanup:
  if (working_dir) {
    OV_ARRAY_DESTROY(&working_dir);
  }
  if (exe_path) {
    OV_ARRAY_DESTROY(&exe_path);
  }
  return result;
}

static wchar_t const plugin_window_class_name[] = L"PSDToolKitWindowContainer";

void *
psdtoolkit_create_plugin_window(struct psdtoolkit *const ptk, wchar_t const *const title, struct ov_error *const err) {
  HWND plugin_window = NULL;

  HINSTANCE const hinst = GetModuleHandleW(NULL);
  ATOM const atom = RegisterClassExW(&(WNDCLASSEXW){
      .cbSize = sizeof(WNDCLASSEXW),
      .lpszClassName = plugin_window_class_name,
      .lpfnWndProc = plugin_wnd_proc,
      .hInstance = hinst,
      .hbrBackground = (HBRUSH)(COLOR_WINDOW + 1),
      .hCursor = LoadCursorW(NULL, MAKEINTRESOURCEW(32512)),
  });
  if (!atom) {
    OV_ERROR_SET_HRESULT(err, HRESULT_FROM_WIN32(GetLastError()));
    goto cleanup;
  }
  ptk->plugin_window_class = atom;

  plugin_window = CreateWindowExW(0,
                                  plugin_window_class_name,
                                  title,
                                  WS_POPUP,
                                  CW_USEDEFAULT,
                                  CW_USEDEFAULT,
                                  CW_USEDEFAULT,
                                  CW_USEDEFAULT,
                                  NULL,
                                  NULL,
                                  hinst,
                                  ptk);
  if (!plugin_window) {
    OV_ERROR_SET_HRESULT(err, HRESULT_FROM_WIN32(GetLastError()));
    goto cleanup;
  }

  // Get window handle from Go process and set up as child window
  ptk->hwnd_psdtoolkit = ipc_get_window_handle(ptk->ipc, err);
  if (!ptk->hwnd_psdtoolkit) {
    OV_ERROR_ADD_TRACE(err);
    DestroyWindow(plugin_window);
    goto cleanup;
  }
  SetParent(ptk->hwnd_psdtoolkit, plugin_window);
  SetWindowLongPtrW(ptk->hwnd_psdtoolkit,
                    GWL_STYLE,
                    (GetWindowLongPtrW(ptk->hwnd_psdtoolkit, GWL_STYLE) | WS_CHILD) &
                        ~(WS_POPUP | WS_CAPTION | WS_THICKFRAME));
  SendMessageW(plugin_window, WM_SIZE, 0, 0);
  ShowWindow(ptk->hwnd_psdtoolkit, SW_SHOW);

  ptk->plugin_window = plugin_window;
  return plugin_window;

cleanup:
  if (atom) {
    UnregisterClassW(plugin_window_class_name, hinst);
    ptk->plugin_window_class = 0;
  }
  return NULL;
}

void psdtoolkit_set_edit_handle(struct psdtoolkit *const ptk, struct aviutl2_edit_handle *const edit) {
  if (ptk) {
    ptk->edit = edit;
  }
}

void psdtoolkit_set_anm2editor(struct psdtoolkit *const ptk, struct ptk_anm2editor *const anm2editor) {
  if (ptk) {
    ptk->anm2editor = anm2editor;
  }
}

NODISCARD struct psdtoolkit *psdtoolkit_create(struct ptk_cache *const cache, struct ov_error *const err) {
  if (!cache) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_invalid_argument);
    return NULL;
  }

  struct psdtoolkit *ptk = NULL;

  {
    if (!OV_REALLOC(&ptk, 1, sizeof(struct psdtoolkit))) {
      OV_ERROR_SET_GENERIC(err, ov_error_generic_out_of_memory);
      goto cleanup;
    }
    *ptk = (struct psdtoolkit){0};

    ptk->cache = cache;

    // Create and load config
    ptk->config = ptk_config_create(err);
    if (!ptk->config) {
      OV_ERROR_ADD_TRACE(err);
      goto cleanup;
    }
    if (!ptk_config_load(ptk->config, err)) {
      ptk_logf_warn(err, "%s", "%s", gettext("failed to load config, continuing with default settings."));
      OV_ERROR_DESTROY(err);
    }

    void *dll_hinst = NULL;
    if (!ovl_os_get_hinstance_from_fnptr((void *)psdtoolkit_create, &dll_hinst, NULL)) {
      OV_ERROR_SET_HRESULT(err, HRESULT_FROM_WIN32(GetLastError()));
      goto cleanup;
    }
    if (!initialize_ipc((HINSTANCE)dll_hinst, ptk, err)) {
      OV_ERROR_ADD_TRACE(err);
      goto cleanup;
    }

    ptk->script_module = ptk_script_module_create(
        &(struct ptk_script_module_callbacks){
            .userdata = ptk,
            .get_debug_mode = sm_get_debug_mode,
            .add_file = sm_add_file,
            .set_props = sm_set_props,
            .get_drop_config = sm_get_drop_config,
            .draw = sm_draw,
        },
        err);
    if (!ptk->script_module) {
      OV_ERROR_ADD_TRACE(err);
      goto cleanup;
    }
  }

  return ptk;

cleanup:
  psdtoolkit_destroy(&ptk);
  return NULL;
}

void psdtoolkit_destroy(struct psdtoolkit **const ptk_ptr) {
  if (!ptk_ptr || !*ptk_ptr) {
    return;
  }
  struct psdtoolkit *ptk = *ptk_ptr;

  // Destroy plugin window
  if (ptk->plugin_window) {
    DestroyWindow(ptk->plugin_window);
    ptk->plugin_window = NULL;
  }
  if (ptk->plugin_window_class) {
    UnregisterClassW(plugin_window_class_name, GetModuleHandleW(NULL));
    ptk->plugin_window_class = 0;
  }
  // Note: cache is not owned by this instance, do not destroy it
  ptk->cache = NULL;
  if (ptk->script_module) {
    ptk_script_module_destroy(&ptk->script_module);
  }
  if (ptk->ipc) {
    ipc_exit(&ptk->ipc);
  }
  if (ptk->config) {
    ptk_config_destroy(&ptk->config);
  }

  OV_FREE(ptk_ptr);
}
