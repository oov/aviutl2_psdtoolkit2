#include "error.h"
#include "logf.h"
#include "script_module.h"

#include <ovtest.h>

#include <aviutl2_module2.h>

#include <stdarg.h>
#include <string.h>

#ifndef SOURCE_DIR
#  define SOURCE_DIR .
#endif

#define STR(x) #x
#define STRINGIZE(x) STR(x)
#define TEST_PATH(relative_path) STRINGIZE(SOURCE_DIR) "/test_data/" relative_path

// Stub for logging functions
void ptk_logf_error(struct ov_error const *const err, char const *const reference, char const *const format, ...) {
  (void)err;
  (void)reference;
  (void)format;
}

// Stub for error message function
bool ptk_error_get_main_message(struct ov_error *const err, wchar_t **const dest) {
  (void)err;
  (void)dest;
  return false;
}

struct mock_context {
  bool pushed_boolean_values[8];
  int pushed_boolean_count;
  bool callback_value;
  int resize_quality_value;
  int pushed_int;
  char const *pushed_string;
  int pushed_int_values[8];
  int pushed_int_count;

  // For push_result_table_int
  char const *pushed_table_keys[8];
  int pushed_table_values[8];
  int pushed_table_num;

  // For param getters
  char const *param_strings[8];
  int param_ints[8];
  double param_table_doubles[8];
  int param_table_ints[8];
  char const *param_table_strings[8];

  // For add_psd_file test
  char const *received_path;
  uint32_t received_tag;
  bool add_file_called;
  bool add_file_should_succeed;

  // For set_props test
  bool set_props_called;
  bool set_props_should_succeed;
  int set_props_received_id;
  char const *set_props_received_path;
  char const *set_props_received_layer;
  double set_props_received_scale;
  int set_props_received_offset_x;
  int set_props_received_offset_y;
  int set_props_received_tag;
  struct ptk_script_module_set_props_result set_props_result;

  // For get_drop_config test
  bool get_drop_config_called;
  bool get_drop_config_should_succeed;
  struct ptk_script_module_drop_config drop_config_result;

  // For draw test
  bool draw_called;
  bool draw_should_succeed;
  int draw_received_id;
  char const *draw_received_path;
  int32_t draw_received_width;
  int32_t draw_received_height;
  uint64_t draw_received_ckey;

  // For get_preferred_languages test
  char const *pushed_array_strings[8];
  int pushed_array_string_count;
};

static struct mock_context *g_ctx = NULL;

static void mock_push_result_boolean(bool value) {
  if (g_ctx->pushed_boolean_count < 8) {
    g_ctx->pushed_boolean_values[g_ctx->pushed_boolean_count++] = value;
  }
}

static void mock_push_result_int(int value) {
  g_ctx->pushed_int = value;
  if (g_ctx->pushed_int_count < 8) {
    g_ctx->pushed_int_values[g_ctx->pushed_int_count++] = value;
  }
}

static void mock_push_result_string(char const *value) { g_ctx->pushed_string = value; }

static void mock_push_result_table_int(char const **keys, int *values, int num) {
  g_ctx->pushed_table_num = num;
  for (int i = 0; i < num && i < 8; ++i) {
    g_ctx->pushed_table_keys[i] = keys[i];
    g_ctx->pushed_table_values[i] = values[i];
  }
}

static void mock_push_result_array_string(char const **values, int num) {
  g_ctx->pushed_array_string_count = num;
  for (int i = 0; i < num && i < 8; ++i) {
    g_ctx->pushed_array_strings[i] = values[i];
  }
}

static char const *mock_get_param_string(int index) { return g_ctx->param_strings[index]; }

static int mock_get_param_int(int index) { return g_ctx->param_ints[index]; }

static double mock_get_param_table_double(int index, char const *key) {
  (void)index;
  (void)key;
  return g_ctx->param_table_doubles[0];
}

static int mock_get_param_table_int(int index, char const *key) {
  (void)index;
  if (strcmp(key, "offsetx") == 0) {
    return g_ctx->param_table_ints[0];
  } else if (strcmp(key, "offsety") == 0) {
    return g_ctx->param_table_ints[1];
  } else if (strcmp(key, "tag") == 0) {
    return g_ctx->param_table_ints[2];
  }
  return 0;
}

static char const *mock_get_param_table_string(int index, char const *key) {
  (void)index;
  (void)key;
  return g_ctx->param_table_strings[0];
}

static bool
mock_get_render_config_callback(void *userdata, bool *debug_mode, int *resize_quality, struct ov_error *err) {
  (void)userdata;
  (void)err;
  *debug_mode = g_ctx->callback_value;
  *resize_quality = g_ctx->resize_quality_value;
  return true;
}

static bool mock_add_file_callback(void *userdata, char const *path_utf8, uint32_t tag, struct ov_error *err) {
  (void)userdata;
  g_ctx->add_file_called = true;
  g_ctx->received_path = path_utf8;
  g_ctx->received_tag = tag;
  if (!g_ctx->add_file_should_succeed) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_fail);
    return false;
  }
  return true;
}

static bool mock_set_props_callback(void *userdata,
                                    struct ptk_script_module_set_props_params const *params,
                                    struct ptk_script_module_set_props_result *result,
                                    struct ov_error *err) {
  (void)userdata;
  g_ctx->set_props_called = true;
  g_ctx->set_props_received_id = params->id;
  g_ctx->set_props_received_path = params->path_utf8;
  g_ctx->set_props_received_layer = params->layer;
  g_ctx->set_props_received_scale = params->scale;
  g_ctx->set_props_received_offset_x = params->offset_x;
  g_ctx->set_props_received_offset_y = params->offset_y;
  g_ctx->set_props_received_tag = params->tag;
  if (!g_ctx->set_props_should_succeed) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_fail);
    return false;
  }
  *result = g_ctx->set_props_result;
  return true;
}

static bool
mock_get_drop_config_callback(void *userdata, struct ptk_script_module_drop_config *config, struct ov_error *err) {
  (void)userdata;
  g_ctx->get_drop_config_called = true;
  if (!g_ctx->get_drop_config_should_succeed) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_fail);
    return false;
  }
  *config = g_ctx->drop_config_result;
  return true;
}

static bool mock_draw_callback(
    void *userdata, int id, char const *path_utf8, int32_t width, int32_t height, uint64_t ckey, struct ov_error *err) {
  (void)userdata;
  g_ctx->draw_called = true;
  g_ctx->draw_received_id = id;
  g_ctx->draw_received_path = path_utf8;
  g_ctx->draw_received_width = width;
  g_ctx->draw_received_height = height;
  g_ctx->draw_received_ckey = ckey;
  if (!g_ctx->draw_should_succeed) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_fail);
    return false;
  }
  return true;
}

static void test_script_module_get_render_config(void) {
  struct mock_context ctx = {0};
  g_ctx = &ctx;

  struct ov_error err = {0};
  struct ptk_script_module_callbacks callbacks = {.get_render_config = mock_get_render_config_callback};
  struct ptk_script_module *sm = ptk_script_module_create(&callbacks, &err);
  if (!TEST_SUCCEEDED(sm != NULL, &err)) {
    return;
  }

  struct aviutl2_script_module_param param = {.push_result_boolean = mock_push_result_boolean,
                                              .push_result_int = mock_push_result_int};

  ctx.callback_value = true;
  ctx.resize_quality_value = 1;
  ctx.pushed_boolean_count = 0;
  ctx.pushed_int_count = 0;
  ptk_script_module_get_render_config(sm, &param, 42);
  TEST_CHECK(ctx.pushed_boolean_values[0] == true);
  TEST_CHECK(ctx.pushed_int_values[0] == 42);
  TEST_CHECK(ctx.pushed_int_values[1] == 1);

  ctx.callback_value = false;
  ctx.resize_quality_value = 0;
  ctx.pushed_boolean_count = 0;
  ctx.pushed_int_count = 0;
  ptk_script_module_get_render_config(sm, &param, 123);
  TEST_CHECK(ctx.pushed_boolean_values[0] == false);
  TEST_CHECK(ctx.pushed_int_values[0] == 123);
  TEST_CHECK(ctx.pushed_int_values[1] == 0);

  ptk_script_module_destroy(&sm);
  g_ctx = NULL;
}

static void test_script_module_generate_tag(void) {
  struct mock_context ctx = {0};
  g_ctx = &ctx;

  struct ov_error err = {0};
  struct ptk_script_module_callbacks callbacks = {0};
  struct ptk_script_module *sm = ptk_script_module_create(&callbacks, &err);
  if (!TEST_SUCCEEDED(sm != NULL, &err)) {
    return;
  }

  struct aviutl2_script_module_param param = {.push_result_int = mock_push_result_int};

  ptk_script_module_generate_tag(sm, &param);
  int const first_tag = ctx.pushed_int;
  TEST_CHECK(first_tag >= 0);

  ptk_script_module_generate_tag(sm, &param);
  int const second_tag = ctx.pushed_int;
  TEST_CHECK(second_tag >= 0);
  TEST_CHECK(first_tag != second_tag);

  ptk_script_module_destroy(&sm);
  g_ctx = NULL;
}

static void test_script_module_add_psd_file(void) {
  struct mock_context ctx = {0};
  g_ctx = &ctx;

  struct ov_error err = {0};
  struct ptk_script_module_callbacks callbacks = {.add_file = mock_add_file_callback};
  struct ptk_script_module *sm = ptk_script_module_create(&callbacks, &err);
  if (!TEST_SUCCEEDED(sm != NULL, &err)) {
    return;
  }

  struct aviutl2_script_module_param param = {
      .push_result_boolean = mock_push_result_boolean,
      .get_param_string = mock_get_param_string,
      .get_param_int = mock_get_param_int,
  };

  // Test: successful add
  ctx.param_strings[0] = "C:/test/image.psd";
  ctx.param_ints[1] = 12345;
  ctx.add_file_should_succeed = true;
  ctx.add_file_called = false;
  ctx.pushed_boolean_count = 0;

  ptk_script_module_add_psd_file(sm, &param);

  TEST_CHECK(ctx.add_file_called);
  TEST_CHECK(strcmp(ctx.received_path, "C:/test/image.psd") == 0);
  TEST_CHECK(ctx.received_tag == 12345);
  TEST_CHECK(ctx.pushed_boolean_values[0] == true);

  // Test: callback failure
  ctx.param_strings[0] = "C:/test/another.psd";
  ctx.param_ints[1] = 99999;
  ctx.add_file_should_succeed = false;
  ctx.add_file_called = false;
  ctx.pushed_boolean_count = 0;

  ptk_script_module_add_psd_file(sm, &param);

  TEST_CHECK(ctx.add_file_called);
  TEST_CHECK(ctx.pushed_boolean_values[0] == false);

  // Test: null path
  ctx.param_strings[0] = NULL;
  ctx.add_file_called = false;
  ctx.pushed_boolean_count = 0;

  ptk_script_module_add_psd_file(sm, &param);

  TEST_CHECK(!ctx.add_file_called);
  TEST_CHECK(ctx.pushed_boolean_values[0] == false);

  ptk_script_module_destroy(&sm);
  g_ctx = NULL;
}

static void test_script_module_set_props(void) {
  struct mock_context ctx = {0};
  g_ctx = &ctx;

  struct ov_error err = {0};
  struct ptk_script_module_callbacks callbacks = {.set_props = mock_set_props_callback};
  struct ptk_script_module *sm = ptk_script_module_create(&callbacks, &err);
  if (!TEST_SUCCEEDED(sm != NULL, &err)) {
    return;
  }

  struct aviutl2_script_module_param param = {
      .push_result_boolean = mock_push_result_boolean,
      .push_result_string = mock_push_result_string,
      .push_result_int = mock_push_result_int,
      .get_param_string = mock_get_param_string,
      .get_param_int = mock_get_param_int,
      .get_param_table_string = mock_get_param_table_string,
      .get_param_table_double = mock_get_param_table_double,
      .get_param_table_int = mock_get_param_table_int,
  };

  // Test: successful set_props with all parameters
  ctx.param_ints[0] = 42;                     // id
  ctx.param_strings[1] = "C:/test/image.psd"; // path
  ctx.param_table_strings[0] = "layer1";      // layer
  ctx.param_table_doubles[0] = 1.5;           // scale
  ctx.param_table_ints[0] = 10;               // offsetx
  ctx.param_table_ints[1] = 20;               // offsety
  ctx.param_table_ints[2] = 12345;            // tag
  ctx.set_props_should_succeed = true;
  ctx.set_props_result.modified = true;
  ctx.set_props_result.ckey = 0xabcdef0123456789ULL;
  ctx.set_props_result.width = 800;
  ctx.set_props_result.height = 600;
  ctx.set_props_result.flip_x = true;
  ctx.set_props_result.flip_y = false;
  ctx.pushed_int_count = 0;
  ctx.pushed_boolean_count = 0;

  ptk_script_module_set_props(sm, &param);

  TEST_CHECK(ctx.set_props_called);
  TEST_CHECK(ctx.set_props_received_id == 42);
  TEST_CHECK(strcmp(ctx.set_props_received_path, "C:/test/image.psd") == 0);
  TEST_CHECK(strcmp(ctx.set_props_received_layer, "layer1") == 0);
  TEST_CHECK(ctx.set_props_received_scale == 1.5);
  TEST_CHECK(ctx.set_props_received_offset_x == 10);
  TEST_CHECK(ctx.set_props_received_offset_y == 20);
  TEST_CHECK(ctx.set_props_received_tag == 12345);
  // 6 values: modified, cachekey, width, height, flip_x, flip_y
  TEST_CHECK(ctx.pushed_boolean_values[0] == true); // modified
  TEST_CHECK(strcmp(ctx.pushed_string, "abcdef0123456789") == 0);
  TEST_CHECK(ctx.pushed_int_values[0] == 800);
  TEST_CHECK(ctx.pushed_int_values[1] == 600);
  TEST_CHECK(ctx.pushed_boolean_values[1] == true);  // flip_x
  TEST_CHECK(ctx.pushed_boolean_values[2] == false); // flip_y

  // Test: scale = 0 means not set
  ctx.set_props_called = false;
  ctx.param_table_doubles[0] = 0.0;
  ctx.pushed_int_count = 0;

  ptk_script_module_set_props(sm, &param);

  TEST_CHECK(ctx.set_props_called);
  TEST_CHECK(ctx.set_props_received_scale == 0.0);

  // Test: tag = 0 means not set
  ctx.set_props_called = false;
  ctx.param_table_doubles[0] = 1.0;
  ctx.param_table_ints[2] = 0;
  ctx.pushed_int_count = 0;
  ctx.pushed_boolean_count = 0;

  ptk_script_module_set_props(sm, &param);

  TEST_CHECK(ctx.set_props_called);
  TEST_CHECK(ctx.set_props_received_tag == 0);

  // Test: layer = NULL
  ctx.set_props_called = false;
  ctx.param_table_strings[0] = NULL;
  ctx.pushed_int_count = 0;
  ctx.pushed_boolean_count = 0;

  ptk_script_module_set_props(sm, &param);

  TEST_CHECK(ctx.set_props_called);
  TEST_CHECK(ctx.set_props_received_layer == NULL);

  // Test: path = NULL -> callback not called, failure result
  ctx.set_props_called = false;
  ctx.param_strings[1] = NULL;
  ctx.pushed_int_count = 0;
  ctx.pushed_boolean_count = 0;

  ptk_script_module_set_props(sm, &param);

  TEST_CHECK(!ctx.set_props_called);
  TEST_CHECK(ctx.pushed_boolean_values[0] == false);
  TEST_CHECK(strcmp(ctx.pushed_string, "") == 0);
  TEST_CHECK(ctx.pushed_int_values[0] == 0);
  TEST_CHECK(ctx.pushed_int_values[1] == 0);

  // Test: callback failure -> failure result
  ctx.set_props_called = false;
  ctx.param_strings[1] = "C:/test/image.psd";
  ctx.set_props_should_succeed = false;
  ctx.pushed_int_count = 0;
  ctx.pushed_boolean_count = 0;

  ptk_script_module_set_props(sm, &param);

  TEST_CHECK(ctx.set_props_called);
  TEST_CHECK(ctx.pushed_boolean_values[0] == false);
  TEST_CHECK(strcmp(ctx.pushed_string, "") == 0);
  TEST_CHECK(ctx.pushed_int_values[0] == 0);
  TEST_CHECK(ctx.pushed_int_values[1] == 0);

  ptk_script_module_destroy(&sm);
  g_ctx = NULL;
}

static void test_script_module_get_drop_config(void) {
  struct mock_context ctx = {0};
  g_ctx = &ctx;

  struct ov_error err = {0};
  struct ptk_script_module_callbacks callbacks = {.get_drop_config = mock_get_drop_config_callback};
  struct ptk_script_module *sm = ptk_script_module_create(&callbacks, &err);
  if (!TEST_SUCCEEDED(sm != NULL, &err)) {
    return;
  }

  struct aviutl2_script_module_param param = {
      .push_result_boolean = mock_push_result_boolean,
      .push_result_table_int = mock_push_result_table_int,
  };

  // Test: successful get_drop_config with all true
  ctx.get_drop_config_should_succeed = true;
  ctx.get_drop_config_called = false;
  ctx.drop_config_result.debug_mode = true;
  ctx.drop_config_result.manual_shift_wav = true;
  ctx.drop_config_result.manual_shift_psd = true;
  ctx.drop_config_result.manual_wav_txt_pair = true;
  ctx.drop_config_result.manual_object_audio_text = true;
  ctx.drop_config_result.external_wav_txt_pair = true;
  ctx.drop_config_result.external_object_audio_text = true;
  ctx.pushed_table_num = 0;

  ptk_script_module_get_drop_config(sm, &param);

  TEST_CHECK(ctx.get_drop_config_called);
  TEST_CHECK(ctx.pushed_table_num == 7);
  TEST_CHECK(strcmp(ctx.pushed_table_keys[0], "debug_mode") == 0);
  TEST_CHECK(ctx.pushed_table_values[0] == 1);
  TEST_CHECK(strcmp(ctx.pushed_table_keys[1], "manual_shift_wav") == 0);
  TEST_CHECK(ctx.pushed_table_values[1] == 1);
  TEST_CHECK(strcmp(ctx.pushed_table_keys[2], "manual_shift_psd") == 0);
  TEST_CHECK(ctx.pushed_table_values[2] == 1);
  TEST_CHECK(strcmp(ctx.pushed_table_keys[3], "manual_wav_txt_pair") == 0);
  TEST_CHECK(ctx.pushed_table_values[3] == 1);
  TEST_CHECK(strcmp(ctx.pushed_table_keys[4], "manual_object_audio_text") == 0);
  TEST_CHECK(ctx.pushed_table_values[4] == 1);
  TEST_CHECK(strcmp(ctx.pushed_table_keys[5], "external_wav_txt_pair") == 0);
  TEST_CHECK(ctx.pushed_table_values[5] == 1);
  TEST_CHECK(strcmp(ctx.pushed_table_keys[6], "external_object_audio_text") == 0);
  TEST_CHECK(ctx.pushed_table_values[6] == 1);

  // Test: successful get_drop_config with all false
  ctx.get_drop_config_called = false;
  ctx.drop_config_result.debug_mode = false;
  ctx.drop_config_result.manual_shift_wav = false;
  ctx.drop_config_result.manual_shift_psd = false;
  ctx.drop_config_result.manual_wav_txt_pair = false;
  ctx.drop_config_result.manual_object_audio_text = false;
  ctx.drop_config_result.external_wav_txt_pair = false;
  ctx.drop_config_result.external_object_audio_text = false;
  ctx.pushed_table_num = 0;

  ptk_script_module_get_drop_config(sm, &param);

  TEST_CHECK(ctx.get_drop_config_called);
  TEST_CHECK(ctx.pushed_table_num == 7);
  TEST_CHECK(ctx.pushed_table_values[0] == 0);
  TEST_CHECK(ctx.pushed_table_values[1] == 0);
  TEST_CHECK(ctx.pushed_table_values[2] == 0);
  TEST_CHECK(ctx.pushed_table_values[3] == 0);
  TEST_CHECK(ctx.pushed_table_values[4] == 0);
  TEST_CHECK(ctx.pushed_table_values[5] == 0);
  TEST_CHECK(ctx.pushed_table_values[6] == 0);

  // Test: mixed values
  ctx.get_drop_config_called = false;
  ctx.drop_config_result.debug_mode = true;
  ctx.drop_config_result.manual_shift_wav = true;
  ctx.drop_config_result.manual_shift_psd = false;
  ctx.drop_config_result.manual_wav_txt_pair = false;
  ctx.drop_config_result.manual_object_audio_text = true;
  ctx.drop_config_result.external_wav_txt_pair = false;
  ctx.drop_config_result.external_object_audio_text = true;
  ctx.pushed_table_num = 0;

  ptk_script_module_get_drop_config(sm, &param);

  TEST_CHECK(ctx.get_drop_config_called);
  TEST_CHECK(ctx.pushed_table_num == 7);
  TEST_CHECK(ctx.pushed_table_values[0] == 1);
  TEST_CHECK(ctx.pushed_table_values[1] == 1);
  TEST_CHECK(ctx.pushed_table_values[2] == 0);
  TEST_CHECK(ctx.pushed_table_values[3] == 0);
  TEST_CHECK(ctx.pushed_table_values[4] == 1);
  TEST_CHECK(ctx.pushed_table_values[5] == 0);
  TEST_CHECK(ctx.pushed_table_values[6] == 1);

  // Test: callback failure
  ctx.get_drop_config_called = false;
  ctx.get_drop_config_should_succeed = false;
  ctx.pushed_table_num = 0;
  ctx.pushed_boolean_count = 0;

  ptk_script_module_get_drop_config(sm, &param);

  TEST_CHECK(ctx.get_drop_config_called);
  TEST_CHECK(ctx.pushed_boolean_values[0] == false);

  ptk_script_module_destroy(&sm);
  g_ctx = NULL;
}

static void test_script_module_draw(void) {
  struct mock_context ctx = {0};
  g_ctx = &ctx;

  struct ov_error err = {0};
  struct ptk_script_module_callbacks callbacks = {.draw = mock_draw_callback};
  struct ptk_script_module *sm = ptk_script_module_create(&callbacks, &err);
  if (!TEST_SUCCEEDED(sm != NULL, &err)) {
    return;
  }

  struct aviutl2_script_module_param param = {
      .push_result_boolean = mock_push_result_boolean,
      .get_param_string = mock_get_param_string,
      .get_param_int = mock_get_param_int,
  };

  // Test: successful draw
  ctx.param_ints[0] = 42;                     // id
  ctx.param_strings[1] = "C:/test/image.psd"; // path
  ctx.param_ints[2] = 800;                    // width
  ctx.param_ints[3] = 600;                    // height
  ctx.param_strings[4] = "abcdef0123456789";  // cachekey_hex
  ctx.draw_should_succeed = true;
  ctx.draw_called = false;
  ctx.pushed_boolean_count = 0;

  ptk_script_module_draw(sm, &param);

  TEST_CHECK(ctx.draw_called);
  TEST_CHECK(ctx.draw_received_id == 42);
  TEST_CHECK(strcmp(ctx.draw_received_path, "C:/test/image.psd") == 0);
  TEST_CHECK(ctx.draw_received_width == 800);
  TEST_CHECK(ctx.draw_received_height == 600);
  TEST_CHECK(ctx.draw_received_ckey == 0xabcdef0123456789ULL);
  TEST_CHECK(ctx.pushed_boolean_values[0] == true);

  // Test: uppercase hex also works
  ctx.param_strings[4] = "ABCDEF0123456789";
  ctx.draw_called = false;
  ctx.pushed_boolean_count = 0;

  ptk_script_module_draw(sm, &param);

  TEST_CHECK(ctx.draw_called);
  TEST_CHECK(ctx.draw_received_ckey == 0xABCDEF0123456789ULL);
  TEST_CHECK(ctx.pushed_boolean_values[0] == true);

  // Test: callback failure
  ctx.param_strings[4] = "abcdef0123456789";
  ctx.draw_should_succeed = false;
  ctx.draw_called = false;
  ctx.pushed_boolean_count = 0;

  ptk_script_module_draw(sm, &param);

  TEST_CHECK(ctx.draw_called);
  TEST_CHECK(ctx.pushed_boolean_values[0] == false);

  // Test: null path -> callback not called
  ctx.param_strings[1] = NULL;
  ctx.draw_should_succeed = true;
  ctx.draw_called = false;
  ctx.pushed_boolean_count = 0;

  ptk_script_module_draw(sm, &param);

  TEST_CHECK(!ctx.draw_called);
  TEST_CHECK(ctx.pushed_boolean_values[0] == false);

  // Test: null cachekey -> callback not called
  ctx.param_strings[1] = "C:/test/image.psd";
  ctx.param_strings[4] = NULL;
  ctx.draw_called = false;
  ctx.pushed_boolean_count = 0;

  ptk_script_module_draw(sm, &param);

  TEST_CHECK(!ctx.draw_called);
  TEST_CHECK(ctx.pushed_boolean_values[0] == false);

  // Test: invalid width (<= 0) -> callback not called
  ctx.param_strings[4] = "abcdef0123456789";
  ctx.param_ints[2] = 0;
  ctx.draw_called = false;
  ctx.pushed_boolean_count = 0;

  ptk_script_module_draw(sm, &param);

  TEST_CHECK(!ctx.draw_called);
  TEST_CHECK(ctx.pushed_boolean_values[0] == false);

  // Test: invalid height (<= 0) -> callback not called
  ctx.param_ints[2] = 800;
  ctx.param_ints[3] = -1;
  ctx.draw_called = false;
  ctx.pushed_boolean_count = 0;

  ptk_script_module_draw(sm, &param);

  TEST_CHECK(!ctx.draw_called);
  TEST_CHECK(ctx.pushed_boolean_values[0] == false);

  // Test: invalid hex character -> callback not called
  ctx.param_ints[3] = 600;
  ctx.param_strings[4] = "abcdef012345678g"; // 'g' is invalid
  ctx.draw_called = false;
  ctx.pushed_boolean_count = 0;

  ptk_script_module_draw(sm, &param);

  TEST_CHECK(!ctx.draw_called);
  TEST_CHECK(ctx.pushed_boolean_values[0] == false);

  ptk_script_module_destroy(&sm);
  g_ctx = NULL;
}

static void test_script_module_read_text_file(void) {
  struct mock_context ctx = {0};
  g_ctx = &ctx;

  struct ov_error err = {0};
  struct ptk_script_module_callbacks callbacks = {0};
  struct ptk_script_module *sm = ptk_script_module_create(&callbacks, &err);
  if (!TEST_SUCCEEDED(sm != NULL, &err)) {
    return;
  }

  struct aviutl2_script_module_param param = {
      .push_result_string = mock_push_result_string,
      .get_param_string = mock_get_param_string,
  };

  // Test: read Japanese filename
  ctx.param_strings[0] = TEST_PATH("script_module/日本語ファイル.txt");
  ctx.pushed_string = NULL;

  ptk_script_module_read_text_file(sm, &param);

  TEST_CHECK(ctx.pushed_string != NULL);
  if (ctx.pushed_string) {
    TEST_CHECK(strstr(ctx.pushed_string, "日本語") != NULL);
  }

  // Test: file not found -> returns nil, error_message
  ctx.param_strings[0] = TEST_PATH("script_module/nonexistent_file.txt");
  ctx.pushed_string = NULL;

  ptk_script_module_read_text_file(sm, &param);

  // On failure, first push is NULL (nil), second push is error message
  // Since mock only stores last push, we check it's the error message (not NULL content)
  // The function pushes nil first, then error string, so pushed_string should be error message
  TEST_CHECK(ctx.pushed_string != NULL);

  ptk_script_module_destroy(&sm);
  g_ctx = NULL;
}

static void test_script_module_get_preferred_languages(void) {
  struct mock_context ctx = {0};
  g_ctx = &ctx;

  struct ov_error err = {0};
  struct ptk_script_module_callbacks callbacks = {0};
  struct ptk_script_module *sm = ptk_script_module_create(&callbacks, &err);
  if (!TEST_SUCCEEDED(sm != NULL, &err)) {
    return;
  }

  struct aviutl2_script_module_param param = {
      .push_result_array_string = mock_push_result_array_string,
  };

  ctx.pushed_array_string_count = -1;

  ptk_script_module_get_preferred_languages(sm, &param);

  // Should return at least one language (system always has a default)
  TEST_CHECK(ctx.pushed_array_string_count >= 1);
  if (ctx.pushed_array_string_count >= 1) {
    // First language should be non-empty string
    TEST_CHECK(ctx.pushed_array_strings[0] != NULL);
    TEST_CHECK(strlen(ctx.pushed_array_strings[0]) > 0);
  }

  ptk_script_module_destroy(&sm);
  g_ctx = NULL;
}

static void test_script_module_detect_encoding(void) {
  struct mock_context ctx = {0};
  g_ctx = &ctx;

  struct ov_error err = {0};
  struct ptk_script_module_callbacks callbacks = {0};
  struct ptk_script_module *sm = ptk_script_module_create(&callbacks, &err);
  if (!TEST_SUCCEEDED(sm != NULL, &err)) {
    return;
  }

  struct aviutl2_script_module_param param = {
      .push_result_int = mock_push_result_int,
      .get_param_string = mock_get_param_string,
  };

  // Test: UTF-8 BOM
  ctx.param_strings[0] = "\xEF\xBB\xBFHello";
  ctx.pushed_int = -1;
  ptk_script_module_detect_encoding(sm, &param);
  TEST_CHECK(ctx.pushed_int == 1); // UTF-8

  // Test: UTF-16LE BOM
  ctx.param_strings[0] = "\xFF\xFEHello";
  ctx.pushed_int = -1;
  ptk_script_module_detect_encoding(sm, &param);
  TEST_CHECK(ctx.pushed_int == 2); // UTF-16LE

  // Test: UTF-16BE BOM
  ctx.param_strings[0] = "\xFE\xFFHello";
  ctx.pushed_int = -1;
  ptk_script_module_detect_encoding(sm, &param);
  TEST_CHECK(ctx.pushed_int == 3); // UTF-16BE

  // Test: Valid UTF-8 without BOM (ASCII)
  ctx.param_strings[0] = "Hello World";
  ctx.pushed_int = -1;
  ptk_script_module_detect_encoding(sm, &param);
  TEST_CHECK(ctx.pushed_int == 1); // UTF-8

  // Test: Valid UTF-8 without BOM (Japanese)
  ctx.param_strings[0] = "こんにちは";
  ctx.pushed_int = -1;
  ptk_script_module_detect_encoding(sm, &param);
  TEST_CHECK(ctx.pushed_int == 1); // UTF-8

  // Test: Invalid UTF-8 (Shift_JIS "あ" = 0x82 0xA0)
  ctx.param_strings[0] = "\x82\xA0";
  ctx.pushed_int = -1;
  ptk_script_module_detect_encoding(sm, &param);
  TEST_CHECK(ctx.pushed_int == 0); // Unknown

  // Test: Empty string
  ctx.param_strings[0] = "";
  ctx.pushed_int = -1;
  ptk_script_module_detect_encoding(sm, &param);
  TEST_CHECK(ctx.pushed_int == 1); // Empty is valid UTF-8

  // Test: NULL string
  ctx.param_strings[0] = NULL;
  ctx.pushed_int = -1;
  ptk_script_module_detect_encoding(sm, &param);
  TEST_CHECK(ctx.pushed_int == 0); // Unknown

  ptk_script_module_destroy(&sm);
  g_ctx = NULL;
}

TEST_LIST = {
    {"test_script_module_get_render_config", test_script_module_get_render_config},
    {"test_script_module_generate_tag", test_script_module_generate_tag},
    {"test_script_module_add_psd_file", test_script_module_add_psd_file},
    {"test_script_module_set_props", test_script_module_set_props},
    {"test_script_module_get_drop_config", test_script_module_get_drop_config},
    {"test_script_module_draw", test_script_module_draw},
    {"test_script_module_read_text_file", test_script_module_read_text_file},
    {"test_script_module_get_preferred_languages", test_script_module_get_preferred_languages},
    {"test_script_module_detect_encoding", test_script_module_detect_encoding},
    {NULL, NULL},
};
