#pragma once
#include <cstdint>
#include <cstddef>
#include <array>
#include <cstring>
#include <sys/mman.h>
#include <unistd.h>

constexpr size_t MAX_BLOCKS = 0x00800000; // 32MB / 4 bytes = 8M bloques
constexpr size_t CODE_CACHE_SIZE = 64 * 1024 * 1024; // 64 MB de memoria para código ARM64

class CodeCache {
public:
    using BlockFn = void (*)();
    
    CodeCache() {
        // Reservar memoria con permisos de Lectura, Escritura y EJECUCIÓN
        m_code = (uint8_t*)mmap(NULL, CODE_CACHE_SIZE, PROT_READ | PROT_WRITE | PROT_EXEC,
                                MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (m_code == MAP_FAILED) {
            m_code = nullptr;
            m_capacity = 0;
            m_offset = 0;
            return;
        }
        m_capacity = CODE_CACHE_SIZE;
        m_offset = 0;
        for (auto& p : m_blocks) p = nullptr;
    }
    
    ~CodeCache() {
        if (m_code) munmap(m_code, m_capacity);
    }
    
    void* alloc(size_t size) {
        if (!m_code) return nullptr;
        if (m_offset + size > m_capacity) {
            flush(); // Si se llena, vaciar
        }
        void* ptr = m_code + m_offset;
        m_offset += size;
        return ptr;
    }
    
    void* lookup(uint32_t pc) {
        uint32_t index = (pc & 0x01FFFFFF) >> 2;
        if (index >= MAX_BLOCKS) return nullptr;
        return m_blocks[index];
    }
    
    void register_block(uint32_t pc, BlockFn fn, size_t size) {
        uint32_t index = (pc & 0x01FFFFFF) >> 2;
        if (index >= MAX_BLOCKS) return;
        m_blocks[index] = (void*)fn;
    }
    
    void flush() {
        m_offset = 0;
        // Limpiar la tabla de bloques para que no ejecute código viejo
        for (auto& p : m_blocks) {
            p = nullptr;
        }
    }
    
    void invalidate_range(uint32_t start, uint32_t end) {
        uint32_t start_idx = (start & 0x01FFFFFF) >> 2;
        uint32_t end_idx = (end & 0x01FFFFFF) >> 2;
        if (start_idx > end_idx) std::swap(start_idx, end_idx);
        for (uint32_t i = start_idx; i <= end_idx && i < MAX_BLOCKS; i++) {
            m_blocks[i] = nullptr;
        }
    }
    
private:
    uint8_t* m_code = nullptr;
    size_t m_capacity = 0;
    size_t m_offset = 0;
    std::array<void*, MAX_BLOCKS> m_blocks;
};