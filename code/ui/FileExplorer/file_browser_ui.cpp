#include "pch.hpp"

#include "file_browser_ui.hpp"

ImGui::FileBrowser::FileBrowser(ImGuiFileBrowserFlags flags, std::filesystem::path defaultDirectory)
	: width_(700)
	, height_(450)
	, posX_(0)
	, posY_(0)
	, flags_(flags)
	, defaultDirectory_(std::move(defaultDirectory))
	, shouldOpen_(false)
	, shouldClose_(false)
	, isOpened_(false)
	, isOk_(false)
	, isPosSet_(false)
	, rangeSelectionStart_(0)
	, editDir_(false)
	, setFocusToEditDir_(false)
	, sortField_(SortField::Name)
	, mediaFilter_(MediaFilter::All)
	, keepOpen_(false) {
	assert(!((flags_ & ImGuiFileBrowserFlags_SelectDirectory) && (flags_ & ImGuiFileBrowserFlags_EnterNewFilename))
		&& "'EnterNewFilename' doesn't work when 'SelectDirectory' is enabled");
	if (flags_ & ImGuiFileBrowserFlags_CreateNewDir) {
		newDirNameBuffer_.resize(32, '\0');
	}

	SetTitle("file browser");
	SetDirectory(defaultDirectory_);

	typeFilters_.clear();
	typeFilterIndex_ = 0;
	hasAllFilter_    = false;

#ifdef _WIN32
	drives_ = GetDrivesBitMask();
#endif
}

void ImGui::FileBrowser::SetWindowPos(int posX, int posY) noexcept {
	posX_     = posX;
	posY_     = posY;
	isPosSet_ = true;
}

void ImGui::FileBrowser::SetWindowSize(int width, int height) noexcept {
	assert(width > 0 && height > 0);
	width_  = width;
	height_ = height;
}

void ImGui::FileBrowser::SetTitle(std::string title) {
	title_ = std::move(title);

	std::string const thisPtrStr = std::to_string(std::bit_cast<size_t>(this));
	openLabel_                   = title_ + "##filebrowser_" + thisPtrStr;
	openNewDirLabel_             = "new dir##new_dir_" + thisPtrStr;
}

void ImGui::FileBrowser::Open() {
	RequestReload();
	ClearSelected();
	statusStr_   = std::string();
	shouldOpen_  = true;
	shouldClose_ = false;
	if ((flags_ & ImGuiFileBrowserFlags_EnterNewFilename) && !customizedInputName_.empty()) {
		AssignToArrayStyleString(inputNameBuffer_, customizedInputName_);
		selectedFilenames_ = {u8StrToPath(inputNameBuffer_.data())};
	}
}

void ImGui::FileBrowser::Close() {
	ClearSelected();
	statusStr_   = std::string();
	shouldClose_ = true;
	shouldOpen_  = false;
}

bool ImGui::FileBrowser::IsOpened() const noexcept { return isOpened_; }

void ImGui::FileBrowser::Display() { // SUPER HOT MUST BE IN ITS OWN THREAD
	PushID(this);
	ScopeGuard exitThis([this] {
		shouldOpen_  = false;
		shouldClose_ = false;
		PopID();
	});

	bool const isWindowMode = (flags_ & ImGuiFileBrowserFlags_Window) != 0;

	// In popup/modal modes OpenPopup must be called before BeginPopup/Modal.
	if (!isWindowMode && shouldOpen_) {
		OpenPopup(openLabel_.c_str());
	}

	isOpened_ = false;

	// Apply any finished background directory scan before rendering rows.
	PollScan();

	// Thumbnail engine frame boundary: reset the upload budget, free retired textures,
	// drain async results, and enforce the LRU texture cap. No-op until Setup().
	m_thumbnails.begin_frame();

	// ---- Open the appropriate container ----

	if (isWindowMode) {
		if (shouldOpen_)
			windowVisible_ = true;
		if (shouldClose_)
			windowVisible_ = false;
		if (!windowVisible_)
			return;

		if (isPosSet_) {
			SetNextWindowPos(ImVec2(static_cast<float>(posX_), static_cast<float>(posY_)), ImGuiCond_FirstUseEver);
		}
		SetNextWindowSize(ImVec2(static_cast<float>(width_), static_cast<float>(height_)), ImGuiCond_FirstUseEver);

		ImGuiWindowFlags winFlags = ImGuiWindowFlags_NoFocusOnAppearing;
		if (flags_ & ImGuiFileBrowserFlags_NoTitleBar)
			winFlags |= ImGuiWindowFlags_NoTitleBar;

		bool open = true;
		if (!Begin(title_.c_str(), &open, winFlags) || !open) {
			if (!open)
				windowVisible_ = false;
			End();
			return;
		}
	} else {
		if (shouldOpen_ && (flags_ & ImGuiFileBrowserFlags_NoModal)) {
			if (isPosSet_) {
				SetNextWindowPos(ImVec2(static_cast<float>(posX_), static_cast<float>(posY_)));
			}
			SetNextWindowSize(ImVec2(static_cast<float>(width_), static_cast<float>(height_)));
		} else {
			if (isPosSet_) {
				SetNextWindowPos(ImVec2(static_cast<float>(posX_), static_cast<float>(posY_)), ImGuiCond_FirstUseEver);
			}
			SetNextWindowSize(ImVec2(static_cast<float>(width_), static_cast<float>(height_)), ImGuiCond_FirstUseEver);
		}
		if (flags_ & ImGuiFileBrowserFlags_NoModal) {
			if (!BeginPopup(openLabel_.c_str()))
				return;
		} else if (!BeginPopupModal(openLabel_.c_str(), nullptr,
					   flags_ & ImGuiFileBrowserFlags_NoTitleBar ? ImGuiWindowFlags_NoTitleBar : 0)) {
			return;
		}
	}

	isOpened_ = true;

	// Helper: close the container correctly for each mode.
	auto closeContainer = [&]() {
		if (isWindowMode)
			windowVisible_ = false;
		else
			CloseCurrentPopup();
	};
	auto endContainer = [&]() {
		if (isWindowMode)
			End();
		else
			EndPopup();
	};
	ScopeGuard endContainerGuard(std::move(endContainer));

	// ---- Shared body ----

	std::filesystem::path newDir;
	bool                  shouldSetNewDir = false;

	if (editDir_) {
		if (setFocusToEditDir_) {
			SetKeyboardFocusHere();
		}

		PushItemWidth(-1);
		bool const enter = InputText("##directory", currDirBuffer_.data(), currDirBuffer_.size(),
			ImGuiInputTextFlags_CallbackResize | ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll,
			ExpandInputBuffer, &currDirBuffer_);
		PopItemWidth();

		if (!IsItemActive() && !setFocusToEditDir_) {
			editDir_ = false;
		}
		setFocusToEditDir_ = false;

		if (enter) {
			std::filesystem::path enteredDir = u8StrToPath(currDirBuffer_.data());
			if (is_directory(enteredDir)) {
				newDir          = std::move(enteredDir);
				shouldSetNewDir = true;
			} else if (is_directory(enteredDir.parent_path())) {
				newDir          = enteredDir.parent_path();
				shouldSetNewDir = true;
			} else {
				statusStr_ = "[" + std::string(currDirBuffer_.data()) + "] is not a valid directory";
			}
		}
	} else {
		// display elements in pwd

#ifdef _WIN32
		const char currentDrive = static_cast<char>(currentDirectory_.c_str()[0]);
		char const driveStr[]   = {currentDrive, ':', '\0'};

		PushItemWidth(4 * GetFontSize());
		if (BeginCombo("##select_drive", driveStr)) {
			ScopeGuard guard([&] { EndCombo(); });

			for (int i = 0; i < 26; ++i) {
				if (!(drives_ & (1 << i))) {
					continue;
				}

				char const driveCh         = static_cast<char>('A' + i);
				char const selectableStr[] = {driveCh, ':', '\0'};
				bool const selected        = currentDrive == driveCh;

				if (Selectable(selectableStr, selected) && !selected) {
					char newPwd[] = {driveCh, ':', '\\', '\0'};
					SetDirectory(newPwd);
				}
			}
		}
		PopItemWidth();

		SameLine();
#endif

		int secIdx = 0, newDirLastSecIdx = -1;
		for (auto const& sec : currentDirectory_) {
#ifdef _WIN32
			if (secIdx == 1) {
				++secIdx;
				continue;
			}
#endif

			PushID(secIdx);
			if (secIdx > 0) {
				SameLine();
			}
			if (SmallButton(u8StrToStr(sec.u8string()).c_str())) {
				newDirLastSecIdx = secIdx;
			}
			PopID();

			++secIdx;
		}

		if (newDirLastSecIdx >= 0) {
			int                   i = 0;
			std::filesystem::path dstDir;
			for (auto const& sec : currentDirectory_) {
				if (i++ > newDirLastSecIdx) {
					break;
				}
				dstDir /= sec;
			}

#ifdef _WIN32
			if (newDirLastSecIdx == 0) {
				dstDir /= "\\";
			}
#endif

			SetDirectory(dstDir);
		}

		if (flags_ & ImGuiFileBrowserFlags_EditPathString) {
			SameLine();

			if (SmallButton("#")) {
				auto const currDirStr = u8StrToStr(currentDirectory_.u8string());
				currDirBuffer_.resize(currDirStr.size() + 1);
				std::memcpy(currDirBuffer_.data(), currDirStr.data(), currDirStr.size());
				currDirBuffer_.back() = '\0';

				editDir_           = true;
				setFocusToEditDir_ = true;
			} else {
				ToolTip("Edit the current path");
			}
		}

		SameLine();
		SetNextItemWidth(14 * GetFontSize());

		std::string recentPreview = u8StrToStr(currentDirectory_.filename().u8string());
		if (recentPreview.empty()) {
			recentPreview = u8StrToStr(currentDirectory_.u8string());
		}
		if (recentPreview.empty()) {
			recentPreview = "Recent";
		}

		if (BeginCombo("##recent_dirs", recentPreview.c_str())) {
			ScopeGuard endRecentDirsCombo([] { EndCombo(); });

			for (auto const& dir : recentDirectories_) {
				std::string const dirLabel = u8StrToStr(dir.u8string());
				bool const        selected = (dir == currentDirectory_);
				if (Selectable(dirLabel.c_str(), selected) && !selected) {
					newDir          = dir;
					shouldSetNewDir = true;
				}
			}
		}
	}

	SameLine();
	if (SmallButton("*")) {
#ifdef _WIN32
		drives_ = GetDrivesBitMask();
#endif

		// Refresh: re-scan the current directory asynchronously. PollScan()
		// prunes selections that no longer exist once the result arrives.
		RequestReload();
	} else {
		ToolTip("Refresh");
	}

	bool focusOnInputText = false;
	if (flags_ & ImGuiFileBrowserFlags_CreateNewDir) {
		SameLine();
		if (SmallButton("+")) {
			OpenPopup(openNewDirLabel_.c_str());
			newDirNameBuffer_[0] = '\0';
		} else {
			ToolTip("Create a new directory");
		}

		if (BeginPopup(openNewDirLabel_.c_str())) {
			ScopeGuard endNewDirPopup([] { EndPopup(); });

			InputText("name", newDirNameBuffer_.data(), newDirNameBuffer_.size(), ImGuiInputTextFlags_CallbackResize,
				ExpandInputBuffer, &newDirNameBuffer_);
			focusOnInputText |= IsItemFocused();
			SameLine();

			if (Button("ok") && newDirNameBuffer_[0] != '\0') {
				ScopeGuard closeNewDirPopup([] { CloseCurrentPopup(); });
				if (create_directory(currentDirectory_ / u8StrToPath(newDirNameBuffer_.data()))) {
					RequestReload();
				} else {
					statusStr_ = "failed to create " + std::string(newDirNameBuffer_.data());
				}
			}
		}
	}

	// browse files in a child window

	// Reserve bottom rows: action buttons + optional input-name row.
	float reserveHeight = GetFrameHeightWithSpacing();
	if (flags_ & ImGuiFileBrowserFlags_EnterNewFilename) {
		reserveHeight += GetFrameHeightWithSpacing();
	}

	// Sort controls consume one row above the file list: a field selector and
	// an independent ascending/descending direction.
	{
		static char const* sortLabels[] = {"Name", "Type", "Size", "Modified", "Created"};
		TextDisabled("Sort:");
		SameLine();
		SetNextItemWidth(110.0f);
		int sortIdx = static_cast<int>(sortField_);
		if (Combo("##fb_sort", &sortIdx, sortLabels, IM_ARRAYSIZE(sortLabels))) {
			sortField_ = static_cast<SortField>(sortIdx);
			RebuildView();
		}
		SameLine();
		static char const* dirLabels[] = {"Ascending", "Descending"};
		SetNextItemWidth(120.0f);
		int dirIdx = sortAscending_ ? 0 : 1;
		if (Combo("##fb_sort_dir", &dirIdx, dirLabels, 2)) {
			sortAscending_ = (dirIdx == 0);
			RebuildView();
		}
	}
	SameLine();
	SetNextItemWidth(90.0f);
	{
		static char const* mediaLabels[] = {"All", "Videos", "Images"};
		int                mfIdx         = static_cast<int>(mediaFilter_);
		if (Combo("##fb_media", &mfIdx, mediaLabels, 3))
			mediaFilter_ = static_cast<MediaFilter>(mfIdx);
	}
	if (!availableTags_.empty()) {
		SameLine();
		SetNextItemWidth(110.0f);
		std::string const tagPreview = tagFilter_.empty() ? "Tag: All" : ("Tag: " + tagFilter_);
		if (BeginCombo("##fb_tags", tagPreview.c_str())) {
			if (Selectable("All##fb_tag_all", tagFilter_.empty()))
				tagFilter_.clear();
			for (auto const& t : availableTags_) {
				bool const sel = (tagFilter_ == t);
				if (Selectable(t.c_str(), sel))
					tagFilter_ = t;
			}
			EndCombo();
		}
		ToolTip("Filter by Dolphin tag");
	}
	Checkbox("Preview", &previewEnabled_);
	SameLine();
	Checkbox("Keep open", &keepOpen_);
	if (m_thumbnails.is_setup()) {
		SameLine();
		Checkbox("Thumbnails", &showThumbnails_);
		ToolTip("Show or hide file thumbnails");
		if (showThumbnails_) {
			SameLine();
			if (SmallButton(viewMode_ == ViewMode::List ? "Grid" : "List"))
				viewMode_ = (viewMode_ == ViewMode::List) ? ViewMode::Grid : ViewMode::List;
			ToolTip("Toggle grid / list view");
		}
	}

	// Search bar
	{
		PushItemWidth(-1);
		char searchBuf[256] = {};
		std::strncpy(searchBuf, searchStr_.c_str(), sizeof(searchBuf) - 1);
		if (InputTextWithHint("##fb_search", "Search...", searchBuf, sizeof(searchBuf))) {
			searchStr_ = searchBuf;
		}
		PopItemWidth();
	}

	{
		BeginChild("ch", ImVec2(0, -reserveHeight), true,
			(flags_ & ImGuiFileBrowserFlags_NoModal) ? ImGuiWindowFlags_AlwaysHorizontalScrollbar : 0);
		ScopeGuard endChild([] { EndChild(); });

		// Shift + mouse-wheel scales thumbnails when the file list is hovered.
		if (m_thumbnails.is_setup() && showThumbnails_ && IsWindowHovered(ImGuiHoveredFlags_ChildWindows)
			&& GetIO().KeyShift) {
			float const wheel = GetIO().MouseWheel;
			if (wheel != 0.0f) {
				float const factor = (wheel > 0.0f) ? 1.1f : (1.0f / 1.1f);
				bool const  isGrid = (viewMode_ == ViewMode::Grid);
				ImVec2&     sz     = isGrid ? gridThumbnailSize_ : thumbnailSize_;
				// Maintain 16:9 aspect ratio; clamp to a sensible range.
				float       newW   = std::clamp(sz.x * factor, isGrid ? 64.0f : 24.0f, isGrid ? 512.0f*2 : 128.0f);
				sz                 = ImVec2(newW, std::round(newW * (9.0f / 16.0f)));
			}
		}

		bool const shouldHideRegularFiles
			= (flags_ & ImGuiFileBrowserFlags_HideRegularFiles) && (flags_ & ImGuiFileBrowserFlags_SelectDirectory);
		std::string const lowerSearch = searchStr_.empty() ? std::string() : ToLower(searchStr_);

		// Returns true when the file should be shown given the current mediaFilter_.
		auto const isMediaMatched = [this](std::filesystem::path const& ext) -> bool {
			if (mediaFilter_ == MediaFilter::All)
				return true;
			std::string e = ext.string();
			std::transform(e.begin(), e.end(), e.begin(), [](unsigned char c) {
				return static_cast<char>(std::tolower(c));
			});
			if (mediaFilter_ == MediaFilter::Images)
				return e == ".jpg" || e == ".jpeg" || e == ".png" || e == ".webp" || e == ".bmp" || e == ".gif";
			// MediaFilter::Videos — video + animated formats
			return e == ".mp4" || e == ".mkv" || e == ".avi" || e == ".mov" || e == ".wmv" || e == ".flv"
				|| e == ".webm" || e == ".m4v" || e == ".ts" || e == ".gif";
		};

		bool const useGridView = (viewMode_ == ViewMode::Grid) && m_thumbnails.is_setup() && showThumbnails_;

		// ── Grid view ──────────────────────────────────────────────────────────
		if (useGridView) {
			float const availW = GetContentRegionAvail().x;
			float const cellW  = gridThumbnailSize_.x + GetStyle().ItemSpacing.x;
			int const   cols   = std::max(1, static_cast<int>(availW / cellW));

			if (BeginTable("##fb_grid", cols)) {
				ScopeGuard endGrid([] { EndTable(); });
				for (unsigned int rscIdx = 0; rscIdx < fileRecords_.size(); ++rscIdx) {
					auto const& rsc = fileRecords_[rscIdx];
					if (!rsc.isDir && shouldHideRegularFiles)
						continue;
					if (!rsc.isDir && !IsExtensionMatched(rsc.extension))
						continue;
					if (!rsc.isDir && !isMediaMatched(rsc.extension))
						continue;
					if (!rsc.isDir && !tagFilter_.empty()
						&& std::find(rsc.tags.begin(), rsc.tags.end(), tagFilter_) == rsc.tags.end())
						continue;
					if (!rsc.name.empty() && rsc.name.c_str()[0] == '$')
						continue;
					if (!lowerSearch.empty()) {
						if (ToLower(u8StrToStr(rsc.name.u8string())).find(lowerSearch) == std::string::npos)
							continue;
					}

					TableNextColumn();
					PushID(static_cast<int>(rscIdx));

					bool const  selected = selectedFilenames_.find(rsc.name) != selectedFilenames_.end();
					float const thumbW   = gridThumbnailSize_.x;
					float const thumbH   = gridThumbnailSize_.y;

					// Thumbnail image or placeholder.
					if (rsc.isDir) {
						Dummy({thumbW, thumbH});
					} else {
						// thumbKey precomputed at scan time; empty => not thumbnailable.
						ImTextureID const thumb = rsc.thumbKey.empty()
							? ImTextureID(0)
							: m_thumbnails.get(rsc.thumbKey, currentDirectory_, rsc.name, rsc.isVideoThumb);
						if (thumb)
							Image(thumb, {thumbW, thumbH});
						else
							Dummy({thumbW, thumbH});
					}

					bool const hovered = IsItemHovered(ImGuiHoveredFlags_None);

					// Single-click selection.
					if (hovered && IsMouseClicked(ImGuiMouseButton_Left)) {
						bool const wantDir   = (flags_ & ImGuiFileBrowserFlags_SelectDirectory) != 0;
						bool const canSelect = rsc.name != ".." && rsc.isDir == wantDir;
						if (canSelect) {
							bool const multiSel = (flags_ & ImGuiFileBrowserFlags_MultipleSelection) != 0
								&& IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
							bool const ctrlHeld   = GetIO().KeyCtrl;
							bool const shiftHeld  = GetIO().KeyShift;
							bool const alreadySel = selectedFilenames_.find(rsc.name) != selectedFilenames_.end();

							if (multiSel && ctrlHeld) {
								// Ctrl+click: toggle this item
								if (alreadySel)
									selectedFilenames_.erase(rsc.name);
								else {
									selectedFilenames_.insert(rsc.name);
									rangeSelectionStart_ = static_cast<unsigned int>(rscIdx);
								}
							} else if (multiSel && shiftHeld && rangeSelectionStart_ < fileRecords_.size()) {
								// Shift+click: fill range
								unsigned int const first
									= (std::min)(rangeSelectionStart_, static_cast<unsigned int>(rscIdx));
								unsigned int const last
									= (std::max)(rangeSelectionStart_, static_cast<unsigned int>(rscIdx));
								selectedFilenames_.clear();
								for (unsigned int i = first; i <= last; ++i) {
									if (fileRecords_[i].isDir != wantDir)
										continue;
									selectedFilenames_.insert(fileRecords_[i].name);
								}
							} else {
								selectedFilenames_   = {rsc.name};
								rangeSelectionStart_ = static_cast<unsigned int>(rscIdx);
							}
						}
					}
					// Double-click: navigate into directory or confirm file selection.
					if (IsMouseDoubleClicked(ImGuiMouseButton_Left) && hovered) {
						if (rsc.isDir) {
							shouldSetNewDir = true;
							newDir
								= (rsc.name != "..") ? (currentDirectory_ / rsc.name) : currentDirectory_.parent_path();
						} else if (!(flags_ & ImGuiFileBrowserFlags_SelectDirectory)) {
							selectedFilenames_ = {rsc.name};
							isOk_              = true;
							if (!keepOpen_)
								closeContainer();
						}
					}
					if (previewEnabled_ && hoverFileCallback_ && !rsc.isDir && hovered)
						hoverFileCallback_(currentDirectory_ / rsc.name);
					if (!rsc.isDir) {
						auto const fullPath = currentDirectory_ / rsc.name;
						if (contextMenuCallback_)
							contextMenuCallback_(fullPath);
					}

					// Selection highlight rect.
					if (selected) {
						ImVec2 const p0 = GetItemRectMin();
						ImVec2 const p1 = GetItemRectMax();
						GetWindowDrawList()->AddRect(p0, p1, GetColorU32(ImGuiCol_ButtonHovered), 2.0f);
					}
					// Filename label below the thumbnail.
					TextUnformatted(rsc.showName.c_str());
					PopID();
				}
			}
		}

		// ── List view ──────────────────────────────────────────────────────────
		if (!useGridView) {
			for (unsigned int rscIndex = 0; rscIndex < fileRecords_.size(); ++rscIndex) {
				auto const& rsc = fileRecords_[rscIndex];
				if (!rsc.isDir && shouldHideRegularFiles) {
					continue;
				}
				if (!rsc.isDir && !IsExtensionMatched(rsc.extension)) {
					continue;
				}
				if (!rsc.isDir && !isMediaMatched(rsc.extension)) {
					continue;
				}
				if (!rsc.isDir && !tagFilter_.empty()
					&& std::find(rsc.tags.begin(), rsc.tags.end(), tagFilter_) == rsc.tags.end()) {
					continue;
				}
				if (!rsc.name.empty() && rsc.name.c_str()[0] == '$') {
					continue;
				}
				if (!lowerSearch.empty()) {
					std::string const lowerName = ToLower(u8StrToStr(rsc.name.u8string()));
					if (lowerName.find(lowerSearch) == std::string::npos) {
						continue;
					}
				}

				bool const selected = selectedFilenames_.find(rsc.name) != selectedFilenames_.end();

#if IMGUI_VERSION_NUM >= 19100
				const ImGuiSelectableFlags selectableFlag = ImGuiSelectableFlags_NoAutoClosePopups;
#else
				const ImGuiSelectableFlags selectableFlag = ImGuiSelectableFlags_DontClosePopups;
#endif

				// Inline thumbnail when the thumbnail engine is set up.
				float rowHeight = 0.0f;
				if (m_thumbnails.is_setup() && showThumbnails_ && !rsc.isDir) {
					// thumbKey precomputed at scan time; empty => not thumbnailable.
					ImTextureID const thumb = rsc.thumbKey.empty()
						? ImTextureID(0)
						: m_thumbnails.get(rsc.thumbKey, currentDirectory_, rsc.name, rsc.isVideoThumb);
					float const       th    = thumbnailSize_.y;
					float const       ty    = GetCursorPosY();
					if (thumb) {
						SetCursorPosY(ty + 1.0f);
						Image(thumb, thumbnailSize_);
					} else {
						Dummy(thumbnailSize_);
					}
					SameLine();
					SetCursorPosY(ty + (th - GetTextLineHeight()) * 0.5f);
					rowHeight = th + 2.0f;
				}

				if (Selectable(rsc.showName.c_str(), selected, selectableFlag,
						rowHeight > 0.0f ? ImVec2(0.0f, rowHeight) : ImVec2(0.0f, 0.0f))) {
					bool const wantDir     = flags_ & ImGuiFileBrowserFlags_SelectDirectory;
					bool const canSelect   = rsc.name != ".." && rsc.isDir == wantDir;
					bool const rangeSelect = canSelect && GetIO().KeyShift && rangeSelectionStart_ < fileRecords_.size()
						&& (flags_ & ImGuiFileBrowserFlags_MultipleSelection)
						&& IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
					bool const multiSelect = !rangeSelect && GetIO().KeyCtrl
						&& (flags_ & ImGuiFileBrowserFlags_MultipleSelection)
						&& IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);

					if (rangeSelect) {
						unsigned int const first = (std::min)(rangeSelectionStart_, rscIndex);
						unsigned int const last  = (std::max)(rangeSelectionStart_, rscIndex);
						selectedFilenames_.clear();
						for (unsigned int i = first; i <= last; ++i) {
							if (fileRecords_[i].isDir != wantDir) {
								continue;
							}
							if (!wantDir && !IsExtensionMatched(fileRecords_[i].extension)) {
								continue;
							}
							selectedFilenames_.insert(fileRecords_[i].name);
						}
					} else if (selected) {
						if (!multiSelect) {
							selectedFilenames_   = {rsc.name};
							rangeSelectionStart_ = rscIndex;
						} else {
							selectedFilenames_.erase(rsc.name);
						}
						if (flags_ & ImGuiFileBrowserFlags_EnterNewFilename) {
							AssignToArrayStyleString(inputNameBuffer_, "");
						}
					} else if (canSelect) {
						if (multiSelect) {
							selectedFilenames_.insert(rsc.name);
						} else {
							selectedFilenames_ = {rsc.name};
						}
						if (flags_ & ImGuiFileBrowserFlags_EnterNewFilename) {
							auto const rscName = u8StrToStr(rsc.name.u8string());
							AssignToArrayStyleString(inputNameBuffer_, rscName);
						}
						rangeSelectionStart_ = rscIndex;
					}
				}

				if (IsMouseDoubleClicked(ImGuiMouseButton_Left) && IsItemHovered(ImGuiHoveredFlags_None)) {
					if (rsc.isDir) {
						shouldSetNewDir = true;
						newDir = (rsc.name != "..") ? (currentDirectory_ / rsc.name) : currentDirectory_.parent_path();
					} else if (!(flags_ & ImGuiFileBrowserFlags_SelectDirectory)) {
						selectedFilenames_ = {rsc.name};
						isOk_              = true;
						if (!keepOpen_)
							closeContainer();
					}
				} else if (IsKeyPressed(ImGuiKey_GamepadFaceDown) && IsItemHovered()) {
					if (rsc.isDir) {
						shouldSetNewDir = true;
						newDir = (rsc.name != "..") ? (currentDirectory_ / rsc.name) : currentDirectory_.parent_path();
						SetKeyboardFocusHere(-1);
					} else if (!(flags_ & ImGuiFileBrowserFlags_SelectDirectory)) {
						selectedFilenames_ = {rsc.name};
						isOk_              = true;
						if (!keepOpen_)
							closeContainer();
					}
				}

				// Invoke the hover-file callback when the cursor rests on a regular file.
				if (previewEnabled_ && hoverFileCallback_ && !rsc.isDir && IsItemHovered(ImGuiHoveredFlags_None)) {
					hoverFileCallback_(currentDirectory_ / rsc.name);
				}

				// Invoke the context-menu callback for right-click on a regular file.
				if (!rsc.isDir) {
					auto const fullPath = currentDirectory_ / rsc.name;
					if (contextMenuCallback_)
						contextMenuCallback_(fullPath);
				}
			}
		} // if (!useGridView)
	}

	if (shouldSetNewDir) {
		SetDirectory(newDir);
	}

	if (flags_ & ImGuiFileBrowserFlags_EnterNewFilename) {
		PushID(this);
		ScopeGuard popTextID([] { PopID(); });

		if (inputNameBuffer_.empty()) {
			inputNameBuffer_.resize(1, '\0');
		}

		PushItemWidth(-1);
		if (InputText("", inputNameBuffer_.data(), inputNameBuffer_.size(), ImGuiInputTextFlags_CallbackResize,
				ExpandInputBuffer, &inputNameBuffer_)) {
			if (inputNameBuffer_[0] != '\0') {
				selectedFilenames_ = {u8StrToPath(inputNameBuffer_.data())};
			} else {
				selectedFilenames_.clear();
			}
		}
		focusOnInputText |= IsItemFocused();
		PopItemWidth();
	}

	if (!focusOnInputText && !editDir_) {
		bool const selectAll = (flags_ & ImGuiFileBrowserFlags_MultipleSelection) && IsKeyPressed(ImGuiKey_A)
			&& (IsKeyDown(ImGuiKey_LeftCtrl) || IsKeyDown(ImGuiKey_RightCtrl));
		if (selectAll) {
			bool const needDir = flags_ & ImGuiFileBrowserFlags_SelectDirectory;
			selectedFilenames_.clear();
			for (size_t i = 1; i < fileRecords_.size(); ++i) {
				auto& record = fileRecords_[i];
				if (record.isDir == needDir && (needDir || IsExtensionMatched(record.extension))) {
					selectedFilenames_.insert(record.name);
				}
			}
		}
	}

	bool const isEnterPressed = (flags_ & ImGuiFileBrowserFlags_ConfirmOnEnter)
		&& IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && IsKeyPressed(ImGuiKey_Enter);
	if (!(flags_ & ImGuiFileBrowserFlags_SelectDirectory)) {
		BeginDisabled(selectedFilenames_.empty());
		bool const ok = Button("ok");
		EndDisabled();
		if ((ok || isEnterPressed) && !selectedFilenames_.empty()) {
			isOk_ = true;
			if (!keepOpen_)
				closeContainer();
		}
	} else {
		if (Button(" ok ") || isEnterPressed) {
			isOk_ = true;
			if (!keepOpen_)
				closeContainer();
		}
	}

	SameLine();

	bool const doClose = Button("cancel") || shouldClose_
		|| ((flags_ & ImGuiFileBrowserFlags_CloseOnEsc) && IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows)
			&& IsKeyPressed(ImGuiKey_Escape));
	if (doClose) {
		closeContainer();
	}

	if (!statusStr_.empty() && !(flags_ & ImGuiFileBrowserFlags_NoStatusBar)) {
		SameLine();
		Text("%s", statusStr_.c_str());
		if (ImGui::IsItemHovered()) {
			ImGui::BeginTooltip();
			ImGui::PushTextWrapPos(300.0f);
			ImGui::Text("%s", statusStr_.c_str());
			ImGui::PopTextWrapPos();
			ImGui::EndTooltip();
		}
	}

	if (!typeFilters_.empty()) {
		// Right-align the type filter combo.
		float const comboW    = 8.0f * GetFontSize();
		float const rightEdge = GetWindowWidth() - GetStyle().WindowPadding.x;
		float const comboX    = rightEdge - comboW;
		if (comboX > GetCursorPosX() + GetStyle().ItemSpacing.x)
			SameLine(comboX);
		else
			SameLine();
		PushItemWidth(comboW);
		if (BeginCombo("##type_filters", typeFilters_[typeFilterIndex_].c_str())) {
			ScopeGuard guard([&] { EndCombo(); });

			for (size_t i = 0; i < typeFilters_.size(); ++i) {
				bool selected = i == typeFilterIndex_;
				if (Selectable(typeFilters_[i].c_str(), selected) && !selected) {
					typeFilterIndex_ = static_cast<unsigned int>(i);
				}
			}
		}
		PopItemWidth();
	}
}

bool ImGui::FileBrowser::HasSelected() const noexcept { return isOk_; }

bool ImGui::FileBrowser::SetDirectory(std::filesystem::path const& dir) {
	std::filesystem::path const preferredFallback = this->GetDirectory();
	return SetCurrentDirectoryInternal(dir, preferredFallback);
}

std::filesystem::path const& ImGui::FileBrowser::GetDirectory() const noexcept { return currentDirectory_; }

std::filesystem::path ImGui::FileBrowser::GetSelected() const {
	// when isOk_ is true, selectedFilenames_ may be empty if SelectDirectory
	// is enabled. return pwd in that case.
	if (selectedFilenames_.empty()) {
		return currentDirectory_;
	}
	return currentDirectory_ / *selectedFilenames_.begin();
}

std::vector<std::filesystem::path> ImGui::FileBrowser::GetMultiSelected() const {
	if (selectedFilenames_.empty()) {
		return {currentDirectory_};
	}

	std::vector<std::filesystem::path> ret;
	ret.reserve(selectedFilenames_.size());
	for (auto& s : selectedFilenames_) {
		ret.push_back(currentDirectory_ / s);
	}

	return ret;
}

void ImGui::FileBrowser::ClearSelected() {
	selectedFilenames_.clear();
	if ((flags_ & ImGuiFileBrowserFlags_EnterNewFilename)) {
		AssignToArrayStyleString(inputNameBuffer_, "");
	}
	isOk_ = false;
}

void ImGui::FileBrowser::SetTypeFilters(std::vector<std::string> const& _typeFilters) {
	typeFilters_.clear();

	// remove duplicate filter names due to case unsensitivity on windows

#ifdef _WIN32

	std::vector<std::string> typeFilters;
	for (auto& rawFilter : _typeFilters) {
		std::string lowerFilter = ToLower(rawFilter);
		auto const  it          = std::find(typeFilters.begin(), typeFilters.end(), lowerFilter);
		if (it == typeFilters.end()) {
			typeFilters.push_back(std::move(lowerFilter));
		}
	}

#else

	auto& typeFilters = _typeFilters;

#endif

	// insert auto-generated filter
	hasAllFilter_ = false;
	if (typeFilters.size() > 1) {
		hasAllFilter_              = true;
		std::string allFiltersName = std::string();
		for (size_t i = 0; i < typeFilters.size(); ++i) {
			if (typeFilters[i] == std::string_view(".*")) {
				hasAllFilter_ = false;
				break;
			}

			if (i > 0) {
				allFiltersName += ',';
			}
			allFiltersName += typeFilters[i];
		}

		if (hasAllFilter_) {
			typeFilters_.push_back(std::move(allFiltersName));
		}
	}

	std::copy(typeFilters.begin(), typeFilters.end(), std::back_inserter(typeFilters_));
	typeFilterIndex_ = 0;
}

void ImGui::FileBrowser::SetCurrentTypeFilterIndex(int index) { typeFilterIndex_ = static_cast<unsigned int>(index); }

void ImGui::FileBrowser::SetSortModeIndex(int index) {
	if (index < 0) {
		index = 0;
	} else if (index > static_cast<int>(SortField::Created)) {
		index = static_cast<int>(SortField::Created);
	}

	sortField_ = static_cast<SortField>(index);
	RebuildView();
}

int ImGui::FileBrowser::GetSortModeIndex() const noexcept { return static_cast<int>(sortField_); }

void ImGui::FileBrowser::SetSortAscending(bool ascending) {
	sortAscending_ = ascending;
	RebuildView();
}

bool ImGui::FileBrowser::GetSortAscending() const noexcept { return sortAscending_; }

void ImGui::FileBrowser::SetRecentDirectories(std::vector<std::filesystem::path> const& directories) {
	recentDirectories_.clear();
	recentDirectories_.reserve(directories.size() + 1);

	for (auto const& dir : directories) {
		if (dir.empty()) {
			continue;
		}

		std::filesystem::path const absDir = absolute(dir);
		auto const alreadyStored           = std::find(recentDirectories_.begin(), recentDirectories_.end(), absDir);
		if (alreadyStored == recentDirectories_.end()) {
			recentDirectories_.push_back(absDir);
		}
	}

	TouchRecentDirectory(currentDirectory_);
}

std::vector<std::filesystem::path> ImGui::FileBrowser::GetRecentDirectories() const { return recentDirectories_; }

void ImGui::FileBrowser::SetInputName(std::string_view input) {
	assert((flags_ & ImGuiFileBrowserFlags_EnterNewFilename)
		&& "SetInputName can only be called when "
		   "ImGuiFileBrowserFlags_EnterNewFilename is enabled");
	customizedInputName_ = input;
}

void ImGui::FileBrowser::SetHoverFileCallback(std::function<void(std::filesystem::path const&)> cb) {
	hoverFileCallback_ = std::move(cb);
}

void ImGui::FileBrowser::SetContextMenuCallback(std::function<void(std::filesystem::path const&)> cb) {
	contextMenuCallback_ = std::move(cb);
}

void ImGui::FileBrowser::SetRebuildThumbnailCallback(std::function<void(std::filesystem::path const&)> cb) {
	rebuildThumbnailCallback_ = std::move(cb);
}

void ImGui::FileBrowser::SetPreviewEnabled(bool enabled) noexcept { previewEnabled_ = enabled; }

bool ImGui::FileBrowser::IsPreviewEnabled() const noexcept { return previewEnabled_; }

void ImGui::FileBrowser::Setup(vulkan_context* vk, std::filesystem::path thumb_dir, std::string thumbnail_format) {
	m_thumbnails.setup(vk, std::move(thumb_dir), std::move(thumbnail_format));
}

void ImGui::FileBrowser::ClearThumbnailCache() { m_thumbnails.clear(); }

void ImGui::FileBrowser::RebuildThumbnail(std::filesystem::path const& path) { m_thumbnails.evict(path); }

void ImGui::FileBrowser::ShutdownThumbnails() { m_thumbnails.shutdown(); }

void ImGui::FileBrowser::SetThumbnailSize(ImVec2 size) noexcept { thumbnailSize_ = size; }

ImVec2 ImGui::FileBrowser::GetThumbnailSize() const noexcept { return thumbnailSize_; }

void ImGui::FileBrowser::SetViewMode(ViewMode mode) noexcept { viewMode_ = mode; }

ImGui::FileBrowser::ViewMode ImGui::FileBrowser::GetViewMode() const noexcept { return viewMode_; }

void ImGui::FileBrowser::SetGridThumbnailSize(ImVec2 size) noexcept { gridThumbnailSize_ = size; }

ImVec2 ImGui::FileBrowser::GetGridThumbnailSize() const noexcept { return gridThumbnailSize_; }

void ImGui::FileBrowser::SetShowThumbnails(bool show) noexcept { showThumbnails_ = show; }

bool ImGui::FileBrowser::GetShowThumbnails() const noexcept { return showThumbnails_; }

void ImGui::FileBrowser::SetKeepOpen(bool keepOpen) noexcept { keepOpen_ = keepOpen; }

bool ImGui::FileBrowser::GetKeepOpen() const noexcept { return keepOpen_; }

void ImGui::FileBrowser::SetMediaFilter(ImGui::FileBrowser::MediaFilter filter) noexcept { mediaFilter_ = filter; }

ImGui::FileBrowser::MediaFilter ImGui::FileBrowser::GetMediaFilter() const noexcept { return mediaFilter_; }

std::string ImGui::FileBrowser::ToLower(std::string const& s) {
	std::string ret = s;
	for (char& c : ret) {
		c = static_cast<char>(std::tolower(c));
	}
	return ret;
}

void ImGui::FileBrowser::TouchRecentDirectory(std::filesystem::path const& dir) {
	if (dir.empty()) {
		return;
	}

	auto const it = std::find(recentDirectories_.begin(), recentDirectories_.end(), dir);
	if (it != recentDirectories_.end()) {
		recentDirectories_.erase(it);
	}

	recentDirectories_.insert(recentDirectories_.begin(), dir);

	constexpr size_t kMaxRecentDirectories = 32;
	if (recentDirectories_.size() > kMaxRecentDirectories) {
		recentDirectories_.resize(kMaxRecentDirectories);
	}
}

void ImGui::FileBrowser::ToolTip(std::string_view const& s) {
	if (!ImGui::IsItemHovered()) {
		return;
	}
	ImGui::SetTooltip("%s", s.data());
}

void ImGui::FileBrowser::RequestReload() {
	// Hand the heavy directory enumeration to the scanner thread. The previous
	// listing stays on screen until PollScan() applies the finished result.
	statusStr_ = "Scanning...";
	m_scanner.request(currentDirectory_, (flags_ & ImGuiFileBrowserFlags_SkipItemsCausingError) != 0);
}

void ImGui::FileBrowser::PollScan() {
	FileBrowserScanner::Result res;
	if (!m_scanner.poll(res))
		return;

	if (!res.ok) {
		// Keep the previous listing visible; surface the error in the status bar.
		statusStr_ = res.status.empty() ? std::string("error: failed to read directory") : res.status;
		return;
	}

	// Index 0 is always "..", followed by the freshly scanned entries.
	fileRecords_.clear();
	fileRecords_.reserve(res.records.size() + 1);
	fileRecords_.push_back(FileRecord {true, "..", "[D] ..", ""});
	for (auto& rec : res.records) {
		// Precompute the thumbnail fast-path ONCE per scan: classify the extension and
		// build the normalized cache key here (render thread, once per navigation) so the
		// per-frame Display() loop never re-normalizes paths or re-parses extensions.
		if (!rec.isDir) {
			auto const cls = FileBrowserThumbnailContext::classify(rec.name);
			if (cls.thumbnailable) {
				rec.thumbKey     = FileBrowserThumbnailContext::make_key(currentDirectory_ / rec.name);
				rec.isVideoThumb = cls.is_video;
			}
		}
		fileRecords_.push_back(std::move(rec));
	}

	RebuildView();

	// Drop selections that no longer exist in the freshly loaded directory.
	if (!selectedFilenames_.empty()) {
		std::set<std::filesystem::path> kept;
		for (auto const& name : selectedFilenames_)
			if (std::any_of(fileRecords_.begin(), fileRecords_.end(), [&](FileRecord const& rec) {
					return rec.name == name;
				}))
				kept.insert(name);
		selectedFilenames_ = std::move(kept);
	}

	statusStr_.clear();
}

void ImGui::FileBrowser::RebuildView() {
	// Sort entries (index 0 is always ".." and is kept in place). Pure CPU work
	// on the already-loaded records — no directory I/O.
	if (fileRecords_.size() > 2) {
		std::sort(fileRecords_.begin() + 1, fileRecords_.end(), [this](FileRecord const& a, FileRecord const& b) -> bool {
			// Directories always before files, regardless of field/direction.
			if (a.isDir != b.isDir)
				return a.isDir > b.isDir;

			int const cmp = CompareByField(a, b);
			if (cmp != 0)
				return sortAscending_ ? (cmp < 0) : (cmp > 0);

			// Tie on the active field: stable secondary order by name (A-Z),
			// kept ascending so equal-keyed rows read naturally in both
			// directions.
			auto const nameA = ToLower(u8StrToStr(a.name.u8string()));
			auto const nameB = ToLower(u8StrToStr(b.name.u8string()));
			return nameA < nameB;
		});
	}

	// Rebuild the set of tags present in this directory (for the filter combo).
	{
		std::set<std::string> tagSet;
		for (auto const& rec : fileRecords_)
			if (!rec.isDir)
				for (auto const& t : rec.tags)
					tagSet.insert(t);
		availableTags_.assign(tagSet.begin(), tagSet.end());
		// Clear the active filter if the tag is no longer present.
		if (!tagFilter_.empty()
			&& std::find(availableTags_.begin(), availableTags_.end(), tagFilter_) == availableTags_.end())
			tagFilter_.clear();
	}

	ClearRangeSelectionState();
}

int ImGui::FileBrowser::CompareByField(FileRecord const& a, FileRecord const& b) const {
	// Ascending three-way compare for the active field: <0 if a precedes b,
	// >0 if it follows, 0 if the two are equal on this field (the caller then
	// breaks the tie by name). Text fields fold case, matching the rest of the
	// class, which compares names via ToLower.
	auto const threeWay = [](auto const& x, auto const& y) -> int {
		if (x < y)
			return -1;
		if (y < x)
			return 1;
		return 0;
	};

	switch (sortField_) {
	case SortField::Type:
		return threeWay(ToLower(u8StrToStr(a.extension.u8string())), ToLower(u8StrToStr(b.extension.u8string())));
	case SortField::Size:
		return threeWay(a.size, b.size);
	case SortField::Modified:
		return threeWay(a.lastWriteTime, b.lastWriteTime);
	case SortField::Created:
		return threeWay(a.creationTime, b.creationTime);
	case SortField::Name:
		break;
	}

	// SortField::Name (and the default).
	return threeWay(ToLower(u8StrToStr(a.name.u8string())), ToLower(u8StrToStr(b.name.u8string())));
}

void ImGui::FileBrowser::SetCurrentDirectoryUncatched(std::filesystem::path const& pwd) {
	std::filesystem::path const target = absolute(pwd);

	if (!(flags_ & ImGuiFileBrowserFlags_SkipItemsCausingError)) {
		// Openability probe: opening the iterator throws on a bad/denied
		// directory, so SetCurrentDirectoryInternal's catch falls back before we
		// commit. (With SkipItemsCausingError the scanner reports the failure
		// asynchronously instead, matching the previous behaviour.)
		std::filesystem::directory_iterator probe(target);
		(void)probe;
	}

	currentDirectory_ = target;
	TouchRecentDirectory(currentDirectory_);
	RequestReload();

	bool shouldClearInputNameBuffer = true;

	if ((flags_ & ImGuiFileBrowserFlags_EnterNewFilename) && selectedFilenames_.size() == 1
		&& !customizedInputName_.empty() && !inputNameBuffer_.empty()
		&& std::strcmp(inputNameBuffer_.data(), customizedInputName_.data()) == 0) {
		shouldClearInputNameBuffer = false;
	}

	if (shouldClearInputNameBuffer) {
		selectedFilenames_.clear();
		AssignToArrayStyleString(inputNameBuffer_, "");
	}
}

bool ImGui::FileBrowser::SetCurrentDirectoryInternal(std::filesystem::path const& dir,
	std::filesystem::path const&                                                  preferredFallback) {
	try {
		SetCurrentDirectoryUncatched(dir);
		return true;
	} catch (std::exception const& err) {
		statusStr_ = std::string("error: ") + err.what();
	} catch (...) {
		statusStr_ = "unknown error";
	}

	if (preferredFallback != defaultDirectory_) {
		try {
			SetCurrentDirectoryUncatched(preferredFallback);
		} catch (...) {
			SetCurrentDirectoryUncatched(defaultDirectory_);
		}
	} else {
		SetCurrentDirectoryUncatched(defaultDirectory_);
	}

	return false;
}

bool ImGui::FileBrowser::IsExtensionMatched(std::filesystem::path const& _extension) const {
#ifdef _WIN32
	std::filesystem::path extension = ToLower(u8StrToStr(_extension.u8string()));
#else
	auto& extension = _extension;
#endif

	// no type filters
	if (typeFilters_.empty()) {
		return true;
	}

	// invalid type filter index
	if (static_cast<size_t>(typeFilterIndex_) >= typeFilters_.size()) {
		return true;
	}

	// all type filters
	if (hasAllFilter_ && typeFilterIndex_ == 0) {
		for (size_t i = 1; i < typeFilters_.size(); ++i) {
			if (extension == typeFilters_[i]) {
				return true;
			}
		}
		return false;
	}

	// universal filter
	if (typeFilters_[typeFilterIndex_] == std::string_view(".*")) {
		return true;
	}

	// regular filter
	return extension == typeFilters_[typeFilterIndex_];
}

void ImGui::FileBrowser::ClearRangeSelectionState() {
	rangeSelectionStart_ = 9999999;
	bool const dir       = flags_ & ImGuiFileBrowserFlags_SelectDirectory;
	for (unsigned int i = 1; i < fileRecords_.size(); ++i) {
		if (fileRecords_[i].isDir == dir) {
			if (!dir && !IsExtensionMatched(fileRecords_[i].extension)) {
				continue;
			}
			rangeSelectionStart_ = i;
			break;
		}
	}
}

void ImGui::FileBrowser::AssignToArrayStyleString(std::vector<char>& arr, std::string_view content) {
	if (content.empty()) {
		if (!arr.empty()) {
			arr[0] = '\0';
		}
		return;
	}

	if (arr.size() < content.size() + 1) {
		arr.resize(content.size() + 1);
	}
	std::memcpy(arr.data(), content.data(), content.size());
	arr[content.size()] = '\0';
}

int ImGui::FileBrowser::ExpandInputBuffer(ImGuiInputTextCallbackData* callbackData) {
	if (callbackData && callbackData->EventFlag & ImGuiInputTextFlags_CallbackResize) {
		auto   buffer  = static_cast<std::vector<char>*>(callbackData->UserData);
		size_t newSize = buffer->size();
		while (newSize < static_cast<size_t>(callbackData->BufSize)) {
			newSize <<= 1;
		}
		buffer->resize(newSize, '\0');
		callbackData->Buf      = buffer->data();
		callbackData->BufDirty = true;
	}
	return 0;
}

#if defined(__cpp_lib_char8_t)
std::string ImGui::FileBrowser::u8StrToStr(std::u8string s) {
	std::string result;
	result.resize(s.length());
	std::memcpy(result.data(), s.data(), s.length());
	return result;
}
#endif

std::string ImGui::FileBrowser::u8StrToStr(std::string s) { return s; }

std::filesystem::path ImGui::FileBrowser::u8StrToPath(char const* str) {
#if defined(__cpp_lib_char8_t)
	// With C++20/23, it's impossible to efficiently convert a `char*` string to a
	// `char8_t*` string without violating the strict aliasing rule. Bad joke!
	size_t const  len = std::strlen(str);
	std::u8string u8Str;
	u8Str.resize(len);
	std::memcpy(u8Str.data(), str, len);
	return std::filesystem::path(u8Str);
#else
	// u8path is deprecated in C++20
	return std::filesystem::u8path(str);
#endif
}

#ifdef _WIN32

std::uint32_t ImGui::FileBrowser::GetDrivesBitMask() {
	std::uint32_t ret = 0;
	for (int i = 0; i < 26; ++i) {
		char const rootName[4] = {static_cast<char>('A' + i), ':', '\\', '\0'};
		try {
			if (std::filesystem::exists(rootName)) {
				ret |= (1 << i);
			}
		} catch (std::filesystem::filesystem_error const&) {
			// Ignore invalid paths or inaccessible drives, e.g., empty CD drives or
			// network shares
		}
	}
	return ret;
}
#endif