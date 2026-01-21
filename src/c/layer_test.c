// Stub out external dependencies before including layer.c
// We need to include headers first, then provide stub implementations

#include "anm2.h"
#include "anm2_edit.h"
#include "anm2editor.h"
#include "dialog.h"
#include "error.h"
#include "layer.h"
#include "logf.h"

#include <ovarray.h>
#include <ovbase.h>

// Stub implementations - these must match the declarations in the headers
bool ptk_error_get_main_message(struct ov_error *const err, wchar_t **const dest) {
  (void)err;
  (void)dest;
  return false;
}

void ptk_logf_error(struct ov_error const *const err, char const *const reference, char const *const format, ...) {
  (void)err;
  (void)reference;
  (void)format;
}

int ptk_dialog_show(struct ptk_dialog_params const *params) {
  (void)params;
  return IDOK;
}

int ptk_error_dialog(HWND owner,
                     struct ov_error *const err,
                     wchar_t const *const window_title,
                     wchar_t const *const main_instruction,
                     wchar_t const *const content,
                     PCWSTR icon,
                     TASKDIALOG_COMMON_BUTTON_FLAGS buttons) {
  (void)owner;
  (void)err;
  (void)window_title;
  (void)main_instruction;
  (void)content;
  (void)icon;
  (void)buttons;
  return IDOK;
}

// Stub implementations for anm2 functions
struct ptk_anm2 *ptk_anm2_create(struct ov_error *const err) {
  (void)err;
  return NULL;
}

void ptk_anm2_destroy(struct ptk_anm2 **doc) { (void)doc; }

bool ptk_anm2_load(struct ptk_anm2 *doc, wchar_t const *path, struct ov_error *const err) {
  (void)doc;
  (void)path;
  (void)err;
  return false;
}

bool ptk_anm2_save(struct ptk_anm2 *doc, wchar_t const *path, struct ov_error *const err) {
  (void)doc;
  (void)path;
  (void)err;
  return false;
}

bool ptk_anm2_verify_checksum(struct ptk_anm2 const *doc) {
  (void)doc;
  return true;
}

uint32_t ptk_anm2_item_insert_animation(
    struct ptk_anm2 *doc, uint32_t before_id, char const *script_name, char const *name, struct ov_error *const err) {
  (void)doc;
  (void)before_id;
  (void)script_name;
  (void)name;
  (void)err;
  return 0;
}

uint32_t ptk_anm2_param_insert(struct ptk_anm2 *doc,
                               uint32_t item_id,
                               uint32_t before_param_id,
                               char const *key,
                               char const *value,
                               struct ov_error *const err) {
  (void)doc;
  (void)item_id;
  (void)before_param_id;
  (void)key;
  (void)value;
  (void)err;
  return 0;
}

bool ptk_anm2_set_psd_path(struct ptk_anm2 *doc, char const *path, struct ov_error *const err) {
  (void)doc;
  (void)path;
  (void)err;
  return false;
}

uint32_t
ptk_anm2_selector_insert(struct ptk_anm2 *doc, uint32_t before_id, char const *group, struct ov_error *const err) {
  (void)doc;
  (void)before_id;
  (void)group;
  (void)err;
  return 0;
}

uint32_t ptk_anm2_item_insert_value(
    struct ptk_anm2 *doc, uint32_t before_id, char const *name, char const *value, struct ov_error *const err) {
  (void)doc;
  (void)before_id;
  (void)name;
  (void)value;
  (void)err;
  return 0;
}

// Stub implementations for anm2editor functions
bool ptk_anm2editor_is_open(struct ptk_anm2editor *editor) {
  (void)editor;
  return false;
}

struct ptk_anm2_edit *ptk_anm2editor_get_edit(struct ptk_anm2editor *editor) {
  (void)editor;
  return NULL;
}

bool ptk_anm2editor_add_value_items(struct ptk_anm2editor *editor,
                                    char const *psd_path,
                                    char const *group,
                                    char const *const *names,
                                    char const *const *values,
                                    size_t count,
                                    struct ov_error *err) {
  (void)editor;
  (void)psd_path;
  (void)group;
  (void)names;
  (void)values;
  (void)count;
  (void)err;
  return false;
}

bool ptk_anm2editor_add_value_item_to_selected(struct ptk_anm2editor *editor,
                                               char const *psd_path,
                                               char const *group,
                                               char const *name,
                                               char const *value,
                                               struct ov_error *err) {
  (void)editor;
  (void)psd_path;
  (void)group;
  (void)name;
  (void)value;
  (void)err;
  return false;
}

// Stub implementations for anm2_edit ptkl functions
void ptk_anm2_edit_ptkl_targets_free(struct ptk_anm2_edit_ptkl_targets *targets) {
  if (targets && targets->items) {
    size_t const len = OV_ARRAY_LENGTH(targets->items);
    for (size_t i = 0; i < len; i++) {
      struct ptk_anm2_edit_ptkl_target *t = &targets->items[i];
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
  if (targets) {
    *targets = (struct ptk_anm2_edit_ptkl_targets){0};
  }
}

bool ptk_anm2_edit_collect_ptkl_targets(struct ptk_anm2_edit *edit,
                                        struct ptk_anm2_edit_ptkl_targets *targets,
                                        struct ov_error *err) {
  (void)edit;
  (void)err;
  if (targets) {
    *targets = (struct ptk_anm2_edit_ptkl_targets){0};
  }
  return true;
}

bool ptk_anm2_edit_set_param_value_by_id(struct ptk_anm2_edit *edit,
                                         uint32_t param_id,
                                         char const *value,
                                         struct ov_error *err) {
  (void)edit;
  (void)param_id;
  (void)value;
  (void)err;
  return false;
}

// Include layer.c directly to test static functions
#include "layer.c"

#include <ovtest.h>

#include <stdio.h>
#include <string.h>

static void test_is_digits(void) {
  TEST_CHECK(is_digits("123", 3));
  TEST_CHECK(is_digits("0", 1));
  TEST_CHECK(is_digits("9876543210", 10));
  TEST_CHECK(!is_digits("12a", 3));
  TEST_CHECK(!is_digits("a12", 3));
  TEST_CHECK(!is_digits("1a2", 3));
  TEST_CHECK(!is_digits("abc", 3));
  TEST_CHECK(!is_digits("", 0));
  TEST_CHECK(!is_digits(NULL, 0));
  // Partial check - only first 2 chars
  TEST_CHECK(is_digits("12abc", 2));
  TEST_CHECK(!is_digits("12abc", 3));
}

static void test_ends_with_ptkl_suffix(void) {
  TEST_CHECK(ends_with_ptkl_suffix("foo~ptkl", 8));
  TEST_CHECK(ends_with_ptkl_suffix("~ptkl", 5));
  TEST_CHECK(ends_with_ptkl_suffix("a~ptkl", 6));
  TEST_CHECK(!ends_with_ptkl_suffix("foo", 3));
  TEST_CHECK(!ends_with_ptkl_suffix("~ptk", 4));
  TEST_CHECK(!ends_with_ptkl_suffix("ptkl", 4));
  TEST_CHECK(!ends_with_ptkl_suffix("", 0));
  TEST_CHECK(!ends_with_ptkl_suffix("~ptklx", 6));
  TEST_CHECK(!ends_with_ptkl_suffix("foo~ptkl ", 9));
  // Test with explicit length shorter than actual string
  TEST_CHECK(ends_with_ptkl_suffix("foo~ptklextra", 8)); // only check "foo~ptkl"
  TEST_CHECK(!ends_with_ptkl_suffix("foo~ptklextra", 13));
}

static void test_has_children(void) {
  struct layer_info li = {0};
  struct ov_error err = {0};

  // Build layer_info with: "parent", "parent/a", "parent/b", "other"
  char const names[] = "parent\0parent/a\0parent/b\0other";
  char const values[] = "vp\0va\0vb\0vo";
  size_t const names_len = sizeof(names);
  size_t const values_len = sizeof(values);

  if (!TEST_SUCCEEDED(layer_info_parse(&li, names, names_len, values, values_len, NULL, NULL, &err), &err)) {
    return;
  }

  // "parent" (index 0) has children "parent/a" and "parent/b"
  TEST_CHECK(has_children(&li, 0) == true);

  // "parent/a" (index 1) has no children
  TEST_CHECK(has_children(&li, 1) == false);

  // "parent/b" (index 2) has no children
  TEST_CHECK(has_children(&li, 2) == false);

  // "other" (index 3) has no children
  TEST_CHECK(has_children(&li, 3) == false);

  layer_info_free(&li);
}

static void test_enumerate_children(void) {
  struct ov_error err = {0};
  size_t *indices = NULL;
  struct layer_info li = {0};

  // Build layer_info with: "parent", "parent/a", "parent/b", "parent/a/x", "other"
  char const names[] = "parent\0parent/a\0parent/b\0parent/a/x\0other";
  char const values[] = "vp\0va\0vb\0vax\0vo";
  size_t const names_len = sizeof(names);
  size_t const values_len = sizeof(values);

  if (!TEST_SUCCEEDED(layer_info_parse(&li, names, names_len, values, values_len, NULL, NULL, &err), &err)) {
    return;
  }

  // Select "parent" (index 0), should find direct children: parent/a, parent/b (not parent/a/x)
  if (!TEST_SUCCEEDED(enumerate_children(&li, 0, &indices, &err), &err)) {
    layer_info_free(&li);
    return;
  }
  size_t count = OV_ARRAY_LENGTH(indices);
  TEST_CHECK(count == 2);
  TEST_MSG("want 2, got %zu", count);
  if (count == 2) {
    TEST_CHECK(indices[0] == 1); // parent/a
    TEST_CHECK(indices[1] == 2); // parent/b
  }
  OV_ARRAY_DESTROY(&indices);

  // Select "parent/a" (index 1), should find direct child: parent/a/x
  if (!TEST_SUCCEEDED(enumerate_children(&li, 1, &indices, &err), &err)) {
    layer_info_free(&li);
    return;
  }
  count = OV_ARRAY_LENGTH(indices);
  TEST_CHECK(count == 1);
  TEST_MSG("want 1, got %zu", count);
  if (count == 1) {
    TEST_CHECK(indices[0] == 3); // parent/a/x
  }
  OV_ARRAY_DESTROY(&indices);

  // Select "other" (index 4), should find no children
  if (!TEST_SUCCEEDED(enumerate_children(&li, 4, &indices, &err), &err)) {
    layer_info_free(&li);
    return;
  }
  count = OV_ARRAY_LENGTH(indices);
  TEST_CHECK(count == 0);
  TEST_MSG("want 0, got %zu", count);
  if (indices) {
    OV_ARRAY_DESTROY(&indices);
  }

  layer_info_free(&li);
}

static void test_collect_ptkl_targets_from_alias(void) {
  struct ov_error err = {0};
  struct ptkl_targets targets = {0};

  // Test with empty input
  if (!TEST_SUCCEEDED(collect_ptkl_targets_from_alias("", 0, &targets, &err), &err)) {
    return;
  }
  TEST_CHECK(OV_ARRAY_LENGTH(targets.items) == 0);
  ptkl_targets_free(&targets);

  // Test with valid alias data containing ~ptkl items
  char const alias[] = "[Object.1]\n"
                       "effect.name=TestEffect@PSDToolKit\n"
                       "open~ptkl=value1\n"
                       "close~ptkl=value2\n"
                       "normalkey=value3\n"
                       "[Object.2]\n"
                       "effect.name=AnotherEffect\n"
                       "item~ptkl=value4\n";
  size_t const alias_len = sizeof(alias) - 1;

  targets = (struct ptkl_targets){0};
  if (!TEST_SUCCEEDED(collect_ptkl_targets_from_alias(alias, alias_len, &targets, &err), &err)) {
    return;
  }

  TEST_CHECK(OV_ARRAY_LENGTH(targets.items) == 3);
  TEST_MSG("want 3, got %zu", OV_ARRAY_LENGTH(targets.items));

  // Check that effects array has 2 unique effect names
  TEST_CHECK(OV_ARRAY_LENGTH(targets.effects) == 2);
  TEST_MSG("want 2 effects, got %zu", OV_ARRAY_LENGTH(targets.effects));

  // Check that all expected items are present (order may vary based on ini_reader iteration order)
  bool found_open = false, found_close = false, found_item = false;
  for (size_t i = 0; i < OV_ARRAY_LENGTH(targets.items); i++) {
    char const *effect_name = targets.items[i].effect_name;
    if (strcmp(effect_name, "TestEffect@PSDToolKit") == 0) {
      if (strcmp(targets.items[i].item_name, "open~ptkl") == 0) {
        found_open = true;
      } else if (strcmp(targets.items[i].item_name, "close~ptkl") == 0) {
        found_close = true;
      }
    } else if (strcmp(effect_name, "AnotherEffect") == 0) {
      if (strcmp(targets.items[i].item_name, "item~ptkl") == 0) {
        found_item = true;
      }
    }
  }
  TEST_CHECK(found_open);
  TEST_MSG("open~ptkl not found");
  TEST_CHECK(found_close);
  TEST_MSG("close~ptkl not found");
  TEST_CHECK(found_item);
  TEST_MSG("item~ptkl not found");

  ptkl_targets_free(&targets);

  // Test section without effect.name (should be skipped)
  char const no_effect[] = "[Object.1]\n"
                           "key~ptkl=value\n";
  targets = (struct ptkl_targets){0};
  if (!TEST_SUCCEEDED(collect_ptkl_targets_from_alias(no_effect, sizeof(no_effect) - 1, &targets, &err), &err)) {
    return;
  }
  TEST_CHECK(OV_ARRAY_LENGTH(targets.items) == 0);
  TEST_MSG("want 0, got %zu", OV_ARRAY_LENGTH(targets.items));
  ptkl_targets_free(&targets);

  // Test non-Object section (should be skipped)
  char const other_section[] = "[Settings]\n"
                               "effect.name=Test\n"
                               "key~ptkl=value\n";
  targets = (struct ptkl_targets){0};
  if (!TEST_SUCCEEDED(collect_ptkl_targets_from_alias(other_section, sizeof(other_section) - 1, &targets, &err),
                      &err)) {
    return;
  }
  TEST_CHECK(OV_ARRAY_LENGTH(targets.items) == 0);
  TEST_MSG("want 0, got %zu", OV_ARRAY_LENGTH(targets.items));
  ptkl_targets_free(&targets);

  // Test Object section with non-numeric suffix (should be skipped)
  char const non_numeric[] = "[Object.abc]\n"
                             "effect.name=Test\n"
                             "key~ptkl=value\n";
  targets = (struct ptkl_targets){0};
  if (!TEST_SUCCEEDED(collect_ptkl_targets_from_alias(non_numeric, sizeof(non_numeric) - 1, &targets, &err), &err)) {
    return;
  }
  TEST_CHECK(OV_ARRAY_LENGTH(targets.items) == 0);
  TEST_MSG("want 0, got %zu", OV_ARRAY_LENGTH(targets.items));
  ptkl_targets_free(&targets);

  // Test Object section with mixed numeric/alpha suffix (should be skipped)
  char const mixed_suffix[] = "[Object.1a]\n"
                              "effect.name=Test\n"
                              "key~ptkl=value\n";
  targets = (struct ptkl_targets){0};
  if (!TEST_SUCCEEDED(collect_ptkl_targets_from_alias(mixed_suffix, sizeof(mixed_suffix) - 1, &targets, &err), &err)) {
    return;
  }
  TEST_CHECK(OV_ARRAY_LENGTH(targets.items) == 0);
  TEST_MSG("want 0, got %zu", OV_ARRAY_LENGTH(targets.items));
  ptkl_targets_free(&targets);
}

static void test_layer_info_get_item_name(void) {
  struct ov_error err = {0};
  struct layer_info li = {0};
  char *name = NULL;

  // Test layer names (path/to/item -> item)
  char const names[] = "root/child/leaf\0other";
  char const values[] = "v1\0v2";

  if (!TEST_SUCCEEDED(layer_info_parse(&li, names, sizeof(names), values, sizeof(values), NULL, NULL, &err), &err)) {
    return;
  }

  if (!TEST_SUCCEEDED(layer_info_get_item_name(&li, 0, &name, &err), &err)) {
    layer_info_free(&li);
    return;
  }
  TEST_CHECK(strcmp(name, "leaf") == 0);
  TEST_MSG("want 'leaf', got '%s'", name);
  OV_FREE(&name);

  if (!TEST_SUCCEEDED(layer_info_get_item_name(&li, 1, &name, &err), &err)) {
    layer_info_free(&li);
    return;
  }
  TEST_CHECK(strcmp(name, "other") == 0);
  TEST_MSG("want 'other', got '%s'", name);
  OV_FREE(&name);

  layer_info_free(&li);

  // Test FAView (full name returned)
  li = (struct layer_info){0};
  char const fav_names[] = "item1\0item2";
  char const fav_values[] = "v1\0v2";

  if (!TEST_SUCCEEDED(
          layer_info_parse(&li, fav_names, sizeof(fav_names), fav_values, sizeof(fav_values), "slider", NULL, &err),
          &err)) {
    return;
  }

  if (!TEST_SUCCEEDED(layer_info_get_item_name(&li, 0, &name, &err), &err)) {
    layer_info_free(&li);
    return;
  }
  TEST_CHECK(strcmp(name, "item1") == 0);
  TEST_MSG("want 'item1', got '%s'", name);
  OV_FREE(&name);

  layer_info_free(&li);
}

static void test_layer_info_get_group_name(void) {
  struct ov_error err = {0};
  struct layer_info li = {0};
  char *name = NULL;

  // Test layer names (parent folder name)
  char const names[] = "root/parent/leaf";
  char const values[] = "v1";

  if (!TEST_SUCCEEDED(layer_info_parse(&li, names, sizeof(names), values, sizeof(values), NULL, NULL, &err), &err)) {
    return;
  }

  if (!TEST_SUCCEEDED(layer_info_get_group_name(&li, 0, &name, &err), &err)) {
    layer_info_free(&li);
    return;
  }
  TEST_CHECK(strcmp(name, "parent") == 0);
  TEST_MSG("want 'parent', got '%s'", name);
  OV_FREE(&name);

  layer_info_free(&li);

  // Test FAView (slider name last component)
  li = (struct layer_info){0};
  char const fav_names[] = "item1";
  char const fav_values[] = "v1";

  if (!TEST_SUCCEEDED(
          layer_info_parse(
              &li, fav_names, sizeof(fav_names), fav_values, sizeof(fav_values), "*path\\slider", NULL, &err),
          &err)) {
    return;
  }

  if (!TEST_SUCCEEDED(layer_info_get_group_name(&li, 0, &name, &err), &err)) {
    layer_info_free(&li);
    return;
  }
  TEST_CHECK(strcmp(name, "slider") == 0);
  TEST_MSG("want 'slider', got '%s'", name);
  OV_FREE(&name);

  layer_info_free(&li);
}

TEST_LIST = {
    {"is_digits", test_is_digits},
    {"ends_with_ptkl_suffix", test_ends_with_ptkl_suffix},
    {"has_children", test_has_children},
    {"enumerate_children", test_enumerate_children},
    {"collect_ptkl_targets_from_alias", test_collect_ptkl_targets_from_alias},
    {"layer_info_get_item_name", test_layer_info_get_item_name},
    {"layer_info_get_group_name", test_layer_info_get_group_name},
    {NULL, NULL},
};
