#include "anm2editor_treeview.h"

#include "logf.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <commctrl.h>

#include <ovarray.h>
#include <ovmo.h>
#include <ovprintf.h>
#include <ovutf.h>

enum {
  CMD_RENAME = 1,
  CMD_DELETE = 2,
  CMD_REVERSE = 3,
  CMD_ADD_SELECTOR = 4,
};

// Selector userdata encoding:
// - bit 0 = expanded state (1=expanded, 0=collapsed)
static inline bool selector_userdata_is_expanded(uintptr_t userdata) { return (userdata & 1) != 0; }
static inline uintptr_t selector_userdata_encode_expanded(bool expanded) { return expanded ? 1 : 0; }

#define TREEVIEW_SUBCLASS_ID 1

struct anm2editor_treeview {
  HWND window;
  HWND parent;
  int control_id;

  struct anm2editor_treeview_callbacks callbacks;
  struct ptk_anm2_edit *edit; // Edit core for data access (not owned)

  // Drag & drop UI state (Windows-specific)
  HTREEITEM drag_item;
  HIMAGELIST drag_imagelist;
  HTREEITEM insert_mark_target; // Item for insert marker display
  bool insert_after;            // true = after item, false = before item

  // Label editing state
  bool dragging;
  bool adding_new_selector;

  // Selection event suppression (for programmatic selection changes)
  bool suppress_selection_changed;

  // Transaction state for batch updates
  int transaction_depth;

  // Pending single-select on mouse up (for multi-select drag support)
  uint32_t pending_select_id;
  bool pending_select_valid;
};

// Helper to report errors via callback
// If on_error is set, callback takes ownership and must destroy err.
// If on_error is not set, this function logs and destroys err.
static void report_error(struct anm2editor_treeview *tv, struct ov_error *err) {
  if (tv->callbacks.on_error) {
    tv->callbacks.on_error(tv->callbacks.userdata, err);
  } else {
    ptk_logf_error(err, "%1$hs", "%1$hs", gettext("Operation failed."));
    OV_ERROR_DESTROY(err);
  }
}

// Selector: high bit set, lower 31 bits = selector ID
// Item: high bit NOT set, lower 31 bits = item ID

#define TREEVIEW_LPARAM_SELECTOR_BIT 0x80000000

static LPARAM treeview_encode_selector_id(uint32_t selector_id) {
  return (LPARAM)(selector_id | TREEVIEW_LPARAM_SELECTOR_BIT);
}

static LPARAM treeview_encode_item_id(uint32_t item_id) { return (LPARAM)item_id; }

// Decode lParam to get ID and type
// Returns true if selector, false if item
// out_id receives the ID (selector ID or item ID)
static bool treeview_decode_lparam(LPARAM lparam, uint32_t *out_id) {
  if (lparam & TREEVIEW_LPARAM_SELECTOR_BIT) {
    *out_id = (uint32_t)(lparam & 0x7FFFFFFF);
    return true;
  }
  *out_id = (uint32_t)lparam;
  return false;
}

// Find TreeView item by lParam
static HTREEITEM find_treeview_item_by_lparam(HWND treeview, LPARAM target_lparam) {
  HTREEITEM hRoot = (HTREEITEM)(SendMessageW(treeview, TVM_GETNEXTITEM, TVGN_ROOT, 0));
  while (hRoot) {
    TVITEMW tvi = {.mask = TVIF_PARAM, .hItem = hRoot};
    if (TreeView_GetItem(treeview, &tvi) && tvi.lParam == target_lparam) {
      return hRoot;
    }

    // Check children
    HTREEITEM hChild = (HTREEITEM)(SendMessageW(treeview, TVM_GETNEXTITEM, TVGN_CHILD, (LPARAM)hRoot));
    while (hChild) {
      TVITEMW child_tvi = {.mask = TVIF_PARAM, .hItem = hChild};
      if (TreeView_GetItem(treeview, &child_tvi) && child_tvi.lParam == target_lparam) {
        return hChild;
      }
      hChild = (HTREEITEM)(SendMessageW(treeview, TVM_GETNEXTITEM, TVGN_NEXT, (LPARAM)hChild));
    }

    hRoot = (HTREEITEM)(SendMessageW(treeview, TVM_GETNEXTITEM, TVGN_NEXT, (LPARAM)hRoot));
  }
  return NULL;
}

// Sync TreeView expand state with userdata for a specific selector
static void sync_selector_expand_state(struct anm2editor_treeview *tv, uint32_t selector_id) {
  if (!tv || !tv->window || !tv->edit || selector_id == 0) {
    return;
  }
  LPARAM const sel_lparam = treeview_encode_selector_id(selector_id);
  HTREEITEM hSelector = find_treeview_item_by_lparam(tv->window, sel_lparam);
  if (!hSelector) {
    return;
  }
  uintptr_t const userdata = ptk_anm2_edit_selector_get_userdata(tv->edit, selector_id);
  bool const should_expand = selector_userdata_is_expanded(userdata);
  UINT const state = (UINT)SendMessageW(tv->window, TVM_GETITEMSTATE, (WPARAM)hSelector, (LPARAM)TVIS_EXPANDED);
  bool const is_expanded = (state & TVIS_EXPANDED) != 0;
  if (should_expand != is_expanded) {
    TreeView_Expand(tv->window, hSelector, should_expand ? TVE_EXPAND : TVE_COLLAPSE);
  }
}

// Caret state for save/restore operations
struct treeview_caret_state {
  LPARAM lparam; // 0 = no caret saved
};

// Save the current caret position (item with focus rectangle)
static struct treeview_caret_state treeview_save_caret(struct anm2editor_treeview *tv) {
  struct treeview_caret_state state = {0};
  if (!tv || !tv->window) {
    return state;
  }
  HTREEITEM const hCaret = (HTREEITEM)(uintptr_t)SendMessageW(tv->window, TVM_GETNEXTITEM, TVGN_CARET, 0);
  if (hCaret) {
    TVITEMW tvi = {.mask = TVIF_PARAM, .hItem = hCaret};
    if (SendMessageW(tv->window, TVM_GETITEMW, 0, (LPARAM)&tvi)) {
      state.lparam = tvi.lParam;
    }
  }
  return state;
}

// Restore caret position, suppressing selection change notifications
static void treeview_restore_caret(struct anm2editor_treeview *tv, struct treeview_caret_state const *state) {
  if (!tv || !tv->window || !state || state->lparam == 0) {
    return;
  }
  HTREEITEM hItem = find_treeview_item_by_lparam(tv->window, state->lparam);
  if (hItem) {
    tv->suppress_selection_changed = true;
    TreeView_SelectItem(tv->window, hItem);
    tv->suppress_selection_changed = false;
  }
}

// Get lParam from TreeView item, fetching it if not provided in notification
static LPARAM get_treeview_item_lparam(HWND treeview, HTREEITEM hItem, LPARAM provided_lparam) {
  if (provided_lparam != 0) {
    return provided_lparam;
  }
  if (hItem) {
    TVITEMW tvi = {.mask = TVIF_PARAM, .hItem = hItem};
    if (SendMessageW(treeview, TVM_GETITEMW, 0, (LPARAM)&tvi)) {
      return tvi.lParam;
    }
  }
  return 0;
}

// Insert selector by ID, before the element with before_id (0 = insert at end)
static HTREEITEM
treeview_insert_selector_by_id(struct anm2editor_treeview *tv, uint32_t new_sel_id, uint32_t before_id, bool expand) {
  wchar_t group_name[256] = {0};
  ptk_anm2_edit_format_selector_display_name(
      tv->edit, new_sel_id, group_name, sizeof(group_name) / sizeof(group_name[0]));

  LPARAM const sel_lparam = treeview_encode_selector_id(new_sel_id);

  // Find insertion point: insert before the element with before_id
  HTREEITEM hInsertAfter = TVI_LAST;
  if (before_id != 0) {
    LPARAM const before_lparam = treeview_encode_selector_id(before_id);
    HTREEITEM hBefore = find_treeview_item_by_lparam(tv->window, before_lparam);
    if (hBefore) {
      // Need to insert before hBefore, so find the previous sibling
      HTREEITEM hPrev = (HTREEITEM)(SendMessageW(tv->window, TVM_GETNEXTITEM, TVGN_PREVIOUS, (LPARAM)hBefore));
      hInsertAfter = hPrev ? hPrev : TVI_FIRST;
    }
  }

  HTREEITEM hSelector =
      (HTREEITEM)(SendMessageW(tv->window,
                               TVM_INSERTITEMW,
                               0,
                               (LPARAM) & (TVINSERTSTRUCTW){
                                              .hParent = TVI_ROOT,
                                              .hInsertAfter = hInsertAfter,
                                              .item =
                                                  {
                                                      .mask = TVIF_TEXT | TVIF_PARAM | TVIF_IMAGE | TVIF_SELECTEDIMAGE,
                                                      .pszText = group_name,
                                                      .iImage = 0,
                                                      .iSelectedImage = 0,
                                                      .lParam = sel_lparam,
                                                  },
                                          }));

  // Insert all items under this selector
  size_t sel_idx = 0;
  if (!ptk_anm2_edit_find_selector(tv->edit, new_sel_id, &sel_idx)) {
    return hSelector;
  }
  size_t const item_count = ptk_anm2_edit_item_count(tv->edit, new_sel_id);

  for (size_t j = 0; j < item_count; j++) {
    uint32_t const item_id = ptk_anm2_edit_item_get_id(tv->edit, sel_idx, j);

    wchar_t item_name[256] = {0};
    ptk_anm2_edit_format_item_display_name(tv->edit, item_id, item_name, sizeof(item_name) / sizeof(item_name[0]));

    LPARAM const item_lparam = treeview_encode_item_id(item_id);

    SendMessageW(tv->window,
                 TVM_INSERTITEMW,
                 0,
                 (LPARAM) & (TVINSERTSTRUCTW){
                                .hParent = hSelector,
                                .hInsertAfter = TVI_LAST,
                                .item =
                                    {
                                        .mask = TVIF_TEXT | TVIF_PARAM | TVIF_IMAGE | TVIF_SELECTEDIMAGE,
                                        .pszText = item_name,
                                        .iImage = 1,
                                        .iSelectedImage = 1,
                                        .lParam = item_lparam,
                                    },
                            });
  }

  if (expand) {
    TreeView_Expand(tv->window, hSelector, TVE_EXPAND);
  }

  return hSelector;
}

// Insert item by ID, before the element with before_id (0 = insert at end)
static HTREEITEM treeview_insert_item_by_id(struct anm2editor_treeview *tv,
                                            uint32_t parent_sel_id,
                                            uint32_t new_item_id,
                                            uint32_t before_id) {
  LPARAM const sel_lparam = treeview_encode_selector_id(parent_sel_id);
  HTREEITEM hSelector = find_treeview_item_by_lparam(tv->window, sel_lparam);
  if (!hSelector) {
    return NULL;
  }

  wchar_t item_name[256] = {0};
  ptk_anm2_edit_format_item_display_name(tv->edit, new_item_id, item_name, sizeof(item_name) / sizeof(item_name[0]));

  LPARAM const item_lparam = treeview_encode_item_id(new_item_id);

  // Find insertion point: insert before the element with before_id
  HTREEITEM hInsertAfter = TVI_LAST;
  if (before_id != 0) {
    LPARAM const before_lparam = treeview_encode_item_id(before_id);
    HTREEITEM hBefore = NULL;
    // Search only within hSelector's children
    HTREEITEM hChild = (HTREEITEM)(SendMessageW(tv->window, TVM_GETNEXTITEM, TVGN_CHILD, (LPARAM)hSelector));
    while (hChild) {
      TVITEMW tvi = {.mask = TVIF_PARAM, .hItem = hChild};
      if (TreeView_GetItem(tv->window, &tvi) && tvi.lParam == before_lparam) {
        hBefore = hChild;
        break;
      }
      hChild = (HTREEITEM)(SendMessageW(tv->window, TVM_GETNEXTITEM, TVGN_NEXT, (LPARAM)hChild));
    }
    if (hBefore) {
      HTREEITEM hPrev = (HTREEITEM)(SendMessageW(tv->window, TVM_GETNEXTITEM, TVGN_PREVIOUS, (LPARAM)hBefore));
      hInsertAfter = hPrev ? hPrev : TVI_FIRST;
    }
  }

  return (
      HTREEITEM)(SendMessageW(tv->window,
                              TVM_INSERTITEMW,
                              0,
                              (LPARAM) & (TVINSERTSTRUCTW){
                                             .hParent = hSelector,
                                             .hInsertAfter = hInsertAfter,
                                             .item =
                                                 {
                                                     .mask = TVIF_TEXT | TVIF_PARAM | TVIF_IMAGE | TVIF_SELECTEDIMAGE,
                                                     .pszText = item_name,
                                                     .iImage = 1,
                                                     .iSelectedImage = 1,
                                                     .lParam = item_lparam,
                                                 },
                                         }));
}

// Update selector text
static void treeview_update_selector_text(struct anm2editor_treeview *tv, size_t sel_idx) {
  uint32_t const sel_id = ptk_anm2_edit_selector_get_id(tv->edit, sel_idx);
  LPARAM const sel_lparam = treeview_encode_selector_id(sel_id);
  HTREEITEM hItem = find_treeview_item_by_lparam(tv->window, sel_lparam);
  if (!hItem) {
    return;
  }

  wchar_t group_name[256] = {0};
  ptk_anm2_edit_format_selector_display_name(tv->edit, sel_id, group_name, sizeof(group_name) / sizeof(group_name[0]));

  TVITEMW tvi = {
      .mask = TVIF_TEXT,
      .hItem = hItem,
      .pszText = group_name,
  };
  SendMessageW(tv->window, TVM_SETITEMW, 0, (LPARAM)&tvi);
}

// Update item text
static void treeview_update_item_text(struct anm2editor_treeview *tv, size_t sel_idx, size_t item_idx) {
  uint32_t const item_id = ptk_anm2_edit_item_get_id(tv->edit, sel_idx, item_idx);
  LPARAM const item_lparam = treeview_encode_item_id(item_id);
  HTREEITEM hItem = find_treeview_item_by_lparam(tv->window, item_lparam);
  if (!hItem) {
    return;
  }

  wchar_t item_name[256] = {0};
  ptk_anm2_edit_format_item_display_name(tv->edit, item_id, item_name, sizeof(item_name) / sizeof(item_name[0]));

  TVITEMW tvi = {
      .mask = TVIF_TEXT,
      .hItem = hItem,
      .pszText = item_name,
  };
  SendMessageW(tv->window, TVM_SETITEMW, 0, (LPARAM)&tvi);
}

// Handle custom draw for multi-selection highlighting
// Use anm2_selection state as primary, TreeView handles its own focus rect
static LRESULT handle_customdraw(struct anm2editor_treeview *tv, NMTVCUSTOMDRAW *nmcd) {
  switch (nmcd->nmcd.dwDrawStage) {
  case CDDS_PREPAINT:
    return CDRF_NOTIFYITEMDRAW;
  case CDDS_ITEMPREPAINT: {
    // Use lItemlParam directly instead of calling TreeView_GetItem for better performance
    uint32_t id = 0;
    bool const is_selector = treeview_decode_lparam(nmcd->nmcd.lItemlParam, &id);

    // For items: use anm2_selection state
    if (!is_selector && tv->edit) {
      bool const is_selected = ptk_anm2_edit_is_item_selected(tv->edit, id);
      if (is_selected) {
        nmcd->clrTextBk = GetSysColor(COLOR_HIGHLIGHT);
        nmcd->clrText = GetSysColor(COLOR_HIGHLIGHTTEXT);
      } else {
        nmcd->clrTextBk = GetSysColor(COLOR_WINDOW);
        nmcd->clrText = GetSysColor(COLOR_WINDOWTEXT);
      }
      return CDRF_NEWFONT;
    }
    return CDRF_DODEFAULT;
  }
  default:
    return CDRF_DODEFAULT;
  }
}

// These two functions work together to provide Explorer-like selection behavior:
// - Mouse down on unselected item: immediate select
// - Mouse down on already-selected item: remember for potential single-select on mouse up
// - Mouse up without drag: perform the pending single-select
// - Drag cancels the pending single-select (handled in TVN_BEGINDRAG)
//
// handle_explorer_mouse_down() is called from TreeView subclass proc (WM_LBUTTONDOWN)
// handle_explorer_mouse_up() is called from NM_CLICK notification (fires on mouse up)

// Handle mouse down for Explorer-like selection behavior
// Called from TreeView subclass proc to intercept before TreeView's default handling
static void handle_explorer_mouse_down(struct anm2editor_treeview *tv, HWND hwnd, LPARAM lparam) {
  if (!tv || !tv->edit) {
    return;
  }

  // Clear any pending select
  tv->pending_select_valid = false;

  // Hit test to find item under cursor
  POINT pt = {.x = (int)(short)LOWORD(lparam), .y = (int)(short)HIWORD(lparam)};
  TVHITTESTINFO ht = {.pt = pt};
  HTREEITEM hItem = (HTREEITEM)(SendMessageW(hwnd, TVM_HITTEST, 0, (LPARAM)&ht));

  if (!hItem || !(ht.flags & TVHT_ONITEM)) {
    return;
  }

  // Get item info
  TVITEMW tvi = {.mask = TVIF_PARAM, .hItem = hItem};
  if (!SendMessageW(hwnd, TVM_GETITEMW, 0, (LPARAM)&tvi)) {
    return;
  }

  uint32_t item_id = 0;
  bool const is_selector = treeview_decode_lparam(tvi.lParam, &item_id);

  bool const ctrl_pressed = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
  bool const shift_pressed = (GetKeyState(VK_SHIFT) & 0x8000) != 0;

  // Explorer-like behavior:
  // - If clicking on an already selected item (without Ctrl/Shift), don't change selection
  //   This allows dragging multiple selected items
  //   But remember the item for potential single-select on mouse up
  // - If clicking on an unselected item (without Ctrl/Shift), clear and select that item
  // - If Ctrl/Shift is pressed, handle multi-selection as normal
  bool already_selected = !is_selector && ptk_anm2_edit_is_item_selected(tv->edit, item_id);

  if (already_selected && !ctrl_pressed && !shift_pressed) {
    // Remember this item for potential single-select on mouse up
    tv->pending_select_id = item_id;
    tv->pending_select_valid = true;
  } else {
    struct ov_error err = {0};
    if (!ptk_anm2_edit_apply_treeview_selection(tv->edit, item_id, is_selector, ctrl_pressed, shift_pressed, &err)) {
      OV_ERROR_DESTROY(&err);
    }
    // Invalidate to show updated selection
    InvalidateRect(hwnd, NULL, FALSE);
  }
}

// Handle mouse up for Explorer-like selection behavior
// Called from NM_CLICK notification (which fires on mouse up)
static void handle_explorer_mouse_up(struct anm2editor_treeview *tv) {
  if (!tv || !tv->edit || !tv->pending_select_valid) {
    return;
  }

  // Mouse up without drag - perform the single select
  struct ov_error err = {0};
  if (!ptk_anm2_edit_apply_treeview_selection(tv->edit, tv->pending_select_id, false, false, false, &err)) {
    OV_ERROR_DESTROY(&err);
  }
  InvalidateRect(tv->window, NULL, FALSE);
  tv->pending_select_valid = false;
}

// TreeView subclass procedure - intercepts WM_LBUTTONDOWN for immediate selection
static LRESULT CALLBACK
treeview_subclass_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam, UINT_PTR id, DWORD_PTR data) {
  (void)id;
  struct anm2editor_treeview *tv = (struct anm2editor_treeview *)data;

  if (msg == WM_LBUTTONDOWN) {
    handle_explorer_mouse_down(tv, hwnd, lparam);
  }

  return DefSubclassProc(hwnd, msg, wparam, lparam);
}

// Handle begin label edit
static LRESULT handle_begin_label_edit(struct anm2editor_treeview *tv, NMTVDISPINFOW const *nmtv) {
  LPARAM const item_lparam = get_treeview_item_lparam(tv->window, nmtv->item.hItem, nmtv->item.lParam);

  HWND hEdit = (HWND)(SendMessageW(tv->window, TVM_GETEDITCONTROL, 0, 0));
  if (hEdit) {
    uint32_t id = 0;
    bool const is_selector = treeview_decode_lparam(item_lparam, &id);

    wchar_t edit_text[256] = {0};
    ptk_anm2_edit_get_editable_name(tv->edit, id, is_selector, edit_text, sizeof(edit_text) / sizeof(edit_text[0]));
    if (edit_text[0] != L'\0') {
      SetWindowTextW(hEdit, edit_text);
    }
  }

  return FALSE; // Allow editing
}

// Handle end label edit
static LRESULT handle_end_label_edit(struct anm2editor_treeview *tv, NMTVDISPINFOW const *nmtv) {
  bool const adding_new = tv->adding_new_selector;
  tv->adding_new_selector = false;

  // Handle cancelled edit or empty text
  if (!nmtv->item.pszText || nmtv->item.pszText[0] == L'\0') {
    if (adding_new) {
      // Remove the temporary tree item
      SendMessageW(tv->window, TVM_DELETEITEM, 0, (LPARAM)nmtv->item.hItem);
    }
    return FALSE;
  }

  // Convert to UTF-8
  wchar_t const *new_name_w = nmtv->item.pszText;
  size_t const wlen = wcslen(new_name_w);
  size_t const utf8_buf_len = (wlen + 1) * 4;
  char *utf8_buf = NULL;
  LRESULT result = FALSE;

  if (!OV_ARRAY_GROW(&utf8_buf, utf8_buf_len)) {
    goto cleanup;
  }

  {
    size_t const utf8_written = ov_wchar_to_utf8(new_name_w, wlen, utf8_buf, utf8_buf_len, NULL);
    if (utf8_written == 0 && wlen > 0) {
      goto cleanup;
    }
    OV_ARRAY_SET_LENGTH(utf8_buf, utf8_written);

    if (adding_new) {
      // Adding new selector
      if (tv->edit) {
        struct ov_error err = {0};
        if (ptk_anm2_edit_add_selector(tv->edit, utf8_buf, &err)) {
          // Remove temporary item - the actual item will be added via view callback
          SendMessageW(tv->window, TVM_DELETEITEM, 0, (LPARAM)nmtv->item.hItem);
        } else {
          // Failed - remove temporary item and report error
          SendMessageW(tv->window, TVM_DELETEITEM, 0, (LPARAM)nmtv->item.hItem);
          report_error(tv, &err);
        }
      } else {
        SendMessageW(tv->window, TVM_DELETEITEM, 0, (LPARAM)nmtv->item.hItem);
      }
    } else {
      // Editing existing item
      LPARAM const item_lparam = get_treeview_item_lparam(tv->window, nmtv->item.hItem, nmtv->item.lParam);
      uint32_t id = 0;
      bool const is_selector = treeview_decode_lparam(item_lparam, &id);

      if (tv->edit) {
        struct ov_error err = {0};
        bool success = false;
        if (is_selector) {
          success = ptk_anm2_edit_rename_selector(tv->edit, id, utf8_buf, &err);
        } else {
          success = ptk_anm2_edit_rename_item(tv->edit, id, utf8_buf, &err);
        }
        if (success) {
          // Let the TreeView update the text (via view callback that triggers update_differential)
          result = FALSE;
        } else {
          report_error(tv, &err);
        }
      }
    }
  }

cleanup:
  if (utf8_buf) {
    OV_ARRAY_DESTROY(&utf8_buf);
  }
  return result;
}

struct anm2editor_treeview *anm2editor_treeview_create(void *parent_window,
                                                       int control_id,
                                                       struct ptk_anm2_edit *edit,
                                                       struct anm2editor_treeview_callbacks const *callbacks,
                                                       struct ov_error *err) {
  if (!parent_window || !callbacks) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_invalid_argument);
    return NULL;
  }

  struct anm2editor_treeview *tv = NULL;
  bool success = false;

  if (!OV_REALLOC(&tv, 1, sizeof(*tv))) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_out_of_memory);
    goto cleanup;
  }

  *tv = (struct anm2editor_treeview){
      .parent = (HWND)parent_window,
      .control_id = control_id,
      .edit = edit,
      .callbacks = *callbacks,
  };

  // Create TreeView with drag support and label editing
  tv->window = CreateWindowExW(WS_EX_CLIENTEDGE,
                               WC_TREEVIEWW,
                               NULL,
                               WS_CHILD | WS_VISIBLE | WS_TABSTOP | TVS_HASLINES | TVS_LINESATROOT | TVS_HASBUTTONS |
                                   TVS_SHOWSELALWAYS | TVS_EDITLABELS,
                               0,
                               0,
                               100,
                               100,
                               (HWND)parent_window,
                               (HMENU)(intptr_t)control_id,
                               GetModuleHandleW(NULL),
                               NULL);

  if (!tv->window) {
    OV_ERROR_SET_HRESULT(err, HRESULT_FROM_WIN32(GetLastError()));
    goto cleanup;
  }

  // Subclass TreeView for immediate selection on mouse down
  SetWindowSubclass(tv->window, treeview_subclass_proc, TREEVIEW_SUBCLASS_ID, (DWORD_PTR)tv);

  // Enable double buffering to prevent flickering during splitter resize
  SendMessageW(tv->window, TVM_SETEXTENDEDSTYLE, TVS_EX_DOUBLEBUFFER, TVS_EX_DOUBLEBUFFER);

  // Create and set an image list for the TreeView (required for drag image to work)
  {
    HIMAGELIST himl = ImageList_Create(16, 16, ILC_COLOR32 | ILC_MASK, 2, 2);
    if (himl) {
      // Add icons from shell32.dll for folder (selector) and document (item)
      HMODULE hShell32 = GetModuleHandleW(L"shell32.dll");
      if (hShell32) {
        HICON hFolderIcon = LoadIconW(hShell32, MAKEINTRESOURCEW(4)); // Folder icon
        HICON hDocIcon = LoadIconW(hShell32, MAKEINTRESOURCEW(1));    // Document icon
        if (hFolderIcon) {
          ImageList_AddIcon(himl, hFolderIcon);
        }
        if (hDocIcon) {
          ImageList_AddIcon(himl, hDocIcon);
        }
      }
      // Set the image list to TreeView (TVSIL_NORMAL owns the image list and will destroy it)
      SendMessageW(tv->window, TVM_SETIMAGELIST, TVSIL_NORMAL, (LPARAM)himl);
    }
  }

  success = true;

cleanup:
  if (!success) {
    if (tv) {
      if (tv->window) {
        DestroyWindow(tv->window);
      }
      OV_FREE(&tv);
    }
    return NULL;
  }
  return tv;
}

void anm2editor_treeview_destroy(struct anm2editor_treeview **tv) {
  if (!tv || !*tv) {
    return;
  }

  struct anm2editor_treeview *p = *tv;

  if (p->drag_imagelist) {
    ImageList_Destroy(p->drag_imagelist);
    p->drag_imagelist = NULL;
  }

  if (p->window) {
    RemoveWindowSubclass(p->window, treeview_subclass_proc, TREEVIEW_SUBCLASS_ID);
  }

  p->window = NULL;

  OV_FREE(tv);
}

void anm2editor_treeview_set_position(struct anm2editor_treeview *tv, int x, int y, int width, int height) {
  if (!tv || !tv->window) {
    return;
  }
  SetWindowPos(tv->window, NULL, x, y, width, height, SWP_NOZORDER);
}

void anm2editor_treeview_rebuild(struct anm2editor_treeview *tv) {
  if (!tv || !tv->window || !tv->edit) {
    return;
  }

  size_t const selector_count = ptk_anm2_edit_selector_count(tv->edit);

  SendMessageW(tv->window, WM_SETREDRAW, FALSE, 0);
  TreeView_DeleteAllItems(tv->window);

  for (size_t i = 0; i < selector_count; i++) {
    uint32_t const sel_id = ptk_anm2_edit_selector_get_id(tv->edit, i);

    wchar_t group_name[256] = {0};
    ptk_anm2_edit_format_selector_display_name(
        tv->edit, sel_id, group_name, sizeof(group_name) / sizeof(group_name[0]));

    LPARAM const sel_lparam = treeview_encode_selector_id(sel_id);

    HTREEITEM hSelector = (HTREEITEM)(SendMessageW(
        tv->window,
        TVM_INSERTITEMW,
        0,
        (LPARAM) & (TVINSERTSTRUCTW){
                       .hParent = TVI_ROOT,
                       .hInsertAfter = TVI_LAST,
                       .item =
                           {
                               .mask = TVIF_TEXT | TVIF_PARAM | TVIF_IMAGE | TVIF_SELECTEDIMAGE,
                               .pszText = group_name,
                               .iImage = 0,
                               .iSelectedImage = 0,
                               .lParam = sel_lparam,
                           },
                   }));

    size_t const item_count = ptk_anm2_edit_item_count(tv->edit, sel_id);

    for (size_t j = 0; j < item_count; j++) {
      uint32_t const item_id = ptk_anm2_edit_item_get_id(tv->edit, i, j);

      wchar_t item_name[256] = {0};
      ptk_anm2_edit_format_item_display_name(tv->edit, item_id, item_name, sizeof(item_name) / sizeof(item_name[0]));
      LPARAM const item_lparam = treeview_encode_item_id(item_id);

      SendMessageW(tv->window,
                   TVM_INSERTITEMW,
                   0,
                   (LPARAM) & (TVINSERTSTRUCTW){
                                  .hParent = hSelector,
                                  .hInsertAfter = TVI_LAST,
                                  .item =
                                      {
                                          .mask = TVIF_TEXT | TVIF_PARAM | TVIF_IMAGE | TVIF_SELECTEDIMAGE,
                                          .pszText = item_name,
                                          .iImage = 1,
                                          .iSelectedImage = 1,
                                          .lParam = item_lparam,
                                      },
                              });
    }

    // Expand only if userdata indicates expanded state
    uintptr_t const userdata = ptk_anm2_edit_selector_get_userdata(tv->edit, sel_id);
    if (selector_userdata_is_expanded(userdata)) {
      TreeView_Expand(tv->window, hSelector, TVE_EXPAND);
    }
  }

  SendMessageW(tv->window, WM_SETREDRAW, TRUE, 0);
  InvalidateRect(tv->window, NULL, TRUE);
}

void anm2editor_treeview_update_differential(struct anm2editor_treeview *tv,
                                             enum anm2editor_treeview_op_type op_type,
                                             uint32_t id,
                                             uint32_t parent_id,
                                             uint32_t before_id) {
  if (!tv || !tv->window) {
    return;
  }

  switch (op_type) {
  case anm2editor_treeview_op_reset:
    anm2editor_treeview_rebuild(tv);
    break;

  case anm2editor_treeview_op_selector_insert: {
    uintptr_t const userdata = ptk_anm2_edit_selector_get_userdata(tv->edit, id);
    bool const expand = selector_userdata_is_expanded(userdata);
    treeview_insert_selector_by_id(tv, id, before_id, expand);
    sync_selector_expand_state(tv, id);
  } break;

  case anm2editor_treeview_op_selector_remove: {
    LPARAM const sel_lparam = treeview_encode_selector_id(id);
    HTREEITEM hSelector = find_treeview_item_by_lparam(tv->window, sel_lparam);
    if (hSelector) {
      TreeView_DeleteItem(tv->window, hSelector);
    }
  } break;

  case anm2editor_treeview_op_selector_set_name: {
    LPARAM const sel_lparam = treeview_encode_selector_id(id);
    HTREEITEM hSelector = find_treeview_item_by_lparam(tv->window, sel_lparam);
    if (hSelector) {
      size_t sel_idx = 0;
      if (ptk_anm2_edit_find_selector(tv->edit, id, &sel_idx)) {
        treeview_update_selector_text(tv, sel_idx);
      }
    }
  } break;

  case anm2editor_treeview_op_selector_move: {
    LPARAM const sel_lparam = treeview_encode_selector_id(id);
    HTREEITEM hOldSelector = find_treeview_item_by_lparam(tv->window, sel_lparam);
    if (hOldSelector) {
      struct treeview_caret_state const caret_state = treeview_save_caret(tv);

      if (tv->transaction_depth == 0) {
        SendMessageW(tv->window, WM_SETREDRAW, FALSE, 0);
      }

      uintptr_t const userdata = ptk_anm2_edit_selector_get_userdata(tv->edit, id);
      bool const expand = selector_userdata_is_expanded(userdata);

      TreeView_DeleteItem(tv->window, hOldSelector);
      treeview_insert_selector_by_id(tv, id, before_id, expand);
      treeview_restore_caret(tv, &caret_state);
      sync_selector_expand_state(tv, id);

      if (tv->transaction_depth == 0) {
        SendMessageW(tv->window, WM_SETREDRAW, TRUE, 0);
        InvalidateRect(tv->window, NULL, TRUE);
      }
    }
  } break;

  case anm2editor_treeview_op_item_insert:
    treeview_insert_item_by_id(tv, parent_id, id, before_id);
    sync_selector_expand_state(tv, parent_id);
    break;

  case anm2editor_treeview_op_item_remove: {
    LPARAM const item_lparam = treeview_encode_item_id(id);
    HTREEITEM hItem = find_treeview_item_by_lparam(tv->window, item_lparam);
    if (hItem) {
      TreeView_DeleteItem(tv->window, hItem);
    }
  } break;

  case anm2editor_treeview_op_item_set_name: {
    size_t sel_idx = 0;
    size_t item_idx = 0;
    if (ptk_anm2_edit_find_item(tv->edit, id, &sel_idx, &item_idx)) {
      treeview_update_item_text(tv, sel_idx, item_idx);
    }
  } break;

  case anm2editor_treeview_op_item_move: {
    LPARAM const item_lparam = treeview_encode_item_id(id);
    HTREEITEM hOldItem = find_treeview_item_by_lparam(tv->window, item_lparam);
    if (hOldItem) {
      struct treeview_caret_state const caret_state = treeview_save_caret(tv);

      if (tv->transaction_depth == 0) {
        SendMessageW(tv->window, WM_SETREDRAW, FALSE, 0);
      }

      TreeView_DeleteItem(tv->window, hOldItem);
      treeview_insert_item_by_id(tv, parent_id, id, before_id);
      treeview_restore_caret(tv, &caret_state);
      sync_selector_expand_state(tv, parent_id);

      if (tv->transaction_depth == 0) {
        SendMessageW(tv->window, WM_SETREDRAW, TRUE, 0);
        InvalidateRect(tv->window, NULL, TRUE);
      }
    }
  } break;

  case anm2editor_treeview_op_group_begin:
    if (tv->transaction_depth == 0) {
      SendMessageW(tv->window, WM_SETREDRAW, FALSE, 0);
    }
    tv->transaction_depth++;
    break;

  case anm2editor_treeview_op_group_end:
    tv->transaction_depth--;
    if (tv->transaction_depth == 0) {
      SendMessageW(tv->window, WM_SETREDRAW, TRUE, 0);
      InvalidateRect(tv->window, NULL, TRUE);
    }
    break;
  }
}

void anm2editor_treeview_select_by_id(struct anm2editor_treeview *tv, uint32_t id, bool is_selector) {
  if (!tv || !tv->window) {
    return;
  }

  LPARAM target_lparam;
  if (is_selector) {
    target_lparam = treeview_encode_selector_id(id);
  } else {
    target_lparam = treeview_encode_item_id(id);
  }

  HTREEITEM hItem = find_treeview_item_by_lparam(tv->window, target_lparam);
  if (hItem) {
    TreeView_SelectItem(tv->window, hItem);
  }
}

void anm2editor_treeview_select_by_index(struct anm2editor_treeview *tv, size_t sel_idx, size_t item_idx) {
  if (!tv || !tv->edit) {
    return;
  }

  if (item_idx == SIZE_MAX) {
    uint32_t const sel_id = ptk_anm2_edit_selector_get_id(tv->edit, sel_idx);
    if (sel_id != 0) {
      anm2editor_treeview_select_by_id(tv, sel_id, true);
    }
  } else {
    uint32_t const item_id = ptk_anm2_edit_item_get_id(tv->edit, sel_idx, item_idx);
    if (item_id != 0) {
      anm2editor_treeview_select_by_id(tv, item_id, false);
    }
  }
}

static void begin_edit_selected(struct anm2editor_treeview *tv) {
  if (!tv || !tv->window) {
    return;
  }

  HTREEITEM hItem = (HTREEITEM)(SendMessageW(tv->window, TVM_GETNEXTITEM, TVGN_CARET, 0));
  if (hItem) {
    SendMessageW(tv->window, TVM_EDITLABELW, 0, (LPARAM)hItem);
  }
}

static void begin_edit_new_selector(struct anm2editor_treeview *tv) {
  if (!tv || !tv->window) {
    return;
  }

  // Insert a temporary placeholder selector at the end
  HTREEITEM hNewSelector =
      (HTREEITEM)(SendMessageW(tv->window,
                               TVM_INSERTITEMW,
                               0,
                               (LPARAM) & (TVINSERTSTRUCTW){
                                              .hParent = TVI_ROOT,
                                              .hInsertAfter = TVI_LAST,
                                              .item =
                                                  {
                                                      .mask = TVIF_TEXT | TVIF_PARAM | TVIF_IMAGE | TVIF_SELECTEDIMAGE,
                                                      .pszText = L"",
                                                      .iImage = 0,
                                                      .iSelectedImage = 0,
                                                      .lParam = 0, // Temporary item has no valid ID
                                                  },
                                          }));

  if (hNewSelector) {
    tv->adding_new_selector = true;
    TreeView_SelectItem(tv->window, hNewSelector);
    SendMessageW(tv->window, TVM_EDITLABELW, 0, (LPARAM)hNewSelector);
  }
}

void anm2editor_treeview_update_selected_text(struct anm2editor_treeview *tv) {
  if (!tv || !tv->window) {
    return;
  }

  HTREEITEM hItem = (HTREEITEM)(SendMessageW(tv->window, TVM_GETNEXTITEM, TVGN_CARET, 0));
  if (!hItem) {
    return;
  }

  TVITEMW tvi = {.mask = TVIF_PARAM, .hItem = hItem};
  if (!SendMessageW(tv->window, TVM_GETITEMW, 0, (LPARAM)&tvi)) {
    return;
  }

  uint32_t id = 0;
  bool const is_selector = treeview_decode_lparam(tvi.lParam, &id);

  wchar_t new_text[256] = {0};
  if (is_selector) {
    ptk_anm2_edit_format_selector_display_name(tv->edit, id, new_text, sizeof(new_text) / sizeof(new_text[0]));
  } else {
    ptk_anm2_edit_format_item_display_name(tv->edit, id, new_text, sizeof(new_text) / sizeof(new_text[0]));
  }

  tvi.mask = TVIF_TEXT;
  tvi.pszText = new_text;
  SendMessageW(tv->window, TVM_SETITEMW, 0, (LPARAM)&tvi);
}

void anm2editor_treeview_invalidate(struct anm2editor_treeview *tv) {
  if (!tv || !tv->window) {
    return;
  }
  InvalidateRect(tv->window, NULL, TRUE);
}

void anm2editor_treeview_suppress_selection_changed(struct anm2editor_treeview *tv, bool suppress) {
  if (!tv) {
    return;
  }
  tv->suppress_selection_changed = suppress;
}

intptr_t anm2editor_treeview_handle_notify(struct anm2editor_treeview *tv, void *nmhdr_ptr) {
  if (!tv || !nmhdr_ptr) {
    return 0;
  }

  NMHDR const *nmhdr = (NMHDR const *)nmhdr_ptr;

  switch (nmhdr->code) {
  case NM_CUSTOMDRAW:
    return handle_customdraw(tv, (NMTVCUSTOMDRAW *)nmhdr_ptr);

  case TVN_SELCHANGEDW: {
    // Selection is updated in WM_LBUTTONDOWN subclass or keyboard navigation
    // This callback just handles detail list and toolbar updates
    if (tv->suppress_selection_changed) {
      return 0;
    }

    NMTREEVIEWW const *nmtv = (NMTREEVIEWW const *)nmhdr_ptr;
    uint32_t id = 0;
    bool is_selector = true;
    if (nmtv->itemNew.hItem) {
      is_selector = treeview_decode_lparam(nmtv->itemNew.lParam, &id);
    }

    // For keyboard navigation, update selection here
    // TVC_BYMOUSE is handled in WM_LBUTTONDOWN subclass
    // TVC_UNKNOWN (programmatic changes) should not modify selection
    if (nmtv->action == TVC_BYKEYBOARD && tv->edit) {
      bool const ctrl_pressed = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
      bool const shift_pressed = (GetKeyState(VK_SHIFT) & 0x8000) != 0;

      struct ov_error err = {0};
      if (!ptk_anm2_edit_apply_treeview_selection(tv->edit, id, is_selector, ctrl_pressed, shift_pressed, &err)) {
        OV_ERROR_DESTROY(&err);
      }
    }

    struct anm2editor_treeview_item_info info = {.id = id, .is_selector = is_selector};

    bool const ctrl_pressed = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
    bool const shift_pressed = (GetKeyState(VK_SHIFT) & 0x8000) != 0;

    if (tv->callbacks.on_selection_changed) {
      tv->callbacks.on_selection_changed(
          tv->callbacks.userdata, nmtv->itemNew.hItem ? &info : NULL, ctrl_pressed, shift_pressed);
    }
    return 0;
  }

  case TVN_ITEMEXPANDEDW: {
    NMTREEVIEWW const *nmtv = (NMTREEVIEWW const *)nmhdr_ptr;
    if (nmtv->itemNew.hItem && tv->edit) {
      uint32_t id = 0;
      if (treeview_decode_lparam(nmtv->itemNew.lParam, &id)) {
        // It's a selector - save expanded state to userdata
        bool const is_expanded = (nmtv->action == TVE_EXPAND);
        ptk_anm2_edit_selector_set_userdata(tv->edit, id, selector_userdata_encode_expanded(is_expanded));
      }
    }
    return 0;
  }

  case TVN_KEYDOWN: {
    NMTVKEYDOWN const *nmkd = (NMTVKEYDOWN const *)nmhdr_ptr;
    if (nmkd->wVKey == VK_DELETE && tv->edit) {
      struct ov_error err = {0};
      if (!ptk_anm2_edit_delete_selected(tv->edit, &err)) {
        report_error(tv, &err);
      }
      return 1;
    }
    return 0;
  }

  case NM_CLICK: {
    // NM_CLICK fires on mouse up - use this for Explorer-like pending single-select
    // See handle_explorer_mouse_down() for mouse down handling
    handle_explorer_mouse_up(tv);
    return 0;
  }

  case NM_DBLCLK: {
    TVHITTESTINFO ht = {0};
    GetCursorPos(&ht.pt);
    ScreenToClient(tv->window, &ht.pt);
    HTREEITEM hItem = (HTREEITEM)(SendMessageW(tv->window, TVM_HITTEST, 0, (LPARAM)&ht));
    if (!hItem) {
      // Double-click on empty area - add new selector
      begin_edit_new_selector(tv);
    }
    return 0;
  }

  case NM_RCLICK: {
    TVHITTESTINFO ht = {0};
    GetCursorPos(&ht.pt);
    POINT screen_pt = ht.pt;
    ScreenToClient(tv->window, &ht.pt);
    HTREEITEM hItem = (HTREEITEM)(SendMessageW(tv->window, TVM_HITTEST, 0, (LPARAM)&ht));

    struct anm2editor_treeview_item_info info = {0};
    bool is_selector = false;

    if (hItem) {
      // Select the clicked item
      SendMessageW(tv->window, TVM_SELECTITEM, TVGN_CARET, (LPARAM)hItem);

      TVITEMW tvi = {.mask = TVIF_PARAM, .hItem = hItem};
      if (SendMessageW(tv->window, TVM_GETITEMW, 0, (LPARAM)&tvi)) {
        is_selector = treeview_decode_lparam(tvi.lParam, &info.id);
        info.is_selector = is_selector;
      }

      // Show context menu for item/selector
      HMENU hMenu = CreatePopupMenu();
      if (hMenu) {
        wchar_t rename_text[64];
        wchar_t delete_text[64];
        ov_snprintf_wchar(rename_text,
                          sizeof(rename_text) / sizeof(rename_text[0]),
                          L"%1$hs",
                          L"%1$hs",
                          pgettext("anm2editor", "Rename"));
        ov_snprintf_wchar(delete_text,
                          sizeof(delete_text) / sizeof(delete_text[0]),
                          L"%1$hs",
                          L"%1$hs",
                          pgettext("anm2editor", "Delete"));
        AppendMenuW(hMenu, MF_STRING, CMD_RENAME, rename_text);
        AppendMenuW(hMenu, MF_STRING, CMD_DELETE, delete_text);

        if (is_selector) {
          AppendMenuW(hMenu, MF_SEPARATOR, 0, NULL);
          wchar_t reverse_text[64];
          ov_snprintf_wchar(reverse_text,
                            sizeof(reverse_text) / sizeof(reverse_text[0]),
                            L"%1$hs",
                            L"%1$hs",
                            pgettext("anm2editor", "Reverse Items"));
          // Check if selector has items to reverse
          bool const can_rev = tv->edit && ptk_anm2_edit_item_count(tv->edit, info.id) > 0;
          UINT const flags = can_rev ? MF_STRING : (MF_STRING | MF_GRAYED);
          AppendMenuW(hMenu, flags, CMD_REVERSE, reverse_text);
        }

        int const cmd =
            TrackPopupMenu(hMenu, TPM_RETURNCMD | TPM_RIGHTBUTTON, screen_pt.x, screen_pt.y, 0, tv->parent, NULL);
        DestroyMenu(hMenu);

        switch (cmd) {
        case CMD_RENAME:
          begin_edit_selected(tv);
          break;
        case CMD_DELETE:
          if (tv->edit) {
            struct ov_error err = {0};
            if (!ptk_anm2_edit_delete_selected(tv->edit, &err)) {
              report_error(tv, &err);
            }
          }
          break;
        case CMD_REVERSE:
          if (tv->edit) {
            struct ov_error err = {0};
            if (!ptk_anm2_edit_reverse_focus_selector(tv->edit, &err)) {
              report_error(tv, &err);
            }
          }
          break;
        }
      }
    } else {
      // Show context menu for empty area
      HMENU hMenu = CreatePopupMenu();
      if (hMenu) {
        wchar_t add_selector_text[64];
        ov_snprintf_wchar(add_selector_text,
                          sizeof(add_selector_text) / sizeof(add_selector_text[0]),
                          L"%1$hs",
                          L"%1$hs",
                          pgettext("anm2editor", "Add Selector"));
        AppendMenuW(hMenu, MF_STRING, CMD_ADD_SELECTOR, add_selector_text);

        int const cmd =
            TrackPopupMenu(hMenu, TPM_RETURNCMD | TPM_RIGHTBUTTON, screen_pt.x, screen_pt.y, 0, tv->parent, NULL);
        DestroyMenu(hMenu);

        if (cmd == CMD_ADD_SELECTOR) {
          begin_edit_new_selector(tv);
        }
      }
    }
    return TRUE; // Prevent further processing
  }

  case TVN_BEGINLABELEDITW:
    return handle_begin_label_edit(tv, (NMTVDISPINFOW const *)nmhdr_ptr);

  case TVN_ENDLABELEDITW:
    return handle_end_label_edit(tv, (NMTVDISPINFOW const *)nmhdr_ptr);

  case TVN_BEGINDRAGW: {
    NMTREEVIEWW const *nmtv = (NMTREEVIEWW const *)nmhdr_ptr;
    tv->drag_item = nmtv->itemNew.hItem;

    // Cancel any pending single-select since drag is starting
    tv->pending_select_valid = false;

    // Get dragged item info
    uint32_t drag_id = 0;
    bool drag_is_selector = false;
    {
      TVITEMW drag_tvi = {.mask = TVIF_PARAM, .hItem = tv->drag_item};
      if (SendMessageW(tv->window, TVM_GETITEMW, 0, (LPARAM)&drag_tvi)) {
        drag_is_selector = treeview_decode_lparam(drag_tvi.lParam, &drag_id);
      }
    }

    // Explorer-like behavior: if dragging an unselected item, switch selection to it
    if (!drag_is_selector && tv->edit && drag_id != 0 && !ptk_anm2_edit_is_item_selected(tv->edit, drag_id)) {
      struct ov_error err = {0};
      if (!ptk_anm2_edit_apply_treeview_selection(tv->edit, drag_id, false, false, false, &err)) {
        OV_ERROR_DESTROY(&err);
      }
    }

    tv->dragging = true;

    tv->drag_imagelist = (HIMAGELIST)(SendMessageW(tv->window, TVM_CREATEDRAGIMAGE, 0, (LPARAM)tv->drag_item));
    if (tv->drag_imagelist) {
      ImageList_BeginDrag(tv->drag_imagelist, 0, 0, 0);
      POINT pt = nmtv->ptDrag;
      ClientToScreen(tv->window, &pt);
      ImageList_DragEnter(GetDesktopWindow(), pt.x, pt.y);
    }

    SetCapture(tv->parent);
    SetCursor(LoadCursorW(NULL, MAKEINTRESOURCEW(32512))); // IDC_ARROW

    return 0;
  }

  default:
    return 0;
  }
}

void anm2editor_treeview_handle_view_event(struct anm2editor_treeview *tv,
                                           struct ptk_anm2_edit_view_event const *event) {
  if (!tv || !event) {
    return;
  }

  switch (event->op) {
  case ptk_anm2_edit_view_treeview_rebuild:
    anm2editor_treeview_update_differential(tv, anm2editor_treeview_op_reset, 0, 0, 0);
    break;

  case ptk_anm2_edit_view_treeview_insert_selector:
    anm2editor_treeview_update_differential(tv, anm2editor_treeview_op_selector_insert, event->id, 0, event->before_id);
    break;

  case ptk_anm2_edit_view_treeview_remove_selector:
    anm2editor_treeview_update_differential(tv, anm2editor_treeview_op_selector_remove, event->id, 0, 0);
    break;

  case ptk_anm2_edit_view_treeview_update_selector:
    anm2editor_treeview_update_differential(tv, anm2editor_treeview_op_selector_set_name, event->id, 0, 0);
    break;

  case ptk_anm2_edit_view_treeview_move_selector:
    anm2editor_treeview_update_differential(tv, anm2editor_treeview_op_selector_move, event->id, 0, event->before_id);
    break;

  case ptk_anm2_edit_view_treeview_insert_item:
    anm2editor_treeview_update_differential(
        tv, anm2editor_treeview_op_item_insert, event->id, event->parent_id, event->before_id);
    break;

  case ptk_anm2_edit_view_treeview_remove_item:
    anm2editor_treeview_update_differential(tv, anm2editor_treeview_op_item_remove, event->id, event->parent_id, 0);
    break;

  case ptk_anm2_edit_view_treeview_update_item:
    anm2editor_treeview_update_differential(tv, anm2editor_treeview_op_item_set_name, event->id, 0, 0);
    break;

  case ptk_anm2_edit_view_treeview_move_item:
    anm2editor_treeview_update_differential(
        tv, anm2editor_treeview_op_item_move, event->id, event->parent_id, event->before_id);
    break;

  case ptk_anm2_edit_view_treeview_select:
    // Selection changes - read from edit_core, just invalidate for visual update
    anm2editor_treeview_invalidate(tv);
    break;

  case ptk_anm2_edit_view_treeview_set_focus:
    // Focus change - sync focus from edit_core
    anm2editor_treeview_suppress_selection_changed(tv, true);
    if (event->id == 0) {
      anm2editor_treeview_select_by_id(tv, 0, false);
    } else {
      anm2editor_treeview_select_by_id(tv, event->id, event->is_selector);
    }
    anm2editor_treeview_suppress_selection_changed(tv, false);
    break;

  case ptk_anm2_edit_view_treeview_group_begin:
    anm2editor_treeview_update_differential(tv, anm2editor_treeview_op_group_begin, 0, 0, 0);
    break;

  case ptk_anm2_edit_view_treeview_group_end:
    anm2editor_treeview_update_differential(tv, anm2editor_treeview_op_group_end, 0, 0, 0);
    break;

  case ptk_anm2_edit_view_detail_refresh:
  case ptk_anm2_edit_view_detail_insert_param:
  case ptk_anm2_edit_view_detail_remove_param:
  case ptk_anm2_edit_view_detail_update_param:
  case ptk_anm2_edit_view_detail_update_item:
  case ptk_anm2_edit_view_detail_item_selected:
  case ptk_anm2_edit_view_detail_item_deselected:
  case ptk_anm2_edit_view_undo_redo_state_changed:
  case ptk_anm2_edit_view_modified_state_changed:
  case ptk_anm2_edit_view_save_state_changed:
  case ptk_anm2_edit_view_before_undo_redo:
    // Non-treeview events - handled by parent
    break;
  }
}

// Result of drop target calculation
struct drop_target_state {
  HTREEITEM target_item;
  bool insert_after;
  bool use_drop_highlight;
};

// Calculate drop target state for current drag position
static void calculate_drop_target(struct anm2editor_treeview *tv,
                                  HTREEITEM hTarget,
                                  POINT tv_pt,
                                  uint32_t drag_id,
                                  bool drag_is_selector,
                                  uint32_t target_id,
                                  bool target_is_selector,
                                  struct drop_target_state *out_state) {
  out_state->target_item = NULL;
  out_state->insert_after = false;
  out_state->use_drop_highlight = false;

  if (!hTarget || hTarget == tv->drag_item || !tv->edit) {
    return;
  }

  // Determine valid drop and visual feedback
  // - Selector -> Selector: insert marker (reorder)
  // - Item -> Selector: drop highlight (insert as child)
  // - Item -> Item: insert marker (reorder)
  // - Selector -> Item: invalid

  if (drag_is_selector && !target_is_selector) {
    // Selector -> Item: invalid drop target
    return;
  }

  if (!drag_is_selector && target_is_selector) {
    // Item -> Selector: use drop highlight to indicate "insert into"
    size_t sel_count = 0;
    uint32_t const *sel_ids = ptk_anm2_edit_get_selected_item_ids(tv->edit, &sel_count);
    bool would_move = false;
    if (sel_count > 0) {
      would_move = ptk_anm2_edit_would_move_items(tv->edit, sel_ids, sel_count, target_id, true, false);
    } else {
      would_move = ptk_anm2_edit_would_move_items(tv->edit, &drag_id, 1, target_id, true, false);
    }
    if (would_move) {
      out_state->target_item = hTarget;
      out_state->insert_after = true;
      out_state->use_drop_highlight = true;
    }
    return;
  }

  // Selector -> Selector or Item -> Item: use insert marker
  union {
    HTREEITEM hitem;
    RECT rect;
  } item_rect_u;
  item_rect_u.hitem = hTarget;
  if (!SendMessageW(tv->window, TVM_GETITEMRECT, TRUE, (LPARAM)&item_rect_u.rect)) {
    return;
  }

  int item_mid_y = (item_rect_u.rect.top + item_rect_u.rect.bottom) / 2;
  bool insert_after = (tv_pt.y >= item_mid_y);

  bool would_move = false;
  if (drag_is_selector) {
    would_move = ptk_anm2_edit_would_move_selector(tv->edit, drag_id, target_id, insert_after);
  } else {
    size_t sel_count = 0;
    uint32_t const *sel_ids = ptk_anm2_edit_get_selected_item_ids(tv->edit, &sel_count);
    if (sel_count > 0) {
      would_move = ptk_anm2_edit_would_move_items(tv->edit, sel_ids, sel_count, target_id, false, insert_after);
    } else {
      would_move = ptk_anm2_edit_would_move_items(tv->edit, &drag_id, 1, target_id, false, insert_after);
    }
  }

  if (would_move) {
    out_state->target_item = hTarget;
    out_state->insert_after = insert_after;
  }
}

// Update visual indicators (insert marker or drop highlight)
static void update_drop_indicators(struct anm2editor_treeview *tv,
                                   struct drop_target_state const *new_state,
                                   bool drag_is_selector,
                                   bool target_is_selector) {
  // Determine if state changed (reduces flicker)
  bool const state_changed =
      (new_state->target_item != tv->insert_mark_target) || (new_state->insert_after != tv->insert_after) ||
      (new_state->use_drop_highlight != (tv->insert_mark_target != NULL && !drag_is_selector && target_is_selector));

  if (!state_changed) {
    return;
  }

  // Clear previous indicators
  SendMessageW(tv->window, TVM_SETINSERTMARK, 0, 0);
  SendMessageW(tv->window, TVM_SELECTITEM, TVGN_DROPHILITE, 0);

  // Set new indicators
  if (new_state->target_item) {
    if (new_state->use_drop_highlight) {
      SendMessageW(tv->window, TVM_SELECTITEM, TVGN_DROPHILITE, (LPARAM)new_state->target_item);
    } else {
      SendMessageW(tv->window, TVM_SETINSERTMARK, (WPARAM)new_state->insert_after, (LPARAM)new_state->target_item);
    }
  }

  tv->insert_mark_target = new_state->target_item;
  tv->insert_after = new_state->insert_after;
}

// Clear all drag state and visual indicators
static void clear_drag_state(struct anm2editor_treeview *tv, bool release_capture) {
  tv->dragging = false;

  if (tv->drag_imagelist) {
    ImageList_DragLeave(GetDesktopWindow());
    ImageList_EndDrag();
    ImageList_Destroy(tv->drag_imagelist);
    tv->drag_imagelist = NULL;
  }

  if (release_capture) {
    ReleaseCapture();
  }

  SendMessageW(tv->window, TVM_SETINSERTMARK, 0, 0);
  SendMessageW(tv->window, TVM_SELECTITEM, TVGN_DROPHILITE, 0);

  tv->drag_item = NULL;
  tv->insert_mark_target = NULL;
  tv->insert_after = false;
}

void anm2editor_treeview_handle_mouse_move(struct anm2editor_treeview *tv, int x, int y) {
  if (!tv || !tv->dragging) {
    return;
  }

  // Convert parent window client coordinates to screen coordinates
  POINT screen_pt = {.x = x, .y = y};
  ClientToScreen(tv->parent, &screen_pt);

  if (tv->drag_imagelist) {
    ImageList_DragMove(screen_pt.x, screen_pt.y);
  }

  // Convert to TreeView client coordinates
  POINT tv_pt = screen_pt;
  ScreenToClient(tv->window, &tv_pt);

  // Hit test to find item under cursor
  TVHITTESTINFO ht = {.pt = tv_pt};
  HTREEITEM hTarget = (HTREEITEM)(SendMessageW(tv->window, TVM_HITTEST, 0, (LPARAM)&ht));

  if (tv->drag_imagelist) {
    ImageList_DragShowNolock(FALSE);
  }

  // Get dragged item info
  bool drag_is_selector = false;
  uint32_t drag_id = 0;
  if (tv->drag_item) {
    TVITEMW drag_tvi = {.mask = TVIF_PARAM, .hItem = tv->drag_item};
    if (SendMessageW(tv->window, TVM_GETITEMW, 0, (LPARAM)&drag_tvi)) {
      drag_is_selector = treeview_decode_lparam(drag_tvi.lParam, &drag_id);
    }
  }

  // Get target item info
  bool target_is_selector = false;
  uint32_t target_id = 0;
  if (hTarget && hTarget != tv->drag_item) {
    TVITEMW target_tvi = {.mask = TVIF_PARAM, .hItem = hTarget};
    if (SendMessageW(tv->window, TVM_GETITEMW, 0, (LPARAM)&target_tvi)) {
      target_is_selector = treeview_decode_lparam(target_tvi.lParam, &target_id);
    }
  }

  // Calculate and update drop target indicators
  struct drop_target_state new_state = {0};
  calculate_drop_target(tv, hTarget, tv_pt, drag_id, drag_is_selector, target_id, target_is_selector, &new_state);
  update_drop_indicators(tv, &new_state, drag_is_selector, target_is_selector);

  if (tv->drag_imagelist) {
    ImageList_DragShowNolock(TRUE);
  }
}

void anm2editor_treeview_handle_lbutton_up(struct anm2editor_treeview *tv) {
  if (!tv || !tv->dragging) {
    return;
  }

  // Get target from saved insert marker position before clearing
  HTREEITEM hTarget = tv->insert_mark_target;
  bool const insert_after = tv->insert_after;
  HTREEITEM hDragItem = tv->drag_item;

  clear_drag_state(tv, true);

  // Get dragged item info
  struct anm2editor_treeview_item_info drag_info = {0};
  if (hDragItem) {
    TVITEMW drag_tvi = {.mask = TVIF_PARAM, .hItem = hDragItem};
    if (SendMessageW(tv->window, TVM_GETITEMW, 0, (LPARAM)&drag_tvi)) {
      drag_info.is_selector = treeview_decode_lparam(drag_tvi.lParam, &drag_info.id);
    }
  }

  // Get drop target info
  struct anm2editor_treeview_item_info drop_info = {0};
  bool has_drop_target = false;

  if (hTarget && hTarget != hDragItem) {
    TVITEMW tvi = {.mask = TVIF_PARAM, .hItem = hTarget};
    if (SendMessageW(tv->window, TVM_GETITEMW, 0, (LPARAM)&tvi)) {
      drop_info.is_selector = treeview_decode_lparam(tvi.lParam, &drop_info.id);
      has_drop_target = true;
    }
  }

  // Perform move operation
  if (has_drop_target && tv->edit) {
    struct ov_error err = {0};
    bool success = false;

    if (drag_info.is_selector && drop_info.is_selector) {
      // Selector to selector - move group
      success = ptk_anm2_edit_move_selector(tv->edit, drag_info.id, drop_info.id, insert_after, &err);
    } else if (!drag_info.is_selector) {
      // Item(s) to target - use selection state set during TVN_BEGINDRAG
      size_t sel_count = 0;
      uint32_t const *sel_ids = ptk_anm2_edit_get_selected_item_ids(tv->edit, &sel_count);
      if (sel_count > 0) {
        success = ptk_anm2_edit_move_items(
            tv->edit, sel_ids, sel_count, drop_info.id, drop_info.is_selector, insert_after, &err);
      } else {
        // Fallback: single item drag
        success = ptk_anm2_edit_move_items(
            tv->edit, &drag_info.id, 1, drop_info.id, drop_info.is_selector, insert_after, &err);
      }
    }

    if (!success) {
      report_error(tv, &err);
    }
  }
}

void anm2editor_treeview_cancel_drag(struct anm2editor_treeview *tv) {
  if (!tv || !tv->dragging) {
    return;
  }
  clear_drag_state(tv, true);
}
