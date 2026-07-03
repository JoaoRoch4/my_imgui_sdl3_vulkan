#include "pch.hpp"

#include "Memory_management.hpp"
#include "file_browser_thumbnail_context.hpp"
#include "file_browser_ui.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <memory>
#include <ranges>
#include <span>
#include <string_view>
#include <utility>

// ─────────────────────────────────────────────────────────────────────────────
// File-local dispatch tables and helpers.
//
// The media-type filter used to classify each extension with a hand-written
// switch whose `||` chains were rebuilt on every call. The classification is
// pure data, so it now lives in constexpr, sorted string_view tables that the
// MediaFilter cases binary-search. Combo label arrays are likewise constexpr
// instead of function-local statics.
// ─────────────────────────────────────────────────────────────────────────────
namespace {

// Lowercase, kept SORTED so the lookups below can binary-search. The MediaFilter
// cases compose these two sets: Images already contains ".gif", therefore
// Media == Images ∪ Videos and "Videos + Gifs" == Videos ∪ {".gif"}.
constexpr std::array<std::string_view, 9> kImageExtensions {
	".avif",
	".bmp",
	".gif",
	".heic",
	".heif",
	".jpeg",
	".jpg",
	".png",
	".webp",
};
constexpr std::array<std::string_view, 9> kVideoExtensions {
	".avi",
	".flv",
	".m4v",
	".mkv",
	".mov",
	".mp4",
	".ts",
	".webm",
	".wmv",
};
static_assert(std::ranges::is_sorted(kImageExtensions), "binary_search requires sorted table");
static_assert(std::ranges::is_sorted(kVideoExtensions), "binary_search requires sorted table");

[[nodiscard]] bool
ExtensionInSet(std::span<std::string_view const> set, std::string_view ext) noexcept {
	return std::ranges::binary_search(set, ext);
}

// Combo label tables (were function-local statics rebuilt on first call).
constexpr std::array<char const*, 5> kSortLabels {"Name", "Type", "Size", "Modified", "Created"};
constexpr std::array<char const*, 2> kSortDirLabels {"Ascending", "Descending"};
constexpr std::array<char const*, 6> kMediaLabels {
	"All", "Media", "Videos", "Images", "Gifs", "Videos + Gifs"};
constexpr std::array<char const*, 3> kViewModeLabels {"List", "Grid", "Masonry"};
constexpr std::array<char const*, 3> kGifPlaybackLabels {"GIF: hover", "GIF: all", "GIF: hybrid"};

// Smooth-scroll easing constants.
constexpr float kPi      = 3.14159265f;
constexpr float kStepRef = 50.0f; // scrollStepPx_ at which scrollAnimDuration_ is used as-is

} // namespace

// ─────────────────────────────────────────────────────────────────────────────
// Construction
// ─────────────────────────────────────────────────────────────────────────────
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
	, keepOpen_(true) {
	assert(!((flags_ & ImGuiFileBrowserFlags_SelectDirectory)
			   && (flags_ & ImGuiFileBrowserFlags_EnterNewFilename))
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

// ─────────────────────────────────────────────────────────────────────────────
// Scroll-animation tuning + window/title accessors
// ─────────────────────────────────────────────────────────────────────────────
void  ImGui::FileBrowser::SetScrollMinPageSizePx(float px) noexcept { scrollMinPageSizePx_ = px; }
float ImGui::FileBrowser::GetScrollMinPageSizePx() const noexcept { return scrollMinPageSizePx_; }

void ImGui::FileBrowser::SetScrollShiftMultiplier(float multiplier) noexcept {
	scrollShiftMultiplier_ = multiplier;
}
float ImGui::FileBrowser::GetScrollShiftMultiplier() const noexcept {
	return scrollShiftMultiplier_;
}

void ImGui::FileBrowser::SetScrollPageSizeRatio(float ratio) noexcept {
	scrollPageSizeRatio_ = ratio;
}
float ImGui::FileBrowser::GetScrollPageSizeRatio() const noexcept { return scrollPageSizeRatio_; }

void ImGui::FileBrowser::SetScrollDistanceSensitivity(float s) noexcept {
	scrollDistanceSensitivity_ = s;
}
float ImGui::FileBrowser::GetScrollDistanceSensitivity() const noexcept {
	return scrollDistanceSensitivity_;
}

void ImGui::FileBrowser::SetScrollAnimDuration(float seconds) noexcept {
	scrollAnimDuration_ = seconds;
}
float ImGui::FileBrowser::GetScrollAnimDuration() const noexcept { return scrollAnimDuration_; }

float ImGui::FileBrowser::GetSelectedThumbnailAspectRatio() const noexcept {
	// Aspect of the FIRST selected file, queried from the globally-shared thumbnail
	// context's LOADED texture (distinct from ThumbnailAspectRatio(), which uses each
	// record's scan-time source_w/source_h). Used by the runtime-config preview pane.
	if (selectedFilenames_.empty()) {
		return 1.f;
	}

	std::string const pathKey = selectedFilenames_.begin()->string();

	auto static const fbc = MemoryManagement::GetInstance<FileBrowserThumbnailContext>();
	if (!fbc) {
		return 64.0f / 36.0f;
	}

	int outW = 0;
	int outH = 0;
	if (fbc->GetLoadedTextureDimensions(pathKey, outW, outH) && outW > 0 && outH > 0) {
		return static_cast<float>(outW) / static_cast<float>(outH);
	}

	// Fallback while background worker threads are still computing the image.
	return 64.0f / 36.0f;
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

// ─────────────────────────────────────────────────────────────────────────────
// Display() — one frame of the browser, decomposed into focused steps.
// SUPER HOT: runs every frame the browser is open.
// ─────────────────────────────────────────────────────────────────────────────
void ImGui::FileBrowser::Display() {
	PushID(this);
	ScopeGuard exitThis([this] {
		shouldOpen_  = false;
		shouldClose_ = false;
		PopID();
	});

	// Reset the per-frame scratch (see the "Per-frame Display() scratch" members).
	isOpened_         = false;
	inputTextFocused_ = false;
	pendingDirSet_    = false;

	// Apply any finished background directory scan, then advance the thumbnail engine
	// frame boundary (reset upload budget, free retired textures, drain async results,
	// advance GIF animation clocks via DeltaTime).
	PollScan();
	m_thumbnails.begin_frame(GetIO().DeltaTime);

	if (!BeginContainer()) {
		return;
	}
	ScopeGuard endContainerGuard([this] { EndContainer(); });

	DrawDirectoryRow(); // breadcrumb / drive / recent combo, or the path edit field
	DrawTopButtons(); // refresh + create-new-directory
	DrawToolbar(); // sort / media / tag / checkboxes / view-mode / search
	DrawFileListChild(); // the scrolling list (+ keyboard handling)
	DrawPermDeleteModal();

	if (pendingDirSet_) {
		SetDirectory(pendingDir_);
	}

	DrawFilenameInputRow();
	HandleSelectAllShortcut();
	DrawActionsRow(); // ok / cancel / status / type-filter
}

bool ImGui::FileBrowser::IsWindowMode() const noexcept {
	return (flags_ & ImGuiFileBrowserFlags_Window) != 0;
}

// Open the right container for the active mode. Returns false when the body must
// be skipped (window not visible, or BeginPopup/Modal returned closed); in the
// window-mode failure case it already calls End() itself, so the caller installs
// the EndContainer() guard ONLY when this returns true.
bool ImGui::FileBrowser::BeginContainer() {
	bool const isWindowMode = IsWindowMode();

	// In popup/modal modes OpenPopup must precede BeginPopup/Modal in the same frame.
	if (!isWindowMode && shouldOpen_) {
		OpenPopup(openLabel_.c_str());
	}

	if (isWindowMode) {
		if (shouldOpen_) {
			windowVisible_ = true;
		}
		if (shouldClose_) {
			windowVisible_ = false;
		}
		if (!windowVisible_) {
			return false;
		}

		if (isPosSet_) {
			SetNextWindowPos(ImVec2(static_cast<float>(posX_), static_cast<float>(posY_)),
				ImGuiCond_FirstUseEver);
		}
		SetNextWindowSize(ImVec2(static_cast<float>(width_), static_cast<float>(height_)),
			ImGuiCond_FirstUseEver);

		ImGuiWindowFlags winFlags = ImGuiWindowFlags_NoFocusOnAppearing;
		if (flags_ & ImGuiFileBrowserFlags_NoTitleBar) {
			winFlags |= ImGuiWindowFlags_NoTitleBar;
		}

		bool open = true;
		if (!Begin(title_.c_str(), &open, winFlags) || !open) {
			if (!open) {
				windowVisible_ = false;
			}
			End();
			return false;
		}
	} else if (shouldOpen_ && (flags_ & ImGuiFileBrowserFlags_NoModal)) {
		if (isPosSet_) {
			SetNextWindowPos(ImVec2(static_cast<float>(posX_), static_cast<float>(posY_)));
		}
		SetNextWindowSize(ImVec2(static_cast<float>(width_), static_cast<float>(height_)));
	} else {
		if (isPosSet_) {
			SetNextWindowPos(ImVec2(static_cast<float>(posX_), static_cast<float>(posY_)),
				ImGuiCond_FirstUseEver);
		}
		SetNextWindowSize(ImVec2(static_cast<float>(width_), static_cast<float>(height_)),
			ImGuiCond_FirstUseEver);
	}

	if (!isWindowMode) {
		if (flags_ & ImGuiFileBrowserFlags_NoModal) {
			if (!BeginPopup(openLabel_.c_str())) {
				return false;
			}
		} else if (
			!BeginPopupModal(openLabel_.c_str(), nullptr,
				flags_ & ImGuiFileBrowserFlags_NoTitleBar ? ImGuiWindowFlags_NoTitleBar : 0)) {
			return false;
		}
	}

	isOpened_ = true;
	return true;
}

void ImGui::FileBrowser::EndContainer() {
	if (IsWindowMode()) {
		End();
	} else {
		EndPopup();
	}
}

void ImGui::FileBrowser::CloseContainer() {
	if (IsWindowMode()) {
		windowVisible_ = false;
	} else {
		CloseCurrentPopup();
	}
}

// ── Directory row ─────────────────────────────────────────────────────────────
void ImGui::FileBrowser::DrawDirectoryRow() {
	if (editDir_) {
		DrawPathEditField();
	} else {
		DrawBreadcrumbBar();
	}
}

void ImGui::FileBrowser::DrawPathEditField() {
	if (setFocusToEditDir_) {
		SetKeyboardFocusHere();
	}

	PushItemWidth(-1);
	bool const enter = InputText("##directory", currDirBuffer_.data(), currDirBuffer_.size(),
		ImGuiInputTextFlags_CallbackResize | ImGuiInputTextFlags_EnterReturnsTrue
			| ImGuiInputTextFlags_AutoSelectAll,
		ExpandInputBuffer, &currDirBuffer_);
	PopItemWidth();

	if (!IsItemActive() && !setFocusToEditDir_) {
		editDir_ = false;
	}
	setFocusToEditDir_ = false;

	if (!enter) {
		return;
	}

	std::filesystem::path enteredDir = u8StrToPath(currDirBuffer_.data());
	if (is_directory(enteredDir)) {
		pendingDir_    = std::move(enteredDir);
		pendingDirSet_ = true;
	} else if (is_directory(enteredDir.parent_path())) {
		pendingDir_    = enteredDir.parent_path();
		pendingDirSet_ = true;
	} else {
		statusStr_ = "[" + std::string(currDirBuffer_.data()) + "] is not a valid directory";
	}
}

void ImGui::FileBrowser::DrawBreadcrumbBar() {
#ifdef _WIN32
	const char currentDrive = static_cast<char>(currentDirectory_.c_str()[0]);
	char const driveStr[]   = {currentDrive, ':', '\0'};

	PushItemWidth(4 * GetFontSize());
	if (BeginCombo("##select_drive", driveStr)) {
		ScopeGuard guard([] { EndCombo(); });

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

	// Breadcrumb path sections. A click navigates immediately (the dstDir build below).
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
				pendingDir_    = dir;
				pendingDirSet_ = true;
			}
		}
	}
}

// ── Refresh + create-new-directory ────────────────────────────────────────────
void ImGui::FileBrowser::DrawTopButtons() {
	SameLine();
	if (SmallButton("*")) {
#ifdef _WIN32
		drives_ = GetDrivesBitMask();
#endif
		// Refresh: re-scan the current directory asynchronously. PollScan() prunes
		// selections that no longer exist once the result arrives.
		RequestReload();
	} else {
		ToolTip("Refresh");
	}

	if (!(flags_ & ImGuiFileBrowserFlags_CreateNewDir)) {
		return;
	}

	SameLine();
	if (SmallButton("+")) {
		OpenPopup(openNewDirLabel_.c_str());
		newDirNameBuffer_[0] = '\0';
	} else {
		ToolTip("Create a new directory");
	}

	if (BeginPopup(openNewDirLabel_.c_str())) {
		ScopeGuard endNewDirPopup([] { EndPopup(); });

		InputText("name", newDirNameBuffer_.data(), newDirNameBuffer_.size(),
			ImGuiInputTextFlags_CallbackResize, ExpandInputBuffer, &newDirNameBuffer_);
		inputTextFocused_ |= IsItemFocused();
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

// ── Toolbar: sort / media / tag / checkboxes / view-mode / search ─────────────
void ImGui::FileBrowser::DrawToolbar() {
	// Sort field selector + independent ascending/descending direction.
	TextDisabled("Sort:");
	SameLine();
	SetNextItemWidth(110.0f);
	int sortIdx = static_cast<int>(sortField_);
	if (Combo("##fb_sort", &sortIdx, kSortLabels.data(), static_cast<int>(kSortLabels.size()))) {
		sortField_ = static_cast<SortField>(sortIdx);
		RebuildView();
	}
	SameLine();
	SetNextItemWidth(120.0f);
	int dirIdx = sortAscending_ ? 0 : 1;
	if (Combo("##fb_sort_dir", &dirIdx, kSortDirLabels.data(),
			static_cast<int>(kSortDirLabels.size()))) {
		sortAscending_ = (dirIdx == 0);
		RebuildView();
	}

	SameLine();
	SetNextItemWidth(90.0f);
	int mfIdx = static_cast<int>(mediaFilter_);
	if (Combo("##fb_media", &mfIdx, kMediaLabels.data(), static_cast<int>(kMediaLabels.size()))) {
		mediaFilter_ = static_cast<MediaFilter>(mfIdx);
	}

	if (!availableTags_.empty()) {
		SameLine();
		SetNextItemWidth(110.0f);
		std::string const tagPreview = tagFilter_.empty() ? "Tag: All" : ("Tag: " + tagFilter_);
		if (BeginCombo("##fb_tags", tagPreview.c_str())) {
			if (Selectable("All##fb_tag_all", tagFilter_.empty())) {
				tagFilter_.clear();
			}
			for (auto const& t : availableTags_) {
				if (Selectable(t.c_str(), tagFilter_ == t)) {
					tagFilter_ = t;
				}
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
		if (Checkbox("Thumbnails", &showThumbnails_) && !showThumbnails_) {
			m_thumbnails.release_textures();
		}
		ToolTip("Show or hide file thumbnails");
		if (showThumbnails_) {
			SameLine();
			int vmIdx = static_cast<int>(viewMode_);
			if (Combo("##View mode", &vmIdx, kViewModeLabels.data(),
					static_cast<int>(kViewModeLabels.size()))) {
				viewMode_ = static_cast<ViewMode>(vmIdx);
			}
			ToolTip("Cycle list / grid / masonry view");

			// Inline animated-GIF playback policy (run config).
			SameLine();
			SetNextItemWidth(110.0f);
			int gpIdx = static_cast<int>(GetGifPlayback());
			if (Combo("##gif_playback", &gpIdx, kGifPlaybackLabels.data(),
					static_cast<int>(kGifPlaybackLabels.size()))) {
				SetGifPlayback(static_cast<GifPlayback>(gpIdx));
				// Re-evaluate GIF thumbnails immediately under the new policy (drop animations,
				// revert to stills / start animating) instead of waiting for the next hover.
				m_thumbnails.refresh_gifs();
			}
			ToolTip("Animate GIFs: hovered only / all visible / hybrid (throttled)");
		}
	}

	DrawSearchBar();
}

void ImGui::FileBrowser::DrawSearchBar() {
	PushItemWidth(-1);
	char searchBuf[256] = {};
	std::strncpy(searchBuf, searchStr_.c_str(), sizeof(searchBuf) - 1);
	if (InputTextWithHint("##fb_search", "Search...", searchBuf, sizeof(searchBuf))) {
		searchStr_ = searchBuf;
	}
	PopItemWidth();
}

// ─────────────────────────────────────────────────────────────────────────────
// Per-row helpers shared by the List / Grid / Masonry views.
// ─────────────────────────────────────────────────────────────────────────────

// The display filter applied to every record in every view. Reads the per-frame
// caches searchLower_ / hideRegularFiles_ filled at the top of DrawFileListChild().
bool ImGui::FileBrowser::PassesDisplayFilter(FileRecord const& rsc) const {
	if (!rsc.isDir) {
		if (hideRegularFiles_) {
			return false;
		}
		if (!IsExtensionMatched(rsc.extension)) {
			return false;
		}
		if (!MatchesMediaFilter(rsc.extension)) {
			return false;
		}
		if (!tagFilter_.empty() && std::ranges::find(rsc.tags, tagFilter_) == rsc.tags.end()) {
			return false;
		}
	}
	if (!rsc.name.empty() && rsc.name.c_str()[0] == '$') {
		return false;
	}
	if (!searchLower_.empty()
		&& ToLower(u8StrToStr(rsc.name.u8string())).find(searchLower_) == std::string::npos) {
		return false;
	}
	return true;
}

// A/D navigation target test: a visible, thumbnailable IMAGE (skip dirs + videos).
bool ImGui::FileBrowser::IsImageNavCandidate(FileRecord const& r) const {
	if (r.isDir || r.thumbKey.empty() || r.isVideoThumb) {
		return false;
	}
	if (!IsExtensionMatched(r.extension) || !MatchesMediaFilter(r.extension)) {
		return false;
	}
	if (!tagFilter_.empty() && std::ranges::find(r.tags, tagFilter_) == r.tags.end()) {
		return false;
	}
	if (!r.name.empty() && r.name.c_str()[0] == '$') {
		return false;
	}
	if (!searchLower_.empty()
		&& ToLower(u8StrToStr(r.name.u8string())).find(searchLower_) == std::string::npos) {
		return false;
	}
	return true; // FIX: the original returned false here, so A/D image-nav never matched.
}

bool ImGui::FileBrowser::MatchesMediaFilter(std::filesystem::path const& extension) const {
	if (mediaFilter_ == MediaFilter::All) {
		return true;
	}

	std::string e = extension.string();
	std::ranges::transform(e, e.begin(), [](unsigned char c) {
		return static_cast<char>(std::tolower(c));
	});
	std::string_view const ext = e;

	switch (mediaFilter_) {
	case MediaFilter::Media:
		return ExtensionInSet(kImageExtensions, ext) || ExtensionInSet(kVideoExtensions, ext);
	case MediaFilter::Videos:
		return ExtensionInSet(kVideoExtensions, ext);
	case MediaFilter::Images:
		return ExtensionInSet(kImageExtensions, ext);
	case MediaFilter::Gifs:
		return ext == ".gif";
	case MediaFilter::Videos_Gifs:
		return ExtensionInSet(kVideoExtensions, ext) || ext == ".gif";
	case MediaFilter::All:
		break;
	}
	return true;
}

// Single-click selection (Grid + Masonry). Mirrors the Selectable-driven logic in
// the list view, which lives inline because it also drives the rename buffer.
void ImGui::FileBrowser::HandleSelectionClick(FileRecord const& rsc, unsigned int idx) {
	bool const wantDir   = (flags_ & ImGuiFileBrowserFlags_SelectDirectory) != 0;
	bool const canSelect = rsc.name != ".." && rsc.isDir == wantDir;
	if (!canSelect) {
		return;
	}

	bool const multiSel = (flags_ & ImGuiFileBrowserFlags_MultipleSelection) != 0
		&& IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
	bool const ctrlHeld   = GetIO().KeyCtrl;
	bool const shiftHeld  = GetIO().KeyShift;
	bool const alreadySel = selectedFilenames_.contains(rsc.name);

	if (multiSel && ctrlHeld) {
		if (alreadySel) {
			selectedFilenames_.erase(rsc.name);
		} else {
			selectedFilenames_.insert(rsc.name);
			rangeSelectionStart_ = idx;
		}
	} else if (multiSel && shiftHeld && rangeSelectionStart_ < fileRecords_.size()) {
		FillRangeSelection(rangeSelectionStart_, idx, wantDir, /*requireExtMatch=*/false);
	} else {
		selectedFilenames_   = {rsc.name};
		rangeSelectionStart_ = idx;
	}
}

// Double-click / gamepad activation: navigate into a directory or confirm a file.
void ImGui::FileBrowser::HandleActivate(FileRecord const& rsc, bool focusPrevAfterDirNav) {
	if (rsc.isDir) {
		pendingDirSet_ = true;
		pendingDir_
			= (rsc.name != "..") ? (currentDirectory_ / rsc.name) : currentDirectory_.parent_path();
		if (focusPrevAfterDirNav) {
			SetKeyboardFocusHere(-1);
		}
	} else if (!(flags_ & ImGuiFileBrowserFlags_SelectDirectory)) {
		selectedFilenames_ = {rsc.name};
		isOk_              = true;
		if (!keepOpen_) {
			CloseContainer();
		}
	}
}

// Hover-preview + context-menu callbacks for a regular-file row.
void ImGui::FileBrowser::FireRowCallbacks(FileRecord const& rsc, bool hovered) {
	if (rsc.isDir) {
		return;
	}
	// Tell the thumbnail engine which GIF is hovered (consumed next frame) so the
	// HoverOnly / Hybrid playback modes can animate it at full rate.
	if (rsc.isGifThumb && hovered && !rsc.thumbKey.empty()) {
		m_thumbnails.note_gif_hover(rsc.thumbKey);
	}
	auto const fullPath = currentDirectory_ / rsc.name;
	if (previewEnabled_ && hoverFileCallback_ && hovered) {
		hoverFileCallback_(fullPath);
	}
	if (contextMenuCallback_) {
		contextMenuCallback_(fullPath);
	}
}

// Replace the current selection with records in [a, b] matching the wanted kind.
void ImGui::FileBrowser::FillRangeSelection(unsigned int a, unsigned int b, bool wantDir,
	bool requireExtMatch) {
	unsigned int const first = (std::min)(a, b);
	unsigned int const last  = (std::max)(a, b);
	selectedFilenames_.clear();
	for (unsigned int i = first; i <= last && i < fileRecords_.size(); ++i) {
		if (fileRecords_[i].isDir != wantDir) {
			continue;
		}
		if (requireExtMatch && !wantDir && !IsExtensionMatched(fileRecords_[i].extension)) {
			continue;
		}
		selectedFilenames_.insert(fileRecords_[i].name);
	}
}

ImTextureID ImGui::FileBrowser::ThumbnailFor(FileRecord const& rsc) {
	// thumbKey is precomputed at scan time; empty => not thumbnailable.
	if (rsc.thumbKey.empty()) {
		return ImTextureID(0);
	}
	return m_thumbnails.get(rsc.thumbKey, currentDirectory_, rsc.name, rsc.isVideoThumb,
		rsc.isGifThumb);
}

// Native aspect (width / height) of a record's thumbnail. Images carry their real
// (source_w, source_h) from scan time; videos/unknowns fall back to square. This is
// the per-thumbnail proportion used to letterbox/size every cell.
float ImGui::FileBrowser::ThumbnailAspectRatio(FileRecord const& rsc) const noexcept {
	if (rsc.source_w > 0 && rsc.source_h > 0) {
		return static_cast<float>(rsc.source_w) / static_cast<float>(rsc.source_h);
	}
	return 1.0f;
}

// Draw an image at its CORRECT proportion, centred inside `cell`. Image thumbnails are
// stored at native aspect/full resolution, so they letterbox at display time without
// distortion. Videos keep a fixed-AR letterboxed texture, so they fill the cell.
void ImGui::FileBrowser::DrawThumbnailFitted(ImTextureID tex, FileRecord const& rsc,
	ImVec2 cell) const {
	if (rsc.isVideoThumb) { // letterboxed video frame — fill the cell
		ImVec2 const cur = GetCursorScreenPos();
		Dummy(cell);
		GetWindowDrawList()->AddImage(tex, cur, {cur.x + cell.x, cur.y + cell.y});
		return;
	}

	float const a = std::clamp(ThumbnailAspectRatio(rsc), 0.25f, 4.0f);
	float       w = cell.x, h = cell.x / a;
	if (h > cell.y) {
		h = cell.y;
		w = cell.y * a;
	}
	ImVec2 const cur = GetCursorScreenPos();
	Dummy(cell); // reserve the full cell so hit-testing/layout match the old Image()
	ImVec2 const o = {(cell.x - w) * 0.5f, (cell.y - h) * 0.5f};
	GetWindowDrawList()->AddImage(tex, {cur.x + o.x, cur.y + o.y},
		{cur.x + o.x + w, cur.y + o.y + h});
}

// ── File-list child: keyboard handling + view dispatch ────────────────────────
void ImGui::FileBrowser::DrawFileListChild() {
	// Reserve bottom rows: action buttons + optional input-name row.
	float reserveHeight = GetFrameHeightWithSpacing();
	if (flags_ & ImGuiFileBrowserFlags_EnterNewFilename) {
		reserveHeight += GetFrameHeightWithSpacing();
	}

	BeginChild("ch", ImVec2(0, -reserveHeight), true,
		(flags_ & ImGuiFileBrowserFlags_NoModal) ? ImGuiWindowFlags_AlwaysHorizontalScrollbar : 0);
	ScopeGuard endChild([] { EndChild(); });

	HandleThumbnailWheelZoom();

	// Per-frame filter caches consumed by PassesDisplayFilter / IsImageNavCandidate.
	hideRegularFiles_ = (flags_ & ImGuiFileBrowserFlags_HideRegularFiles)
		&& (flags_ & ImGuiFileBrowserFlags_SelectDirectory);
	searchLower_ = searchStr_.empty() ? std::string() : ToLower(searchStr_);

	HandleListKeyboard();

	bool const useGridView
		= (viewMode_ == ViewMode::Grid) && m_thumbnails.is_setup() && showThumbnails_;
	bool const useMasonryView
		= (viewMode_ == ViewMode::Masonry) && m_thumbnails.is_setup() && showThumbnails_;

	if (useGridView) {
		RenderGridView();
	} else if (useMasonryView) {
		RenderMasonryView();
	} else {
		RenderListView();
	}

	scrollToIdx_ = -1; // consumed by the view loops above (SetScrollHereY)
}

// Shift + mouse-wheel scales thumbnails when the file list is hovered.
void ImGui::FileBrowser::HandleThumbnailWheelZoom() {
	if (!(m_thumbnails.is_setup() && showThumbnails_
			&& IsWindowHovered(ImGuiHoveredFlags_ChildWindows) && GetIO().KeyShift)) {
		return;
	}
	float const wheel = GetIO().MouseWheel;
	if (wheel == 0.0f) {
		return;
	}

	float const factor = (wheel > 0.0f) ? 1.1f : (1.0f / 1.1f);
	// Per-mode minimum width + which size field to scale.
	float       minW   = 24.0f;
	ImVec2*     sz     = &thumbnailSize_;
	if (viewMode_ == ViewMode::Grid) {
		minW = 64.0f;
		sz   = &gridThumbnailSize_;
	} else if (viewMode_ == ViewMode::Masonry) {
		minW = 80.0f;
		sz   = &masonryThumbnailSize_;
	}
	sz->x = std::clamp(sz->x * factor, minW, std::numeric_limits<float>::max());
}

// Keyboard navigation / actions, active only when the explorer is focused and no
// text field is being edited, so it never steals keys from the search/rename inputs.
void ImGui::FileBrowser::HandleListKeyboard() {
	if (!IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) || IsAnyItemActive()) {
		return;
	}
	ImGuiIO const& io    = GetIO();
	bool const     shift = io.KeyShift;
	bool const     ctrl  = io.KeyCtrl;

	if (!ctrl) {
		HandleSmoothScroll(io, shift);
		HandleImageNavKeys();
	}
	HandleDeleteKeys(shift);
}

// W/S scroll the list with a smooth ease-in-out-cosine glide (Shift pages a near-full
// view height). Each press re-aims the target from the CURRENT position and restarts
// the ease — tap glides once, hold scrolls continuously. A wheel tick cancels the glide.
void ImGui::FileBrowser::HandleSmoothScroll(ImGuiIO const& io, bool shift) {
	float const maxScrollY = GetScrollMaxY();

	// scrollStepPx_ IS the pixel distance per keypress — use it directly.
	float const stepSize = static_cast<float>(scrollStepPx_) * scrollDistanceSensitivity_;

	// Larger step → shorter duration → snappier feel. At scrollStepPx_ == kStepRef the
	// base duration is used as-is; above it the duration shrinks, floored at 50 ms.
	float const dynamicDuration = std::clamp(
		scrollAnimDuration_ * (kStepRef / static_cast<float>(scrollStepPx_)), 0.05f,
		scrollAnimDuration_);

	float deltaY = 0.0f;
	if (IsKeyPressed(ImGuiKey_W, true)) {
		deltaY -= shift ? stepSize * scrollShiftMultiplier_ : stepSize;
	}
	if (IsKeyPressed(ImGuiKey_S, true)) {
		deltaY += shift ? stepSize * scrollShiftMultiplier_ : stepSize;
	}

	if (deltaY != 0.0f) {
		scrollAnimStart_   = GetScrollY();
		scrollAnimTarget_  = std::clamp(GetScrollY() + deltaY, 0.0f, maxScrollY);
		scrollAnimElapsed_ = 0.0f;
		scrollAnimActive_  = true;
	}

	if (!scrollAnimActive_) {
		return;
	}
	if (io.MouseWheel != 0.0f) {
		scrollAnimActive_ = false;
		return;
	}

	scrollAnimElapsed_       += io.DeltaTime;
	float const progress      = (dynamicDuration > 0.0f)
			 ? std::clamp(scrollAnimElapsed_ / dynamicDuration, 0.0f, 1.0f)
			 : 1.0f;
	float const easingFactor  = (1.0f - std::cos(kPi * progress)) * 0.5f;
	SetScrollY(scrollAnimStart_ + (scrollAnimTarget_ - scrollAnimStart_) * easingFactor);
	if (progress >= 1.0f) {
		scrollAnimActive_ = false;
	}
}

// A/D jump to the previous/next visible IMAGE (skip dirs + videos, wrap at the ends):
// move the selection, scroll it into view, fire the hover preview + open callbacks.
void ImGui::FileBrowser::HandleImageNavKeys() {
	int dir = 0;
	if (IsKeyPressed(ImGuiKey_D, false)) {
		dir = +1;
	} else if (IsKeyPressed(ImGuiKey_A, false)) {
		dir = -1;
	}
	if (dir == 0 || fileRecords_.empty()) {
		return;
	}

	auto const n   = static_cast<int>(fileRecords_.size());
	int        cur = -1;
	if (!selectedFilenames_.empty()) {
		auto const& sel = *selectedFilenames_.begin();
		for (int i = 0; i < n; ++i) {
			if (fileRecords_[static_cast<std::size_t>(i)].name == sel) {
				cur = i;
				break;
			}
		}
	}

	int const start = (cur >= 0) ? cur : (dir > 0 ? -1 : n);
	for (int step = 1; step <= n; ++step) {
		int const   idx = ((start + dir * step) % n + n) % n;
		auto const& rec = fileRecords_[static_cast<std::size_t>(idx)];
		if (!IsImageNavCandidate(rec)) {
			continue;
		}
		selectedFilenames_   = {rec.name};
		rangeSelectionStart_ = static_cast<unsigned int>(idx);
		scrollToIdx_         = idx;
		auto const full      = currentDirectory_ / rec.name;
		// A/D only MOVES the selection (+ scrolls it into view + updates the hover preview).
		// It deliberately does NOT open the file in the viewer — openFileCallback_ is left
		// for an explicit open action (e.g. Enter / double-click), not navigation.
		if (hoverFileCallback_) {
			hoverFileCallback_(full);
		}
		break;
	}
}

// Delete -> OS trash (recoverable). Shift+Delete -> permanent (confirm modal, opened
// outside the child). Batch: every selected file; ".." is skipped.
void ImGui::FileBrowser::HandleDeleteKeys(bool shift) {
	if (!IsKeyPressed(ImGuiKey_Delete, false) || selectedFilenames_.empty()) {
		return;
	}

	std::vector<std::filesystem::path> targets;
	for (auto const& nm : selectedFilenames_) {
		if (nm == "..") {
			continue;
		}
		targets.push_back(currentDirectory_ / nm);
	}
	if (targets.empty()) {
		return;
	}

	if (shift) {
		pendingPermDelete_   = std::move(targets);
		wantPermDeleteModal_ = true;
		return;
	}

	// Single-quote each path (escaping embedded quotes) so spaces and shell
	// metacharacters in filenames can't break or inject into the command.
	auto const shq = [](std::string const& s) {
		std::string o = "'";
		for (char c : s) {
			o += (c == '\'') ? std::string("'\\''") : std::string(1, c);
		}
		o += "'";
		return o;
	};
	for (auto const& p : targets) {
		std::string const cmd = "gio trash -- " + shq(p.string()) + " >/dev/null 2>&1";
		std::system(cmd.c_str()); // NOLINT(cert-env33-c)
		m_thumbnails.evict(p);
	}
	selectedFilenames_.clear();
	RequestReload();
}

// ── Grid view ─────────────────────────────────────────────────────────────────
void ImGui::FileBrowser::RenderGridView() {
	float const availW = GetContentRegionAvail().x;
	float const cellW  = gridThumbnailSize_.x + GetStyle().ItemSpacing.x;
	int const   cols   = std::max(1, static_cast<int>(availW / cellW));

	if (!BeginTable("##fb_grid", cols)) {
		return;
	}
	ScopeGuard endGrid([] { EndTable(); });

	float const thumbW = gridThumbnailSize_.x;
	float const thumbH = gridThumbnailSize_.y;

	for (unsigned int rscIdx = 0; rscIdx < fileRecords_.size(); ++rscIdx) {
		auto const& rsc = fileRecords_[rscIdx];
		if (!PassesDisplayFilter(rsc)) {
			continue;
		}

		TableNextColumn();
		PushID(static_cast<int>(rscIdx));
		ScopeGuard popId([] { PopID(); });

		if (static_cast<int>(rscIdx) == scrollToIdx_) {
			SetScrollHereY(0.5f); // A/D navigation: bring the new selection into view
		}

		bool const selected = selectedFilenames_.contains(rsc.name);

		// Thumbnail image or placeholder.
		if (rsc.isDir) {
			Dummy({thumbW, thumbH});
		} else {
			ImTextureID const thumb = ThumbnailFor(rsc);
			if (thumb) {
				DrawThumbnailFitted(thumb, rsc, {thumbW, thumbH});
			} else {
				Dummy({thumbW, thumbH});
			}
		}

		bool const hovered = IsItemHovered(ImGuiHoveredFlags_None);
		if (hovered && IsMouseClicked(ImGuiMouseButton_Left)) {
			HandleSelectionClick(rsc, rscIdx);
		}
		if (IsMouseDoubleClicked(ImGuiMouseButton_Left) && hovered) {
			HandleActivate(rsc);
		}
		FireRowCallbacks(rsc, hovered);

		if (selected) {
			GetWindowDrawList()->AddRect(GetItemRectMin(), GetItemRectMax(),
				GetColorU32(ImGuiCol_ButtonHovered), 2.0f);
		}
		TextUnformatted(rsc.showName.c_str());
	}
}

// ── Masonry view ──────────────────────────────────────────────────────────────
// Column-balanced packing: every cell is sized to `colW × colW / aspect` and placed
// in the column with the smallest current Y. Image thumbnails are stored at native
// aspect/full resolution, so the cell aspect matches the texture and we fill it
// directly — no letterbox bars, no distortion. (Video thumbs keep a fixed-AR frame.)
void ImGui::FileBrowser::RenderMasonryView() {
	float const availW   = GetContentRegionAvail().x;
	float const spacingX = GetStyle().ItemSpacing.x;
	float const spacingY = GetStyle().ItemSpacing.y;
	float const desiredW = std::max(80.0f, masonryThumbnailSize_.x);
	// A non-zero masonryColumns_ forces an exact column count (run-config slider);
	// 0 auto-picks using desiredW as a *maximum* per-column width hint (ceil-based so a
	// wide window gets several narrower columns, not one giant column).
	int const   cols     = (masonryColumns_ > 0)
			  ? std::max(1, masonryColumns_)
			  : std::max(1, static_cast<int>(std::ceil((availW + spacingX) / (desiredW + spacingX))));
	// Distribute leftover slack evenly so the rightmost column hugs the edge.
	float const colW
		= std::floor((availW - static_cast<float>(cols - 1) * spacingX) / static_cast<float>(cols));

	ImVec2 const       origin = GetCursorPos(); // top-left of the BeginChild content
	std::vector<float> colY(static_cast<std::size_t>(cols), 0.0f);

	for (unsigned int rscIdx = 0; rscIdx < fileRecords_.size(); ++rscIdx) {
		auto const& rsc = fileRecords_[rscIdx];
		if (!PassesDisplayFilter(rsc)) {
			continue;
		}

		// Pick the shortest column for this cell (ties: leftmost — std::min_element).
		auto const  colIt  = std::min_element(colY.begin(), colY.end());
		int const   col    = static_cast<int>(std::distance(colY.begin(), colIt));
		float const aspect = std::clamp(ThumbnailAspectRatio(rsc), 0.25f, 4.0f);
		float const cellW  = colW;
		float const cellH  = std::round(cellW / aspect);
		float const labelH = GetTextLineHeightWithSpacing();
		float const slotH  = cellH + labelH + spacingY;

		ImVec2 const slotPos {origin.x + static_cast<float>(col) * (colW + spacingX),
			origin.y + colY[static_cast<std::size_t>(col)]};
		SetCursorPos(slotPos);
		PushID(static_cast<int>(rscIdx));
		ScopeGuard popId([] { PopID(); });

		if (static_cast<int>(rscIdx) == scrollToIdx_) {
			SetScrollHereY(1.0f); // A/D navigation: bring the new selection into view
		}

		bool const selected = selectedFilenames_.contains(rsc.name);

		if (rsc.isDir) {
			Dummy({cellW, cellH});
		} else {
			ImTextureID const thumb = ThumbnailFor(rsc);
			if (thumb) {
				Image(thumb, {cellW, cellH});
			} else {
				Dummy({cellW, cellH});
			}
		}

		bool const hovered = IsItemHovered(ImGuiHoveredFlags_None);
		if (hovered && IsMouseClicked(ImGuiMouseButton_Left)) {
			HandleSelectionClick(rsc, rscIdx);
		}
		if (IsMouseDoubleClicked(ImGuiMouseButton_Left) && hovered) {
			HandleActivate(rsc);
		}
		FireRowCallbacks(rsc, hovered);

		if (selected) {
			GetWindowDrawList()->AddRect(GetItemRectMin(), GetItemRectMax(),
				GetColorU32(ImGuiCol_ButtonHovered), 2.0f);
		}

		// Place the label tight under the thumbnail.
		SetCursorPos({slotPos.x, slotPos.y + cellH});
		PushTextWrapPos(slotPos.x + cellW);
		TextUnformatted(rsc.showName.c_str());
		PopTextWrapPos();

		colY[static_cast<std::size_t>(col)] += slotH;
	}

	// Reserve the full masonry height so the scrollbar matches what was drawn.
	float const totalH = colY.empty() ? 0.0f : *std::max_element(colY.begin(), colY.end());
	SetCursorPos({origin.x, origin.y + totalH});
	Dummy({1.0f, 1.0f});
}

// ── List view ─────────────────────────────────────────────────────────────────
void ImGui::FileBrowser::RenderListView() {
#if IMGUI_VERSION_NUM >= 19100
	constexpr ImGuiSelectableFlags selectableFlag = ImGuiSelectableFlags_NoAutoClosePopups;
#else
	constexpr ImGuiSelectableFlags selectableFlag = ImGuiSelectableFlags_DontClosePopups;
#endif

	bool const wantDir          = (flags_ & ImGuiFileBrowserFlags_SelectDirectory) != 0;
	bool const showInlineThumbs = m_thumbnails.is_setup() && showThumbnails_;

	for (unsigned int rscIndex = 0; rscIndex < fileRecords_.size(); ++rscIndex) {
		auto const& rsc = fileRecords_[rscIndex];
		if (!PassesDisplayFilter(rsc)) {
			continue;
		}

		if (static_cast<int>(rscIndex) == scrollToIdx_) {
			SetScrollHereY(1.0f); // A/D navigation: bring the new selection into view
		}

		bool const selected = selectedFilenames_.contains(rsc.name);

		// Inline thumbnail when the thumbnail engine is set up.
		float rowHeight = 0.0f;
		if (showInlineThumbs && !rsc.isDir) {
			ImTextureID const thumb = ThumbnailFor(rsc);
			float const       th    = thumbnailSize_.y;
			float const       ty    = GetCursorPosY();
			if (thumb) {
				SetCursorPosY(ty + 1.0f);
				DrawThumbnailFitted(thumb, rsc, thumbnailSize_);
			} else {
				Dummy(thumbnailSize_);
			}
			SameLine();
			SetCursorPosY(ty + (th - GetTextLineHeight()) * 0.5f);
			rowHeight = th + 2.0f;
		}

		if (Selectable(rsc.showName.c_str(), selected, selectableFlag,
				rowHeight > 0.0f ? ImVec2(0.0f, rowHeight) : ImVec2(0.0f, 0.0f))) {
			bool const canSelect   = rsc.name != ".." && rsc.isDir == wantDir;
			bool const rangeSelect = canSelect && GetIO().KeyShift
				&& rangeSelectionStart_ < fileRecords_.size()
				&& (flags_ & ImGuiFileBrowserFlags_MultipleSelection)
				&& IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
			bool const multiSelect = !rangeSelect && GetIO().KeyCtrl
				&& (flags_ & ImGuiFileBrowserFlags_MultipleSelection)
				&& IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);

			if (rangeSelect) {
				FillRangeSelection(rangeSelectionStart_, rscIndex, wantDir, /*requireExtMatch=*/true);
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
					AssignToArrayStyleString(inputNameBuffer_, u8StrToStr(rsc.name.u8string()));
				}
				rangeSelectionStart_ = rscIndex;
			}
		}

		if (IsMouseDoubleClicked(ImGuiMouseButton_Left) && IsItemHovered(ImGuiHoveredFlags_None)) {
			HandleActivate(rsc);
		} else if (IsKeyPressed(ImGuiKey_GamepadFaceDown) && IsItemHovered()) {
			HandleActivate(rsc, /*focusPrevAfterDirNav=*/true);
		}

		FireRowCallbacks(rsc, IsItemHovered(ImGuiHoveredFlags_None));
	}
}

// ── Permanent-delete confirmation (Shift+Delete) ──────────────────────────────
// Opened OUTSIDE the "ch" child so OpenPopup and BeginPopupModal share the same ID
// scope. Trash deletes need no confirmation (recoverable); this bypasses the trash.
void ImGui::FileBrowser::DrawPermDeleteModal() {
	if (wantPermDeleteModal_) {
		OpenPopup("##fb_perm_delete");
		wantPermDeleteModal_ = false;
	}
	if (!BeginPopupModal("##fb_perm_delete", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
		return;
	}
	ScopeGuard endPopup([] { EndPopup(); });

	Text("Permanently delete %d item(s)?", static_cast<int>(pendingPermDelete_.size()));
	TextDisabled("This bypasses the trash and cannot be undone.");
	Separator();
	bool const confirm = Button("Delete permanently", ImVec2(180.0f, 0.0f));
	SameLine();
	bool const cancel = Button("Cancel", ImVec2(120.0f, 0.0f)) || IsKeyPressed(ImGuiKey_Escape);

	if (confirm) {
		for (auto const& p : pendingPermDelete_) {
			std::error_code ec;
			std::filesystem::remove_all(p, ec);
			m_thumbnails.evict(p);
		}
		pendingPermDelete_.clear();
		selectedFilenames_.clear();
		RequestReload();
		CloseCurrentPopup();
	} else if (cancel) {
		pendingPermDelete_.clear();
		CloseCurrentPopup();
	}
}

// ── Bottom rows: rename input, select-all, ok/cancel/status/type-filter ───────
void ImGui::FileBrowser::DrawFilenameInputRow() {
	if (!(flags_ & ImGuiFileBrowserFlags_EnterNewFilename)) {
		return;
	}

	PushID(this);
	ScopeGuard popTextID([] { PopID(); });

	if (inputNameBuffer_.empty()) {
		inputNameBuffer_.resize(1, '\0');
	}

	PushItemWidth(-1);
	if (InputText("", inputNameBuffer_.data(), inputNameBuffer_.size(),
			ImGuiInputTextFlags_CallbackResize, ExpandInputBuffer, &inputNameBuffer_)) {
		if (inputNameBuffer_[0] != '\0') {
			selectedFilenames_ = {u8StrToPath(inputNameBuffer_.data())};
		} else {
			selectedFilenames_.clear();
		}
	}
	inputTextFocused_ |= IsItemFocused();
	PopItemWidth();
}

void ImGui::FileBrowser::HandleSelectAllShortcut() {
	if (inputTextFocused_ || editDir_) {
		return;
	}

	bool const selectAll = (flags_ & ImGuiFileBrowserFlags_MultipleSelection)
		&& IsKeyPressed(ImGuiKey_A)
		&& (IsKeyDown(ImGuiKey_LeftCtrl) || IsKeyDown(ImGuiKey_RightCtrl));
	if (!selectAll) {
		return;
	}

	bool const needDir = flags_ & ImGuiFileBrowserFlags_SelectDirectory;
	selectedFilenames_.clear();
	for (size_t i = 1; i < fileRecords_.size(); ++i) {
		auto& record = fileRecords_[i];
		if (record.isDir == needDir && (needDir || IsExtensionMatched(record.extension))) {
			selectedFilenames_.insert(record.name);
		}
	}
}

void ImGui::FileBrowser::DrawActionsRow() {
	bool const isEnterPressed = (flags_ & ImGuiFileBrowserFlags_ConfirmOnEnter)
		&& IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && IsKeyPressed(ImGuiKey_Enter);

	if (!(flags_ & ImGuiFileBrowserFlags_SelectDirectory)) {
		BeginDisabled(selectedFilenames_.empty());
		bool const ok = Button("ok");
		EndDisabled();
		if ((ok || isEnterPressed) && !selectedFilenames_.empty()) {
			isOk_ = true;
			if (!keepOpen_) {
				CloseContainer();
			}
		}
	} else if (Button(" ok ") || isEnterPressed) {
		isOk_ = true;
		if (!keepOpen_) {
			CloseContainer();
		}
	}

	SameLine();

	// Esc closes the browser — but NOT while a popup/modal is open (e.g. the permanent-
	// delete confirmation), so Esc there only dismisses the popup.
	bool const escClose = (flags_ & ImGuiFileBrowserFlags_CloseOnEsc)
		&& IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && IsKeyPressed(ImGuiKey_Escape)
		&& !IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId);
	if (Button("cancel") || shouldClose_ || escClose) {
		CloseContainer();
	}

	if (!statusStr_.empty() && !(flags_ & ImGuiFileBrowserFlags_NoStatusBar)) {
		SameLine();
		Text("%s", statusStr_.c_str());
		if (IsItemHovered()) {
			BeginTooltip();
			PushTextWrapPos(300.0f);
			Text("%s", statusStr_.c_str());
			PopTextWrapPos();
			EndTooltip();
		}
	}

	DrawTypeFilterCombo();
}

void ImGui::FileBrowser::DrawTypeFilterCombo() {
	if (typeFilters_.empty()) {
		return;
	}

	// Right-align the type filter combo.
	float const comboW    = 8.0f * GetFontSize();
	float const rightEdge = GetWindowWidth() - GetStyle().WindowPadding.x;
	float const comboX    = rightEdge - comboW;
	if (comboX > GetCursorPosX() + GetStyle().ItemSpacing.x) {
		SameLine(comboX);
	} else {
		SameLine();
	}

	PushItemWidth(comboW);
	if (BeginCombo("##type_filters", typeFilters_[typeFilterIndex_].c_str())) {
		ScopeGuard guard([] { EndCombo(); });

		for (size_t i = 0; i < typeFilters_.size(); ++i) {
			bool selected = i == typeFilterIndex_;
			if (Selectable(typeFilters_[i].c_str(), selected) && !selected) {
				typeFilterIndex_ = static_cast<unsigned int>(i);
			}
		}
	}
	PopItemWidth();
}

// ─────────────────────────────────────────────────────────────────────────────
// Selection / directory / filter state
// ─────────────────────────────────────────────────────────────────────────────
bool ImGui::FileBrowser::HasSelected() const noexcept { return isOk_; }

bool ImGui::FileBrowser::SetDirectory(std::filesystem::path const& dir) {
	std::filesystem::path const preferredFallback = this->GetDirectory();
	return SetCurrentDirectoryInternal(dir, preferredFallback);
}

std::filesystem::path const& ImGui::FileBrowser::GetDirectory() const noexcept {
	return currentDirectory_;
}

std::filesystem::path ImGui::FileBrowser::GetSelected() const {
	// When isOk_ is true, selectedFilenames_ may be empty if SelectDirectory is
	// enabled. Return pwd in that case.
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
	if (flags_ & ImGuiFileBrowserFlags_EnterNewFilename) {
		AssignToArrayStyleString(inputNameBuffer_, "");
	}
	isOk_ = false;
}

void ImGui::FileBrowser::SetTypeFilters(std::vector<std::string> const& _typeFilters) {
	typeFilters_.clear();

#ifdef _WIN32
	// Remove duplicate filter names due to case-insensitivity on Windows.
	std::vector<std::string> typeFilters;
	for (auto& rawFilter : _typeFilters) {
		std::string lowerFilter = ToLower(rawFilter);
		if (std::find(typeFilters.begin(), typeFilters.end(), lowerFilter) == typeFilters.end()) {
			typeFilters.push_back(std::move(lowerFilter));
		}
	}
#else
	auto& typeFilters = _typeFilters;
#endif

	// Insert the auto-generated combined filter.
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

void ImGui::FileBrowser::SetCurrentTypeFilterIndex(int index) {
	typeFilterIndex_ = static_cast<unsigned int>(index);
}

void ImGui::FileBrowser::SetSortModeIndex(int index) {
	index      = std::clamp(index, 0, static_cast<int>(SortField::Created));
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
		if (std::find(recentDirectories_.begin(), recentDirectories_.end(), absDir)
			== recentDirectories_.end()) {
			recentDirectories_.push_back(absDir);
		}
	}

	TouchRecentDirectory(currentDirectory_);
}

std::vector<std::filesystem::path> ImGui::FileBrowser::GetRecentDirectories() const {
	return recentDirectories_;
}

void ImGui::FileBrowser::SetInputName(std::string_view input) {
	assert((flags_ & ImGuiFileBrowserFlags_EnterNewFilename)
		&& "SetInputName can only be called when ImGuiFileBrowserFlags_EnterNewFilename is "
		   "enabled");
	customizedInputName_ = input;
}

void ImGui::FileBrowser::SetHoverFileCallback(std::function<void(std::filesystem::path const&)> cb) {
	hoverFileCallback_ = std::move(cb);
}

void ImGui::FileBrowser::SetContextMenuCallback(std::function<void(std::filesystem::path const&)> cb) {
	contextMenuCallback_ = std::move(cb);
}

void ImGui::FileBrowser::SetRebuildThumbnailCallback(
	std::function<void(std::filesystem::path const&)> cb) {
	rebuildThumbnailCallback_ = std::move(cb);
}

void ImGui::FileBrowser::SetOpenFileCallback(std::function<void(std::filesystem::path const&)> cb) {
	openFileCallback_ = std::move(cb);
}

void ImGui::FileBrowser::SetScrollStep(int px) noexcept { scrollStepPx_ = std::clamp(px, 1, 4000); }
int  ImGui::FileBrowser::GetScrollStep() const noexcept { return scrollStepPx_; }

void ImGui::FileBrowser::SetPreviewEnabled(bool enabled) noexcept { previewEnabled_ = enabled; }
bool ImGui::FileBrowser::IsPreviewEnabled() const noexcept { return previewEnabled_; }

// ─────────────────────────────────────────────────────────────────────────────
// Thumbnail engine wiring
// ─────────────────────────────────────────────────────────────────────────────
void ImGui::FileBrowser::Setup(vulkan_context* vk, std::filesystem::path thumb_dir,
	std::string thumbnail_format, std::string image_tier, std::string video_tier) {
	m_thumbnails.setup(vk, std::move(thumb_dir), std::move(thumbnail_format), std::move(image_tier),
		std::move(video_tier));
}

void ImGui::FileBrowser::ClearThumbnailCache() { m_thumbnails.clear(); }

void ImGui::FileBrowser::RebuildThumbnail(std::filesystem::path const& path) {
	m_thumbnails.evict(path);
}

void ImGui::FileBrowser::ShutdownThumbnails() { m_thumbnails.shutdown(); }

void   ImGui::FileBrowser::SetThumbnailSize(ImVec2 size) noexcept { thumbnailSize_ = size; }
ImVec2 ImGui::FileBrowser::GetThumbnailSize() const noexcept { return thumbnailSize_; }

void ImGui::FileBrowser::SetViewMode(ViewMode mode) noexcept { viewMode_ = mode; }
ImGui::FileBrowser::ViewMode ImGui::FileBrowser::GetViewMode() const noexcept { return viewMode_; }

void   ImGui::FileBrowser::SetGridThumbnailSize(ImVec2 size) noexcept { gridThumbnailSize_ = size; }
ImVec2 ImGui::FileBrowser::GetGridThumbnailSize() const noexcept { return gridThumbnailSize_; }

void ImGui::FileBrowser::SetMasonryThumbnailSize(ImVec2 size) noexcept {
	masonryThumbnailSize_ = size;
}
ImVec2 ImGui::FileBrowser::GetMasonryThumbnailSize() const noexcept {
	return masonryThumbnailSize_;
}

void ImGui::FileBrowser::SetMasonryColumns(int columns) noexcept {
	masonryColumns_ = std::max(0, columns);
}
int ImGui::FileBrowser::GetMasonryColumns() const noexcept { return masonryColumns_; }

void ImGui::FileBrowser::SetShowThumbnails(bool show) noexcept { showThumbnails_ = show; }
bool ImGui::FileBrowser::GetShowThumbnails() const noexcept { return showThumbnails_; }

void ImGui::FileBrowser::SetKeepOpen(bool keepOpen) noexcept { keepOpen_ = keepOpen; }
bool ImGui::FileBrowser::GetKeepOpen() const noexcept { return keepOpen_; }

void ImGui::FileBrowser::SetMediaFilter(MediaFilter filter) noexcept { mediaFilter_ = filter; }
ImGui::FileBrowser::MediaFilter ImGui::FileBrowser::GetMediaFilter() const noexcept {
	return mediaFilter_;
}

void ImGui::FileBrowser::SetGifPlayback(GifPlayback mode) noexcept {
	// FileBrowser::GifPlayback mirrors FileBrowserThumbnailContext::GifPlayback (same order).
	m_thumbnails.set_gif_playback(static_cast<FileBrowserThumbnailContext::GifPlayback>(mode));
}
ImGui::FileBrowser::GifPlayback ImGui::FileBrowser::GetGifPlayback() const noexcept {
	return static_cast<GifPlayback>(m_thumbnails.gif_playback());
}

// ─────────────────────────────────────────────────────────────────────────────
// Internal helpers
// ─────────────────────────────────────────────────────────────────────────────
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
	if (!IsItemHovered()) {
		return;
	}
	SetTooltip("%s", s.data());
}

void ImGui::FileBrowser::RequestReload() {
	// Hand the heavy directory enumeration to the scanner thread. The previous
	// listing stays on screen until PollScan() applies the finished result.
	RebuildView();
	ClearSelected();
	m_thumbnails.release_textures();
	RebuildThumbnail(currentDirectory_);
	statusStr_ = "Scanning...";
	m_scanner.request(currentDirectory_, (flags_ & ImGuiFileBrowserFlags_SkipItemsCausingError) != 0);
}

void ImGui::FileBrowser::PollScan() {
	FileBrowserScanner::Result res;
	if (!m_scanner.poll(res)) {
		return;
	}

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
				rec.thumbKey = FileBrowserThumbnailContext::make_key(currentDirectory_ / rec.name);
				rec.isVideoThumb = cls.is_video;
				rec.isGifThumb   = cls.is_gif;
			}
		}
		fileRecords_.push_back(std::move(rec));
	}

	RebuildView();

	// Drop selections that no longer exist in the freshly loaded directory.
	if (!selectedFilenames_.empty()) {
		std::set<std::filesystem::path> kept;
		for (auto const& name : selectedFilenames_) {
			if (std::any_of(fileRecords_.begin(), fileRecords_.end(), [&](FileRecord const& rec) {
					return rec.name == name;
				})) {
				kept.insert(name);
			}
		}
		selectedFilenames_ = std::move(kept);
	}

	statusStr_.clear();
}

void ImGui::FileBrowser::RebuildView() {
	// Sort entries (index 0 is always ".." and is kept in place). Pure CPU work on the
	// already-loaded records — no directory I/O.
	if (fileRecords_.size() > 2) {
		std::sort(fileRecords_.begin() + 1, fileRecords_.end(),
			[this](FileRecord const& a, FileRecord const& b) -> bool {
				// Directories always before files, regardless of field/direction.
				if (a.isDir != b.isDir) {
					return a.isDir > b.isDir;
				}

				int const cmp = CompareByField(a, b);
				if (cmp != 0) {
					return sortAscending_ ? (cmp < 0) : (cmp > 0);
				}

				// Tie on the active field: stable secondary order by name (A-Z), kept
				// ascending so equal-keyed rows read naturally in both directions.
				return ToLower(u8StrToStr(a.name.u8string()))
					< ToLower(u8StrToStr(b.name.u8string()));
			});
	}

	// Rebuild the set of tags present in this directory (for the filter combo).
	std::set<std::string> tagSet;
	for (auto const& rec : fileRecords_) {
		if (!rec.isDir) {
			for (auto const& t : rec.tags) {
				tagSet.insert(t);
			}
		}
	}
	availableTags_.assign(tagSet.begin(), tagSet.end());
	// Clear the active filter if the tag is no longer present.
	if (!tagFilter_.empty()
		&& std::find(availableTags_.begin(), availableTags_.end(), tagFilter_)
			== availableTags_.end()) {
		tagFilter_.clear();
	}

	ClearRangeSelectionState();
}

int ImGui::FileBrowser::CompareByField(FileRecord const& a, FileRecord const& b) const {
	// Ascending three-way compare for the active field: <0 if a precedes b, >0 if it
	// follows, 0 if equal on this field (the caller breaks the tie by name). Text fields
	// fold case, matching the rest of the class.
	auto const threeWay = [](auto const& x, auto const& y) -> int {
		if (x < y) {
			return -1;
		}
		if (y < x) {
			return 1;
		}
		return 0;
	};

	switch (sortField_) {
	case SortField::Type:
		return threeWay(ToLower(u8StrToStr(a.extension.u8string())),
			ToLower(u8StrToStr(b.extension.u8string())));
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
		// Openability probe: opening the iterator throws on a bad/denied directory, so
		// SetCurrentDirectoryInternal's catch falls back before we commit. (With
		// SkipItemsCausingError the scanner reports the failure asynchronously instead.)
		std::filesystem::directory_iterator probe(target);
		(void)probe;
	}

	// Free the previous folder's thumbnail textures so VRAM doesn't accumulate as the
	// user navigates (full-res image thumbnails are large). Skip on a same-folder refresh.
	bool const dirChanged = (currentDirectory_ != target);
	currentDirectory_     = target;
	if (dirChanged) {
		m_thumbnails.release_textures();
		searchStr_ = "";
	}
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
	std::filesystem::path const& preferredFallback) {
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

	// No type filters.
	if (typeFilters_.empty()) {
		return true;
	}

	// Invalid type filter index.
	if (static_cast<size_t>(typeFilterIndex_) >= typeFilters_.size()) {
		return true;
	}

	// "All" type filters (the auto-generated combined entry at index 0).
	if (hasAllFilter_ && typeFilterIndex_ == 0) {
		for (size_t i = 1; i < typeFilters_.size(); ++i) {
			if (extension == typeFilters_[i]) {
				return true;
			}
		}
		return false;
	}

	// Universal filter.
	if (typeFilters_[typeFilterIndex_] == std::string_view(".*")) {
		return true;
	}

	// Regular filter.
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
	// u8path is deprecated in C++20.
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
			// Ignore invalid paths or inaccessible drives, e.g. empty CD drives or
			// network shares.
		}
	}
	return ret;
}
#endif
