#ifndef FAT12_H
#define FAT12_H

#include <stdint.h>

#include <string>
#include <vector>

struct UINT16LE {
    uint16_t le;

    operator uint16_t() const;
    void operator=(uint16_t host);
};

struct UINT32LE {
    uint32_t le;

    operator uint32_t() const;
    void operator=(uint32_t host);
};

enum FileAttributes {
    ATTR_READ_ONLY = 0x01,
    ATTR_HIDDEN = 0x02,
    ATTR_SYSTEM = 0x04,
    ATTR_VOLUME_ID = 0x08,
    ATTR_DIRECTORY = 0x10,
    ATTR_ARCHIVE = 0x20
};

struct __attribute__((__packed__)) FileEntry {
  public:
    uint8_t filename[8 + 3];
    uint8_t attr;
    uint8_t reserved;
    uint8_t creationTimeTenth;
    UINT16LE creationTime;
    UINT16LE creationDate;
    UINT16LE lastAccessDate;
    UINT16LE firstDataClusterHigh;
    UINT16LE writeTime;
    UINT16LE writeDate;
    UINT16LE firstDataClusterLow;
    UINT32LE size;

    FileEntry();
    FileEntry(const char (&name)[8 + 3], FileAttributes attributes_);

    bool is(const char (&filename_)[8 + 3]);

    bool isValid();

    bool isDirectory();

    bool isRO();

    bool isDotOrDotDot();
    void getCanonicalNulTerm(char (&canonName)[8 + 1 + 3 + 1]);

    void reset();
};

struct Region {
    uint8_t *ptr;
    size_t offset;
    size_t size;
};

struct Volume {
    uint8_t *buffer;
    size_t size;
};

struct __attribute__((__packed__)) BPB {
    uint8_t jump[3];
    uint8_t name[8];
    UINT16LE bytesPerSector;
    uint8_t sectorsPerCluster;
    UINT16LE reservedSectors;
    uint8_t fatCount;
    UINT16LE rootEntries;
    UINT16LE totalSectors16;
    uint8_t mediaType;
    UINT16LE sectorPerFat;
    UINT16LE sectionsPerTrack;
    UINT16LE headCount;
    UINT32LE hiddenSectors;
    UINT32LE totalSectors32;
    uint8_t driveNumber;
    uint8_t reserved;
    uint8_t bootSignature;
    UINT32LE serialNumber;
    uint8_t volumeLabel[11];
    uint8_t fileSysType[8];
    uint8_t remainingDataTillSignature[448];
    uint8_t signature[2];

    void check();
};

struct RegionBPB {
    BPB &bootBlock;
    Region region;
};

struct FatEntry {
    uint8_t *ptr;
    bool odd;

    uint16_t getValue() const;
    void setValue(uint16_t value) const;

    FatEntry next() const;
};

struct Fat12Volume {
    Volume volume;
    RegionBPB regionBPB;
    Region fatRegion;
    Region rootRegion;
    Region dataRegion;
    size_t clusterSize;
    uint16_t maxCluster;
};

Fat12Volume getFatVolume(std::vector<uint8_t> &filedata);

void syncFAT(BPB &bootBlock, Volume &volume);

void printFileEntry(FileEntry &entry, uint32_t padding);

void printDirectoryRecursive(Region &fatRegion, Region &dataRegion,
                             FileEntry *file, size_t clusterSize,
                             uint32_t depth);

#endif // FAT12_H
