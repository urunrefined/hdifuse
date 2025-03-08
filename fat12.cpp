#include "fat12.h"
#include "codepage.h"
#include "util.h"

#include <assert.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static void checkJump(uint8_t (&jumpBoot)[3]) {
    if (jumpBoot[0] == 0xEB && jumpBoot[2] == 0x90) {

    } else if (jumpBoot[0] == 0xE9) {

    } else {
        throw -1;
    }

    printf("JMP Bootstrap INSTR %02hhx%02hhx%02hhx\n", jumpBoot[0], jumpBoot[1],
           jumpBoot[2]);
}

// 12 bits bytespersector + 7 bits sectors percluster = 19 bits maxClusterSize

static void checkBytesPerSector(uint16_t bytesPerSector) {
    if (bytesPerSector != 512 && bytesPerSector != 1024 &&
        bytesPerSector != 2048 && bytesPerSector != 4096) {
        printf("Bytes per sector is not valid\n");
        throw -1;
    }
}

static void checkSectorsPerCluster(uint16_t sectorsPerCluster) {
    if (sectorsPerCluster != 1 && sectorsPerCluster != 2 &&
        sectorsPerCluster != 4 && sectorsPerCluster != 8 &&
        sectorsPerCluster != 16 && sectorsPerCluster != 32 &&
        sectorsPerCluster != 64 && sectorsPerCluster != 128) {
        printf("Sectors per cluster is not valid\n");
        throw -1;
    }
}

static void checkMediaType(uint8_t mediatype) {
    if (mediatype != 0xF0 && mediatype != 0xF8 && mediatype != 0xF9 &&
        mediatype != 0xFA && mediatype != 0xFB && mediatype != 0xFC &&
        mediatype != 0xFD && mediatype != 0xFE && mediatype != 0xFF) {
        printf("Media type is not valid\n");
        throw -1;
    }
}

static void checkSectorCount(uint16_t totalSectors16, uint32_t totalSectors32) {
    if ((!totalSectors16 && !totalSectors32) ||
        (totalSectors16 && totalSectors32)) {

        printf("SectorCounts are not valid\n");
        printf("Will ignore totalSectors32 and use totalSectors16\n");
    }
}

static void checkDriveNumber(uint8_t driveNumber) {
    if (driveNumber != 0x00 && driveNumber != 0x80) {
        printf("SectorCounts are not valid\n");
        throw -1;
    }
}

static void checkReserved(uint8_t reserved) {
    if (reserved) {
        printf("Reserved byte is not valid\n");
        throw -1;
    }
}

static void checkSignature(uint8_t (&signature)[2]) {
    if (signature[0] != 0x55 || signature[1] != 0x0A) {
        printf("-- Signature check failed -- This seems to be normal for some "
               "reason...\n");
    }
}

static void checkRootEntries(uint16_t rootEntries, uint16_t bytesPerSector) {
    uint32_t rootSize = rootEntries * 32;

    if (rootSize % bytesPerSector) {
        printf(
            "Rootentry * 32 should be cleanly divisible by bytes per sector\n");
        throw -1;
    }
}

static RegionBPB scanForBPBRegion(std::vector<uint8_t> &buffer) {
    if (buffer.size() + 512 < buffer.size()) {
        printf("File is too big. Possible overflow [%zu]", buffer.size());
        throw -1;
    }

    for (size_t i = 0; (i + 1) * 512 < buffer.size(); i++) {

        try {

            BPB *bpb = (BPB *)(buffer.data() + (512 * i));
            printf("[%zu] ", i);

            bpb->check();

            size_t reservedRegionSize =
                (bpb->reservedSectors * bpb->bytesPerSector);

            size_t offset = (i * 512);

            return {*bpb,
                    {buffer.data() + (i * 512), offset, reservedRegionSize}};

        } catch (...) {
        }
    }

    printf("\n");
    printf("No valid location found\n");

    throw -1;
}

UINT16LE::operator uint16_t() const { return le16toh(le); }

void UINT16LE::operator=(uint16_t host) { le = htole16(host); }

UINT32LE::operator uint32_t() const { return le32toh(le); }

void UINT32LE::operator=(uint32_t host) { le = htole32(host); }

FileEntry::FileEntry() { memset(this, 0, sizeof(*this)); }

FileEntry::FileEntry(const char (&name)[8 + 3], FileAttributes attributes_) {
    memset(this, 0, sizeof(*this));
    // filename is not zero terminated
    memcpy(filename, name, sizeof(filename));
    attr = attributes_;
}

bool FileEntry::is(const char (&filename_)[8 + 3]) {
    return memcmp(filename, filename_, sizeof(filename)) == 0;
}

bool FileEntry::isValid() {
    if (filename[0] == 0xE5) {
        return false;
    }
    if (filename[0] == 0) {
        return false;
    }
    if (firstDataClusterLow == 1) {
        return false;
    }
    if (firstDataClusterLow == 0 && size != 0) {
        return false;
    }
    return true;
}

bool FileEntry::isDirectory() { return attr & ATTR_DIRECTORY; }

bool FileEntry::isDotOrDotDot() { return filename[0] == '.'; }

bool FileEntry::isRO() { return attr & ATTR_READ_ONLY; }

void FileEntry::getCanonicalNulTerm(char (&canonName)[8 + 1 + 3 + 1]) {
    size_t cur = 0;

    for (int i = 0; i < 8; i++) {
        if (filename[i] == 0 || filename[i] == ' ') {
            break;
        }

        canonName[cur] = filename[i];
        cur++;
    }

    // Check for any extension
    for (int i = 8; i < 11; i++) {
        if (filename[i] != 0 && filename[i] != ' ') {
            break;
        }

        if (i == 10) {
            canonName[cur] = '\0';
            return;
        }
    }

    canonName[cur] = '.';
    cur++;

    for (int i = 8; i < 11; i++) {
        if (filename[i] == 0 || filename[i] == ' ') {
            break;
        }

        canonName[cur] = filename[i];
        cur++;
    }

    canonName[cur] = '\0';
}

void FileEntry::reset() { memset(this, 0x00, sizeof(*this)); }

void BPB::check() {
    checkJump(jump);

    printf("Name:\n");
    hexdump(name, sizeof(name));

    printf("Bytes per sector: %hu\n", (uint16_t)bytesPerSector);
    checkBytesPerSector(bytesPerSector);

    printf("Sectors per cluster: %hu\n", sectorsPerCluster);
    checkSectorsPerCluster(sectorsPerCluster);

    printf("Reserved sectors: %hu\n", (uint16_t)reservedSectors);

    printf("FAT count: %hhu\n", fatCount);

    printf("Root entries count: %hu\n", (uint16_t)rootEntries);

    checkRootEntries(rootEntries, bytesPerSector);

    printf("Total sectors 16: %hu\n", (uint16_t)totalSectors16);

    printf("Media type: %hhX\n", mediaType);
    checkMediaType(mediaType);

    printf("FAT sector count: %hu\n", (uint16_t)sectorPerFat);

    printf("Sections per track: %hu\n", (uint16_t)sectionsPerTrack);

    printf("Head count: %hu\n", (uint16_t)headCount);

    printf("Hidden sectors: %u\n", (uint32_t)hiddenSectors);

    printf("Total sectors 32: %u\n", (uint32_t)totalSectors32);

    checkSectorCount(totalSectors16, totalSectors32);

    printf("Drive number: 0x%hhX\n", driveNumber);
    checkDriveNumber(driveNumber);

    printf("Reserved: 0x%hhX\n", reserved);
    checkReserved(reserved);

    printf("Boot signature: 0x%hhX (%s)\n", bootSignature,
           bootSignature == 0x29 ? "Valid" : "Invalid");

    // Print all fields even if boot sig is invalid -- expecting the FAT12
    // string to be present, so the boot signature should be valid,
    // otherwise the program can't even get this far

    printf("Serial number: %u\n", (uint32_t)serialNumber);

    printf("Volume label:\n");
    hexdump(volumeLabel, sizeof(volumeLabel));

    printf("File sys type:\n");
    hexdump(fileSysType, sizeof(fileSysType));

    printf("Custom data:\n");
    hexdump(remainingDataTillSignature, sizeof(remainingDataTillSignature));

    printf("Signature:\n");
    hexdump(signature, sizeof(signature));
    checkSignature(signature);
}

static void checkFatTable(Volume &volume, size_t fatOffset, size_t fatSize,
                          size_t fatCount) {
    if (fatOffset + fatSize * fatCount > volume.size) {
        printf("Not enough data left on volume to parse FATs\n");
        throw -1;
    }

    for (uint8_t i = 0; i < fatCount - 1; i++) {

        size_t curFatOffsetIP0 = fatOffset + i * fatSize;
        size_t curFatOffsetIP1 = fatOffset + (i + 1) * fatSize;

        printf("Compare FAT %hhu with %d\n", i, i + 1);

        if (memcmp(volume.buffer + curFatOffsetIP0,
                   volume.buffer + curFatOffsetIP1, fatSize) != 0) {
            printf("FAT %hhu and %d do not match\n", i, i + 1);
            throw -1;
        }
    }

    printf("Fat table OK\n");
}

uint16_t FatEntry::getValue() const {
    if (odd) {
        // Skip the first 4 bits
        return ((*ptr) >> 4) + (ptr[1] << 4);
    } else {
        return (*ptr) + ((ptr[1] & 0x0f) << 8);
    }
}

void FatEntry::setValue(uint16_t value) const {
    if (odd) {
        ptr[0] = (ptr[0] & 0b1111) | (((value & 0b1111) << 4));
        ptr[1] = (value >> 4) & 0b11111111;

    } else {
        ptr[0] = value & 0b11111111;
        ptr[1] = (ptr[1] & 0b11110000) + ((value >> 8));
    }
}

Fat12Volume getFatVolume(std::vector<uint8_t> &filedata) {
    // Volume starts here as well
    const auto &regionBPB = scanForBPBRegion(filedata);
    const auto &bpb = regionBPB.bootBlock;

    size_t bootOffset = regionBPB.region.offset;

    size_t volumeSize = bpb.totalSectors16
                            ? bpb.totalSectors16 * bpb.bytesPerSector
                            : bpb.totalSectors32 * bpb.bytesPerSector;

    Volume volume{filedata.data() + regionBPB.region.offset, volumeSize};

    size_t remainingBufferSize = filedata.size() - bootOffset;

    printf("Volume starts at 0x%zX, size 0x%zX\n", bootOffset, volumeSize);
    printf("Remaining buffer size %zX\n", remainingBufferSize);

    if (volumeSize > remainingBufferSize) {
        printf("Volume size greater than remaining buffer\n");
        throw -1;
    }

    const size_t fatOffset =
        ((uint16_t)bpb.reservedSectors * (uint16_t)bpb.bytesPerSector);
    const size_t fatSize =
        (uint16_t)bpb.sectorPerFat * (uint16_t)bpb.bytesPerSector;
    const size_t fatRegionSize = bpb.fatCount * fatSize;

    checkFatTable(volume, fatOffset, fatSize, bpb.fatCount);

    Region fatRegion{volume.buffer + fatOffset, fatOffset, fatRegionSize};
    printf("Fat Region 0x%zX, size %zX\n", fatRegion.offset, fatRegion.size);

    const size_t rootDirOffset = fatRegion.offset + fatRegion.size;
    const size_t rootDirSize = bpb.rootEntries * 32;

    Region rootDirRegion{volume.buffer + rootDirOffset, rootDirOffset,
                         rootDirSize};
    printf("Root Region 0x%zX, size %zX\n", rootDirRegion.offset,
           rootDirRegion.size);

    const size_t dataOffset = rootDirRegion.offset + rootDirRegion.size;
    const size_t dataSize =
        volumeSize - fatRegionSize - rootDirSize - regionBPB.region.size;

    Region dataRegion{volume.buffer + dataOffset, dataOffset, dataSize};
    printf("Data Region 0x%zX, size %zX\n", dataRegion.offset, dataRegion.size);

    size_t clusterSize = bpb.sectorsPerCluster * bpb.bytesPerSector;
    printf("Cs %zu\n", clusterSize);

    uint16_t maxCluster =
        std::min((int)(dataSize / clusterSize), (int)(1 << 12) - 2);

    maxCluster = std::min((unsigned long)maxCluster, fatSize * 8 / 12);

    printf("Max cluster index %hu\n", maxCluster);

    // TODO: Check if fat table contains eny clusters > MaxCluster

    return {volume,     regionBPB,   fatRegion, rootDirRegion,
            dataRegion, clusterSize, maxCluster};
}

void syncFAT(BPB &bootBlock, Volume &volume) {

    const size_t fatOffset = ((uint16_t)bootBlock.reservedSectors *
                              (uint16_t)bootBlock.bytesPerSector);
    const size_t fatSize =
        (uint16_t)bootBlock.sectorPerFat * (uint16_t)bootBlock.bytesPerSector;

    for (uint8_t i = 0; i < bootBlock.fatCount - 1; i++) {

        size_t curFatOffsetIP0 = fatOffset + i * fatSize;
        size_t curFatOffsetIP1 = fatOffset + (i + 1) * fatSize;

        printf("Sync fat FAT %hhu with %d\n", i, i + 1);

        memcpy(volume.buffer + curFatOffsetIP1, volume.buffer + curFatOffsetIP0,
               fatSize);
    }

    printf("Synced fat tables\n");
}

static void leftPad(uint32_t padCount) { printf("%*s", padCount, ""); }

void printFileEntry(FileEntry &entry, uint32_t padding) {
    if (entry.filename[0] == 0xE5 || entry.filename[0] == 0) {
        return;
    } else {
        // TODO: 0x05 in filename [0] indicates a special variant and should
        // be replaced by 0xe5 when printed
        leftPad(padding);
        printf("%s", getCanonicalString(entry.filename).c_str());
    }

    printf("[%c", (entry.attr & ATTR_READ_ONLY) ? 'R' : ' ');
    printf("%c", (entry.attr & ATTR_HIDDEN) ? 'H' : ' ');
    printf("%c", (entry.attr & ATTR_SYSTEM) ? 'S' : ' ');
    printf("%c", (entry.attr & ATTR_VOLUME_ID) ? 'V' : ' ');
    printf("%c", (entry.attr & ATTR_DIRECTORY) ? 'D' : ' ');
    printf("%c]", (entry.attr & ATTR_ARCHIVE) ? 'A' : ' ');

    printf(" 0x%X, ", (uint32_t)entry.size);
    printf(" %hu, ", (uint16_t)entry.firstDataClusterLow);
    printf("\n");

    if (entry.firstDataClusterHigh) {
        printf("HIGH %hu -- Should be Zero\n",
               (uint16_t)entry.firstDataClusterHigh);
    }

    if (entry.firstDataClusterLow == 1 && !entry.isDotOrDotDot()) {
        printf("Entry is invalid -- data cluster low is less than 2\n");
    }

    if (entry.firstDataClusterLow == 0 && !entry.isDotOrDotDot()) {
        // If the cluster is 0, the length must be 0 and vice versa
        if (entry.size) {
            printf("Entry is invalid -- cluster is 0, but size is not\n");
        }
    }
};

static FatEntry getFatEntry(Region &regionFat, uint16_t i) {
    uint32_t idx = ((uint32_t)i * 12) / 8;
    bool odd = (i % 2);

    return {regionFat.ptr + idx, odd};
};

static void printDirectoryRecursive(Region &fatRegion, Region &dataRegion,
                                    FileEntry *file, size_t clusterSize,
                                    uint32_t depth) {
    uint16_t clusterNumber = file->firstDataClusterLow;

    while (clusterNumber != 0xFFF) {

        uint8_t *curBuffer =
            dataRegion.ptr + ((clusterNumber - 2) * clusterSize);
        size_t entries = clusterSize / 32;

        for (uint16_t i = 0; i < entries; i++) {
            FileEntry *entry = (FileEntry *)(curBuffer + i * 32);

            if (entry->isValid()) {
                printFileEntry(*entry, depth * 4);
            }

            if (entry->isValid() && entry->isDirectory() &&
                !entry->isDotOrDotDot()) {

                printDirectoryRecursive(fatRegion, dataRegion, entry,
                                        clusterSize, depth + 1);
            }
        }

        clusterNumber = getFatEntry(fatRegion, clusterNumber).getValue();
    }
}

void printRootDirectoryRecursive(Region &fatRegion, Region &dataRegion,
                                 uint8_t *rootRegionBuffer, uint16_t entries,
                                 size_t clusterSize) {
    for (uint16_t i = 0; i < entries; i++) {
        // TODO Replace
        FileEntry *entry = (FileEntry *)(rootRegionBuffer + i * 32);

        printFileEntry(*entry, 0);

        if (entry->isValid() && entry->isDirectory()) {
            printDirectoryRecursive(fatRegion, dataRegion, entry, clusterSize,
                                    1);
        }
    }
}
