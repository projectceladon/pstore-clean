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

#define LOG_TAG "pstore-clean"
#include <cutils/log.h>
#define MNT "/dev/pstore"
#define DST "/data/kpanic/pstore"
#define BUFFER_SIZE 4096
#define MAX_DIR_COUNT 1024
#define MAX_COUNT 1024

/* This tool iteratively mounts, copies, and clean kernel dumps
 * from the pstore filesystem. Otherwise that data will accumulate
 * in NVRAM and eventually fill storage.
 */

int filecopy(const char *srcfile, const char *dstfile)
{
    int srcfd, dstfd, read_ret, write_ret;
    char buffer[BUFFER_SIZE];

    srcfd = open(srcfile, O_RDONLY);
    if (srcfd == -1) {
        ALOGE("Open %s error: %s\n", srcfile, strerror(errno));
        return -1;
    }

    dstfd = open(dstfile, O_WRONLY|O_CREAT, 0640);
    if (dstfd == -1) {
        close(srcfd);
        ALOGE("Open %s error: %s\n", dstfile, strerror(errno));
        return -1;
    }

    do {
        memset(buffer, BUFFER_SIZE, 0);
        read_ret = read(srcfd, buffer, BUFFER_SIZE);
        if (read_ret == -1) {
            ALOGE("Read %s error: %s\n", srcfile, strerror(errno));
            close(srcfd);
            close(dstfd);
            return -1;
        }

        write_ret = write(dstfd, buffer, read_ret);
        if(write_ret == -1) {
            ALOGE("Write %s error: %s\n", dstfile, strerror(errno));
            close(srcfd);
            close(dstfd);
            return -1;
        }
    } while(read_ret > 0 && write_ret > 0);

    close(srcfd);
    close(dstfd);
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

int main()
{
    int n = 0;
    int count = 0;
    int rc,status;
    char srcfile[256];
    char dstpath[128];
    char dstfile[256];
    DIR* dir;

    umask(0027);
    mkdir(MNT, 0755);

    if (system("/system/bin/mkdir -p '"DST"'") != 0) {
        ALOGE("Unable to create %s (%s)\n", DST, strerror(errno));
    }

    struct dirent* dent = NULL;
    time_t cur_time = time(NULL);
    struct tm *gmt = gmtime(&cur_time);
    char date[12];
    snprintf(date, sizeof(date), "%d-%d-%d", gmt->tm_year+1900, gmt->tm_mon+1, gmt->tm_mday);

    do {
        count++;
        rc = mount("pstore", MNT, "pstore", 0, NULL);
        if (rc) {
            ALOGE("Mount pstore failed (%s)", strerror(errno));
            goto error1;
        }
        status = dir_not_empty(MNT);
        if (status == -1) {
            ALOGE("Opendir %s failed (%s)", MNT, strerror(errno));
            goto error2;
        }
        else if (status == 0) {
            ALOGI("Pstore is clean.");
            umount(MNT);
            break;
        }
        dir = opendir(MNT);

        if (!dir) {
            ALOGE("Opendir %s failed (%s)", MNT, strerror(errno));
            goto error2;
        }

        do {
            snprintf(dstpath, sizeof(dstpath), "%s/%s-%d", DST, date, n);
        }while(!access(dstpath, F_OK) && ++n < MAX_DIR_COUNT);

        if (mkdir(dstpath, 0755) == -1)
            ALOGE("Unable to create %s (%s)\n", dstpath, strerror(errno));

        ALOGW("Kernel pstore crash dump found, copying to %s\n", dstpath);

        while((dent = readdir(dir))) {
            if ((strcmp(dent->d_name, ".") == 0) || (strcmp(dent->d_name, "..") == 0))
                continue;

            snprintf(srcfile, sizeof(srcfile), "%s/%s", MNT, dent->d_name);
            snprintf(dstfile, sizeof(dstfile), "%s/%s", dstpath, dent->d_name);
            if (filecopy(srcfile, dstfile) == -1) {
                ALOGE("Copy file %s failed (%s)", srcfile, strerror(errno));
            }
            if (unlink(srcfile) != 0) {
                ALOGE("Remove %s failed (%s)", srcfile, strerror(errno));
                goto error3;
            }
        }
        closedir(dir);
        if (umount(MNT)) {
            ALOGE("Umount %s failed (%s)", srcfile, strerror(errno));
            goto error1;
        }

    } while(status > 0 && count < MAX_COUNT);

    if (rmdir(MNT))
        ALOGE("Remove dir %s failed (%s)", MNT, strerror(errno));

    return 0;

error3:
    closedir(dir);

error2:
    if (umount(MNT))
        ALOGE("Umount %s failed (%s)", srcfile, strerror(errno));

error1:
    if (rmdir(MNT))
        ALOGE("Remove dir %s failed (%s)", MNT, strerror(errno));

    ALOGE("Clean pstore failed.\n");
    return -1;
}
