
#include "file.h"

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <unistd.h>

#include <sys/stat.h>

#include <vector>

FileDescriptorRO::FileDescriptorRO(const char *filename) {
    fd = open(filename, O_RDONLY);

    if (fd == -1) {
        printf("Cannot open file for reading\n");
        throw -1;
    }
}

FileDescriptorRO::~FileDescriptorRO() { close(fd); }

FileDescriptorWO::FileDescriptorWO(const char *filename) {
    fd = open(filename, O_WRONLY | O_CREAT, S_IRUSR | S_IWUSR);

    if (fd == -1) {
        printf("Cannot open file %s for writing, cwd %s, errno %d\n", filename,
               get_current_dir_name(), errno);
        throw -1;
    }
}

FileDescriptorWO::~FileDescriptorWO() { close(fd); }

std::vector<uint8_t> getBuffer(int fd) {
    struct stat statbuf;
    if (fstat(fd, &statbuf) != 0) {
        printf("Cannot open file\n");
        throw -1;
    }

    std::vector<uint8_t> buf(statbuf.st_size);

    ssize_t hasRead = read(fd, buf.data(), buf.size());

    if (hasRead == -1) {
        printf("Cannot read from file\n");
        throw -1;
    }

    if ((size_t)hasRead != buf.size()) {
        printf("Short read from file\n");
        throw -1;
    }

    return buf;
}

bool pumpBuffer(std::vector<uint8_t> &buffer, int fd) {

    size_t hasWritten = 0;

    while (hasWritten != buffer.size()) {
        ssize_t written =
            write(fd, buffer.data() + hasWritten, buffer.size() - hasWritten);

        if (written <= 0) {
            if (errno != EAGAIN && errno != EINTR) {
                return false;
            }
        } else {
            hasWritten += (size_t)written;
        }
    }

    return true;
}