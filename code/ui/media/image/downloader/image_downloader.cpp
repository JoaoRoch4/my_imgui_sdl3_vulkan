#include "image_downloader.hpp"

#include <curl/curl.h>

#include <string_view>
#include <unistd.h>

// ============================================================================
// File-local helpers
// ============================================================================

namespace {

struct CurlBuf {
    std::vector<uint8_t> data;
};

static size_t curl_write_cb(void *ptr, size_t size, size_t nmemb, void *user) { // NOLINT
    auto *buf = static_cast<CurlBuf *>(user);
    const auto *bytes = static_cast<const uint8_t *>(ptr);
    buf->data.insert(buf->data.end(), bytes, bytes + size * nmemb);
    return size * nmemb;
}

static std::string ext_from_url(const std::string &url) { // NOLINT
    const std::string clean = url.substr(0, url.find('?'));
    std::string ext = std::filesystem::path(clean).extension().string();

    constexpr std::array<std::string_view, 7> valid{
        ".jpg", ".jpeg", ".png", ".bmp", ".tga", ".gif", ".webp"};

    for (auto v : valid)
        if (ext == v)
            return ext;

    return ".jpg";
}

} // namespace

// ============================================================================
// ImageDownloader
// ============================================================================

std::filesystem::path ImageDownloader::download_to_temp(const std::string &url) {
    CurlBuf buf;
    CURL *curl = curl_easy_init();
    if (!curl)
        return {};

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buf);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);

    const CURLcode res = curl_easy_perform(curl);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK || buf.data.empty())
        return {};

    char tmp_tpl[] = "/tmp/imgview_XXXXXX";
    const int fd = mkstemp(tmp_tpl);
    if (fd < 0)
        return {};
    close(fd);

    std::filesystem::path final_path = std::string(tmp_tpl) + ext_from_url(url);

    std::error_code ec;
    std::filesystem::rename(tmp_tpl, final_path, ec);
    if (ec)
        return {};

    std::ofstream ofs(final_path, std::ios::binary);
    if (!ofs) {
        std::filesystem::remove(final_path);
        return {};
    }
    ofs.write(std::bit_cast<const char *>(buf.data.data()),
              static_cast<std::streamsize>(buf.data.size()));

    return final_path;
}

std::string ImageDownloader::title_from_url(const std::string &url) {
    if (const auto qpos = url.find('?'); qpos != std::string::npos) {
        const std::string_view query(url.c_str() + qpos + 1);

        auto hex_nibble = [](char c) -> int {
            if (c >= '0' && c <= '9')
                return c - '0';
            if (c >= 'a' && c <= 'f')
                return c - 'a' + 10;
            if (c >= 'A' && c <= 'F')
                return c - 'A' + 10;
            return -1;
        };

        auto url_decode = [&](std::string_view src) {
            std::string out;
            out.reserve(src.size());
            for (size_t i = 0; i < src.size(); ++i) {
                const char ch = src[i];
                if (ch == '+') {
                    out.push_back(' ');
                    continue;
                }
                if (ch == '%' && i + 2 < src.size()) {
                    const int hi = hex_nibble(src[i + 1]);
                    const int lo = hex_nibble(src[i + 2]);
                    if (hi >= 0 && lo >= 0) {
                        out.push_back(static_cast<char>((hi << 4) | lo));
                        i += 2;
                        continue;
                    }
                }
                out.push_back(ch);
            }
            return out;
        };

        size_t pos = 0;
        while (pos <= query.size()) {
            const size_t amp = query.find('&', pos);
            const std::string_view token = query.substr(
                pos, amp == std::string_view::npos ? std::string_view::npos : amp - pos);

            const size_t eq = token.find('=');
            if (eq != std::string_view::npos) {
                const std::string_view key = token.substr(0, eq);
                std::string_view value = token.substr(eq + 1);
                if (key == "filename" || key == "download_filename" ||
                    key.ends_with("filename")) {
                    while (!value.empty() && value.front() == ' ')
                        value.remove_prefix(1);
                    const std::string decoded = url_decode(value);
                    const std::string fname = std::filesystem::path(decoded).filename().string();
                    if (!fname.empty())
                        return fname;
                }
            }

            if (amp == std::string_view::npos)
                break;
            pos = amp + 1;
        }
    }

    const std::string clean = url.substr(0, url.find('?'));
    auto t = std::filesystem::path(clean).filename().string();
    return t.empty() ? "online_image" : t;
}
