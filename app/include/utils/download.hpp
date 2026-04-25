#pragma once

#include <borealis/core/singleton.hpp>
#include <borealis/core/event.hpp>
#include <nlohmann/json.hpp>
#include <atomic>
#include <mutex>
#include <string>
#include <vector>

enum class DownloadStatus { Queued, Downloading, Completed, Failed };
enum class DownloadQuality { Max, Q8M, Q4M, Q2M, Q1M, Q500K, Q250K };

NLOHMANN_JSON_SERIALIZE_ENUM(DownloadStatus, {
    {DownloadStatus::Queued, "Queued"},
    {DownloadStatus::Downloading, "Downloading"},
    {DownloadStatus::Completed, "Completed"},
    {DownloadStatus::Failed, "Failed"},
})

NLOHMANN_JSON_SERIALIZE_ENUM(DownloadQuality, {
    {DownloadQuality::Max, "Max"},
    {DownloadQuality::Q8M, "8 Mb/s"},
    {DownloadQuality::Q4M, "4 Mb/s"},
    {DownloadQuality::Q2M, "2 Mb/s"},
    {DownloadQuality::Q1M, "1 Mb/s"},
    {DownloadQuality::Q500K, "500 Kb/s"},
    {DownloadQuality::Q250K, "250 Kb/s"},
    {DownloadQuality::Max, "Original"},
    {DownloadQuality::Q4M, "1080p"},
    {DownloadQuality::Q2M, "720p"},
    {DownloadQuality::Q1M, "480p"},
})

inline int64_t downloadBitrate(DownloadQuality q) {
    switch (q) {
    case DownloadQuality::Max:  return 0;
    case DownloadQuality::Q8M:  return 8000000;
    case DownloadQuality::Q4M:  return 4000000;
    case DownloadQuality::Q2M:  return 2000000;
    case DownloadQuality::Q1M:  return 1000000;
    case DownloadQuality::Q500K: return 500000;
    case DownloadQuality::Q250K: return 250000;
    }
    return 0;
}

struct DownloadItem {
    std::string itemId;
    std::string name;
    std::string type;
    std::string seriesName;
    int seasonIndex = 0;
    int episodeIndex = 0;
    long productionYear = 0;
    uint64_t runTimeTicks = 0;
    std::string imagePrimaryTag;
    DownloadQuality quality = DownloadQuality::Max;
    DownloadStatus status = DownloadStatus::Queued;
    std::string filePath;
    int64_t totalBytes = 0;
    int64_t downloadedBytes = 0;
    std::string errorMessage;
    int64_t playbackPositionTicks = 0;
    float playedPercentage = 0;
    bool played = false;
    bool needsSync = false;
    std::string serverId;
    std::string userId;
    std::string lastPlayedAt;
    std::string seriesId;
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(DownloadItem, itemId, name, type, seriesName,
    seasonIndex, episodeIndex, productionYear, runTimeTicks, imagePrimaryTag, quality, status,
    filePath, totalBytes, downloadedBytes, errorMessage,
    playbackPositionTicks, playedPercentage, played, needsSync, serverId, userId,
    lastPlayedAt, seriesId);

namespace jellyfin {
struct Item;
struct Episode;
}

class DownloadManager : public brls::Singleton<DownloadManager> {
public:
    using ProgressEvent = brls::Event<std::string, int64_t, int64_t>;
    using StatusEvent = brls::Event<std::string, DownloadStatus>;

    void init();

    void addDownload(const jellyfin::Item& item, DownloadQuality quality);
    void addDownload(const jellyfin::Episode& item, DownloadQuality quality);
    void cancelDownload(const std::string& itemId);
    void removeDownload(const std::string& itemId);
    void resumeQueue();
    void updatePlaybackState(const std::string& itemId, int64_t positionTicks, bool markPlayed = false);
    void syncPlaybackStates();
    void autoQueueNextEpisodes(const std::string& seriesId, const std::string& seriesName);

    bool isDownloaded(const std::string& itemId) const;
    bool isDownloading(const std::string& itemId) const;
    std::string getLocalPath(const std::string& itemId) const;

    std::vector<DownloadItem> getItems() const;

    ProgressEvent* getProgressEvent() { return &progressEvent; }
    StatusEvent* getStatusEvent() { return &statusEvent; }

private:
    void saveIndex();
    void loadIndex();
    void processQueue();
    void doDownload(DownloadItem& item);
    std::string downloadDir() const;

    mutable std::mutex mutex;
    std::vector<DownloadItem> items;
    std::shared_ptr<std::atomic_bool> currentCancel;
    bool downloading = false;

    ProgressEvent progressEvent;
    StatusEvent statusEvent;
};
