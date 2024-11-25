#ifndef UTIL_H
#define UTIL_H

#include <pthread.h>
#include <stddef.h>
#include <stdint.h>
#include <string>

void hexdump(const uint8_t *buffer, size_t sz);
std::string hexenc(const uint8_t *buffer, size_t sz);

class Mutex {
  public:
    pthread_mutex_t mut;

    Mutex();
    ~Mutex();
};

class LockGuard {
    Mutex &mut;

  public:
    LockGuard(Mutex &mut_);
    ~LockGuard();
};

#endif // UTIL_H