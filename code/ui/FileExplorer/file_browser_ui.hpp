#pragma once

#include "pch.hpp"

#include "file_browser_thread.hpp"
#include "file_browser_thumbnail_context.hpp"

class vulkan_context;

#ifndef IMGUI_VERSION
#error "include imgui.h before this header"
#endif

using ImGuiFileBrowserFlags = std::uint32_t;

enum ImGuiFileBrowserFlags_ : std::uint32_t {
    ImGuiFileBrowserFlags_SelectDirectory   = 1 << 0, // select directory instead of regular file
    ImGuiFileBrowserFlags_EnterNewFilename  = 1 << 1, // allow user to enter new filename when selecting regular file
    ImGuiFileBrowserFlags_NoModal	    = 1 << 2, // file browsing window is modal by default. specify this to use a
					    // popup window
    ImGuiFileBrowserFlags_NoTitleBar	    = 1 << 3, // hide window title bar
    ImGuiFileBrowserFlags_NoStatusBar	    = 1 << 4, // hide status bar at the bottom of browsing window
    ImGuiFileBrowserFlags_CloseOnEsc	    = 1 << 5, // close file browser when pressing 'ESC'
    ImGuiFileBrowserFlags_CreateNewDir	    = 1 << 6, // allow user to create new directory
    ImGuiFileBrowserFlags_MultipleSelection = 1 << 7, // allow user to select multiple files. this will hide
						      // ImGuiFileBrowserFlags_EnterNewFilename
    ImGuiFileBrowserFlags_HideRegularFiles  = 1 << 8, // hide regular files when ImGuiFileBrowserFlags_SelectDirectory
						     // is enabled
    ImGuiFileBrowserFlags_ConfirmOnEnter    = 1 << 9, // confirm selection when pressing 'ENTER'
    ImGuiFileBrowserFlags_SkipItemsCausingError = 1 << 10, // when entering a new directory, any error will interrupt
							   // the process, causing the file browser to fall back to the
							   // working directory. with this flag, if an error is caused
							   // by a specific item in the directory, that item will be
							   // skipped, allowing the process to continue.
    ImGuiFileBrowserFlags_EditPathString	= 1 << 11, // allow user to directly edit the whole path string
    ImGuiFileBrowserFlags_Window		= 1 << 12, // render as a plain ImGui::Begin window (not a popup/modal).
					    // the window never steals focus and stays open until explicitly
					    // closed via Close() or the title-bar X button.
};

namespace ImGui {
class FileBrowser {
    public:

	explicit FileBrowser(ImGuiFileBrowserFlags flags	    = 0,
	    std::filesystem::path		   defaultDirectory = std::filesystem::current_path());

	// Non-copyable: owns a FileBrowserScanner (background std::jthread).
	FileBrowser(const FileBrowser&)		   = delete;
	FileBrowser& operator=(const FileBrowser&) = delete;

	// set the window position (in pixels)
	// default is centered
	void SetWindowPos(int posX, int posY) noexcept;

	// set the window size (in pixels)
	// default is (700, 450)
	void SetWindowSize(int width, int height) noexcept;

	// set the window title text
	void SetTitle(std::string title);

	// open the browsing window
	void Open();

	// close the browsing window
	void Close();

	// the browsing window is opened or not
	[[nodiscard]] bool IsOpened() const noexcept;

	// display the browsing window if opened
	void Display();

	// returns true when there is a selected filename
	[[nodiscard]] bool HasSelected() const noexcept;

	// set current browsing directory
	bool SetDirectory(const std::filesystem::path& dir = std::filesystem::current_path());

	// legacy interface. use SetDirectory instead.
	bool SetPwd(const std::filesystem::path& dir = std::filesystem::current_path()) { return SetDirectory(dir); }

	// get current browsing directory
	[[nodiscard]] const std::filesystem::path& GetDirectory() const noexcept;

	// legacy interface. use GetDirectory instead.
	[[nodiscard]] const std::filesystem::path& GetPwd() const noexcept { return GetDirectory(); }

	// returns selected filename. make sense only when HasSelected returns true
	// when ImGuiFileBrowserFlags_MultipleSelection is enabled, only one of
	// selected filename will be returned
	[[nodiscard]] std::filesystem::path GetSelected() const;

	// returns all selected filenames.
	// when ImGuiFileBrowserFlags_MultipleSelection is enabled, use this
	// instead of GetSelected
	[[nodiscard]] std::vector<std::filesystem::path> GetMultiSelected() const;

	// set selected filename to empty
	void ClearSelected();

	// (optional) set file type filters. eg. { ".h", ".cpp", ".hpp" }
	// ".*" matches any file types
	void SetTypeFilters(const std::vector<std::string>& typeFilters);

	// set currently applied type filter
	// default value is 0 (the first type filter)
	void SetCurrentTypeFilterIndex(int index);

	// set/get current file sort field by index:
	// 0=Name, 1=Type, 2=Size, 3=Modified
	void		  SetSortModeIndex(int index);
	[[nodiscard]] int GetSortModeIndex() const noexcept;

	// set/get the sort direction applied to the active field.
	// true = ascending (A-Z, smallest/oldest first), false = descending.
	void		   SetSortAscending(bool ascending);
	[[nodiscard]] bool GetSortAscending() const noexcept;

	// set/get recent directories shown by the explorer combo.
	void SetRecentDirectories(const std::vector<std::filesystem::path>& directories);
	[[nodiscard]] std::vector<std::filesystem::path> GetRecentDirectories() const;

	// when ImGuiFileBrowserFlags_EnterNewFilename is set
	// this function will pre-fill the input dialog with a filename.
	void SetInputName(std::string_view input);

	// set a callback invoked each frame a non-directory file item is hovered.
	// the argument is the full absolute path of the hovered file.
	void SetHoverFileCallback(std::function<void(const std::filesystem::path&)> cb);

	// set a callback invoked right after a non-directory file item is rendered.
	// use it to attach a BeginPopupContextItem() right-click menu.
	void SetContextMenuCallback(std::function<void(const std::filesystem::path&)> cb);

	// set a callback invoked when the user clicks "Rebuild Thumbnail" in the
	// built-in right-click context menu.  Has no effect if no thumbnail provider
	// is set.  Pass nullptr to remove the menu item.
	void SetRebuildThumbnailCallback(std::function<void(const std::filesystem::path&)> cb);

	// set a callback invoked to OPEN a file (e.g. in the image viewer). Fired by the
	// A/D image-navigation keys after they move the selection. The argument is the full
	// absolute path. Decoupled so the browser need not know about the viewer.
	void SetOpenFileCallback(std::function<void(const std::filesystem::path&)> cb);

	// Keyboard scroll step in pixels for the W/S list-scroll keys (Shift+W/S page-scrolls
	// a full view height instead). Persisted across runs; editable in the Runtime Config.
	void              SetScrollStep(int px) noexcept;
	[[nodiscard]] int GetScrollStep() const noexcept;

	// enable/disable the hover preview (also togglable via the in-UI checkbox).
	void		   SetPreviewEnabled(bool enabled) noexcept;
	[[nodiscard]] bool IsPreviewEnabled() const noexcept;

	// Attach the Vulkan context + on-disk thumbnail cache dir, enabling the browser-
	// owned async thumbnail engine. Call once (render thread) after construction.
	void Setup(vulkan_context* vk, std::filesystem::path thumb_dir, std::string thumbnail_format = "bc1",
		std::string image_tier = "original", std::string video_tier = "medium");
	// Flush all cached thumbnails (in-memory + on-disk PNGs); regenerated on demand.
	void ClearThumbnailCache();
	// Drop one file's cached thumbnail so it regenerates on the next get().
	void RebuildThumbnail(const std::filesystem::path& path);
	// Stop the thumbnail engine + free its GPU textures. MUST be called on the render
	// thread before Vulkan/ImGui teardown (the static FileBrowser dtor runs too late).
	void ShutdownThumbnails();

	// Override the inline-list thumbnail size (default 64×36).
	void		     SetThumbnailSize(ImVec2 size) noexcept;
	[[nodiscard]] ImVec2 GetThumbnailSize() const noexcept;

	// View mode: list (with optional inline thumbnails), uniform grid, or
	// Pinterest-style masonry (fixed column width, native aspect-ratio cells).
	enum class ViewMode { List, Grid, Masonry };
	void		       SetViewMode(ViewMode mode) noexcept;
	[[nodiscard]] ViewMode GetViewMode() const noexcept;

	// Override the grid-cell thumbnail size (default 160×90).
	void		     SetGridThumbnailSize(ImVec2 size) noexcept;
	[[nodiscard]] ImVec2 GetGridThumbnailSize() const noexcept;

	// Override the masonry column width (default 200 px). The y component is
	// unused — masonry cell heights are derived from each thumbnail's aspect.
	void		     SetMasonryThumbnailSize(ImVec2 size) noexcept;
	[[nodiscard]] ImVec2 GetMasonryThumbnailSize() const noexcept;

	// Force the masonry view to use exactly N columns. 0 = auto-pick from
	// MasonryThumbnailSize().x as a max-column-width hint (the previous behaviour).
	void               SetMasonryColumns(int columns) noexcept;
	[[nodiscard]] int  GetMasonryColumns() const noexcept;

	// Show/hide thumbnails entirely (inline list thumbnails and grid view).
	// Has no visible effect unless a thumbnail provider is set.
	// Also togglable via the in-UI "Thumbnails" checkbox.
	void		   SetShowThumbnails(bool show) noexcept;
	[[nodiscard]] bool GetShowThumbnails() const noexcept;

	// Keep the browser open after the user confirms a file.
	void		   SetKeepOpen(bool keepOpen) noexcept;
	[[nodiscard]] bool GetKeepOpen() const noexcept;

	// Media type filter (combo box in the toolbar).
	enum class MediaFilter { All, Videos, Images };
	void			  SetMediaFilter(MediaFilter filter) noexcept;
	[[nodiscard]] MediaFilter GetMediaFilter() const noexcept;

    private:

	template <class Functor> struct ScopeGuard {
		explicit ScopeGuard(Functor&& t)
		    : func(std::move(t)) { }

		~ScopeGuard() { func(); }

	    private:

		Functor func;
	};

	// The column/attribute rows are ordered by. Direction (ascending vs
	// descending) is tracked separately in sortAscending_.
	enum class SortField { Name, Type, Size, Modified, Created };

	// Defined in file_browser_thread.hpp so the scanner thread can fill it.
	using FileRecord = ::FileRecord;

	static std::string ToLower(const std::string& s);

	void TouchRecentDirectory(const std::filesystem::path& dir);

	void ToolTip(const std::string_view& s);

	// Queue an async rescan of currentDirectory_ on the scanner thread. The old
	// listing stays visible until the result is applied by PollScan().
	void RequestReload();

	// Drain a finished scan (if any) into fileRecords_ and rebuild the view.
	// Called once per frame at the top of Display().
	void PollScan();

	// Re-derive the displayed listing (prepend "..", sort, rebuild tag set) from
	// the records already loaded. Pure CPU work — no directory I/O.
	void RebuildView();

	// Three-way comparison of two records by the active sort field, expressed in
	// ASCENDING order: <0 if a precedes b, 0 if equal on this field, >0 if a
	// follows b. The caller applies the direction and the directories-first
	// grouping, so this only encodes each field's natural order.
	[[nodiscard]] int CompareByField(const FileRecord& a, const FileRecord& b) const;

	void SetCurrentDirectoryUncatched(const std::filesystem::path& pwd);

	bool
	SetCurrentDirectoryInternal(const std::filesystem::path& dir, const std::filesystem::path& preferredFallback);

	[[nodiscard]] bool IsExtensionMatched(const std::filesystem::path& extension) const;

	void ClearRangeSelectionState();

	static void AssignToArrayStyleString(std::vector<char>& arr, std::string_view content);

	static int ExpandInputBuffer(ImGuiInputTextCallbackData* callbackData);

#ifdef _WIN32
	static std::uint32_t GetDrivesBitMask();
#endif

	// for c++17 compatibility

#if defined(__cpp_lib_char8_t)
	static std::string u8StrToStr(std::u8string s);
#endif
	static std::string u8StrToStr(std::string s);

	static std::filesystem::path u8StrToPath(const char* str);

	int		      width_;
	int		      height_;
	int		      posX_;
	int		      posY_;
	ImGuiFileBrowserFlags flags_;
	std::filesystem::path defaultDirectory_;

	std::string title_;
	std::string openLabel_;

	bool shouldOpen_;
	bool shouldClose_;
	bool isOpened_;
	bool isOk_;
	bool isPosSet_;

	std::string statusStr_;

	std::vector<std::string> typeFilters_;
	unsigned int		 typeFilterIndex_;
	bool			 hasAllFilter_;

	std::filesystem::path	currentDirectory_;
	std::vector<FileRecord> fileRecords_;

	FileBrowserScanner m_scanner; // background directory scanner (jthread)

	unsigned int			rangeSelectionStart_; // enable range selection when shift is pressed
	std::set<std::filesystem::path> selectedFilenames_;

	std::string	  openNewDirLabel_;
	std::vector<char> newDirNameBuffer_;
	std::vector<char> inputNameBuffer_;
	std::string	  customizedInputName_;

	bool							 editDir_;
	bool							 setFocusToEditDir_;
	std::vector<char>					 currDirBuffer_;
	std::function<void(const std::filesystem::path&)>	 hoverFileCallback_;
	std::function<void(const std::filesystem::path&)>	 contextMenuCallback_;
	std::function<void(const std::filesystem::path&)>	 rebuildThumbnailCallback_;
	std::function<void(const std::filesystem::path&)>	 openFileCallback_; // A/D image-open
	// W/S keyboard scroll step (px); Shift+W/S page-scrolls. Editable in Runtime Config.
	int							 scrollStepPx_ = 40;
	// Smooth (ease-in-out-sine) KEYBOARD scroll. A W/S press re-aims the target one step from
	// the current position and restarts the ease; the position glides there over k_dur. Only
	// keyboard scroll is animated — the mouse wheel and scrollbar are left to ImGui untouched.
	float							 scrollAnimStart_   = 0.0f;
	float							 scrollAnimTarget_  = 0.0f;
	float							 scrollAnimElapsed_ = 0.0f;
	bool							 scrollAnimActive_  = false;
	// Set by A/D image navigation to a fileRecords_ index; the next frame's view loop
	// calls SetScrollHereY on that row to bring it into view, then resets this to -1.
	int							 scrollToIdx_ = -1;
	// Shift+Delete staging: paths awaiting permanent-delete confirmation, and a one-shot
	// flag to open the confirm modal OUTSIDE the scroll child (so the popup ID scope matches).
	std::vector<std::filesystem::path>			 pendingPermDelete_;
	bool							 wantPermDeleteModal_ = false;
	FileBrowserThumbnailContext				 m_thumbnails; // browser-owned async thumbnail engine
	ImVec2							 thumbnailSize_	    = {64.0f, 36.0f};
	ImVec2							 gridThumbnailSize_ = {160.0f, 90.0f};
	// Only .x (column width) is consumed; per-cell height comes from the
	// thumbnail's own aspect ratio (16:9 fallback when unknown).
	ImVec2							 masonryThumbnailSize_ = {200.0f, 200.0f};
	// 0 = auto column count (derived from masonryThumbnailSize_.x as a soft cap),
	// otherwise force exactly N columns. Persisted in window_state.toml.
	int 						 masonryColumns_   = 0;
	ViewMode						 viewMode_	    = ViewMode::List;
	SortField						 sortField_	    = SortField::Name;
	bool				   sortAscending_ = true; // direction applied to sortField_
	MediaFilter			   mediaFilter_	  = MediaFilter::All;
	std::string			   searchStr_;
	std::vector<std::filesystem::path> recentDirectories_;
	bool				   previewEnabled_ = true;
	bool showThumbnails_ = true; // render inline/grid thumbnails when a provider is set
	bool keepOpen_	     = false; // when true, confirming a file does not close the browser
	bool windowVisible_  = false; // tracked open state for ImGuiFileBrowserFlags_Window

	std::vector<std::string> availableTags_; ///< union of user.xdg.tags across all files in currentDirectory_
	std::string		 tagFilter_; ///< empty = show all; non-empty = show only files with this tag

#ifdef _WIN32
	std::uint32_t drives_;
#endif
};
} // namespace ImGui
