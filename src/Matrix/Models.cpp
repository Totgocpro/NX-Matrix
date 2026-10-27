#include "NxMatrix/Matrix/Models.hpp"

#include "NxMatrix/Util/Json.hpp"
#include "NxMatrix/Util/String.hpp"

#include <cstdio>

namespace NxMatrix::Matrix {

namespace {

char LowerAscii(char C) {
    return (C >= 'A' && C <= 'Z') ? static_cast<char>(C - 'A' + 'a') : C;
}

bool IsHtmlSpace(char C) {
    return C == ' ' || C == '\t' || C == '\n' || C == '\r' || C == '\f' || C == '\v';
}

// "@alice:matrix.org" -> "alice"
std::string SenderLocalpart(const std::string &SenderId) {
    if(!SenderId.empty() && SenderId.front() == '@') {
        const std::size_t Colon = SenderId.find(':');
        if(Colon != std::string::npos && Colon > 1) {
            return SenderId.substr(1, Colon - 1);
        }
        return SenderId.substr(1);
    }
    return SenderId;
}

std::string FormatHumanSize(std::uint64_t Bytes) {
    static const char *Units[] = {"B", "KB", "MB", "GB"};
    double Value = static_cast<double>(Bytes);
    int Unit = 0;
    while(Value >= 1024.0 && Unit < 3) {
        Value /= 1024.0;
        Unit++;
    }
    char Buf[40];
    if(Unit == 0) {
        std::snprintf(Buf, sizeof(Buf), "%llu B", static_cast<unsigned long long>(Bytes));
    } else {
        std::snprintf(Buf, sizeof(Buf), "%.1f %s", Value, Units[Unit]);
    }
    return Buf;
}

// Rich reply fallback (pre-v1.13 senders): spec says clients SHOULD still
// remove it before rendering. Body: drop the leading "> " lines plus the
// blank separator; formatted_body: drop the leading <mx-reply> element.
std::string StripReplyFallbackBody(const std::string &Body) {
    std::size_t Pos = 0;
    bool Stripped = false;
    while(true) {
        const std::size_t Eol = Body.find('\n', Pos);
        const std::size_t End = (Eol == std::string::npos) ? Body.size() : Eol;
        std::size_t LineEnd = End;
        if(LineEnd > Pos && Body[LineEnd - 1] == '\r') {
            LineEnd--;
        }
        if(LineEnd - Pos < 2 || Body.compare(Pos, 2, "> ") != 0) {
            break;
        }
        Stripped = true;
        if(Eol == std::string::npos) {
            Pos = Body.size();
            break;
        }
        Pos = Eol + 1;
    }
    if(!Stripped) {
        return Body;
    }
    while(Pos < Body.size() && (Body[Pos] == '\n' || Body[Pos] == '\r')) {
        Pos++;
    }
    return Body.substr(Pos);
}

std::string StripReplyFallbackHtml(const std::string &Html) {
    const std::string Open = "<mx-reply>";
    const std::string Close = "</mx-reply>";
    if(!Util::StartsWith(Html, Open)) {
        return Html;
    }
    const std::size_t ClosePos = Html.find(Close);
    if(ClosePos == std::string::npos) {
        return Html;
    }
    std::size_t Pos = ClosePos + Close.size();
    while(Pos < Html.size() && (Html[Pos] == '\n' || Html[Pos] == '\r')) {
        Pos++;
    }
    return Html.substr(Pos);
}

// content["m.relates_to"] when present and an object.
const nlohmann::json *RelatesTo(const nlohmann::json &Content) {
    if(!Content.is_object()) {
        return nullptr;
    }
    const auto Rel = Content.find("m.relates_to");
    if(Rel == Content.end() || !Rel->is_object()) {
        return nullptr;
    }
    return &(*Rel);
}

std::string DecodeEntities(std::string Text) {
    Text = Util::ReplaceAll(Text, "&lt;", "<");
    Text = Util::ReplaceAll(Text, "&gt;", ">");
    Text = Util::ReplaceAll(Text, "&quot;", "\"");
    Text = Util::ReplaceAll(Text, "&#39;", "'");
    Text = Util::ReplaceAll(Text, "&#x27;", "'");
    Text = Util::ReplaceAll(Text, "&nbsp;", " ");
    Text = Util::ReplaceAll(Text, "&amp;", "&"); // last, so "&amp;lt;" decodes once
    return Text;
}

// Whitespace runs -> single spaces, leading/trailing dropped.
std::string CollapseSpaces(const std::string &Text) {
    std::string Out;
    bool PendingSpace = false;
    for(const char C : Text) {
        if(IsHtmlSpace(C)) {
            if(!Out.empty()) {
                PendingSpace = true;
            }
            continue;
        }
        if(PendingSpace) {
            Out.push_back(' ');
            PendingSpace = false;
        }
        Out.push_back(C);
    }
    return Out;
}

// Empty line, or one that only carries whitespace/blockquote markers.
bool IsBlankLine(const std::string &Line) {
    for(const char C : Line) {
        if(C != ' ' && C != '\t' && C != '\r' && C != '>') {
            return false;
        }
    }
    return true;
}

// Case-insensitive search for a closing tag; Needle must be lowercase.
std::size_t FindCloseTag(const std::string &Html, std::size_t From, const std::string &Needle) {
    if(Needle.empty() || From + Needle.size() > Html.size()) {
        return std::string::npos;
    }
    for(std::size_t P = From; P + Needle.size() <= Html.size(); ++P) {
        bool Match = true;
        for(std::size_t K = 0; K < Needle.size(); ++K) {
            if(LowerAscii(Html[P + K]) != Needle[K]) {
                Match = false;
                break;
            }
        }
        if(Match) {
            return P;
        }
    }
    return std::string::npos;
}

// href attribute value of an opening tag ("a href=\"...\"").
std::string ExtractHref(const std::string &Tag) {
    for(std::size_t P = 0; P + 4 <= Tag.size(); ++P) {
        if(LowerAscii(Tag[P]) != 'h' || LowerAscii(Tag[P + 1]) != 'r' || LowerAscii(Tag[P + 2]) != 'e' || LowerAscii(Tag[P + 3]) != 'f') {
            continue;
        }
        std::size_t I = P + 4;
        while(I < Tag.size() && IsHtmlSpace(Tag[I])) {
            ++I;
        }
        if(I >= Tag.size() || Tag[I] != '=') {
            return {};
        }
        ++I;
        while(I < Tag.size() && IsHtmlSpace(Tag[I])) {
            ++I;
        }
        if(I >= Tag.size()) {
            return {};
        }
        if(Tag[I] == '"' || Tag[I] == '\'') {
            const char Quote = Tag[I];
            ++I;
            const std::size_t End = Tag.find(Quote, I);
            if(End == std::string::npos) {
                return Tag.substr(I);
            }
            return Tag.substr(I, End - I);
        }
        std::size_t End = I;
        while(End < Tag.size() && !IsHtmlSpace(Tag[End])) {
            ++End;
        }
        return Tag.substr(I, End - I);
    }
    return {};
}

} // namespace

MembershipState MembershipFromString(const std::string &Value) {
    const std::string Lower = Util::ToLower(Value);
    if(Lower == "invite") {
        return MembershipState::Invite;
    }
    if(Lower == "join") {
        return MembershipState::Join;
    }
    if(Lower == "knock") {
        return MembershipState::Knock;
    }
    if(Lower == "leave") {
        return MembershipState::Leave;
    }
    if(Lower == "ban") {
        return MembershipState::Ban;
    }
    return MembershipState::None;
}

const char *MembershipToString(MembershipState Value) {
    switch(Value) {
    case MembershipState::Invite:
        return "invite";
    case MembershipState::Join:
        return "join";
    case MembershipState::Knock:
        return "knock";
    case MembershipState::Leave:
        return "leave";
    case MembershipState::Ban:
        return "ban";
    default:
        return "none";
    }
}

Event Event::FromJson(const nlohmann::json &Json, const std::string &RoomId) {
    Event Out;
    if(!Json.is_object()) {
        return Out;
    }
    Out.RoomId = RoomId;
    Out.EventId = Util::JsonGetString(Json, "event_id");
    Out.Sender = Util::JsonGetString(Json, "sender");
    Out.Type = Util::JsonGetString(Json, "type");
    Out.StateKey = Util::JsonGetString(Json, "state_key");
    Out.Timestamp = Util::JsonGetInt(Json, "origin_server_ts");
    if(Json.contains("content") && Json["content"].is_object()) {
        Out.Content = Json["content"];
    }
    if(Json.contains("unsigned") && Json["unsigned"].is_object()) {
        Out.Unsigned = Json["unsigned"];
        Out.IsRedacted = Out.Unsigned.contains("redacted_because");
        Out.TransactionId = Util::JsonGetString(Out.Unsigned, "transaction_id");
    }
    return Out;
}

bool CompareSpaceChildren(const SpaceChild &A, const SpaceChild &B) {
    // Spec v1.19 §Ordering of children within a space: children carrying a
    // valid `order` sort among themselves lexicographically (ASCII-only
    // content, so byte order == code-point order) and come before the rest.
    if(!A.Order.empty() || !B.Order.empty()) {
        if(A.Order.empty() || B.Order.empty()) {
            return !A.Order.empty();
        }
        if(A.Order != B.Order) {
            return A.Order < B.Order;
        }
        // Equal orders: fall through to the timestamp/room-id tie-breakers.
    }
    if(A.OriginServerTs != B.OriginServerTs) {
        return A.OriginServerTs < B.OriginServerTs;
    }
    return A.RoomId < B.RoomId;
}

std::string RoomDisplayName(const RoomSummary &Summary) {
    if(!Summary.Name.empty()) {
        return Summary.Name;
    }
    if(!Summary.CanonicalAlias.empty()) {
        return Summary.CanonicalAlias;
    }
    return Summary.RoomId;
}

bool IsMessageEvent(const Event &Ev) {
    return Ev.Type == "m.room.message" || Ev.Type == "m.sticker";
}

std::string GetMessageBody(const Event &Ev) {
    if(!Ev.Content.is_object()) {
        return {};
    }
    std::string Body = Util::JsonGetString(Ev.Content, "body");
    if(!GetReplyToEventId(Ev).empty()) {
        Body = StripReplyFallbackBody(Body);
    }
    return Body;
}

std::string GetFormattedMessageBody(const Event &Ev) {
    if(!Ev.Content.is_object()) {
        return {};
    }
    std::string Formatted = Util::JsonGetString(Ev.Content, "formatted_body");
    if(!GetReplyToEventId(Ev).empty()) {
        Formatted = StripReplyFallbackHtml(Formatted);
    }
    return Formatted;
}

std::string EventDisplayText(const Event &Ev, const std::string &SelfUserId) {
    (void)SelfUserId;
    if(Ev.IsRedacted) {
        return "Message deleted";
    }
    if(Ev.IsDecryptionFailed) {
        return "Unable to decrypt this message";
    }
    if(Ev.Type == "m.room.message" && Util::JsonGetString(Ev.Content, "msgtype") == "m.emote") {
        return "* " + SenderLocalpart(Ev.Sender) + " " + GetMessageBody(Ev);
    }
    const std::string Formatted = GetFormattedMessageBody(Ev);
    if(!Formatted.empty()) {
        return HtmlToPlainText(Formatted);
    }
    return GetMessageBody(Ev);
}

std::string AttachmentMarker(const Event &Ev) {
    if(Ev.IsRedacted || Ev.IsDecryptionFailed) {
        return {};
    }
    std::string Kind;
    if(Ev.Type == "m.sticker") {
        Kind = "Sticker";
    } else {
        const std::string MsgType = Util::JsonGetString(Ev.Content, "msgtype");
        if(MsgType == "m.image") {
            Kind = "Image";
        } else if(MsgType == "m.video") {
            Kind = "Video";
        } else if(MsgType == "m.audio") {
            Kind = "Audio";
        } else if(MsgType == "m.file") {
            Kind = "File";
        } else {
            return {};
        }
    }
    const std::string Body = GetMessageBody(Ev);
    const std::string Name = !Body.empty() ? Body : "file";
    std::uint64_t Size = 0;
    if(Ev.Content.contains("info") && Ev.Content["info"].is_object()) {
        const auto &Info = Ev.Content["info"];
        if(Info.contains("size") && (Info["size"].is_number_unsigned() || Info["size"].is_number_integer())) {
            Size = Info["size"].get<std::uint64_t>();
        }
    }
    if(Size == 0) {
        return Kind + ": " + Name;
    }
    return Kind + ": " + Name + " (" + FormatHumanSize(Size) + ")";
}

AttachmentImage GetAttachmentImage(const Event &Ev) {
    AttachmentImage Out;
    if(Ev.IsRedacted || Ev.IsDecryptionFailed) {
        return Out;
    }
    const bool Sticker = Ev.Type == "m.sticker";
    if(!Sticker && Util::JsonGetString(Ev.Content, "msgtype") != "m.image") {
        return Out;
    }
    if(Ev.Content.contains("info") && Ev.Content["info"].is_object()) {
        const auto &Info = Ev.Content["info"];
        Out.Width = static_cast<std::int32_t>(Util::JsonGetInt(Info, "w"));
        Out.Height = static_cast<std::int32_t>(Util::JsonGetInt(Info, "h"));
    }
    // Encrypted attachments: the plaintext lives in content.file (its url
    // serves the ciphertext); MediaLoader verifies and decrypts it.
    const auto File = Ev.Content.find("file");
    if(File != Ev.Content.end() && File->is_object()) {
        Out.MxcUrl = Util::JsonGetString(*File, "url");
        if(Util::StartsWith(Out.MxcUrl, "mxc://")) {
            Out.Encrypted = true;
            Out.EncryptedFile = *File;
            return Out;
        }
        Out.MxcUrl.clear();
        return Out;
    }
    Out.MxcUrl = Util::JsonGetString(Ev.Content, "url");
    if(!Util::StartsWith(Out.MxcUrl, "mxc://")) {
        Out.MxcUrl.clear();
    }
    return Out;
}

std::string EventPreviewText(const Event &Ev, const std::string &SelfUserId) {
    // Attachments preview as their kind/name/size label: the raw body is
    // just the filename and reads like ordinary text.
    const std::string Marker = AttachmentMarker(Ev);
    if(!Marker.empty()) {
        return Marker;
    }
    const std::string Text = EventDisplayText(Ev, SelfUserId);
    std::string Collapsed;
    bool PendingSpace = false;
    for(const char C : Text) {
        if(IsHtmlSpace(C)) {
            if(!Collapsed.empty()) {
                PendingSpace = true;
            }
            continue;
        }
        if(PendingSpace) {
            Collapsed.push_back(' ');
            PendingSpace = false;
        }
        Collapsed.push_back(C);
    }
    return Util::TruncateWithEllipsis(Collapsed, 120);
}

std::string GetReplacesEventId(const Event &Ev) {
    const nlohmann::json *Rel = RelatesTo(Ev.Content);
    if(Rel == nullptr || Util::JsonGetString(*Rel, "rel_type") != "m.replace") {
        return {};
    }
    return Util::JsonGetString(*Rel, "event_id");
}

std::string GetReplyToEventId(const Event &Ev) {
    const nlohmann::json *Rel = RelatesTo(Ev.Content);
    if(Rel == nullptr) {
        return {};
    }
    // Spec rich replies: m.relates_to.m.in_reply_to.event_id, no rel_type.
    const auto InReply = Rel->find("m.in_reply_to");
    if(InReply == Rel->end() || !InReply->is_object()) {
        return {};
    }
    return Util::JsonGetString(*InReply, "event_id");
}

std::vector<ReactionGroup> CollectReactions(const std::vector<Event> &Timeline, const std::string &SelfUserId) {
    std::vector<ReactionGroup> Out;
    std::map<std::string, std::size_t> IndexByKey;
    for(const Event &Ev : Timeline) {
        if(Ev.Type != "m.reaction") {
            continue;
        }
        const nlohmann::json *Rel = RelatesTo(Ev.Content);
        if(Rel == nullptr || Util::JsonGetString(*Rel, "rel_type") != "m.annotation") {
            continue;
        }
        const std::string Key = Util::JsonGetString(*Rel, "key");
        if(Key.empty()) {
            continue;
        }
        const bool Mine = (Ev.Sender == SelfUserId);
        const auto Found = IndexByKey.find(Key);
        if(Found == IndexByKey.end()) {
            ReactionGroup Group;
            Group.Key = Key;
            Group.Senders.push_back(Ev.Sender);
            Group.Mine = Mine;
            IndexByKey[Key] = Out.size();
            Out.push_back(std::move(Group));
        } else {
            ReactionGroup &Group = Out[Found->second];
            Group.Senders.push_back(Ev.Sender);
            if(Mine) {
                Group.Mine = true;
            }
        }
    }
    return Out;
}

std::string HtmlToPlainText(const std::string &Html) {
    std::vector<std::string> Lines;
    std::string Cur;
    int BlockDepth = 0;
    int VerbatimDepth = 0;

    auto PushLine = [&](bool Force) {
        std::string Line = Cur;
        Cur.clear();
        if(VerbatimDepth == 0) {
            Line = Util::Trim(Line);
        } else if(!Line.empty() && Line.back() == '\r') {
            Line.pop_back();
        }
        if(Line.empty() && !Force) {
            return;
        }
        std::string Prefixed;
        for(int I = 0; I < BlockDepth; ++I) {
            Prefixed += "> ";
        }
        Prefixed += Line;
        Lines.push_back(std::move(Prefixed));
    };

    auto AppendNormal = [&Cur](const std::string &Text) {
        std::size_t J = 0;
        while(J < Text.size()) {
            if(IsHtmlSpace(Text[J])) {
                while(J < Text.size() && IsHtmlSpace(Text[J])) {
                    ++J;
                }
                if(!Cur.empty()) {
                    Cur.push_back(' ');
                }
            } else {
                Cur.push_back(Text[J]);
                ++J;
            }
        }
    };

    std::size_t I = 0;
    while(I < Html.size()) {
        if(Html[I] != '<') {
            const std::size_t Next = Html.find('<', I);
            const std::size_t ChunkEnd = (Next == std::string::npos) ? Html.size() : Next;
            const std::string Chunk = DecodeEntities(Html.substr(I, ChunkEnd - I));
            if(VerbatimDepth > 0) {
                std::size_t Start = 0;
                while(true) {
                    const std::size_t Nl = Chunk.find('\n', Start);
                    if(Nl == std::string::npos) {
                        Cur += Chunk.substr(Start);
                        break;
                    }
                    Cur += Chunk.substr(Start, Nl - Start);
                    PushLine(true);
                    Start = Nl + 1;
                }
            } else {
                AppendNormal(Chunk);
            }
            I = ChunkEnd;
            continue;
        }

        const std::size_t TagEnd = Html.find('>', I + 1);
        if(TagEnd == std::string::npos) {
            break; // unterminated tag: drop the rest
        }
        const std::string Tag = Html.substr(I + 1, TagEnd - I - 1);
        I = TagEnd + 1;

        if(!Tag.empty() && Tag[0] == '!') {
            if(Tag.size() >= 2 && Tag[1] == '-') {
                const std::size_t CommentEnd = Html.find("-->", I);
                I = (CommentEnd == std::string::npos) ? Html.size() : CommentEnd + 3;
            }
            continue; // comment or doctype
        }

        const bool Closing = !Tag.empty() && Tag[0] == '/';
        std::size_t NameStart = Closing ? 1 : 0;
        while(NameStart < Tag.size() && IsHtmlSpace(Tag[NameStart])) {
            ++NameStart;
        }
        std::size_t NameEnd = NameStart;
        while(NameEnd < Tag.size() && !IsHtmlSpace(Tag[NameEnd]) && Tag[NameEnd] != '/') {
            ++NameEnd;
        }
        std::string Name;
        for(std::size_t K = NameStart; K < NameEnd; ++K) {
            Name.push_back(LowerAscii(Tag[K]));
        }

        if(Name == "br") {
            const bool HasContent = (VerbatimDepth > 0) ? !Cur.empty() : !Util::Trim(Cur).empty();
            if(HasContent) {
                PushLine(false);
            } else {
                Cur.clear();
                if(!Lines.empty() && !IsBlankLine(Lines.back())) {
                    PushLine(true); // deliberate blank line between content
                }
            }
            continue;
        }
        if(Name == "p" || Name == "div") {
            if(Closing) {
                PushLine(false);
            } else if(!Util::Trim(Cur).empty()) {
                PushLine(false);
            } else {
                Cur.clear(); // drop source whitespace before a block
            }
            continue;
        }
        if(Name == "blockquote") {
            PushLine(false);
            if(Closing) {
                if(BlockDepth > 0) {
                    --BlockDepth;
                }
            } else {
                ++BlockDepth;
            }
            continue;
        }
        if(Name == "li") {
            if(Closing) {
                PushLine(false);
            } else {
                PushLine(false);
                Cur = "- ";
            }
            continue;
        }
        if(Name == "pre") {
            PushLine(false);
            if(Closing) {
                if(VerbatimDepth > 0) {
                    --VerbatimDepth;
                }
            } else {
                ++VerbatimDepth;
            }
            continue;
        }
        if(Name == "code") {
            if(Closing) {
                if(VerbatimDepth > 0) {
                    --VerbatimDepth;
                }
            } else {
                ++VerbatimDepth;
            }
            continue;
        }
        if(Name == "a" && !Closing) {
            const std::string Href = ExtractHref(Tag);
            const std::size_t ClosePos = FindCloseTag(Html, I, "</a>");
            if(ClosePos == std::string::npos) {
                continue; // unpaired anchor: tag stripped, inner text flows on
            }
            const std::string Inner = Html.substr(I, ClosePos - I);
            I = ClosePos + 4;
            const std::string T = CollapseSpaces(HtmlToPlainText(Inner));
            const std::string U = DecodeEntities(Href);
            AppendNormal(T != U ? T : U);
            continue;
        }
        // any other tag: stripped
    }

    if(!Cur.empty()) {
        PushLine(false);
    }

    // Leading/trailing blanks out, blank runs collapse to a single line.
    std::size_t Begin = 0;
    while(Begin < Lines.size() && IsBlankLine(Lines[Begin])) {
        ++Begin;
    }
    std::size_t End = Lines.size();
    while(End > Begin && IsBlankLine(Lines[End - 1])) {
        --End;
    }
    std::vector<std::string> Kept;
    int BlankRun = 0;
    for(std::size_t K = Begin; K < End; ++K) {
        if(IsBlankLine(Lines[K])) {
            ++BlankRun;
            if(BlankRun > 1) {
                continue;
            }
            Kept.push_back({});
        } else {
            BlankRun = 0;
            Kept.push_back(Lines[K]);
        }
    }
    return Util::Join(Kept, "\n");
}

PowerLevels PowerLevels::FromJson(const nlohmann::json &Json) {
    PowerLevels Out; // header defaults are the spec defaults
    if(!Json.is_object()) {
        return Out;
    }
    auto ReadLevel = [](const nlohmann::json &Object, const char *Key, int Fallback) {
        const auto It = Object.find(Key);
        if(It == Object.end()) {
            return Fallback;
        }
        if(It->is_number_integer()) {
            return static_cast<int>(It->get<std::int64_t>());
        }
        if(It->is_number_float()) {
            return static_cast<int>(It->get<double>());
        }
        return Fallback;
    };
    Out.UsersDefault = ReadLevel(Json, "users_default", 0);
    Out.EventsDefault = ReadLevel(Json, "events_default", 0);
    Out.StateDefault = ReadLevel(Json, "state_default", 50);
    Out.Ban = ReadLevel(Json, "ban", 50);
    Out.Kick = ReadLevel(Json, "kick", 50);
    Out.Redact = ReadLevel(Json, "redact", 50);
    Out.Invite = ReadLevel(Json, "invite", 0);
    Out.Name = ReadLevel(Json, "name", 50);
    Out.Topic = ReadLevel(Json, "topic", 50);

    auto ReadMap = [](const nlohmann::json &Object, const char *Key, std::map<std::string, int> &Into) {
        const auto Map = Object.find(Key);
        if(Map == Object.end() || !Map->is_object()) {
            return;
        }
        for(auto It = Map->begin(); It != Map->end(); ++It) {
            if(It.value().is_number_integer()) {
                Into[It.key()] = static_cast<int>(It.value().get<std::int64_t>());
            } else if(It.value().is_number_float()) {
                Into[It.key()] = static_cast<int>(It.value().get<double>());
            }
        }
    };
    ReadMap(Json, "users", Out.Users);
    ReadMap(Json, "events", Out.Events);
    return Out;
}

int PowerLevels::GetUserLevel(const std::string &UserId) const {
    // Spec v1.19: creators are special before any users-map lookup.
    if(!UserId.empty() && UserId == CreatorUserId) {
        if(RoomVersion >= 12) {
            return InfiniteLevel; // not listed in users, immutable
        }
        if(!HasPowerLevelsEvent) {
            return 100; // no m.room.power_levels in the room at all
        }
    }
    const auto It = Users.find(UserId);
    return (It != Users.end()) ? It->second : UsersDefault;
}

bool PowerLevels::CanSendEvent(const std::string &EventType, int UserLevel) const {
    const auto It = Events.find(EventType);
    const int Required = (It != Events.end()) ? It->second : EventsDefault;
    return UserLevel >= Required;
}

bool PowerLevels::CanSendState(const std::string &EventType, int UserLevel) const {
    const auto It = Events.find(EventType);
    const int Required = (It != Events.end()) ? It->second : StateDefault;
    return UserLevel >= Required;
}

bool RoomSummary::HasUnread() const {
    return NotificationCount > 0 || HighlightCount > 0;
}

} // namespace NxMatrix::Matrix