#pragma once

#include <ovbase.h>

struct ptk_alias_available_script;

/**
 * @brief Script picker dialog parameters
 */
struct ptk_script_picker_params {
  struct ptk_alias_available_script *items; // Array of items (in/out, uses 'selected' field)
  size_t item_count;                        // Number of items
  char const *current_psd_path;             // Current document's PSD path (can be NULL)
  char const *source_psd_path;              // Source PSD path from alias (can be NULL)
  bool update_psd_path;                     // [out] Whether to update PSD path
};

/**
 * @brief Show script picker dialog
 *
 * Displays a dialog with:
 * - Checkboxes for each script item
 * - If PSD paths differ: warning message and "Update PSD path" checkbox
 *
 * User can:
 * - Select which scripts to import
 * - Choose to update PSD path (only shown if paths differ)
 * - Import only PSD path update without any scripts
 *
 * @param parent Parent window handle (HWND)
 * @param params [in,out] Dialog parameters (items.checked and update_psd_path updated)
 * @param err [out] Error information on failure
 * @return ov_true if user clicked Import, ov_false if Cancel, ov_indeterminate on error
 */
ov_tribool ptk_script_picker_show(void *parent, struct ptk_script_picker_params *params, struct ov_error *err);
