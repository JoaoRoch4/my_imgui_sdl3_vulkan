#pragma once

#include "imgui.h"

#include "pch.hpp"


// Forward declaration so ConsoleCommandFn can reference the class.
class ImGuiConsole;
class EmojiAtlas;

// ── ConsoleCommandArgs ────────────────────────────────────────────────────────
// Parsed view of a single command line, passed to every command handler.
//
//   name     : upper-cased command token, e.g. "BASH"
//   args     : whitespace-split tokens after the command, e.g. {"-la", "/tmp"}
//   raw_args : everything after the command token with leading whitespace
//              stripped, preserving the original quoting/spacing for shells
//              e.g. "ls -la \"/some dir\""
struct ConsoleCommandArgs
{
    std::string_view         name;     // upper-cased command name
    std::vector<std::string> args;     // tokenised arguments
    std::string_view         raw_args; // raw tail of input after command name
};

// Signature every command handler must satisfy.
using ConsoleCommandFn = std::function<void(ImGuiConsole&, const ConsoleCommandArgs&)>;

// Record stored in the command registry.
struct ConsoleCommandDef
{
    std::string      name;        // upper-cased
    std::string      description;
    ConsoleCommandFn fn;
};

// Opaque PTY session state — defined in imgui_console.cpp.
// Kept as a shared_ptr so detached worker threads and the main thread can both
// reference it safely; the main thread uses it to write the password.
struct BashSession;

// ── ImGuiConsole ──────────────────────────────────────────────────────────────
// Core interactive console window with:
//   • a scrollable, filterable log
//   • a command input line with Tab-completion and arrow-key history
//   • a command registry — call RegisterCommand() to add your own commands
//
// Derive from this class (as ConsoleCommands does) or use it directly.
class ImGuiConsole
{
public:
    ImGuiConsole();
    ~ImGuiConsole();

    // Erase all log items.
    void ClearLog();

    // Append a line to the log (call from main thread only).
    void AddLog(std::string line);

    // Append a formatted line using C++23 std::format syntax (main thread only).
    template <typename... Args>
    void AddLog(std::format_string<Args...> fmt, Args&&... args)
    {
        AddLog(std::format(fmt, std::forward<Args>(args)...));
    }

    // Thread-safe variant: safe to call from worker threads.
    // Lines are buffered and flushed to the log on the next Draw() call.
    void AddLogThreadSafe(std::string line);

    // Render the console window once per frame, inside an ImGui frame scope.
    void Draw(const char* title, bool* p_open = nullptr);

    // Render the console body only (toolbar + log + input), without wrapping
    // Begin()/End().  Call this inside a BeginTabItem()/EndTabItem() block.
    // 'id' is passed to ImGui::PushID() to keep widget IDs unique per instance.
    void DrawContents(const char* id);

    // Register a named command.  'name' is stored and matched case-insensitively.
    void RegisterCommand(const char* name, const char* description,
                         ConsoleCommandFn fn);

    // Parse and dispatch a command-line string (called internally on Enter).
    void ExecCommand(const char* command_line);

    void SetEmojiAtlas(const EmojiAtlas* atlas) { EmojiAtlasView_ = atlas; }

protected:
    std::array<char, 512>          InputBuf;
    std::vector<std::string>       Items;
    std::vector<ConsoleCommandDef> Commands;
    std::vector<std::string>       History;
    int                            HistoryPos;
    ImGuiTextFilter                Filter;
    bool                           AutoScroll;
    bool                           ScrollToBottom;
    std::string                    SelectedItem_; // selected log line (empty = none)

    static int  TextEditCallbackStub(ImGuiInputTextCallbackData* data);
    int         TextEditCallback(ImGuiInputTextCallbackData* data);

    static int  Stricmp(const char* s1, const char* s2);
    static int  Strnicmp(const char* s1, const char* s2, int n);
    static void Strtrim(char* s);

    // Thread-safe log queue: background threads push here; Draw() flushes.
    std::mutex               PendingMutex_;
    std::vector<std::string> PendingLines_;
    void FlushPendingLogs();

    // Shared alive flag: background threads check this before touching *this.
    std::shared_ptr<std::atomic<bool>> Alive_;

    // Count of bash jobs currently in flight.
    std::atomic<int> BashJobCount_;

    // Active PTY session — non-null while a BASH command is running.
    // Protected by BashSessionMutex_ for pointer swap; the BashSession fields
    // themselves are protected per-field (atomics + fd_mutex inside BashSession).
    std::mutex                   BashSessionMutex_;
    std::shared_ptr<BashSession> ActiveBashSession_;

    const EmojiAtlas* EmojiAtlasView_ { nullptr };
};

// ── ConsoleCommands ───────────────────────────────────────────────────────────
// Concrete sub-class of ImGuiConsole with a useful set of built-in commands.
//
//   HELP                     List all registered commands
//   HISTORY                  Print the last 10 command history entries
//   CLEAR                    Erase the log
//   ECHO <text…>             Echo text back to the log
//   FPS                      Print current framerate
//   STYLE DARK|LIGHT|CLASSIC Switch Dear ImGui colour theme
//   DEMO ON|OFF              Show / hide the Dear ImGui demo window
//   LOG [n]                  Tail last n lines of /tmp/imgui_debug.log (default 20)
//   SET <name> <value…>      Store a simple string variable
//   GET [name]               Retrieve a variable (or list all)
//   BASH <cmd> [args…]       Run a shell command; output streamed to the log
//   COPILOT <question…>      Ask GitHub Copilot CLI: `gh copilot suggest "<question>"`
//   TERMINAL                 Open an interactive shell ($SHELL) in the console
//   KONSOLE                  Alias for TERMINAL
//   QUIT                     Signal the application to exit
//
// Wire the three callbacks below to make DEMO / STYLE / QUIT work across
// the application.
class ConsoleCommands : public ImGuiConsole
{
public:
    // Set these from the owning layer before first use.
    std::function<void()>     OnQuit;        // fired by QUIT
    std::function<void(bool)> OnDemoToggle;  // fired by DEMO ON|OFF (true = show)
    std::function<void(int)>  OnStyleChange; // fired by STYLE (0=dark,1=light,2=classic)

    ConsoleCommands();

private:
    std::unordered_map<std::string, std::string> Variables;

    // One private method per command — registered in the ctor via lambdas.
    void CmdHelp    (const ConsoleCommandArgs& a);
    void CmdHistory (const ConsoleCommandArgs& a);
    void CmdClear   (const ConsoleCommandArgs& a);
    void CmdEcho    (const ConsoleCommandArgs& a);
    void CmdFps     (const ConsoleCommandArgs& a);
    void CmdStyle   (const ConsoleCommandArgs& a);
    void CmdDemo    (const ConsoleCommandArgs& a);
    void CmdLog     (const ConsoleCommandArgs& a);
    void CmdSet     (const ConsoleCommandArgs& a);
    void CmdGet     (const ConsoleCommandArgs& a);
    void CmdQuit    (const ConsoleCommandArgs& a);
    void CmdBash    (const ConsoleCommandArgs& a);
    void CmdCopilot (const ConsoleCommandArgs& a);
    void CmdTerminal(const ConsoleCommandArgs& a);
};
