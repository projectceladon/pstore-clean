#include <stdio.h>
#include <dirent.h>
#include <sys/types.h>
#include <time.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/mount.h>
#include <linux/fs.h>
#include <cutils/fs.h>

#define LOG_TAG "pstore-clean"
#include <cutils/log.h>
#define MNT "/dev/pstore"
#define DST "/data/dontpanic/apanic_console"
#define BUFFER_SIZE 4096
#define MAX_DIR_COUNT 1024
#define MAX_COUNT 1024

/* This tool iteratively mounts, copies, and clean kernel dumps
 * from the pstore filesystem. Otherwise that data will accumulate
 * in NVRAM and eventually fill storage.
 */

int filecopy(const char *srcfile, int dstfd)
{
    int srcfd, read_ret, write_ret;
    char buffer[BUFFER_SIZE];

    srcfd = open(srcfile, O_RDONLY);
    if (srcfd == -1) {
        ALOGE("Open %s error: %s\n", srcfile, strerror(errno));
        return -1;
    }

    do {
        memset(buffer, 0, BUFFER_SIZE);
        read_ret = read(srcfd, buffer, BUFFER_SIZE);
        if (read_ret == -1) {
            ALOGE("Read %s error: %s\n", srcfile, strerror(errno));
            close(srcfd);
            return -1;
        }

        write_ret = write(dstfd, buffer, read_ret);
        if(write_ret == -1) {
            ALOGE("Write error: %s\n", strerror(errno));
            close(srcfd);
            return -1;
        }
    } while(read_ret > 0 && write_ret > 0);

    close(srcfd);
    return 0;
}

int dir_not_empty(char* path)
{
    DIR* dir;
    if ((dir = opendir(path)) == NULL) {
        ALOGE("Opendir %s failed (%s)", MNT, strerror(errno));
        return -1;
    }

    struct dirent* dent = NULL;
    while ((dent = readdir(dir)) != NULL) {
        if ((strcmp(dent->d_name, ".") != 0) && (strcmp(dent->d_name, "..") != 0)) {
            closedir(dir);
            return 1; //dir is not empty
        }
    }
    closedir(dir);
    return 0; //dir is empty
}

int is_fs_mounted(char* path)
{
    FILE* proc_mounts = NULL;
    char part [PAGE_SIZE];
    int is_mounted = 0;

    if ((proc_mounts = fopen("/proc/mounts", "r")) != NULL) {
        while (fgets(part, PAGE_SIZE, proc_mounts) != NULL) {
            if (strstr(part, path) != NULL) {
                is_mounted = 1;
                break;
            }
        }
        fclose(proc_mounts);
    }
    return is_mounted;
}

int main()
{
    int n = 0;
    int count = 0;
    int status;
    int dstfd = -1;
    char srcfile[256];
    struct dirent **namelist = NULL;
    int namelist_len = 0;

    umask(0027);

    struct dirent* dent = NULL;

    do {
        count++;
        if (!is_fs_mounted(MNT)) {
            ALOGE("Pstore fs isn't mounted. Exiting.");
            goto error1;
        }
        status = dir_not_empty(MNT);
        if (status == -1) {
            ALOGE("Opendir %s failed (%s)", MNT, strerror(errno));
            goto error2;
        }
        else if (status == 0) {
            ALOGI("Pstore is clean.");
            break;
        }

        ALOGW("Kernel pstore crash dump found, copying to " DST "\n");

        if ((dstfd = open(DST, O_CREAT | O_TRUNC | O_WRONLY, 0666)) < 0) {
            ALOGE("Failed to open " DST " (%s)", strerror(errno));
            goto error2;
        }

        if ((namelist_len = scandir(MNT, &namelist, NULL, alphasort)) < 1) {
            ALOGE("Failed to list " MNT " (%s)", strerror(errno));
            goto error2;
        }

        for (n = namelist_len - 1; n >= 0; n--) {
            dent = namelist[n];
            if ((strcmp(dent->d_name, ".") == 0) || (strcmp(dent->d_name, "..") == 0))
                continue;

            snprintf(srcfile, sizeof(srcfile), "%s/%s", MNT, dent->d_name);
            if (filecopy(srcfile, dstfd) == -1) {
                ALOGE("Copy file %s failed (%s)", srcfile, strerror(errno));
            }
            if (unlink(srcfile) != 0) {
                ALOGE("Remove %s failed (%s)", srcfile, strerror(errno));
                goto error3;
            }
        }
    } while(status > 0 && count < MAX_COUNT);

    if (umount(MNT)) {
        ALOGE("Umount %s failed (%s)", srcfile, strerror(errno));
        goto error1;
    }

    if (rmdir(MNT))
        ALOGE("Remove dir %s failed (%s)", MNT, strerror(errno));

    return 0;

error3:
    for (n = 0; n < namelist_len; n++) {
        free(namelist[n]);
    }
    free(namelist);

error2:
    if (dstfd >= 0)
        close(dstfd);

    if (umount(MNT))
        ALOGE("Umount %s failed (%s)", srcfile, strerror(errno));

error1:
    if (rmdir(MNT))
        ALOGE("Remove dir %s failed (%s)", MNT, strerror(errno));

    ALOGE("Clean pstore failed.\n");
    return -1;
}
