#pragma once

#include <ovbase.h>

struct ptk_config;

/**
 * @brief Create and initialize configuration with default values
 *
 * @param err Error information
 * @return Configuration structure pointer on success, NULL on failure
 */
struct ptk_config *ptk_config_create(struct ov_error *const err);

/**
 * @brief Destroy configuration and free memory
 *
 * @param config Configuration structure pointer
 */
void ptk_config_destroy(struct ptk_config **const config);

/**
 * @brief Load configuration from JSON file
 *
 * @param config Configuration structure
 * @param err Error information
 * @return true on success, false on failure
 */
bool ptk_config_load(struct ptk_config *const config, struct ov_error *const err);

/**
 * @brief Save configuration to JSON file
 *
 * @param config Configuration structure
 * @param err Error information
 * @return true on success, false on failure
 */
bool ptk_config_save(struct ptk_config const *const config, struct ov_error *const err);

// Manual drop trigger settings

bool ptk_config_get_manual_shift_wav(struct ptk_config const *const config,
                                     bool *const value,
                                     struct ov_error *const err);
bool ptk_config_set_manual_shift_wav(struct ptk_config *const config, bool const value, struct ov_error *const err);

bool ptk_config_get_manual_shift_psd(struct ptk_config const *const config,
                                     bool *const value,
                                     struct ov_error *const err);
bool ptk_config_set_manual_shift_psd(struct ptk_config *const config, bool const value, struct ov_error *const err);

bool ptk_config_get_manual_wav_txt_pair(struct ptk_config const *const config,
                                        bool *const value,
                                        struct ov_error *const err);
bool ptk_config_set_manual_wav_txt_pair(struct ptk_config *const config, bool const value, struct ov_error *const err);

bool ptk_config_get_manual_object_audio_text(struct ptk_config const *const config,
                                             bool *const value,
                                             struct ov_error *const err);
bool ptk_config_set_manual_object_audio_text(struct ptk_config *const config,
                                             bool const value,
                                             struct ov_error *const err);

// External API drop trigger settings

bool ptk_config_get_external_wav_txt_pair(struct ptk_config const *const config,
                                          bool *const value,
                                          struct ov_error *const err);
bool ptk_config_set_external_wav_txt_pair(struct ptk_config *const config,
                                          bool const value,
                                          struct ov_error *const err);

bool ptk_config_get_external_object_audio_text(struct ptk_config const *const config,
                                               bool *const value,
                                               struct ov_error *const err);
bool ptk_config_set_external_object_audio_text(struct ptk_config *const config,
                                               bool const value,
                                               struct ov_error *const err);

// Debug mode setting

bool ptk_config_get_debug_mode(struct ptk_config const *const config, bool *const value, struct ov_error *const err);
bool ptk_config_set_debug_mode(struct ptk_config *const config, bool const value, struct ov_error *const err);

// Draft mode setting (use fast quality for IPC rendering)

bool ptk_config_get_draft_mode(struct ptk_config const *const config, bool *const value, struct ov_error *const err);
bool ptk_config_set_draft_mode(struct ptk_config *const config, bool const value, struct ov_error *const err);
