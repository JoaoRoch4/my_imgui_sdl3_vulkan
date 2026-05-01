#include "history_preview.hpp"
#include "video_player.hpp"
#include "Image_viewer_panel.hpp"

#include "imgui.h"

#include <curl/curl.h>

#include <algorithm>
#include <array>
#include <fstream>
#include <string_view>
#include <unistd.h>
#include <utility>
#include <vector>
#include <mutex>
#include <condition_variable>
#include <filesystem>

using std::jthread;

ImVec2 g_history_preview_max_size = ImVec2(1000.0f, 1000.0f);

namespace {

struct CurlBuf {
    std::vector<uint8_t> data;
};

static size_t curl_write_cb(void *ptr, size_t size, size_t nmemb, void *user)
{
    auto *buf = static_cast<CurlBuf *>(user);
    const auto *bytes = static_cast<const uint8_t *>(ptr);
    buf->data.insert(buf->data.end(), bytes, bytes + size * nmemb);
    return size * nmemb;
}

static std::string ext_from_url(const std::string &url)
{
    const std::string clean = url.substr(0, url.find('?'));
    std::string ext = std::filesystem::path(clean).extension().string();

    constexpr std::array<std::string_view, 7> valid{
        ".jpg", ".jpeg", ".png", ".bmp", ".tga", ".gif", ".webp"};

    for (auto v : valid)
        if (ext == v)
            return ext;

    return ".jpg";
}

static std::filesystem::path download_to_temp(const std::string &url)
{
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

    char tmp_tpl[] = "/tmp/imgpreview_XXXXXX";
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

    ofs.write(static_cast<const char *>(static_cast<const void *>(buf.data.data())),
              static_cast<std::streamsize>(buf.data.size()));

    return final_path;
}

} // namespace

HistoryPreview::HistoryPreview()
    : m_vk{nullptr}
    , m_video_player{nullptr}
    , m_viewer{nullptr}
    , m_worker{}
    , m_mutex{}
    , m_cv{}
    , m_has_pending_job{false}
    , m_pending_job{}
    , m_has_ready_result{false}
    , m_ready_result{}
    , m_active_texture{}
    , m_active_source{}
    , m_active_temp_path{}
{
}

void HistoryPreview::setup(vulkan_context *vk, VideoPlayer *vp, ImageViewerPanel *viewer)
{

    using JthreadWorker = std::jthread; /// Avoid including <thread> in the header just for this.
    shutdown();

    m_vk           = vk;
    m_video_player = vp;
    m_viewer       = viewer;
    m_has_pending_job = false;
    m_has_ready_result = false;
    m_pending_job = Job{};
    m_ready_result = JobResult{};
    m_worker = JthreadWorker{&HistoryPreview::worker_loop, this}; // NOLINT
}

void HistoryPreview::shutdown()
{
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_has_pending_job = false;
    }
    m_worker.request_stop();
    m_cv.notify_all();

    if (m_worker.joinable()) {
        m_worker.join();
    }

    clear_active_preview();

    if (m_has_ready_result && m_ready_result.is_temp_file && !m_ready_result.path.empty()) {
        std::error_code ec;
        std::filesystem::remove(m_ready_result.path, ec);
    }

    m_has_ready_result = false;
    m_ready_result = JobResult{};
    m_vk = nullptr;
}

void HistoryPreview::request_preview(const WindowStateToml::ImageHistoryEntry &hentry)
{
    std::lock_guard<std::mutex> lock(m_mutex);

    if (m_has_pending_job &&
        m_pending_job.source == hentry.source &&
        m_pending_job.kind == hentry.kind)
        return;

    m_pending_job = Job{hentry.source, hentry.kind};
    m_has_pending_job = true;
    m_cv.notify_one();
}

void HistoryPreview::clear_active_preview()
{
    if (m_vk && m_active_texture.is_loaded()) {
        vkDeviceWaitIdle(m_vk->device);
        m_active_texture.unload(*m_vk);
    }

    if (!m_active_temp_path.empty()) {
        std::error_code ec;
        std::filesystem::remove(m_active_temp_path, ec);
        m_active_temp_path.clear();
    }

    m_active_source.clear();
}

void HistoryPreview::apply_ready_result(const WindowStateToml::ImageHistoryEntry &hentry)
{
    JobResult result;
    bool has_result = false;

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_has_ready_result && m_ready_result.source == hentry.source) {
            result = std::move(m_ready_result);
            m_ready_result = JobResult{};
            m_has_ready_result = false;
            has_result = true;
        }
    }

    if (!has_result)
        return;

    clear_active_preview();

    if (result.ok && m_vk && m_active_texture.load(result.path, *m_vk)) {
        m_active_source = result.source;
        if (result.is_temp_file)
            m_active_temp_path = result.path;
    } else if (result.is_temp_file && !result.path.empty()) {
        std::error_code ec;
        std::filesystem::remove(result.path, ec);
    }
}

void HistoryPreview::draw_for_hover(const WindowStateToml::ImageHistoryEntry &hentry)
{
    if (!m_vk)
        return;

    // Detect video entries and use VideoPlayer's hover thumbnail
    const bool is_video = m_video_player != nullptr &&
        (hentry.kind == "file"
            ? VideoPlayer::is_video_path(std::filesystem::path(hentry.source))
            : VideoPlayer::is_video_url(hentry.source));

    const ImVec2 mouse = ImGui::GetMousePos();
    ImGui::SetNextWindowPos(ImVec2(mouse.x + 24.0f, mouse.y + 24.0f), ImGuiCond_Always);
    ImGui::BeginTooltip();
    ImGui::Text("%s", hentry.source.c_str());

    if (is_video) {
        // Try open playback frame first (live frame from running video),
        // fall back to hover-mpv thumbnail for history entries.
        VkDescriptorSet ds = m_video_player->get_open_thumbnail(hentry.source);
        if (ds == VK_NULL_HANDLE)
            ds = m_video_player->hover_thumbnail(hentry.source);

        if (ds != VK_NULL_HANDLE)
            ImGui::Image(std::bit_cast<ImTextureID>(ds), VideoHoverPreview::preview_size);
        else
            ImGui::TextDisabled("Loading preview...");
    } else {
        // For image URLs: check if already open in the viewer first
        if (hentry.kind == "url" && m_viewer) {
            const ImTextureID existing = m_viewer->get_imgui_id_for_source(hentry.source);
            if (existing) {
                ImGui::Image(existing, g_history_preview_max_size);
                ImGui::TextDisabled("%s", hentry.opened_at.c_str());
                ImGui::EndTooltip();
                return;
            }
        }

        if (m_active_source != hentry.source)
            request_preview(hentry);
        apply_ready_result(hentry);

        if (m_active_source == hentry.source && m_active_texture.is_loaded()) {
            const float src_w = static_cast<float>(m_active_texture.width);
            const float src_h = static_cast<float>(m_active_texture.height);
            const float max_w = g_history_preview_max_size.x;
            const float max_h = g_history_preview_max_size.y;
            const float scale = std::min(max_w / src_w, max_h / src_h);
            const float draw_w = src_w * std::min(scale, 1.0f);
            const float draw_h = src_h * std::min(scale, 1.0f);
            ImGui::Image(m_active_texture.imgui_id(), ImVec2(draw_w, draw_h));
        } else {
            ImGui::TextDisabled("Loading preview...");
        }
    }

    ImGui::TextDisabled("%s", hentry.opened_at.c_str());
    ImGui::EndTooltip();
}

void HistoryPreview::worker_loop(std::stop_token stoken)
{
    for (;;) {
        Job job;
        {
            std::unique_lock<std::mutex> lock(m_mutex);
            m_cv.wait(lock, [this, &stoken] {
                return stoken.stop_requested() || m_has_pending_job;
            });

            if (stoken.stop_requested())
                break;

            job = std::move(m_pending_job);
            m_pending_job = Job{};
            m_has_pending_job = false;
        }

        JobResult result;
        result.source = job.source;
        result.path.clear();
        result.is_temp_file = false;
        result.ok = false;

        if (job.kind == "file") {
            const std::filesystem::path path(job.source);
            if (std::filesystem::exists(path)) {
                result.path = path;
                result.ok = true;
            }
        } else if (job.kind == "url") {
            result.path = download_to_temp(job.source);
            result.is_temp_file = !result.path.empty();
            result.ok = result.is_temp_file;
        }

        std::lock_guard<std::mutex> lock(m_mutex);

        if (stoken.stop_requested()) {
            if (result.is_temp_file && !result.path.empty()) {
                std::error_code ec;
                std::filesystem::remove(result.path, ec);
            }
            break;
        }

        if (m_has_ready_result && m_ready_result.is_temp_file && !m_ready_result.path.empty()) {
            std::error_code ec;
            std::filesystem::remove(m_ready_result.path, ec);
        }

        m_ready_result = std::move(result);
        m_has_ready_result = true;
    }
}
