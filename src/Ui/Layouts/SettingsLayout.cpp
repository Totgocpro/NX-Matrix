// Account settings: display name, SD-card avatar, presence, dark/light theme,
// device identity, PIN change, logout (keep data) and full wipe.
#include "NxMatrix/Ui/Layouts/SettingsLayout.hpp"

#include "NxMatrix/Core/Dispatcher.hpp"
#include "NxMatrix/Core/JobQueue.hpp"
#include "NxMatrix/Ui/Dialogs.hpp"
#include "NxMatrix/Ui/Keyboard.hpp"
#include "NxMatrix/Ui/Layouts/FilePickerLayout.hpp"
#include "NxMatrix/Ui/Layouts/HomeServerLayout.hpp"
#include "NxMatrix/Ui/Layouts/PinLayout.hpp"
#include "NxMatrix/Ui/Layouts/RoomListLayout.hpp"
#include "NxMatrix/Ui/Layouts/VerificationLayout.hpp"
#include "NxMatrix/Ui/Theme.hpp"
#include "NxMatrix/Util/String.hpp"

#include <fstream>

namespace NxMatrix::Ui {

namespace {

std::string ContentTypeFromExt(const std::string &ExtLower) {
    if(ExtLower == "png") {
        return "image/png";
    }
    if(ExtLower == "jpg" || ExtLower == "jpeg") {
        return "image/jpeg";
    }
    if(ExtLower == "webp") {
        return "image/webp";
    }
    if(ExtLower == "gif") {
        return "image/gif";
    }
    return "application/octet-stream";
}

} // namespace

SettingsLayout::SettingsLayout(std::shared_ptr<AppContext> Context) : m_Context(std::move(Context)) {
    this->SetBackgroundColor(Theme::Background);

    auto Header = pu::ui::elm::TextBlock::New(Theme::ScreenMargin, 30, "Settings");
    Header->SetFont(Theme::FontHeading);
    Header->SetColor(Theme::Accent);
    this->Add(Header);

    m_List = std::make_shared<ListWidget>(Theme::ScreenMargin, Theme::ListTop, Theme::ScreenWidth - 2 * Theme::ScreenMargin,
                                          Theme::ScreenHeight - Theme::ListTop - Theme::ListBottom);
    this->Add(m_List);
    GetList()->SetOnActivate([this](std::size_t Index) { OnRowActivated(Index); });

    auto Hint = pu::ui::elm::TextBlock::New(Theme::ScreenMargin, Theme::ScreenHeight - 56, "A: select");
    Hint->SetFont(Theme::FontTiny);
    Hint->SetColor(Theme::TextFaint);
    this->Add(Hint);

    // Account/profile/device data changes (avatar upload, presence, sync
    // profile updates) only show up through a version bump + signature change.
    this->AddRenderCallback([this]() {
        const auto Version = m_Context->State.GetVersion();
        if(Version == m_LastVersion) {
            return;
        }
        m_LastVersion = Version;
        if(ComputeSignature() != m_LastSignature) {
            RebuildRows();
        }
    });

    RebuildRows();
    EnsureEncryptionData(); // fetch devices + backup status once the layout is up
}

SettingsLayout::~SettingsLayout() {
    *m_Alive = false;
}

void SettingsLayout::Refresh() {
    RebuildRows();
    EnsureEncryptionData();
}

ListWidget *SettingsLayout::GetList() const {
    return static_cast<ListWidget *>(m_List.get());
}

std::string SettingsLayout::ComputeSignature() const {
    auto &Cross = m_Context->E2ee.GetCrossSigning();
    auto &Verify = m_Context->E2ee.GetVerification();
    std::string Signature = m_Context->Account.DisplayName + '\x1F' + m_Context->Profile.AvatarFileName + '\x1F' +
                            m_Context->Account.Presence + '\x1F' + m_Context->Account.DeviceId + '\x1F' +
                            m_Context->E2ee.GetDeviceIdentity().Ed25519Key;
    Signature += Cross.IsSetUp() ? "|xs:1" : "|xs:0";
    Signature += Theme::IsDark() ? "|dark:1" : "|dark:0";
    Signature += '|' + std::to_string(m_BackupStatus.Exists) + ':' + m_BackupStatus.Version + ':' +
                 std::to_string(m_BackupStatus.UploadedSessions) + ':' + std::to_string(m_BackupStatus.LocalSessions);
    for(const auto &Device : m_Devices) {
        Signature += "|dev:" + Device.DeviceId + (Verify.IsVerified(m_Context->Account.UserId, Device.DeviceId) ? ":1" : ":0");
    }
    return Signature;
}

void SettingsLayout::RebuildRows() {
    const auto Width = Theme::ScreenWidth - 2 * Theme::ScreenMargin;
    const auto Focus = GetList()->GetFocusIndex();

    const auto Identity = m_Context->E2ee.GetDeviceIdentity();
    std::string KeyHint;
    if(!Identity.Ed25519Key.empty()) {
        KeyHint = "ed25519: " + (Identity.Ed25519Key.size() > 16 ? Identity.Ed25519Key.substr(0, 16) + "..." : Identity.Ed25519Key);
    }

    std::vector<Row::Ref> Rows;
    m_RowTags.clear();
    m_RowDeviceIds.clear();

    auto Push = [&](RowKind Kind, const Row::Ref &RowElem) {
        Rows.push_back(RowElem);
        m_RowTags.push_back(Kind);
        m_RowDeviceIds.push_back({});
    };

    Push(RowKind::ProfileHeader, std::make_shared<SectionHeaderRow>(Width, "Profile"));
    Push(RowKind::DisplayName, std::make_shared<SimpleRow>(Width, "Display name", m_Context->Account.DisplayName));
    Push(RowKind::Avatar, std::make_shared<SimpleRow>(Width, "Avatar", m_Context->Profile.AvatarFileName.empty() ? "Default" : "Custom"));

    Push(RowKind::PreferencesHeader, std::make_shared<SectionHeaderRow>(Width, "Preferences"));
    Push(RowKind::Presence, std::make_shared<SimpleRow>(Width, "Presence", m_Context->Account.Presence));
    Push(RowKind::DarkMode, std::make_shared<ToggleRow>(Width, "Dark mode", Theme::IsDark()));

    Push(RowKind::SecurityHeader, std::make_shared<SectionHeaderRow>(Width, "Security"));
    Push(RowKind::Device, std::make_shared<SimpleRow>(Width, "Device", m_Context->Account.DeviceId, KeyHint));
    Push(RowKind::ChangePin, std::make_shared<SimpleRow>(Width, "Change PIN / password"));

    Push(RowKind::EncryptionHeader, std::make_shared<SectionHeaderRow>(Width, "Encryption"));
    auto &CrossSigning = m_Context->E2ee.GetCrossSigning();
    Push(RowKind::CrossSigning,
         std::make_shared<SimpleRow>(Width, "Cross-signing",
                                     CrossSigning.IsSetUp() ? "Enabled (" + CrossSigning.GetMasterKeyFingerprint() + ")" : "Not set up"));

    Push(RowKind::DevicesHeader, std::make_shared<SectionHeaderRow>(Width, "Your devices"));
    if(m_Devices.empty()) {
        Push(RowKind::Device, std::make_shared<SimpleRow>(Width, "Device", m_DevicesLoaded ? "None" : "Loading..."));
    } else {
        auto &Verify = m_Context->E2ee.GetVerification();
        for(const auto &Device : m_Devices) {
            const std::string Hint = Device.DeviceId == m_Context->Account.DeviceId
                                         ? "This device"
                                         : (Verify.IsVerified(m_Context->Account.UserId, Device.DeviceId) ? "Verified" : "Unverified");
            Push(RowKind::DeviceItem,
                 std::make_shared<SimpleRow>(Width, "Device", Device.DisplayName.empty() ? Device.DeviceId : Device.DisplayName, Hint));
            m_RowDeviceIds.back() = Device.DeviceId;
        }
    }

    std::string BackupValue = "Not configured";
    if(m_BackupStatus.Exists) {
        BackupValue = "Backup v" + m_BackupStatus.Version + " (" + std::to_string(m_BackupStatus.UploadedSessions) + "/" +
                      std::to_string(m_BackupStatus.LocalSessions) + " sessions)";
    }
    Push(RowKind::Backup, std::make_shared<SimpleRow>(Width, "Secure backup", BackupValue));

    Push(RowKind::AccountHeader, std::make_shared<SectionHeaderRow>(Width, "Account"));
    Push(RowKind::Logout, std::make_shared<SimpleRow>(Width, "Log out (keep data)", "Leave"));
    Push(RowKind::Erase, std::make_shared<SimpleRow>(Width, "Erase everything", "Erase"));

    auto IsHeader = [](RowKind Kind) {
        return Kind == RowKind::ProfileHeader || Kind == RowKind::PreferencesHeader || Kind == RowKind::SecurityHeader ||
               Kind == RowKind::EncryptionHeader || Kind == RowKind::DevicesHeader || Kind == RowKind::AccountHeader;
    };
    GetList()->SetRows(std::move(Rows));
    if(Focus < m_RowTags.size() && !IsHeader(m_RowTags[Focus])) {
        GetList()->SetFocusIndex(Focus);
    }

    m_LastSignature = ComputeSignature();
    m_LastVersion = m_Context->State.GetVersion();
}

void SettingsLayout::OnRowActivated(std::size_t Index) {
    if(Index >= m_RowTags.size()) {
        return;
    }
    auto &App = m_Context->Nav.GetApp();
    auto Ctx = m_Context;
    auto Alive = m_Alive;

    switch(m_RowTags[Index]) {
    case RowKind::DisplayName: {
        std::string Name = m_Context->Account.DisplayName;
        if(!Keyboard::QueryText("Display name", "Your display name", Name)) {
            return;
        }
        if(Name == m_Context->Account.DisplayName) {
            return;
        }
        Core::JobQueue::Instance().Enqueue([Ctx, Alive, Name, this]() {
            const auto Status = Ctx->Api.SetDisplayName(Name);
            Core::Dispatcher::Instance().Post([Ctx, Alive, Status, Name, this]() {
                if(!*Alive) {
                    return;
                }
                if(!Status.Ok) {
                    ShowError(Ctx->Nav.GetApp(), "Display name not saved", Status.Error.ToDisplayString());
                    return;
                }
                Ctx->Account.DisplayName = Name;
                ShowToast(Ctx->Nav.GetApp(), "Display name updated");
                Refresh();
                Core::JobQueue::Instance().Enqueue([Ctx]() { Ctx->PersistAccount(); });
            });
        });
        break;
    }
    case RowKind::Avatar:
        OnSetAvatar();
        break;
    case RowKind::Presence: {
        const auto Choice = ShowOptions(App, "Presence", "Set your presence.", {"Online", "Offline", "Unavailable"});
        if(Choice.empty()) {
            return;
        }
        const auto PresenceValue = Util::ToLower(Choice);
        Core::JobQueue::Instance().Enqueue([Ctx, Alive, PresenceValue, this]() {
            const auto Status = Ctx->Api.SetPresence(PresenceValue);
            Core::Dispatcher::Instance().Post([Ctx, Alive, Status, PresenceValue, this]() {
                if(!*Alive) {
                    return;
                }
                if(!Status.Ok) {
                    ShowError(Ctx->Nav.GetApp(), "Presence not saved", Status.Error.ToDisplayString());
                    return;
                }
                Ctx->Account.Presence = PresenceValue;
                ShowToast(Ctx->Nav.GetApp(), "Presence updated");
                Refresh();
                Core::JobQueue::Instance().Enqueue([Ctx]() { Ctx->PersistAccount(); });
            });
        });
        break;
    }
    case RowKind::DarkMode: {
        const bool Dark = !Theme::IsDark();
        Theme::SetDark(Dark);
        m_Context->Store.SaveThemeDark(Dark);
        // Existing elements captured the previous palette: rebuild the room
        // list root once this input pass is over so the new colors apply to
        // the whole UI. Deferred so this layout is not destroyed mid-callback.
        Core::Dispatcher::Instance().Post([Ctx]() {
            const bool DarkNow = Theme::IsDark();
            Ctx->Nav.ReplaceRoot(RoomListLayout::New(Ctx));
            ShowToast(Ctx->Nav.GetApp(), DarkNow ? "Dark mode on" : "Dark mode off");
        });
        break;
    }
    case RowKind::Device: {
        const auto Identity = m_Context->E2ee.GetDeviceIdentity();
        ShowMessage(App, "Device",
                    "Device id: " + m_Context->Account.DeviceId +
                        "\nEd25519: " + (Identity.Ed25519Key.empty() ? std::string("unavailable") : Identity.Ed25519Key));
        break;
    }
    case RowKind::ChangePin:
        m_Context->Nav.Push(PinLayout::New(m_Context, PinLayout::Mode::Change));
        break;
    case RowKind::CrossSigning:
        OnCrossSigning();
        break;
    case RowKind::DeviceItem: {
        const std::string TargetDevice = Index < m_RowDeviceIds.size() ? m_RowDeviceIds[Index] : std::string();
        if(TargetDevice.empty()) {
            break;
        }
        if(TargetDevice == m_Context->Account.DeviceId) {
            for(const auto &Device : m_Devices) {
                if(Device.DeviceId != TargetDevice) {
                    continue;
                }
                std::string Info = "Device id: " + Device.DeviceId;
                if(!Device.DisplayName.empty()) {
                    Info = "Name: " + Device.DisplayName + '\n' + Info;
                }
                if(!Device.LastSeenIp.empty()) {
                    Info += "\nLast seen: " + Device.LastSeenIp;
                }
                ShowMessage(App, "This device", Info);
                break;
            }
            break;
        }
        m_Context->Nav.Push(VerificationLayout::New(m_Context, m_Context->Account.UserId, TargetDevice));
        break;
    }
    case RowKind::Backup:
        OnBackup();
        break;
    case RowKind::Logout:
        OnLogout(true);
        break;
    case RowKind::Erase:
        OnLogout(false);
        break;
    default:
        break;
    }
}

void SettingsLayout::OnSetAvatar() {
    auto &App = m_Context->Nav.GetApp();
    const auto Choice = ShowOptions(App, "Avatar", "Choose a new profile picture.", {"Choose image from SD", "Remove avatar"});

    if(Choice == "Choose image from SD") {
        auto Ctx = m_Context;
        auto Alive = m_Alive;
        const std::vector<std::string> Extensions{"png", "jpg", "jpeg", "webp", "gif"};
        m_Context->Nav.Push(FilePickerLayout::New(m_Context, Extensions, [Ctx, Alive, this](const std::string &Path) {
            Core::JobQueue::Instance().Enqueue([Ctx, Alive, Path, this]() {
                std::ifstream In(Path, std::ios::binary);
                if(!In) {
                    Core::Dispatcher::Instance().Post([Ctx, Alive, Path]() {
                        if(!*Alive) {
                            return;
                        }
                        ShowError(Ctx->Nav.GetApp(), "Avatar not set", "Cannot read " + Path);
                    });
                    return;
                }
                In.seekg(0, std::ios::end);
                const auto Size = static_cast<std::streamoff>(In.tellg());
                if(Size <= 0) {
                    Core::Dispatcher::Instance().Post([Ctx, Alive]() {
                        if(!*Alive) {
                            return;
                        }
                        ShowError(Ctx->Nav.GetApp(), "Avatar not set", "The file is empty or unreadable.");
                    });
                    return;
                }
                In.seekg(0, std::ios::beg);
                ByteArray Bytes(static_cast<std::size_t>(Size));
                In.read(reinterpret_cast<char *>(Bytes.data()), static_cast<std::streamsize>(Size));

                const auto Slash = Path.find_last_of('/');
                const auto FileName = (Slash == std::string::npos) ? Path : Path.substr(Slash + 1);
                const auto Dot = FileName.find_last_of('.');
                const auto Ext = Util::ToLower(Dot == std::string::npos ? std::string() : FileName.substr(Dot + 1));

                const auto Uploaded = Ctx->Api.UploadMedia(Bytes, ContentTypeFromExt(Ext), FileName);
                if(!Uploaded.Ok) {
                    Core::Dispatcher::Instance().Post([Ctx, Alive, Uploaded]() {
                        if(!*Alive) {
                            return;
                        }
                        ShowError(Ctx->Nav.GetApp(), "Avatar not set", Uploaded.Error.ToDisplayString());
                    });
                    return;
                }
                const auto SetStatus = Ctx->Api.SetAvatarMxc(Uploaded.Value);
                if(!SetStatus.Ok) {
                    Core::Dispatcher::Instance().Post([Ctx, Alive, SetStatus]() {
                        if(!*Alive) {
                            return;
                        }
                        ShowError(Ctx->Nav.GetApp(), "Avatar not set", SetStatus.Error.ToDisplayString());
                    });
                    return;
                }

                Ctx->Store.SaveAvatarImage(Bytes);
                Ctx->Profile.AvatarFileName = "Avatar.img";
                Ctx->Store.SavePublicProfile(Ctx->Profile);
                Ctx->Account.AvatarMxc = Uploaded.Value;
                Ctx->PersistAccount();

                Core::Dispatcher::Instance().Post([Ctx, Alive, this]() {
                    if(!*Alive) {
                        return;
                    }
                    ShowToast(Ctx->Nav.GetApp(), "Avatar updated");
                    Refresh();
                });
            });
        }));
        return;
    }

    if(Choice == "Remove avatar") {
        auto Ctx = m_Context;
        auto Alive = m_Alive;
        Core::JobQueue::Instance().Enqueue([Ctx, Alive, this]() {
            const auto Status = Ctx->Api.SetAvatarMxc("");
            if(Status.Ok) {
                Ctx->Store.SaveAvatarImage({});
                Ctx->Profile.AvatarFileName.clear();
                Ctx->Store.SavePublicProfile(Ctx->Profile);
                Ctx->Account.AvatarMxc.clear();
                Ctx->PersistAccount();
            }
            Core::Dispatcher::Instance().Post([Ctx, Alive, Status, this]() {
                if(!*Alive) {
                    return;
                }
                if(!Status.Ok) {
                    ShowError(Ctx->Nav.GetApp(), "Avatar not removed", Status.Error.ToDisplayString());
                    return;
                }
                ShowToast(Ctx->Nav.GetApp(), "Avatar removed");
                Refresh();
            });
        });
    }
}

void SettingsLayout::OnLogout(bool KeepData) {
    auto &App = m_Context->Nav.GetApp();
    auto Ctx = m_Context;
    auto Alive = m_Alive;

    if(KeepData) {
        if(!Confirm(App, "Log out", "Sign out on this console? Your messages and cache stay on the SD card.", "Log out")) {
            return;
        }
        Core::JobQueue::Instance().Enqueue([Ctx, Alive]() {
            Ctx->Sync.Stop();
            (void)Ctx->Api.Logout();
            Core::Dispatcher::Instance().Post([Ctx, Alive]() {
                if(!*Alive) {
                    return;
                }
                Ctx->Account.AccessToken.clear();
                Ctx->Account.RefreshToken.clear();
                Ctx->Account.SyncNextBatch.clear();
                Ctx->Api.ClearSession();
                ShowToast(Ctx->Nav.GetApp(), "Logged out");
                Core::JobQueue::Instance().Enqueue([Ctx]() { Ctx->PersistAccount(); });
                Ctx->Nav.ReplaceRoot(HomeServerLayout::New(Ctx));
            });
        });
        return;
    }

    if(!Confirm(App, "Erase everything", "Deletes your account data, cached rooms and avatar from this console. This cannot be undone.",
                "Erase")) {
        return;
    }
    Core::JobQueue::Instance().Enqueue([Ctx, Alive]() {
        Ctx->Sync.Stop();
        Ctx->Store.Wipe();
        Core::Dispatcher::Instance().Post([Ctx, Alive]() {
            if(!*Alive) {
                return;
            }
            Ctx->Api.ClearSession();
            Ctx->Vault = Store::Vault{};
            Ctx->Account = Store::AccountData{};
            Ctx->Profile = Store::PublicProfile{};
            Ctx->Nav.ReplaceRoot(HomeServerLayout::New(Ctx));
        });
    });
}

void SettingsLayout::EnsureEncryptionData() {
    auto Ctx = m_Context;
    auto Alive = m_Alive;

    if(!m_DevicesLoaded && !m_DevicesFetching) {
        m_DevicesFetching = true;
        Core::JobQueue::Instance().Enqueue([Ctx, Alive, this]() {
            const auto Devices = Ctx->Api.GetDevices();
            Core::Dispatcher::Instance().Post([Ctx, Alive, Devices, this]() {
                if(!*Alive) {
                    return;
                }
                m_DevicesFetching = false;
                m_DevicesLoaded = true;
                if(Devices.Ok) {
                    m_Devices = Devices.Value;
                }
                RebuildRows();
            });
        });
    }

    if(!m_BackupFetching) {
        m_BackupFetching = true;
        Core::JobQueue::Instance().Enqueue([Ctx, Alive, this]() {
            const auto Status = Ctx->E2ee.GetKeyBackup().GetStatus();
            Core::Dispatcher::Instance().Post([Ctx, Alive, Status, this]() {
                if(!*Alive) {
                    return;
                }
                m_BackupFetching = false;
                m_BackupStatus = Status;
                RebuildRows();
            });
        });
    }
}

void SettingsLayout::OnCrossSigning() {
    auto &App = m_Context->Nav.GetApp();
    auto Ctx = m_Context;
    auto Alive = m_Alive;
    auto &Cross = Ctx->E2ee.GetCrossSigning();

    if(Cross.IsSetUp()) {
        ShowMessage(App, "Cross-signing", "Enabled (" + Cross.GetMasterKeyFingerprint() + ")");
        return;
    }
    if(m_Busy) {
        return;
    }
    if(!Confirm(App, "Cross-signing", "Set up cross-signing keys for this account? You will be asked for your account password.",
                "Set up")) {
        return;
    }
    std::string Password;
    if(!Keyboard::QueryPassword("Account password", "Password for your Matrix account", Password, 1)) {
        return;
    }
    m_Busy = true;
    Core::JobQueue::Instance().Enqueue([Ctx, Alive, Password, this]() {
        const bool Ok = Ctx->E2ee.GetCrossSigning().SetUpKeys(Password);
        Core::Dispatcher::Instance().Post([Ctx, Alive, Ok, this]() {
            if(!*Alive) {
                return;
            }
            m_Busy = false;
            if(Ok) {
                ShowToast(Ctx->Nav.GetApp(), "Cross-signing set up");
                Core::JobQueue::Instance().Enqueue([Ctx]() { Ctx->PersistAccount(); });
            } else {
                ShowError(Ctx->Nav.GetApp(), "Cross-signing failed", "The keys could not be created and uploaded.");
            }
            Refresh();
        });
    });
}

void SettingsLayout::OnBackup() {
    auto &App = m_Context->Nav.GetApp();
    auto Ctx = m_Context;
    auto Alive = m_Alive;
    if(m_Busy) {
        return;
    }

    // Only offer what the current backup state allows.
    std::vector<std::string> Options;
    if(!m_BackupStatus.Exists) {
        Options.push_back("Set up backup");
    } else {
        if(!m_BackupStatus.KeyAvailable) {
            Options.push_back("Use recovery key");
        }
        Options.push_back("Backup now");
        Options.push_back("Restore from backup");
    }
    const std::string Choice = ShowOptions(App, "Secure backup", "Back up your encrypted message keys to the server.", Options);
    if(Choice.empty()) {
        return;
    }

    if(Choice == "Set up backup") {
        const std::string Mode =
            ShowOptions(App, "Set up backup", "Choose how to protect the backup key.", {"Recovery key (recommended)", "Passphrase"});
        if(Mode.empty()) {
            return;
        }
        std::string Passphrase;
        if(Mode == "Passphrase" &&
           !Keyboard::QueryPassword("Passphrase", "Protects the backup key instead of a recovery key", Passphrase, 1)) {
            return;
        }
        m_Busy = true;
        Core::JobQueue::Instance().Enqueue([Ctx, Alive, Passphrase, this]() {
            std::string RecoveryKey;
            const bool Ok = Ctx->E2ee.GetKeyBackup().CreateBackup(Passphrase, RecoveryKey);
            Core::Dispatcher::Instance().Post([Ctx, Alive, Ok, RecoveryKey, this]() {
                if(!*Alive) {
                    return;
                }
                m_Busy = false;
                if(!Ok) {
                    ShowError(Ctx->Nav.GetApp(), "Backup not created", "The server rejected the new backup version.");
                    Refresh();
                    return;
                }
                ShowMessage(Ctx->Nav.GetApp(), "Recovery key - write it down",
                            "This key is the only way to unlock your backup. It is shown once and cannot be recovered.\n\n" + RecoveryKey);
                Core::JobQueue::Instance().Enqueue([Ctx]() { Ctx->PersistAccount(); });
                Refresh();
            });
        });
        return;
    }

    if(Choice == "Use recovery key") {
        std::string RecoveryKey;
        if(!Keyboard::QueryText("Recovery key", "Paste the recovery key of your backup", RecoveryKey, 128)) {
            return;
        }
        m_Busy = true;
        Core::JobQueue::Instance().Enqueue([Ctx, Alive, RecoveryKey, this]() {
            const bool Ok = Ctx->E2ee.GetKeyBackup().ActivateWithRecoveryKey(RecoveryKey);
            Core::Dispatcher::Instance().Post([Ctx, Alive, Ok, this]() {
                if(!*Alive) {
                    return;
                }
                m_Busy = false;
                if(Ok) {
                    ShowToast(Ctx->Nav.GetApp(), "Backup key accepted");
                    Core::JobQueue::Instance().Enqueue([Ctx]() { Ctx->PersistAccount(); });
                } else {
                    ShowError(Ctx->Nav.GetApp(), "Wrong recovery key", "The key does not match this backup.");
                }
                Refresh();
            });
        });
        return;
    }

    if(Choice == "Backup now") {
        m_Busy = true;
        Core::JobQueue::Instance().Enqueue([Ctx, Alive, this]() {
            const bool Ok = Ctx->E2ee.GetKeyBackup().UploadAllSessions();
            const auto Status = Ok ? Ctx->E2ee.GetKeyBackup().GetStatus() : E2ee::KeyBackup::Status{};
            Core::Dispatcher::Instance().Post([Ctx, Alive, Ok, Status, this]() {
                if(!*Alive) {
                    return;
                }
                m_Busy = false;
                if(Ok) {
                    ShowToast(Ctx->Nav.GetApp(), "Backup: " + std::to_string(Status.UploadedSessions) + "/" +
                                                     std::to_string(Status.LocalSessions) + " sessions");
                    Core::JobQueue::Instance().Enqueue([Ctx]() { Ctx->PersistAccount(); });
                } else {
                    ShowError(Ctx->Nav.GetApp(), "Backup failed", "The sessions could not be uploaded.");
                }
                Refresh();
            });
        });
        return;
    }

    if(Choice == "Restore from backup") {
        if(!Confirm(App, "Restore from backup", "Download and decrypt every session stored in your backup?", "Restore")) {
            return;
        }
        m_Busy = true;
        Core::JobQueue::Instance().Enqueue([Ctx, Alive, this]() {
            const bool Ok = Ctx->E2ee.GetKeyBackup().RestoreAllSessions();
            Core::Dispatcher::Instance().Post([Ctx, Alive, Ok, this]() {
                if(!*Alive) {
                    return;
                }
                m_Busy = false;
                if(Ok) {
                    ShowToast(Ctx->Nav.GetApp(), "Backup restored");
                    Core::JobQueue::Instance().Enqueue([Ctx]() { Ctx->PersistAccount(); });
                } else {
                    ShowError(Ctx->Nav.GetApp(), "Restore failed", "The backup could not be read with the stored key.");
                }
                Refresh();
            });
        });
    }
}

} // namespace NxMatrix::Ui