/*
  HDI/FAT12 FUSE mount
  Copyright (C) 2023  Tom Unbehau <git@unrefined.xyz>

  This program can be distributed under the terms of the MIT License.
  See the file LICENSE.
*/

#define FUSE_USE_VERSION 34

#include <fuse_lowlevel.h>

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "codepage.h"
#include "fat12.h"
#include "file.h"
#include "util.h"

#include <algorithm>
#include <memory>
#include <string>

static FatEntry getFatEntry(Region &regionFat, uint16_t i) {
    uint32_t idx = ((uint32_t)i * 12) / 8;
    bool odd = (i % 2);

    return {regionFat.ptr + idx, odd};
};

class Fat12Inode {
  public:
    FileEntry *file;
    std::vector<Fat12Inode> children;
    uint32_t inode;
    uint64_t nlookup;
    bool zombie = false;

    bool operator==(const Fat12Inode &ref) { return ref.inode == inode; }

    Fat12Inode(Fat12Volume &fat12Volume, FileEntry *file_,
               uint32_t &inodeCounter)
        : file(file_), inode(inodeCounter), nlookup(0) {
        inodeCounter++;

        if (file->isDirectory() && !file->isDotOrDotDot()) {

            uint16_t clusterNumber = file->firstDataClusterLow;

            while (clusterNumber != 0xFFF) {

                uint8_t *curBuffer =
                    fat12Volume.dataRegion.ptr +
                    ((clusterNumber - 2) * fat12Volume.clusterSize);

                size_t entries = fat12Volume.clusterSize / 32;

                for (uint16_t i = 0; i < entries; i++) {
                    FileEntry *entry = (FileEntry *)(curBuffer + i * 32);

                    if (entry->isValid() && !file->isDotOrDotDot()) {
                        children.push_back({fat12Volume, entry, inodeCounter});
                    }
                }

                clusterNumber =
                    getFatEntry(fat12Volume.fatRegion, clusterNumber)
                        .getValue();
            }
        }
    }

    Fat12Inode(Fat12Volume &fat12Volume, FileEntry *file_, uint16_t entries,
               uint8_t *ptr, uint32_t &inodeCounter)
        : file(file_), inode(inodeCounter) {
        inodeCounter++;

        for (uint16_t i = 0; i < entries; i++) {
            FileEntry *entry = (FileEntry *)(ptr + i * 32);

            if (entry->isValid()) {
                children.push_back({fat12Volume, entry, inodeCounter});
            }
        }
    }

    FileEntry *find(uint32_t inode_) {
        if (inode_ == inode) {
            return file;
        }

        for (auto &child : children) {
            FileEntry *entry = child.find(inode_);
            if (entry) {
                return entry;
            }
        }

        return 0;
    }

    Fat12Inode *findParent(uint32_t inode_) {
        if (inode_ == inode) {
            return this;
        }

        for (auto &child : children) {
            Fat12Inode *result = child.findParent(inode_);
            if (result) {
                return this;
            }
        }

        return 0;
    }

    Fat12Inode *findInode(uint32_t inode_) {
        if (inode_ == inode) {
            return this;
        }

        for (auto &child : children) {
            Fat12Inode *result = child.findInode(inode_);
            if (result) {
                return result;
            }
        }

        return 0;
    }

    FileEntry *getFreeFileEntry(Fat12Volume &fat12Volume) {
        assert(file->isDirectory());

        if (inode == FUSE_ROOT_ID) {
            // Root, use root region

            for (FileEntry *entry = (FileEntry *)fat12Volume.rootRegion.ptr;
                 entry < ((FileEntry *)fat12Volume.rootRegion.ptr) +
                             fat12Volume.regionBPB.bootBlock.rootEntries;
                 entry++) {

                if (!entry->isValid()) {
                    return entry;
                }
            }
        } else {

            uint16_t clusterNumber = file->firstDataClusterLow;

            while (clusterNumber != 0xFFF) {

                uint8_t *curBuffer =
                    fat12Volume.dataRegion.ptr +
                    ((clusterNumber - 2) * fat12Volume.clusterSize);

                size_t entries = fat12Volume.clusterSize / 32;

                for (uint16_t i = 0; i < entries; i++) {
                    FileEntry *entry = (FileEntry *)(curBuffer + i * 32);

                    if (!entry->isValid()) {
                        return entry;
                    }
                }

                clusterNumber =
                    getFatEntry(fat12Volume.fatRegion, clusterNumber)
                        .getValue();
            }
        }

        return 0;
    }
};

class Memory {
  public:
    uint8_t *bytes;
    size_t size;
    size_t used;

    Memory(size_t size_) : size(size_), used(0) {
        bytes = (uint8_t *)malloc(size);

        if (!bytes) {
            printf("Cannot allocate memory, size %zu\n", size);
            throw -2;
        }
    }

    void push(uint8_t *data, size_t dataSize) {
        if (used + dataSize > size) {
            printf("Pushed too much data on memory slot, data size %zu, used "
                   "%zu, slotsize %zu\n",
                   dataSize, used, size);
        }

        memcpy(bytes + used, data, dataSize);
        used += dataSize;
    }

    bool isValid(size_t offset, size_t dataSize) {
        return offset + dataSize <= used;
    }

    ~Memory() { free(bytes); }
};

// size_t offset, size_t sz;

struct ClusterPos {
    uint16_t cluster;
    size_t fileClusterOffset;
    uint32_t clusterOffset;
};

static ClusterPos seek(Region &fatRegion, uint16_t cluster, size_t clusterSize,
                       size_t offset) {

    unsigned int skipCluster = offset / clusterSize;
    unsigned int skippedCluster = 0;

    for (unsigned int i = 0; i < skipCluster && cluster != 0xFFF; i++) {
        cluster = getFatEntry(fatRegion, cluster).getValue();
        skippedCluster++;
    }

    return {cluster, skippedCluster * clusterSize,
            (uint32_t)offset % (uint32_t)clusterSize};
}

using MemoryHdl = std::unique_ptr<Memory>;

static size_t readCluster(Region &dataRegion, size_t clusterSize,
                          ClusterPos &pos, size_t readCount,
                          MemoryHdl &memory) {

    uint8_t *ptr =
        dataRegion.ptr + ((pos.cluster - 2) * clusterSize) + pos.clusterOffset;

    size_t readSize = clusterSize - pos.clusterOffset;
    readSize = std::min(readSize, readCount);

    memory.get()->push(ptr, readSize);
    return readSize;
}

static MemoryHdl dumpRegularFile(Region &fatRegion, Region &dataRegion,
                                 FileEntry *file, size_t clusterSize,
                                 size_t size, off_t offset) {

    // hexdump(dataRegion.ptr + ((clusterNumber - 2) * clusterSize),
    // clusterSize);

    printf("Read size %zu, offset %zu\n", size, offset);

    if (file->size == 0 || file->size <= offset) {
        // We need to define a size, as malloc may fail with 0
        return std::unique_ptr<Memory>(std::make_unique<Memory>(1024));
    }

    size_t toRead = std::min(file->size - (size_t)offset, size);

    // printf("Dump %s, inode %hd, size %#X\n",
    //        getCanonicalString(file->filename).c_str(),
    //        (uint16_t)file->firstDataClusterLow, (uint32_t)file->size);

    MemoryHdl memory(std::make_unique<Memory>(size));

    ClusterPos pos =
        seek(fatRegion, file->firstDataClusterLow, clusterSize, offset);

    if (pos.fileClusterOffset + pos.clusterOffset != (size_t)offset) {
        printf("Cannot seek to position %zu\n", offset);
        throw EINVAL;
    }

    size_t hasRead = 0;

    while (pos.cluster != 0xFFF && hasRead != toRead) {
        // printf("Cluster %hu [%d]\n", pos.cluster, pos.cluster - 2);

        int rd =
            readCluster(dataRegion, clusterSize, pos, toRead - hasRead, memory);

        hasRead += rd;

        pos.cluster = getFatEntry(fatRegion, pos.cluster).getValue();

        pos.clusterOffset = 0;
    }

    printf("Filesize %u hasRead, %zu\n", (uint32_t)file->size, hasRead);

    return memory;
}

std::unique_ptr<Memory> readFile(FileEntry *entry, Fat12Volume &volume,
                                 size_t size, off_t off) {
    return dumpRegularFile(volume.fatRegion, volume.dataRegion, entry,
                           volume.clusterSize, size, off);
}

static size_t writeCluster(Region &dataRegion, size_t clusterSize,
                           ClusterPos &pos, size_t writeCount,
                           const char *data) {

    // printf("------\n");

    uint8_t *ptr =
        dataRegion.ptr + ((pos.cluster - 2) * clusterSize) + pos.clusterOffset;

    size_t maxWriteSize = clusterSize - pos.clusterOffset;
    size_t toWrite = std::min(maxWriteSize, writeCount);

    memcpy(ptr, data, toWrite);

    // printf("Write %p to %p, count %zu\n", ptr, data, toWrite);
    //  hexdump((uint8_t *)data, toWrite);
    //  printf("------\n");

    return toWrite;
}

static uint16_t getFreeCluster(Region &fatRegion, uint16_t maxCluster) {
    for (uint16_t i = 0; i < maxCluster; i++) {
        FatEntry entry = getFatEntry(fatRegion, i);

        if (entry.getValue() == 0) {
            return i;
        }
    }

    return 0xfff;
}

static size_t writeFile(Region &fatRegion, Region &dataRegion, FileEntry *file,
                        size_t clusterSize, size_t toWrite, off_t offset,
                        const char *data, uint16_t maxCluster) {

    printf("Write size %zu, offset %zu\n", toWrite, offset);

    ClusterPos pos;

    pos = seek(fatRegion, file->firstDataClusterLow, clusterSize, offset);

    if (pos.fileClusterOffset + pos.clusterOffset != (size_t)offset) {
        printf("Seek to offset %zu failed -- seeked to: %zu\n", offset,
               pos.fileClusterOffset);

        throw ESPIPE;
    }

    size_t written = 0;

    if (!toWrite) {
        return 0;
    }

    if (file->firstDataClusterLow == 0) {
        uint16_t newCluster = getFreeCluster(fatRegion, maxCluster);
        printf("Allocate cluster  result: %hu\n", newCluster);

        if (newCluster == 0xFFF)
            return 0;

        file->firstDataClusterLow = newCluster;

        getFatEntry(fatRegion, newCluster).setValue(0xFFF);
        pos.cluster = newCluster;
    }

    while (written != toWrite) {
        printf("Write cluster: %hu, clusterOffset: %u, fileCOffset: %zu\n",
               pos.cluster, pos.clusterOffset, pos.fileClusterOffset);

        written += writeCluster(dataRegion, clusterSize, pos, toWrite - written,
                                data + written);

        if (written != toWrite) {
            FatEntry curEntry = getFatEntry(fatRegion, pos.cluster);

            if (curEntry.getValue() == 0xFFF) {
                pos.cluster = getFreeCluster(fatRegion, maxCluster);
                printf("Allocate cluster result: %hu\n", pos.cluster);
                if (pos.cluster == 0xFFF) {
                    return written;
                }

                curEntry.setValue(pos.cluster);
                getFatEntry(fatRegion, pos.cluster).setValue(0xFFF);
                pos.clusterOffset = 0;
            } else {
                pos.cluster = curEntry.getValue();
                pos.clusterOffset = 0;
            }
        }
    }

    file->size = std::max((uint32_t)(offset + written), (uint32_t)file->size);

    printf("Resulting file-size %u\n", (uint32_t)file->size);

    return written;
}

static void f_unlink(Region &fatRegion, FileEntry *file) {
    printf("CLUSTER %hu \n", (uint16_t)file->firstDataClusterLow);

    if (file->firstDataClusterLow == 0) {
        file->reset();
        return;
    }

    uint16_t cluster = file->firstDataClusterLow;

    if (cluster == 0xFFF) {
        printf("First allocated cluster in file should not be an end of file "
               "marker\n");
        throw EILSEQ;
    }

    while (1) {
        FatEntry entry = getFatEntry(fatRegion, cluster);

        if (entry.getValue() == 0xFFF) {
            entry.setValue(0);
            break;
        }

        cluster = entry.getValue();
        entry.setValue(0);
    }

    file->reset();
}

class FuseFile {
  public:
    uint64_t handle;
    Fat12Inode inode;

    FuseFile(uint64_t handle_, Fat12Inode inode_)
        : handle(handle_), inode(inode_) {}
};

class FuseContext {
  public:
    Fat12Volume &fat12Volume;
    uint32_t inodeCounter;
    FileEntry entry;
    Fat12Inode rootInode;
    Mutex mutex;
    std::vector<std::unique_ptr<FuseFile>> activeFiles;

    FuseContext(Fat12Volume &fat12Volume_)
        : fat12Volume(fat12Volume_),
          // rootInode gets inode number 1, which is
          // the root directory in FUSE
          inodeCounter(1), entry("root      ", ATTR_DIRECTORY),
          rootInode(fat12Volume, &entry,
                    fat12Volume.regionBPB.bootBlock.rootEntries,
                    fat12Volume.rootRegion.ptr, inodeCounter) {}

    bool existsFile(uint64_t handle) {
        for (auto &cDir : activeFiles) {
            if (cDir->handle == handle) {
                return true;
            }
        }

        return false;
    }

    uint64_t getFreeFileHandle() {

        for (uint64_t i = 0; i < 128; i++) {
            if (!existsFile(i)) {
                return i;
            }
        }

        throw EMFILE;
    }

    FuseFile *getOpenFile(uint64_t handle) {
        for (auto &cDir : activeFiles) {

            if (cDir->handle == handle) {
                printf("Get open file %ld\n", handle);

                return &(*cDir);
            }
        }

        return 0;
    }

    void releaseFile(uint64_t handle) {
        for (size_t i = 0; i < activeFiles.size(); i++) {
            if (activeFiles[i]->handle == handle) {
                activeFiles.erase(activeFiles.begin() + i);
            }
        }
    }

    bool isInUse(uint32_t inode) {
        for (size_t i = 0; i < activeFiles.size(); i++) {
            if (activeFiles[i]->inode.inode == inode) {
                return true;
            }
        }

        return false;
    }
};

// Return a valid time, otherwise return all zero
static void getDateTime(uint16_t date, uint16_t clock, struct tm &result) {
    memset(&result, 0, sizeof(result));
    result.tm_year = 80;
    result.tm_mday = 1;

    if (!date) {
        return;
    }

    struct tm temp;
    memset(&temp, 0, sizeof(temp));

    {
        uint8_t day = (date)&0b1'1111;
        uint8_t month = (date >> 5) & 0b1111;
        uint8_t year = (date >> (5 + 4));

        if (day == 0 || day > 31) {
            return;
        }

        if (month == 0 || month > 12) {
            return;
        }

        // year doesn't need to be checked, as its 7 bits are always valid

        // DOS epoch starts at 1980 instead of 1970
        temp.tm_year = 80 + year;
        temp.tm_mon = month;
        temp.tm_mday = day;
    }

    {
        uint8_t seconds = (clock & 0b1'1111) * 2;
        uint8_t minutes = ((clock >> 5) & 0b11'1111);
        uint8_t hours = (clock >> (5 + 6));

        if (seconds > 58) {
            return;
        }

        if (minutes > 59) {
            return;
        }

        if (hours > 23) {
            return;
        }

        temp.tm_sec = seconds;
        temp.tm_min = minutes;
        temp.tm_hour = hours;
    }

    result = temp;
}

static int fat12_stat(fuse_ino_t ino, FuseContext *userdata,
                      struct stat *stbuf) {

    FileEntry *entry = userdata->rootInode.find(ino);

    if (!entry)
        return -1;

    stbuf->st_ino = ino;
    stbuf->st_size = entry->size;

    if (entry->isDirectory()) {
        stbuf->st_mode = S_IFDIR | 0555;
    } else {
        stbuf->st_mode = S_IFREG | 0444;
    }

    struct tm result;

    // Not supported by FUSE afaict // I supported by some FS however,
    // listed as "birth time" getDateTime(entry->creationDate,
    // entry->creationTime,result);

    getDateTime(entry->lastAccessDate, 0, result);
    stbuf->st_atim = {timegm(&result), 0};

    getDateTime(entry->writeDate, entry->writeTime, result);
    stbuf->st_mtim = {timegm(&result), 0};
    // There is no status change info, so just copy the modified one
    stbuf->st_ctim = stbuf->st_mtim;

    stbuf->st_nlink = 1;

    return 0;
}

static void fat12_ll_getattr(fuse_req_t req, fuse_ino_t ino,
                             struct fuse_file_info *fi) {

    try {

        (void)fi;

        FuseContext *userdata = (FuseContext *)fuse_req_userdata(req);
        LockGuard lg(userdata->mutex);

        struct stat stbuf;
        memset(&stbuf, 0, sizeof(stbuf));

        if (fat12_stat(ino, userdata, &stbuf) == -1) {
            fuse_reply_err(req, ENOENT);
        } else {
            fuse_reply_attr(req, &stbuf, 1.0);
        }

    } catch (int err) {
        fuse_reply_err(req, err);
    } catch (...) {
        fuse_reply_err(req, ENOMEM);
    }
}

static void lookup(fuse_req_t req, const char *name,
                   std::vector<Fat12Inode> &children) {

    FuseContext *userdata = (FuseContext *)fuse_req_userdata(req);

    for (size_t i = 0; i < children.size(); i++) {
        FileEntry *entry = children[i].file;

        std::string canonicalFilename = getCanonicalString(entry->filename);

        if (strcasecmp(canonicalFilename.c_str(), name) == 0) {
            printf("Name found %s\n", name);

            struct fuse_entry_param e;
            memset(&e, 0, sizeof(e));
            e.ino = children[i].inode;
            e.attr_timeout = 1.0;
            e.entry_timeout = 1.0;

            fat12_stat(children[i].inode, userdata, &e.attr);

            children[i].nlookup++;
            fuse_reply_entry(req, &e);
            return;
        }
    }

    printf("Not found %s\n", name);
    fuse_reply_err(req, ENOENT);
}

static void fat12_ll_lookup(fuse_req_t req, fuse_ino_t parent,
                            const char *name) {

    try {

        FuseContext *context = (FuseContext *)fuse_req_userdata(req);
        LockGuard lg(context->mutex);

        Fat12Inode &rootInode = context->rootInode;

        printf("Lookup name %s\n", name);

        Fat12Inode *inode = rootInode.findInode(parent);

        if (inode) {
            lookup(req, name, inode->children);
            return;
        }

        fuse_reply_err(req, ENOENT);

    } catch (int err) {
        fuse_reply_err(req, err);
    } catch (...) {
        fuse_reply_err(req, ENOMEM);
    }
}

static size_t getDirectoryInodeBufferSize(fuse_req_t req, Fat12Inode &inode,
                                          size_t maxSize, off_t off) {
    struct stat stbuf;
    memset(&stbuf, 0, sizeof(stbuf));
    size_t curSize = 0;

    for (size_t i = off; i < inode.children.size(); i++) {
        FileEntry *entry = inode.children[i].file;
        std::string filename = getCanonicalString(entry->filename);

        stbuf.st_ino = inode.children[i].inode;

        size_t entrysize =
            fuse_add_direntry(req, 0, 0, filename.c_str(), &stbuf, i + 1);

        if (curSize + entrysize < maxSize) {
            curSize += entrysize;
        } else {
            return curSize;
        }
    }

    return curSize;
}

static void addDirectoryInode(FuseContext *userdata, std::vector<uint8_t> &vec,
                              fuse_req_t req, Fat12Inode &inode, size_t maxSize,
                              off_t off) {

    struct stat stbuf;
    memset(&stbuf, 0, sizeof(stbuf));

    size_t bufsz = 0;

    for (size_t i = off; i < inode.children.size(); i++) {

        FileEntry *entry = inode.children[i].file;
        // printFileEntry(*entry, 0);
        //  hexdump((uint8_t *)entry, sizeof(*entry));

        std::string filename = getCanonicalString(entry->filename);

        stbuf.st_ino = inode.children[i].inode;
        fat12_stat(inode.children[i].inode, userdata, &stbuf);

        size_t addch = fuse_add_direntry(req, (char *)vec.data() + bufsz,
                                         vec.size() - bufsz, filename.c_str(),
                                         &stbuf, i + 1);

        if (bufsz + addch < maxSize) {
            bufsz += addch;
        } else {
            return;
        }
    }
}

static void fat12_ll_opendir(fuse_req_t req, fuse_ino_t ino,
                             struct fuse_file_info *fi) {

    try {

        FuseContext *context = (FuseContext *)fuse_req_userdata(req);
        LockGuard lg(context->mutex);

        Fat12Inode *fat12Inode = context->rootInode.findInode(ino);

        if (fat12Inode) {
            uint64_t handle = context->getFreeFileHandle();

            context->activeFiles.push_back(
                std::make_unique<FuseFile>(handle, *fat12Inode));

            fi->fh = handle;

            fuse_reply_open(req, fi);
        } else {
            fuse_reply_err(req, ENOENT);
        }

    } catch (int err) {
        fuse_reply_err(req, err);
    } catch (...) {
        fuse_reply_err(req, ENOMEM);
    }
}

static void fat12_ll_readdir(fuse_req_t req, fuse_ino_t ino, size_t size,
                             off_t off, struct fuse_file_info *fi) {
    try {

        (void)ino;

        FuseContext *context = (FuseContext *)fuse_req_userdata(req);
        LockGuard lg(context->mutex);

        FuseFile *dir = context->getOpenFile(fi->fh);

        if (dir) {
            size_t bufsz =
                getDirectoryInodeBufferSize(req, dir->inode, size, off);

            std::vector<uint8_t> buf(bufsz);
            addDirectoryInode(context, buf, req, dir->inode, size, off);

            if (!buf.empty()) {
                fuse_reply_buf(req, (char *)buf.data(), buf.size());
            } else {
                fuse_reply_buf(req, NULL, 0);
            }
        } else {
            fuse_reply_buf(req, NULL, ENOENT);
        }

    } catch (int err) {
        fuse_reply_err(req, err);
    } catch (...) {
        fuse_reply_err(req, ENOMEM);
    }
}

static void fat12_ll_releasedir(fuse_req_t req, fuse_ino_t ino,
                                struct fuse_file_info *fi) {
    (void)ino;

    try {

        FuseContext *context = (FuseContext *)fuse_req_userdata(req);
        LockGuard lg(context->mutex);

        context->releaseFile(fi->fh);

        fuse_reply_err(req, 0);

    } catch (int err) {
        fuse_reply_err(req, err);
    } catch (...) {
        fuse_reply_err(req, ENOMEM);
    }
}

static void trunc(FileEntry *entry, Region &fatRegion) {

    if (entry->size == 0 || entry->firstDataClusterLow == 0) {
        return;
    }

    uint16_t cluster = entry->firstDataClusterLow;

    while (cluster != 0xFFF) {
        FatEntry fatEntry = getFatEntry(fatRegion, cluster);
        // printf("TRUNC cluster %hu\n", cluster);
        cluster = fatEntry.getValue();
        fatEntry.setValue(0xFFF);
    }

    entry->firstDataClusterLow = 0;
}

static void fat12_ll_open(fuse_req_t req, fuse_ino_t ino,
                          struct fuse_file_info *fi) {
    try {

        FuseContext *userdata = (FuseContext *)fuse_req_userdata(req);
        LockGuard lg(userdata->mutex);

        Fat12Inode *fileNode = userdata->rootInode.findInode(ino);

        if (!fileNode) {
            fuse_reply_err(req, ENOENT);
            return;
        }

        FileEntry *fileEntry = fileNode->file;

        if (fileEntry->isDirectory()) {
            fuse_reply_err(req, EISDIR);
            return;
        }

        if ((fi->flags & O_WRONLY || fi->flags & O_RDWR) && fileEntry->isRO()) {
            fuse_reply_err(req, EACCES);
            return;
        }

        if (fi->flags & O_TRUNC) {
            // printf("TRUNC requested\n");

            trunc(fileEntry, userdata->fat12Volume.fatRegion);
            fileEntry->size = 0;
        }

        uint64_t fileHandle = userdata->getFreeFileHandle();

        try {
            userdata->activeFiles.push_back(
                std::make_unique<FuseFile>(fileHandle, *fileNode));

            printf("Open inode %zu\n", ino);

            fi->fh = fileHandle;

        } catch (...) {
            printf("Cannot open/read file Inode %zu\n", ino);
            fuse_reply_err(req, EIO);
            return;
        }

        fuse_reply_open(req, fi);

    } catch (int err) {
        fuse_reply_err(req, err);
    } catch (...) {
        fuse_reply_err(req, ENOMEM);
    }
}

static void fat12_ll_read(fuse_req_t req, fuse_ino_t ino, size_t size,
                          off_t off, struct fuse_file_info *fi) {
    (void)ino;

    try {
        if (off < 0) {
            fuse_reply_buf(req, NULL, 0);
            return;
        }

        FuseContext *userdata = (FuseContext *)fuse_req_userdata(req);
        LockGuard lg(userdata->mutex);

        FuseFile *fuseFile = userdata->getOpenFile(fi->fh);

        if (!fuseFile) {
            fuse_reply_buf(req, NULL, 0);
            return;
        }

        std::unique_ptr<Memory> memory =
            readFile(fuseFile->inode.file, userdata->fat12Volume, size, off);

        fuse_reply_buf(req, (char *)(memory->bytes), memory->used);

    } catch (int err) {
        fuse_reply_err(req, err);
    } catch (...) {
        fuse_reply_err(req, ENOMEM);
    }
}

static void fat12_ll_release(fuse_req_t req, fuse_ino_t ino,
                             struct fuse_file_info *fi) {

    (void)ino;

    try {
        FuseContext *userdata = (FuseContext *)fuse_req_userdata(req);
        LockGuard lg(userdata->mutex);

        userdata->releaseFile(fi->fh);

        fuse_reply_err(req, 0);

    } catch (int err) {
        fuse_reply_err(req, err);
    } catch (...) {
        fuse_reply_err(req, ENOMEM);
    }
}

static void fat12_ll_create(fuse_req_t req, fuse_ino_t parent, const char *name,
                            mode_t mode, struct fuse_file_info *fi) {
    try {
        (void)mode;

        FuseContext *fuseContext = (FuseContext *)fuse_req_userdata(req);
        LockGuard lg(fuseContext->mutex);

        uint8_t dosName[11];
        if (!getDOSName((uint8_t *)name, dosName)) {
            printf("Name invalid -- Cannot create node\n");
            fuse_reply_err(req, EINVAL);
            return;
        }

        Fat12Inode *inode = fuseContext->rootInode.findInode(parent);
        if (!inode) {
            printf("Cannot find inode in which to create entry\n");
            fuse_reply_err(req, ENOTDIR);
            return;
        }

        uint64_t handle = fuseContext->getFreeFileHandle();

        FileEntry *entry = inode->getFreeFileEntry(fuseContext->fat12Volume);

        if (!entry) {
            printf("Cannot allocate additional entry\n");
            fuseContext->releaseFile(handle);
            fuse_reply_err(req, ENOMEM);
            return;
        }

        FileEntry backup = *entry;

        entry->reset();
        memcpy(entry->filename, dosName, sizeof(entry->filename));

        if (!entry->isValid()) {
            printf("Entry still invalid\n");
            fuseContext->releaseFile(handle);
            (*entry) = backup;
            fuse_reply_err(req, EFAULT);
            return;
        }

        Fat12Inode inodeBackup = *inode;

        try {
            Fat12Inode newInode(fuseContext->fat12Volume, entry,
                                fuseContext->inodeCounter);
            newInode.nlookup = 1;

            inode->children.push_back(newInode);

            fuseContext->activeFiles.push_back(
                std::make_unique<FuseFile>(handle, newInode));

            struct fuse_entry_param e;
            memset(&e, 0, sizeof(e));
            e.ino = newInode.inode;
            e.attr_timeout = 1.0;
            e.entry_timeout = 1.0;

            fat12_stat(newInode.inode, fuseContext, &e.attr);

            fi->fh = handle;

            fuse_reply_create(req, &e, fi);

        } catch (...) {
            fuseContext->releaseFile(handle);
            (*entry) = backup;
            (*inode) = inodeBackup;

            fuse_reply_err(req, ENOMEM);
        }

    } catch (int err) {
        fuse_reply_err(req, err);
    } catch (...) {
        fuse_reply_err(req, ENOMEM);
    }
}

static void fat12_ll_write(fuse_req_t req, fuse_ino_t ino, const char *buf,
                           size_t size, off_t off, struct fuse_file_info *fi) {

    printf("ino %lu, off %lu\n", ino, off);
    // hexdump((uint8_t *)buf, size);

    try {

        FuseContext *fuseContext = (FuseContext *)fuse_req_userdata(req);
        LockGuard lg(fuseContext->mutex);

        FuseFile *fuseFile = fuseContext->getOpenFile(fi->fh);

        if (!fuseFile) {
            printf("Cannot address file, which should currently be opened\n");
            fuse_reply_err(req, EINVAL);
            return;
        }

        size_t sz =
            writeFile(fuseContext->fat12Volume.fatRegion,
                      fuseContext->fat12Volume.dataRegion, fuseFile->inode.file,
                      fuseContext->fat12Volume.clusterSize, size, off, buf,
                      fuseContext->fat12Volume.maxCluster);

        if (sz == 0) {
            fuse_reply_err(req, ENOSPC);
        } else {
            fuse_reply_write(req, sz);
        }

    } catch (int err) {
        fuse_reply_err(req, err);
    } catch (...) {
        fuse_reply_err(req, ENOMEM);
    }
}

static void cleanDirectoryFilesRoot(Region &rootRegion, uint16_t rootEntries) {
    // Root, use root region

    FileEntry *entry = (FileEntry *)rootRegion.ptr + (rootEntries - 1);

    while (true) {
        if (entry->isValid()) {
            break;
        } else {
            entry->filename[0] = 0;
        }

        entry--;

        if ((void *)entry == rootRegion.ptr) {
            break;
        }
    }
}

static void cleanDirectoryFilesEntry(FileEntry *dir, Region &fatRegion,
                                     Region &dataRegion, uint32_t clusterSize) {
    assert(dir->isDirectory());

    uint16_t clusterNumber = dir->firstDataClusterLow;
    // Dirs always have a cluster alloced for . and .. (First two entries)
    assert(clusterNumber != 0);

    uint32_t lastValidEntry = 2;

    while (clusterNumber != 0xFFF) {

        uint8_t *curBuffer =
            dataRegion.ptr + ((clusterNumber - 2) * clusterSize);

        size_t entriesPerCluster = clusterSize / 32;

        for (uint16_t i = 0; i < entriesPerCluster; i++) {
            FileEntry *entry = (FileEntry *)(curBuffer + i * 32);

            if (entry->isValid()) {
                lastValidEntry = i;
            }
        }

        clusterNumber = getFatEntry(fatRegion, clusterNumber).getValue();
    }

    clusterNumber = dir->firstDataClusterLow;
    uint32_t curEntry = 0;

    while (clusterNumber != 0xFFF) {
        size_t entriesPerCluster = clusterSize / 32;

        uint8_t *curBuffer =
            dataRegion.ptr + ((clusterNumber - 2) * clusterSize);

        for (uint16_t i = 0; i < entriesPerCluster; i++) {
            if (lastValidEntry + 1 <= curEntry) {
                FileEntry *entry = (FileEntry *)(curBuffer + i * 32);
                entry->filename[0] = 0x00;
            }

            curEntry++;
        }

        clusterNumber = dir->firstDataClusterLow;
    }
}

static void cleanDirectoryFiles(Fat12Inode *fuseDir, BPB &bootBlock,
                                Region &rootRegion, Region &fatRegion,
                                Region &dataRegion, uint32_t clusterSize) {
    // All files after the last valid file should have the first byte of their
    // filename set to 0

    if (fuseDir->inode == FUSE_ROOT_ID) {
        cleanDirectoryFilesRoot(rootRegion, bootBlock.rootEntries);
    } else {
        cleanDirectoryFilesEntry(fuseDir->file, fatRegion, dataRegion,
                                 clusterSize);
    }
}

static void fat12_ll_unlink(fuse_req_t req, fuse_ino_t parent,
                            const char *name) {
    try {

        FuseContext *context = (FuseContext *)fuse_req_userdata(req);
        LockGuard lg(context->mutex);
        Fat12Inode &rootInode = context->rootInode;

        printf("unlink name %s\n", name);

        Fat12Inode *parentNode = rootInode.findInode(parent);

        if (parentNode) {
            for (size_t i = 0; i < parentNode->children.size(); i++) {
                Fat12Inode &child = parentNode->children[i];

                if (getCanonicalString(child.file->filename) == name) {

                    if (context->isInUse(child.inode)) {
                        fuse_reply_err(req, EBUSY);
                        return;
                    }

                    child.zombie = true;
                    fuse_reply_err(req, 0);
                    return;
                }
            }
        }

        fuse_reply_err(req, ENOENT);
    } catch (int err) {
        fuse_reply_err(req, err);
    } catch (...) {
        fuse_reply_err(req, ENOMEM);
    }
}

static void fat12_ll_forget(fuse_req_t req, fuse_ino_t ino, uint64_t nlookup) {
    printf("Forget ino %lu, %lu\n", ino, nlookup);

    try {

        FuseContext *context = (FuseContext *)fuse_req_userdata(req);
        LockGuard lg(context->mutex);

        Fat12Inode &rootInode = context->rootInode;
        Fat12Volume fat12Volume = context->fat12Volume;
        Fat12Inode *child = rootInode.findInode(ino);

        if (child) {
            printf("Lookup cur %lu, dec %lu\n", child->nlookup, nlookup);

            child->nlookup -= nlookup;

            if (child->nlookup == 0 && child->zombie) {
                printf("Delete ino %lu\n", ino);

                Fat12Inode *parent = rootInode.findParent(ino);

                if (!parent) {
                    printf("Parent of %lu not found for unlinking\n", ino);
                    fuse_reply_err(req, ENOENT);
                    return;
                }

                f_unlink(context->fat12Volume.fatRegion, child->file);
                child->file->filename[0] = 0xE5;

                cleanDirectoryFiles(
                    parent, fat12Volume.regionBPB.bootBlock,
                    fat12Volume.rootRegion, fat12Volume.fatRegion,
                    fat12Volume.dataRegion, fat12Volume.clusterSize);

                // TODO: Remove empty directory clusters
                parent->children.erase(std::find(
                    parent->children.begin(), parent->children.end(), *child));

                fuse_reply_err(req, 0);
                return;
            }

            printf("Ino has %lu lookups remaining\n", child->nlookup);
            fuse_reply_err(req, 0);
            return;

        } else {
            printf("File %lu not found for unlinking\n", ino);
        }

        fuse_reply_err(req, ENOENT);
    } catch (int err) {
        fuse_reply_err(req, err);
    } catch (...) {
        fuse_reply_err(req, ENOMEM);
    }
}

class FuseArgs {
  public:
    struct fuse_args args;

    FuseArgs(int argc, char *argv[]) { args = FUSE_ARGS_INIT(argc, argv); }

    ~FuseArgs() { fuse_opt_free_args(&args); }
};

class FuseOpts {
  public:
    struct fuse_cmdline_opts opts;

    FuseOpts(struct fuse_args &args) {
        if (fuse_parse_cmdline(&args, &opts) != 0)
            throw -1;
    }

    ~FuseOpts() {
        if (opts.mountpoint) {
            free(opts.mountpoint);
        }
    }
};

class FuseSession {
  public:
    struct fuse_session *se;

    FuseSession(struct fuse_args &args, fuse_lowlevel_ops &fat12_ll_ops,
                FuseContext &fuseContext) {
        se = fuse_session_new(&args, &fat12_ll_ops, sizeof(fat12_ll_ops),
                              &fuseContext);

        if (!se) {
            throw -1;
        }
    }
    ~FuseSession() { fuse_session_destroy(se); }
};

class FuseSignals {
    struct fuse_session *se;

  public:
    FuseSignals(struct fuse_session *se_) : se(se_) {
        if (fuse_set_signal_handlers(se) != 0)
            throw -1;
    }

    ~FuseSignals() { fuse_remove_signal_handlers(se); }
};

class FuseMount {
    struct fuse_session *se;

  public:
    FuseMount(struct fuse_session *se_, const char *mountpoint) : se(se_) {
        fuse_session_mount(se, mountpoint);
    }

    ~FuseMount() { fuse_session_unmount(se); }
};

int main(int argc, char *argv[]) {

    if (argc == 0) {
        printf("Shell error\n");
        return -2;
    }

    if (argc < 3) {
        printf("Needs one filename, afterwards one mountpoint\n");
        printf("usage: %s [options] <hdifile> <mountpoint>\n", argv[0]);
        return -3;
    }

    try {

        std::string filename(argv[argc - 2]);
        argv[argc - 2] = argv[argc - 1];
        argc--;

        printf("Mount %s on %s\n", filename.c_str(), argv[argc - 1]);

        FileDescriptorRO fd(filename.c_str());
        auto filedata = getBuffer(fd.fd);
        Fat12Volume fat12Volume(getFatVolume(filedata));

        printf("Volume OK - Mount via fuse\n");
        char *cwdc = get_current_dir_name();
        if (!cwdc) {
            printf("Cannot get current working directory\n");
            return -1;
        }

        std::string cwd(cwdc);

        {
            FuseContext fuseContext(fat12Volume);
            struct fuse_lowlevel_ops fat12_ll_ops {};

            fat12_ll_ops.readdir = fat12_ll_readdir;
            fat12_ll_ops.write = fat12_ll_write;
            fat12_ll_ops.lookup = fat12_ll_lookup;
            fat12_ll_ops.getattr = fat12_ll_getattr;
            fat12_ll_ops.open = fat12_ll_open;
            fat12_ll_ops.read = fat12_ll_read;
            fat12_ll_ops.create = fat12_ll_create;
            fat12_ll_ops.release = fat12_ll_release;
            fat12_ll_ops.opendir = fat12_ll_opendir;
            fat12_ll_ops.releasedir = fat12_ll_releasedir;
            fat12_ll_ops.unlink = fat12_ll_unlink;
            fat12_ll_ops.forget = fat12_ll_forget;

            FuseArgs fuseArgs(argc, argv);
            FuseOpts fuseOpts(fuseArgs.args);

            if (fuseOpts.opts.show_help) {
                printf("usage: %s [options] <hdifile> <mountpoint>\n\n",
                       argv[0]);
                fuse_cmdline_help();
                fuse_lowlevel_help();
                return 0;
            }

            if (fuseOpts.opts.show_version) {
                printf("FUSE library version %s\n", fuse_pkgversion());
                fuse_lowlevel_version();
                return 0;
            }

            if (!fuseOpts.opts.mountpoint) {
                printf("usage: %s [options] <hdifile> <mountpoint>\n", argv[0]);
                printf("       %s --help\n", argv[0]);
                return 0;
            }

            FuseSession fuseSession(fuseArgs.args, fat12_ll_ops, fuseContext);
            FuseSignals fuseSignals(fuseSession.se);
            FuseMount fuseMount(fuseSession.se, fuseOpts.opts.mountpoint);

            fuse_daemonize(fuseOpts.opts.foreground);
            fuse_session_loop(fuseSession.se);
        }

        printf("Sync fat\n");
        syncFAT(fat12Volume.regionBPB.bootBlock, fat12Volume.volume);

        printf("Write file \n");

        chdir(cwd.c_str());

        std::string shadowFilename = filename + ".shadow";
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
            return -2;
        }

    } catch (int ex) {
        printf("Exception in main %d", ex);
        return ex;
    } catch (...) {
        return -1;
    }

    return 0;
}
