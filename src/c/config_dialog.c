#include "config_dialog.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <commctrl.h>

#include <ovmo.h>
#include <ovprintf.h>
#include <ovutf.h>

#include <ovl/os.h>

#include "config.h"
#include "win32.h"

enum {
  id_group_audio_drop = 100,

  id_group_trigger_conditions = 110,

  id_label_manual_drop = 120,
  id_check_manual_shift_wav = 121,
  id_check_manual_wav_txt_pair = 122,
  id_check_manual_object_audio_text = 123,

  id_label_external_api_drop = 130,
  id_check_external_wav_txt_pair = 131,
  id_check_external_object_audio_text = 132,

  id_group_psd_drop = 140,
  id_check_manual_shift_psd = 141,

  id_group_debug = 150,
  id_check_debug_mode = 151,
};

static NATIVE_CHAR const g_config_dialog_prop_name[] = L"PTKConfigDialogData";

struct dialog_data {
  struct ptk_config *config;
  HFONT dialog_font;
};

static bool check_font_availability(wchar_t const *font_name, struct ov_error *const err) {
  if (!font_name || !font_name[0]) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_invalid_argument);
    return false;
  }

  wchar_t actual_name[LF_FACESIZE] = {0};
  HFONT hfont = NULL;
  HFONT old_font = NULL;
  bool result = false;

  HDC hdc = GetDC(NULL);
  if (!hdc) {
    OV_ERROR_SET_HRESULT(err, HRESULT_FROM_WIN32(GetLastError()));
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
    OV_ERROR_SET_HRESULT(err, HRESULT_FROM_WIN32(GetLastError()));
    goto cleanup;
  }
  old_font = (HFONT)SelectObject(hdc, hfont);
  if (!old_font) {
    OV_ERROR_SET_HRESULT(err, HRESULT_FROM_WIN32(GetLastError()));
    goto cleanup;
  }
  if (!GetTextFaceW(hdc, LF_FACESIZE, actual_name)) {
    OV_ERROR_SET_HRESULT(err, HRESULT_FROM_WIN32(GetLastError()));
    goto cleanup;
  }
  if (wcscmp(font_name, actual_name) == 0) {
    result = true;
  }

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

static HFONT create_dialog_font(HWND dialog, char const *font_list_utf8, struct ov_error *const err) {
  if (!dialog || !font_list_utf8 || !font_list_utf8[0]) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_invalid_argument);
    return NULL;
  }

  HDC hdc = NULL;
  HFONT hfont = NULL;
  int font_height = 0;

  {
    hdc = GetDC(NULL);
    if (!hdc) {
      OV_ERROR_SET_HRESULT(err, HRESULT_FROM_WIN32(GetLastError()));
      goto cleanup;
    }

    // Get font size from dialog resource
    HFONT current_font = (HFONT)(SendMessageW(dialog, WM_GETFONT, 0, 0));
    if (current_font) {
      LOGFONTW current_logfont = {0};
      if (GetObjectW(current_font, sizeof(LOGFONTW), &current_logfont)) {
        font_height = current_logfont.lfHeight;
      }
    }
    if (font_height == 0) {
      // Fallback to 9 point if we can't get current font size
      font_height = -MulDiv(9, GetDeviceCaps(hdc, LOGPIXELSY), 72);
    }

    char const *start = font_list_utf8;
    while (start && *start) {
      char const *end = strchr(start, '\n');
      size_t len = end ? (size_t)(end - start) : strlen(start);

      // Trim whitespace
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
          struct ov_error check_err = {0};
          if (check_font_availability(font_name, &check_err)) {
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
          OV_ERROR_DESTROY(&check_err);
        }
      }

      start = end ? end + 1 : NULL;
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
  }

cleanup:
  if (hdc) {
    ReleaseDC(NULL, hdc);
  }
  return hfont;
}

static void set_dialog_font(HWND hwnd, HFONT hfont) {
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

static INT_PTR init_dialog(HWND dialog, struct dialog_data *data) {
  SetPropW(dialog, g_config_dialog_prop_name, data);

  static wchar_t const ph[] = L"%1$s";
  struct ov_error err = {0};
  WCHAR buf[256];

  // Set up font
  static char const font_list_key[] = gettext_noop("dialog_ui_font");
  char const *font_list = gettext(font_list_key);
  if (strcmp(font_list, font_list_key) == 0) {
    font_list = "Segoe UI\nTahoma\nMS Sans Serif";
  }
  data->dialog_font = create_dialog_font(dialog, font_list, &err);
  if (!data->dialog_font) {
    OV_ERROR_REPORT(&err, NULL);
  } else {
    set_dialog_font(dialog, data->dialog_font);
  }

  // Set dialog title
  ov_snprintf_wchar(buf, sizeof(buf) / sizeof(WCHAR), ph, ph, pgettext("config", "PSDToolKit Settings"));
  SetWindowTextW(dialog, buf);

  // Set button labels
  ov_snprintf_wchar(buf, sizeof(buf) / sizeof(WCHAR), ph, ph, pgettext("config", "OK"));
  SetWindowTextW(GetDlgItem(dialog, IDOK), buf);
  ov_snprintf_wchar(buf, sizeof(buf) / sizeof(WCHAR), ph, ph, pgettext("config", "Cancel"));
  SetWindowTextW(GetDlgItem(dialog, IDCANCEL), buf);

  // Audio File Drop Extension group
  ov_snprintf_wchar(buf, sizeof(buf) / sizeof(WCHAR), ph, ph, pgettext("config", "Audio File Drop Extension"));
  SetWindowTextW(GetDlgItem(dialog, id_group_audio_drop), buf);

  // Trigger Conditions group
  ov_snprintf_wchar(buf, sizeof(buf) / sizeof(WCHAR), ph, ph, pgettext("config", "Trigger Conditions"));
  SetWindowTextW(GetDlgItem(dialog, id_group_trigger_conditions), buf);

  // Manual drop heading
  ov_snprintf_wchar(buf, sizeof(buf) / sizeof(WCHAR), ph, ph, pgettext("config", "When dropping files manually:"));
  SetWindowTextW(GetDlgItem(dialog, id_label_manual_drop), buf);

  // Manual drop checkboxes
  ov_snprintf_wchar(buf,
                    sizeof(buf) / sizeof(WCHAR),
                    ph,
                    ph,
                    pgettext("config", "When dropping *.wav file while holding &Shift key"));
  SetWindowTextW(GetDlgItem(dialog, id_check_manual_shift_wav), buf);

  ov_snprintf_wchar(buf,
                    sizeof(buf) / sizeof(WCHAR),
                    ph,
                    ph,
                    pgettext("config", "When dropping *.wav and *.txt files with the same name &together"));
  SetWindowTextW(GetDlgItem(dialog, id_check_manual_wav_txt_pair), buf);

  ov_snprintf_wchar(buf,
                    sizeof(buf) / sizeof(WCHAR),
                    ph,
                    ph,
                    pgettext("config", "When dropping *.object containing only &audio and text on the same frame"));
  SetWindowTextW(GetDlgItem(dialog, id_check_manual_object_audio_text), buf);

  // External API drop heading
  ov_snprintf_wchar(buf, sizeof(buf) / sizeof(WCHAR), ph, ph, pgettext("config", "When dropping via external API:"));
  SetWindowTextW(GetDlgItem(dialog, id_label_external_api_drop), buf);

  // External API checkboxes
  ov_snprintf_wchar(buf,
                    sizeof(buf) / sizeof(WCHAR),
                    ph,
                    ph,
                    pgettext("config", "When dropping *.wav and *.txt files with the same name t&ogether"));
  SetWindowTextW(GetDlgItem(dialog, id_check_external_wav_txt_pair), buf);

  ov_snprintf_wchar(buf,
                    sizeof(buf) / sizeof(WCHAR),
                    ph,
                    ph,
                    pgettext("config", "When dropping *.object containing only a&udio and text on the same frame"));
  SetWindowTextW(GetDlgItem(dialog, id_check_external_object_audio_text), buf);

  // PSD File Drop group
  ov_snprintf_wchar(buf, sizeof(buf) / sizeof(WCHAR), ph, ph, pgettext("config", "PSD File Drop"));
  SetWindowTextW(GetDlgItem(dialog, id_group_psd_drop), buf);

  ov_snprintf_wchar(
      buf,
      sizeof(buf) / sizeof(WCHAR),
      ph,
      ph,
      pgettext("config", "Only create PSD file object when dropping *.&psd/*.psb file while holding Shift key"));
  SetWindowTextW(GetDlgItem(dialog, id_check_manual_shift_psd), buf);

  // Debug group
  ov_snprintf_wchar(buf, sizeof(buf) / sizeof(WCHAR), ph, ph, pgettext("config", "Debug"));
  SetWindowTextW(GetDlgItem(dialog, id_group_debug), buf);

  ov_snprintf_wchar(buf, sizeof(buf) / sizeof(WCHAR), ph, ph, pgettext("config", "Enable &debug mode"));
  SetWindowTextW(GetDlgItem(dialog, id_check_debug_mode), buf);

  // Load checkbox states from config
  {
    bool value = true;
    if (ptk_config_get_manual_shift_wav(data->config, &value, &err)) {
      SendMessageW(GetDlgItem(dialog, id_check_manual_shift_wav), BM_SETCHECK, value ? BST_CHECKED : BST_UNCHECKED, 0);
    } else {
      OV_ERROR_REPORT(&err, NULL);
    }

    value = true;
    if (ptk_config_get_manual_wav_txt_pair(data->config, &value, &err)) {
      SendMessageW(
          GetDlgItem(dialog, id_check_manual_wav_txt_pair), BM_SETCHECK, value ? BST_CHECKED : BST_UNCHECKED, 0);
    } else {
      OV_ERROR_REPORT(&err, NULL);
    }

    value = true;
    if (ptk_config_get_manual_object_audio_text(data->config, &value, &err)) {
      SendMessageW(
          GetDlgItem(dialog, id_check_manual_object_audio_text), BM_SETCHECK, value ? BST_CHECKED : BST_UNCHECKED, 0);
    } else {
      OV_ERROR_REPORT(&err, NULL);
    }

    value = true;
    if (ptk_config_get_external_wav_txt_pair(data->config, &value, &err)) {
      SendMessageW(
          GetDlgItem(dialog, id_check_external_wav_txt_pair), BM_SETCHECK, value ? BST_CHECKED : BST_UNCHECKED, 0);
    } else {
      OV_ERROR_REPORT(&err, NULL);
    }

    value = true;
    if (ptk_config_get_external_object_audio_text(data->config, &value, &err)) {
      SendMessageW(
          GetDlgItem(dialog, id_check_external_object_audio_text), BM_SETCHECK, value ? BST_CHECKED : BST_UNCHECKED, 0);
    } else {
      OV_ERROR_REPORT(&err, NULL);
    }

    value = false;
    if (ptk_config_get_manual_shift_psd(data->config, &value, &err)) {
      SendMessageW(GetDlgItem(dialog, id_check_manual_shift_psd), BM_SETCHECK, value ? BST_CHECKED : BST_UNCHECKED, 0);
    } else {
      OV_ERROR_REPORT(&err, NULL);
    }

    value = false;
    if (ptk_config_get_debug_mode(data->config, &value, &err)) {
      SendMessageW(GetDlgItem(dialog, id_check_debug_mode), BM_SETCHECK, value ? BST_CHECKED : BST_UNCHECKED, 0);
    } else {
      OV_ERROR_REPORT(&err, NULL);
    }
  }

  return TRUE;
}

static bool click_ok(HWND dialog, struct dialog_data *data) {
  WCHAR buf[256];
  WCHAR buf_caption[256];

  struct ov_error err = {0};
  bool result = false;

  {
    // Save manual_shift_wav
    LRESULT const checked = SendMessageW(GetDlgItem(dialog, id_check_manual_shift_wav), BM_GETCHECK, 0, 0);
    if (!ptk_config_set_manual_shift_wav(data->config, checked == BST_CHECKED, &err)) {
      OV_ERROR_ADD_TRACE(&err);
      goto cleanup;
    }
  }

  {
    // Save manual_wav_txt_pair
    LRESULT const checked = SendMessageW(GetDlgItem(dialog, id_check_manual_wav_txt_pair), BM_GETCHECK, 0, 0);
    if (!ptk_config_set_manual_wav_txt_pair(data->config, checked == BST_CHECKED, &err)) {
      OV_ERROR_ADD_TRACE(&err);
      goto cleanup;
    }
  }

  {
    // Save manual_object_audio_text
    LRESULT const checked = SendMessageW(GetDlgItem(dialog, id_check_manual_object_audio_text), BM_GETCHECK, 0, 0);
    if (!ptk_config_set_manual_object_audio_text(data->config, checked == BST_CHECKED, &err)) {
      OV_ERROR_ADD_TRACE(&err);
      goto cleanup;
    }
  }

  {
    // Save external_wav_txt_pair
    LRESULT const checked = SendMessageW(GetDlgItem(dialog, id_check_external_wav_txt_pair), BM_GETCHECK, 0, 0);
    if (!ptk_config_set_external_wav_txt_pair(data->config, checked == BST_CHECKED, &err)) {
      OV_ERROR_ADD_TRACE(&err);
      goto cleanup;
    }
  }

  {
    // Save external_object_audio_text
    LRESULT const checked = SendMessageW(GetDlgItem(dialog, id_check_external_object_audio_text), BM_GETCHECK, 0, 0);
    if (!ptk_config_set_external_object_audio_text(data->config, checked == BST_CHECKED, &err)) {
      OV_ERROR_ADD_TRACE(&err);
      goto cleanup;
    }
  }

  {
    // Save manual_shift_psd
    LRESULT const checked = SendMessageW(GetDlgItem(dialog, id_check_manual_shift_psd), BM_GETCHECK, 0, 0);
    if (!ptk_config_set_manual_shift_psd(data->config, checked == BST_CHECKED, &err)) {
      OV_ERROR_ADD_TRACE(&err);
      goto cleanup;
    }
  }

  {
    // Save debug_mode
    LRESULT const checked = SendMessageW(GetDlgItem(dialog, id_check_debug_mode), BM_GETCHECK, 0, 0);
    if (!ptk_config_set_debug_mode(data->config, checked == BST_CHECKED, &err)) {
      OV_ERROR_ADD_TRACE(&err);
      goto cleanup;
    }
  }

  result = true;

cleanup:
  if (!result) {
    ov_snprintf_wchar(buf, sizeof(buf) / sizeof(WCHAR), L"%s", L"%s", gettext("Failed to save settings."));
    ov_snprintf_wchar(buf_caption, sizeof(buf_caption) / sizeof(WCHAR), L"%s", L"%s", gettext("PSDToolKit"));
    MessageBoxW(dialog, buf, buf_caption, MB_OK | MB_ICONERROR);
  }
  OV_ERROR_REPORT(&err, NULL);
  return result;
}

static INT_PTR CALLBACK dialog_proc(HWND dialog, UINT message, WPARAM wParam, LPARAM lParam) {
  WCHAR buf[256];
  WCHAR buf_caption[256];

  struct dialog_data *data = (struct dialog_data *)GetPropW(dialog, g_config_dialog_prop_name);

  switch (message) {
  case WM_INITDIALOG:
    return init_dialog(dialog, (struct dialog_data *)lParam);

  case WM_COMMAND:
    switch (LOWORD(wParam)) {
    case IDOK:
      if (click_ok(dialog, data)) {
        struct ov_error err = {0};
        if (!ptk_config_save(data->config, &err)) {
          OV_ERROR_REPORT(&err, NULL);
          ov_snprintf_wchar(buf, sizeof(buf) / sizeof(WCHAR), L"%s", L"%s", gettext("Failed to save settings."));
          ov_snprintf_wchar(buf_caption, sizeof(buf_caption) / sizeof(WCHAR), L"%s", L"%s", gettext("PSDToolKit"));
          MessageBoxW(dialog, buf, buf_caption, MB_OK | MB_ICONERROR);
          return TRUE;
        }
        EndDialog(dialog, IDOK);
      }
      return TRUE;

    case IDCANCEL:
      EndDialog(dialog, IDCANCEL);
      return TRUE;
    }
    break;

  case WM_DESTROY:
    if (data) {
      // Cleanup font
      if (data->dialog_font) {
        DeleteObject(data->dialog_font);
        data->dialog_font = NULL;
      }

      RemovePropW(dialog, g_config_dialog_prop_name);
    }
    return TRUE;
  }

  return FALSE;
}

bool ptk_config_dialog_show(struct ptk_config *config, void *parent_window, struct ov_error *const err) {
  if (!config) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_invalid_argument);
    return false;
  }
  if (!err) {
    return false;
  }

  struct dialog_data data = {0};
  bool result = false;
  void *hinstance = NULL;
  HWND *disabled_windows = NULL;

  {
    data.config = config;

    if (!ovl_os_get_hinstance_from_fnptr((void *)ptk_config_dialog_show, &hinstance, err)) {
      OV_ERROR_ADD_TRACE(err);
      goto cleanup;
    }

    // Disable other windows in the process for modal behavior
    disabled_windows = ptk_win32_disable_family_windows((HWND)parent_window);

    INT_PTR dialog_result =
        DialogBoxParamW((HINSTANCE)hinstance, L"PTKCONFIGDIALOG", (HWND)parent_window, dialog_proc, (LPARAM)&data);
    if (dialog_result == -1) {
      OV_ERROR_SET_HRESULT(err, HRESULT_FROM_WIN32(GetLastError()));
      goto cleanup;
    }
  }

  result = true;

cleanup:
  ptk_win32_restore_disabled_family_windows(disabled_windows);
  return result;
}
