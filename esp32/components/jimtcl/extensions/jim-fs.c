/* Jim Tcl Filesystem Extension for ESP32
 *
 * Provides Tcl commands for SPIFFS filesystem access via ESP-IDF VFS:
 *
 *   fs mount ?-partition storage? ?-path /data? ?-maxfiles 5?
 *   fs unmount ?path?
 *   fs read <path>
 *   fs write <path> <data>
 *   fs append <path> <data>
 *   fs delete <path>
 *   fs exists <path>
 *   fs list ?path?
 *   fs info ?partition?
 */

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <sys/stat.h>
#include <dirent.h>
#include "jim.h"
#include "jim-subcmd.h"
#include "esp_spiffs.h"
#include "esp_log.h"

static const char *TAG = "jim-fs";

static int fs_cmd_mount(Jim_Interp *interp, int argc, Jim_Obj *const *argv)
{
    const char *partition = "storage";
    const char *base_path = "/data";
    long max_files = 5;

    /* Parse optional keyword arguments */
    for (int i = 0; i < argc; i += 2) {
        if (i + 1 >= argc) {
            Jim_SetResultString(interp, "missing value for option", -1);
            return JIM_ERR;
        }
        const char *opt = Jim_String(argv[i]);
        if (strcmp(opt, "-partition") == 0) {
            partition = Jim_String(argv[i + 1]);
        } else if (strcmp(opt, "-path") == 0) {
            base_path = Jim_String(argv[i + 1]);
        } else if (strcmp(opt, "-maxfiles") == 0) {
            if (Jim_GetLong(interp, argv[i + 1], &max_files) != JIM_OK) {
                return JIM_ERR;
            }
        } else {
            Jim_SetResultFormatted(interp, "unknown option \"%s\": should be -partition, -path, or -maxfiles", opt);
            return JIM_ERR;
        }
    }

    esp_vfs_spiffs_conf_t conf = {
        .base_path = base_path,
        .partition_label = partition,
        .max_files = (size_t)max_files,
        .format_if_mount_failed = true,
    };

    esp_err_t err = esp_vfs_spiffs_register(&conf);
    if (err != ESP_OK) {
        if (err == ESP_ERR_INVALID_STATE) {
            Jim_SetResultString(interp, "SPIFFS already mounted", -1);
        } else {
            Jim_SetResultFormatted(interp, "SPIFFS mount failed: %s", esp_err_to_name(err));
        }
        return JIM_ERR;
    }

    ESP_LOGI(TAG, "SPIFFS mounted: partition=%s path=%s maxfiles=%ld", partition, base_path, max_files);
    return JIM_OK;
}

static int fs_cmd_unmount(Jim_Interp *interp, int argc, Jim_Obj *const *argv)
{
    const char *partition = NULL;
    if (argc >= 1) {
        partition = Jim_String(argv[0]);
    }

    esp_err_t err = esp_vfs_spiffs_unregister(partition);
    if (err != ESP_OK) {
        Jim_SetResultFormatted(interp, "SPIFFS unmount failed: %s", esp_err_to_name(err));
        return JIM_ERR;
    }

    ESP_LOGI(TAG, "SPIFFS unmounted: partition=%s", partition ? partition : "(default)");
    return JIM_OK;
}

static int fs_cmd_read(Jim_Interp *interp, int argc, Jim_Obj *const *argv)
{
    const char *path = Jim_String(argv[0]);

    FILE *f = fopen(path, "r");
    if (!f) {
        Jim_SetResultFormatted(interp, "cannot open file \"%s\" for reading", path);
        return JIM_ERR;
    }

    /* Get file size */
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (size < 0) {
        fclose(f);
        Jim_SetResultFormatted(interp, "cannot determine size of \"%s\"", path);
        return JIM_ERR;
    }

    if (size == 0) {
        fclose(f);
        Jim_SetResultString(interp, "", 0);
        return JIM_OK;
    }

    char *buf = malloc(size + 1);
    if (!buf) {
        fclose(f);
        Jim_SetResultString(interp, "out of memory", -1);
        return JIM_ERR;
    }

    size_t nread = fread(buf, 1, size, f);
    fclose(f);

    buf[nread] = '\0';
    Jim_SetResult(interp, Jim_NewStringObj(interp, buf, (int)nread));
    free(buf);
    return JIM_OK;
}

static int fs_cmd_write(Jim_Interp *interp, int argc, Jim_Obj *const *argv)
{
    const char *path = Jim_String(argv[0]);
    int data_len;
    const char *data = Jim_GetString(argv[1], &data_len);

    FILE *f = fopen(path, "w");
    if (!f) {
        Jim_SetResultFormatted(interp, "cannot open file \"%s\" for writing", path);
        return JIM_ERR;
    }

    size_t written = fwrite(data, 1, data_len, f);
    fclose(f);

    if ((int)written != data_len) {
        Jim_SetResultFormatted(interp, "write incomplete: %d of %d bytes", (int)written, data_len);
        return JIM_ERR;
    }

    Jim_SetResultInt(interp, (int)written);
    return JIM_OK;
}

static int fs_cmd_append(Jim_Interp *interp, int argc, Jim_Obj *const *argv)
{
    const char *path = Jim_String(argv[0]);
    int data_len;
    const char *data = Jim_GetString(argv[1], &data_len);

    FILE *f = fopen(path, "a");
    if (!f) {
        Jim_SetResultFormatted(interp, "cannot open file \"%s\" for appending", path);
        return JIM_ERR;
    }

    size_t written = fwrite(data, 1, data_len, f);
    fclose(f);

    if ((int)written != data_len) {
        Jim_SetResultFormatted(interp, "append incomplete: %d of %d bytes", (int)written, data_len);
        return JIM_ERR;
    }

    Jim_SetResultInt(interp, (int)written);
    return JIM_OK;
}

static int fs_cmd_delete(Jim_Interp *interp, int argc, Jim_Obj *const *argv)
{
    const char *path = Jim_String(argv[0]);

    if (remove(path) != 0) {
        Jim_SetResultFormatted(interp, "cannot delete \"%s\"", path);
        return JIM_ERR;
    }

    return JIM_OK;
}

static int fs_cmd_exists(Jim_Interp *interp, int argc, Jim_Obj *const *argv)
{
    const char *path = Jim_String(argv[0]);
    struct stat st;

    Jim_SetResultInt(interp, (stat(path, &st) == 0) ? 1 : 0);
    return JIM_OK;
}

static int fs_cmd_list(Jim_Interp *interp, int argc, Jim_Obj *const *argv)
{
    const char *path = "/data";
    if (argc >= 1) {
        path = Jim_String(argv[0]);
    }

    DIR *dir = opendir(path);
    if (!dir) {
        Jim_SetResultFormatted(interp, "cannot open directory \"%s\"", path);
        return JIM_ERR;
    }

    Jim_Obj *result = Jim_NewListObj(interp, NULL, 0);
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        Jim_ListAppendElement(interp, result, Jim_NewStringObj(interp, entry->d_name, -1));
    }
    closedir(dir);

    Jim_SetResult(interp, result);
    return JIM_OK;
}

static int fs_cmd_info(Jim_Interp *interp, int argc, Jim_Obj *const *argv)
{
    const char *partition = NULL;
    if (argc >= 1) {
        partition = Jim_String(argv[0]);
    }

    size_t total = 0, used = 0;
    esp_err_t err = esp_spiffs_info(partition, &total, &used);
    if (err != ESP_OK) {
        Jim_SetResultFormatted(interp, "esp_spiffs_info failed: %s", esp_err_to_name(err));
        return JIM_ERR;
    }

    Jim_Obj *result = Jim_NewListObj(interp, NULL, 0);
    Jim_ListAppendElement(interp, result, Jim_NewStringObj(interp, "total", -1));
    Jim_ListAppendElement(interp, result, Jim_NewIntObj(interp, (jim_wide)total));
    Jim_ListAppendElement(interp, result, Jim_NewStringObj(interp, "used", -1));
    Jim_ListAppendElement(interp, result, Jim_NewIntObj(interp, (jim_wide)used));
    Jim_SetResult(interp, result);
    return JIM_OK;
}

static const jim_subcmd_type fs_command_table[] = {
    {   "mount",
        "?-partition name? ?-path path? ?-maxfiles n?",
        fs_cmd_mount,
        0,
        -1,
        /* Description: Mount SPIFFS filesystem */
    },
    {   "unmount",
        "?partition?",
        fs_cmd_unmount,
        0,
        1,
        /* Description: Unmount SPIFFS filesystem */
    },
    {   "read",
        "path",
        fs_cmd_read,
        1,
        1,
        /* Description: Read entire file and return contents */
    },
    {   "write",
        "path data",
        fs_cmd_write,
        2,
        2,
        /* Description: Write data to file (overwrite) */
    },
    {   "append",
        "path data",
        fs_cmd_append,
        2,
        2,
        /* Description: Append data to file */
    },
    {   "delete",
        "path",
        fs_cmd_delete,
        1,
        1,
        /* Description: Delete a file */
    },
    {   "exists",
        "path",
        fs_cmd_exists,
        1,
        1,
        /* Description: Check if file exists (returns 0 or 1) */
    },
    {   "list",
        "?path?",
        fs_cmd_list,
        0,
        1,
        /* Description: List files in directory */
    },
    {   "info",
        "?partition?",
        fs_cmd_info,
        0,
        1,
        /* Description: Return total and used bytes as dict */
    },
    { NULL }
};

int Jim_fsInit(Jim_Interp *interp)
{
    Jim_PackageProvideCheck(interp, "fs");
    Jim_RegisterSubCmd(interp, "fs", fs_command_table, NULL);
    return JIM_OK;
}
