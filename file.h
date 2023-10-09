#ifndef FILE_H
#define FILE_H

#include <stdint.h>
#include <vector>

class FileDescriptorRO {
  public:
    int fd;

    FileDescriptorRO(const char *filename);
    ~FileDescriptorRO();
};

class FileDescriptorWO {
  public:
    int fd;

    FileDescriptorWO(const char *filename);
    ~FileDescriptorWO();
};

std::vector<uint8_t> getBuffer(int fd);
bool pumpBuffer(std::vector<uint8_t> &buffer, int fd);

#endif // FILE_H
