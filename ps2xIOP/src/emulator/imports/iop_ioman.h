#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace ps2x::iop
{
    class IopHost;
}

namespace ps2x::iop::detail
{
    struct IopCpuState;
    class IopGuestExecutor;
    class IopMemory;

    class IopIoman
    {
    public:
        IopIoman(IopHost &host, IopMemory &memory) noexcept;
        ~IopIoman();

        IopIoman(const IopIoman &) = delete;
        IopIoman &operator=(const IopIoman &) = delete;

        void reset();
        [[nodiscard]] bool dispatchImport(uint16_t ordinal, IopCpuState &cpu, IopGuestExecutor &executor);

    private:
        struct Device
        {
            uint32_t address = 0u;
            uint32_t gp = 0u;
            std::string name;
        };

        // A read-only file on the host, opened through the guest path translation
        // (cdrom0:, host0:, ...). Reads are positional, so the cursor lives here.
        struct OpenFile
        {
            uint64_t handle = 0u;
            uint64_t size = 0u;
            uint64_t position = 0u;
            std::string path;
        };

        int32_t open(uint32_t pathAddress, uint32_t flags);
        int32_t close(int32_t fd);
        int32_t read(int32_t fd, uint32_t buffer, uint32_t size);
        int32_t write(int32_t fd, uint32_t buffer, uint32_t size);
        int32_t lseek(int32_t fd, int32_t offset, uint32_t whence);
        void closeAll();

        IopHost &m_host;
        IopMemory &m_memory;
        std::vector<Device> m_devices;
        std::map<int32_t, OpenFile> m_files;
    };
}
