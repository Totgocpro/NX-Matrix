// SD card browser: directories first, then files filtered by extension.
// A enters a folder or hands the absolute path to the owner callback.
#include "NxMatrix/Ui/Layouts/FilePickerLayout.hpp"

#include "NxMatrix/Ui/Dialogs.hpp"
#include "NxMatrix/Ui/Theme.hpp"
#include "NxMatrix/Util/String.hpp"

#include <algorithm>
#include <dirent.h>
#include <sys/stat.h>

namespace NxMatrix::Ui {

namespace {

constexpr const char *RootDir = "sdmc:/";

std::string JoinPath(const std::string &Dir, const std::string &Name) {
    if(Dir.empty()) {
        return Name;
    }
    if(Dir.back() == '/') {
        return Dir + Name;
    }
    return Dir + "/" + Name;
}

bool MatchesExtension(const std::string &File, const std::vector<std::string> &Extensions) {
    if(Extensions.empty()) {
        return true;
    }
    const auto Dot = File.find_last_of('.');
    if(Dot == std::string::npos) {
        return false;
    }
    const auto Ext = Util::ToLower(File.substr(Dot + 1));
    for(const auto &Wanted : Extensions) {
        auto Pattern = Util::ToLower(Wanted);
        if(!Pattern.empty() && Pattern.front() == '.') {
            Pattern.erase(0, 1);
        }
        if(Pattern == Ext) {
            return true;
        }
    }
    return false;
}

} // namespace

FilePickerLayout::FilePickerLayout(std::shared_ptr<AppContext> Context, const std::vector<std::string> &Extensions, PickedCallback OnPicked)
    : m_Context(std::move(Context)), m_Extensions(Extensions), m_OnPicked(std::move(OnPicked)), m_CurrentDir(RootDir) {
    this->SetBackgroundColor(Theme::Background);

    m_Header = pu::ui::elm::TextBlock::New(Theme::ScreenMargin, 24, "Select file");
    m_Header->SetFont(Theme::FontHeading);
    m_Header->SetColor(Theme::Accent);
    this->Add(m_Header);

    m_Path = pu::ui::elm::TextBlock::New(Theme::ScreenMargin, 84, m_CurrentDir);
    m_Path->SetFont(Theme::FontTiny);
    m_Path->SetColor(Theme::TextFaint);
    this->Add(m_Path);

    m_List = std::make_shared<ListWidget>(Theme::ScreenMargin, Theme::ListTop, Theme::ScreenWidth - 2 * Theme::ScreenMargin,
                                          Theme::ScreenHeight - Theme::ListTop - Theme::ListBottom);
    this->Add(m_List);
    GetList()->SetOnActivate([this](std::size_t Index) { OnEntryActivated(Index); });

    m_Hint = pu::ui::elm::TextBlock::New(Theme::ScreenMargin, Theme::ScreenHeight - 56, "A Select  B Back");
    m_Hint->SetFont(Theme::FontTiny);
    m_Hint->SetColor(Theme::TextFaint);
    this->Add(m_Hint);

    RebuildRows();
}

ListWidget *FilePickerLayout::GetList() const {
    return static_cast<ListWidget *>(m_List.get());
}

void FilePickerLayout::RebuildRows() {
    const auto Width = Theme::ScreenWidth - 2 * Theme::ScreenMargin;
    m_Path->SetText(m_CurrentDir);

    std::vector<Row::Ref> Rows;
    m_Entries.clear();

    DIR *Dir = ::opendir(m_CurrentDir.c_str());
    if(Dir == nullptr) {
        m_ReadFailed = true;
        Rows.push_back(std::make_shared<SimpleRow>(Width, "Cannot read this folder"));
        GetList()->SetRows(std::move(Rows));
        GetList()->SetFocusIndex(0);
        GetList()->ScrollToTop();
        ShowToast(m_Context->Nav.GetApp(), "Cannot read " + m_CurrentDir);
        return;
    }
    m_ReadFailed = false;

    std::vector<std::pair<bool, std::string>> Dirs;
    std::vector<std::pair<bool, std::string>> Files;
    while(struct dirent *Entry = ::readdir(Dir)) {
        const std::string Name = Entry->d_name;
        if(Name == "." || Name == "..") {
            continue;
        }
        bool IsDir = Entry->d_type == DT_DIR;
        if(Entry->d_type == DT_UNKNOWN) {
            // FAT does not always fill d_type; fall back to a stat probe.
            struct stat Info{};
            IsDir = ::stat(JoinPath(m_CurrentDir, Name).c_str(), &Info) == 0 && S_ISDIR(Info.st_mode);
        }
        if(IsDir) {
            Dirs.push_back({true, Name});
        } else if(MatchesExtension(Name, m_Extensions)) {
            Files.push_back({false, Name});
        }
    }
    ::closedir(Dir);

    const auto ByName = [](const std::pair<bool, std::string> &Left, const std::pair<bool, std::string> &Right) {
        return Left.second < Right.second;
    };
    std::sort(Dirs.begin(), Dirs.end(), ByName);
    std::sort(Files.begin(), Files.end(), ByName);

    if(m_CurrentDir != RootDir) {
        m_Entries.push_back({true, ".."});
    }
    m_Entries.insert(m_Entries.end(), Dirs.begin(), Dirs.end());
    m_Entries.insert(m_Entries.end(), Files.begin(), Files.end());

    for(const auto &Entry : m_Entries) {
        Rows.push_back(std::make_shared<SimpleRow>(Width, Entry.first ? ("[DIR] " + Entry.second) : Entry.second));
    }
    if(Rows.empty()) {
        Rows.push_back(std::make_shared<SimpleRow>(Width, "(empty)"));
    }

    GetList()->SetRows(std::move(Rows));
    GetList()->SetFocusIndex(0);
    GetList()->ScrollToTop();
}

void FilePickerLayout::OnEntryActivated(std::size_t Index) {
    if(m_ReadFailed) {
        RebuildRows();
        return;
    }
    if(Index >= m_Entries.size()) {
        return;
    }
    const auto &Entry = m_Entries[Index];
    if(Entry.first) {
        if(Entry.second == "..") {
            const auto Slash = m_CurrentDir.find_last_of('/');
            if(Slash != std::string::npos) {
                std::string Parent = m_CurrentDir.substr(0, Slash);
                if(Parent.empty() || Parent.back() == ':') {
                    Parent += "/";
                }
                m_CurrentDir = Parent;
            }
        } else {
            m_CurrentDir = JoinPath(m_CurrentDir, Entry.second);
        }
        RebuildRows();
        return;
    }
    if(m_OnPicked) {
        m_OnPicked(JoinPath(m_CurrentDir, Entry.second));
    }
}

} // namespace NxMatrix::Ui