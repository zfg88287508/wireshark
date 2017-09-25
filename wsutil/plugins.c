/* plugins.c
 * plugin routines
 *
 * Wireshark - Network traffic analyzer
 * By Gerald Combs <gerald@wireshark.org>
 * Copyright 1998 Gerald Combs
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#include "config.h"

#ifdef HAVE_PLUGINS

#include <time.h>

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>

#include <glib.h>
#include <gmodule.h>

#include <wsutil/filesystem.h>
#include <wsutil/privileges.h>
#include <wsutil/file_util.h>
#include <wsutil/report_message.h>

#include <wsutil/plugins.h>
#include <wsutil/ws_printf.h> /* ws_debug_printf */

typedef struct _plugin {
    GModule        *handle;       /* handle returned by g_module_open */
    gchar          *name;         /* plugin name */
    const gchar    *version;      /* plugin version */
    GString        *types;        /* description with types this plugin supports */
} plugin;

static GHashTable *plugins_table = NULL;

static void
free_plugin(gpointer _p)
{
    plugin *p = (plugin *)_p;
    g_module_close(p->handle);
    g_free(p->name);
    g_string_free(p->types, TRUE);
    g_free(p);
}

/*
 * Add a new plugin type.
 * Takes a callback routine as an argument; it is called for each plugin
 * we find, and handed a handle for the plugin, the name of the plugin,
 * and the version string for the plugin.  The plugin returns TRUE if
 * it's a plugin for that type and FALSE if not.
 */
typedef struct {
    const char *name;
    plugin_check_type_callback check_type;
} plugin_type;

static GSList *plugin_types = NULL;

static void
free_plugin_type(gpointer p, gpointer user_data _U_)
{
    g_free(p);
}

void
add_plugin_type(const char *name, plugin_check_type_callback check_type)
{
    plugin_type *new_type;

    new_type = (plugin_type *)g_malloc(sizeof (plugin_type));
    new_type->name = name;
    new_type->check_type = check_type;
    plugin_types = g_slist_prepend(plugin_types, new_type);
}

static void
call_plugin_callback(gpointer data, gpointer user_data)
{
    plugin_type *type = (plugin_type *)data;
    plugin *new_plug = (plugin *)user_data;

    if (type->check_type(new_plug->handle)) {
        /* The plugin supports this type */
        if (new_plug->types->len > 0)
            g_string_append(new_plug->types, ", ");
        g_string_append(new_plug->types, type->name);
    }
}

static void
plugins_scan_dir(const char *dirname, plugin_load_failure_mode mode)
{
#define FILENAME_LEN        1024
    WS_DIR        *dir;             /* scanned directory */
    WS_DIRENT     *file;            /* current file */
    const char    *name;
    gchar          filename[FILENAME_LEN];   /* current file name */
    GModule       *handle;          /* handle returned by g_module_open */
    gpointer       gp;
    plugin        *new_plug;
    gchar         *dot;

    if (!g_file_test(dirname, G_FILE_TEST_EXISTS) || !g_file_test(dirname, G_FILE_TEST_IS_DIR)) {
        return;
    }

    if ((dir = ws_dir_open(dirname, 0, NULL)) != NULL)
    {
        while ((file = ws_dir_read_name(dir)) != NULL)
        {
            name = ws_dir_get_name(file);

            /*
             * GLib 2.x defines G_MODULE_SUFFIX as the extension used on
             * this platform for loadable modules.
             */
            /* skip anything but files with G_MODULE_SUFFIX */
            dot = strrchr(name, '.');
            if (dot == NULL || strcmp(dot+1, G_MODULE_SUFFIX) != 0)
                continue;
#if WIN32
            if (strncmp(name, "nordic_ble.dll", 14) == 0)
                /*
                 * Skip the Nordic BLE Sniffer dll on WIN32 because
                 * the dissector has been added as internal.
                 */
                continue;
#endif
            g_snprintf(filename, FILENAME_LEN, "%s" G_DIR_SEPARATOR_S "%s",
                       dirname, name);

            /*
             * Check if the same name is already registered.
             */
            if (g_hash_table_lookup(plugins_table, name)) {
                /* Yes, it is. */
                if (mode == REPORT_LOAD_FAILURE) {
                    report_warning("The plugin '%s' was found "
                            "in multiple directories.\n", name);
                }
                continue;
            }

            if ((handle = g_module_open(filename, G_MODULE_BIND_LOCAL)) == NULL)
            {
                /*
                 * Only report load failures if we were asked to.
                 *
                 * XXX - we really should put different types of plugins
                 * (libwiretap, libwireshark) in different subdirectories,
                 * give libwiretap and libwireshark init routines that
                 * load the plugins, and have them scan the appropriate
                 * subdirectories so tha we don't even *try* to, for
                 * example, load libwireshark plugins in programs that
                 * only use libwiretap.
                 */
                if (mode == REPORT_LOAD_FAILURE) {
                    report_failure("Couldn't load module %s: %s", filename,
                                   g_module_error());
                }
                continue;
            }

            if (!g_module_symbol(handle, "version", &gp))
            {
                report_failure("The plugin %s has no version symbol", name);
                g_module_close(handle);
                continue;
            }

            new_plug = (plugin *)g_malloc(sizeof(plugin));
            new_plug->handle = handle;
            new_plug->name = g_strdup(name);
            new_plug->version = (char *)gp;
            new_plug->types = g_string_new(NULL);

            /*
             * Hand the plugin to each of the plugin type callbacks.
             */
            g_slist_foreach(plugin_types, call_plugin_callback, new_plug);

            /*
             * Does this dissector do anything useful?
             */
            if (new_plug->types->len == 0)
            {
                /*
                 * No.
                 *
                 * Only report this failure if we were asked to; it might
                 * just mean that it's a plugin type that this program
                 * doesn't support, such as a libwireshark plugin in
                 * a program that doesn't use libwireshark.
                 *
                 * XXX - we really should put different types of plugins
                 * (libwiretap, libwireshark) in different subdirectories,
                 * give libwiretap and libwireshark init routines that
                 * load the plugins, and have them scan the appropriate
                 * subdirectories so tha we don't even *try* to, for
                 * example, load libwireshark plugins in programs that
                 * only use libwiretap.
                 */
                if (mode == REPORT_LOAD_FAILURE) {
                    report_failure("The plugin '%s' has no registration routines",
                                   name);
                }
                free_plugin(new_plug);
                continue;
            }

            /*
             * OK, add it to the list of plugins.
             */
            g_hash_table_insert(plugins_table, new_plug->name, new_plug);
        }
        ws_dir_close(dir);
    }
}


/*
 * Scan for plugins.
 */
void
scan_plugins(plugin_load_failure_mode mode)
{
    const char *plugin_dir;
    const char *name;
    char *plugin_dir_path;
    WS_DIR *dir;                /* scanned directory */
    WS_DIRENT *file;            /* current file */

    if (plugins_table == NULL)    /* only scan for plugins once */
    {
        plugins_table = g_hash_table_new_full(g_str_hash, g_str_equal, NULL, free_plugin);
        /*
         * Scan the global plugin directory.
         * If we're running from a build directory, scan the "plugins"
         * subdirectory, as that's where plugins are located in an
         * out-of-tree build. If we find subdirectories scan those since
         * they will contain plugins in the case of an in-tree build.
         */
        plugin_dir = get_plugins_dir();
        if (plugin_dir == NULL)
        {
            /* We couldn't find the plugin directory. */
            return;
        }
        if (running_in_build_directory())
        {
            if ((dir = ws_dir_open(plugin_dir, 0, NULL)) != NULL)
            {
                plugins_scan_dir(plugin_dir, mode);
                while ((file = ws_dir_read_name(dir)) != NULL)
                {
                    name = ws_dir_get_name(file);
                    if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0)
                        continue;        /* skip "." and ".." */
                    /*
                     * Get the full path of a ".libs" subdirectory of that
                     * directory.
                     */
                    plugin_dir_path = g_strdup_printf(
                        "%s" G_DIR_SEPARATOR_S "%s" G_DIR_SEPARATOR_S ".libs",
                        plugin_dir, name);
                    if (test_for_directory(plugin_dir_path) != EISDIR) {
                        /*
                         * Either it doesn't refer to a directory or it
                         * refers to something that doesn't exist.
                         *
                         * Assume that means that the plugins are in
                         * the subdirectory of the plugin directory, not
                         * a ".libs" subdirectory of that subdirectory.
                         */
                        g_free(plugin_dir_path);
                        plugin_dir_path = g_strdup_printf("%s" G_DIR_SEPARATOR_S "%s",
                            plugin_dir, name);
                    }
                    plugins_scan_dir(plugin_dir_path, mode);
                    g_free(plugin_dir_path);
                }
                ws_dir_close(dir);
            }
        }
        else
        {
            plugins_scan_dir(get_plugins_dir_with_version(), mode);
        }

        /*
         * If the program wasn't started with special privileges,
         * scan the users plugin directory.  (Even if we relinquish
         * them, plugins aren't safe unless we've *permanently*
         * relinquished them, and we can't do that in Wireshark as,
         * if we need privileges to start capturing, we'd need to
         * reclaim them before each time we start capturing.)
         */
        if (!started_with_special_privs())
        {
            plugins_scan_dir(get_plugins_pers_dir_with_version(), mode);
        }
    }
}

struct plugin_description {
    const char *name;
    const char *version;
    const char *types;
    const char *filename;
};

static void
add_plugin_type_description(gpointer key _U_, gpointer value, gpointer user_data)
{
    GPtrArray *descriptions = (GPtrArray *)user_data;
    struct plugin_description *plug_desc;
    plugin *plug = (plugin *)value;

    plug_desc = g_new(struct plugin_description, 1);
    plug_desc->name = plug->name;
    plug_desc->version = plug->version;
    plug_desc->types = plug->types->str;
    plug_desc->filename = g_module_name(plug->handle);

    g_ptr_array_add(descriptions, plug_desc);
}

static int
compare_descriptions(gconstpointer _a, gconstpointer _b)
{
    const struct plugin_description *a = *(const struct plugin_description **)_a;
    const struct plugin_description *b = *(const struct plugin_description **)_b;

    return strcmp(a->name, b->name);
}

WS_DLL_PUBLIC void
plugins_get_descriptions(plugin_description_callback callback, void *user_data)
{
    GPtrArray *descriptions;
    struct plugin_description *desc;

    descriptions = g_ptr_array_sized_new(g_hash_table_size(plugins_table));
    g_hash_table_foreach(plugins_table, add_plugin_type_description, descriptions);
    g_ptr_array_sort(descriptions, compare_descriptions);
    for (guint i = 0; i < descriptions->len; i++) {
        desc = (struct plugin_description *)descriptions->pdata[i];
        callback(desc->name, desc->version, desc->types, desc->filename, user_data);
    }
    g_ptr_array_free(descriptions, TRUE);
}

static void
print_plugin_description(const char *name, const char *version,
                         const char *description, const char *filename,
                         void *user_data _U_)
{
    ws_debug_printf("%s\t%s\t%s\t%s\n", name, version, description, filename);
}

void
plugins_dump_all(void)
{
    plugins_get_descriptions(print_plugin_description, NULL);
}

void
plugins_cleanup(void)
{
    g_hash_table_destroy(plugins_table);
    g_slist_foreach(plugin_types, free_plugin_type, NULL);
    g_slist_free(plugin_types);
}

#endif /* HAVE_PLUGINS */

/*
 * Editor modelines
 *
 * Local Variables:
 * c-basic-offset: 4
 * tab-width: 8
 * indent-tabs-mode: nil
 * End:
 *
 * ex: set shiftwidth=4 tabstop=8 expandtab:
 * :indentSize=4:tabSize=8:noTabs=true:
 */
