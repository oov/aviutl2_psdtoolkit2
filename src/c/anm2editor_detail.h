#pragma once

#include "anm2_edit.h"
#include <ovbase.h>

struct anm2editor_detail;

/**
 * @brief Row type in the detail list
 */
enum anm2editor_detail_row_type {
  anm2editor_detail_row_type_placeholder,               // "(Add new...)" placeholder
  anm2editor_detail_row_type_label,                     // Label property (document level)
  anm2editor_detail_row_type_psd_path,                  // PSD File Path property (document level)
  anm2editor_detail_row_type_exclusive_support_default, // Exclusive Support Default property (document level)
  anm2editor_detail_row_type_information,               // Information property (document level)
  anm2editor_detail_row_type_default_character_id,      // Default Character ID property (document level)
  anm2editor_detail_row_type_multisel_item,             // Value item in multi-selection mode
  anm2editor_detail_row_type_animation_param,           // Animation item parameter
  anm2editor_detail_row_type_value_item,                // Value item (single selection)
};

/**
 * @brief Row information for the detail list
 */
struct anm2editor_detail_row {
  enum anm2editor_detail_row_type type;
  union {
    uint32_t item_id;  // For multisel_item: the item's unique ID
    uint32_t param_id; // For animation_param: parameter ID
  };
};

/**
 * @brief Callbacks for detail list events
 *
 * All callbacks receive the userdata pointer and can optionally set error information.
 */
struct anm2editor_detail_callbacks {
  void *userdata;

  /**
   * @brief Called when selection changes in the detail list
   *
   * @param userdata User data pointer
   */
  void (*on_selection_changed)(void *userdata);

  /**
   * @brief Called when an error occurs during an operation
   *
   * The callback takes ownership of the error and MUST call OV_ERROR_DESTROY on it.
   * If this callback is not set, the error is logged and destroyed internally.
   *
   * @param userdata User data pointer
   * @param err Error to display (callback takes ownership)
   */
  void (*on_error)(void *userdata, struct ov_error *err);
};

/**
 * @brief Create a detail list component
 *
 * @param parent_window Parent window handle (HWND cast to void*)
 * @param control_id Control ID for the ListView (used for WM_NOTIFY)
 * @param edit The anm2_edit instance for data access
 * @param callbacks Event callbacks
 * @param err Error information on failure
 * @return Created detail list, or NULL on failure
 */
NODISCARD struct anm2editor_detail *anm2editor_detail_create(void *parent_window,
                                                             int control_id,
                                                             struct ptk_anm2_edit *edit,
                                                             struct anm2editor_detail_callbacks const *callbacks,
                                                             struct ov_error *err);

/**
 * @brief Destroy a detail list component
 *
 * @param detail Pointer to detail list to destroy, will be set to NULL
 */
void anm2editor_detail_destroy(struct anm2editor_detail **detail);

/**
 * @brief Get the window handle of the detail list
 *
 * @param detail Detail list instance
 * @return Window handle (HWND cast to void*)
 */
void *anm2editor_detail_get_window(struct anm2editor_detail *detail);

/**
 * @brief Set the position and size of the detail list
 *
 * @param detail Detail list instance
 * @param x X position
 * @param y Y position
 * @param width Width
 * @param height Height
 */
void anm2editor_detail_set_position(struct anm2editor_detail *detail, int x, int y, int width, int height);

/**
 * @brief Clear all rows from the detail list
 *
 * @param detail Detail list instance
 */
void anm2editor_detail_clear(struct anm2editor_detail *detail);

/**
 * @brief Refresh the detail list content based on current selection state
 *
 * Clears and rebuilds the detail list based on the current selection state
 * from the associated anm2_edit instance. Handles three modes:
 * - Multi-selection: Shows selected items in tree order
 * - Single item selection: Shows item parameters
 * - No selection: Shows document properties
 *
 * @param detail Detail list instance
 */
void anm2editor_detail_refresh(struct anm2editor_detail *detail);

/**
 * @brief Add a row to the detail list
 *
 * @param detail Detail list instance
 * @param property Property name (UTF-8)
 * @param value Property value (UTF-8)
 * @param row Row information (type and metadata)
 * @param err Error information on failure
 * @return true on success, false on failure
 */
NODISCARD bool anm2editor_detail_add_row(struct anm2editor_detail *detail,
                                         char const *property,
                                         char const *value,
                                         struct anm2editor_detail_row const *row,
                                         struct ov_error *err);

/**
 * @brief Update an existing row's text
 *
 * @param detail Detail list instance
 * @param row_index Index of the row to update
 * @param property New property name (UTF-8)
 * @param value New property value (UTF-8)
 */
void anm2editor_detail_update_row(struct anm2editor_detail *detail,
                                  size_t row_index,
                                  char const *property,
                                  char const *value);

/**
 * @brief Insert a row at a specific position
 *
 * @param detail Detail list instance
 * @param row_index Index at which to insert
 * @param property Property name (UTF-8)
 * @param value Property value (UTF-8)
 * @param row Row information (type and metadata)
 * @param err Error information on failure
 * @return true on success, false on failure
 */
NODISCARD bool anm2editor_detail_insert_row(struct anm2editor_detail *detail,
                                            size_t row_index,
                                            char const *property,
                                            char const *value,
                                            struct anm2editor_detail_row const *row,
                                            struct ov_error *err);

/**
 * @brief Remove a row from the detail list
 *
 * @param detail Detail list instance
 * @param row_index Index of the row to remove
 */
void anm2editor_detail_remove_row(struct anm2editor_detail *detail, size_t row_index);

/**
 * @brief Get the number of rows in the detail list
 *
 * @param detail Detail list instance
 * @return Number of rows
 */
size_t anm2editor_detail_row_count(struct anm2editor_detail const *detail);

/**
 * @brief Find a row by its type
 *
 * @param detail Detail list instance
 * @param type Row type to search for
 * @return Index of the first row with the specified type, or SIZE_MAX if not found
 */
size_t anm2editor_detail_find_row_by_type(struct anm2editor_detail const *detail, enum anm2editor_detail_row_type type);

/**
 * @brief Check if a row type is editable
 *
 * @param type Row type
 * @return true if the row type can be edited inline
 */
bool anm2editor_detail_row_type_is_editable(enum anm2editor_detail_row_type type);

/**
 * @brief Check if a row type represents a deletable parameter
 *
 * @param type Row type
 * @return true if the row type can be deleted
 */
bool anm2editor_detail_row_type_is_deletable_param(enum anm2editor_detail_row_type type);

/**
 * @brief Start inline editing for a specific cell
 *
 * @param detail Detail list instance
 * @param row_index Index of the row to edit
 * @param column Column to edit (0=property, 1=value)
 */
void anm2editor_detail_start_edit(struct anm2editor_detail *detail, size_t row_index, int column);

/**
 * @brief Start inline editing for adding a new parameter
 *
 * Shows an edit control at the placeholder row position.
 *
 * @param detail Detail list instance
 */
void anm2editor_detail_start_edit_new(struct anm2editor_detail *detail);

/**
 * @brief Cancel the current inline edit
 *
 * @param detail Detail list instance
 */
void anm2editor_detail_cancel_edit(struct anm2editor_detail *detail);

/**
 * @brief Check if inline editing is currently active
 *
 * @param detail Detail list instance
 * @return true if editing is active
 */
bool anm2editor_detail_is_editing(struct anm2editor_detail const *detail);

/**
 * @brief Handle WM_NOTIFY message for detail list
 *
 * Call this from the parent window's WM_NOTIFY handler when nmhdr->idFrom matches
 * the detail list's control ID. Returns the LRESULT to be returned from the window procedure.
 *
 * @param detail Detail list instance
 * @param nmhdr NMHDR pointer
 * @return LRESULT to return from window procedure
 */
intptr_t anm2editor_detail_handle_notify(struct anm2editor_detail *detail, void *nmhdr);

/**
 * @brief Handle view change event from anm2_edit
 *
 * Processes detail-related view change events forwarded from anm2_edit.
 *
 * @param detail Detail list instance
 * @param event View event to handle
 */
void anm2editor_detail_handle_view_event(struct anm2editor_detail *detail,
                                         struct ptk_anm2_edit_view_event const *event);
