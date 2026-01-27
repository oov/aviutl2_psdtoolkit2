#pragma once

#include <ovbase.h>

struct ptk_anm2_script_mapper;

/**
 * @brief Result of script mapper lookup
 */
struct ptk_anm2_script_mapper_result {
  char const *ptr; // Pointer to effect name (NOT null-terminated), or NULL if not found
  size_t size;     // Length of effect name in bytes
};

/**
 * @brief Create a script mapper from INI configuration
 *
 * Loads script definitions from PSDToolKit.ini and PSDToolKit.user.ini.
 * The mapper provides script_name to effect_name lookups for i18n support.
 *
 * @param err [out] Error information on failure
 * @return Script mapper instance, or NULL on failure
 */
NODISCARD struct ptk_anm2_script_mapper *ptk_anm2_script_mapper_create(struct ov_error *err);

/**
 * @brief Destroy a script mapper
 *
 * @param mapper [in/out] Mapper instance to destroy
 */
void ptk_anm2_script_mapper_destroy(struct ptk_anm2_script_mapper **mapper);

/**
 * @brief Get effect name for a script name
 *
 * Looks up the effect name (e.g., "目パチ@PSDToolKit") for a given
 * script name (e.g., "PSDToolKit.Blinker").
 *
 * @param mapper Script mapper instance
 * @param script_name Script name to look up (UTF-8)
 * @return Result with pointer and size, ptr is NULL if not found
 *         Note: The returned pointer is NOT null-terminated
 */
struct ptk_anm2_script_mapper_result ptk_anm2_script_mapper_get_effect_name(struct ptk_anm2_script_mapper const *mapper,
                                                                            char const *script_name);
