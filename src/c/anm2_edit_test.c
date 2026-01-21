#include "anm2_edit.h"

#include "anm2.h"

#include <ovtest.h>

#include <string.h>

static bool selection_contains(uint32_t const *ids, size_t count, uint32_t id) {
  for (size_t i = 0; i < count; ++i) {
    if (ids[i] == id) {
      return true;
    }
  }
  return false;
}

// Test helper to get mutable doc pointer (bypasses const for testing)
static struct ptk_anm2 *get_doc(struct ptk_anm2_edit *edit) {
  return (struct ptk_anm2 *)ov_deconster_(ptk_anm2_edit_get_doc(edit));
}

static void test_edit_create_destroy(void) {
  struct ov_error err = {0};
  struct ptk_anm2_edit *edit = NULL;

  edit = ptk_anm2_edit_create(&err);
  TEST_ASSERT_SUCCEEDED(edit != NULL, &err);

  struct ptk_anm2_edit_state state = {0};
  ptk_anm2_edit_get_state(edit, &state);
  TEST_CHECK(state.focus_type == ptk_anm2_edit_focus_none);
  TEST_CHECK(state.focus_id == 0);
  TEST_CHECK(state.anchor_id == 0);
  TEST_CHECK(ptk_anm2_edit_get_selected_item_count(edit) == 0);

  ptk_anm2_edit_destroy(&edit);
  TEST_CHECK(edit == NULL);
}

static void test_selection_click(void) {
  struct ov_error err = {0};
  struct ptk_anm2_edit *edit = NULL;
  uint32_t sel_id = 0;
  uint32_t id_a = 0;
  uint32_t id_b = 0;

  edit = ptk_anm2_edit_create(&err);
  TEST_ASSERT_SUCCEEDED(edit != NULL, &err);
  struct ptk_anm2 *doc = get_doc(edit);
  TEST_ASSERT_SUCCEEDED(edit != NULL, &err);

  sel_id = ptk_anm2_selector_insert(doc, 0, "Group", &err);
  TEST_ASSERT_SUCCEEDED(sel_id != 0, &err);
  id_a = ptk_anm2_item_insert_value(doc, sel_id, "A", "a", &err);
  id_b = ptk_anm2_item_insert_value(doc, sel_id, "B", "b", &err);
  TEST_ASSERT_SUCCEEDED(id_a != 0, &err);
  TEST_ASSERT_SUCCEEDED(id_b != 0, &err);

  TEST_ASSERT_SUCCEEDED(ptk_anm2_edit_apply_treeview_selection(edit, id_a, false, false, false, &err), &err);
  size_t count = 0;
  uint32_t const *ids = ptk_anm2_edit_get_selected_item_ids(edit, &count);
  TEST_CHECK(count == 1);
  TEST_CHECK(ids && ids[0] == id_a);

  struct ptk_anm2_edit_state state = {0};
  ptk_anm2_edit_get_state(edit, &state);
  TEST_CHECK(state.focus_type == ptk_anm2_edit_focus_item);
  TEST_CHECK(state.focus_id == id_a);
  TEST_CHECK(state.anchor_id == id_a);

  TEST_ASSERT_SUCCEEDED(ptk_anm2_edit_apply_treeview_selection(edit, id_b, false, false, false, &err), &err);
  ids = ptk_anm2_edit_get_selected_item_ids(edit, &count);
  TEST_CHECK(count == 1);
  TEST_CHECK(ids && ids[0] == id_b);
  ptk_anm2_edit_get_state(edit, &state);
  TEST_CHECK(state.focus_id == id_b);
  TEST_CHECK(state.anchor_id == id_b);

  ptk_anm2_edit_destroy(&edit);
}

static void test_selection_ctrl_toggle(void) {
  struct ov_error err = {0};
  struct ptk_anm2_edit *edit = NULL;
  uint32_t sel_id = 0;
  uint32_t id_a = 0;
  uint32_t id_b = 0;

  edit = ptk_anm2_edit_create(&err);
  TEST_ASSERT_SUCCEEDED(edit != NULL, &err);
  struct ptk_anm2 *doc = get_doc(edit);
  TEST_ASSERT_SUCCEEDED(edit != NULL, &err);

  sel_id = ptk_anm2_selector_insert(doc, 0, "Group", &err);
  TEST_ASSERT_SUCCEEDED(sel_id != 0, &err);
  id_a = ptk_anm2_item_insert_value(doc, sel_id, "A", "a", &err);
  id_b = ptk_anm2_item_insert_value(doc, sel_id, "B", "b", &err);
  TEST_ASSERT_SUCCEEDED(id_a != 0, &err);
  TEST_ASSERT_SUCCEEDED(id_b != 0, &err);

  TEST_ASSERT_SUCCEEDED(ptk_anm2_edit_apply_treeview_selection(edit, id_a, false, false, false, &err), &err);
  TEST_ASSERT_SUCCEEDED(ptk_anm2_edit_apply_treeview_selection(edit, id_b, false, true, false, &err), &err);

  size_t count = 0;
  uint32_t const *ids = ptk_anm2_edit_get_selected_item_ids(edit, &count);
  TEST_CHECK(count == 2);
  TEST_CHECK(selection_contains(ids, count, id_a));
  TEST_CHECK(selection_contains(ids, count, id_b));

  struct ptk_anm2_edit_state state = {0};
  ptk_anm2_edit_get_state(edit, &state);
  TEST_CHECK(state.anchor_id == id_b);

  TEST_ASSERT_SUCCEEDED(ptk_anm2_edit_apply_treeview_selection(edit, id_a, false, true, false, &err), &err);
  ids = ptk_anm2_edit_get_selected_item_ids(edit, &count);
  TEST_CHECK(count == 1);
  TEST_CHECK(selection_contains(ids, count, id_b));

  ptk_anm2_edit_get_state(edit, &state);
  TEST_CHECK(state.focus_type == ptk_anm2_edit_focus_item);
  TEST_CHECK(state.focus_id == id_a);
  TEST_CHECK(state.anchor_id == id_b);

  ptk_anm2_edit_destroy(&edit);
}

static void test_selection_shift_range(void) {
  struct ov_error err = {0};
  struct ptk_anm2_edit *edit = NULL;
  uint32_t sel_id = 0;
  uint32_t id_a = 0;
  uint32_t id_b = 0;
  uint32_t id_c = 0;
  uint32_t id_d = 0;

  edit = ptk_anm2_edit_create(&err);
  TEST_ASSERT_SUCCEEDED(edit != NULL, &err);
  struct ptk_anm2 *doc = get_doc(edit);
  TEST_ASSERT_SUCCEEDED(edit != NULL, &err);

  sel_id = ptk_anm2_selector_insert(doc, 0, "Group", &err);
  TEST_ASSERT_SUCCEEDED(sel_id != 0, &err);
  id_a = ptk_anm2_item_insert_value(doc, sel_id, "A", "a", &err);
  id_b = ptk_anm2_item_insert_value(doc, sel_id, "B", "b", &err);
  id_c = ptk_anm2_item_insert_value(doc, sel_id, "C", "c", &err);
  id_d = ptk_anm2_item_insert_value(doc, sel_id, "D", "d", &err);
  TEST_ASSERT_SUCCEEDED(id_a != 0, &err);
  TEST_ASSERT_SUCCEEDED(id_b != 0, &err);
  TEST_ASSERT_SUCCEEDED(id_c != 0, &err);
  TEST_ASSERT_SUCCEEDED(id_d != 0, &err);

  TEST_ASSERT_SUCCEEDED(ptk_anm2_edit_apply_treeview_selection(edit, id_b, false, false, false, &err), &err);
  TEST_ASSERT_SUCCEEDED(ptk_anm2_edit_apply_treeview_selection(edit, id_d, false, false, true, &err), &err);

  size_t count = 0;
  uint32_t const *ids = ptk_anm2_edit_get_selected_item_ids(edit, &count);
  TEST_CHECK(count == 3);
  TEST_CHECK(selection_contains(ids, count, id_b));
  TEST_CHECK(selection_contains(ids, count, id_c));
  TEST_CHECK(selection_contains(ids, count, id_d));

  struct ptk_anm2_edit_state state = {0};
  ptk_anm2_edit_get_state(edit, &state);
  TEST_CHECK(state.focus_id == id_d);
  TEST_CHECK(state.anchor_id == id_b);

  ptk_anm2_edit_destroy(&edit);
}

static void test_selection_ctrl_selector(void) {
  struct ov_error err = {0};
  struct ptk_anm2_edit *edit = NULL;
  uint32_t group_id = 0;
  uint32_t id_a = 0;
  uint32_t id_b = 0;

  edit = ptk_anm2_edit_create(&err);
  TEST_ASSERT_SUCCEEDED(edit != NULL, &err);
  struct ptk_anm2 *doc = get_doc(edit);
  TEST_ASSERT_SUCCEEDED(edit != NULL, &err);

  group_id = ptk_anm2_selector_insert(doc, 0, "Group", &err);
  TEST_ASSERT_SUCCEEDED(group_id != 0, &err);
  id_a = ptk_anm2_item_insert_value(doc, group_id, "A", "a", &err);
  id_b = ptk_anm2_item_insert_value(doc, group_id, "B", "b", &err);
  TEST_ASSERT_SUCCEEDED(id_a != 0, &err);
  TEST_ASSERT_SUCCEEDED(id_b != 0, &err);

  TEST_ASSERT_SUCCEEDED(ptk_anm2_edit_apply_treeview_selection(edit, id_a, false, false, false, &err), &err);
  TEST_ASSERT_SUCCEEDED(ptk_anm2_edit_apply_treeview_selection(edit, id_b, false, true, false, &err), &err);

  TEST_ASSERT_SUCCEEDED(ptk_anm2_edit_apply_treeview_selection(edit, group_id, true, true, false, &err), &err);

  size_t count = 0;
  uint32_t const *ids = ptk_anm2_edit_get_selected_item_ids(edit, &count);
  TEST_CHECK(count == 2);
  TEST_CHECK(selection_contains(ids, count, id_a));
  TEST_CHECK(selection_contains(ids, count, id_b));

  struct ptk_anm2_edit_state state = {0};
  ptk_anm2_edit_get_state(edit, &state);
  TEST_CHECK(state.focus_type == ptk_anm2_edit_focus_selector);
  TEST_CHECK(state.focus_id == group_id);
  TEST_CHECK(state.anchor_id == id_b);

  TEST_ASSERT_SUCCEEDED(ptk_anm2_edit_apply_treeview_selection(edit, group_id, true, false, false, &err), &err);
  ids = ptk_anm2_edit_get_selected_item_ids(edit, &count);
  TEST_CHECK(count == 0);
  ptk_anm2_edit_get_state(edit, &state);
  TEST_CHECK(state.focus_type == ptk_anm2_edit_focus_selector);
  TEST_CHECK(state.anchor_id == 0);

  ptk_anm2_edit_destroy(&edit);
}

static void test_edit_selector_ops(void) {
  struct ov_error err = {0};
  struct ptk_anm2_edit *edit = NULL;

  edit = ptk_anm2_edit_create(&err);
  TEST_ASSERT_SUCCEEDED(edit != NULL, &err);
  struct ptk_anm2 *doc = get_doc(edit);
  TEST_ASSERT_SUCCEEDED(edit != NULL, &err);

  TEST_ASSERT_SUCCEEDED(ptk_anm2_edit_add_selector(edit, "A", &err), &err);
  TEST_ASSERT_SUCCEEDED(ptk_anm2_edit_add_selector(edit, "B", &err), &err);
  TEST_ASSERT_SUCCEEDED(ptk_anm2_edit_add_selector(edit, "C", &err), &err);

  uint32_t const id_a = ptk_anm2_edit_selector_get_id(edit, 0);
  uint32_t const id_b = ptk_anm2_edit_selector_get_id(edit, 1);
  uint32_t const id_c = ptk_anm2_edit_selector_get_id(edit, 2);

  TEST_ASSERT_SUCCEEDED(ptk_anm2_edit_rename_selector(edit, id_b, "B2", &err), &err);
  TEST_CHECK(strcmp(ptk_anm2_selector_get_name(doc, id_b), "B2") == 0);

  TEST_ASSERT_SUCCEEDED(ptk_anm2_edit_move_selector(edit, id_a, id_c, false, &err), &err);
  TEST_CHECK(strcmp(ptk_anm2_selector_get_name(doc, id_b), "B2") == 0);
  TEST_CHECK(strcmp(ptk_anm2_selector_get_name(doc, id_c), "C") == 0);
  TEST_CHECK(strcmp(ptk_anm2_selector_get_name(doc, id_a), "A") == 0);

  ptk_anm2_edit_destroy(&edit);
}

static void test_edit_item_rename_value(void) {
  struct ov_error err = {0};
  struct ptk_anm2_edit *edit = NULL;
  uint32_t sel_id = 0;
  uint32_t id_a = 0;

  edit = ptk_anm2_edit_create(&err);
  TEST_ASSERT_SUCCEEDED(edit != NULL, &err);
  struct ptk_anm2 *doc = get_doc(edit);
  TEST_ASSERT_SUCCEEDED(edit != NULL, &err);

  sel_id = ptk_anm2_selector_insert(doc, 0, "Group", &err);
  TEST_ASSERT_SUCCEEDED(sel_id != 0, &err);
  id_a = ptk_anm2_item_insert_value(doc, sel_id, "A", "a", &err);
  TEST_ASSERT_SUCCEEDED(id_a != 0, &err);

  TEST_ASSERT_SUCCEEDED(ptk_anm2_edit_rename_item(edit, id_a, "A2", &err), &err);
  TEST_ASSERT_SUCCEEDED(ptk_anm2_edit_set_item_value(edit, id_a, "a2", &err), &err);

  {
    uint32_t const item_id = ptk_anm2_item_get_id(doc, 0, 0);
    TEST_CHECK(strcmp(ptk_anm2_item_get_name(doc, item_id), "A2") == 0);
    TEST_CHECK(strcmp(ptk_anm2_item_get_value(doc, item_id), "a2") == 0);
  }

  ptk_anm2_edit_destroy(&edit);
}

static void test_edit_multisel_detail_updates(void) {
  struct ov_error err = {0};
  struct ptk_anm2_edit *edit = NULL;
  uint32_t sel_id = 0;
  uint32_t id_a = 0;
  uint32_t id_b = 0;

  edit = ptk_anm2_edit_create(&err);
  TEST_ASSERT_SUCCEEDED(edit != NULL, &err);
  struct ptk_anm2 *doc = get_doc(edit);
  TEST_ASSERT_SUCCEEDED(edit != NULL, &err);

  sel_id = ptk_anm2_selector_insert(doc, 0, "Group", &err);
  TEST_ASSERT_SUCCEEDED(sel_id != 0, &err);
  id_a = ptk_anm2_item_insert_value(doc, sel_id, "A", "a", &err);
  id_b = ptk_anm2_item_insert_value(doc, sel_id, "B", "b", &err);
  TEST_ASSERT_SUCCEEDED(id_a != 0, &err);
  TEST_ASSERT_SUCCEEDED(id_b != 0, &err);

  TEST_ASSERT_SUCCEEDED(ptk_anm2_edit_apply_treeview_selection(edit, id_a, false, false, false, &err), &err);
  TEST_ASSERT_SUCCEEDED(ptk_anm2_edit_apply_treeview_selection(edit, id_b, false, true, false, &err), &err);

  TEST_ASSERT_SUCCEEDED(ptk_anm2_edit_rename_item(edit, id_a, "A2", &err), &err);
  TEST_ASSERT_SUCCEEDED(ptk_anm2_edit_set_item_value(edit, id_b, "b2", &err), &err);
  {
    uint32_t const item_id = ptk_anm2_item_get_id(doc, 0, 0);
    TEST_CHECK(strcmp(ptk_anm2_item_get_name(doc, item_id), "A2") == 0);
  }
  {
    uint32_t const item_id = ptk_anm2_item_get_id(doc, 0, 1);
    TEST_CHECK(strcmp(ptk_anm2_item_get_value(doc, item_id), "b2") == 0);
  }

  size_t count = 0;
  uint32_t const *ids = ptk_anm2_edit_get_selected_item_ids(edit, &count);
  TEST_CHECK(count == 2);
  TEST_CHECK(selection_contains(ids, count, id_a));
  TEST_CHECK(selection_contains(ids, count, id_b));

  struct ptk_anm2_edit_state state = {0};
  ptk_anm2_edit_get_state(edit, &state);
  TEST_CHECK(state.focus_type == ptk_anm2_edit_focus_item);
  TEST_CHECK(state.focus_id == id_b);
  TEST_CHECK(state.anchor_id == id_b);

  ptk_anm2_edit_destroy(&edit);
}

static void test_edit_delete_selected(void) {
  struct ov_error err = {0};
  struct ptk_anm2_edit *edit = NULL;
  uint32_t group_id = 0;
  uint32_t id_a = 0;
  uint32_t id_b = 0;

  edit = ptk_anm2_edit_create(&err);
  TEST_ASSERT_SUCCEEDED(edit != NULL, &err);
  struct ptk_anm2 *doc = get_doc(edit);
  TEST_ASSERT_SUCCEEDED(edit != NULL, &err);

  group_id = ptk_anm2_selector_insert(doc, 0, "Group", &err);
  TEST_ASSERT_SUCCEEDED(group_id != 0, &err);
  id_a = ptk_anm2_item_insert_value(doc, group_id, "A", "a", &err);
  id_b = ptk_anm2_item_insert_value(doc, group_id, "B", "b", &err);
  TEST_ASSERT_SUCCEEDED(id_a != 0, &err);
  TEST_ASSERT_SUCCEEDED(id_b != 0, &err);

  TEST_ASSERT_SUCCEEDED(ptk_anm2_edit_apply_treeview_selection(edit, id_a, false, false, false, &err), &err);
  TEST_ASSERT_SUCCEEDED(ptk_anm2_edit_apply_treeview_selection(edit, id_b, false, true, false, &err), &err);
  TEST_ASSERT_SUCCEEDED(ptk_anm2_edit_apply_treeview_selection(edit, group_id, true, true, false, &err), &err);
  TEST_ASSERT_SUCCEEDED(ptk_anm2_edit_delete_selected(edit, &err), &err);
  TEST_CHECK(ptk_anm2_selector_count(doc) == 1);
  TEST_CHECK(ptk_anm2_item_count(doc, group_id) == 0);

  TEST_ASSERT_SUCCEEDED(ptk_anm2_edit_apply_treeview_selection(edit, group_id, true, false, false, &err), &err);
  TEST_ASSERT_SUCCEEDED(ptk_anm2_edit_delete_selected(edit, &err), &err);
  TEST_CHECK(ptk_anm2_selector_count(doc) == 0);

  ptk_anm2_edit_destroy(&edit);
}

static void test_edit_reverse_focus_selector(void) {
  struct ov_error err = {0};
  struct ptk_anm2_edit *edit = NULL;
  uint32_t group_id = 0;
  uint32_t id_a = 0;
  uint32_t id_b = 0;
  uint32_t id_c = 0;

  edit = ptk_anm2_edit_create(&err);
  TEST_ASSERT_SUCCEEDED(edit != NULL, &err);
  struct ptk_anm2 *doc = get_doc(edit);
  TEST_ASSERT_SUCCEEDED(edit != NULL, &err);

  group_id = ptk_anm2_selector_insert(doc, 0, "Group", &err);
  TEST_ASSERT_SUCCEEDED(group_id != 0, &err);
  id_a = ptk_anm2_item_insert_value(doc, group_id, "A", "a", &err);
  id_b = ptk_anm2_item_insert_value(doc, group_id, "B", "b", &err);
  id_c = ptk_anm2_item_insert_value(doc, group_id, "C", "c", &err);
  TEST_ASSERT_SUCCEEDED(id_a != 0, &err);
  TEST_ASSERT_SUCCEEDED(id_b != 0, &err);
  TEST_ASSERT_SUCCEEDED(id_c != 0, &err);

  TEST_ASSERT_SUCCEEDED(ptk_anm2_edit_apply_treeview_selection(edit, id_b, false, false, false, &err), &err);
  TEST_ASSERT_SUCCEEDED(ptk_anm2_edit_reverse_focus_selector(edit, &err), &err);

  TEST_CHECK(ptk_anm2_item_get_id(doc, 0, 0) == id_c);
  TEST_CHECK(ptk_anm2_item_get_id(doc, 0, 1) == id_b);
  TEST_CHECK(ptk_anm2_item_get_id(doc, 0, 2) == id_a);

  ptk_anm2_edit_destroy(&edit);
}

static void test_edit_move_items_order(void) {
  struct ov_error err = {0};
  struct ptk_anm2_edit *edit = NULL;
  uint32_t group_a = 0;
  uint32_t group_b = 0;
  uint32_t id_a = 0;
  uint32_t id_b = 0;
  uint32_t id_c = 0;
  uint32_t id_d = 0;

  edit = ptk_anm2_edit_create(&err);
  TEST_ASSERT_SUCCEEDED(edit != NULL, &err);
  struct ptk_anm2 *doc = get_doc(edit);
  TEST_ASSERT_SUCCEEDED(edit != NULL, &err);

  group_a = ptk_anm2_selector_insert(doc, 0, "A", &err);
  group_b = ptk_anm2_selector_insert(doc, 0, "B", &err);
  TEST_ASSERT_SUCCEEDED(group_a != 0, &err);
  TEST_ASSERT_SUCCEEDED(group_b != 0, &err);

  id_a = ptk_anm2_item_insert_value(doc, group_a, "A1", "a1", &err);
  id_b = ptk_anm2_item_insert_value(doc, group_a, "A2", "a2", &err);
  id_c = ptk_anm2_item_insert_value(doc, group_a, "A3", "a3", &err);
  id_d = ptk_anm2_item_insert_value(doc, group_b, "B1", "b1", &err);
  TEST_ASSERT_SUCCEEDED(id_a != 0, &err);
  TEST_ASSERT_SUCCEEDED(id_b != 0, &err);
  TEST_ASSERT_SUCCEEDED(id_c != 0, &err);
  TEST_ASSERT_SUCCEEDED(id_d != 0, &err);

  uint32_t move_ids[2] = {id_c, id_a};
  TEST_ASSERT_SUCCEEDED(ptk_anm2_edit_move_items(edit, move_ids, 2, group_b, true, false, &err), &err);

  TEST_CHECK(ptk_anm2_item_count(doc, group_a) == 1);
  TEST_CHECK(ptk_anm2_item_get_id(doc, 0, 0) == id_b);
  TEST_CHECK(ptk_anm2_item_count(doc, group_b) == 3);
  TEST_CHECK(ptk_anm2_item_get_id(doc, 1, 0) == id_d);
  TEST_CHECK(ptk_anm2_item_get_id(doc, 1, 1) == id_a);
  TEST_CHECK(ptk_anm2_item_get_id(doc, 1, 2) == id_c);

  size_t count = 0;
  uint32_t const *ids = ptk_anm2_edit_get_selected_item_ids(edit, &count);
  TEST_CHECK(count == 2);
  TEST_CHECK(ids && ids[0] == move_ids[0]);
  TEST_CHECK(ids && ids[1] == move_ids[1]);
  struct ptk_anm2_edit_state state = {0};
  ptk_anm2_edit_get_state(edit, &state);
  TEST_CHECK(state.focus_type == ptk_anm2_edit_focus_item);
  TEST_CHECK(state.focus_id == move_ids[0]);
  TEST_CHECK(state.anchor_id == move_ids[0]);

  ptk_anm2_edit_destroy(&edit);
}

static void test_edit_param_ops(void) {
  struct ov_error err = {0};
  struct ptk_anm2_edit *edit = NULL;

  edit = ptk_anm2_edit_create(&err);
  TEST_ASSERT_SUCCEEDED(edit != NULL, &err);
  struct ptk_anm2 *doc = get_doc(edit);
  TEST_ASSERT_SUCCEEDED(edit != NULL, &err);

  uint32_t group_id = ptk_anm2_selector_insert(doc, 0, "Group", &err);
  TEST_ASSERT_SUCCEEDED(group_id != 0, &err);

  // Create animation item using insert API (at end of group)
  TEST_ASSERT_SUCCEEDED(ptk_anm2_edit_insert_animation_item(edit, group_id, "Script", "Display", &err), &err);
  uint32_t item_id = ptk_anm2_item_get_id(doc, 0, 0);
  TEST_ASSERT(item_id != 0);

  TEST_ASSERT_SUCCEEDED(ptk_anm2_edit_apply_treeview_selection(edit, item_id, false, false, false, &err), &err);

  TEST_ASSERT_SUCCEEDED(ptk_anm2_edit_param_add_for_focus(edit, "", &err), &err);
  size_t sel_idx = 0;
  size_t item_idx = 0;
  TEST_CHECK(ptk_anm2_find_item(doc, item_id, &sel_idx, &item_idx));
  TEST_CHECK(ptk_anm2_param_count(doc, item_id) == 0);

  TEST_ASSERT_SUCCEEDED(ptk_anm2_edit_param_add_for_focus(edit, "Key", &err), &err);
  TEST_CHECK(ptk_anm2_param_count(doc, item_id) == 1);
  uint32_t param_id = 0;
  {
    param_id = ptk_anm2_param_get_id(doc, sel_idx, item_idx, 0);
    TEST_CHECK(strcmp(ptk_anm2_param_get_key(doc, param_id), "Key") == 0);
  }

  TEST_ASSERT_SUCCEEDED(ptk_anm2_edit_param_set_key(edit, param_id, "Key2", &err), &err);
  TEST_ASSERT_SUCCEEDED(ptk_anm2_edit_param_set_value(edit, param_id, "Value", &err), &err);
  {
    TEST_CHECK(strcmp(ptk_anm2_param_get_key(doc, param_id), "Key2") == 0);
    TEST_CHECK(strcmp(ptk_anm2_param_get_value(doc, param_id), "Value") == 0);
  }

  TEST_FAILED_WITH(
      ptk_anm2_edit_param_remove(edit, 9999, &err), &err, ov_error_type_generic, ov_error_generic_invalid_argument);
  TEST_ASSERT_SUCCEEDED(ptk_anm2_edit_param_remove(edit, param_id, &err), &err);
  TEST_CHECK(ptk_anm2_param_count(doc, item_id) == 0);

  ptk_anm2_edit_destroy(&edit);
}

static void test_edit_document_props(void) {
  struct ov_error err = {0};
  struct ptk_anm2_edit *edit = NULL;

  edit = ptk_anm2_edit_create(&err);
  TEST_ASSERT_SUCCEEDED(edit != NULL, &err);
  struct ptk_anm2 *doc = get_doc(edit);
  TEST_ASSERT_SUCCEEDED(edit != NULL, &err);

  TEST_ASSERT_SUCCEEDED(ptk_anm2_edit_set_label(edit, "Label", &err), &err);
  TEST_CHECK(strcmp(ptk_anm2_get_label(doc), "Label") == 0);

  TEST_ASSERT_SUCCEEDED(ptk_anm2_edit_set_psd_path(edit, "path.psd", &err), &err);
  TEST_CHECK(strcmp(ptk_anm2_get_psd_path(doc), "path.psd") == 0);

  TEST_ASSERT_SUCCEEDED(ptk_anm2_edit_set_exclusive_support_default(edit, false, &err), &err);
  TEST_CHECK(!ptk_anm2_get_exclusive_support_default(doc));

  TEST_ASSERT_SUCCEEDED(ptk_anm2_edit_set_information(edit, "Info", &err), &err);
  TEST_CHECK(strcmp(ptk_anm2_get_information(doc), "Info") == 0);
  TEST_ASSERT_SUCCEEDED(ptk_anm2_edit_set_information(edit, "", &err), &err);
  TEST_CHECK(ptk_anm2_get_information(doc) == NULL);

  ptk_anm2_edit_destroy(&edit);
}

static void test_edit_update_on_doc_op(void) {
  struct ov_error err = {0};
  struct ptk_anm2_edit *edit = NULL;
  uint32_t sel_id = 0;
  uint32_t item_id = 0;
  uint32_t item_id2 = 0;
  uint32_t item_id3 = 0;

  edit = ptk_anm2_edit_create(&err);
  TEST_ASSERT_SUCCEEDED(edit != NULL, &err);
  struct ptk_anm2 *doc = get_doc(edit);
  TEST_ASSERT_SUCCEEDED(edit != NULL, &err);

  sel_id = ptk_anm2_selector_insert(doc, 0, "Group", &err);
  TEST_ASSERT_SUCCEEDED(sel_id != 0, &err);
  item_id = ptk_anm2_item_insert_value(doc, sel_id, "A", "a", &err);
  TEST_ASSERT_SUCCEEDED(item_id != 0, &err);

  TEST_ASSERT_SUCCEEDED(ptk_anm2_edit_apply_treeview_selection(edit, item_id, false, false, false, &err), &err);
  TEST_ASSERT_SUCCEEDED(ptk_anm2_item_remove(doc, item_id, &err), &err);
  ptk_anm2_edit_update_on_doc_op(edit, ptk_anm2_op_item_remove, 0, 0, 0);

  struct ptk_anm2_edit_state state = {0};
  ptk_anm2_edit_get_state(edit, &state);
  TEST_CHECK(state.focus_type == ptk_anm2_edit_focus_none);
  TEST_CHECK(ptk_anm2_edit_get_selected_item_count(edit) == 0);

  item_id2 = ptk_anm2_item_insert_value(doc, sel_id, "B", "b", &err);
  TEST_ASSERT_SUCCEEDED(item_id2 != 0, &err);
  TEST_ASSERT_SUCCEEDED(ptk_anm2_edit_apply_treeview_selection(edit, item_id2, false, false, false, &err), &err);
  ptk_anm2_edit_update_on_doc_op(edit, ptk_anm2_op_reset, 0, 0, 0);
  ptk_anm2_edit_get_state(edit, &state);
  TEST_CHECK(state.focus_type == ptk_anm2_edit_focus_none);
  TEST_CHECK(ptk_anm2_edit_get_selected_item_count(edit) == 0);

  item_id3 = ptk_anm2_item_insert_value(doc, sel_id, "C", "c", &err);
  TEST_ASSERT_SUCCEEDED(item_id3 != 0, &err);
  TEST_ASSERT_SUCCEEDED(ptk_anm2_edit_apply_treeview_selection(edit, item_id3, false, false, false, &err), &err);

  TEST_ASSERT_SUCCEEDED(ptk_anm2_item_set_value(doc, item_id3, "c2", &err), &err);
  ptk_anm2_edit_update_on_doc_op(edit, ptk_anm2_op_item_set_value, item_id3, 0, 0);

  ptk_anm2_edit_get_state(edit, &state);
  TEST_CHECK(state.focus_type == ptk_anm2_edit_focus_item);
  TEST_CHECK(state.focus_id == item_id3);
  TEST_CHECK(state.anchor_id == item_id3);
  TEST_CHECK(ptk_anm2_edit_get_selected_item_count(edit) == 1);

  ptk_anm2_edit_destroy(&edit);
}

static void test_update_on_doc_op_set_operations(void) {
  struct ov_error err = {0};
  struct ptk_anm2_edit *edit = NULL;
  uint32_t group_id = 0;
  uint32_t id_a = 0;
  uint32_t id_b = 0;

  edit = ptk_anm2_edit_create(&err);
  TEST_ASSERT_SUCCEEDED(edit != NULL, &err);
  struct ptk_anm2 *doc = get_doc(edit);
  TEST_ASSERT_SUCCEEDED(edit != NULL, &err);

  group_id = ptk_anm2_selector_insert(doc, 0, "Group", &err);
  TEST_ASSERT_SUCCEEDED(group_id != 0, &err);
  id_a = ptk_anm2_item_insert_value(doc, group_id, "A", "a", &err);
  id_b = ptk_anm2_item_insert_value(doc, group_id, "B", "b", &err);
  TEST_ASSERT_SUCCEEDED(id_a != 0, &err);
  TEST_ASSERT_SUCCEEDED(id_b != 0, &err);

  TEST_ASSERT_SUCCEEDED(ptk_anm2_edit_apply_treeview_selection(edit, id_a, false, false, false, &err), &err);
  TEST_ASSERT_SUCCEEDED(ptk_anm2_edit_apply_treeview_selection(edit, id_b, false, true, false, &err), &err);

  // set_label should preserve selection
  TEST_ASSERT_SUCCEEDED(ptk_anm2_set_label(doc, "NewLabel", &err), &err);
  ptk_anm2_edit_update_on_doc_op(edit, ptk_anm2_op_set_label, 0, 0, 0);

  size_t count = 0;
  uint32_t const *ids = ptk_anm2_edit_get_selected_item_ids(edit, &count);
  TEST_CHECK(count == 2);
  TEST_CHECK(selection_contains(ids, count, id_a));
  TEST_CHECK(selection_contains(ids, count, id_b));

  // set_psd_path should preserve selection
  TEST_ASSERT_SUCCEEDED(ptk_anm2_set_psd_path(doc, "test.psd", &err), &err);
  ptk_anm2_edit_update_on_doc_op(edit, ptk_anm2_op_set_psd_path, 0, 0, 0);
  TEST_CHECK(ptk_anm2_edit_get_selected_item_count(edit) == 2);

  // set_exclusive should preserve selection
  TEST_ASSERT_SUCCEEDED(ptk_anm2_set_exclusive_support_default(doc, false, &err), &err);
  ptk_anm2_edit_update_on_doc_op(edit, ptk_anm2_op_set_exclusive_support_default, 0, 0, 0);
  TEST_CHECK(ptk_anm2_edit_get_selected_item_count(edit) == 2);

  // set_information should preserve selection
  TEST_ASSERT_SUCCEEDED(ptk_anm2_set_information(doc, "Info", &err), &err);
  ptk_anm2_edit_update_on_doc_op(edit, ptk_anm2_op_set_information, 0, 0, 0);
  TEST_CHECK(ptk_anm2_edit_get_selected_item_count(edit) == 2);

  // item_set_name should preserve selection
  TEST_ASSERT_SUCCEEDED(ptk_anm2_item_set_name(doc, id_a, "A2", &err), &err);
  ptk_anm2_edit_update_on_doc_op(edit, ptk_anm2_op_item_set_name, 0, 0, 0);
  TEST_CHECK(ptk_anm2_edit_get_selected_item_count(edit) == 2);

  // selector_set_group should preserve selection
  TEST_ASSERT_SUCCEEDED(ptk_anm2_selector_set_name(doc, group_id, "Group2", &err), &err);
  ptk_anm2_edit_update_on_doc_op(edit, ptk_anm2_op_selector_set_name, 0, 0, 0);
  TEST_CHECK(ptk_anm2_edit_get_selected_item_count(edit) == 2);

  struct ptk_anm2_edit_state state = {0};
  ptk_anm2_edit_get_state(edit, &state);
  TEST_CHECK(state.focus_type == ptk_anm2_edit_focus_item);
  TEST_CHECK(state.focus_id == id_b);
  TEST_CHECK(state.anchor_id == id_b);

  ptk_anm2_edit_destroy(&edit);
}

static void test_update_on_doc_op_insert_operations(void) {
  struct ov_error err = {0};
  struct ptk_anm2_edit *edit = NULL;
  uint32_t sel1_id = 0;
  uint32_t sel2_id = 0;
  uint32_t id_a = 0;
  uint32_t id_b = 0;

  edit = ptk_anm2_edit_create(&err);
  TEST_ASSERT_SUCCEEDED(edit != NULL, &err);
  struct ptk_anm2 *doc = get_doc(edit);
  TEST_ASSERT_SUCCEEDED(edit != NULL, &err);

  sel1_id = ptk_anm2_selector_insert(doc, 0, "Group1", &err);
  TEST_ASSERT_SUCCEEDED(sel1_id != 0, &err);
  id_a = ptk_anm2_item_insert_value(doc, sel1_id, "A", "a", &err);
  TEST_ASSERT_SUCCEEDED(id_a != 0, &err);

  TEST_ASSERT_SUCCEEDED(ptk_anm2_edit_apply_treeview_selection(edit, id_a, false, false, false, &err), &err);

  // selector_insert should preserve selection
  sel2_id = ptk_anm2_selector_insert(doc, 0, "Group2", &err);
  TEST_ASSERT_SUCCEEDED(sel2_id != 0, &err);
  ptk_anm2_edit_update_on_doc_op(edit, ptk_anm2_op_selector_insert, sel2_id, 0, 1);

  struct ptk_anm2_edit_state state = {0};
  ptk_anm2_edit_get_state(edit, &state);
  TEST_CHECK(state.focus_type == ptk_anm2_edit_focus_item);
  TEST_CHECK(state.focus_id == id_a);
  TEST_CHECK(ptk_anm2_edit_get_selected_item_count(edit) == 1);

  // item_insert should preserve selection
  id_b = ptk_anm2_item_insert_value(doc, sel1_id, "B", "b", &err);
  TEST_ASSERT_SUCCEEDED(id_b != 0, &err);
  ptk_anm2_edit_update_on_doc_op(edit, ptk_anm2_op_item_insert, id_b, sel1_id, 1);

  ptk_anm2_edit_get_state(edit, &state);
  TEST_CHECK(state.focus_type == ptk_anm2_edit_focus_item);
  TEST_CHECK(state.focus_id == id_a);
  TEST_CHECK(ptk_anm2_edit_get_selected_item_count(edit) == 1);
  TEST_CHECK(ptk_anm2_edit_is_item_selected(edit, id_a));
  TEST_CHECK(!ptk_anm2_edit_is_item_selected(edit, id_b));

  ptk_anm2_edit_destroy(&edit);
}

static void test_update_on_doc_op_move_operations(void) {
  struct ov_error err = {0};
  struct ptk_anm2_edit *edit = NULL;
  uint32_t sel1_id = 0;
  uint32_t sel2_id = 0;
  uint32_t id_a = 0;
  uint32_t id_b = 0;
  uint32_t id_c = 0;

  edit = ptk_anm2_edit_create(&err);
  TEST_ASSERT_SUCCEEDED(edit != NULL, &err);
  struct ptk_anm2 *doc = get_doc(edit);
  TEST_ASSERT_SUCCEEDED(edit != NULL, &err);

  sel1_id = ptk_anm2_selector_insert(doc, 0, "Group1", &err);
  sel2_id = ptk_anm2_selector_insert(doc, 0, "Group2", &err);
  TEST_ASSERT_SUCCEEDED(sel1_id != 0, &err);
  TEST_ASSERT_SUCCEEDED(sel2_id != 0, &err);
  id_a = ptk_anm2_item_insert_value(doc, sel1_id, "A", "a", &err);
  id_b = ptk_anm2_item_insert_value(doc, sel1_id, "B", "b", &err);
  id_c = ptk_anm2_item_insert_value(doc, sel1_id, "C", "c", &err);
  TEST_ASSERT_SUCCEEDED(id_a != 0, &err);
  TEST_ASSERT_SUCCEEDED(id_b != 0, &err);
  TEST_ASSERT_SUCCEEDED(id_c != 0, &err);

  TEST_ASSERT_SUCCEEDED(ptk_anm2_edit_apply_treeview_selection(edit, id_a, false, false, false, &err), &err);
  TEST_ASSERT_SUCCEEDED(ptk_anm2_edit_apply_treeview_selection(edit, id_c, false, true, false, &err), &err);

  // selector_move should preserve selection (IDs are stable)
  // Move sel1 to end (after sel2, resulting in order: sel2, sel1)
  TEST_ASSERT_SUCCEEDED(ptk_anm2_selector_move(doc, sel1_id, 0, &err), &err);
  ptk_anm2_edit_update_on_doc_op(edit, ptk_anm2_op_selector_move, sel1_id, 0, 1);

  size_t count = 0;
  uint32_t const *ids = ptk_anm2_edit_get_selected_item_ids(edit, &count);
  TEST_CHECK(count == 2);
  TEST_CHECK(selection_contains(ids, count, id_a));
  TEST_CHECK(selection_contains(ids, count, id_c));

  // item_move should preserve selection (IDs are stable)
  // After selector move, Group1 is now at index 1. Move item A to end within sel1
  TEST_ASSERT_SUCCEEDED(ptk_anm2_item_move(doc, id_a, sel1_id, &err), &err);
  ptk_anm2_edit_update_on_doc_op(edit, ptk_anm2_op_item_move, id_a, sel1_id, 2);

  ids = ptk_anm2_edit_get_selected_item_ids(edit, &count);
  TEST_CHECK(count == 2);
  TEST_CHECK(selection_contains(ids, count, id_a));
  TEST_CHECK(selection_contains(ids, count, id_c));

  struct ptk_anm2_edit_state state = {0};
  ptk_anm2_edit_get_state(edit, &state);
  TEST_CHECK(state.focus_type == ptk_anm2_edit_focus_item);
  TEST_CHECK(state.focus_id == id_c);

  ptk_anm2_edit_destroy(&edit);
}

static void test_update_on_doc_op_remove_selector(void) {
  struct ov_error err = {0};
  struct ptk_anm2_edit *edit = NULL;
  uint32_t group1 = 0;
  uint32_t group2 = 0;
  uint32_t id_a = 0;
  uint32_t id_b = 0;

  edit = ptk_anm2_edit_create(&err);
  TEST_ASSERT_SUCCEEDED(edit != NULL, &err);
  struct ptk_anm2 *doc = get_doc(edit);
  TEST_ASSERT_SUCCEEDED(edit != NULL, &err);

  group1 = ptk_anm2_selector_insert(doc, 0, "Group1", &err);
  group2 = ptk_anm2_selector_insert(doc, 0, "Group2", &err);
  TEST_ASSERT_SUCCEEDED(group1 != 0, &err);
  TEST_ASSERT_SUCCEEDED(group2 != 0, &err);
  id_a = ptk_anm2_item_insert_value(doc, group1, "A", "a", &err);
  id_b = ptk_anm2_item_insert_value(doc, group2, "B", "b", &err);
  TEST_ASSERT_SUCCEEDED(id_a != 0, &err);
  TEST_ASSERT_SUCCEEDED(id_b != 0, &err);

  // Select item from group1 and item from group2
  TEST_ASSERT_SUCCEEDED(ptk_anm2_edit_apply_treeview_selection(edit, id_a, false, false, false, &err), &err);
  TEST_ASSERT_SUCCEEDED(ptk_anm2_edit_apply_treeview_selection(edit, id_b, false, true, false, &err), &err);
  TEST_CHECK(ptk_anm2_edit_get_selected_item_count(edit) == 2);

  // Remove group1 - should trigger refresh and remove id_a from selection
  TEST_ASSERT_SUCCEEDED(ptk_anm2_selector_remove(doc, group1, &err), &err);
  ptk_anm2_edit_update_on_doc_op(edit, ptk_anm2_op_selector_remove, group1, 0, 0);

  size_t count = 0;
  uint32_t const *ids = ptk_anm2_edit_get_selected_item_ids(edit, &count);
  TEST_CHECK(count == 1);
  TEST_CHECK(selection_contains(ids, count, id_b));

  struct ptk_anm2_edit_state state = {0};
  ptk_anm2_edit_get_state(edit, &state);
  TEST_CHECK(state.focus_type == ptk_anm2_edit_focus_item);
  TEST_CHECK(state.focus_id == id_b);

  ptk_anm2_edit_destroy(&edit);
}

static void test_edit_move_items_within_same_group(void) {
  struct ov_error err = {0};
  struct ptk_anm2_edit *edit = NULL;
  uint32_t sel_id = 0;
  uint32_t id_a = 0;
  uint32_t id_b = 0;
  uint32_t id_c = 0;
  uint32_t id_d = 0;

  edit = ptk_anm2_edit_create(&err);
  TEST_ASSERT_SUCCEEDED(edit != NULL, &err);
  struct ptk_anm2 *doc = get_doc(edit);
  TEST_ASSERT_SUCCEEDED(edit != NULL, &err);

  sel_id = ptk_anm2_selector_insert(doc, 0, "Group", &err);
  TEST_ASSERT_SUCCEEDED(sel_id != 0, &err);
  id_a = ptk_anm2_item_insert_value(doc, sel_id, "A", "a", &err);
  id_b = ptk_anm2_item_insert_value(doc, sel_id, "B", "b", &err);
  id_c = ptk_anm2_item_insert_value(doc, sel_id, "C", "c", &err);
  id_d = ptk_anm2_item_insert_value(doc, sel_id, "D", "d", &err);
  TEST_ASSERT_SUCCEEDED(id_a != 0, &err);
  TEST_ASSERT_SUCCEEDED(id_b != 0, &err);
  TEST_ASSERT_SUCCEEDED(id_c != 0, &err);
  TEST_ASSERT_SUCCEEDED(id_d != 0, &err);

  // Move A to D (drop on D = insert before D)
  // Initial: A(0), B(1), C(2), D(3)
  // Remove A: B(0), C(1), D(2)
  // Insert before D (idx 2): B(0), C(1), A(2), D(3)
  uint32_t move_ids[1] = {id_a};
  TEST_ASSERT_SUCCEEDED(ptk_anm2_edit_move_items(edit, move_ids, 1, id_d, false, false, &err), &err);

  // Expected order: B, C, A, D (A inserted before D)
  TEST_CHECK(ptk_anm2_item_get_id(doc, 0, 0) == id_b);
  TEST_CHECK(ptk_anm2_item_get_id(doc, 0, 1) == id_c);
  TEST_CHECK(ptk_anm2_item_get_id(doc, 0, 2) == id_a);
  TEST_CHECK(ptk_anm2_item_get_id(doc, 0, 3) == id_d);

  // Selection should contain moved item
  size_t count = 0;
  uint32_t const *ids = ptk_anm2_edit_get_selected_item_ids(edit, &count);
  TEST_CHECK(count == 1);
  TEST_CHECK(ids && ids[0] == id_a);

  ptk_anm2_edit_destroy(&edit);
}

static void test_edit_move_items_to_item(void) {
  struct ov_error err = {0};
  struct ptk_anm2_edit *edit = NULL;
  uint32_t sel1_id = 0;
  uint32_t sel2_id = 0;
  uint32_t id_a = 0;
  uint32_t id_b = 0;
  uint32_t id_c = 0;

  edit = ptk_anm2_edit_create(&err);
  TEST_ASSERT_SUCCEEDED(edit != NULL, &err);
  struct ptk_anm2 *doc = get_doc(edit);
  TEST_ASSERT_SUCCEEDED(edit != NULL, &err);

  sel1_id = ptk_anm2_selector_insert(doc, 0, "Group1", &err);
  sel2_id = ptk_anm2_selector_insert(doc, 0, "Group2", &err);
  TEST_ASSERT_SUCCEEDED(sel1_id != 0, &err);
  TEST_ASSERT_SUCCEEDED(sel2_id != 0, &err);
  id_a = ptk_anm2_item_insert_value(doc, sel1_id, "A", "a", &err);
  id_b = ptk_anm2_item_insert_value(doc, sel2_id, "B", "b", &err);
  id_c = ptk_anm2_item_insert_value(doc, sel2_id, "C", "c", &err);
  TEST_ASSERT_SUCCEEDED(id_a != 0, &err);
  TEST_ASSERT_SUCCEEDED(id_b != 0, &err);
  TEST_ASSERT_SUCCEEDED(id_c != 0, &err);

  // Move A to Group2, drop on C (insert before C)
  uint32_t move_ids[1] = {id_a};
  TEST_ASSERT_SUCCEEDED(ptk_anm2_edit_move_items(edit, move_ids, 1, id_c, false, false, &err), &err);

  TEST_CHECK(ptk_anm2_item_count(doc, sel1_id) == 0);
  TEST_CHECK(ptk_anm2_item_count(doc, sel2_id) == 3);
  // Expected order in Group2: B, A, C (A inserted before C)
  TEST_CHECK(ptk_anm2_item_get_id(doc, 1, 0) == id_b);
  TEST_CHECK(ptk_anm2_item_get_id(doc, 1, 1) == id_a);
  TEST_CHECK(ptk_anm2_item_get_id(doc, 1, 2) == id_c);

  ptk_anm2_edit_destroy(&edit);
}

static void test_selection_refresh_selector_removed(void) {
  struct ov_error err = {0};
  struct ptk_anm2_edit *edit = NULL;

  edit = ptk_anm2_edit_create(&err);
  TEST_ASSERT_SUCCEEDED(edit != NULL, &err);
  struct ptk_anm2 *doc = get_doc(edit);
  TEST_ASSERT_SUCCEEDED(edit != NULL, &err);

  uint32_t group1 = ptk_anm2_selector_insert(doc, 0, "Group1", &err);
  TEST_ASSERT_SUCCEEDED(group1 != 0, &err);

  // Focus on the selector
  TEST_ASSERT_SUCCEEDED(ptk_anm2_edit_apply_treeview_selection(edit, group1, true, false, false, &err), &err);

  struct ptk_anm2_edit_state state = {0};
  ptk_anm2_edit_get_state(edit, &state);
  TEST_CHECK(state.focus_type == ptk_anm2_edit_focus_selector);
  TEST_CHECK(state.focus_id == group1);

  // Remove the selector
  TEST_ASSERT_SUCCEEDED(ptk_anm2_selector_remove(doc, group1, &err), &err);
  ptk_anm2_edit_update_on_doc_op(edit, ptk_anm2_op_selector_remove, group1, 0, 0);

  // Focus should be cleared since the selector no longer exists
  ptk_anm2_edit_get_state(edit, &state);
  TEST_CHECK(state.focus_type == ptk_anm2_edit_focus_none);
  TEST_CHECK(state.focus_id == 0);

  ptk_anm2_edit_destroy(&edit);
}

// Helper for view callback tests
struct view_callback_log {
  size_t count;
  enum ptk_anm2_edit_view_op ops[32];
  uint32_t ids[32];
  uint32_t before_ids[32];
};

static void view_callback_logger(void *userdata, struct ptk_anm2_edit_view_event const *event) {
  struct view_callback_log *log = (struct view_callback_log *)userdata;
  if (log->count < 32) {
    log->ops[log->count] = event->op;
    log->ids[log->count] = event->id;
    log->before_ids[log->count] = event->before_id;
    log->count++;
  }
}

static bool log_contains_op(struct view_callback_log const *log, enum ptk_anm2_edit_view_op op) {
  for (size_t i = 0; i < log->count; ++i) {
    if (log->ops[i] == op) {
      return true;
    }
  }
  return false;
}

static size_t log_count_op(struct view_callback_log const *log, enum ptk_anm2_edit_view_op op) {
  size_t count = 0;
  for (size_t i = 0; i < log->count; ++i) {
    if (log->ops[i] == op) {
      ++count;
    }
  }
  return count;
}

static void test_view_callback_basic(void) {
  struct ov_error err = {0};
  struct ptk_anm2_edit *edit = NULL;

  edit = ptk_anm2_edit_create(&err);
  TEST_ASSERT_SUCCEEDED(edit != NULL, &err);

  struct view_callback_log log = {0};
  ptk_anm2_edit_set_view_callback(edit, view_callback_logger, &log);

  // Trigger a reset notification
  ptk_anm2_edit_update_on_doc_op(edit, ptk_anm2_op_reset, 0, 0, 0);

  // Should have received rebuild + select (cleared) + detail_refresh
  TEST_CHECK(log.count == 3);
  TEST_CHECK(log.ops[0] == ptk_anm2_edit_view_treeview_rebuild);
  TEST_CHECK(log.ops[1] == ptk_anm2_edit_view_treeview_select);
  TEST_CHECK(log.ops[2] == ptk_anm2_edit_view_detail_refresh);

  ptk_anm2_edit_destroy(&edit);
}

static void test_view_callback_on_add_selector(void) {
  struct ov_error err = {0};
  struct ptk_anm2_edit *edit = NULL;

  edit = ptk_anm2_edit_create(&err);
  TEST_ASSERT_SUCCEEDED(edit != NULL, &err);
  struct ptk_anm2 *doc = get_doc(edit);
  TEST_ASSERT_SUCCEEDED(edit != NULL, &err);

  struct view_callback_log log = {0};
  ptk_anm2_edit_set_view_callback(edit, view_callback_logger, &log);

  uint32_t group_id = ptk_anm2_selector_insert(doc, 0, "Group1", &err);
  TEST_ASSERT_SUCCEEDED(group_id != 0, &err);

  TEST_CHECK(log.count >= 1);
  TEST_CHECK(log.ops[0] == ptk_anm2_edit_view_treeview_insert_selector);
  TEST_CHECK(log.ids[0] == group_id);
  TEST_CHECK(log_contains_op(&log, ptk_anm2_edit_view_undo_redo_state_changed));
  TEST_CHECK(log_contains_op(&log, ptk_anm2_edit_view_modified_state_changed));

  ptk_anm2_edit_destroy(&edit);
}

static void test_view_callback_on_focus_change(void) {
  struct ov_error err = {0};
  struct ptk_anm2_edit *edit = NULL;
  uint32_t group_id = 0;
  uint32_t item_id = 0;

  edit = ptk_anm2_edit_create(&err);
  TEST_ASSERT_SUCCEEDED(edit != NULL, &err);
  struct ptk_anm2 *doc = get_doc(edit);
  TEST_ASSERT_SUCCEEDED(edit != NULL, &err);

  group_id = ptk_anm2_selector_insert(doc, 0, "Group1", &err);
  TEST_ASSERT_SUCCEEDED(group_id != 0, &err);
  item_id = ptk_anm2_item_insert_value(doc, group_id, "Item1", "val", &err);
  TEST_ASSERT_SUCCEEDED(item_id != 0, &err);

  struct view_callback_log log = {0};
  ptk_anm2_edit_set_view_callback(edit, view_callback_logger, &log);

  // Click on item - should trigger focus + select + detail_refresh
  TEST_ASSERT_SUCCEEDED(ptk_anm2_edit_apply_treeview_selection(edit, item_id, false, false, false, &err), &err);

  TEST_CHECK(log.count == 2);
  TEST_CHECK(log.ops[0] == ptk_anm2_edit_view_treeview_select);
  TEST_CHECK(log.ops[1] == ptk_anm2_edit_view_detail_refresh);

  ptk_anm2_edit_destroy(&edit);
}

static void test_view_callback_transaction_buffering(void) {
  struct ov_error err = {0};
  struct ptk_anm2_edit *edit = NULL;
  uint32_t sel_id = 0;

  edit = ptk_anm2_edit_create(&err);
  TEST_ASSERT_SUCCEEDED(edit != NULL, &err);
  struct ptk_anm2 *doc = get_doc(edit);
  TEST_ASSERT_SUCCEEDED(edit != NULL, &err);

  struct view_callback_log log = {0};
  ptk_anm2_edit_set_view_callback(edit, view_callback_logger, &log);

  // Start transaction
  TEST_ASSERT_SUCCEEDED(ptk_anm2_begin_transaction(doc, &err), &err);

  // Add multiple items - events should be suppressed during transaction
  sel_id = ptk_anm2_selector_insert(doc, 0, "Group1", &err);
  (void)ptk_anm2_item_insert_value(doc, sel_id, "Item1", "val1", &err);
  (void)ptk_anm2_item_insert_value(doc, sel_id, "Item2", "val2", &err);

  // During transaction, only state change events should be forwarded
  // Structural events (insert_group, insert_item) should be suppressed
  size_t structural_count = 0;
  for (size_t i = 0; i < log.count; ++i) {
    if (log.ops[i] != ptk_anm2_edit_view_undo_redo_state_changed &&
        log.ops[i] != ptk_anm2_edit_view_modified_state_changed &&
        log.ops[i] != ptk_anm2_edit_view_save_state_changed) {
      structural_count++;
    }
  }
  TEST_CHECK(structural_count == 0);

  // Clear log before end_transaction
  log.count = 0;

  // End transaction - should trigger rebuild
  TEST_ASSERT_SUCCEEDED(ptk_anm2_end_transaction(doc, &err), &err);

  // After transaction ends, should have rebuild + detail_refresh
  bool has_rebuild = false;
  bool has_detail_refresh = false;
  for (size_t i = 0; i < log.count; ++i) {
    if (log.ops[i] == ptk_anm2_edit_view_treeview_rebuild) {
      has_rebuild = true;
    }
    if (log.ops[i] == ptk_anm2_edit_view_detail_refresh) {
      has_detail_refresh = true;
    }
  }
  TEST_CHECK(has_rebuild);
  TEST_CHECK(has_detail_refresh);

  ptk_anm2_edit_destroy(&edit);
}

static void test_view_callback_undo_redo(void) {
  struct ov_error err = {0};
  struct ptk_anm2_edit *edit = NULL;
  uint32_t sel_id = 0;
  uint32_t item_id = 0;

  edit = ptk_anm2_edit_create(&err);
  TEST_ASSERT_SUCCEEDED(edit != NULL, &err);
  struct ptk_anm2 *doc = get_doc(edit);
  TEST_ASSERT_SUCCEEDED(edit != NULL, &err);

  // Add some items via transaction
  TEST_ASSERT_SUCCEEDED(ptk_anm2_begin_transaction(doc, &err), &err);
  sel_id = ptk_anm2_selector_insert(doc, 0, "Group1", &err);
  TEST_CHECK(sel_id != 0);
  item_id = ptk_anm2_item_insert_value(doc, sel_id, "Item1", "val1", &err);
  TEST_CHECK(item_id != 0);
  TEST_ASSERT_SUCCEEDED(ptk_anm2_end_transaction(doc, &err), &err);

  struct view_callback_log log = {0};
  ptk_anm2_edit_set_view_callback(edit, view_callback_logger, &log);

  // UNDO the transaction - should suppress intermediate events and emit rebuild at end
  TEST_ASSERT_SUCCEEDED(ptk_anm2_edit_undo(edit, &err), &err);

  // Should have rebuild + detail_refresh (not individual remove events)
  bool has_rebuild = false;
  bool has_detail_refresh = false;
  for (size_t i = 0; i < log.count; ++i) {
    if (log.ops[i] == ptk_anm2_edit_view_treeview_rebuild) {
      has_rebuild = true;
    }
    if (log.ops[i] == ptk_anm2_edit_view_detail_refresh) {
      has_detail_refresh = true;
    }
  }
  TEST_CHECK(has_rebuild);
  TEST_CHECK(has_detail_refresh);

  // Clear log
  log.count = 0;

  // REDO the transaction
  TEST_ASSERT_SUCCEEDED(ptk_anm2_edit_redo(edit, &err), &err);

  // Should also have rebuild + detail_refresh
  has_rebuild = false;
  has_detail_refresh = false;
  for (size_t i = 0; i < log.count; ++i) {
    if (log.ops[i] == ptk_anm2_edit_view_treeview_rebuild) {
      has_rebuild = true;
    }
    if (log.ops[i] == ptk_anm2_edit_view_detail_refresh) {
      has_detail_refresh = true;
    }
  }
  TEST_CHECK(has_rebuild);
  TEST_CHECK(has_detail_refresh);

  ptk_anm2_edit_destroy(&edit);
}

static void test_view_callback_single_op_undo(void) {
  struct ov_error err = {0};
  struct ptk_anm2_edit *edit = NULL;

  edit = ptk_anm2_edit_create(&err);
  TEST_ASSERT_SUCCEEDED(edit != NULL, &err);
  struct ptk_anm2 *doc = get_doc(edit);
  TEST_ASSERT_SUCCEEDED(edit != NULL, &err);

  // Add a single item (no transaction)
  uint32_t group_id = ptk_anm2_selector_insert(doc, 0, "Group1", &err);
  TEST_ASSERT_SUCCEEDED(group_id != 0, &err);

  struct view_callback_log log = {0};
  ptk_anm2_edit_set_view_callback(edit, view_callback_logger, &log);

  // UNDO single operation - should emit differential event (remove_group), not rebuild
  TEST_ASSERT_SUCCEEDED(ptk_anm2_edit_undo(edit, &err), &err);

  // Should have remove_group event (differential update)
  bool has_remove = false;
  for (size_t i = 0; i < log.count; ++i) {
    if (log.ops[i] == ptk_anm2_edit_view_treeview_remove_selector) {
      has_remove = true;
    }
  }
  TEST_CHECK(has_remove);

  ptk_anm2_edit_destroy(&edit);
}

static void test_view_callback_state_dedup(void) {
  struct ov_error err = {0};
  struct ptk_anm2_edit *edit = NULL;

  edit = ptk_anm2_edit_create(&err);
  TEST_ASSERT_SUCCEEDED(edit != NULL, &err);
  struct ptk_anm2 *doc = get_doc(edit);
  TEST_ASSERT_SUCCEEDED(edit != NULL, &err);

  struct view_callback_log log = {0};
  ptk_anm2_edit_set_view_callback(edit, view_callback_logger, &log);

  // First operation: can_undo changes from false to true
  (void)ptk_anm2_selector_insert(doc, 0, "Group1", &err);

  // Should have exactly 1 undo_redo_state_changed (state changed)
  TEST_CHECK(log_count_op(&log, ptk_anm2_edit_view_undo_redo_state_changed) == 1);
  // Should have exactly 1 modified_state_changed (state changed)
  TEST_CHECK(log_count_op(&log, ptk_anm2_edit_view_modified_state_changed) == 1);

  // Clear log
  log.count = 0;

  // Second operation: can_undo stays true, can_redo stays false
  (void)ptk_anm2_selector_insert(doc, 0, "Group2", &err);

  // Should NOT have undo_redo_state_changed (no state change)
  TEST_CHECK(log_count_op(&log, ptk_anm2_edit_view_undo_redo_state_changed) == 0);
  // Should NOT have modified_state_changed (already modified)
  TEST_CHECK(log_count_op(&log, ptk_anm2_edit_view_modified_state_changed) == 0);
  // But should have structural event
  TEST_CHECK(log_count_op(&log, ptk_anm2_edit_view_treeview_insert_selector) == 1);

  // Clear log
  log.count = 0;

  // Undo: can_undo stays true, can_redo changes from false to true
  TEST_ASSERT_SUCCEEDED(ptk_anm2_edit_undo(edit, &err), &err);

  // Should have exactly 1 undo_redo_state_changed (can_redo changed)
  TEST_CHECK(log_count_op(&log, ptk_anm2_edit_view_undo_redo_state_changed) == 1);

  // Clear log
  log.count = 0;

  // Undo again: can_undo changes from true to false, can_redo stays true
  TEST_ASSERT_SUCCEEDED(ptk_anm2_edit_undo(edit, &err), &err);

  // Should have exactly 1 undo_redo_state_changed (can_undo changed)
  TEST_CHECK(log_count_op(&log, ptk_anm2_edit_view_undo_redo_state_changed) == 1);

  ptk_anm2_edit_destroy(&edit);
}

static void test_undo_restores_multiselection(void) {
  struct ov_error err = {0};
  struct ptk_anm2_edit *edit = NULL;

  edit = ptk_anm2_edit_create(&err);
  TEST_ASSERT_SUCCEEDED(edit != NULL, &err);
  struct ptk_anm2 *doc = get_doc(edit);
  TEST_ASSERT_SUCCEEDED(edit != NULL, &err);

  // Create items A, B, C in a group
  uint32_t group_id = ptk_anm2_selector_insert(doc, 0, "Group", &err);
  TEST_ASSERT_SUCCEEDED(group_id != 0, &err);
  uint32_t id_a = ptk_anm2_item_insert_value(doc, group_id, "A", "a", &err);
  uint32_t id_b = ptk_anm2_item_insert_value(doc, group_id, "B", "b", &err);
  uint32_t id_c = ptk_anm2_item_insert_value(doc, group_id, "C", "c", &err);
  TEST_ASSERT_SUCCEEDED(id_a != 0, &err);
  TEST_ASSERT_SUCCEEDED(id_b != 0, &err);
  TEST_ASSERT_SUCCEEDED(id_c != 0, &err);

  // Select A and B (multi-selection)
  TEST_ASSERT_SUCCEEDED(ptk_anm2_edit_apply_treeview_selection(edit, id_a, false, false, false, &err), &err);
  TEST_ASSERT_SUCCEEDED(ptk_anm2_edit_apply_treeview_selection(edit, id_b, false, false, true, &err), &err);

  // Verify multi-selection state
  size_t count = 0;
  uint32_t const *ids = ptk_anm2_edit_get_selected_item_ids(edit, &count);
  TEST_CHECK(count == 2);
  TEST_CHECK(ids != NULL);

  // Move selected items to another position
  uint32_t move_ids[2] = {id_a, id_b};
  TEST_ASSERT_SUCCEEDED(ptk_anm2_edit_move_items(edit, move_ids, 2, id_c, false, false, &err), &err);

  // After move, selection should still be A and B
  ids = ptk_anm2_edit_get_selected_item_ids(edit, &count);
  TEST_CHECK(count == 2);

  // Undo the move
  TEST_ASSERT_SUCCEEDED(ptk_anm2_edit_undo(edit, &err), &err);

  // After undo, selection should be restored to A and B
  ids = ptk_anm2_edit_get_selected_item_ids(edit, &count);
  TEST_CHECK(count == 2);
  // Check that both A and B are selected (order may vary)
  bool has_a = false;
  bool has_b = false;
  for (size_t i = 0; i < count; ++i) {
    if (ids[i] == id_a) {
      has_a = true;
    }
    if (ids[i] == id_b) {
      has_b = true;
    }
  }
  TEST_CHECK(has_a);
  TEST_CHECK(has_b);

  // Focus should also be restored
  struct ptk_anm2_edit_state state = {0};
  ptk_anm2_edit_get_state(edit, &state);
  TEST_CHECK(state.focus_type == ptk_anm2_edit_focus_item);
  TEST_CHECK(state.focus_id == id_a || state.focus_id == id_b);

  ptk_anm2_edit_destroy(&edit);
}

static void test_undo_after_move_selector(void) {
  struct ov_error err = {0};
  struct ptk_anm2_edit *edit = NULL;

  edit = ptk_anm2_edit_create(&err);
  TEST_ASSERT_SUCCEEDED(edit != NULL, &err);
  struct ptk_anm2 *doc = get_doc(edit);
  TEST_ASSERT_SUCCEEDED(edit != NULL, &err);

  // Create groups A, B, C
  uint32_t grp_a = ptk_anm2_selector_insert(doc, 0, "A", &err);
  uint32_t grp_b = ptk_anm2_selector_insert(doc, 0, "B", &err);
  uint32_t grp_c = ptk_anm2_selector_insert(doc, 0, "C", &err);
  TEST_ASSERT_SUCCEEDED(grp_a != 0, &err);
  TEST_ASSERT_SUCCEEDED(grp_b != 0, &err);
  TEST_ASSERT_SUCCEEDED(grp_c != 0, &err);

  // Initial order: A(0), B(1), C(2)
  TEST_CHECK(ptk_anm2_selector_get_id(doc, 0) == grp_a);
  TEST_CHECK(ptk_anm2_selector_get_id(doc, 1) == grp_b);
  TEST_CHECK(ptk_anm2_selector_get_id(doc, 2) == grp_c);

  // Move C before A (drag C, drop on A)
  TEST_ASSERT_SUCCEEDED(ptk_anm2_edit_move_selector(edit, grp_c, grp_a, false, &err), &err);

  // After move: C(0), A(1), B(2)
  TEST_CHECK(ptk_anm2_selector_get_id(doc, 0) == grp_c);
  TEST_CHECK(ptk_anm2_selector_get_id(doc, 1) == grp_a);
  TEST_CHECK(ptk_anm2_selector_get_id(doc, 2) == grp_b);

  // Undo the move
  TEST_ASSERT_SUCCEEDED(ptk_anm2_edit_undo(edit, &err), &err);

  // Order should be restored: A(0), B(1), C(2)
  TEST_CHECK(ptk_anm2_selector_get_id(doc, 0) == grp_a);
  TEST_CHECK(ptk_anm2_selector_get_id(doc, 1) == grp_b);
  TEST_CHECK(ptk_anm2_selector_get_id(doc, 2) == grp_c);

  // Move again after undo - doc state should be correct
  TEST_ASSERT_SUCCEEDED(ptk_anm2_edit_move_selector(edit, grp_c, grp_a, false, &err), &err);

  // After move: C(0), A(1), B(2)
  TEST_CHECK(ptk_anm2_selector_get_id(doc, 0) == grp_c);
  TEST_CHECK(ptk_anm2_selector_get_id(doc, 1) == grp_a);
  TEST_CHECK(ptk_anm2_selector_get_id(doc, 2) == grp_b);

  ptk_anm2_edit_destroy(&edit);
}

static void test_view_callback_move_selector_and_undo(void) {
  struct ov_error err = {0};
  struct ptk_anm2_edit *edit = NULL;

  edit = ptk_anm2_edit_create(&err);
  TEST_ASSERT_SUCCEEDED(edit != NULL, &err);
  struct ptk_anm2 *doc = get_doc(edit);
  TEST_ASSERT_SUCCEEDED(edit != NULL, &err);

  // Create groups A, B, C
  uint32_t grp_a = ptk_anm2_selector_insert(doc, 0, "A", &err);
  uint32_t grp_b = ptk_anm2_selector_insert(doc, 0, "B", &err);
  uint32_t grp_c = ptk_anm2_selector_insert(doc, 0, "C", &err);
  TEST_ASSERT_SUCCEEDED(grp_a != 0, &err);
  TEST_ASSERT_SUCCEEDED(grp_b != 0, &err);
  TEST_ASSERT_SUCCEEDED(grp_c != 0, &err);

  // Initial order: A(0), B(1), C(2)
  TEST_CHECK(ptk_anm2_selector_get_id(doc, 0) == grp_a);
  TEST_CHECK(ptk_anm2_selector_get_id(doc, 1) == grp_b);
  TEST_CHECK(ptk_anm2_selector_get_id(doc, 2) == grp_c);

  struct view_callback_log log = {0};
  ptk_anm2_edit_set_view_callback(edit, view_callback_logger, &log);

  // Move C before A (drag C, drop on A)
  TEST_ASSERT_SUCCEEDED(ptk_anm2_edit_move_selector(edit, grp_c, grp_a, false, &err), &err);

  // After move: C(0), A(1), B(2)
  TEST_CHECK(ptk_anm2_selector_get_id(doc, 0) == grp_c);
  TEST_CHECK(ptk_anm2_selector_get_id(doc, 1) == grp_a);
  TEST_CHECK(ptk_anm2_selector_get_id(doc, 2) == grp_b);

  // Check that move_group event was fired with correct id and before_id
  bool found_move = false;
  for (size_t i = 0; i < log.count; ++i) {
    if (log.ops[i] == ptk_anm2_edit_view_treeview_move_selector) {
      found_move = true;
      TEST_CHECK(log.ids[i] == grp_c);
      TEST_CHECK(log.before_ids[i] == grp_a); // C moved before A
    }
  }
  TEST_CHECK(found_move);

  // Clear log
  log.count = 0;

  // Undo the move
  TEST_ASSERT_SUCCEEDED(ptk_anm2_edit_undo(edit, &err), &err);

  // Order should be restored: A(0), B(1), C(2)
  TEST_CHECK(ptk_anm2_selector_get_id(doc, 0) == grp_a);
  TEST_CHECK(ptk_anm2_selector_get_id(doc, 1) == grp_b);
  TEST_CHECK(ptk_anm2_selector_get_id(doc, 2) == grp_c);

  // Check that move_group event was fired for UNDO with correct id and before_id
  found_move = false;
  for (size_t i = 0; i < log.count; ++i) {
    if (log.ops[i] == ptk_anm2_edit_view_treeview_move_selector) {
      found_move = true;
      TEST_CHECK(log.ids[i] == grp_c);
      TEST_CHECK(log.before_ids[i] == 0); // C moved back to end (no element after)
    }
  }
  TEST_CHECK(found_move);

  // Clear log
  log.count = 0;

  // Move again after undo - should work correctly
  TEST_ASSERT_SUCCEEDED(ptk_anm2_edit_move_selector(edit, grp_c, grp_a, false, &err), &err);

  // After move: C(0), A(1), B(2)
  TEST_CHECK(ptk_anm2_selector_get_id(doc, 0) == grp_c);
  TEST_CHECK(ptk_anm2_selector_get_id(doc, 1) == grp_a);
  TEST_CHECK(ptk_anm2_selector_get_id(doc, 2) == grp_b);

  // Check event fired correctly
  found_move = false;
  for (size_t i = 0; i < log.count; ++i) {
    if (log.ops[i] == ptk_anm2_edit_view_treeview_move_selector) {
      found_move = true;
      TEST_CHECK(log.ids[i] == grp_c);
      TEST_CHECK(log.before_ids[i] == grp_a); // C moved before A
    }
  }
  TEST_CHECK(found_move);

  ptk_anm2_edit_destroy(&edit);
}

static void test_view_callback_move_item_and_undo(void) {
  struct ov_error err = {0};
  struct ptk_anm2_edit *edit = NULL;

  edit = ptk_anm2_edit_create(&err);
  TEST_ASSERT_SUCCEEDED(edit != NULL, &err);
  struct ptk_anm2 *doc = get_doc(edit);
  TEST_ASSERT_SUCCEEDED(edit != NULL, &err);

  // Create group with items A, B, C
  uint32_t grp = ptk_anm2_selector_insert(doc, 0, "Group", &err);
  TEST_ASSERT_SUCCEEDED(grp != 0, &err);
  uint32_t item_a = ptk_anm2_item_insert_value(doc, grp, "A", "a", &err);
  uint32_t item_b = ptk_anm2_item_insert_value(doc, grp, "B", "b", &err);
  uint32_t item_c = ptk_anm2_item_insert_value(doc, grp, "C", "c", &err);
  TEST_ASSERT_SUCCEEDED(item_a != 0, &err);
  TEST_ASSERT_SUCCEEDED(item_b != 0, &err);
  TEST_ASSERT_SUCCEEDED(item_c != 0, &err);

  // Initial order: A(0), B(1), C(2)
  TEST_CHECK(ptk_anm2_item_get_id(doc, 0, 0) == item_a);
  TEST_CHECK(ptk_anm2_item_get_id(doc, 0, 1) == item_b);
  TEST_CHECK(ptk_anm2_item_get_id(doc, 0, 2) == item_c);

  struct view_callback_log log = {0};
  ptk_anm2_edit_set_view_callback(edit, view_callback_logger, &log);

  // Move C before A using ptk_anm2_item_move directly (no transaction)
  TEST_ASSERT_SUCCEEDED(ptk_anm2_item_move(doc, item_c, item_a, &err), &err);

  // After move: C(0), A(1), B(2)
  TEST_CHECK(ptk_anm2_item_get_id(doc, 0, 0) == item_c);
  TEST_CHECK(ptk_anm2_item_get_id(doc, 0, 1) == item_a);
  TEST_CHECK(ptk_anm2_item_get_id(doc, 0, 2) == item_b);

  // Check move_item event fired with correct id and before_id
  bool found_move = false;
  for (size_t i = 0; i < log.count; ++i) {
    if (log.ops[i] == ptk_anm2_edit_view_treeview_move_item) {
      found_move = true;
      TEST_CHECK(log.ids[i] == item_c);
      TEST_CHECK(log.before_ids[i] == item_a); // C moved before A
    }
  }
  TEST_CHECK(found_move);

  // Clear log
  log.count = 0;

  // Undo the move
  TEST_ASSERT_SUCCEEDED(ptk_anm2_edit_undo(edit, &err), &err);

  // Order should be restored: A(0), B(1), C(2)
  TEST_CHECK(ptk_anm2_item_get_id(doc, 0, 0) == item_a);
  TEST_CHECK(ptk_anm2_item_get_id(doc, 0, 1) == item_b);
  TEST_CHECK(ptk_anm2_item_get_id(doc, 0, 2) == item_c);

  // Check move_item event fired for UNDO with correct id and before_id
  found_move = false;
  for (size_t i = 0; i < log.count; ++i) {
    if (log.ops[i] == ptk_anm2_edit_view_treeview_move_item) {
      found_move = true;
      TEST_CHECK(log.ids[i] == item_c);
      TEST_CHECK(log.before_ids[i] == 0); // C moved back to end (no element after)
    }
  }
  TEST_CHECK(found_move);

  ptk_anm2_edit_destroy(&edit);
}

static void test_add_selector_and_undo(void) {
  struct ov_error err = {0};
  struct ptk_anm2_edit *edit = NULL;
  struct view_callback_log log = {0};

  edit = ptk_anm2_edit_create(&err);
  TEST_ASSERT_SUCCEEDED(edit != NULL, &err);
  struct ptk_anm2 *doc = get_doc(edit);
  TEST_ASSERT_SUCCEEDED(edit != NULL, &err);

  // Set up view callback to monitor events
  ptk_anm2_edit_set_view_callback(edit, view_callback_logger, &log);

  // Simulate "existing file" by adding a selector first (direct on doc, not via edit)
  uint32_t existing_grp = ptk_anm2_selector_insert(doc, 0, "Existing", &err);
  TEST_ASSERT_SUCCEEDED(existing_grp != 0, &err);

  // Clear the log to focus on add/undo operations
  log.count = 0;

  // Select the existing selector
  TEST_ASSERT_SUCCEEDED(ptk_anm2_edit_apply_treeview_selection(edit, existing_grp, true, false, false, &err), &err);

  log.count = 0; // Clear again to focus on add operation

  // Use ptk_anm2_edit_add_selector (same as GUI uses)
  TEST_ASSERT_SUCCEEDED(ptk_anm2_edit_add_selector(edit, "New", &err), &err);
  TEST_CHECK(ptk_anm2_selector_count(doc) == 2);

  // Get the new selector's ID
  uint32_t new_grp = ptk_anm2_edit_selector_get_id(edit, 1);
  TEST_CHECK(new_grp != 0);

  // Verify insert_group event was sent
  TEST_CHECK(log_contains_op(&log, ptk_anm2_edit_view_treeview_insert_selector));

  // Simulate GUI behavior: user selects the newly added selector
  TEST_ASSERT_SUCCEEDED(ptk_anm2_edit_apply_treeview_selection(edit, new_grp, true, false, false, &err), &err);

  // Verify focus is on the new selector
  struct ptk_anm2_edit_state state = {0};
  ptk_anm2_edit_get_state(edit, &state);
  TEST_CHECK(state.focus_id == new_grp);

  log.count = 0; // Clear to focus on undo operation

  // Undo - should remove the new selector and send remove_group event
  TEST_ASSERT_SUCCEEDED(ptk_anm2_edit_undo(edit, &err), &err);
  TEST_CHECK(ptk_anm2_selector_count(doc) == 1);

  TEST_MSG("UNDO must send remove_group event to TreeView");
  TEST_CHECK(log_contains_op(&log, ptk_anm2_edit_view_treeview_remove_selector));

  // Verify the remove event has the correct ID
  bool found_correct_id = false;
  for (size_t i = 0; i < log.count; ++i) {
    if (log.ops[i] == ptk_anm2_edit_view_treeview_remove_selector) {
      TEST_CHECK(log.ids[i] == new_grp);
      found_correct_id = (log.ids[i] == new_grp);
      break;
    }
  }
  TEST_CHECK(found_correct_id);

  // Focus should be cleared (the selector we had focus on was deleted)
  ptk_anm2_edit_get_state(edit, &state);
  TEST_CHECK(state.focus_id == 0 || state.focus_id == existing_grp);

  ptk_anm2_edit_destroy(&edit);
}

static void test_swap_adjacent_selectors(void) {
  struct ov_error err = {0};
  struct ptk_anm2_edit *edit = NULL;

  edit = ptk_anm2_edit_create(&err);
  TEST_ASSERT_SUCCEEDED(edit != NULL, &err);
  struct ptk_anm2 *doc = get_doc(edit);
  TEST_ASSERT_SUCCEEDED(edit != NULL, &err);

  // Create A, B - swap A with B (move A to B position = move A after B)
  uint32_t grp_a = ptk_anm2_selector_insert(doc, 0, "A", &err);
  uint32_t grp_b = ptk_anm2_selector_insert(doc, 0, "B", &err);
  TEST_ASSERT_SUCCEEDED(grp_a != 0, &err);
  TEST_ASSERT_SUCCEEDED(grp_b != 0, &err);

  // Initial: A(0), B(1)
  TEST_CHECK(ptk_anm2_selector_get_id(doc, 0) == grp_a);
  TEST_CHECK(ptk_anm2_selector_get_id(doc, 1) == grp_b);

  // Move A before B - should result in no change since A is already before B
  // Move A to B position means "insert A before B" -> no change
  // But if user drags A to B, they want to swap them
  // Let's test what happens when we move the first to second position
  TEST_ASSERT_SUCCEEDED(ptk_anm2_edit_move_selector(edit, grp_a, grp_b, false, &err), &err);

  // After move: A should still be at 0 (no-op when before_id is immediately after)
  // or B(0), A(1) if swap is expected
  size_t idx_a = SIZE_MAX;
  size_t idx_b = SIZE_MAX;
  ptk_anm2_find_selector(doc, grp_a, &idx_a);
  ptk_anm2_find_selector(doc, grp_b, &idx_b);

  // Document current behavior
  TEST_MSG("After move A before B: A at %zu, B at %zu", idx_a, idx_b);
  // If this is a swap, A should be at 1 and B at 0
  // If this is "insert before", A stays at 0

  ptk_anm2_edit_destroy(&edit);
}

static void test_swap_adjacent_items(void) {
  struct ov_error err = {0};
  struct ptk_anm2_edit *edit = NULL;

  edit = ptk_anm2_edit_create(&err);
  TEST_ASSERT_SUCCEEDED(edit != NULL, &err);
  struct ptk_anm2 *doc = get_doc(edit);
  TEST_ASSERT_SUCCEEDED(edit != NULL, &err);

  uint32_t grp = ptk_anm2_selector_insert(doc, 0, "Group", &err);
  TEST_ASSERT_SUCCEEDED(grp != 0, &err);
  uint32_t item_a = ptk_anm2_item_insert_value(doc, grp, "A", "a", &err);
  uint32_t item_b = ptk_anm2_item_insert_value(doc, grp, "B", "b", &err);
  TEST_ASSERT_SUCCEEDED(item_a != 0, &err);
  TEST_ASSERT_SUCCEEDED(item_b != 0, &err);

  // Initial: A(0), B(1)
  TEST_CHECK(ptk_anm2_item_get_id(doc, 0, 0) == item_a);
  TEST_CHECK(ptk_anm2_item_get_id(doc, 0, 1) == item_b);

  // Move A to B position
  uint32_t move_ids[1] = {item_a};
  TEST_ASSERT_SUCCEEDED(ptk_anm2_edit_move_items(edit, move_ids, 1, item_b, false, false, &err), &err);

  // Document current behavior
  size_t sel_idx = 0;
  size_t idx_a = SIZE_MAX;
  size_t idx_b = SIZE_MAX;
  ptk_anm2_find_item(doc, item_a, &sel_idx, &idx_a);
  ptk_anm2_find_item(doc, item_b, &sel_idx, &idx_b);
  TEST_MSG("After move A to B: A at %zu, B at %zu", idx_a, idx_b);

  ptk_anm2_edit_destroy(&edit);
}

static void test_add_item_and_undo(void) {
  struct ov_error err = {0};
  struct ptk_anm2_edit *edit = NULL;
  struct view_callback_log log = {0};

  edit = ptk_anm2_edit_create(&err);
  TEST_ASSERT_SUCCEEDED(edit != NULL, &err);
  struct ptk_anm2 *doc = get_doc(edit);
  TEST_ASSERT_SUCCEEDED(edit != NULL, &err);

  ptk_anm2_edit_set_view_callback(edit, view_callback_logger, &log);

  // Create a selector first
  uint32_t grp_id = ptk_anm2_selector_insert(doc, 0, "Sel", &err);
  TEST_ASSERT_SUCCEEDED(grp_id != 0, &err);

  log.count = 0;

  // Add a new item via doc layer
  uint32_t item_id = ptk_anm2_item_insert_value(doc, grp_id, "name", "value", &err);
  TEST_CHECK(item_id != 0);

  TEST_CHECK(log_contains_op(&log, ptk_anm2_edit_view_treeview_insert_item));

  log.count = 0;

  // Undo
  TEST_ASSERT_SUCCEEDED(ptk_anm2_edit_undo(edit, &err), &err);

  // Verify the remove event has the correct ID
  TEST_CHECK(log_contains_op(&log, ptk_anm2_edit_view_treeview_remove_item));
  bool found_correct_id = false;
  for (size_t i = 0; i < log.count; ++i) {
    if (log.ops[i] == ptk_anm2_edit_view_treeview_remove_item) {
      TEST_CHECK(log.ids[i] == item_id);
      found_correct_id = (log.ids[i] == item_id);
      break;
    }
  }
  TEST_CHECK(found_correct_id);

  ptk_anm2_edit_destroy(&edit);
}

// Test would_move_items with selection range check
static void test_would_move_items_selection_range(void) {
  struct ov_error err = {0};
  struct ptk_anm2_edit *edit = NULL;

  edit = ptk_anm2_edit_create(&err);
  TEST_ASSERT_SUCCEEDED(edit != NULL, &err);
  struct ptk_anm2 *doc = get_doc(edit);
  TEST_ASSERT_SUCCEEDED(edit != NULL, &err);

  // Create A, B, C, D items in one group
  uint32_t group_id = ptk_anm2_selector_insert(doc, 0, "Group1", &err);
  TEST_ASSERT_SUCCEEDED(group_id != 0, &err);
  uint32_t item_a = ptk_anm2_item_insert_value(doc, group_id, "A", "vA", &err);
  TEST_ASSERT_SUCCEEDED(item_a != 0, &err);
  uint32_t item_b = ptk_anm2_item_insert_value(doc, group_id, "B", "vB", &err);
  TEST_ASSERT_SUCCEEDED(item_b != 0, &err);
  uint32_t item_c = ptk_anm2_item_insert_value(doc, group_id, "C", "vC", &err);
  TEST_ASSERT_SUCCEEDED(item_c != 0, &err);
  uint32_t item_d = ptk_anm2_item_insert_value(doc, group_id, "D", "vD", &err);
  TEST_ASSERT_SUCCEEDED(item_d != 0, &err);

  // Current order: A(0), B(1), C(2), D(3)

  // Test 1: Select A,B,C (indices 0,1,2), drop within selection range should be no-op
  uint32_t sel_abc[] = {item_a, item_b, item_c};

  // Drop on A (within range) - should be no-op
  TEST_CHECK(ptk_anm2_edit_would_move_items(edit, sel_abc, 3, item_a, false, false) == false);
  TEST_MSG("Dropping A,B,C on A should be no-op");

  // Drop on B (within range) - should be no-op
  TEST_CHECK(ptk_anm2_edit_would_move_items(edit, sel_abc, 3, item_b, false, false) == false);
  TEST_MSG("Dropping A,B,C before B should be no-op");

  // Drop after B (within range) - should be no-op
  TEST_CHECK(ptk_anm2_edit_would_move_items(edit, sel_abc, 3, item_b, false, true) == false);
  TEST_MSG("Dropping A,B,C after B should be no-op");

  // Drop on C (within range) - should be no-op
  TEST_CHECK(ptk_anm2_edit_would_move_items(edit, sel_abc, 3, item_c, false, false) == false);
  TEST_MSG("Dropping A,B,C before C should be no-op");

  // Drop after C (immediately after selection, index 3) - should be no-op
  TEST_CHECK(ptk_anm2_edit_would_move_items(edit, sel_abc, 3, item_c, false, true) == false);
  TEST_MSG("Dropping A,B,C after C should be no-op");

  // Test 2: Drop before D (index 3 = max_sel_idx + 1) - should be no-op (adjacent)
  TEST_CHECK(ptk_anm2_edit_would_move_items(edit, sel_abc, 3, item_d, false, false) == false);
  TEST_MSG("Dropping A,B,C before D should be no-op (adjacent to selection end)");

  // Drop after D (outside range, index 4) - should be valid
  TEST_CHECK(ptk_anm2_edit_would_move_items(edit, sel_abc, 3, item_d, false, true) == true);
  TEST_MSG("Dropping A,B,C after D should be valid");

  // Test 3: Select B,C (indices 1,2), drop before A (index 0) - should be valid
  uint32_t sel_bc[] = {item_b, item_c};
  TEST_CHECK(ptk_anm2_edit_would_move_items(edit, sel_bc, 2, item_a, false, false) == true);
  TEST_MSG("Dropping B,C before A should be valid");

  // Drop after A (index 1, which is B) - should be no-op (selection starts at index 1)
  TEST_CHECK(ptk_anm2_edit_would_move_items(edit, sel_bc, 2, item_a, false, true) == false);
  TEST_MSG("Dropping B,C after A should be no-op (adjacent to selection start)");

  // Test 4: Drop on group (different handling) - should be valid if items would move
  TEST_CHECK(ptk_anm2_edit_would_move_items(edit, sel_bc, 2, group_id, true, false) == true);
  TEST_MSG("Dropping B,C on group should be valid (moves to end)");

  ptk_anm2_edit_destroy(&edit);
}

TEST_LIST = {
    {"edit_create_destroy", test_edit_create_destroy},
    {"selection_click", test_selection_click},
    {"selection_ctrl_toggle", test_selection_ctrl_toggle},
    {"selection_shift_range", test_selection_shift_range},
    {"selection_ctrl_selector", test_selection_ctrl_selector},
    {"edit_selector_ops", test_edit_selector_ops},
    {"edit_item_rename_value", test_edit_item_rename_value},
    {"edit_multisel_detail_updates", test_edit_multisel_detail_updates},
    {"edit_delete_selected", test_edit_delete_selected},
    {"edit_reverse_focus_selector", test_edit_reverse_focus_selector},
    {"edit_move_items_order", test_edit_move_items_order},
    {"edit_param_ops", test_edit_param_ops},
    {"edit_document_props", test_edit_document_props},
    {"edit_update_on_doc_op", test_edit_update_on_doc_op},
    {"update_on_doc_op_set_operations", test_update_on_doc_op_set_operations},
    {"update_on_doc_op_insert_operations", test_update_on_doc_op_insert_operations},
    {"update_on_doc_op_move_operations", test_update_on_doc_op_move_operations},
    {"update_on_doc_op_remove_selector", test_update_on_doc_op_remove_selector},
    {"edit_move_items_within_same_group", test_edit_move_items_within_same_group},
    {"edit_move_items_to_item", test_edit_move_items_to_item},
    {"selection_refresh_selector_removed", test_selection_refresh_selector_removed},
    {"view_callback_basic", test_view_callback_basic},
    {"view_callback_on_add_selector", test_view_callback_on_add_selector},
    {"view_callback_on_focus_change", test_view_callback_on_focus_change},
    {"view_callback_transaction_buffering", test_view_callback_transaction_buffering},
    {"view_callback_undo_redo", test_view_callback_undo_redo},
    {"view_callback_single_op_undo", test_view_callback_single_op_undo},
    {"view_callback_state_dedup", test_view_callback_state_dedup},
    {"undo_restores_multiselection", test_undo_restores_multiselection},
    {"undo_after_move_selector", test_undo_after_move_selector},
    {"view_callback_move_selector_and_undo", test_view_callback_move_selector_and_undo},
    {"view_callback_move_item_and_undo", test_view_callback_move_item_and_undo},
    {"add_selector_and_undo", test_add_selector_and_undo},
    {"swap_adjacent_selectors", test_swap_adjacent_selectors},
    {"swap_adjacent_items", test_swap_adjacent_items},
    {"add_item_and_undo", test_add_item_and_undo},
    {"would_move_items_selection_range", test_would_move_items_selection_range},
    {NULL, NULL},
};
