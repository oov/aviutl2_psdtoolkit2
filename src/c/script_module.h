#pragma once

#include <ovbase.h>

struct aviutl2_script_module_param;
struct ptk_script_module;

/**
 * @brief Input parameters for set_props operation
 *
 * All fields are required.
 */
struct ptk_script_module_set_props_params {
  int id;
  char const *path_utf8;
  char const *layer;
  double scale;
  int offset_x;
  int offset_y;
  int tag;
};

/**
 * @brief Result structure for set_props operation
 */
struct ptk_script_module_set_props_result {
  bool modified;
  uint64_t ckey;
  int32_t width;
  int32_t height;
};

/**
 * @brief Result structure for get_drop_config operation
 */
struct ptk_script_module_drop_config {
  bool manual_shift_wav;
  bool manual_shift_psd;
  bool manual_wav_txt_pair;
  bool manual_object_audio_text;
  bool external_wav_txt_pair;
  bool external_object_audio_text;
};

/**
 * @brief Callback function table for script module dependencies
 *
 * These callbacks are invoked by the script module to obtain required data.
 * The userdata pointer is passed to each callback for context.
 */
struct ptk_script_module_callbacks {
  void *userdata;

  /**
   * @brief Get the current debug mode setting
   * @param userdata Context pointer
   * @param debug_mode [out] Receives true if debug mode is enabled
   * @param err [out] Error information on failure
   * @return true on success, false on failure
   */
  bool (*get_debug_mode)(void *userdata, bool *debug_mode, struct ov_error *err);

  /**
   * @brief Add a PSD file to the manager
   * @param userdata Context pointer
   * @param path_utf8 Path to the PSD file (UTF-8)
   * @param tag Tag value for the file
   * @param err [out] Error information on failure
   * @return true on success, false on failure
   */
  bool (*add_file)(void *userdata, char const *path_utf8, uint32_t tag, struct ov_error *err);

  /**
   * @brief Set properties for a PSD object
   * @param userdata Context pointer
   * @param params Input parameters
   * @param result [out] Result containing modified flag, cache key, and dimensions
   * @param err [out] Error information on failure
   * @return true on success, false on failure
   */
  bool (*set_props)(void *userdata,
                    struct ptk_script_module_set_props_params const *params,
                    struct ptk_script_module_set_props_result *result,
                    struct ov_error *err);

  /**
   * @brief Get drop configuration settings
   * @param userdata Context pointer
   * @param config [out] Drop configuration result
   * @param err [out] Error information on failure
   * @return true on success, false on failure
   */
  bool (*get_drop_config)(void *userdata, struct ptk_script_module_drop_config *config, struct ov_error *err);

  /**
   * @brief Draw a PSD image and store result in cache
   *
   * Renders the PSD image via IPC, flips vertically for BITMAP format,
   * and stores the result in the cache.
   *
   * @param userdata Context pointer
   * @param id Object ID
   * @param path_utf8 Path to the PSD file (UTF-8)
   * @param width Image width
   * @param height Image height
   * @param ckey Cache key for storing the result
   * @param err [out] Error information on failure
   * @return true on success, false on failure
   */
  bool (*draw)(void *userdata,
               int id,
               char const *path_utf8,
               int32_t width,
               int32_t height,
               uint64_t ckey,
               struct ov_error *err);
};

/**
 * @brief Create a script module instance
 *
 * @param callbacks Callback function table (contents are copied)
 * @param err [out] Error information on failure
 * @return Pointer to created script module, or NULL on failure
 */
NODISCARD struct ptk_script_module *ptk_script_module_create(struct ptk_script_module_callbacks const *callbacks,
                                                             struct ov_error *err);

/**
 * @brief Destroy a script module instance
 *
 * @param sm [in,out] Pointer to script module to destroy, will be set to NULL
 */
void ptk_script_module_destroy(struct ptk_script_module **sm);

/**
 * @brief Script function: Get debug mode setting and cache index
 *
 * Pushes two results: a boolean indicating whether debug mode is enabled,
 * and an integer cache index that increments when caches should be cleared.
 *
 * @param sm Script module instance
 * @param param Script module parameter interface
 * @param cache_index Current cache index value
 */
void ptk_script_module_get_debug_mode(struct ptk_script_module *sm,
                                      struct aviutl2_script_module_param *param,
                                      int cache_index);

/**
 * @brief Script function: Generate a unique tag value
 *
 * Pushes an integer result containing a random tag value.
 *
 * @param sm Script module instance
 * @param param Script module parameter interface
 */
void ptk_script_module_generate_tag(struct ptk_script_module *sm, struct aviutl2_script_module_param *param);

/**
 * @brief Script function: Add a PSD file
 *
 * Parameters from script:
 *   [0] string: path_utf8 - Path to the PSD file
 *   [1] int: tag - Tag value for the file
 *
 * Pushes a boolean result indicating success.
 *
 * @param sm Script module instance
 * @param param Script module parameter interface
 */
void ptk_script_module_add_psd_file(struct ptk_script_module *sm, struct aviutl2_script_module_param *param);

/**
 * @brief Script function: Set PSD properties
 *
 * Parameters from script:
 *   [0] int: id - Object ID
 *   [1] string: path_utf8 - Path to the PSD file
 *   [2] table: props - Properties table with required keys: layer, scale, offsetx, offsety, tag
 *
 * Pushes 4 results: modified (bool), cachekey_hex (string), width (int), height (int)
 *
 * @param sm Script module instance
 * @param param Script module parameter interface
 */
void ptk_script_module_set_props(struct ptk_script_module *sm, struct aviutl2_script_module_param *param);

/**
 * @brief Script function: Get drop configuration
 *
 * Pushes a table result with drop configuration settings.
 *
 * @param sm Script module instance
 * @param param Script module parameter interface
 */
void ptk_script_module_get_drop_config(struct ptk_script_module *sm, struct aviutl2_script_module_param *param);

/**
 * @brief Script function: Draw PSD image
 *
 * Parameters from script:
 *   [0] int: id - Object ID
 *   [1] string: path_utf8 - Path to the PSD file
 *   [2] int: width - Image width
 *   [3] int: height - Image height
 *   [4] string: cachekey_hex - Cache key for storing the result
 *
 * Pushes a boolean result indicating success.
 *
 * @param sm Script module instance
 * @param param Script module parameter interface
 */
void ptk_script_module_draw(struct ptk_script_module *sm, struct aviutl2_script_module_param *param);

/**
 * @brief Script function: Get preferred UI languages
 *
 * Returns an array of preferred UI language codes from the system.
 * Language codes use underscore format (e.g., "ja_JP", "en_US") for Lua compatibility.
 *
 * Pushes a string array result with language codes.
 *
 * @param sm Script module instance
 * @param param Script module parameter interface
 */
void ptk_script_module_get_preferred_languages(struct ptk_script_module *sm, struct aviutl2_script_module_param *param);

/**
 * @brief Script function: Read a text file
 *
 * Reads the entire content of a text file as UTF-8 string.
 * Handles Windows file paths with non-ASCII characters correctly.
 *
 * Parameters from script:
 *   [0] string: path_utf8 - Path to the text file (UTF-8)
 *
 * Pushes a string result containing the file content, or nil if file cannot be opened.
 *
 * @param sm Script module instance
 * @param param Script module parameter interface
 */
void ptk_script_module_read_text_file(struct ptk_script_module *sm, struct aviutl2_script_module_param *param);
