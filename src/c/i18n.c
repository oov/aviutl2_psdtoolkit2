#include "i18n.h"

#include <string.h>

#include <aviutl2_config2.h>

#include <ovutf.h>

static struct aviutl2_config_handle *g_config = NULL;

void ptk_i18n_set_config_handle(struct aviutl2_config_handle *const handle) { g_config = handle; }

wchar_t const *ptk_i18n_get_translated_text_w(wchar_t const *const section, wchar_t const *const text) {
  if (!g_config || !g_config->get_language_text || !section || !text) {
    return NULL;
  }

  wchar_t const *const result = g_config->get_language_text(g_config, section, text);
  if (!result || result == text) {
    // No translation found (SDK returns the argument pointer if undefined)
    return NULL;
  }

  return result;
}

wchar_t const *ptk_i18n_get_translated_text_n(char const *const section,
                                              size_t const section_len,
                                              char const *const text,
                                              size_t const text_len) {
  if (!g_config || !g_config->get_language_text || !section || section_len == 0 || !text || text_len == 0) {
    return NULL;
  }

  // Convert section and text from UTF-8 to wchar_t
  wchar_t section_wchar[256];
  wchar_t text_wchar[256];

  if (ov_utf8_to_wchar(section, section_len, section_wchar, 256, NULL) == 0) {
    return NULL;
  }
  if (ov_utf8_to_wchar(text, text_len, text_wchar, 256, NULL) == 0) {
    return NULL;
  }

  wchar_t const *const result = g_config->get_language_text(g_config, section_wchar, text_wchar);
  if (!result || result == text_wchar) {
    // No translation found (SDK returns the argument pointer if undefined)
    return NULL;
  }

  return result;
}

wchar_t const *ptk_i18n_get_translated_text(char const *const section, char const *const text) {
  if (!section || !text) {
    return NULL;
  }
  return ptk_i18n_get_translated_text_n(section, strlen(section), text, strlen(text));
}
