/* Standalone read/write mapping and unlink probe for the stock FW4 runtime. */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

int main(void)
{
    char name[64];
    snprintf(name, sizeof name, "/kobalt-wpe-probe-%ld", (long)getpid());
    int writer = shm_open(name, O_CREAT | O_EXCL | O_RDWR, 0600);
    if (writer < 0) {
        fprintf(stderr, "shm_open: %s\n", strerror(errno));
        return 1;
    }
    int passed = 0;
    if (!ftruncate(writer, 4096) && pwrite(writer, "WPE", 3, 0) == 3) {
        int reader = shm_open(name, O_RDONLY, 0);
        if (reader >= 0) {
            const void *bytes = mmap(NULL, 4096, PROT_READ, MAP_SHARED, reader, 0);
            close(reader);
            if (bytes != MAP_FAILED) {
                passed = !shm_unlink(name) && !memcmp(bytes, "WPE", 3);
                munmap((void *)bytes, 4096);
            }
        }
    }
    close(writer);
    if (!passed) {
        shm_unlink(name);
        return 1;
    }
    puts("shm_open, mmap and shm_unlink passed");
    return 0;
}
