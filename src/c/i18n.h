#pragma once

#include <ovbase.h>

struct aviutl2_config_handle;

/**
 * @brief Set the config handle for i18n functionality
 *
 * This function stores the config handle provided by AviUtl2 for later use
 * in translation functions.
 *
 * @param handle Config handle provided by AviUtl2
 */
void ptk_i18n_set_config_handle(struct aviutl2_config_handle *handle);

/**
 * @brief Get translated text from language settings (wchar_t version)
 *
 * Uses the AviUtl2 config handle to get translated text from language settings.
 * This is a thin wrapper around the SDK function.
 *
 * @param section Section name in .aul2 file (wchar_t)
 * @param text Original text/key name (wchar_t)
 * @return Translated wide string, or NULL if no translation found or handle not set.
 *         The returned pointer is valid until language settings are updated.
 */
wchar_t const *ptk_i18n_get_translated_text_w(wchar_t const *section, wchar_t const *text);

/**
 * @brief Get translated text from language settings (UTF-8 version)
 *
 * Converts UTF-8 section and text to wchar_t, then calls the wchar_t version.
 *
 * @param section Section name in .aul2 file (UTF-8)
 * @param text Original text/key name (UTF-8)
 * @return Translated wide string, or NULL if no translation found, handle not set,
 *         or conversion failed. The returned pointer is valid until language
 *         settings are updated.
 */
wchar_t const *ptk_i18n_get_translated_text(char const *section, char const *text);

/**
 * @brief Get translated text from language settings (UTF-8 version with explicit length)
 *
 * Like ptk_i18n_get_translated_text but accepts explicit lengths for section and text.
 * Useful when the strings are not null-terminated.
 *
 * @param section Section name in .aul2 file (UTF-8)
 * @param section_len Length of section in bytes
 * @param text Original text/key name (UTF-8)
 * @param text_len Length of text in bytes
 * @return Translated wide string, or NULL if no translation found, handle not set,
 *         or conversion failed. The returned pointer is valid until language
 *         settings are updated.
 */
wchar_t const *
ptk_i18n_get_translated_text_n(char const *section, size_t section_len, char const *text, size_t text_len);
