#ifndef CODEPAGE_H
#define CODEPAGE_H

#include <optional>
#include <string>
#include <vector>

#include <stdint.h>

bool getDOSName(const uint8_t *codeName, uint8_t (&dosName)[8 + 3]);
std::string getCanonicalString(const uint8_t (&filename)[8 + 3]);

#endif // CODEPAGE_H
