#include "anm2editor.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <commctrl.h>
#include <objidl.h>

#include <gdiplus/gdiplus.h>

#include <ovarray.h>
#include <ovmo.h>
#include <ovprintf.h>
#include <ovutf.h>

#include <ovl/dialog.h>
#include <ovl/file.h>
#include <ovl/os.h>
#include <ovl/path.h>

#include <aviutl2_plugin2.h>

#include "alias.h"
#include "anm2.h"
#include "anm2_script_picker.h"
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

// Toolbar icon resource IDs (must match anm2editor.rc)
enum {
  IDB_TOOLBAR_IMPORT = 101,
  IDB_TOOLBAR_NEW = 102,
  IDB_TOOLBAR_OPEN = 103,
  IDB_TOOLBAR_SAVE = 104,
  IDB_TOOLBAR_SAVEAS = 105,
  IDB_TOOLBAR_UNDO = 106,
  IDB_TOOLBAR_REDO = 107,
  IDB_TOOLBAR_CONVERT = 108,
};

#define ID_FILE_NEW 40001
#define ID_FILE_OPEN 40002
#define ID_FILE_SAVE 40003
#define ID_FILE_SAVEAS 40004

#define ID_EDIT_MOVE_UP 40101
#define ID_EDIT_MOVE_DOWN 40102
#define ID_EDIT_DELETE 40103
#define ID_EDIT_REVERSE 40104
#define ID_EDIT_ADD_SELECTOR 40105
#define ID_EDIT_RENAME 40106
#define ID_EDIT_DELETE_PARAM 40108
#define ID_EDIT_IMPORT_SCRIPTS 40109
#define ID_EDIT_UNDO 40110
#define ID_EDIT_REDO 40111
#define ID_EDIT_CONVERT_ANM 40112

// Custom window messages
#define WM_ANM2EDITOR_UPDATE_TITLE (WM_APP + 100)

// Context menu IDs
#define ID_DETAILLIST_DELETE 50001
#define ID_TREEVIEW_RENAME 50101
#define ID_TREEVIEW_DELETE 50102
#define ID_TREEVIEW_ADD_SELECTOR 50103
#define ID_TREEVIEW_REVERSE 50104

static wchar_t const anm2editor_window_class_name[] = L"PSDToolKitAnm2Editor";

// Toolbar icon indices in ImageList
enum {
  ICON_NEW,
  ICON_OPEN,
  ICON_SAVE,
  ICON_SAVEAS,
  ICON_UNDO,
  ICON_REDO,
  ICON_IMPORT,
  ICON_CONVERT,
  ICON_COUNT,
};

enum {
  ICON_SIZE = 24,
};

// Mapping from icon index to resource ID
static int const icon_resources[ICON_COUNT] = {
    [ICON_NEW] = IDB_TOOLBAR_NEW,
    [ICON_OPEN] = IDB_TOOLBAR_OPEN,
    [ICON_SAVE] = IDB_TOOLBAR_SAVE,
    [ICON_SAVEAS] = IDB_TOOLBAR_SAVEAS,
    [ICON_UNDO] = IDB_TOOLBAR_UNDO,
    [ICON_REDO] = IDB_TOOLBAR_REDO,
    [ICON_IMPORT] = IDB_TOOLBAR_IMPORT,
    [ICON_CONVERT] = IDB_TOOLBAR_CONVERT,
};

// Load a single PNG resource and add to both normal and disabled ImageLists.
// GDI+ must already be initialized.
// Returns true on success.
static bool
load_single_icon(HINSTANCE hinst, int resource_id, int target_size, HIMAGELIST normal_list, HIMAGELIST disabled_list) {
  bool success = false;
  GpBitmap *src_bitmap = NULL;
  GpBitmap *scaled_bitmap = NULL;
  GpBitmap *disabled_bitmap = NULL;
  GpGraphics *graphics = NULL;
  GpImageAttributes *image_attr = NULL;
  IStream *stream = NULL;
  HGLOBAL hmem = NULL;
  void *mem_ptr = NULL;
  HRSRC hres = NULL;
  HGLOBAL hres_data = NULL;
  void const *res_ptr = NULL;
  DWORD res_size = 0;
  HBITMAP hbmp_normal = NULL;
  HBITMAP hbmp_disabled = NULL;
  // Grayscale matrix with 25% alpha
  // clang-format off
  ColorMatrix gray_matrix = {{
      {0.299f, 0.299f, 0.299f, 0.0f, 0.0f},
      {0.587f, 0.587f, 0.587f, 0.0f, 0.0f},
      {0.114f, 0.114f, 0.114f, 0.0f, 0.0f},
      {0.0f,   0.0f,   0.0f,   0.25f, 0.0f},
      {0.0f,   0.0f,   0.0f,   0.0f, 1.0f},
  }};
  // clang-format on

  // Find and load PNG resource
  hres = FindResourceW(hinst, MAKEINTRESOURCEW(resource_id), L"PNG");
  if (!hres) {
    goto cleanup;
  }

  res_size = SizeofResource(hinst, hres);
  hres_data = LoadResource(hinst, hres);
  if (!hres_data) {
    goto cleanup;
  }

  res_ptr = LockResource(hres_data);
  if (!res_ptr) {
    goto cleanup;
  }

  // Create IStream from resource data
  hmem = GlobalAlloc(GMEM_MOVEABLE, res_size);
  if (!hmem) {
    goto cleanup;
  }

  mem_ptr = GlobalLock(hmem);
  if (!mem_ptr) {
    goto cleanup;
  }
  memcpy(mem_ptr, res_ptr, res_size);
  GlobalUnlock(hmem);
  mem_ptr = NULL;

  if (FAILED(CreateStreamOnHGlobal(hmem, TRUE, &stream))) {
    goto cleanup;
  }
  hmem = NULL; // Stream now owns hmem

  // Create GDI+ bitmap from stream
  if (GdipCreateBitmapFromStream(stream, &src_bitmap) != Ok) {
    goto cleanup;
  }

  // Create scaled bitmap for normal icon
  if (GdipCreateBitmapFromScan0(target_size, target_size, 0, PixelFormat32bppPARGB, NULL, &scaled_bitmap) != Ok) {
    goto cleanup;
  }

  if (GdipGetImageGraphicsContext((GpImage *)scaled_bitmap, &graphics) != Ok) {
    goto cleanup;
  }

  GdipSetInterpolationMode(graphics, InterpolationModeHighQualityBicubic);
  GdipSetPixelOffsetMode(graphics, PixelOffsetModeHighSpeed);
  GdipDrawImageRectI(graphics, (GpImage *)src_bitmap, 0, 0, target_size, target_size);

  GdipDeleteGraphics(graphics);
  graphics = NULL;

  // Create HBITMAP for normal icon
  if (GdipCreateHBITMAPFromBitmap(scaled_bitmap, &hbmp_normal, 0x00000000) != Ok) {
    goto cleanup;
  }

  // Create disabled bitmap with grayscale + reduced alpha using ColorMatrix
  if (GdipCreateBitmapFromScan0(target_size, target_size, 0, PixelFormat32bppPARGB, NULL, &disabled_bitmap) != Ok) {
    goto cleanup;
  }

  if (GdipGetImageGraphicsContext((GpImage *)disabled_bitmap, &graphics) != Ok) {
    goto cleanup;
  }

  // Create ImageAttributes with grayscale + alpha reduction ColorMatrix
  if (GdipCreateImageAttributes(&image_attr) != Ok) {
    goto cleanup;
  }

  if (GdipSetImageAttributesColorMatrix(
          image_attr, ColorAdjustTypeDefault, TRUE, &gray_matrix, NULL, ColorMatrixFlagsDefault) != Ok) {
    goto cleanup;
  }

  // Draw scaled bitmap with grayscale effect (no interpolation needed, same size)
  GdipDrawImageRectRectI(graphics,
                         (GpImage *)scaled_bitmap,
                         0,
                         0,
                         target_size,
                         target_size,
                         0,
                         0,
                         target_size,
                         target_size,
                         UnitPixel,
                         image_attr,
                         NULL,
                         NULL);

  // Create HBITMAP for disabled icon
  if (GdipCreateHBITMAPFromBitmap(disabled_bitmap, &hbmp_disabled, 0x00000000) != Ok) {
    goto cleanup;
  }

  // Add to ImageLists
  ImageList_Add(normal_list, hbmp_normal, NULL);
  ImageList_Add(disabled_list, hbmp_disabled, NULL);

  success = true;

cleanup:
  if (hbmp_disabled) {
    DeleteObject(hbmp_disabled);
  }
  if (hbmp_normal) {
    DeleteObject(hbmp_normal);
  }
  if (image_attr) {
    GdipDisposeImageAttributes(image_attr);
  }
  if (graphics) {
    GdipDeleteGraphics(graphics);
  }
  if (disabled_bitmap) {
    GdipDisposeImage((GpImage *)disabled_bitmap);
  }
  if (scaled_bitmap) {
    GdipDisposeImage((GpImage *)scaled_bitmap);
  }
  if (src_bitmap) {
    GdipDisposeImage((GpImage *)src_bitmap);
  }
  if (stream) {
    stream->lpVtbl->Release(stream);
  }
  if (hmem) {
    GlobalFree(hmem);
  }
  return success;
}

// Load all toolbar icons into ImageLists.
// Returns true on success.
static bool load_toolbar_icons(HINSTANCE hinst, HIMAGELIST normal_list, HIMAGELIST disabled_list) {
  ULONG_PTR gdiplus_token = 0;
  GdiplusStartupInput startup_input = {.GdiplusVersion = 1};
  bool success = false;

  // Initialize GDI+ once for all icons
  if (GdiplusStartup(&gdiplus_token, &startup_input, NULL) != Ok) {
    return false;
  }

  // Load each icon
  for (int i = 0; i < ICON_COUNT; i++) {
    if (!load_single_icon(hinst, icon_resources[i], ICON_SIZE, normal_list, disabled_list)) {
      goto cleanup;
    }
  }

  success = true;

cleanup:
  if (gdiplus_token) {
    GdiplusShutdown(gdiplus_token);
  }
  return success;
}

// =============================================================================
// TreeView lParam encoding/decoding (ID-based)
// =============================================================================
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

// =============================================================================
// ListView (detail list) lParam encoding/decoding
// =============================================================================
// Normal parameter row: param index (0, 1, 2, ...)
// Placeholder row for adding new parameter: DETAILLIST_LPARAM_PLACEHOLDER
// Special rows (PSD path, Layer Selection label): DETAILLIST_LPARAM_NOT_EDITABLE

#define DETAILLIST_LPARAM_PLACEHOLDER ((LPARAM) - 1)
#define DETAILLIST_LPARAM_NOT_EDITABLE ((LPARAM) - 2)
#define DETAILLIST_LPARAM_LABEL ((LPARAM) - 3)
#define DETAILLIST_LPARAM_PSD_PATH ((LPARAM) - 4)
#define DETAILLIST_LPARAM_EXCLUSIVE_SUPPORT_DEFAULT ((LPARAM) - 5)
#define DETAILLIST_LPARAM_INFORMATION ((LPARAM) - 6)

static bool detaillist_is_placeholder(LPARAM lparam) { return lparam == DETAILLIST_LPARAM_PLACEHOLDER; }

static bool detaillist_is_label_row(LPARAM lparam) { return lparam == DETAILLIST_LPARAM_LABEL; }

static bool detaillist_is_psd_path_row(LPARAM lparam) { return lparam == DETAILLIST_LPARAM_PSD_PATH; }

static bool detaillist_is_exclusive_support_default_row(LPARAM lparam) {
  return lparam == DETAILLIST_LPARAM_EXCLUSIVE_SUPPORT_DEFAULT;
}

static bool detaillist_is_information_row(LPARAM lparam) { return lparam == DETAILLIST_LPARAM_INFORMATION; }

static bool detaillist_is_editable_param(LPARAM lparam) {
  return lparam != DETAILLIST_LPARAM_PLACEHOLDER && lparam != DETAILLIST_LPARAM_NOT_EDITABLE &&
         lparam != DETAILLIST_LPARAM_LABEL && lparam != DETAILLIST_LPARAM_PSD_PATH &&
         lparam != DETAILLIST_LPARAM_EXCLUSIVE_SUPPORT_DEFAULT && lparam != DETAILLIST_LPARAM_INFORMATION;
}

static bool detaillist_is_editable(LPARAM lparam) {
  return lparam != DETAILLIST_LPARAM_PLACEHOLDER && lparam != DETAILLIST_LPARAM_NOT_EDITABLE;
}

// Check if lParam represents an item ID in multi-selection mode
// Item IDs are positive 32-bit values, special constants are negative
static bool detaillist_is_multisel_item(LPARAM lparam) { return lparam > 0 && lparam <= UINT32_MAX; }

static size_t detaillist_get_param_index(LPARAM lparam) { return (size_t)lparam; }

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

static int const splitter_width = 4; // Width of the splitter bar in pixels

struct ptk_anm2editor {
  HWND window;
  HWND treeview;
  HWND toolbar;
  HWND detaillist; // ListView for showing details of selected item
  HIMAGELIST toolbar_imagelist;
  HIMAGELIST toolbar_disabled_imagelist;
  HIMAGELIST drag_imagelist;
  ATOM window_class;

  struct ptk_anm2 *doc;             // anm2 document (replaces metadata)
  struct aviutl2_edit_handle *edit; // Edit handle for accessing AviUtl2 edit section
  wchar_t *file_path;               // ovarray, current file path (NULL for new document)
  bool modified;

  // Drag & drop state for TreeView items
  bool dragging;
  HTREEITEM drag_item;

  // Splitter state
  int splitter_pos; // X position of splitter (left edge)
  bool splitter_dragging;

  // Inline edit state for detail list
  HWND edit_control;    // Edit control for inline editing
  int edit_row;         // Row being edited (-1 if not editing)
  int edit_column;      // Column being edited (0=Name, 1=Value)
  LPARAM edit_lparam;   // lParam of the row being edited
  WNDPROC edit_oldproc; // Original window procedure for edit control
  bool edit_committing; // Reentrancy guard for commit/cancel
  bool edit_adding_new; // True if editing a new parameter (not yet added to doc)

  // TreeView edit state
  bool adding_new_selector; // True if editing a new selector name (not yet added to doc)

  // Focus tracking for UNDO/REDO
  bool focus_valid;      // True if focus tracking is active
  size_t focus_sel_idx;  // Tracked selector index
  size_t focus_item_idx; // Tracked item index

  // Transaction state for batch TreeView updates
  int transaction_depth; // Nesting depth of group_begin/group_end

  // Multi-selection state for items
  // Stores item IDs that are currently selected
  uint32_t *selected_item_ids; // ovarray of item IDs
  uint32_t anchor_item_id;     // Anchor item ID for Shift+click range selection (0 = none)
};

// Clear all multi-selections
static void multisel_clear(struct ptk_anm2editor *editor) {
  if (editor->selected_item_ids) {
    OV_ARRAY_SET_LENGTH(editor->selected_item_ids, 0);
  }
}

// Check if an item ID is in the multi-selection
static bool multisel_contains(struct ptk_anm2editor const *editor, uint32_t item_id) {
  size_t const len = OV_ARRAY_LENGTH(editor->selected_item_ids);
  for (size_t i = 0; i < len; i++) {
    if (editor->selected_item_ids[i] == item_id) {
      return true;
    }
  }
  return false;
}

// Add an item ID to the multi-selection (if not already present)
static bool multisel_add(struct ptk_anm2editor *editor, uint32_t item_id, struct ov_error *const err) {
  if (multisel_contains(editor, item_id)) {
    return true;
  }
  if (!OV_ARRAY_PUSH(&editor->selected_item_ids, item_id)) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_out_of_memory);
    return false;
  }
  return true;
}

// Remove an item ID from the multi-selection
static void multisel_remove(struct ptk_anm2editor *editor, uint32_t item_id) {
  size_t const len = OV_ARRAY_LENGTH(editor->selected_item_ids);
  for (size_t i = 0; i < len; i++) {
    if (editor->selected_item_ids[i] == item_id) {
      // Move last item to this position and shrink
      editor->selected_item_ids[i] = editor->selected_item_ids[len - 1];
      OV_ARRAY_SET_LENGTH(editor->selected_item_ids, len - 1);
      return;
    }
  }
}

// Toggle an item ID in the multi-selection
static bool multisel_toggle(struct ptk_anm2editor *editor, uint32_t item_id, struct ov_error *const err) {
  if (multisel_contains(editor, item_id)) {
    multisel_remove(editor, item_id);
    return true;
  }
  if (!multisel_add(editor, item_id, err)) {
    OV_ERROR_ADD_TRACE(err);
    return false;
  }
  return true;
}

// Get the number of selected items
static size_t multisel_count(struct ptk_anm2editor const *editor) { return OV_ARRAY_LENGTH(editor->selected_item_ids); }

// Get the selected item IDs array
static uint32_t const *multisel_get_ids(struct ptk_anm2editor const *editor) { return editor->selected_item_ids; }

// Remove any selected item IDs that no longer exist in the document
static void multisel_cleanup_invalid(struct ptk_anm2editor *editor) {
  if (!editor->doc || !editor->selected_item_ids) {
    return;
  }
  size_t i = 0;
  while (i < OV_ARRAY_LENGTH(editor->selected_item_ids)) {
    uint32_t const id = editor->selected_item_ids[i];
    size_t sel_idx = 0, item_idx = 0;
    if (!ptk_anm2_find_item_by_id(editor->doc, id, &sel_idx, &item_idx)) {
      // Item no longer exists - remove from selection
      size_t const len = OV_ARRAY_LENGTH(editor->selected_item_ids);
      editor->selected_item_ids[i] = editor->selected_item_ids[len - 1];
      OV_ARRAY_SET_LENGTH(editor->selected_item_ids, len - 1);
      // Don't increment i - check the swapped item
    } else {
      i++;
    }
  }
}

// Add a single item in range to multi-selection (helper for range loop)
static bool
multisel_add_range_item(struct ptk_anm2editor *editor, size_t sel_idx, size_t item_idx, struct ov_error *const err) {
  uint32_t const id = ptk_anm2_item_get_id(editor->doc, sel_idx, item_idx);
  if (!multisel_add(editor, id, err)) {
    OV_ERROR_ADD_TRACE(err);
    return false;
  }
  return true;
}

// Add items in document order between two item IDs to multi-selection
static bool
multisel_add_range_by_id(struct ptk_anm2editor *editor, uint32_t from_id, uint32_t to_id, struct ov_error *const err) {
  if (from_id == 0 || to_id == 0 || !editor->doc) {
    return true;
  }

  // Find positions of both items
  size_t from_sel = 0, from_item = 0;
  size_t to_sel = 0, to_item = 0;
  if (!ptk_anm2_find_item_by_id(editor->doc, from_id, &from_sel, &from_item)) {
    return true;
  }
  if (!ptk_anm2_find_item_by_id(editor->doc, to_id, &to_sel, &to_item)) {
    return true;
  }

  // Ensure from is before to in document order
  if (from_sel > to_sel || (from_sel == to_sel && from_item > to_item)) {
    size_t tmp_sel = from_sel;
    size_t tmp_item = from_item;
    from_sel = to_sel;
    from_item = to_item;
    to_sel = tmp_sel;
    to_item = tmp_item;
  }

  bool success = false;

  // Add all items in range
  size_t const sel_count = ptk_anm2_selector_count(editor->doc);
  for (size_t sel_idx = from_sel; sel_idx <= to_sel && sel_idx < sel_count; sel_idx++) {
    size_t const item_count = ptk_anm2_item_count(editor->doc, sel_idx);
    size_t start_item = (sel_idx == from_sel) ? from_item : 0;
    size_t end_item = (sel_idx == to_sel) ? to_item : item_count - 1;

    for (size_t item_idx = start_item; item_idx <= end_item && item_idx < item_count; item_idx++) {
      if (!multisel_add_range_item(editor, sel_idx, item_idx, err)) {
        OV_ERROR_ADD_TRACE(err);
        goto cleanup;
      }
    }
  }

  success = true;

cleanup:
  return success;
}

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
    ov_snprintf_char2wchar(buf, sizeof(buf) / sizeof(buf[0]), "%1$hs", "%1$hs", gettext("PSDToolKit anm2 Editor"));
  }
  return buf;
}

static void show_error_dialog(struct ptk_anm2editor *editor, struct ov_error *const err) {
  wchar_t msg[256];
  ov_snprintf_char2wchar(
      msg, sizeof(msg) / sizeof(msg[0]), "%1$hs", "%1$hs", pgettext("anm2editor", "An error occurred."));
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
  ov_snprintf_wchar(title,
                    sizeof(title) / sizeof(title[0]),
                    L"%1$ls%2$hs%3$hs",
                    L"%1$ls%2$hs%3$hs - %4$ls",
                    filename ? filename : L"",
                    filename ? "" : pgettext("anm2editor", "Unsaved"),
                    editor->modified ? "*" : "",
                    get_window_title());
  // The window is changed to WS_POPUP and registered as WS_CHILD in AviUtl ExEdit2, so this window title is not
  // visible. Although it is currently useless, the title is set just in case.
  SetWindowTextW(editor->window, title);
}

// Helper to format selector display name
static void format_selector_display_name(struct ptk_anm2 const *doc, size_t sel_idx, wchar_t *out, size_t out_len) {
  char const *group = ptk_anm2_selector_get_group(doc, sel_idx);
  if (!group || group[0] == '\0') {
    group = pgettext("anm2editor", "(Unnamed Selector)");
  }
  ov_snprintf_char2wchar(out, out_len, "%hs", "%hs", group);
}

// Helper to format item display name with type prefix
static void
format_item_display_name(struct ptk_anm2 const *doc, size_t sel_idx, size_t item_idx, wchar_t *out, size_t out_len) {
  char const *name = ptk_anm2_item_get_name(doc, sel_idx, item_idx);
  if (!name || name[0] == '\0') {
    name = pgettext("anm2editor", "(Unnamed Item)");
  }
  if (ptk_anm2_item_is_animation(doc, sel_idx, item_idx)) {
    char const *script_name = ptk_anm2_item_get_script_name(doc, sel_idx, item_idx);
    ov_snprintf_char2wchar(out, out_len, "%1$hs%2$hs", "[%1$hs] %2$hs", script_name, name);
  } else {
    ov_snprintf_char2wchar(out, out_len, "%1$hs", "%1$hs", name);
  }
}

// TreeView rebuild for reset operations (no state preservation)
static void rebuild_treeview(struct ptk_anm2editor *editor) {
  if (!editor || !editor->treeview || !editor->doc) {
    return;
  }

  size_t const num_selectors = ptk_anm2_selector_count(editor->doc);

  SendMessageW(editor->treeview, WM_SETREDRAW, FALSE, 0);
  TreeView_DeleteAllItems(editor->treeview);

  for (size_t i = 0; i < num_selectors; i++) {
    wchar_t group_name[256];
    format_selector_display_name(editor->doc, i, group_name, sizeof(group_name) / sizeof(group_name[0]));
    uint32_t const sel_id = ptk_anm2_selector_get_id(editor->doc, i);
    LPARAM const sel_lparam = treeview_encode_selector_id(sel_id);
    HTREEITEM hSelector = (HTREEITEM)(SendMessageW(
        editor->treeview,
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

    size_t const num_items = ptk_anm2_item_count(editor->doc, i);
    for (size_t j = 0; j < num_items; j++) {
      wchar_t item_name[256];
      format_item_display_name(editor->doc, i, j, item_name, sizeof(item_name) / sizeof(item_name[0]));
      uint32_t const item_id = ptk_anm2_item_get_id(editor->doc, i, j);
      LPARAM const item_lparam = treeview_encode_item_id(item_id);
      SendMessageW(editor->treeview,
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

    // Default: expand all selectors
    TreeView_Expand(editor->treeview, hSelector, TVE_EXPAND);
  }

  SendMessageW(editor->treeview, WM_SETREDRAW, TRUE, 0);
  InvalidateRect(editor->treeview, NULL, TRUE);
}

static bool refresh_treeview(struct ptk_anm2editor *editor, struct ov_error *const err) {
  (void)err;
  if (!editor || !editor->treeview || !editor->doc) {
    return true;
  }

  size_t const num_selectors = ptk_anm2_selector_count(editor->doc);

  HTREEITEM selected_item = NULL;
  LPARAM selected_lparam = 0;

  // Save scroll position
  int const scroll_pos = GetScrollPos(editor->treeview, SB_VERT);

  // Save expanded state of each selector into userdata
  // userdata bit 0: expanded state (1 = expanded, 0 = collapsed)
  // userdata bit 1: state has been set (1 = set, 0 = not set / use default)
  for (HTREEITEM hItem = (HTREEITEM)(SendMessageW(editor->treeview, TVM_GETNEXTITEM, TVGN_ROOT, 0)); hItem;
       hItem = (HTREEITEM)(SendMessageW(editor->treeview, TVM_GETNEXTITEM, TVGN_NEXT, (LPARAM)hItem))) {
    TVITEMW tvi = {.mask = TVIF_STATE | TVIF_PARAM, .hItem = hItem, .stateMask = TVIS_EXPANDED};
    if (SendMessageW(editor->treeview, TVM_GETITEMW, 0, (LPARAM)&tvi)) {
      uint32_t id = 0;
      if (treeview_decode_lparam(tvi.lParam, &id)) {
        // It's a selector - save expanded state to userdata
        size_t sel_idx = 0;
        if (ptk_anm2_find_selector_by_id(editor->doc, id, &sel_idx)) {
          uintptr_t userdata = 2; // Set bit 1 to indicate state has been set
          if (tvi.state & TVIS_EXPANDED) {
            userdata |= 1; // Set expanded bit
          }
          ptk_anm2_selector_set_userdata(editor->doc, sel_idx, userdata);
        }
      }
    }
  }

  // Save selected item's lParam (ID-based, so it survives rebuild)
  {
    HTREEITEM hSel = (HTREEITEM)(SendMessageW(editor->treeview, TVM_GETNEXTITEM, TVGN_CARET, 0));
    if (hSel) {
      TVITEMW tvi = {.mask = TVIF_PARAM, .hItem = hSel};
      if (SendMessageW(editor->treeview, TVM_GETITEMW, 0, (LPARAM)&tvi)) {
        selected_lparam = tvi.lParam;
      }
    }
  }

  // Rebuild tree
  SendMessageW(editor->treeview, WM_SETREDRAW, FALSE, 0);
  TreeView_DeleteAllItems(editor->treeview);

  for (size_t i = 0; i < num_selectors; i++) {
    wchar_t group_name[256];
    format_selector_display_name(editor->doc, i, group_name, sizeof(group_name) / sizeof(group_name[0]));
    uint32_t const sel_id = ptk_anm2_selector_get_id(editor->doc, i);
    LPARAM const sel_lparam = treeview_encode_selector_id(sel_id);
    HTREEITEM hSelector = (HTREEITEM)(SendMessageW(
        editor->treeview,
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

    if (selected_lparam == sel_lparam) {
      selected_item = hSelector;
    }

    size_t const num_items = ptk_anm2_item_count(editor->doc, i);
    for (size_t j = 0; j < num_items; j++) {
      wchar_t item_name[256];
      format_item_display_name(editor->doc, i, j, item_name, sizeof(item_name) / sizeof(item_name[0]));
      uint32_t const item_id = ptk_anm2_item_get_id(editor->doc, i, j);
      LPARAM const item_lparam = treeview_encode_item_id(item_id);
      HTREEITEM hItem = (HTREEITEM)(SendMessageW(
          editor->treeview,
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
                     }));
      if (selected_lparam == item_lparam) {
        selected_item = hItem;
      }
    }

    // Restore expanded state from userdata (default: expanded for new selectors)
    uintptr_t const userdata = ptk_anm2_selector_get_userdata(editor->doc, i);
    // If bit 1 is not set (state not saved yet), default to expanded
    // Otherwise, check bit 0 for expanded state
    bool const state_saved = (userdata & 2) != 0;
    bool const should_expand = !state_saved || (userdata & 1);
    if (should_expand) {
      TreeView_Expand(editor->treeview, hSelector, TVE_EXPAND);
    }
  }

  // Restore selection
  if (selected_item) {
    SendMessageW(editor->treeview, TVM_SELECTITEM, TVGN_CARET, (LPARAM)selected_item);
  }

  SendMessageW(editor->treeview, WM_SETREDRAW, TRUE, 0);

  // Restore scroll position
  SetScrollPos(editor->treeview, SB_VERT, scroll_pos, FALSE);
  SendMessageW(editor->treeview, WM_VSCROLL, MAKEWPARAM(SB_THUMBPOSITION, scroll_pos), 0);

  return true;
}

// Select item in TreeView by ID
// is_selector: true to select a selector, false to select an item
static void select_treeview_by_id(struct ptk_anm2editor *editor, uint32_t id, bool is_selector) {
  if (!editor || !editor->treeview) {
    return;
  }

  LPARAM target_lparam;
  if (is_selector) {
    target_lparam = treeview_encode_selector_id(id);
  } else {
    target_lparam = treeview_encode_item_id(id);
  }

  // Iterate through all items to find the matching one
  HTREEITEM hRoot = (HTREEITEM)(SendMessageW(editor->treeview, TVM_GETNEXTITEM, TVGN_ROOT, 0));
  while (hRoot) {
    TVITEMW tvi = {.mask = TVIF_PARAM, .hItem = hRoot};
    if (TreeView_GetItem(editor->treeview, &tvi) && tvi.lParam == target_lparam) {
      TreeView_SelectItem(editor->treeview, hRoot);
      return;
    }

    // Check children
    HTREEITEM hChild = (HTREEITEM)(SendMessageW(editor->treeview, TVM_GETNEXTITEM, TVGN_CHILD, (LPARAM)hRoot));
    while (hChild) {
      TVITEMW child_tvi = {.mask = TVIF_PARAM, .hItem = hChild};
      if (TreeView_GetItem(editor->treeview, &child_tvi) && child_tvi.lParam == target_lparam) {
        TreeView_SelectItem(editor->treeview, hChild);
        return;
      }
      hChild = (HTREEITEM)(SendMessageW(editor->treeview, TVM_GETNEXTITEM, TVGN_NEXT, (LPARAM)hChild));
    }

    hRoot = (HTREEITEM)(SendMessageW(editor->treeview, TVM_GETNEXTITEM, TVGN_NEXT, (LPARAM)hRoot));
  }
}

// Select item in TreeView by indices (convenience wrapper)
// sel_idx: selector index, item_idx: item index (SIZE_MAX to select selector)
static void select_treeview_by_index(struct ptk_anm2editor *editor, size_t sel_idx, size_t item_idx) {
  if (!editor || !editor->doc) {
    return;
  }

  if (item_idx == SIZE_MAX) {
    uint32_t const sel_id = ptk_anm2_selector_get_id(editor->doc, sel_idx);
    if (sel_id != 0) {
      select_treeview_by_id(editor, sel_id, true);
    }
  } else {
    uint32_t const item_id = ptk_anm2_item_get_id(editor->doc, sel_idx, item_idx);
    if (item_id != 0) {
      select_treeview_by_id(editor, item_id, false);
    }
  }
}

// =============================================================================
// TreeView incremental update helpers
// =============================================================================

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

// Get n-th root item (selector) in TreeView
static HTREEITEM get_treeview_selector_at_index(HWND treeview, size_t index) {
  HTREEITEM hItem = (HTREEITEM)(SendMessageW(treeview, TVM_GETNEXTITEM, TVGN_ROOT, 0));
  for (size_t i = 0; i < index && hItem; i++) {
    hItem = (HTREEITEM)(SendMessageW(treeview, TVM_GETNEXTITEM, TVGN_NEXT, (LPARAM)hItem));
  }
  return hItem;
}

// Get n-th child item under a selector
static HTREEITEM get_treeview_item_at_index(HWND treeview, HTREEITEM hSelector, size_t index) {
  HTREEITEM hItem = (HTREEITEM)(SendMessageW(treeview, TVM_GETNEXTITEM, TVGN_CHILD, (LPARAM)hSelector));
  for (size_t i = 0; i < index && hItem; i++) {
    hItem = (HTREEITEM)(SendMessageW(treeview, TVM_GETNEXTITEM, TVGN_NEXT, (LPARAM)hItem));
  }
  return hItem;
}

// Insert selector at index
static HTREEITEM treeview_insert_selector_ex(struct ptk_anm2editor *editor, size_t sel_idx, bool expand) {
  wchar_t group_name[256];
  format_selector_display_name(editor->doc, sel_idx, group_name, sizeof(group_name) / sizeof(group_name[0]));
  uint32_t const sel_id = ptk_anm2_selector_get_id(editor->doc, sel_idx);
  LPARAM const sel_lparam = treeview_encode_selector_id(sel_id);

  // Find insertion point
  HTREEITEM hInsertAfter = TVI_FIRST;
  if (sel_idx > 0) {
    HTREEITEM hPrev = get_treeview_selector_at_index(editor->treeview, sel_idx - 1);
    if (hPrev) {
      hInsertAfter = hPrev;
    }
  }

  HTREEITEM hSelector =
      (HTREEITEM)(SendMessageW(editor->treeview,
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
  size_t const num_items = ptk_anm2_item_count(editor->doc, sel_idx);
  for (size_t j = 0; j < num_items; j++) {
    wchar_t item_name[256];
    format_item_display_name(editor->doc, sel_idx, j, item_name, sizeof(item_name) / sizeof(item_name[0]));
    uint32_t const item_id = ptk_anm2_item_get_id(editor->doc, sel_idx, j);
    LPARAM const item_lparam = treeview_encode_item_id(item_id);
    SendMessageW(editor->treeview,
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

  // Expand if requested
  if (expand) {
    TreeView_Expand(editor->treeview, hSelector, TVE_EXPAND);
  }

  return hSelector;
}

// Insert selector at index (expanded by default)
static HTREEITEM treeview_insert_selector(struct ptk_anm2editor *editor, size_t sel_idx) {
  return treeview_insert_selector_ex(editor, sel_idx, true);
}

// Insert item at index under selector
static HTREEITEM treeview_insert_item(struct ptk_anm2editor *editor, size_t sel_idx, size_t item_idx) {
  HTREEITEM hSelector = get_treeview_selector_at_index(editor->treeview, sel_idx);
  if (!hSelector) {
    return NULL;
  }

  wchar_t item_name[256];
  format_item_display_name(editor->doc, sel_idx, item_idx, item_name, sizeof(item_name) / sizeof(item_name[0]));
  uint32_t const item_id = ptk_anm2_item_get_id(editor->doc, sel_idx, item_idx);
  LPARAM const item_lparam = treeview_encode_item_id(item_id);

  // Find insertion point
  HTREEITEM hInsertAfter = TVI_FIRST;
  if (item_idx > 0) {
    HTREEITEM hPrev = get_treeview_item_at_index(editor->treeview, hSelector, item_idx - 1);
    if (hPrev) {
      hInsertAfter = hPrev;
    }
  }

  return (
      HTREEITEM)(SendMessageW(editor->treeview,
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
static void treeview_update_selector_text(struct ptk_anm2editor *editor, size_t sel_idx) {
  uint32_t const sel_id = ptk_anm2_selector_get_id(editor->doc, sel_idx);
  LPARAM const sel_lparam = treeview_encode_selector_id(sel_id);
  HTREEITEM hItem = find_treeview_item_by_lparam(editor->treeview, sel_lparam);
  if (!hItem) {
    return;
  }

  wchar_t group_name[256];
  format_selector_display_name(editor->doc, sel_idx, group_name, sizeof(group_name) / sizeof(group_name[0]));

  TVITEMW tvi = {
      .mask = TVIF_TEXT,
      .hItem = hItem,
      .pszText = group_name,
  };
  SendMessageW(editor->treeview, TVM_SETITEMW, 0, (LPARAM)&tvi);
}

// Update item text
static void treeview_update_item_text(struct ptk_anm2editor *editor, size_t sel_idx, size_t item_idx) {
  uint32_t const item_id = ptk_anm2_item_get_id(editor->doc, sel_idx, item_idx);
  LPARAM const item_lparam = treeview_encode_item_id(item_id);
  HTREEITEM hItem = find_treeview_item_by_lparam(editor->treeview, item_lparam);
  if (!hItem) {
    return;
  }

  wchar_t item_name[256];
  format_item_display_name(editor->doc, sel_idx, item_idx, item_name, sizeof(item_name) / sizeof(item_name[0]));

  TVITEMW tvi = {
      .mask = TVIF_TEXT,
      .hItem = hItem,
      .pszText = item_name,
  };
  SendMessageW(editor->treeview, TVM_SETITEMW, 0, (LPARAM)&tvi);
}

// Update focus tracking based on document changes
static void update_focus_tracking(struct ptk_anm2editor *editor,
                                  enum ptk_anm2_op_type op_type,
                                  size_t sel_idx,
                                  size_t item_idx,
                                  size_t move_to_sel_idx,
                                  size_t move_to_idx) {
  // Handle RESET - invalidate focus
  if (op_type == ptk_anm2_op_reset) {
    editor->focus_valid = false;
    return;
  }

  // If focus is not valid, nothing to track
  if (!editor->focus_valid) {
    return;
  }

  switch (op_type) {
  case ptk_anm2_op_selector_insert:
    if (sel_idx <= editor->focus_sel_idx) {
      editor->focus_sel_idx++;
    }
    break;

  case ptk_anm2_op_selector_remove:
    if (sel_idx < editor->focus_sel_idx) {
      editor->focus_sel_idx--;
    } else if (sel_idx == editor->focus_sel_idx) {
      editor->focus_valid = false;
    }
    break;

  case ptk_anm2_op_selector_move:
    // For selector move, move_to_sel_idx contains the destination index
    if (editor->focus_sel_idx == sel_idx) {
      editor->focus_sel_idx = move_to_sel_idx;
    } else if (sel_idx < editor->focus_sel_idx && move_to_sel_idx >= editor->focus_sel_idx) {
      editor->focus_sel_idx--;
    } else if (sel_idx > editor->focus_sel_idx && move_to_sel_idx <= editor->focus_sel_idx) {
      editor->focus_sel_idx++;
    }
    break;

  case ptk_anm2_op_item_insert:
    // Only track if we have an item selected (not just selector)
    if (sel_idx == editor->focus_sel_idx && editor->focus_item_idx != SIZE_MAX && item_idx <= editor->focus_item_idx) {
      editor->focus_item_idx++;
    }
    break;

  case ptk_anm2_op_item_remove:
    if (sel_idx == editor->focus_sel_idx && editor->focus_item_idx != SIZE_MAX) {
      if (item_idx < editor->focus_item_idx) {
        editor->focus_item_idx--;
      } else if (item_idx == editor->focus_item_idx) {
        // Item was removed, fall back to selector
        editor->focus_item_idx = SIZE_MAX;
      }
    }
    break;

  case ptk_anm2_op_item_move:
    if (editor->focus_item_idx == SIZE_MAX) {
      // Selector is selected, item moves don't affect it
      break;
    }
    if (editor->focus_sel_idx == sel_idx && editor->focus_item_idx == item_idx) {
      // The focused item is being moved
      editor->focus_sel_idx = move_to_sel_idx;
      editor->focus_item_idx = move_to_idx;
    } else if (sel_idx == move_to_sel_idx) {
      // Same selector move
      if (editor->focus_sel_idx == sel_idx) {
        if (item_idx < editor->focus_item_idx && move_to_idx >= editor->focus_item_idx) {
          editor->focus_item_idx--;
        } else if (item_idx > editor->focus_item_idx && move_to_idx <= editor->focus_item_idx) {
          editor->focus_item_idx++;
        }
      }
    } else {
      // Cross-selector move
      if (editor->focus_sel_idx == sel_idx && item_idx < editor->focus_item_idx) {
        editor->focus_item_idx--;
      }
      if (editor->focus_sel_idx == move_to_sel_idx && move_to_idx <= editor->focus_item_idx) {
        editor->focus_item_idx++;
      }
    }
    break;

  // Operations that don't affect focus tracking
  case ptk_anm2_op_reset:
  case ptk_anm2_op_group_begin:
  case ptk_anm2_op_group_end:
  case ptk_anm2_op_set_label:
  case ptk_anm2_op_set_psd_path:
  case ptk_anm2_op_set_exclusive_support_default:
  case ptk_anm2_op_set_information:
  case ptk_anm2_op_selector_set_group:
  case ptk_anm2_op_item_set_name:
  case ptk_anm2_op_item_set_value:
  case ptk_anm2_op_item_set_script_name:
  case ptk_anm2_op_param_insert:
  case ptk_anm2_op_param_remove:
  case ptk_anm2_op_param_set_key:
  case ptk_anm2_op_param_set_value:
    // These operations don't change selector/item indices
    break;
  }
}

// Update TreeView based on document changes (differential updates)
static void update_treeview_differential(struct ptk_anm2editor *editor,
                                         enum ptk_anm2_op_type op_type,
                                         size_t sel_idx,
                                         size_t item_idx,
                                         size_t move_to_sel_idx,
                                         size_t move_to_idx) {
  switch (op_type) {
  case ptk_anm2_op_reset:
    // Full rebuild - no state preservation needed (new document)
    // Note: detail panel update is handled by the caller (e.g., ptk_anm2editor_open)
    rebuild_treeview(editor);
    break;

  case ptk_anm2_op_selector_insert:
    treeview_insert_selector(editor, sel_idx);
    break;

  case ptk_anm2_op_selector_remove:
    // Element is already removed from doc, but TreeView still has it at sel_idx
    {
      HTREEITEM hSelector = get_treeview_selector_at_index(editor->treeview, sel_idx);
      if (hSelector) {
        TreeView_DeleteItem(editor->treeview, hSelector);
      }
    }
    break;

  case ptk_anm2_op_selector_set_group:
    treeview_update_selector_text(editor, sel_idx);
    break;

  case ptk_anm2_op_selector_move:
    // Selector already moved in doc from sel_idx to move_to_sel_idx
    // TreeView still has the selector at its old position - we need to move it
    {
      // Get the selector ID at the new position in the document
      uint32_t const sel_id = ptk_anm2_selector_get_id(editor->doc, move_to_sel_idx);
      LPARAM const sel_lparam = treeview_encode_selector_id(sel_id);

      // Find and delete the selector from its old position in TreeView
      HTREEITEM hOldSelector = find_treeview_item_by_lparam(editor->treeview, sel_lparam);
      if (hOldSelector) {
        // Only manage redraw if not in a transaction
        if (editor->transaction_depth == 0) {
          SendMessageW(editor->treeview, WM_SETREDRAW, FALSE, 0);
        }

        // Remember the expanded state before deleting
        UINT const old_state =
            (UINT)SendMessageW(editor->treeview, TVM_GETITEMSTATE, (WPARAM)hOldSelector, (LPARAM)TVIS_EXPANDED);
        bool const was_expanded = (old_state & TVIS_EXPANDED) != 0;

        // Deleting the selector also deletes its children
        TreeView_DeleteItem(editor->treeview, hOldSelector);

        // Insert at new position (this also inserts all child items)
        HTREEITEM hNewSelector = treeview_insert_selector_ex(editor, move_to_sel_idx, was_expanded);

        // Select the moved selector (only if not in a transaction)
        if (hNewSelector && editor->transaction_depth == 0) {
          TreeView_SelectItem(editor->treeview, hNewSelector);
        }

        if (editor->transaction_depth == 0) {
          SendMessageW(editor->treeview, WM_SETREDRAW, TRUE, 0);
          InvalidateRect(editor->treeview, NULL, TRUE);
        }
      }
    }
    break;

  case ptk_anm2_op_item_insert:
    treeview_insert_item(editor, sel_idx, item_idx);
    break;

  case ptk_anm2_op_item_remove:
    // Element is already removed from doc, but TreeView still has it at [sel_idx, item_idx]
    {
      HTREEITEM hSelector = get_treeview_selector_at_index(editor->treeview, sel_idx);
      if (hSelector) {
        HTREEITEM hItem = get_treeview_item_at_index(editor->treeview, hSelector, item_idx);
        if (hItem) {
          TreeView_DeleteItem(editor->treeview, hItem);
        }
      }
    }
    break;

  case ptk_anm2_op_item_set_name:
    treeview_update_item_text(editor, sel_idx, item_idx);
    break;

  case ptk_anm2_op_item_move:
    // Item already moved in doc from (sel_idx, item_idx) to (move_to_sel_idx, move_to_idx)
    // TreeView still has the item at its old position - we need to move it
    {
      // Get the item ID at the new position in the document
      uint32_t const item_id = ptk_anm2_item_get_id(editor->doc, move_to_sel_idx, move_to_idx);
      LPARAM const item_lparam = treeview_encode_item_id(item_id);

      // Find and delete the item from its old position in TreeView
      HTREEITEM hOldItem = find_treeview_item_by_lparam(editor->treeview, item_lparam);
      if (hOldItem) {
        // Only manage redraw if not in a transaction
        if (editor->transaction_depth == 0) {
          SendMessageW(editor->treeview, WM_SETREDRAW, FALSE, 0);
        }

        // Get the destination selector and remember its expanded state
        HTREEITEM hDstSelector = get_treeview_selector_at_index(editor->treeview, move_to_sel_idx);
        bool dst_was_expanded = false;
        if (hDstSelector) {
          UINT const dst_state =
              (UINT)SendMessageW(editor->treeview, TVM_GETITEMSTATE, (WPARAM)hDstSelector, (LPARAM)TVIS_EXPANDED);
          dst_was_expanded = (dst_state & TVIS_EXPANDED) != 0;
        }

        TreeView_DeleteItem(editor->treeview, hOldItem);

        // Insert at new position
        HTREEITEM hNewItem = treeview_insert_item(editor, move_to_sel_idx, move_to_idx);

        // Select the moved item (only if not in a transaction)
        if (hNewItem && editor->transaction_depth == 0) {
          TreeView_SelectItem(editor->treeview, hNewItem);

          // Restore the destination selector's expanded state if it was collapsed
          // (TreeView_SelectItem may have expanded it)
          if (hDstSelector && !dst_was_expanded) {
            TreeView_Expand(editor->treeview, hDstSelector, TVE_COLLAPSE);
          }
        }

        if (editor->transaction_depth == 0) {
          SendMessageW(editor->treeview, WM_SETREDRAW, TRUE, 0);
          InvalidateRect(editor->treeview, NULL, TRUE);
        }
      }
    }
    break;

  // Operations that don't affect TreeView structure
  case ptk_anm2_op_group_begin:
    // Start of a transaction (or end during UNDO) - manage redraw batching
    // During normal execution: begin comes first, depth goes 0 -> 1
    // During UNDO: begin comes last, depth goes -1 -> 0
    if (editor->transaction_depth == 0) {
      SendMessageW(editor->treeview, WM_SETREDRAW, FALSE, 0);
    }
    editor->transaction_depth++;
    if (editor->transaction_depth == 0) {
      SendMessageW(editor->treeview, WM_SETREDRAW, TRUE, 0);
      InvalidateRect(editor->treeview, NULL, TRUE);
    }
    break;

  case ptk_anm2_op_group_end:
    // End of a transaction (or start during UNDO) - manage redraw batching
    // During normal execution: end comes last, depth goes 1 -> 0
    // During UNDO: end comes first, depth goes 0 -> -1
    if (editor->transaction_depth == 0) {
      SendMessageW(editor->treeview, WM_SETREDRAW, FALSE, 0);
    }
    editor->transaction_depth--;
    if (editor->transaction_depth == 0) {
      SendMessageW(editor->treeview, WM_SETREDRAW, TRUE, 0);
      InvalidateRect(editor->treeview, NULL, TRUE);
    }
    break;

  case ptk_anm2_op_set_label:
  case ptk_anm2_op_set_psd_path:
  case ptk_anm2_op_set_exclusive_support_default:
  case ptk_anm2_op_set_information:
  case ptk_anm2_op_item_set_value:
  case ptk_anm2_op_item_set_script_name:
  case ptk_anm2_op_param_insert:
  case ptk_anm2_op_param_remove:
  case ptk_anm2_op_param_set_key:
  case ptk_anm2_op_param_set_value:
    // These operations don't affect TreeView
    break;
  }
}

// Get the currently selected item's indices
// Returns: 0 = nothing selected, 1 = selector selected, 2 = item selected
static int get_selected_indices(struct ptk_anm2editor *editor, size_t *selector_idx, size_t *item_idx) {
  HTREEITEM hItem = (HTREEITEM)(SendMessageW(editor->treeview, TVM_GETNEXTITEM, TVGN_CARET, 0));
  if (!hItem) {
    return 0;
  }

  TVITEMW tvi = {
      .mask = TVIF_PARAM,
      .hItem = hItem,
  };
  if (!TreeView_GetItem(editor->treeview, &tvi)) {
    return 0;
  }

  uint32_t id = 0;
  bool is_selector = treeview_decode_lparam(tvi.lParam, &id);
  if (is_selector) {
    if (ptk_anm2_find_selector_by_id(editor->doc, id, selector_idx)) {
      return 1;
    }
    return 0;
  } else {
    if (ptk_anm2_find_item_by_id(editor->doc, id, selector_idx, item_idx)) {
      return 2;
    }
    return 0;
  }
}

// Helper function to add a row to the detail list with UTF-8 property and value
static void detail_list_add_row(HWND listview, char const *property, char const *value, LPARAM lparam) {
  int const idx = (int)SendMessageW(listview, LVM_GETITEMCOUNT, 0, 0);
  wchar_t prop_buf[256];
  wchar_t val_buf[512];
  ov_snprintf_char2wchar(prop_buf, sizeof(prop_buf) / sizeof(prop_buf[0]), "%1$hs", "%1$hs", property ? property : "");
  ov_snprintf_char2wchar(val_buf, sizeof(val_buf) / sizeof(val_buf[0]), "%1$hs", "%1$hs", value ? value : "");

  LVITEMW lvi = {
      .mask = LVIF_TEXT | LVIF_PARAM,
      .iItem = idx,
      .iSubItem = 0,
      .pszText = prop_buf,
      .lParam = lparam,
  };
  SendMessageW(listview, LVM_INSERTITEMW, 0, (LPARAM)&lvi);
  lvi.iSubItem = 1;
  lvi.pszText = val_buf;
  SendMessageW(listview, LVM_SETITEMTEXTW, (WPARAM)idx, (LPARAM)&lvi);
}

// Populate detail panel for multi-selection mode
// Shows value items in tree view order (ignores animation items)
static void update_detail_panel_multisel(struct ptk_anm2editor *editor) {
  // Iterate through tree in order to collect selected value items
  size_t const sel_count = ptk_anm2_selector_count(editor->doc);
  for (size_t sel_idx = 0; sel_idx < sel_count; sel_idx++) {
    size_t const item_count = ptk_anm2_item_count(editor->doc, sel_idx);
    for (size_t item_idx = 0; item_idx < item_count; item_idx++) {
      uint32_t const id = ptk_anm2_item_get_id(editor->doc, sel_idx, item_idx);
      if (!multisel_contains(editor, id)) {
        continue;
      }
      if (ptk_anm2_item_is_animation(editor->doc, sel_idx, item_idx)) {
        continue;
      }
      char const *name = ptk_anm2_item_get_name(editor->doc, sel_idx, item_idx);
      char const *value = ptk_anm2_item_get_value(editor->doc, sel_idx, item_idx);
      detail_list_add_row(editor->detaillist, name, value, (LPARAM)id);
    }
  }
}

// Populate detail panel for no selection or selector selection
static void update_detail_panel_document(struct ptk_anm2editor *editor) {
  detail_list_add_row(
      editor->detaillist, pgettext("anm2editor", "Label"), ptk_anm2_get_label(editor->doc), DETAILLIST_LPARAM_LABEL);
  detail_list_add_row(editor->detaillist,
                      pgettext("anm2editor", "Information"),
                      ptk_anm2_get_information(editor->doc),
                      DETAILLIST_LPARAM_INFORMATION);
  detail_list_add_row(editor->detaillist,
                      pgettext("anm2editor", "PSD File Path"),
                      ptk_anm2_get_psd_path(editor->doc),
                      DETAILLIST_LPARAM_PSD_PATH);
  detail_list_add_row(editor->detaillist,
                      pgettext("anm2editor", "Exclusive Support Default"),
                      ptk_anm2_get_exclusive_support_default(editor->doc) ? "1" : "",
                      DETAILLIST_LPARAM_EXCLUSIVE_SUPPORT_DEFAULT);
}

// Populate detail panel for single item selection
static void update_detail_panel_item(struct ptk_anm2editor *editor, size_t sel_idx, size_t item_idx) {
  if (sel_idx >= ptk_anm2_selector_count(editor->doc)) {
    return;
  }
  if (item_idx >= ptk_anm2_item_count(editor->doc, sel_idx)) {
    return;
  }

  bool const is_animation = ptk_anm2_item_is_animation(editor->doc, sel_idx, item_idx);
  if (is_animation) {
    size_t const num_params = ptk_anm2_param_count(editor->doc, sel_idx, item_idx);
    for (size_t i = 0; i < num_params; i++) {
      char const *key = ptk_anm2_param_get_key(editor->doc, sel_idx, item_idx, i);
      char const *value = ptk_anm2_param_get_value(editor->doc, sel_idx, item_idx, i);
      detail_list_add_row(editor->detaillist, key, value, (LPARAM)i);
    }
    detail_list_add_row(editor->detaillist, pgettext("anm2editor", "(Add new...)"), "", DETAILLIST_LPARAM_PLACEHOLDER);
  } else {
    char const *name = ptk_anm2_item_get_name(editor->doc, sel_idx, item_idx);
    char const *value = ptk_anm2_item_get_value(editor->doc, sel_idx, item_idx);
    detail_list_add_row(editor->detaillist, name, value, (LPARAM)0);
  }
}

static void update_detail_panel(struct ptk_anm2editor *editor) {
  if (!editor || !editor->detaillist || !editor->doc) {
    return;
  }

  SendMessageW(editor->detaillist, LVM_DELETEALLITEMS, 0, 0);

  if (multisel_count(editor) > 1) {
    update_detail_panel_multisel(editor);
  } else {
    size_t sel_idx = 0;
    size_t item_idx = 0;
    int const sel_type = get_selected_indices(editor, &sel_idx, &item_idx);
    if (sel_type == 2) {
      update_detail_panel_item(editor, sel_idx, item_idx);
    } else {
      update_detail_panel_document(editor);
    }
  }
}

// Helper to update a single row's text in the detail list
static void detail_list_update_row(HWND listview, int row_idx, char const *property, char const *value) {
  wchar_t prop_buf[256];
  wchar_t val_buf[512];
  ov_snprintf_char2wchar(prop_buf, sizeof(prop_buf) / sizeof(prop_buf[0]), "%1$hs", "%1$hs", property ? property : "");
  ov_snprintf_char2wchar(val_buf, sizeof(val_buf) / sizeof(val_buf[0]), "%1$hs", "%1$hs", value ? value : "");

  LVITEMW lvi = {
      .iItem = row_idx,
      .iSubItem = 0,
      .pszText = prop_buf,
  };
  SendMessageW(listview, LVM_SETITEMTEXTW, (WPARAM)row_idx, (LPARAM)&lvi);
  lvi.iSubItem = 1;
  lvi.pszText = val_buf;
  SendMessageW(listview, LVM_SETITEMTEXTW, (WPARAM)row_idx, (LPARAM)&lvi);
}

// Helper to insert a row at a specific position in the detail list
static void
detail_list_insert_row_at(HWND listview, int row_idx, char const *property, char const *value, LPARAM lparam) {
  wchar_t prop_buf[256];
  wchar_t val_buf[512];
  ov_snprintf_char2wchar(prop_buf, sizeof(prop_buf) / sizeof(prop_buf[0]), "%1$hs", "%1$hs", property ? property : "");
  ov_snprintf_char2wchar(val_buf, sizeof(val_buf) / sizeof(val_buf[0]), "%1$hs", "%1$hs", value ? value : "");

  LVITEMW lvi = {
      .mask = LVIF_TEXT | LVIF_PARAM,
      .iItem = row_idx,
      .iSubItem = 0,
      .pszText = prop_buf,
      .lParam = lparam,
  };
  SendMessageW(listview, LVM_INSERTITEMW, 0, (LPARAM)&lvi);
  lvi.iSubItem = 1;
  lvi.pszText = val_buf;
  SendMessageW(listview, LVM_SETITEMTEXTW, (WPARAM)row_idx, (LPARAM)&lvi);
}

// Find the ListView row index by lParam value
// Returns -1 if not found
static int detail_list_find_row_by_lparam(HWND listview, LPARAM target_lparam) {
  int const count = (int)SendMessageW(listview, LVM_GETITEMCOUNT, 0, 0);
  for (int i = 0; i < count; i++) {
    LVITEMW lvi = {
        .mask = LVIF_PARAM,
        .iItem = i,
    };
    if (SendMessageW(listview, LVM_GETITEMW, 0, (LPARAM)&lvi)) {
      if (lvi.lParam == target_lparam) {
        return i;
      }
    }
  }
  return -1;
}

// Update detail list based on document changes (differential updates)
// This function checks if the change affects the currently displayed content and updates accordingly.
static void update_detaillist_differential(
    struct ptk_anm2editor *editor, enum ptk_anm2_op_type op_type, size_t sel_idx, size_t item_idx, size_t param_idx) {
  if (!editor || !editor->detaillist || !editor->doc) {
    return;
  }

  // Get current selection
  size_t cur_sel_idx = 0;
  size_t cur_item_idx = 0;
  int const sel_type = get_selected_indices(editor, &cur_sel_idx, &cur_item_idx);

  switch (op_type) {
  case ptk_anm2_op_set_label:
    // Label row is shown when nothing or a selector is selected (sel_type == 0 or 1)
    if (sel_type == 0 || sel_type == 1) {
      int const row_idx = detail_list_find_row_by_lparam(editor->detaillist, DETAILLIST_LPARAM_LABEL);
      if (row_idx >= 0) {
        detail_list_update_row(
            editor->detaillist, row_idx, pgettext("anm2editor", "Label"), ptk_anm2_get_label(editor->doc));
      }
    }
    break;

  case ptk_anm2_op_set_psd_path:
    // PSD Path row is shown when nothing or a selector is selected (sel_type == 0 or 1)
    if (sel_type == 0 || sel_type == 1) {
      int const row_idx = detail_list_find_row_by_lparam(editor->detaillist, DETAILLIST_LPARAM_PSD_PATH);
      if (row_idx >= 0) {
        detail_list_update_row(
            editor->detaillist, row_idx, pgettext("anm2editor", "PSD File Path"), ptk_anm2_get_psd_path(editor->doc));
      }
    }
    break;

  case ptk_anm2_op_item_set_name:
    // If a value item is selected and it's the changed item, update the first column
    if (sel_type == 2 && cur_sel_idx == sel_idx && cur_item_idx == item_idx) {
      if (!ptk_anm2_item_is_animation(editor->doc, sel_idx, item_idx)) {
        char const *name = ptk_anm2_item_get_name(editor->doc, sel_idx, item_idx);
        char const *value = ptk_anm2_item_get_value(editor->doc, sel_idx, item_idx);
        detail_list_update_row(editor->detaillist, 0, name, value);
      }
    }
    break;

  case ptk_anm2_op_item_set_value:
    // If a value item is selected and it's the changed item, update the second column
    if (sel_type == 2 && cur_sel_idx == sel_idx && cur_item_idx == item_idx) {
      if (!ptk_anm2_item_is_animation(editor->doc, sel_idx, item_idx)) {
        char const *name = ptk_anm2_item_get_name(editor->doc, sel_idx, item_idx);
        char const *value = ptk_anm2_item_get_value(editor->doc, sel_idx, item_idx);
        detail_list_update_row(editor->detaillist, 0, name, value);
      }
    }
    break;

  case ptk_anm2_op_param_insert:
    // If an animation item is selected and it's the changed item, insert a new row
    if (sel_type == 2 && cur_sel_idx == sel_idx && cur_item_idx == item_idx) {
      if (ptk_anm2_item_is_animation(editor->doc, sel_idx, item_idx)) {
        char const *key = ptk_anm2_param_get_key(editor->doc, sel_idx, item_idx, param_idx);
        char const *value = ptk_anm2_param_get_value(editor->doc, sel_idx, item_idx, param_idx);
        // Insert at param_idx position (before the placeholder row)
        detail_list_insert_row_at(editor->detaillist, (int)param_idx, key, value, (LPARAM)param_idx);
        // Update lParam values for subsequent rows (they shifted by 1)
        int const count = (int)SendMessageW(editor->detaillist, LVM_GETITEMCOUNT, 0, 0);
        for (int i = (int)param_idx + 1; i < count; i++) {
          LVITEMW lvi = {
              .mask = LVIF_PARAM,
              .iItem = i,
          };
          if (SendMessageW(editor->detaillist, LVM_GETITEMW, 0, (LPARAM)&lvi)) {
            // Skip special lParams (negative values)
            if (lvi.lParam >= 0) {
              lvi.lParam = (LPARAM)i;
              SendMessageW(editor->detaillist, LVM_SETITEMW, 0, (LPARAM)&lvi);
            }
          }
        }
      }
    }
    break;

  case ptk_anm2_op_param_remove:
    // If an animation item is selected and it's the changed item, remove the row
    // Note: param_idx refers to the index BEFORE removal (doc already has it removed)
    if (sel_type == 2 && cur_sel_idx == sel_idx && cur_item_idx == item_idx) {
      // Delete the row at param_idx
      SendMessageW(editor->detaillist, LVM_DELETEITEM, (WPARAM)param_idx, 0);
      // Update lParam values for subsequent rows (they shifted by 1)
      int const count = (int)SendMessageW(editor->detaillist, LVM_GETITEMCOUNT, 0, 0);
      for (int i = (int)param_idx; i < count; i++) {
        LVITEMW lvi = {
            .mask = LVIF_PARAM,
            .iItem = i,
        };
        if (SendMessageW(editor->detaillist, LVM_GETITEMW, 0, (LPARAM)&lvi)) {
          // Skip special lParams (negative values)
          if (lvi.lParam >= 0) {
            lvi.lParam = (LPARAM)i;
            SendMessageW(editor->detaillist, LVM_SETITEMW, 0, (LPARAM)&lvi);
          }
        }
      }
    }
    break;

  case ptk_anm2_op_param_set_key:
    // If an animation item is selected and it's the changed item, update the key column
    if (sel_type == 2 && cur_sel_idx == sel_idx && cur_item_idx == item_idx) {
      if (ptk_anm2_item_is_animation(editor->doc, sel_idx, item_idx)) {
        char const *key = ptk_anm2_param_get_key(editor->doc, sel_idx, item_idx, param_idx);
        char const *value = ptk_anm2_param_get_value(editor->doc, sel_idx, item_idx, param_idx);
        detail_list_update_row(editor->detaillist, (int)param_idx, key, value);
      }
    }
    break;

  case ptk_anm2_op_param_set_value:
    // If an animation item is selected and it's the changed item, update the value column
    if (sel_type == 2 && cur_sel_idx == sel_idx && cur_item_idx == item_idx) {
      if (ptk_anm2_item_is_animation(editor->doc, sel_idx, item_idx)) {
        char const *key = ptk_anm2_param_get_key(editor->doc, sel_idx, item_idx, param_idx);
        char const *value = ptk_anm2_param_get_value(editor->doc, sel_idx, item_idx, param_idx);
        detail_list_update_row(editor->detaillist, (int)param_idx, key, value);
      }
    }
    break;

  case ptk_anm2_op_reset:
    // Full rebuild needed
    update_detail_panel(editor);
    break;

  // Operations that might invalidate the current selection
  case ptk_anm2_op_selector_remove:
    // If the removed selector was selected, the TreeView selection will change,
    // triggering TVN_SELCHANGEDW which calls update_detail_panel
    break;

  case ptk_anm2_op_item_remove:
    // If the removed item was selected, the TreeView selection will change,
    // triggering TVN_SELCHANGEDW which calls update_detail_panel
    break;

  // Operations that don't affect the detail list content directly
  case ptk_anm2_op_group_begin:
  case ptk_anm2_op_group_end:
  case ptk_anm2_op_selector_insert:
  case ptk_anm2_op_selector_set_group:
  case ptk_anm2_op_selector_move:
  case ptk_anm2_op_item_insert:
  case ptk_anm2_op_item_set_script_name:
  case ptk_anm2_op_item_move:
    // These operations don't affect the detail list
    break;

  case ptk_anm2_op_set_exclusive_support_default: {
    // Update Exclusive Support Default row if visible (sel_type == 0 or 1)
    int const row_idx = detail_list_find_row_by_lparam(editor->detaillist, DETAILLIST_LPARAM_EXCLUSIVE_SUPPORT_DEFAULT);
    if (row_idx >= 0) {
      detail_list_update_row(editor->detaillist,
                             row_idx,
                             pgettext("anm2editor", "Exclusive Support Default"),
                             ptk_anm2_get_exclusive_support_default(editor->doc) ? "1" : "");
    }
    break;
  }

  case ptk_anm2_op_set_information: {
    // Update Information row if visible (sel_type == 0 or 1)
    int const row_idx = detail_list_find_row_by_lparam(editor->detaillist, DETAILLIST_LPARAM_INFORMATION);
    if (row_idx >= 0) {
      detail_list_update_row(
          editor->detaillist, row_idx, pgettext("anm2editor", "Information"), ptk_anm2_get_information(editor->doc));
    }
    break;
  }
  }
}

// Callback for document changes - updates focus tracking, TreeView and detail list
static void on_doc_change(void *userdata,
                          enum ptk_anm2_op_type op_type,
                          size_t sel_idx,
                          size_t item_idx,
                          size_t param_idx,
                          size_t move_to_sel_idx,
                          size_t move_to_idx) {
  struct ptk_anm2editor *editor = (struct ptk_anm2editor *)userdata;
  if (!editor) {
    return;
  }

  // Clear multi-selection on document reset
  if (op_type == ptk_anm2_op_reset) {
    multisel_clear(editor);
  }

  // Clean up invalid selections when items are removed
  if (op_type == ptk_anm2_op_item_remove) {
    multisel_cleanup_invalid(editor);
  }

  // Update focus tracking
  update_focus_tracking(editor, op_type, sel_idx, item_idx, move_to_sel_idx, move_to_idx);

  // Update TreeView if it exists
  if (editor->treeview) {
    update_treeview_differential(editor, op_type, sel_idx, item_idx, move_to_sel_idx, move_to_idx);
  }

  // Update detail list if it exists
  if (editor->detaillist) {
    update_detaillist_differential(editor, op_type, sel_idx, item_idx, param_idx);
  }
}

/**
 * @brief Update toolbar button states based on current document state
 *
 * Updates Undo/Redo and Save/Save As button states.
 */
static void update_toolbar_state(struct ptk_anm2editor *editor) {
  if (!editor || !editor->toolbar || !editor->doc) {
    return;
  }

  // Update Undo/Redo button states
  SendMessageW(editor->toolbar, TB_ENABLEBUTTON, ID_EDIT_UNDO, MAKELPARAM(ptk_anm2_can_undo(editor->doc), 0));
  SendMessageW(editor->toolbar, TB_ENABLEBUTTON, ID_EDIT_REDO, MAKELPARAM(ptk_anm2_can_redo(editor->doc), 0));

  // Update Save/Save As button states
  BOOL const can_save = ptk_anm2_can_save(editor->doc) ? TRUE : FALSE;
  SendMessageW(editor->toolbar, TB_ENABLEBUTTON, ID_FILE_SAVE, MAKELPARAM(can_save, 0));
  SendMessageW(editor->toolbar, TB_ENABLEBUTTON, ID_FILE_SAVEAS, MAKELPARAM(can_save, 0));
}

static void mark_modified(struct ptk_anm2editor *editor) {
  if (!editor->modified) {
    editor->modified = true;
    update_window_title(editor);
  }
  update_toolbar_state(editor);
}

// Update the currently selected TreeView item's text without rebuilding the tree
static void update_selected_treeview_text(struct ptk_anm2editor *editor) {
  if (!editor || !editor->treeview || !editor->doc) {
    return;
  }

  HTREEITEM hItem = (HTREEITEM)(SendMessageW(editor->treeview, TVM_GETNEXTITEM, TVGN_CARET, 0));
  if (!hItem) {
    return;
  }

  TVITEMW tvi = {
      .mask = TVIF_PARAM,
      .hItem = hItem,
  };
  if (!SendMessageW(editor->treeview, TVM_GETITEMW, 0, (LPARAM)&tvi)) {
    return;
  }

  uint32_t id = 0;
  bool const is_selector = treeview_decode_lparam(tvi.lParam, &id);

  wchar_t new_text[256];
  if (is_selector) {
    size_t sel_idx = 0;
    if (!ptk_anm2_find_selector_by_id(editor->doc, id, &sel_idx)) {
      return;
    }
    format_selector_display_name(editor->doc, sel_idx, new_text, sizeof(new_text) / sizeof(new_text[0]));
  } else {
    size_t sel_idx = 0;
    size_t item_idx = 0;
    if (!ptk_anm2_find_item_by_id(editor->doc, id, &sel_idx, &item_idx)) {
      return;
    }
    format_item_display_name(editor->doc, sel_idx, item_idx, new_text, sizeof(new_text) / sizeof(new_text[0]));
  }

  tvi.mask = TVIF_TEXT;
  tvi.pszText = new_text;
  SendMessageW(editor->treeview, TVM_SETITEMW, 0, (LPARAM)&tvi);
}

// Get the editable name for a TreeView item (without type prefix)
// Returns the name in out buffer. For selector: group name, for item: item name or value
static void
get_treeview_item_editable_name(struct ptk_anm2editor *editor, LPARAM item_lparam, wchar_t *out, size_t out_len) {
  out[0] = L'\0';

  if (!editor->doc) {
    return;
  }

  uint32_t id = 0;
  bool const is_selector = treeview_decode_lparam(item_lparam, &id);

  if (is_selector) {
    size_t sel_idx = 0;
    if (!ptk_anm2_find_selector_by_id(editor->doc, id, &sel_idx)) {
      return;
    }
    char const *group = ptk_anm2_selector_get_group(editor->doc, sel_idx);
    if (group && group[0] != '\0') {
      ov_snprintf_char2wchar(out, out_len, "%1$hs", "%1$hs", group);
    }
  } else {
    size_t sel_idx = 0;
    size_t item_idx = 0;
    if (!ptk_anm2_find_item_by_id(editor->doc, id, &sel_idx, &item_idx)) {
      return;
    }
    char const *name = ptk_anm2_item_get_name(editor->doc, sel_idx, item_idx);
    if (name && name[0] != '\0') {
      ov_snprintf_char2wchar(out, out_len, "%1$hs", "%1$hs", name);
    }
  }
}

// Get lParam from TreeView item, fetching it if not provided in notification
static LPARAM get_treeview_item_lparam(HWND treeview, HTREEITEM hItem, LPARAM provided_lparam) {
  LPARAM result = 0;

  if (provided_lparam != 0) {
    result = provided_lparam;
  } else if (hItem) {
    TVITEMW tvi = {
        .mask = TVIF_PARAM,
        .hItem = hItem,
    };
    if (SendMessageW(treeview, TVM_GETITEMW, 0, (LPARAM)&tvi)) {
      result = tvi.lParam;
    }
  }

  return result;
}

// Handle TVN_BEGINLABELEDITW: set edit control text to actual name without prefix
static LRESULT handle_treeview_begin_label_edit(struct ptk_anm2editor *editor, NMTVDISPINFOW const *nmtv) {
  LPARAM const item_lparam = get_treeview_item_lparam(editor->treeview, nmtv->item.hItem, nmtv->item.lParam);

  HWND hEdit = (HWND)(SendMessageW(editor->treeview, TVM_GETEDITCONTROL, 0, 0));
  if (hEdit) {
    wchar_t edit_text[256];
    get_treeview_item_editable_name(editor, item_lparam, edit_text, sizeof(edit_text) / sizeof(edit_text[0]));
    if (edit_text[0] != L'\0') {
      SetWindowTextW(hEdit, edit_text);
    }
  }

  return FALSE; // Allow editing
}

// Update TreeView item name from edit text
// Returns true if update succeeded
static bool update_treeview_item_name(struct ptk_anm2editor *editor,
                                      LPARAM item_lparam,
                                      wchar_t const *new_name_w,
                                      struct ov_error *const err) {
  if (!new_name_w || !editor->doc) {
    return false;
  }

  char *utf8_buf = NULL;
  char *new_name_utf8 = NULL;
  bool success = false;

  // Convert wide string to UTF-8
  {
    size_t const wlen = wcslen(new_name_w);
    size_t const utf8_buf_len = (wlen + 1) * 4;

    if (!OV_ARRAY_GROW(&utf8_buf, utf8_buf_len)) {
      OV_ERROR_SET_GENERIC(err, ov_error_generic_out_of_memory);
      goto cleanup;
    }

    size_t const utf8_written = ov_wchar_to_utf8(new_name_w, wlen, utf8_buf, utf8_buf_len, NULL);
    if (utf8_written == 0 && wlen > 0) {
      OV_ERROR_SET_GENERIC(err, ov_error_generic_fail);
      goto cleanup;
    }

    if (!OV_ARRAY_GROW(&new_name_utf8, utf8_written + 1)) {
      OV_ERROR_SET_GENERIC(err, ov_error_generic_out_of_memory);
      goto cleanup;
    }
    memcpy(new_name_utf8, utf8_buf, utf8_written + 1);
    OV_ARRAY_SET_LENGTH(new_name_utf8, utf8_written);
  }

  // Update data structure using anm2 API
  {
    uint32_t id = 0;
    bool const is_selector = treeview_decode_lparam(item_lparam, &id);

    if (is_selector) {
      size_t sel_idx = 0;
      if (!ptk_anm2_find_selector_by_id(editor->doc, id, &sel_idx)) {
        OV_ERROR_SET_GENERIC(err, ov_error_generic_invalid_argument);
        goto cleanup;
      }
      if (!ptk_anm2_selector_set_group(editor->doc, sel_idx, new_name_utf8, err)) {
        OV_ERROR_ADD_TRACE(err);
        goto cleanup;
      }
      mark_modified(editor);
      success = true;
    } else {
      size_t sel_idx = 0;
      size_t item_idx = 0;
      if (!ptk_anm2_find_item_by_id(editor->doc, id, &sel_idx, &item_idx)) {
        OV_ERROR_SET_GENERIC(err, ov_error_generic_invalid_argument);
        goto cleanup;
      }
      if (!ptk_anm2_item_set_name(editor->doc, sel_idx, item_idx, new_name_utf8, err)) {
        OV_ERROR_ADD_TRACE(err);
        goto cleanup;
      }
      mark_modified(editor);
      update_detail_panel(editor);
      success = true;
    }
  }

cleanup:
  if (utf8_buf) {
    OV_ARRAY_DESTROY(&utf8_buf);
  }
  if (new_name_utf8) {
    OV_ARRAY_DESTROY(&new_name_utf8);
  }
  return success;
}

// Handle TVN_ENDLABELEDITW: update data structure and TreeView text with proper prefix
static LRESULT handle_treeview_end_label_edit(struct ptk_anm2editor *editor, NMTVDISPINFOW const *nmtv) {
  bool const adding_new = editor->adding_new_selector;
  editor->adding_new_selector = false;

  // Handle cancelled edit or empty text
  if (!nmtv->item.pszText || nmtv->item.pszText[0] == L'\0') {
    if (adding_new) {
      // Remove the temporary tree item
      SendMessageW(editor->treeview, TVM_DELETEITEM, 0, (LPARAM)nmtv->item.hItem);
    }
    return FALSE;
  }

  struct ov_error err = {0};
  bool success = false;

  if (adding_new) {
    // Convert wide string to UTF-8 for adding selector
    char name_utf8[256] = {0};
    ov_snprintf_wchar2char(name_utf8, sizeof(name_utf8), L"%1$ls", L"%1$ls", nmtv->item.pszText);

    // Delete the temporary tree item first, before the document operation
    // The on_doc_change callback will insert the real item
    SendMessageW(editor->treeview, TVM_DELETEITEM, 0, (LPARAM)nmtv->item.hItem);

    if (!ptk_anm2_selector_add(editor->doc, name_utf8, &err)) {
      OV_ERROR_ADD_TRACE(&err);
      goto cleanup;
    }

    mark_modified(editor);
  } else {
    LPARAM const item_lparam = get_treeview_item_lparam(editor->treeview, nmtv->item.hItem, nmtv->item.lParam);
    if (!update_treeview_item_name(editor, item_lparam, nmtv->item.pszText, &err)) {
      OV_ERROR_ADD_TRACE(&err);
      goto cleanup;
    }
    update_selected_treeview_text(editor);
  }

  success = true;

cleanup:
  if (!success) {
    if (adding_new) {
      // Remove the temporary tree item on failure
      SendMessageW(editor->treeview, TVM_DELETEITEM, 0, (LPARAM)nmtv->item.hItem);
    }
    show_error_dialog(editor, &err);
  }
  // Return FALSE to reject the raw edit text (we've already updated with proper prefix)
  return FALSE;
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
  if (!editor) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_invalid_argument);
    return false;
  }
  if (editor->doc) {
    ptk_anm2_destroy(&editor->doc);
  }
  // Create a new empty document
  editor->doc = ptk_anm2_new(err);
  if (!editor->doc) {
    OV_ERROR_ADD_TRACE(err);
    return false;
  }

  // Register change callback for the new document
  ptk_anm2_set_change_callback(editor->doc, on_doc_change, editor);

  if (editor->file_path) {
    OV_ARRAY_DESTROY(&editor->file_path);
  }
  editor->modified = false;
  return true;
}

/**
 * @brief Refresh all UI views to reflect current document state
 *
 * Updates TreeView, detail panel, and window title.
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
  if (!refresh_treeview(editor, err)) {
    OV_ERROR_ADD_TRACE(err);
    return false;
  }
  update_detail_panel(editor);
  update_window_title(editor);
  update_toolbar_state(editor);
  return true;
}

// Cancel inline edit without saving
static void cancel_inline_edit(struct ptk_anm2editor *editor) {
  if (!editor || !editor->edit_control || editor->edit_committing) {
    return;
  }

  editor->edit_committing = true;

  // Save state before modifying
  HWND edit_hwnd = editor->edit_control;
  WNDPROC old_proc = editor->edit_oldproc;

  // Clear state first
  editor->edit_control = NULL;
  editor->edit_row = -1;
  editor->edit_column = -1;
  editor->edit_lparam = 0;
  editor->edit_oldproc = NULL;
  editor->edit_adding_new = false;

  // Remove property and restore original window procedure before destroying
  RemovePropW(edit_hwnd, L"ptk_editor");
  if (old_proc) {
    SetWindowLongPtrW(edit_hwnd, GWLP_WNDPROC, (LONG_PTR)old_proc);
  }
  DestroyWindow(edit_hwnd);

  editor->edit_committing = false;
}

// Commit inline edit and update data
static void commit_inline_edit(struct ptk_anm2editor *editor) {
  if (!editor || !editor->edit_control || editor->edit_row < 0 || editor->edit_committing || !editor->doc) {
    return;
  }

  editor->edit_committing = true;

  bool success = false;
  struct ov_error err = {0};

  // Get new value from edit control
  int const text_len = GetWindowTextLengthW(editor->edit_control);
  wchar_t *new_value_w = NULL;
  char *utf8_buf = NULL;
  char *new_value_utf8 = NULL;

  if (text_len > 0) {
    if (!OV_ARRAY_GROW(&new_value_w, (size_t)text_len + 1)) {
      OV_ERROR_SET_GENERIC(&err, ov_error_generic_out_of_memory);
      goto cleanup;
    }
    GetWindowTextW(editor->edit_control, new_value_w, text_len + 1);
    OV_ARRAY_SET_LENGTH(new_value_w, (size_t)text_len);

    // Convert to UTF-8 using temporary buffer
    size_t const utf8_buf_len = (size_t)(text_len + 1) * 4; // Worst case
    if (!OV_ARRAY_GROW(&utf8_buf, utf8_buf_len)) {
      OV_ERROR_SET_GENERIC(&err, ov_error_generic_out_of_memory);
      goto cleanup;
    }
    size_t const utf8_written = ov_wchar_to_utf8(new_value_w, (size_t)text_len, utf8_buf, utf8_buf_len, NULL);
    // ov_wchar_to_utf8 already null-terminates the output

    // Copy to OV_ARRAY_GROW-allocated memory
    if (!OV_ARRAY_GROW(&new_value_utf8, utf8_written + 1)) {
      OV_ERROR_SET_GENERIC(&err, ov_error_generic_out_of_memory);
      goto cleanup;
    }
    memcpy(new_value_utf8, utf8_buf, utf8_written + 1);
    OV_ARRAY_SET_LENGTH(new_value_utf8, utf8_written);
  }

  // Handle special rows (Label, PSD Path, Exclusive Support Default, Information)
  {
    char const *value = new_value_utf8 ? new_value_utf8 : "";

    if (detaillist_is_label_row(editor->edit_lparam)) {
      // Editing Label row - skip if unchanged
      char const *current = ptk_anm2_get_label(editor->doc);
      if (strcmp(value, current ? current : "") == 0) {
        success = true;
        goto cleanup;
      }
      if (!ptk_anm2_set_label(editor->doc, value, &err)) {
        OV_ERROR_ADD_TRACE(&err);
        goto cleanup;
      }
      mark_modified(editor);
      success = true;
      goto cleanup;
    }

    if (detaillist_is_psd_path_row(editor->edit_lparam)) {
      // Editing PSD Path row - skip if unchanged
      char const *current = ptk_anm2_get_psd_path(editor->doc);
      if (strcmp(value, current ? current : "") == 0) {
        success = true;
        goto cleanup;
      }
      if (!ptk_anm2_set_psd_path(editor->doc, value, &err)) {
        OV_ERROR_ADD_TRACE(&err);
        goto cleanup;
      }
      mark_modified(editor);
      success = true;
      goto cleanup;
    }

    if (detaillist_is_exclusive_support_default_row(editor->edit_lparam)) {
      // Editing Exclusive Support Default row
      // "0" or empty = false, anything else = true
      bool new_value = (value[0] != '\0' && value[0] != '0');
      // Skip if unchanged
      if (new_value == ptk_anm2_get_exclusive_support_default(editor->doc)) {
        success = true;
        goto cleanup;
      }
      if (!ptk_anm2_set_exclusive_support_default(editor->doc, new_value, &err)) {
        OV_ERROR_ADD_TRACE(&err);
        goto cleanup;
      }
      mark_modified(editor);
      success = true;
      goto cleanup;
    }

    if (detaillist_is_information_row(editor->edit_lparam)) {
      // Editing Information row
      // Empty string means auto-generate (NULL)
      char const *info_value = (value[0] == '\0') ? NULL : value;
      // Skip if unchanged
      char const *current = ptk_anm2_get_information(editor->doc);
      if ((info_value == NULL && current == NULL) ||
          (info_value != NULL && current != NULL && strcmp(info_value, current) == 0)) {
        success = true;
        goto cleanup;
      }
      if (!ptk_anm2_set_information(editor->doc, info_value, &err)) {
        OV_ERROR_ADD_TRACE(&err);
        goto cleanup;
      }
      mark_modified(editor);
      success = true;
      goto cleanup;
    }
  }

  // Multi-selection mode: lParam contains item ID
  if (detaillist_is_multisel_item(editor->edit_lparam)) {
    uint32_t const item_id = (uint32_t)editor->edit_lparam;
    size_t sel_idx = 0;
    size_t item_idx = 0;
    if (ptk_anm2_find_item_by_id(editor->doc, item_id, &sel_idx, &item_idx)) {
      // Value items only in multi-selection mode (animation items are skipped)
      char const *value = new_value_utf8 ? new_value_utf8 : "";
      if (editor->edit_column == 0) {
        // Editing Name column
        char const *current = ptk_anm2_item_get_name(editor->doc, sel_idx, item_idx);
        if (strcmp(value, current ? current : "") != 0) {
          if (!ptk_anm2_item_set_name(editor->doc, sel_idx, item_idx, value, &err)) {
            OV_ERROR_ADD_TRACE(&err);
            goto cleanup;
          }
          mark_modified(editor);
        }
      } else {
        // Editing Value column
        char const *current = ptk_anm2_item_get_value(editor->doc, sel_idx, item_idx);
        if (strcmp(value, current ? current : "") != 0) {
          if (!ptk_anm2_item_set_value(editor->doc, sel_idx, item_idx, value, &err)) {
            OV_ERROR_ADD_TRACE(&err);
            goto cleanup;
          }
          mark_modified(editor);
        }
      }
    }
    success = true;
    goto cleanup;
  }

  // Get currently selected item to update
  {
    size_t sel_idx = 0;
    size_t item_idx = 0;
    int const sel_type = get_selected_indices(editor, &sel_idx, &item_idx);

    if (sel_type == 2 && sel_idx < ptk_anm2_selector_count(editor->doc)) {
      if (item_idx < ptk_anm2_item_count(editor->doc, sel_idx)) {
        bool const is_animation = ptk_anm2_item_is_animation(editor->doc, sel_idx, item_idx);
        char const *value = new_value_utf8 ? new_value_utf8 : "";

        if (editor->edit_adding_new) {
          // Adding new parameter - only add if key is not empty
          if (is_animation && value[0] != '\0') {
            if (!ptk_anm2_param_add(editor->doc, sel_idx, item_idx, value, "", &err)) {
              OV_ERROR_ADD_TRACE(&err);
              goto cleanup;
            }
            mark_modified(editor);
            update_detail_panel(editor);
          }
          // If empty, just cancel (do nothing)
        } else if (editor->edit_column == 0) {
          // Editing Name column
          if (is_animation) {
            // Animation item - update parameter key
            if ((size_t)editor->edit_row < ptk_anm2_param_count(editor->doc, sel_idx, item_idx)) {
              // Skip if unchanged
              char const *current = ptk_anm2_param_get_key(editor->doc, sel_idx, item_idx, (size_t)editor->edit_row);
              if (strcmp(value, current ? current : "") != 0) {
                if (!ptk_anm2_param_set_key(editor->doc, sel_idx, item_idx, (size_t)editor->edit_row, value, &err)) {
                  OV_ERROR_ADD_TRACE(&err);
                  goto cleanup;
                }
                mark_modified(editor);
              }
            }
          } else {
            // Value item - update item name
            if (editor->edit_row == 0) {
              // Skip if unchanged
              char const *current = ptk_anm2_item_get_name(editor->doc, sel_idx, item_idx);
              if (strcmp(value, current ? current : "") != 0) {
                if (!ptk_anm2_item_set_name(editor->doc, sel_idx, item_idx, value, &err)) {
                  OV_ERROR_ADD_TRACE(&err);
                  goto cleanup;
                }
                mark_modified(editor);
                // Update TreeView to reflect new name
                update_selected_treeview_text(editor);
              }
            }
          }
        } else {
          // Editing Value column (column == 1)
          if (is_animation) {
            // Animation item - update parameter value at edit_row
            if ((size_t)editor->edit_row < ptk_anm2_param_count(editor->doc, sel_idx, item_idx)) {
              // Skip if unchanged
              char const *current = ptk_anm2_param_get_value(editor->doc, sel_idx, item_idx, (size_t)editor->edit_row);
              if (strcmp(value, current ? current : "") != 0) {
                if (!ptk_anm2_param_set_value(editor->doc, sel_idx, item_idx, (size_t)editor->edit_row, value, &err)) {
                  OV_ERROR_ADD_TRACE(&err);
                  goto cleanup;
                }
                mark_modified(editor);
              }
            }
          } else {
            // Value item - update value (only one row, so edit_row should be 0)
            if (editor->edit_row == 0) {
              // Skip if unchanged
              char const *current = ptk_anm2_item_get_value(editor->doc, sel_idx, item_idx);
              if (strcmp(value, current ? current : "") != 0) {
                if (!ptk_anm2_item_set_value(editor->doc, sel_idx, item_idx, value, &err)) {
                  OV_ERROR_ADD_TRACE(&err);
                  goto cleanup;
                }
                mark_modified(editor);
              }
            }
          }
        }
      }
    }
  }

  success = true;

cleanup:
  if (!success) {
    show_error_dialog(editor, &err);
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

  // Destroy edit control directly (can't call cancel_inline_edit due to reentrancy guard)
  if (editor->edit_control) {
    HWND edit_hwnd = editor->edit_control;
    WNDPROC old_proc = editor->edit_oldproc;

    editor->edit_control = NULL;
    editor->edit_row = -1;
    editor->edit_column = -1;
    editor->edit_lparam = 0;
    editor->edit_oldproc = NULL;
    editor->edit_adding_new = false;

    RemovePropW(edit_hwnd, L"ptk_editor");
    if (old_proc) {
      SetWindowLongPtrW(edit_hwnd, GWLP_WNDPROC, (LONG_PTR)old_proc);
    }
    DestroyWindow(edit_hwnd);
  }

  editor->edit_committing = false;

  // Refresh detail panel to show updated value
  update_detail_panel(editor);
}

// Subclass procedure for inline edit control
static LRESULT CALLBACK edit_subclass_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
  struct ptk_anm2editor *editor = (struct ptk_anm2editor *)GetPropW(hwnd, L"ptk_editor");
  if (!editor) {
    return DefWindowProcW(hwnd, msg, wparam, lparam);
  }

  switch (msg) {
  case WM_KEYDOWN:
    if (wparam == VK_RETURN) {
      commit_inline_edit(editor);
      return 0;
    }
    if (wparam == VK_ESCAPE) {
      cancel_inline_edit(editor);
      return 0;
    }
    break;
  case WM_KILLFOCUS:
    // Commit when losing focus
    commit_inline_edit(editor);
    return 0;
  }

  return CallWindowProcW(editor->edit_oldproc, hwnd, msg, wparam, lparam);
}

// Start inline editing for a row/column in the detail list
static void start_inline_edit(struct ptk_anm2editor *editor, int row, int column) {
  if (!editor || !editor->detaillist || !editor->doc || row < 0 || column < 0 || column > 1) {
    return;
  }

  // Get lParam to check if this row is editable
  LVITEMW check_lvi = {
      .mask = LVIF_PARAM,
      .iItem = row,
  };
  SendMessageW(editor->detaillist, LVM_GETITEMW, 0, (LPARAM)&check_lvi);

  // Check if this is a special editable row (Label, PSD Path, Exclusive Support Default, Information)
  bool const is_label_row = detaillist_is_label_row(check_lvi.lParam);
  bool const is_psd_path_row = detaillist_is_psd_path_row(check_lvi.lParam);
  bool const is_exclusive_support_default_row = detaillist_is_exclusive_support_default_row(check_lvi.lParam);
  bool const is_information_row = detaillist_is_information_row(check_lvi.lParam);

  if (is_label_row || is_psd_path_row || is_exclusive_support_default_row || is_information_row) {
    // These rows can only edit the Value column (column 1)
    if (column != 1) {
      return;
    }
  } else {
    // Placeholder and NOT_EDITABLE rows cannot be edited inline
    if (!detaillist_is_editable_param(check_lvi.lParam)) {
      return;
    }

    // Check if editing is allowed for this cell based on item type
    size_t sel_idx = 0;
    size_t item_idx = 0;
    int const sel_type = get_selected_indices(editor, &sel_idx, &item_idx);
    if (sel_type != 2) {
      return;
    }
    if (sel_idx >= ptk_anm2_selector_count(editor->doc)) {
      return;
    }
    if (item_idx >= ptk_anm2_item_count(editor->doc, sel_idx)) {
      return;
    }
    bool const is_animation = ptk_anm2_item_is_animation(editor->doc, sel_idx, item_idx);
    (void)is_animation; // Both animation and value items are editable
  }

  // Cancel any existing edit
  cancel_inline_edit(editor);

  // Get the rectangle for the specified column
  RECT rc = {0};
  if (column == 0) {
    // For column 0 (Name), get the label rect
    rc.left = LVIR_LABEL;
    if (!SendMessageW(editor->detaillist, LVM_GETITEMRECT, (WPARAM)row, (LPARAM)&rc)) {
      return;
    }
  } else {
    // For column 1 (Value), get the subitem rect
    rc.top = 1;
    rc.left = LVIR_BOUNDS;
    if (!SendMessageW(editor->detaillist, LVM_GETSUBITEMRECT, (WPARAM)row, (LPARAM)&rc)) {
      return;
    }
  }

  // Get current text from the specified column
  wchar_t text[512] = {0};
  SendMessageW(editor->detaillist,
               LVM_GETITEMTEXTW,
               (WPARAM)row,
               (LPARAM) & (LVITEMW){
                              .mask = LVIF_TEXT,
                              .iItem = row,
                              .iSubItem = column,
                              .pszText = text,
                              .cchTextMax = sizeof(text) / sizeof(text[0]),
                          });

  // Create edit control
  editor->edit_control = CreateWindowExW(0,
                                         L"EDIT",
                                         text,
                                         WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL,
                                         rc.left,
                                         rc.top,
                                         rc.right - rc.left,
                                         rc.bottom - rc.top,
                                         editor->detaillist,
                                         NULL,
                                         GetModuleHandleW(NULL),
                                         NULL);

  if (!editor->edit_control) {
    return;
  }

  editor->edit_row = row;
  editor->edit_column = column;
  editor->edit_lparam = check_lvi.lParam;
  editor->edit_adding_new = false;

  // Store editor pointer in edit control
  SetPropW(editor->edit_control, L"ptk_editor", editor);

  // Subclass the edit control
  editor->edit_oldproc = (WNDPROC)(SetWindowLongPtrW(editor->edit_control, GWLP_WNDPROC, (LONG_PTR)edit_subclass_proc));

  // Set font to match ListView
  HFONT hFont = (HFONT)(SendMessageW(editor->detaillist, WM_GETFONT, 0, 0));
  if (hFont) {
    SendMessageW(editor->edit_control, WM_SETFONT, (WPARAM)hFont, TRUE);
  }

  // Select all text and focus
  SendMessageW(editor->edit_control, EM_SETSEL, 0, -1);
  SetFocus(editor->edit_control);
}

// Start inline editing for adding a new parameter
// Shows edit control at the placeholder row position with empty text
static void start_inline_edit_for_new(struct ptk_anm2editor *editor) {
  size_t sel_idx = 0;
  size_t item_idx = 0;
  int sel_type = 0;
  int placeholder_row = 0;
  RECT rc = {0};

  if (!editor || !editor->detaillist || !editor->doc) {
    return;
  }

  sel_type = get_selected_indices(editor, &sel_idx, &item_idx);
  if (sel_type != 2) {
    return;
  }
  if (sel_idx >= ptk_anm2_selector_count(editor->doc)) {
    return;
  }
  if (item_idx >= ptk_anm2_item_count(editor->doc, sel_idx)) {
    return;
  }
  if (!ptk_anm2_item_is_animation(editor->doc, sel_idx, item_idx)) {
    return;
  }

  // Cancel any existing edit
  cancel_inline_edit(editor);

  // Find the placeholder row (last row in ListView)
  placeholder_row = (int)SendMessageW(editor->detaillist, LVM_GETITEMCOUNT, 0, 0) - 1;
  if (placeholder_row < 0) {
    return;
  }

  // Get the rectangle for column 0 (Name/Key column)
  rc.left = LVIR_LABEL;
  if (!SendMessageW(editor->detaillist, LVM_GETITEMRECT, (WPARAM)placeholder_row, (LPARAM)&rc)) {
    return;
  }

  // Create edit control with empty text
  editor->edit_control = CreateWindowExW(0,
                                         L"EDIT",
                                         L"",
                                         WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL,
                                         rc.left,
                                         rc.top,
                                         rc.right - rc.left,
                                         rc.bottom - rc.top,
                                         editor->detaillist,
                                         NULL,
                                         GetModuleHandleW(NULL),
                                         NULL);

  if (!editor->edit_control) {
    return;
  }

  editor->edit_row = placeholder_row;
  editor->edit_column = 0;
  editor->edit_adding_new = true;

  // Store editor pointer in edit control
  SetPropW(editor->edit_control, L"ptk_editor", editor);

  // Subclass the edit control
  editor->edit_oldproc = (WNDPROC)(SetWindowLongPtrW(editor->edit_control, GWLP_WNDPROC, (LONG_PTR)edit_subclass_proc));

  // Set font to match ListView
  HFONT hFont = (HFONT)(SendMessageW(editor->detaillist, WM_GETFONT, 0, 0));
  if (hFont) {
    SendMessageW(editor->edit_control, WM_SETFONT, (WPARAM)hFont, TRUE);
  }

  // Focus the edit control
  SetFocus(editor->edit_control);
}

// Move selector up in the array
static bool move_selector_up(struct ptk_anm2editor *editor, size_t idx, struct ov_error *const err) {
  if (!editor->doc || idx == 0) {
    return false;
  }
  size_t const len = ptk_anm2_selector_count(editor->doc);
  if (idx >= len) {
    return false;
  }

  if (!ptk_anm2_selector_move_to(editor->doc, idx, idx - 1, err)) {
    OV_ERROR_ADD_TRACE(err);
    return false;
  }

  mark_modified(editor);
  return true;
}

// Move selector down in the array
static bool move_selector_down(struct ptk_anm2editor *editor, size_t idx, struct ov_error *const err) {
  if (!editor->doc) {
    return false;
  }
  size_t const len = ptk_anm2_selector_count(editor->doc);
  if (idx + 1 >= len) {
    return false;
  }

  if (!ptk_anm2_selector_move_to(editor->doc, idx, idx + 1, err)) {
    OV_ERROR_ADD_TRACE(err);
    return false;
  }

  mark_modified(editor);
  return true;
}

// Move item up within its selector
static bool move_item_up(struct ptk_anm2editor *editor, size_t sel_idx, size_t item_idx, struct ov_error *const err) {
  if (!editor->doc || item_idx == 0) {
    return false;
  }
  if (sel_idx >= ptk_anm2_selector_count(editor->doc)) {
    return false;
  }
  size_t const items_len = ptk_anm2_item_count(editor->doc, sel_idx);
  if (item_idx >= items_len) {
    return false;
  }

  if (!ptk_anm2_item_move_to(editor->doc, sel_idx, item_idx, sel_idx, item_idx - 1, err)) {
    OV_ERROR_ADD_TRACE(err);
    return false;
  }

  mark_modified(editor);
  return true;
}

// Move item down within its selector
static bool move_item_down(struct ptk_anm2editor *editor, size_t sel_idx, size_t item_idx, struct ov_error *const err) {
  if (!editor->doc) {
    return false;
  }
  if (sel_idx >= ptk_anm2_selector_count(editor->doc)) {
    return false;
  }
  size_t const items_len = ptk_anm2_item_count(editor->doc, sel_idx);
  if (item_idx + 1 >= items_len) {
    return false;
  }

  if (!ptk_anm2_item_move_to(editor->doc, sel_idx, item_idx, sel_idx, item_idx + 1, err)) {
    OV_ERROR_ADD_TRACE(err);
    return false;
  }

  mark_modified(editor);
  return true;
}

// Delete a selector
static bool delete_selector(struct ptk_anm2editor *editor, size_t idx, struct ov_error *const err) {
  if (!editor->doc) {
    return false;
  }
  if (idx >= ptk_anm2_selector_count(editor->doc)) {
    return false;
  }

  if (!ptk_anm2_selector_remove(editor->doc, idx, err)) {
    OV_ERROR_ADD_TRACE(err);
    return false;
  }

  mark_modified(editor);
  return true;
}

// Delete an item from a selector
static bool delete_item(struct ptk_anm2editor *editor, size_t sel_idx, size_t item_idx, struct ov_error *const err) {
  if (!editor->doc) {
    return false;
  }
  if (sel_idx >= ptk_anm2_selector_count(editor->doc)) {
    return false;
  }
  if (item_idx >= ptk_anm2_item_count(editor->doc, sel_idx)) {
    return false;
  }

  if (!ptk_anm2_item_remove(editor->doc, sel_idx, item_idx, err)) {
    OV_ERROR_ADD_TRACE(err);
    return false;
  }

  mark_modified(editor);
  return true;
}

// Helper function for swapping items in reverse operation
static bool
reverse_selector_items_swap(struct ptk_anm2 *doc, size_t sel_idx, size_t i, size_t other, struct ov_error *const err) {
  // Swap positions: move i to other, then move other-1 to i
  // Move_to shifts elements, so we need to be careful with indices
  if (!ptk_anm2_item_move_to(doc, sel_idx, i, sel_idx, other, err)) {
    OV_ERROR_ADD_TRACE(err);
    return false;
  }
  if (i + 1 < other) {
    if (!ptk_anm2_item_move_to(doc, sel_idx, other - 1, sel_idx, i, err)) {
      OV_ERROR_ADD_TRACE(err);
      return false;
    }
  }
  return true;
}

// Reverse items in a selector
static bool reverse_selector_items(struct ptk_anm2editor *editor, size_t sel_idx, struct ov_error *const err) {
  if (!editor->doc) {
    return false;
  }
  if (sel_idx >= ptk_anm2_selector_count(editor->doc)) {
    return false;
  }
  size_t const items_len = ptk_anm2_item_count(editor->doc, sel_idx);
  if (items_len < 2) {
    return false;
  }

  bool success = false;

  // Use transaction to group all moves as single undo
  if (!ptk_anm2_begin_transaction(editor->doc, err)) {
    OV_ERROR_ADD_TRACE(err);
    return false;
  }

  // Reverse by moving items
  for (size_t i = 0; i < items_len / 2; i++) {
    size_t const other = items_len - 1 - i;
    if (!reverse_selector_items_swap(editor->doc, sel_idx, i, other, err)) {
      OV_ERROR_ADD_TRACE(err);
      goto cleanup;
    }
  }

  success = true;

cleanup:
  if (!ptk_anm2_end_transaction(editor->doc, success ? err : NULL)) {
    if (success) {
      OV_ERROR_ADD_TRACE(err);
    }
    return false;
  }

  if (success) {
    mark_modified(editor);
  }
  return success;
}

// Delete the selected parameter from the detail list
// Returns true if nothing to delete or deletion succeeded, false only on actual error
static bool delete_selected_parameter(struct ptk_anm2editor *editor, struct ov_error *const err) {
  bool success = false;
  int selected_row = 0;
  LVITEMW lvi = {0};
  size_t sel_idx = 0;
  size_t item_idx = 0;
  int sel_type = 0;
  size_t param_idx = 0;

  if (!editor || !editor->detaillist || !editor->doc) {
    // Nothing to do - not an error
    success = true;
    goto cleanup;
  }

  // Get selected row in detail list
  selected_row = (int)(SendMessageW(editor->detaillist, LVM_GETNEXTITEM, (WPARAM)-1, LVNI_SELECTED));
  if (selected_row < 0) {
    // No selection - not an error
    success = true;
    goto cleanup;
  }

  // Get lParam to check if this row is deletable
  lvi.mask = LVIF_PARAM;
  lvi.iItem = selected_row;
  SendMessageW(editor->detaillist, LVM_GETITEMW, 0, (LPARAM)&lvi);

  if (!detaillist_is_editable_param(lvi.lParam)) {
    // Placeholder or NOT_EDITABLE rows cannot be deleted - not an error
    success = true;
    goto cleanup;
  }

  sel_type = get_selected_indices(editor, &sel_idx, &item_idx);
  if (sel_type != 2) {
    // No item selected - not an error
    success = true;
    goto cleanup;
  }
  if (sel_idx >= ptk_anm2_selector_count(editor->doc)) {
    success = true;
    goto cleanup;
  }
  if (item_idx >= ptk_anm2_item_count(editor->doc, sel_idx)) {
    success = true;
    goto cleanup;
  }
  if (!ptk_anm2_item_is_animation(editor->doc, sel_idx, item_idx)) {
    // Only Animation items have deletable parameters - not an error
    success = true;
    goto cleanup;
  }

  param_idx = detaillist_get_param_index(lvi.lParam);
  if (param_idx >= ptk_anm2_param_count(editor->doc, sel_idx, item_idx)) {
    success = true;
    goto cleanup;
  }

  if (!ptk_anm2_param_remove(editor->doc, sel_idx, item_idx, param_idx, err)) {
    OV_ERROR_ADD_TRACE(err);
    goto cleanup;
  }

  mark_modified(editor);
  update_detail_panel(editor);
  success = true;

cleanup:
  return success;
}

// Command handler for ID_FILE_NEW
static void handle_cmd_file_new(struct ptk_anm2editor *editor) {
  struct ov_error err = {0};
  bool success = false;

  if (!ptk_anm2editor_new(editor, &err)) {
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

// Command handler for ID_EDIT_ADD_SELECTOR
// Adds a temporary tree item and starts label editing.
// The actual selector is added when the user confirms with a non-empty name.
static void handle_cmd_add_selector(struct ptk_anm2editor *editor) {
  if (!editor || !editor->treeview || !editor->doc) {
    return;
  }

  // Insert a temporary item at the end of the tree
  HTREEITEM hNewItem =
      (HTREEITEM)(SendMessageW(editor->treeview,
                               TVM_INSERTITEMW,
                               0,
                               (LPARAM) & (TVINSERTSTRUCTW){
                                              .hParent = TVI_ROOT,
                                              .hInsertAfter = TVI_LAST,
                                              .item =
                                                  {
                                                      .mask = TVIF_TEXT | TVIF_PARAM,
                                                      .pszText = L"",
                                                      .lParam = (LPARAM)-1, // Special marker for new item
                                                  },
                                          }));

  if (hNewItem) {
    editor->adding_new_selector = true;
    SendMessageW(editor->treeview, TVM_SELECTITEM, TVGN_CARET, (LPARAM)hNewItem);
    SendMessageW(editor->treeview, TVM_EDITLABELW, 0, (LPARAM)hNewItem);
  }
}

// Delete a single item by ID (helper for multi-selection delete loop)
static bool delete_item_by_id(struct ptk_anm2editor *editor, uint32_t id, struct ov_error *const err) {
  size_t item_sel_idx = 0;
  size_t item_item_idx = 0;
  if (!ptk_anm2_find_item_by_id(editor->doc, id, &item_sel_idx, &item_item_idx)) {
    return true; // Item already deleted or doesn't exist - not an error
  }
  if (!delete_item(editor, item_sel_idx, item_item_idx, err)) {
    OV_ERROR_ADD_TRACE(err);
    return false;
  }
  return true;
}

// Delete all multi-selected items in a transaction
static bool delete_multisel_items(struct ptk_anm2editor *editor, struct ov_error *const err) {
  bool success = false;

  if (!ptk_anm2_begin_transaction(editor->doc, err)) {
    OV_ERROR_ADD_TRACE(err);
    goto cleanup;
  }

  {
    uint32_t const *selected_ids = multisel_get_ids(editor);
    size_t const count = multisel_count(editor);
    for (size_t i = 0; i < count; i++) {
      if (!delete_item_by_id(editor, selected_ids[i], err)) {
        OV_ERROR_ADD_TRACE(err);
        goto cleanup;
      }
    }
  }

  if (!ptk_anm2_end_transaction(editor->doc, err)) {
    OV_ERROR_ADD_TRACE(err);
    goto cleanup;
  }

  multisel_clear(editor);
  InvalidateRect(editor->treeview, NULL, FALSE);
  success = true;

cleanup:
  if (!success) {
    struct ov_error end_err = {0};
    if (!ptk_anm2_end_transaction(editor->doc, &end_err)) {
      ptk_logf_error(&end_err, "%1$hs", "%1$hs", gettext("failed to end transaction in error recovery"));
      OV_ERROR_DESTROY(&end_err);
    }
  }
  return success;
}

// Command handler for ID_EDIT_DELETE with multi-selection support
static void handle_cmd_delete(struct ptk_anm2editor *editor) {
  struct ov_error err = {0};
  bool success = false;
  size_t sel_idx = 0;
  size_t item_idx = 0;

  if (multisel_count(editor) > 1) {
    if (!delete_multisel_items(editor, &err)) {
      OV_ERROR_ADD_TRACE(&err);
      goto cleanup;
    }
  } else {
    int const sel_type = get_selected_indices(editor, &sel_idx, &item_idx);
    if (sel_type == 1) {
      if (!delete_selector(editor, sel_idx, &err)) {
        OV_ERROR_ADD_TRACE(&err);
        goto cleanup;
      }
    } else if (sel_type == 2) {
      if (!delete_item(editor, sel_idx, item_idx, &err)) {
        OV_ERROR_ADD_TRACE(&err);
        goto cleanup;
      }
      multisel_clear(editor);
    }
  }

  success = true;

cleanup:
  if (!success) {
    show_error_dialog(editor, &err);
  }
}

// Command handler for ID_EDIT_REVERSE
static void handle_cmd_reverse(struct ptk_anm2editor *editor) {
  struct ov_error err = {0};
  bool success = false;
  size_t sel_idx = 0;
  size_t item_idx = 0;

  int const sel_type = get_selected_indices(editor, &sel_idx, &item_idx);
  if (sel_type == 1 || sel_type == 2) {
    if (!reverse_selector_items(editor, sel_idx, &err)) {
      OV_ERROR_ADD_TRACE(&err);
      goto cleanup;
    }
  }

  success = true;

cleanup:
  if (!success) {
    show_error_dialog(editor, &err);
  }
}

// Command handler for ID_EDIT_MOVE_UP
static void handle_cmd_move_up(struct ptk_anm2editor *editor) {
  struct ov_error err = {0};
  bool success = false;
  size_t sel_idx = 0;
  size_t item_idx = 0;

  int const sel_type = get_selected_indices(editor, &sel_idx, &item_idx);
  if (sel_type == 1) {
    if (!move_selector_up(editor, sel_idx, &err)) {
      OV_ERROR_ADD_TRACE(&err);
      goto cleanup;
    }
  } else if (sel_type == 2) {
    if (!move_item_up(editor, sel_idx, item_idx, &err)) {
      OV_ERROR_ADD_TRACE(&err);
      goto cleanup;
    }
  }

  success = true;

cleanup:
  if (!success) {
    show_error_dialog(editor, &err);
  }
}

// Command handler for ID_EDIT_MOVE_DOWN
static void handle_cmd_move_down(struct ptk_anm2editor *editor) {
  struct ov_error err = {0};
  bool success = false;
  size_t sel_idx = 0;
  size_t item_idx = 0;

  int const sel_type = get_selected_indices(editor, &sel_idx, &item_idx);
  if (sel_type == 1) {
    if (!move_selector_down(editor, sel_idx, &err)) {
      OV_ERROR_ADD_TRACE(&err);
      goto cleanup;
    }
  } else if (sel_type == 2) {
    if (!move_item_down(editor, sel_idx, item_idx, &err)) {
      OV_ERROR_ADD_TRACE(&err);
      goto cleanup;
    }
  }

  success = true;

cleanup:
  if (!success) {
    show_error_dialog(editor, &err);
  }
}

// Command handler for ID_EDIT_DELETE_PARAM
static void handle_cmd_delete_param(struct ptk_anm2editor *editor) {
  struct ov_error err = {0};
  bool success = false;

  if (!delete_selected_parameter(editor, &err)) {
    OV_ERROR_ADD_TRACE(&err);
    goto cleanup;
  }

  success = true;

cleanup:
  if (!success) {
    show_error_dialog(editor, &err);
  }
}

// Command handler for ID_EDIT_UNDO
static void handle_cmd_undo(struct ptk_anm2editor *editor) {
  if (!editor || !editor->doc || !ptk_anm2_can_undo(editor->doc)) {
    return;
  }

  struct ov_error err = {0};
  bool success = false;

  // Start focus tracking with current selection
  size_t sel_idx = 0;
  size_t item_idx = 0;
  int sel_type = get_selected_indices(editor, &sel_idx, &item_idx);
  if (sel_type > 0) {
    editor->focus_valid = true;
    editor->focus_sel_idx = sel_idx;
    editor->focus_item_idx = (sel_type == 2) ? item_idx : SIZE_MAX;
  } else {
    editor->focus_valid = false;
  }

  // Save ListView selection before undo
  int const detail_sel = (int)SendMessageW(editor->detaillist, LVM_GETNEXTITEM, (WPARAM)-1, LVNI_SELECTED);

  if (!ptk_anm2_undo(editor->doc, &err)) {
    OV_ERROR_ADD_TRACE(&err);
    goto cleanup;
  }

  mark_modified(editor);

  // Restore focus based on tracking
  if (editor->focus_valid) {
    select_treeview_by_index(editor, editor->focus_sel_idx, editor->focus_item_idx);
  }

  update_detail_panel(editor);

  // Restore ListView selection if still tracking the same logical item
  if (detail_sel >= 0 && editor->focus_valid) {
    int const item_count = (int)SendMessageW(editor->detaillist, LVM_GETITEMCOUNT, 0, 0);
    if (detail_sel < item_count) {
      SendMessageW(editor->detaillist,
                   LVM_SETITEMSTATE,
                   (WPARAM)detail_sel,
                   (LPARAM) & (LVITEMW){
                                  .stateMask = LVIS_SELECTED | LVIS_FOCUSED,
                                  .state = LVIS_SELECTED | LVIS_FOCUSED,
                              });
    }
  }

  update_toolbar_state(editor);

  success = true;

cleanup:
  editor->focus_valid = false; // Reset tracking
  if (!success) {
    show_error_dialog(editor, &err);
  }
}

// Command handler for ID_EDIT_REDO
static void handle_cmd_redo(struct ptk_anm2editor *editor) {
  if (!editor || !editor->doc || !ptk_anm2_can_redo(editor->doc)) {
    return;
  }

  struct ov_error err = {0};
  bool success = false;

  // Start focus tracking with current selection
  size_t sel_idx = 0;
  size_t item_idx = 0;
  int sel_type = get_selected_indices(editor, &sel_idx, &item_idx);
  if (sel_type > 0) {
    editor->focus_valid = true;
    editor->focus_sel_idx = sel_idx;
    editor->focus_item_idx = (sel_type == 2) ? item_idx : SIZE_MAX;
  } else {
    editor->focus_valid = false;
  }

  // Save ListView selection before redo
  int const detail_sel = (int)SendMessageW(editor->detaillist, LVM_GETNEXTITEM, (WPARAM)-1, LVNI_SELECTED);

  if (!ptk_anm2_redo(editor->doc, &err)) {
    OV_ERROR_ADD_TRACE(&err);
    goto cleanup;
  }

  mark_modified(editor);

  // Restore focus based on tracking
  if (editor->focus_valid) {
    select_treeview_by_index(editor, editor->focus_sel_idx, editor->focus_item_idx);
  }

  update_detail_panel(editor);

  // Restore ListView selection if still tracking the same logical item
  if (detail_sel >= 0 && editor->focus_valid) {
    int const item_count = (int)SendMessageW(editor->detaillist, LVM_GETITEMCOUNT, 0, 0);
    if (detail_sel < item_count) {
      SendMessageW(editor->detaillist,
                   LVM_SETITEMSTATE,
                   (WPARAM)detail_sel,
                   (LPARAM) & (LVITEMW){
                                  .stateMask = LVIS_SELECTED | LVIS_FOCUSED,
                                  .state = LVIS_SELECTED | LVIS_FOCUSED,
                              });
    }
  }

  update_toolbar_state(editor);

  success = true;

cleanup:
  editor->focus_valid = false; // Reset tracking
  if (!success) {
    show_error_dialog(editor, &err);
  }
}

// Context for import scripts callback
struct import_scripts_context {
  struct ptk_anm2editor *editor;
  struct ov_error *err;
  bool success;
};

// Build selector name from PSD path
// Always succeeds - uses "Unnamed Selector" as fallback if path extraction fails
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
    // Extract path before '|' (format: "path|pfv_file" or just "path")
    char const *pipe = strchr(psd_path, '|');
    size_t path_len = pipe ? (size_t)(pipe - psd_path) : strlen(psd_path);
    if (path_len >= MAX_PATH) {
      goto cleanup;
    }

    // Create null-terminated copy for path functions
    char path_buf[MAX_PATH];
    memcpy(path_buf, psd_path, path_len);
    path_buf[path_len] = '\0';

    // Extract file name and remove extension
    char const *base_name = ovl_path_extract_file_name(path_buf);
    char const *ext = ovl_path_find_ext(base_name);
    size_t base_len = ext ? (size_t)(ext - base_name) : strlen(base_name);
    if (base_len == 0 || base_len >= selector_name_size - 32 || base_len >= 256) {
      goto cleanup;
    }

    // Null-terminate the base name portion and build selector name
    char base_name_buf[256];
    memcpy(base_name_buf, base_name, base_len);
    base_name_buf[base_len] = '\0';

    // Build selector name: "{basename} {Selector}"
    ov_snprintf_char(
        selector_name, selector_name_size, "%1$hs", pgettext("anm2editor", "%1$hs Selector"), base_name_buf);
    success = true;
  }

cleanup:
  if (!success) {
    ov_snprintf_char(selector_name, selector_name_size, "%1$hs", "%1$hs", pgettext("anm2editor", "Unnamed Selector"));
  }
}

// Import a single script item to the editor
static bool import_single_script(struct ptk_anm2editor *const editor,
                                 size_t const sel_idx,
                                 char const *const alias,
                                 struct ptk_alias_available_script const *const item,
                                 struct ov_error *const err) {
  struct ptk_alias_extracted_animation anim = {0};
  bool success = false;

  if (!ptk_alias_extract_animation(alias, strlen(alias), item->script_name, item->effect_name, &anim, err)) {
    OV_ERROR_ADD_TRACE(err);
    goto cleanup;
  }
  if (!ptk_anm2editor_add_animation_item(
          editor, sel_idx, anim.script_name, anim.effect_name, anim.params, OV_ARRAY_LENGTH(anim.params), err)) {
    OV_ERROR_ADD_TRACE(err);
    goto cleanup;
  }
  success = true;

cleanup:
  ptk_alias_extracted_animation_free(&anim);
  return success;
}

// Execute import scripts transaction
// Handles the actual import of selected scripts within a transaction
static bool import_scripts_execute_transaction(struct ptk_anm2editor *const editor,
                                               char const *const alias,
                                               struct ptk_alias_available_scripts const *const scripts,
                                               bool const has_selected,
                                               bool const update_psd_path,
                                               struct ov_error *const err) {
  bool success = false;

  // Get the selected selector index (or create a new one if none selected)
  size_t sel_idx = ptk_anm2editor_get_selected_selector_index(editor);
  if (sel_idx == SIZE_MAX && has_selected) {
    // No selector selected, add a new one with name based on PSD file
    char selector_name[256] = {0};
    build_selector_name_from_psd_path(scripts->psd_path, selector_name, sizeof(selector_name));
    if (!ptk_anm2_selector_add(editor->doc, selector_name, err)) {
      OV_ERROR_ADD_TRACE(err);
      goto cleanup;
    }
    sel_idx = ptk_anm2_selector_count(editor->doc) - 1;
  }

  // Import selected scripts to the editor
  if (has_selected) {
    size_t const scripts_count = OV_ARRAY_LENGTH(scripts->items);
    for (size_t i = 0; i < scripts_count; i++) {
      if (!scripts->items[i].selected) {
        continue;
      }
      if (!import_single_script(editor, sel_idx, alias, &scripts->items[i], err)) {
        OV_ERROR_ADD_TRACE(err);
        goto cleanup;
      }
    }
  }

  // Update PSD path if requested
  if (update_psd_path && scripts->psd_path) {
    if (!ptk_anm2editor_set_psd_path(editor, scripts->psd_path, err)) {
      OV_ERROR_ADD_TRACE(err);
      goto cleanup;
    }
  }

  success = true;

cleanup:
  return success;
}

// Callback for call_edit_section_param - runs in edit section context
static void import_scripts_callback(void *param, struct aviutl2_edit_section *edit) {
  struct import_scripts_context *ctx = (struct import_scripts_context *)param;
  struct ptk_anm2editor *editor = ctx->editor;
  struct ov_error *err = ctx->err;

  struct ptk_alias_script_definitions defs = {0};
  struct ptk_alias_available_scripts scripts = {0};
  char const *alias = NULL;
  aviutl2_object_handle obj = NULL;

  // Get the currently focused object
  obj = edit->get_focus_object();
  if (!obj) {
    OV_ERROR_SET(err,
                 ov_error_type_generic,
                 ptk_alias_error_no_object_selected,
                 gettext("no object is selected in AviUtl ExEdit2."));
    goto cleanup;
  }

  // Get the alias data from the object
  alias = edit->get_object_alias(obj);
  if (!alias || alias[0] == '\0') {
    OV_ERROR_SET(err,
                 ov_error_type_generic,
                 ptk_alias_error_failed_to_get_alias,
                 gettext("failed to get alias data from the selected object."));
    goto cleanup;
  }

  // Load script definitions from INI
  if (!ptk_alias_load_script_definitions(&defs, err)) {
    OV_ERROR_ADD_TRACE(err);
    goto cleanup;
  }

  // Enumerate available scripts from the alias
  if (!ptk_alias_enumerate_available_scripts(alias, strlen(alias), &defs, &scripts, err)) {
    OV_ERROR_ADD_TRACE(err);
    goto cleanup;
  }

  // Check if there are any scripts to import
  if (OV_ARRAY_LENGTH(scripts.items) == 0) {
    OV_ERROR_SET(err,
                 ov_error_type_generic,
                 ptk_alias_error_no_scripts,
                 gettext("no importable scripts found in the selected object."));
    goto cleanup;
  }

  // Show script picker dialog and execute import
  {
    struct ptk_script_picker_params picker_params = {
        .items = scripts.items,
        .item_count = OV_ARRAY_LENGTH(scripts.items),
        .current_psd_path = ptk_anm2editor_get_psd_path(editor),
        .source_psd_path = scripts.psd_path,
        .update_psd_path = false,
    };

    HWND *disabled_windows = ptk_win32_disable_family_windows(editor->window);
    ov_tribool const result = ptk_script_picker_show(editor->window, &picker_params, err);
    ptk_win32_restore_disabled_family_windows(disabled_windows);

    if (result == ov_false) {
      ctx->success = true;
      goto cleanup;
    }
    if (result == ov_indeterminate) {
      OV_ERROR_ADD_TRACE(err);
      goto cleanup;
    }

    bool has_selected = false;
    size_t const n = OV_ARRAY_LENGTH(scripts.items);
    for (size_t i = 0; i < n; i++) {
      if (scripts.items[i].selected) {
        has_selected = true;
        break;
      }
    }
    if (!has_selected && !picker_params.update_psd_path) {
      ctx->success = true;
      goto cleanup;
    }

    if (!ptk_anm2_begin_transaction(editor->doc, err)) {
      OV_ERROR_ADD_TRACE(err);
      goto cleanup;
    }
    bool const transaction_success =
        import_scripts_execute_transaction(editor, alias, &scripts, has_selected, picker_params.update_psd_path, err);
    if (!ptk_anm2_end_transaction(editor->doc, transaction_success ? err : NULL)) {
      if (transaction_success) {
        OV_ERROR_ADD_TRACE(err);
      }
      goto cleanup;
    }

    if (!transaction_success) {
      goto cleanup;
    }
  }

  ctx->success = true;

cleanup:
  ptk_alias_available_scripts_free(&scripts);
  ptk_alias_script_definitions_free(&defs);
}

// Command handler for ID_EDIT_IMPORT_SCRIPTS
static void handle_cmd_import_scripts(struct ptk_anm2editor *editor) {
  if (!editor || !editor->edit) {
    return;
  }

  struct ov_error err = {0};
  struct import_scripts_context ctx = {
      .editor = editor,
      .err = &err,
      .success = false,
  };

  // Call the edit section to access the focused object
  if (!editor->edit->call_edit_section_param(&ctx, import_scripts_callback)) {
    OV_ERROR_SET(&err, ov_error_type_generic, ov_error_generic_fail, gettext("edit section is not available."));
    goto cleanup;
  }

cleanup:
  if (!ctx.success) {
    // Check if this is a "how to use" error that needs a hint
    if (ov_error_is(&err, ov_error_type_generic, ptk_alias_error_psd_not_found) ||
        ov_error_is(&err, ov_error_type_generic, ptk_alias_error_no_scripts) ||
        ov_error_is(&err, ov_error_type_generic, ptk_alias_error_no_object_selected) ||
        ov_error_is(&err, ov_error_type_generic, ptk_alias_error_failed_to_get_alias)) {
      wchar_t msg[256];
      wchar_t hint_text[768];
      ov_snprintf_char2wchar(
          msg, sizeof(msg) / sizeof(msg[0]), NULL, pgettext("anm2editor", "Welcome to Script Importer!"), NULL);
      ov_snprintf_char2wchar(hint_text,
                             768,
                             NULL,
                             pgettext("anm2editor",
                                      "1. Select a PSD File object in AviUtl ExEdit2\n"
                                      "2. Add effects like \"Blinker@PSDToolKit\" and configure them\n"
                                      "3. Press this button\n\n"
                                      "This feature imports animation settings from the selected PSD File object."),
                             NULL);
      ptk_error_dialog(editor->window, &err, get_window_title(), msg, hint_text, TD_INFORMATION_ICON, TDCBF_OK_BUTTON);
      OV_ERROR_DESTROY(&err);
    } else {
      show_error_dialog(editor, &err);
    }
  }
}

// Build file filter string for anm file dialogs
static wchar_t const *get_anm_file_filter(void) {
  static wchar_t buf[256];
  if (!buf[0]) {
    ov_snprintf_char2wchar(buf,
                           sizeof(buf) / sizeof(buf[0]),
                           "%1$hs%2$hs",
                           "%1$hs (*.anm)|*.anm|%2$hs (*.*)|*.*|",
                           pgettext("anm2editor", "AviUtl1 Animation Script"),
                           pgettext("anm2editor", "All Files"));
    for (wchar_t *p = buf; *p; ++p) {
      if (*p == L'|') {
        *p = L'\0';
      }
    }
  }
  return buf;
}

// Build file filter string for anm2 save dialog (convert command)
static wchar_t const *get_anm2_convert_save_filter(void) {
  static wchar_t buf[256];
  if (!buf[0]) {
    ov_snprintf_char2wchar(buf,
                           sizeof(buf) / sizeof(buf[0]),
                           "%1$hs%2$hs",
                           "%1$hs (*.anm2)|*.anm2|%2$hs (*.*)|*.*|",
                           pgettext("anm2editor", "AviUtl ExEdit2 Animation Script"),
                           pgettext("anm2editor", "All Files"));
    for (wchar_t *p = buf; *p; ++p) {
      if (*p == L'|') {
        *p = L'\0';
      }
    }
  }
  return buf;
}

// Command handler for ID_EDIT_CONVERT_ANM
static void handle_cmd_convert_anm(struct ptk_anm2editor *editor) {
  if (!editor) {
    return;
  }

  struct ov_error err = {0};
  bool success = false;
  wchar_t *src_path = NULL;
  wchar_t *dst_path = NULL;
  char *src_data = NULL;
  char *dst_data = NULL;
  wchar_t *default_dir = NULL;
  struct ovl_file *src_file = NULL;
  struct ovl_file *dst_file = NULL;

  // Get default directory (Script folder)
  if (!get_script_dir(&default_dir, &err)) {
    OV_ERROR_ADD_TRACE(&err);
    goto cleanup;
  }

  // Show file open dialog for source anm file
  {
    // {83F03793-4997-442C-A4E4-AEE63D6117FC}
    static GUID const open_dialog_guid = {0x83f03793, 0x4997, 0x442c, {0xa4, 0xe4, 0xae, 0xe6, 0x3d, 0x61, 0x17, 0xfc}};
    wchar_t title[256];
    ov_snprintf_char2wchar(
        title, sizeof(title) / sizeof(title[0]), NULL, pgettext("anm2editor", "Select *.anm to convert"), NULL);
    if (!ovl_dialog_select_file(
            editor->window, title, get_anm_file_filter(), &open_dialog_guid, default_dir, &src_path, &err)) {
      if (ov_error_is(&err, ov_error_type_hresult, (int)HRESULT_FROM_WIN32(ERROR_CANCELLED))) {
        // User cancelled
        OV_ERROR_DESTROY(&err);
        success = true;
        goto cleanup;
      }
      OV_ERROR_ADD_TRACE(&err);
      goto cleanup;
    }
  }

  // Read source file
  {
    uint64_t file_size = 0;
    size_t bytes_read = 0;

    if (!ovl_file_open(src_path, &src_file, &err)) {
      OV_ERROR_ADD_TRACE(&err);
      goto cleanup;
    }

    if (!ovl_file_size(src_file, &file_size, &err)) {
      OV_ERROR_ADD_TRACE(&err);
      goto cleanup;
    }

    if (!OV_ARRAY_GROW(&src_data, file_size + 1)) {
      OV_ERROR_SET_GENERIC(&err, ov_error_generic_out_of_memory);
      goto cleanup;
    }

    if (!ovl_file_read(src_file, src_data, (size_t)file_size, &bytes_read, &err)) {
      OV_ERROR_ADD_TRACE(&err);
      goto cleanup;
    }
    src_data[bytes_read] = '\0';
    OV_ARRAY_SET_LENGTH(src_data, bytes_read);

    ovl_file_close(src_file);
    src_file = NULL;
  }

  // Convert anm to anm2
  {
    size_t const src_len = OV_ARRAY_LENGTH(src_data);
    if (!ptk_anm_to_anm2(src_data, src_len, &dst_data, &err)) {
      OV_ERROR_ADD_TRACE(&err);
      goto cleanup;
    }
  }

  // Generate default output path: same name without extension (dialog will add .anm2)
  {
    wchar_t *default_save_path = NULL;

    // Find base name without extension
    wchar_t const *ext = ovl_path_find_ext(src_path);
    size_t const base_len = ext ? (size_t)(ext - src_path) : wcslen(src_path);

    if (!OV_ARRAY_GROW(&default_save_path, base_len + 1)) {
      OV_ERROR_SET_GENERIC(&err, ov_error_generic_out_of_memory);
      goto cleanup;
    }
    memcpy(default_save_path, src_path, base_len * sizeof(wchar_t));
    default_save_path[base_len] = L'\0';
    OV_ARRAY_SET_LENGTH(default_save_path, base_len);

    // Show save dialog
    // {B979B21D-C448-4079-A94F-DCD12FC8D15C}
    static GUID const save_dialog_guid = {0xb979b21d, 0xc448, 0x4079, {0xa9, 0x4f, 0xdc, 0xd1, 0x2f, 0xc8, 0xd1, 0x5c}};
    wchar_t title[256];
    ov_snprintf_char2wchar(
        title, sizeof(title) / sizeof(title[0]), NULL, pgettext("anm2editor", "Save converted *.anm2"), NULL);
    if (!ovl_dialog_save_file(editor->window,
                              title,
                              get_anm2_convert_save_filter(),
                              &save_dialog_guid,
                              default_save_path,
                              L"anm2",
                              &dst_path,
                              &err)) {
      if (default_save_path) {
        OV_ARRAY_DESTROY(&default_save_path);
      }
      if (ov_error_is(&err, ov_error_type_hresult, (int)HRESULT_FROM_WIN32(ERROR_CANCELLED))) {
        // User cancelled
        OV_ERROR_DESTROY(&err);
        success = true;
        goto cleanup;
      }
      OV_ERROR_ADD_TRACE(&err);
      goto cleanup;
    }

    if (default_save_path) {
      OV_ARRAY_DESTROY(&default_save_path);
    }
  }

  // Write destination file
  {
    size_t const dst_len = OV_ARRAY_LENGTH(dst_data);
    size_t bytes_written = 0;

    if (!ovl_file_create(dst_path, &dst_file, &err)) {
      OV_ERROR_ADD_TRACE(&err);
      goto cleanup;
    }

    if (!ovl_file_write(dst_file, dst_data, dst_len, &bytes_written, &err)) {
      OV_ERROR_ADD_TRACE(&err);
      goto cleanup;
    }

    ovl_file_close(dst_file);
    dst_file = NULL;
  }

  // Show success dialog with warning
  {
    wchar_t msg[256];
    wchar_t content[512];
    ov_snprintf_char2wchar(
        msg, sizeof(msg) / sizeof(msg[0]), NULL, pgettext("anm2editor", "Conversion completed."), NULL);
    ov_snprintf_char2wchar(
        content,
        sizeof(content) / sizeof(content[0]),
        NULL,
        pgettext("anm2editor",
                 "Note: This conversion uses simple string replacement and may not work correctly in all cases.\n"
                 "Also, this converted script is different from *.ptk.anm2 and cannot be edited in this editor."),
        NULL);
    ptk_dialog_show(&(struct ptk_dialog_params){
        .owner = editor->window,
        .icon = TD_INFORMATION_ICON,
        .buttons = TDCBF_OK_BUTTON,
        .window_title = get_window_title(),
        .main_instruction = msg,
        .content = content,
    });
  }

  success = true;

cleanup:
  if (!success) {
    show_error_dialog(editor, &err);
  }
  if (src_file) {
    ovl_file_close(src_file);
  }
  if (dst_file) {
    ovl_file_close(dst_file);
  }
  if (default_dir) {
    OV_ARRAY_DESTROY(&default_dir);
  }
  if (src_path) {
    OV_ARRAY_DESTROY(&src_path);
  }
  if (dst_path) {
    OV_ARRAY_DESTROY(&dst_path);
  }
  if (src_data) {
    OV_ARRAY_DESTROY(&src_data);
  }
  if (dst_data) {
    OV_ARRAY_DESTROY(&dst_data);
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

// Move a single item by ID to the destination (helper for multi-selection move loop)
static bool move_item_by_id(
    struct ptk_anm2editor *editor, uint32_t id, size_t dst_sel, size_t dst_idx, struct ov_error *const err) {
  size_t item_sel_idx = 0;
  size_t item_item_idx = 0;
  if (!ptk_anm2_find_item_by_id(editor->doc, id, &item_sel_idx, &item_item_idx)) {
    return true; // Item no longer exists - not an error
  }
  if (item_sel_idx == dst_sel && item_item_idx == dst_idx) {
    return true; // Already at destination - skip
  }
  if (!ptk_anm2_item_move_to(editor->doc, item_sel_idx, item_item_idx, dst_sel, dst_idx, err)) {
    OV_ERROR_ADD_TRACE(err);
    return false;
  }
  return true;
}

// Move all multi-selected items to destination in a transaction
static bool move_multisel_items(struct ptk_anm2editor *editor,
                                bool dst_is_selector,
                                size_t dst_sel,
                                size_t dst_item_idx,
                                struct ov_error *const err) {
  bool success = false;

  if (!ptk_anm2_begin_transaction(editor->doc, err)) {
    OV_ERROR_ADD_TRACE(err);
    goto cleanup;
  }

  {
    uint32_t const *selected_ids = multisel_get_ids(editor);
    size_t const count = multisel_count(editor);
    size_t const move_dst_idx = dst_is_selector ? ptk_anm2_item_count(editor->doc, dst_sel) : dst_item_idx;

    for (size_t i = count; i > 0; i--) {
      if (!move_item_by_id(editor, selected_ids[i - 1], dst_sel, move_dst_idx, err)) {
        OV_ERROR_ADD_TRACE(err);
        goto cleanup;
      }
    }
  }

  if (!ptk_anm2_end_transaction(editor->doc, err)) {
    OV_ERROR_ADD_TRACE(err);
    goto cleanup;
  }

  InvalidateRect(editor->treeview, NULL, FALSE);
  success = true;

cleanup:
  if (!success) {
    struct ov_error end_err = {0};
    if (!ptk_anm2_end_transaction(editor->doc, &end_err)) {
      ptk_logf_error(&end_err, "%1$hs", "%1$hs", gettext("failed to end transaction in error recovery"));
      OV_ERROR_DESTROY(&end_err);
    }
  }
  return success;
}

// Move a single item to an item position
static bool move_single_item_to_item(struct ptk_anm2editor *editor,
                                     size_t src_sel,
                                     size_t src_item_idx,
                                     size_t dst_sel,
                                     size_t dst_item_idx,
                                     struct ov_error *const err) {
  size_t const sel_count = ptk_anm2_selector_count(editor->doc);
  if (src_sel >= sel_count || dst_sel >= sel_count) {
    return true;
  }

  size_t const src_items_len = ptk_anm2_item_count(editor->doc, src_sel);
  if (src_item_idx >= src_items_len) {
    return true;
  }

  size_t const dst_items_len = ptk_anm2_item_count(editor->doc, dst_sel);
  size_t const max_dst_idx = (src_sel == dst_sel) ? src_items_len - 1 : dst_items_len;
  if (dst_item_idx > max_dst_idx) {
    return true;
  }

  if (src_sel == dst_sel && src_item_idx == dst_item_idx) {
    return true;
  }

  if (!ptk_anm2_item_move_to(editor->doc, src_sel, src_item_idx, dst_sel, dst_item_idx, err)) {
    OV_ERROR_ADD_TRACE(err);
    return false;
  }
  return true;
}

// Move a single item onto a selector (append to end)
static bool move_single_item_to_selector(
    struct ptk_anm2editor *editor, size_t src_sel, size_t src_item_idx, size_t dst_sel, struct ov_error *const err) {
  size_t const sel_count = ptk_anm2_selector_count(editor->doc);
  if (src_sel >= sel_count || dst_sel >= sel_count || src_sel == dst_sel) {
    return true;
  }

  size_t const src_items_len = ptk_anm2_item_count(editor->doc, src_sel);
  if (src_item_idx >= src_items_len) {
    return true;
  }

  size_t const dst_items_len = ptk_anm2_item_count(editor->doc, dst_sel);
  if (!ptk_anm2_item_move_to(editor->doc, src_sel, src_item_idx, dst_sel, dst_items_len, err)) {
    OV_ERROR_ADD_TRACE(err);
    return false;
  }
  return true;
}

// Helper function for drag-and-drop move operation with multi-selection support
static void handle_drag_move(struct ptk_anm2editor *editor,
                             bool src_is_selector,
                             size_t src_sel,
                             size_t src_item_idx,
                             bool dst_is_selector,
                             size_t dst_sel,
                             size_t dst_item_idx) {
  struct ov_error err = {0};
  bool success = false;

  if (src_is_selector && dst_is_selector) {
    size_t const sel_count = ptk_anm2_selector_count(editor->doc);
    if (src_sel != dst_sel && src_sel < sel_count && dst_sel < sel_count) {
      if (!ptk_anm2_selector_move_to(editor->doc, src_sel, dst_sel, &err)) {
        OV_ERROR_ADD_TRACE(&err);
        goto cleanup;
      }
      mark_modified(editor);
    }
  } else if (!src_is_selector) {
    if (multisel_count(editor) > 1) {
      if (!move_multisel_items(editor, dst_is_selector, dst_sel, dst_item_idx, &err)) {
        OV_ERROR_ADD_TRACE(&err);
        goto cleanup;
      }
      mark_modified(editor);
    } else if (dst_is_selector) {
      if (!move_single_item_to_selector(editor, src_sel, src_item_idx, dst_sel, &err)) {
        OV_ERROR_ADD_TRACE(&err);
        goto cleanup;
      }
      mark_modified(editor);
    } else {
      if (!move_single_item_to_item(editor, src_sel, src_item_idx, dst_sel, dst_item_idx, &err)) {
        OV_ERROR_ADD_TRACE(&err);
        goto cleanup;
      }
      mark_modified(editor);
    }
  }

  success = true;

cleanup:
  if (!success) {
    show_error_dialog(editor, &err);
  }
}

// Handle NM_CUSTOMDRAW for TreeView multi-selection highlighting
static LRESULT handle_treeview_customdraw(struct ptk_anm2editor *editor, NMTVCUSTOMDRAW *nmcd) {
  switch (nmcd->nmcd.dwDrawStage) {
  case CDDS_PREPAINT:
    return CDRF_NOTIFYITEMDRAW;
  case CDDS_ITEMPREPAINT: {
    TVITEMW tvi = {.mask = TVIF_PARAM, .hItem = (HTREEITEM)nmcd->nmcd.dwItemSpec};
    if (!TreeView_GetItem(editor->treeview, &tvi)) {
      return CDRF_DODEFAULT;
    }
    uint32_t id = 0;
    bool const is_selector = treeview_decode_lparam(tvi.lParam, &id);
    if (!is_selector && multisel_contains(editor, id)) {
      nmcd->clrTextBk = GetSysColor(COLOR_HIGHLIGHT);
      nmcd->clrText = GetSysColor(COLOR_HIGHLIGHTTEXT);
      return CDRF_NEWFONT;
    }
    return CDRF_DODEFAULT;
  }
  default:
    return CDRF_DODEFAULT;
  }
}

static bool confirm_discard_changes(struct ptk_anm2editor *editor) {
  if (!editor->modified) {
    return true;
  }

  wchar_t main_instruction[256];
  ov_snprintf_char2wchar(main_instruction,
                         sizeof(main_instruction) / sizeof(main_instruction[0]),
                         NULL,
                         pgettext("anm2editor", "Do you want to save changes before closing?"),
                         NULL);

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

    // Create Toolbar
    editor->toolbar = CreateWindowExW(0,
                                      TOOLBARCLASSNAMEW,
                                      NULL,
                                      WS_CHILD | WS_VISIBLE | TBSTYLE_FLAT | TBSTYLE_TOOLTIPS | CCS_TOP,
                                      0,
                                      0,
                                      0,
                                      0,
                                      hwnd,
                                      (HMENU)IDC_TOOLBAR,
                                      GetModuleHandleW(NULL),
                                      NULL);

    SendMessageW(editor->toolbar, TB_BUTTONSTRUCTSIZE, sizeof(TBBUTTON), 0);

    enum {
      BUTTON_WIDTH = 40,
      BUTTON_HEIGHT = 28,
    };

    // Set toolbar icon and button size
    SendMessageW(editor->toolbar, TB_SETBITMAPSIZE, 0, MAKELPARAM(ICON_SIZE, ICON_SIZE));
    SendMessageW(editor->toolbar, TB_SETBUTTONSIZE, 0, MAKELPARAM(BUTTON_WIDTH, BUTTON_HEIGHT));

    // Create custom ImageList for toolbar (normal and disabled)
    editor->toolbar_imagelist = ImageList_Create(ICON_SIZE, ICON_SIZE, ILC_COLOR32, ICON_COUNT, 4);
    editor->toolbar_disabled_imagelist = ImageList_Create(ICON_SIZE, ICON_SIZE, ILC_COLOR32, ICON_COUNT, 4);

    // Load PNG icons (initializes GDI+ once for all icons)
    void *dll_hinst = NULL;
    ovl_os_get_hinstance_from_fnptr((void *)load_toolbar_icons, &dll_hinst, NULL);
    load_toolbar_icons((HINSTANCE)dll_hinst, editor->toolbar_imagelist, editor->toolbar_disabled_imagelist);

    // Set the custom ImageList to toolbar (normal and disabled)
    SendMessageW(editor->toolbar, TB_SETIMAGELIST, 0, (LPARAM)editor->toolbar_imagelist);
    SendMessageW(editor->toolbar, TB_SETDISABLEDIMAGELIST, 0, (LPARAM)editor->toolbar_disabled_imagelist);

    // Add toolbar buttons
    TBBUTTON buttons[] = {
        {ICON_NEW, ID_FILE_NEW, TBSTATE_ENABLED, BTNS_BUTTON, {0}, 0, 0},
        {ICON_OPEN, ID_FILE_OPEN, TBSTATE_ENABLED, BTNS_BUTTON, {0}, 0, 0},
        {ICON_SAVE, ID_FILE_SAVE, TBSTATE_ENABLED, BTNS_BUTTON, {0}, 0, 0},
        {ICON_SAVEAS, ID_FILE_SAVEAS, TBSTATE_ENABLED, BTNS_BUTTON, {0}, 0, 0},
        {0, 0, 0, BTNS_SEP, {0}, 0, 0},
        {ICON_UNDO, ID_EDIT_UNDO, TBSTATE_ENABLED, BTNS_BUTTON, {0}, 0, 0},
        {ICON_REDO, ID_EDIT_REDO, TBSTATE_ENABLED, BTNS_BUTTON, {0}, 0, 0},
        {0, 0, 0, BTNS_SEP, {0}, 0, 0},
        {ICON_IMPORT, ID_EDIT_IMPORT_SCRIPTS, TBSTATE_ENABLED, BTNS_BUTTON, {0}, 0, 0},
        {ICON_CONVERT, ID_EDIT_CONVERT_ANM, TBSTATE_ENABLED, BTNS_BUTTON, {0}, 0, 0},
    };
    SendMessageW(editor->toolbar, TB_ADDBUTTONSW, sizeof(buttons) / sizeof(buttons[0]), (LPARAM)buttons);

    // Create TreeView with drag support and label editing
    // (TVS_DISABLEDRAGDROP is NOT set to enable drag)
    editor->treeview = CreateWindowExW(WS_EX_CLIENTEDGE,
                                       WC_TREEVIEWW,
                                       NULL,
                                       WS_CHILD | WS_VISIBLE | WS_TABSTOP | TVS_HASLINES | TVS_LINESATROOT |
                                           TVS_HASBUTTONS | TVS_SHOWSELALWAYS | TVS_EDITLABELS,
                                       0,
                                       0,
                                       100,
                                       100,
                                       hwnd,
                                       (HMENU)IDC_TREEVIEW,
                                       GetModuleHandleW(NULL),
                                       NULL);

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
        SendMessageW(editor->treeview, TVM_SETIMAGELIST, TVSIL_NORMAL, (LPARAM)himl);
      }
    }

    // Create detail ListView (report view with two columns: Property and Value)
    editor->detaillist =
        CreateWindowExW(WS_EX_CLIENTEDGE,
                        WC_LISTVIEWW,
                        NULL,
                        WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_SINGLESEL | LVS_NOSORTHEADER | LVS_SHOWSELALWAYS,
                        0,
                        0,
                        100,
                        100,
                        hwnd,
                        (HMENU)IDC_DETAILLIST,
                        GetModuleHandleW(NULL),
                        NULL);

    // Set extended styles for better appearance
    SendMessageW(editor->detaillist, LVM_SETEXTENDEDLISTVIEWSTYLE, 0, LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES);

    // Add columns
    {
      wchar_t col_property[64];
      wchar_t col_value[64];
      ov_snprintf_char2wchar(col_property,
                             sizeof(col_property) / sizeof(col_property[0]),
                             "%1$hs",
                             "%1$hs",
                             pgettext("anm2editor", "Property"));
      ov_snprintf_char2wchar(
          col_value, sizeof(col_value) / sizeof(col_value[0]), "%1$hs", "%1$hs", pgettext("anm2editor", "Value"));

      LVCOLUMNW lvc = {
          .mask = LVCF_TEXT | LVCF_WIDTH,
          .cx = 100,
          .pszText = col_property,
      };
      SendMessageW(editor->detaillist, LVM_INSERTCOLUMNW, 0, (LPARAM)&lvc);

      lvc.cx = 200;
      lvc.pszText = col_value;
      SendMessageW(editor->detaillist, LVM_INSERTCOLUMNW, 1, (LPARAM)&lvc);
    }

    // Initialize splitter position (will be set properly in WM_SIZE)
    editor->splitter_pos = -1; // -1 means not initialized yet

    update_window_title(editor);
    update_detail_panel(editor);
    update_toolbar_state(editor);
    return 0;
  }

  case WM_SIZE: {
    if (!editor) {
      break;
    }
    RECT rc;
    GetClientRect(hwnd, &rc);

    // Position toolbar at top
    SendMessageW(editor->toolbar, TB_AUTOSIZE, 0, 0);
    RECT toolbar_rc;
    GetWindowRect(editor->toolbar, &toolbar_rc);
    int const toolbar_height = toolbar_rc.bottom - toolbar_rc.top;

    int const content_height = rc.bottom - rc.top - toolbar_height;
    int const content_width = rc.right - rc.left;

    // Initialize splitter position if not set (40% of width)
    if (editor->splitter_pos < 0) {
      editor->splitter_pos = content_width * 40 / 100;
    }

    // Clamp splitter position to valid range
    int const min_width = 50;
    if (editor->splitter_pos < min_width) {
      editor->splitter_pos = min_width;
    }
    if (editor->splitter_pos > content_width - min_width - splitter_width) {
      editor->splitter_pos = content_width - min_width - splitter_width;
    }

    int const treeview_width = editor->splitter_pos;
    int const detaillist_x = editor->splitter_pos + splitter_width;
    int const detaillist_width = content_width - detaillist_x;

    // Position treeview on left
    SetWindowPos(editor->treeview, NULL, 0, toolbar_height, treeview_width, content_height, SWP_NOZORDER);

    // Position detail list on right (after splitter)
    SetWindowPos(
        editor->detaillist, NULL, detaillist_x, toolbar_height, detaillist_width, content_height, SWP_NOZORDER);

    // Auto-size columns in detail list
    SendMessageW(editor->detaillist, LVM_SETCOLUMNWIDTH, 0, detaillist_width * 35 / 100);
    SendMessageW(editor->detaillist, LVM_SETCOLUMNWIDTH, 1, detaillist_width * 60 / 100);

    return 0;
  }

  case WM_COMMAND: {
    if (!editor) {
      break;
    }
    switch (LOWORD(wparam)) {
    case ID_FILE_NEW:
      handle_cmd_file_new(editor);
      return 0;

    case ID_FILE_OPEN:
      handle_cmd_file_open(editor);
      return 0;

    case ID_FILE_SAVE:
      handle_cmd_file_save(editor);
      return 0;

    case ID_FILE_SAVEAS:
      handle_cmd_file_saveas(editor);
      return 0;

    case ID_EDIT_ADD_SELECTOR:
      handle_cmd_add_selector(editor);
      return 0;

    case ID_EDIT_DELETE:
      handle_cmd_delete(editor);
      return 0;

    case ID_EDIT_REVERSE:
      handle_cmd_reverse(editor);
      return 0;

    case ID_EDIT_MOVE_UP:
      handle_cmd_move_up(editor);
      return 0;

    case ID_EDIT_MOVE_DOWN:
      handle_cmd_move_down(editor);
      return 0;

    case ID_EDIT_RENAME: {
      // Start label editing on the selected TreeView item
      HTREEITEM hItem = (HTREEITEM)(SendMessageW(editor->treeview, TVM_GETNEXTITEM, TVGN_CARET, 0));
      if (hItem) {
        SetFocus(editor->treeview);
        SendMessageW(editor->treeview, TVM_EDITLABELW, 0, (LPARAM)hItem);
      }
      return 0;
    }

    case ID_EDIT_DELETE_PARAM:
      handle_cmd_delete_param(editor);
      return 0;

    case ID_EDIT_IMPORT_SCRIPTS:
      handle_cmd_import_scripts(editor);
      return 0;

    case ID_EDIT_UNDO:
      handle_cmd_undo(editor);
      return 0;

    case ID_EDIT_REDO:
      handle_cmd_redo(editor);
      return 0;

    case ID_EDIT_CONVERT_ANM:
      handle_cmd_convert_anm(editor);
      return 0;
    }
    break;
  }

  case WM_NOTIFY: {
    NMHDR const *nmhdr = (NMHDR const *)lparam;
    if (nmhdr->code == TTN_GETDISPINFOW) {
      NMTTDISPINFOW *ttdi = (NMTTDISPINFOW *)lparam;
      char const *text = NULL;
      switch (ttdi->hdr.idFrom) {
      case ID_FILE_NEW:
        text = pgettext("anm2editor", "New");
        break;
      case ID_FILE_OPEN:
        text = pgettext("anm2editor", "Open");
        break;
      case ID_FILE_SAVE:
        text = pgettext("anm2editor", "Save");
        break;
      case ID_FILE_SAVEAS:
        text = pgettext("anm2editor", "Save As");
        break;
      case ID_EDIT_UNDO:
        text = pgettext("anm2editor", "Undo");
        break;
      case ID_EDIT_REDO:
        text = pgettext("anm2editor", "Redo");
        break;
      case ID_EDIT_IMPORT_SCRIPTS:
        text = pgettext("anm2editor", "Import Scripts from Selected Object in AviUtl");
        break;
      case ID_EDIT_CONVERT_ANM:
        text = pgettext("anm2editor", "Convert Old Animation Script(*.anm) to New(*.anm2)");
        break;
      }
      if (text) {
        ov_utf8_to_wchar(text, strlen(text), ttdi->szText, sizeof(ttdi->szText) / sizeof(ttdi->szText[0]), NULL);
        ttdi->lpszText = ttdi->szText;
      }
      return 0;
    }
    if (nmhdr->idFrom == IDC_TREEVIEW && nmhdr->code == NM_CUSTOMDRAW) {
      return handle_treeview_customdraw(editor, (NMTVCUSTOMDRAW *)lparam);
    }
    if (nmhdr->idFrom == IDC_TREEVIEW && nmhdr->code == TVN_SELCHANGEDW) {
      cancel_inline_edit(editor);

      struct ov_error err = {0};
      NMTREEVIEWW const *nmtv = (NMTREEVIEWW const *)lparam;
      uint32_t id = 0;
      bool is_selector = true;
      if (nmtv->itemNew.hItem) {
        is_selector = treeview_decode_lparam(nmtv->itemNew.lParam, &id);
      }

      bool const ctrl_pressed = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
      bool const shift_pressed = (GetKeyState(VK_SHIFT) & 0x8000) != 0;

      if (shift_pressed && editor->anchor_item_id != 0 && !is_selector) {
        // Shift+click: range selection from anchor to clicked item
        if (!ctrl_pressed) {
          multisel_clear(editor);
        }
        if (!multisel_add_range_by_id(editor, editor->anchor_item_id, id, &err)) {
          show_error_dialog(editor, &err);
        }
        // Don't update anchor on Shift+click
      } else if (ctrl_pressed) {
        // Ctrl+click: toggle selection (items only)
        if (!is_selector) {
          if (!multisel_toggle(editor, id, &err)) {
            show_error_dialog(editor, &err);
          }
          // Update anchor if item was added
          if (multisel_contains(editor, id)) {
            editor->anchor_item_id = id;
          }
        }
      } else {
        // Normal click: single selection
        multisel_clear(editor);
        if (!is_selector) {
          if (!multisel_add(editor, id, &err)) {
            show_error_dialog(editor, &err);
          }
          editor->anchor_item_id = id;
        } else {
          editor->anchor_item_id = 0;
        }
      }
      InvalidateRect(editor->treeview, NULL, FALSE);

      update_detail_panel(editor);
      update_toolbar_state(editor);
      return 0;
    }
    if (nmhdr->idFrom == IDC_TREEVIEW && nmhdr->code == NM_DBLCLK) {
      // Double-click on TreeView - check if clicked on empty space
      TVHITTESTINFO ht = {0};
      GetCursorPos(&ht.pt);
      ScreenToClient(editor->treeview, &ht.pt);
      HTREEITEM hItem = (HTREEITEM)(SendMessageW(editor->treeview, TVM_HITTEST, 0, (LPARAM)&ht));
      if (!hItem) {
        // Clicked on empty space - add new selector
        handle_cmd_add_selector(editor);
      }
      return 0;
    }
    if (nmhdr->idFrom == IDC_DETAILLIST && nmhdr->code == NM_DBLCLK) {
      NMITEMACTIVATE const *nmia = (NMITEMACTIVATE const *)lparam;
      size_t sel_idx = 0;
      size_t item_idx = 0;
      int const sel_type = get_selected_indices(editor, &sel_idx, &item_idx);

      if (nmia->iItem >= 0) {
        // Get lParam to check if this is a placeholder row
        LVITEMW lvi = {
            .mask = LVIF_PARAM,
            .iItem = nmia->iItem,
        };
        SendMessageW(editor->detaillist, LVM_GETITEMW, 0, (LPARAM)&lvi);

        if (detaillist_is_placeholder(lvi.lParam)) {
          // Placeholder row - start editing for new parameter (don't add yet)
          if (sel_type == 2) {
            start_inline_edit_for_new(editor);
          }
        } else if (detaillist_is_editable(lvi.lParam)) {
          // Editable row (normal parameter, Label, or PSD Path) - start inline edit
          start_inline_edit(editor, nmia->iItem, nmia->iSubItem);
        }
        // NOT_EDITABLE rows are ignored
      } else {
        // Clicked on empty space - start editing for new parameter if Animation item is selected
        if (sel_type == 2 && ptk_anm2_item_is_animation(editor->doc, sel_idx, item_idx)) {
          start_inline_edit_for_new(editor);
        }
      }
      return 0;
    }
    if (nmhdr->idFrom == IDC_DETAILLIST && nmhdr->code == LVN_KEYDOWN) {
      NMLVKEYDOWN const *nmkd = (NMLVKEYDOWN const *)lparam;
      if (nmkd->wVKey == VK_DELETE) {
        // Delete selected parameter using existing handler
        handle_cmd_delete_param(editor);
        return 0;
      }
    }
    if (nmhdr->idFrom == IDC_DETAILLIST && nmhdr->code == LVN_ITEMCHANGED) {
      // Selection changed in detail list - update toolbar state
      NMLISTVIEW const *nmlv = (NMLISTVIEW const *)lparam;
      if ((nmlv->uChanged & LVIF_STATE) && ((nmlv->uNewState ^ nmlv->uOldState) & LVIS_SELECTED)) {
        update_toolbar_state(editor);
      }
      return 0;
    }
    if (nmhdr->idFrom == IDC_DETAILLIST && nmhdr->code == NM_RCLICK) {
      // Right-click context menu for parameter deletion
      NMITEMACTIVATE const *nmia = (NMITEMACTIVATE const *)lparam;
      if (nmia->iItem >= 0) {
        // Select the clicked row first
        SendMessageW(editor->detaillist,
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
        SendMessageW(editor->detaillist, LVM_GETITEMW, 0, (LPARAM)&lvi);

        if (detaillist_is_editable_param(lvi.lParam)) {
          // Show context menu
          HMENU hMenu = CreatePopupMenu();
          if (hMenu) {
            wchar_t delete_text[64];
            ov_snprintf_char2wchar(delete_text,
                                   sizeof(delete_text) / sizeof(delete_text[0]),
                                   "%1$hs",
                                   "%1$hs",
                                   pgettext("anm2editor", "Delete"));
            AppendMenuW(hMenu, MF_STRING, ID_DETAILLIST_DELETE, delete_text);

            POINT pt;
            GetCursorPos(&pt);
            int const cmd = TrackPopupMenu(hMenu, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, 0, editor->window, NULL);
            DestroyMenu(hMenu);

            if (cmd == ID_DETAILLIST_DELETE) {
              handle_cmd_delete_param(editor);
            }
          }
        }
      }
      return TRUE; // Prevent further processing (e.g., AviUtl's context menu)
    }
    if (nmhdr->idFrom == IDC_TREEVIEW && nmhdr->code == NM_RCLICK) {
      // Right-click context menu for tree item rename/delete
      TVHITTESTINFO ht = {0};
      HTREEITEM hItem = NULL;
      GetCursorPos(&ht.pt);
      ScreenToClient(editor->treeview, &ht.pt);
      hItem = (HTREEITEM)(SendMessageW(editor->treeview, TVM_HITTEST, 0, (LPARAM)&ht));

      if (hItem) {
        // Select the clicked item
        SendMessageW(editor->treeview, TVM_SELECTITEM, TVGN_CARET, (LPARAM)hItem);

        // Get item info to check if it's a selector
        TVITEMW tvi = {
            .mask = TVIF_PARAM,
            .hItem = hItem,
        };
        SendMessageW(editor->treeview, TVM_GETITEMW, 0, (LPARAM)&tvi);

        uint32_t id = 0;
        bool const is_selector = treeview_decode_lparam(tvi.lParam, &id);
        size_t sel_idx = 0;
        if (is_selector) {
          ptk_anm2_find_selector_by_id(editor->doc, id, &sel_idx);
        }

        // Show context menu for item
        HMENU hMenu = CreatePopupMenu();
        if (hMenu) {
          wchar_t rename_text[64];
          wchar_t delete_text[64];
          ov_snprintf_char2wchar(rename_text,
                                 sizeof(rename_text) / sizeof(rename_text[0]),
                                 "%1$hs",
                                 "%1$hs",
                                 pgettext("anm2editor", "Rename"));
          ov_snprintf_char2wchar(delete_text,
                                 sizeof(delete_text) / sizeof(delete_text[0]),
                                 "%1$hs",
                                 "%1$hs",
                                 pgettext("anm2editor", "Delete"));
          AppendMenuW(hMenu, MF_STRING, ID_TREEVIEW_RENAME, rename_text);
          AppendMenuW(hMenu, MF_STRING, ID_TREEVIEW_DELETE, delete_text);

          // Add Reverse Items menu for selectors
          if (is_selector) {
            AppendMenuW(hMenu, MF_SEPARATOR, 0, NULL);
            wchar_t reverse_text[64];
            ov_snprintf_char2wchar(reverse_text,
                                   sizeof(reverse_text) / sizeof(reverse_text[0]),
                                   "%1$hs",
                                   "%1$hs",
                                   pgettext("anm2editor", "Reverse Items"));
            size_t const item_count = ptk_anm2_item_count(editor->doc, sel_idx);
            UINT const flags = (item_count > 0) ? MF_STRING : (MF_STRING | MF_GRAYED);
            AppendMenuW(hMenu, flags, ID_TREEVIEW_REVERSE, reverse_text);
          }

          POINT pt;
          GetCursorPos(&pt);
          int const cmd = TrackPopupMenu(hMenu, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, 0, editor->window, NULL);
          DestroyMenu(hMenu);

          if (cmd == ID_TREEVIEW_RENAME) {
            SendMessageW(editor->treeview, TVM_EDITLABELW, 0, (LPARAM)hItem);
          } else if (cmd == ID_TREEVIEW_DELETE) {
            SendMessageW(editor->window, WM_COMMAND, MAKEWPARAM(ID_EDIT_DELETE, 0), 0);
          } else if (cmd == ID_TREEVIEW_REVERSE) {
            SendMessageW(editor->window, WM_COMMAND, MAKEWPARAM(ID_EDIT_REVERSE, 0), 0);
          }
        }
      } else {
        // Clicked on empty space - show menu to add new selector
        HMENU hMenu = CreatePopupMenu();
        if (hMenu) {
          wchar_t add_selector_text[64];
          ov_snprintf_char2wchar(add_selector_text,
                                 sizeof(add_selector_text) / sizeof(add_selector_text[0]),
                                 "%1$hs",
                                 "%1$hs",
                                 pgettext("anm2editor", "Add Selector"));
          AppendMenuW(hMenu, MF_STRING, ID_TREEVIEW_ADD_SELECTOR, add_selector_text);

          POINT pt;
          GetCursorPos(&pt);
          int const cmd = TrackPopupMenu(hMenu, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, 0, editor->window, NULL);
          DestroyMenu(hMenu);

          if (cmd == ID_TREEVIEW_ADD_SELECTOR) {
            SendMessageW(editor->window, WM_COMMAND, MAKEWPARAM(ID_EDIT_ADD_SELECTOR, 0), 0);
          }
        }
      }
      return TRUE; // Prevent further processing
    }
    if (nmhdr->idFrom == IDC_TREEVIEW && nmhdr->code == TVN_BEGINLABELEDITW) {
      return handle_treeview_begin_label_edit(editor, (NMTVDISPINFOW const *)lparam);
    }
    if (nmhdr->idFrom == IDC_TREEVIEW && nmhdr->code == TVN_ENDLABELEDITW) {
      return handle_treeview_end_label_edit(editor, (NMTVDISPINFOW const *)lparam);
    }
    if (nmhdr->idFrom == IDC_TREEVIEW && nmhdr->code == TVN_BEGINDRAGW) {
      NMTREEVIEWW const *nmtv = (NMTREEVIEWW const *)lparam;
      editor->drag_item = nmtv->itemNew.hItem;
      editor->dragging = true;

      editor->drag_imagelist =
          (HIMAGELIST)(SendMessageW(editor->treeview, TVM_CREATEDRAGIMAGE, 0, (LPARAM)editor->drag_item));
      if (editor->drag_imagelist) {
        ImageList_BeginDrag(editor->drag_imagelist, 0, 0, 0);
        POINT pt = nmtv->ptDrag;
        ClientToScreen(editor->treeview, &pt);
        ImageList_DragEnter(GetDesktopWindow(), pt.x, pt.y);
      }

      SetCapture(hwnd);
      SetCursor(LoadCursorW(NULL, MAKEINTRESOURCEW(32512))); // IDC_ARROW
      return 0;
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

    RECT toolbar_rc;
    GetWindowRect(editor->toolbar, &toolbar_rc);
    int const toolbar_height = toolbar_rc.bottom - toolbar_rc.top;

    if (pt.y >= toolbar_height && pt.x >= editor->splitter_pos && pt.x < editor->splitter_pos + splitter_width) {
      SetCursor(LoadCursorW(NULL, MAKEINTRESOURCEW(32644)));
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

    RECT toolbar_rc;
    GetWindowRect(editor->toolbar, &toolbar_rc);
    int const toolbar_height = toolbar_rc.bottom - toolbar_rc.top;

    // Check if click is on splitter
    if (y >= toolbar_height && x >= editor->splitter_pos && x < editor->splitter_pos + splitter_width) {
      editor->splitter_dragging = true;
      SetCapture(hwnd);
      SetCursor(LoadCursorW(NULL, MAKEINTRESOURCEW(32644)));
      return 0;
    }
    break;
  }

  case WM_MOUSEMOVE:
    if (editor && editor->splitter_dragging) {
      int const x = (short)LOWORD(lparam);

      RECT rc;
      GetClientRect(hwnd, &rc);
      int const content_width = rc.right - rc.left;
      int const min_width = 50;

      // Update splitter position with constraints
      int new_pos = x;
      if (new_pos < min_width) {
        new_pos = min_width;
      }
      if (new_pos > content_width - min_width - splitter_width) {
        new_pos = content_width - min_width - splitter_width;
      }

      if (new_pos != editor->splitter_pos) {
        editor->splitter_pos = new_pos;
        // Trigger resize to update layout
        SendMessageW(hwnd, WM_SIZE, 0, MAKELPARAM(rc.right - rc.left, rc.bottom - rc.top));
      }
      return 0;
    }
    if (editor && editor->dragging) {
      POINT pt;
      pt.x = (short)LOWORD(lparam);
      pt.y = (short)HIWORD(lparam);
      ClientToScreen(hwnd, &pt);

      if (editor->drag_imagelist) {
        ImageList_DragMove(pt.x, pt.y);
      }

      // Highlight drop target
      POINT tv_pt = pt;
      ScreenToClient(editor->treeview, &tv_pt);
      TVHITTESTINFO ht = {.pt = tv_pt};
      HTREEITEM hTarget = (HTREEITEM)(SendMessageW(editor->treeview, TVM_HITTEST, 0, (LPARAM)&ht));

      if (editor->drag_imagelist) {
        ImageList_DragShowNolock(FALSE);
      }

      if (hTarget) {
        TreeView_SelectDropTarget(editor->treeview, hTarget);
      } else {
        TreeView_SelectDropTarget(editor->treeview, NULL);
      }

      if (editor->drag_imagelist) {
        ImageList_DragShowNolock(TRUE);
      }
      return 0;
    }
    break;

  case WM_LBUTTONUP:
    if (editor && editor->splitter_dragging) {
      editor->splitter_dragging = false;
      ReleaseCapture();
      return 0;
    }
    if (editor && editor->dragging) {
      editor->dragging = false;

      if (editor->drag_imagelist) {
        ImageList_DragLeave(GetDesktopWindow());
        ImageList_EndDrag();
        ImageList_Destroy(editor->drag_imagelist);
        editor->drag_imagelist = NULL;
      }

      ReleaseCapture();

      HTREEITEM hTarget = (HTREEITEM)(SendMessageW(editor->treeview, TVM_GETNEXTITEM, TVGN_DROPHILITE, 0));
      TreeView_SelectDropTarget(editor->treeview, NULL);

      if (hTarget && hTarget != editor->drag_item && editor->doc) {
        // Get source and target info
        TVITEMW src_item = {.mask = TVIF_PARAM, .hItem = editor->drag_item};
        TVITEMW dst_item = {.mask = TVIF_PARAM, .hItem = hTarget};
        TreeView_GetItem(editor->treeview, &src_item);
        TreeView_GetItem(editor->treeview, &dst_item);

        uint32_t src_id = 0, dst_id = 0;
        bool src_is_selector = treeview_decode_lparam(src_item.lParam, &src_id);
        bool dst_is_selector = treeview_decode_lparam(dst_item.lParam, &dst_id);

        size_t src_sel = 0, src_item_idx = 0, dst_sel = 0, dst_item_idx = 0;
        bool src_found = false, dst_found = false;
        if (src_is_selector) {
          src_found = ptk_anm2_find_selector_by_id(editor->doc, src_id, &src_sel);
        } else {
          src_found = ptk_anm2_find_item_by_id(editor->doc, src_id, &src_sel, &src_item_idx);
        }
        if (dst_is_selector) {
          dst_found = ptk_anm2_find_selector_by_id(editor->doc, dst_id, &dst_sel);
        } else {
          dst_found = ptk_anm2_find_item_by_id(editor->doc, dst_id, &dst_sel, &dst_item_idx);
        }

        if (src_found && dst_found) {
          handle_drag_move(editor, src_is_selector, src_sel, src_item_idx, dst_is_selector, dst_sel, dst_item_idx);
        }
      }

      editor->drag_item = NULL;
      return 0;
    }
    break;

  case WM_CLOSE:
    if (editor) {
      // Commit any pending inline edit
      commit_inline_edit(editor);
      if (!confirm_discard_changes(editor)) {
        return 0;
      }
    }
    DestroyWindow(hwnd);
    return 0;

  case WM_DESTROY:
    if (editor) {
      if (editor->toolbar_imagelist) {
        ImageList_Destroy(editor->toolbar_imagelist);
        editor->toolbar_imagelist = NULL;
      }
      if (editor->toolbar_disabled_imagelist) {
        ImageList_Destroy(editor->toolbar_disabled_imagelist);
        editor->toolbar_disabled_imagelist = NULL;
      }
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
      .edit_row = -1,
      .edit_column = -1,
      .edit = edit_handle,
  };

  editor->doc = ptk_anm2_new(err);
  if (!editor->doc) {
    OV_ERROR_ADD_TRACE(err);
    goto cleanup;
  }

  // Register change callback for focus tracking
  ptk_anm2_set_change_callback(editor->doc, on_doc_change, editor);

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
      if (editor->doc) {
        ptk_anm2_destroy(&editor->doc);
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
  // Note: Toolbar is a child window and is automatically destroyed with the parent
  editor->toolbar = NULL;
  editor->treeview = NULL;

  if (editor->window_class) {
    UnregisterClassW(anm2editor_window_class_name, GetModuleHandleW(NULL));
    editor->window_class = 0;
  }

  if (editor->doc) {
    ptk_anm2_destroy(&editor->doc);
  }

  if (editor->file_path) {
    OV_ARRAY_DESTROY(&editor->file_path);
  }

  if (editor->selected_item_ids) {
    OV_ARRAY_DESTROY(&editor->selected_item_ids);
  }

  OV_FREE(editor_ptr);
}

void *ptk_anm2editor_get_window(struct ptk_anm2editor *editor) { return editor ? editor->window : NULL; }

bool ptk_anm2editor_new(struct ptk_anm2editor *editor, struct ov_error *err) {
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
  if (!editor || !editor->doc) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_invalid_argument);
    return false;
  }

  if (!confirm_discard_changes(editor)) {
    return true; // User cancelled, not an error
  }

  struct ptk_anm2 *temp_doc = NULL;
  wchar_t *selected_path = NULL;
  wchar_t *default_dir = NULL;
  bool success = false;

  if (!path) {
    // Get default directory (Script folder)
    if (!get_script_dir(&default_dir, err)) {
      OV_ERROR_ADD_TRACE(err);
      goto cleanup;
    }

    // Show open dialog
    wchar_t title[256];
    ov_snprintf_char2wchar(
        title, sizeof(title) / sizeof(title[0]), "%1$hs", "%1$hs", pgettext("anm2editor", "Open"), NULL);
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

  // Load into a temporary document first to verify checksum
  temp_doc = ptk_anm2_new(err);
  if (!temp_doc) {
    OV_ERROR_ADD_TRACE(err);
    goto cleanup;
  }

  if (!ptk_anm2_load(temp_doc, selected_path, err)) {
    OV_ERROR_ADD_TRACE(err);
    goto cleanup;
  }

  // Verify checksum - warn if file was manually edited
  if (!ptk_anm2_verify_checksum(temp_doc)) {
    wchar_t main_instr[256];
    wchar_t content[512];
    ov_snprintf_char2wchar(main_instr,
                           sizeof(main_instr) / sizeof(main_instr[0]),
                           NULL,
                           pgettext("anm2editor", "Do you want to continue opening this file?"),
                           NULL);
    ov_snprintf_char2wchar(content,
                           sizeof(content) / sizeof(content[0]),
                           NULL,
                           pgettext("anm2editor",
                                    "This file appears to have been manually edited. "
                                    "If you continue editing in this editor, the manual changes may be lost."),
                           NULL);
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

  // Verification passed - load into the actual document
  // The callback is already registered, so reset notification will trigger TreeView rebuild
  if (!ptk_anm2_load(editor->doc, selected_path, err)) {
    OV_ERROR_ADD_TRACE(err);
    goto cleanup;
  }

  // Update file path
  if (editor->file_path) {
    OV_ARRAY_DESTROY(&editor->file_path);
  }
  editor->file_path = selected_path;
  selected_path = NULL; // Transfer ownership

  editor->modified = false;

  // Update UI (TreeView is already updated via reset notification)
  update_detail_panel(editor);
  update_window_title(editor);
  update_toolbar_state(editor);

  success = true;

cleanup:
  if (temp_doc) {
    ptk_anm2_destroy(&temp_doc);
  }
  if (default_dir) {
    OV_ARRAY_DESTROY(&default_dir);
  }
  if (selected_path) {
    OV_ARRAY_DESTROY(&selected_path);
  }
  return success;
}

bool ptk_anm2editor_save(struct ptk_anm2editor *editor, struct ov_error *err) {
  if (!editor || !editor->doc) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_invalid_argument);
    return false;
  }

  if (!editor->file_path || editor->file_path[0] == L'\0') {
    return ptk_anm2editor_save_as(editor, err);
  }

  if (!ptk_anm2_save(editor->doc, editor->file_path, err)) {
    OV_ERROR_ADD_TRACE(err);
    return false;
  }

  editor->modified = false;
  update_window_title(editor);
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
    char const *const psd_path = ptk_anm2_get_psd_path(editor->doc);
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
    ov_snprintf_char2wchar(
        title, sizeof(title) / sizeof(title[0]), "%1$hs", "%1$hs", pgettext("anm2editor", "Save As"), NULL);
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

  if (!ptk_anm2_save(editor->doc, selected_path, err)) {
    OV_ERROR_ADD_TRACE(err);
    goto cleanup;
  }

  if (editor->file_path) {
    OV_ARRAY_DESTROY(&editor->file_path);
  }
  editor->file_path = selected_path;
  selected_path = NULL;

  editor->modified = false;
  update_window_title(editor);

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

bool ptk_anm2editor_is_modified(struct ptk_anm2editor *editor) { return editor ? editor->modified : false; }

bool ptk_anm2editor_is_open(struct ptk_anm2editor *editor) {
  if (!editor || !editor->window) {
    return false;
  }
  return IsWindowVisible(editor->window) != 0;
}

char const *ptk_anm2editor_get_psd_path(struct ptk_anm2editor *editor) {
  if (!editor || !editor->doc) {
    return NULL;
  }
  return ptk_anm2_get_psd_path(editor->doc);
}

bool ptk_anm2editor_set_psd_path(struct ptk_anm2editor *editor, char const *path, struct ov_error *err) {
  if (!editor || !editor->doc) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_invalid_argument);
    return false;
  }

  if (!ptk_anm2_set_psd_path(editor->doc, path ? path : "", err)) {
    OV_ERROR_ADD_TRACE(err);
    return false;
  }

  mark_modified(editor);
  update_detail_panel(editor);
  return true;
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
  char const *current_psd_path = ptk_anm2_get_psd_path(editor->doc);
  if (!current_psd_path || current_psd_path[0] == '\0') {
    return true;
  }
  if (strcmp(current_psd_path, psd_path) == 0) {
    return true;
  }
  // Mismatch - show warning dialog
  wchar_t main_instr[256];
  ov_snprintf_char2wchar(main_instr, 256, NULL, pgettext("anm2editor", "Do you want to continue adding?"));
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
  char const *current_psd_path = ptk_anm2_get_psd_path(editor->doc);
  if (current_psd_path && current_psd_path[0] != '\0') {
    return true;
  }
  return ptk_anm2_set_psd_path(editor->doc, psd_path, err);
}

bool ptk_anm2editor_add_value_items(struct ptk_anm2editor *editor,
                                    char const *psd_path,
                                    char const *group,
                                    char const *const *names,
                                    char const *const *values,
                                    size_t count,
                                    struct ov_error *err) {
  if (!editor || !editor->doc) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_invalid_argument);
    return false;
  }

  if (!check_psd_path_mismatch(editor, psd_path)) {
    return true; // User cancelled, not an error
  }

  bool transaction_started = false;
  bool success = false;
  size_t sel_idx = 0;

  // Use transaction to group all operations for single undo
  if (!ptk_anm2_begin_transaction(editor->doc, err)) {
    OV_ERROR_ADD_TRACE(err);
    goto cleanup;
  }
  transaction_started = true;

  if (!set_psd_path_if_empty(editor, psd_path, err)) {
    OV_ERROR_ADD_TRACE(err);
    goto cleanup;
  }

  // Add the selector
  if (!ptk_anm2_selector_add(editor->doc, group ? group : "", err)) {
    OV_ERROR_ADD_TRACE(err);
    goto cleanup;
  }

  sel_idx = ptk_anm2_selector_count(editor->doc) - 1;

  // Add value items to the selector
  for (size_t i = 0; i < count; i++) {
    char const *name = names ? names[i] : "";
    char const *value = values ? values[i] : "";
    if (!ptk_anm2_item_add_value(editor->doc, sel_idx, name ? name : "", value ? value : "", err)) {
      OV_ERROR_ADD_TRACE(err);
      goto cleanup;
    }
  }

  success = true;

cleanup:
  if (transaction_started) {
    if (!ptk_anm2_end_transaction(editor->doc, err)) {
      if (success) {
        OV_ERROR_ADD_TRACE(err);
        success = false;
      }
    }
  }

  if (success) {
    mark_modified(editor);
  }

  return success;
}

bool ptk_anm2editor_add_value_item_to_selected(struct ptk_anm2editor *editor,
                                               char const *psd_path,
                                               char const *group,
                                               char const *name,
                                               char const *value,
                                               struct ov_error *err) {
  if (!editor || !editor->doc) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_invalid_argument);
    return false;
  }

  if (!check_psd_path_mismatch(editor, psd_path)) {
    return true; // User cancelled, not an error
  }

  bool transaction_started = false;
  bool success = false;
  size_t sel_idx = ptk_anm2editor_get_selected_selector_index(editor);

  // Use transaction to group all operations for single undo
  if (!ptk_anm2_begin_transaction(editor->doc, err)) {
    OV_ERROR_ADD_TRACE(err);
    goto cleanup;
  }
  transaction_started = true;

  if (!set_psd_path_if_empty(editor, psd_path, err)) {
    OV_ERROR_ADD_TRACE(err);
    goto cleanup;
  }

  if (sel_idx == SIZE_MAX) {
    // No selector selected, create a new one
    if (!ptk_anm2_selector_add(editor->doc, group ? group : "", err)) {
      OV_ERROR_ADD_TRACE(err);
      goto cleanup;
    }
    sel_idx = ptk_anm2_selector_count(editor->doc) - 1;
  }

  // Add value item to the selector
  if (!ptk_anm2_item_add_value(editor->doc, sel_idx, name ? name : "", value ? value : "", err)) {
    OV_ERROR_ADD_TRACE(err);
    goto cleanup;
  }

  success = true;

cleanup:
  if (transaction_started) {
    if (!ptk_anm2_end_transaction(editor->doc, err)) {
      if (success) {
        OV_ERROR_ADD_TRACE(err);
        success = false;
      }
    }
  }

  if (success) {
    mark_modified(editor);
  }

  return success;
}

void ptk_anm2editor_ptkl_targets_free(struct ptk_anm2editor_ptkl_targets *targets) {
  if (!targets) {
    return;
  }
  if (targets->items) {
    size_t const len = OV_ARRAY_LENGTH(targets->items);
    for (size_t i = 0; i < len; i++) {
      struct ptk_anm2editor_ptkl_target *t = &targets->items[i];
      if (t->selector_name) {
        OV_ARRAY_DESTROY(&t->selector_name);
      }
      if (t->effect_name) {
        OV_ARRAY_DESTROY(&t->effect_name);
      }
      if (t->param_key) {
        OV_ARRAY_DESTROY(&t->param_key);
      }
    }
    OV_ARRAY_DESTROY(&targets->items);
  }
  *targets = (struct ptk_anm2editor_ptkl_targets){0};
}

static bool ends_with_ptkl(char const *s) {
  if (!s) {
    return false;
  }
  size_t const len = strlen(s);
  if (len < 5) {
    return false;
  }
  return strcmp(s + len - 5, "~ptkl") == 0;
}

static bool add_ptkl_target(struct ptk_anm2editor_ptkl_targets *targets,
                            char const *selector_name,
                            char const *effect_name,
                            char const *param_key,
                            size_t sel_idx,
                            size_t item_idx,
                            size_t param_idx,
                            struct ov_error *err) {
  size_t const current_len = OV_ARRAY_LENGTH(targets->items);
  if (!OV_ARRAY_GROW(&targets->items, current_len + 1)) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_out_of_memory);
    return false;
  }

  struct ptk_anm2editor_ptkl_target *t = &targets->items[current_len];
  *t = (struct ptk_anm2editor_ptkl_target){
      .sel_idx = sel_idx,
      .item_idx = item_idx,
      .param_idx = param_idx,
  };

  if (selector_name) {
    size_t const len = strlen(selector_name);
    if (!OV_ARRAY_GROW(&t->selector_name, len + 1)) {
      OV_ERROR_SET_GENERIC(err, ov_error_generic_out_of_memory);
      return false;
    }
    memcpy(t->selector_name, selector_name, len + 1);
    OV_ARRAY_SET_LENGTH(t->selector_name, len);
  }

  if (effect_name) {
    size_t const len = strlen(effect_name);
    if (!OV_ARRAY_GROW(&t->effect_name, len + 1)) {
      OV_ERROR_SET_GENERIC(err, ov_error_generic_out_of_memory);
      return false;
    }
    memcpy(t->effect_name, effect_name, len + 1);
    OV_ARRAY_SET_LENGTH(t->effect_name, len);
  }

  if (param_key) {
    size_t const len = strlen(param_key);
    if (!OV_ARRAY_GROW(&t->param_key, len + 1)) {
      OV_ERROR_SET_GENERIC(err, ov_error_generic_out_of_memory);
      return false;
    }
    memcpy(t->param_key, param_key, len + 1);
    OV_ARRAY_SET_LENGTH(t->param_key, len);
  }

  OV_ARRAY_SET_LENGTH(targets->items, current_len + 1);
  return true;
}

bool ptk_anm2editor_collect_selected_ptkl_targets(struct ptk_anm2editor *editor,
                                                  struct ptk_anm2editor_ptkl_targets *targets,
                                                  struct ov_error *err) {
  if (!editor || !targets) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_invalid_argument);
    return false;
  }

  *targets = (struct ptk_anm2editor_ptkl_targets){0};

  if (!editor->doc) {
    return true;
  }

  // Get currently selected selector
  size_t sel_idx = 0;
  size_t item_idx = 0;
  int const sel_type = get_selected_indices(editor, &sel_idx, &item_idx);
  if (sel_type == 0) {
    // Nothing selected
    return true;
  }

  // sel_type 1 or 2 means a selector (or item within it) is selected
  if (sel_idx >= ptk_anm2_selector_count(editor->doc)) {
    return true;
  }

  size_t const num_items = ptk_anm2_item_count(editor->doc, sel_idx);
  for (size_t i = 0; i < num_items; i++) {
    if (!ptk_anm2_item_is_animation(editor->doc, sel_idx, i)) {
      continue;
    }

    size_t const num_params = ptk_anm2_param_count(editor->doc, sel_idx, i);
    for (size_t param_idx = 0; param_idx < num_params; param_idx++) {
      char const *key = ptk_anm2_param_get_key(editor->doc, sel_idx, i, param_idx);
      if (!ends_with_ptkl(key)) {
        continue;
      }

      char const *group = ptk_anm2_selector_get_group(editor->doc, sel_idx);
      char const *name = ptk_anm2_item_get_name(editor->doc, sel_idx, i);
      if (!add_ptkl_target(targets, group, name, key, sel_idx, i, param_idx, err)) {
        OV_ERROR_ADD_TRACE(err);
        ptk_anm2editor_ptkl_targets_free(targets);
        return false;
      }
    }
  }

  return true;
}

bool ptk_anm2editor_set_param_value(struct ptk_anm2editor *editor,
                                    size_t sel_idx,
                                    size_t item_idx,
                                    size_t param_idx,
                                    char const *value,
                                    struct ov_error *err) {
  if (!editor || !editor->doc || !value) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_invalid_argument);
    return false;
  }

  if (!ptk_anm2_param_set_value(editor->doc, sel_idx, item_idx, param_idx, value, err)) {
    OV_ERROR_ADD_TRACE(err);
    return false;
  }

  mark_modified(editor);
  update_detail_panel(editor);
  return true;
}

size_t ptk_anm2editor_get_selected_selector_index(struct ptk_anm2editor *editor) {
  if (!editor || !editor->doc) {
    return SIZE_MAX;
  }

  size_t sel_idx = 0;
  size_t item_idx = 0;
  int const sel_type = get_selected_indices(editor, &sel_idx, &item_idx);

  // Return the selector index if something is selected
  if (sel_type == 0) {
    return SIZE_MAX;
  }

  // Verify the selector index is valid
  if (sel_idx >= ptk_anm2_selector_count(editor->doc)) {
    return SIZE_MAX;
  }

  return sel_idx;
}

bool ptk_anm2editor_add_animation_item(struct ptk_anm2editor *editor,
                                       size_t sel_idx,
                                       char const *script_name,
                                       char const *display_name,
                                       struct ptk_alias_extracted_param const *params,
                                       size_t param_count,
                                       struct ov_error *err) {
  if (!editor || !editor->doc || !script_name || !display_name) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_invalid_argument);
    return false;
  }

  // Verify selector index
  if (sel_idx >= ptk_anm2_selector_count(editor->doc)) {
    OV_ERROR_SET(err, ov_error_type_generic, ov_error_generic_invalid_argument, "selector index out of range");
    return false;
  }

  // Use transaction to group all operations for single undo
  if (!ptk_anm2_begin_transaction(editor->doc, err)) {
    OV_ERROR_ADD_TRACE(err);
    return false;
  }

  bool success = false;

  // Insert animation item at the beginning of the selector
  if (!ptk_anm2_item_insert_animation(editor->doc, sel_idx, 0, script_name, display_name, err)) {
    OV_ERROR_ADD_TRACE(err);
    goto cleanup;
  }

  // Add parameters to the inserted animation item (which is now at index 0)
  for (size_t i = 0; i < param_count; i++) {
    if (params[i].key && params[i].value) {
      if (!ptk_anm2_param_add(editor->doc, sel_idx, 0, params[i].key, params[i].value, err)) {
        OV_ERROR_ADD_TRACE(err);
        goto cleanup;
      }
    }
  }

  success = true;

cleanup:
  if (!ptk_anm2_end_transaction(editor->doc, err)) {
    if (success) {
      OV_ERROR_ADD_TRACE(err);
      success = false;
    }
  }

  if (success) {
    mark_modified(editor);
  }

  return success;
}
