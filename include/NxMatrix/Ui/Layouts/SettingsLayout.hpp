#pragma once
#include "NxMatrix/Ui/AppContext.hpp"
#include "NxMatrix/Ui/Elements/ListWidget.hpp"

#include <pu/Plutonium>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace NxMatrix::Ui {

// Account and app settings: display name, avatar (from SD), presence, theme,
// device identity, PIN change, logout (keep data) and full wipe.
class SettingsLayout : public pu::ui::Layout {
public:
    explicit SettingsLayout(std::shared_ptr<AppContext> Context);
    PU_SMART_CTOR(SettingsLayout)
    ~SettingsLayout();

    void Refresh();

private:
    // Identifies what a row does (rows and headers share one index space).
    enum class RowKind : std::uint8_t {
        ProfileHeader,
        DisplayName,
        Avatar,
        PreferencesHeader,
        Presence,
        DarkMode,
        SecurityHeader,
        Device,
        ChangePin,
        EncryptionHeader,
        CrossSigning,
        DevicesHeader,
        DeviceItem,
        Backup,
        AccountHeader,
        Logout,
        Erase,
    };

    void RebuildRows();
    void OnRowActivated(std::size_t Index);
    void OnSetAvatar();
    void OnLogout(bool KeepData);
    // Encryption section: cross-signing setup and the secure backup flows.
    void OnCrossSigning();
    void OnBackup();
    // Schedules the JobQueue fetches behind the Encryption rows (guarded so
    // concurrent refreshes never stack; completions rebuild the rows).
    void EnsureEncryptionData();
    // Cheap fingerprint of everything the rows display (invalidation guard).
    std::string ComputeSignature() const;
    ListWidget *GetList() const;

    std::shared_ptr<AppContext> m_Context;
    ListWidget::Ref m_List;
    std::vector<RowKind> m_RowTags;
    std::vector<std::string> m_RowDeviceIds; // parallel to m_RowTags, set for DeviceItem rows
    std::vector<Matrix::DeviceInfo> m_Devices;
    E2ee::KeyBackup::Status m_BackupStatus;
    bool m_DevicesLoaded = false;
    bool m_DevicesFetching = false;
    bool m_BackupFetching = false;
    bool m_Busy = false; // one encryption action at a time
    std::uint64_t m_LastVersion = 0;
    std::string m_LastSignature;
    // Flipped by the destructor so queued Dispatcher callbacks never touch a
    // popped layout.
    std::shared_ptr<bool> m_Alive = std::make_shared<bool>(true);
};

} // namespace NxMatrix::Ui