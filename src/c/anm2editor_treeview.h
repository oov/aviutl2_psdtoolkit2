#pragma once

#include "anm2_edit.h"
#include <ovbase.h>

struct anm2editor_treeview;

/**
 * @brief Information about a TreeView item (selector or item)
 */
struct anm2editor_treeview_item_info {
  uint32_t id;      // Selector ID or Item ID
  bool is_selector; // true = selector, false = item
};

/**
 * @brief Operation types for differential updates
 *
 * These match ptk_anm2_op_type for easy mapping.
 */
enum anm2editor_treeview_op_type {
  anm2editor_treeview_op_reset,
  anm2editor_treeview_op_selector_insert,
  anm2editor_treeview_op_selector_remove,
  anm2editor_treeview_op_selector_set_name,
  anm2editor_treeview_op_selector_move,
  anm2editor_treeview_op_item_insert,
  anm2editor_treeview_op_item_remove,
  anm2editor_treeview_op_item_set_name,
  anm2editor_treeview_op_item_move,
  anm2editor_treeview_op_group_begin,
  anm2editor_treeview_op_group_end,
};

/**
 * @brief Callbacks for TreeView events
 *
 * These callbacks allow the TreeView component to communicate with the
 * parent editor for events that require external handling.
 */
struct anm2editor_treeview_callbacks {
  void *userdata;

  /**
   * @brief Called when selection changes in the TreeView
   *
   * @param userdata User data pointer
   * @param item Selected item info (NULL if nothing selected)
   * @param ctrl_pressed Whether Ctrl key was held
   * @param shift_pressed Whether Shift key was held
   */
  void (*on_selection_changed)(void *userdata,
                               struct anm2editor_treeview_item_info const *item,
                               bool ctrl_pressed,
                               bool shift_pressed);

  /**
   * @brief Called when an error occurs during an operation
   *
   * The callback is responsible for displaying the error and destroying it with OV_ERROR_DESTROY.
   *
   * @param userdata User data pointer
   * @param err Error information (callback must destroy with OV_ERROR_DESTROY)
   */
  void (*on_error)(void *userdata, struct ov_error *err);
};

/**
 * @brief Create a TreeView component
 *
 * @param parent_window Parent window handle (HWND cast to void*)
 * @param control_id Control ID for WM_NOTIFY
 * @param edit The anm2_edit instance for data access
 * @param callbacks Event callbacks
 * @param err Error information on failure
 * @return Created TreeView, or NULL on failure
 */
NODISCARD struct anm2editor_treeview *anm2editor_treeview_create(void *parent_window,
                                                                 int control_id,
                                                                 struct ptk_anm2_edit *edit,
                                                                 struct anm2editor_treeview_callbacks const *callbacks,
                                                                 struct ov_error *err);

/**
 * @brief Destroy a TreeView component
 *
 * @param tv Pointer to TreeView to destroy, will be set to NULL
 */
void anm2editor_treeview_destroy(struct anm2editor_treeview **tv);

/**
 * @brief Set the position and size of the TreeView
 *
 * @param tv TreeView instance
 * @param x X position
 * @param y Y position
 * @param width Width
 * @param height Height
 */
void anm2editor_treeview_set_position(struct anm2editor_treeview *tv, int x, int y, int width, int height);

/**
 * @brief Rebuild the entire TreeView from scratch
 *
 * Clears all items and rebuilds from the data source via callbacks.
 *
 * @param tv TreeView instance
 */
void anm2editor_treeview_rebuild(struct anm2editor_treeview *tv);

/**
 * @brief Update TreeView incrementally based on operation type
 *
 * @param tv TreeView instance
 * @param op_type Type of operation
 * @param id ID of affected element
 * @param parent_id Parent ID (selector ID for item ops)
 * @param before_id For insert/move: ID of element before which insertion occurred (0=end)
 */
void anm2editor_treeview_update_differential(struct anm2editor_treeview *tv,
                                             enum anm2editor_treeview_op_type op_type,
                                             uint32_t id,
                                             uint32_t parent_id,
                                             uint32_t before_id);

/**
 * @brief Select an item by its ID
 *
 * @param tv TreeView instance
 * @param id Selector or item ID
 * @param is_selector true if ID is a selector ID
 */
void anm2editor_treeview_select_by_id(struct anm2editor_treeview *tv, uint32_t id, bool is_selector);

/**
 * @brief Select an item by its indices
 *
 * @param tv TreeView instance
 * @param sel_idx Selector index
 * @param item_idx Item index (SIZE_MAX to select the selector itself)
 */
void anm2editor_treeview_select_by_index(struct anm2editor_treeview *tv, size_t sel_idx, size_t item_idx);

/**
 * @brief Update the text of the currently selected item
 *
 * Refreshes the display text via callbacks.
 *
 * @param tv TreeView instance
 */
void anm2editor_treeview_update_selected_text(struct anm2editor_treeview *tv);

/**
 * @brief Invalidate the TreeView for repainting
 *
 * @param tv TreeView instance
 */
void anm2editor_treeview_invalidate(struct anm2editor_treeview *tv);

/**
 * @brief Suppress selection changed callback
 *
 * When suppressed, programmatic selection changes (via select_by_id, etc.)
 * will not trigger the on_selection_changed callback. Useful for batch
 * selection restoration.
 *
 * @param tv TreeView instance
 * @param suppress true to suppress, false to restore normal behavior
 */
void anm2editor_treeview_suppress_selection_changed(struct anm2editor_treeview *tv, bool suppress);

/**
 * @brief Handle WM_NOTIFY message for TreeView
 *
 * Call this from the parent window's WM_NOTIFY handler when nmhdr->idFrom matches
 * the TreeView's control ID. Returns the LRESULT to be returned from the window procedure.
 *
 * @param tv TreeView instance
 * @param nmhdr NMHDR pointer
 * @return LRESULT to return from window procedure
 */
intptr_t anm2editor_treeview_handle_notify(struct anm2editor_treeview *tv, void *nmhdr);

/**
 * @brief Handle mouse move during drag operation
 *
 * @param tv TreeView instance
 * @param x Client X coordinate
 * @param y Client Y coordinate
 */
void anm2editor_treeview_handle_mouse_move(struct anm2editor_treeview *tv, int x, int y);

/**
 * @brief Handle left button up (end drag operation)
 *
 * @param tv TreeView instance
 */
void anm2editor_treeview_handle_lbutton_up(struct anm2editor_treeview *tv);

/**
 * @brief Cancel the current drag operation
 *
 * @param tv TreeView instance
 */
void anm2editor_treeview_cancel_drag(struct anm2editor_treeview *tv);

/**
 * @brief Handle view change event from anm2_edit
 *
 * Processes treeview-related view change events forwarded from anm2_edit.
 *
 * @param tv TreeView instance
 * @param event View event to handle
 */
void anm2editor_treeview_handle_view_event(struct anm2editor_treeview *tv,
                                           struct ptk_anm2_edit_view_event const *event);
