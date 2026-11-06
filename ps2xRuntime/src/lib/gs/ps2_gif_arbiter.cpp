#include "runtime/gs/ps2_gif_arbiter.h"
#include <algorithm>
#include <cstring>

GifArbiter::GifArbiter(ProcessPacketFn processFn)
    : m_processFn(std::move(processFn))
{
}

bool GifArbiter::isImagePacket(const uint8_t *data, uint32_t sizeBytes)
{
    if (!data || sizeBytes < 16u)
        return false;

    uint64_t tagLo = 0;
    std::memcpy(&tagLo, data, sizeof(tagLo));
    const uint8_t flg = static_cast<uint8_t>((tagLo >> 58) & 0x3u);
    // FLG=3 ("disabled" in the GS manual) behaves exactly like IMAGE: NLOOP qwords of image data.
    return flg == 2u || flg == 3u;
}

void GifArbiter::submit(GifPathId pathId, const uint8_t *data, uint32_t sizeBytes, bool path2DirectHl)
{
    if (!data || sizeBytes < 16 || (!m_processFn && !m_processPathFn))
        return;

    GifArbiterPacket pkt;
    pkt.pathId = pathId;
    pkt.path2DirectHl = (pathId == GifPathId::Path2) && path2DirectHl;
    pkt.path3Image = (pathId == GifPathId::Path3) && isImagePacket(data, sizeBytes);
    // Best fit among the spare buffers, so a big packet doesn't regrow a small one.
    size_t pick = m_spare.size();
    for (size_t i = 0; i < m_spare.size(); ++i)
    {
        const size_t capacity = m_spare[i].capacity();
        if (capacity >= sizeBytes && (pick == m_spare.size() || capacity < m_spare[pick].capacity()))
            pick = i;
    }
    if (pick != m_spare.size())
    {
        pkt.data = std::move(m_spare[pick]);
        m_spare[pick] = std::move(m_spare.back());
        m_spare.pop_back();
    }
    pkt.data.assign(data, data + sizeBytes);
    m_queue.push_back(std::move(pkt));
}

void GifArbiter::drain()
{
    if (!m_processFn && !m_processPathFn)
        return;

    const auto before = [](const GifArbiterPacket &a, const GifArbiterPacket &b)
                     {
                         // DIRECTHL cannot preempt PATH3 IMAGE transfers.
                         if (a.path2DirectHl != b.path2DirectHl || a.path3Image != b.path3Image)
                         {
                             if (a.path3Image && b.path2DirectHl)
                                 return true;
                             if (a.path2DirectHl && b.path3Image)
                                 return false;
                         }
                         return pathPriority(a.pathId) < pathPriority(b.pathId);
                     };
    // stable_sort allocates a temporary buffer for more than a few dozen packets; they usually
    // arrive in order already.
    if (!std::is_sorted(m_queue.begin(), m_queue.end(), before))
        std::stable_sort(m_queue.begin(), m_queue.end(), before);

    for (size_t i = 0; i < m_queue.size(); ++i)
    {
        auto &pkt = m_queue[i];
        if (!pkt.data.empty())
        {
            if (m_processPathFn)
                m_processPathFn(pkt.pathId, pkt.data.data(), static_cast<uint32_t>(pkt.data.size()));
            else
                m_processFn(pkt.data.data(), static_cast<uint32_t>(pkt.data.size()));
        }
    }
    for (auto &pkt : m_queue)
    {
        if (m_spare.size() < 32u && pkt.data.capacity() <= (8u << 20))
            m_spare.push_back(std::move(pkt.data));
    }
    m_queue.clear();
}

uint8_t GifArbiter::pathPriority(GifPathId id)
{
    return static_cast<uint8_t>(id);
}
