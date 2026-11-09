#pragma once
#include <cstdint>
#include <map>
#include <string>
#include <vector>
#include <sys/stat.h>

namespace kobalt::webkit {
struct Resource
{
    std::string path, mime;
    bool linear = true;
};

struct Navigation
{
    std::string title, href;
};

// Private helper-side reader. No extraction, Qt, Nickel services or saved data.
// Budgets match the publication reader; a resource is verified before delivery.
class Epub
{
public:
    bool open(const std::string &path);
    bool read(const std::string &name, std::string &bytes) const;
    bool resource(const std::string &name, Resource &result) const;
    bool resolveLink(unsigned spine, const std::string &href, unsigned &target,
                     std::string &fragment) const;
    static bool resolve(const std::string &base, const std::string &href, std::string &path,
                        std::string &fragment);
    std::string uri(const std::string &path) const;

    const std::string &host() const { return host_; }

    const std::vector<Resource> &spine() const { return spine_; }

    const std::vector<Navigation> &navigation() const { return navigation_; }

    const std::string &error() const { return error_; }

    std::string description() const;

private:
    struct Member
    {
        uint32_t offset, packed, size, crc;
        uint16_t method, flags;
    };

    std::string filename_, error_, host_;
    std::map<std::string, Member> members_;
    std::map<std::string, Resource> resources_;
    std::vector<Resource> spine_;
    std::vector<Navigation> navigation_;

    struct ObfuscatedFont
    {
        std::string key;
        size_t prefix;
    };

    std::map<std::string, ObfuscatedFont> obfuscated_;
    struct stat identity_{};
    uint32_t directoryOffset_ = 0;
    bool fail(const char *message);
    bool package();
    bool fonts(const std::string &identifier, const std::string &adobeKey);
};

std::string jsonString(const std::string &text);
} // namespace kobalt::webkit
