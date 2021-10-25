#include "UI/Browser/SongBrowserUI.hpp"
#include "SongBrowserApplication.hpp"

#include "modloader/shared/modloader.hpp"

#include "UnityEngine/Object.hpp"
#include "UnityEngine/Resources.hpp"
#include "UnityEngine/Transform.hpp"
#include "UnityEngine/GameObject.hpp"
#include "UnityEngine/WaitForEndOfFrame.hpp"

#include "HMUI/ViewController.hpp"
#include "HMUI/ViewController_AnimationDirection.hpp"
#include "HMUI/TitleViewController.hpp"
#include "HMUI/CurvedCanvasSettings.hpp"
#include "HMUI/ScrollView.hpp"

#include "System/Linq/Enumerable.hpp"
#include "System/Collections/Generic/IEnumerable_1.hpp"

#include "GlobalNamespace/IBeatmapLevelPackCollection.hpp"
#include "GlobalNamespace/IBeatmapLevel.hpp"
#include "GlobalNamespace/PreviewBeatmapLevelPackSO.hpp"
#include "GlobalNamespace/IDifficultyBeatmap.hpp"
#include "GlobalNamespace/SoloFreePlayFlowCoordinator.hpp"
#include "GlobalNamespace/PartyFreePlayFlowCoordinator.hpp"
#include "GlobalNamespace/MultiplayerLevelSelectionFlowCoordinator.hpp"
#include "GlobalNamespace/LevelParamsPanel.hpp"

#include "Utils/EventUtils.hpp"
#include "Utils/ArrayUtil.hpp"
#include "Utils/UIUtils.hpp"
#include "Utils/SpriteUtils.hpp"
#include "Utils/EnumToStringUtils.hpp"
#include "Utils/SongDataCoreUtils.hpp"

#include "sdc-wrapper/shared/BeatStarSong.hpp"

#include "questui/shared/BeatSaberUI.hpp"
#include "logging.hpp"

#include <unordered_map>
#include "System/Enum.hpp"

#include "sombrero/shared/RandomUtils.hpp"

#include "songloader/shared/API.hpp"

using namespace UnityEngine;
using namespace UnityEngine::UI;

DEFINE_TYPE(SongBrowser::UI, SongBrowserUI);
DEFINE_TYPE(SongBrowser::UI, SongBrowserViewController);

namespace SongBrowser::UI
{
    void SongBrowserUI::ctor()
    {
        INVOKE_CTOR();
    }

    void SongBrowserUI::Show()
    {
        SetVisibility(true);
    }

    void SongBrowserUI::Hide(bool dontHideFields)
    {
        SetVisibility(false, dontHideFields);
    }

    void SongBrowserUI::UpdateLevelDataModel()
    {
        // get a current beatmap characteristic...
        if (!model->currentBeatmapCharacteristicSO && uiCreated)
        {
            model->currentBeatmapCharacteristicSO = beatUi->BeatmapCharacteristicSelectionViewController->selectedBeatmapCharacteristic;
        }

        model->UpdateLevelRecords();
    }

    bool SongBrowserUI::UpdateLevelCollectionSelection()
    {
        if (uiCreated)
        {
            auto currentSelected = beatUi->GetCurrentSelectedAnnotatedBeatmapLevelCollection();

            // select category
            if (config.currentLevelCategoryName != "")
            {
                selectingCategory = true;
                beatUi->SelectLevelCategory(config.currentLevelCategoryName);
                selectingCategory = false;
            }


            // select collection
            if (config.currentLevelCollectionName == "")
            {
                if (!currentSelected && config.currentLevelCategoryName == "")
                {
                    INFO("No level collection selected, acquiring the first available, likely OST1...");
                    currentSelected = reinterpret_cast<GlobalNamespace::IAnnotatedBeatmapLevelCollection*>(beatUi->BeatmapLevelsModel->get_allLoadedBeatmapLevelPackCollection()->get_beatmapLevelPacks()->values[0]);
                }
            }
            else if (!currentSelected || (to_utf8(csstrtostr(currentSelected->get_collectionName())) != config.currentLevelCollectionName))
            {
                EventUtils::DidSelectAnnotatedBeatmapLevelCollection() -= {&SongBrowserUI::_levelFilteringNavController_didSelectAnnotatedBeatmapLevelCollectionEvent, this};

                lastLevelCollection = beatUi->GetLevelCollectionByName(config.currentLevelCollectionName);
                if (il2cpp_utils::try_cast<GlobalNamespace::PreviewBeatmapLevelPackSO>(lastLevelCollection))
                    Hide();
                
                beatUi->SelectLevelCollection(config.currentLevelCollectionName);
                EventUtils::DidSelectAnnotatedBeatmapLevelCollection() += {&SongBrowserUI::_levelFilteringNavController_didSelectAnnotatedBeatmapLevelCollectionEvent, this};
            }

            if (!lastLevelCollection)
            {
                if (currentSelected)
                {
                    std::string collectionName = to_utf8(csstrtostr(currentSelected->get_collectionName()));
                    if (strcmp(collectionName.c_str(), SongBrowserModel::filteredSongsCollectionName) && strcmp(collectionName.c_str(), SongBrowserModel::playlistSongsCollectionName))
                    {
                        lastLevelCollection = currentSelected;
                    }
                }
            }

            ProcessSongList();
        }

        return false;
    }

    void SongBrowserUI::RefreshSongList()
    {
        if (!uiCreated)
            return;
        beatUi->RefreshSongList(model->lastSelectedLevelId);
    }

#pragma region creation
    // builds the ui
    void SongBrowserUI::CreateUI(GlobalNamespace::MainMenuViewController::MenuButton mode)
    {
        GlobalNamespace::LevelSelectionFlowCoordinator* flowCoordinator = nullptr;
        switch(mode)
        {
            case GlobalNamespace::MainMenuViewController::MenuButton::SoloFreePlay:
                flowCoordinator = ArrayUtil::Last(Resources::FindObjectsOfTypeAll<GlobalNamespace::SoloFreePlayFlowCoordinator*>());
                break;
            case GlobalNamespace::MainMenuViewController::MenuButton::Party:
                flowCoordinator = ArrayUtil::Last(Resources::FindObjectsOfTypeAll<GlobalNamespace::PartyFreePlayFlowCoordinator*>());
                break;
            case GlobalNamespace::MainMenuViewController::MenuButton::Multiplayer:
                flowCoordinator = ArrayUtil::Last(Resources::FindObjectsOfTypeAll<GlobalNamespace::MultiplayerLevelSelectionFlowCoordinator*>());
                break;
            default:
                return;
        }

        beatUi = *il2cpp_utils::New<SongBrowser::DataAccess::BeatSaberUIController*>(flowCoordinator);
        lastLevelCollection = nullptr;
        
        auto screenContainer = ArrayUtil::First(Resources::FindObjectsOfTypeAll<Transform*>(), [](auto x) {
                static Il2CppString* screenContainerName = il2cpp_utils::newcsstr<il2cpp_utils::CreationType::Manual>("ScreenContainer");
                return screenContainerName->Equals(x->get_name());
            });

        INFO("screenContainer: %p", screenContainer);
        
        auto curvedCanvasSettings = screenContainer->GetComponent<HMUI::CurvedCanvasSettings*>();
        INFO("curvedCanvasSettings: %p", curvedCanvasSettings);

        INFO("viewController: %p", viewController);
        if (uiCreated)
        {
            auto vcCanvasSettings = viewController->GetComponent<HMUI::CurvedCanvasSettings*>();
            INFO("vcCanvasSettings: %p", vcCanvasSettings);
            vcCanvasSettings->SetRadius(curvedCanvasSettings->get_radius());
            return;
        }

        if (viewController)
        {
            Object::Destroy(viewController);
        }

        INFO("creating view controller");
        viewController = UIUtils::CreateCurvedViewController<SongBrowser::UI::SongBrowserViewController*>("SongBrowserViewController", curvedCanvasSettings->get_radius());
        INFO("viewController: %p", viewController);
        auto rectTransform = viewController->get_rectTransform();
        rectTransform->SetParent(beatUi->LevelCollectionNavigationController->get_transform(), false);
        rectTransform->set_anchorMin(Vector2(0.0f, 0.0f));
        rectTransform->set_anchorMax(Vector2(1.0f, 1.0f));
        rectTransform->set_anchoredPosition(Vector2(0.0f, 0.0f));
        rectTransform->set_sizeDelta(Vector2(curvedCanvasSettings->get_radius(), 25));
        viewController->get_gameObject()->SetActive(true);

        CreateOuterUI();
        CreateSortButtons();
        CreateFilterButtons();
        CreateDeleteUI();
        CreateFastPageButtons();

        InstallHandlers();

        ModifySongStatsPanel();
        ResizeSongUI();

        uiCreated = true;

        RefreshSortButtonUI();
        StartCoroutine(reinterpret_cast<System::Collections::IEnumerator*>(custom_types::Helpers::CoroutineHelper::New(ProcessSongListEndOfFrame())));
    }
    void SongBrowserUI::CreateOuterUI()
    {
        INFO("CreateOuterUI");
        static constexpr const float clearButtonX = -72.5f;
        static constexpr const float clearButtonY = CLEAR_BUTTON_Y;
        static constexpr const float buttonY = BUTTON_ROW_Y;
        static constexpr const float buttonHeight = 5.0f;
        static constexpr const float sortByButtonX = -62.5f + buttonHeight;
        static constexpr const float outerButtonFontSize = 3.0f;
        static constexpr const float displayButtonFontSize = 2.5f;
        static constexpr const float outerButtonWidth = 24.0f;
        static constexpr const float randomButtonWidth = 10.0f;
        
                    // clear button
        clearSortFilterButton = UIUtils::CreateIconButton( "ClearSortAndFilterButton", viewController->get_transform(), "PracticeButton", Vector2(clearButtonX, clearButtonY), Vector2(randomButtonWidth, randomButtonWidth),
            [&]() {
                if (currentUiState == UIState::FilterBy || currentUiState == UIState::SortBy)
                {
                    RefreshOuterUIState(UIState::Main);
                }
                else
                {
                    OnClearButtonClickEvent();
                }
            },
            SpriteUtils::get_XIcon(),
            "Clear");
        UIUtils::SetButtonBackgroundActive(clearSortFilterButton, false);

        // create SortBy button and its display
        float curX = sortByButtonX;

        INFO("Creating Sort By...");
        sortByButton = UIUtils::CreateUIButton("sortBy", viewController->get_transform(), "PracticeButton", Vector2(curX, buttonY), Vector2(outerButtonWidth, buttonHeight), 
            [&]() {
                    RefreshOuterUIState(UIState::SortBy);
        }, "Sort By");
        UIUtils::SetButtonTextSize(sortByButton, outerButtonFontSize);
        UIUtils::ToggleWordWrapping(sortByButton, false);

        curX += outerButtonWidth;

        INFO("Creating Sort By Display...");
        sortByDisplay = UIUtils::CreateUIButton("sortByValue", viewController->get_transform(), "PracticeButton", Vector2(curX, buttonY), Vector2(outerButtonWidth, buttonHeight), 
        [&]() {
            OnSortButtonClickEvent(config.sortMode);
        }, "");
        UIUtils::SetButtonTextSize(sortByDisplay, displayButtonFontSize);
        UIUtils::ToggleWordWrapping(sortByDisplay, false);

        curX += outerButtonWidth;

        // create FilterBy button and its display
        INFO("Creating Filter By...");
        filterByButton = UIUtils::CreateUIButton("filterBy", viewController->get_transform(), "PracticeButton", Vector2(curX, buttonY), Vector2(outerButtonWidth, buttonHeight), 
        [&]() {
            RefreshOuterUIState(UIState::FilterBy);
        }, "Filter By");
        UIUtils::SetButtonTextSize(filterByButton, outerButtonFontSize);
        UIUtils::ToggleWordWrapping(filterByButton, false);

        curX += outerButtonWidth;

        INFO("Creating Filter By Display...");
        filterByDisplay = UIUtils::CreateUIButton("filterValue", viewController->get_transform(), "PracticeButton", Vector2(curX, buttonY), Vector2(outerButtonWidth, buttonHeight), 
        [&]() {
            config.filterMode = SongFilterMode::None;
            CancelFilter();
            ProcessSongList();
            RefreshSongUI();
        }, "");
        UIUtils::SetButtonTextSize(filterByDisplay, displayButtonFontSize);
        UIUtils::ToggleWordWrapping(filterByDisplay, false);

        curX += (outerButtonWidth / 2.0f);

        // random button
        INFO("Creating Random Button...");
        randomButton = UIUtils::CreateIconButton("randomButton", viewController->get_transform(), "PracticeButton", Vector2(curX + (randomButtonWidth / 4.0f), clearButtonY), Vector2(randomButtonWidth, randomButtonWidth), 
        [&]() {
            OnSortButtonClickEvent(SongSortMode::Random);
        }, SpriteUtils::get_RandomIcon(), "Random");
        UIUtils::SetButtonBackgroundActive(randomButton, false);

        curX += (randomButtonWidth / 4.0f) * 2.0f;

        // playlist export
        INFO("Creating playlist export button...");
        playlistExportButton = nullptr;
        /*
        // we dont need no playlist button
        playlistExportButton =  UIUtils::CreateIconButton("playlistExportButton", viewController->get_transform(), "PracticeButton", Vector2(curX + (randomButtonWidth / 4.0f), clearButtonY), Vector2(randomButtonWidth, randomButtonWidth), 
        [&]() {
            ShowInputKeyboard(std::bind(&SongBrowserUI::CreatePlaylistButtonPressed, this, std::placeholders::_1));
        }, SpriteUtils::get_PlaylistIcon(), "Export Playlist");
        UIUtils::SetButtonBackgroundActive(playlistExportButton, false);
        playlistExportButton->set_interactable(false);
        UIUtils::SetHoverHint(playlistExportButton->get_transform(), "Playlist Export Currently Disabled!_hoverHintText", "Playlist Export Currently Disabled!");
        */
    }

    void SongBrowserUI::CreateSortButtons()
    {
        INFO("Create sort buttons...");
        static constexpr const float sortButtonFontSize = 2.0f;
        static constexpr const float sortButtonX = -63.0f;
        static constexpr const float sortButtonWidth = 10.50f;//= 11.75f;
        static constexpr const float buttonSpacing = 0.25f;
        static constexpr const float buttonY = BUTTON_ROW_Y;
        static constexpr const float buttonHeight = 5.0f;

        std::unordered_map<std::string, SongSortMode> sortModes = {
            {"Title", SongSortMode::Default},
            {"Author", SongSortMode::Author},
            {"Newest", SongSortMode::Newest},
            {"Plays", SongSortMode::YourPlayCount},
            {"BPM", SongSortMode::Bpm},
            {"Length", SongSortMode::Length},
            {"NJS", SongSortMode::NJS},
            {"PP", SongSortMode::PP},
            {"Stars", SongSortMode::Stars},
            {"UpVotes", SongSortMode::UpVotes},
            {"Rating", SongSortMode::Rating},
            {"Heat", SongSortMode::Heat},
            {"Downloads", SongSortMode::Downloads}
        };

        sortButtonGroup = List<SongSortButton*>::New_ctor();
        int i = 0;
        for (auto& p : sortModes)
        {
            float curButtonX = sortButtonX + (sortButtonWidth * i) + (buttonSpacing * i);
            auto sortButton = *il2cpp_utils::New<SongSortButton*>();
            sortButton->sortMode = p.second;
            sortButton->button = UIUtils::CreateUIButton(string_format("Sort%sButton", p.first.c_str()), viewController->get_transform(), "PracticeButton",
                Vector2(curButtonX, buttonY), Vector2(sortButtonWidth, buttonHeight),
                [&, sortButton]() -> void {
                    OnSortButtonClickEvent(sortButton->sortMode);
                    RefreshOuterUIState(UIState::Main);
                }, p.first);
            UIUtils::SetButtonTextSize(sortButton->button, sortButtonFontSize);
            UIUtils::ToggleWordWrapping(sortButton->button, false);

            sortButtonGroup->Add(sortButton);
            i++;
        }
    }

    void SongBrowserUI::CreateFilterButtons()
    {
        INFO("Creating filter buttons...");
        static constexpr const float filterButtonFontSize = 2.25f;
        static constexpr const float filterButtonX = -63.0f;
        static constexpr const float filterButtonWidth = 14.25f;
        static constexpr const float buttonSpacing = 0.5f;
        static constexpr const float buttonY = BUTTON_ROW_Y;
        static constexpr const float buttonHeight = 5.0f;

        std::unordered_map<std::string, SongFilterMode> filterModes = {
            {"Unplayed", SongFilterMode::Unplayed},
            {"Requirements", SongFilterMode::Requirements},
            {"Unranked", SongFilterMode::Unranked},
            {"Ranked", SongFilterMode::Ranked},
        };

        filterButtonGroup = List<SongFilterButton*>::New_ctor();
        int i = 0;
        for (auto& p : filterModes)
        {
            INFO("Creating button %s", p.first.c_str());

            float curButtonX = filterButtonX + (filterButtonWidth * i) + (buttonSpacing * i);
            /*
            if (i == 0)
            {
                // skip search for now
                i++;
                continue;
                QuestUI::BeatSaberUI::CreateStringSetting(viewController->get_transform(), "Search", "", Vector2(curButtonX, buttonY), [&](std::string value){
                    if (value.back() == '\n')
                    {
                        SearchViewControllerSearchButtonPressed(value.substr(0, value.size() - 1));
                    }
                })->GetComponent<RectTransform*>()->set_sizeDelta(Vector2(filterButtonWidth, buttonHeight));
                continue;
            }
            */

            auto filterButton = *il2cpp_utils::New<SongFilterButton*>();
            filterButton->filterMode = p.second;
            filterButton->button = UIUtils::CreateUIButton(string_format("Filter%sButton", p.first.c_str()), viewController->get_transform(), "PracticeButton",
                Vector2(curButtonX, buttonY), Vector2(filterButtonWidth, buttonHeight),
                [&, filterButton]() -> void {
                    OnFilterButtonClickEvent(filterButton->filterMode);
                    RefreshOuterUIState(UIState::Main);
                }, p.first);
            UIUtils::SetButtonTextSize(filterButton->button, filterButtonFontSize);
            UIUtils::ToggleWordWrapping(filterButton->button, false);
/*     >>>>>>> NOT REQUIRED IN QUEST
            // don't allow to filter for reqs if no cjd
            if (p.second == SongFilterMode::Requirements)
            {
                auto modList = Modloader::getMods();
                auto itr = modList.find("CustomJSONData");
                if (itr != modList.end())
                {
                    // set interactable to whether or not cjd is loaded
                    filterButton->button->set_interactable(itr->second.get_loaded());
                }
                else filterButton->button->set_interactable(false);

            }
*/
            filterButtonGroup->Add(filterButton);
            i++;
        }
    }

    void SongBrowserUI::CreateFastPageButtons()
    {
        INFO("Creating fast scroll button...");
        pageUpFastButton = UIUtils::CreatePageButton("PageUpFast",
            beatUi->LevelCollectionNavigationController->get_transform(), "UpButton",
            Vector2(2.0f, 24.0f), Vector2(8.0f, 8.0f),
            [&]() -> void {
                JumpSongList(-1, SEGMENT_PERCENT);
            }, SpriteUtils::get_DoubleArrow());

        pageDownFastButton = UIUtils::CreatePageButton("PageDownFast",
            beatUi->LevelCollectionNavigationController->get_transform(), "DownButton",
            Vector2(2.0f, -24.0f), Vector2(8.0f, 8.0f),
            [&]() -> void {
                JumpSongList(1, SEGMENT_PERCENT);
            }, SpriteUtils::get_DoubleArrow());
    }

    void SongBrowserUI::CreateDeleteUI()
    {
        INFO("Creating delete dialog...");
        INFO("Due to songloader already implementing deletion we don't need to do it here ^_^");
        /*
        static auto deleteLevelButtonName = il2cpp_utils::newcsstr<il2cpp_utils::CreationType::Manual>("DeleteLevelButton");
        auto transform = beatUi->StandardLevelDetailView->practiceButton->get_transform()->get_parent()->Find(deleteLevelButtonName);

        if (transform) 
        {
            deleteButton = transform->get_gameObject()->GetComponentInChildren<UnityEngine::UI::Button*>();
            std::function<void(void)> fun = std::bind(&SongBrowserUI::HandleDeleteSelectedLevel, this);
            deleteButton->get_onClick()->AddListener(il2cpp_utils::MakeDelegate<Events::UnityAction*>(classof(Events::UnityAction*), fun));
        }
        */
        /*
        deleteDialog = Object::Instantiate<GlobalNamespace::SimpleDialogPromptViewController*>(beatUi->SimpleDialogPromptViewControllerPrefab);
        deleteDialog->GetComponent<VRUIControls::VRGraphicRaycaster*>()->physicsRaycaster = UIUtils::get_PhysicsRaycasterWithCache();
        deleteDialog->set_name(il2cpp_utils::newcsstr("DeleteDialogPromptViewController"));
        deleteDialog->get_gameObject()->SetActive(false);

        INFO("Creating delete button...");
        deleteButton = UIUtils::CreateIconButton("DeleteLevelButton_SB", reinterpret_cast<UnityEngine::Transform*>(beatUi->ActionButtons), "PracticeButton", SpriteUtils::get_DeleteIcon(), "Delete Level");
        deleteButton->get_transform()->SetAsFirstSibling();

        std::function<void(void)> fun = std::bind(&SongBrowserUI::HandleDeleteSelectedLevel, this);
        deleteButton->get_onClick()->AddListener(il2cpp_utils::MakeDelegate<Events::UnityAction*>(classof(Events::UnityAction*), fun));
        */
    }
#pragma endregion
    
    void SongBrowserUI::ModifySongStatsPanel()
    {
        // modify stat panel, inject extra row of stats
        INFO("Resizing Stats Panel...");

        auto statsPanel = beatUi->StandardLevelDetailView->levelParamsPanel;
        reinterpret_cast<RectTransform*>(statsPanel->get_transform())->Translate(0.0f, 0.05f, 0.0f);

        auto NPS_cs = il2cpp_utils::newcsstr("NPS");
        auto NotesCount_cs = il2cpp_utils::newcsstr("NotesCount");
        auto ObstaclesCount_cs = il2cpp_utils::newcsstr("ObstaclesCount");
        auto BombsCount_cs = il2cpp_utils::newcsstr("BombsCount");

        ppStatButton = reinterpret_cast<UnityEngine::RectTransform*>(UIUtils::CreateStatIcon("PPStatLabel",
            reinterpret_cast<UnityEngine::Transform*>(ArrayUtil::First(statsPanel->get_gameObject()->GetComponentsInChildren<RectTransform*>(), [NPS_cs](auto x) {
                return x->get_name()->Equals(NPS_cs); 
            })),
            statsPanel->get_transform(),
            SpriteUtils::get_GraphIcon(),
            "PP Value"));

        starStatButton = reinterpret_cast<UnityEngine::RectTransform*>(UIUtils::CreateStatIcon("StarStatLabel",
            reinterpret_cast<UnityEngine::Transform*>(ArrayUtil::First(statsPanel->get_gameObject()->GetComponentsInChildren<RectTransform*>(), [NotesCount_cs](auto x) {
                return x->get_name()->Equals(NotesCount_cs); 
            })),
            statsPanel->get_transform(),
            SpriteUtils::get_StarFullIcon(),
            "Star Difficulty Rating"));

        njsStatButton = reinterpret_cast<UnityEngine::RectTransform*>(UIUtils::CreateStatIcon("NoteJumpSpeedLabel",
            reinterpret_cast<UnityEngine::Transform*>(ArrayUtil::First(statsPanel->get_gameObject()->GetComponentsInChildren<RectTransform*>(), [ObstaclesCount_cs](auto x) {
                return x->get_name()->Equals(ObstaclesCount_cs); 
            })),
            statsPanel->get_transform(),
            SpriteUtils::get_SpeedIcon(),
            "Note Jump Speed"));

        noteJumpStartBeatOffsetLabel = reinterpret_cast<UnityEngine::RectTransform*>(UIUtils::CreateStatIcon("NoteJumpStartBeatOffsetLabel",
            reinterpret_cast<UnityEngine::Transform*>(ArrayUtil::First(statsPanel->get_gameObject()->GetComponentsInChildren<RectTransform*>(), [BombsCount_cs](auto x) {
                return x->get_name()->Equals(BombsCount_cs); 
            })),
            statsPanel->get_transform(),
            SpriteUtils::get_NoteStartOffsetIcon(),
                "Note Jump Start Beat Offset"));
    }

    void SongBrowserUI::ResizeSongUI()
    {
        // shrink play button container
        beatUi->ActionButtons->set_localScale(Vector3(0.875f, 0.875f, 0.875f));
    }

    void SongBrowserUI::InstallHandlers()
    {
        // update stats
        EventUtils::DidSelectLevel() -= {&SongBrowserUI::OnDidSelectLevelEvent, this};
        EventUtils::DidSelectLevel() += {&SongBrowserUI::OnDidSelectLevelEvent, this};

        EventUtils::DidChangeContent() -= {&SongBrowserUI::OnDidPresentContentEvent, this};
        EventUtils::DidChangeContent() += {&SongBrowserUI::OnDidPresentContentEvent, this};

        EventUtils::DidChangeDifficultyBeatmap() -= {&SongBrowserUI::OnDidChangeDifficultyEvent, this};
        EventUtils::DidChangeDifficultyBeatmap() += {&SongBrowserUI::OnDidChangeDifficultyEvent, this};

        // update our view of the game state
        EventUtils::DidSelectAnnotatedBeatmapLevelCollection() -= {&SongBrowserUI::_levelFilteringNavController_didSelectAnnotatedBeatmapLevelCollectionEvent, this};
        EventUtils::DidSelectAnnotatedBeatmapLevelCollection() += {&SongBrowserUI::_levelFilteringNavController_didSelectAnnotatedBeatmapLevelCollectionEvent, this};

        EventUtils::DidSelectAnnotatedBeatmapLevelCollectionEvent_1Arg() -= {&SongBrowserUI::handleDidSelectAnnotatedBeatmapLevelCollection, this};
        EventUtils::DidSelectAnnotatedBeatmapLevelCollectionEvent_1Arg() += {&SongBrowserUI::handleDidSelectAnnotatedBeatmapLevelCollection, this};

        // Respond to characteristics changes
        EventUtils::DidSelectBeatmapCharacteristic() -= {&SongBrowserUI::OnDidSelectBeatmapCharacteristic, this};
        EventUtils::DidSelectBeatmapCharacteristic() += {&SongBrowserUI::OnDidSelectBeatmapCharacteristic, this};

        EventUtils::DidSelectLevelCategory() -= {&SongBrowserUI::OnDidSelectLevelCategory, this};
        EventUtils::DidSelectLevelCategory() += {&SongBrowserUI::OnDidSelectLevelCategory, this};


        std::function<void(void)> fun = [this](){
            this->StartCoroutine(reinterpret_cast<System::Collections::IEnumerator*>(custom_types::Helpers::CoroutineHelper::New(this->RefreshQuickScrollButtonsAsync())));
        };

        auto delegate = il2cpp_utils::MakeDelegate<UnityEngine::Events::UnityAction*>(classof(UnityEngine::Events::UnityAction*), fun);
        // make sure the quick scroll buttons don't desync with regular scrolling
        beatUi->TableViewPageDownButton->get_onClick()->AddListener(delegate);
        beatUi->TableViewPageUpButton->get_onClick()->AddListener(delegate);

        // stop add favorites from scrolling to the top
        EventUtils::DidFavoriteToggleChange() += {&SongBrowserUI::OnDidFavoriteToggleChangeEvent, this};
    }

    void SongBrowserUI::OnDidFavoriteToggleChangeEvent(GlobalNamespace::StandardLevelDetailView* arg1, UnityEngine::UI::Toggle* arg2)
    {
        if (config.currentLevelCategoryName == "Favorites")
        {
            // TODO - still scrolls to top in this view
        }
        else
        {
            StartCoroutine(reinterpret_cast<System::Collections::IEnumerator*>(custom_types::Helpers::CoroutineHelper::New(AsyncForceScrollToPosition(model->lastScrollIndex))));
        }
    }

    void SongBrowserUI::OnDidSelectLevelCategory(GlobalNamespace::SelectLevelCategoryViewController* viewController, GlobalNamespace::SelectLevelCategoryViewController::LevelCategory levelCategory)
    {
        switch (levelCategory)
        {
            case GlobalNamespace::SelectLevelCategoryViewController::LevelCategory::None: [[fallthrough]];
            case GlobalNamespace::SelectLevelCategoryViewController::LevelCategory::OstAndExtras:  [[fallthrough]];
            case GlobalNamespace::SelectLevelCategoryViewController::LevelCategory::CustomSongs: [[fallthrough]];
            default:
                break;
            case GlobalNamespace::SelectLevelCategoryViewController::LevelCategory::MusicPacks:  [[fallthrough]];
            case GlobalNamespace::SelectLevelCategoryViewController::LevelCategory::Favorites: [[fallthrough]];
            case GlobalNamespace::SelectLevelCategoryViewController::LevelCategory::All:
                Hide(true);
                break;
        }
        
        ProcessSongList();
        RefreshSongUI();
    }

    custom_types::Helpers::Coroutine SongBrowserUI::AsyncForceScrollToPosition(float position)
    {
        INFO("Will attempt force scrolling to position [%.2f] at end of frame.", position);

        co_yield reinterpret_cast<System::Collections::IEnumerator*>(UnityEngine::WaitForEndOfFrame::New_ctor());

        auto tv = beatUi->LevelCollectionTableView->tableView;
        auto sv = tv->scrollView;
        INFO("Force scrolling to %.2f", position);
        sv->ScrollTo(position, false);
        co_return;
    }

    custom_types::Helpers::Coroutine SongBrowserUI::AsyncWaitForSongUIUpdate()
    {
        if (asyncUpdating || !uiCreated || !model->get_sortWasMissingData())
            co_return;

        asyncUpdating = true;

        while (beatUi && (
               beatUi->LevelSelectionNavigationController->get_isInTransition() ||
               beatUi->LevelDetailViewController->get_isInTransition() ||
               !beatUi->LevelSelectionNavigationController->get_isInViewControllerHierarchy() ||
               !beatUi->LevelDetailViewController->get_isInViewControllerHierarchy() ||
               !beatUi->LevelSelectionNavigationController->get_isActiveAndEnabled() ||
               !beatUi->LevelDetailViewController->get_isActiveAndEnabled()))
        {
            co_yield nullptr;
        }

        if (NeedsScoreSaberData(config.sortMode) && SongDataCoreUtils::get_loaded())
        {
            ProcessSongList();
            RefreshSongUI();
        }

        asyncUpdating = false;
        co_return;
    }

    void SongBrowserUI::RefreshSongUI(bool scrollToLevel)
    {
        if (!uiCreated)
            return;

        RefreshSongList();
        RefreshSortButtonUI();
        
        if (!scrollToLevel)
            beatUi->ScrollToLevelByRow(0);

        RefreshQuickScrollButtons();
        RefreshCurrentSelectionDisplay();
    }

    void SongBrowserUI::ProcessSongList()
    {
        if (!uiCreated)
            return;

        // dont do stuff on the filter menu
        switch (beatUi->LevelFilteringNavigationController->get_selectedLevelCategory())
        {
            case GlobalNamespace::SelectLevelCategoryViewController::LevelCategory::None: [[fallthrough]];
            case GlobalNamespace::SelectLevelCategoryViewController::LevelCategory::OstAndExtras:  [[fallthrough]];
            case GlobalNamespace::SelectLevelCategoryViewController::LevelCategory::CustomSongs: [[fallthrough]];
            default:
                break;
            case GlobalNamespace::SelectLevelCategoryViewController::LevelCategory::MusicPacks:  [[fallthrough]];
            case GlobalNamespace::SelectLevelCategoryViewController::LevelCategory::Favorites: [[fallthrough]];
            case GlobalNamespace::SelectLevelCategoryViewController::LevelCategory::All:
                return;
                break;
        }
        model->ProcessSongList(lastLevelCollection ? lastLevelCollection : beatUi->GetCurrentSelectedAnnotatedBeatmapLevelCollection(), beatUi->LevelSelectionNavigationController);
    }

    void SongBrowserUI::CancelFilter()
    {
        INFO("Cancelling filter, levelCollection %s", lastLevelCollection && lastLevelCollection->get_collectionName() ? to_utf8(csstrtostr(lastLevelCollection->get_collectionName())).c_str() : "NULL");
        config.filterMode = SongFilterMode::None;

        auto noDataGO = beatUi->LevelCollectionViewController->noDataInfoGO;
        auto headerText = beatUi->LevelCollectionTableView->headerText;
        auto headerSprite = beatUi->LevelCollectionTableView->headerSprite;

        auto levelCollection = beatUi->GetCurrentSelectedAnnotatedBeatmapLevelCollection()->get_beatmapLevelCollection();
        beatUi->LevelCollectionViewController->SetData(levelCollection, headerText, headerSprite, false, noDataGO);
    }



    void SongBrowserUI::handleDidSelectAnnotatedBeatmapLevelCollection(GlobalNamespace::IAnnotatedBeatmapLevelCollection* annotatedBeatmapLevelCollection)
    {
        lastLevelCollection = annotatedBeatmapLevelCollection;
        config.currentLevelCategoryName = LevelCategoryToString(beatUi->LevelFilteringNavigationController->get_selectedLevelCategory().value);
        SaveConfig();
        INFO("AnnotatedBeatmapLevelCollection, Selected Level Collection={%s}", to_utf8(csstrtostr(lastLevelCollection->get_collectionName())).c_str());
    }

    void SongBrowserUI::_levelFilteringNavController_didSelectAnnotatedBeatmapLevelCollectionEvent(GlobalNamespace::LevelFilteringNavigationController* arg1, GlobalNamespace::IAnnotatedBeatmapLevelCollection* arg2,
            GameObject* arg3, GlobalNamespace::BeatmapCharacteristicSO* arg4)
    {
        if (!arg2)
        {
            // Probably means we transitioned between Music Packs and Playlists
            arg2 = beatUi->GetCurrentSelectedAnnotatedBeatmapLevelCollection();
            if (!arg2)
            {
                ERROR("Nothing selected. This is likely an error.");
                return;
            }
        }

        // Do something about preview level packs, they can't be used past this point
        if (il2cpp_utils::try_cast<GlobalNamespace::PreviewBeatmapLevelPackSO>(arg2))
        {
            INFO("Hiding SongBrowser, previewing a song pack.");
            Hide();
            return;
        }

        // dont do stuff on the filter menu
        switch (beatUi->LevelFilteringNavigationController->get_selectedLevelCategory())
        {
            case GlobalNamespace::SelectLevelCategoryViewController::LevelCategory::None: [[fallthrough]];
            case GlobalNamespace::SelectLevelCategoryViewController::LevelCategory::OstAndExtras:  [[fallthrough]];
            case GlobalNamespace::SelectLevelCategoryViewController::LevelCategory::CustomSongs: [[fallthrough]];
            default:
                Show();
                break;
            case GlobalNamespace::SelectLevelCategoryViewController::LevelCategory::MusicPacks:  [[fallthrough]];
            case GlobalNamespace::SelectLevelCategoryViewController::LevelCategory::Favorites: [[fallthrough]];
            case GlobalNamespace::SelectLevelCategoryViewController::LevelCategory::All:
                return;
                break;
        }


        // category transition, just record the new collection
        if (selectingCategory)
        {
            INFO("Transitioning level category");
            lastLevelCollection = arg2;
            StartCoroutine(reinterpret_cast<System::Collections::IEnumerator*>(custom_types::Helpers::CoroutineHelper::New(RefreshSongListEndOfFrame())));
            return;
        }

        // Skip the first time - prevents a bunch of reload content spam
        if (!lastLevelCollection)
            return;

        SelectLevelCollection(arg2);
    }

    void SongBrowserUI::SelectLevelCollection(GlobalNamespace::IAnnotatedBeatmapLevelCollection* levelCollection)
    {
        if (!levelCollection)
        {
            INFO("No level collection selected...");
            return;
        }

        std::string collectionName = levelCollection->get_collectionName() ? to_utf8(csstrtostr(levelCollection->get_collectionName())) : "";
        // store the real level collection
        if (strcmp(collectionName.c_str(), SongBrowserModel::filteredSongsCollectionName) && lastLevelCollection)
        {
            INFO("Recording levelCollection: {%s}", collectionName.c_str());
            lastLevelCollection = levelCollection;
            config.currentLevelCategoryName = LevelCategoryToString(beatUi->LevelFilteringNavigationController->get_selectedLevelCategory().value);
        }

        // reset level selection
        model->lastSelectedLevelId = "";

        // save level collection
        config.currentLevelCollectionName = collectionName;
        SaveConfig();

        StartCoroutine(reinterpret_cast<System::Collections::IEnumerator*>(custom_types::Helpers::CoroutineHelper::New(ProcessSongListEndOfFrame())));
    }

    custom_types::Helpers::Coroutine SongBrowserUI::ProcessSongListEndOfFrame()
    {
        co_yield reinterpret_cast<System::Collections::IEnumerator*>(UnityEngine::WaitForEndOfFrame::New_ctor());

        bool scrollToLevel = true;
        if (lastLevelCollection && il2cpp_utils::try_cast<GlobalNamespace::IPlaylist>(lastLevelCollection))
        {
            scrollToLevel = false;
            config.sortMode = SongSortMode::Original;
            RefreshSortButtonUI();
        }

        ProcessSongList();
        RefreshSongUI(scrollToLevel);
        co_return;
    }

    custom_types::Helpers::Coroutine SongBrowserUI::RefreshSongListEndOfFrame()
    {
        co_yield reinterpret_cast<System::Collections::IEnumerator*>(UnityEngine::WaitForEndOfFrame::New_ctor());
        RefreshSongUI();
        co_return;
    }

    void SongBrowserUI::OnClearButtonClickEvent()
    {
        INFO("Clearing all sorts and filters.");

        config.sortMode = SongSortMode::Original;
        config.invertSortResults = false;
        config.filterMode = SongFilterMode::None;
        SaveConfig();

        CancelFilter();
        ProcessSongList();
        RefreshSongUI();
    }

    void SongBrowserUI::OnSortButtonClickEvent(SongSortMode sortMode)
    {
        INFO("Sort button - %d - pressed.", sortMode);

        if (NeedsScoreSaberData(sortMode) && !SongDataCoreUtils::get_loaded())
        {
            INFO("Data for sort type is not available.");
            return;
        }

        /*
        auto curCollection = beatUi->GetCurrentSelectedAnnotatedBeatmapLevelCollection();
        std::string collectionName = curCollection ? to_utf8(csstrtostr(curCollection->get_collectionName())) : "";
        if (!lastLevelCollection ||
            (curCollection &&
            // strcmp returns 0 for same, so we need both to not be same
            strcmp(collectionName.c_str(), SongBrowserModel::filteredSongsCollectionName) &&
            strcmp(collectionName.c_str(), SongBrowserModel::playlistSongsCollectionName)))
        {
            lastLevelCollection = curCollection;
        }
        */

        // Clear current selected level id so our song list jumps to the start
        model->lastSelectedLevelId = "";

        if (config.sortMode == sortMode)
        {
            model->ToggleInverting();
        }

        config.sortMode = sortMode;

        // update the seed
        if (config.sortMode == SongSortMode::Random)
        {
            srand(time(0));
            config.randomSongSeed = rand();
        }

        SaveConfig();

        ProcessSongList();
        RefreshSongUI();
    }

    void SongBrowserUI::OnFilterButtonClickEvent(SongFilterMode mode)
    {
        INFO("FilterButton %d clicked.", mode);

        auto curCollection = beatUi->GetCurrentSelectedAnnotatedBeatmapLevelCollection();
        std::string collectionName = curCollection ? to_utf8(csstrtostr(curCollection->get_collectionName())) : "";
        if (!lastLevelCollection ||
            (curCollection &&
            // strcmp returns 0 for same, so we need both to not be same
            strcmp(collectionName.c_str(), SongBrowserModel::filteredSongsCollectionName) &&
            strcmp(collectionName.c_str(), SongBrowserModel::playlistSongsCollectionName)))
        {
            lastLevelCollection = curCollection;
        }

        if (mode == SongFilterMode::Favorites)
        {
            beatUi->SelectLevelCategory("Favorites");
        }
        else
        {
            auto noDataGO = beatUi->LevelCollectionViewController->noDataInfoGO;
            auto headerText = beatUi->LevelCollectionTableView->headerText;
            auto headerSprite = beatUi->LevelCollectionTableView->headerSprite;

            auto levelCollection = beatUi->GetCurrentSelectedAnnotatedBeatmapLevelCollection()->get_beatmapLevelCollection();
            beatUi->LevelCollectionViewController->SetData(levelCollection, headerText, headerSprite, false, noDataGO);
        }

        // If selecting the same filter, cancel
        if (config.filterMode == mode)
        {
            config.filterMode = SongFilterMode::None;
        }
        else
        {
            config.filterMode = mode;
        }

        switch (mode)
        {
            case SongFilterMode::Search:
                OnSearchButtonClickEvent();
                break;
            default:
                SaveConfig();
                ProcessSongList();
                RefreshSongUI();
                break;
        }
    }

    void SongBrowserUI::OnSearchButtonClickEvent()
    {
        ShowSearchKeyboard();
    }

    void SongBrowserUI::OnDidSelectLevelEvent(GlobalNamespace::LevelCollectionViewController* view, GlobalNamespace::IPreviewBeatmapLevel* level)
    {
        if (!level)
        {
            INFO("No level selected?");
            return;
        }

        if (!model)
        {
            ERROR("No model?");
            return;
        }

        model->lastSelectedLevelId = level->get_levelID() ? to_utf8(csstrtostr(level->get_levelID())) : "";
        HandleDidSelectLevelRow(level);
    }

    void SongBrowserUI::OnDidSelectBeatmapCharacteristic(GlobalNamespace::BeatmapCharacteristicSegmentedControlController* view, GlobalNamespace::BeatmapCharacteristicSO* bc)
    {
        model->currentBeatmapCharacteristicSO = bc;

        if (beatUi->StandardLevelDetailView)
        {
            RefreshScoreSaberData(reinterpret_cast<GlobalNamespace::IPreviewBeatmapLevel*>(beatUi->StandardLevelDetailView->get_selectedDifficultyBeatmap()->get_level()));
            RefreshNoteJumpSpeed(beatUi->StandardLevelDetailView->get_selectedDifficultyBeatmap()->get_noteJumpMovementSpeed(),
                beatUi->StandardLevelDetailView->get_selectedDifficultyBeatmap()->get_noteJumpStartBeatOffset());
        }
    }

    void SongBrowserUI::OnDidChangeDifficultyEvent(GlobalNamespace::StandardLevelDetailViewController* view, GlobalNamespace::IDifficultyBeatmap* beatmap)
    {
        if (!view || !view->get_selectedDifficultyBeatmap() || !view->get_selectedDifficultyBeatmap()->get_level()) 
            return;
        
        RefreshScoreSaberData(reinterpret_cast<GlobalNamespace::IPreviewBeatmapLevel*>(view->get_selectedDifficultyBeatmap()->get_level()));
        RefreshNoteJumpSpeed(beatmap->get_noteJumpMovementSpeed(), beatmap->get_noteJumpStartBeatOffset());
    }

    void SongBrowserUI::OnDidPresentContentEvent(GlobalNamespace::StandardLevelDetailViewController* view, GlobalNamespace::StandardLevelDetailViewController::ContentType type)
    {
        if (type != GlobalNamespace::StandardLevelDetailViewController::ContentType::OwnedAndReady)
            return;

        if (!view->get_selectedDifficultyBeatmap())
            return;

        // stash the scroll index
        auto tv = beatUi->LevelCollectionTableView->tableView;
        auto sv = tv->scrollView;
        model->lastScrollIndex = sv->get_position();

        RefreshScoreSaberData(reinterpret_cast<GlobalNamespace::IPreviewBeatmapLevel*>(view->get_selectedDifficultyBeatmap()->get_level()));
        RefreshNoteJumpSpeed(view->get_selectedDifficultyBeatmap()->get_noteJumpMovementSpeed(), view->get_selectedDifficultyBeatmap()->get_noteJumpStartBeatOffset());
    }

    void SongBrowserUI::HandleDidSelectLevelRow(GlobalNamespace::IPreviewBeatmapLevel* level)
    {
        RefreshQuickScrollButtons();
    }

    void SongBrowserUI::HandleDeleteSelectedLevel()
    {
        /*
        auto level = reinterpret_cast<GlobalNamespace::IPreviewBeatmapLevel*>(beatUi->LevelDetailViewController->get_selectedDifficultyBeatmap()->get_level());

        std::function<void(int)> fun = [&](int selectedButton){
            deleteDialog->__DismissViewController(nullptr, HMUI::ViewController::AnimationDirection::Horizontal, false);
            beatUi->ScreenSystem->titleViewController->get_gameObject()->SetActive(true);

            if (selectedButton == 0)
            {
                
                
                 
                auto levels = System::Linq::Enumerable::ToList((System::Collections::Generic::IEnumerable_1<GlobalNamespace::IPreviewBeatmapLevel*>)beatUi->GetCurrentLevelCollectionLevels());
                auto collection = beatUi->GetCurrentSelectedAnnotatedBeatmapLevelCollection()->get_collectionName();
                std::string collectionCpp = to_utf8(csstrtostr(collection));
                auto selectedLevelID = reinterpret_cast<GlobalNamespace::IPreviewBeatmapLevel*>(beatUi->StandardLevelDetailView->get_selectedDifficultyBeatmap()->get_level())->get_levelID();
                
                int selectedIndex = ArrayUtil::FirstIndexOf(levels, [&](auto x){
                    return x->get_levelID()->Equals(selectedLevelID);
                });

                if (selectedIndex > -1)
                {
                    GlobalNamespace::CustomPreviewBeatmapLevel* song = nullptr;
                    if (collectionCpp == "")
                    {
                        song = ArrayUtil::First(RuntimeSongLoader::API::GetLoadedSongs(), [&](auto x){
                            return x->get_levelID()->Equals(selectedLevelID);
                        });
                    }
                    else if (collectionCpp == "WIP Levels")
                    {
                        // there is no seperate list for WIP levels implemented in the API
                        song = ArrayUtil::First(RuntimeSongLoader::API::GetLoadedSongs(), [&](auto x){
                            return x->get_levelID()->Equals(selectedLevelID);
                        });
                    }
                    else if (collectionCpp == "Cached WIP Levels")
                    {
                        ERROR("Cannot delete Cached levels");
                        return;
                    }
                    else if (collectionCpp == "Custom Levels")
                    {
                        song = ArrayUtil::First(RuntimeSongLoader::API::GetLoadedSongs(), [&](auto x){
                            return x->get_levelID()->Equals(selectedLevelID);
                        });
                    }
                    else
                    {
                        // I don't think we have this in runtime song loader
                        //
                        //var names = SongCore.Loader.SeperateSongFolders.Select(x => x.SongFolderEntry.Name);
                        //var separateFolders = SongCore.Loader.SeperateSongFolders;
                        //
                        //if (names.Count() > 0 && names.Contains(collection))
                        //{
                        //    int folder_index = separateFolders.FindIndex(x => x.SongFolderEntry.Name.Equals(collection));
                        //    song = separateFolders[folder_index].Levels.First(x => x.Value.levelID == selectedLevelID).Value;
                        //}
                        //else
                        //{
                        //    // final guess - playlist
                        //    song = SongCore.Loader.CustomLevels.First(x => x.Value.levelID == selectedLevelID).Value;
                        //}
                        //
                        song = ArrayUtil::First(RuntimeSongLoader::API::GetLoadedSongs(), [&](auto x){
                            return x->get_levelID()->Equals(selectedLevelID);
                        });
                    }

                    if (!song)
                    {
                        ERROR("Unable to find selected level.  Is it an official song?");
                        return;
                    }

                    std::string path = to_utf8(csstrtostr(song->get_customLevelPath()));
                    INFO("Deleting song: %s", path.c_str());

                    RuntimeSongLoader::API::DeleteSong(path, [&](){
                        std::vector<GlobalNamespace::IPreviewBeatmapLevel*> toRemove = {};

                        ArrayUtil::ForEach(levels, [&](auto x){
                            if (x->get_levelID()->Equals(selectedLevelID))
                                toRemove.push_back(x);
                        });

                        int removedLevels = toRemove.size();
                        for (auto x : toRemove) levels->Remove(x);
                        INFO("Removed [%d] level(s) from song list!", removedLevels);

                        UpdateLevelDataModel();

                        // if we have a song to select at the same index, set the last selected level id, UI updates takes care of the rest.
                        if (selectedIndex < levels->get_Count())
                        {
                            auto selectedLevelID = levels->items->values[selectedIndex]->get_levelID();
                            if (selectedLevelID)
                            {
                                model->lastSelectedLevelId = to_utf8(csstrtostr(selectedLevelID));
                            }
                        }

                        RefreshSongList();
                    });
                }
            }
        };

        auto delegate = il2cpp_utils::MakeDelegate<System::Action_1<int>*>(classof(System::Action_1<int>*), fun);
        std::string question = string_format("Do you really want to delete \"%s %s\"?", to_utf8(csstrtostr(level->get_songName())).c_str(), to_utf8(csstrtostr(level->get_songSubName())).c_str());
        deleteDialog->Init(il2cpp_utils::newcsstr("Delete song"), il2cpp_utils::newcsstr(question), il2cpp_utils::newcsstr("Delete"), il2cpp_utils::newcsstr("Cancel"), delegate);

        beatUi->ScreenSystem->titleViewController->get_gameObject()->SetActive(false);
        beatUi->LevelSelectionNavigationController->__PresentViewController(deleteDialog, nullptr, HMUI::ViewController::AnimationDirection::Horizontal, false);
        */
    }

    void SongBrowserUI::ShowSearchKeyboard()
    {
        //ShowInputKeyboard(std::bind(&SongBrowserUI::SearchViewControllerSearchButtonPressed, this, std::placeholders::_1));
    }

    void SongBrowserUI::ShowInputKeyboard(std::function<void(Il2CppString*)> enterPressedHandler)
    {
        /*
        // make keyboard from bsml
        auto gameObject = UIUtils::CreateModalKeyboard(beatUi->LevelSelectionNavigationController->get_transform());
        // kb modal view setactive
        gameObject->SetActive(true);
        // get modal
        auto modalKb = gameObject->GetComponent<HMUI::ModalKeyboard*>();
        // add our enter handler
        modalKb->keyboard->add_EnterPressed(il2cpp_utils::MakeDelegate<System::Action_1<Il2CppString*>*>(classof(System::Action_1<Il2CppString*>*), enterPressedHandler));
        // show it
        modalKb->modalView->Show(true, true, nullptr);
        */
    }

    void SongBrowserUI::SearchViewControllerSearchButtonPressed(std::string searchFor)
    {
        config.filterMode = SongFilterMode::Search;
        config.searchTerms.insert(config.searchTerms.begin(), searchFor);
        SaveConfig();
        model->lastSelectedLevelId = "";

        ProcessSongList();
        RefreshSongUI();
    }

    void SongBrowserUI::CreatePlaylistButtonPressed(Il2CppString* playlistName)
    {
        if (Il2CppString::IsNullOrWhiteSpace(playlistName))
            return;
        #warning playlists not properly made to work
        //BeatSaberPlaylistsLib.Types.IPlaylist playlist = Playlist.CreateNew(playlistName, beatUi.GetCurrentLevelCollectionLevels());
        //BeatSaberPlaylistsLib.PlaylistManager.DefaultManager.RequestRefresh(Assembly.GetExecutingAssembly().FullName);
        //SongBrowserApplication::mainProgressBar->ShowMessage("Successfully Exported Playlist", 5.0f);
    }

    void SongBrowserUI::JumpSongList(int numJumps, float segmentPercent)
    {
        auto levels = beatUi->GetCurrentLevelCollectionLevels();
        if (!levels)
            return;

        int totalSize = levels->Length();
        int segmentSize = (int)(totalSize * segmentPercent);

        // Jump at least one scree size.
        if (segmentSize < LIST_ITEMS_VISIBLE_AT_ONCE)
        {
            segmentSize = LIST_ITEMS_VISIBLE_AT_ONCE;
        }

        int currentRow = beatUi->LevelCollectionTableView->selectedRow;
        int jumpDirection = 1 - ((numJumps < 0) * 2);
        int newRow = currentRow + (jumpDirection * segmentSize);
        if (newRow <= 0)
        {
            newRow = 0;
        }
        else if (newRow >= totalSize)
        {
            newRow = totalSize - 1;
        }

        INFO("jumpDirection: %d, newRow: %d", jumpDirection, newRow);
        beatUi->ScrollToLevelByRow(newRow);
        RefreshQuickScrollButtons();
    }

    void SongBrowserUI::RefreshScoreSaberData(GlobalNamespace::IPreviewBeatmapLevel* level)
    {
        if (!SongDataCoreUtils::get_loaded()) return;
        auto difficulty = beatUi->LevelDifficultyViewController->get_selectedDifficulty();
        auto diffVal = difficulty.value;
        std::string difficultyString = BeatmapDifficultyToString(diffVal);
        //if (difficultyString == "ExpertPlus")
        //{
        //    difficultyString = "Expert+";
        //}

        INFO("%s", difficultyString.c_str());

        // Check if we have data for this song
        INFO("Checking if have info for song %s", to_utf8(csstrtostr(level->get_songName())).c_str());
        
        auto levelIDcs = level->get_levelID();
        std::string levelID = levelIDcs ? to_utf8(csstrtostr(levelIDcs)) : "";
        auto hash = SongBrowserModel::GetSongHash(levelID);
        // BeatStarSong*, if null not found 
        auto song = SDC_wrapper::BeatStarSong::GetSong(hash);
        if (song)
        {
            INFO("Song existed!");
            auto char_ = SDC_wrapper::BeatStarCharacteristic(beatUi->BeatmapCharacteristicSelectionViewController->get_selectedBeatmapCharacteristic());
            // BeatStarSongDifficultyStats*, null if nonexistent
            auto songDifficulty = song->GetDifficulty(char_, difficultyString);
            if (songDifficulty)
            {
                INFO("Display pp for diff %s", songDifficulty->GetName().data());
                // no pp cause songdatacore no pp
                double pp = songDifficulty->approximate_pp_value;
                double star = songDifficulty->stars;

                UIUtils::SetStatButtonText(ppStatButton, string_format("%.1f", pp));
                UIUtils::SetStatButtonText(starStatButton, string_format("%.1f", star));
            }
            else
            {
                UIUtils::SetStatButtonText(ppStatButton, "NA");
                UIUtils::SetStatButtonText(starStatButton, "NA");
            }
        }
        else
        {
            UIUtils::SetStatButtonText(ppStatButton, "NA");
            UIUtils::SetStatButtonText(starStatButton, "NA");
        }

        INFO("Done refreshing score saber stats.");
    }

    void SongBrowserUI::RefreshNoteJumpSpeed(float noteJumpMovementSpeed, float noteJumpStartBeatOffset)
    {
        UIUtils::SetStatButtonText(njsStatButton, string_format("%.2f", noteJumpMovementSpeed));
        UIUtils::SetStatButtonText(noteJumpStartBeatOffsetLabel, string_format("%.2f", noteJumpStartBeatOffset));
    }

    void SongBrowserUI::RefreshQuickScrollButtons()
    {
        if (!uiCreated)
            return;
        
        pageUpFastButton->set_interactable(beatUi->TableViewPageUpButton->get_interactable());
        pageUpFastButton->get_gameObject()->SetActive(beatUi->TableViewPageUpButton->IsActive());

        pageDownFastButton->set_interactable(beatUi->TableViewPageDownButton->get_interactable());
        pageDownFastButton->get_gameObject()->SetActive(beatUi->TableViewPageDownButton->IsActive());
    }

    custom_types::Helpers::Coroutine SongBrowserUI::RefreshQuickScrollButtonsAsync()
    {
        co_yield reinterpret_cast<System::Collections::IEnumerator*>(UnityEngine::WaitForEndOfFrame::New_ctor());

        RefreshQuickScrollButtons();
        co_return;
    }

    void SongBrowserUI::UpdateDeleteButtonState(Il2CppString* levelId)
    {
        if (!deleteButton)
            return; 
        /*
        deleteButton->get_gameObject()->SetActive(levelId ? levelId->get_Length() >= 32 : 0);
        deleteButton->get_transform()->SetAsFirstSibling();

        // disable the songloader delete button
        static auto deleteLevelButtonName = il2cpp_utils::newcsstr<il2cpp_utils::CreationType::Manual>("DeleteLevelButton");
        auto transform = beatUi->StandardLevelDetailView->practiceButton->get_transform()->get_parent()->Find(deleteLevelButtonName);
        if (transform) transform->get_gameObject()->SetActive(false);
        */
    }

    void SongBrowserUI::SetVisibility(bool visible, bool fieldsVisibility)
    {
        if (!uiCreated)
            return;
        if (ppStatButton) ppStatButton->get_gameObject()->SetActive(visible || fieldsVisibility);
        if (starStatButton) starStatButton->get_gameObject()->SetActive(visible || fieldsVisibility);
        if (njsStatButton) njsStatButton->get_gameObject()->SetActive(visible || fieldsVisibility);
        if (noteJumpStartBeatOffsetLabel) noteJumpStartBeatOffsetLabel->get_gameObject()->SetActive(visible || fieldsVisibility);

        RefreshOuterUIState(visible ? UIState::Main : UIState::Disabled);

        if (deleteButton) deleteButton->get_gameObject()->SetActive(visible);

        if (pageUpFastButton) pageUpFastButton->get_gameObject()->SetActive(visible);
        if (pageDownFastButton) pageDownFastButton->get_gameObject()->SetActive(visible);
    }

    void SongBrowserUI::RefreshOuterUIState(UIState state)
    {
        bool sortButtons = false;
        bool filterButtons = false;
        bool outerButtons = false;
        bool clearButton = true;
        switch (state)
        {
            case UIState::SortBy:
                sortButtons = true;
                break;
            case UIState::FilterBy:
                filterButtons = true;
                break;
            case UIState::Main:
                outerButtons = true;
                break;
            default:
                clearButton = false;
                break;
        }
        
        int sortLength = sortButtonGroup->get_Count();
        for (int i = 0; i < sortLength; i++)
            sortButtonGroup->items->values[i]->button->get_gameObject()->SetActive(sortButtons);

        int filterLength = filterButtonGroup->get_Count();
        for (int i = 0; i < filterLength; i++)
            filterButtonGroup->items->values[i]->button->get_gameObject()->SetActive(filterButtons);

        if (sortByButton) sortByButton->get_gameObject()->SetActive(outerButtons);
        if (sortByDisplay) sortByDisplay->get_gameObject()->SetActive(outerButtons);
        if (filterByButton) filterByButton->get_gameObject()->SetActive(outerButtons);
        if (filterByDisplay) filterByDisplay->get_gameObject()->SetActive(outerButtons);
        if (clearSortFilterButton) clearSortFilterButton->get_gameObject()->SetActive(clearButton);
        if (randomButton) randomButton->get_gameObject()->SetActive(outerButtons);
        if (playlistExportButton) playlistExportButton->get_gameObject()->SetActive(outerButtons);

        RefreshCurrentSelectionDisplay();
        currentUiState = state;
    }

    void SongBrowserUI::RefreshCurrentSelectionDisplay()
    {
        std::string sortByDisplayText;
        if (config.sortMode == SongSortMode::Default)
        {
            sortByDisplayText = "Title";
        }
        else
        {
            sortByDisplayText = SongSortModeToString(config.sortMode);
        }

        UIUtils::SetButtonText(sortByDisplay, sortByDisplayText);
        if (config.filterMode != SongFilterMode::CustomFilter)
        {
            // Custom SongFilterMod implies that another mod has modified the text of this button (do not overwrite)
            UIUtils::SetButtonText(filterByDisplay, SongFilterModeToString(config.filterMode));
        }
    }

    void SongBrowserUI::RefreshSortButtonUI()
    {
        if (!uiCreated)
            return;

        INFO("Refreshing song sort UI");
        int length = sortButtonGroup->get_Count();
        for (int i = 0; i < length; i++)
        {
            auto sortButton = sortButtonGroup->items->values[i];
            if (NeedsScoreSaberData(sortButton->sortMode) && !SongDataCoreUtils::get_loaded())
                UIUtils::SetButtonUnderlineColor(sortButton->button, {0.5f, 0.5f, 0.5f, 1.0f});
            else
                UIUtils::SetButtonUnderlineColor(sortButton->button, {1.0f, 1.0f, 1.0f, 1.0f});

            if (sortButton->sortMode == config.sortMode)
            {
                if (config.invertSortResults)
                    UIUtils::SetButtonUnderlineColor(sortButton->button, {1.0f, 0.0f, 0.0f, 1.0f});
                else
                    UIUtils::SetButtonUnderlineColor(sortButton->button, {0.0f, 1.0f, 0.0f, 1.0f});
            }
        }

        length = filterButtonGroup->get_Count();
        for (int i = 0; i < length; i++)
        {
            auto filterButton = filterButtonGroup->items->values[i];
            if (filterButton->filterMode == config.filterMode)
                UIUtils::SetButtonUnderlineColor(filterButton->button, {0.0f, 1.0f, 0.0f, 1.0f});
            else
                UIUtils::SetButtonUnderlineColor(filterButton->button, {1.0f, 1.0f, 1.0f, 1.0f});
        }

        if (config.invertSortResults)
            UIUtils::SetButtonUnderlineColor(sortByDisplay, {1.0f, 0.0f, 0.0f, 1.0f});
        else
            UIUtils::SetButtonUnderlineColor(sortByDisplay, {0.0f, 1.0f, 0.0f, 1.0f});

        if (config.filterMode != SongFilterMode::None)
            UIUtils::SetButtonUnderlineColor(filterByDisplay, {0.0f, 1.0f, 0.0f, 1.0f});
        else
            UIUtils::SetButtonUnderlineColor(filterByDisplay, {1.0f, 1.0f, 1.0f, 1.0f});
    }
}