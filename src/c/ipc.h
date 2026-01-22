#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include "ovbase.h"

/**
 * @brief Custom error codes for IPC operations
 *
 * These codes are used with ov_error_type_generic to identify specific
 * error conditions during IPC initialization and communication.
 */
enum ptk_ipc_error {
  /**
   * @brief IPC target executable not found
   *
   * PSDToolKit.exe was not found. This typically indicates that
   * the installation files are corrupted or missing.
   */
  ptk_ipc_error_target_not_found = 4000,

  /**
   * @brief Access denied when launching IPC target
   *
   * Access to PSDToolKit.exe was denied. This is often caused by
   * antivirus software blocking the execution.
   */
  ptk_ipc_error_target_access_denied = 4001,
};

struct ipc;

struct ipc_update_editing_image_state_params {
  char const *file_path_utf8;
  char const *state_utf8;
};

struct ipc_export_faview_slider_params {
  char const *file_path_utf8;
  char const *slider_name_utf8;
  char const *names_utf8;
  char const *values_utf8;
  int32_t selected_index;
};

struct ipc_export_layer_names_params {
  char const *file_path_utf8;
  char const *names_utf8;
  char const *values_utf8;
  int32_t selected_index;
};

struct ipc_options {
  wchar_t const *exe_path;
  wchar_t const *working_dir;
  void *userdata;
  void (*on_update_editing_image_state)(void *const userdata,
                                        struct ipc_update_editing_image_state_params *const params);
  void (*on_export_faview_slider)(void *const userdata, struct ipc_export_faview_slider_params *const params);
  void (*on_export_layer_names)(void *const userdata, struct ipc_export_layer_names_params *const params);
};

NODISCARD bool ipc_init(struct ipc **const ipc, struct ipc_options const *const opt, struct ov_error *const err);
void ipc_exit(struct ipc **const ipc);

NODISCARD bool
ipc_add_file(struct ipc *const ipc, char const *const path_utf8, uint32_t const tag, struct ov_error *const err);
NODISCARD bool
ipc_update_current_project_path(struct ipc *const ipc, char const *const path_utf8, struct ov_error *const err);
NODISCARD bool ipc_clear_files(struct ipc *const ipc, struct ov_error *const err);
NODISCARD bool ipc_deserialize(struct ipc *const ipc, char const *const src_utf8, struct ov_error *const err);
NODISCARD bool ipc_draw(struct ipc *const ipc,
                        int32_t const id,
                        char const *const path_utf8,
                        void *const p,
                        int32_t const width,
                        int32_t const height,
                        struct ov_error *const err);
NODISCARD bool ipc_get_layer_names(struct ipc *const ipc,
                                   int32_t const id,
                                   char const *const path_utf8,
                                   char **const dest_utf8,
                                   struct ov_error *const err);
NODISCARD bool ipc_serialize(struct ipc *const ipc, char **const dest_utf8, struct ov_error *const err);
HWND ipc_get_window_handle(struct ipc *const ipc, struct ov_error *const err);

struct ipc_prop_params {
  char const *layer;
  float const *scale;
  int32_t const *offset_x;
  int32_t const *offset_y;
  uint32_t const *tag;
  int32_t const *quality; // 0=Fast, 1=Beautiful (matches Go ScaleQuality)
};

struct ipc_prop_result {
  bool modified;
  uint64_t ckey;
  int32_t width;
  int32_t height;
};

NODISCARD bool ipc_set_props(struct ipc *const ipc,
                             int32_t const id,
                             char const *const path_utf8,
                             struct ipc_prop_params const *const params,
                             struct ipc_prop_result *const result,
                             struct ov_error *const err);
