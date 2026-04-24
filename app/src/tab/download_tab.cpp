#include "tab/download_tab.hpp"
#include "view/recycling_grid.hpp"
#include "view/video_view.hpp"
#include "view/mpv_core.hpp"
#include "view/video_profile.hpp"
#include "view/player_setting.hpp"
#include "utils/config.hpp"
#include "utils/dialog.hpp"
#include "utils/misc.hpp"
#include "api/jellyfin/media.hpp"
#include <chrono>
#include <map>

#ifdef USE_BOOST_FILESYSTEM
#include <boost/filesystem.hpp>
namespace fs = boost::filesystem;
#elif __has_include(<filesystem>)
#include <filesystem>
namespace fs = std::filesystem;
#endif

using namespace brls::literals;

class DownloadCard : public RecyclingGridItem {
public:
    DownloadCard() { this->inflateFromXMLRes("xml/view/download_card.xml"); }

    void setItem(const DownloadItem& item, const std::string& downloadDir) {
        this->rectProgress->getParent()->setVisibility(brls::Visibility::GONE);
        this->thumb->setImageFromRes("img/video-card-bg.png");
        std::string thumbPath = downloadDir + "/" + item.itemId + "/thumb.png";
        if (fs::exists(thumbPath)) {
            this->thumb->setImageFromFile(thumbPath);
        }


        this->name->setText(item.seriesName.empty() ? item.name
            : fmt::format("{} - S{}E{} {}", item.seriesName, item.seasonIndex, item.episodeIndex, item.name));

        std::string detail;
        if (item.runTimeTicks > 0) {
            detail = misc::sec2Time(item.runTimeTicks / jellyfin::PLAYTICKS);
        }
        if (item.productionYear > 0) {
            if (!detail.empty()) detail += " · ";
            detail += std::to_string(item.productionYear);
        }
        this->detail->setText(detail);

        switch (item.status) {
        case DownloadStatus::Queued:
            this->status->setText("main/download/queued"_i18n);
            break;
        case DownloadStatus::Downloading:
            if (item.totalBytes > 0) {
                int pct = static_cast<int>(item.downloadedBytes * 100 / item.totalBytes);
                this->status->setText(fmt::format("{}%", pct));
            } else if (item.downloadedBytes > 0 && item.quality != DownloadQuality::Original) {
                std::string size = misc::formatSize(item.downloadedBytes);
                int64_t bitrate = item.quality == DownloadQuality::Q1080p ? 4000000
                    : item.quality == DownloadQuality::Q720p ? 2000000 : 1000000;
                int64_t durationSec = item.runTimeTicks / 10000000;
                int64_t estimated = bitrate * durationSec / 8;
                if (estimated > 0) {
                    int pct = std::min(99, static_cast<int>(item.downloadedBytes * 100 / estimated));
                    this->status->setText(fmt::format("~{}% ({})", pct, size));
                } else {
                    this->status->setText(size);
                }
            } else {
                this->status->setText("main/download/downloading"_i18n);
            }
            break;
        case DownloadStatus::Completed:
            if (item.played) {
                this->status->setText("main/download/watched"_i18n);
            } else if (item.playbackPositionTicks > 0 && item.runTimeTicks > 0) {
                this->rectProgress->setWidthPercentage(item.playedPercentage);
                this->rectProgress->getParent()->setVisibility(brls::Visibility::VISIBLE);
                this->status->setText(fmt::format("{}/{}",
                    misc::sec2Time(item.playbackPositionTicks / jellyfin::PLAYTICKS),
                    misc::sec2Time(item.runTimeTicks / jellyfin::PLAYTICKS)));
            } else {
                this->status->setText("main/download/completed"_i18n);
            }
            break;
        case DownloadStatus::Failed:
            this->status->setText("main/download/failed"_i18n);
            break;
        }
    }

    BRLS_BIND(brls::Image, thumb, "download/thumb");
    BRLS_BIND(brls::Label, name, "download/name");
    BRLS_BIND(brls::Label, detail, "download/detail");
    BRLS_BIND(brls::Label, status, "download/status");
    BRLS_BIND(brls::Rectangle, rectProgress, "download/progress");
};

struct DownloadGroup {
    std::string seriesName;
    int seasonIndex = 0;
    std::vector<DownloadItem> items;

    std::string thumbPath(const std::string& dlDir) const {
        for (auto& item : items) {
            std::string path = dlDir + "/" + item.itemId + "/thumb.png";
            if (fs::exists(path)) return path;
        }
        return "";
    }

    std::string statusSummary() const {
        int completed = 0, downloading = 0, queued = 0, failed = 0;
        for (auto& item : items) {
            switch (item.status) {
            case DownloadStatus::Completed: completed++; break;
            case DownloadStatus::Downloading: downloading++; break;
            case DownloadStatus::Queued: queued++; break;
            case DownloadStatus::Failed: failed++; break;
            }
        }
        if (downloading > 0 || queued > 0) return fmt::format("{}/{}", completed, items.size());
        if (failed > 0) return fmt::format("{}/{}", completed, items.size());
        return "main/download/completed"_i18n;
    }
};

class DownloadDataSource : public RecyclingGridDataSource {
public:
    DownloadDataSource(std::vector<DownloadItem> items)
        : items(std::move(items)), dlDir(AppConfig::instance().configDir() + "/downloads") {}

    size_t getItemCount() override { return this->items.size(); }

    RecyclingGridItem* cellForRow(RecyclingView* recycler, size_t index) override {
        DownloadCard* cell = dynamic_cast<DownloadCard*>(recycler->dequeueReusableCell("Cell"));
        cell->setItem(this->items.at(index), this->dlDir);
        return cell;
    }

    void onItemSelected(brls::Box* recycler, size_t index) override {
        auto& item = this->items.at(index);
        auto& dm = DownloadManager::instance();

        if (item.status == DownloadStatus::Completed) {
            std::string path = dm.getLocalPath(item.itemId);
            if (!path.empty()) {
                std::string playItemId = item.itemId;
                uint64_t playRunTimeTicks = item.runTimeTicks;
                bool wasPlayed = false;
                int64_t resumeTicks = 0;
                for (auto& fresh : dm.getItems()) {
                    if (fresh.itemId == item.itemId) {
                        resumeTicks = fresh.playbackPositionTicks;
                        wasPlayed = fresh.played;
                        break;
                    }
                }

                VideoView* view = new VideoView();
                float width = brls::Application::contentWidth;
                float height = brls::Application::contentHeight;
                view->setDimensions(width, height);
                view->setWidthPercentage(100);
                view->setHeightPercentage(100);
                view->setTitie(item.name);
                view->hideVideoQuality();

                auto* profile = view->getProfile();
                auto& mpv = MPVCore::instance();
                auto subId = std::make_shared<MPVEvent::Subscription>();
                auto unsub = std::make_shared<std::atomic_bool>(false);
                auto lastSave = std::make_shared<std::chrono::steady_clock::time_point>();
                *subId = mpv.getEvent()->subscribe(
                    [profile, subId, unsub, lastSave, playItemId, playRunTimeTicks](MpvEventEnum event) {
                    if (unsub->load()) return;
                    if (event == MpvEventEnum::MPV_RESUME) {
                        profile->init("Local");
                    } else if (event == MpvEventEnum::UPDATE_PROGRESS) {
                        auto now = std::chrono::steady_clock::now();
                        if (now - *lastSave < std::chrono::seconds(10)) return;
                        *lastSave = now;
                        int64_t ticks = static_cast<int64_t>(MPVCore::instance().playback_time * jellyfin::PLAYTICKS);
                        brls::sync([playItemId, ticks]() {
                            DownloadManager::instance().updatePlaybackState(playItemId, ticks);
                        });
                    } else if (event == MpvEventEnum::END_OF_FILE) {
                        brls::sync([playItemId, playRunTimeTicks]() {
                            DownloadManager::instance().updatePlaybackState(playItemId, playRunTimeTicks, true);
                        });
                        unsub->store(true);
                        auto id = *subId;
                        brls::sync([id]() {
                            MPVCore::instance().getEvent()->unsubscribe(id);
                        });
                    } else if (event == MpvEventEnum::MPV_STOP || event == MpvEventEnum::MPV_FILE_ERROR) {
                        int64_t ticks = static_cast<int64_t>(MPVCore::instance().playback_time * jellyfin::PLAYTICKS);
                        brls::sync([playItemId, ticks]() {
                            DownloadManager::instance().updatePlaybackState(playItemId, ticks);
                        });
                        unsub->store(true);
                        auto id = *subId;
                        brls::sync([id]() {
                            MPVCore::instance().getEvent()->unsubscribe(id);
                        });
                    }
                });

                view->getPlayEvent()->subscribe([](int) { return VideoView::close(true); });
                view->getSettingEvent()->subscribe([]() {
                    brls::Application::pushActivity(new brls::Activity(new PlayerSetting()));
                });

                brls::Box* container = new brls::Box();
                container->setDimensions(width, height);
                container->addView(view);
                brls::Application::pushActivity(new brls::Activity(container), brls::TransitionAnimation::NONE);

                if (resumeTicks > 0 && !wasPlayed) {
                    mpv.setUrl(path, "start=" + misc::sec2Time(resumeTicks / jellyfin::PLAYTICKS));
                } else {
                    mpv.setUrl(path);
                }
            }
        } else if (item.status == DownloadStatus::Downloading) {
            std::string id = item.itemId;
            Dialog::cancelable("main/download/cancel"_i18n, [id]() {
                DownloadManager::instance().cancelDownload(id);
            });
        } else if (item.status == DownloadStatus::Queued) {
            DownloadManager::instance().resumeQueue();
        } else if (item.status == DownloadStatus::Failed) {
            std::string id = item.itemId;
            Dialog::cancelable("main/download/confirm_remove"_i18n, [id]() {
                DownloadManager::instance().removeDownload(id);
            });
        }
    }

    void clearData() override { this->items.clear(); }

    const std::string& getItemId(size_t index) const { return this->items.at(index).itemId; }
    size_t itemCount() const { return this->items.size(); }

private:
    std::vector<DownloadItem> items;
    std::string dlDir;
};

class SeasonGroupDataSource : public RecyclingGridDataSource {
public:
    SeasonGroupDataSource(std::vector<DownloadGroup> groups, DownloadView* parent)
        : groups(std::move(groups)), dlDir(AppConfig::instance().configDir() + "/downloads"), parent(parent) {}

    size_t getItemCount() override { return this->groups.size(); }

    RecyclingGridItem* cellForRow(RecyclingView* recycler, size_t index) override {
        DownloadCard* cell = dynamic_cast<DownloadCard*>(recycler->dequeueReusableCell("Cell"));
        auto& group = this->groups.at(index);

        cell->rectProgress->getParent()->setVisibility(brls::Visibility::GONE);
        cell->thumb->setImageFromRes("img/video-card-bg.png");
        std::string thumb = group.thumbPath(this->dlDir);
        if (!thumb.empty()) cell->thumb->setImageFromFile(thumb);

        cell->name->setText(fmt::format("Season {}", group.seasonIndex));
        cell->detail->setText(fmt::format("{} episodes", group.items.size()));
        cell->status->setText(group.statusSummary());
        return cell;
    }

    void onItemSelected(brls::Box* recycler, size_t index) override {
        auto& group = this->groups.at(index);
        this->parent->pushEpisodeList(group.items);
    }

    void clearData() override { this->groups.clear(); }

private:
    std::vector<DownloadGroup> groups;
    std::string dlDir;
    DownloadView* parent;
};

class ShowGroupDataSource : public RecyclingGridDataSource {
public:
    ShowGroupDataSource(std::vector<DownloadGroup> groups, DownloadView* parent)
        : groups(std::move(groups)), dlDir(AppConfig::instance().configDir() + "/downloads"), parent(parent) {}

    size_t getItemCount() override { return this->groups.size(); }

    RecyclingGridItem* cellForRow(RecyclingView* recycler, size_t index) override {
        DownloadCard* cell = dynamic_cast<DownloadCard*>(recycler->dequeueReusableCell("Cell"));
        auto& group = this->groups.at(index);

        cell->rectProgress->getParent()->setVisibility(brls::Visibility::GONE);
        cell->thumb->setImageFromRes("img/video-card-bg.png");
        std::string thumb = group.thumbPath(this->dlDir);
        if (!thumb.empty()) cell->thumb->setImageFromFile(thumb);

        cell->name->setText(group.seriesName);
        cell->detail->setText(fmt::format("{} episodes", group.items.size()));
        cell->status->setText(group.statusSummary());
        return cell;
    }

    void onItemSelected(brls::Box* recycler, size_t index) override {
        auto& group = this->groups.at(index);

        std::map<int, std::vector<DownloadItem>> seasons;
        for (auto& item : group.items) {
            seasons[item.seasonIndex].push_back(item);
        }

        if (seasons.size() <= 1) {
            this->parent->pushEpisodeList(group.items);
            return;
        }

        std::vector<DownloadGroup> seasonGroups;
        for (auto& [seasonIdx, eps] : seasons) {
            DownloadGroup sg;
            sg.seriesName = group.seriesName;
            sg.seasonIndex = seasonIdx;
            sg.items = std::move(eps);
            seasonGroups.push_back(std::move(sg));
        }

        this->parent->pushSeasonList(std::move(seasonGroups));
    }

    void clearData() override { this->groups.clear(); }

private:
    std::vector<DownloadGroup> groups;
    std::string dlDir;
    DownloadView* parent;
};

DownloadView::DownloadView() {
    brls::Logger::debug("DownloadView: create");

    RecyclingGrid* grid = this->newRecycler();
    this->stack.push_back(grid);
    this->setContent(grid);

    this->statusSubId = DownloadManager::instance().getStatusEvent()->subscribe(
        [this](const std::string&, DownloadStatus) {
            this->loadItems();
        });

    this->progressSubId = DownloadManager::instance().getProgressEvent()->subscribe(
        [this](const std::string&, int64_t, int64_t) {
            this->loadItems();
        });

    this->loadItems();
}

DownloadView::~DownloadView() {
    brls::Logger::debug("DownloadView: deleted");
    DownloadManager::instance().getStatusEvent()->unsubscribe(this->statusSubId);
    DownloadManager::instance().getProgressEvent()->unsubscribe(this->progressSubId);
    for (auto* grid : this->stack) {
        if (grid != this->recycler) grid->freeView();
    }
}

brls::View* DownloadView::getDefaultFocus() { return this->recycler; }

void DownloadView::loadItems() {
    if (this->stack.size() > 1) return;

    auto items = DownloadManager::instance().getItems();
    if (items.empty()) {
        this->recycler->setEmpty("main/download/no_downloads"_i18n);
        return;
    }

    std::vector<DownloadItem> standalone;
    std::map<std::string, std::vector<DownloadItem>> byShow;

    for (auto& item : items) {
        if (item.seriesName.empty()) {
            standalone.push_back(std::move(item));
        } else {
            byShow[item.seriesName].push_back(std::move(item));
        }
    }

    if (byShow.empty()) {
        this->recycler->setDataSource(new DownloadDataSource(std::move(standalone)));
        return;
    }

    std::vector<DownloadGroup> groups;
    for (auto& [name, eps] : byShow) {
        DownloadGroup g;
        g.seriesName = name;
        g.items = std::move(eps);
        groups.push_back(std::move(g));
    }
    if (!standalone.empty()) {
        DownloadGroup g;
        g.seriesName = "main/download/movies"_i18n;
        g.items = std::move(standalone);
        groups.push_back(std::move(g));
    }

    this->recycler->setDataSource(new ShowGroupDataSource(std::move(groups), this));
}

RecyclingGrid* DownloadView::newRecycler() {
    RecyclingGrid* grid = new RecyclingGrid();
    grid->spanCount = 1;
    grid->estimatedRowHeight = 130;
    grid->estimatedRowSpace = 5;
    grid->setDefaultCellFocus(1);
    grid->registerCell("Cell", []() { return new DownloadCard(); });

    auto deleteAction = [this](brls::View*) {
        auto* focus = dynamic_cast<RecyclingGridItem*>(brls::Application::getCurrentFocus());
        if (!focus) return false;
        auto* ds = dynamic_cast<DownloadDataSource*>(this->recycler->getDataSource());
        if (!ds) return false;
        size_t idx = focus->getIndex();
        if (idx >= ds->itemCount()) return false;
        std::string id = ds->getItemId(idx);
        Dialog::cancelable("main/download/confirm_remove"_i18n, [this, id]() {
            DownloadManager::instance().removeDownload(id);
            this->loadItems();
        });
        return true;
    };
    grid->registerAction("main/download/remove"_i18n, brls::BUTTON_X, deleteAction);
    grid->registerAction(brls::BRLS_KBD_KEY_BACKSPACE, deleteAction);

    grid->registerAction("hints/back"_i18n, brls::BUTTON_B, [this](...) {
        this->dismiss();
        return true;
    });

    return grid;
}

RecyclingGrid* DownloadView::newGroupRecycler() {
    RecyclingGrid* grid = new RecyclingGrid();
    grid->spanCount = 1;
    grid->estimatedRowHeight = 130;
    grid->estimatedRowSpace = 5;
    grid->setDefaultCellFocus(1);
    grid->registerCell("Cell", []() { return new DownloadCard(); });

    grid->registerAction("hints/back"_i18n, brls::BUTTON_B, [this](...) {
        this->dismiss();
        return true;
    });

    return grid;
}

void DownloadView::pushEpisodeList(std::vector<DownloadItem> items) {
    RecyclingGrid* grid = this->newRecycler();
    grid->setDataSource(new DownloadDataSource(std::move(items)));
    this->stack.push_back(grid);
    this->setContent(grid);
}

void DownloadView::pushSeasonList(std::vector<DownloadGroup> groups) {
    RecyclingGrid* grid = this->newGroupRecycler();
    grid->setDataSource(new SeasonGroupDataSource(std::move(groups), this));
    this->stack.push_back(grid);
    this->setContent(grid);
}

void DownloadView::setContent(RecyclingGrid* view) {
    if (this->recycler) {
        this->removeView(this->recycler, false);
        this->recycler = nullptr;
    }
    this->recycler = view;
    this->recycler->setDimensions(brls::View::AUTO, brls::View::AUTO);
    this->recycler->setGrow(1.0f);
    this->addView(this->recycler);
    brls::Application::giveFocus(this->recycler);
}

void DownloadView::dismiss(std::function<void(void)> cb) {
    if (this->stack.size() > 1) {
        brls::View* lastView = this->recycler;
        this->stack.pop_back();
        this->setContent(this->stack.back());
        cb();
        lastView->freeView();
        if (this->stack.size() == 1) this->loadItems();
    } else {
        AutoTabFrame::focus2Sidebar(this);
    }
}
