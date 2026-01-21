#include "anm2editor_detail.h"

#include "anm2_edit.h"
#include "logf.h"

#include <ovmo.h>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <commctrl.h>

#include <ovarray.h>
#include <ovprintf.h>
#include <ovutf.h>

/**
 * @brief Internal structure for detail list component
 */
struct anm2editor_detail {
  HWND listview;                                // ListView control handle
  HWND parent;                                  // Parent window handle
  struct anm2editor_detail_callbacks callbacks; // Event callbacks
  struct anm2editor_detail_row *rows;           // ovarray of row metadata
  struct ptk_anm2_edit *edit;                   // anm2_edit instance for direct access

  // Inline edit state
  HWND edit_control;     // Edit control for inline editing
  int edit_row;          // Row being edited (-1 if not editing)
  int edit_column;       // Column being edited (0=property, 1=value)
  size_t edit_row_index; // Index into rows for the row being edited
  WNDPROC edit_oldproc;  // Original window procedure for edit control
  bool edit_committing;  // Reentrancy guard for commit/cancel
  bool edit_adding_new;  // True if editing a new parameter (not yet added)
};

/**
 * @brief Report an error through the on_error callback or log it
 */
static void report_error(struct anm2editor_detail *detail, struct ov_error *err) {
  if (detail->callbacks.on_error) {
    detail->callbacks.on_error(detail->callbacks.userdata, err);
  } else {
    ptk_logf_error(err, "%1$hs", "%1$hs", gettext("failed to perform detail operation."));
    OV_ERROR_DESTROY(err);
  }
}

/**
 * @brief Delete a parameter row by its row index
 */
static void delete_param_row(struct anm2editor_detail *detail, size_t row_index) {
  if (!detail || !detail->edit) {
    return;
  }
  struct anm2editor_detail_row const *row_info = anm2editor_detail_get_row(detail, row_index);
  if (!row_info || !anm2editor_detail_row_type_is_deletable_param(row_info->type)) {
    return;
  }
  struct ov_error err = {0};
  if (!ptk_anm2_edit_param_remove(detail->edit, row_info->param_id, &err)) {
    OV_ERROR_ADD_TRACE(&err);
    report_error(detail, &err);
  }
}

/**
 * @brief Check if adding new parameter is allowed
 */
static bool can_add_new_param(struct anm2editor_detail *detail) {
  if (!detail || !detail->edit) {
    return false;
  }
  struct ptk_anm2_edit_state state = {0};
  ptk_anm2_edit_get_state(detail->edit, &state);
  if (state.focus_type != ptk_anm2_edit_focus_item) {
    return false;
  }
  return ptk_anm2_edit_item_is_animation(detail->edit, state.focus_id);
}

static void rows_clear(struct anm2editor_detail *detail) {
  if (detail->rows) {
    OV_ARRAY_SET_LENGTH(detail->rows, 0);
  }
}

static bool rows_add(struct anm2editor_detail *detail,
                     struct anm2editor_detail_row const *row,
                     size_t *out_index,
                     struct ov_error *const err) {
  size_t const idx = OV_ARRAY_LENGTH(detail->rows);
  if (!OV_ARRAY_PUSH(&detail->rows, *row)) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_out_of_memory);
    return false;
  }
  if (out_index) {
    *out_index = idx;
  }
  return true;
}

static bool rows_insert_at(struct anm2editor_detail *detail,
                           size_t index,
                           struct anm2editor_detail_row const *row,
                           struct ov_error *const err) {
  size_t const len = OV_ARRAY_LENGTH(detail->rows);
  if (index > len) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_invalid_argument);
    return false;
  }
  // First, grow the array by pushing a copy of the row (will be overwritten)
  if (!OV_ARRAY_PUSH(&detail->rows, *row)) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_out_of_memory);
    return false;
  }
  // Shift elements from index to end
  for (size_t i = len; i > index; i--) {
    detail->rows[i] = detail->rows[i - 1];
  }
  detail->rows[index] = *row;
  return true;
}

static void rows_remove_at(struct anm2editor_detail *detail, size_t index) {
  size_t const len = OV_ARRAY_LENGTH(detail->rows);
  if (index >= len) {
    return;
  }
  // Shift elements from index+1 to end
  for (size_t i = index; i < len - 1; i++) {
    detail->rows[i] = detail->rows[i + 1];
  }
  OV_ARRAY_SET_LENGTH(detail->rows, len - 1);
}

static struct anm2editor_detail_row const *rows_get(struct anm2editor_detail const *detail, size_t index) {
  if (index >= OV_ARRAY_LENGTH(detail->rows)) {
    return NULL;
  }
  return &detail->rows[index];
}

static size_t rows_find_by_type(struct anm2editor_detail const *detail, enum anm2editor_detail_row_type type) {
  size_t const len = OV_ARRAY_LENGTH(detail->rows);
  for (size_t i = 0; i < len; i++) {
    if (detail->rows[i].type == type) {
      return i;
    }
  }
  return SIZE_MAX;
}

static void listview_update_row_lparam(HWND listview, int start_row) {
  int const count = (int)SendMessageW(listview, LVM_GETITEMCOUNT, 0, 0);
  for (int i = start_row; i < count; i++) {
    SendMessageW(listview,
                 LVM_SETITEMW,
                 0,
                 (LPARAM) & (LVITEMW){
                                .mask = LVIF_PARAM,
                                .iItem = i,
                                .lParam = (LPARAM)(size_t)i,
                            });
  }
}

static void cancel_edit_internal(struct anm2editor_detail *detail) {
  if (!detail || !detail->edit_control || detail->edit_committing) {
    return;
  }

  detail->edit_committing = true;

  // Save state before modifying
  HWND edit_hwnd = detail->edit_control;
  WNDPROC old_proc = detail->edit_oldproc;

  // Clear state first
  detail->edit_control = NULL;
  detail->edit_row = -1;
  detail->edit_column = -1;
  detail->edit_row_index = 0;
  detail->edit_oldproc = NULL;
  detail->edit_adding_new = false;

  // Remove property and restore original window procedure before destroying
  RemovePropW(edit_hwnd, L"anm2editor_detail");
  if (old_proc) {
    SetWindowLongPtrW(edit_hwnd, GWLP_WNDPROC, (LONG_PTR)old_proc);
  }
  DestroyWindow(edit_hwnd);

  detail->edit_committing = false;
}

static void commit_edit_internal(struct anm2editor_detail *detail) {
  if (!detail || !detail->edit_control || detail->edit_row < 0 || detail->edit_committing) {
    return;
  }

  detail->edit_committing = true;

  struct ov_error err = {0};
  bool success = false;

  // Get new value from edit control
  int const text_len = GetWindowTextLengthW(detail->edit_control);
  wchar_t *new_value_w = NULL;
  char *utf8_buf = NULL;
  char *new_value_utf8 = NULL;

  if (text_len > 0) {
    if (!OV_ARRAY_GROW(&new_value_w, (size_t)text_len + 1)) {
      goto cleanup;
    }
    GetWindowTextW(detail->edit_control, new_value_w, text_len + 1);
    OV_ARRAY_SET_LENGTH(new_value_w, (size_t)text_len);

    // Convert to UTF-8 using temporary buffer
    size_t const utf8_buf_len = (size_t)(text_len + 1) * 4; // Worst case
    if (!OV_ARRAY_GROW(&utf8_buf, utf8_buf_len)) {
      goto cleanup;
    }
    size_t const utf8_written = ov_wchar_to_utf8(new_value_w, (size_t)text_len, utf8_buf, utf8_buf_len, NULL);

    // Copy to OV_ARRAY_GROW-allocated memory
    if (!OV_ARRAY_GROW(&new_value_utf8, utf8_written + 1)) {
      goto cleanup;
    }
    memcpy(new_value_utf8, utf8_buf, utf8_written + 1);
    OV_ARRAY_SET_LENGTH(new_value_utf8, utf8_written);
  }

  // Get row metadata to determine what we're editing
  {
    struct anm2editor_detail_row const *row_info = rows_get(detail, detail->edit_row_index);
    if (!row_info) {
      // Invalid row index - should not happen
      success = true;
      goto cleanup;
    }

    char const *value = new_value_utf8 ? new_value_utf8 : "";

    if (row_info->type == anm2editor_detail_row_type_placeholder) {
      // Adding new parameter
      if (detail->edit_adding_new && value[0] != '\0' && detail->edit) {
        if (!ptk_anm2_edit_param_add_for_focus(detail->edit, value, &err)) {
          OV_ERROR_ADD_TRACE(&err);
          goto cleanup;
        }
      }
      // If empty, just cancel (do nothing)
      success = true;
      goto cleanup;
    }

    // Handle edit based on row type
    if (!detail->edit) {
      success = true;
      goto cleanup;
    }

    switch (row_info->type) {
    case anm2editor_detail_row_type_label:
      if (!ptk_anm2_edit_set_label(detail->edit, value, &err)) {
        OV_ERROR_ADD_TRACE(&err);
        goto cleanup;
      }
      break;

    case anm2editor_detail_row_type_psd_path:
      if (!ptk_anm2_edit_set_psd_path(detail->edit, value, &err)) {
        OV_ERROR_ADD_TRACE(&err);
        goto cleanup;
      }
      break;

    case anm2editor_detail_row_type_exclusive_support_default: {
      // "0" or empty = false, anything else = true
      bool const new_bool = (value[0] != '\0' && value[0] != '0');
      if (!ptk_anm2_edit_set_exclusive_support_default(detail->edit, new_bool, &err)) {
        OV_ERROR_ADD_TRACE(&err);
        goto cleanup;
      }
      break;
    }

    case anm2editor_detail_row_type_information: {
      // Empty string means auto-generate (NULL)
      char const *info_value = (value[0] == '\0') ? NULL : value;
      if (!ptk_anm2_edit_set_information(detail->edit, info_value, &err)) {
        OV_ERROR_ADD_TRACE(&err);
        goto cleanup;
      }
      break;
    }

    case anm2editor_detail_row_type_default_character_id: {
      // Empty string means clear character ID (NULL)
      char const *char_id = (value[0] == '\0') ? NULL : value;
      if (!ptk_anm2_edit_set_default_character_id(detail->edit, char_id, &err)) {
        OV_ERROR_ADD_TRACE(&err);
        goto cleanup;
      }
      break;
    }

    case anm2editor_detail_row_type_multisel_item: {
      uint32_t const item_id = row_info->item_id;
      if (detail->edit_column == 0) {
        // Editing Name column
        if (!ptk_anm2_edit_rename_item(detail->edit, item_id, value, &err)) {
          OV_ERROR_ADD_TRACE(&err);
          goto cleanup;
        }
      } else {
        // Editing Value column
        if (!ptk_anm2_edit_set_item_value(detail->edit, item_id, value, &err)) {
          OV_ERROR_ADD_TRACE(&err);
          goto cleanup;
        }
      }
      break;
    }

    case anm2editor_detail_row_type_animation_param: {
      uint32_t const pid = row_info->param_id;
      if (detail->edit_column == 0) {
        // Editing key column
        if (!ptk_anm2_edit_param_set_key(detail->edit, pid, value, &err)) {
          OV_ERROR_ADD_TRACE(&err);
          goto cleanup;
        }
      } else {
        // Editing value column
        if (!ptk_anm2_edit_param_set_value(detail->edit, pid, value, &err)) {
          OV_ERROR_ADD_TRACE(&err);
          goto cleanup;
        }
      }
      break;
    }

    case anm2editor_detail_row_type_value_item: {
      // Value item (single selection) - use focus item
      struct ptk_anm2_edit_state state = {0};
      ptk_anm2_edit_get_state(detail->edit, &state);
      if (state.focus_type != ptk_anm2_edit_focus_item) {
        success = true;
        goto cleanup;
      }
      if (detail->edit_column == 0) {
        // Editing Name column
        if (!ptk_anm2_edit_rename_item(detail->edit, state.focus_id, value, &err)) {
          OV_ERROR_ADD_TRACE(&err);
          goto cleanup;
        }
      } else {
        // Editing Value column
        if (!ptk_anm2_edit_set_item_value(detail->edit, state.focus_id, value, &err)) {
          OV_ERROR_ADD_TRACE(&err);
          goto cleanup;
        }
      }
      break;
    }

    case anm2editor_detail_row_type_placeholder:
      // Already handled above
      break;
    }
  }

  success = true;

cleanup:
  if (!success) {
    report_error(detail, &err);
  }
  if (new_value_w) {
    OV_ARRAY_DESTROY(&new_value_w);
  }
  if (utf8_buf) {
    OV_ARRAY_DESTROY(&utf8_buf);
  }
  if (new_value_utf8) {
    OV_ARRAY_DESTROY(&new_value_utf8);
  }

  // Destroy edit control directly (can't call cancel_edit_internal due to reentrancy guard)
  if (detail->edit_control) {
    HWND edit_hwnd = detail->edit_control;
    WNDPROC old_proc = detail->edit_oldproc;

    detail->edit_control = NULL;
    detail->edit_row = -1;
    detail->edit_column = -1;
    detail->edit_row_index = 0;
    detail->edit_oldproc = NULL;
    detail->edit_adding_new = false;

    RemovePropW(edit_hwnd, L"anm2editor_detail");
    if (old_proc) {
      SetWindowLongPtrW(edit_hwnd, GWLP_WNDPROC, (LONG_PTR)old_proc);
    }
    DestroyWindow(edit_hwnd);
  }

  detail->edit_committing = false;
}

static LRESULT CALLBACK edit_subclass_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
  struct anm2editor_detail *detail = (struct anm2editor_detail *)GetPropW(hwnd, L"anm2editor_detail");
  if (!detail) {
    return DefWindowProcW(hwnd, msg, wparam, lparam);
  }

  switch (msg) {
  case WM_KEYDOWN:
    if (wparam == VK_RETURN) {
      commit_edit_internal(detail);
      return 0;
    }
    if (wparam == VK_ESCAPE) {
      cancel_edit_internal(detail);
      return 0;
    }
    break;
  case WM_KILLFOCUS:
    // Commit when losing focus
    commit_edit_internal(detail);
    return 0;
  }

  return CallWindowProcW(detail->edit_oldproc, hwnd, msg, wparam, lparam);
}

struct anm2editor_detail *anm2editor_detail_create(void *parent_window,
                                                   int control_id,
                                                   struct ptk_anm2_edit *edit,
                                                   struct anm2editor_detail_callbacks const *callbacks,
                                                   struct ov_error *err) {
  if (!parent_window) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_invalid_argument);
    return NULL;
  }

  struct anm2editor_detail *detail = NULL;
  bool success = false;

  if (!OV_REALLOC(&detail, 1, sizeof(*detail))) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_out_of_memory);
    goto cleanup;
  }
  *detail = (struct anm2editor_detail){
      .parent = (HWND)parent_window,
      .edit = edit,
      .edit_row = -1,
      .edit_column = -1,
  };

  if (callbacks) {
    detail->callbacks = *callbacks;
  }

  // Create ListView control
  detail->listview = CreateWindowExW(0,
                                     WC_LISTVIEWW,
                                     NULL,
                                     WS_CHILD | WS_VISIBLE | WS_BORDER | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS,
                                     0,
                                     0,
                                     100,
                                     100,
                                     (HWND)parent_window,
                                     (HMENU)(intptr_t)control_id,
                                     GetModuleHandleW(NULL),
                                     NULL);
  if (!detail->listview) {
    OV_ERROR_SET_HRESULT(err, HRESULT_FROM_WIN32(GetLastError()));
    goto cleanup;
  }

  // Set extended styles (full row select, grid lines)
  SendMessageW(detail->listview, LVM_SETEXTENDEDLISTVIEWSTYLE, 0, LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES);

  // Store pointer to self for subclass callbacks
  SetPropW(detail->listview, L"anm2editor_detail", detail);

  // Add columns
  // Note: Column headers should be set by the caller via a localization function
  // For now, we use generic names that the caller can override
  {
    LVCOLUMNW lvc = {
        .mask = LVCF_FMT | LVCF_WIDTH | LVCF_TEXT,
        .fmt = LVCFMT_LEFT,
        .cx = 120,
        .pszText = L"Property",
    };
    SendMessageW(detail->listview, LVM_INSERTCOLUMNW, 0, (LPARAM)&lvc);

    lvc.cx = 200;
    lvc.pszText = L"Value";
    SendMessageW(detail->listview, LVM_INSERTCOLUMNW, 1, (LPARAM)&lvc);
  }

  success = true;

cleanup:
  if (!success && detail) {
    if (detail->listview) {
      DestroyWindow(detail->listview);
    }
    OV_FREE(&detail);
  }
  return detail;
}

void anm2editor_detail_destroy(struct anm2editor_detail **detail) {
  if (!detail || !*detail) {
    return;
  }
  struct anm2editor_detail *d = *detail;

  // Cancel any active edit
  cancel_edit_internal(d);

  if (d->listview) {
    RemovePropW(d->listview, L"anm2editor_detail");
    d->listview = NULL;
  }

  if (d->rows) {
    OV_ARRAY_DESTROY(&d->rows);
  }

  OV_FREE(detail);
}

void *anm2editor_detail_get_window(struct anm2editor_detail *detail) {
  if (!detail) {
    return NULL;
  }
  return detail->listview;
}

void anm2editor_detail_set_position(struct anm2editor_detail *detail, int x, int y, int width, int height) {
  if (!detail || !detail->listview) {
    return;
  }
  MoveWindow(detail->listview, x, y, width, height, TRUE);
}

void anm2editor_detail_clear(struct anm2editor_detail *detail) {
  if (!detail) {
    return;
  }
  cancel_edit_internal(detail);
  SendMessageW(detail->listview, LVM_DELETEALLITEMS, 0, 0);
  rows_clear(detail);
}

bool anm2editor_detail_add_row(struct anm2editor_detail *detail,
                               char const *property,
                               char const *value,
                               struct anm2editor_detail_row const *row,
                               struct ov_error *err) {
  if (!detail || !row) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_invalid_argument);
    return false;
  }

  size_t meta_idx = 0;
  if (!rows_add(detail, row, &meta_idx, err)) {
    OV_ERROR_ADD_TRACE(err);
    return false;
  }

  int const idx = (int)SendMessageW(detail->listview, LVM_GETITEMCOUNT, 0, 0);
  wchar_t prop_buf[256];
  wchar_t val_buf[512];
  ov_snprintf_char2wchar(prop_buf, sizeof(prop_buf) / sizeof(prop_buf[0]), "%1$hs", "%1$hs", property ? property : "");
  ov_snprintf_char2wchar(val_buf, sizeof(val_buf) / sizeof(val_buf[0]), "%1$hs", "%1$hs", value ? value : "");

  SendMessageW(detail->listview,
               LVM_INSERTITEMW,
               0,
               (LPARAM) & (LVITEMW){
                              .mask = LVIF_TEXT | LVIF_PARAM,
                              .iItem = idx,
                              .iSubItem = 0,
                              .pszText = prop_buf,
                              .lParam = (LPARAM)meta_idx,
                          });
  SendMessageW(detail->listview,
               LVM_SETITEMTEXTW,
               (WPARAM)idx,
               (LPARAM) & (LVITEMW){
                              .iSubItem = 1,
                              .pszText = val_buf,
                          });
  return true;
}

void anm2editor_detail_update_row(struct anm2editor_detail *detail,
                                  size_t row_index,
                                  char const *property,
                                  char const *value) {
  if (!detail || !detail->listview) {
    return;
  }

  wchar_t prop_buf[256];
  wchar_t val_buf[512];
  ov_snprintf_char2wchar(prop_buf, sizeof(prop_buf) / sizeof(prop_buf[0]), "%1$hs", "%1$hs", property ? property : "");
  ov_snprintf_char2wchar(val_buf, sizeof(val_buf) / sizeof(val_buf[0]), "%1$hs", "%1$hs", value ? value : "");

  LVITEMW lvi = {
      .iItem = (int)row_index,
      .iSubItem = 0,
      .pszText = prop_buf,
  };
  SendMessageW(detail->listview, LVM_SETITEMTEXTW, (WPARAM)row_index, (LPARAM)&lvi);
  lvi.iSubItem = 1;
  lvi.pszText = val_buf;
  SendMessageW(detail->listview, LVM_SETITEMTEXTW, (WPARAM)row_index, (LPARAM)&lvi);
}

bool anm2editor_detail_insert_row(struct anm2editor_detail *detail,
                                  size_t row_index,
                                  char const *property,
                                  char const *value,
                                  struct anm2editor_detail_row const *row,
                                  struct ov_error *err) {
  if (!detail || !row) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_invalid_argument);
    return false;
  }

  // Insert metadata at the corresponding position
  if (!rows_insert_at(detail, row_index, row, err)) {
    OV_ERROR_ADD_TRACE(err);
    return false;
  }

  wchar_t prop_buf[256];
  wchar_t val_buf[512];
  ov_snprintf_char2wchar(prop_buf, sizeof(prop_buf) / sizeof(prop_buf[0]), "%1$hs", "%1$hs", property ? property : "");
  ov_snprintf_char2wchar(val_buf, sizeof(val_buf) / sizeof(val_buf[0]), "%1$hs", "%1$hs", value ? value : "");

  SendMessageW(detail->listview,
               LVM_INSERTITEMW,
               0,
               (LPARAM) & (LVITEMW){
                              .mask = LVIF_TEXT | LVIF_PARAM,
                              .iItem = (int)row_index,
                              .iSubItem = 0,
                              .pszText = prop_buf,
                              .lParam = (LPARAM)row_index,
                          });
  SendMessageW(detail->listview,
               LVM_SETITEMTEXTW,
               (WPARAM)row_index,
               (LPARAM) & (LVITEMW){
                              .iSubItem = 1,
                              .pszText = val_buf,
                          });

  // Update lParam for all rows after the inserted one
  listview_update_row_lparam(detail->listview, (int)row_index + 1);
  return true;
}

void anm2editor_detail_remove_row(struct anm2editor_detail *detail, size_t row_index) {
  if (!detail || !detail->listview) {
    return;
  }

  // Remove the ListView item
  SendMessageW(detail->listview, LVM_DELETEITEM, (WPARAM)row_index, 0);

  // Remove the metadata
  rows_remove_at(detail, row_index);

  // Update lParam for all rows after the removed one
  listview_update_row_lparam(detail->listview, (int)row_index);
}

struct anm2editor_detail_row const *anm2editor_detail_get_row(struct anm2editor_detail const *detail,
                                                              size_t row_index) {
  if (!detail) {
    return NULL;
  }
  return rows_get(detail, row_index);
}

size_t anm2editor_detail_row_count(struct anm2editor_detail const *detail) {
  if (!detail || !detail->rows) {
    return 0;
  }
  return OV_ARRAY_LENGTH(detail->rows);
}

size_t anm2editor_detail_find_row_by_type(struct anm2editor_detail const *detail,
                                          enum anm2editor_detail_row_type type) {
  if (!detail) {
    return SIZE_MAX;
  }
  return rows_find_by_type(detail, type);
}

bool anm2editor_detail_row_type_is_editable(enum anm2editor_detail_row_type type) {
  return type != anm2editor_detail_row_type_placeholder;
}

bool anm2editor_detail_row_type_is_deletable_param(enum anm2editor_detail_row_type type) {
  return type == anm2editor_detail_row_type_animation_param;
}

void anm2editor_detail_start_edit(struct anm2editor_detail *detail, size_t row_index, int column) {
  if (!detail || !detail->listview || column < 0 || column > 1) {
    return;
  }

  // Get row metadata
  struct anm2editor_detail_row const *row_info = rows_get(detail, row_index);
  if (!row_info) {
    return;
  }

  // Check if this row type is editable and determine column restrictions
  switch (row_info->type) {
  case anm2editor_detail_row_type_label:
  case anm2editor_detail_row_type_psd_path:
  case anm2editor_detail_row_type_exclusive_support_default:
  case anm2editor_detail_row_type_information:
  case anm2editor_detail_row_type_default_character_id:
    // These rows can only edit the Value column (column 1)
    if (column != 1) {
      return;
    }
    break;
  case anm2editor_detail_row_type_placeholder:
    // Placeholder row cannot be edited inline (use start_edit_new instead)
    return;
  case anm2editor_detail_row_type_multisel_item:
  case anm2editor_detail_row_type_animation_param:
  case anm2editor_detail_row_type_value_item:
    // These are editable
    break;
  }

  // Cancel any existing edit
  cancel_edit_internal(detail);

  // Get the rectangle for the specified column
  RECT rc = {0};
  if (column == 0) {
    // For column 0 (Name), get the label rect
    rc.left = LVIR_LABEL;
    if (!SendMessageW(detail->listview, LVM_GETITEMRECT, (WPARAM)row_index, (LPARAM)&rc)) {
      return;
    }
  } else {
    // For column 1 (Value), get the subitem rect
    rc.top = 1;
    rc.left = LVIR_BOUNDS;
    if (!SendMessageW(detail->listview, LVM_GETSUBITEMRECT, (WPARAM)row_index, (LPARAM)&rc)) {
      return;
    }
  }

  // Get current text from the specified column
  wchar_t text[512] = {0};
  SendMessageW(detail->listview,
               LVM_GETITEMTEXTW,
               (WPARAM)row_index,
               (LPARAM) & (LVITEMW){
                              .mask = LVIF_TEXT,
                              .iItem = (int)row_index,
                              .iSubItem = column,
                              .pszText = text,
                              .cchTextMax = sizeof(text) / sizeof(text[0]),
                          });

  // Create edit control
  detail->edit_control = CreateWindowExW(0,
                                         L"EDIT",
                                         text,
                                         WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL,
                                         rc.left,
                                         rc.top,
                                         rc.right - rc.left,
                                         rc.bottom - rc.top,
                                         detail->listview,
                                         NULL,
                                         GetModuleHandleW(NULL),
                                         NULL);

  if (!detail->edit_control) {
    return;
  }

  detail->edit_row = (int)row_index;
  detail->edit_column = column;
  detail->edit_row_index = row_index;
  detail->edit_adding_new = false;

  // Store detail pointer in edit control
  SetPropW(detail->edit_control, L"anm2editor_detail", detail);

  // Subclass the edit control
  detail->edit_oldproc = (WNDPROC)(SetWindowLongPtrW(detail->edit_control, GWLP_WNDPROC, (LONG_PTR)edit_subclass_proc));

  // Set font to match ListView
  HFONT hFont = (HFONT)(SendMessageW(detail->listview, WM_GETFONT, 0, 0));
  if (hFont) {
    SendMessageW(detail->edit_control, WM_SETFONT, (WPARAM)hFont, TRUE);
  }

  // Select all text and focus
  SendMessageW(detail->edit_control, EM_SETSEL, 0, -1);
  SetFocus(detail->edit_control);
}

void anm2editor_detail_start_edit_new(struct anm2editor_detail *detail) {
  if (!detail || !detail->listview) {
    return;
  }

  // Cancel any existing edit
  cancel_edit_internal(detail);

  // Find the placeholder row (last row in ListView)
  int const placeholder_row = (int)SendMessageW(detail->listview, LVM_GETITEMCOUNT, 0, 0) - 1;
  if (placeholder_row < 0) {
    return;
  }

  // Verify it's actually a placeholder row
  struct anm2editor_detail_row const *row_info = rows_get(detail, (size_t)placeholder_row);
  if (!row_info || row_info->type != anm2editor_detail_row_type_placeholder) {
    return;
  }

  // Get the rectangle for column 0 (Name/Key column)
  RECT rc = {0};
  rc.left = LVIR_LABEL;
  if (!SendMessageW(detail->listview, LVM_GETITEMRECT, (WPARAM)placeholder_row, (LPARAM)&rc)) {
    return;
  }

  // Create edit control with empty text
  detail->edit_control = CreateWindowExW(0,
                                         L"EDIT",
                                         L"",
                                         WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL,
                                         rc.left,
                                         rc.top,
                                         rc.right - rc.left,
                                         rc.bottom - rc.top,
                                         detail->listview,
                                         NULL,
                                         GetModuleHandleW(NULL),
                                         NULL);

  if (!detail->edit_control) {
    return;
  }

  detail->edit_row = placeholder_row;
  detail->edit_column = 0;
  detail->edit_row_index = (size_t)placeholder_row;
  detail->edit_adding_new = true;

  // Store detail pointer in edit control
  SetPropW(detail->edit_control, L"anm2editor_detail", detail);

  // Subclass the edit control
  detail->edit_oldproc = (WNDPROC)(SetWindowLongPtrW(detail->edit_control, GWLP_WNDPROC, (LONG_PTR)edit_subclass_proc));

  // Set font to match ListView
  HFONT hFont = (HFONT)(SendMessageW(detail->listview, WM_GETFONT, 0, 0));
  if (hFont) {
    SendMessageW(detail->edit_control, WM_SETFONT, (WPARAM)hFont, TRUE);
  }

  // Focus the edit control
  SetFocus(detail->edit_control);
}

void anm2editor_detail_cancel_edit(struct anm2editor_detail *detail) { cancel_edit_internal(detail); }

bool anm2editor_detail_is_editing(struct anm2editor_detail const *detail) {
  if (!detail) {
    return false;
  }
  return detail->edit_control != NULL;
}

intptr_t anm2editor_detail_handle_notify(struct anm2editor_detail *detail, void *nmhdr_ptr) {
  if (!detail || !nmhdr_ptr) {
    return 0;
  }

  NMHDR const *nmhdr = (NMHDR const *)nmhdr_ptr;

  switch (nmhdr->code) {
  case NM_DBLCLK: {
    NMITEMACTIVATE const *nmia = (NMITEMACTIVATE const *)nmhdr_ptr;

    if (nmia->iItem >= 0) {
      // Get lParam (row index) to check row type
      LVITEMW lvi = {
          .mask = LVIF_PARAM,
          .iItem = nmia->iItem,
      };
      SendMessageW(detail->listview, LVM_GETITEMW, 0, (LPARAM)&lvi);

      struct anm2editor_detail_row const *row_info = anm2editor_detail_get_row(detail, (size_t)lvi.lParam);
      if (row_info) {
        if (row_info->type == anm2editor_detail_row_type_placeholder) {
          // Placeholder row - check if adding new parameter is allowed
          if (can_add_new_param(detail)) {
            anm2editor_detail_start_edit_new(detail);
          }
        } else if (anm2editor_detail_row_type_is_editable(row_info->type)) {
          // Editable row - start inline edit
          anm2editor_detail_start_edit(detail, (size_t)nmia->iItem, nmia->iSubItem);
        }
      }
    } else {
      // Clicked on empty space - check if adding new parameter is allowed
      if (can_add_new_param(detail)) {
        anm2editor_detail_start_edit_new(detail);
      }
    }
    return 0;
  }

  case LVN_KEYDOWN: {
    NMLVKEYDOWN const *nmkd = (NMLVKEYDOWN const *)nmhdr_ptr;
    if (nmkd->wVKey == VK_DELETE) {
      // Get selected row
      int const sel_item = (int)SendMessageW(detail->listview, LVM_GETNEXTITEM, (WPARAM)-1, LVNI_SELECTED);
      if (sel_item >= 0) {
        LVITEMW lvi = {
            .mask = LVIF_PARAM,
            .iItem = sel_item,
        };
        SendMessageW(detail->listview, LVM_GETITEMW, 0, (LPARAM)&lvi);

        delete_param_row(detail, (size_t)lvi.lParam);
      }
    }
    return 0;
  }

  case LVN_ITEMCHANGED: {
    NMLISTVIEW const *nmlv = (NMLISTVIEW const *)nmhdr_ptr;
    if ((nmlv->uChanged & LVIF_STATE) && ((nmlv->uNewState ^ nmlv->uOldState) & LVIS_SELECTED)) {
      if (detail->callbacks.on_selection_changed) {
        detail->callbacks.on_selection_changed(detail->callbacks.userdata);
      }
    }
    return 0;
  }

  case NM_RCLICK: {
    NMITEMACTIVATE const *nmia = (NMITEMACTIVATE const *)nmhdr_ptr;
    if (nmia->iItem >= 0) {
      // Select the clicked row first
      SendMessageW(detail->listview,
                   LVM_SETITEMSTATE,
                   (WPARAM)nmia->iItem,
                   (LPARAM) & (LVITEMW){
                                  .stateMask = LVIS_SELECTED | LVIS_FOCUSED,
                                  .state = LVIS_SELECTED | LVIS_FOCUSED,
                              });

      LVITEMW lvi = {
          .mask = LVIF_PARAM,
          .iItem = nmia->iItem,
      };
      SendMessageW(detail->listview, LVM_GETITEMW, 0, (LPARAM)&lvi);

      struct anm2editor_detail_row const *row_info = anm2editor_detail_get_row(detail, (size_t)lvi.lParam);
      if (row_info && anm2editor_detail_row_type_is_deletable_param(row_info->type)) {
        // Show context menu
        HMENU hMenu = CreatePopupMenu();
        if (hMenu) {
          wchar_t delete_text[64];
          ov_snprintf_char2wchar(delete_text,
                                 sizeof(delete_text) / sizeof(delete_text[0]),
                                 "%1$hs",
                                 "%1$hs",
                                 pgettext("anm2editor", "Delete"));
          AppendMenuW(hMenu, MF_STRING, 1, delete_text);

          POINT pt;
          GetCursorPos(&pt);
          int const cmd = TrackPopupMenu(hMenu, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, 0, detail->parent, NULL);
          DestroyMenu(hMenu);

          if (cmd == 1) {
            delete_param_row(detail, (size_t)lvi.lParam);
          }
        }
      }
    }
    return TRUE; // Prevent further processing
  }

  default:
    return 0;
  }
}
