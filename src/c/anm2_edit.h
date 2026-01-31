#pragma once

#include "anm2.h"
#include <ovbase.h>

struct ptk_anm2_edit;
struct ptk_anm2_script_mapper;

enum ptk_anm2_edit_focus_type {
  ptk_anm2_edit_focus_none,
  ptk_anm2_edit_focus_selector,
  ptk_anm2_edit_focus_item,
};

struct ptk_anm2_edit_state {
  uint32_t focus_id;
  uint32_t anchor_id;
  enum ptk_anm2_edit_focus_type focus_type;
};

// View update operation types
enum ptk_anm2_edit_view_op {
  // Structure change events (for differential updates)
  ptk_anm2_edit_view_treeview_rebuild,         // Full rebuild required
  ptk_anm2_edit_view_treeview_insert_selector, // id=selector_id, index=position
  ptk_anm2_edit_view_treeview_remove_selector, // id=selector_id
  ptk_anm2_edit_view_treeview_update_selector, // id=selector_id (name changed)
  ptk_anm2_edit_view_treeview_move_selector,   // id=selector_id, index=new_position
  ptk_anm2_edit_view_treeview_insert_item,     // id=item_id, parent_id=selector_id, index=position
  ptk_anm2_edit_view_treeview_remove_item,     // id=item_id
  ptk_anm2_edit_view_treeview_update_item,     // id=item_id (name changed)
  ptk_anm2_edit_view_treeview_move_item,       // id=item_id, parent_id=new_selector_id, index=new_position
  ptk_anm2_edit_view_treeview_select,          // id=item_id/selector_id, is_selector, selected
  ptk_anm2_edit_view_treeview_set_focus,       // id=item_id/selector_id, is_selector
  ptk_anm2_edit_view_treeview_group_begin,     // Start of grouped operations (disable redraw)
  ptk_anm2_edit_view_treeview_group_end,       // End of grouped operations (enable redraw)
  // Detail panel events (for differential updates)
  ptk_anm2_edit_view_detail_refresh,         // Full refresh required (selection mode changed, etc.)
  ptk_anm2_edit_view_detail_insert_param,    // id=param_id, parent_id=item_id
  ptk_anm2_edit_view_detail_remove_param,    // id=param_id, parent_id=item_id
  ptk_anm2_edit_view_detail_update_param,    // id=param_id, parent_id=item_id (key or value changed)
  ptk_anm2_edit_view_detail_update_item,     // id=item_id (name or value changed)
  ptk_anm2_edit_view_detail_item_selected,   // id=item_id (multisel: item added)
  ptk_anm2_edit_view_detail_item_deselected, // id=item_id (multisel: item removed)
  // State notification events (notifies what changed, not what to do)
  ptk_anm2_edit_view_undo_redo_state_changed, // can_undo or can_redo changed
  ptk_anm2_edit_view_modified_state_changed,  // modified flag changed
  ptk_anm2_edit_view_save_state_changed,      // can_save changed
  ptk_anm2_edit_view_before_undo_redo,        // About to perform undo/redo - save transient UI state
};

// View update event
struct ptk_anm2_edit_view_event {
  uint32_t id;        // Target element ID (item_id or selector_id)
  uint32_t parent_id; // Parent ID (selector_id for item ops, item_id for param ops)
  uint32_t before_id; // For insert/move: ID of element before which insertion occurred (0=end)
  enum ptk_anm2_edit_view_op op;
  bool is_selector;
  bool selected;
};

// View callback function type
typedef void (*ptk_anm2_edit_view_callback)(void *userdata, struct ptk_anm2_edit_view_event const *event);

// Create anm2_edit with a new empty document
NODISCARD struct ptk_anm2_edit *ptk_anm2_edit_create(struct ov_error *err);

void ptk_anm2_edit_destroy(struct ptk_anm2_edit **edit);

// Get the underlying document (read-only)
struct ptk_anm2 const *ptk_anm2_edit_get_doc(struct ptk_anm2_edit const *edit);

// Get the script mapper for translating script names to effect names
struct ptk_anm2_script_mapper const *ptk_anm2_edit_get_script_mapper(struct ptk_anm2_edit const *edit);

// Set view callback to receive differential updates
void ptk_anm2_edit_set_view_callback(struct ptk_anm2_edit *edit, ptk_anm2_edit_view_callback callback, void *userdata);

// Modified state (true if document changed since last save/load/reset)
bool ptk_anm2_edit_is_modified(struct ptk_anm2_edit const *edit);

void ptk_anm2_edit_get_state(struct ptk_anm2_edit const *edit, struct ptk_anm2_edit_state *out);
uint32_t const *ptk_anm2_edit_get_selected_item_ids(struct ptk_anm2_edit const *edit, size_t *count);
size_t ptk_anm2_edit_get_selected_item_count(struct ptk_anm2_edit const *edit);
bool ptk_anm2_edit_is_item_selected(struct ptk_anm2_edit const *edit, uint32_t item_id);

NODISCARD bool ptk_anm2_edit_apply_treeview_selection(struct ptk_anm2_edit *edit,
                                                      uint32_t item_id,
                                                      bool is_selector,
                                                      bool ctrl_pressed,
                                                      bool shift_pressed,
                                                      struct ov_error *err);

void ptk_anm2_edit_refresh_selection(struct ptk_anm2_edit *edit);
void ptk_anm2_edit_update_on_doc_op(
    struct ptk_anm2_edit *edit, enum ptk_anm2_op_type op_type, uint32_t id, uint32_t parent_id, uint32_t before_id);

NODISCARD bool ptk_anm2_edit_rename_selector(struct ptk_anm2_edit *edit,
                                             uint32_t selector_id,
                                             char const *new_name,
                                             struct ov_error *err);
NODISCARD bool ptk_anm2_edit_move_selector(struct ptk_anm2_edit *edit,
                                           uint32_t dragged_selector_id,
                                           uint32_t dropped_on_selector_id,
                                           bool insert_after,
                                           struct ov_error *err);
NODISCARD bool ptk_anm2_edit_reverse_focus_selector(struct ptk_anm2_edit *edit, struct ov_error *err);

// Direct selector/item operations (ID-based)
NODISCARD bool ptk_anm2_edit_add_selector(struct ptk_anm2_edit *edit, char const *name, struct ov_error *err);
NODISCARD bool ptk_anm2_edit_add_value_item_to_selector(
    struct ptk_anm2_edit *edit, uint32_t selector_id, char const *name, char const *value, struct ov_error *err);
NODISCARD bool ptk_anm2_edit_insert_animation_item(struct ptk_anm2_edit *edit,
                                                   uint32_t before_id,
                                                   char const *script_name,
                                                   char const *display_name,
                                                   struct ov_error *err);
NODISCARD bool ptk_anm2_edit_param_add(
    struct ptk_anm2_edit *edit, uint32_t item_id, char const *key, char const *value, struct ov_error *err);

NODISCARD bool
ptk_anm2_edit_rename_item(struct ptk_anm2_edit *edit, uint32_t item_id, char const *new_name, struct ov_error *err);
NODISCARD bool
ptk_anm2_edit_set_item_value(struct ptk_anm2_edit *edit, uint32_t item_id, char const *new_value, struct ov_error *err);

NODISCARD bool ptk_anm2_edit_delete_selected(struct ptk_anm2_edit *edit, struct ov_error *err);
NODISCARD bool ptk_anm2_edit_move_items(struct ptk_anm2_edit *edit,
                                        uint32_t const *item_ids,
                                        size_t item_count,
                                        uint32_t dropped_on_id,
                                        bool dropped_on_is_selector,
                                        bool insert_after,
                                        struct ov_error *err);

// Check if a move operation would result in an actual position change (for drag visual feedback)
bool ptk_anm2_edit_would_move_items(struct ptk_anm2_edit const *edit,
                                    uint32_t const *item_ids,
                                    size_t item_count,
                                    uint32_t dropped_on_id,
                                    bool dropped_on_is_selector,
                                    bool insert_after);
bool ptk_anm2_edit_would_move_selector(struct ptk_anm2_edit const *edit,
                                       uint32_t selector_id,
                                       uint32_t target_selector_id,
                                       bool insert_after);

NODISCARD bool ptk_anm2_edit_param_add_for_focus(struct ptk_anm2_edit *edit, char const *key, struct ov_error *err);
NODISCARD bool ptk_anm2_edit_param_remove(struct ptk_anm2_edit *edit, uint32_t param_id, struct ov_error *err);
NODISCARD bool
ptk_anm2_edit_param_set_key(struct ptk_anm2_edit *edit, uint32_t param_id, char const *value, struct ov_error *err);
NODISCARD bool
ptk_anm2_edit_param_set_value(struct ptk_anm2_edit *edit, uint32_t param_id, char const *value, struct ov_error *err);

NODISCARD bool ptk_anm2_edit_set_label(struct ptk_anm2_edit *edit, char const *label, struct ov_error *err);
NODISCARD bool ptk_anm2_edit_set_psd_path(struct ptk_anm2_edit *edit, char const *path, struct ov_error *err);
NODISCARD bool
ptk_anm2_edit_set_exclusive_support_default(struct ptk_anm2_edit *edit, bool value, struct ov_error *err);
NODISCARD bool ptk_anm2_edit_set_information(struct ptk_anm2_edit *edit, char const *info, struct ov_error *err);
NODISCARD bool
ptk_anm2_edit_set_default_character_id(struct ptk_anm2_edit *edit, char const *char_id, struct ov_error *err);

// Document metadata
char const *ptk_anm2_edit_get_label(struct ptk_anm2_edit const *edit);
char const *ptk_anm2_edit_get_information(struct ptk_anm2_edit const *edit);
char const *ptk_anm2_edit_get_psd_path(struct ptk_anm2_edit const *edit);
bool ptk_anm2_edit_get_exclusive_support_default(struct ptk_anm2_edit const *edit);
char const *ptk_anm2_edit_get_default_character_id(struct ptk_anm2_edit const *edit);

// Selector/item counts and IDs
size_t ptk_anm2_edit_selector_count(struct ptk_anm2_edit const *edit);
size_t ptk_anm2_edit_item_count(struct ptk_anm2_edit const *edit, uint32_t selector_id);
uint32_t ptk_anm2_edit_selector_get_id(struct ptk_anm2_edit const *edit, size_t sel_idx);
uint32_t ptk_anm2_edit_item_get_id(struct ptk_anm2_edit const *edit, size_t sel_idx, size_t item_idx);

// Selector/item properties
char const *ptk_anm2_edit_selector_get_name(struct ptk_anm2_edit const *edit, uint32_t selector_id);
char const *ptk_anm2_edit_item_get_name(struct ptk_anm2_edit const *edit, uint32_t item_id);
char const *ptk_anm2_edit_item_get_value(struct ptk_anm2_edit const *edit, uint32_t item_id);
bool ptk_anm2_edit_item_is_animation(struct ptk_anm2_edit const *edit, uint32_t item_id);

// Lookup by ID
bool ptk_anm2_edit_find_selector(struct ptk_anm2_edit const *edit, uint32_t id, size_t *out_sel_idx);
bool ptk_anm2_edit_find_item(struct ptk_anm2_edit const *edit, uint32_t id, size_t *out_sel_idx, size_t *out_item_idx);

// Display name formatting (for TreeView display)
void ptk_anm2_edit_format_selector_display_name(struct ptk_anm2_edit const *edit,
                                                uint32_t selector_id,
                                                wchar_t *out,
                                                size_t out_len);
void ptk_anm2_edit_format_item_display_name(struct ptk_anm2_edit const *edit,
                                            uint32_t item_id,
                                            wchar_t *out,
                                            size_t out_len);
// Get editable name (raw UTF-8 name for editing, converted to wchar_t)
void ptk_anm2_edit_get_editable_name(
    struct ptk_anm2_edit const *edit, uint32_t id, bool is_selector, wchar_t *out, size_t out_len);

// Parameters
size_t ptk_anm2_edit_param_count(struct ptk_anm2_edit const *edit, uint32_t item_id);
uint32_t
ptk_anm2_edit_param_get_id(struct ptk_anm2_edit const *edit, size_t sel_idx, size_t item_idx, size_t param_idx);
char const *ptk_anm2_edit_param_get_key(struct ptk_anm2_edit const *edit, uint32_t param_id);
char const *ptk_anm2_edit_param_get_value(struct ptk_anm2_edit const *edit, uint32_t param_id);

// Selector userdata
uintptr_t ptk_anm2_edit_selector_get_userdata(struct ptk_anm2_edit const *edit, uint32_t selector_id);
void ptk_anm2_edit_selector_set_userdata(struct ptk_anm2_edit *edit, uint32_t selector_id, uintptr_t data);

bool ptk_anm2_edit_can_undo(struct ptk_anm2_edit const *edit);
bool ptk_anm2_edit_can_redo(struct ptk_anm2_edit const *edit);
bool ptk_anm2_edit_can_save(struct ptk_anm2_edit const *edit);
NODISCARD bool ptk_anm2_edit_undo(struct ptk_anm2_edit *edit, struct ov_error *err);
NODISCARD bool ptk_anm2_edit_redo(struct ptk_anm2_edit *edit, struct ov_error *err);

// Transaction support
NODISCARD bool ptk_anm2_edit_begin_transaction(struct ptk_anm2_edit *edit, struct ov_error *err);
NODISCARD bool ptk_anm2_edit_end_transaction(struct ptk_anm2_edit *edit, bool success, struct ov_error *err);

// Verify checksum of a file without loading it
// Returns: ov_true = checksum matches, ov_false = checksum mismatch (manually edited), ov_indeterminate = error
ov_tribool ptk_anm2_edit_verify_file_checksum(wchar_t const *path, struct ov_error *err);

NODISCARD bool ptk_anm2_edit_load(struct ptk_anm2_edit *edit, wchar_t const *path, struct ov_error *err);
NODISCARD bool ptk_anm2_edit_save(struct ptk_anm2_edit *edit, wchar_t const *path, struct ov_error *err);
NODISCARD bool ptk_anm2_edit_reset(struct ptk_anm2_edit *edit, struct ov_error *err);
bool ptk_anm2_edit_verify_checksum(struct ptk_anm2_edit const *edit);

/**
 * @brief Target item for ~ptkl parameter assignment
 *
 * Represents a parameter that ends with "~ptkl" suffix in an animation item.
 * Uses ID-based references for stability across UNDO/REDO operations.
 */
struct ptk_anm2_edit_ptkl_target {
  char *selector_name; // Selector group name, e.g., "目パチ" (ovarray)
  char *display_name;  // Display name for UI, e.g., "目パチ@PSDToolKit" (ovarray, user-editable)
  char *effect_name;   // Effect name from INI for i18n lookup, e.g., "目パチ@PSDToolKit" (ovarray, can be NULL)
  char *param_key;     // Parameter key, e.g., "開き~ptkl" (ovarray)
  uint32_t selector_id;
  uint32_t item_id;
  uint32_t param_id;
};

/**
 * @brief Collection of ~ptkl targets
 */
struct ptk_anm2_edit_ptkl_targets {
  struct ptk_anm2_edit_ptkl_target *items; // ovarray
};

/**
 * @brief Free ptkl targets structure
 *
 * @param targets Targets to free
 */
void ptk_anm2_edit_ptkl_targets_free(struct ptk_anm2_edit_ptkl_targets *targets);

/**
 * @brief Collect ~ptkl parameter targets from the currently focused selector
 *
 * Scans animation items in the currently focused selector for parameters
 * that end with "~ptkl" suffix. If no selector is focused, returns an empty result.
 *
 * @param edit Edit instance
 * @param targets [out] Output targets (caller must free with ptk_anm2_edit_ptkl_targets_free)
 * @param err [out] Error information on failure
 * @return true on success, false on failure
 */
NODISCARD bool ptk_anm2_edit_collect_ptkl_targets(struct ptk_anm2_edit *edit,
                                                  struct ptk_anm2_edit_ptkl_targets *targets,
                                                  struct ov_error *err);

/**
 * @brief Set a parameter value by ID
 *
 * @param edit Edit instance
 * @param param_id Parameter ID
 * @param value New value to set (UTF-8)
 * @param err [out] Error information on failure
 * @return true on success, false on failure
 */
NODISCARD bool ptk_anm2_edit_set_param_value_by_id(struct ptk_anm2_edit *edit,
                                                   uint32_t param_id,
                                                   char const *value,
                                                   struct ov_error *err);
