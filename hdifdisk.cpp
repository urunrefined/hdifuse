#include <fcntl.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <string>
#include <vector>

#include "fat12.h"
#include "file.h"
#include "util.h"

static FatEntry getFatEntry(Region &regionFat, uint16_t i) {
    uint32_t idx = ((uint32_t)i * 12) / 8;
    bool odd = (i % 2);

    return {regionFat.ptr + idx, odd};
};

static bool checkChain(Region &fatRegion, FileEntry *file,
                       uint16_t clusterToBeFound) {
    uint16_t clusterNumber = file->firstDataClusterLow;

    if (clusterNumber == clusterToBeFound)
        return true;

    while (clusterNumber != 0xFFF) {
        clusterNumber = getFatEntry(fatRegion, clusterNumber).getValue();
        if (clusterNumber == clusterToBeFound)
            return true;
    }

    return false;
}

static bool entryPresentDirectoryRecursive(Region &fatRegion,
                                           Region &dataRegion, FileEntry *file,
                                           size_t clusterSize,
                                           uint16_t clusterToBeFound) {
    uint16_t clusterNumber = file->firstDataClusterLow;

    while (clusterNumber != 0xFFF) {
        // The directory was already checked on the call before

        uint8_t *curBuffer =
            dataRegion.ptr + ((clusterNumber - 2) * clusterSize);
        size_t entries = clusterSize / 32;

        for (uint16_t i = 0; i < entries; i++) {
            FileEntry *entry = (FileEntry *)(curBuffer + i * 32);

            if (entry->isValid()) {
                if (checkChain(fatRegion, entry, clusterToBeFound))
                    return true;
            }

            if (entry->isValid() && entry->isDirectory() &&
                !entry->isDotOrDotDot()) {

                if (entryPresentDirectoryRecursive(fatRegion, dataRegion, entry,
                                                   clusterSize,
                                                   clusterToBeFound)) {
                    return true;
                }
            }
        }

        clusterNumber = getFatEntry(fatRegion, clusterNumber).getValue();
    }

    return false;
}

static bool entryPresentRootDirectoryRecursive(
    Region &fatRegion, Region &dataRegion, uint8_t *buffer, uint16_t entries,
    size_t clusterSize, uint16_t clusterToBeFound) {
    for (uint16_t i = 0; i < entries; i++) {
        FileEntry *entry = (FileEntry *)(buffer + i * 32);

        if (entry->isValid()) {
            if (checkChain(fatRegion, entry, clusterToBeFound))
                return true;
        }

        if (entry->isValid() && entry->isDirectory()) {
            if (entryPresentDirectoryRecursive(fatRegion, dataRegion, entry,
                                               clusterSize, clusterToBeFound))
                return true;
        }
    }

    return false;
}

static void printusage(const char *progname) {
    printf("Use \"%s <hdifile>\" to do a basic, "
           "non-complete evaluation of the first FAT12 Volume found in the "
           "image\n",
           progname);

    printf("Use -l [list of inodes] to print fat12 inode information. If no "
           "inodes are specified, print all\n");
    printf("Use -m <list of inodes> to set which inodes should be modified. "
           "Use in combination with -s\n");
    printf("Use -s <value> in decimal to set which value the modified inodes "
           "should be set to\n");
}

// TODO: Change to sw1tch
struct Sw1tch {
    std::string name;
    std::vector<std::string> params;

    bool operator==(const std::string &name_) { return name == name_; }
};

struct ArgArray {
    std::vector<Sw1tch> switches;
    std::string filename;

    bool has(const std::string &name) {
        return std::find(switches.begin(), switches.end(), name) !=
               switches.end();
    }

    Sw1tch get(const std::string &name) {
        return *(std::find(switches.begin(), switches.end(), name));
    }
};

static ArgArray getArgs(int argc, char *argv[]) {

    ArgArray args;

    if (argc < 1) {
        printf("Shell error\n");
        throw -1;
    }

    if (argc < 2) {
        printf("No filename\n");
        printusage(argv[0]);
        throw -2;
    }

    const char *progname = argv[0];
    argv++;
    argc--;

    args.filename = argv[argc - 1];
    argc--;

    std::string prev;

    while (argc) {
        std::string argStr(argv[0]);

        if (!argStr.empty()) {
            if (argStr[0] == '-') {
                const auto &it = std::find(args.switches.begin(),
                                           args.switches.end(), argStr);
                if (it == args.switches.end()) {
                    args.switches.push_back({argStr, {}});
                }
                prev = argStr;
            } else {
                if (prev.empty()) {
                    printf("No option set\n");
                    printusage(progname);
                    throw -3;
                }

                const auto &it =
                    std::find(args.switches.begin(), args.switches.end(), prev);
                it->params.push_back(argStr);
            }
        }

        argc--;
        argv++;
    }

    return args;
}

static void writeFile(Fat12Volume &fat12Volume, const std::string &filename,
                      std::vector<uint8_t> &filedata) {
    printf("Sync fat\n");
    syncFAT(fat12Volume.regionBPB.bootBlock, fat12Volume.volume);

    std::string shadowFilename = filename + ".shadow";
    printf("Write shadow file %s\n", shadowFilename.c_str());

    bool ret = false;

    {
        FileDescriptorWO fdwo(shadowFilename.c_str());
        ret = pumpBuffer(filedata, fdwo.fd);
    }

    if (ret) {
        // todo: eval ret
        rename(shadowFilename.c_str(), filename.c_str());
        printf("Written data to image\n");
    } else {
        printf("Could not write shadow file\n");
        throw -2;
    }
}

int main(int argc, char *argv[]) {

    try {
        ArgArray args(getArgs(argc, argv));

        FileDescriptorRO fd(args.filename.c_str());

        auto filedata = getBuffer(fd.fd);

        printf("Process buffer %zu\n", filedata.size());

        {
            Fat12Volume fat12Volume(getFatVolume(filedata));

            if (args.has("-m")) {
                if (!args.has("-s")) {
                    printf("Option -s needs to be set if -m is present\n");
                    throw -4;
                }

                const auto mArg = args.get("-m");
                const auto sArg = args.get("-s");

                if (mArg.params.empty()) {
                    printf("Option -m needs at least one inode\n");
                    throw -5;
                }

                if (sArg.params.size() != 1) {
                    printf("Option -s needs exactly 1 parameter\n");
                    throw -6;
                }

                uint16_t clusterValue = atoi(sArg.params[0].c_str());

                if (clusterValue >= fat12Volume.maxCluster) {
                    printf("Cluster value to be set is out of range\n");
                    throw -7;
                }

                for (const auto &str : mArg.params) {
                    uint16_t clusterToBeSet = atoi(str.c_str());

                    if (clusterToBeSet >= fat12Volume.maxCluster) {
                        printf("Cluster index %s, (%hu) to be set is out of "
                               "range\n",
                               str.c_str(), clusterToBeSet);
                        throw -8;
                    }

                    getFatEntry(fat12Volume.fatRegion, clusterToBeSet)
                        .setValue(clusterValue);
                }

                writeFile(fat12Volume, args.filename, filedata);
            }

            printRootDirectoryRecursive(
                fat12Volume.fatRegion, fat12Volume.dataRegion,
                fat12Volume.rootRegion.ptr,
                fat12Volume.regionBPB.bootBlock.rootEntries,
                fat12Volume.clusterSize);

            {
                uint16_t expectedFat0Entry =
                    0xF00 + fat12Volume.regionBPB.bootBlock.mediaType;
                if (getFatEntry(fat12Volume.fatRegion, 0).getValue() !=
                    expectedFat0Entry) {

                    printf("First entry in fat is not 0x%hX, 0x%hX instead\n",
                           expectedFat0Entry,
                           getFatEntry(fat12Volume.fatRegion, 0).getValue());
                    printf("First 16 bytes (Fat 0)\n");
                    hexdump(fat12Volume.fatRegion.ptr, 16);
                }
            }

            if (getFatEntry(fat12Volume.fatRegion, 1).getValue() != 0xFFF) {
                printf("Second entry in fat is not 0xFFF, 0x%hX instead\n",
                       getFatEntry(fat12Volume.fatRegion, 0).getValue());
                printf("First 16 bytes (Fat 0)\n");
                hexdump(fat12Volume.fatRegion.ptr, 16);
            }

            // Check for orphans
            {
                std::vector<uint16_t> orphans;

                for (size_t i = 2; i < fat12Volume.maxCluster; i++) {
                    FatEntry entry = getFatEntry(fat12Volume.fatRegion, i);
                    uint16_t cluster = entry.getValue();

                    // printf("Fat Entry %zu, value %hu\n", i, cluster);

                    if (cluster != 0) {
                        if (!entryPresentRootDirectoryRecursive(
                                fat12Volume.fatRegion, fat12Volume.dataRegion,
                                fat12Volume.rootRegion.ptr,
                                fat12Volume.regionBPB.bootBlock.rootEntries,
                                fat12Volume.clusterSize, i)) {
                            orphans.push_back(i);
                        }
                    }
                }

                if (!orphans.empty()) {
                    printf("The following clusters may be orphans\n");

                    for (uint16_t orphan : orphans) {
                        printf("%hu ", orphan);
                    }

                    printf("\n");
                }
            }

            if (args.has("-l")) {
                Sw1tch arg = args.get("-l");

                if (arg.params.empty()) {
                    for (size_t i = 0; i < fat12Volume.maxCluster; i++) {
                        printf(
                            "Fat Entry %zu, value %hu\n", i,
                            getFatEntry(fat12Volume.fatRegion, i).getValue());
                    }
                } else {
                    for (const auto &param : arg.params) {
                        uint16_t cluster(atoi(param.c_str()));

                        if (cluster < fat12Volume.maxCluster) {
                            printf("Fat Entry %hu, value %hu\n", cluster,
                                   getFatEntry(fat12Volume.fatRegion, cluster)
                                       .getValue());
                        } else {
                            printf("Fat Entry %hu is out of range\n", cluster);
                        }
                    }
                }
            }

            // TODO: Catch multiple usages of clusters
            // TODO: Catch cluster loops

            uint16_t freeCount = 0;

            for (size_t i = 2; i < fat12Volume.maxCluster; i++) {
                FatEntry entry = getFatEntry(fat12Volume.fatRegion, i);
                if (entry.getValue() == 0) {
                    freeCount++;
                }
            }

            printf("%hu clusters free, equal to %zu bytes\n", freeCount,
                   freeCount * fat12Volume.clusterSize);
        }

    } catch (int &ex) {
        return ex;
    } catch (...) {
        printf("Exception\n");
        return -2;
    }

    return 0;
}
