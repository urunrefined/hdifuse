#include "codepage.h"
#include "ms932.h"
#include "util.h"

#include <algorithm>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <vector>

#include <optional>
#include <string>

static bool validContByte(uint8_t ch) {
    (void)ch;
    // todo:
    return true;
}

static int nextUTF8(const uint8_t *chstr) {
    if (chstr[0] == '\0')
        return 0;

    // Single-byte
    if (!(chstr[0] & 0b1000'0000)) {
        return 1;
    }

    // multi-byte
    if ((chstr[0] & 0b1100'0000) == 0b1100'0000) {
        if (!(chstr[0] & 0b0010'0000)) {
            // Check extent
            if (validContByte(chstr[1])) {
                return 2;
            }
        }
    }

    if ((chstr[0] & 0b1110'0000) == 0b1110'0000) {
        if (!(chstr[0] & 0b0001'0000)) {
            if (validContByte(chstr[1]) && validContByte(chstr[2])) {
                return 3;
            }
        }
    }

    if ((chstr[0] & 0b1111'0000) == 0b1111'0000) {
        if (!(chstr[0] & 0b0000'1000)) {
            if (validContByte(chstr[1]) && validContByte(chstr[2]) &&
                validContByte(chstr[3])) {
                return 4;
            }
        }
    }

    return -1;
}

static std::optional<std::vector<uint32_t>>
getUnicodeFromUTF8(const uint8_t *utf8) {
    std::vector<uint32_t> codePoints;

    while (true) {
        int ret = nextUTF8((const unsigned char *)utf8);

        if (ret == 0)
            break;

        if (ret == -1) {
            printf("Error parsing UTF8 string\n");
            return std::nullopt;
        }

        uint32_t codePoint = 0;

        if (ret == 1) {
            codePoint = utf8[0];
        } else if (ret == 2) {
            codePoint |= (utf8[0] & 0b0001'1111) << 6;
            codePoint |= (utf8[1] & 0b0011'1111);
        } else if (ret == 3) {
            codePoint |= (utf8[0] & 0b0000'1111) << 12;
            codePoint |= (utf8[1] & 0b0011'1111) << 6;
            codePoint |= (utf8[2] & 0b0011'1111);
        } else if (ret == 4) {
            codePoint |= (utf8[0] & 0b0000'0111) << 18;
            codePoint |= (utf8[1] & 0b0011'1111) << 12;
            codePoint |= (utf8[2] & 0b0011'1111) << 6;
            codePoint |= (utf8[3] & 0b0011'1111);
        }

        codePoints.push_back(codePoint);

        utf8 += ret;
    }

    return codePoints;
}

static std::optional<std::string>
getUTF8FromUnicode(const std::vector<uint32_t> &codePoints) {
    std::string str;

    // clang-format off

    for (auto e : codePoints) {
        if (e < 0x80) {
            str += (char)e;
        } else if (e < 0x800) {
            unsigned char utf8[2];

            utf8[0] = 0b1100'0000 | ((e & 0b111'1100'0000) >> 6);
            utf8[1] = 0b1000'0000 | ((e & 0b11'1111));

            str.append((const char *)utf8, 2);
        } else if (e < 0x10000) {
            char utf8[3];

            utf8[0] = 0b1110'0000 | ((e & 0b1111'0000'0000'0000) >> 12);
            utf8[1] = 0b1000'0000 | ((e & 0b0000'1111'1100'0000) >> 6);
            utf8[2] = 0b1000'0000 | ((e & 0b0000'0000'0011'1111));

            str.append((const char *)utf8, 3);

        } else if (e <= 0x10FFFF) {
            char utf8[4];

            utf8[0] =
                0b1111'0000 | ((e & 0b0001'1100'0000'0000'0000'0000) >> 18);
            utf8[1] =
                0b1000'0000 | ((e & 0b0000'0011'1111'0000'0000'0000) >> 12);
            utf8[2] =
                0b1000'0000 | ((e & 0b0000'0000'0000'1111'1100'0000) >> 6);

            utf8[3] = 0b1000'0000 | ((e & 0b0000'0000'0000'0000'0011'1111));

            str.append((const char *)utf8, 4);
        } else {
            printf("Invalid unicode code point %u\n", e);
            return std::nullopt;
        }
    }

    // clang-format on

    return str;
}

static std::optional<std::vector<uint16_t>>
getMS932UpperCaseString(const uint8_t *utf8Name) {

    std::optional<std::vector<uint32_t>> unicode = getUnicodeFromUTF8(utf8Name);

    if (!unicode) {
        return std::nullopt;
    }

    for (auto &e : *unicode) {
        // Transform to upper case
        if (e >= 0x0061 && e <= 0x007A) {
            e -= 0x20;
        }
    }

    std::vector<uint16_t> msCodepoints;

    for (auto e : *unicode) {
        uint16_t ms932 = 0;

        if (!unicodeToMS932(e, ms932)) {
            printf("Cannot map code point to ms932\n");
            return std::nullopt;
        }

        msCodepoints.push_back(ms932);
    }

    return msCodepoints;
}

static std::optional<std::vector<uint32_t>>
getUnicodeFromDOSName(const uint8_t *dosName, size_t sz) {
    if (sz == 0) {
        return std::nullopt;
    }

    std::vector<uint32_t> unicode;

    for (size_t i = 0; i < sz; i++) {

        uint16_t ms932;

        if (isLeadByte(dosName[i])) {
            if (i == sz) {
                printf("Lead byte but no more data available\n");
                return std::nullopt;
            }

            ms932 = (((uint16_t)dosName[i]) << 8);
            ms932 += dosName[i + 1];

            i++;
        } else {
            ms932 = dosName[i];
        }

        uint32_t codePoint;

        if (!ms932ToUnicode(ms932, codePoint)) {
            printf("Unable to map 0x%hX to codepoint\n", ms932);
            return std::nullopt;
        }

        unicode.push_back(codePoint);
    }

    return unicode;
}

static std::string removeTrailingSpaces(const std::string &str) {
    char space[] = {' '};
    auto it = std::find_first_of(str.begin(), str.end(), space, space + 1);

    return std::string(str.begin(), it);
}

static std::optional<std::string>
getUTF8Name(const uint8_t (&dosNameDirty)[11]) {

    uint8_t dosName[11];
    memcpy(dosName, dosNameDirty, sizeof(dosName));

    // 0xE5 is normally an indicator that the file is invalid, however E5 is a
    // lead byte for ms932. As such, 0x05 is used in place of 0xE5 instead
    if (dosName[0] == 0x05) {
        dosName[0] = 0xE5;
    }

    auto codePointsBase = getUnicodeFromDOSName(dosName, 8);

    if (!codePointsBase) {
        printf("Cannot get codepoints from base\n");
        return std::nullopt;
    }

    auto utf8Base = getUTF8FromUnicode(*codePointsBase);

    if (dosName[8] == ' ' && dosName[9] == ' ' && dosName[10] == ' ') {
        // No extension
        return removeTrailingSpaces(*utf8Base);
    }

    auto codePointsExt = getUnicodeFromDOSName(dosName + 8, 3);

    if (!codePointsExt) {
        return std::nullopt;
    }

    auto utf8Ext = getUTF8FromUnicode(*codePointsExt);

    if (!utf8Ext) {
        return std::nullopt;
    }

    return removeTrailingSpaces(*utf8Base) + "." +
           removeTrailingSpaces(*utf8Ext);
}

static std::vector<std::string> split(const std::string &str, const char term) {
    std::vector<std::string> result;
    size_t begin = 0;

    while (begin < str.size()) {
        size_t end = str.find(term, begin);

        if (end == std::string::npos) {
            end = str.size();
        }

        if (end - begin) {
            result.push_back(str.substr(begin, end - begin));
        }

        begin = end + 1;
    }

    return result;
}

bool decodeLimited(const std::vector<uint16_t> &ms932, uint8_t *dosName,
                   size_t sz) {
    {
        size_t decodeLen = 0;

        for (size_t i = 0; i < ms932.size(); i++) {
            if (ms932[i] > 0xFF) {
                decodeLen += 2;
            } else {
                decodeLen++;
            }
        }

        if (decodeLen > sz) {
            return false;
        }
    }

    size_t cur = 0;

    for (size_t i = 0; i < ms932.size(); i++) {
        if (ms932[i] > 0xFF) {
            dosName[cur] = ms932[i];
            dosName[cur + 1] = ms932[i] >> 8;
            cur += 2;
        } else {
            dosName[cur] = ms932[i];
            cur += 1;
        }
    }

    return true;
}

static bool getDOSName(const std::string &base, uint8_t (&dosName)[8 + 3]) {
    std::optional<std::vector<uint16_t>> ms932 =
        getMS932UpperCaseString((const unsigned char *)base.c_str());

    if (!ms932) {
        return false;
    }

    return decodeLimited(*ms932, dosName, 8);
}

static bool getDOSName(const std::string &base, const std::string &ext,
                       uint8_t (&dosName)[8 + 3]) {

    if (!getDOSName(base, dosName)) {
        return false;
    }

    std::optional<std::vector<uint16_t>> ms932 =
        getMS932UpperCaseString((const unsigned char *)ext.c_str());

    if (!ms932) {
        return false;
    }

    return decodeLimited(*ms932, dosName + 8, 3);
}

static bool getDOSNameDirty(const uint8_t *codeName,
                            uint8_t (&dosName)[8 + 3]) {
    memset(dosName, ' ', sizeof(dosName));

    std::vector<std::string> parts = split((const char *)codeName, '.');

    if (parts.size() == 1) {
        return getDOSName(parts[0], dosName);
    } else {
        return getDOSName(parts[0], parts[1], dosName);
    }

    return false;
}

bool getDOSName(const uint8_t *codeName, uint8_t (&dosName)[8 + 3]) {
    bool ret = getDOSNameDirty(codeName, dosName);

    if (ret) {
        if (dosName[0] == 0xE5) {
            dosName[0] = 0x05;
        }
    }

    return ret;
}

std::string getCanonicalString(uint8_t (&filename)[8 + 3]) {
    std::optional<std::string> utf8Name = getUTF8Name(filename);

    if (!utf8Name) {
        return hexenc((uint8_t *)filename, sizeof(filename));
    }

    return *utf8Name;
}
