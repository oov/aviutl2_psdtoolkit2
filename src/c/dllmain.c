// Plugin startup sequence:
// The exported functions are called by AviUtl ExEdit2 in the following order:
// 1. DllMain(DLL_PROCESS_ATTACH) - Standard Windows DLL entry point
// 2. InitializeLogger - Called to set up logging functionality
// 3. InitializePlugin - Called to initialize the plugin with AviUtl ExEdit2 version info
// 4. RegisterPlugin - Called to register callbacks and handlers with AviUtl ExEdit2

#include <ovarray.h>
#include <ovbase.h>
#include <ovmo.h>
#include <ovprintf.h>
#include <ovutf.h>

#include <string.h>

#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <mmsystem.h>

#include <ovl/os.h>

#include <aviutl2_logger2.h>
#include <aviutl2_module2.h>
#include <aviutl2_plugin2.h>

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wmissing-declarations"
#include <aviutl2_input2.h>
#pragma clang diagnostic pop

#include <ovl/path.h>

#include "anm2_script_picker.h"
#include "anm2editor.h"
#include "cache.h"
#include "error.h"
#include "input.h"
#include "layer.h"
#include "logf.h"
#include "psdtoolkit.h"
#include "script_module.h"
#include "version.h"

extern unsigned char PSDToolKitHandler_lua[];
extern unsigned int PSDToolKitHandler_lua_len;

#include "PSDToolKitHandler_lua.h"

static struct psdtoolkit *g_psdtoolkit = NULL;
static struct ptk_anm2editor *g_anm2editor = NULL;
static struct ptk_script_module *g_script_module = NULL;
static struct ptk_cache *g_cache = NULL;
static struct ptk_input *g_input = NULL;
static struct mo *g_mo = NULL;
static HMODULE g_gcmzdrops = NULL;
static HHOOK g_msg_hook = NULL;
static HWND g_plugin_window = NULL;
static HWND g_anm2editor_window = NULL;
static int g_cache_index = 0;

/**
 * @brief Increment cache index and clear the image cache.
 *
 * This function is called when the project is loaded or cache is cleared
 * to invalidate all cached data and notify Lua side.
 */
static void update_cache_index(void) {
  ++g_cache_index;
  if (g_cache) {
    ptk_cache_clear(g_cache);
  }
}

/**
 * @brief Find aviutl2Manager window
 *
 * @return found window handle on success, desktop window handle on failure
 */
static HWND find_manager_window(void) {
  static wchar_t const class_name[] = L"aviutl2Manager";
  DWORD const pid = GetCurrentProcessId();
  HWND h = NULL;
  DWORD wpid;
  while ((h = FindWindowExW(NULL, h, class_name, NULL)) != NULL) {
    GetWindowThreadProcessId(h, &wpid);
    if (wpid != pid) {
      continue;
    }
    return h;
  }
  return GetDesktopWindow();
}

/**
 * @brief Hook procedure to intercept keyboard messages for the PSDToolKit window
 *
 * This hook captures keyboard input (WM_KEYDOWN, WM_KEYUP, WM_SYSKEYDOWN, WM_SYSKEYUP, WM_CHAR)
 * when the mouse cursor is over the PSDToolKit window, and forwards them to the window.
 * This allows the PSDToolKit window to receive keyboard shortcuts even when it doesn't have focus.
 *
 * The hook performs several checks before forwarding:
 * - Verifies the cursor is within the plugin window bounds
 * - Ensures no other window is obscuring the plugin window
 * - Skips forwarding when the top-level window is disabled (e.g. modal dialog is open)
 *
 * @param code Hook code from Windows
 * @param wparam Additional message information
 * @param lparam Pointer to MSG structure
 * @return Result from CallNextHookEx
 */
static LRESULT CALLBACK get_msg_hook_proc(int code, WPARAM wparam, LPARAM lparam) {
  static HWND psdtoolkit_window = NULL;

  if (code < 0 || !g_plugin_window) {
    goto cleanup;
  }
  {
    MSG *msg = (MSG *)lparam;
    if (msg->message != WM_KEYDOWN && msg->message != WM_KEYUP && msg->message != WM_SYSKEYDOWN &&
        msg->message != WM_SYSKEYUP && msg->message != WM_CHAR) {
      goto cleanup;
    }
    POINT pt;
    RECT rc;
    GetCursorPos(&pt);
    if (!GetWindowRect(g_plugin_window, &rc) || !PtInRect(&rc, pt)) {
      goto cleanup;
    }
    // Check if the cursor is actually over our window (not obscured by another window)
    HWND under_cursor = WindowFromPoint(pt);
    HWND toplevel = GetAncestor(g_plugin_window, GA_ROOT);
    // Skip if top-level window is disabled (e.g. a modal dialog is open)
    if (!IsWindowEnabled(toplevel)) {
      goto cleanup;
    }
    HWND under_cursor_toplevel = GetAncestor(under_cursor, GA_ROOT);
    if (under_cursor_toplevel != toplevel) {
      goto cleanup;
    }
    if (!psdtoolkit_window) {
      psdtoolkit_window = GetWindow(g_plugin_window, GW_CHILD);
    }
    if (!psdtoolkit_window) {
      goto cleanup;
    }
    // Find the deepest visible child window at the cursor position
    POINT client_pt = pt;
    ScreenToClient(toplevel, &client_pt);
    HWND at_point = toplevel;
    HWND child;
    while ((child = RealChildWindowFromPoint(at_point, client_pt)) != NULL && child != at_point) {
      MapWindowPoints(at_point, child, &client_pt, 1);
      at_point = child;
    }
    if (at_point != psdtoolkit_window && !IsChild(psdtoolkit_window, at_point)) {
      goto cleanup;
    }
    // Forward the message to the PSDToolKit window
    PostMessageW(psdtoolkit_window, msg->message, msg->wParam, msg->lParam);
    msg->message = WM_NULL;
  }

cleanup:
  return CallNextHookEx(g_msg_hook, code, wparam, lparam);
}

static aviutl2_input_handle input_ptkcache_open(wchar_t const *file) { return ptk_input_open(g_input, file); }
static bool input_ptkcache_close(aviutl2_input_handle ih) { return ptk_input_close(g_input, ih); }
static bool input_ptkcache_info_get(aviutl2_input_handle ih, struct aviutl2_input_info *iip) {
  return ptk_input_info_get(g_input, ih, iip);
}
static int input_ptkcache_read_video(aviutl2_input_handle ih, int frame, void *buf) {
  return ptk_input_read_video(g_input, ih, frame, buf);
}

void __declspec(dllexport) InitializeLogger(struct aviutl2_log_handle *logger);
void __declspec(dllexport) InitializeLogger(struct aviutl2_log_handle *logger) { ptk_logf_set_handle(logger); }

BOOL __declspec(dllexport) InitializePlugin(DWORD version);
BOOL __declspec(dllexport) InitializePlugin(DWORD version) {
  struct ov_error err = {0};
  void *dll_hinst = NULL;
  bool success = false;

  // Check minimum required AviUtl ExEdit2 version
  if (version < 2002800) {
    OV_ERROR_SETF(&err,
                  ov_error_type_generic,
                  ov_error_generic_fail,
                  "%1$s",
                  gettext("PSDToolKit requires AviUtl ExEdit2 %1$s or later."),
                  "version2.0beta28");
    OV_ERROR_ADD_TRACE(&err);
    goto cleanup;
  }

  if (!ovl_os_get_hinstance_from_fnptr((void *)InitializePlugin, &dll_hinst, &err)) {
    OV_ERROR_ADD_TRACE(&err);
    goto cleanup;
  }

  if (!g_mo) {
    g_mo = mo_parse_from_resource((HINSTANCE)dll_hinst, &err);
    if (g_mo) {
      mo_set_default(g_mo);
    } else {
      ptk_logf_warn(NULL, "%s", "%s", gettext("failed to load language resources, continuing without them."));
      OV_ERROR_DESTROY(&err);
    }
  }

  g_cache = ptk_cache_create(&err);
  if (!g_cache) {
    OV_ERROR_ADD_TRACE(&err);
    goto cleanup;
  }
  g_psdtoolkit = psdtoolkit_create(g_cache, &err);
  if (!g_psdtoolkit) {
    OV_ERROR_ADD_TRACE(&err);
    goto cleanup;
  }
  g_script_module = psdtoolkit_get_script_module(g_psdtoolkit);
  g_input = ptk_input_create(g_cache, &err);
  if (!g_input) {
    OV_ERROR_ADD_TRACE(&err);
    goto cleanup;
  }

  success = true;

cleanup:
  if (!success) {
    if (g_input) {
      ptk_input_destroy(&g_input);
    }
    if (g_psdtoolkit) {
      psdtoolkit_destroy(&g_psdtoolkit);
    }
    if (g_cache) {
      ptk_cache_destroy(&g_cache);
    }
    if (g_mo) {
      mo_set_default(NULL);
      mo_free(&g_mo);
    }
    wchar_t main_instruction[256];
    ov_snprintf_wchar(main_instruction,
                      sizeof(main_instruction) / sizeof(main_instruction[0]),
                      L"%1$hs",
                      L"%1$hs",
                      gettext("failed to initialize plugin."));
    ptk_error_dialog(
        find_manager_window(), &err, L"PSDToolKit", main_instruction, NULL, TD_ERROR_ICON, TDCBF_OK_BUTTON);
    OV_ERROR_DESTROY(&err);
    return FALSE;
  }
  return TRUE;
}

void __declspec(dllexport) UninitializePlugin(void);
void __declspec(dllexport) UninitializePlugin(void) {
  if (g_msg_hook) {
    UnhookWindowsHookEx(g_msg_hook);
    g_msg_hook = NULL;
  }
  g_plugin_window = NULL;
  g_anm2editor_window = NULL;
  if (g_anm2editor) {
    ptk_anm2editor_destroy(&g_anm2editor);
  }
  if (g_input) {
    ptk_input_destroy(&g_input);
  }
  if (g_psdtoolkit) {
    psdtoolkit_destroy(&g_psdtoolkit);
  }
  if (g_cache) {
    ptk_cache_destroy(&g_cache);
  }
  if (g_gcmzdrops) {
    FreeLibrary(g_gcmzdrops);
    g_gcmzdrops = NULL;
  }
  if (g_mo) {
    mo_set_default(NULL);
    mo_free(&g_mo);
  }
}

static void project_load_handler(struct aviutl2_project_file *const project) {
  update_cache_index();
  psdtoolkit_project_load_handler(g_psdtoolkit, project);
}

static void project_save_handler(struct aviutl2_project_file *const project) {
  psdtoolkit_project_save_handler(g_psdtoolkit, project);
}

static void clear_cache_handler(struct aviutl2_edit_section *const edit) {
  (void)edit;
  update_cache_index();
}

static void config_menu_handler(HWND const hwnd, HINSTANCE const dll_hinst) {
  (void)dll_hinst;
  psdtoolkit_show_config_dialog(g_psdtoolkit, hwnd);
}

static void script_module_get_debug_mode(struct aviutl2_script_module_param *param) {
  ptk_script_module_get_debug_mode(g_script_module, param, g_cache_index);
}
static void script_module_generate_tag(struct aviutl2_script_module_param *param) {
  ptk_script_module_generate_tag(g_script_module, param);
}
static void script_module_add_psd_file(struct aviutl2_script_module_param *param) {
  ptk_script_module_add_psd_file(g_script_module, param);
}
static void script_module_set_props(struct aviutl2_script_module_param *param) {
  ptk_script_module_set_props(g_script_module, param);
}
static void script_module_get_drop_config(struct aviutl2_script_module_param *param) {
  ptk_script_module_get_drop_config(g_script_module, param);
}
static void script_module_draw(struct aviutl2_script_module_param *param) {
  ptk_script_module_draw(g_script_module, param);
}
static void script_module_get_preferred_languages(struct aviutl2_script_module_param *param) {
  ptk_script_module_get_preferred_languages(g_script_module, param);
}
static void script_module_read_text_file(struct aviutl2_script_module_param *param) {
  ptk_script_module_read_text_file(g_script_module, param);
}

static bool load_gcmzdrops(struct aviutl2_script_module_table *const script_module_table, struct ov_error *const err) {
  wchar_t *path = NULL;
  void *dll_hinst = NULL;
  bool success = false;

  if (!ovl_os_get_hinstance_from_fnptr((void *)load_gcmzdrops, &dll_hinst, NULL)) {
    OV_ERROR_SET_HRESULT(err, HRESULT_FROM_WIN32(GetLastError()));
    goto cleanup;
  }

  if (!ovl_path_get_module_name(&path, (HINSTANCE)dll_hinst, err)) {
    OV_ERROR_ADD_TRACE(err);
    goto cleanup;
  }

  {
    wchar_t *sep = ovl_path_find_last_path_sep(path);
    if (sep) {
      *(sep + 1) = L'\0';
      OV_ARRAY_SET_LENGTH(path, (size_t)(sep - path + 1));
    } else {
      path[0] = L'\0';
      OV_ARRAY_SET_LENGTH(path, 0);
    }

    static wchar_t const name[] = L"GCMZDrops.aux2";
    size_t const name_len = sizeof(name) / sizeof(name[0]) - 1;
    size_t const current_len = OV_ARRAY_LENGTH(path);
    if (!OV_ARRAY_GROW(&path, current_len + name_len + 1)) {
      OV_ERROR_SET_GENERIC(err, ov_error_generic_out_of_memory);
      goto cleanup;
    }
    memcpy(path + current_len, name, (name_len + 1) * sizeof(wchar_t));
    OV_ARRAY_SET_LENGTH(path, current_len + name_len);
  }

  g_gcmzdrops = LoadLibraryW(path);
  if (!g_gcmzdrops) {
    OV_ERROR_SETF(err,
                  ov_error_type_hresult,
                  HRESULT_FROM_WIN32(GetLastError()),
                  "%1$hs",
                  "%1$hs",
                  gettext("failed to load GCMZDrops.aux2 plug-in."));
    goto cleanup;
  }

  {
    static uint32_t const target_version = 67108876;
    static char const target_version_str[] = "v2.0.0alpha12";

    typedef uint32_t (*get_version_func)(void);
    typedef bool (*register_script_module_func)(struct aviutl2_script_module_table *const table,
                                                char const *const module_name);
    typedef bool (*add_handler_script_func)(char const *const script, size_t const script_len);

    get_version_func gcmz_get_version = (get_version_func)(void *)(GetProcAddress(g_gcmzdrops, "GetVersion"));
    register_script_module_func gcmz_register_script_module =
        (register_script_module_func)(void *)(GetProcAddress(g_gcmzdrops, "RegisterScriptModule"));
    add_handler_script_func gcmz_add_handler_script =
        (add_handler_script_func)(void *)(GetProcAddress(g_gcmzdrops, "AddHandlerScript"));

    // Verify all functions are available
    if (!gcmz_get_version || !gcmz_register_script_module || !gcmz_add_handler_script) {
      OV_ERROR_SETF(err,
                    ov_error_type_hresult,
                    HRESULT_FROM_WIN32(ERROR_PROC_NOT_FOUND),
                    "%1$hs",
                    "%1$hs",
                    gettext("failed to get required functions from GCMZDrops.aux2 plug-in."));
      goto cleanup;
    }

    if (gcmz_get_version() < target_version) {
      OV_ERROR_SETF(err,
                    ov_error_type_generic,
                    ov_error_generic_fail,
                    "%1$hs",
                    gettext("GCMZDrops version is too old. PSDToolKit requires GCMZDrops %1$hs or later."),
                    target_version_str);
      goto cleanup;
    }

    if (!gcmz_register_script_module(script_module_table, "PSDToolKit")) {
      OV_ERROR_SET_GENERIC(err, ov_error_generic_fail);
      goto cleanup;
    }
    if (!gcmz_add_handler_script((char const *)PSDToolKitHandler_lua, PSDToolKitHandler_lua_len)) {
      OV_ERROR_SET_GENERIC(err, ov_error_generic_fail);
      goto cleanup;
    }
  }

  success = true;

cleanup:
  if (path) {
    OV_ARRAY_DESTROY(&path);
  }
  if (!success) {
    if (g_gcmzdrops) {
      FreeLibrary(g_gcmzdrops);
      g_gcmzdrops = NULL;
    }
  }
  return success;
}

void __declspec(dllexport) RegisterPlugin(struct aviutl2_host_app_table *host);
void __declspec(dllexport) RegisterPlugin(struct aviutl2_host_app_table *host) {
  struct ov_error err = {0};
  bool success = false;

  // Set plugin information
  static wchar_t information[64];
  ov_snprintf_wchar(
      information, sizeof(information) / sizeof(information[0]), L"%1$hs", L"PSDToolKit %1$s by oov", PTK_VERSION);
  host->set_plugin_information(information);

  // Register handlers
  host->register_project_load_handler(project_load_handler);
  host->register_project_save_handler(project_save_handler);
  host->register_clear_cache_handler(clear_cache_handler);

  // Register config menu
  static wchar_t config_menu_name[64];
  ov_snprintf_wchar(config_menu_name,
                    sizeof(config_menu_name) / sizeof(config_menu_name[0]),
                    L"%s",
                    L"%s",
                    gettext("PSDToolKit Settings..."));
  host->register_config_menu(config_menu_name, config_menu_handler);

  struct aviutl2_edit_handle *const edit_handle = host->create_edit_handle();
  psdtoolkit_set_edit_handle(g_psdtoolkit, edit_handle);

  static struct aviutl2_script_module_function script_module_functions[] = {
      {L"get_debug_mode", script_module_get_debug_mode},
      {L"get_drop_config", script_module_get_drop_config},
      {L"get_preferred_languages", script_module_get_preferred_languages},
      {L"generate_tag", script_module_generate_tag},
      {L"add_psd_file", script_module_add_psd_file},
      {L"set_props", script_module_set_props},
      {L"draw", script_module_draw},
      {L"read_text_file", script_module_read_text_file},
      {NULL, NULL},
  };
  static struct aviutl2_script_module_table script_module_table = {
      L"PSDToolKit",
      script_module_functions,
  };
  host->register_script_module(&script_module_table);

  if (!load_gcmzdrops(&script_module_table, &err)) {
    OV_ERROR_ADD_TRACE(&err);
    goto cleanup;
  }

  static struct aviutl2_input_plugin_table input_plugin_table = {
      .flag = aviutl2_input_plugin_table_flag_video,
      .name = L"PSDToolKit Cache Input",
      .filefilter = L"PSDToolKit Cache (*.ptkcache)\0*.ptkcache\0",
      .information = L"PSDToolKit Cache Input Plugin",
      .func_open = input_ptkcache_open,
      .func_close = input_ptkcache_close,
      .func_info_get = input_ptkcache_info_get,
      .func_read_video = input_ptkcache_read_video,
      .func_read_audio = NULL,
      .func_config = NULL,
      .func_set_track = NULL,
      .func_time_to_frame = NULL,
  };
  host->register_input_plugin(&input_plugin_table);

  static wchar_t plugin_window_title[64];
  ov_snprintf_wchar(plugin_window_title,
                    sizeof(plugin_window_title) / sizeof(plugin_window_title[0]),
                    L"%s",
                    L"%s",
                    gettext("PSDToolKit"));
  g_plugin_window = (HWND)psdtoolkit_create_plugin_window(g_psdtoolkit, plugin_window_title, &err);
  if (!g_plugin_window) {
    OV_ERROR_ADD_TRACE(&err);
    goto cleanup;
  }
  host->register_window_client(plugin_window_title, g_plugin_window);

  // Create and register PSDToolKit anm2 Editor window
  static wchar_t anm2editor_window_title[64];
  ov_snprintf_wchar(anm2editor_window_title,
                    sizeof(anm2editor_window_title) / sizeof(anm2editor_window_title[0]),
                    L"%s",
                    L"%s",
                    gettext("PSDToolKit anm2 Editor"));
  if (!ptk_anm2editor_create(
          &g_anm2editor, anm2editor_window_title, edit_handle, (void **)&g_anm2editor_window, &err)) {
    OV_ERROR_ADD_TRACE(&err);
    goto cleanup;
  }
  host->register_window_client(anm2editor_window_title, g_anm2editor_window);
  psdtoolkit_set_anm2editor(g_psdtoolkit, g_anm2editor);

  g_msg_hook = SetWindowsHookExW(WH_GETMESSAGE, get_msg_hook_proc, NULL, GetCurrentThreadId());

  success = true;

cleanup:
  if (!success) {
    wchar_t main_instruction[256];
    ov_snprintf_wchar(main_instruction,
                      sizeof(main_instruction) / sizeof(main_instruction[0]),
                      L"%1$hs",
                      L"%1$hs",
                      gettext("failed to register plugin."));
    ptk_error_dialog(
        find_manager_window(), &err, L"PSDToolKit", main_instruction, NULL, TD_ERROR_ICON, TDCBF_OK_BUTTON);
    OV_ERROR_DESTROY(&err);
  }
}

static void error_output_hook(enum ov_error_severity severity, char const *str) {
  (void)severity;
  if (!str) {
    return;
  }
  wchar_t buf[1024];
  size_t const str_len = strlen(str);
  size_t pos = 0;
  while (pos < str_len) {
    size_t const remaining = str_len - pos;
    size_t bytes_read = 0;
    size_t const converted = ov_utf8_to_wchar(str + pos, remaining, buf, sizeof(buf) / sizeof(buf[0]) - 1, &bytes_read);
    if (converted == 0 || bytes_read == 0) {
      pos++;
      continue;
    }
    buf[converted] = L'\0';
    OutputDebugStringW(buf);
    pos += bytes_read;
  }
}

BOOL WINAPI DllMain(HINSTANCE const inst, DWORD const reason, LPVOID const reserved);
BOOL WINAPI DllMain(HINSTANCE const inst, DWORD const reason, LPVOID const reserved) {
  // trans: This dagger helps UTF-8 detection. You don't need to translate this.
  (void)gettext_noop("†");
  (void)reserved;
  switch (reason) {
  case DLL_PROCESS_ATTACH:
    DisableThreadLibraryCalls(inst);
    ov_init();
    ov_error_set_output_hook(error_output_hook);
    return TRUE;
  case DLL_PROCESS_DETACH:
    ov_exit();
    return TRUE;
  }
  return TRUE;
}
