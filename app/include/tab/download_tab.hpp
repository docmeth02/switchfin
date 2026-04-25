#pragma once

#include <view/auto_tab_frame.hpp>
#include "utils/download.hpp"

class RecyclingGrid;

class DownloadView : public AttachedView {
public:
    DownloadView();
    ~DownloadView() override;

    brls::View* getDefaultFocus() override;
    void dismiss(std::function<void(void)> cb = [] {}) override;

    void pushEpisodeList(std::vector<DownloadItem> items, const std::string& seriesName, int seasonIndex = -1);
    void pushSeasonList(std::vector<struct DownloadGroup> groups, const std::string& seriesName);

private:
    struct StackEntry {
        RecyclingGrid* grid;
        std::string seriesName;
        int seasonIndex = -1;
    };

    void loadItems();
    void reloadCurrentView();
    void updateProgress(const std::string& itemId, int64_t downloaded, int64_t total);
    RecyclingGrid* newRecycler();
    RecyclingGrid* newGroupRecycler();
    void setContent(RecyclingGrid* view);

    std::vector<StackEntry> stack;
    RecyclingGrid* recycler = nullptr;
    bool reloading = false;

    DownloadManager::StatusEvent::Subscription statusSubId;
    DownloadManager::ProgressEvent::Subscription progressSubId;
};
