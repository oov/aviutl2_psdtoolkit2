#include "anm2_edit.h"

#include "anm2.h"
#include "anm2_selection.h"

#include <ovarray.h>
#include <ovmo.h>
#include <ovprintf.h>
#include <ovsort.h>
#include <ovutf.h>
#include <string.h>

struct ptk_anm2_edit {
  struct ptk_anm2 *doc;
  struct anm2_selection *selection;
  ptk_anm2_edit_view_callback view_callback;
  void *view_userdata;
  // Transaction tracking for buffering events during UNDO/REDO
  int transaction_depth;
  // State tracking for change detection
  bool prev_can_undo;
  bool prev_can_redo;
  bool prev_modified;
  bool prev_can_save;
  bool needs_rebuild;
};

struct drag_item_pos {
  uint32_t id;
  size_t sel_idx;
  size_t item_idx;
};

static int compare_drag_item_pos(void const *const a, void const *const b, void *const userdata) {
  (void)userdata;
  struct drag_item_pos const *pa = (struct drag_item_pos const *)a;
  struct drag_item_pos const *pb = (struct drag_item_pos const *)b;
  if (pa->sel_idx != pb->sel_idx) {
    return (pa->sel_idx < pb->sel_idx) ? -1 : 1;
  }
  if (pa->item_idx != pb->item_idx) {
    return (pa->item_idx < pb->item_idx) ? -1 : 1;
  }
  return 0;
}

// Helper to notify view layer of changes
// During transactions (transaction_depth != 0), structural events are suppressed
// and needs_rebuild is set instead. State change events are always forwarded.
// Note: depth can be negative during UNDO (GROUP_END comes before GROUP_BEGIN).
static void notify_view(struct ptk_anm2_edit *edit, struct ptk_anm2_edit_view_event const *event) {
  if (!edit || !edit->view_callback || !event) {
    return;
  }
  // State change events are always forwarded
  if (event->op == ptk_anm2_edit_view_undo_redo_state_changed ||
      event->op == ptk_anm2_edit_view_modified_state_changed || event->op == ptk_anm2_edit_view_save_state_changed) {
    edit->view_callback(edit->view_userdata, event);
    return;
  }
  // During transactions (depth != 0), suppress structural events and mark for rebuild
  if (edit->transaction_depth != 0) {
    edit->needs_rebuild = true;
    return;
  }
  edit->view_callback(edit->view_userdata, event);
}

// Helper to detect and notify state changes
static void notify_state_changes(struct ptk_anm2_edit *edit) {
  if (!edit || !edit->doc) {
    return;
  }

  bool const cur_can_undo = ptk_anm2_can_undo(edit->doc);
  bool const cur_can_redo = ptk_anm2_can_redo(edit->doc);
  bool const cur_modified = ptk_anm2_is_modified(edit->doc);
  bool const cur_can_save = ptk_anm2_can_save(edit->doc);

  if (cur_can_undo != edit->prev_can_undo || cur_can_redo != edit->prev_can_redo) {
    edit->prev_can_undo = cur_can_undo;
    edit->prev_can_redo = cur_can_redo;
    notify_view(edit, &(struct ptk_anm2_edit_view_event){.op = ptk_anm2_edit_view_undo_redo_state_changed});
  }

  if (cur_modified != edit->prev_modified) {
    edit->prev_modified = cur_modified;
    notify_view(edit, &(struct ptk_anm2_edit_view_event){.op = ptk_anm2_edit_view_modified_state_changed});
  }

  if (cur_can_save != edit->prev_can_save) {
    edit->prev_can_save = cur_can_save;
    notify_view(edit, &(struct ptk_anm2_edit_view_event){.op = ptk_anm2_edit_view_save_state_changed});
  }
}

// Internal change_callback handler
static void on_doc_change_internal(
    void *userdata, enum ptk_anm2_op_type op_type, uint32_t id, uint32_t parent_id, uint32_t before_id) {
  struct ptk_anm2_edit *edit = (struct ptk_anm2_edit *)userdata;
  if (!edit) {
    return;
  }

  // Update internal state and emit structure change view callbacks
  ptk_anm2_edit_update_on_doc_op(edit, op_type, id, parent_id, before_id);

  // Note: state changes (undo/redo/modified) are notified via state_callback
}

static void on_state_change_internal(void *userdata) {
  struct ptk_anm2_edit *edit = (struct ptk_anm2_edit *)userdata;
  if (!edit) {
    return;
  }

  // Detect and notify state changes (undo/redo/modified)
  notify_state_changes(edit);
}

NODISCARD struct ptk_anm2_edit *ptk_anm2_edit_create(struct ov_error *err) {
  struct ptk_anm2_edit *out = NULL;
  struct ptk_anm2 *doc = NULL;

  doc = ptk_anm2_create(err);
  if (!doc) {
    OV_ERROR_ADD_TRACE(err);
    goto cleanup;
  }

  if (!OV_REALLOC(&out, 1, sizeof(*out))) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_out_of_memory);
    goto cleanup;
  }
  *out = (struct ptk_anm2_edit){
      .doc = doc,
  };

  out->selection = anm2_selection_create(doc, err);
  if (!out->selection) {
    OV_ERROR_ADD_TRACE(err);
    goto cleanup;
  }

  // Register internal callbacks to receive document changes
  ptk_anm2_set_change_callback(doc, on_doc_change_internal, out);
  ptk_anm2_set_state_callback(doc, on_state_change_internal, out);

  return out;

cleanup:
  if (out) {
    OV_FREE(&out);
  }
  ptk_anm2_destroy(&doc);
  return NULL;
}

void ptk_anm2_edit_destroy(struct ptk_anm2_edit **edit) {
  if (!edit || !*edit) {
    return;
  }
  struct ptk_anm2_edit *p = *edit;
  // Clear callbacks before destroying
  if (p->doc) {
    ptk_anm2_set_change_callback(p->doc, NULL, NULL);
    ptk_anm2_set_state_callback(p->doc, NULL, NULL);
    ptk_anm2_destroy(&p->doc);
  }
  anm2_selection_destroy(&p->selection);
  OV_FREE(&p);
  *edit = NULL;
}

struct ptk_anm2 const *ptk_anm2_edit_get_doc(struct ptk_anm2_edit const *edit) { return edit ? edit->doc : NULL; }

void ptk_anm2_edit_set_view_callback(struct ptk_anm2_edit *edit, ptk_anm2_edit_view_callback callback, void *userdata) {
  if (!edit) {
    return;
  }
  edit->view_callback = callback;
  edit->view_userdata = userdata;
}

bool ptk_anm2_edit_is_modified(struct ptk_anm2_edit const *edit) {
  if (!edit || !edit->doc) {
    return false;
  }
  return ptk_anm2_is_modified(edit->doc);
}

void ptk_anm2_edit_get_state(struct ptk_anm2_edit const *edit, struct ptk_anm2_edit_state *out) {
  if (!edit || !out) {
    return;
  }
  struct anm2_selection_state state = {0};
  anm2_selection_get_state(edit->selection, &state);
  out->anchor_id = state.anchor_id;
  out->focus_id = state.focus_id;
  out->focus_type = (enum ptk_anm2_edit_focus_type)state.focus_type;
}

uint32_t const *ptk_anm2_edit_get_selected_item_ids(struct ptk_anm2_edit const *edit, size_t *count) {
  if (!edit) {
    if (count) {
      *count = 0;
    }
    return NULL;
  }
  return anm2_selection_get_selected_ids(edit->selection, count);
}

size_t ptk_anm2_edit_get_selected_item_count(struct ptk_anm2_edit const *edit) {
  return edit ? anm2_selection_get_selected_count(edit->selection) : 0;
}

bool ptk_anm2_edit_is_item_selected(struct ptk_anm2_edit const *edit, uint32_t item_id) {
  return edit ? anm2_selection_is_selected(edit->selection, item_id) : false;
}

NODISCARD bool ptk_anm2_edit_apply_treeview_selection(struct ptk_anm2_edit *edit,
                                                      uint32_t item_id,
                                                      bool is_selector,
                                                      bool ctrl_pressed,
                                                      bool shift_pressed,
                                                      struct ov_error *err) {
  if (!edit) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_invalid_argument);
    return false;
  }
  if (!anm2_selection_apply_treeview_selection(
          edit->selection, item_id, is_selector, ctrl_pressed, shift_pressed, err)) {
    return false;
  }

  // Notify view of selection change (TreeView selection already changed via TVN_SELCHANGED)
  struct ptk_anm2_edit_view_event event = {
      .op = ptk_anm2_edit_view_treeview_select,
      .id = item_id,
      .is_selector = is_selector,
      .parent_id = 0,
      .before_id = 0,
      .selected = false,
  };
  notify_view(edit, &event);
  notify_view(edit, &(struct ptk_anm2_edit_view_event){.op = ptk_anm2_edit_view_detail_refresh});

  return true;
}

void ptk_anm2_edit_refresh_selection(struct ptk_anm2_edit *edit) {
  if (!edit) {
    return;
  }
  anm2_selection_refresh(edit->selection);
}

void ptk_anm2_edit_update_on_doc_op(
    struct ptk_anm2_edit *edit, enum ptk_anm2_op_type op_type, uint32_t id, uint32_t parent_id, uint32_t before_id) {
  if (!edit) {
    return;
  }

  struct ptk_anm2_edit_view_event event = {
      .op = ptk_anm2_edit_view_treeview_rebuild,
      .id = id,
      .parent_id = parent_id,
      .before_id = before_id,
      .is_selector = false,
      .selected = false,
  };

  switch (op_type) {
  case ptk_anm2_op_reset:
    // Document reset: clear all selection and request full rebuild
    anm2_selection_clear(edit->selection);
    event.op = ptk_anm2_edit_view_treeview_rebuild;
    notify_view(edit, &event);
    // Notify selection cleared
    event.op = ptk_anm2_edit_view_treeview_select;
    event.id = 0;
    notify_view(edit, &event);
    event.op = ptk_anm2_edit_view_detail_refresh;
    notify_view(edit, &event);
    break;

  case ptk_anm2_op_selector_insert:
    event.op = ptk_anm2_edit_view_treeview_insert_selector;
    event.is_selector = true;
    notify_view(edit, &event);
    break;

  case ptk_anm2_op_selector_remove:
    // Remove operations may invalidate selected items
    anm2_selection_refresh(edit->selection);
    event.op = ptk_anm2_edit_view_treeview_remove_selector;
    event.is_selector = true;
    notify_view(edit, &event);
    // Notify selection may have changed
    event.op = ptk_anm2_edit_view_treeview_select;
    notify_view(edit, &event);
    event.op = ptk_anm2_edit_view_detail_refresh;
    notify_view(edit, &event);
    break;

  case ptk_anm2_op_selector_set_name:
    event.op = ptk_anm2_edit_view_treeview_update_selector;
    event.is_selector = true;
    notify_view(edit, &event);
    break;

  case ptk_anm2_op_selector_move:
    event.op = ptk_anm2_edit_view_treeview_move_selector;
    event.is_selector = true;
    notify_view(edit, &event);
    break;

  case ptk_anm2_op_item_insert:
    event.op = ptk_anm2_edit_view_treeview_insert_item;
    notify_view(edit, &event);
    break;

  case ptk_anm2_op_item_remove:
    // Remove operations may invalidate selected items
    anm2_selection_refresh(edit->selection);
    event.op = ptk_anm2_edit_view_treeview_remove_item;
    notify_view(edit, &event);
    // Notify selection may have changed
    event.op = ptk_anm2_edit_view_treeview_select;
    notify_view(edit, &event);
    event.op = ptk_anm2_edit_view_detail_refresh;
    notify_view(edit, &event);
    break;

  case ptk_anm2_op_item_set_name:
  case ptk_anm2_op_item_set_value:
  case ptk_anm2_op_item_set_script_name:
    event.op = ptk_anm2_edit_view_treeview_update_item;
    notify_view(edit, &event);
    event.op = ptk_anm2_edit_view_detail_refresh;
    notify_view(edit, &event);
    break;

  case ptk_anm2_op_item_move:
    event.op = ptk_anm2_edit_view_treeview_move_item;
    notify_view(edit, &event);
    break;

  case ptk_anm2_op_param_insert:
  case ptk_anm2_op_param_remove:
  case ptk_anm2_op_param_set_key:
  case ptk_anm2_op_param_set_value:
    // Param changes only affect detail view
    event.op = ptk_anm2_edit_view_detail_refresh;
    notify_view(edit, &event);
    break;

  case ptk_anm2_op_set_label:
  case ptk_anm2_op_set_psd_path:
  case ptk_anm2_op_set_exclusive_support_default:
  case ptk_anm2_op_set_information:
  case ptk_anm2_op_set_default_character_id:
    // Document property changes only affect detail view
    event.op = ptk_anm2_edit_view_detail_refresh;
    notify_view(edit, &event);
    break;

  case ptk_anm2_op_transaction_begin:
    // Start of transaction (or end of UNDO group): increment depth
    edit->transaction_depth++;
    // When depth returns to 0 (UNDO case: was -1, now 0), emit rebuild
    if (edit->transaction_depth == 0 && edit->needs_rebuild) {
      edit->needs_rebuild = false;
      event.op = ptk_anm2_edit_view_treeview_rebuild;
      notify_view(edit, &event);
      event.op = ptk_anm2_edit_view_detail_refresh;
      notify_view(edit, &event);
    }
    break;

  case ptk_anm2_op_transaction_end:
    // End of transaction (or start of UNDO group): decrement depth
    edit->transaction_depth--;
    // When depth returns to 0 (normal case: was 1, now 0), emit rebuild
    if (edit->transaction_depth == 0 && edit->needs_rebuild) {
      edit->needs_rebuild = false;
      event.op = ptk_anm2_edit_view_treeview_rebuild;
      notify_view(edit, &event);
      event.op = ptk_anm2_edit_view_detail_refresh;
      notify_view(edit, &event);
    }
    break;
  }
}

NODISCARD bool ptk_anm2_edit_rename_selector(struct ptk_anm2_edit *edit,
                                             uint32_t selector_id,
                                             char const *new_name,
                                             struct ov_error *err) {
  if (!edit || !edit->doc || !new_name) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_invalid_argument);
    return false;
  }
  size_t sel_idx = 0;
  if (!ptk_anm2_find_selector(edit->doc, selector_id, &sel_idx)) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_invalid_argument);
    return false;
  }
  char const *current = ptk_anm2_selector_get_name(edit->doc, selector_id);
  if (strcmp(new_name, current ? current : "") == 0) {
    return true;
  }
  if (!ptk_anm2_selector_set_name(edit->doc, selector_id, new_name, err)) {
    OV_ERROR_ADD_TRACE(err);
    return false;
  }
  return true;
}

NODISCARD bool ptk_anm2_edit_move_selector(struct ptk_anm2_edit *edit,
                                           uint32_t dragged_selector_id,
                                           uint32_t dropped_on_selector_id,
                                           bool insert_after,
                                           struct ov_error *err) {
  if (!edit || !edit->doc) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_invalid_argument);
    return false;
  }
  if (dragged_selector_id == dropped_on_selector_id) {
    return true;
  }

  // If insert_after, we need to find the next selector and insert before it.
  // If there's no next selector, insert at the end (using before_id = 0).
  uint32_t before_id = dropped_on_selector_id;
  if (insert_after) {
    size_t dropped_idx = 0;
    if (!ptk_anm2_find_selector(edit->doc, dropped_on_selector_id, &dropped_idx)) {
      OV_ERROR_SET_GENERIC(err, ov_error_generic_invalid_argument);
      return false;
    }
    size_t const sel_count = ptk_anm2_selector_count(edit->doc);
    if (dropped_idx + 1 < sel_count) {
      before_id = ptk_anm2_selector_get_id(edit->doc, dropped_idx + 1);
    } else {
      before_id = 0; // Append at end
    }
  }

  // Move the dragged selector before the target
  if (!ptk_anm2_selector_move(edit->doc, dragged_selector_id, before_id, err)) {
    OV_ERROR_ADD_TRACE(err);
    return false;
  }
  return true;
}

static bool reverse_items_in_transaction(
    struct ptk_anm2 *doc, uint32_t const *ids, size_t items_len, uint32_t selector_id, struct ov_error *err) {
  // To reverse: process items from last to first, moving each to the end
  // Original: A, B, C (ids = [A, B, C])
  // Move C to end: A, B, C (no change since C is already last)
  // Move B to end: A, C, B
  // Move A to end: C, B, A (reversed)
  for (size_t i = items_len; i > 0; i--) {
    // Move each item to the end of the selector (before_id = selector_id means end)
    if (!ptk_anm2_item_move(doc, ids[i - 1], selector_id, err)) {
      OV_ERROR_ADD_TRACE(err);
      return false;
    }
  }
  return true;
}

NODISCARD bool ptk_anm2_edit_reverse_focus_selector(struct ptk_anm2_edit *edit, struct ov_error *err) {
  if (!edit || !edit->doc) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_invalid_argument);
    return false;
  }
  struct anm2_selection_state state = {0};
  anm2_selection_get_state(edit->selection, &state);
  if (state.focus_type == anm2_selection_focus_none) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_invalid_argument);
    return false;
  }
  size_t sel_idx = 0;
  if (state.focus_type == anm2_selection_focus_selector) {
    if (!ptk_anm2_find_selector(edit->doc, state.focus_id, &sel_idx)) {
      OV_ERROR_SET_GENERIC(err, ov_error_generic_invalid_argument);
      return false;
    }
  } else if (state.focus_type == anm2_selection_focus_item) {
    size_t item_idx = 0;
    if (!ptk_anm2_find_item(edit->doc, state.focus_id, &sel_idx, &item_idx)) {
      OV_ERROR_SET_GENERIC(err, ov_error_generic_invalid_argument);
      return false;
    }
  }
  uint32_t const selector_id = ptk_anm2_selector_get_id(edit->doc, sel_idx);
  size_t const items_len = ptk_anm2_item_count(edit->doc, selector_id);
  if (items_len < 2) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_invalid_argument);
    return false;
  }

  uint32_t *ids = NULL;
  bool success = false;
  bool in_transaction = false;

  if (!OV_REALLOC(&ids, items_len, sizeof(ids[0]))) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_out_of_memory);
    goto cleanup;
  }
  for (size_t i = 0; i < items_len; i++) {
    ids[i] = ptk_anm2_item_get_id(edit->doc, sel_idx, i);
  }

  if (!ptk_anm2_begin_transaction(edit->doc, err)) {
    OV_ERROR_ADD_TRACE(err);
    goto cleanup;
  }
  in_transaction = true;

  if (!reverse_items_in_transaction(edit->doc, ids, items_len, selector_id, err)) {
    OV_ERROR_ADD_TRACE(err);
    goto cleanup;
  }

  success = true;

cleanup:
  if (ids) {
    OV_FREE(&ids);
  }
  if (in_transaction) {
    if (!ptk_anm2_end_transaction(edit->doc, success ? err : NULL)) {
      if (success) {
        OV_ERROR_ADD_TRACE(err);
        success = false;
      }
    }
  }
  return success;
}

NODISCARD bool
ptk_anm2_edit_rename_item(struct ptk_anm2_edit *edit, uint32_t item_id, char const *new_name, struct ov_error *err) {
  if (!edit || !edit->doc || !new_name) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_invalid_argument);
    return false;
  }
  char const *current = ptk_anm2_item_get_name(edit->doc, item_id);
  if (!current) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_invalid_argument);
    return false;
  }
  if (strcmp(new_name, current) == 0) {
    return true;
  }
  if (!ptk_anm2_item_set_name(edit->doc, item_id, new_name, err)) {
    OV_ERROR_ADD_TRACE(err);
    return false;
  }
  return true;
}

NODISCARD bool ptk_anm2_edit_set_item_value(struct ptk_anm2_edit *edit,
                                            uint32_t item_id,
                                            char const *new_value,
                                            struct ov_error *err) {
  if (!edit || !edit->doc || !new_value) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_invalid_argument);
    return false;
  }
  char const *current = ptk_anm2_item_get_value(edit->doc, item_id);
  if (!current) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_invalid_argument);
    return false;
  }
  if (strcmp(new_value, current) == 0) {
    return true;
  }
  if (!ptk_anm2_item_set_value(edit->doc, item_id, new_value, err)) {
    OV_ERROR_ADD_TRACE(err);
    return false;
  }
  return true;
}

NODISCARD bool ptk_anm2_edit_delete_selected(struct ptk_anm2_edit *edit, struct ov_error *err) {
  if (!edit || !edit->doc) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_invalid_argument);
    return false;
  }
  struct anm2_selection_state state = {0};
  anm2_selection_get_state(edit->selection, &state);

  size_t count = 0;
  uint32_t const *selected = anm2_selection_get_selected_ids(edit->selection, &count);

  if (state.focus_type == anm2_selection_focus_selector && count <= 1) {
    if (!ptk_anm2_selector_remove(edit->doc, state.focus_id, err)) {
      OV_ERROR_ADD_TRACE(err);
      return false;
    }
    anm2_selection_clear(edit->selection);
    return true;
  }

  if (count == 0) {
    return true;
  }

  if (!ptk_anm2_begin_transaction(edit->doc, err)) {
    OV_ERROR_ADD_TRACE(err);
    return false;
  }

  bool success = false;
  for (size_t i = 0; i < count; i++) {
    if (!ptk_anm2_item_remove(edit->doc, selected[i], err)) {
      OV_ERROR_ADD_TRACE(err);
      goto cleanup;
    }
  }
  success = true;

cleanup:
  if (!ptk_anm2_end_transaction(edit->doc, success ? err : NULL)) {
    if (success) {
      OV_ERROR_ADD_TRACE(err);
      success = false;
    }
  }
  if (success) {
    anm2_selection_clear(edit->selection);
  }
  return success;
}

static bool collect_sorted_items(struct ptk_anm2 *doc,
                                 uint32_t const *item_ids,
                                 size_t item_count,
                                 struct drag_item_pos **out_items,
                                 size_t *out_count,
                                 struct ov_error *err) {
  struct drag_item_pos *sorted_items = NULL;
  size_t sorted_count = 0;
  for (size_t i = 0; i < item_count; ++i) {
    size_t sel_idx = 0;
    size_t item_idx = 0;
    if (!ptk_anm2_find_item(doc, item_ids[i], &sel_idx, &item_idx)) {
      continue;
    }
    if (!OV_REALLOC(&sorted_items, sorted_count + 1, sizeof(sorted_items[0]))) {
      OV_ERROR_SET_GENERIC(err, ov_error_generic_out_of_memory);
      OV_FREE(&sorted_items);
      return false;
    }
    sorted_items[sorted_count++] = (struct drag_item_pos){.id = item_ids[i], .sel_idx = sel_idx, .item_idx = item_idx};
  }
  ov_qsort(sorted_items, sorted_count, sizeof(sorted_items[0]), compare_drag_item_pos, NULL);
  *out_items = sorted_items;
  *out_count = sorted_count;
  return true;
}

// Helper: compute before_id for item move
static uint32_t compute_item_move_before_id(struct ptk_anm2 const *doc,
                                            uint32_t dropped_on_id,
                                            bool dropped_on_is_selector,
                                            bool insert_after) {
  if (dropped_on_is_selector) {
    return dropped_on_id; // Append at end
  }

  size_t dst_sel = 0;
  size_t dst_item_idx = 0;
  if (!ptk_anm2_find_item(doc, dropped_on_id, &dst_sel, &dst_item_idx)) {
    return 0;
  }
  uint32_t dst_selector_id = ptk_anm2_selector_get_id(doc, dst_sel);

  if (insert_after) {
    size_t const item_count_in_sel = ptk_anm2_item_count(doc, dst_selector_id);
    if (dst_item_idx + 1 < item_count_in_sel) {
      return ptk_anm2_item_get_id(doc, dst_sel, dst_item_idx + 1);
    } else {
      return dst_selector_id; // Append at end
    }
  } else {
    return dropped_on_id;
  }
}

NODISCARD bool ptk_anm2_edit_move_items(struct ptk_anm2_edit *edit,
                                        uint32_t const *item_ids,
                                        size_t item_count,
                                        uint32_t dropped_on_id,
                                        bool dropped_on_is_selector,
                                        bool insert_after,
                                        struct ov_error *err) {
  if (!edit || !edit->doc) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_invalid_argument);
    return false;
  }
  if (!item_ids || item_count == 0) {
    return true;
  }

  // Early check: would any move actually happen?
  // This handles all cases including dropping within selection range
  if (!ptk_anm2_edit_would_move_items(
          edit, item_ids, item_count, dropped_on_id, dropped_on_is_selector, insert_after)) {
    return true; // No-op, but not an error
  }

  size_t dst_sel = 0;
  uint32_t dst_selector_id = 0;
  uint32_t before_id = 0;

  if (dropped_on_is_selector) {
    if (!ptk_anm2_find_selector(edit->doc, dropped_on_id, &dst_sel)) {
      OV_ERROR_SET_GENERIC(err, ov_error_generic_invalid_argument);
      return false;
    }
    dst_selector_id = dropped_on_id;
    before_id = dst_selector_id; // Append at end (insert_after is irrelevant for selector drop)
  } else {
    size_t dst_item_idx = 0;
    if (!ptk_anm2_find_item(edit->doc, dropped_on_id, &dst_sel, &dst_item_idx)) {
      OV_ERROR_SET_GENERIC(err, ov_error_generic_invalid_argument);
      return false;
    }
    dst_selector_id = ptk_anm2_selector_get_id(edit->doc, dst_sel);

    if (insert_after) {
      // Insert after dropped_on = before the next item, or at end
      size_t const item_count_in_sel = ptk_anm2_item_count(edit->doc, dst_selector_id);
      if (dst_item_idx + 1 < item_count_in_sel) {
        before_id = ptk_anm2_item_get_id(edit->doc, dst_sel, dst_item_idx + 1);
      } else {
        before_id = dst_selector_id; // Append at end
      }
    } else {
      // Insert before dropped_on
      before_id = dropped_on_id;
    }
  }

  struct drag_item_pos *sorted_items = NULL;
  size_t sorted_count = 0;
  bool success = false;
  bool in_transaction = false;

  if (!collect_sorted_items(edit->doc, item_ids, item_count, &sorted_items, &sorted_count, err)) {
    OV_ERROR_ADD_TRACE(err);
    goto cleanup;
  }

  if (!ptk_anm2_begin_transaction(edit->doc, err)) {
    OV_ERROR_ADD_TRACE(err);
    goto cleanup;
  }
  in_transaction = true;

  // Move items in order, each inserted before `before_id`
  for (size_t i = 0; i < sorted_count; ++i) {
    uint32_t const id = sorted_items[i].id;
    if (!ptk_anm2_item_move(edit->doc, id, before_id, err)) {
      OV_ERROR_ADD_TRACE(err);
      goto cleanup;
    }
  }

  success = true;

cleanup:
  if (sorted_items) {
    OV_FREE(&sorted_items);
  }
  if (in_transaction) {
    if (!ptk_anm2_end_transaction(edit->doc, success ? err : NULL)) {
      if (success) {
        OV_ERROR_ADD_TRACE(err);
        success = false;
      }
    }
  }
  if (success) {
    if (!anm2_selection_replace_selected_items(edit->selection, item_ids, item_count, item_ids[0], item_ids[0], err)) {
      OV_ERROR_ADD_TRACE(err);
      success = false;
    }
  }
  return success;
}

bool ptk_anm2_edit_would_move_items(struct ptk_anm2_edit const *edit,
                                    uint32_t const *item_ids,
                                    size_t item_count,
                                    uint32_t dropped_on_id,
                                    bool dropped_on_is_selector,
                                    bool insert_after) {
  if (!edit || !edit->doc || !item_ids || item_count == 0) {
    return false;
  }

  // If dropping on an item (not a selector), check if drop would cause confusing reordering
  if (!dropped_on_is_selector) {
    // Get destination selector and position
    size_t dst_sel_idx = 0;
    size_t dst_item_idx = 0;
    if (!ptk_anm2_find_item(edit->doc, dropped_on_id, &dst_sel_idx, &dst_item_idx)) {
      return false;
    }
    size_t drop_pos = insert_after ? dst_item_idx + 1 : dst_item_idx;

    // Find min/max indices of selected items in the same selector
    size_t min_sel_idx = SIZE_MAX;
    size_t max_sel_idx = 0;
    bool any_in_same_selector = false;

    for (size_t i = 0; i < item_count; ++i) {
      size_t sel_idx = 0;
      size_t item_idx = 0;
      if (ptk_anm2_find_item(edit->doc, item_ids[i], &sel_idx, &item_idx)) {
        if (sel_idx == dst_sel_idx) {
          any_in_same_selector = true;
          if (item_idx < min_sel_idx) {
            min_sel_idx = item_idx;
          }
          if (item_idx > max_sel_idx) {
            max_sel_idx = item_idx;
          }
        }
      }
    }

    // If any selected item is in the same selector and drop position is within
    // or immediately adjacent to the selection range, treat as no-op
    if (any_in_same_selector && drop_pos >= min_sel_idx && drop_pos <= max_sel_idx + 1) {
      return false; // No-op
    }
  }

  uint32_t before_id = compute_item_move_before_id(edit->doc, dropped_on_id, dropped_on_is_selector, insert_after);
  if (before_id == 0 && !dropped_on_is_selector) {
    return false; // Invalid drop target
  }

  // Check if any item would actually move
  for (size_t i = 0; i < item_count; ++i) {
    if (ptk_anm2_item_would_move(edit->doc, item_ids[i], before_id)) {
      return true;
    }
  }
  return false;
}

bool ptk_anm2_edit_would_move_selector(struct ptk_anm2_edit const *edit,
                                       uint32_t selector_id,
                                       uint32_t target_selector_id,
                                       bool insert_after) {
  if (!edit || !edit->doc) {
    return false;
  }

  // Compute before_id based on target and insert_after
  uint32_t before_id = 0;
  if (insert_after) {
    // Insert after target = before the next selector, or 0 for end
    size_t target_idx = 0;
    if (ptk_anm2_find_selector(edit->doc, target_selector_id, &target_idx)) {
      size_t const sel_count = ptk_anm2_selector_count(edit->doc);
      if (target_idx + 1 < sel_count) {
        before_id = ptk_anm2_selector_get_id(edit->doc, target_idx + 1);
      } else {
        before_id = 0; // End
      }
    }
  } else {
    before_id = target_selector_id;
  }

  return ptk_anm2_selector_would_move(edit->doc, selector_id, before_id);
}

NODISCARD bool ptk_anm2_edit_add_selector(struct ptk_anm2_edit *edit, char const *name, struct ov_error *err) {
  if (!edit || !edit->doc) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_invalid_argument);
    return false;
  }
  // Insert at end (before_id = 0)
  if (!ptk_anm2_selector_insert(edit->doc, 0, name ? name : "", err)) {
    OV_ERROR_ADD_TRACE(err);
    return false;
  }
  return true;
}

NODISCARD bool ptk_anm2_edit_add_value_item_to_selector(
    struct ptk_anm2_edit *edit, uint32_t selector_id, char const *name, char const *value, struct ov_error *err) {
  if (!edit || !edit->doc) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_invalid_argument);
    return false;
  }
  // Insert at end of selector (before_id = selector_id)
  if (!ptk_anm2_item_insert_value(edit->doc, selector_id, name ? name : "", value ? value : "", err)) {
    OV_ERROR_ADD_TRACE(err);
    return false;
  }
  return true;
}

NODISCARD bool ptk_anm2_edit_insert_animation_item(struct ptk_anm2_edit *edit,
                                                   uint32_t before_id,
                                                   char const *script_name,
                                                   char const *display_name,
                                                   struct ov_error *err) {
  if (!edit || !edit->doc) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_invalid_argument);
    return false;
  }
  if (!ptk_anm2_item_insert_animation(edit->doc, before_id, script_name, display_name, err)) {
    OV_ERROR_ADD_TRACE(err);
    return false;
  }
  return true;
}

NODISCARD bool ptk_anm2_edit_param_add(
    struct ptk_anm2_edit *edit, uint32_t item_id, char const *key, char const *value, struct ov_error *err) {
  if (!edit || !edit->doc) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_invalid_argument);
    return false;
  }
  // Insert at end (before_param_id = 0)
  if (ptk_anm2_param_insert(edit->doc, item_id, 0, key ? key : "", value ? value : "", err) == 0) {
    OV_ERROR_ADD_TRACE(err);
    return false;
  }
  return true;
}

NODISCARD bool ptk_anm2_edit_param_add_for_focus(struct ptk_anm2_edit *edit, char const *key, struct ov_error *err) {
  if (!edit || !edit->doc) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_invalid_argument);
    return false;
  }
  if (!key || key[0] == '\0') {
    return true;
  }
  struct anm2_selection_state state = {0};
  anm2_selection_get_state(edit->selection, &state);
  if (state.focus_type != anm2_selection_focus_item) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_invalid_argument);
    return false;
  }
  if (!ptk_anm2_item_is_animation(edit->doc, state.focus_id)) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_invalid_argument);
    return false;
  }
  // Insert at end (before_param_id = 0)
  if (ptk_anm2_param_insert(edit->doc, state.focus_id, 0, key, "", err) == 0) {
    OV_ERROR_ADD_TRACE(err);
    return false;
  }
  return true;
}

NODISCARD bool ptk_anm2_edit_param_remove(struct ptk_anm2_edit *edit, uint32_t param_id, struct ov_error *err) {
  if (!edit || !edit->doc) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_invalid_argument);
    return false;
  }
  if (!ptk_anm2_param_remove(edit->doc, param_id, err)) {
    OV_ERROR_ADD_TRACE(err);
    return false;
  }
  return true;
}

NODISCARD bool
ptk_anm2_edit_param_set_key(struct ptk_anm2_edit *edit, uint32_t param_id, char const *value, struct ov_error *err) {
  if (!edit || !edit->doc || !value) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_invalid_argument);
    return false;
  }
  char const *current = ptk_anm2_param_get_key(edit->doc, param_id);
  if (strcmp(value, current ? current : "") == 0) {
    return true;
  }
  if (!ptk_anm2_param_set_key(edit->doc, param_id, value, err)) {
    OV_ERROR_ADD_TRACE(err);
    return false;
  }
  return true;
}

NODISCARD bool
ptk_anm2_edit_param_set_value(struct ptk_anm2_edit *edit, uint32_t param_id, char const *value, struct ov_error *err) {
  if (!edit || !edit->doc || !value) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_invalid_argument);
    return false;
  }
  char const *current = ptk_anm2_param_get_value(edit->doc, param_id);
  if (strcmp(value, current ? current : "") == 0) {
    return true;
  }
  if (!ptk_anm2_param_set_value(edit->doc, param_id, value, err)) {
    OV_ERROR_ADD_TRACE(err);
    return false;
  }
  return true;
}

NODISCARD bool ptk_anm2_edit_set_label(struct ptk_anm2_edit *edit, char const *label, struct ov_error *err) {
  if (!edit || !edit->doc) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_invalid_argument);
    return false;
  }
  char const *current = ptk_anm2_get_label(edit->doc);
  char const *value = label ? label : "";
  if (strcmp(value, current ? current : "") == 0) {
    return true;
  }
  if (!ptk_anm2_set_label(edit->doc, value, err)) {
    OV_ERROR_ADD_TRACE(err);
    return false;
  }
  return true;
}

NODISCARD bool ptk_anm2_edit_set_psd_path(struct ptk_anm2_edit *edit, char const *path, struct ov_error *err) {
  if (!edit || !edit->doc) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_invalid_argument);
    return false;
  }
  char const *current = ptk_anm2_get_psd_path(edit->doc);
  char const *value = path ? path : "";
  if (strcmp(value, current ? current : "") == 0) {
    return true;
  }
  if (!ptk_anm2_set_psd_path(edit->doc, value, err)) {
    OV_ERROR_ADD_TRACE(err);
    return false;
  }
  return true;
}

NODISCARD bool
ptk_anm2_edit_set_exclusive_support_default(struct ptk_anm2_edit *edit, bool value, struct ov_error *err) {
  if (!edit || !edit->doc) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_invalid_argument);
    return false;
  }
  if (value == ptk_anm2_get_exclusive_support_default(edit->doc)) {
    return true;
  }
  if (!ptk_anm2_set_exclusive_support_default(edit->doc, value, err)) {
    OV_ERROR_ADD_TRACE(err);
    return false;
  }
  return true;
}

NODISCARD bool ptk_anm2_edit_set_information(struct ptk_anm2_edit *edit, char const *info, struct ov_error *err) {
  if (!edit || !edit->doc) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_invalid_argument);
    return false;
  }
  char const *current = ptk_anm2_get_information(edit->doc);
  char const *value = (info && info[0] != '\0') ? info : NULL;
  if ((value == NULL && current == NULL) || (value && current && strcmp(value, current) == 0)) {
    return true;
  }
  if (!ptk_anm2_set_information(edit->doc, value, err)) {
    OV_ERROR_ADD_TRACE(err);
    return false;
  }
  return true;
}

NODISCARD bool
ptk_anm2_edit_set_default_character_id(struct ptk_anm2_edit *edit, char const *char_id, struct ov_error *err) {
  if (!edit || !edit->doc) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_invalid_argument);
    return false;
  }
  char const *current = ptk_anm2_get_default_character_id(edit->doc);
  char const *value = (char_id && char_id[0] != '\0') ? char_id : NULL;
  if ((value == NULL && current == NULL) || (value && current && strcmp(value, current) == 0)) {
    return true;
  }
  if (!ptk_anm2_set_default_character_id(edit->doc, value, err)) {
    OV_ERROR_ADD_TRACE(err);
    return false;
  }
  return true;
}

char const *ptk_anm2_edit_get_label(struct ptk_anm2_edit const *edit) {
  if (!edit || !edit->doc) {
    return NULL;
  }
  return ptk_anm2_get_label(edit->doc);
}

char const *ptk_anm2_edit_get_information(struct ptk_anm2_edit const *edit) {
  if (!edit || !edit->doc) {
    return NULL;
  }
  return ptk_anm2_get_information(edit->doc);
}

char const *ptk_anm2_edit_get_psd_path(struct ptk_anm2_edit const *edit) {
  if (!edit || !edit->doc) {
    return NULL;
  }
  return ptk_anm2_get_psd_path(edit->doc);
}

bool ptk_anm2_edit_get_exclusive_support_default(struct ptk_anm2_edit const *edit) {
  if (!edit || !edit->doc) {
    return false;
  }
  return ptk_anm2_get_exclusive_support_default(edit->doc);
}

char const *ptk_anm2_edit_get_default_character_id(struct ptk_anm2_edit const *edit) {
  if (!edit || !edit->doc) {
    return NULL;
  }
  return ptk_anm2_get_default_character_id(edit->doc);
}

size_t ptk_anm2_edit_selector_count(struct ptk_anm2_edit const *edit) {
  if (!edit || !edit->doc) {
    return 0;
  }
  return ptk_anm2_selector_count(edit->doc);
}

size_t ptk_anm2_edit_item_count(struct ptk_anm2_edit const *edit, uint32_t selector_id) {
  if (!edit || !edit->doc) {
    return 0;
  }
  return ptk_anm2_item_count(edit->doc, selector_id);
}

uint32_t ptk_anm2_edit_selector_get_id(struct ptk_anm2_edit const *edit, size_t sel_idx) {
  if (!edit || !edit->doc) {
    return 0;
  }
  return ptk_anm2_selector_get_id(edit->doc, sel_idx);
}

uint32_t ptk_anm2_edit_item_get_id(struct ptk_anm2_edit const *edit, size_t sel_idx, size_t item_idx) {
  if (!edit || !edit->doc) {
    return 0;
  }
  return ptk_anm2_item_get_id(edit->doc, sel_idx, item_idx);
}

char const *ptk_anm2_edit_selector_get_name(struct ptk_anm2_edit const *edit, uint32_t selector_id) {
  if (!edit || !edit->doc) {
    return NULL;
  }
  return ptk_anm2_selector_get_name(edit->doc, selector_id);
}

char const *ptk_anm2_edit_item_get_name(struct ptk_anm2_edit const *edit, uint32_t item_id) {
  if (!edit || !edit->doc) {
    return NULL;
  }
  return ptk_anm2_item_get_name(edit->doc, item_id);
}

char const *ptk_anm2_edit_item_get_value(struct ptk_anm2_edit const *edit, uint32_t item_id) {
  if (!edit || !edit->doc) {
    return NULL;
  }
  return ptk_anm2_item_get_value(edit->doc, item_id);
}

bool ptk_anm2_edit_item_is_animation(struct ptk_anm2_edit const *edit, uint32_t item_id) {
  if (!edit || !edit->doc) {
    return false;
  }
  return ptk_anm2_item_is_animation(edit->doc, item_id);
}

bool ptk_anm2_edit_find_selector(struct ptk_anm2_edit const *edit, uint32_t id, size_t *out_sel_idx) {
  if (!edit || !edit->doc) {
    return false;
  }
  return ptk_anm2_find_selector(edit->doc, id, out_sel_idx);
}

bool ptk_anm2_edit_find_item(struct ptk_anm2_edit const *edit, uint32_t id, size_t *out_sel_idx, size_t *out_item_idx) {
  if (!edit || !edit->doc) {
    return false;
  }
  return ptk_anm2_find_item(edit->doc, id, out_sel_idx, out_item_idx);
}

size_t ptk_anm2_edit_param_count(struct ptk_anm2_edit const *edit, uint32_t item_id) {
  if (!edit || !edit->doc) {
    return 0;
  }
  return ptk_anm2_param_count(edit->doc, item_id);
}

uint32_t
ptk_anm2_edit_param_get_id(struct ptk_anm2_edit const *edit, size_t sel_idx, size_t item_idx, size_t param_idx) {
  if (!edit || !edit->doc) {
    return 0;
  }
  return ptk_anm2_param_get_id(edit->doc, sel_idx, item_idx, param_idx);
}

char const *ptk_anm2_edit_param_get_key(struct ptk_anm2_edit const *edit, uint32_t param_id) {
  if (!edit || !edit->doc) {
    return NULL;
  }
  return ptk_anm2_param_get_key(edit->doc, param_id);
}

char const *ptk_anm2_edit_param_get_value(struct ptk_anm2_edit const *edit, uint32_t param_id) {
  if (!edit || !edit->doc) {
    return NULL;
  }
  return ptk_anm2_param_get_value(edit->doc, param_id);
}

uintptr_t ptk_anm2_edit_selector_get_userdata(struct ptk_anm2_edit const *edit, uint32_t selector_id) {
  if (!edit || !edit->doc) {
    return 0;
  }
  return ptk_anm2_selector_get_userdata(edit->doc, selector_id);
}

void ptk_anm2_edit_selector_set_userdata(struct ptk_anm2_edit *edit, uint32_t selector_id, uintptr_t data) {
  if (!edit || !edit->doc) {
    return;
  }
  ptk_anm2_selector_set_userdata(edit->doc, selector_id, data);
}

bool ptk_anm2_edit_can_undo(struct ptk_anm2_edit const *edit) {
  if (!edit || !edit->doc) {
    return false;
  }
  return ptk_anm2_can_undo(edit->doc);
}

bool ptk_anm2_edit_can_redo(struct ptk_anm2_edit const *edit) {
  if (!edit || !edit->doc) {
    return false;
  }
  return ptk_anm2_can_redo(edit->doc);
}

bool ptk_anm2_edit_can_save(struct ptk_anm2_edit const *edit) {
  if (!edit || !edit->doc) {
    return false;
  }
  return ptk_anm2_can_save(edit->doc);
}

NODISCARD bool ptk_anm2_edit_undo(struct ptk_anm2_edit *edit, struct ov_error *err) {
  if (!edit || !edit->doc) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_invalid_argument);
    return false;
  }
  if (!ptk_anm2_can_undo(edit->doc)) {
    return true;
  }
  // Notify views to save transient UI state before undo
  notify_view(edit, &(struct ptk_anm2_edit_view_event){.op = ptk_anm2_edit_view_before_undo_redo});
  // ptk_anm2_undo will fire change_callback for each operation.
  // For grouped operations: GROUP_END first (depth--), then ops, then GROUP_BEGIN (depth++, rebuild).
  // For single operations: just the op (differential update).
  if (!ptk_anm2_undo(edit->doc, err)) {
    OV_ERROR_ADD_TRACE(err);
    return false;
  }
  // Refresh selection to remove references to deleted items while preserving valid selections
  ptk_anm2_edit_refresh_selection(edit);
  return true;
}

NODISCARD bool ptk_anm2_edit_redo(struct ptk_anm2_edit *edit, struct ov_error *err) {
  if (!edit || !edit->doc) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_invalid_argument);
    return false;
  }
  if (!ptk_anm2_can_redo(edit->doc)) {
    return true;
  }
  // Notify views to save transient UI state before redo
  notify_view(edit, &(struct ptk_anm2_edit_view_event){.op = ptk_anm2_edit_view_before_undo_redo});
  // ptk_anm2_redo will fire change_callback for each operation.
  // Redo stack has ops in reverse order, so: GROUP_END first (depth--), then ops, then GROUP_BEGIN (depth++, rebuild).
  // For single operations: just the op (differential update).
  if (!ptk_anm2_redo(edit->doc, err)) {
    OV_ERROR_ADD_TRACE(err);
    return false;
  }
  // Refresh selection to remove references to deleted items while preserving valid selections
  ptk_anm2_edit_refresh_selection(edit);
  return true;
}

NODISCARD bool ptk_anm2_edit_begin_transaction(struct ptk_anm2_edit *edit, struct ov_error *err) {
  if (!edit || !edit->doc) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_invalid_argument);
    return false;
  }
  if (!ptk_anm2_begin_transaction(edit->doc, err)) {
    OV_ERROR_ADD_TRACE(err);
    return false;
  }
  return true;
}

NODISCARD bool ptk_anm2_edit_end_transaction(struct ptk_anm2_edit *edit, bool success, struct ov_error *err) {
  if (!edit || !edit->doc) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_invalid_argument);
    return false;
  }
  if (!ptk_anm2_end_transaction(edit->doc, success ? err : NULL)) {
    if (success) {
      OV_ERROR_ADD_TRACE(err);
    }
    return false;
  }
  return true;
}

ov_tribool ptk_anm2_edit_verify_file_checksum(wchar_t const *path, struct ov_error *err) {
  if (!path) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_invalid_argument);
    return ov_indeterminate;
  }
  struct ptk_anm2 *temp_doc = ptk_anm2_create(err);
  if (!temp_doc) {
    OV_ERROR_ADD_TRACE(err);
    return ov_indeterminate;
  }
  ov_tribool result = ov_indeterminate;
  if (!ptk_anm2_load(temp_doc, path, err)) {
    OV_ERROR_ADD_TRACE(err);
    goto cleanup;
  }
  result = ptk_anm2_verify_checksum(temp_doc) ? ov_true : ov_false;
cleanup:
  ptk_anm2_destroy(&temp_doc);
  return result;
}

NODISCARD bool ptk_anm2_edit_load(struct ptk_anm2_edit *edit, wchar_t const *path, struct ov_error *err) {
  if (!edit || !edit->doc || !path) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_invalid_argument);
    return false;
  }
  if (!ptk_anm2_load(edit->doc, path, err)) {
    OV_ERROR_ADD_TRACE(err);
    return false;
  }
  anm2_selection_clear(edit->selection);
  notify_view(edit, &(struct ptk_anm2_edit_view_event){.op = ptk_anm2_edit_view_treeview_rebuild});
  notify_view(edit, &(struct ptk_anm2_edit_view_event){.op = ptk_anm2_edit_view_detail_refresh});
  notify_state_changes(edit);
  return true;
}

NODISCARD bool ptk_anm2_edit_save(struct ptk_anm2_edit *edit, wchar_t const *path, struct ov_error *err) {
  if (!edit || !edit->doc || !path) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_invalid_argument);
    return false;
  }
  if (!ptk_anm2_save(edit->doc, path, err)) {
    OV_ERROR_ADD_TRACE(err);
    return false;
  }
  // ptk_anm2_save clears modified flag internally
  notify_state_changes(edit);
  return true;
}

NODISCARD bool ptk_anm2_edit_reset(struct ptk_anm2_edit *edit, struct ov_error *err) {
  if (!edit || !edit->doc) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_invalid_argument);
    return false;
  }
  if (!ptk_anm2_reset(edit->doc, err)) {
    OV_ERROR_ADD_TRACE(err);
    return false;
  }
  anm2_selection_clear(edit->selection);
  notify_view(edit, &(struct ptk_anm2_edit_view_event){.op = ptk_anm2_edit_view_treeview_rebuild});
  notify_view(edit, &(struct ptk_anm2_edit_view_event){.op = ptk_anm2_edit_view_detail_refresh});
  notify_state_changes(edit);
  return true;
}

bool ptk_anm2_edit_verify_checksum(struct ptk_anm2_edit const *edit) {
  if (!edit || !edit->doc) {
    return false;
  }
  return ptk_anm2_verify_checksum(edit->doc);
}

void ptk_anm2_edit_format_selector_display_name(struct ptk_anm2_edit const *edit,
                                                uint32_t selector_id,
                                                wchar_t *out,
                                                size_t out_len) {
  if (!edit || !edit->doc || !out || out_len == 0) {
    return;
  }
  char const *group = selector_id ? ptk_anm2_selector_get_name(edit->doc, selector_id) : NULL;
  if (!group || group[0] == '\0') {
    group = pgettext("anm2editor", "(Unnamed Selector)");
  }
  ov_snprintf_char2wchar(out, out_len, "%hs", "%hs", group);
}

void ptk_anm2_edit_format_item_display_name(struct ptk_anm2_edit const *edit,
                                            uint32_t item_id,
                                            wchar_t *out,
                                            size_t out_len) {
  if (!edit || !edit->doc || !out || out_len == 0) {
    return;
  }
  char const *name = ptk_anm2_item_get_name(edit->doc, item_id);
  if (!name || name[0] == '\0') {
    name = pgettext("anm2editor", "(Unnamed Item)");
  }
  if (ptk_anm2_item_is_animation(edit->doc, item_id)) {
    char const *script_name = ptk_anm2_item_get_script_name(edit->doc, item_id);
    ov_snprintf_char2wchar(out, out_len, "%1$hs%2$hs", "[%1$hs] %2$hs", script_name, name);
  } else {
    ov_snprintf_char2wchar(out, out_len, "%1$hs", "%1$hs", name);
  }
}

void ptk_anm2_edit_get_editable_name(
    struct ptk_anm2_edit const *edit, uint32_t id, bool is_selector, wchar_t *out, size_t out_len) {
  if (!edit || !edit->doc || !out || out_len == 0) {
    return;
  }
  char const *name = NULL;
  if (is_selector) {
    name = ptk_anm2_selector_get_name(edit->doc, id);
  } else {
    name = ptk_anm2_item_get_name(edit->doc, id);
  }
  if (name) {
    ov_utf8_to_wchar(name, strlen(name), out, out_len, NULL);
  } else {
    out[0] = L'\0';
  }
}

void ptk_anm2_edit_ptkl_targets_free(struct ptk_anm2_edit_ptkl_targets *targets) {
  if (!targets || !targets->items) {
    return;
  }
  size_t const n = OV_ARRAY_LENGTH(targets->items);
  for (size_t i = 0; i < n; i++) {
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
  *targets = (struct ptk_anm2_edit_ptkl_targets){0};
}

static bool strdup_to_array_internal(char **out, char const *src, struct ov_error *const err) {
  if (!src || src[0] == '\0') {
    *out = NULL;
    return true;
  }

  bool success = false;

  size_t const len = strlen(src);
  if (!OV_ARRAY_GROW(out, len + 1)) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_out_of_memory);
    goto cleanup;
  }
  memcpy(*out, src, len + 1);
  OV_ARRAY_SET_LENGTH(*out, len);

  success = true;

cleanup:
  return success;
}

static bool add_ptkl_target_internal(struct ptk_anm2_edit_ptkl_targets *targets,
                                     char const *selector_name,
                                     char const *effect_name,
                                     char const *param_key,
                                     uint32_t selector_id,
                                     uint32_t item_id,
                                     uint32_t param_id,
                                     struct ov_error *const err) {
  bool success = false;
  struct ptk_anm2_edit_ptkl_target *t = NULL;

  size_t const current_len = OV_ARRAY_LENGTH(targets->items);
  if (!OV_ARRAY_GROW(&targets->items, current_len + 1)) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_out_of_memory);
    goto cleanup;
  }

  t = &targets->items[current_len];
  *t = (struct ptk_anm2_edit_ptkl_target){
      .selector_id = selector_id,
      .item_id = item_id,
      .param_id = param_id,
  };

  if (!strdup_to_array_internal(&t->selector_name, selector_name, err)) {
    OV_ERROR_ADD_TRACE(err);
    goto cleanup;
  }
  if (!strdup_to_array_internal(&t->effect_name, effect_name, err)) {
    OV_ERROR_ADD_TRACE(err);
    goto cleanup;
  }
  if (!strdup_to_array_internal(&t->param_key, param_key, err)) {
    OV_ERROR_ADD_TRACE(err);
    goto cleanup;
  }

  OV_ARRAY_SET_LENGTH(targets->items, current_len + 1);
  success = true;

cleanup:
  return success;
}

bool ptk_anm2_edit_collect_ptkl_targets(struct ptk_anm2_edit *edit,
                                        struct ptk_anm2_edit_ptkl_targets *targets,
                                        struct ov_error *const err) {
  if (!edit || !targets) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_invalid_argument);
    return false;
  }

  bool success = false;
  uint32_t *item_ids = NULL;
  uint32_t *param_ids = NULL;
  *targets = (struct ptk_anm2_edit_ptkl_targets){0};

  // Get current focus
  struct ptk_anm2_edit_state state = {0};
  ptk_anm2_edit_get_state(edit, &state);

  // Need a selector to be focused (either directly or via an item)
  uint32_t selector_id = 0;
  if (state.focus_type == ptk_anm2_edit_focus_selector) {
    selector_id = state.focus_id;
  } else if (state.focus_type == ptk_anm2_edit_focus_item) {
    // Find selector containing this item
    size_t sel_idx = 0;
    size_t item_idx = 0;
    if (ptk_anm2_edit_find_item(edit, state.focus_id, &sel_idx, &item_idx)) {
      selector_id = ptk_anm2_edit_selector_get_id(edit, sel_idx);
    }
  }

  if (selector_id == 0) {
    // No selector focused - return empty result (not an error)
    success = true;
    goto cleanup;
  }

  {
    char const *group = ptk_anm2_selector_get_name(edit->doc, selector_id);

    // Get all item IDs for this selector
    item_ids = ptk_anm2_get_item_ids(edit->doc, selector_id, err);
    if (!item_ids) {
      // If selector exists but has no items, this is not an error
      if (ptk_anm2_item_count(edit->doc, selector_id) == 0) {
        OV_ERROR_DESTROY(err);
        success = true;
        goto cleanup;
      }
      OV_ERROR_ADD_TRACE(err);
      goto cleanup;
    }

    size_t const item_count = OV_ARRAY_LENGTH(item_ids);
    for (size_t i = 0; i < item_count; i++) {
      uint32_t const item_id = item_ids[i];
      if (!ptk_anm2_item_is_animation(edit->doc, item_id)) {
        continue;
      }

      char const *name = ptk_anm2_item_get_name(edit->doc, item_id);

      // Get all parameter IDs for this item
      if (param_ids) {
        OV_ARRAY_DESTROY(&param_ids);
      }
      param_ids = ptk_anm2_get_param_ids(edit->doc, item_id, err);
      if (!param_ids) {
        // If item exists but has no params, this is not an error
        if (ptk_anm2_param_count(edit->doc, item_id) == 0) {
          OV_ERROR_DESTROY(err);
          continue;
        }
        OV_ERROR_ADD_TRACE(err);
        goto cleanup;
      }

      size_t const param_count = OV_ARRAY_LENGTH(param_ids);
      for (size_t j = 0; j < param_count; j++) {
        uint32_t const param_id = param_ids[j];
        char const *key = ptk_anm2_param_get_key(edit->doc, param_id);
        if (!key) {
          continue;
        }

        // Check if key ends with "~ptkl"
        size_t const key_len = strlen(key);
        if (key_len < 5 || strcmp(key + key_len - 5, "~ptkl") != 0) {
          continue;
        }

        if (!add_ptkl_target_internal(targets, group, name, key, selector_id, item_id, param_id, err)) {
          OV_ERROR_ADD_TRACE(err);
          ptk_anm2_edit_ptkl_targets_free(targets);
          goto cleanup;
        }
      }
    }
  }

  success = true;

cleanup:
  if (item_ids) {
    OV_ARRAY_DESTROY(&item_ids);
  }
  if (param_ids) {
    OV_ARRAY_DESTROY(&param_ids);
  }
  return success;
}

bool ptk_anm2_edit_set_param_value_by_id(struct ptk_anm2_edit *edit,
                                         uint32_t param_id,
                                         char const *value,
                                         struct ov_error *const err) {
  if (!edit || !edit->doc) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_invalid_argument);
    return false;
  }

  // Use existing param_set_value which accepts param_id
  if (!ptk_anm2_edit_param_set_value(edit, param_id, value, err)) {
    OV_ERROR_ADD_TRACE(err);
    return false;
  }

  return true;
}
