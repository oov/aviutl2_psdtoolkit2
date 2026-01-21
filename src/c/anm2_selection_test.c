#include "anm2_selection.h"

#include "anm2.h"

#include <ovtest.h>

static bool selection_contains(uint32_t const *ids, size_t count, uint32_t id) {
  for (size_t i = 0; i < count; ++i) {
    if (ids[i] == id) {
      return true;
    }
  }
  return false;
}

static void test_selection_create_destroy(void) {
  struct ov_error err = {0};
  struct ptk_anm2 *doc = ptk_anm2_create(&err);
  struct anm2_selection *sel = NULL;

  TEST_ASSERT_SUCCEEDED(doc != NULL, &err);

  TEST_FAILED_WITH(
      anm2_selection_create(NULL, &err) != NULL, &err, ov_error_type_generic, ov_error_generic_invalid_argument);

  sel = anm2_selection_create(doc, &err);
  TEST_ASSERT_SUCCEEDED(sel != NULL, &err);

  struct anm2_selection_state state = {0};
  anm2_selection_get_state(sel, &state);
  TEST_CHECK(state.focus_type == anm2_selection_focus_none);
  TEST_CHECK(state.focus_id == 0);
  TEST_CHECK(state.anchor_id == 0);

  size_t count = 1;
  uint32_t const *ids = anm2_selection_get_selected_ids(sel, &count);
  TEST_CHECK(count == 0);
  TEST_CHECK(ids == NULL);

  anm2_selection_destroy(&sel);
  TEST_CHECK(sel == NULL);
  ptk_anm2_destroy(&doc);
}

static void test_selection_set_focus_selector(void) {
  struct ov_error err = {0};
  struct ptk_anm2 *doc = ptk_anm2_create(&err);
  struct anm2_selection *sel = NULL;

  TEST_ASSERT_SUCCEEDED(doc != NULL, &err);
  sel = anm2_selection_create(doc, &err);
  TEST_ASSERT_SUCCEEDED(sel != NULL, &err);

  uint32_t id_a = ptk_anm2_selector_insert(doc, 0, "A", &err);
  uint32_t id_b = ptk_anm2_selector_insert(doc, 0, "B", &err);
  TEST_ASSERT_SUCCEEDED(id_a != 0, &err);
  TEST_ASSERT_SUCCEEDED(id_b != 0, &err);

  TEST_ASSERT_SUCCEEDED(anm2_selection_set_focus_selector(sel, id_b, &err), &err);

  struct anm2_selection_state state = {0};
  anm2_selection_get_state(sel, &state);
  TEST_CHECK(state.focus_type == anm2_selection_focus_selector);
  TEST_CHECK(state.focus_id == id_b);
  TEST_CHECK(state.anchor_id == 0);
  TEST_CHECK(anm2_selection_get_selected_count(sel) == 0);

  TEST_FAILED_WITH(anm2_selection_set_focus_selector(sel, 999999, &err),
                   &err,
                   ov_error_type_generic,
                   ov_error_generic_invalid_argument);

  anm2_selection_destroy(&sel);
  ptk_anm2_destroy(&doc);
}

static void test_selection_set_focus_item_update_anchor(void) {
  struct ov_error err = {0};
  struct ptk_anm2 *doc = ptk_anm2_create(&err);
  struct anm2_selection *sel = NULL;
  uint32_t sel_id = 0;

  TEST_ASSERT_SUCCEEDED(doc != NULL, &err);
  sel = anm2_selection_create(doc, &err);
  TEST_ASSERT_SUCCEEDED(sel != NULL, &err);

  sel_id = ptk_anm2_selector_insert(doc, 0, "Group", &err);
  TEST_ASSERT_SUCCEEDED(sel_id != 0, &err);
  uint32_t id_a = ptk_anm2_item_insert_value(doc, sel_id, "A", "a", &err);
  uint32_t id_b = ptk_anm2_item_insert_value(doc, sel_id, "B", "b", &err);
  TEST_ASSERT_SUCCEEDED(id_a != 0, &err);
  TEST_ASSERT_SUCCEEDED(id_b != 0, &err);

  TEST_ASSERT_SUCCEEDED(anm2_selection_set_focus_item(sel, id_a, true, &err), &err);
  struct anm2_selection_state state = {0};
  anm2_selection_get_state(sel, &state);
  TEST_CHECK(state.focus_type == anm2_selection_focus_item);
  TEST_CHECK(state.focus_id == id_a);
  TEST_CHECK(state.anchor_id == id_a);

  TEST_ASSERT_SUCCEEDED(anm2_selection_set_focus_item(sel, id_b, false, &err), &err);
  anm2_selection_get_state(sel, &state);
  TEST_CHECK(state.focus_id == id_b);
  TEST_CHECK(state.anchor_id == id_a);

  size_t count = 0;
  uint32_t const *ids = anm2_selection_get_selected_ids(sel, &count);
  TEST_CHECK(count == 1);
  TEST_CHECK(ids && ids[0] == id_b);

  TEST_FAILED_WITH(anm2_selection_set_focus_item(sel, 999999, true, &err),
                   &err,
                   ov_error_type_generic,
                   ov_error_generic_invalid_argument);

  anm2_selection_destroy(&sel);
  ptk_anm2_destroy(&doc);
}

static void test_selection_apply_treeview_selection_basic(void) {
  struct ov_error err = {0};
  struct ptk_anm2 *doc = ptk_anm2_create(&err);
  struct anm2_selection *sel = NULL;
  uint32_t sel_id = 0;

  TEST_ASSERT_SUCCEEDED(doc != NULL, &err);
  sel = anm2_selection_create(doc, &err);
  TEST_ASSERT_SUCCEEDED(sel != NULL, &err);

  sel_id = ptk_anm2_selector_insert(doc, 0, "Group", &err);
  TEST_ASSERT_SUCCEEDED(sel_id != 0, &err);
  uint32_t id_a = ptk_anm2_item_insert_value(doc, sel_id, "A", "a", &err);
  uint32_t id_b = ptk_anm2_item_insert_value(doc, sel_id, "B", "b", &err);
  uint32_t id_c = ptk_anm2_item_insert_value(doc, sel_id, "C", "c", &err);
  TEST_ASSERT_SUCCEEDED(id_a != 0, &err);
  TEST_ASSERT_SUCCEEDED(id_b != 0, &err);
  TEST_ASSERT_SUCCEEDED(id_c != 0, &err);

  TEST_ASSERT_SUCCEEDED(anm2_selection_apply_treeview_selection(sel, id_a, false, false, false, &err), &err);
  size_t count = 0;
  uint32_t const *ids = anm2_selection_get_selected_ids(sel, &count);
  TEST_CHECK(count == 1);
  TEST_CHECK(ids && ids[0] == id_a);

  struct anm2_selection_state state = {0};
  anm2_selection_get_state(sel, &state);
  TEST_CHECK(state.anchor_id == id_a);
  TEST_CHECK(state.focus_id == id_a);

  TEST_ASSERT_SUCCEEDED(anm2_selection_apply_treeview_selection(sel, id_b, false, true, false, &err), &err);
  ids = anm2_selection_get_selected_ids(sel, &count);
  TEST_CHECK(count == 2);
  TEST_CHECK(selection_contains(ids, count, id_a));
  TEST_CHECK(selection_contains(ids, count, id_b));
  anm2_selection_get_state(sel, &state);
  TEST_CHECK(state.anchor_id == id_b);
  TEST_CHECK(state.focus_id == id_b);

  TEST_ASSERT_SUCCEEDED(anm2_selection_apply_treeview_selection(sel, id_c, false, false, true, &err), &err);
  ids = anm2_selection_get_selected_ids(sel, &count);
  TEST_CHECK(count == 2);
  TEST_CHECK(selection_contains(ids, count, id_b));
  TEST_CHECK(selection_contains(ids, count, id_c));
  anm2_selection_get_state(sel, &state);
  TEST_CHECK(state.anchor_id == id_b);
  TEST_CHECK(state.focus_id == id_c);

  TEST_ASSERT_SUCCEEDED(anm2_selection_apply_treeview_selection(sel, id_c, false, true, false, &err), &err);
  ids = anm2_selection_get_selected_ids(sel, &count);
  TEST_CHECK(count == 1);
  TEST_CHECK(selection_contains(ids, count, id_b));
  anm2_selection_get_state(sel, &state);
  TEST_CHECK(state.anchor_id == id_b);
  TEST_CHECK(state.focus_id == id_c);

  TEST_ASSERT_SUCCEEDED(anm2_selection_apply_treeview_selection(sel, 0, false, false, false, &err), &err);
  ids = anm2_selection_get_selected_ids(sel, &count);
  TEST_CHECK(count == 0);
  TEST_CHECK(ids == NULL);
  anm2_selection_get_state(sel, &state);
  TEST_CHECK(state.focus_type == anm2_selection_focus_none);
  TEST_CHECK(state.anchor_id == 0);

  anm2_selection_destroy(&sel);
  ptk_anm2_destroy(&doc);
}

static void test_selection_apply_treeview_selector(void) {
  struct ov_error err = {0};
  struct ptk_anm2 *doc = ptk_anm2_create(&err);
  struct anm2_selection *sel = NULL;

  TEST_ASSERT_SUCCEEDED(doc != NULL, &err);
  sel = anm2_selection_create(doc, &err);
  TEST_ASSERT_SUCCEEDED(sel != NULL, &err);

  uint32_t group_id = ptk_anm2_selector_insert(doc, 0, "Group", &err);
  TEST_ASSERT_SUCCEEDED(group_id != 0, &err);
  uint32_t id_a = ptk_anm2_item_insert_value(doc, group_id, "A", "a", &err);
  uint32_t id_b = ptk_anm2_item_insert_value(doc, group_id, "B", "b", &err);
  TEST_ASSERT_SUCCEEDED(id_a != 0, &err);
  TEST_ASSERT_SUCCEEDED(id_b != 0, &err);

  TEST_ASSERT_SUCCEEDED(anm2_selection_apply_treeview_selection(sel, id_a, false, false, false, &err), &err);
  TEST_ASSERT_SUCCEEDED(anm2_selection_apply_treeview_selection(sel, id_b, false, true, false, &err), &err);
  TEST_ASSERT_SUCCEEDED(anm2_selection_apply_treeview_selection(sel, group_id, true, true, false, &err), &err);

  size_t count = 0;
  uint32_t const *ids = anm2_selection_get_selected_ids(sel, &count);
  TEST_CHECK(count == 2);
  TEST_CHECK(selection_contains(ids, count, id_a));
  TEST_CHECK(selection_contains(ids, count, id_b));

  struct anm2_selection_state state = {0};
  anm2_selection_get_state(sel, &state);
  TEST_CHECK(state.focus_type == anm2_selection_focus_selector);
  TEST_CHECK(state.focus_id == group_id);
  TEST_CHECK(state.anchor_id == id_b);

  TEST_ASSERT_SUCCEEDED(anm2_selection_apply_treeview_selection(sel, group_id, true, false, false, &err), &err);
  ids = anm2_selection_get_selected_ids(sel, &count);
  TEST_CHECK(count == 0);
  TEST_CHECK(ids == NULL);
  anm2_selection_get_state(sel, &state);
  TEST_CHECK(state.focus_type == anm2_selection_focus_selector);
  TEST_CHECK(state.anchor_id == 0);

  anm2_selection_destroy(&sel);
  ptk_anm2_destroy(&doc);
}

static void test_selection_apply_range_across_selectors(void) {
  struct ov_error err = {0};
  struct ptk_anm2 *doc = ptk_anm2_create(&err);
  struct anm2_selection *sel = NULL;
  uint32_t sel_a = 0;
  uint32_t sel_b = 0;

  TEST_ASSERT_SUCCEEDED(doc != NULL, &err);
  sel = anm2_selection_create(doc, &err);
  TEST_ASSERT_SUCCEEDED(sel != NULL, &err);

  sel_a = ptk_anm2_selector_insert(doc, 0, "A", &err);
  TEST_ASSERT_SUCCEEDED(sel_a != 0, &err);
  sel_b = ptk_anm2_selector_insert(doc, 0, "B", &err);
  TEST_ASSERT_SUCCEEDED(sel_b != 0, &err);
  uint32_t id_a1 = ptk_anm2_item_insert_value(doc, sel_a, "A1", "a1", &err);
  uint32_t id_a2 = ptk_anm2_item_insert_value(doc, sel_a, "A2", "a2", &err);
  uint32_t id_b1 = ptk_anm2_item_insert_value(doc, sel_b, "B1", "b1", &err);
  uint32_t id_b2 = ptk_anm2_item_insert_value(doc, sel_b, "B2", "b2", &err);
  TEST_ASSERT_SUCCEEDED(id_a1 != 0, &err);
  TEST_ASSERT_SUCCEEDED(id_a2 != 0, &err);
  TEST_ASSERT_SUCCEEDED(id_b1 != 0, &err);
  TEST_ASSERT_SUCCEEDED(id_b2 != 0, &err);

  TEST_ASSERT_SUCCEEDED(anm2_selection_apply_treeview_selection(sel, id_a1, false, false, false, &err), &err);
  TEST_ASSERT_SUCCEEDED(anm2_selection_apply_treeview_selection(sel, id_b2, false, false, true, &err), &err);

  size_t count = 0;
  uint32_t const *ids = anm2_selection_get_selected_ids(sel, &count);
  TEST_CHECK(count == 4);
  TEST_CHECK(selection_contains(ids, count, id_a1));
  TEST_CHECK(selection_contains(ids, count, id_a2));
  TEST_CHECK(selection_contains(ids, count, id_b1));
  TEST_CHECK(selection_contains(ids, count, id_b2));

  anm2_selection_destroy(&sel);
  ptk_anm2_destroy(&doc);
}

static void test_selection_replace_selected_items(void) {
  struct ov_error err = {0};
  struct ptk_anm2 *doc = ptk_anm2_create(&err);
  struct anm2_selection *sel = NULL;
  uint32_t sel_id = 0;

  TEST_ASSERT_SUCCEEDED(doc != NULL, &err);
  sel = anm2_selection_create(doc, &err);
  TEST_ASSERT_SUCCEEDED(sel != NULL, &err);

  sel_id = ptk_anm2_selector_insert(doc, 0, "Group", &err);
  TEST_ASSERT_SUCCEEDED(sel_id != 0, &err);
  uint32_t id_a = ptk_anm2_item_insert_value(doc, sel_id, "A", "a", &err);
  uint32_t id_b = ptk_anm2_item_insert_value(doc, sel_id, "B", "b", &err);
  TEST_ASSERT_SUCCEEDED(id_a != 0, &err);
  TEST_ASSERT_SUCCEEDED(id_b != 0, &err);

  uint32_t ids_in[] = {id_a, id_b};
  TEST_ASSERT_SUCCEEDED(anm2_selection_replace_selected_items(sel, ids_in, 2, id_b, id_a, &err), &err);

  size_t count = 0;
  uint32_t const *ids = anm2_selection_get_selected_ids(sel, &count);
  TEST_CHECK(count == 2);
  TEST_CHECK(selection_contains(ids, count, id_a));
  TEST_CHECK(selection_contains(ids, count, id_b));

  struct anm2_selection_state state = {0};
  anm2_selection_get_state(sel, &state);
  TEST_CHECK(state.focus_type == anm2_selection_focus_item);
  TEST_CHECK(state.focus_id == id_b);
  TEST_CHECK(state.anchor_id == id_a);

  anm2_selection_destroy(&sel);
  ptk_anm2_destroy(&doc);
}

static void test_selection_refresh_invalid_anchor(void) {
  struct ov_error err = {0};
  struct ptk_anm2 *doc = ptk_anm2_create(&err);
  struct anm2_selection *sel = NULL;
  uint32_t sel_id = 0;

  TEST_ASSERT_SUCCEEDED(doc != NULL, &err);
  sel = anm2_selection_create(doc, &err);
  TEST_ASSERT_SUCCEEDED(sel != NULL, &err);

  sel_id = ptk_anm2_selector_insert(doc, 0, "Group", &err);
  TEST_ASSERT_SUCCEEDED(sel_id != 0, &err);
  uint32_t id_a = ptk_anm2_item_insert_value(doc, sel_id, "A", "a", &err);
  uint32_t id_b = ptk_anm2_item_insert_value(doc, sel_id, "B", "b", &err);
  TEST_ASSERT_SUCCEEDED(id_a != 0, &err);
  TEST_ASSERT_SUCCEEDED(id_b != 0, &err);

  uint32_t ids_in[] = {id_a, id_b};
  TEST_ASSERT_SUCCEEDED(anm2_selection_replace_selected_items(sel, ids_in, 2, id_b, id_a, &err), &err);

  TEST_ASSERT_SUCCEEDED(ptk_anm2_item_remove(doc, id_a, &err), &err);

  anm2_selection_refresh(sel);

  size_t count = 0;
  uint32_t const *ids = anm2_selection_get_selected_ids(sel, &count);
  TEST_CHECK(count == 1);
  TEST_CHECK(selection_contains(ids, count, id_b));

  struct anm2_selection_state state = {0};
  anm2_selection_get_state(sel, &state);
  TEST_CHECK(state.focus_type == anm2_selection_focus_item);
  TEST_CHECK(state.focus_id == id_b);
  TEST_CHECK(state.anchor_id == 0);

  anm2_selection_destroy(&sel);
  ptk_anm2_destroy(&doc);
}

static void test_selection_refresh_focus_removed(void) {
  struct ov_error err = {0};
  struct ptk_anm2 *doc = ptk_anm2_create(&err);
  struct anm2_selection *sel = NULL;
  uint32_t sel_id = 0;

  TEST_ASSERT_SUCCEEDED(doc != NULL, &err);
  sel = anm2_selection_create(doc, &err);
  TEST_ASSERT_SUCCEEDED(sel != NULL, &err);

  sel_id = ptk_anm2_selector_insert(doc, 0, "Group", &err);
  TEST_ASSERT_SUCCEEDED(sel_id != 0, &err);
  uint32_t id_a = ptk_anm2_item_insert_value(doc, sel_id, "A", "a", &err);
  TEST_ASSERT_SUCCEEDED(id_a != 0, &err);

  TEST_ASSERT_SUCCEEDED(anm2_selection_set_focus_item(sel, id_a, true, &err), &err);

  TEST_ASSERT_SUCCEEDED(ptk_anm2_item_remove(doc, id_a, &err), &err);

  anm2_selection_refresh(sel);

  struct anm2_selection_state state = {0};
  anm2_selection_get_state(sel, &state);
  TEST_CHECK(state.focus_type == anm2_selection_focus_none);
  TEST_CHECK(state.focus_id == 0);
  TEST_CHECK(state.anchor_id == 0);
  TEST_CHECK(anm2_selection_get_selected_count(sel) == 0);

  anm2_selection_destroy(&sel);
  ptk_anm2_destroy(&doc);
}

static void test_selection_refresh_selector_focus_removed(void) {
  struct ov_error err = {0};
  struct ptk_anm2 *doc = ptk_anm2_create(&err);
  struct anm2_selection *sel = NULL;

  TEST_ASSERT_SUCCEEDED(doc != NULL, &err);
  sel = anm2_selection_create(doc, &err);
  TEST_ASSERT_SUCCEEDED(sel != NULL, &err);

  uint32_t group_id = ptk_anm2_selector_insert(doc, 0, "Group", &err);
  TEST_ASSERT_SUCCEEDED(group_id != 0, &err);

  TEST_ASSERT_SUCCEEDED(anm2_selection_set_focus_selector(sel, group_id, &err), &err);

  struct anm2_selection_state state = {0};
  anm2_selection_get_state(sel, &state);
  TEST_CHECK(state.focus_type == anm2_selection_focus_selector);
  TEST_CHECK(state.focus_id == group_id);

  // Remove the selector
  TEST_ASSERT_SUCCEEDED(ptk_anm2_selector_remove(doc, group_id, &err), &err);
  anm2_selection_refresh(sel);

  // Focus should be cleared
  anm2_selection_get_state(sel, &state);
  TEST_CHECK(state.focus_type == anm2_selection_focus_none);
  TEST_CHECK(state.focus_id == 0);

  anm2_selection_destroy(&sel);
  ptk_anm2_destroy(&doc);
}

static void test_selection_multisel_partial_remove(void) {
  struct ov_error err = {0};
  struct ptk_anm2 *doc = ptk_anm2_create(&err);
  struct anm2_selection *sel = NULL;
  uint32_t sel_id = 0;

  TEST_ASSERT_SUCCEEDED(doc != NULL, &err);
  sel = anm2_selection_create(doc, &err);
  TEST_ASSERT_SUCCEEDED(sel != NULL, &err);

  sel_id = ptk_anm2_selector_insert(doc, 0, "Group", &err);
  TEST_ASSERT_SUCCEEDED(sel_id != 0, &err);
  uint32_t id_a = ptk_anm2_item_insert_value(doc, sel_id, "A", "a", &err);
  uint32_t id_b = ptk_anm2_item_insert_value(doc, sel_id, "B", "b", &err);
  uint32_t id_c = ptk_anm2_item_insert_value(doc, sel_id, "C", "c", &err);
  TEST_ASSERT_SUCCEEDED(id_a != 0, &err);
  TEST_ASSERT_SUCCEEDED(id_b != 0, &err);
  TEST_ASSERT_SUCCEEDED(id_c != 0, &err);

  // Select A, B, C with Ctrl
  TEST_ASSERT_SUCCEEDED(anm2_selection_apply_treeview_selection(sel, id_a, false, false, false, &err), &err);
  TEST_ASSERT_SUCCEEDED(anm2_selection_apply_treeview_selection(sel, id_b, false, true, false, &err), &err);
  TEST_ASSERT_SUCCEEDED(anm2_selection_apply_treeview_selection(sel, id_c, false, true, false, &err), &err);
  TEST_CHECK(anm2_selection_get_selected_count(sel) == 3);

  // Remove B from document
  TEST_ASSERT_SUCCEEDED(ptk_anm2_item_remove(doc, id_b, &err), &err);

  anm2_selection_refresh(sel);

  // Only A and C should remain selected
  size_t count = 0;
  uint32_t const *ids = anm2_selection_get_selected_ids(sel, &count);
  TEST_CHECK(count == 2);
  TEST_CHECK(selection_contains(ids, count, id_a));
  TEST_CHECK(selection_contains(ids, count, id_c));
  TEST_CHECK(!selection_contains(ids, count, id_b));

  // Focus should still be valid (id_c was the last clicked)
  struct anm2_selection_state state = {0};
  anm2_selection_get_state(sel, &state);
  TEST_CHECK(state.focus_type == anm2_selection_focus_item);
  TEST_CHECK(state.focus_id == id_c);

  anm2_selection_destroy(&sel);
  ptk_anm2_destroy(&doc);
}

TEST_LIST = {
    {"selection_create_destroy", test_selection_create_destroy},
    {"selection_set_focus_selector", test_selection_set_focus_selector},
    {"selection_set_focus_item_update_anchor", test_selection_set_focus_item_update_anchor},
    {"selection_apply_treeview_selection_basic", test_selection_apply_treeview_selection_basic},
    {"selection_apply_treeview_selector", test_selection_apply_treeview_selector},
    {"selection_apply_range_across_selectors", test_selection_apply_range_across_selectors},
    {"selection_replace_selected_items", test_selection_replace_selected_items},
    {"selection_refresh_invalid_anchor", test_selection_refresh_invalid_anchor},
    {"selection_refresh_focus_removed", test_selection_refresh_focus_removed},
    {"selection_refresh_selector_focus_removed", test_selection_refresh_selector_focus_removed},
    {"selection_multisel_partial_remove", test_selection_multisel_partial_remove},
    {NULL, NULL},
};
