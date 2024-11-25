#include "util.h"

#include <cctype>
#include <stdio.h>

void hexdump(const uint8_t *buffer, size_t sz) {
    for (size_t i = 0; i < sz; i += 16) {
        printf("%08zX   ", i);

        for (size_t j = 0; j < 16; j++) {
            if (i + j < sz) {
                printf("%02hhx ", buffer[i + j]);
            } else {
                printf("   ");
            }
        }

        printf("  ");

        for (size_t j = 0; j < 16; j++) {
            if (i + j < sz) {
                if (isprint(buffer[i + j])) {
                    printf("%c", (char)buffer[i + j]);
                } else {
                    printf(".");
                }
            } else {
                printf(" ");
            }
        }

        printf("\n");
    }
}

static const char hextable[16]{'0', '1', '2', '3', '4', '5', '6', '7',
                               '8', '9', 'A', 'B', 'C', 'D', 'E', 'F'};

std::string hexenc(const uint8_t *buffer, size_t sz) {
    std::string hex;

    for (size_t i = 0; i < sz; i++) {
        hex += (hextable[buffer[i] >> 4]);
        hex += (hextable[buffer[i] & 0b1111]);
    }

    return hex;
}

LockGuard::LockGuard(Mutex &mut_) : mut(mut_) { pthread_mutex_lock(&mut.mut); }

LockGuard::~LockGuard() { pthread_mutex_unlock(&mut.mut); }

Mutex::Mutex() { pthread_mutex_init(&mut, 0); }

Mutex::~Mutex() { pthread_mutex_destroy(&mut); }
