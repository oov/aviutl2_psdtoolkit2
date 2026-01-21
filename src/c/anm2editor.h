#pragma once

#include <ovbase.h>

#include "alias.h"

struct ptk_anm2editor;
struct ptk_anm2_edit;
struct aviutl2_edit_handle;

/**
 * @brief Create the PSDToolKit anm2 Editor instance
 *
 * Creates the editor and optionally a window for it.
 *
 * @param editor [out] Pointer to receive the created editor
 * @param title Window title (if window is not NULL)
 * @param edit_handle Edit handle for accessing AviUtl2 edit section (if window is not NULL)
 * @param window [out] Optional pointer to receive window handle (HWND), pass NULL to skip window creation
 * @param err [out] Error information on failure
 * @return true on success, false on failure
 */
NODISCARD bool ptk_anm2editor_create(struct ptk_anm2editor **editor,
                                     wchar_t const *title,
                                     struct aviutl2_edit_handle *edit_handle,
                                     void **window,
                                     struct ov_error *err);

/**
 * @brief Destroy the PSDToolKit anm2 Editor instance
 *
 * @param editor [in,out] Pointer to editor to destroy, will be set to NULL
 */
void ptk_anm2editor_destroy(struct ptk_anm2editor **editor);

/**
 * @brief Get the editor window handle
 *
 * @param editor Editor instance
 * @return Window handle (HWND), or NULL if not created
 */
void *ptk_anm2editor_get_window(struct ptk_anm2editor *editor);

/**
 * @brief Get the underlying edit core
 *
 * @param editor Editor instance
 * @return Edit core instance, or NULL if not initialized
 */
struct ptk_anm2_edit *ptk_anm2editor_get_edit(struct ptk_anm2editor *editor);

/**
 * @brief Create a new empty document for editing
 *
 * Initializes the editor with an empty metadata structure.
 * If there are unsaved changes, prompts the user to save.
 *
 * @param editor Editor instance
 * @param err [out] Error information on failure
 * @return true on success, false on failure
 */
NODISCARD bool ptk_anm2editor_new_document(struct ptk_anm2editor *editor, struct ov_error *err);

/**
 * @brief Open an existing anm2 file for editing
 *
 * Loads the file and populates the editor with its contents.
 * If there are unsaved changes, prompts the user to save.
 *
 * @param editor Editor instance
 * @param path Path to the anm2 file (NULL to show open dialog)
 * @param err [out] Error information on failure
 * @return true on success, false on failure
 */
NODISCARD bool ptk_anm2editor_open(struct ptk_anm2editor *editor, wchar_t const *path, struct ov_error *err);

/**
 * @brief Save the current document
 *
 * Saves the metadata to the current file path.
 * If no path is set, shows a save dialog.
 *
 * @param editor Editor instance
 * @param err [out] Error information on failure
 * @return true on success, false on failure
 */
NODISCARD bool ptk_anm2editor_save(struct ptk_anm2editor *editor, struct ov_error *err);

/**
 * @brief Save the current document to a new path
 *
 * Shows a save dialog and saves the metadata to the selected path.
 *
 * @param editor Editor instance
 * @param err [out] Error information on failure
 * @return true on success, false on failure
 */
NODISCARD bool ptk_anm2editor_save_as(struct ptk_anm2editor *editor, struct ov_error *err);

/**
 * @brief Check if the document has unsaved changes
 *
 * @param editor Editor instance
 * @return true if there are unsaved changes
 */
bool ptk_anm2editor_is_modified(struct ptk_anm2editor *editor);

/**
 * @brief Check if the editor is open (window exists and visible)
 *
 * @param editor Editor instance
 * @return true if the editor window is open
 */
bool ptk_anm2editor_is_open(struct ptk_anm2editor *editor);

/**
 * @brief Add a selector with value items to the editor
 *
 * Creates a new selector with the specified group name and adds value items
 * (name/value pairs). If psd_path is provided and the editor has no PSD path set,
 * it will be set as part of the same undo transaction.
 *
 * @param editor Editor instance
 * @param psd_path PSD file path to set if editor has none (UTF-8, can be NULL)
 * @param group Selector group name (UTF-8)
 * @param names Array of item names (UTF-8, count elements)
 * @param values Array of item values (UTF-8, count elements)
 * @param count Number of items
 * @param err [out] Error information on failure
 * @return true on success, false on failure
 */
NODISCARD bool ptk_anm2editor_add_value_items(struct ptk_anm2editor *editor,
                                              char const *psd_path,
                                              char const *group,
                                              char const *const *names,
                                              char const *const *values,
                                              size_t count,
                                              struct ov_error *err);

/**
 * @brief Add a single value item to the selected selector or create a new one
 *
 * If a selector is currently selected, adds the value item to that selector.
 * If no selector is selected, creates a new selector with the specified group
 * name and adds the value item to it. If psd_path is provided and the editor
 * has no PSD path set, it will be set as part of the same undo transaction.
 *
 * @param editor Editor instance
 * @param psd_path PSD file path to set if editor has none (UTF-8, can be NULL)
 * @param group Selector group name (UTF-8, used only when creating new selector)
 * @param name Item name (UTF-8)
 * @param value Item value (UTF-8)
 * @param err [out] Error information on failure
 * @return true on success, false on failure
 */
NODISCARD bool ptk_anm2editor_add_value_item_to_selected(struct ptk_anm2editor *editor,
                                                         char const *psd_path,
                                                         char const *group,
                                                         char const *name,
                                                         char const *value,
                                                         struct ov_error *err);
