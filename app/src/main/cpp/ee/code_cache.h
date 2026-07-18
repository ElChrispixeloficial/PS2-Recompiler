#pragma once
#include <cstdint>
#include <cstddef>
#include <cstring>
#include <sys/mman.h>
#include <unistd.h>
#include <android/log.h>

#define CC_LOGI(...) __android_log_print(ANDROID_LOG_INFO,  "PS2-CC", __VA_ARGS__)
#define CC_LOGE(...) __android_log_print(ANDROID_LOG_ERROR, "PS2-CC", __VA_ARGS__)

constexpr size_t MAX_BLOCKS     = 0x00100000; // 1M blocks (8 MB pointer table)
constexpr size_t CODE_CACHE_SIZE = 32 * 1024 * 1024; // 32 MB executable code

class CodeCache {
public:
    using BlockFn = void (*)();
    
    CodeCache() : m_code(nullptr), m_capacity(0), m_offset(0),
                  m_blocks(nullptr), m_block_count(0) {
        m_code = (uint8_t*)mmap(NULL, CODE_CACHE_SIZE, PROT_READ | PROT_WRITE | PROT_EXEC,
                                MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (m_code == MAP_FAILED) {
            CC_LOGE("CodeCache: mmap %zu MB failed", CODE_CACHE_SIZE / (1024*1024));
            m_code = nullptr;
            return;
        }
        m_capacity = CODE_CACHE_SIZE;
        m_offset = 0;

        m_blocks = new(std::nothrow) void*[MAX_BLOCKS];
        if (!m_blocks) {
            CC_LOGE("CodeCache: block table alloc failed (%zu MB)", (MAX_BLOCKS * sizeof(void*)) / (1024*1024));
            munmap(m_code, CODE_CACHE_SIZE);
            m_code = nullptr;
            m_capacity = 0;
            return;
        }
        m_block_count = MAX_BLOCKS;
        memset(m_blocks, 0, MAX_BLOCKS * sizeof(void*));
        CC_LOGI("CodeCache: OK — code=%zu MB, blocks=%zu (%zu KB)",
                CODE_CACHE_SIZE / (1024*1024), MAX_BLOCKS, (MAX_BLOCKS * sizeof(void*)) / 1024);
    }
    
    ~CodeCache() {
        delete[] m_blocks;
        if (m_code) munmap(m_code, m_capacity);
    }

    // Non-copyable, non-movable (owns mmap + raw pointer)
    CodeCache(const CodeCache&) = delete;
    CodeCache& operator=(const CodeCache&) = delete;
    CodeCache(CodeCache&&) = delete;
    CodeCache& operator=(CodeCache&&) = delete;
    
    void* alloc(size_t size) {
        if (!m_code) return nullptr;
        if (m_offset + size > m_capacity) {
            flush();
        }
        void* ptr = m_code + m_offset;
        m_offset += size;
        return ptr;
    }
    
    void* lookup(uint32_t pc) {
        uint32_t index = (pc & 0x01FFFFFF) >> 2;
        if (index >= m_block_count) return nullptr;
        return m_blocks[index];
    }
    
    void register_block(uint32_t pc, BlockFn fn, size_t) {
        uint32_t index = (pc & 0x01FFFFFF) >> 2;
        if (index >= m_block_count) return;
        m_blocks[index] = (void*)fn;
    }
    
    void flush() {
        m_offset = 0;
        for (size_t i = 0; i < m_block_count; i++) {
            m_blocks[i] = nullptr;
        }
    }
    
    void invalidate_range(uint32_t start, uint32_t end) {
        uint32_t start_idx = (start & 0x01FFFFFF) >> 2;
        uint32_t end_idx   = (end   & 0x01FFFFFF) >> 2;
        if (start_idx > end_idx) std::swap(start_idx, end_idx);
        for (uint32_t i = start_idx; i <= end_idx && i < m_block_count; i++) {
            m_blocks[i] = nullptr;
        }
    }

    bool is_valid() const { return m_code != nullptr && m_blocks != nullptr; }
    
private:
    uint8_t* m_code;
    size_t   m_capacity;
    size_t   m_offset;
    void**   m_blocks;
    size_t   m_block_count;
};
