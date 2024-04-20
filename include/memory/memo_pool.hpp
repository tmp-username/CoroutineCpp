#include <sys/mman.h>
#include <unistd.h>
#include <fcntl.h>
#include <unistd.h>
#include <semaphore.h>
#include <iostream>
#include <atomic>
#include <thread>
#include <vector>
#include <memory>
#include <map>
#include <shared_mutex>
#include <algorithm>
#include <cstring>

namespace memo::shm {

struct WrBlock {
    int idx;
    char *buf{nullptr};
    std::size_t size;
};
typedef WrBlock RdBlock;

class ShmBase {
    public:
        struct Block {
            int b_idx;
            char *b_buf{nullptr};
            std::size_t b_size;
            std::shared_mutex sh_mtx_;
        };

        ShmBase(const std::string &sh_name, int count, int size)
            : m_count{count}, m_size{size} {
            m_total_size = (sizeof(Block) + m_size) * m_count;
            m_sh_name = "shm_" + sh_name;
        }

        virtual ~ShmBase() {}

        virtual bool CreateShm() = 0;
        virtual bool AcquireWrBlock(WrBlock &block) = 0;
        virtual bool AcquireRdBlock(int idx, RdBlock &block) = 0;
        virtual bool ReleaseWrBlock(const WrBlock &) = 0;
        virtual bool ReleaseRdBlock(const RdBlock &) = 0;

    protected:
        Block *m_block{nullptr};
        int m_count{0};
        int m_size{0};
        int m_total_size;
        std::string m_sh_name;
        void *m_start_addr{nullptr};
};

class MmapShmAlloc : public ShmBase {
    public:
        MmapShmAlloc(const std::string &sh_name, int count, int size)
            : ShmBase(sh_name, count, size) {
        }

        ~MmapShmAlloc() {
            munmap(m_start_addr, m_total_size);
        }

        bool CreateShm() override {
            int fd = open(m_sh_name.data(), O_CREAT | O_RDWR, 0644);
            if (-1 == fd) {
                std::cout << "create shm failed\n";
                return false;
            }
            ftruncate(fd, m_total_size);
            m_start_addr = mmap(nullptr, m_total_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
            close(fd);
            if (m_start_addr == MAP_FAILED) {
                std::cout << "mmap failed\n";
                return false;
            }
            m_block = new (m_start_addr)Block[m_count];
            for (int i=0; i<m_count; i++) {
                m_block[i].b_buf = new ((char *)m_start_addr + sizeof(Block) * m_count + i * m_size)char[m_size];
                m_block[i].b_size = m_size;
                m_block[i].b_idx = i;
            }
            return true;
        }

        bool AcquireWrBlock(WrBlock &block) override {
            int i{0};
            for (i=0; i<m_count; i++) {
                if (m_block[i].sh_mtx_.try_lock()) {
                    break;
                }
            }
            if (i == m_count) {
                std::cout << "Acquire wr_block failed";
                return false;
            }
            block.buf = m_block[i].b_buf;
            block.idx = i;
            block.size = m_block[i].b_size;
            return true;
        }

        bool AcquireRdBlock(int idx, RdBlock &block) override {
            int i{0};
            for (i=0; i<m_count; i++) {
                if (m_block[i].b_idx == idx) {
                    break;
                }
            }
            if (i == m_count) {
                std::cout << "Acquire rd_block failed\n";
                return false;
            }
            block.buf = m_block[i].b_buf;
            block.idx = m_block[i].b_idx;
            block.size = m_block[i].b_size;
            return true;
        }

        bool ReleaseWrBlock(const WrBlock &block) override {
            int i{0};
            for (i=0; i<m_count; i++) {
                if (i == block.idx) {
                    break;
                }
            }
            if (i == m_count) {
                return false;
            }
            m_block[i].sh_mtx_.unlock();
            return true;
        }

        bool ReleaseRdBlock(const RdBlock &block) override {
            int i{0};
            for (i=0; i<m_count; i++) {
                if (i == block.idx) {
                    break;
                }
            }
            if (i == m_count) {
                return false;
            }
            m_block[i].sh_mtx_.unlock_shared();
            return true;
        }
};

}   /// end of namespace

#ifdef TEST
int main() {
    using namespace memo::shm;
    std::shared_ptr<ShmBase> shm_ptr(new MmapShmAlloc("data", 10, 1024));
    shm_ptr->CreateShm();
    int pipefd[2];
    pid_t cpid;
    if (pipe(pipefd) == -1) {
        exit(EXIT_FAILURE);
    }
    cpid = fork();
    if (cpid == -1) {
        exit(EXIT_FAILURE);
    }
    char buf[16]{'\0'};
    if (cpid == 0) {    /* Child reads from pipe */
        close(pipefd[1]);          /* Close unused write end */
        while (read(pipefd[0], buf, sizeof(buf)) > 0) {
            close(pipefd[0]);
            RdBlock rd_block;
            int idx = std::stoi(buf);
            if (shm_ptr->AcquireRdBlock(idx, rd_block)) {
                std::cout << rd_block.buf << "\n";
                shm_ptr->ReleaseRdBlock(rd_block);
            }
            _exit(EXIT_SUCCESS);
        }
    } else {            /* Parent writes argv[1] to pipe */
        close(pipefd[0]);          /* Close unused read end */
        WrBlock wr_block;
        if (shm_ptr->AcquireWrBlock(wr_block)) {
            strcpy(wr_block.buf, "hello message");
            std::string idx = std::to_string(wr_block.idx);
            write(pipefd[1], idx.data(), idx.size());
            shm_ptr->ReleaseWrBlock(wr_block);
        }
        close(pipefd[1]);          /* Reader will see EOF */
        exit(EXIT_SUCCESS);
    }

    return 0;
}
#endif
