#include <fcntl.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <string>
#include <vector>

class FileDescriptorRO {
  public:
    int fd;

    FileDescriptorRO(const char *filename) {
        fd = open(filename, O_RDONLY);

        if (fd == -1) {
            printf("Cannot open file for reading\n");
            throw -1;
        }
    }

    ~FileDescriptorRO() { close(fd); }
};

class FileDescriptorWO {
  public:
    int fd;

    FileDescriptorWO(const char *filename) {
        fd = open(filename, O_WRONLY | O_TRUNC | O_CREAT, S_IRUSR | S_IWUSR);

        if (fd == -1) {
            printf("Cannot open file for writing\n");
            throw -1;
        }
    }

    ~FileDescriptorWO() { close(fd); }
};

static std::vector<uint8_t> getBuffer(int fd) {
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

static void hexdump(uint8_t *buffer, size_t sz) {
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

static void checkJump(uint8_t (&jumpBoot)[3]) {
    if (jumpBoot[0] == 0xEB && jumpBoot[2] == 0x90) {

    } else if (jumpBoot[0] == 0xE9) {

    } else {
        printf("Jump header incorrect\n");
        throw -1;
    }

    printf("JMP Bootstrap INSTR %02hhx%02hhx%02hhx\n", jumpBoot[0], jumpBoot[1],
           jumpBoot[2]);
}

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
        throw -1;
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

struct UINT16LE {
    uint16_t le;

    operator uint16_t() const { return le16toh(le); }

    void operator=(uint16_t le_) { le = htole16(le_); }
};

struct UINT32LE {
    uint32_t le;

    operator uint32_t() const { return le32toh(le); }

    void operator=(uint32_t le_) { le = htole32(le_); }
};

enum FileAttributes {
    ATTR_READ_ONLY = 0x01,
    ATTR_HIDDEN = 0x02,
    ATTR_SYSTEM = 0x04,
    ATTR_VOLUME_ID = 0x08,
    ATTR_DIRECTORY = 0x10,
    ATTR_ARCHIVE = 0x20
};

struct FileEntry {
  public:
    uint8_t filename[8 + 3];
    uint8_t attr;
    uint8_t reserved;
    uint8_t creationTimeTenth;
    uint8_t creationTime[2];
    uint8_t creationDate[2];
    uint8_t lastAccessDate[2];
    UINT16LE firstDataClusterHigh;
    uint8_t writeTime[2];
    uint8_t writeDate[2];
    UINT16LE firstDataClusterLow;
    UINT32LE size;

    bool is(const char (&filename_)[8 + 3]) {
        return memcmp(filename, filename_, sizeof(filename)) == 0;
    }

    bool isValid() {
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

    bool isDirectory() { return attr & ATTR_DIRECTORY; }

    bool isDotOrDotDot() { return filename[0] == '.'; }
};

static void leftPad(uint32_t padCount) { printf("%*s", padCount, ""); }

static void printFileEntry(FileEntry &entry, uint32_t padding) {
    if (entry.filename[0] == 0xE5 || entry.filename[0] == 0) {
        // In the spec it says that if filename[0] is 0, then
        return;
    } else {
        // TODO: 0x05 in filename [0] indicates a special variant and should
        // be replaced by 0xe5
        leftPad(padding);
        printf("%.8s.%.3s ", entry.filename, entry.filename + 8);
    }

    //

    printf("[%c", (entry.attr & ATTR_READ_ONLY) ? 'R' : ' ');
    printf("%c", (entry.attr & ATTR_HIDDEN) ? 'H' : ' ');
    printf("%c", (entry.attr & ATTR_SYSTEM) ? 'S' : ' ');
    printf("%c", (entry.attr & ATTR_VOLUME_ID) ? 'V' : ' ');
    printf("%c", (entry.attr & ATTR_DIRECTORY) ? 'D' : ' ');
    printf("%c]", (entry.attr & ATTR_ARCHIVE) ? 'A' : ' ');

    printf(" 0x%X, ", (uint32_t)entry.size);
    printf(" %hu, ", (uint16_t)entry.firstDataClusterLow);
    printf("\n");

    bool invalid = false;

    if (entry.firstDataClusterHigh) {
        printf("HIGH %hu -- Should be Zero\n",
               (uint16_t)entry.firstDataClusterHigh);

        invalid = true;
    }

    if (entry.firstDataClusterLow == 1 && !entry.isDotOrDotDot()) {
        printf("Entry is invalid -- data cluster is 1\n");

        invalid = true;
    }

    if (entry.firstDataClusterLow == 0 && entry.size != 0) {
        printf("Entry is invalid -- Cluster is 0, but size is not\n");
        invalid = true;
    }

    if (invalid) {
        hexdump((uint8_t *)&entry, sizeof(entry));
    }
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

    void check() {
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
};

struct RegionBPB {
    BPB &bootBlock;
    Region region;
};

static RegionBPB scanForBPBRegion(std::vector<uint8_t> &buffer) {
    if (buffer.size() + 512 < buffer.size()) {
        printf("File is too big. Possible overflow [%zu]", buffer.size());
        throw -1;
    }

    for (size_t i = 0; (i + 1) * 512 < buffer.size(); i++) {

        try {

            BPB *bpb = (BPB *)(buffer.data() + (512 * i));
            bpb->check();

            size_t reservedRegionSize =
                (bpb->reservedSectors * bpb->bytesPerSector);

            size_t offset = (i * 512);

            return {*bpb,
                    {buffer.data() + (i * 512), offset, reservedRegionSize}};

        } catch (...) {
        }
    }

    throw -1;
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
            printf("FAT %d and %d do not match\n", i, i + 1);
            throw -1;
        }
    }

    printf("Fat table OK\n");
}

struct FatEntry {
    uint8_t *ptr;
    bool odd;

    uint16_t getValue() const {
        if (odd) {
            // Skip the first 4 bits
            return ((*ptr) >> 4) + (ptr[1] << 4);
        } else {
            return (*ptr) + ((ptr[1] & 0x0f) << 8);
        }
    }

    FatEntry next() const {
        if (odd) {
            return {ptr + 2, true};
        }

        return {ptr + 1, false};
    }
};

FatEntry getFatEntry(Region &regionFat, uint16_t i) {
    uint32_t idx = ((uint32_t)i * 12) / 8;
    bool odd = (i % 2);

    return {regionFat.ptr + idx, odd};
};

// static void printDirectory(uint8_t *buffer, uint16_t entries, uint32_t depth)
// {
//
//     for (uint16_t i = 0; i < entries; i++) {
//         // TODO Replace
//         FileEntry *entry = (FileEntry *)(buffer + i * 32);
//         printFileEntry(*entry, depth * 4);
//     }
// }

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

static void printRootDirectoryRecursive(Region &fatRegion, Region &dataRegion,
                                        uint8_t *buffer, uint16_t entries,
                                        size_t clusterSize) {
    for (uint16_t i = 0; i < entries; i++) {
        // TODO Replace
        FileEntry *entry = (FileEntry *)(buffer + i * 32);

        printFileEntry(*entry, 0);

        if (entry->isValid() && entry->isDirectory()) {
            printDirectoryRecursive(fatRegion, dataRegion, entry, clusterSize,
                                    1);
        }
    }
}

FileEntry *findEntry(uint8_t *data, size_t entries,
                     const char (&name)[8 + 3 + 1]) {
    for (uint16_t i = 0; i < entries; i++) {
        FileEntry &entry = ((FileEntry *)data)[i];
        if (entry.is((const char(&)[11])name)) {
            return &entry;
        }
    }

    return 0;
}

void dumpRegularFile(Region &fatRegion, Region &dataRegion, FileEntry *file,
                     size_t clusterSize) {
    uint16_t clusterNumber = file->firstDataClusterLow;
    // hexdump(dataRegion.ptr + ((clusterNumber - 2) * clusterSize),
    // clusterSize);

    uint32_t remFileSize = (uint32_t)file->size;

    while (clusterNumber != 0xFFF) {
        printf("Cluster %hu [%d]\n", clusterNumber, clusterNumber - 2);
        if (!remFileSize) {
            printf("File has more clusters allocated, but has no more data\n");
            printf("Referenced cluster %hu\n",
                   getFatEntry(fatRegion, clusterNumber).getValue());
            // hexdump(dataRegion.ptr + ((clusterNumber - 2) * clusterSize),
            // clusterSize);
            printf("----\n");
        }

        if (remFileSize / clusterSize == 0) {
            hexdump(dataRegion.ptr + ((clusterNumber - 2) * clusterSize),
                    remFileSize);
            remFileSize -= remFileSize;
        } else {
            hexdump(dataRegion.ptr + ((clusterNumber - 2) * clusterSize),
                    clusterSize);
            remFileSize -= clusterSize;
        }

        clusterNumber = getFatEntry(fatRegion, clusterNumber).getValue();
    }
}

// static void dumpFile(Region &fatRegion, Region &dataRegion, FileEntry *file,
//                      size_t clusterSize) {
//     if (!file)
//         return;
//
//     printf("Dump %11s, filesize: [%u]\n", file->filename,
//            (uint32_t)(file->size));
//
//     if (file->attr & ATTR_DIRECTORY) {
//         dumpDirectoryFile(fatRegion, dataRegion, file, clusterSize);
//     } else {
//         dumpRegularFile(fatRegion, dataRegion, file, clusterSize);
//     }
// }

// clang-format off

//0x00 4 Byte INT LE | Reserved           | Must be set to zero
//0x04 4 Byte INT LE | Type identifier    | Only used for FDDs. Otherwise most likely set to 0
//0x08 4 Byte INT LE | Header Size        | Size of the header. This header will be cut
//0x0C 4 Byte INT LE | Data Size          | Size of the entire image (after the header)
//0x10 4 Byte INT LE | Bytes per Sector   | What is says
//0x14 4 Byte INT LE | Sectors            | Sector Count
//0x18 4 Byte INT LE | Heads              | Head Count
//0x1C 4 Byte INT LE | Cylinders          | Cylinder Count

//clang-format on

struct HDIHeader {
    UINT32LE reserved;
    UINT32LE type;
    UINT32LE hdrSize;
    UINT32LE dataSize;
    UINT32LE bytesPerSector;
    UINT32LE sectors;
    UINT32LE heads;
    UINT32LE cylinders;
    
    void check() {
        if(reserved != 0){
            printf("First bytes in HDI header should be zero");
            throw -1;
        }
    }
};

static void checkBootHeader(std::vector<uint8_t> &data) {
    
    size_t nextHeader = 0;
    
    if(data.size() < 512){
        printf("File too small -- Cannot parse any header\n");
        throw -1;
    }
    
    try{
        //TODO: Check data size
        
        HDIHeader& hdr = (HDIHeader&) *(data.data());
        hdr.check();
        
        
        nextHeader = hdr.hdrSize;
    }
    catch(...){
        
    }
    
    printf("NextHeader 0x%zX\n", nextHeader);
    printf("FE: %#hhx\n", *(data.data() + nextHeader + 0x0FE));
    printf("FF: %#hhx\n", *(data.data() + nextHeader + 0x0FF));
    
    if(
            (*(data.data() + nextHeader + 0x0FE) == 0x55) &&
            (*(data.data() + nextHeader + 0x0FF) == 0xAA)
            
     ){
        printf("Header OK");
        return;
    }
    
    printf("No known header\n");
    throw -1;
}

int main(int argc, char *argv[]) {

    if (argc < 2) {
        return -1;
    }

    try {
        FileDescriptorRO fd(argv[1]);

        auto filedata = getBuffer(fd.fd);

        printf("Process buffer %zu\n", filedata.size());

        {
            checkBootHeader(filedata);
            
            // Volume starts here as well
            
            auto regionBPB = scanForBPBRegion(filedata);
            printf("Boot Region %#zX, size %#zX\n", regionBPB.region.offset,
                   regionBPB.region.size);

            const auto &bpb = regionBPB.bootBlock;

            size_t volumeSize = bpb.totalSectors16
                                    ? bpb.totalSectors16 * bpb.bytesPerSector
                                    : bpb.totalSectors32 * bpb.bytesPerSector;

            Volume volume{filedata.data() + regionBPB.region.offset,
                          volumeSize};

            size_t remainingBufferSize = filedata.size() - regionBPB.region.offset;

            printf("Volume starts at %#zX, size %#zX\n", regionBPB.region.offset,
                   volumeSize);
            printf("Remaining buffer size %#zX\n", remainingBufferSize);

            if (volumeSize > remainingBufferSize) {
                printf("Volume size greater than remaining buffer\n");
                throw -1;
            }

            const size_t fatOffset =
                ((uint16_t)bpb.reservedSectors * (uint16_t)bpb.bytesPerSector);
            
            const size_t fatSize =
                (uint16_t)bpb.sectorPerFat * (uint16_t)bpb.bytesPerSector;
            
            const size_t fatRegionSize = bpb.fatCount * fatSize;

            Region fatRegion{volume.buffer + fatOffset, fatOffset,
                             fatRegionSize};
            
            printf("Fat Region %#zX, size %#zX\n", fatRegion.offset,
                   fatRegion.size);
            
            checkFatTable(volume, fatOffset, fatSize, bpb.fatCount);

            const size_t rootDirOffset = fatRegion.offset + fatRegion.size;
            const size_t rootDirSize = bpb.rootEntries * 32;

            Region rootDirRegion{volume.buffer + rootDirOffset, rootDirOffset,
                                 rootDirSize};
            
            printf("Root Region %#zX, size %#zX\n", rootDirRegion.offset,
                   rootDirRegion.size);

            const size_t dataOffset = rootDirRegion.offset + rootDirRegion.size;
            const size_t dataSize = volumeSize - fatRegionSize - rootDirSize -
                                    regionBPB.region.size;

            Region dataRegion{volume.buffer + dataOffset, dataOffset, dataSize};
            printf("Data Region %#zX, size %#zX\n", dataRegion.offset,
                   dataRegion.size);

            size_t clusterSize = bpb.sectorsPerCluster * bpb.bytesPerSector;
            printf("ClusterSize %zu\n", clusterSize);
            
            
            printRootDirectoryRecursive(fatRegion, dataRegion, rootDirRegion.ptr, bpb.rootEntries, clusterSize);
            
            //Max Fat Entry
            
            printf("Possible fatentries %zu\n", fatSize * 8 / 12);
            
            size_t maxCluster = std::min((int)(dataSize / clusterSize), (int)(1<<12) - 2);
            maxCluster = std::min(maxCluster, fatSize * 8 / 12);
            
            printf("Max cluster index %zu\n", maxCluster);
            
            uint16_t freeCount = 0;
                    
            for(size_t i = 0; i < maxCluster; i++){
                FatEntry entry = getFatEntry(fatRegion, i);
                if(entry.getValue() == 0){
                    freeCount++;
                }
            }
            
            printf("%hu clusters free, equal to %zu bytes\n", freeCount, freeCount * clusterSize);
            
        }

    } catch (int &ex) {
        return ex;
    } catch (...) {
        printf("Exception\n");
        return -2;
    }

    return 0;
}
