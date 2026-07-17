#ifndef MAINDOB_STUBS_DOBCONFIG_H
#define MAINDOB_STUBS_DOBCONFIG_H

/* DobConfig Entry Point
 *
 * Usage:
 *   #include <DobConfig.h>
 *
 *   // Settings
 *   char value[256];
 *   dobconfig.settings.Get("system.name", value, sizeof(value));
 *   dobconfig.settings.Set("display.resolution", "1280x1024");
 *
 *   // File associations
 *   char prog[256];
 *   dobconfig.associations.Get(".png", prog, sizeof(prog));
 *   dobconfig.associations.Set(".png", "/SYSTEM/PROGRAMS/imageviewer/imageviewer.mdl");
 *   dobconfig.associations.Remove(".png");
 */

#include <dob/types.h>

/* Settings */
int dobconfig_Get(const char *key, char *value_out, uint32_t max_len);
int dobconfig_Set(const char *key, const char *value);
int dobconfig_List(char *buf, uint32_t max_len);

/* File associations */
int dobconfig_GetAssoc(const char *ext, char *prog_out, uint32_t max_len);
int dobconfig_SetAssoc(const char *ext, const char *prog_path);
int dobconfig_RemoveAssoc(const char *ext);

typedef struct
{
    int (*Get)(const char *key, char *value_out, uint32_t max_len);
    int (*Set)(const char *key, const char *value);
    int (*List)(char *buf, uint32_t max_len);
} dobconfig_settings_t;

typedef struct
{
    int (*Get)(const char *ext, char *prog_out, uint32_t max_len);
    int (*Set)(const char *ext, const char *prog_path);
    int (*Remove)(const char *ext);
} dobconfig_associations_t;

typedef struct
{
    dobconfig_settings_t settings;
    dobconfig_associations_t associations;
} dobconfig_interface_t;

extern dobconfig_interface_t dobconfig;

#endif
