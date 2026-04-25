#include "utils/download.hpp"
#include "utils/config.hpp"
#include "api/http.hpp"
#include "api/jellyfin/media.hpp"

#include <borealis/core/logger.hpp>
#include <borealis/core/thread.hpp>
#include <chrono>
#include <fstream>

#ifdef BOREALIS_USE_STD_THREAD
#include <thread>
#else
#include <pthread.h>
#endif

#ifdef USE_BOOST_FILESYSTEM
#include <boost/filesystem.hpp>
namespace fs = boost::filesystem;
#elif __has_include(<filesystem>)
#include <filesystem>
namespace fs = std::filesystem;
#endif

namespace {

void runDetached(std::function<void()> fn) {
#ifdef BOREALIS_USE_STD_THREAD
    std::thread(std::move(fn)).detach();
#else
    auto* task = new std::function<void()>(std::move(fn));
    pthread_t th;
    pthread_create(&th, nullptr, [](void* arg) -> void* {
        auto* f = static_cast<std::function<void()>*>(arg);
        (*f)();
        delete f;
        return nullptr;
    }, task);
    pthread_detach(th);
#endif
}

nlohmann::json buildDownloadProfile(int64_t maxBitrate) {
    return {
        {"MaxStreamingBitrate", maxBitrate},
        {"MaxStaticBitrate", maxBitrate},
        {"DirectPlayProfiles", nlohmann::json::array({
            {{"Container", "mp4,mkv,avi,mov,ts,webm"},
             {"Type", "Video"},
             {"VideoCodec", "h264,hevc,mpeg4,vp8,vp9,av1"}}
        })},
        {"TranscodingProfiles", nlohmann::json::array({
            {{"Container", "ts"},
             {"Type", "Video"},
             {"VideoCodec", "h264"},
             {"AudioCodec", "aac,mp3,ac3"},
             {"Context", "Streaming"},
             {"Protocol", "hls"},
             {"CopyTimestamps", false},
             {"EnableSubtitlesInManifest", true}}
        })},
        {"SubtitleProfiles", nlohmann::json::array({
            {{"Format", "srt"}, {"Method", "Embed"}},
            {{"Format", "ass"}, {"Method", "Embed"}},
            {{"Format", "ssa"}, {"Method", "Embed"}},
            {{"Format", "vtt"}, {"Method", "Embed"}},
            {{"Format", "sub"}, {"Method", "Embed"}},
            {{"Format", "subrip"}, {"Method", "Embed"}},
            {{"Format", "pgssub"}, {"Method", "Encode"}},
            {{"Format", "dvdsub"}, {"Method", "Encode"}},
            {{"Format", "pgs"}, {"Method", "Encode"}}
        })}
    };
}

}  // namespace

std::string DownloadManager::downloadDir() const {
    return AppConfig::instance().configDir() + "/downloads";
}

void DownloadManager::init() {
    auto dir = this->downloadDir();
    if (!fs::exists(dir)) {
        fs::create_directories(dir);
    }
    this->loadIndex();

    std::lock_guard<std::mutex> lock(this->mutex);
    for (auto& item : this->items) {
        if (item.status == DownloadStatus::Downloading) {
            item.status = DownloadStatus::Queued;
        }
    }
    this->saveIndex();
}

void DownloadManager::loadIndex() {
    std::string path = this->downloadDir() + "/index.json";
    if (!fs::exists(path)) return;

    try {
        std::ifstream f(path);
        nlohmann::json j = nlohmann::json::parse(f);
        this->items = j.get<std::vector<DownloadItem>>();
    } catch (const std::exception& e) {
        brls::Logger::error("Failed to load download index: {}", e.what());
    }
}

void DownloadManager::saveIndex() {
    std::string path = this->downloadDir() + "/index.json";
    try {
        nlohmann::json j = this->items;
        std::ofstream f(path);
        f << j.dump(2);
    } catch (const std::exception& e) {
        brls::Logger::error("Failed to save download index: {}", e.what());
    }
}

void DownloadManager::addDownload(const jellyfin::Item& item, DownloadQuality quality) {
    std::lock_guard<std::mutex> lock(this->mutex);

    for (auto& existing : this->items) {
        if (existing.itemId == item.Id) {
            brls::Logger::info("Download already exists: {}", item.Name);
            return;
        }
    }

    DownloadItem dl;
    dl.itemId = item.Id;
    dl.name = item.Name;
    dl.type = item.Type;
    dl.productionYear = item.ProductionYear;
    dl.runTimeTicks = item.RunTimeTicks;
    dl.quality = quality;
    dl.status = DownloadStatus::Queued;
    dl.serverId = AppConfig::instance().getServerId();
    dl.userId = AppConfig::instance().getUserId();

    auto primaryTag = item.ImageTags.find(jellyfin::imageTypePrimary);
    if (primaryTag != item.ImageTags.end()) {
        dl.imagePrimaryTag = primaryTag->second;
    }

    this->items.push_back(dl);
    this->saveIndex();
    brls::Logger::info("Download queued: {}", item.Name);
    this->processQueue();
}

void DownloadManager::addDownload(const jellyfin::Episode& item, DownloadQuality quality) {
    std::lock_guard<std::mutex> lock(this->mutex);

    for (auto& existing : this->items) {
        if (existing.itemId == item.Id) {
            brls::Logger::info("Download already exists: {}", item.Name);
            return;
        }
    }

    DownloadItem dl;
    dl.itemId = item.Id;
    dl.name = item.Name;
    dl.type = item.Type;
    dl.productionYear = item.ProductionYear;
    dl.runTimeTicks = item.RunTimeTicks;
    dl.quality = quality;
    dl.status = DownloadStatus::Queued;
    dl.serverId = AppConfig::instance().getServerId();
    dl.userId = AppConfig::instance().getUserId();
    dl.seriesName = item.SeriesName;
    dl.seasonIndex = item.ParentIndexNumber;
    dl.episodeIndex = item.IndexNumber;

    auto primaryTag = item.ImageTags.find(jellyfin::imageTypePrimary);
    if (primaryTag != item.ImageTags.end()) {
        dl.imagePrimaryTag = primaryTag->second;
    }

    this->items.push_back(dl);
    this->saveIndex();
    brls::Logger::info("Download queued: {}", item.Name);
    this->processQueue();
}

void DownloadManager::resumeQueue() {
    std::lock_guard<std::mutex> lock(this->mutex);
    this->processQueue();
}

void DownloadManager::updatePlaybackState(const std::string& itemId, int64_t positionTicks, bool markPlayed) {
    {
        std::lock_guard<std::mutex> lock(this->mutex);
        for (auto& item : this->items) {
            if (item.itemId == itemId) {
                item.playbackPositionTicks = positionTicks;
                item.playedPercentage = item.runTimeTicks > 0
                    ? std::min(100.0f, static_cast<float>(positionTicks * 100.0 / item.runTimeTicks)) : 0;
                if (markPlayed || item.playedPercentage >= 90.0f) item.played = true;
                item.needsSync = true;
                this->saveIndex();
                break;
            }
        }
    }
}

void DownloadManager::syncPlaybackStates() {
    std::vector<DownloadItem> pending;
    {
        std::lock_guard<std::mutex> lock(this->mutex);
        for (auto& item : this->items) {
            if (item.needsSync && !item.serverId.empty() && !item.userId.empty()) {
                pending.push_back(item);
            }
        }
    }
    if (pending.empty()) return;

    auto& conf = AppConfig::instance();
    std::vector<DownloadItem> synced;

    for (auto& item : pending) {
        std::string serverUrl;
        std::string token;

        for (auto& s : conf.getServers()) {
            if (s.id == item.serverId && !s.urls.empty()) {
                serverUrl = s.urls.front();
                break;
            }
        }
        if (serverUrl.empty()) continue;

        for (auto& u : conf.getUsers(item.serverId)) {
            if (u.id == item.userId) {
                token = u.access_token;
                break;
            }
        }
        if (token.empty()) continue;

        HTTP::Header header = {"Content-Type: application/json", conf.getAuth(token)};

        try {
            if (item.played) {
                std::string url = serverUrl +
                    fmt::format(fmt::runtime(jellyfin::apiPlayedItems), item.userId, item.itemId);
                HTTP::post(url, "{}", header, HTTP::Timeout{});
            }
            if (item.playbackPositionTicks > 0) {
                nlohmann::json payload = {
                    {"ItemId", item.itemId},
                    {"PlayMethod", jellyfin::methodDirectPlay},
                    {"PositionTicks", item.playbackPositionTicks},
                };
                HTTP::post(serverUrl + std::string(jellyfin::apiPlayStop),
                    payload.dump(), header, HTTP::Timeout{});
            }
            synced.push_back(item);
        } catch (const std::exception& e) {
            brls::Logger::warning("Sync failed for {}: {}", item.name, e.what());
        }
    }

    if (!synced.empty()) {
        std::lock_guard<std::mutex> lock(this->mutex);
        for (auto& item : this->items) {
            for (auto& p : synced) {
                if (item.itemId == p.itemId &&
                    item.playbackPositionTicks == p.playbackPositionTicks &&
                    item.played == p.played) {
                    item.needsSync = false;
                    break;
                }
            }
        }
        this->saveIndex();
    }
}

void DownloadManager::cancelDownload(const std::string& itemId) {
    bool erased = false;
    {
        std::lock_guard<std::mutex> lock(this->mutex);

        for (auto& item : this->items) {
            if (item.itemId == itemId && item.status == DownloadStatus::Downloading && this->currentCancel) {
                this->currentCancel->store(true);
                return;
            }
        }

        for (auto it = this->items.begin(); it != this->items.end(); ++it) {
            if (it->itemId == itemId && it->status == DownloadStatus::Queued) {
                this->items.erase(it);
                this->saveIndex();
                erased = true;
                break;
            }
        }
    }

    if (erased) {
        brls::sync([this, itemId]() {
            this->statusEvent.fire(itemId, DownloadStatus::Failed);
        });
    }
}

void DownloadManager::removeDownload(const std::string& itemId) {
    bool wasActive = false;
    bool erased = false;
    {
        std::lock_guard<std::mutex> lock(this->mutex);

        for (auto& item : this->items) {
            if (item.itemId == itemId && item.status == DownloadStatus::Downloading && this->currentCancel) {
                this->currentCancel->store(true);
                item.errorMessage = "removed";
                wasActive = true;
                break;
            }
        }

        if (!wasActive) {
            for (auto it = this->items.begin(); it != this->items.end(); ++it) {
                if (it->itemId == itemId) {
                    this->items.erase(it);
                    erased = true;
                    break;
                }
            }
            this->saveIndex();
        }
    }

    if (!wasActive) {
        std::string dir = this->downloadDir() + "/" + itemId;
        brls::async([dir]() {
            try {
                if (fs::exists(dir)) fs::remove_all(dir);
            } catch (const std::exception& e) {
                brls::Logger::error("Failed to remove download dir: {}", e.what());
            }
        });
    }

    if (erased) {
        brls::sync([this, itemId]() {
            this->statusEvent.fire(itemId, DownloadStatus::Failed);
        });
    }
}

bool DownloadManager::isDownloaded(const std::string& itemId) const {
    std::lock_guard<std::mutex> lock(this->mutex);
    for (auto& item : this->items) {
        if (item.itemId == itemId && item.status == DownloadStatus::Completed) return true;
    }
    return false;
}

bool DownloadManager::isDownloading(const std::string& itemId) const {
    std::lock_guard<std::mutex> lock(this->mutex);
    for (auto& item : this->items) {
        if (item.itemId == itemId &&
            (item.status == DownloadStatus::Downloading || item.status == DownloadStatus::Queued))
            return true;
    }
    return false;
}

std::string DownloadManager::getLocalPath(const std::string& itemId) const {
    std::lock_guard<std::mutex> lock(this->mutex);
    for (auto& item : this->items) {
        if (item.itemId == itemId && item.status == DownloadStatus::Completed) {
            return this->downloadDir() + "/" + itemId + "/" + item.filePath;
        }
    }
    return "";
}

std::vector<DownloadItem> DownloadManager::getItems() const {
    std::lock_guard<std::mutex> lock(this->mutex);
    return this->items;
}

// Must be called with mutex held
void DownloadManager::processQueue() {
    if (this->downloading) return;

    for (auto& item : this->items) {
        if (item.status == DownloadStatus::Queued) {
            this->downloading = true;
            this->doDownload(item);
            return;
        }
    }
}

// Must be called with mutex held. Copies what it needs, then releases via async.
void DownloadManager::doDownload(DownloadItem& item) {
    item.status = DownloadStatus::Downloading;

    std::string itemId = item.itemId;
    std::string imagePrimaryTag = item.imagePrimaryTag;
    DownloadQuality quality = item.quality;
    uint64_t runTimeTicks = item.runTimeTicks;
    int64_t bitrate = downloadBitrate(quality);
    std::string itemDir = this->downloadDir() + "/" + itemId;

    item.filePath = "video.mp4";
    this->saveIndex();

    auto cancel = std::make_shared<std::atomic_bool>(false);
    this->currentCancel = cancel;

    brls::sync([this, itemId]() {
        this->statusEvent.fire(itemId, DownloadStatus::Downloading);
    });

    runDetached([this, itemId, imagePrimaryTag, quality, bitrate, runTimeTicks, itemDir, cancel]() {
        auto resetQueue = [this, itemId](const std::string& error) {
            brls::sync([this, itemId, error]() {
                {
                    std::lock_guard<std::mutex> lock(this->mutex);
                    for (auto& item : this->items) {
                        if (item.itemId == itemId) {
                            item.status = DownloadStatus::Failed;
                            item.errorMessage = error;
                            break;
                        }
                    }
                    this->downloading = false;
                    this->currentCancel.reset();
                    this->saveIndex();
                }
                this->statusEvent.fire(itemId, DownloadStatus::Failed);
                {
                    std::lock_guard<std::mutex> lock(this->mutex);
                    this->processQueue();
                }
            });
        };

        try {
            if (!fs::exists(itemDir)) fs::create_directories(itemDir);
        } catch (const std::exception& e) {
            brls::Logger::error("Failed to create download dir: {}", e.what());
            resetQueue(e.what());
            return;
        }

        auto& conf = AppConfig::instance();
        std::string server = conf.getUrl();
        std::string token = conf.getToken();
        HTTP::Header header = {conf.getAuth(token)};

        if (cancel->load()) {
            resetQueue("Cancelled");
            return;
        }

        std::string url;
        std::string ext;

        if (quality == DownloadQuality::Max) {
            ext = "mp4";
            try {
                auto resp = HTTP::get(server +
                    fmt::format(fmt::runtime(jellyfin::apiUserItem),
                        conf.getUserId(), itemId),
                    header, HTTP::Timeout{});
                if (!resp.empty()) {
                    auto detail = nlohmann::json::parse(resp).get<jellyfin::Detail>();
                    if (!detail.MediaSources.empty()) {
                        auto& path = detail.MediaSources[0].Path;
                        auto dot = path.find_last_of('.');
                        if (dot != std::string::npos) {
                            ext = path.substr(dot + 1);
                            std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
                        }
                    }
                }
            } catch (const std::exception& e) {
                brls::Logger::warning("Failed to fetch item detail for extension: {}", e.what());
            }
            url = server + fmt::format(fmt::runtime(jellyfin::apiDownload), itemId,
                HTTP::encode_form({{"api_key", token}}));
        } else {
            ext = "ts";
            try {
                nlohmann::json body = {
                    {"UserId", conf.getUserId()},
                    {"MediaSourceId", itemId},
                    {"AllowAudioStreamCopy", true},
                    {"AllowVideoStreamCopy", true},
                    {"DeviceProfile", buildDownloadProfile(bitrate)},
                };

                HTTP h;
                HTTP::Header postHeader = {"Content-Type: application/json", conf.getAuth(token)};
                HTTP::set_option(h, postHeader, HTTP::Timeout{});
                auto resp = h._post(
                    server + fmt::format(fmt::runtime(jellyfin::apiPlayback), itemId),
                    body.dump());
                auto result = nlohmann::json::parse(resp).get<jellyfin::PlaybackResult>();

                if (!result.MediaSources.empty() && !result.MediaSources[0].TranscodingUrl.empty()) {
                    std::string tUrl = result.MediaSources[0].TranscodingUrl;
                    auto pos = tUrl.find("master.m3u8");
                    if (pos != std::string::npos) tUrl.replace(pos, 11, "stream");
                    url = server + tUrl;
                } else {
                    url = server + fmt::format(fmt::runtime(jellyfin::apiDownload), itemId,
                        HTTP::encode_form({{"api_key", token}}));
                    ext = "mp4";
                }
            } catch (const std::exception& e) {
                brls::Logger::error("PlaybackInfo failed: {} - {}", itemId, e.what());
                resetQueue(std::string("PlaybackInfo failed: ") + e.what());
                return;
            }
        }

        if (cancel->load()) {
            resetQueue("Cancelled");
            return;
        }

        std::string fileName = "video." + ext;
        std::string filePath = itemDir + "/" + fileName;

        {
            std::lock_guard<std::mutex> lock(this->mutex);
            for (auto& it : this->items) {
                if (it.itemId == itemId) {
                    it.filePath = fileName;
                    break;
                }
            }
            this->saveIndex();
        }

        if (!imagePrimaryTag.empty() && !cancel->load()) {
            try {
                std::string thumbUrl = conf.getUrl() +
                    fmt::format("/Items/{}/Images/Primary?format=Png&{}",
                        itemId, HTTP::encode_form({{"tag", imagePrimaryTag}, {"maxWidth", "300"}}));
                HTTP::download(thumbUrl, itemDir + "/thumb.png", header, HTTP::Timeout{});
            } catch (const std::exception& e) {
                brls::Logger::warning("Failed to download thumbnail: {}", e.what());
            }
        }

        int64_t estimatedTotal = 0;
        if (bitrate > 0 && runTimeTicks > 0) {
            double durationSec = runTimeTicks / 10000000.0;
            estimatedTotal = static_cast<int64_t>((bitrate * durationSec / 8) * 1.1);
        }

        auto lastProgress = std::make_shared<std::chrono::steady_clock::time_point>();
        HTTP::Progress::Callback progressCb = [this, itemId, estimatedTotal, lastProgress](curl_off_t total, curl_off_t now) {
            auto tp = std::chrono::steady_clock::now();
            if (tp - *lastProgress < std::chrono::milliseconds(500)) return;
            *lastProgress = tp;

            int64_t reportTotal = total > 0 ? total : estimatedTotal;
            brls::sync([this, itemId, reportTotal, now]() {
                {
                    std::lock_guard<std::mutex> lock(this->mutex);
                    for (auto& item : this->items) {
                        if (item.itemId == itemId) {
                            item.totalBytes = reportTotal;
                            item.downloadedBytes = now;
                            break;
                        }
                    }
                }
                this->progressEvent.fire(itemId, now, reportTotal);
            });
        };

        bool cancelled = false;
        bool success = false;
        std::string error;

        try {
            std::ofstream of(filePath, std::ios::binary);
            if (!of) throw std::runtime_error("Failed to open file for writing");

            HTTP s;
            HTTP::set_option(s, header, cancel, progressCb);
            s._get(url, &of);
            of.close();

            cancelled = cancel->load();
            if (!cancelled) success = true;
        } catch (const std::exception& ex) {
            error = ex.what();
            brls::Logger::error("Download failed: {} - {}", itemId, error);
        }

        brls::sync([this, itemId, fileName, cancelled, success, error]() {
            DownloadStatus finalStatus = DownloadStatus::Failed;

            {
                std::lock_guard<std::mutex> lock(this->mutex);

                if (cancelled) {
                    bool removed = false;
                    for (auto it = this->items.begin(); it != this->items.end(); ++it) {
                        if (it->itemId == itemId) {
                            if (it->errorMessage == "removed") {
                                this->items.erase(it);
                                removed = true;
                            } else {
                                it->status = DownloadStatus::Failed;
                                it->errorMessage = "Cancelled";
                            }
                            break;
                        }
                    }
                    this->saveIndex();
                    if (removed) {
                        std::string dir = this->downloadDir() + "/" + itemId;
                        brls::async([dir]() {
                            try {
                                if (fs::exists(dir)) fs::remove_all(dir);
                            } catch (const std::exception& e) {
                                brls::Logger::error("Failed to remove download dir: {}", e.what());
                            }
                        });
                    }
                } else if (success) {
                    finalStatus = DownloadStatus::Completed;
                    for (auto& item : this->items) {
                        if (item.itemId == itemId) {
                            item.status = DownloadStatus::Completed;
                            item.filePath = fileName;

                            std::string metaPath = this->downloadDir() + "/" + itemId + "/metadata.json";
                            try {
                                nlohmann::json j = item;
                                std::ofstream f(metaPath);
                                f << j.dump(2);
                            } catch (...) {}
                            break;
                        }
                    }
                    this->saveIndex();
                    brls::Logger::info("Download completed: {}", itemId);
                } else {
                    for (auto& item : this->items) {
                        if (item.itemId == itemId) {
                            item.status = DownloadStatus::Failed;
                            item.errorMessage = error;
                            break;
                        }
                    }
                    this->saveIndex();
                }

                this->downloading = false;
                this->currentCancel.reset();
            }

            this->statusEvent.fire(itemId, finalStatus);
            {
                std::lock_guard<std::mutex> lock(this->mutex);
                this->processQueue();
            }
        });
    });
}
