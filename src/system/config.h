#pragma once

#include <stdbool.h>

#define WC_MAX_CONFIG_ENTRIES 64
#define WC_MAX_KEY_LENGTH 32
#define WC_MAX_VALUE_LENGTH 64

typedef struct wc_config_entry
{
	char key[WC_MAX_KEY_LENGTH];
	char value[WC_MAX_VALUE_LENGTH];
} wc_config_entry;

typedef struct wc_config
{
	wc_config_entry entries[WC_MAX_CONFIG_ENTRIES];
	int count;
} wc_config;

// Load config from a file (returns non-zero on failure)
int wc_config_load(wc_config* config, const char* filename);

// Save config to a file (returns non-zero on failure)
int wc_config_save(const wc_config* config, const char* filename);

// Get a string value (returns NULL if not found)
const char* wc_config_get_str(const wc_config* config, const char* key, const char* default_value);

// Get an integer value (returns default if not found or invalid)
int wc_config_get_int(const wc_config* config, const char* key, int default_value);

// Get a boolean value (returns default if not found or invalid)
bool wc_config_get_bool(const wc_config* config, const char* key, bool default_value);

// Set a string value (returns non-zero if key is too long or table is full)
int wc_config_set_str(wc_config* config, const char* key, const char* value);

// Set an integer value (returns non-zero if key is too long or table is full)
int wc_config_set_int(wc_config* config, const char* key, int value);

// Set a boolean value (returns non-zero if key is too long or table is full)
int wc_config_set_bool(wc_config* config, const char* key, bool value);
