#ifndef ANT_CONFIG_CONFIG_H
#define ANT_CONFIG_CONFIG_H

#include <wchar.h>
#include <stdbool.h>

/* sections of data\config.ant */
enum {
	CONFIG_PATH,   /* [path]   program folders, searched before PATH */
	CONFIG_EXPORT, /* [export] environment variables */
	CONFIG_ALIAS,  /* [alias]  name = command */
	CONFIG_LINK,   /* [link]   shortcuts */
	CONFIG_SECTIONS
};

extern bool ant_config_file (wchar_t *, int);
extern bool ant_config_load (void);
extern bool ant_config_open (void);
extern bool ant_config_created (void);
extern const wchar_t *ant_config_warnings (void);

extern void ant_config_parse (const wchar_t *);
extern wchar_t *ant_config_template (const wchar_t *);
extern int ant_config_expand (const wchar_t *, wchar_t *, int);

extern int ant_config_count (int);
extern const wchar_t *ant_config_name (int, int);
extern const wchar_t *ant_config_raw (int, int);
extern bool ant_config_value (int, int, wchar_t *, int);
extern const wchar_t *ant_config_alias (const wchar_t *);
extern bool ant_config_link (const wchar_t *, wchar_t *, int);
extern int ant_config_path (wchar_t *, int);

#endif
