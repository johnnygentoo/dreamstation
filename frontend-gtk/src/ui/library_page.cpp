// SPDX-License-Identifier: GPL-3.0-or-later
#include "library_page.hpp"
#include "../config.hpp"

#include <algorithm>
#include <optional>
#include <cstdlib>

namespace ds::frontend {

namespace fs = std::filesystem;

// ── Covers ────────────────────────────────────────────────────────────────

// Our own folder comes first; after that we reuse art other frontends on
// this machine have already downloaded.
std::vector<fs::path> cover_dirs() {
    std::vector<fs::path> dirs{Config::covers_dir()};
    if (const char* home = std::getenv("HOME")) {
        const fs::path h(home);
        for (const fs::path& d : {
                 h / "ES-DE/downloaded_media/dreamcast/covers",
                 h / ".emulationstation/downloaded_media/dreamcast/covers",
                 h / ".config/retroarch/thumbnails/Sega - Dreamcast/Named_Boxarts"}) {
            std::error_code ec;
            if (fs::is_directory(d, ec))
                dirs.push_back(d);
        }
    }
    return dirs;
}

// The same, for arcade boards.
static std::vector<fs::path> arcade_cover_dirs() {
    std::vector<fs::path> dirs;
    if (const char* home = std::getenv("HOME")) {
        const fs::path h(home);
        for (const fs::path& d : {
                 h / "ES-DE/downloaded_media/naomi/covers",
                 h / "ES-DE/downloaded_media/naomi2/covers",
                 h / "ES-DE/downloaded_media/atomiswave/covers",
                 h / ".emulationstation/downloaded_media/naomi/covers",
                 h / ".emulationstation/downloaded_media/atomiswave/covers",
                 h / ".config/retroarch/thumbnails/Sega - Naomi/Named_Boxarts",
                 h / ".config/retroarch/thumbnails/Sega - Naomi 2/Named_Boxarts",
                 h / ".config/retroarch/thumbnails/Atomiswave/Named_Boxarts"}) {
            std::error_code ec;
            if (fs::is_directory(d, ec))
                dirs.push_back(d);
        }
    }
    return dirs;
}

// "Dead or Alive 2 (Rev A)" -> "deadoralive2": bracketed notes, case and
// punctuation dropped, so differently written names of a game compare equal.
static std::string cover_key(const std::string& name) {
    std::string key;
    int depth = 0;
    for (const char c : name) {
        if (c == '(' || c == '[') ++depth;
        else if ((c == ')' || c == ']') && depth > 0) --depth;
        else if (depth == 0 && std::isalnum(static_cast<unsigned char>(c)))
            key += char(std::tolower(static_cast<unsigned char>(c)));
    }
    return key;
}

// How well a cover name fits beyond its title: the game's disc, then its
// region, then the fewest extra notes ("(Genteiban)", "- Copy").
static long cover_bonus(const std::string& name, const std::string& region, int disc) {
    long score = 0;
    if (disc > 0) {
        if (name.find("(Disc " + std::to_string(disc) + ")") != std::string::npos)
            score += 1000;
        else if (name.find("(Disc ") != std::string::npos)
            score -= 1000;
    }
    static const std::pair<char, const char*> kRegions[] = {
        {'U', "(USA"}, {'E', "(Europe"}, {'J', "(Japan"}};
    long weight = 300;
    for (const auto& [letter, tag] : kRegions) {
        if (region.find(letter) != std::string::npos && name.find(tag) != std::string::npos) {
            score += weight;
            break;
        }
        weight -= 100;
    }
    // Extra notes: bracket groups and " - " parts ("(Genteiban)", "- Copy").
    long notes = 0;
    for (std::size_t i = 0; i < name.size(); ++i)
        if (name[i] == '(' || name[i] == '[' || name.compare(i, 3, " - ") == 0)
            ++notes;
    return score - notes;
}

int match_cover_name(const std::string& title, const std::vector<std::string>& names,
                     const std::string& region, int disc) {
    std::vector<std::string> keys;
    keys.reserve(names.size());
    for (const auto& n : names)
        keys.push_back(cover_key(n));
    // The whole title, then each of its alternatives ("Cannon Spike / Gun Spike").
    std::vector<std::string> wanted{cover_key(title)};
    if (title.find(" / ") != std::string::npos)
        for (std::size_t pos = 0;;) {
            const std::size_t cut = title.find(" / ", pos);
            wanted.push_back(cover_key(title.substr(pos, cut - pos)));   // npos: to the end
            if (cut == std::string::npos)
                break;
            pos = cut + 3;
        }

    // Lower tier wins; within a tier the higher score.
    int best = -1, best_tier = 5;
    long best_score = 0;
    const auto consider = [&](std::size_t i, int tier, long score) {
        if (tier < best_tier || (tier == best_tier && score > best_score)) {
            best = static_cast<int>(i);
            best_tier = tier;
            best_score = score;
        }
    };
    for (std::size_t w = 0; w < wanted.size(); ++w) {
        const auto& want = wanted[w];
        if (want.empty())
            continue;
        for (std::size_t i = 0; i < keys.size(); ++i) {
            const auto& k = keys[i];
            const long bonus = cover_bonus(names[i], region, disc);
            if (k == want) {
                consider(i, w == 0 ? 0 : 1, bonus);
            } else if (want.size() >= 6 && k.size() >= 6 && want.rfind(k, 0) == 0) {
                // A cover named after the start of the title ("Marvel vs.
                // Capcom 2" for "... New Age of Heroes"): the longest such.
                consider(i, 2, static_cast<long>(k.size()) * 10000 + bonus);
            } else if (want.size() >= 6 && k.rfind(want, 0) == 0) {
                // The title as the start of a longer cover name ("Giant Gram
                // 2000" for "Giant Gram 2000 Zennihon ..."): the shortest such.
                consider(i, 3, -static_cast<long>(k.size()) * 10000 + bonus);
            }
        }
    }
    if (best >= 0)
        return best;
    // Last resort: the title as one part of a cover name ("SGGG - Segagaga").
    for (std::size_t i = 0; i < names.size(); ++i)
        for (std::size_t pos = 0;;) {
            const std::size_t cut = names[i].find(" - ", pos);
            const std::string part = cover_key(names[i].substr(pos, cut - pos));
            for (const auto& want : wanted)
                if (want.size() >= 6 && part == want)
                    consider(i, 4, cover_bonus(names[i], region, disc));
            if (cut == std::string::npos)
                break;
            pos = cut + 3;
        }
    return best;
}

// Covers other frontends have downloaded are named after the game's title,
// each in its own way ("Crazy_Taxi.png", "Carrier (USA).png", or an image
// inside a folder named after the game, possibly a few folders deep).
struct CoverIndex {
    std::vector<std::string> names;
    std::vector<fs::path>    files;
};

static CoverIndex index_covers(const std::vector<fs::path>& dirs) {
    CoverIndex index;
    const auto is_image = [](const fs::path& p) {
        const auto ext = p.extension();
        return ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".webp";
    };
    // Images by their own names first, so that on a tie the image named after
    // the game wins over one inside a folder named after it.
    CoverIndex folders;
    for (const auto& dir : dirs) {
        std::error_code ec;
        std::vector<fs::path> images;
        for (auto it = fs::recursive_directory_iterator(dir, ec);
             !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
            if (it.depth() >= 3)
                it.disable_recursion_pending();
            if (it->is_regular_file(ec) && is_image(it->path()))
                images.push_back(it->path());
        }
        // Shallower first, then by name ("Disc 1" before "Disc 2").
        const auto depth = [](const fs::path& p) { return std::distance(p.begin(), p.end()); };
        std::sort(images.begin(), images.end(), [&](const fs::path& x, const fs::path& y) {
            const auto dx = depth(x), dy = depth(y);
            return dx != dy ? dx < dy : x < y;
        });
        std::vector<fs::path> named_folders;       // folders already pointing at an image
        for (const auto& p : images) {
            index.names.push_back(p.stem().string());
            index.files.push_back(p);
            // Each folder between the cover directory and the image names it too.
            for (fs::path folder = p.parent_path(); folder != dir && folder.has_parent_path();
                 folder = folder.parent_path()) {
                if (std::find(named_folders.begin(), named_folders.end(), folder) != named_folders.end())
                    break;
                named_folders.push_back(folder);
                folders.names.push_back(folder.filename().string());
                folders.files.push_back(p);
            }
        }
    }
    index.names.insert(index.names.end(), folders.names.begin(), folders.names.end());
    index.files.insert(index.files.end(), folders.files.begin(), folders.files.end());
    return index;
}

std::string cover_title(const GameEntry& g) {
    // Arcade games whose covers elsewhere go by another name, by set.
    static const std::map<std::string, std::string> kAliases = {
        {"samsptk",  "Samurai Spirits Tenkaichi Kenkakuden"},
        {"qmegamis", "Quiz Aa! Megami-sama"},
    };
    std::string title = g.set_title.empty() ? g.title : g.set_title;
    if (g.is_arcade())
        if (const auto it = kAliases.find(g.product_id); it != kAliases.end())
            title += " / " + it->second;
    return title;
}

// Indexes of the folders other programs keep covers in, made when first
// needed after start-up or forget_cover_index(). Covers we download ourselves
// are found by file name before it comes to these.
static std::optional<CoverIndex> g_dreamcast_index, g_arcade_index;

void forget_cover_index() {
    g_dreamcast_index.reset();
    g_arcade_index.reset();
}

static fs::path find_cover_by_title(const GameEntry& g, const std::vector<fs::path>& dirs) {
    if (!g_dreamcast_index) {
        std::vector<fs::path> others(dirs.begin(), dirs.end());
        std::erase(others, Config::covers_dir());
        g_dreamcast_index = index_covers(others);
    }
    if (!g_arcade_index)
        g_arcade_index = index_covers(arcade_cover_dirs());
    const CoverIndex& index = g.is_arcade() ? *g_arcade_index : *g_dreamcast_index;
    const int i = match_cover_name(cover_title(g), index.names, g.region, g.disc);
    return i >= 0 ? index.files[static_cast<std::size_t>(i)] : fs::path{};
}

// Cover naming differs per source: by path relative to the ROM directory, by
// the game's folder, by file name, by cleaned title or by product number.
fs::path find_cover(const GameEntry& g, const std::vector<fs::path>& dirs) {
    std::error_code ec;
    // Most specific first. A generic file name ("disc") identifies nothing,
    // so it is never used on its own.
    std::vector<fs::path> names;
    const bool generic = GameLibrary::is_generic_name(g.path.stem().string());
    const fs::path rel = fs::relative(g.path, g.root, ec);
    if (!ec && !rel.empty()) {
        if (!generic)
            names.push_back(fs::path(rel).replace_extension());
        if (rel.has_parent_path()) {
            names.push_back(rel.parent_path());      // the game's own folder
            names.push_back(rel.parent_path().filename());   // ... by its name alone
            names.push_back(*rel.begin());           // top-level game folder
        }
    }
    if (!generic)
        names.push_back(g.path.stem());
    names.push_back(g.title);
    if (!g.product_id.empty())
        names.push_back(g.product_id);

    for (const auto& dir : dirs)
        for (const auto& name : names)
            for (const char* ext : {".png", ".jpg", ".jpeg", ".webp"}) {
                fs::path candidate = dir / name;
                candidate += ext;
                if (fs::is_regular_file(candidate, ec))
                    return candidate;
            }
    return find_cover_by_title(g, dirs);
}

namespace {

// One row of the table view.
class GameItem : public Glib::Object {
public:
    static Glib::RefPtr<GameItem> create(const GameEntry& g, PlayStats::Entry stats) {
        return Glib::make_refptr_for_instance<GameItem>(new GameItem(g, stats));
    }
    GameEntry        game;
    PlayStats::Entry stats;

private:
    GameItem(const GameEntry& g, PlayStats::Entry s) : game(g), stats(s) {}
};

std::string lower(std::string s) {
    for (auto& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string format_size(std::uintmax_t bytes) {
    if (bytes == 0)
        return {};
    char buf[32];
    const double mb = static_cast<double>(bytes) / (1024.0 * 1024.0);
    if (mb >= 1024.0) std::snprintf(buf, sizeof(buf), "%.2f GB", mb / 1024.0);
    else              std::snprintf(buf, sizeof(buf), "%.0f MB", mb);
    return buf;
}

// CSS class giving each system's badge its own colour.
std::string system_css_class(const std::string& system) {
    if (system == "Naomi")      return "ds-system-naomi";
    if (system == "Naomi 2")    return "ds-system-naomi2";
    if (system == "Atomiswave") return "ds-system-atomiswave";
    if (system == "System SP")  return "ds-system-systemsp";
    return "ds-system-dreamcast";
}

// Dreamcast area symbols ("JUE") -> readable region list.
std::string region_name(const std::string& area) {
    std::string out;
    const auto add = [&](const char* n) { out += (out.empty() ? "" : ", "); out += n; };
    if (area.find('J') != std::string::npos) add("Japan");
    if (area.find('U') != std::string::npos) add("USA");
    if (area.find('E') != std::string::npos) add("Europe");
    return out;
}

const char* const kSortKeys[]   = {"title", "region", "format", "size", "played", "last"};
const char* const kSortLabels[] = {"Title", "Region", "Format", "Size", "Time Played", "Last Played"};

} // namespace

// ── Construction ──────────────────────────────────────────────────────────

LibraryPage::LibraryPage(Config& config, PlayStats& stats,
                         std::function<std::string(const fs::path&)> game_id_for)
    : Gtk::Box(Gtk::Orientation::VERTICAL), m_config(config), m_stats(stats),
      m_game_id_for(std::move(game_id_for)) {
    set_hexpand(true);
    set_vexpand(true);

    build_header();
    append(m_header);

    // Empty state
    m_empty_page.set_icon_name("applications-games-symbolic");
    m_empty_page.set_title("No Games Found");
    m_empty_page.set_description(
        "No games in supported formats were found. Add a directory with games in "
        "Settings → Game List to begin.\n\nSupported: GDI, CDI, CHD and CUE disc images, "
        "and Naomi / Atomiswave ROM sets (zip, 7z).");
    m_empty_page.set_vexpand(true);

    // Grid
    m_flow.set_homogeneous(true);
    m_flow.set_column_spacing(12);
    m_flow.set_row_spacing(12);
    m_flow.set_margin(16);
    m_flow.set_min_children_per_line(2);
    m_flow.set_max_children_per_line(32);
    m_flow.set_valign(Gtk::Align::START);
    m_flow.set_selection_mode(Gtk::SelectionMode::SINGLE);
    m_flow.set_activate_on_single_click(false);
    m_flow.signal_child_activated().connect([this](Gtk::FlowBoxChild* child) {
        signal_launch_game.emit(child->get_name());
    });
    m_grid_scroll.set_child(m_flow);
    m_grid_scroll.set_policy(Gtk::PolicyType::NEVER, Gtk::PolicyType::AUTOMATIC);
    m_grid_scroll.set_vexpand(true);

    // Ctrl+wheel zooms the grid.
    auto wheel = Gtk::EventControllerScroll::create();
    wheel->set_flags(Gtk::EventControllerScroll::Flags::VERTICAL);
    wheel->set_propagation_phase(Gtk::PropagationPhase::CAPTURE);
    wheel->signal_scroll().connect([this, wheel](double, double dy) {
        const auto state = wheel->get_current_event_state();
        if ((state & Gdk::ModifierType::CONTROL_MASK) == Gdk::ModifierType{})
            return false;
        zoom(dy < 0 ? 1 : -1);
        return true;
    }, false);
    m_grid_scroll.add_controller(wheel);

    // List
    m_store = Gio::ListStore<Glib::Object>::create();
    build_list_columns();
    m_list_scroll.set_child(m_columns);
    m_list_scroll.set_vexpand(true);

    m_inner_stack.add(m_empty_page, "empty");
    m_inner_stack.add(m_grid_scroll, "grid");
    m_inner_stack.add(m_list_scroll, "list");
    m_inner_stack.set_vexpand(true);
    append(m_inner_stack);

    // Context menu
    m_actions = Gio::SimpleActionGroup::create();
    for (const char* name : {"launch", "properties", "open-dir", "set-cover", "fresh-boot",
                             "play-online", "delete-states", "exclude", "reset-playtime"}) {
        m_actions->add_action(name, [this, name] {
            if (m_menu_path.empty())
                return;
            if (std::string(name) == "launch") signal_launch_game.emit(m_menu_path);
            else                               signal_game_action.emit(name, m_menu_path);
        });
    }
    insert_action_group("game", m_actions);

    auto menu = Gio::Menu::create();
    auto top = Gio::Menu::create();
    top->append("Properties…",                 "game.properties");
    top->append("Open Containing Directory…",  "game.open-dir");
    top->append("Set Cover Image…",            "game.set-cover");
    menu->append_section({}, top);
    auto boot = Gio::Menu::create();
    boot->append("Default Boot",               "game.launch");
    boot->append("Fresh Boot (Ignore Resume State)", "game.fresh-boot");
    boot->append("Play Online…",               "game.play-online");
    boot->append("Delete Save States",         "game.delete-states");
    menu->append_section({}, boot);
    auto manage = Gio::Menu::create();
    manage->append("Exclude From List",        "game.exclude");
    manage->append("Reset Play Time",          "game.reset-playtime");
    menu->append_section({}, manage);
    m_menu.set_menu_model(menu);
    m_menu.set_parent(*this);
    m_menu.set_has_arrow(false);
}

void LibraryPage::build_header() {
    m_header.set_margin_start(16);
    m_header.set_margin_end(16);
    m_header.set_margin_top(10);
    m_header.set_margin_bottom(4);
    m_header.add_css_class("ds-library-header");

    m_grid_btn.set_icon_name("view-grid-symbolic");
    m_grid_btn.set_tooltip_text("Game Grid");
    m_list_btn.set_icon_name("view-list-symbolic");
    m_list_btn.set_tooltip_text("Game List");
    m_list_btn.set_group(m_grid_btn);
    (m_config.library.view == "list" ? m_list_btn : m_grid_btn).set_active(true);
    m_grid_btn.signal_toggled().connect([this] {
        if (m_building) return;
        m_config.library.view = m_grid_btn.get_active() ? "grid" : "list";
        m_config.save();
        rebuild();
    });
    auto* views = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL);
    views->add_css_class("linked");
    views->append(m_grid_btn);
    views->append(m_list_btn);
    m_header.append(*views);

    const auto make_drop = [this](Gtk::DropDown& drop, std::vector<Glib::ustring> items,
                                  const char* tip) {
        drop.set_model(Gtk::StringList::create(items));
        drop.set_tooltip_text(tip);
        drop.property_selected().signal_changed().connect([this] { if (!m_building) rebuild(); });
        m_header.append(drop);
    };
    make_drop(m_type_filter,   {"All Systems", "Dreamcast", "All Arcade", "Naomi", "Naomi 2",
                                "Atomiswave", "System SP"}, "Filter by system");
    make_drop(m_region_filter, {"All Regions", "Japan", "USA", "Europe"}, "Filter by region");

    std::vector<Glib::ustring> sort_items;
    guint sort_sel = 0;
    for (guint i = 0; i < std::size(kSortKeys); ++i) {
        sort_items.push_back(std::string("Sort: ") + kSortLabels[i]);
        if (m_config.library.sort_key == kSortKeys[i]) sort_sel = i;
    }
    m_sort.set_model(Gtk::StringList::create(sort_items));
    m_sort.set_selected(sort_sel);
    m_sort.set_tooltip_text("Sort by");
    m_sort.property_selected().signal_changed().connect([this] {
        if (m_building) return;
        m_config.library.sort_key = kSortKeys[std::min<guint>(m_sort.get_selected(),
                                                              std::size(kSortKeys) - 1)];
        m_config.save();
        rebuild();
    });
    m_header.append(m_sort);

    m_desc_btn.set_icon_name("view-sort-descending-symbolic");
    m_desc_btn.set_tooltip_text("Descending order");
    m_desc_btn.set_active(m_config.library.descending);
    m_desc_btn.signal_toggled().connect([this] {
        if (m_building) return;
        m_config.library.descending = m_desc_btn.get_active();
        m_config.save();
        rebuild();
    });
    m_header.append(m_desc_btn);

    m_scale.set_range(120, 420);
    m_scale.set_increments(20, 60);
    m_scale.set_value(m_config.library.cover_size);
    m_scale.set_size_request(140, -1);
    m_scale.set_draw_value(false);
    m_scale.set_tooltip_text("Cover size (Ctrl + mouse wheel)");
    m_scale.signal_value_changed().connect([this] {
        if (m_building) return;
        m_config.library.cover_size = static_cast<int>(m_scale.get_value());
        m_config.save();
        if (m_config.library.view == "grid")
            rebuild();
    });
    m_header.append(m_scale);

    m_search_entry.set_hexpand(true);
    m_search_entry.set_placeholder_text("Search…");
    m_search_entry.signal_search_changed().connect([this] { if (!m_building) rebuild(); });
    m_header.append(m_search_entry);

    m_count.add_css_class("dim-label");
    m_header.append(m_count);
}

void LibraryPage::build_list_columns() {
    auto selection = Gtk::SingleSelection::create(m_store);
    selection->set_autoselect(false);
    m_columns.set_model(selection);
    m_columns.set_show_row_separators(true);
    m_columns.add_css_class("ds-game-table");
    m_columns.signal_activate().connect([this](guint pos) {
        if (auto item = std::dynamic_pointer_cast<GameItem>(m_store->get_object(pos)))
            signal_launch_game.emit(item->game.path.string());
    });

    const auto add_column = [this](const char* title, bool expand,
                                   std::function<std::string(const GameItem&)> getter,
                                   bool system_pill = false) {
        auto factory = Gtk::SignalListItemFactory::create();
        factory->signal_setup().connect([this, system_pill](const Glib::RefPtr<Gtk::ListItem>& li) {
            auto* label = Gtk::make_managed<Gtk::Label>();
            label->set_xalign(0.f);
            // A badge is short and must stay whole; the column fits it.
            label->set_ellipsize(system_pill ? Pango::EllipsizeMode::NONE : Pango::EllipsizeMode::END);
            label->set_margin_start(6);
            label->set_margin_end(6);
            // Right-click anywhere in a row opens the game's menu.
            auto click = Gtk::GestureClick::create();
            click->set_button(GDK_BUTTON_SECONDARY);
            click->signal_pressed().connect([this, label](int, double x, double y) {
                show_context_menu(*label, x, y, label->get_name());
            });
            label->add_controller(click);
            li->set_child(*label);
        });
        factory->signal_bind().connect([getter, system_pill](const Glib::RefPtr<Gtk::ListItem>& li) {
            auto item  = std::dynamic_pointer_cast<GameItem>(li->get_item());
            auto* label = dynamic_cast<Gtk::Label*>(li->get_child());
            if (!item || !label) return;
            label->set_text(getter(*item));
            label->set_name(item->game.path.string());
            if (system_pill) {
                // Rows are recycled: drop the previous game's system first.
                label->set_css_classes({"ds-system-pill", system_css_class(item->game.system)});
                label->set_hexpand(false);
                label->set_halign(Gtk::Align::START);
            }
        });
        auto column = Gtk::ColumnViewColumn::create(title, factory);
        column->set_expand(expand);
        column->set_resizable(true);
        m_columns.append_column(column);
    };

    add_column("Title", true,  [](const GameItem& i) { return i.game.title; });
    add_column("Product", false, [](const GameItem& i) { return i.game.product_id; });
    add_column("System", false, [](const GameItem& i) { return i.game.system; }, true);
    add_column("Region", false, [](const GameItem& i) { return region_name(i.game.region); });
    add_column("Format", false, [](const GameItem& i) { return i.game.format; });
    add_column("Size", false,   [](const GameItem& i) { return format_size(i.game.size); });
    add_column("Time Played", false,
               [](const GameItem& i) { return PlayStats::format_duration(i.stats.seconds); });
    add_column("Last Played", false,
               [](const GameItem& i) { return PlayStats::format_date(i.stats.last); });
}

// ── Public ────────────────────────────────────────────────────────────────

void LibraryPage::set_library(GameLibrary* lib) {
    m_library = lib;
    if (lib) {
        // A rescan can bring new games and covers: look them up again, and
        // stay where the user had scrolled to.
        lib->signal_updated.connect(sigc::mem_fun(*this, &LibraryPage::refresh));
        rebuild();
    }
}

void LibraryPage::set_search_visible(bool v) {
    if (v) m_search_entry.grab_focus();
    else   m_search_entry.set_text("");
}

bool LibraryPage::search_visible() const { return m_search_entry.has_focus(); }

void LibraryPage::set_view(const std::string& view) {
    (view == "list" ? m_list_btn : m_grid_btn).set_active(true);
}

void LibraryPage::zoom(int steps) {
    m_scale.set_value(std::clamp(m_scale.get_value() + 20.0 * steps, 120.0, 420.0));
}

std::pair<int, int> LibraryPage::covers_shown() const {
    int shown = 0, tiles = 0;
    for (auto* child = m_flow.get_first_child(); child; child = child->get_next_sibling()) {
        ++tiles;
        const auto* flow_child = dynamic_cast<const Gtk::FlowBoxChild*>(child);
        const auto* tile = flow_child ? flow_child->get_child() : nullptr;
        const auto* art = tile ? dynamic_cast<const Gtk::Stack*>(tile->get_first_child()) : nullptr;
        if (art && art->get_visible_child_name() == "cover")
            ++shown;
    }
    return {shown, tiles};
}

void LibraryPage::refresh() {
    m_covers.forget();      // covers may have been added
    // Same games with new details (a cover, play time): stay where the
    // user had scrolled to once the rebuilt list has its size.
    const double grid_at = m_grid_scroll.get_vadjustment()->get_value();
    const double list_at = m_list_scroll.get_vadjustment()->get_value();
    rebuild();
    Glib::signal_idle().connect_once([this, grid_at, list_at] {
        m_grid_scroll.get_vadjustment()->set_value(grid_at);
        m_list_scroll.get_vadjustment()->set_value(list_at);
    });
}

// ── Building ──────────────────────────────────────────────────────────────

std::vector<const GameEntry*> LibraryPage::visible_games() const {
    std::vector<const GameEntry*> out;
    if (!m_library)
        return out;
    const std::string query = lower(m_search_entry.get_text());
    const guint type   = m_type_filter.get_selected();
    const guint region = m_region_filter.get_selected();
    static const char kRegionChar[] = {0, 'J', 'U', 'E'};

    for (const auto& g : m_library->games()) {
        static const char* const kTypeSystem[] = {nullptr, "Dreamcast", nullptr, "Naomi",
                                                  "Naomi 2", "Atomiswave", "System SP"};
        if (type == 2 && !g.is_arcade()) continue;
        if (type < std::size(kTypeSystem) && kTypeSystem[type] && g.system != kTypeSystem[type])
            continue;
        if (region >= 1 && region <= 3 && g.region.find(kRegionChar[region]) == std::string::npos)
            continue;
        if (!query.empty() &&
            lower(g.title).find(query) == std::string::npos &&
            lower(g.product_id).find(query) == std::string::npos &&
            lower(g.path.string()).find(query) == std::string::npos)
            continue;
        out.push_back(&g);
    }

    // One entry per multi-disc game: keep the lowest-numbered disc.
    if (m_config.library.merge_discs) {
        std::map<std::string, const GameEntry*> first;
        for (const GameEntry* g : out)
            if (!g->set_key.empty()) {
                auto& slot = first[g->set_key];
                if (!slot || g->disc < slot->disc) slot = g;
            }
        std::erase_if(out, [&](const GameEntry* g) {
            return !g->set_key.empty() && first[g->set_key] != g;
        });
    }

    const std::string key = m_config.library.sort_key;
    const auto stat = [this](const GameEntry* g) { return m_stats.get(m_game_id_for(g->path)); };
    std::stable_sort(out.begin(), out.end(), [&](const GameEntry* a, const GameEntry* b) {
        if (key == "region") return a->region < b->region;
        if (key == "format") return a->format < b->format;
        if (key == "size")   return a->size < b->size;
        if (key == "played") return stat(a).seconds < stat(b).seconds;
        if (key == "last")   return stat(a).last < stat(b).last;
        return lower(a->title) < lower(b->title);
    });
    if (m_config.library.descending)
        std::reverse(out.begin(), out.end());
    return out;
}

std::string LibraryPage::display_title(const GameEntry& g) const {
    return disc_count(g) > 1 ? g.set_title : g.title;
}

int LibraryPage::disc_count(const GameEntry& g) const {
    if (!m_config.library.merge_discs || g.set_key.empty())
        return 1;
    const auto it = m_set_counts.find(g.set_key);
    return it == m_set_counts.end() ? 1 : it->second;
}

void LibraryPage::rebuild() {
    if (m_building)
        return;
    m_building = true;
    m_set_counts.clear();
    if (m_library)
        for (const auto& g : m_library->games())
            if (!g.set_key.empty()) ++m_set_counts[g.set_key];

    const auto games = visible_games();
    const std::size_t total = m_library ? m_library->games().size() : 0;
    m_count.set_text(std::to_string(games.size()) + " / " + std::to_string(total));
    m_scale.set_sensitive(m_config.library.view == "grid");

    if (total == 0) {
        m_inner_stack.set_visible_child("empty");
    } else if (m_config.library.view == "list") {
        rebuild_list(games);
        m_inner_stack.set_visible_child("list");
    } else {
        rebuild_grid(games);
        m_inner_stack.set_visible_child("grid");
    }
    m_building = false;
}

void LibraryPage::rebuild_list(const std::vector<const GameEntry*>& games) {
    m_store->remove_all();
    for (const GameEntry* g : games)
    {
        GameEntry shown = *g;
        if (const int discs = disc_count(*g); discs > 1) {
            shown.title  = g->set_title;
            shown.format = g->format + " ×" + std::to_string(discs);
        }
        m_store->append(GameItem::create(shown, m_stats.get(m_game_id_for(g->path))));
    }
}

void LibraryPage::rebuild_grid(const std::vector<const GameEntry*>& games) {
    while (auto* child = m_flow.get_first_child())
        m_flow.remove(*child);

    const int  size = std::clamp(m_config.library.cover_size, 120, 420);
    const int  px   = size * std::max(1, get_scale_factor());
    const unsigned generation = ++m_grid_generation;
    m_covers.set_box_art(m_config.library.box_covers);

    for (const GameEntry* gp : games) {
        const GameEntry& game = *gp;
        auto* tile = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 6);
        tile->set_margin(4);
        tile->add_css_class("ds-game-tile");
        tile->set_tooltip_text(game.disc_title.empty()
            ? game.path.filename().string()
            : game.disc_title + "\n" + game.path.filename().string());

        // Cover art over a placeholder disc icon.
        auto* art = Gtk::make_managed<Gtk::Stack>();
        art->set_size_request(size, size);
        art->set_halign(Gtk::Align::CENTER);
        auto* placeholder = Gtk::make_managed<Gtk::Image>();
        placeholder->set_from_icon_name(game.is_arcade() ? "applications-games-symbolic"
                                                         : "media-optical-symbolic");
        placeholder->set_pixel_size(size / 2);
        placeholder->add_css_class("dim-label");
        auto* picture = Gtk::make_managed<Gtk::Picture>();
        picture->set_content_fit(Gtk::ContentFit::CONTAIN);
        picture->set_can_shrink(true);
        picture->set_size_request(size, size);
        art->add(*placeholder, "placeholder");
        art->add(*picture, "cover");
        tile->append(*art);
        if (const auto file = m_covers.path_for(game); !file.empty()) {
            // The picture made last time shows at once, even at another size,
            // while the right one is made.
            bool exact = false;
            if (const auto texture = m_covers.lookup(file, px, exact)) {
                picture->set_paintable(texture);
                art->set_visible_child("cover");
            }
            if (!exact)
                m_covers.request(file, px, [this, generation, picture, art](
                                               const Glib::RefPtr<Gdk::Texture>& texture) {
                    if (generation != m_grid_generation)
                        return;             // that tile is gone
                    picture->set_paintable(texture);
                    art->set_visible_child("cover");
                });
        }

        auto* lbl = Gtk::make_managed<Gtk::Label>(display_title(game));
        lbl->set_wrap(true);
        lbl->set_wrap_mode(Pango::WrapMode::WORD_CHAR);
        lbl->set_justify(Gtk::Justification::CENTER);
        lbl->set_lines(2);
        lbl->set_ellipsize(Pango::EllipsizeMode::END);
        lbl->set_max_width_chars(std::max(12, size / 10));
        lbl->add_css_class("ds-game-title-label");
        tile->append(*lbl);

        auto* system = Gtk::make_managed<Gtk::Label>(game.system);
        system->set_css_classes({"ds-system-pill", system_css_class(game.system)});
        system->set_halign(Gtk::Align::CENTER);
        tile->append(*system);

        // Arcade sets have no region; their set name identifies the dump.
        std::string badge = game.is_arcade() ? game.product_id : region_name(game.region);
        if (!badge.empty()) badge += " · ";
        badge += game.format;
        if (const int discs = disc_count(game); discs > 1)
            badge += " · " + std::to_string(discs) + " discs";
        auto* meta = Gtk::make_managed<Gtk::Label>(badge);
        meta->add_css_class("ds-game-meta-label");
        tile->append(*meta);

        auto* child = Gtk::make_managed<Gtk::FlowBoxChild>();
        child->set_halign(Gtk::Align::CENTER);
        child->set_child(*tile);
        child->set_name(game.path.string());     // path for launch / menu

        auto click = Gtk::GestureClick::create();
        click->set_button(GDK_BUTTON_SECONDARY);
        const std::string path = game.path.string();
        click->signal_pressed().connect([this, child, path](int, double x, double y) {
            m_flow.select_child(*child);
            show_context_menu(*child, x, y, path);
        });
        child->add_controller(click);
        m_flow.append(*child);
    }
}

void LibraryPage::show_context_menu(Gtk::Widget& anchor, double x, double y,
                                    const std::string& path) {
    if (path.empty())
        return;
    m_menu_path = path;
    double px = 0, py = 0;
    anchor.translate_coordinates(*this, x, y, px, py);
    m_menu.set_pointing_to(Gdk::Rectangle(static_cast<int>(px), static_cast<int>(py), 1, 1));
    m_menu.popup();
}

} // namespace ds::frontend
