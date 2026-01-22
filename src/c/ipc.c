#include "ipc.h"

#include "logf.h"
#include "ovarray.h"
#include "ovthreads.h"

#include <windows.h>

#define FOURCC(c0, c1, c2, c3)                                                                                         \
  ((uint32_t)(((uint32_t)(uint8_t)(c0)) | (((uint32_t)(uint8_t)(c1)) << 8) | (((uint32_t)(uint8_t)(c2)) << 16) |       \
              (((uint32_t)(uint8_t)(c3)) << 24)))

struct ipc {
  HANDLE process;
  HANDLE h_stdin;
  HANDLE h_stdout;
  thrd_t thread;
  bool thread_created;
  mtx_t mtx_stdin;
  mtx_t mtx_reply;
  cnd_t cnd_reply;
  cnd_t cnd_reply_consumed;
  bool reply_received;
  bool reply_consumed;
  uint32_t reply_value;
  char *reply_error;

  struct ipc_options opt;
  bool exit_requested;
};

static bool write_all(HANDLE h, void const *const buf, size_t const len, struct ov_error *const err) {
  DWORD written = 0;
  if (!WriteFile(h, buf, (DWORD)len, &written, NULL)) {
    OV_ERROR_SET_HRESULT(err, HRESULT_FROM_WIN32(GetLastError()));
    return false;
  }
  if (written != len) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_fail);
    return false;
  }
  return true;
}

static bool read_all(HANDLE h, void *const buf, size_t const len, struct ov_error *const err) {
  DWORD read = 0;
  if (!ReadFile(h, buf, (DWORD)len, &read, NULL)) {
    OV_ERROR_SET_HRESULT(err, HRESULT_FROM_WIN32(GetLastError()));
    return false;
  }
  if (read != len) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_fail);
    return false;
  }
  return true;
}

static bool write_int32(HANDLE h, int32_t const v, struct ov_error *const err) {
  return write_all(h, &v, sizeof(v), err);
}

static bool write_uint32(HANDLE h, uint32_t const v, struct ov_error *const err) {
  return write_all(h, &v, sizeof(v), err);
}

static bool write_float32(HANDLE h, float const v, struct ov_error *const err) {
  return write_all(h, &v, sizeof(v), err);
}

static bool write_string(HANDLE h, char const *const s, struct ov_error *const err) {
  size_t const len = s ? strlen(s) : 0;
  if (!write_int32(h, (int32_t)len, err)) {
    return false;
  }
  if (len > 0) {
    if (!write_all(h, s, len, err)) {
      return false;
    }
  }
  return true;
}

static bool read_int32(HANDLE h, int32_t *const v, struct ov_error *const err) {
  return read_all(h, v, sizeof(*v), err);
}

static bool read_uint32(HANDLE h, uint32_t *const v, struct ov_error *const err) {
  return read_all(h, v, sizeof(*v), err);
}

static bool read_uint64(HANDLE h, uint64_t *const v, struct ov_error *const err) {
  return read_all(h, v, sizeof(*v), err);
}

static bool read_string(HANDLE h, char **const s, struct ov_error *const err) {
  int32_t len = 0;
  bool result = false;
  if (!read_int32(h, &len, err)) {
    goto cleanup;
  }
  if (len < 0) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_fail);
    goto cleanup;
  }
  if (!OV_ARRAY_GROW(s, (size_t)len + 1)) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_out_of_memory);
    goto cleanup;
  }
  if (len > 0) {
    if (!read_all(h, *s, (size_t)len, err)) {
      goto cleanup;
    }
  }
  (*s)[len] = '\0';
  OV_ARRAY_SET_LENGTH(*s, (size_t)len + 1);
  result = true;
cleanup:
  return result;
}

static void ipc_reply_consumed(struct ipc *const self) {
  mtx_lock(&self->mtx_reply);
  self->reply_consumed = true;
  cnd_signal(&self->cnd_reply_consumed);
  mtx_unlock(&self->mtx_reply);
}

static bool wait_for_reply(struct ipc *const self, uint32_t *const reply, struct ov_error *const err) {
  bool result = false;
  mtx_lock(&self->mtx_reply);
  while (!self->reply_received && !self->exit_requested) {
    cnd_wait(&self->cnd_reply, &self->mtx_reply);
  }
  if (self->exit_requested) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_abort);
    goto cleanup;
  }
  *reply = self->reply_value;
  self->reply_received = false;
  if (self->reply_error) {
    OV_ERROR_SET(err, ov_error_type_generic, ov_error_generic_fail, self->reply_error);
    OV_ARRAY_DESTROY(&self->reply_error);
    self->reply_error = NULL;
    goto cleanup;
  }
  result = true;
cleanup:
  mtx_unlock(&self->mtx_reply);
  return result;
}

static bool handle_request(struct ipc *const self, uint32_t cmd, struct ov_error *const err) {
  char *path = NULL;
  char *state = NULL;
  char *slider_name = NULL;
  char *names = NULL;
  char *values = NULL;
  bool result = false;

  if (cmd == FOURCC('E', 'D', 'I', 'S')) {
    if (!read_string(self->h_stdout, &path, err)) {
      OV_ERROR_ADD_TRACE(err);
      goto cleanup;
    }
    if (!read_string(self->h_stdout, &state, err)) {
      OV_ERROR_ADD_TRACE(err);
      goto cleanup;
    }
    if (self->opt.on_update_editing_image_state) {
      struct ipc_update_editing_image_state_params params = {
          .file_path_utf8 = path,
          .state_utf8 = state,
      };
      self->opt.on_update_editing_image_state(self->opt.userdata, &params);
    }
  } else if (cmd == FOURCC('E', 'X', 'F', 'S')) {
    int32_t selected_index = 0;
    if (!read_string(self->h_stdout, &path, err)) {
      OV_ERROR_ADD_TRACE(err);
      goto cleanup;
    }
    if (!read_string(self->h_stdout, &slider_name, err)) {
      OV_ERROR_ADD_TRACE(err);
      goto cleanup;
    }
    if (!read_string(self->h_stdout, &names, err)) {
      OV_ERROR_ADD_TRACE(err);
      goto cleanup;
    }
    if (!read_string(self->h_stdout, &values, err)) {
      OV_ERROR_ADD_TRACE(err);
      goto cleanup;
    }
    if (!read_int32(self->h_stdout, &selected_index, err)) {
      OV_ERROR_ADD_TRACE(err);
      goto cleanup;
    }
    if (self->opt.on_export_faview_slider) {
      struct ipc_export_faview_slider_params params = {
          .file_path_utf8 = path,
          .slider_name_utf8 = slider_name,
          .names_utf8 = names,
          .values_utf8 = values,
          .selected_index = selected_index,
      };
      self->opt.on_export_faview_slider(self->opt.userdata, &params);
    }
  } else if (cmd == FOURCC('E', 'X', 'L', 'N')) {
    int32_t selected_index = 0;
    if (!read_string(self->h_stdout, &path, err)) {
      OV_ERROR_ADD_TRACE(err);
      goto cleanup;
    }
    if (!read_string(self->h_stdout, &names, err)) {
      OV_ERROR_ADD_TRACE(err);
      goto cleanup;
    }
    if (!read_string(self->h_stdout, &values, err)) {
      OV_ERROR_ADD_TRACE(err);
      goto cleanup;
    }
    if (!read_int32(self->h_stdout, &selected_index, err)) {
      OV_ERROR_ADD_TRACE(err);
      goto cleanup;
    }
    if (self->opt.on_export_layer_names) {
      struct ipc_export_layer_names_params params = {
          .file_path_utf8 = path,
          .names_utf8 = names,
          .values_utf8 = values,
          .selected_index = selected_index,
      };
      self->opt.on_export_layer_names(self->opt.userdata, &params);
    }
  } else {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_fail);
    goto cleanup;
  }

  mtx_lock(&self->mtx_stdin);
  if (!write_uint32(self->h_stdin, 0x80000000, err)) {
    mtx_unlock(&self->mtx_stdin);
    OV_ERROR_ADD_TRACE(err);
    goto cleanup;
  }
  mtx_unlock(&self->mtx_stdin);
  result = true;

cleanup:
  if (path) {
    OV_ARRAY_DESTROY(&path);
  }
  if (state) {
    OV_ARRAY_DESTROY(&state);
  }
  if (slider_name) {
    OV_ARRAY_DESTROY(&slider_name);
  }
  if (names) {
    OV_ARRAY_DESTROY(&names);
  }
  if (values) {
    OV_ARRAY_DESTROY(&values);
  }
  return result;
}

static int read_thread(void *userdata) {
  struct ipc *self = (struct ipc *)userdata;
  struct ov_error err = {0};
  while (!self->exit_requested) {
    uint32_t cmd = 0;
    if (!read_uint32(self->h_stdout, &cmd, &err)) {
      break;
    }

    if (cmd & 0x80000000) {
      uint32_t len = cmd & 0x7fffffff;
      char *error_msg = NULL;
      if (len > 0) {
        if (!OV_ARRAY_GROW(&error_msg, (size_t)len + 1)) {
          break;
        }
        if (!read_all(self->h_stdout, error_msg, len, &err)) {
          if (error_msg) {
            OV_ARRAY_DESTROY(&error_msg);
          }
          break;
        }
        error_msg[len] = '\0';
        OV_ARRAY_SET_LENGTH(error_msg, (size_t)len + 1);
      }
      mtx_lock(&self->mtx_reply);
      self->reply_received = true;
      self->reply_value = cmd;
      self->reply_error = error_msg;
      self->reply_consumed = false;
      cnd_signal(&self->cnd_reply);
      while (!self->reply_consumed && !self->exit_requested) {
        cnd_wait(&self->cnd_reply_consumed, &self->mtx_reply);
      }
      mtx_unlock(&self->mtx_reply);
    } else {
      if (!handle_request(self, cmd, &err)) {
        break;
      }
    }
  }
  if (err.stack[0].info.type != ov_error_type_invalid && !self->exit_requested) {
    ptk_logf_error(&err, "read_thread", "error in read_thread");
  }
  self->exit_requested = true;
  mtx_lock(&self->mtx_reply);
  cnd_broadcast(&self->cnd_reply);
  mtx_unlock(&self->mtx_reply);
  if (err.stack[0].info.type != ov_error_type_invalid) {
    OV_ERROR_DESTROY(&err);
  }
  return 0;
}

static bool ipc_helo(struct ipc *const self, struct ov_error *const err) {
  uint32_t const cmd = FOURCC('H', 'E', 'L', 'O');
  uint32_t reply = 0;
  bool result = false;
  mtx_lock(&self->mtx_stdin);
  if (!write_uint32(self->h_stdin, cmd, err)) {
    mtx_unlock(&self->mtx_stdin);
    OV_ERROR_ADD_TRACE(err);
    goto cleanup;
  }
  mtx_unlock(&self->mtx_stdin);

  if (!wait_for_reply(self, &reply, err)) {
    OV_ERROR_ADD_TRACE(err);
    goto cleanup;
  }
  if (reply != 0x80000000) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_fail);
    goto cleanup;
  }
  result = true;
cleanup:
  ipc_reply_consumed(self);
  return result;
}

bool ipc_init(struct ipc **const ipc, struct ipc_options const *const opt, struct ov_error *const err) {
  if (!ipc || !opt || !opt->exe_path) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_invalid_argument);
    return false;
  }

  struct ipc *self = NULL;
  HANDLE h_stdin_r = INVALID_HANDLE_VALUE;
  HANDLE h_stdin_w = INVALID_HANDLE_VALUE;
  HANDLE h_stdout_r = INVALID_HANDLE_VALUE;
  HANDLE h_stdout_w = INVALID_HANDLE_VALUE;
  wchar_t *cmdline = NULL;
  bool result = false;

  if (!OV_REALLOC(&self, 1, sizeof(struct ipc))) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_out_of_memory);
    goto cleanup;
  }
  memset(self, 0, sizeof(struct ipc));
  self->process = INVALID_HANDLE_VALUE;
  self->h_stdin = INVALID_HANDLE_VALUE;
  self->h_stdout = INVALID_HANDLE_VALUE;
  self->opt = *opt;

  {
    SECURITY_ATTRIBUTES sa = {
        .nLength = sizeof(sa),
        .lpSecurityDescriptor = NULL,
        .bInheritHandle = TRUE,
    };

    if (!CreatePipe(&h_stdin_r, &h_stdin_w, &sa, 0)) {
      OV_ERROR_SET_HRESULT(err, HRESULT_FROM_WIN32(GetLastError()));
      goto cleanup;
    }
    if (!SetHandleInformation(h_stdin_w, HANDLE_FLAG_INHERIT, 0)) {
      OV_ERROR_SET_HRESULT(err, HRESULT_FROM_WIN32(GetLastError()));
      goto cleanup;
    }

    if (!CreatePipe(&h_stdout_r, &h_stdout_w, &sa, 0)) {
      OV_ERROR_SET_HRESULT(err, HRESULT_FROM_WIN32(GetLastError()));
      goto cleanup;
    }
    if (!SetHandleInformation(h_stdout_r, HANDLE_FLAG_INHERIT, 0)) {
      OV_ERROR_SET_HRESULT(err, HRESULT_FROM_WIN32(GetLastError()));
      goto cleanup;
    }

    STARTUPINFOW si = {
        .cb = sizeof(si),
        .dwFlags = STARTF_USESTDHANDLES,
        .hStdInput = h_stdin_r,
        .hStdOutput = h_stdout_w,
        .hStdError = GetStdHandle(STD_ERROR_HANDLE),
    };
    PROCESS_INFORMATION pi = {0};

    size_t const exe_path_len = wcslen(opt->exe_path);
    if (!OV_ARRAY_GROW(&cmdline, exe_path_len + 3)) {
      OV_ERROR_SET_GENERIC(err, ov_error_generic_out_of_memory);
      goto cleanup;
    }
    cmdline[0] = L'\"';
    wcscpy(cmdline + 1, opt->exe_path);
    cmdline[exe_path_len + 1] = L'\"';
    cmdline[exe_path_len + 2] = L'\0';
    OV_ARRAY_SET_LENGTH(cmdline, exe_path_len + 3);

    if (!CreateProcessW(opt->exe_path, cmdline, NULL, NULL, TRUE, CREATE_NO_WINDOW, NULL, opt->working_dir, &si, &pi)) {
      HRESULT hr = HRESULT_FROM_WIN32(GetLastError());
      if (hr == HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND) || hr == HRESULT_FROM_WIN32(ERROR_PATH_NOT_FOUND)) {
        OV_ERROR_SET(err, ov_error_type_generic, ptk_ipc_error_target_not_found, NULL);
        goto cleanup;
      }
      if (hr == HRESULT_FROM_WIN32(ERROR_ACCESS_DENIED)) {
        OV_ERROR_SET(err, ov_error_type_generic, ptk_ipc_error_target_access_denied, NULL);
        goto cleanup;
      }
      OV_ERROR_SET_HRESULT(err, hr);
      goto cleanup;
    }
    CloseHandle(pi.hThread);
    self->process = pi.hProcess;
    self->h_stdin = h_stdin_w;
    self->h_stdout = h_stdout_r;
    h_stdin_w = INVALID_HANDLE_VALUE;
    h_stdout_r = INVALID_HANDLE_VALUE;
  }

  CloseHandle(h_stdin_r);
  h_stdin_r = INVALID_HANDLE_VALUE;
  CloseHandle(h_stdout_w);
  h_stdout_w = INVALID_HANDLE_VALUE;

  mtx_init(&self->mtx_stdin, mtx_plain);
  mtx_init(&self->mtx_reply, mtx_plain);
  cnd_init(&self->cnd_reply);
  cnd_init(&self->cnd_reply_consumed);

  if (thrd_create(&self->thread, read_thread, self) != thrd_success) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_fail);
    goto cleanup;
  }
  self->thread_created = true;

  if (!ipc_helo(self, err)) {
    OV_ERROR_ADD_TRACE(err);
    goto cleanup;
  }

  *ipc = self;
  result = true;

cleanup:
  if (cmdline) {
    OV_ARRAY_DESTROY(&cmdline);
  }
  if (h_stdin_r != INVALID_HANDLE_VALUE) {
    CloseHandle(h_stdin_r);
  }
  if (h_stdin_w != INVALID_HANDLE_VALUE) {
    CloseHandle(h_stdin_w);
  }
  if (h_stdout_r != INVALID_HANDLE_VALUE) {
    CloseHandle(h_stdout_r);
  }
  if (h_stdout_w != INVALID_HANDLE_VALUE) {
    CloseHandle(h_stdout_w);
  }
  if (!result && self) {
    ipc_exit(&self);
  }
  return result;
}

void ipc_exit(struct ipc **const ipc) {
  if (!ipc || !*ipc) {
    return;
  }
  struct ipc *self = *ipc;
  self->exit_requested = true;
  if (self->h_stdin != INVALID_HANDLE_VALUE) {
    CloseHandle(self->h_stdin);
    self->h_stdin = INVALID_HANDLE_VALUE;
  }
  if (self->thread_created) {
    thrd_join(self->thread, NULL);
  }
  if (self->h_stdout != INVALID_HANDLE_VALUE) {
    CloseHandle(self->h_stdout);
  }
  if (self->process != INVALID_HANDLE_VALUE) {
    WaitForSingleObject(self->process, 5000);
    TerminateProcess(self->process, 0);
    CloseHandle(self->process);
  }
  mtx_destroy(&self->mtx_stdin);
  mtx_destroy(&self->mtx_reply);
  cnd_destroy(&self->cnd_reply);
  cnd_destroy(&self->cnd_reply_consumed);
  if (self->reply_error) {
    OV_ARRAY_DESTROY(&self->reply_error);
  }
  OV_FREE(ipc);
}

bool ipc_add_file(struct ipc *const self, char const *const path_utf8, uint32_t const tag, struct ov_error *const err) {
  uint32_t const cmd = FOURCC('A', 'D', 'D', 'F');
  uint32_t reply = 0;
  bool result = false;
  mtx_lock(&self->mtx_stdin);
  if (!write_uint32(self->h_stdin, cmd, err) || !write_string(self->h_stdin, path_utf8, err) ||
      !write_uint32(self->h_stdin, tag, err)) {
    mtx_unlock(&self->mtx_stdin);
    OV_ERROR_ADD_TRACE(err);
    goto cleanup;
  }
  mtx_unlock(&self->mtx_stdin);

  if (!wait_for_reply(self, &reply, err)) {
    OV_ERROR_ADD_TRACE(err);
    goto cleanup;
  }
  result = true;
cleanup:
  ipc_reply_consumed(self);
  return result;
}

bool ipc_update_current_project_path(struct ipc *const self, char const *const path_utf8, struct ov_error *const err) {
  uint32_t const cmd = FOURCC('U', 'P', 'D', 'P');
  uint32_t reply = 0;
  bool result = false;
  mtx_lock(&self->mtx_stdin);
  if (!write_uint32(self->h_stdin, cmd, err) || !write_string(self->h_stdin, path_utf8, err)) {
    mtx_unlock(&self->mtx_stdin);
    OV_ERROR_ADD_TRACE(err);
    goto cleanup;
  }
  mtx_unlock(&self->mtx_stdin);

  if (!wait_for_reply(self, &reply, err)) {
    OV_ERROR_ADD_TRACE(err);
    goto cleanup;
  }
  result = true;
cleanup:
  ipc_reply_consumed(self);
  return result;
}

bool ipc_clear_files(struct ipc *const self, struct ov_error *const err) {
  uint32_t const cmd = FOURCC('C', 'L', 'R', 'F');
  uint32_t reply = 0;
  bool result = false;
  mtx_lock(&self->mtx_stdin);
  if (!write_uint32(self->h_stdin, cmd, err)) {
    mtx_unlock(&self->mtx_stdin);
    OV_ERROR_ADD_TRACE(err);
    goto cleanup;
  }
  mtx_unlock(&self->mtx_stdin);

  if (!wait_for_reply(self, &reply, err)) {
    OV_ERROR_ADD_TRACE(err);
    goto cleanup;
  }
  result = true;
cleanup:
  ipc_reply_consumed(self);
  return result;
}

bool ipc_deserialize(struct ipc *const self, char const *const src_utf8, struct ov_error *const err) {
  uint32_t const cmd = FOURCC('D', 'S', 'L', 'Z');
  uint32_t reply = 0;
  int32_t success = 0;
  bool result = false;
  mtx_lock(&self->mtx_stdin);
  if (!write_uint32(self->h_stdin, cmd, err) || !write_string(self->h_stdin, src_utf8, err)) {
    mtx_unlock(&self->mtx_stdin);
    OV_ERROR_ADD_TRACE(err);
    goto cleanup;
  }
  mtx_unlock(&self->mtx_stdin);

  if (!wait_for_reply(self, &reply, err)) {
    OV_ERROR_ADD_TRACE(err);
    goto cleanup;
  }

  if (!read_int32(self->h_stdout, &success, err)) {
    OV_ERROR_ADD_TRACE(err);
    goto cleanup;
  }
  result = success != 0;
cleanup:
  ipc_reply_consumed(self);
  return result;
}

bool ipc_draw(struct ipc *const self,
              int32_t const id,
              char const *const path_utf8,
              void *const p,
              int32_t const width,
              int32_t const height,
              struct ov_error *const err) {
  uint32_t const cmd = FOURCC('D', 'R', 'A', 'W');
  uint32_t reply = 0;
  int32_t len = 0;
  bool result = false;

  LARGE_INTEGER freq, t0, t1, t2, t3;
  QueryPerformanceFrequency(&freq);
  QueryPerformanceCounter(&t0);

  mtx_lock(&self->mtx_stdin);
  if (!write_uint32(self->h_stdin, cmd, err) || !write_int32(self->h_stdin, id, err) ||
      !write_string(self->h_stdin, path_utf8, err) || !write_int32(self->h_stdin, width, err) ||
      !write_int32(self->h_stdin, height, err)) {
    mtx_unlock(&self->mtx_stdin);
    OV_ERROR_ADD_TRACE(err);
    goto cleanup;
  }
  mtx_unlock(&self->mtx_stdin);
  QueryPerformanceCounter(&t1);

  if (!wait_for_reply(self, &reply, err)) {
    OV_ERROR_ADD_TRACE(err);
    goto cleanup;
  }
  QueryPerformanceCounter(&t2);

  if (!read_int32(self->h_stdout, &len, err)) {
    OV_ERROR_ADD_TRACE(err);
    goto cleanup;
  }
  if (len > width * height * 4) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_fail);
    goto cleanup;
  }
  if (len > 0) {
    if (!read_all(self->h_stdout, p, (size_t)len, err)) {
      OV_ERROR_ADD_TRACE(err);
      goto cleanup;
    }
  }
  QueryPerformanceCounter(&t3);

  ptk_logf_info(NULL,
                NULL,
                "[ipc_draw] size=%dx%d send=%.2fms wait=%.2fms read=%.2fms (len=%d)",
                width,
                height,
                (double)(t1.QuadPart - t0.QuadPart) * 1000.0 / (double)freq.QuadPart,
                (double)(t2.QuadPart - t1.QuadPart) * 1000.0 / (double)freq.QuadPart,
                (double)(t3.QuadPart - t2.QuadPart) * 1000.0 / (double)freq.QuadPart,
                len);

  result = true;
cleanup:
  ipc_reply_consumed(self);
  return result;
}

bool ipc_get_layer_names(struct ipc *const self,
                         int32_t const id,
                         char const *const path_utf8,
                         char **const dest_utf8,
                         struct ov_error *const err) {
  uint32_t const cmd = FOURCC('L', 'N', 'A', 'M');
  uint32_t reply = 0;
  bool result = false;
  mtx_lock(&self->mtx_stdin);
  if (!write_uint32(self->h_stdin, cmd, err) || !write_int32(self->h_stdin, id, err) ||
      !write_string(self->h_stdin, path_utf8, err)) {
    mtx_unlock(&self->mtx_stdin);
    OV_ERROR_ADD_TRACE(err);
    goto cleanup;
  }
  mtx_unlock(&self->mtx_stdin);

  if (!wait_for_reply(self, &reply, err)) {
    OV_ERROR_ADD_TRACE(err);
    goto cleanup;
  }

  if (!read_string(self->h_stdout, dest_utf8, err)) {
    OV_ERROR_ADD_TRACE(err);
    goto cleanup;
  }
  result = true;
cleanup:
  ipc_reply_consumed(self);
  return result;
}

bool ipc_serialize(struct ipc *const self, char **const dest_utf8, struct ov_error *const err) {
  uint32_t const cmd = FOURCC('S', 'R', 'L', 'Z');
  uint32_t reply = 0;
  bool result = false;
  mtx_lock(&self->mtx_stdin);
  if (!write_uint32(self->h_stdin, cmd, err)) {
    mtx_unlock(&self->mtx_stdin);
    OV_ERROR_ADD_TRACE(err);
    goto cleanup;
  }
  mtx_unlock(&self->mtx_stdin);

  if (!wait_for_reply(self, &reply, err)) {
    OV_ERROR_ADD_TRACE(err);
    goto cleanup;
  }

  if (!read_string(self->h_stdout, dest_utf8, err)) {
    OV_ERROR_ADD_TRACE(err);
    goto cleanup;
  }
  result = true;
cleanup:
  ipc_reply_consumed(self);
  return result;
}

bool ipc_set_props(struct ipc *const self,
                   int32_t const id,
                   char const *const path_utf8,
                   struct ipc_prop_params const *const params,
                   struct ipc_prop_result *const result,
                   struct ov_error *const err) {
  uint32_t const cmd = FOURCC('P', 'R', 'O', 'P');
  uint32_t reply = 0;
  int32_t modified = 0;
  bool res = false;
  mtx_lock(&self->mtx_stdin);
  if (!write_uint32(self->h_stdin, cmd, err) || !write_int32(self->h_stdin, id, err) ||
      !write_string(self->h_stdin, path_utf8, err)) {
    mtx_unlock(&self->mtx_stdin);
    OV_ERROR_ADD_TRACE(err);
    goto cleanup;
  }

  if (params->layer) {
    if (!write_int32(self->h_stdin, 1, err) || !write_string(self->h_stdin, params->layer, err)) {
      mtx_unlock(&self->mtx_stdin);
      OV_ERROR_ADD_TRACE(err);
      goto cleanup;
    }
  }
  if (params->scale) {
    if (!write_int32(self->h_stdin, 2, err) || !write_float32(self->h_stdin, *params->scale, err)) {
      mtx_unlock(&self->mtx_stdin);
      OV_ERROR_ADD_TRACE(err);
      goto cleanup;
    }
  }
  if (params->offset_x) {
    if (!write_int32(self->h_stdin, 3, err) || !write_int32(self->h_stdin, *params->offset_x, err)) {
      mtx_unlock(&self->mtx_stdin);
      OV_ERROR_ADD_TRACE(err);
      goto cleanup;
    }
  }
  if (params->offset_y) {
    if (!write_int32(self->h_stdin, 4, err) || !write_int32(self->h_stdin, *params->offset_y, err)) {
      mtx_unlock(&self->mtx_stdin);
      OV_ERROR_ADD_TRACE(err);
      goto cleanup;
    }
  }
  if (params->tag) {
    if (!write_int32(self->h_stdin, 5, err) || !write_uint32(self->h_stdin, *params->tag, err)) {
      mtx_unlock(&self->mtx_stdin);
      OV_ERROR_ADD_TRACE(err);
      goto cleanup;
    }
  }
  if (params->quality) {
    if (!write_int32(self->h_stdin, 6, err) || !write_int32(self->h_stdin, *params->quality, err)) {
      mtx_unlock(&self->mtx_stdin);
      OV_ERROR_ADD_TRACE(err);
      goto cleanup;
    }
  }
  if (!write_int32(self->h_stdin, 0, err)) { // propEnd
    mtx_unlock(&self->mtx_stdin);
    OV_ERROR_ADD_TRACE(err);
    goto cleanup;
  }
  mtx_unlock(&self->mtx_stdin);

  if (!wait_for_reply(self, &reply, err)) {
    OV_ERROR_ADD_TRACE(err);
    goto cleanup;
  }

  if (!read_int32(self->h_stdout, &modified, err)) {
    OV_ERROR_ADD_TRACE(err);
    goto cleanup;
  }
  result->modified = modified != 0;
  if (!read_uint64(self->h_stdout, &result->ckey, err)) {
    OV_ERROR_ADD_TRACE(err);
    goto cleanup;
  }
  if (!read_uint32(self->h_stdout, (uint32_t *)&result->width, err)) {
    OV_ERROR_ADD_TRACE(err);
    goto cleanup;
  }
  if (!read_uint32(self->h_stdout, (uint32_t *)&result->height, err)) {
    OV_ERROR_ADD_TRACE(err);
    goto cleanup;
  }
  res = true;
cleanup:
  ipc_reply_consumed(self);
  return res;
}

HWND ipc_get_window_handle(struct ipc *const self, struct ov_error *const err) {
  uint32_t const cmd = FOURCC('G', 'W', 'N', 'D');
  uint32_t reply = 0;
  uint64_t h = 0;
  bool success = false;
  mtx_lock(&self->mtx_stdin);
  if (!write_uint32(self->h_stdin, cmd, err)) {
    mtx_unlock(&self->mtx_stdin);
    OV_ERROR_ADD_TRACE(err);
    goto cleanup;
  }
  mtx_unlock(&self->mtx_stdin);

  if (!wait_for_reply(self, &reply, err)) {
    OV_ERROR_ADD_TRACE(err);
    goto cleanup;
  }

  if (!read_uint64(self->h_stdout, &h, err)) {
    OV_ERROR_ADD_TRACE(err);
    goto cleanup;
  }
  success = true;
cleanup:
  ipc_reply_consumed(self);
  return success ? (HWND)(uintptr_t)h : NULL;
}
