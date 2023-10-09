#include <optional>
#include <stdint.h>

bool unicodeToMS932(uint32_t unicode, uint16_t &ms);
bool ms932ToUnicode(uint16_t ms, uint32_t &unicode);
bool isLeadByte(uint8_t byte);
