#include "iop_ioman.h"

#include "../core/iop_cpu.h"
#include "../core/iop_memory.h"
#include "../services/iop_rpc.h"
#include "ps2x/iop/iop_host.h"

#include <algorithm>

namespace ps2x::iop::detail
{
    namespace
    {
        // ioman returns negated errno values.
        constexpr int32_t kEnoent = -2;
        constexpr int32_t kEbadf = -9;
        constexpr int32_t kEacces = -13;
        constexpr int32_t kEfault = -14;
        constexpr int32_t kEinval = -22;
        constexpr int32_t kEmfile = -24;

        constexpr uint32_t kOpenWrite = 0x0002u; // O_WRONLY bit, also set by O_RDWR
        constexpr int32_t kFirstFd = 3;          // 0-2 are the console
        constexpr int32_t kMaxFiles = 32;
    }

    IopIoman::IopIoman(IopHost &host, IopMemory &memory) noexcept
        : m_host(host), m_memory(memory)
    {
    }

    IopIoman::~IopIoman()
    {
        closeAll();
    }

    void IopIoman::reset()
    {
        m_devices.clear();
        closeAll();
    }

    void IopIoman::closeAll()
    {
        for (const auto &[fd, file] : m_files)
            m_host.closeHostFile(file.handle);
        m_files.clear();
    }

    bool IopIoman::dispatchImport(uint16_t ordinal, IopCpuState &cpu, IopGuestExecutor &executor)
    {
        constexpr size_t kMaxDevices = 16u;
        const uint32_t a0 = cpu.gpr[4];
        const uint32_t a1 = cpu.gpr[5];
        const uint32_t a2 = cpu.gpr[6];
        const auto setV0 = [&](uint32_t value)
        {
            cpu.gpr[2] = value;
        };

        switch (ordinal)
        {
        case 4: // open
            setV0(static_cast<uint32_t>(open(a0, a1)));
            return true;
        case 5: // close
            setV0(static_cast<uint32_t>(close(static_cast<int32_t>(a0))));
            return true;
        case 6: // read
            setV0(static_cast<uint32_t>(read(static_cast<int32_t>(a0), a1, a2)));
            return true;
        case 7: // write
            setV0(static_cast<uint32_t>(write(static_cast<int32_t>(a0), a1, a2)));
            return true;
        case 8: // lseek
            setV0(static_cast<uint32_t>(lseek(static_cast<int32_t>(a0), static_cast<int32_t>(a1), a2)));
            return true;
        case 20: // AddDrv
        {
            if (a0 == 0u || m_devices.size() >= kMaxDevices)
            {
                setV0(0xFFFFFFFFu);
                return true;
            }

            const uint32_t nameAddress = m_memory.read32(a0);
            const uint32_t operations = m_memory.read32(a0 + 16u);
            const std::string name = m_memory.readString(nameAddress, 64u);
            if (nameAddress == 0u || operations == 0u || name.empty())
            {
                setV0(0xFFFFFFFFu);
                return true;
            }

            m_devices.push_back({a0, cpu.gpr[28], name});
            const uint32_t init = m_memory.read32(operations);
            if (init != 0u)
            {
                const int32_t result = static_cast<int32_t>(
                    executor.executeGuestFunction(init, a0, 0u, 0u, 0u, cpu.gpr[28]));
                if (result < 0)
                {
                    m_devices.pop_back();
                    setV0(0xFFFFFFFFu);
                    return true;
                }
            }

            setV0(0u);
            return true;
        }
        case 21: // DelDrv
        {
            const std::string name = m_memory.readString(a0, 64u);
            const auto device = std::find_if(
                m_devices.begin(), m_devices.end(),
                [&](const Device &candidate)
                { return candidate.name == name; });
            if (device == m_devices.end())
            {
                setV0(0xFFFFFFFFu);
                return true;
            }

            const uint32_t operations = m_memory.read32(device->address + 16u);
            const uint32_t deinit = operations != 0u ? m_memory.read32(operations + 4u) : 0u;
            if (deinit != 0u)
                (void)executor.executeGuestFunction(deinit, device->address, 0u, 0u, 0u, device->gp);
            m_devices.erase(device);
            setV0(0u);
            return true;
        }
        default:
            return false;
        }
    }

    // Files are served read-only from the host through the guest path translation. Devices a
    // module registers with AddDrv are not dispatched to yet.
    int32_t IopIoman::open(uint32_t pathAddress, uint32_t flags)
    {
        const std::string path = m_memory.readString(pathAddress, 256u);
        if ((flags & kOpenWrite) != 0u)
        {
            m_host.log(LogLevel::Warning, "[IOP] ioman open for writing is not supported: " + path);
            return kEacces;
        }

        const std::string hostPath = m_host.translateGuestPath(path);
        const uint64_t handle = hostPath.empty() ? 0u : m_host.openHostFile(hostPath);
        uint64_t size = 0u;
        if (handle == 0u || !m_host.hostFileSize(handle, size))
        {
            if (handle != 0u)
                m_host.closeHostFile(handle);
            m_host.log(LogLevel::Warning, "[IOP] ioman open failed: " + path);
            return kEnoent;
        }

        int32_t fd = kFirstFd;
        while (m_files.count(fd) != 0u)
            ++fd;
        if (fd >= kFirstFd + kMaxFiles)
        {
            m_host.closeHostFile(handle);
            return kEmfile;
        }

        m_files.emplace(fd, OpenFile{handle, size, 0u, path});
        m_host.log(LogLevel::Info,
                   "[IOP] ioman open " + path + " -> fd " + std::to_string(fd) + " (" + std::to_string(size) + " bytes)");
        return fd;
    }

    int32_t IopIoman::close(int32_t fd)
    {
        const auto file = m_files.find(fd);
        if (file == m_files.end())
            return kEbadf;
        m_host.closeHostFile(file->second.handle);
        m_files.erase(file);
        return 0;
    }

    int32_t IopIoman::read(int32_t fd, uint32_t buffer, uint32_t size)
    {
        const auto found = m_files.find(fd);
        if (found == m_files.end())
            return kEbadf;
        OpenFile &file = found->second;
        if (file.position >= file.size || size == 0u)
            return 0;

        const size_t wanted = static_cast<size_t>(std::min<uint64_t>(size, file.size - file.position));
        if (!m_memory.ownsRamRange(buffer, wanted))
        {
            static unsigned failures = 0;
            if (failures++ < 16)
                m_host.log(LogLevel::Warning, "[IOP ioman] read buffer not owned: " + file.path +
                    " buffer=" + std::to_string(buffer) + " bytes=" + std::to_string(wanted));
            return kEfault;
        }
        std::vector<uint8_t> bytes(wanted);
        size_t bytesRead = 0u;
        if (!m_host.readHostFile(file.handle, file.position, bytes.data(), wanted, bytesRead) ||
            !m_memory.writeRam(buffer, bytes.data(), bytesRead))
            return kEfault;
        file.position += bytesRead;
        return static_cast<int32_t>(bytesRead);
    }

    int32_t IopIoman::write(int32_t fd, uint32_t buffer, uint32_t size)
    {
        if (fd == 1 || fd == 2) // console
        {
            m_host.log(LogLevel::Info, "[IOP ioman write] " + m_memory.readString(buffer, std::min<uint32_t>(size, 2048u)));
            return static_cast<int32_t>(size);
        }
        return m_files.count(fd) != 0u ? kEacces : kEbadf;
    }

    int32_t IopIoman::lseek(int32_t fd, int32_t offset, uint32_t whence)
    {
        const auto found = m_files.find(fd);
        if (found == m_files.end())
            return kEbadf;
        OpenFile &file = found->second;

        int64_t base = 0;
        switch (whence)
        {
        case 0: // SEEK_SET
            break;
        case 1: // SEEK_CUR
            base = static_cast<int64_t>(file.position);
            break;
        case 2: // SEEK_END
            base = static_cast<int64_t>(file.size);
            break;
        default:
            return kEinval;
        }
        const int64_t position = base + offset;
        if (position < 0 || position > INT32_MAX)
            return kEinval;
        file.position = static_cast<uint64_t>(position);
        return static_cast<int32_t>(position);
    }
}
