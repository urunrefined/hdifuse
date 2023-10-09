// HDI Format

// clang-format off

//0x00 4 Byte INT LE | Reserved           | Must be set to zero
//0x04 4 Byte INT LE | Type identifier    | Only used for FDDs. Otherwise most likely set to 0
//0x08 4 Byte INT LE | Header Size        | Size of the header. This header will be cut
//0x0C 4 Byte INT LE | Data Size          | Size of the entire image (after the header)
//0x10 4 Byte INT LE | Bytes per Sector   | What it says
//0x14 4 Byte INT LE | Sectors            | Sector Count
//0x18 4 Byte INT LE | Heads              | Head Count
//0x1C 4 Byte INT LE | Cylinders          | Cylinder Count

//clang-format on

// The sector / head / cylider count are irrelevant for us here.


#include <unistd.h>
#include <fcntl.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <stdint.h>

#include <string>
#include <vector>

class FileDescriptorRO {
public:
    int fd;

    FileDescriptorRO(const char *filename){
        fd = open(filename, O_RDONLY);

        if(fd == -1){
            throw "Cannot open file for reading";
        }
    }

    ~FileDescriptorRO(){
        close(fd);
    }
};

class FileDescriptorWO {
public:
    int fd;

    FileDescriptorWO(const char *filename){
        fd = open(filename, O_WRONLY | O_TRUNC | O_CREAT, S_IRUSR | S_IWUSR);

        if(fd == -1){
            throw "Cannot open file for writing";
        }
    }

    ~FileDescriptorWO(){
        close(fd);
    }
};

static std::vector<uint8_t> getBuffer(int fd){
    struct stat statbuf;
    if(fstat(fd, &statbuf) != 0){
        throw "Cannot open file";
    }
    
    std::vector<uint8_t> buf(statbuf.st_size);

    ssize_t hasRead = read(fd, buf.data(), buf.size());

    if(hasRead == -1){
        throw ("Cannot read from file");
    }

    if((size_t)hasRead != buf.size()){
        throw ("Short read from file");
    }

    return buf;
}

class Position {
public:
    size_t cur;
    const size_t size;
};

template <class N1>
N1 nextIntLEToHost(Position& pos, uint8_t *buffer){
    if(sizeof(N1) + pos.cur >= pos.size){
        throw "Cannot advance. No more buffer";
    }
    
    N1 n1 = buffer[pos.cur];
    pos.cur++;
   
    for(size_t i = 1; i < sizeof(N1); i++){
        n1 += (buffer[pos.cur] << (i * 8));
        pos.cur++;
    }
    
    return n1;
}

static void skipTo(Position& pos, size_t newPos){
    if(newPos >= pos.size){
        throw "Cannot skip. Out of range";
    }
    
    pos.cur = newPos;
}

int main(int argc, char *argv[]){
    
    if (argc < 2){
        return -1;
    }
    
    try{
        FileDescriptorRO fd (argv[1]);
    
        auto filedata = getBuffer(fd.fd);
    
        Position pos {0, filedata.size()};
    
        printf("Process buffer %zu\n", filedata.size());
    
        uint32_t reserved = nextIntLEToHost<uint32_t>(pos, filedata.data());
        
        if(reserved){
            printf("Is not a supported format. First 4 bytes must be 0\n");
            return -2;
        }
        
        uint32_t identifier = nextIntLEToHost<uint32_t>(pos, filedata.data());
        printf("Identifier %u\n", identifier);
        
        uint32_t headerSize = nextIntLEToHost<uint32_t>(pos, filedata.data());
        printf("headerSize %u\n", headerSize);
        
        uint32_t dataSize = nextIntLEToHost<uint32_t>(pos, filedata.data());
        printf("dataSize %u\n", dataSize);
        
        uint32_t bytesPerSector = nextIntLEToHost<uint32_t>(pos, filedata.data());
        printf("bytesPerSector %u\n", bytesPerSector);
        
        uint32_t sectors = nextIntLEToHost<uint32_t>(pos, filedata.data());
        printf("sectors %u\n", sectors);
        
        uint32_t heads = nextIntLEToHost<uint32_t>(pos, filedata.data());
        printf("heads %u\n", heads);
        
        uint32_t cylinders = nextIntLEToHost<uint32_t>(pos, filedata.data());
        printf("cylinders %u\n", cylinders);
        
        skipTo(pos, headerSize);
        
        if(argc == 3){
            printf("Write image without headers to %s\n", argv[2]);
            FileDescriptorWO outfile(argv[2]);
        
            write(outfile.fd, filedata.data() + pos.cur, pos.size - pos.cur);
        }
        
    }catch(const char *ex){
        printf("Exception %s\n", ex);
    }
    
    return 0;
}
