#include "DataAccess/SongBrowserModel.hpp"
#include "DataAccess/config.hpp"
#include "logging.hpp"

#include "GlobalNamespace/BeatmapLevelCollection.hpp"
#include "GlobalNamespace/LevelCollectionTableView.hpp"
#include "GlobalNamespace/BeatmapLevelPack.hpp"
#include "GlobalNamespace/IBeatmapLevelCollection.hpp"
#include "GlobalNamespace/PlayerDataModel.hpp"
#include "GlobalNamespace/PlayerData.hpp"
#include "GlobalNamespace/PlayerLevelStatsData.hpp"
#include "GlobalNamespace/LevelCollectionNavigationController.hpp"
#include "GlobalNamespace/LevelCollectionViewController.hpp"
#include "GlobalNamespace/LevelPackDetailViewController.hpp"
#include "GlobalNamespace/SongPreviewPlayer.hpp"
#include "GlobalNamespace/LevelPackHeaderTableCell.hpp"
#include "GlobalNamespace/LevelListTableCell.hpp"

#include "System/Collections/Generic/HashSet_1.hpp"
#include "System/Diagnostics/Stopwatch.hpp"
#include "System/IO/File.hpp"
#include "System/IO/Path.hpp"
#include "System/TimeSpan.hpp"
#include "System/Linq/Enumerable.hpp"
#include "System/Random.hpp"

#include "songloader/shared/API.hpp"

#include "Utils/ArrayUtil.hpp"
#include "Utils/EnumToStringUtils.hpp"
#include "Utils/SongDataCoreUtils.hpp"

#include "sdc-wrapper/shared/BeatStarSong.hpp"

#include "UnityEngine/Rect.hpp"
#include "UnityEngine/Resources.hpp"
#include "UnityEngine/Sprite.hpp"
#include "UnityEngine/GameObject.hpp"
#include "UnityEngine/Object.hpp"

#include "Zenject/DiContainer.hpp"

#include "HMUI/AlphabetScrollbar.hpp"
#include "HMUI/TableView.hpp"
#include "HMUI/TableView_ScrollPositionType.hpp"
#include "HMUI/ScrollView.hpp"
#include "HMUI/TableCell.hpp"

#include "System/Action_1.hpp"
#include "System/Action_3.hpp"
#include "System/Tuple_2.hpp"

#include <algorithm>
DEFINE_TYPE(SongBrowser, SongBrowserModel);

using Stopwatch = System::Diagnostics::Stopwatch;
using namespace UnityEngine;

std::vector<std::string> split(std::string string, const std::string& delimiter)
{
    // not sure if this is correct, but it might be /shrug
    std::vector<std::string> result = {};

    int pos = string.find(delimiter);
    while (pos != std::string::npos)
    {
        result.push_back(string.substr(0, pos));
        string.erase(0, pos + delimiter.size());    
        pos = string.find(delimiter);
    }

    return result;
}

namespace SongBrowser
{
    std::function<List<GlobalNamespace::IPreviewBeatmapLevel*>*(Array<GlobalNamespace::IPreviewBeatmapLevel*>*)> SongBrowserModel::customFilterHandler;
    std::function<List<GlobalNamespace::IPreviewBeatmapLevel*>*(List<GlobalNamespace::IPreviewBeatmapLevel*>*)> SongBrowserModel::customSortHandler;
    UnorderedEventCallback<const std::vector<GlobalNamespace::CustomPreviewBeatmapLevel*>&> SongBrowserModel::didFinishProcessingSongs;

    void SongBrowserModel::ctor()
    {
        INVOKE_CTOR();
        EPOCH = System::DateTime(1970, 1, 1);
        customFilterHandler = nullptr;
        customSortHandler = nullptr;
    }

    void SongBrowserModel::Init()
    {
        LoadConfig();
        INFO("Config loaded, sort mode is %d", config.sortMode);
    }

    bool SongBrowserModel::get_sortWasMissingData()
    {
        return sortWasMissingData;
    }

    void SongBrowserModel::ToggleInverting()
    {
        config.invertSortResults = !config.invertSortResults;
    }

    void SongBrowserModel::UpdateLevelRecords()
    {
        auto timer = Stopwatch::New_ctor();
        timer->Start();

        // Calculate some information about the custom song dir
        static std::string customSongsPath = RuntimeSongLoader::API::GetCustomLevelsPath();
        static auto customSongsPath_cs = il2cpp_utils::newcsstr<il2cpp_utils::CreationType::Manual>(customSongsPath);
        double currentCustomSongDirLastWriteTIme = (System::IO::File::GetLastWriteTime(customSongsPath_cs) - EPOCH).get_TotalMilliseconds();
        bool customSongDirChanged = false;
        if (customSongDirLastWriteTime != currentCustomSongDirLastWriteTIme)
        {
            customSongDirChanged = true;
            customSongDirLastWriteTime = currentCustomSongDirLastWriteTIme;
        }

        if (!direxists(customSongsPath))
        {
            ERROR("CustomSong directory is missing...");
            return;
        }

        // Map some data for custom songs
        auto lastWriteTimer = Stopwatch::New_ctor();
        lastWriteTimer->Start();
        auto customLevels = RuntimeSongLoader::API::GetLoadedSongs();
        for (auto level : customLevels)
        {
            // If we already know this levelID, don't both updating it.
            // SongLoader should filter duplicates but in case of failure we don't want to crash
            std::string levelID = to_utf8(csstrtostr(level->get_levelID()));
            auto it = cachedLastWriteTimes.find(levelID); 
            if (it != cachedLastWriteTimes.end() || customSongDirChanged)
            {
                double lastWriteTime = GetSongUserDate(level);
                cachedLastWriteTimes[levelID] = lastWriteTime;
            }
        }

        lastWriteTimer->Stop();
        INFO("Determining song download time and determining mappings took %ld ms", lastWriteTimer->get_ElapsedMilliseconds());

        // Update song Infos, directory tree, and sort
        this->UpdatePlayCounts();

        // Signal complete
        if (customLevels.size() > 0)
        {
            didFinishProcessingSongs.invoke(customLevels);
        }

        timer->Stop();
        INFO("Updating songs infos took %ld ms", timer->get_ElapsedMilliseconds());
    }

    void SongBrowserModel::RemoveSongFromLevelCollection(GlobalNamespace::IAnnotatedBeatmapLevelCollection* levelCollection, std::string_view levelId)
    {
        auto levelList = System::Linq::Enumerable::ToList(reinterpret_cast<System::Collections::Generic::IEnumerable_1<GlobalNamespace::IPreviewBeatmapLevel*>*>(levelCollection->get_beatmapLevelCollection()->get_beatmapLevels()));
        
        std::vector<GlobalNamespace::IPreviewBeatmapLevel*> remove = {};
        auto levelID_cs = il2cpp_utils::newcsstr(levelId);
        int length = levelList->get_Count();
        for (int i = 0; i < length; i++)
        {
            auto level = levelList->items->values[i];
            if (level->get_levelID()->Equals(levelID_cs)) remove.push_back(level);
        }

        for (auto level : remove)
        {
            levelList->Remove(level);
        }
    }

    void SongBrowserModel::UpdatePlayCounts()
    {
                // Reset current playcounts
        levelIdToPlayCount.clear();

        // Build a map of levelId to sum of all playcounts and sort.
        auto playerData = ArrayUtil::First(Resources::FindObjectsOfTypeAll<GlobalNamespace::PlayerDataModel*>());
        auto levelsStatsData = playerData->get_playerData()->get_levelsStatsData();
        int length = levelsStatsData->get_Count();
        
        for (int i = 0; i < length; i++)
        {
            auto levelData = levelsStatsData->items->values[i];
            std::string levelID = to_utf8(csstrtostr(levelData->get_levelID()));
            auto itr = levelIdToPlayCount.find(levelID);
            // if not found, just set it
            if (itr == levelIdToPlayCount.end())
            {
                levelIdToPlayCount[levelID] = levelData->get_playCount();
                continue;
            }
            // if found, add to it
            itr->second += levelData->get_playCount();
        }
    }
#pragma region Self implementation for bugfixing
    /*
    void ScrollView_UpdateContentSize(HMUI::ScrollView* self)
    {
        auto scrollViewDirection = self->scrollViewDirection;
		if (scrollViewDirection != HMUI::ScrollView::ScrollViewDirection::Vertical)
		{
			if (scrollViewDirection == HMUI::ScrollView::ScrollViewDirection::Horizontal)
			{
                INFO("width set size");
				self->SetContentSize(self->contentRectTransform->get_rect().get_width());
			}
		}
		else
		{
            INFO("height set size");
			self->SetContentSize(self->contentRectTransform->get_rect().get_height());
		}
        INFO("scroll to");
		self->ScrollTo(0.0f, true);
    }

    void TableView_RefreshContentSize(HMUI::TableView* self)
    {
        if (self->tableType == HMUI::TableView::TableType::Vertical)
		{
            INFO("vert update");
			self->contentTransform->set_sizeDelta(Vector2(0.0f, (float)self->numberOfCells * self->cellSize));
		}
		else
		{
            INFO("horizon update");
			self->contentTransform->set_sizeDelta(Vector2((float)self->numberOfCells * self->cellSize, 0.0f));
		}
        INFO("update size");
		ScrollView_UpdateContentSize(self->scrollView);
    }

    HMUI::TableCell* LevelCollectionTableView_CellForIdx(GlobalNamespace::LevelCollectionTableView* self, HMUI::TableView* tableView, int row)
    {
        INFO("Start cell");
		if (row == 0 && self->showLevelPackHeader)
		{
            INFO("row 0 & show");
			auto levelPackHeaderTableCell = reinterpret_cast<GlobalNamespace::LevelPackHeaderTableCell*>(tableView->DequeueReusableCellForIdentifier(self->packCellsReuseIdentifier));
            INFO("check if found");
			if (!levelPackHeaderTableCell)
			{
                INFO("make new");
				levelPackHeaderTableCell = UnityEngine::Object::Instantiate<GlobalNamespace::LevelPackHeaderTableCell*>(self->packCellPrefab);
				levelPackHeaderTableCell->reuseIdentifier = self->packCellsReuseIdentifier;
			}
            INFO("set data");
			levelPackHeaderTableCell->SetData(self->headerText);
            INFO("return");
			return levelPackHeaderTableCell;
		}
        INFO("not 0 so next");
		
        auto levelListTableCell = reinterpret_cast<GlobalNamespace::LevelListTableCell*>(tableView->DequeueReusableCellForIdentifier(self->levelCellsReuseIdentifier));
        INFO("check if found");
		if (!levelListTableCell)
		{
            INFO("make new ");
			levelListTableCell = UnityEngine::Object::Instantiate<GlobalNamespace::LevelListTableCell*>(self->levelCellPrefab);
			levelListTableCell->reuseIdentifier = self->levelCellsReuseIdentifier;
		}
        INFO("get num");
		int num = self->showLevelPackHeader ? (row - 1) : row;
        INFO("get level");
        INFO("Getting level %d, from array of %lu", num, self->previewBeatmapLevels->Length());
		auto previewBeatmapLevel = self->previewBeatmapLevels->values[num];
        if (previewBeatmapLevel)
        {
            INFO("set data from level async");
		    levelListTableCell->SetDataFromLevelAsync(previewBeatmapLevel, self->favoriteLevelIds->Contains(previewBeatmapLevel->get_levelID()));
            INFO("refresh availability");
		    levelListTableCell->RefreshAvailabilityAsync(self->additionalContentModel, previewBeatmapLevel->get_levelID());
            INFO("levelList");
        }
		return levelListTableCell;
    }

    void TableView_RefreshCells(HMUI::TableView* self, bool forcedVisualsRefresh, bool forcedContentRefresh)
    {
        int counter = 0;
        INFO("%d", counter++);
		self->LazyInit();
		int num;
		int num2;
		num = self->GetVisibleCellsIdRange()->get_Item1();
		num2 = self->GetVisibleCellsIdRange()->get_Item2();
        INFO("%d", counter++);
		if (num == self->prevMinIdx && num2 == self->prevMaxIdx && !forcedVisualsRefresh && !forcedContentRefresh)
		{
            INFO("%d", counter++);
			return;
		}
		for (int i = self->visibleCells->get_Count() - 1; i >= 0; i--)
		{
            INFO("%d", counter++);
			auto tableCell = self->visibleCells->items->values[i];
			if (tableCell->idx < num || tableCell->idx > num2 || forcedContentRefresh)
			{
				tableCell->get_gameObject()->SetActive(false);
				self->visibleCells->RemoveAt(i);
				self->AddCellToReusableCells(tableCell);
			}
            INFO("%d", counter++);
		}

		auto rect = self->viewportTransform->get_rect();
		float num5 = (self->tableType == HMUI::TableView::TableType::Vertical) ? rect.get_height() : rect.get_width();
		float offset = 0.0f;
        INFO("%d", counter++);
		if (self->alignToCenter && self->scrollView->get_scrollableSize() == 0.0f)
		{
			offset = (num5 - (float)self->numberOfCells * self->cellSize) * 0.5f;
		}
        INFO("%d", counter++);
		for (int j = num; j <= num2; j++)
		{
			HMUI::TableCell* tableCell2 = nullptr;
			for (int k = 0; k < self->visibleCells->get_Count(); k++)
			{
				if (self->visibleCells->items->values[k]->idx == j)
				{
					tableCell2 = self->visibleCells->items->values[k];
					break;
				}
			}
            INFO("%d", counter++);
			if (!tableCell2 || forcedVisualsRefresh || forcedContentRefresh)
			{
                INFO("%d", counter++);
				bool flag = false;
                INFO("%d", counter++);
				if (!tableCell2)
				{
                    INFO("%d", counter++);
					flag = true;
                    INFO("%d", counter++);
                    INFO("self->dataSource: %p", self->dataSource);
                    INFO("self->dataSource->klass->name: %s", ((Il2CppObject*)self->dataSource)->klass->name);
					tableCell2 = LevelCollectionTableView_CellForIdx(reinterpret_cast<GlobalNamespace::LevelCollectionTableView*>(self->dataSource), self, j);
                    INFO("%d", counter++);
					self->visibleCells->Add(tableCell2);
                    INFO("%d", counter++);
				}
                INFO("%d", counter++);
                INFO("TableCell: %p", tableCell2);
				tableCell2->get_gameObject()->SetActive(true);
                INFO("%d", counter++);
				tableCell2->TableViewSetup(reinterpret_cast<HMUI::ITableCellOwner*>(self), j);
                INFO("%d", counter++);
                std::function<void(HMUI::SelectableCell*, HMUI::SelectableCell::TransitionType, ::Il2CppObject*)> fun = std::bind(&HMUI::TableView::HandleCellSelectionDidChange, self, std::placeholders::_1, std::placeholders::_2, std::placeholders::_3);
                INFO("%d", counter++);
                auto delegate = il2cpp_utils::MakeDelegate<System::Action_3<HMUI::SelectableCell*, HMUI::SelectableCell::TransitionType, ::Il2CppObject*>*>(classof(System::Action_3<HMUI::SelectableCell*, HMUI::SelectableCell::TransitionType, ::Il2CppObject*>*), fun);
                INFO("%d", counter++);
                tableCell2->remove_selectionDidChangeEvent(delegate);
                INFO("%d", counter++);
				tableCell2->add_selectionDidChangeEvent(delegate);
                INFO("%d", counter++);
				if (flag)
				{
                    INFO("%d", counter++);
					tableCell2->ClearHighlight(HMUI::SelectableCell::TransitionType::Instant);
				}
                INFO("%d", counter++);
				tableCell2->SetSelected(self->selectedCellIdxs->Contains(j), HMUI::SelectableCell::TransitionType::Instant, self, flag);
				if (tableCell2->get_transform()->get_parent() != self->contentTransform)
				{
					tableCell2->get_transform()->SetParent(self->contentTransform, false);
				}
                INFO("%d", counter++);
				self->LayoutCellForIdx(tableCell2, j, offset);
				if (self->visibleCells->get_Count() == num2 - num + 1 && !forcedVisualsRefresh)
				{
					break;
				}
			}
		}
        INFO("");
		self->prevMinIdx = num;
		self->prevMaxIdx = num2;   
        INFO("end");
    }

    void TableView_ReloadData(HMUI::TableView* self)
    {
        INFO("Initialize Check");
        if (!self->isInitialized)
		{
			self->LazyInit();
		}
        INFO("for every cell");
        ArrayUtil::ForEach(self->visibleCells, [&](auto tableCell){
            tableCell->get_gameObject()->SetActive(false);
			self->AddCellToReusableCells(tableCell);
        });

        INFO("clear visible");
		self->visibleCells->Clear();
        INFO("check ifd data source");
		if (self->dataSource)
		{
            INFO("there was a data source");
			self->numberOfCells = self->dataSource->NumberOfCells();
            INFO("setting cell size");
			self->cellSize = self->dataSource->CellSize();
		}
		else
		{
            INFO("there was no data source");
			self->numberOfCells = 0;
            INFO("setting cell size");
			self->cellSize = 1.0f;
		}
        INFO("setting fixed size");
		self->scrollView->fixedCellSize = self->cellSize;
        INFO("Refresh size");
		TableView_RefreshContentSize(self);
        INFO("check if now or later refresh");
		if (!self->get_gameObject()->get_activeInHierarchy())
		{
        INFO("set refresh bool");
			self->refreshCellsOnEnable = true;
		}
		else
		{
            INFO("refresh cells");
			TableView_RefreshCells(self, true, false);
		}

        INFO("get action");
		auto action = self->didReloadDataEvent;
        INFO("invoke action");
		if (action) action->Invoke(self);
    }

    void LevelCollectionTableView_SetData(GlobalNamespace::LevelCollectionTableView* self, Array<GlobalNamespace::IPreviewBeatmapLevel*>* previewBeatmapLevels, System::Collections::Generic::HashSet_1<Il2CppString*>* favoriteLevelIds, bool beatmapLevelsAreSorted)
    {
        INFO("Init");
		self->Init();
		self->previewBeatmapLevels = previewBeatmapLevels;
		self->favoriteLevelIds = favoriteLevelIds;
        INFO("get rect from table view");
		auto rectTransform = reinterpret_cast<UnityEngine::RectTransform*>(self->tableView->get_transform());
		if (false)//beatmapLevelsAreSorted && previewBeatmapLevels.Length > self->showAlphabetScrollbarLevelCountThreshold)
		{
            // we're never doing this so it's kind of irrelevant
		}
		else
		{
			rectTransform->set_offsetMin(Vector2(0.0f, 0.0f));
            INFO("alphabetscrollbar");
			self->alphabetScrollbar->get_gameObject()->SetActive(false);
		}

        INFO("reload data");
		TableView_ReloadData(self->tableView);
        INFO("scroll to Idx");
		self->tableView->ScrollToCellWithIdx(0, HMUI::TableView::ScrollPositionType::Beginning, false);
    }

    void LevelCollectionViewController_SetData(GlobalNamespace::LevelCollectionViewController* self, GlobalNamespace::IBeatmapLevelCollection* beatmapLevelCollection, Il2CppString* headerText, UnityEngine::Sprite* headerSprite, bool sortLevels, UnityEngine::GameObject* noDataInfoPrefab)
    {
        self->showHeader = !Il2CppString::IsNullOrEmpty(headerText);
		self->levelCollectionTableView->Init(headerText, headerSprite);
        INFO("Destroying no data GO");
		if (self->noDataInfoGO)
		{
			UnityEngine::Object::Destroy(self->noDataInfoGO);
			self->noDataInfoGO = nullptr;
		}

        INFO("Checking to refresh table");
		if (beatmapLevelCollection && beatmapLevelCollection->get_beatmapLevels() && beatmapLevelCollection->get_beatmapLevels()->Length() != 0)
		{
            INFO("Updating table view");
            INFO("Activate GO");
			self->levelCollectionTableView->get_gameObject()->SetActive(true);
            INFO("Get Level Array");
			Array<GlobalNamespace::IPreviewBeatmapLevel*>* beatmapLevels = beatmapLevelCollection->get_beatmapLevels();
            INFO("Setting data");
            auto dataModel = self->playerDataModel;
            INFO("dataModel: %p", dataModel);
            auto data = dataModel ? dataModel->get_playerData() : nullptr;
            INFO("data: %p", data);
            auto favIds = data ? data->get_favoritesLevelIds() : nullptr;
            INFO("favIds: %p", favIds);
			LevelCollectionTableView_SetData(self->levelCollectionTableView, beatmapLevels, favIds, sortLevels);
            INFO("Refresh levels availability");
			self->levelCollectionTableView->RefreshLevelsAvailability();
		}
		else
		{
            INFO("instantiating new no data GO if available");
			if (noDataInfoPrefab)
			{
				self->noDataInfoGO = self->container->InstantiatePrefab(noDataInfoPrefab, self->noDataInfoContainer);
			}
            INFO("Disablign table view");
			self->levelCollectionTableView->get_gameObject()->SetActive(false);
		}

        INFO("checking if in hierarchy");
		if (self->get_isInViewControllerHierarchy())
		{
			if (self->showHeader)
			{
                INFO("Showing header");
				self->levelCollectionTableView->SelectLevelPackHeaderCell();
			}
			else
			{
                INFO("Clear selection");
				self->levelCollectionTableView->ClearSelection();
			}
                INFO("Crossfade to menu music");
			self->songPreviewPlayer->CrossfadeToDefault();
		}
    }
    */
#pragma endregion
    
    void SongBrowserModel::ProcessSongList(GlobalNamespace::IAnnotatedBeatmapLevelCollection* selectedBeatmapCollection, GlobalNamespace::LevelSelectionNavigationController* navController)
    {
        Array<GlobalNamespace::IPreviewBeatmapLevel*>* unsortedSongs = nullptr;
        List<GlobalNamespace::IPreviewBeatmapLevel*>* filteredSongs = nullptr;
        List<GlobalNamespace::IPreviewBeatmapLevel*>* sortedSongs = nullptr;

        // Abort
        if (!selectedBeatmapCollection)
        {
            INFO("Cannot process songs yet, no level collection selected...");
            return;
        }
        std::string selectedCollectionName = selectedBeatmapCollection->get_collectionName() ? to_utf8(csstrtostr(selectedBeatmapCollection->get_collectionName())) : "";

        INFO("Using songs from level collection: %s [num=%lu]", selectedCollectionName.c_str(), selectedBeatmapCollection->get_beatmapLevelCollection()->get_beatmapLevels()->Length());
        unsortedSongs = GetLevelsForLevelCollection(selectedBeatmapCollection);

        // filter
        INFO("Starting filtering songs by %s", SongFilterModeToString(config.filterMode).c_str());
        auto stopwatch = System::Diagnostics::Stopwatch::New_ctor();
        stopwatch->Start();
/*     >>>>>>> NOT REQUIRED IN QUEST
        if (config.filterMode == SongFilterMode::Requirements)
        {
            auto modList = Modloader::getMods();
            auto itr = modList.find("CustomJSONData");
            if (itr == modList.end() || !itr->second.get_loaded())
            {
                // if cjd not loaded
                config.filterMode = SongFilterMode::None;
            }
        }
*/
        switch (config.filterMode)
        {
            case SongFilterMode::Favorites:
                filteredSongs = FilterFavorites(unsortedSongs);
                break;
            case SongFilterMode::Search:
                filteredSongs = FilterSearch(unsortedSongs);
                break;
            case SongFilterMode::Ranked:
                filteredSongs = FilterRanked(unsortedSongs, true, false);
                break;
            case SongFilterMode::Unranked:
                filteredSongs = FilterRanked(unsortedSongs, false, true);
                break;
            case SongFilterMode::Requirements:
                filteredSongs = FilterRequirements(unsortedSongs);
                break;
            case SongFilterMode::Unplayed:
                filteredSongs = FilterUnplayed(unsortedSongs);
                break;
            case SongFilterMode::CustomFilter:
                INFO("Song filter mode set to custom. Deferring filter behaviour to another mod.");
                filteredSongs = customFilterHandler ? customFilterHandler(unsortedSongs) : FilterOriginal(unsortedSongs);
                break;            
            case SongFilterMode::None:
            default:
                INFO("No song filter selected...");
                filteredSongs = FilterOriginal(unsortedSongs);
                break;
        }

        stopwatch->Stop();
        INFO("Filtering songs took %ld ms", stopwatch->get_ElapsedMilliseconds());

        // sort
        INFO("Starting to sort songs by %s", SongSortModeToString(config.sortMode).c_str());
        stopwatch->Reset();
        stopwatch->Start();
        sortWasMissingData = false;
        switch (config.sortMode)
        {
            case SongSortMode::Original:
                sortedSongs = SortOriginal(filteredSongs);
                break;
            case SongSortMode::Newest:
                sortedSongs = SortNewest(filteredSongs);
                break;
            case SongSortMode::Author:
                sortedSongs = SortAuthor(filteredSongs);
                break;
            case SongSortMode::UpVotes:
                sortedSongs = SortUpVotes(filteredSongs);
                break;
            case SongSortMode::PlayCount:
                sortedSongs = SortBeatSaverPlayCount(filteredSongs);
                break;
            case SongSortMode::Rating:
                sortedSongs = SortBeatSaverRating(filteredSongs);
                break;
            case SongSortMode::Heat:
                sortedSongs = SortBeatSaverHeat(filteredSongs);
                break;
            case SongSortMode::YourPlayCount:
                sortedSongs = SortPlayCount(filteredSongs);
                break;
            case SongSortMode::PP:
                sortedSongs = SortPerformancePoints(filteredSongs);
                break;
            case SongSortMode::Stars:
                sortedSongs = SortStars(filteredSongs);
                break;
            case SongSortMode::Random:
                sortedSongs = SortRandom(filteredSongs);
                break;
            case SongSortMode::Bpm:
                sortedSongs = SortSongBpm(filteredSongs);
                break;
            case SongSortMode::NJS:
                sortedSongs = SortSongNJS(filteredSongs);
                break;
            case SongSortMode::Length:
                sortedSongs = SortSongLength(filteredSongs);
                break;
            case SongSortMode::Downloads:
                sortedSongs = SortBeatSaverDownloads(filteredSongs);
                break;
            case SongSortMode::CustomSort:
                sortedSongs = customSortHandler ? customSortHandler(filteredSongs) : filteredSongs;
                break;
            case SongSortMode::Default:
            default:
                sortedSongs = SortSongName(filteredSongs);
                break;
        }

        switch (config.sortMode)
        {
            // dont auto revert for these cases
            case SongSortMode::Original:
            case SongSortMode::Author:
            case SongSortMode::CustomSort:
            case SongSortMode::Default:
                if (config.invertSortResults)
                    sortedSongs->Reverse();
                break;
            // for these cases we want to reverse by default so newest/longest/best end up on top when green
            case SongSortMode::Newest:
            case SongSortMode::UpVotes:
            case SongSortMode::PlayCount:
            case SongSortMode::Rating:
            case SongSortMode::Heat:
            case SongSortMode::YourPlayCount:
            case SongSortMode::PP:
            case SongSortMode::Stars:
            case SongSortMode::Bpm:
            case SongSortMode::NJS:
            case SongSortMode::Length:
            case SongSortMode::Downloads:
                if (!config.invertSortResults)
                    sortedSongs->Reverse();
                break;
            default:
                break;
        }

        stopwatch->Stop();
        INFO("Sorting songs took %lu ms", stopwatch->get_ElapsedMilliseconds());

        // Still hacking in a custom level pack
        // Asterisk the pack name so it is identifable as filtered.
        if (!selectedCollectionName.ends_with('*') && config.filterMode != SongFilterMode::None)
        {
            selectedCollectionName += "*";
        }

        // Some level categories have a null cover image, supply something, it won't show it anyway
        auto coverImage = selectedBeatmapCollection->get_coverImage();
        /*
        // we dont have bsml so get fucked
        // if image bad just get fucked lul
        if (!coverImage)
        {
            coverImage = BeatSaberMarkupLanguage.Utilities.ImageResources.BlankSprite;
        }
        */
       
        INFO("Creating filtered level pack...");
        static auto collectionName = il2cpp_utils::newcsstr<il2cpp_utils::CreationType::Manual>(filteredSongsCollectionName);
                                                                        
        auto levelCollection = reinterpret_cast<GlobalNamespace::IBeatmapLevelCollection*>(GlobalNamespace::BeatmapLevelCollection::New_ctor(sortedSongs->ToArray()));

        INFO("levelCollection size: %lu", levelCollection->get_beatmapLevels()->Length());
        auto levelPack = GlobalNamespace::BeatmapLevelPack::New_ctor(   collectionName, 
                                                                        il2cpp_utils::newcsstr(selectedCollectionName),
                                                                        selectedBeatmapCollection->get_collectionName(), 
                                                                        coverImage, 
                                                                        levelCollection);
        /*
         public virtual void SetData(
            IAnnotatedBeatmapLevelCollection annotatedBeatmapLevelCollection,
            bool showPackHeader, bool showPlayerStats, bool showPracticeButton,
            string actionButtonText,
            GameObject noDataInfoPrefab, BeatmapDifficultyMask allowedBeatmapDifficultyMask, BeatmapCharacteristicSO[] notAllowedCharacteristics);
        */
        INFO("Acquiring necessary fields to call SetData(pack)...");
        auto lcnvc = navController->levelCollectionNavigationController;
        auto showPlayerStatsInDetailView = navController->showPlayerStatsInDetailView;
        auto hidePracticeButton = navController->hidePracticeButton;
        auto actionButtonText = navController->actionButtonText;
        auto noDataInfoPrefab = lcnvc->levelCollectionViewController->noDataInfoGO;
        auto allowedBeatmapDifficultyMask = navController->allowedBeatmapDifficultyMask;
        auto notAllowedCharacteristics = navController->notAllowedCharacteristics;


        INFO("Calling lcnvc.SetData...");

        INFO("lcnvc: %p", lcnvc);
        INFO("levelPack: %p", levelPack);
        INFO("showPlayerStatsInDetailView: %d", showPlayerStatsInDetailView);
        INFO("hidePracticeButton: %d", hidePracticeButton);
        INFO("actionButtonText: %p", actionButtonText);
        INFO("noDataInfoPrefab: %p", noDataInfoPrefab);
        INFO("allowedBeatmapDifficultyMask: %d", allowedBeatmapDifficultyMask.value);
        INFO("notAllowedCharacteristics: %p", notAllowedCharacteristics);
        INFO("lcnvc->levelCollectionViewController: %p", lcnvc->levelCollectionViewController);
        INFO("lcnvc->levelPackDetailViewController: %p", lcnvc->levelPackDetailViewController);
        /*
        // basically SetDataForPack but skipping the redundant re-assignments
        lcnvc->levelPack = reinterpret_cast<GlobalNamespace::IBeatmapLevelPack*>(levelPack);
        INFO("Collection SetData");
        LevelCollectionViewController_SetData(lcnvc->levelCollectionViewController, levelCollection, il2cpp_utils::newcsstr(selectedCollectionName), coverImage, false, nullptr);
		//lcnvc->levelCollectionViewController->SetData(levelCollection, il2cpp_utils::newcsstr(selectedCollectionName), coverImage, false, nullptr);
        INFO("Detail SetData");
		lcnvc->levelPackDetailViewController->SetData(lcnvc->levelPack);
        INFO("Presenting");
		lcnvc->PresentViewControllersForPack();
        */
        
        /*
        lcnvc->SetDataForPack(reinterpret_cast<GlobalNamespace::IBeatmapLevelPack*>(levelPack),
            true,
            showPlayerStatsInDetailView,
            !hidePracticeButton,
            actionButtonText);
        */
        
        lcnvc->SetData(reinterpret_cast<GlobalNamespace::IAnnotatedBeatmapLevelCollection*>(levelPack),
            true,
            showPlayerStatsInDetailView,
            !hidePracticeButton,
            actionButtonText,
            noDataInfoPrefab,
            allowedBeatmapDifficultyMask,
            notAllowedCharacteristics);
        
        INFO("Done Filtering & sorting");
    }

    std::string SongBrowserModel::GetSongHash(std::string_view levelId)
    {
        std::string id(levelId);
        if (id.starts_with("custom_level_")) return id.substr(13);
        else return id;
    }

    Array<GlobalNamespace::IPreviewBeatmapLevel*>* SongBrowserModel::GetLevelsForLevelCollection(GlobalNamespace::IAnnotatedBeatmapLevelCollection* levelCollection)
    {
        return levelCollection->get_beatmapLevelCollection()->get_beatmapLevels();        
    }

#pragma region Filtering
    /* -- Filtering --*/
    List<GlobalNamespace::IPreviewBeatmapLevel*>* SongBrowserModel::FilterOriginal(Array<GlobalNamespace::IPreviewBeatmapLevel*>* levels)
    {
        List<GlobalNamespace::IPreviewBeatmapLevel*>* filtered = List<GlobalNamespace::IPreviewBeatmapLevel*>::New_ctor();
        ArrayUtil::ForEach(levels, [&](auto x){
            filtered->Add(x);
        });
        return filtered;
    }

    List<GlobalNamespace::IPreviewBeatmapLevel*>* SongBrowserModel::FilterFavorites(Array<GlobalNamespace::IPreviewBeatmapLevel*>* levels)
    {
        // new list
        List<GlobalNamespace::IPreviewBeatmapLevel*>* filtered = List<GlobalNamespace::IPreviewBeatmapLevel*>::New_ctor();
        
        // get the favourited level IDs
        auto playerData = ArrayUtil::First(Resources::FindObjectsOfTypeAll<GlobalNamespace::PlayerDataModel*>());
        auto favLevelIds = playerData->get_playerData()->get_favoritesLevelIds();
        // for each item in the list
        ArrayUtil::ForEach(levels, [&](auto x) {
            // if level ID is favd, add to the filtered list
            if (favLevelIds->Contains(x->get_levelID())) filtered->Add(x);
        });
        // return the resulting list  
        return filtered;
    }

    List<GlobalNamespace::IPreviewBeatmapLevel*>* SongBrowserModel::FilterSearch(Array<GlobalNamespace::IPreviewBeatmapLevel*>* levels)
    {
        
        // Make sure we can actually search.
        if (config.searchTerms.size() <= 0)
        {
            ERROR("Tried to search for a song with no valid search terms...");
            return SortSongName(FilterOriginal(levels));
        }

        std::string searchTerm = config.searchTerms[0];
        if (searchTerm == "")
        {
            ERROR("Empty search term entered.");
            return SortSongName(FilterOriginal(levels));
        }

        INFO("Filtering song list by search term: %s", searchTerm.c_str());


        List<GlobalNamespace::IPreviewBeatmapLevel*>* filtered = List<GlobalNamespace::IPreviewBeatmapLevel*>::New_ctor();
        auto terms =  split(searchTerm, " ");
        int length = levels->Length();

        // for every level passed in
        for (int i = 0; i < length; i++)
        {
            auto level = levels->values[i];
            if (!level) continue;
            std::string searchString = "";
            searchString += to_utf8(csstrtostr(level->get_songName()));
            searchString += ' ';
            searchString += to_utf8(csstrtostr(level->get_songSubName()));
            searchString += ' ';
            searchString += to_utf8(csstrtostr(level->get_songAuthorName()));
            searchString += ' ';
            searchString += to_utf8(csstrtostr(level->get_levelAuthorName()));

            #warning add search for song key with songdatacore
            for (auto term : terms)
            {   
                // if the term is found, it's a match!
                if (searchString.find(term) != std::string::npos) filtered->Add(level);
            }
        }

        return filtered;
    }

    List<GlobalNamespace::IPreviewBeatmapLevel*>* SongBrowserModel::FilterRanked(Array<GlobalNamespace::IPreviewBeatmapLevel*>* levels, bool includeRanked, bool includeUnranked)
    {
        List<GlobalNamespace::IPreviewBeatmapLevel*>* filtered = List<GlobalNamespace::IPreviewBeatmapLevel*>::New_ctor();

        ArrayUtil::ForEach(levels, [&](auto x){
            if (!x) return;
            std::string levelId = to_utf8(csstrtostr(x->get_levelID()));
            auto hash = GetSongHash(levelId);
            auto song = SDC_wrapper::BeatStarSong::GetSong(hash);
            if (!song) return;
            auto diffVec = song->GetDifficultyVector();

            bool isRanked = false;
            for (auto diff : diffVec)
            {
                // if only 1 is ranked
                if (diff->ranked)
                {
                    isRanked = true;
                    break;
                }
            }

            // if ranked and we want ranked, add
            if (isRanked && includeRanked)
            {
                filtered->Add(x);
            }
            // else if not ranked and we want not ranked, add
            else if (!isRanked && includeUnranked)
            {
                filtered->Add(x);
            }
        });
        return filtered;
    }

    List<GlobalNamespace::IPreviewBeatmapLevel*>* SongBrowserModel::FilterRequirements(Array<GlobalNamespace::IPreviewBeatmapLevel*>* levels)
    {
        List<GlobalNamespace::IPreviewBeatmapLevel*>* filtered = List<GlobalNamespace::IPreviewBeatmapLevel*>::New_ctor();
        ArrayUtil::ForEach(levels, [&](auto x){
            if (!x) return;
            std::string levelId = to_utf8(csstrtostr(x->get_levelID()));
            auto hash = GetSongHash(levelId);
            auto song = SDC_wrapper::BeatStarSong::GetSong(hash);
            if (!song) return;
            // now we have our song

            auto diffVec = song->GetDifficultyVector();

            for (auto diff : diffVec)
            {
                auto reqVec = diff->GetRequirementVector();
                if (reqVec.size() > 0)
                {
                    // we have requirements
                    filtered->Add(x);
                    return;
                }
            }
        });
        return filtered;
    }

    List<GlobalNamespace::IPreviewBeatmapLevel*>* SongBrowserModel::FilterUnplayed(Array<GlobalNamespace::IPreviewBeatmapLevel*>* levels)
    {
        List<GlobalNamespace::IPreviewBeatmapLevel*>* filtered = List<GlobalNamespace::IPreviewBeatmapLevel*>::New_ctor();
        ArrayUtil::ForEach(levels, [&](auto x){
            if (!x) return;
            std::string levelId = to_utf8(csstrtostr(x->get_levelID()));
            auto itr = levelIdToPlayCount.find(levelId);
            if (itr != levelIdToPlayCount.end() && itr->second == 0) filtered->Add(x);
        });
        return filtered;
    }

#pragma endregion

#pragma region Sorting
    /* -- Sorting --*/
    List<GlobalNamespace::IPreviewBeatmapLevel*>* SongBrowserModel::SortOriginal(List<GlobalNamespace::IPreviewBeatmapLevel*>* levels)
    {
        return levels;
    }

    List<GlobalNamespace::IPreviewBeatmapLevel*>* SongBrowserModel::SortNewest(List<GlobalNamespace::IPreviewBeatmapLevel*>* levels)
    {
        // this is sketch but I think it should work
        std::sort(&levels->items->values[0], &levels->items->values[levels->get_Count()], [&](auto x, auto y) {
            std::string firstLevelID = x ? to_utf8(csstrtostr(x->get_levelID())) : "";
            std::string secondLevelID = y ? to_utf8(csstrtostr(y->get_levelID())) : "";

            auto firstItr = cachedLastWriteTimes.find(firstLevelID);
            auto secondItr = cachedLastWriteTimes.find(secondLevelID);

            if (secondItr == cachedLastWriteTimes.end())
                return false;
            if (firstItr == cachedLastWriteTimes.end())
                return true;
            
            return firstItr->second < secondItr->second;
        });
        return levels;
    }

    List<GlobalNamespace::IPreviewBeatmapLevel*>* SongBrowserModel::SortAuthor(List<GlobalNamespace::IPreviewBeatmapLevel*>* levels)
    {
        // this is sketch but I think it should work
        std::sort(&levels->items->values[0], &levels->items->values[levels->get_Count()], [&](auto x, auto y) {
            std::string firstsongAuthorName = x ? to_utf8(csstrtostr(x->get_songAuthorName())) : "";
            std::string secondsongAuthorName = y ? to_utf8(csstrtostr(y->get_songAuthorName())) : "";

            if (firstsongAuthorName == secondsongAuthorName)
            {
                std::string firstsongName = x ? to_utf8(csstrtostr(x->get_songName())) : "";
                std::string secondsongName = y ? to_utf8(csstrtostr(y->get_songName())) : "";
                return firstsongName < secondsongName;
            }
            else return firstsongAuthorName < secondsongAuthorName;
        });
        return levels;
    }

    List<GlobalNamespace::IPreviewBeatmapLevel*>* SongBrowserModel::SortPlayCount(List<GlobalNamespace::IPreviewBeatmapLevel*>* levels)
    {
        // this is sketch but I think it should work
        std::sort(&levels->items->values[0], &levels->items->values[levels->get_Count()], [&](auto x, auto y) {
            std::string firstlevelID = x ? to_utf8(csstrtostr(x->get_levelID())) : "";
            std::string secondlevelID = y ? to_utf8(csstrtostr(y->get_levelID())) : "";
            
            auto firstItr = levelIdToPlayCount.find(firstlevelID);
            auto secondItr = levelIdToPlayCount.find(secondlevelID);

            // if played the same amount
            if (firstItr->second == secondItr->second)
            {
                std::string firstsongName = x ? to_utf8(csstrtostr(x->get_songName())) : "";
                std::string secondsongName = y ? to_utf8(csstrtostr(y->get_songName())) : "";
                return firstsongName < secondsongName;
            }
            else return firstItr->second < secondItr->second;
        });
        return levels;
    }

    List<GlobalNamespace::IPreviewBeatmapLevel*>* SongBrowserModel::SortPerformancePoints(List<GlobalNamespace::IPreviewBeatmapLevel*>* levels)
    {
        if (!SongDataCoreUtils::get_loaded())
        {
            sortWasMissingData = true;
            return levels;
        }

        std::sort(&levels->items->values[0], &levels->items->values[levels->get_Count()], [&](auto x, auto y) {
            std::string firstlevelID = x ? to_utf8(csstrtostr(x->get_levelID())) : "";
            std::string secondlevelID = y ? to_utf8(csstrtostr(y->get_levelID())) : "";

            auto firstHash = GetSongHash(firstlevelID);
            auto secondHash = GetSongHash(secondlevelID);

            auto firstSong = SDC_wrapper::BeatStarSong::GetSong(firstHash);
            auto secondSong = SDC_wrapper::BeatStarSong::GetSong(secondHash);

            if (!secondSong) return false;
            else if (!firstSong) return true;
            else return firstSong->GetMaxPpValue() < secondSong->GetMaxPpValue();
        });
        return levels;
    }

    List<GlobalNamespace::IPreviewBeatmapLevel*>* SongBrowserModel::SortStars(List<GlobalNamespace::IPreviewBeatmapLevel*>* levels)
    {
        if (!SongDataCoreUtils::get_loaded())
        {
            sortWasMissingData = true;
            return levels;
        }

        std::sort(&levels->items->values[0], &levels->items->values[levels->get_Count()], [&](auto x, auto y) {
            std::string firstlevelID = x ? to_utf8(csstrtostr(x->get_levelID())) : "";
            std::string secondlevelID = y ? to_utf8(csstrtostr(y->get_levelID())) : "";

            auto firstHash = GetSongHash(firstlevelID);
            auto secondHash = GetSongHash(secondlevelID);

            auto firstSong = SDC_wrapper::BeatStarSong::GetSong(firstHash);
            auto secondSong = SDC_wrapper::BeatStarSong::GetSong(secondHash);

            if (!secondSong) return false;
            else if (!firstSong) return true;
            else return firstSong->GetMaxStarValue() < secondSong->GetMaxStarValue();
        });
        return levels;
    }

    List<GlobalNamespace::IPreviewBeatmapLevel*>* SongBrowserModel::SortRandom(List<GlobalNamespace::IPreviewBeatmapLevel*>* levels)
    {
        INFO("Sorting by random seed (seed=%d)", config.randomSongSeed);

        auto rnd = System::Random::New_ctor(config.randomSongSeed);
        std::sort(&levels->items->values[0], &levels->items->values[levels->get_Count()], [&](auto x, auto y) {
            return rnd->Next() < rnd->Next();
        });

        return levels;
    }

    List<GlobalNamespace::IPreviewBeatmapLevel*>* SongBrowserModel::SortSongName(List<GlobalNamespace::IPreviewBeatmapLevel*>* levels)
    {
        // this is sketch but I think it should work
        std::sort(&levels->items->values[0], &levels->items->values[levels->get_Count()], [&](auto x, auto y) {
            std::string firstsongName = x ? to_utf8(csstrtostr(x->get_songName())) : "";
            std::string secondsongName = y ? to_utf8(csstrtostr(y->get_songName())) : "";

            if (firstsongName == secondsongName)
            {
                std::string firstsongAuthorName = x ? to_utf8(csstrtostr(x->get_songAuthorName())) : "";
                std::string secondsongAuthorName = y ? to_utf8(csstrtostr(y->get_songAuthorName())) : "";
                return firstsongAuthorName < secondsongAuthorName;
            }
            else return firstsongName < secondsongName;
        });
        return levels;
    }

    List<GlobalNamespace::IPreviewBeatmapLevel*>* SongBrowserModel::SortSongBpm(List<GlobalNamespace::IPreviewBeatmapLevel*>* levels)
    {
        // this is sketch but I think it should work
        std::sort(&levels->items->values[0], &levels->items->values[levels->get_Count()], [&](auto x, auto y) {
            float firstbpm = x ? x->get_beatsPerMinute() : 0.0f;
            float secondbpm = y ? y->get_beatsPerMinute() : 0.0f;

            if (firstbpm == secondbpm)
            {
                std::string firstsongName = x ? to_utf8(csstrtostr(x->get_songName())) : "";
                std::string secondsongName = y ? to_utf8(csstrtostr(y->get_songName())) : "";
                return firstsongName < secondsongName;
            }
            else return firstbpm < secondbpm;
        });
        return levels;
    }

    List<GlobalNamespace::IPreviewBeatmapLevel*>* SongBrowserModel::SortSongNJS(List<GlobalNamespace::IPreviewBeatmapLevel*>* levels)
    {
        // this is sketch but I think it should work
        std::sort(&levels->items->values[0], &levels->items->values[levels->get_Count()], [&](auto x, auto y) {
            std::string firstlevelID = x ? to_utf8(csstrtostr(x->get_levelID())) : "";
            std::string secondlevelID = y ? to_utf8(csstrtostr(y->get_levelID())) : "";

            auto firstHash = GetSongHash(firstlevelID);
            auto secondHash = GetSongHash(secondlevelID);

            auto firstSong = SDC_wrapper::BeatStarSong::GetSong(firstHash);
            auto secondSong = SDC_wrapper::BeatStarSong::GetSong(secondHash);

            float firstMaxNJS = firstSong ? firstSong->GetMaxNJS() : 0.0f;
            float secondMaxNJS = secondSong ? secondSong->GetMaxNJS() : 0.0f;
            
            if (firstMaxNJS == secondMaxNJS)
            {
                std::string firstsongName = x ? to_utf8(csstrtostr(x->get_songName())) : "";
                std::string secondsongName = y ? to_utf8(csstrtostr(y->get_songName())) : "";
                return firstsongName < secondsongName;
            }
            else return firstMaxNJS < secondMaxNJS;
        });
        return levels;
    }

    List<GlobalNamespace::IPreviewBeatmapLevel*>* SongBrowserModel::SortSongLength(List<GlobalNamespace::IPreviewBeatmapLevel*>* levels)
    {
        // this is sketch but I think it should work
        std::sort(&levels->items->values[0], &levels->items->values[levels->get_Count()], [&](auto x, auto y) {
            float firstLength = x ? x->get_songDuration() : 0.0f;
            float secondLength = y ? y->get_songDuration() : 0.0f;

            if (firstLength == secondLength)
            {
                std::string firstsongName = x ? to_utf8(csstrtostr(x->get_songName())) : "";
                std::string secondsongName = y ? to_utf8(csstrtostr(y->get_songName())) : "";
                return firstsongName < secondsongName;
            }
            else return firstLength < secondLength;
        });
        return levels;
    }

    List<GlobalNamespace::IPreviewBeatmapLevel*>* SongBrowserModel::SortUpVotes(List<GlobalNamespace::IPreviewBeatmapLevel*>* levels)
    {
        if (!SongDataCoreUtils::get_loaded())
        {
            sortWasMissingData = true;
            return levels;
        }

        std::sort(&levels->items->values[0], &levels->items->values[levels->get_Count()], [&](auto x, auto y) {
            std::string firstlevelID = x ? to_utf8(csstrtostr(x->get_levelID())) : "";
            std::string secondlevelID = y ? to_utf8(csstrtostr(y->get_levelID())) : "";

            auto firstHash = GetSongHash(firstlevelID);
            auto secondHash = GetSongHash(secondlevelID);

            auto firstSong = SDC_wrapper::BeatStarSong::GetSong(firstHash);
            auto secondSong = SDC_wrapper::BeatStarSong::GetSong(secondHash);

            if (!secondSong) return false;
            else if (!firstSong) return true;
            else return firstSong->upvotes < secondSong->upvotes;
        });
        return levels;
    }

    List<GlobalNamespace::IPreviewBeatmapLevel*>* SongBrowserModel::SortBeatSaverPlayCount(List<GlobalNamespace::IPreviewBeatmapLevel*>* levels)
    {
        return levels;
        /*
        if (!SongDataCoreUtils::get_loaded())
        {
            sortWasMissingData = true;
            return levels;
        }

        std::sort(&levels->items->values[0], &levels->items->values[levels->get_Count()], [&](auto x, auto y) {
            std::string firstlevelID = x ? to_utf8(csstrtostr(x->get_levelID())) : "";
            std::string secondlevelID = y ? to_utf8(csstrtostr(y->get_levelID())) : "";

            auto firstHash = GetSongHash(firstlevelID);
            auto secondHash = GetSongHash(secondlevelID);

            auto firstSong = SDC_wrapper::BeatStarSong::GetSong(firstHash);
            auto secondSong = SDC_wrapper::BeatStarSong::GetSong(secondHash);

            if (!secondSong) return false;
            else if (!firstSong) return true;
            else return firstSong->plays < secondSong->plays;
        });
        return levels;
        */
    }

    List<GlobalNamespace::IPreviewBeatmapLevel*>* SongBrowserModel::SortBeatSaverDownloads(List<GlobalNamespace::IPreviewBeatmapLevel*>* levels)
    {
        if (!SongDataCoreUtils::get_loaded())
        {
            sortWasMissingData = true;
            return levels;
        }

        std::sort(&levels->items->values[0], &levels->items->values[levels->get_Count()], [&](auto x, auto y) {
            std::string firstlevelID = x ? to_utf8(csstrtostr(x->get_levelID())) : "";
            std::string secondlevelID = y ? to_utf8(csstrtostr(y->get_levelID())) : "";

            auto firstHash = GetSongHash(firstlevelID);
            auto secondHash = GetSongHash(secondlevelID);

            auto firstSong = SDC_wrapper::BeatStarSong::GetSong(firstHash);
            auto secondSong = SDC_wrapper::BeatStarSong::GetSong(secondHash);

            if (!secondSong) return false;
            else if (!firstSong) return true;
            else return firstSong->downloads < secondSong->downloads;
        });
        return levels;
    }

    List<GlobalNamespace::IPreviewBeatmapLevel*>* SongBrowserModel::SortBeatSaverRating(List<GlobalNamespace::IPreviewBeatmapLevel*>* levels)
    {
        if (!SongDataCoreUtils::get_loaded())
        {
            sortWasMissingData = true;
            return levels;
        }

        std::sort(&levels->items->values[0], &levels->items->values[levels->get_Count()], [&](auto x, auto y) {
            std::string firstlevelID = x ? to_utf8(csstrtostr(x->get_levelID())) : "";
            std::string secondlevelID = y ? to_utf8(csstrtostr(y->get_levelID())) : "";

            auto firstHash = GetSongHash(firstlevelID);
            auto secondHash = GetSongHash(secondlevelID);

            auto firstSong = SDC_wrapper::BeatStarSong::GetSong(firstHash);
            auto secondSong = SDC_wrapper::BeatStarSong::GetSong(secondHash);

            if (!secondSong) return false;
            else if (!firstSong) return true;
            else return firstSong->GetRating() < secondSong->GetRating();
        });
        return levels;
    }

    List<GlobalNamespace::IPreviewBeatmapLevel*>* SongBrowserModel::SortBeatSaverHeat(List<GlobalNamespace::IPreviewBeatmapLevel*>* levels)
    {
        if (!SongDataCoreUtils::get_loaded())
        {
            sortWasMissingData = true;
            return levels;
        }

        std::sort(&levels->items->values[0], &levels->items->values[levels->get_Count()], [&](auto x, auto y) {
            std::string firstlevelID = x ? to_utf8(csstrtostr(x->get_levelID())) : "";
            std::string secondlevelID = y ? to_utf8(csstrtostr(y->get_levelID())) : "";

            auto firstHash = GetSongHash(firstlevelID);
            auto secondHash = GetSongHash(secondlevelID);

            auto firstSong = SDC_wrapper::BeatStarSong::GetSong(firstHash);
            auto secondSong = SDC_wrapper::BeatStarSong::GetSong(secondHash);

            if (!secondSong) return false;
            else if (!firstSong) return true;
            else return firstSong->heat < secondSong->heat;
        });
        return levels;
    }
#pragma endregion

    double SongBrowserModel::GetSongUserDate(GlobalNamespace::CustomPreviewBeatmapLevel* level)
    {
        // not converting to cpp if all I need it for now is just c# stuff
        auto coverPath = System::IO::Path::Combine(level->get_customLevelPath(), level->get_standardLevelInfoSaveData()->get_coverImageFilename());
        System::DateTime lastTime;
        if (System::IO::File::Exists(coverPath))
        {
            auto lastWriteTime = System::IO::File::GetLastWriteTime(coverPath);
            auto lastCreateTime = System::IO::File::GetCreationTime(coverPath);
            lastTime = lastWriteTime > lastCreateTime ? lastWriteTime : lastCreateTime;
        }
        else
        {
            auto lastCreateTime = System::IO::File::GetCreationTime(level->get_customLevelPath());
            lastTime = lastCreateTime;
        }

        return (lastTime - EPOCH).get_TotalMilliseconds();
    }
}