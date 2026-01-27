#include "anm2_script_picker.h"

#include "alias.h"

#include <string.h>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <commctrl.h>
#include <shellapi.h>

#include <ovarray.h>
#include <ovl/os.h>
#include <ovmo.h>
#include <ovprintf.h>
#include <ovutf.h>

#include "dialog.h"
#include "win32.h"

// Dialog control IDs
enum {
  id_label = 101,
  id_listview = 102,
  id_select_all = 103,
  id_deselect_all = 104,
  id_psd_warning = 105,
  id_update_psd = 106,
  id_psd_icon = 107,
  id_psd_group = 108,
};

// Dialog data passed through window property
static wchar_t const g_dialog_prop_name[] = L"PTKScriptPickerData";

struct dialog_data {
  struct ptk_script_picker_params *params;
  bool show_psd_warning; // True if PSD paths differ
  bool ok_pressed;
  HFONT dialog_font;
};

/**
 * @brief Check font availability
 */
static bool check_font_availability(wchar_t const *const font_name) {
  if (!font_name || !font_name[0]) {
    return false;
  }

  wchar_t actual_name[LF_FACESIZE] = {0};
  HFONT hfont = NULL;
  HFONT old_font = NULL;
  bool result = false;

  HDC hdc = GetDC(NULL);
  if (!hdc) {
    goto cleanup;
  }
  hfont = CreateFontW(0,
                      0,
                      0,
                      0,
                      FW_NORMAL,
                      FALSE,
                      FALSE,
                      FALSE,
                      DEFAULT_CHARSET,
                      OUT_DEFAULT_PRECIS,
                      CLIP_DEFAULT_PRECIS,
                      DEFAULT_QUALITY,
                      DEFAULT_PITCH | FF_DONTCARE,
                      font_name);
  if (!hfont) {
    goto cleanup;
  }
  old_font = (HFONT)SelectObject(hdc, hfont);
  if (!old_font) {
    goto cleanup;
  }
  if (!GetTextFaceW(hdc, LF_FACESIZE, actual_name)) {
    goto cleanup;
  }
  result = wcscmp(font_name, actual_name) == 0;

cleanup:
  if (old_font && hdc) {
    SelectObject(hdc, old_font);
  }
  if (hfont) {
    DeleteObject(hfont);
  }
  if (hdc) {
    ReleaseDC(NULL, hdc);
  }
  return result;
}

/**
 * @brief Create dialog font from font list
 */
static HFONT create_dialog_font(HWND const dialog, char const *const font_list_utf8) {
  if (!dialog || !font_list_utf8 || !font_list_utf8[0]) {
    return NULL;
  }

  HDC hdc = NULL;
  HFONT hfont = NULL;
  HFONT current_font = NULL;
  int font_height = 0;

  hdc = GetDC(NULL);
  if (!hdc) {
    goto cleanup;
  }

  // Get font size from dialog resource
  current_font = (HFONT)(SendMessageW(dialog, WM_GETFONT, 0, 0));
  if (current_font) {
    LOGFONTW current_logfont = {0};
    if (GetObjectW(current_font, sizeof(LOGFONTW), &current_logfont)) {
      font_height = current_logfont.lfHeight;
    }
  }
  if (font_height == 0) {
    font_height = -MulDiv(9, GetDeviceCaps(hdc, LOGPIXELSY), 72);
  }

  {
    char const *start = font_list_utf8;
    while (start && *start) {
      char const *end = strchr(start, '\n');
      size_t len = end ? (size_t)(end - start) : strlen(start);

      while (len > 0 && (*start == ' ' || *start == '\t' || *start == '\r')) {
        start++;
        len--;
      }
      while (len > 0 && (start[len - 1] == ' ' || start[len - 1] == '\t' || start[len - 1] == '\r')) {
        len--;
      }

      if (len > 0) {
        wchar_t font_name[LF_FACESIZE];
        if (ov_utf8_to_wchar(start, len, font_name, LF_FACESIZE, NULL) > 0) {
          if (check_font_availability(font_name)) {
            hfont = CreateFontW(font_height,
                                0,
                                0,
                                0,
                                FW_NORMAL,
                                FALSE,
                                FALSE,
                                FALSE,
                                DEFAULT_CHARSET,
                                OUT_DEFAULT_PRECIS,
                                CLIP_DEFAULT_PRECIS,
                                DEFAULT_QUALITY,
                                DEFAULT_PITCH | FF_DONTCARE,
                                font_name);
            if (hfont) {
              goto cleanup;
            }
          }
        }
      }

      start = end ? end + 1 : NULL;
    }
  }

  hfont = CreateFontW(font_height,
                      0,
                      0,
                      0,
                      FW_NORMAL,
                      FALSE,
                      FALSE,
                      FALSE,
                      DEFAULT_CHARSET,
                      OUT_DEFAULT_PRECIS,
                      CLIP_DEFAULT_PRECIS,
                      DEFAULT_QUALITY,
                      DEFAULT_PITCH | FF_DONTCARE,
                      L"Tahoma");

cleanup:
  if (hdc) {
    ReleaseDC(NULL, hdc);
  }
  return hfont;
}

/**
 * @brief Set font for dialog and all children
 */
static void set_dialog_font(HWND const hwnd, HFONT const hfont) {
  if (!hwnd || !hfont) {
    return;
  }
  SendMessageW(hwnd, WM_SETFONT, (WPARAM)hfont, FALSE);
  HWND child = GetWindow(hwnd, GW_CHILD);
  while (child) {
    set_dialog_font(child, hfont);
    child = GetWindow(child, GW_HWNDNEXT);
  }
}

/**
 * @brief Initialize the listview with script picker items
 */
static void init_listview(HWND const listview, struct ptk_script_picker_params *const params) {
  // Set extended styles for checkboxes
  ListView_SetExtendedListViewStyle(listview, LVS_EX_CHECKBOXES | LVS_EX_FULLROWSELECT);

  // Add a single column that spans full width
  RECT rc;
  GetClientRect(listview, &rc);

  LVCOLUMNW col = {
      .mask = LVCF_WIDTH,
      .cx = rc.right - rc.left - GetSystemMetrics(SM_CXVSCROLL),
  };
  SendMessageW(listview, LVM_INSERTCOLUMNW, 0, (LPARAM)&col);

  // Add items
  for (size_t i = 0; i < params->item_count; ++i) {
    wchar_t text[256];

    // Use translated name if available, otherwise convert effect_name from UTF-8
    if (params->items[i].translated_name) {
      size_t const len = wcslen(params->items[i].translated_name);
      size_t const copy_len = len < 255 ? len : 255;
      memcpy(text, params->items[i].translated_name, copy_len * sizeof(wchar_t));
      text[copy_len] = L'\0';
    } else {
      ov_utf8_to_wchar(params->items[i].effect_name, strlen(params->items[i].effect_name), text, 256, NULL);
    }

    LVITEMW item = {
        .mask = LVIF_TEXT,
        .iItem = (int)i,
        .pszText = text,
    };
    SendMessageW(listview, LVM_INSERTITEMW, 0, (LPARAM)&item);

    // Set check state using SendMessage directly
    LVITEMW state_item = {
        .stateMask = LVIS_STATEIMAGEMASK,
        .state = (UINT)(INDEXTOSTATEIMAGEMASK(params->items[i].selected ? 2 : 1)),
    };
    SendMessageW(listview, LVM_SETITEMSTATE, (WPARAM)i, (LPARAM)&state_item);
  }
}

/**
 * @brief Update item check state from listview
 */
static void update_selection_from_listview(HWND const listview, struct ptk_script_picker_params *const params) {
  for (size_t i = 0; i < params->item_count; ++i) {
    // Get check state: state image index 2 = checked, 1 = unchecked
    UINT const state = (UINT)SendMessageW(listview, LVM_GETITEMSTATE, (WPARAM)i, LVIS_STATEIMAGEMASK);
    params->items[i].selected = ((state >> 12) - 1) != 0;
  }
}

/**
 * @brief Set all checkboxes to specified state
 */
static void set_all_checkboxes(HWND const listview, bool const checked) {
  int const count = ListView_GetItemCount(listview);
  LVITEMW state_item = {
      .stateMask = LVIS_STATEIMAGEMASK,
      .state = (UINT)(INDEXTOSTATEIMAGEMASK(checked ? 2 : 1)),
  };
  for (int i = 0; i < count; ++i) {
    SendMessageW(listview, LVM_SETITEMSTATE, (WPARAM)i, (LPARAM)&state_item);
  }
}

/**
 * @brief Compare PSD paths
 *
 * @param path1 First path (can be NULL)
 * @param path2 Second path (can be NULL)
 * @return true if paths are equal (or both NULL/empty)
 */
static bool compare_psd_paths(char const *path1, char const *path2) {
  if (!path1 || path1[0] == '\0') {
    return !path2 || path2[0] == '\0';
  }
  if (!path2 || path2[0] == '\0') {
    return false;
  }
  return strcmp(path1, path2) == 0;
}

/**
 * @brief Setup warning controls and adjust positions
 */
static void setup_warning(HWND const dialog, struct dialog_data *const data) {
  static wchar_t const ph[] = L"%1$s";
  wchar_t buf[512];

  // Set warning icon
  SHSTOCKICONINFO sii = {.cbSize = sizeof(SHSTOCKICONINFO)};
  SHGetStockIconInfo(SIID_WARNING, SHGSI_ICON, &sii);
  HICON hIcon = sii.hIcon;
  SendMessageW(GetDlgItem(dialog, id_psd_icon), STM_SETICON, (WPARAM)hIcon, 0);

  // Adjust positions based on icon size in dialog units
  RECT icon_rect;
  GetWindowRect(GetDlgItem(dialog, id_psd_icon), &icon_rect);
  MapWindowPoints(HWND_DESKTOP, dialog, (LPPOINT)&icon_rect, 2);
  int icon_pixel_width = icon_rect.right - icon_rect.left;
  // Get group box dimensions dynamically
  RECT group_rect;
  GetWindowRect(GetDlgItem(dialog, id_psd_group), &group_rect);
  MapWindowPoints(HWND_DESKTOP, dialog, (LPPOINT)&group_rect, 2);
  // Get group box client area to avoid overflowing the border
  RECT group_client_rect;
  GetClientRect(GetDlgItem(dialog, id_psd_group), &group_client_rect);
  int group_client_right = group_rect.left + group_client_rect.right;
  // Calculate margin as the distance from group box left to icon left
  int margin_pixel = icon_rect.left - group_rect.left;

  // New X position for text and checkbox: icon left + icon width + margin
  int text_x_pixel = icon_rect.left + icon_pixel_width + margin_pixel;
  int text_width_pixel = group_client_right - text_x_pixel - 4;

  // Get current positions and adjust X and width
  RECT current_text_rect;
  GetWindowRect(GetDlgItem(dialog, id_psd_warning), &current_text_rect);
  MapWindowPoints(HWND_DESKTOP, dialog, (LPPOINT)&current_text_rect, 2);
  int text_y_pixel = current_text_rect.top;
  int text_height_pixel = current_text_rect.bottom - current_text_rect.top;

  MoveWindow(GetDlgItem(dialog, id_psd_warning), text_x_pixel, text_y_pixel, text_width_pixel, text_height_pixel, TRUE);

  RECT current_checkbox_rect;
  GetWindowRect(GetDlgItem(dialog, id_update_psd), &current_checkbox_rect);
  MapWindowPoints(HWND_DESKTOP, dialog, (LPPOINT)&current_checkbox_rect, 2);
  int checkbox_y_pixel = current_checkbox_rect.top;
  int checkbox_height_pixel = current_checkbox_rect.bottom - current_checkbox_rect.top;

  MoveWindow(
      GetDlgItem(dialog, id_update_psd), text_x_pixel, checkbox_y_pixel, text_width_pixel, checkbox_height_pixel, TRUE);

  // Show warning text with paths
  char const *source_path = data->params->source_psd_path ? data->params->source_psd_path : "";
  char const *current_path = data->params->current_psd_path ? data->params->current_psd_path : "";
  ov_snprintf_char2wchar(buf,
                         sizeof(buf) / sizeof(wchar_t),
                         "%1$hs%2$hs",
                         pgettext("script_picker",
                                  "These scripts are assigned to a different PSD file.\n\n"
                                  "Current Editor: %1$hs\n"
                                  "Importing From: %2$hs"),
                         current_path && current_path[0] != '\0' ? current_path : pgettext("script_picker", "(empty)"),
                         source_path && source_path[0] != '\0' ? source_path : pgettext("script_picker", "(empty)"));
  SetWindowTextW(GetDlgItem(dialog, id_psd_warning), buf);

  // Set checkbox label
  ov_snprintf_wchar(buf, sizeof(buf) / sizeof(wchar_t), ph, ph, pgettext("script_picker", "Update PSD file path"));
  SetWindowTextW(GetDlgItem(dialog, id_update_psd), buf);

  // Default to checked if current path is empty
  bool const default_checked = !data->params->current_psd_path || data->params->current_psd_path[0] == '\0';
  SendMessageW(GetDlgItem(dialog, id_update_psd), BM_SETCHECK, default_checked ? BST_CHECKED : BST_UNCHECKED, 0);
}

/**
 * @brief Hide warning controls and adjust dialog size
 */
static void hide_warning_and_adjust_dialog(HWND const dialog) {
  // Hide warning and checkbox
  ShowWindow(GetDlgItem(dialog, id_psd_warning), SW_HIDE);
  ShowWindow(GetDlgItem(dialog, id_update_psd), SW_HIDE);
  ShowWindow(GetDlgItem(dialog, id_psd_icon), SW_HIDE);
  ShowWindow(GetDlgItem(dialog, id_psd_group), SW_HIDE);

  // Move buttons up and resize dialog
  RECT group_rect;
  GetWindowRect(GetDlgItem(dialog, id_psd_group), &group_rect);
  MapWindowPoints(HWND_DESKTOP, dialog, (LPPOINT)&group_rect, 2);
  int group_y = group_rect.top;

  RECT ok_rect;
  GetWindowRect(GetDlgItem(dialog, IDOK), &ok_rect);
  MapWindowPoints(HWND_DESKTOP, dialog, (LPPOINT)&ok_rect, 2);
  int ok_width = ok_rect.right - ok_rect.left;
  int ok_height = ok_rect.bottom - ok_rect.top;
  int offset = ok_rect.top - group_y;
  MoveWindow(GetDlgItem(dialog, IDOK), ok_rect.left, ok_rect.top - offset, ok_width, ok_height, TRUE);

  RECT cancel_rect;
  GetWindowRect(GetDlgItem(dialog, IDCANCEL), &cancel_rect);
  MapWindowPoints(HWND_DESKTOP, dialog, (LPPOINT)&cancel_rect, 2);
  int cancel_width = cancel_rect.right - cancel_rect.left;
  int cancel_height = cancel_rect.bottom - cancel_rect.top;
  MoveWindow(
      GetDlgItem(dialog, IDCANCEL), cancel_rect.left, cancel_rect.top - offset, cancel_width, cancel_height, TRUE);

  // Resize dialog
  RECT dialog_rect;
  GetWindowRect(dialog, &dialog_rect);
  int dialog_width = dialog_rect.right - dialog_rect.left;
  int new_dialog_height = (dialog_rect.bottom - dialog_rect.top) - offset;
  SetWindowPos(dialog, NULL, 0, 0, dialog_width, new_dialog_height, SWP_NOMOVE | SWP_NOZORDER);
}

/**
 * @brief Dialog initialization
 */
static INT_PTR init_dialog(HWND const dialog, struct dialog_data *const data) {
  SetPropW(dialog, g_dialog_prop_name, data);

  static wchar_t const ph[] = L"%1$s";
  wchar_t buf[512];

  // Set up font
  static char const font_list_key[] = gettext_noop("dialog_ui_font");
  char const *font_list = gettext(font_list_key);
  if (strcmp(font_list, font_list_key) == 0) {
    font_list = "Segoe UI\nTahoma\nMS Sans Serif";
  }
  data->dialog_font = create_dialog_font(dialog, font_list);
  if (data->dialog_font) {
    set_dialog_font(dialog, data->dialog_font);
  }

  // Set dialog title
  ov_snprintf_wchar(buf, sizeof(buf) / sizeof(wchar_t), ph, ph, pgettext("script_picker", "Import Scripts"));
  SetWindowTextW(dialog, buf);

  // Set label text
  ov_snprintf_wchar(buf, sizeof(buf) / sizeof(wchar_t), ph, ph, pgettext("script_picker", "Select scripts to import:"));
  SetWindowTextW(GetDlgItem(dialog, id_label), buf);

  // Set button labels
  ov_snprintf_wchar(buf, sizeof(buf) / sizeof(wchar_t), ph, ph, pgettext("script_picker", "Import"));
  SetWindowTextW(GetDlgItem(dialog, IDOK), buf);
  ov_snprintf_wchar(buf, sizeof(buf) / sizeof(wchar_t), ph, ph, pgettext("script_picker", "Cancel"));
  SetWindowTextW(GetDlgItem(dialog, IDCANCEL), buf);
  ov_snprintf_wchar(buf, sizeof(buf) / sizeof(wchar_t), ph, ph, pgettext("script_picker", "Select All"));
  SetWindowTextW(GetDlgItem(dialog, id_select_all), buf);
  ov_snprintf_wchar(buf, sizeof(buf) / sizeof(wchar_t), ph, ph, pgettext("script_picker", "Deselect All"));
  SetWindowTextW(GetDlgItem(dialog, id_deselect_all), buf);

  // Initialize listview
  init_listview(GetDlgItem(dialog, id_listview), data->params);

  // Handle PSD warning and checkbox
  if (data->show_psd_warning) {
    setup_warning(dialog, data);
  } else {
    hide_warning_and_adjust_dialog(dialog);
  }

  return TRUE;
}

/**
 * @brief Dialog procedure
 */
static INT_PTR CALLBACK dialog_proc(HWND const dialog, UINT const msg, WPARAM const wparam, LPARAM const lparam) {
  struct dialog_data *data = (struct dialog_data *)GetPropW(dialog, g_dialog_prop_name);

  switch (msg) {
  case WM_INITDIALOG:
    return init_dialog(dialog, (struct dialog_data *)lparam);

  case WM_COMMAND:
    switch (LOWORD(wparam)) {
    case IDOK:
      update_selection_from_listview(GetDlgItem(dialog, id_listview), data->params);
      // Get PSD update checkbox state
      if (data->show_psd_warning) {
        data->params->update_psd_path =
            SendMessageW(GetDlgItem(dialog, id_update_psd), BM_GETCHECK, 0, 0) == BST_CHECKED;
      }
      data->ok_pressed = true;
      EndDialog(dialog, IDOK);
      return TRUE;

    case IDCANCEL:
      EndDialog(dialog, IDCANCEL);
      return TRUE;

    case id_select_all:
      set_all_checkboxes(GetDlgItem(dialog, id_listview), true);
      return TRUE;

    case id_deselect_all:
      set_all_checkboxes(GetDlgItem(dialog, id_listview), false);
      return TRUE;
    }
    break;

  case WM_DPICHANGED:
    if (data) {
      setup_warning(dialog, data);
    }
    return TRUE;

  case WM_DESTROY:
    if (data && data->dialog_font) {
      DeleteObject(data->dialog_font);
      data->dialog_font = NULL;
    }
    RemovePropW(dialog, g_dialog_prop_name);
    break;
  }

  return FALSE;
}

ov_tribool
ptk_script_picker_show(void *const parent, struct ptk_script_picker_params *const params, struct ov_error *const err) {
  if (!params || !params->items || params->item_count == 0) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_invalid_argument);
    return ov_indeterminate;
  }

  void *hinstance = NULL;
  ov_tribool result = ov_indeterminate;

  {
    if (!ovl_os_get_hinstance_from_fnptr((void *)ptk_script_picker_show, &hinstance, err)) {
      OV_ERROR_ADD_TRACE(err);
      goto cleanup;
    }

    // Determine if PSD paths differ
    bool const show_psd_warning = !compare_psd_paths(params->current_psd_path, params->source_psd_path);

    struct dialog_data data = {
        .params = params,
        .show_psd_warning = show_psd_warning,
        .ok_pressed = false,
        .dialog_font = NULL,
    };

    INT_PTR const dlg_result =
        DialogBoxParamW((HINSTANCE)hinstance, L"PTKSCRIPTPICKER", (HWND)parent, dialog_proc, (LPARAM)&data);

    if (dlg_result == -1) {
      OV_ERROR_SET_HRESULT(err, HRESULT_FROM_WIN32(GetLastError()));
      goto cleanup;
    }

    result = data.ok_pressed ? ov_true : ov_false;
  }

cleanup:
  return result;
}
