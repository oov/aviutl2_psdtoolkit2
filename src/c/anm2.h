#pragma once

#include <ovbase.h>

struct ptk_anm2;

/**
 * @brief Custom error codes for anm2 operations
 *
 * These codes are used with ov_error_type_generic to identify specific
 * error conditions during document operations.
 */
enum ptk_anm2_error {
  /**
   * @brief Invalid file format - not a PSDToolKit anm2 script
   *
   * The file does not contain the expected JSON metadata header.
   * This typically means the file is not a *.ptk.anm2 file created by PSDToolKit.
   */
  ptk_anm2_error_invalid_format = 3000,
};

/**
 * @brief Operation types for change notifications
 *
 * These are exposed for the change callback to identify what operation occurred.
 */
enum ptk_anm2_op_type {
  // Special operation: document reset (load/new)
  ptk_anm2_op_reset = 0,

  // Transaction markers
  ptk_anm2_op_transaction_begin,
  ptk_anm2_op_transaction_end,

  // Metadata operations
  ptk_anm2_op_set_label,
  ptk_anm2_op_set_psd_path,
  ptk_anm2_op_set_exclusive_support_default,
  ptk_anm2_op_set_information,
  ptk_anm2_op_set_default_character_id,

  // Selector operations
  ptk_anm2_op_selector_insert,
  ptk_anm2_op_selector_remove,
  ptk_anm2_op_selector_set_name,
  ptk_anm2_op_selector_move,

  // Item operations
  ptk_anm2_op_item_insert,
  ptk_anm2_op_item_remove,
  ptk_anm2_op_item_set_name,
  ptk_anm2_op_item_set_value,
  ptk_anm2_op_item_set_script_name,
  ptk_anm2_op_item_move,

  // Parameter operations
  ptk_anm2_op_param_insert,
  ptk_anm2_op_param_remove,
  ptk_anm2_op_param_set_key,
  ptk_anm2_op_param_set_value,
};

/**
 * @brief Callback function type for document change notifications
 *
 * Called after each operation is applied to the document.
 *
 * @param userdata User-provided context
 * @param op_type Type of operation that was performed
 * @param id ID of the affected element (selector/item/param), 0 for metadata ops
 * @param parent_id Parent ID (selector for item ops, item for param ops), 0 otherwise
 * @param before_id For insert/move ops: ID of element before which insertion occurred (0=end)
 */
typedef void (*ptk_anm2_change_callback)(
    void *userdata, enum ptk_anm2_op_type op_type, uint32_t id, uint32_t parent_id, uint32_t before_id);

/**
 * @brief Set the change callback for document modifications
 *
 * The callback will be invoked after each successful operation.
 * Set to NULL to disable notifications.
 *
 * @param doc Document handle
 * @param callback Callback function (or NULL to disable)
 * @param userdata User-provided context passed to callback
 */
void ptk_anm2_set_change_callback(struct ptk_anm2 *doc, ptk_anm2_change_callback callback, void *userdata);

/**
 * @brief Callback for state change notifications
 *
 * Called when undo/redo state, modified state, or save-ability changes.
 * This is separate from change_callback because state changes occur
 * AFTER the undo stack is updated, not during the operation.
 *
 * @param userdata User-provided context
 */
typedef void (*ptk_anm2_state_callback)(void *userdata);

/**
 * @brief Set the state callback for undo/redo/modified state changes
 *
 * The callback will be invoked after undo stack updates, saves, etc.
 * Set to NULL to disable notifications.
 *
 * @param doc Document handle
 * @param callback Callback function (or NULL to disable)
 * @param userdata User-provided context passed to callback
 */
void ptk_anm2_set_state_callback(struct ptk_anm2 *doc, ptk_anm2_state_callback callback, void *userdata);

/**
 * @brief Create a new empty anm2 document
 *
 * @param err Error information
 * @return New document handle on success, NULL on failure
 */
NODISCARD struct ptk_anm2 *ptk_anm2_create(struct ov_error *const err);

/**
 * @brief Destroy an anm2 document and free all resources
 *
 * @param doc Pointer to document handle (will be set to NULL)
 */
void ptk_anm2_destroy(struct ptk_anm2 **doc);

/**
 * @brief Reset an anm2 document to empty state
 *
 * Clears all selectors, items, metadata, and UNDO/REDO history.
 * The document becomes equivalent to a newly created one.
 *
 * @param doc Document handle
 * @param err Error information
 * @return true on success, false on failure
 */
NODISCARD bool ptk_anm2_reset(struct ptk_anm2 *doc, struct ov_error *const err);

/**
 * @brief Load an anm2 document from file
 *
 * Parses the PTK JSON metadata embedded in the script file.
 * Clears UNDO/REDO history after successful load.
 *
 * @param doc Document handle
 * @param path Path to the anm2 file
 * @param err Error information
 * @return true on success, false on failure
 */
NODISCARD bool ptk_anm2_load(struct ptk_anm2 *doc, wchar_t const *path, struct ov_error *const err);

/**
 * @brief Check if a document can be saved
 *
 * Returns true if the document has a PSD path set and contains at least one
 * selector with items. A document without a PSD path or without any items
 * would produce an invalid/useless script.
 *
 * @param doc Document handle
 * @return true if save is possible, false otherwise
 */
bool ptk_anm2_can_save(struct ptk_anm2 const *doc);

/**
 * @brief Save an anm2 document to file
 *
 * Generates the script with embedded JSON metadata and writes to file.
 *
 * @param doc Document handle
 * @param path Path to save the anm2 file
 * @param err Error information
 * @return true on success, false on failure
 */
NODISCARD bool ptk_anm2_save(struct ptk_anm2 *doc, wchar_t const *path, struct ov_error *const err);

/**
 * @brief Verify checksum of a loaded anm2 document
 *
 * Compares the stored checksum (from JSON metadata) with the calculated checksum
 * (from script body). This detects manual edits to the file.
 *
 * @param doc Document handle (must be loaded via ptk_anm2_load)
 * @return true if checksum matches, false if mismatch
 */
NODISCARD bool ptk_anm2_verify_checksum(struct ptk_anm2 const *doc);

/**
 * @brief Check if document has been modified since last save/load/reset
 *
 * Returns true if any changes have been made to the document that haven't
 * been saved. This is reset to false by load, save, and reset operations.
 *
 * @param doc Document handle
 * @return true if document has unsaved changes
 */
bool ptk_anm2_is_modified(struct ptk_anm2 const *doc);

/**
 * @brief Get the document label
 *
 * @param doc Document handle
 * @return Label string (owned by doc, do not free)
 */
char const *ptk_anm2_get_label(struct ptk_anm2 const *doc);

/**
 * @brief Set the document label
 *
 * Records UNDO operation.
 *
 * @param doc Document handle
 * @param label New label string
 * @param err Error information
 * @return true on success, false on failure
 */
NODISCARD bool ptk_anm2_set_label(struct ptk_anm2 *doc, char const *label, struct ov_error *const err);

/**
 * @brief Get the PSD file path
 *
 * @param doc Document handle
 * @return PSD path string (owned by doc, do not free)
 */
char const *ptk_anm2_get_psd_path(struct ptk_anm2 const *doc);

/**
 * @brief Set the PSD file path
 *
 * Records UNDO operation.
 *
 * @param doc Document handle
 * @param path New PSD path string
 * @param err Error information
 * @return true on success, false on failure
 */
NODISCARD bool ptk_anm2_set_psd_path(struct ptk_anm2 *doc, char const *path, struct ov_error *const err);

/**
 * @brief Get the exclusive support control default value
 *
 * When true, the generated script will have exclusive support control enabled by default.
 *
 * @param doc Document handle
 * @return Current exclusive support default value (default: true for new documents)
 */
bool ptk_anm2_get_exclusive_support_default(struct ptk_anm2 const *doc);

/**
 * @brief Set the exclusive support control default value
 *
 * Records UNDO operation.
 *
 * @param doc Document handle
 * @param exclusive_support_default New exclusive default value
 * @param err Error information
 * @return true on success, false on failure
 */
NODISCARD bool ptk_anm2_set_exclusive_support_default(struct ptk_anm2 *doc,
                                                      bool exclusive_support_default,
                                                      struct ov_error *const err);

/**
 * @brief Get the custom information text
 *
 * @param doc Document handle
 * @return Custom information string (owned by doc, do not free), NULL if auto-generate from filename
 */
char const *ptk_anm2_get_information(struct ptk_anm2 const *doc);

/**
 * @brief Set the custom information text
 *
 * Records UNDO operation.
 *
 * @param doc Document handle
 * @param information New information string (NULL to auto-generate from PSD filename)
 * @param err Error information
 * @return true on success, false on failure
 */
NODISCARD bool ptk_anm2_set_information(struct ptk_anm2 *doc, char const *information, struct ov_error *const err);

/**
 * @brief Get the default character ID for multi-script format
 *
 * Used in @name.ptk.anm2 multi-script files to set the default character ID
 * for the "Parts Override" script.
 *
 * @param doc Document handle
 * @return Default character ID string (owned by doc, do not free), NULL if not set
 */
char const *ptk_anm2_get_default_character_id(struct ptk_anm2 const *doc);

/**
 * @brief Set the default character ID for multi-script format
 *
 * Records UNDO operation.
 *
 * @param doc Document handle
 * @param character_id New character ID string (NULL or empty to clear)
 * @param err Error information
 * @return true on success, false on failure
 */
NODISCARD bool
ptk_anm2_set_default_character_id(struct ptk_anm2 *doc, char const *character_id, struct ov_error *const err);

/**
 * @brief Get the document version
 *
 * @param doc Document handle
 * @return Version number (read-only, internally managed)
 */
int ptk_anm2_get_version(struct ptk_anm2 const *doc);

/**
 * @brief Get the number of selectors
 *
 * @param doc Document handle
 * @return Number of selectors
 */
size_t ptk_anm2_selector_count(struct ptk_anm2 const *doc);

/**
 * @brief Insert a new selector before the specified selector
 *
 * Records UNDO operation.
 * If before_id is 0 or invalid, the selector is added at the end.
 * If before_id is a valid selector ID, the new selector is inserted before it.
 *
 * @param doc Document handle
 * @param before_id ID of the selector before which to insert, or 0 for end
 * @param name Name for the new selector
 * @param err Error information
 * @return ID of the new selector on success, 0 on failure
 */
NODISCARD uint32_t ptk_anm2_selector_insert(struct ptk_anm2 *doc,
                                            uint32_t before_id,
                                            char const *name,
                                            struct ov_error *const err);

/**
 * @brief Remove a selector by ID
 *
 * Records UNDO operation (including all contained items).
 *
 * @param doc Document handle
 * @param id Selector ID
 * @param err Error information
 * @return true on success, false on failure
 */
NODISCARD bool ptk_anm2_selector_remove(struct ptk_anm2 *doc, uint32_t id, struct ov_error *const err);

/**
 * @brief Get the name of a selector by ID
 *
 * @param doc Document handle
 * @param id Selector ID
 * @return Name string (owned by doc, do not free), NULL if ID is invalid
 */
char const *ptk_anm2_selector_get_name(struct ptk_anm2 const *doc, uint32_t id);

/**
 * @brief Set the name of a selector by ID
 *
 * Records UNDO operation.
 *
 * @param doc Document handle
 * @param id Selector ID
 * @param name New name
 * @param err Error information
 * @return true on success, false on failure
 */
NODISCARD bool
ptk_anm2_selector_set_name(struct ptk_anm2 *doc, uint32_t id, char const *name, struct ov_error *const err);

/**
 * @brief Move a selector before another selector by ID
 *
 * Records UNDO operation.
 * If before_id is 0 or invalid, the selector is moved to the end.
 * If before_id is a valid selector ID, the selector is moved before it.
 *
 * @param doc Document handle
 * @param id Selector ID to move
 * @param before_id ID of the selector before which to move, or 0 for end
 * @param err Error information
 * @return true on success, false on failure
 */
NODISCARD bool
ptk_anm2_selector_move(struct ptk_anm2 *doc, uint32_t id, uint32_t before_id, struct ov_error *const err);

/**
 * @brief Check if moving a selector would result in an actual position change
 *
 * @param doc Document handle (const)
 * @param id Selector ID to move
 * @param before_id ID of the selector before which to move, or 0 for end
 * @return true if the move would change position, false if it would be a no-op
 */
bool ptk_anm2_selector_would_move(struct ptk_anm2 const *doc, uint32_t id, uint32_t before_id);

/**
 * @brief Get the number of items in a selector by selector ID
 *
 * @param doc Document handle
 * @param selector_id Selector ID
 * @return Number of items, 0 if selector ID is invalid
 */
size_t ptk_anm2_item_count(struct ptk_anm2 const *doc, uint32_t selector_id);

/**
 * @brief Check if an item is an animation item by ID
 *
 * @param doc Document handle
 * @param id Item ID
 * @return true if animation item, false if value item or ID is invalid
 */
bool ptk_anm2_item_is_animation(struct ptk_anm2 const *doc, uint32_t id);

/**
 * @brief Insert a value item before the specified item
 *
 * Records UNDO operation.
 * - If before_id is a selector ID, the item is added at the end of that selector.
 * - If before_id is an item ID, the new item is inserted before it.
 * - If before_id is 0 or invalid, returns error.
 *
 * @param doc Document handle
 * @param before_id ID of the item before which to insert, or selector ID for end
 * @param name Display name
 * @param value Layer path value
 * @param err Error information
 * @return ID of the new item on success, 0 on failure
 */
NODISCARD uint32_t ptk_anm2_item_insert_value(
    struct ptk_anm2 *doc, uint32_t before_id, char const *name, char const *value, struct ov_error *const err);

/**
 * @brief Insert an animation item before the specified item
 *
 * Records UNDO operation.
 * - If before_id is a selector ID, the item is added at the end of that selector.
 * - If before_id is an item ID, the new item is inserted before it.
 * - If before_id is 0 or invalid, returns error.
 *
 * @param doc Document handle
 * @param before_id ID of the item before which to insert, or selector ID for end
 * @param script_name Script name (e.g., "PSDToolKit.Blinker")
 * @param name Display name
 * @param err Error information
 * @return ID of the new item on success, 0 on failure
 */
NODISCARD uint32_t ptk_anm2_item_insert_animation(
    struct ptk_anm2 *doc, uint32_t before_id, char const *script_name, char const *name, struct ov_error *const err);

/**
 * @brief Remove an item by ID
 *
 * Records UNDO operation (including all parameters for animation items).
 *
 * @param doc Document handle
 * @param id Item ID
 * @param err Error information
 * @return true on success, false on failure
 */
NODISCARD bool ptk_anm2_item_remove(struct ptk_anm2 *doc, uint32_t id, struct ov_error *const err);

/**
 * @brief Move an item before another item by ID
 *
 * Records UNDO operation.
 * - If before_id is a selector ID, the item is moved to the end of that selector.
 * - If before_id is an item ID, the item is moved before it.
 *
 * @param doc Document handle
 * @param id Item ID to move
 * @param before_id ID of the item before which to move, or selector ID for end
 * @param err Error information
 * @return true on success, false on failure
 */
NODISCARD bool ptk_anm2_item_move(struct ptk_anm2 *doc, uint32_t id, uint32_t before_id, struct ov_error *const err);

/**
 * @brief Check if moving an item would result in an actual position change
 *
 * @param doc Document handle (const)
 * @param id Item ID to move
 * @param before_id Insert before this ID (item or selector ID for end insertion)
 * @return true if the move would change position, false if it would be a no-op
 */
bool ptk_anm2_item_would_move(struct ptk_anm2 const *doc, uint32_t id, uint32_t before_id);

/**
 * @brief Get the display name of an item by ID
 *
 * @param doc Document handle
 * @param id Item ID
 * @return Name string (owned by doc), NULL if ID is invalid
 */
char const *ptk_anm2_item_get_name(struct ptk_anm2 const *doc, uint32_t id);

/**
 * @brief Set the display name of an item by ID
 *
 * Records UNDO operation.
 *
 * @param doc Document handle
 * @param id Item ID
 * @param name New display name
 * @param err Error information
 * @return true on success, false on failure
 */
NODISCARD bool ptk_anm2_item_set_name(struct ptk_anm2 *doc, uint32_t id, char const *name, struct ov_error *const err);

/**
 * @brief Get the value (layer path) of a value item by ID
 *
 * @param doc Document handle
 * @param id Item ID
 * @return Value string (owned by doc), NULL if ID is invalid or item is animation
 */
char const *ptk_anm2_item_get_value(struct ptk_anm2 const *doc, uint32_t id);

/**
 * @brief Set the value (layer path) of a value item by ID
 *
 * Records UNDO operation.
 *
 * @param doc Document handle
 * @param id Item ID
 * @param value New layer path value
 * @param err Error information
 * @return true on success, false on failure
 */
NODISCARD bool
ptk_anm2_item_set_value(struct ptk_anm2 *doc, uint32_t id, char const *value, struct ov_error *const err);

/**
 * @brief Get the script name of an animation item by ID
 *
 * @param doc Document handle
 * @param id Item ID
 * @return Script name (owned by doc), NULL if ID is invalid or item is value
 */
char const *ptk_anm2_item_get_script_name(struct ptk_anm2 const *doc, uint32_t id);

/**
 * @brief Set the script name of an animation item by ID
 *
 * Records UNDO operation.
 *
 * @param doc Document handle
 * @param id Item ID
 * @param script_name New script name
 * @param err Error information
 * @return true on success, false on failure
 */
NODISCARD bool
ptk_anm2_item_set_script_name(struct ptk_anm2 *doc, uint32_t id, char const *script_name, struct ov_error *const err);

/**
 * @brief Get the number of parameters for an animation item by ID
 *
 * @param doc Document handle
 * @param id Item ID
 * @return Number of parameters, 0 if ID is invalid or item is not animation
 */
size_t ptk_anm2_param_count(struct ptk_anm2 const *doc, uint32_t id);

/**
 * @brief Insert a parameter before the specified parameter
 *
 * Records UNDO operation.
 * If before_param_id is 0 or invalid, the parameter is added at the end.
 * If before_param_id is a valid parameter ID, the new parameter is inserted before it.
 *
 * @param doc Document handle
 * @param item_id Item ID
 * @param before_param_id Parameter ID before which to insert, or 0 for end
 * @param key Parameter key
 * @param value Parameter value
 * @param err Error information
 * @return ID of the new parameter on success, 0 on failure
 */
NODISCARD uint32_t ptk_anm2_param_insert(struct ptk_anm2 *doc,
                                         uint32_t item_id,
                                         uint32_t before_param_id,
                                         char const *key,
                                         char const *value,
                                         struct ov_error *const err);

/**
 * @brief Remove a parameter by ID
 *
 * Records UNDO operation.
 *
 * @param doc Document handle
 * @param id Parameter ID
 * @param err Error information
 * @return true on success, false on failure
 */
NODISCARD bool ptk_anm2_param_remove(struct ptk_anm2 *doc, uint32_t id, struct ov_error *const err);

/**
 * @brief Get the key of a parameter by ID
 *
 * @param doc Document handle
 * @param id Parameter ID
 * @return Key string (owned by doc), NULL if ID is invalid
 */
char const *ptk_anm2_param_get_key(struct ptk_anm2 const *doc, uint32_t id);

/**
 * @brief Set the key of a parameter by ID
 *
 * Records UNDO operation.
 *
 * @param doc Document handle
 * @param id Parameter ID
 * @param key New key string
 * @param err Error information
 * @return true on success, false on failure
 */
NODISCARD bool ptk_anm2_param_set_key(struct ptk_anm2 *doc, uint32_t id, char const *key, struct ov_error *const err);

/**
 * @brief Get the value of a parameter by ID
 *
 * @param doc Document handle
 * @param id Parameter ID
 * @return Value string (owned by doc), NULL if ID is invalid
 */
char const *ptk_anm2_param_get_value(struct ptk_anm2 const *doc, uint32_t id);

/**
 * @brief Set the value of a parameter by ID
 *
 * Records UNDO operation.
 *
 * @param doc Document handle
 * @param id Parameter ID
 * @param value New value string
 * @param err Error information
 * @return true on success, false on failure
 */
NODISCARD bool
ptk_anm2_param_set_value(struct ptk_anm2 *doc, uint32_t id, char const *value, struct ov_error *const err);

/**
 * @brief Check if undo is available
 *
 * @param doc Document handle
 * @return true if undo is available
 */
bool ptk_anm2_can_undo(struct ptk_anm2 const *doc);

/**
 * @brief Check if redo is available
 *
 * @param doc Document handle
 * @return true if redo is available
 */
bool ptk_anm2_can_redo(struct ptk_anm2 const *doc);

/**
 * @brief Undo the last operation
 *
 * If the last operation was grouped (via transaction), undoes all operations
 * in the group.
 *
 * @param doc Document handle
 * @param err Error information
 * @return true on success, false on failure
 */
NODISCARD bool ptk_anm2_undo(struct ptk_anm2 *doc, struct ov_error *const err);

/**
 * @brief Redo the last undone operation
 *
 * If the undone operation was grouped (via transaction), redoes all operations
 * in the group.
 *
 * @param doc Document handle
 * @param err Error information
 * @return true on success, false on failure
 */
NODISCARD bool ptk_anm2_redo(struct ptk_anm2 *doc, struct ov_error *const err);

/**
 * @brief Clear all undo/redo history
 *
 * @param doc Document handle
 */
void ptk_anm2_clear_undo_history(struct ptk_anm2 *doc);

/**
 * @brief Begin a transaction (group multiple operations for single undo)
 *
 * Transactions can be nested. GROUP_BEGIN is recorded only when depth becomes 1.
 *
 * @param doc Document handle
 * @param err Error information
 * @return true on success, false on failure
 */
NODISCARD bool ptk_anm2_begin_transaction(struct ptk_anm2 *doc, struct ov_error *const err);

/**
 * @brief End a transaction
 *
 * GROUP_END is recorded only when depth returns to 0.
 *
 * @param doc Document handle
 * @param err Error information
 * @return true on success, false on failure
 */
NODISCARD bool ptk_anm2_end_transaction(struct ptk_anm2 *doc, struct ov_error *const err);

/**
 * @brief Get the unique ID of a selector
 *
 * @param doc Document handle
 * @param idx Selector index
 * @return Unique ID, 0 if index is invalid
 */
uint32_t ptk_anm2_selector_get_id(struct ptk_anm2 const *doc, size_t idx);

/**
 * @brief Get the unique ID of an item
 *
 * @param doc Document handle
 * @param sel_idx Selector index
 * @param item_idx Item index
 * @return Unique ID, 0 if indices are invalid
 */
uint32_t ptk_anm2_item_get_id(struct ptk_anm2 const *doc, size_t sel_idx, size_t item_idx);

/**
 * @brief Get the unique ID of a parameter
 *
 * @param doc Document handle
 * @param sel_idx Selector index
 * @param item_idx Item index
 * @param param_idx Parameter index
 * @return Unique ID, 0 if indices are invalid
 */
uint32_t ptk_anm2_param_get_id(struct ptk_anm2 const *doc, size_t sel_idx, size_t item_idx, size_t param_idx);

/**
 * @brief Get all item IDs for a selector
 *
 * Returns an array of item IDs belonging to the specified selector.
 * Caller must free the returned array with OV_ARRAY_DESTROY.
 *
 * @param doc Document handle
 * @param selector_id Selector ID
 * @param err Error information on failure
 * @return Array of item IDs (ovarray), or NULL on failure
 */
NODISCARD uint32_t *ptk_anm2_get_item_ids(struct ptk_anm2 const *doc, uint32_t selector_id, struct ov_error *err);

/**
 * @brief Get all parameter IDs for an item
 *
 * Returns an array of parameter IDs belonging to the specified item.
 * Caller must free the returned array with OV_ARRAY_DESTROY.
 *
 * @param doc Document handle
 * @param item_id Item ID
 * @param err Error information on failure
 * @return Array of parameter IDs (ovarray), or NULL on failure
 */
NODISCARD uint32_t *ptk_anm2_get_param_ids(struct ptk_anm2 const *doc, uint32_t item_id, struct ov_error *err);

/**
 * @brief Get the item ID that contains a parameter
 *
 * @param doc Document handle
 * @param param_id Parameter ID
 * @return Item ID, 0 if parameter ID is invalid
 */
uint32_t ptk_anm2_param_get_item_id(struct ptk_anm2 const *doc, uint32_t param_id);

/**
 * @brief Get the userdata of a selector by ID
 *
 * @param doc Document handle
 * @param id Selector ID
 * @return Userdata value, 0 if ID is invalid
 */
uintptr_t ptk_anm2_selector_get_userdata(struct ptk_anm2 const *doc, uint32_t id);

/**
 * @brief Set the userdata of a selector by ID
 *
 * This does NOT record UNDO operation (userdata is UI state, not document data).
 *
 * @param doc Document handle
 * @param id Selector ID
 * @param userdata New userdata value
 */
void ptk_anm2_selector_set_userdata(struct ptk_anm2 *doc, uint32_t id, uintptr_t userdata);

/**
 * @brief Get the userdata of an item by ID
 *
 * @param doc Document handle
 * @param id Item ID
 * @return Userdata value, 0 if ID is invalid
 */
uintptr_t ptk_anm2_item_get_userdata(struct ptk_anm2 const *doc, uint32_t id);

/**
 * @brief Set the userdata of an item by ID
 *
 * This does NOT record UNDO operation (userdata is UI state, not document data).
 *
 * @param doc Document handle
 * @param id Item ID
 * @param userdata New userdata value
 */
void ptk_anm2_item_set_userdata(struct ptk_anm2 *doc, uint32_t id, uintptr_t userdata);

/**
 * @brief Get the userdata of a parameter by ID
 *
 * @param doc Document handle
 * @param id Parameter ID
 * @return Userdata value, 0 if ID is invalid
 */
uintptr_t ptk_anm2_param_get_userdata(struct ptk_anm2 const *doc, uint32_t id);

/**
 * @brief Set the userdata of a parameter by ID
 *
 * This does NOT record UNDO operation (userdata is UI state, not document data).
 *
 * @param doc Document handle
 * @param id Parameter ID
 * @param userdata New userdata value
 */
void ptk_anm2_param_set_userdata(struct ptk_anm2 *doc, uint32_t id, uintptr_t userdata);

/**
 * @brief Find a selector by its unique ID
 *
 * @param doc Document handle
 * @param id ID to search for
 * @param out_sel_idx Output: selector index if found
 * @return true if found, false if not found
 */
bool ptk_anm2_find_selector(struct ptk_anm2 const *doc, uint32_t id, size_t *out_sel_idx);

/**
 * @brief Find an item by its unique ID
 *
 * @param doc Document handle
 * @param id ID to search for
 * @param out_sel_idx Output: selector index if found
 * @param out_item_idx Output: item index if found
 * @return true if found, false if not found
 */
bool ptk_anm2_find_item(struct ptk_anm2 const *doc, uint32_t id, size_t *out_sel_idx, size_t *out_item_idx);

/**
 * @brief Find a parameter by its unique ID
 *
 * @param doc Document handle
 * @param id ID to search for
 * @param out_sel_idx Output: selector index if found
 * @param out_item_idx Output: item index if found
 * @param out_param_idx Output: parameter index if found
 * @return true if found, false if not found
 */
bool ptk_anm2_find_param(
    struct ptk_anm2 const *doc, uint32_t id, size_t *out_sel_idx, size_t *out_item_idx, size_t *out_param_idx);
