// The player's system UI (frontend/system_ui.h, profiles.h, console_options.h) without a window: the model
// that turns each request into a dialog and the user's navigation into the kernel's answer shapes, driven
// directly and then through the kernel's own exports (the handler the player sets), as a title calls them; the
// pad's navigation with its repeat; the toasts; the gamertag rules and the profile store; the utf conversions;
// the arguments of the process that follows a title launch.

#include "kernel_harness.h"

#include "../frontend/console_options.h"
#include "../frontend/profiles.h"
#include "../frontend/system_ui.h"
#include "../tools/game.h"

#include "objects.h"
#include "xam.h"

#include <filesystem>
#include <thread>

using namespace xf;
using namespace xf::kernel;
using namespace xf::frontend;

namespace {

KernelHarness h;

uint32_t xam_call(const char *name, std::initializer_list<uint64_t> args = {}) { return h.call32("xam", name, args); }

// what a handler gave back
struct Answer {
    bool called = false;
    UiResult result;
};
UiDone into(Answer &a) {
    return [&a](const UiResult &r) { a.called = true, a.result = r; };
}

UiRequest request(UiKind kind) {
    UiRequest r;
    r.kind = kind;
    r.user = 0;
    return r;
}

void message_box_navigation() {
    SystemUi ui([] { return ProfileSummary{}; });
    UiRequest r = request(UiKind::MessageBox);
    r.title = u"Title", r.text = u"Text", r.buttons = {u"Yes", u"No", u"Cancel"}, r.default_button = 1, r.flags = 2;
    Answer a;
    CHECK(ui.handle(r, into(a)) && ui.active());
    Dialog *d = ui.front();
    CHECK(d && d->title == "Title" && d->choices.size() == 3 && d->selected == 1 && d->icon == 2);
    ui.navigate(Nav::Right), ui.navigate(Nav::Right); // clamps at the last
    CHECK(d->selected == 2);
    ui.navigate(Nav::Left), ui.navigate(Nav::Left), ui.navigate(Nav::Left);
    CHECK(d->selected == 0);
    CHECK(!a.called);
    ui.navigate(Nav::Down);
    ui.navigate(Nav::Accept);
    CHECK(a.called && !a.result.cancelled && a.result.button == 1 && !ui.active() && ui.front() == nullptr);

    // B cancels: the kernel completes the overlapped with ERROR_CANCELLED
    Answer b;
    ui.handle(r, into(b));
    ui.navigate(Nav::Cancel);
    CHECK(b.called && b.result.cancelled);

    // two dialogs: the second waits, shown after the first
    Answer c1, c2;
    ui.handle(r, into(c1));
    r.title = u"Second";
    ui.handle(r, into(c2));
    CHECK(ui.front()->title == "Title");
    ui.navigate(Nav::Accept);
    CHECK(c1.called && !c2.called && ui.active() && ui.front()->title == "Second");
    ui.cancel_all();
    CHECK(c2.called && c2.result.cancelled && !ui.active());
    CHECK(ui.taken() == ui.answered());

    // a request it does not know is declined: the kernel's default stands
    Answer o;
    CHECK(!ui.handle(request(UiKind::Other), into(o)) && !ui.active());
}

void keyboard_and_device() {
    SystemUi ui([] { return ProfileSummary{}; });
    UiRequest r = request(UiKind::Keyboard);
    r.title = u"Name", r.default_text = u"Marcus", r.max_length = 4;
    Answer a;
    ui.handle(r, into(a));
    Dialog *d = ui.front();
    CHECK(d->text_entry && d->text == "Marcus" && d->max_length == 4);
    d->text = "Ana\xC3\xA9" "xyz"; // typed by the view
    ui.confirm_text();
    // cut to the room in characters, UTF-16 as the guest wants
    CHECK(a.called && !a.result.cancelled && a.result.text == u"Ana\u00E9");
    // the Cancel choice
    Answer b;
    ui.handle(r, into(b));
    ui.navigate(Nav::Right), ui.navigate(Nav::Accept);
    CHECK(b.called && b.result.cancelled);

    // the device selector: the hard drive (device 1), or cancelled
    UiRequest s = request(UiKind::DeviceSelector);
    s.bytes_required = 3u << 20;
    Answer c;
    ui.handle(s, into(c));
    CHECK(ui.front()->choices.size() == 2 && ui.front()->choices[0] == "Hard Drive");
    ui.navigate(Nav::Accept);
    CHECK(c.called && !c.result.cancelled && c.result.device_id == 1);
    Answer d2;
    ui.handle(s, into(d2));
    ui.navigate(Nav::Down), ui.navigate(Nav::Accept);
    CHECK(d2.called && d2.result.cancelled);
}

void pages() {
    ProfileSummary p;
    p.gamertag = "Tester", p.xuid = 0xE000000000000001ull, p.title_id = 0x12345678, p.signed_in = true;
    for (uint32_t i = 0; i < 10; i++) {
        ProfileAchievement a;
        a.id = i, a.gamerscore = 10, a.unlocked = i < 3, a.label = u"Name", a.description = u"Do it";
        p.achievements.push_back(a);
        p.gamerscore_total += 10;
        if (a.unlocked) p.gamerscore += 10;
    }
    SystemUi ui([&] { return p; });
    // the achievements page: Up/Down and the shoulders scroll, A closes (not cancelled)
    Answer a;
    ui.handle(request(UiKind::Achievements), into(a));
    Dialog *d = ui.front();
    CHECK(d->achievements && d->profile.achievements.size() == 10 && d->lines[0] == "Tester: 30 / 100 G");
    ui.navigate(Nav::Down), ui.navigate(Nav::PageDown);
    CHECK(d->first_row == 7);
    ui.navigate(Nav::PageDown);
    CHECK(d->first_row == 9); // the last row
    ui.navigate(Nav::PageUp), ui.navigate(Nav::PageUp);
    CHECK(d->first_row == 0);
    ui.navigate(Nav::Accept);
    CHECK(a.called && !a.result.cancelled);

    // the gamer card: the profile, or "not available" for another player
    Answer g;
    ui.handle(request(UiKind::GamerCard), into(g));
    CHECK(ui.front()->lines[0] == "Gamertag: Tester" && ui.front()->lines[2].find("30 / 100 G") != std::string::npos);
    ui.navigate(Nav::Accept);
    UiRequest other = request(UiKind::GamerCard);
    other.xuid = 0xE0000000000000FFull;
    ui.handle(other, into(g));
    CHECK(ui.front()->lines[0].find("E0000000000000FF") != std::string::npos);
    ui.navigate(Nav::Cancel);
    // sign-in: signing in, or backing out (the title's follow-up runs only for the first)
    Answer s;
    ui.handle(request(UiKind::SignIn), into(s));
    CHECK(ui.front()->choices[0] == "Sign in as Tester");
    ui.navigate(Nav::Accept);
    CHECK(s.called && !s.result.cancelled);
    Answer s2;
    ui.handle(request(UiKind::SignIn), into(s2));
    ui.navigate(Nav::Down), ui.navigate(Nav::Accept);
    CHECK(s2.called && s2.result.cancelled);
    // every page the kernel asks for is shown, and closes
    for (UiKind k : {UiKind::Marketplace, UiKind::Friends, UiKind::Messages, UiKind::Players, UiKind::QuickChat,
                     UiKind::GameInvite, UiKind::FriendRequest, UiKind::PlayerReview, UiKind::MessageCompose,
                     UiKind::CustomMessageCompose, UiKind::CustomPlayerList, UiKind::EditProfile}) {
        Answer x;
        CHECK(ui.handle(request(k), into(x)));
        CHECK(!ui.front()->title.empty() && !ui.front()->lines.empty() && ui.front()->choices.size() == 1);
        ui.navigate(Nav::Accept);
        CHECK(x.called && !x.result.cancelled);
    }
}

void pad_navigation() {
    PadNavigator nav;
    input::Gamepad pad;
    CHECK(nav.feed(pad, 0).empty());
    pad.buttons = input::DPAD_DOWN;
    auto first = nav.feed(pad, 1000);
    CHECK(first.size() == 1 && first[0] == Nav::Down);
    CHECK(nav.feed(pad, 1200).empty());                       // held, before the repeat
    auto again = nav.feed(pad, 1400);
    CHECK(again.size() == 1 && again[0] == Nav::Down);       // the repeat after 400 ms
    CHECK(nav.feed(pad, 1450).empty());
    CHECK(nav.feed(pad, 1500).size() == 1);                   // then every 100 ms
    pad.buttons = input::A;
    auto a = nav.feed(pad, 1600);
    CHECK(a.size() == 1 && a[0] == Nav::Accept);
    CHECK(nav.feed(pad, 2600).empty());                       // a button press does not repeat
    pad.buttons = 0;
    pad.thumb_ly = -30000; // the stick down
    auto stick = nav.feed(pad, 3000);
    CHECK(stick.size() == 1 && stick[0] == Nav::Down);
    pad = {};
    CHECK(nav.feed(pad, 3050).empty());
    // a button down as a dialog opens is not an answer
    pad.buttons = input::A;
    nav.reset(pad, 4000);
    CHECK(nav.feed(pad, 4010).empty());
    pad.buttons = input::B;
    auto b = nav.feed(pad, 4020);
    CHECK(b.size() == 1 && b[0] == Nav::Cancel);
}

void toasts() {
    SystemUi ui([] { return ProfileSummary{}; });
    CHECK(!ui.has_toasts());
    Toast t;
    t.kind = Toast::Kind::Achievement, t.title = u"First", t.gamerscore = 15;
    ui.toast(t);
    CHECK(ui.has_toasts());
    CHECK(ui.toasts(10.0).size() == 1);
    CHECK(ui.toasts(12.0).size() == 1 && ui.toasts(12.0)[0].toast.gamerscore == 15);
    CHECK(ui.toasts(15.5).empty() && !ui.has_toasts()); // gone after 5 s
}

void text_and_rules() {
    CHECK(utf8_of(u"A\u00E9\u20AC\U0001F600") == "A\xC3\xA9\xE2\x82\xAC\xF0\x9F\x98\x80");
    CHECK(utf16_of("A\xC3\xA9\xE2\x82\xAC\xF0\x9F\x98\x80") == u"A\u00E9\u20AC\U0001F600");
    CHECK(utf16_of("abcdef", 3) == u"abc");
    std::string why;
    CHECK(valid_gamertag("Marcus 07", why));
    CHECK(valid_gamertag("A", why) && valid_gamertag("123456789012345", why) == false); // starts with a digit
    CHECK(valid_gamertag("Abcdefghijklmno", why) && !valid_gamertag("Abcdefghijklmnop", why) &&
          why.find("15") != std::string::npos);
    CHECK(!valid_gamertag("", why) && !valid_gamertag("Bad name ", why) && !valid_gamertag("Two  spaces", why) &&
          !valid_gamertag("Caf\xC3\xA9", why) && !valid_gamertag("a_b", why));
    Settings::System sys;
    sys.language = 99, sys.country = 12345, sys.refresh = 75, sys.widescreen = 3, sys.width = 800, sys.height = 600;
    clamp_system(sys);
    CHECK(sys.language == 0 && sys.country == -1 && sys.refresh == 0 && sys.widescreen == -1 && sys.width == 0);
    set_resolution(sys, 1080);
    CHECK(sys.width == 1920 && resolution_of(sys) == 1080);
}

void profile_store() {
    namespace fs = std::filesystem;
    fs::path root = fs::temp_directory_path() / "xf_system_ui_test";
    fs::remove_all(root);
    Settings s;
    std::string xuid, error;
    CHECK(list_profiles(root, s).empty());
    CHECK(!create_profile(root, s, "bad name ", xuid, error) && !error.empty());
    CHECK(create_profile(root, s, "First", xuid, error) && xuid.size() == 16);
    CHECK(s.profile.xuid == xuid && s.profile.gamertag == "First" && selected_xuid(s) != 0);
    CHECK(fs::exists(root / "content" / xuid / "FFFE07D1" / "00010000" / xuid));
    CHECK(!create_profile(root, s, "FIRST", xuid, error)); // the name is taken, whatever its case
    std::string second;
    CHECK(create_profile(root, s, "Second", second, error) && second != xuid);
    auto all = list_profiles(root, s);
    CHECK(all.size() == 2 && all[0].selected + all[1].selected == 1);
    CHECK(select_profile(s, second, error) && selected_gamertag(root, s) == "Second");
    CHECK(rename_profile(s, second, "Renamed", error) && s.profile.gamertag == "Renamed");
    CHECK(!rename_profile(s, second, "no good!", error));
    CHECK(!delete_profile(root, s, "nonsense", error));
    CHECK(delete_profile(root, s, second, error) && !fs::exists(root / "content" / second));
    CHECK(s.profile.xuid == xuid); // the selected one went with it: the other is selected
    CHECK(!delete_profile(root, s, xuid, error)); // the only one stays
    // the settings keep it all
    fs::path file = root / "config.toml";
    s.saves.format = "directory";
    CHECK(save(file, s, error));
    Settings back;
    CHECK(load(file, back, error) && back.profile.xuid == xuid && back.profile.names.at(xuid) == "First" &&
          back.saves.format == "directory" && back.follow_launches);
    CHECK(storage_root(back, file) == root / "storage");
    back.saves.root = (root / "elsewhere").string();
    CHECK(storage_root(back, file) == root / "elsewhere");
    fs::remove_all(root);
}

void launch_arguments() {
    CHECK(game::disc_path("game:\\Folder\\Next.xex") == "Folder/Next.xex");
    CHECK(game::disc_path("D:\\Next.xex") == "Next.xex" && game::disc_path("hdd:\\x.xex").empty());
    std::vector<std::string> args = {"player", "disc.iso", "--xex", "default.xex", "--quit-after", "60", "--press",
                                     "a@5", "--launch-data", "old.bin"};
    auto next = game::relaunch_args(args, {{"--xex", "Next.xex"}, {"--launch-data", "new.bin"}, {"--quit-after", ""}});
    std::vector<std::string> want = {"player", "disc.iso", "--press", "a@5", "--xex", "Next.xex", "--launch-data",
                                     "new.bin"};
    CHECK(next == want);
}

// the kernel's own exports, with the player's model as the handler
void through_the_kernel() {
    SystemUi ui([] { return ProfileSummary{}; });
    set_ui_handler(*h.k, [&](const UiRequest &r, UiDone done) { return ui.handle(r, std::move(done)); });
    uint32_t box = h.alloc(4), buttons = h.alloc(8), device = h.alloc(4), text = h.alloc(64);
    h.k->store32(box, 0xEEEEEEEE), h.k->store32(device, 0xEEEEEEEE);
    h.k->store32(buttons, h.wide(u"Yes")), h.k->store32(buttons + 4, h.wide(u"No"));
    auto ov = [&] {
        uint32_t o = h.alloc(0x1C);
        std::memset(h.k->host(o), 0xEE, 0x1C);
        auto event = objects(*h.k).create<Event>(GuestType::EventNotification, header::SIZE);
        h.k->store32(o + xoverlapped::EVENT, event->handle());
        h.k->store32(o + xoverlapped::COMPLETION_ROUTINE, 0);
        return o;
    };
    // a message box: pending until the user answers; the second button chosen
    uint32_t o1 = ov();
    CHECK(xam_call("XamShowMessageBoxUI", {0, h.wide(u"Title"), h.wide(u"Text"), 2, buttons, 0, 0, box, o1}) ==
          X_ERROR_IO_PENDING);
    CHECK(ui.active() && xam_call("XamIsUIActive") == 1 && h.k->load32(box) == 0xEEEEEEEE);
    CHECK(h.k->load32(o1 + xoverlapped::RESULT) == X_ERROR_IO_PENDING);
    ui.navigate(Nav::Right);
    std::thread([&] { ui.navigate(Nav::Accept); }).join();
    CHECK(h.k->load32(box) == 1 && h.k->load32(o1 + xoverlapped::RESULT) == X_ERROR_SUCCESS && !ui.active());
    // backing out: ERROR_CANCELLED, and the result is left alone
    h.k->store32(box, 0xEEEEEEEE);
    uint32_t o2 = ov();
    xam_call("XamShowMessageBoxUI", {0, h.wide(u"T"), h.wide(u"X"), 2, buttons, 0, 0, box, o2});
    ui.navigate(Nav::Cancel);
    CHECK(h.k->load32(o2 + xoverlapped::RESULT) == 0x4C7 && h.k->load32(box) == 0xEEEEEEEE);
    // the device selector: the drive id stored
    uint32_t o3 = ov();
    CHECK(xam_call("XamShowDeviceSelectorUI", {0xFF, 1, 0, 0x10000, device, o3}) == X_ERROR_IO_PENDING);
    CHECK(ui.front() && ui.front()->kind == UiKind::DeviceSelector && ui.front()->request.bytes_required == 0x10000);
    ui.navigate(Nav::Accept);
    CHECK(h.k->load32(device) == 1 && h.k->load32(o3 + xoverlapped::RESULT) == X_ERROR_SUCCESS);
    // the keyboard: the typed text, cut to the room, in the title's buffer
    uint32_t o4 = ov();
    std::memset(h.k->host(text), 0xEE, 64);
    CHECK(xam_call("XamShowKeyboardUI", {0, 0, h.wide(u"Marcus"), h.wide(u"Name"), h.wide(u"Your name"), text, 6, o4}) ==
          X_ERROR_IO_PENDING);
    ui.front()->text = "Zed";
    ui.confirm_text();
    CHECK(h.k->load32(o4 + xoverlapped::RESULT) == X_ERROR_SUCCESS && h.k->load32(text) == 0x005A0065 &&
          h.k->load32(text + 4) == 0x00640000);
    // a page the title does not wait for: success at once, the dialog up until closed
    CHECK(xam_call("XamShowFriendsUI", {0}) == X_ERROR_SUCCESS);
    CHECK(ui.active() && ui.front()->kind == UiKind::Friends);
    ui.navigate(Nav::Accept);
    CHECK(!ui.active());
    set_ui_handler(*h.k, nullptr);
}

} // namespace

int main() {
    if (!h.init()) return 1;
    message_box_navigation();
    keyboard_and_device();
    pages();
    pad_navigation();
    toasts();
    text_and_rules();
    profile_store();
    launch_arguments();
    through_the_kernel();
    return finish("system_ui");
}
