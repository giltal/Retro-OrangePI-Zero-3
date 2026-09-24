/*
 * core-info -- load a libretro core and report what it says about itself.
 *
 *   core-info /usr/lib/libretro/ppsspp_libretro.so
 *
 * Prints the core's library_name / version / extensions and every core option
 * with its allowed values and default. Run it ON THE BOARD, so a successful
 * run also proves the .so links against the target's libraries.
 *
 * Why this exists: option files (.opt) are matched by exact key and value,
 * and an invalid key or value is silently ignored -- the core just keeps its
 * default. The reference project learned to read the values out of the built
 * core rather than guess them; this does that directly. The launcher's
 * save-state table also needs each core's exact library_name.
 *
 * Only the legacy options interface is offered (GET_CORE_OPTIONS_VERSION
 * answers 0), so cores fall back to RETRO_ENVIRONMENT_SET_VARIABLES, whose
 * value strings are "Description; default|other|other". The first listed
 * value is the default.
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <dlfcn.h>

/* Minimal slice of libretro.h -- only what this tool touches. */
#define RETRO_ENVIRONMENT_SET_VARIABLES           16
#define RETRO_ENVIRONMENT_GET_CORE_OPTIONS_VERSION 52

struct retro_system_info {
	const char *library_name;
	const char *library_version;
	const char *valid_extensions;
	bool need_fullpath;
	bool block_extract;
};

struct retro_variable {
	const char *key;
	const char *value;
};

typedef bool (*retro_environment_t)(unsigned cmd, void *data);

static int n_options;

static bool environ_cb(unsigned cmd, void *data)
{
	switch (cmd) {
	case RETRO_ENVIRONMENT_GET_CORE_OPTIONS_VERSION:
		*(unsigned *)data = 0;
		return true;
	case RETRO_ENVIRONMENT_SET_VARIABLES: {
		const struct retro_variable *v = data;
		for (; v && v->key; v++, n_options++)
			printf("option %s = %s\n", v->key, v->value ? v->value : "");
		return true;
	}
	default:
		return false;
	}
}

int main(int argc, char **argv)
{
	if (argc != 2) {
		fprintf(stderr, "usage: %s <core_libretro.so>\n", argv[0]);
		return 2;
	}
	void *h = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
	if (!h) {
		fprintf(stderr, "dlopen failed: %s\n", dlerror());
		return 1;
	}
	void (*get_info)(struct retro_system_info *) = dlsym(h, "retro_get_system_info");
	void (*set_env)(retro_environment_t) = dlsym(h, "retro_set_environment");
	if (!get_info || !set_env) {
		fprintf(stderr, "not a libretro core (missing entry points)\n");
		return 1;
	}

	struct retro_system_info si;
	memset(&si, 0, sizeof(si));
	get_info(&si);
	printf("library_name    = %s\n", si.library_name ? si.library_name : "(null)");
	printf("library_version = %s\n", si.library_version ? si.library_version : "(null)");
	printf("extensions      = %s\n", si.valid_extensions ? si.valid_extensions : "(null)");
	printf("need_fullpath   = %d\n", si.need_fullpath);

	set_env(environ_cb);
	printf("options         = %d\n", n_options);
	return 0;
}
