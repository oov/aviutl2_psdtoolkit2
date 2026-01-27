#include "anm2editor.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <commctrl.h>
#include <objidl.h>

#include <ovarray.h>
#include <ovmo.h>
#include <ovprintf.h>
#include <ovsort.h>
#include <ovutf.h>

#include <ovl/dialog.h>
#include <ovl/file.h>
#include <ovl/os.h>
#include <ovl/path.h>

#include <aviutl2_plugin2.h>

#include "alias.h"
#include "anm2.h"
#include "anm2_edit.h"
#include "anm2_script_picker.h"
#include "anm2editor_convert.h"
#include "anm2editor_detail.h"
#include "anm2editor_import.h"
#include "anm2editor_splitter.h"
#include "anm2editor_toolbar.h"
#include "anm2editor_treeview.h"
#include "anm_to_anm2.h"
#include "dialog.h"
#include "error.h"
#include "logf.h"
#include "win32.h"

// GUID for file dialogs (to remember last used folder)
// {1913A4B0-9040-43EE-BEDB-20CE479E4D2B}
static GUID const g_file_dialog_guid = {0x1913a4b0, 0x9040, 0x43ee, {0xbe, 0xdb, 0x20, 0xce, 0x47, 0x9e, 0x4d, 0x2c}};

// Resource IDs
#define IDC_TREEVIEW 1001
#define IDC_TOOLBAR 1002
#define IDC_DETAILLIST 1003

// Custom window messages
#define WM_ANM2EDITOR_UPDATE_TITLE (WM_APP + 100)

static wchar_t const anm2editor_window_class_name[] = L"PSDToolKitAnm2Editor";

// Build file filter string for file dialogs
// Uses | as separator, then replaces with \0
static wchar_t const *get_file_filter(void) {
  static wchar_t buf[256];
  if (!buf[0]) {
    ov_snprintf_char2wchar(buf,
                           sizeof(buf) / sizeof(buf[0]),
                           "%1$hs%2$hs",
                           "%1$hs (*.ptk.anm2)|*.ptk.anm2|%2$hs (*.*)|*.*|",
                           pgettext("anm2editor", "PSDToolKit anm2"),
                           pgettext("anm2editor", "All Files"));
    for (wchar_t *p = buf; *p; ++p) {
      if (*p == L'|') {
        *p = L'\0';
      }
    }
  }
  return buf;
}

struct ptk_anm2editor {
  HWND window;
  ATOM window_class;

  struct ptk_anm2_edit *edit_core;  // Edit core for selection and editing operations (owns doc)
  struct aviutl2_edit_handle *edit; // Edit handle for accessing AviUtl2 edit section
  wchar_t *file_path;               // ovarray, current file path (NULL for new document)

  // UI components
  struct anm2editor_toolbar *toolbar;
  struct anm2editor_detail *detail;
  struct anm2editor_treeview *treeview;
  struct anm2editor_splitter *splitter;
};

// Get the Script directory path (DLL/../Script/)
static bool get_script_dir(wchar_t **const dir, struct ov_error *const err) {
  static wchar_t const suffix[] = L"\\..\\Script\\";
  static size_t const suffix_len = (sizeof(suffix) / sizeof(wchar_t)) - 1;

  wchar_t *module_path = NULL;
  wchar_t *raw_path = NULL;
  void *hinstance = NULL;
  wchar_t const *last_slash = NULL;
  size_t dir_len = 0;
  size_t raw_path_len = 0;
  DWORD full_path_len = 0;
  bool success = false;

  if (!ovl_os_get_hinstance_from_fnptr((void *)get_script_dir, &hinstance, err)) {
    OV_ERROR_ADD_TRACE(err);
    goto cleanup;
  }
  if (!ovl_path_get_module_name(&module_path, hinstance, err)) {
    OV_ERROR_ADD_TRACE(err);
    goto cleanup;
  }

  // Find last path separator to get directory
  last_slash = ovl_path_find_last_path_sep(module_path);
  if (!last_slash) {
    OV_ERROR_SET(err, ov_error_type_generic, ov_error_generic_fail, "No directory separator found in module path");
    goto cleanup;
  }

  // Build raw path: dir/../Script/
  dir_len = (size_t)(last_slash - module_path);
  raw_path_len = dir_len + suffix_len;

  if (!OV_ARRAY_GROW(&raw_path, raw_path_len + 1)) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_out_of_memory);
    goto cleanup;
  }

  memcpy(raw_path, module_path, dir_len * sizeof(wchar_t));
  memcpy(raw_path + dir_len, suffix, (suffix_len + 1) * sizeof(wchar_t));
  OV_ARRAY_SET_LENGTH(raw_path, raw_path_len);

  // Canonicalize the path using GetFullPathNameW
  full_path_len = GetFullPathNameW(raw_path, 0, NULL, NULL);
  if (full_path_len == 0) {
    OV_ERROR_SET_HRESULT(err, HRESULT_FROM_WIN32(GetLastError()));
    goto cleanup;
  }

  if (!OV_ARRAY_GROW(dir, full_path_len)) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_out_of_memory);
    goto cleanup;
  }

  full_path_len = GetFullPathNameW(raw_path, full_path_len, *dir, NULL);
  if (full_path_len == 0) {
    OV_ERROR_SET_HRESULT(err, HRESULT_FROM_WIN32(GetLastError()));
    goto cleanup;
  }
  OV_ARRAY_SET_LENGTH(*dir, full_path_len);

  success = true;

cleanup:
  if (raw_path) {
    OV_ARRAY_DESTROY(&raw_path);
  }
  if (module_path) {
    OV_ARRAY_DESTROY(&module_path);
  }
  return success;
}

static wchar_t const *get_window_title(void) {
  static wchar_t buf[64];
  if (!buf[0]) {
    ov_snprintf_wchar(buf, sizeof(buf) / sizeof(buf[0]), L"%1$hs", L"%1$hs", gettext("PSDToolKit anm2 Editor"));
  }
  return buf;
}

static void show_error_dialog(struct ptk_anm2editor *editor, struct ov_error *const err) {
  wchar_t msg[256];
  ov_snprintf_wchar(
      msg, sizeof(msg) / sizeof(msg[0]), L"%1$hs", L"%1$hs", pgettext("anm2editor", "An error occurred."));
  ptk_error_dialog(editor->window, err, get_window_title(), msg, NULL, TD_ERROR_ICON, TDCBF_OK_BUTTON);
  OV_ERROR_DESTROY(err);
}

static void update_window_title(struct ptk_anm2editor *editor) {
  if (!editor || !editor->window) {
    return;
  }
  wchar_t title[MAX_PATH + 64];
  wchar_t const *filename = NULL;
  if (editor->file_path && editor->file_path[0] != L'\0') {
    filename = ovl_path_extract_file_name(editor->file_path);
  }
  bool const modified = ptk_anm2_edit_is_modified(editor->edit_core);
  ov_snprintf_wchar(title,
                    sizeof(title) / sizeof(title[0]),
                    L"%1$ls%2$hs%3$hs",
                    L"%1$ls%2$hs%3$hs - %4$ls",
                    filename ? filename : L"",
                    filename ? "" : pgettext("anm2editor", "Unsaved"),
                    modified ? "*" : "",
                    get_window_title());
  // The window is changed to WS_POPUP and registered as WS_CHILD in AviUtl ExEdit2, so this window title is not
  // visible. Although it is currently useless, the title is set just in case.
  SetWindowTextW(editor->window, title);
}

// Get the currently selected item's IDs
// Returns: 0 = nothing selected, 1 = selector selected, 2 = item selected
static int get_selected_ids(struct ptk_anm2editor *editor, uint32_t *selector_id, uint32_t *item_id) {
  struct ptk_anm2_edit_state state = {0};
  ptk_anm2_edit_get_state(editor->edit_core, &state);

  switch (state.focus_type) {
  case ptk_anm2_edit_focus_none:
    return 0;
  case ptk_anm2_edit_focus_selector:
    *selector_id = state.focus_id;
    return 1;
  case ptk_anm2_edit_focus_item: {
    size_t sel_idx = 0;
    size_t item_idx = 0;
    if (ptk_anm2_edit_find_item(editor->edit_core, state.focus_id, &sel_idx, &item_idx)) {
      *selector_id = ptk_anm2_edit_selector_get_id(editor->edit_core, sel_idx);
      *item_id = state.focus_id;
      return 2;
    }
    return 0;
  }
  }
  return 0;
}

// Get the selector ID for current selection
static uint32_t get_selected_selector_id(struct ptk_anm2editor *editor) {
  if (!editor || !editor->edit_core) {
    return 0;
  }

  uint32_t sel_id = 0;
  uint32_t item_id = 0;
  int const sel_type = get_selected_ids(editor, &sel_id, &item_id);

  // Return the selector ID if something is selected
  if (sel_type == 0) {
    return 0;
  }

  return sel_id;
}

/**
 * @brief Update toolbar button states based on current document state
 *
 * Updates Undo/Redo and Save/Save As button states.
 */
static void update_toolbar_state(struct ptk_anm2editor *editor) {
  if (!editor || !editor->toolbar || !editor->edit_core) {
    return;
  }
  anm2editor_toolbar_update_state(editor->toolbar,
                                  ptk_anm2_edit_can_undo(editor->edit_core),
                                  ptk_anm2_edit_can_redo(editor->edit_core),
                                  ptk_anm2_edit_can_save(editor->edit_core));
}

// Helper function for handling param_insert operation in differential update
// This function exists to properly use the goto cleanup pattern as required by ovbase coding standards.
// Update detail list based on document changes (differential updates)
// This function checks if the change affects the currently displayed content and updates accordingly.
/**
 * @brief View callback handler for edit_core
 *
 * Receives differential update events from edit_core and updates UI accordingly.
 * This handles selection state sync and detail refresh.
 *
 * @param userdata Pointer to ptk_anm2editor
 * @param event View event from edit_core
 */
static void on_edit_view_change(void *userdata, struct ptk_anm2_edit_view_event const *event) {
  struct ptk_anm2editor *editor = (struct ptk_anm2editor *)userdata;
  if (!editor || !event) {
    return;
  }

  // Forward treeview events to treeview component
  if (editor->treeview) {
    anm2editor_treeview_handle_view_event(editor->treeview, event);
  }

  // Forward detail events to detail component
  if (editor->detail) {
    anm2editor_detail_handle_view_event(editor->detail, event);
  }

  // Handle editor-specific events
  switch (event->op) {
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
    // Handled by treeview component
    break;

  case ptk_anm2_edit_view_detail_refresh:
  case ptk_anm2_edit_view_detail_insert_param:
  case ptk_anm2_edit_view_detail_remove_param:
  case ptk_anm2_edit_view_detail_update_param:
  case ptk_anm2_edit_view_detail_update_item:
  case ptk_anm2_edit_view_detail_item_selected:
  case ptk_anm2_edit_view_detail_item_deselected:
  case ptk_anm2_edit_view_before_undo_redo:
    // Handled by detail component
    break;

  case ptk_anm2_edit_view_undo_redo_state_changed:
  case ptk_anm2_edit_view_save_state_changed:
    update_toolbar_state(editor);
    break;

  case ptk_anm2_edit_view_modified_state_changed:
    update_toolbar_state(editor);
    update_window_title(editor);
    break;
  }
}

/**
 * @brief Clear the current document and create a new empty one
 *
 * Destroys and recreates the doc, clears file path, and resets modified flag.
 * Does not update UI - call refresh_all_views afterwards.
 *
 * @param editor Editor instance
 * @param err Error information
 * @return true on success, false on failure
 */
static bool clear_document(struct ptk_anm2editor *editor, struct ov_error *const err) {
  if (!editor || !editor->edit_core) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_invalid_argument);
    return false;
  }
  if (!ptk_anm2_edit_reset(editor->edit_core, err)) {
    OV_ERROR_ADD_TRACE(err);
    return false;
  }
  if (editor->file_path) {
    OV_ARRAY_DESTROY(&editor->file_path);
  }
  return true;
}

/**
 * @brief Refresh all UI views to reflect current document state
 *
 * Updates TreeView, detail pane, and window title.
 *
 * @param editor Editor instance
 * @param err Error information
 * @return true on success, false on failure
 */
static bool refresh_all_views(struct ptk_anm2editor *editor, struct ov_error *const err) {
  if (!editor) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_invalid_argument);
    return false;
  }
  if (!anm2editor_treeview_refresh(editor->treeview, err)) {
    OV_ERROR_ADD_TRACE(err);
    return false;
  }
  anm2editor_detail_refresh(editor->detail);
  update_window_title(editor);
  update_toolbar_state(editor);
  return true;
}

/**
 * @brief Callback for when selection changes in the detail list
 */
static void on_detail_selection_changed(void *userdata) {
  struct ptk_anm2editor *editor = (struct ptk_anm2editor *)userdata;
  if (editor) {
    update_toolbar_state(editor);
  }
}

/**
 * @brief Callback for errors in the detail list
 */
static void detail_cb_on_error(void *userdata, struct ov_error *err) {
  struct ptk_anm2editor *editor = (struct ptk_anm2editor *)userdata;
  show_error_dialog(editor, err);
}

// Command handler for ID_FILE_NEW
static void handle_cmd_file_new(struct ptk_anm2editor *editor) {
  struct ov_error err = {0};
  bool success = false;

  if (!ptk_anm2editor_new_document(editor, &err)) {
    OV_ERROR_ADD_TRACE(&err);
    goto cleanup;
  }

  success = true;

cleanup:
  if (!success) {
    show_error_dialog(editor, &err);
  }
}

// Command handler for ID_FILE_OPEN
static void handle_cmd_file_open(struct ptk_anm2editor *editor) {
  struct ov_error err = {0};
  bool success = false;

  if (!ptk_anm2editor_open(editor, NULL, &err)) {
    OV_ERROR_ADD_TRACE(&err);
    goto cleanup;
  }

  success = true;

cleanup:
  if (!success) {
    show_error_dialog(editor, &err);
  }
}

// Command handler for ID_FILE_SAVE
static void handle_cmd_file_save(struct ptk_anm2editor *editor) {
  struct ov_error err = {0};
  bool success = false;

  if (!ptk_anm2editor_save(editor, &err)) {
    OV_ERROR_ADD_TRACE(&err);
    goto cleanup;
  }

  success = true;

cleanup:
  if (!success) {
    show_error_dialog(editor, &err);
  }
}

// Command handler for ID_FILE_SAVEAS
static void handle_cmd_file_saveas(struct ptk_anm2editor *editor) {
  struct ov_error err = {0};
  bool success = false;

  if (!ptk_anm2editor_save_as(editor, &err)) {
    OV_ERROR_ADD_TRACE(&err);
    goto cleanup;
  }

  success = true;

cleanup:
  if (!success) {
    show_error_dialog(editor, &err);
  }
}

static void treeview_cb_on_selection_changed(void *userdata,
                                             struct anm2editor_treeview_item_info const *item,
                                             bool ctrl_pressed,
                                             bool shift_pressed) {
  (void)item;
  (void)ctrl_pressed;
  (void)shift_pressed;
  struct ptk_anm2editor *editor = (struct ptk_anm2editor *)userdata;
  anm2editor_detail_cancel_edit(editor->detail);

  // Selection is already updated in TVN_SELCHANGINGW handler
  // Just update detail list and toolbar here
  anm2editor_detail_refresh(editor->detail);
  update_toolbar_state(editor);
}

// Error callback for TreeView operations
static void treeview_cb_on_error(void *userdata, struct ov_error *err) {
  struct ptk_anm2editor *editor = (struct ptk_anm2editor *)userdata;
  show_error_dialog(editor, err);
  OV_ERROR_DESTROY(err);
}

// Command handler for ID_EDIT_UNDO
static void handle_cmd_undo(struct ptk_anm2editor *editor) {
  if (!editor || !editor->edit_core) {
    return;
  }
  struct ov_error err = {0};
  if (!ptk_anm2_edit_undo(editor->edit_core, &err)) {
    show_error_dialog(editor, &err);
  }
}

// Command handler for ID_EDIT_REDO
static void handle_cmd_redo(struct ptk_anm2editor *editor) {
  if (!editor || !editor->edit_core) {
    return;
  }
  struct ov_error err = {0};
  if (!ptk_anm2_edit_redo(editor->edit_core, &err)) {
    show_error_dialog(editor, &err);
  }
}

// Add animation item with parameters to a selector
static bool add_animation_item(struct ptk_anm2editor *editor,
                               uint32_t selector_id,
                               char const *script_name,
                               char const *display_name,
                               struct ptk_alias_extracted_param const *params,
                               size_t param_count,
                               struct ov_error *err) {
  if (!editor || !editor->edit_core || !script_name || !display_name || selector_id == 0) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_invalid_argument);
    return false;
  }

  // Use transaction to group all operations for single undo
  if (!ptk_anm2_edit_begin_transaction(editor->edit_core, err)) {
    OV_ERROR_ADD_TRACE(err);
    return false;
  }

  bool success = false;
  uint32_t item_id = 0;
  uint32_t *item_ids = NULL;

  // Insert animation item at the beginning of the selector
  // For "before" semantics: if items exist, use first item's ID; otherwise use selector_id (appends to empty)
  uint32_t before_id = selector_id;
  item_ids = ptk_anm2_get_item_ids(ptk_anm2_edit_get_doc(editor->edit_core), selector_id, err);
  if (!item_ids) {
    OV_ERROR_ADD_TRACE(err);
    goto cleanup;
  }
  if (OV_ARRAY_LENGTH(item_ids) > 0) {
    before_id = item_ids[0];
  }
  if (!ptk_anm2_edit_insert_animation_item(editor->edit_core, before_id, script_name, display_name, err)) {
    OV_ERROR_ADD_TRACE(err);
    goto cleanup;
  }

  // Get the inserted item's ID (it's now first in the selector)
  OV_ARRAY_DESTROY(&item_ids);
  item_ids = ptk_anm2_get_item_ids(ptk_anm2_edit_get_doc(editor->edit_core), selector_id, err);
  if (!item_ids) {
    OV_ERROR_ADD_TRACE(err);
    goto cleanup;
  }
  if (OV_ARRAY_LENGTH(item_ids) == 0) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_unexpected);
    goto cleanup;
  }
  item_id = item_ids[0];

  // Add parameters to the inserted animation item
  for (size_t i = 0; i < param_count; i++) {
    if (params[i].key && params[i].value) {
      if (!ptk_anm2_edit_param_add(editor->edit_core, item_id, params[i].key, params[i].value, err)) {
        OV_ERROR_ADD_TRACE(err);
        goto cleanup;
      }
    }
  }

  success = true;

cleanup:
  if (item_ids) {
    OV_ARRAY_DESTROY(&item_ids);
  }
  if (!ptk_anm2_edit_end_transaction(editor->edit_core, success, err)) {
    if (success) {
      OV_ERROR_ADD_TRACE(err);
      success = false;
    }
  }

  return success;
}

// Import a single script item to the editor (ID-based)
static bool import_single_script(struct ptk_anm2editor *const editor,
                                 uint32_t const selector_id,
                                 char const *const alias,
                                 struct ptk_alias_available_script const *const item,
                                 struct ov_error *const err) {
  struct ptk_alias_extracted_animation anim = {0};
  char *translated_name = NULL;
  bool success = false;

  if (!ptk_alias_extract_animation(alias, strlen(alias), item->script_name, item->effect_name, &anim, err)) {
    OV_ERROR_ADD_TRACE(err);
    goto cleanup;
  }

  // Use translated name if available, otherwise use effect_name
  if (item->translated_name) {
    size_t const src_len = wcslen(item->translated_name);
    size_t const utf8_len = ov_wchar_to_utf8_len(item->translated_name, src_len);
    if (utf8_len == 0) {
      OV_ERROR_SET_GENERIC(err, ov_error_generic_out_of_memory);
      goto cleanup;
    }
    if (!OV_ARRAY_GROW(&translated_name, utf8_len + 1)) {
      OV_ERROR_SET_GENERIC(err, ov_error_generic_out_of_memory);
      goto cleanup;
    }
    ov_wchar_to_utf8(item->translated_name, src_len, translated_name, utf8_len + 1, NULL);
    OV_ARRAY_SET_LENGTH(translated_name, utf8_len + 1);
  }

  if (!add_animation_item(editor,
                          selector_id,
                          anim.script_name,
                          translated_name ? translated_name : anim.effect_name,
                          anim.params,
                          OV_ARRAY_LENGTH(anim.params),
                          err)) {
    OV_ERROR_ADD_TRACE(err);
    goto cleanup;
  }
  success = true;

cleanup:
  if (translated_name) {
    OV_ARRAY_DESTROY(&translated_name);
  }
  ptk_alias_extracted_animation_free(&anim);
  return success;
}

// Execute import scripts transaction
// Handles the actual import of selected scripts within a transaction
static bool import_scripts_execute_transaction(struct ptk_anm2editor *const editor,
                                               char const *const alias,
                                               struct ptk_alias_available_scripts const *const scripts,
                                               char const *const selector_name,
                                               bool const has_selected,
                                               bool const update_psd_path,
                                               struct ov_error *const err) {
  bool success = false;

  // Get the selected selector ID (or create a new one if none selected)
  uint32_t selector_id = get_selected_selector_id(editor);
  if (selector_id == 0 && has_selected) {
    // No selector selected, add a new one with provided name
    if (!ptk_anm2_edit_add_selector(editor->edit_core, selector_name ? selector_name : "", err)) {
      OV_ERROR_ADD_TRACE(err);
      goto cleanup;
    }
    // Get the newly created selector's ID
    size_t const sel_count = ptk_anm2_edit_selector_count(editor->edit_core);
    if (sel_count > 0) {
      selector_id = ptk_anm2_edit_selector_get_id(editor->edit_core, sel_count - 1);
    }
  }

  // Import selected scripts to the editor
  if (has_selected) {
    size_t const scripts_count = OV_ARRAY_LENGTH(scripts->items);
    for (size_t i = 0; i < scripts_count; i++) {
      if (!scripts->items[i].selected) {
        continue;
      }
      if (!import_single_script(editor, selector_id, alias, &scripts->items[i], err)) {
        OV_ERROR_ADD_TRACE(err);
        goto cleanup;
      }
    }
  }

  // Update PSD path if requested
  if (update_psd_path && scripts->psd_path) {
    if (!ptk_anm2_edit_set_psd_path(editor->edit_core, scripts->psd_path, err)) {
      OV_ERROR_ADD_TRACE(err);
      goto cleanup;
    }
  }

  success = true;

cleanup:
  return success;
}

// Callback for anm2editor_import_execute
static bool import_callback(void *const context,
                            char const *const alias,
                            struct ptk_alias_available_scripts const *const scripts,
                            char const *const selector_name,
                            bool const update_psd_path,
                            struct ov_error *const err) {
  struct ptk_anm2editor *const editor = (struct ptk_anm2editor *)context;
  bool success = false;
  bool transaction_started = false;

  // Check if there are any selected items
  bool has_selected = false;
  size_t const n = OV_ARRAY_LENGTH(scripts->items);
  for (size_t i = 0; i < n; i++) {
    if (scripts->items[i].selected) {
      has_selected = true;
      break;
    }
  }

  if (!ptk_anm2_edit_begin_transaction(editor->edit_core, err)) {
    OV_ERROR_ADD_TRACE(err);
    goto cleanup;
  }
  transaction_started = true;

  if (!import_scripts_execute_transaction(editor, alias, scripts, selector_name, has_selected, update_psd_path, err)) {
    OV_ERROR_ADD_TRACE(err);
    goto cleanup;
  }
  success = true;

cleanup:
  if (transaction_started) {
    if (!ptk_anm2_edit_end_transaction(editor->edit_core, success, err)) {
      if (success) {
        OV_ERROR_ADD_TRACE(err);
      }
    }
  }
  return success;
}

// New command handler for ID_EDIT_IMPORT_SCRIPTS using anm2editor_import module
static void handle_cmd_import_scripts(struct ptk_anm2editor *editor) {
  if (!editor || !editor->edit) {
    return;
  }

  struct ov_error err = {0};

  char const *const current_psd_path = ptk_anm2_edit_get_psd_path(editor->edit_core);
  bool const has_selected_selector = (get_selected_selector_id(editor) != 0);

  if (!anm2editor_import_execute(
          editor->window, editor->edit, current_psd_path, has_selected_selector, import_callback, editor, &err)) {
    show_error_dialog(editor, &err);
  }
}

// New command handler for ID_EDIT_CONVERT_ANM using anm2editor_convert module
static void handle_cmd_convert_anm(struct ptk_anm2editor *editor) {
  if (!editor) {
    return;
  }

  struct ov_error err = {0};
  wchar_t *default_dir = NULL;
  bool success = false;

  // Get default directory (Script folder)
  if (!get_script_dir(&default_dir, &err)) {
    OV_ERROR_ADD_TRACE(&err);
    goto cleanup;
  }

  if (!anm2editor_convert_execute(editor->window, default_dir, &err)) {
    OV_ERROR_ADD_TRACE(&err);
    goto cleanup;
  }

  success = true;

cleanup:
  if (default_dir) {
    OV_ARRAY_DESTROY(&default_dir);
  }
  if (!success) {
    show_error_dialog(editor, &err);
  }
}

// Helper function to save file with error handling for confirm_discard_changes
static bool save_with_error_dialog(struct ptk_anm2editor *editor) {
  struct ov_error err = {0};
  bool success = false;

  if (!ptk_anm2editor_save(editor, &err)) {
    OV_ERROR_ADD_TRACE(&err);
    goto cleanup;
  }

  success = true;

cleanup:
  if (!success) {
    show_error_dialog(editor, &err);
  }
  return success;
}

static void splitter_cb_on_position_changed(void *userdata) {
  struct ptk_anm2editor *editor = (struct ptk_anm2editor *)userdata;
  if (editor && editor->window) {
    RECT rc;
    GetClientRect(editor->window, &rc);
    SendMessageW(editor->window, WM_SIZE, 0, MAKELPARAM(rc.right - rc.left, rc.bottom - rc.top));
  }
}

static void toolbar_cb_on_file_new(void *userdata) {
  struct ptk_anm2editor *editor = (struct ptk_anm2editor *)userdata;
  handle_cmd_file_new(editor);
}

static void toolbar_cb_on_file_open(void *userdata) {
  struct ptk_anm2editor *editor = (struct ptk_anm2editor *)userdata;
  handle_cmd_file_open(editor);
}

static void toolbar_cb_on_file_save(void *userdata) {
  struct ptk_anm2editor *editor = (struct ptk_anm2editor *)userdata;
  handle_cmd_file_save(editor);
}

static void toolbar_cb_on_file_saveas(void *userdata) {
  struct ptk_anm2editor *editor = (struct ptk_anm2editor *)userdata;
  handle_cmd_file_saveas(editor);
}

static void toolbar_cb_on_edit_undo(void *userdata) {
  struct ptk_anm2editor *editor = (struct ptk_anm2editor *)userdata;
  handle_cmd_undo(editor);
}

static void toolbar_cb_on_edit_redo(void *userdata) {
  struct ptk_anm2editor *editor = (struct ptk_anm2editor *)userdata;
  handle_cmd_redo(editor);
}

static void toolbar_cb_on_edit_import_scripts(void *userdata) {
  struct ptk_anm2editor *editor = (struct ptk_anm2editor *)userdata;
  handle_cmd_import_scripts(editor);
}

static void toolbar_cb_on_edit_convert_anm(void *userdata) {
  struct ptk_anm2editor *editor = (struct ptk_anm2editor *)userdata;
  handle_cmd_convert_anm(editor);
}

// Initialize child windows (called from WM_CREATE)
// Returns true on success, false on failure
static bool handle_wm_create(struct ptk_anm2editor *editor, HWND hwnd, struct ov_error *const err) {
  static int const splitter_width = 4;
  static int const min_pane_width = 50;
  bool success = false;

  editor->toolbar = anm2editor_toolbar_create(hwnd,
                                              IDC_TOOLBAR,
                                              &(struct anm2editor_toolbar_callbacks){
                                                  .userdata = editor,
                                                  .on_file_new = toolbar_cb_on_file_new,
                                                  .on_file_open = toolbar_cb_on_file_open,
                                                  .on_file_save = toolbar_cb_on_file_save,
                                                  .on_file_saveas = toolbar_cb_on_file_saveas,
                                                  .on_edit_undo = toolbar_cb_on_edit_undo,
                                                  .on_edit_redo = toolbar_cb_on_edit_redo,
                                                  .on_edit_import_scripts = toolbar_cb_on_edit_import_scripts,
                                                  .on_edit_convert_anm = toolbar_cb_on_edit_convert_anm,
                                              },
                                              err);
  if (!editor->toolbar) {
    OV_ERROR_ADD_TRACE(err);
    goto cleanup;
  }

  editor->treeview = anm2editor_treeview_create(hwnd,
                                                IDC_TREEVIEW,
                                                editor->edit_core,
                                                &(struct anm2editor_treeview_callbacks){
                                                    .userdata = editor,
                                                    .on_selection_changed = treeview_cb_on_selection_changed,
                                                    .on_error = treeview_cb_on_error,
                                                },
                                                err);
  if (!editor->treeview) {
    OV_ERROR_ADD_TRACE(err);
    goto cleanup;
  }

  editor->detail = anm2editor_detail_create(hwnd,
                                            IDC_DETAILLIST,
                                            editor->edit_core,
                                            &(struct anm2editor_detail_callbacks){
                                                .userdata = editor,
                                                .on_selection_changed = on_detail_selection_changed,
                                                .on_error = detail_cb_on_error,
                                            },
                                            err);
  if (!editor->detail) {
    OV_ERROR_ADD_TRACE(err);
    goto cleanup;
  }

  editor->splitter = anm2editor_splitter_create(splitter_width,
                                                min_pane_width,
                                                &(struct anm2editor_splitter_callbacks){
                                                    .userdata = editor,
                                                    .on_position_changed = splitter_cb_on_position_changed,
                                                },
                                                err);
  if (!editor->splitter) {
    OV_ERROR_ADD_TRACE(err);
    goto cleanup;
  }

  update_window_title(editor);
  anm2editor_detail_refresh(editor->detail);
  update_toolbar_state(editor);

  success = true;

cleanup:
  return success;
}

static bool confirm_discard_changes(struct ptk_anm2editor *editor) {
  if (!ptk_anm2_edit_is_modified(editor->edit_core)) {
    return true;
  }

  wchar_t main_instruction[256];
  ov_snprintf_wchar(main_instruction,
                    sizeof(main_instruction) / sizeof(main_instruction[0]),
                    L"%hs",
                    L"%hs",
                    pgettext("anm2editor", "Do you want to save changes before closing?"));

  int const button_id = ptk_dialog_show(&(struct ptk_dialog_params){
      .owner = editor->window,
      .icon = TD_WARNING_ICON,
      .buttons = TDCBF_YES_BUTTON | TDCBF_NO_BUTTON | TDCBF_CANCEL_BUTTON,
      .window_title = get_window_title(),
      .main_instruction = main_instruction,
  });

  if (button_id == IDCANCEL) {
    return false;
  }

  if (button_id == IDYES) {
    if (!save_with_error_dialog(editor)) {
      return false;
    }
  }

  return true;
}

static LRESULT CALLBACK anm2editor_wnd_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
  static wchar_t const prop_name[] = L"ptk_anm2editor";
  struct ptk_anm2editor *editor = (struct ptk_anm2editor *)GetPropW(hwnd, prop_name);

  switch (message) {
  case WM_CREATE: {
    CREATESTRUCTW const *cs = (CREATESTRUCTW const *)lparam;
    SetPropW(hwnd, prop_name, cs->lpCreateParams);
    editor = (struct ptk_anm2editor *)cs->lpCreateParams;

    struct ov_error err = {0};
    if (!handle_wm_create(editor, hwnd, &err)) {
      ptk_logf_error(&err, "%1$hs", "%1$hs", gettext("failed to create window."));
      OV_ERROR_DESTROY(&err);
      return -1;
    }
    return 0;
  }

  case WM_SIZE: {
    if (!editor) {
      break;
    }
    RECT rc;
    GetClientRect(hwnd, &rc);

    // Position toolbar at top
    anm2editor_toolbar_autosize(editor->toolbar);
    int const toolbar_height = anm2editor_toolbar_get_height(editor->toolbar);

    int const content_height = rc.bottom - rc.top - toolbar_height;
    int const content_width = rc.right - rc.left;

    // Calculate layout using splitter
    struct anm2editor_splitter_layout layout;
    anm2editor_splitter_calculate_layout(editor->splitter, 0, toolbar_height, content_width, content_height, &layout);

    // Position treeview on left
    anm2editor_treeview_set_position(editor->treeview, layout.left_x, layout.y, layout.left_width, layout.height);

    // Position detail list on right (after splitter)
    anm2editor_detail_set_position(editor->detail, layout.right_x, layout.y, layout.right_width, layout.height);

    // Auto-size columns in detail list
    HWND const listview = (HWND)anm2editor_detail_get_window(editor->detail);
    if (listview) {
      SendMessageW(listview, LVM_SETCOLUMNWIDTH, 0, layout.right_width * 35 / 100);
      SendMessageW(listview, LVM_SETCOLUMNWIDTH, 1, layout.right_width * 60 / 100);
    }

    return 0;
  }

  case WM_COMMAND: {
    if (!editor) {
      break;
    }
    if (anm2editor_toolbar_handle_command(editor->toolbar, LOWORD(wparam))) {
      return 0;
    }
    break;
  }

  case WM_NOTIFY: {
    if (anm2editor_toolbar_handle_notify(editor->toolbar, (void *)lparam)) {
      return 0;
    }
    NMHDR const *nmhdr = (NMHDR const *)lparam;
    if (nmhdr->idFrom == IDC_TREEVIEW) {
      return anm2editor_treeview_handle_notify(editor->treeview, (void *)lparam);
    }
    if (nmhdr->idFrom == IDC_DETAILLIST) {
      return anm2editor_detail_handle_notify(editor->detail, (void *)lparam);
    }
    break;
  }

  case WM_SETCURSOR: {
    if (!editor) {
      break;
    }
    // Check if cursor is over the splitter area
    POINT pt;
    GetCursorPos(&pt);
    ScreenToClient(hwnd, &pt);

    if (anm2editor_splitter_handle_setcursor(editor->splitter, hwnd, pt.x, pt.y)) {
      return TRUE;
    }
    break;
  }

  case WM_LBUTTONDOWN: {
    if (!editor) {
      break;
    }
    int const x = (short)LOWORD(lparam);
    int const y = (short)HIWORD(lparam);
    if (anm2editor_splitter_handle_lbutton_down(editor->splitter, hwnd, x, y)) {
      return 0;
    }
    break;
  }

  case WM_MOUSEMOVE: {
    if (!editor) {
      break;
    }
    int const x = (short)LOWORD(lparam);
    int const y = (short)HIWORD(lparam);
    if (anm2editor_splitter_handle_mouse_move(editor->splitter, hwnd, x)) {
      return 0;
    }
    anm2editor_treeview_handle_mouse_move(editor->treeview, x, y);
    return 0;
  }

  case WM_LBUTTONUP: {
    if (!editor) {
      break;
    }
    if (anm2editor_splitter_handle_lbutton_up(editor->splitter, hwnd)) {
      return 0;
    }
    anm2editor_treeview_handle_lbutton_up(editor->treeview);
    return 0;
  }

  case WM_CLOSE:
    if (!editor) {
      break;
    }
    // Cancel any pending inline edit (commit is handled by losing focus)
    anm2editor_detail_cancel_edit(editor->detail);
    if (!confirm_discard_changes(editor)) {
      return 0;
    }
    DestroyWindow(hwnd);
    return 0;

  case WM_NCDESTROY:
    if (!editor) {
      break;
    }
    if (editor->toolbar) {
      anm2editor_toolbar_destroy(&editor->toolbar);
    }
    if (editor->splitter) {
      anm2editor_splitter_destroy(&editor->splitter);
    }
    RemovePropW(hwnd, prop_name);
    return 0;

  case WM_ANM2EDITOR_UPDATE_TITLE:
    if (editor) {
      update_window_title(editor);
    }
    return 0;
  }

  return DefWindowProcW(hwnd, message, wparam, lparam);
}

bool ptk_anm2editor_create(struct ptk_anm2editor **editor_out,
                           wchar_t const *title,
                           struct aviutl2_edit_handle *edit_handle,
                           void **window_out,
                           struct ov_error *err) {
  struct ptk_anm2editor *editor = NULL;
  bool success = false;

  if (!editor_out) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_invalid_argument);
    goto cleanup;
  }

  if (!OV_REALLOC(&editor, 1, sizeof(struct ptk_anm2editor))) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_out_of_memory);
    goto cleanup;
  }
  *editor = (struct ptk_anm2editor){
      .edit = edit_handle,
  };

  editor->edit_core = ptk_anm2_edit_create(err);
  if (!editor->edit_core) {
    OV_ERROR_ADD_TRACE(err);
    goto cleanup;
  }
  // Set view callback for edit_core
  ptk_anm2_edit_set_view_callback(editor->edit_core, on_edit_view_change, editor);

  // Create window if requested
  if (window_out) {
    HINSTANCE const hinst = GetModuleHandleW(NULL);

    ATOM const atom = RegisterClassExW(&(WNDCLASSEXW){
        .cbSize = sizeof(WNDCLASSEXW),
        .lpszClassName = anm2editor_window_class_name,
        .lpfnWndProc = anm2editor_wnd_proc,
        .hInstance = hinst,
        .hbrBackground = (HBRUSH)(COLOR_WINDOW + 1),
        .hCursor = LoadCursorW(NULL, MAKEINTRESOURCEW(32512)),
    });
    if (!atom) {
      OV_ERROR_SET_HRESULT(err, HRESULT_FROM_WIN32(GetLastError()));
      goto cleanup;
    }
    editor->window_class = atom;

    // Use WS_CLIPCHILDREN to prevent parent from drawing over child windows
    HWND window = CreateWindowExW(0,
                                  anm2editor_window_class_name,
                                  title,
                                  WS_POPUP | WS_CLIPCHILDREN,
                                  CW_USEDEFAULT,
                                  CW_USEDEFAULT,
                                  400,
                                  300,
                                  NULL,
                                  NULL,
                                  hinst,
                                  editor);
    if (!window) {
      OV_ERROR_SET_HRESULT(err, HRESULT_FROM_WIN32(GetLastError()));
      UnregisterClassW(anm2editor_window_class_name, hinst);
      editor->window_class = 0;
      goto cleanup;
    }

    editor->window = window;
    *window_out = window;
  }

  *editor_out = editor;
  success = true;

cleanup:
  if (!success) {
    if (editor) {
      if (editor->edit_core) {
        ptk_anm2_edit_destroy(&editor->edit_core);
      }
      OV_FREE(&editor);
    }
  }
  return success;
}

void ptk_anm2editor_destroy(struct ptk_anm2editor **editor_ptr) {
  if (!editor_ptr || !*editor_ptr) {
    return;
  }
  struct ptk_anm2editor *editor = *editor_ptr;

  if (editor->window) {
    DestroyWindow(editor->window);
    editor->window = NULL;
  }
  editor->toolbar = NULL;

  if (editor->treeview) {
    anm2editor_treeview_destroy(&editor->treeview);
  }

  if (editor->detail) {
    anm2editor_detail_destroy(&editor->detail);
  }

  if (editor->window_class) {
    UnregisterClassW(anm2editor_window_class_name, GetModuleHandleW(NULL));
    editor->window_class = 0;
  }

  if (editor->edit_core) {
    ptk_anm2_edit_destroy(&editor->edit_core);
  }

  if (editor->file_path) {
    OV_ARRAY_DESTROY(&editor->file_path);
  }

  OV_FREE(editor_ptr);
}

void *ptk_anm2editor_get_window(struct ptk_anm2editor *editor) { return editor ? editor->window : NULL; }

struct ptk_anm2_edit *ptk_anm2editor_get_edit(struct ptk_anm2editor *editor) {
  return editor ? editor->edit_core : NULL;
}

bool ptk_anm2editor_new_document(struct ptk_anm2editor *editor, struct ov_error *err) {
  if (!editor) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_invalid_argument);
    return false;
  }

  if (!confirm_discard_changes(editor)) {
    return true; // User cancelled, not an error
  }

  if (!clear_document(editor, err)) {
    OV_ERROR_ADD_TRACE(err);
    return false;
  }
  if (!refresh_all_views(editor, err)) {
    OV_ERROR_ADD_TRACE(err);
    return false;
  }

  return true;
}

bool ptk_anm2editor_open(struct ptk_anm2editor *editor, wchar_t const *path, struct ov_error *err) {
  if (!editor || !editor->edit_core) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_invalid_argument);
    return false;
  }

  if (!confirm_discard_changes(editor)) {
    return true; // User cancelled, not an error
  }

  wchar_t *selected_path = NULL;
  wchar_t *default_dir = NULL;
  bool success = false;
  ov_tribool checksum_result = ov_indeterminate;

  if (!path) {
    // Get default directory (Script folder)
    if (!get_script_dir(&default_dir, err)) {
      OV_ERROR_ADD_TRACE(err);
      goto cleanup;
    }

    // Show open dialog
    wchar_t title[256];
    ov_snprintf_wchar(title, sizeof(title) / sizeof(title[0]), L"%1$hs", L"%1$hs", pgettext("anm2editor", "Open"));
    if (!ovl_dialog_select_file(
            editor->window, title, get_file_filter(), &g_file_dialog_guid, default_dir, &selected_path, err)) {
      if (ov_error_is(err, ov_error_type_hresult, (int)HRESULT_FROM_WIN32(ERROR_CANCELLED))) {
        // User cancelled
        OV_ERROR_DESTROY(err);
        success = true;
        goto cleanup;
      }
      OV_ERROR_ADD_TRACE(err);
      goto cleanup;
    }
  } else {
    size_t const len = wcslen(path);
    if (!OV_ARRAY_GROW(&selected_path, len + 1)) {
      OV_ERROR_SET_GENERIC(err, ov_error_generic_out_of_memory);
      goto cleanup;
    }
    memcpy(selected_path, path, (len + 1) * sizeof(wchar_t));
  }

  // Verify checksum - warn if file was manually edited
  checksum_result = ptk_anm2_edit_verify_file_checksum(selected_path, err);
  if (checksum_result == ov_indeterminate) {
    OV_ERROR_ADD_TRACE(err);
    goto cleanup;
  }
  if (checksum_result == ov_false) {
    wchar_t main_instr[256];
    wchar_t content[512];
    ov_snprintf_wchar(main_instr,
                      sizeof(main_instr) / sizeof(main_instr[0]),
                      L"%hs",
                      L"%hs",
                      pgettext("anm2editor", "Do you want to continue opening this file?"));
    ov_snprintf_wchar(content,
                      sizeof(content) / sizeof(content[0]),
                      L"%hs",
                      L"%hs",
                      pgettext("anm2editor",
                               "This file appears to have been manually edited. "
                               "If you continue editing in this editor, the manual changes may be lost."));
    int result = ptk_dialog_show(&(struct ptk_dialog_params){
        .owner = editor->window,
        .icon = TD_WARNING_ICON,
        .buttons = TDCBF_YES_BUTTON | TDCBF_NO_BUTTON,
        .default_button = IDNO,
        .window_title = get_window_title(),
        .main_instruction = main_instr,
        .content = content,
    });
    if (result != IDYES) {
      // User cancelled - keep existing document
      success = true;
      goto cleanup;
    }
  }

  // Verification passed - load into the actual document via edit_core
  // This will trigger selection clear and view callbacks automatically
  if (!ptk_anm2_edit_load(editor->edit_core, selected_path, err)) {
    OV_ERROR_ADD_TRACE(err);
    goto cleanup;
  }

  // Update file path
  if (editor->file_path) {
    OV_ARRAY_DESTROY(&editor->file_path);
  }
  editor->file_path = selected_path;
  selected_path = NULL; // Transfer ownership

  success = true;

cleanup:
  if (default_dir) {
    OV_ARRAY_DESTROY(&default_dir);
  }
  if (selected_path) {
    OV_ARRAY_DESTROY(&selected_path);
  }
  return success;
}

bool ptk_anm2editor_save(struct ptk_anm2editor *editor, struct ov_error *err) {
  if (!editor || !editor->edit_core) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_invalid_argument);
    return false;
  }

  if (!editor->file_path || editor->file_path[0] == L'\0') {
    return ptk_anm2editor_save_as(editor, err);
  }

  if (!ptk_anm2_edit_save(editor->edit_core, editor->file_path, err)) {
    OV_ERROR_ADD_TRACE(err);
    return false;
  }

  return true;
}

bool ptk_anm2editor_save_as(struct ptk_anm2editor *editor, struct ov_error *err) {
  static wchar_t const ext[] = L".ptk.anm2";
  static size_t const ext_len = sizeof(ext) / sizeof(ext[0]) - 1;

  if (!editor) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_invalid_argument);
    return false;
  }

  wchar_t *selected_path = NULL;
  bool success = false;
  wchar_t name_buf[MAX_PATH] = {0};
  size_t name_len = 0;

  // Get default directory (Script folder)
  wchar_t *default_path = NULL;
  if (!get_script_dir(&default_path, err)) {
    OV_ERROR_ADD_TRACE(err);
    goto cleanup;
  }

  // Append default filename (without extension) if available
  if (editor->file_path && editor->file_path[0] != L'\0') {
    // Use existing filename
    wchar_t const *const name = ovl_path_extract_file_name(editor->file_path);
    name_len = wcslen(name);
    if (name_len >= ext_len && ovl_path_is_same_ext(name + name_len - ext_len, ext)) {
      name_len -= ext_len;
    }
    if (name_len > 0 && name_len < MAX_PATH) {
      memcpy(name_buf, name, name_len * sizeof(wchar_t));
    }
  } else {
    // Generate from PSD path
    char const *const psd_path = ptk_anm2_edit_get_psd_path(editor->edit_core);
    if (psd_path && psd_path[0] != '\0') {
      char const *const pipe = strchr(psd_path, '|');
      size_t const path_len = pipe ? (size_t)(pipe - psd_path) : strlen(psd_path);
      if (path_len < MAX_PATH) {
        char path_buf[MAX_PATH];
        memcpy(path_buf, psd_path, path_len);
        path_buf[path_len] = '\0';
        char const *base_name = ovl_path_extract_file_name(path_buf);
        char const *base_ext = ovl_path_find_ext(base_name);
        size_t const base_len = base_ext ? (size_t)(base_ext - base_name) : strlen(base_name);
        if (base_len > 0 && base_len < MAX_PATH) {
          ov_utf8_to_wchar(base_name, base_len, name_buf, MAX_PATH, NULL);
          name_len = wcslen(name_buf);
        }
      }
    }
  }

  if (name_len > 0) {
    // Add @ prefix if not already present
    if (name_buf[0] != L'@') {
      // Shift content to make room for @
      if (name_len + 1 < MAX_PATH) {
        memmove(name_buf + 1, name_buf, (name_len + 1) * sizeof(wchar_t));
        name_buf[0] = L'@';
        name_len++;
      }
    }
    size_t const dir_len = OV_ARRAY_LENGTH(default_path);
    if (!OV_ARRAY_GROW(&default_path, dir_len + name_len + 1)) {
      OV_ERROR_SET_GENERIC(err, ov_error_generic_out_of_memory);
      goto cleanup;
    }
    memcpy(default_path + dir_len, name_buf, (name_len + 1) * sizeof(wchar_t));
    OV_ARRAY_SET_LENGTH(default_path, dir_len + name_len);
  }

  // Show save dialog
  {
    wchar_t title[256];
    ov_snprintf_wchar(title, sizeof(title) / sizeof(title[0]), L"%1$hs", L"%1$hs", pgettext("anm2editor", "Save As"));
    if (!ovl_dialog_save_file(
            editor->window, title, get_file_filter(), &g_file_dialog_guid, default_path, NULL, &selected_path, err)) {
      if (ov_error_is(err, ov_error_type_hresult, (int)HRESULT_FROM_WIN32(ERROR_CANCELLED))) {
        OV_ERROR_DESTROY(err);
        success = true;
      } else {
        OV_ERROR_ADD_TRACE(err);
      }
      goto cleanup;
    }
  }

  // Add extension if not present
  {
    size_t const path_len = wcslen(selected_path);
    if (path_len < ext_len || !ovl_path_is_same_ext(selected_path + path_len - ext_len, ext)) {
      if (!OV_ARRAY_GROW(&selected_path, path_len + ext_len + 1)) {
        OV_ERROR_SET_GENERIC(err, ov_error_generic_out_of_memory);
        goto cleanup;
      }
      wcscpy(selected_path + path_len, ext);
      OV_ARRAY_SET_LENGTH(selected_path, path_len + ext_len + 1);
    }
  }

  if (!ptk_anm2_edit_save(editor->edit_core, selected_path, err)) {
    OV_ERROR_ADD_TRACE(err);
    goto cleanup;
  }

  if (editor->file_path) {
    OV_ARRAY_DESTROY(&editor->file_path);
  }
  editor->file_path = selected_path;
  selected_path = NULL;

  success = true;

cleanup:
  if (default_path) {
    OV_ARRAY_DESTROY(&default_path);
  }
  if (selected_path) {
    OV_ARRAY_DESTROY(&selected_path);
  }
  return success;
}

bool ptk_anm2editor_is_modified(struct ptk_anm2editor *editor) {
  return editor && editor->edit_core ? ptk_anm2_edit_is_modified(editor->edit_core) : false;
}

bool ptk_anm2editor_is_open(struct ptk_anm2editor *editor) {
  if (!editor || !editor->window) {
    return false;
  }
  return IsWindowVisible(editor->window) != 0;
}

/**
 * @brief Check PSD path mismatch and show warning dialog if needed
 *
 * @param editor Editor instance
 * @param psd_path PSD path to check against current editor path
 * @return true to proceed, false if user cancelled
 */
static bool check_psd_path_mismatch(struct ptk_anm2editor *editor, char const *psd_path) {
  if (!psd_path || psd_path[0] == '\0') {
    return true;
  }
  char const *current_psd_path = ptk_anm2_edit_get_psd_path(editor->edit_core);
  if (!current_psd_path || current_psd_path[0] == '\0') {
    return true;
  }
  if (strcmp(current_psd_path, psd_path) == 0) {
    return true;
  }
  // Mismatch - show warning dialog
  wchar_t main_instr[256];
  ov_snprintf_wchar(main_instr, 256, L"%hs", L"%hs", pgettext("anm2editor", "Do you want to continue adding?"));
  wchar_t content[512];
  ov_snprintf_char2wchar(content,
                         512,
                         "%1$hs%2$hs",
                         pgettext("anm2editor",
                                  "The anm2 Editor is editing a script for a different PSD file.\n\n"
                                  "Editor:\n%1$hs\n\n"
                                  "This layer:\n%2$hs\n\n"
                                  "Adding may not work as expected."),
                         current_psd_path,
                         psd_path);
  int result = ptk_dialog_show(&(struct ptk_dialog_params){
      .owner = editor->window,
      .icon = TD_WARNING_ICON,
      .buttons = TDCBF_YES_BUTTON | TDCBF_NO_BUTTON,
      .default_button = IDNO,
      .window_title = get_window_title(),
      .main_instruction = main_instr,
      .content = content,
  });
  return result == IDYES;
}

/**
 * @brief Set PSD path if editor has no path yet (within transaction)
 *
 * @param editor Editor instance
 * @param psd_path PSD path to set
 * @param err Error information
 * @return true on success, false on failure
 */
static bool set_psd_path_if_empty(struct ptk_anm2editor *editor, char const *psd_path, struct ov_error *err) {
  if (!psd_path || psd_path[0] == '\0') {
    return true;
  }
  char const *current_psd_path = ptk_anm2_edit_get_psd_path(editor->edit_core);
  if (current_psd_path && current_psd_path[0] != '\0') {
    return true;
  }
  return ptk_anm2_edit_set_psd_path(editor->edit_core, psd_path, err);
}

bool ptk_anm2editor_add_value_items(struct ptk_anm2editor *editor,
                                    char const *psd_path,
                                    char const *group,
                                    char const *const *names,
                                    char const *const *values,
                                    size_t count,
                                    struct ov_error *err) {
  if (!editor || !editor->edit_core) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_invalid_argument);
    return false;
  }

  if (!check_psd_path_mismatch(editor, psd_path)) {
    return true; // User cancelled, not an error
  }

  bool transaction_started = false;
  bool success = false;
  size_t sel_idx = 0;
  uint32_t selector_id = 0;

  // Use transaction to group all operations for single undo
  if (!ptk_anm2_edit_begin_transaction(editor->edit_core, err)) {
    OV_ERROR_ADD_TRACE(err);
    goto cleanup;
  }
  transaction_started = true;

  if (!set_psd_path_if_empty(editor, psd_path, err)) {
    OV_ERROR_ADD_TRACE(err);
    goto cleanup;
  }

  // Add the selector
  if (!ptk_anm2_edit_add_selector(editor->edit_core, group ? group : "", err)) {
    OV_ERROR_ADD_TRACE(err);
    goto cleanup;
  }

  sel_idx = ptk_anm2_edit_selector_count(editor->edit_core) - 1;
  selector_id = ptk_anm2_edit_selector_get_id(editor->edit_core, sel_idx);

  // Add value items to the selector
  for (size_t i = 0; i < count; i++) {
    char const *name = names ? names[i] : "";
    char const *value = values ? values[i] : "";
    if (!ptk_anm2_edit_add_value_item_to_selector(
            editor->edit_core, selector_id, name ? name : "", value ? value : "", err)) {
      OV_ERROR_ADD_TRACE(err);
      goto cleanup;
    }
  }

  success = true;

cleanup:
  if (transaction_started) {
    if (!ptk_anm2_edit_end_transaction(editor->edit_core, success, err)) {
      if (success) {
        OV_ERROR_ADD_TRACE(err);
        success = false;
      }
    }
  }

  return success;
}

bool ptk_anm2editor_add_value_item_to_selected(struct ptk_anm2editor *editor,
                                               char const *psd_path,
                                               char const *group,
                                               char const *name,
                                               char const *value,
                                               struct ov_error *err) {
  if (!editor || !editor->edit_core) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_invalid_argument);
    return false;
  }

  if (!check_psd_path_mismatch(editor, psd_path)) {
    return true; // User cancelled, not an error
  }

  bool transaction_started = false;
  bool success = false;
  uint32_t selector_id = get_selected_selector_id(editor);

  // Use transaction to group all operations for single undo
  if (!ptk_anm2_edit_begin_transaction(editor->edit_core, err)) {
    OV_ERROR_ADD_TRACE(err);
    goto cleanup;
  }
  transaction_started = true;

  if (!set_psd_path_if_empty(editor, psd_path, err)) {
    OV_ERROR_ADD_TRACE(err);
    goto cleanup;
  }

  if (selector_id == 0) {
    // No selector selected, create a new one
    if (!ptk_anm2_edit_add_selector(editor->edit_core, group ? group : "", err)) {
      OV_ERROR_ADD_TRACE(err);
      goto cleanup;
    }
    // Get the newly created selector's ID
    size_t const sel_count = ptk_anm2_edit_selector_count(editor->edit_core);
    if (sel_count > 0) {
      selector_id = ptk_anm2_edit_selector_get_id(editor->edit_core, sel_count - 1);
    }
  }

  // Add value item to the selector
  if (!ptk_anm2_edit_add_value_item_to_selector(
          editor->edit_core, selector_id, name ? name : "", value ? value : "", err)) {
    OV_ERROR_ADD_TRACE(err);
    goto cleanup;
  }

  success = true;

cleanup:
  if (transaction_started) {
    if (!ptk_anm2_edit_end_transaction(editor->edit_core, success, err)) {
      if (success) {
        OV_ERROR_ADD_TRACE(err);
        success = false;
      }
    }
  }

  return success;
}
