#ifndef TAIKO_CONFIG_H
#define TAIKO_CONFIG_H

#ifdef __cplusplus
extern "C" {
#endif

/* Load taiko_config.cfg and publish its settings to the existing runtime
 * environment switches. Explicit process environment values take precedence. */
void taiko_config_load(void);

/* Absolute or working-directory-relative path selected by the loader. */
const char* taiko_config_path(void);

/* Replace or append one INI value while retaining the rest of the file. */
int taiko_config_set(const char* section, const char* key, const char* value);

#ifdef __cplusplus
}
#endif

#endif
