#pragma once

#include <ovbase.h>

/**
 * @brief Custom error codes for alias processing
 *
 * These codes are used with ov_error_type_generic to identify specific
 * error conditions that may require special handling or user hints.
 */
enum ptk_alias_error {
  /**
   * @brief PSD file effect not found in the alias
   *
   * The alias does not contain a PSD file effect.
   * This typically indicates that the user hasn't set up the object correctly.
   */
  ptk_alias_error_psd_not_found = 1000,

  /**
   * @brief No importable scripts found in the alias
   *
   * The alias contains a PSD file effect, but no animation scripts
   * (like Blinker) were found that can be imported.
   */
  ptk_alias_error_no_scripts = 1001,

  /**
   * @brief No object is selected in AviUtl
   *
   * The user must select an object in AviUtl's object settings window
   * before importing scripts.
   */
  ptk_alias_error_no_object_selected = 1002,

  /**
   * @brief Failed to get object alias data
   *
   * The object's alias data could not be retrieved from AviUtl.
   * This is typically a rare error condition.
   */
  ptk_alias_error_failed_to_get_alias = 1003,
};

/**
 * @brief Script definition from INI file
 *
 * Represents a script that can be imported into the PSDToolKit anm2 Editor.
 */
struct ptk_alias_script_definition {
  char *script_name; // e.g., "PSDToolKit.Blinker"
  char *effect_name; // e.g., "目パチ@PSDToolKit"
};

/**
 * @brief Collection of script definitions
 */
struct ptk_alias_script_definitions {
  struct ptk_alias_script_definition *items; // ovarray
};

/**
 * @brief Free script definitions structure
 *
 * @param defs Definitions to free (can be NULL)
 */
void ptk_alias_script_definitions_free(struct ptk_alias_script_definitions *defs);

/**
 * @brief Load script definitions from INI file
 *
 * Loads the [anm2Editor.AnimationScripts] section from PSDToolKit.ini
 * located in the same directory as the DLL.
 *
 * @param defs [out] Output definitions (caller must free with ptk_alias_script_definitions_free)
 * @param err [out] Error information on failure
 * @return true on success, false on failure
 */
NODISCARD bool ptk_alias_load_script_definitions(struct ptk_alias_script_definitions *defs, struct ov_error *err);

/**
 * @brief Available script for import
 *
 * Represents a script that is both defined in INI and available in the current alias.
 */
struct ptk_alias_available_script {
  char *script_name;              // e.g., "PSDToolKit.Blinker"
  char *effect_name;              // e.g., "目パチ@PSDToolKit"
  wchar_t const *translated_name; // Translated effect name for display (can be NULL)
                                  // Points to SDK-managed memory, valid until language settings update
  bool selected;                  // Selection state for dialog
};

/**
 * @brief Collection of available scripts
 */
struct ptk_alias_available_scripts {
  struct ptk_alias_available_script *items; // ovarray
  char *psd_path;                           // PSD file path extracted from alias (owned)
};

/**
 * @brief Free available scripts structure
 *
 * @param scripts Scripts to free (can be NULL)
 */
void ptk_alias_available_scripts_free(struct ptk_alias_available_scripts *scripts);

/**
 * @brief Enumerate available scripts from alias
 *
 * Parses the alias data and checks which scripts from the definitions
 * are available (i.e., their effect names exist in the alias).
 *
 * @param alias Alias data (INI format)
 * @param alias_len Length of alias data
 * @param defs Script definitions from INI
 * @param scripts [out] Output available scripts (caller must free)
 * @param err [out] Error information on failure
 * @return true on success, false on failure
 */
NODISCARD bool ptk_alias_enumerate_available_scripts(char const *alias,
                                                     size_t alias_len,
                                                     struct ptk_alias_script_definitions const *defs,
                                                     struct ptk_alias_available_scripts *scripts,
                                                     struct ov_error *err);

/**
 * @brief Extracted parameter key-value pair
 */
struct ptk_alias_extracted_param {
  char *key;
  char *value;
};

/**
 * @brief Extracted animation with parameters
 */
struct ptk_alias_extracted_animation {
  char *script_name;                        // e.g., "PSDToolKit.Blinker"
  char *effect_name;                        // e.g., "目パチ@PSDToolKit"
  struct ptk_alias_extracted_param *params; // ovarray
};

/**
 * @brief Free extracted animation structure
 *
 * @param anim Animation to free (can be NULL)
 */
void ptk_alias_extracted_animation_free(struct ptk_alias_extracted_animation *anim);

/**
 * @brief Extract animation parameters from alias
 *
 * Parses the alias data and extracts all parameters for the specified effect.
 *
 * @param alias Alias data (INI format)
 * @param alias_len Length of alias data
 * @param script_name Script name (e.g., "PSDToolKit.Blinker")
 * @param effect_name Effect name to search for (e.g., "目パチ@PSDToolKit")
 * @param anim [out] Output animation (caller must free with ptk_alias_extracted_animation_free)
 * @param err [out] Error information on failure
 * @return true on success, false on failure
 */
NODISCARD bool ptk_alias_extract_animation(char const *alias,
                                           size_t alias_len,
                                           char const *script_name,
                                           char const *effect_name,
                                           struct ptk_alias_extracted_animation *anim,
                                           struct ov_error *err);

/**
 * @brief Populate translated names for available scripts
 *
 * Uses the AviUtl2 config handle to get translated effect names from
 * language settings. The translated name is stored in translated_name field
 * as a pointer to SDK-managed memory.
 *
 * @param scripts Available scripts to populate
 */
void ptk_alias_populate_translated_names(struct ptk_alias_available_scripts *scripts);
