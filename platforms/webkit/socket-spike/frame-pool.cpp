#include "frame-pool.h"

#include <cerrno>
#include <cstdio>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

namespace {
int anonymousShm()
{
    const int random = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    if (random < 0)
        return -1;
    unsigned char bytes[16];
    size_t offset = 0;
    while (offset < sizeof bytes) {
        const auto count = read(random, bytes + offset, sizeof bytes - offset);
        if (count < 0 && errno == EINTR)
            continue;
        if (count <= 0)
            break;
        offset += size_t(count);
    }
    close(random);
    if (offset != sizeof bytes)
        return -1;
    constexpr char hex[] = "0123456789abcdef";
    char name[45] = "/kobalt-wpe-";
    for (unsigned i = 0; i < sizeof bytes; ++i) {
        name[12 + i * 2] = hex[bytes[i] >> 4];
        name[13 + i * 2] = hex[bytes[i] & 15];
    }
    const int fd = shm_open(name, O_CREAT | O_EXCL | O_RDWR, 0600);
    if (fd < 0)
        return -1;
    if (shm_unlink(name) < 0) {
        close(fd);
        return -1;
    }
    const int flags = fcntl(fd, F_GETFD);
    if (flags < 0 || !(flags & FD_CLOEXEC)) {
        close(fd);
        return -1;
    }
    return fd;
}

uint32_t frameSize(uint32_t width, uint32_t height)
{
    if (!width || !height || uint64_t(width) * 4 > spike::maxFrameBytes
        || uint64_t(width) * 4 * height > spike::maxFrameBytes)
        return 0;
    return width * 4 * height;
}
} // namespace

FramePool::Slot::~Slot()
{
    if (map)
        munmap(const_cast<uchar *>(map), capacity);
    if (fd >= 0)
        close(fd);
}

FramePool::FramePool(uint64_t id, uint32_t w, uint32_t h)
    : generation(id), width(w), height(h), capacity(frameSize(w, h))
{
    if (!id || !capacity || QSysInfo::ByteOrder != QSysInfo::LittleEndian)
        return;
    for (auto &slot : buffers) {
        auto next = std::make_shared<Slot>();
        next->fd = anonymousShm();
        next->capacity = capacity;
        if (next->fd < 0 || ftruncate(next->fd, capacity) < 0)
            break;
        void *map = mmap(nullptr, capacity, PROT_READ, MAP_SHARED, next->fd, 0);
        if (map == MAP_FAILED)
            break;
        next->map = static_cast<const uchar *>(map);
        slot = std::move(next);
    }
}

int FramePool::descriptor(unsigned slot) const
{
    return buffers.at(slot)->fd;
}

bool FramePool::reserve(uint32_t color, spike::FrameSpec &spec)
{
    if (!valid() || color > 1)
        return false;
    for (unsigned i = 0; i < buffers.size(); ++i) {
        State expected = Free;
        if (buffers[i]->state.compare_exchange_strong(expected, Writing)) {
            spec = {generation, i, width, height, color};
            return true;
        }
    }
    return false;
}

bool FramePool::matches(const spike::FrameSpec &spec) const
{
    return valid() && spec.generation == generation && spec.slot < buffers.size()
            && spec.width == width && spec.height == height
            && buffers[spec.slot]->state == Writing;
}

void FramePool::discard(const spike::FrameSpec &spec)
{
    if (matches(spec))
        buffers[spec.slot]->state = Free;
}

void FramePool::release(void *opaque)
{
    std::unique_ptr<std::shared_ptr<Slot>> lease(static_cast<std::shared_ptr<Slot> *>(opaque));
    (*lease)->state = Free;
}

QImage FramePool::publish(const spike::FrameSpec &spec, const std::string &reply)
{
    if (!matches(spec) || reply != spec.reply(capacity))
        return {};
    const auto &slot = buffers[spec.slot];
    auto *lease = new std::shared_ptr<Slot>(slot);
    slot->state = Leased;
    QImage image(slot->map, width, height, width * 4, QImage::Format_ARGB32_Premultiplied,
                 release, lease);
    if (image.isNull())
        release(lease);
    return image;
}
