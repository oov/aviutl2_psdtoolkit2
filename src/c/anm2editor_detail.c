#include "anm2editor_detail.h"

#include "anm2_edit.h"
#include "anm2_script_mapper.h"
#include "i18n.h"
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

  // Selection restoration after refresh (e.g., undo/redo)
  int saved_selection; // -1 if no saved selection
};

/**
 * @brief Report an error through the on_error callback or log it
 */
static void report_error(struct anm2editor_detail *detail, struct ov_error *err) {
  if (detail->callbacks.on_error) {
    detail->callbacks.on_error(detail->callbacks.userdata, err);
  } else {
    ptk_logf_error(err, "%1$hs", "%1$hs", gettext("Operation failed."));
    OV_ERROR_DESTROY(err);
  }
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

/**
 * @brief Delete a parameter row by its row index
 */
static void delete_param_row(struct anm2editor_detail *detail, size_t row_index) {
  if (!detail || !detail->edit) {
    return;
  }
  struct anm2editor_detail_row const *row_info = rows_get(detail, row_index);
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

static size_t rows_find_by_type(struct anm2editor_detail const *detail, enum anm2editor_detail_row_type type) {
  size_t const len = OV_ARRAY_LENGTH(detail->rows);
  for (size_t i = 0; i < len; i++) {
    if (detail->rows[i].type == type) {
      return i;
    }
  }
  return SIZE_MAX;
}

/**
 * @brief Find a row by param_id
 * @return Row index, or SIZE_MAX if not found
 */
static size_t find_row_by_param_id(struct anm2editor_detail const *detail, uint32_t param_id) {
  size_t const len = OV_ARRAY_LENGTH(detail->rows);
  for (size_t i = 0; i < len; i++) {
    if (detail->rows[i].type == anm2editor_detail_row_type_animation_param && detail->rows[i].param_id == param_id) {
      return i;
    }
  }
  return SIZE_MAX;
}

/**
 * @brief Find a row by item_id (for multisel mode)
 * @return Row index, or SIZE_MAX if not found
 */
static size_t find_row_by_item_id(struct anm2editor_detail const *detail, uint32_t item_id) {
  size_t const len = OV_ARRAY_LENGTH(detail->rows);
  for (size_t i = 0; i < len; i++) {
    if (detail->rows[i].type == anm2editor_detail_row_type_multisel_item && detail->rows[i].item_id == item_id) {
      return i;
    }
  }
  return SIZE_MAX;
}

/**
 * @brief Get the insertion position for a multisel item based on treeview order
 *
 * When adding an item to multiselection, it should appear in the detail view
 * at a position that matches the treeview order. This function calculates
 * that position by counting how many selected items appear before this one
 * in the treeview order.
 *
 * @return Row index where the new multisel_item should be inserted
 */
static size_t get_multisel_insert_position(struct anm2editor_detail *detail, uint32_t item_id) {
  if (!detail || !detail->edit) {
    return 0;
  }

  size_t position = 0;

  // Iterate through all items in treeview order
  size_t const sel_count = ptk_anm2_edit_selector_count(detail->edit);
  for (size_t sel_idx = 0; sel_idx < sel_count; sel_idx++) {
    uint32_t const sel_id = ptk_anm2_edit_selector_get_id(detail->edit, sel_idx);
    size_t const item_count = ptk_anm2_edit_item_count(detail->edit, sel_id);

    for (size_t item_idx = 0; item_idx < item_count; item_idx++) {
      uint32_t const id = ptk_anm2_edit_item_get_id(detail->edit, sel_idx, item_idx);

      if (id == item_id) {
        // Found the target item
        return position;
      }

      // If this item is selected and is a value item (not animation), count it
      if (ptk_anm2_edit_is_item_selected(detail->edit, id) && !ptk_anm2_edit_item_is_animation(detail->edit, id)) {
        position++;
      }
    }
  }

  // Not found - return end position
  return position;
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
      .saved_selection = -1,
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

  // Set extended styles (full row select, grid lines, double buffer to prevent flickering)
  SendMessageW(
      detail->listview, LVM_SETEXTENDEDLISTVIEWSTYLE, 0, LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES | LVS_EX_DOUBLEBUFFER);

  // Store pointer to self for subclass callbacks
  SetPropW(detail->listview, L"anm2editor_detail", detail);

  // Add columns
  {
    wchar_t prop_header[64], value_header[64];
    ov_snprintf_wchar(prop_header,
                      sizeof(prop_header) / sizeof(prop_header[0]),
                      L"%1$hs",
                      L"%1$hs",
                      pgettext("anm2editor", "Property"));
    ov_snprintf_wchar(value_header,
                      sizeof(value_header) / sizeof(value_header[0]),
                      L"%1$hs",
                      L"%1$hs",
                      pgettext("anm2editor", "Value"));

    LVCOLUMNW lvc = {
        .mask = LVCF_FMT | LVCF_WIDTH | LVCF_TEXT,
        .fmt = LVCFMT_LEFT,
        .cx = 120,
        .pszText = prop_header,
    };
    SendMessageW(detail->listview, LVM_INSERTCOLUMNW, 0, (LPARAM)&lvc);

    lvc.cx = 200;
    lvc.pszText = value_header;
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

// Internal helper to populate detail for multi-selection mode
static bool refresh_multisel(struct anm2editor_detail *detail, struct ov_error *const err) {
  bool success = false;

  // Iterate through tree in order to collect selected value items
  size_t const sel_count = ptk_anm2_edit_selector_count(detail->edit);
  for (size_t sel_idx = 0; sel_idx < sel_count; sel_idx++) {
    uint32_t const sel_id = ptk_anm2_edit_selector_get_id(detail->edit, sel_idx);
    size_t const item_count = ptk_anm2_edit_item_count(detail->edit, sel_id);
    for (size_t item_idx = 0; item_idx < item_count; item_idx++) {
      uint32_t const id = ptk_anm2_edit_item_get_id(detail->edit, sel_idx, item_idx);
      if (!ptk_anm2_edit_is_item_selected(detail->edit, id)) {
        continue;
      }
      if (ptk_anm2_edit_item_is_animation(detail->edit, id)) {
        continue;
      }
      char const *name = ptk_anm2_edit_item_get_name(detail->edit, id);
      char const *value = ptk_anm2_edit_item_get_value(detail->edit, id);
      if (!anm2editor_detail_add_row(detail,
                                     name,
                                     value,
                                     &(struct anm2editor_detail_row){
                                         .type = anm2editor_detail_row_type_multisel_item,
                                         .item_id = id,
                                     },
                                     err)) {
        OV_ERROR_ADD_TRACE(err);
        goto cleanup;
      }
    }
  }

  success = true;

cleanup:
  return success;
}

// Internal helper to populate detail for document properties
static bool refresh_document(struct anm2editor_detail *detail, struct ov_error *const err) {
  bool success = false;

  if (!anm2editor_detail_add_row(detail,
                                 pgettext("anm2editor", "Label"),
                                 ptk_anm2_edit_get_label(detail->edit),
                                 &(struct anm2editor_detail_row){.type = anm2editor_detail_row_type_label},
                                 err)) {
    OV_ERROR_ADD_TRACE(err);
    goto cleanup;
  }

  if (!anm2editor_detail_add_row(detail,
                                 pgettext("anm2editor", "Information"),
                                 ptk_anm2_edit_get_information(detail->edit),
                                 &(struct anm2editor_detail_row){.type = anm2editor_detail_row_type_information},
                                 err)) {
    OV_ERROR_ADD_TRACE(err);
    goto cleanup;
  }

  if (!anm2editor_detail_add_row(detail,
                                 pgettext("anm2editor", "PSD File Path"),
                                 ptk_anm2_edit_get_psd_path(detail->edit),
                                 &(struct anm2editor_detail_row){.type = anm2editor_detail_row_type_psd_path},
                                 err)) {
    OV_ERROR_ADD_TRACE(err);
    goto cleanup;
  }

  if (!anm2editor_detail_add_row(
          detail,
          pgettext("anm2editor", "Exclusive Support Default"),
          ptk_anm2_edit_get_exclusive_support_default(detail->edit) ? "1" : "",
          &(struct anm2editor_detail_row){.type = anm2editor_detail_row_type_exclusive_support_default},
          err)) {
    OV_ERROR_ADD_TRACE(err);
    goto cleanup;
  }

  if (!anm2editor_detail_add_row(
          detail,
          pgettext("anm2editor", "Default Character ID"),
          ptk_anm2_edit_get_default_character_id(detail->edit),
          &(struct anm2editor_detail_row){.type = anm2editor_detail_row_type_default_character_id},
          err)) {
    OV_ERROR_ADD_TRACE(err);
    goto cleanup;
  }

  success = true;

cleanup:
  return success;
}

// Internal helper to populate detail for single item selection
static bool refresh_item(struct anm2editor_detail *detail, uint32_t item_id, struct ov_error *const err) {
  if (item_id == 0) {
    return true;
  }

  bool success = false;
  uint32_t *param_ids = NULL;
  char *translated_key_utf8 = NULL;
  char *translated_value_utf8 = NULL;

  bool const is_animation = ptk_anm2_edit_item_is_animation(detail->edit, item_id);
  if (is_animation) {
    // Get effect_name for translation lookup
    struct ptk_anm2_script_mapper_result effect_name = {NULL, 0};
    char const *script_name = ptk_anm2_item_get_script_name(ptk_anm2_edit_get_doc(detail->edit), item_id);
    if (script_name) {
      struct ptk_anm2_script_mapper const *mapper = ptk_anm2_edit_get_script_mapper(detail->edit);
      if (mapper) {
        effect_name = ptk_anm2_script_mapper_get_effect_name(mapper, script_name);
      }
    }

    param_ids = ptk_anm2_get_param_ids(ptk_anm2_edit_get_doc(detail->edit), item_id, err);
    if (!param_ids) {
      OV_ERROR_ADD_TRACE(err);
      goto cleanup;
    }
    size_t const num_params = OV_ARRAY_LENGTH(param_ids);
    for (size_t i = 0; i < num_params; i++) {
      uint32_t const pid = param_ids[i];
      char const *key = ptk_anm2_edit_param_get_key(detail->edit, pid);
      char const *value = ptk_anm2_edit_param_get_value(detail->edit, pid);

      char const *display_key = key;
      char const *display_value = value;
      if (effect_name.ptr && effect_name.size > 0) {
        // Try to get translated key name
        if (key) {
          wchar_t const *translated =
              ptk_i18n_get_translated_text_n(effect_name.ptr, effect_name.size, key, strlen(key));
          if (translated) {
            size_t const src_len = wcslen(translated);
            size_t const utf8_len = ov_wchar_to_utf8_len(translated, src_len);
            if (utf8_len > 0) {
              if (!OV_ARRAY_GROW(&translated_key_utf8, utf8_len + 1)) {
                OV_ERROR_SET_GENERIC(err, ov_error_generic_out_of_memory);
                goto cleanup;
              }
              ov_wchar_to_utf8(translated, src_len, translated_key_utf8, utf8_len + 1, NULL);
              OV_ARRAY_SET_LENGTH(translated_key_utf8, utf8_len + 1);
              display_key = translated_key_utf8;
            }
          }
        }
        // Try to get translated value
        if (value) {
          wchar_t const *translated =
              ptk_i18n_get_translated_text_n(effect_name.ptr, effect_name.size, value, strlen(value));
          if (translated) {
            size_t const src_len = wcslen(translated);
            size_t const utf8_len = ov_wchar_to_utf8_len(translated, src_len);
            if (utf8_len > 0) {
              if (!OV_ARRAY_GROW(&translated_value_utf8, utf8_len + 1)) {
                OV_ERROR_SET_GENERIC(err, ov_error_generic_out_of_memory);
                goto cleanup;
              }
              ov_wchar_to_utf8(translated, src_len, translated_value_utf8, utf8_len + 1, NULL);
              OV_ARRAY_SET_LENGTH(translated_value_utf8, utf8_len + 1);
              display_value = translated_value_utf8;
            }
          }
        }
      }

      if (!anm2editor_detail_add_row(detail,
                                     display_key,
                                     display_value,
                                     &(struct anm2editor_detail_row){
                                         .type = anm2editor_detail_row_type_animation_param,
                                         .param_id = pid,
                                     },
                                     err)) {
        OV_ERROR_ADD_TRACE(err);
        goto cleanup;
      }
    }
    if (!anm2editor_detail_add_row(detail,
                                   pgettext("anm2editor", "(Add new...)"),
                                   "",
                                   &(struct anm2editor_detail_row){.type = anm2editor_detail_row_type_placeholder},
                                   err)) {
      OV_ERROR_ADD_TRACE(err);
      goto cleanup;
    }
  } else {
    char const *name = ptk_anm2_edit_item_get_name(detail->edit, item_id);
    char const *value = ptk_anm2_edit_item_get_value(detail->edit, item_id);
    if (!anm2editor_detail_add_row(
            detail, name, value, &(struct anm2editor_detail_row){.type = anm2editor_detail_row_type_value_item}, err)) {
      OV_ERROR_ADD_TRACE(err);
      goto cleanup;
    }
  }

  success = true;

cleanup:
  if (translated_value_utf8) {
    OV_ARRAY_DESTROY(&translated_value_utf8);
  }
  if (translated_key_utf8) {
    OV_ARRAY_DESTROY(&translated_key_utf8);
  }
  if (param_ids) {
    OV_ARRAY_DESTROY(&param_ids);
  }
  return success;
}

void anm2editor_detail_refresh(struct anm2editor_detail *detail) {
  if (!detail || !detail->edit) {
    return;
  }

  struct ov_error err = {0};
  bool success = false;

  anm2editor_detail_clear(detail);

  // Check selection count
  size_t multisel_count = 0;
  ptk_anm2_edit_get_selected_item_ids(detail->edit, &multisel_count);

  if (multisel_count > 1) {
    if (!refresh_multisel(detail, &err)) {
      OV_ERROR_ADD_TRACE(&err);
      goto cleanup;
    }
  } else {
    // Get focus state
    struct ptk_anm2_edit_state state = {0};
    ptk_anm2_edit_get_state(detail->edit, &state);

    if (state.focus_type == ptk_anm2_edit_focus_item) {
      if (!refresh_item(detail, state.focus_id, &err)) {
        OV_ERROR_ADD_TRACE(&err);
        goto cleanup;
      }
    } else {
      // No selection or selector selection - show document properties
      if (!refresh_document(detail, &err)) {
        OV_ERROR_ADD_TRACE(&err);
        goto cleanup;
      }
    }
  }

  success = true;

cleanup:
  if (!success) {
    ptk_logf_error(&err, "%1$hs", "%1$hs", gettext("failed to update detail list."));
    OV_ERROR_DESTROY(&err);
  }

  // Restore saved selection if any
  if (detail->saved_selection >= 0 && detail->listview) {
    int const item_count = (int)SendMessageW(detail->listview, LVM_GETITEMCOUNT, 0, 0);
    if (detail->saved_selection < item_count) {
      SendMessageW(detail->listview,
                   LVM_SETITEMSTATE,
                   (WPARAM)detail->saved_selection,
                   (LPARAM) & (LVITEMW){
                                  .stateMask = LVIS_SELECTED | LVIS_FOCUSED,
                                  .state = LVIS_SELECTED | LVIS_FOCUSED,
                              });
    }
    detail->saved_selection = -1;
  }
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
  ov_snprintf_wchar(prop_buf, sizeof(prop_buf) / sizeof(prop_buf[0]), L"%1$hs", L"%1$hs", property ? property : "");
  ov_snprintf_wchar(val_buf, sizeof(val_buf) / sizeof(val_buf[0]), L"%1$hs", L"%1$hs", value ? value : "");

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
  ov_snprintf_wchar(prop_buf, sizeof(prop_buf) / sizeof(prop_buf[0]), L"%1$hs", L"%1$hs", property ? property : "");
  ov_snprintf_wchar(val_buf, sizeof(val_buf) / sizeof(val_buf[0]), L"%1$hs", L"%1$hs", value ? value : "");

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
  ov_snprintf_wchar(prop_buf, sizeof(prop_buf) / sizeof(prop_buf[0]), L"%1$hs", L"%1$hs", property ? property : "");
  ov_snprintf_wchar(val_buf, sizeof(val_buf) / sizeof(val_buf[0]), L"%1$hs", L"%1$hs", value ? value : "");

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

  // Get text for editing
  // For animation_param (column 0: key, column 1: value), use the original text from param_id
  // instead of the translated text displayed in ListView
  wchar_t text[512] = {0};
  if (row_info->type == anm2editor_detail_row_type_animation_param && detail->edit) {
    char const *original = NULL;
    if (column == 0) {
      original = ptk_anm2_edit_param_get_key(detail->edit, row_info->param_id);
    } else {
      original = ptk_anm2_edit_param_get_value(detail->edit, row_info->param_id);
    }
    if (original) {
      ov_utf8_to_wchar(original, strlen(original), text, sizeof(text) / sizeof(text[0]), NULL);
    }
  } else {
    // Use displayed text from ListView
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
  }

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

      struct anm2editor_detail_row const *row_info = rows_get(detail, (size_t)lvi.lParam);
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

      struct anm2editor_detail_row const *row_info = rows_get(detail, (size_t)lvi.lParam);
      if (row_info && anm2editor_detail_row_type_is_deletable_param(row_info->type)) {
        // Show context menu
        HMENU hMenu = CreatePopupMenu();
        if (hMenu) {
          wchar_t delete_text[64];
          ov_snprintf_wchar(delete_text,
                            sizeof(delete_text) / sizeof(delete_text[0]),
                            L"%1$hs",
                            L"%1$hs",
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

void anm2editor_detail_handle_view_event(struct anm2editor_detail *detail,
                                         struct ptk_anm2_edit_view_event const *event) {
  if (!detail || !event) {
    return;
  }

  switch (event->op) {
  case ptk_anm2_edit_view_before_undo_redo:
    // Save selection for restoration after refresh
    if (detail->listview) {
      detail->saved_selection = (int)SendMessageW(detail->listview, LVM_GETNEXTITEM, (WPARAM)-1, LVNI_SELECTED);
    }
    break;

  case ptk_anm2_edit_view_detail_refresh:
    anm2editor_detail_refresh(detail);
    break;

  case ptk_anm2_edit_view_detail_insert_param: {
    // For now, fall back to full refresh since insert position handling
    // with translation is complex. Can be optimized later if needed.
    anm2editor_detail_refresh(detail);
  } break;

  case ptk_anm2_edit_view_detail_remove_param: {
    // Find and remove the row by param_id
    size_t const row_idx = find_row_by_param_id(detail, event->id);
    if (row_idx != SIZE_MAX) {
      anm2editor_detail_remove_row(detail, row_idx);
    }
  } break;

  case ptk_anm2_edit_view_detail_update_param: {
    // Find the row by param_id
    size_t const row_idx = find_row_by_param_id(detail, event->id);
    if (row_idx == SIZE_MAX) {
      break;
    }

    // Get key and value
    char const *key = ptk_anm2_edit_param_get_key(detail->edit, event->id);
    char const *value = ptk_anm2_edit_param_get_value(detail->edit, event->id);

    // Get effect_name for translation
    struct ptk_anm2_script_mapper_result effect_name = {NULL, 0};
    uint32_t const item_id = event->parent_id;
    char const *script_name = ptk_anm2_item_get_script_name(ptk_anm2_edit_get_doc(detail->edit), item_id);
    if (script_name) {
      struct ptk_anm2_script_mapper const *mapper = ptk_anm2_edit_get_script_mapper(detail->edit);
      if (mapper) {
        effect_name = ptk_anm2_script_mapper_get_effect_name(mapper, script_name);
      }
    }

    char const *display_key = key;
    char const *display_value = value;
    char *translated_key_utf8 = NULL;
    char *translated_value_utf8 = NULL;

    if (effect_name.ptr && effect_name.size > 0) {
      // Try to get translated key name
      if (key) {
        wchar_t const *translated = ptk_i18n_get_translated_text_n(effect_name.ptr, effect_name.size, key, strlen(key));
        if (translated) {
          size_t const src_len = wcslen(translated);
          size_t const utf8_len = ov_wchar_to_utf8_len(translated, src_len);
          if (utf8_len > 0 && OV_ARRAY_GROW(&translated_key_utf8, utf8_len + 1)) {
            ov_wchar_to_utf8(translated, src_len, translated_key_utf8, utf8_len + 1, NULL);
            OV_ARRAY_SET_LENGTH(translated_key_utf8, utf8_len + 1);
            display_key = translated_key_utf8;
          }
        }
      }
      // Try to get translated value
      if (value) {
        wchar_t const *translated =
            ptk_i18n_get_translated_text_n(effect_name.ptr, effect_name.size, value, strlen(value));
        if (translated) {
          size_t const src_len = wcslen(translated);
          size_t const utf8_len = ov_wchar_to_utf8_len(translated, src_len);
          if (utf8_len > 0 && OV_ARRAY_GROW(&translated_value_utf8, utf8_len + 1)) {
            ov_wchar_to_utf8(translated, src_len, translated_value_utf8, utf8_len + 1, NULL);
            OV_ARRAY_SET_LENGTH(translated_value_utf8, utf8_len + 1);
            display_value = translated_value_utf8;
          }
        }
      }
    }

    anm2editor_detail_update_row(detail, row_idx, display_key, display_value);

    if (translated_value_utf8) {
      OV_ARRAY_DESTROY(&translated_value_utf8);
    }
    if (translated_key_utf8) {
      OV_ARRAY_DESTROY(&translated_key_utf8);
    }
  } break;

  case ptk_anm2_edit_view_detail_update_item: {
    // Find the value_item row and update its name/value
    // For single selection, there is only one value_item row (if any)
    size_t const row_idx = rows_find_by_type(detail, anm2editor_detail_row_type_value_item);
    if (row_idx == SIZE_MAX) {
      // Also check for multisel_item (used in multiselection mode)
      size_t const multisel_idx = find_row_by_item_id(detail, event->id);
      if (multisel_idx != SIZE_MAX) {
        // Update multisel_item row
        char const *name = ptk_anm2_edit_item_get_name(detail->edit, event->id);
        char const *value = ptk_anm2_edit_item_get_value(detail->edit, event->id);
        anm2editor_detail_update_row(detail, multisel_idx, name, value);
      }
      break;
    }

    // Update value_item row with new name/value
    char const *name = ptk_anm2_edit_item_get_name(detail->edit, event->id);
    char const *value = ptk_anm2_edit_item_get_value(detail->edit, event->id);
    anm2editor_detail_update_row(detail, row_idx, name, value);
  } break;

  case ptk_anm2_edit_view_detail_item_selected: {
    // Add a new row for the selected item in multiselection mode
    // Skip animation items as they are not shown in multiselection detail
    if (ptk_anm2_edit_item_is_animation(detail->edit, event->id)) {
      break;
    }

    // Calculate insert position based on treeview order
    size_t const insert_pos = get_multisel_insert_position(detail, event->id);

    // Get item name and value
    char const *name = ptk_anm2_edit_item_get_name(detail->edit, event->id);
    char const *value = ptk_anm2_edit_item_get_value(detail->edit, event->id);

    // Insert the row
    struct ov_error err = {0};
    if (!anm2editor_detail_insert_row(detail,
                                      insert_pos,
                                      name,
                                      value,
                                      &(struct anm2editor_detail_row){
                                          .type = anm2editor_detail_row_type_multisel_item,
                                          .item_id = event->id,
                                      },
                                      &err)) {
      OV_ERROR_REPORT(&err, NULL);
    }
  } break;

  case ptk_anm2_edit_view_detail_item_deselected: {
    // Remove the row for the deselected item
    size_t const row_idx = find_row_by_item_id(detail, event->id);
    if (row_idx != SIZE_MAX) {
      anm2editor_detail_remove_row(detail, row_idx);
    }
  } break;

  case ptk_anm2_edit_view_treeview_rebuild:
  case ptk_anm2_edit_view_treeview_insert_selector:
  case ptk_anm2_edit_view_treeview_remove_selector:
  case ptk_anm2_edit_view_treeview_update_selector:
  case ptk_anm2_edit_view_treeview_move_selector:
  case ptk_anm2_edit_view_treeview_insert_item:
  case ptk_anm2_edit_view_treeview_remove_item:
  case ptk_anm2_edit_view_treeview_update_item:
  case ptk_anm2_edit_view_treeview_move_item:
  case ptk_anm2_edit_view_treeview_select:
  case ptk_anm2_edit_view_treeview_set_focus:
  case ptk_anm2_edit_view_undo_redo_state_changed:
  case ptk_anm2_edit_view_modified_state_changed:
  case ptk_anm2_edit_view_save_state_changed:
    // Non-detail events - handled by parent
    break;
  }
}
