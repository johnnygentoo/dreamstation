// SPDX-License-Identifier: GPL-3.0-or-later
#include "vmu_manager.hpp"
#include "alert.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <set>

namespace ds::frontend {

namespace fs = std::filesystem;

namespace {

std::vector<std::uint8_t> read_file(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

bool write_file(const fs::path& path, const std::uint8_t* data, std::size_t size) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(size));
    return out.good();
}

std::string lower(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string upper(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

// A file in `dir` whose name matches `name` ignoring case.
fs::path find_ignoring_case(const fs::path& dir, const std::string& name) {
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(dir, ec))
        if (lower(e.path().filename().string()) == lower(name))
            return e.path();
    return {};
}

// Pixel art stays sharp: scale RGBA up by whole pixels before display.
Glib::RefPtr<Gdk::Texture> texture(const std::vector<std::uint8_t>& rgba, int w, int h, int scale) {
    const int sw = w * scale, sh = h * scale;
    auto* bytes = static_cast<std::uint8_t*>(g_malloc(static_cast<gsize>(sw) * static_cast<gsize>(sh) * 4));
    for (int y = 0; y < sh; ++y)
        for (int x = 0; x < sw; ++x)
            std::copy_n(rgba.data() + (static_cast<std::size_t>(y / scale) * static_cast<std::size_t>(w) +
                                       static_cast<std::size_t>(x / scale)) * 4,
                        4, bytes + (static_cast<std::size_t>(y) * static_cast<std::size_t>(sw) +
                                    static_cast<std::size_t>(x)) * 4);
    GBytes* gb = g_bytes_new_take(bytes, static_cast<gsize>(sw) * static_cast<gsize>(sh) * 4);
    GdkTexture* t = gdk_memory_texture_new(sw, sh, GDK_MEMORY_R8G8B8A8, gb,
                                           static_cast<gsize>(sw) * 4);
    g_bytes_unref(gb);
    return Glib::wrap(t);
}

// Frame index of an animation `speed` (1/30 s units per frame) at `ticks`.
std::size_t frame_at(unsigned ticks, int speed, std::size_t frames) {
    if (frames < 2)
        return 0;
    return (ticks / static_cast<unsigned>(std::max(speed, 4))) % frames;
}

// A VMS resource name: at most 8 letters and digits, upper case.
std::string resource_name(const std::string& card_name) {
    std::string out;
    for (char c : card_name)
        if (std::isalnum(static_cast<unsigned char>(c)) && out.size() < 8)
            out += static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return out.empty() ? "VMUFILE" : out;
}

// A file name safe on any file system.
std::string safe_name(const std::string& card_name) {
    std::string out;
    for (char c : card_name)
        out += (std::isalnum(static_cast<unsigned char>(c)) || c == '.' || c == '-' || c == '_') ? c : '_';
    return out.empty() ? "VMUFILE" : out;
}

// "vmu_save_A1.bin" -> "Controller A, slot 1"
std::string card_label(const fs::path& path) {
    const std::string name = path.filename().string();
    if (name.size() >= 14 && name.rfind("vmu_save_", 0) == 0 && std::isupper(static_cast<unsigned char>(name[9])) &&
        std::isdigit(static_cast<unsigned char>(name[10])))
        return std::string("Controller ") + name[9] + ", slot " + name[10] + "   (" + name + ")";
    return name + "   (" + path.parent_path().filename().string() + ")";
}

} // namespace

VmuManager::VmuManager(std::vector<fs::path> vmu_dirs, std::function<bool()> game_running,
                       std::function<void(const fs::path&)> play)
    : m_dirs(std::move(vmu_dirs)), m_game_running(std::move(game_running)), m_play(std::move(play)) {
    set_title("VMU Manager");
    set_default_size(1180, 760);
    add_css_class("ds-settings");
    m_root.set_margin(14);

    m_left.other = &m_right;
    m_right.other = &m_left;
    auto* panes = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 14);
    panes->set_homogeneous(true);
    panes->set_vexpand(true);
    build_pane(m_left, "go-next-symbolic", "go-last-symbolic");
    build_pane(m_right, "go-previous-symbolic", "go-first-symbolic");
    panes->append(m_left.box);
    panes->append(m_right.box);
    m_root.append(*panes);
    m_root.append(build_details());

    m_message.set_xalign(0.f);
    m_message.set_wrap(true);
    m_message.add_css_class("ds-settings-help");
    m_message.set_text("Select saves to copy or move to the other card (or drag them across), export, "
                       "protect or delete them. Drop .dci, .vms/.vmi or card images onto a card to "
                       "import. Changes are written when you press Save.");
    m_root.append(m_message);
    set_child(m_root);

    scan_cards();
    if (!m_cards.empty()) load_card(m_left, 0);
    if (m_cards.size() > 1) load_card(m_right, 1);

    // Icons animate at the Dreamcast's 30 frames a second.
    m_timer = Glib::signal_timeout().connect(sigc::mem_fun(*this, &VmuManager::tick), 33);
}

VmuManager::~VmuManager() { m_timer.disconnect(); }

void VmuManager::test_import(int side, const std::vector<fs::path>& paths) {
    import_paths(side ? m_right : m_left, paths);
}
void VmuManager::test_select_all(int side) { (side ? m_right : m_left).list.select_all(); }
void VmuManager::test_transfer(int side, bool move) {
    Pane& from = side ? m_right : m_left;
    transfer(from, *from.other, move);
}
void VmuManager::test_save(int side) { save_card(side ? m_right : m_left); }

void VmuManager::notify(const std::string& text) { m_message.set_text(text); }

bool VmuManager::engine_card(const fs::path& path) const {
    const std::string p = path.lexically_normal().string();
    for (const auto& dir : m_dirs) {
        const std::string d = dir.lexically_normal().string();
        if (p.rfind(d, 0) == 0)
            return true;
    }
    return false;
}

bool VmuManager::can_write(const Pane& pane) {
    if (engine_card(pane.path) && m_game_running && m_game_running()) {
        notify("A game is running and has this VMU plugged in. Close the game before saving "
               "changes to it.");
        return false;
    }
    return true;
}

// ── Card list ─────────────────────────────────────────────────────────────

void VmuManager::scan_cards() {
    std::vector<fs::path> extra;   // cards opened from elsewhere stay listed
    for (const auto& c : m_cards)
        if (!engine_card(c))
            extra.push_back(c);
    m_cards.clear();
    std::error_code ec;
    for (const auto& dir : m_dirs)
        for (const auto& e : fs::recursive_directory_iterator(dir, ec)) {
            if (!e.is_regular_file(ec) || e.file_size(ec) != VmuImage::kImageSize)
                continue;
            const std::string name = e.path().filename().string();
            if (name.rfind("vmu_save", 0) == 0 || e.path().extension() == ".vmu" ||
                e.path().extension() == ".dcm")
                m_cards.push_back(e.path());
        }
    std::sort(m_cards.begin(), m_cards.end());
    for (const auto& c : extra)
        if (std::find(m_cards.begin(), m_cards.end(), c) == m_cards.end())
            m_cards.push_back(c);
    fill_chooser(m_left);
    fill_chooser(m_right);
}

void VmuManager::fill_chooser(Pane& pane) {
    m_loading = true;
    std::vector<Glib::ustring> items;
    for (const auto& c : m_cards)
        items.push_back(card_label(c));
    if (items.empty())
        items.push_back("No VMU images found");
    pane.paths = m_cards;
    pane.chooser.set_model(Gtk::StringList::create(items));
    const auto it = std::find(m_cards.begin(), m_cards.end(), pane.path);
    if (it != m_cards.end())
        pane.chooser.set_selected(static_cast<guint>(it - m_cards.begin()));
    m_loading = false;
}

void VmuManager::add_card_path(const fs::path& path) {
    if (std::find(m_cards.begin(), m_cards.end(), path) == m_cards.end())
        m_cards.push_back(path);
    fill_chooser(m_left);
    fill_chooser(m_right);
}

// ── Building ──────────────────────────────────────────────────────────────

void VmuManager::build_pane(Pane& pane, const char* copy_icon, const char* move_icon) {
    auto* top = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 6);
    pane.chooser.set_hexpand(true);
    pane.chooser.property_selected().signal_changed().connect([this, &pane] {
        if (m_loading)
            return;
        const auto index = pane.chooser.get_selected();
        confirm_discard(pane, [this, &pane, index] { load_card(pane, index); });
    });
    top->append(pane.chooser);
    const auto tool = [top](const char* icon, const char* tip, std::function<void()> fn) {
        auto* b = Gtk::make_managed<Gtk::Button>();
        b->set_icon_name(icon);
        b->set_tooltip_text(tip);
        b->signal_clicked().connect(std::move(fn));
        top->append(*b);
    };
    tool("document-open-symbolic", "Open a card image from anywhere (.bin, .vmu, .dcm)",
         [this, &pane] { open_dialog(pane); });
    tool("document-new-symbolic", "Create an empty card image", [this, &pane] { new_card(pane); });
    tool("document-save-as-symbolic", "Save this card under another name or as a Nexus .dcm",
         [this, &pane] { save_as(pane); });
    tool("edit-clear-all-symbolic", "Format: erase every file on this card",
         [this, &pane] { format_card(pane); });
    pane.box.append(*top);

    pane.list.set_selection_mode(Gtk::SelectionMode::MULTIPLE);
    pane.list.add_css_class("boxed-list");
    pane.list.signal_selected_rows_changed().connect([this, &pane] { show_details(pane); });
    pane.list.signal_row_activated().connect([this, &pane](Gtk::ListBoxRow*) {
        transfer(pane, *pane.other, false);
    });
    // Saves dragged from the other card, or files from a file manager.
    auto drop = Gtk::DropTarget::create(G_TYPE_INVALID, Gdk::DragAction::COPY);
    drop->set_gtypes({GDK_TYPE_FILE_LIST, G_TYPE_STRING});
    drop->signal_drop().connect([this, &pane](const Glib::ValueBase& value, double, double) {
        const GValue* gv = value.gobj();
        if (G_VALUE_HOLDS(gv, GDK_TYPE_FILE_LIST)) {
            std::vector<fs::path> paths;
            GSList* files = gdk_file_list_get_files(static_cast<GdkFileList*>(g_value_get_boxed(gv)));
            for (GSList* l = files; l; l = l->next)
                if (char* p = g_file_get_path(G_FILE(l->data))) {
                    paths.emplace_back(p);
                    g_free(p);
                }
            g_slist_free(files);
            import_paths(pane, paths);
            return true;
        }
        if (G_VALUE_HOLDS_STRING(gv) && m_drag_from && m_drag_from != &pane) {
            transfer(*m_drag_from, pane, false);
            m_drag_from = nullptr;
            return true;
        }
        return false;
    }, false);
    pane.list.add_controller(drop);

    auto* scroll = Gtk::make_managed<Gtk::ScrolledWindow>();
    scroll->set_child(pane.list);
    scroll->set_vexpand(true);
    pane.box.append(*scroll);

    pane.status.set_xalign(0.f);
    pane.box.append(pane.status);

    auto* buttons = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 6);
    const auto button = [buttons](const char* label, const char* icon, const char* tip,
                                  std::function<void()> fn) {
        auto* b = Gtk::make_managed<Gtk::Button>();
        if (label) b->set_label(label);
        if (icon) b->set_icon_name(icon);
        b->set_tooltip_text(tip);
        b->signal_clicked().connect(std::move(fn));
        buttons->append(*b);
        return b;
    };
    button(nullptr, copy_icon, "Copy the selected saves to the other card",
           [this, &pane] { transfer(pane, *pane.other, false); });
    button(nullptr, move_icon, "Move the selected saves to the other card",
           [this, &pane] { transfer(pane, *pane.other, true); });
    button("Delete", nullptr, "Delete the selected saves", [this, &pane] { delete_selected(pane); });
    button(nullptr, "changes-prevent-symbolic", "Copy protection on or off for the selected saves",
           [this, &pane] { toggle_protect(pane); });

    // Export: DCI or VMS + VMI.
    auto* export_btn = Gtk::make_managed<Gtk::MenuButton>();
    export_btn->set_label("Export");
    export_btn->set_tooltip_text("Write the selected saves to files");
    auto* pop = Gtk::make_managed<Gtk::Popover>();
    auto* pop_box = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 2);
    for (const bool vms : {false, true}) {
        auto* b = Gtk::make_managed<Gtk::Button>(vms ? "As VMS + VMI…" : "As DCI…");
        b->add_css_class("flat");
        b->signal_clicked().connect([this, &pane, pop, vms] { pop->popdown(); export_selected(pane, vms); });
        pop_box->append(*b);
    }
    pop->set_child(*pop_box);
    export_btn->set_popover(*pop);
    buttons->append(*export_btn);
    button("Import…", nullptr, "Add saves from .dci, .vms/.vmi or card image files",
           [this, &pane] { import_dialog(pane); });

    pane.play.set_label("Play");
    pane.play.set_icon_name("media-playback-start-symbolic");
    pane.play.set_tooltip_text("Play this card's mini-game on the VMU");
    pane.play.set_visible(static_cast<bool>(m_play));
    pane.play.signal_clicked().connect([this, &pane] {
        if (!m_play || !pane.loaded)
            return;
        if (pane.modified) {
            notify("Save this card first: the mini-game runs from the card on disk.");
            return;
        }
        m_play(pane.path);
    });
    buttons->append(pane.play);

    auto* spacer = Gtk::make_managed<Gtk::Box>();
    spacer->set_hexpand(true);
    buttons->append(*spacer);
    pane.save.set_sensitive(false);
    pane.save.signal_clicked().connect([this, &pane] { save_card(pane); });
    buttons->append(pane.save);
    pane.box.append(*buttons);
}

Gtk::Widget& VmuManager::build_details() {
    m_details.add_css_class("card");
    m_details.set_margin_top(2);
    m_detail_icon.set_size_request(96, 96);
    m_detail_icon.set_can_shrink(false);
    m_detail_icon.set_valign(Gtk::Align::CENTER);
    m_detail_icon.set_margin(10);
    m_detail_eyecatch.set_size_request(144, 112);
    m_detail_eyecatch.set_can_shrink(false);
    m_detail_eyecatch.set_valign(Gtk::Align::CENTER);
    m_detail_text.set_xalign(0.f);
    m_detail_text.set_valign(Gtk::Align::CENTER);
    m_detail_text.set_wrap(true);
    m_detail_text.set_selectable(true);
    m_detail_text.set_hexpand(true);
    m_detail_text.set_margin(8);
    m_details.append(m_detail_icon);
    m_details.append(m_detail_text);
    m_details.append(m_detail_eyecatch);
    m_detail_text.set_text("No save selected");
    return m_details;
}

// ── Loading and showing ───────────────────────────────────────────────────

void VmuManager::load_card(Pane& pane, std::size_t index) {
    if (index >= pane.paths.size())
        return;
    open_card(pane, pane.paths[index]);
}

void VmuManager::open_card(Pane& pane, const fs::path& path) {
    pane.path     = path;
    pane.loaded   = false;
    pane.modified = false;
    if (auto image = VmuImage::load(path)) {
        pane.image  = *image;
        pane.loaded = image->valid();
    }
    add_card_path(path);
    refresh(pane);
}

void VmuManager::mark_modified(Pane& pane) {
    pane.modified = true;
    refresh(pane);
}

void VmuManager::refresh(Pane& pane) {
    while (auto* child = pane.list.get_first_child())
        pane.list.remove(*child);
    pane.rows.clear();
    pane.save.set_sensitive(pane.modified);
    // Highlighted only when there is something to save.
    if (pane.modified)
        pane.save.add_css_class("suggested-action");
    else
        pane.save.remove_css_class("suggested-action");
    pane.play.set_sensitive(false);
    if (!pane.loaded) {
        pane.status.set_text(pane.path.empty() ? "No card selected"
                                               : "Not a formatted VMU image (Format to use it)");
        return;
    }
    for (VmuImage::File& f : pane.image.files()) {
        Row row;
        row.file = f;
        auto* box = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 10);
        box->set_margin(6);
        row.icon = Gtk::make_managed<Gtk::Picture>();
        row.icon->set_size_request(64, 64);
        row.icon->set_can_shrink(false);
        for (const auto& frame : f.icons)
            row.frames.push_back(texture(frame, 32, 32, 2));
        if (!row.frames.empty())
            row.icon->set_paintable(row.frames.front());
        auto* text = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 2);
        text->set_hexpand(true);
        auto* title = Gtk::make_managed<Gtk::Label>(
            f.long_description.empty() ? (f.description.empty() ? f.name : f.description)
                                       : f.long_description);
        title->set_xalign(0.f);
        title->set_ellipsize(Pango::EllipsizeMode::END);
        std::string sub = f.name + "  ·  " + f.timestamp;
        if (f.game) sub += "  ·  VMU game";
        if (f.copy_protected) sub += "  ·  protected";
        auto* subl = Gtk::make_managed<Gtk::Label>(sub);
        subl->set_xalign(0.f);
        subl->set_ellipsize(Pango::EllipsizeMode::END);
        subl->add_css_class("dim-label");
        text->append(*title);
        text->append(*subl);
        auto* blocks = Gtk::make_managed<Gtk::Label>(std::to_string(f.blocks) + " blocks");
        box->append(*row.icon);
        box->append(*text);
        box->append(*blocks);

        auto* lrow = Gtk::make_managed<Gtk::ListBoxRow>();
        lrow->set_child(*box);
        // Drag a save onto the other card to copy it there.
        auto drag = Gtk::DragSource::create();
        drag->set_actions(Gdk::DragAction::COPY);
        drag->signal_prepare().connect([this, &pane, lrow](double, double) {
            m_drag_from = &pane;
            if (!lrow->is_selected()) {
                pane.list.unselect_all();
                pane.list.select_row(*lrow);
            }
            Glib::Value<Glib::ustring> v;
            v.init(v.value_type());
            v.set("dreamstation-vmu-save");
            return Gdk::ContentProvider::create(v);
        }, false);
        lrow->add_controller(drag);
        pane.list.append(*lrow);
        if (f.game) pane.play.set_sensitive(true);
        pane.rows.push_back(std::move(row));
    }
    const int free = pane.image.free_blocks();
    pane.status.set_text(std::to_string(pane.rows.size()) + " files  ·  " + std::to_string(free) +
                         " of " + std::to_string(VmuImage::kUserBlocks) + " blocks free" +
                         (pane.modified ? "  ·  modified, not saved" : ""));
}

std::vector<const VmuImage::File*> VmuManager::selected(const Pane& pane) const {
    std::vector<const VmuImage::File*> out;
    for (const Gtk::ListBoxRow* row : pane.list.get_selected_rows()) {
        const auto index = static_cast<std::size_t>(row->get_index());
        if (index < pane.rows.size())
            out.push_back(&pane.rows[index].file);
    }
    std::sort(out.begin(), out.end(), [](auto* a, auto* b) { return a->dir_index < b->dir_index; });
    return out;
}

void VmuManager::show_details(const Pane& pane) {
    const auto files = selected(pane);
    m_detail_frames.clear();
    m_detail_icon.set_paintable({});
    m_detail_eyecatch.set_paintable({});
    if (files.empty()) {
        m_detail_text.set_text("No save selected");
        return;
    }
    if (files.size() > 1) {
        int blocks = 0;
        for (auto* f : files) blocks += f->blocks;
        m_detail_text.set_text(std::to_string(files.size()) + " saves selected, " +
                               std::to_string(blocks) + " blocks");
        return;
    }
    const VmuImage::File& f = *files.front();
    for (const auto& frame : f.icons)
        m_detail_frames.push_back(texture(frame, 32, 32, 3));
    m_detail_speed = f.icon_speed;
    if (!m_detail_frames.empty())
        m_detail_icon.set_paintable(m_detail_frames.front());
    if (!f.eyecatch.empty())
        m_detail_eyecatch.set_paintable(texture(f.eyecatch, VmuImage::File::kEyecatchWidth,
                                                VmuImage::File::kEyecatchHeight, 2));
    std::string text = "<b>" + Glib::Markup::escape_text(f.long_description.empty() ? f.name : f.long_description) + "</b>\n";
    if (!f.description.empty())
        text += Glib::Markup::escape_text(f.description) + "\n";
    text += "File " + Glib::Markup::escape_text(f.name) + "  ·  " + (f.game ? "VMU mini-game" : "save data") +
            "  ·  " + std::to_string(f.blocks) + " blocks (" + std::to_string(f.blocks / 2) + " KB)\n";
    text += "Created " + f.timestamp;
    if (!f.app_id.empty())
        text += "  ·  made by " + Glib::Markup::escape_text(f.app_id);
    if (f.copy_protected)
        text += "\nCopy protected: the Dreamcast refuses to copy it; DreamStation copies it anyway";
    m_detail_text.set_markup(text);
}

bool VmuManager::tick() {
    ++m_ticks;
    for (Pane* pane : {&m_left, &m_right})
        for (auto& row : pane->rows)
            if (row.frames.size() > 1)
                row.icon->set_paintable(row.frames[frame_at(m_ticks, row.file.icon_speed, row.frames.size())]);
    if (m_detail_frames.size() > 1)
        m_detail_icon.set_paintable(m_detail_frames[frame_at(m_ticks, m_detail_speed, m_detail_frames.size())]);
    return true;
}

// ── File operations ───────────────────────────────────────────────────────

void VmuManager::transfer(Pane& from, Pane& to, bool move) {
    const auto files = selected(from);
    if (files.empty()) { notify("Select the saves to copy first."); return; }
    if (!to.loaded) { notify("Choose a card on the other side first."); return; }
    if (from.path == to.path) { notify("Both sides show the same card."); return; }
    std::vector<VmuImage::Incoming> incoming;
    for (const auto* f : files)
        incoming.push_back(from.image.take(*f));
    if (!move) {
        add_incoming(to, std::move(incoming), {});
        return;
    }
    // Moving: delete from the source what arrived at the destination.
    add_incoming(to, std::move(incoming), {}, [this, &from](const std::vector<std::string>& names) {
        for (const auto& name : names)
            if (const auto f = from.image.find(name))
                from.image.remove(*f);
        if (!names.empty())
            mark_modified(from);
    });
}

void VmuManager::delete_selected(Pane& pane) {
    const auto files = selected(pane);
    if (files.empty()) { notify("Select the saves to delete first."); return; }
    std::vector<VmuImage::File> doomed;
    for (const auto* f : files) doomed.push_back(*f);
    for (const auto& f : doomed)
        if (const auto now = pane.image.find(f.name))
            pane.image.remove(*now);
    mark_modified(pane);
    notify("Deleted " + std::to_string(doomed.size()) + " save(s). Press Save to make it permanent, "
           "or choose the card again to undo.");
}

void VmuManager::toggle_protect(Pane& pane) {
    const auto files = selected(pane);
    if (files.empty()) { notify("Select a save first."); return; }
    const bool on = !files.front()->copy_protected;
    std::vector<std::string> names;
    for (const auto* f : files) names.push_back(f->name);
    for (const auto& n : names)
        if (const auto f = pane.image.find(n))
            pane.image.set_copy_protected(*f, on);
    mark_modified(pane);
    notify(std::string("Copy protection ") + (on ? "on" : "off") + " for " +
           std::to_string(names.size()) + " save(s).");
}

void VmuManager::export_selected(Pane& pane, bool vms) {
    const auto files = selected(pane);
    if (files.empty()) { notify("Select the saves to export first."); return; }
    std::vector<VmuImage::File> chosen;
    for (const auto* f : files) chosen.push_back(*f);
    const VmuImage image = pane.image;

    // Writes one save into `dir` (VMS/VMI names are at most 8 characters).
    auto write_one = [image, vms](const VmuImage::File& f, const fs::path& dir,
                                  const std::string& base) -> bool {
        if (!vms) {
            const auto dci = image.export_dci(f);
            return write_file(dir / (base + ".DCI"), dci.data(), dci.size());
        }
        const auto data = image.export_vms(f);
        const auto vmi  = image.export_vmi(f, base);
        return write_file(dir / (base + ".VMS"), data.data(), data.size()) &&
               write_file(dir / (base + ".VMI"), vmi.data(), vmi.size());
    };

    if (chosen.size() == 1) {
        const auto& f = chosen.front();
        const std::string base = vms ? resource_name(f.name) : safe_name(f.name);
        auto dlg = Gtk::FileDialog::create();
        dlg->set_title(vms ? "Export As VMS + VMI" : "Export As DCI");
        dlg->set_initial_name(base + (vms ? ".VMS" : ".DCI"));
        dlg->save(*this, [this, dlg, f, vms, write_one](const Glib::RefPtr<Gio::AsyncResult>& res) {
            try {
                const fs::path path = dlg->save_finish(res)->get_path();
                // The VMI names its VMS, so the pair shares a short name.
                const std::string stem = vms ? resource_name(path.stem().string()) : path.stem().string();
                if (write_one(f, path.parent_path(), stem))
                    notify("Exported " + f.name + " to " + (path.parent_path() / stem).string() +
                           (vms ? ".VMS + .VMI" : ".DCI"));
                else
                    notify("Could not write to " + path.parent_path().string());
            } catch (...) {}
        });
        return;
    }
    auto dlg = Gtk::FileDialog::create();
    dlg->set_title("Export Saves To Folder");
    dlg->select_folder(*this, [this, dlg, chosen, vms, write_one](const Glib::RefPtr<Gio::AsyncResult>& res) {
        try {
            const fs::path dir = dlg->select_folder_finish(res)->get_path();
            std::set<std::string> used;
            int written = 0;
            for (const auto& f : chosen) {
                std::string base = vms ? resource_name(f.name) : safe_name(f.name);
                for (int n = 2; used.count(upper(base)); ++n)
                    base = vms ? base.substr(0, 7) + std::to_string(n % 10) : safe_name(f.name) + "_" + std::to_string(n);
                used.insert(upper(base));
                written += write_one(f, dir, base);
            }
            notify("Exported " + std::to_string(written) + " of " + std::to_string(chosen.size()) +
                   " saves to " + dir.string());
        } catch (...) {}
    });
}

void VmuManager::import_dialog(Pane& pane) {
    if (!pane.loaded) { notify("Choose a card first."); return; }
    auto dlg = Gtk::FileDialog::create();
    dlg->set_title("Import VMU Saves");
    auto filters = Gio::ListStore<Gtk::FileFilter>::create();
    auto all = Gtk::FileFilter::create();
    all->set_name("VMU saves and cards (DCI, VMS/VMI, BIN, VMU, DCM)");
    for (const char* p : {"*.dci", "*.DCI", "*.vms", "*.VMS", "*.vmi", "*.VMI", "*.bin", "*.BIN",
                          "*.vmu", "*.VMU", "*.dcm", "*.DCM"})
        all->add_pattern(p);
    filters->append(all);
    dlg->set_filters(filters);
    dlg->open_multiple(*this, [this, dlg, &pane](const Glib::RefPtr<Gio::AsyncResult>& res) {
        try {
            std::vector<fs::path> paths;
            for (const auto& f : dlg->open_multiple_finish(res))
                paths.emplace_back(f->get_path());
            import_paths(pane, paths);
        } catch (...) {}
    });
}

void VmuManager::import_paths(Pane& pane, const std::vector<fs::path>& paths) {
    if (!pane.loaded) { notify("Choose a card first."); return; }
    std::vector<VmuImage::Incoming> incoming;
    std::string notes;
    std::set<std::string> used_vms;   // VMS files already brought in by their VMI
    const auto note = [&notes](const std::string& s) { notes += (notes.empty() ? "" : "; ") + s; };

    // VMIs first: each names its VMS.
    for (const auto& p : paths) {
        if (lower(p.extension().string()) != ".vmi")
            continue;
        const auto vmi = read_file(p);
        const std::string res = VmuImage::vmi_resource(vmi);
        fs::path vms_path = res.empty() ? fs::path{} : find_ignoring_case(p.parent_path(), res + ".vms");
        // Save sites often number both files ("88.VMI" + "88.VMS") whatever
        // the VMI says.
        if (vms_path.empty())
            vms_path = find_ignoring_case(p.parent_path(), p.stem().string() + ".vms");
        if (vms_path.empty()) {
            note(p.filename().string() + ": its " + (res.empty() ? "VMS" : res + ".VMS") + " was not found beside it");
            continue;
        }
        std::string error;
        if (auto in = VmuImage::parse_vms(read_file(vms_path), &vmi, res, error)) {
            incoming.push_back(*in);
            used_vms.insert(lower(vms_path.string()));
        } else {
            note(p.filename().string() + ": " + error);
        }
    }
    for (const auto& p : paths) {
        const std::string ext = lower(p.extension().string());
        std::error_code ec;
        const auto size = fs::file_size(p, ec);
        std::string error;
        if (ext == ".vmi")
            continue;
        if (ext == ".vms") {
            if (used_vms.count(lower(p.string())))
                continue;
            // A VMI beside it that names this VMS gives its card name and type;
            // failing that, one of the same name whose own VMS is not there.
            std::vector<std::uint8_t> vmi;
            std::error_code dec;
            for (const auto& e : fs::directory_iterator(p.parent_path(), dec))
                if (lower(e.path().extension().string()) == ".vmi") {
                    auto candidate = read_file(e.path());
                    const std::string res = VmuImage::vmi_resource(candidate);
                    if (lower(res) == lower(p.stem().string())) {
                        vmi = std::move(candidate);
                        break;
                    }
                    if (lower(e.path().stem().string()) == lower(p.stem().string()) && !res.empty() &&
                        find_ignoring_case(p.parent_path(), res + ".vms").empty())
                        vmi = std::move(candidate);
                }
            if (auto in = VmuImage::parse_vms(read_file(p), vmi.empty() ? nullptr : &vmi,
                                              p.stem().string(), error)) {
                if (vmi.empty())
                    note(p.filename().string() + ": no VMI, named " + in->name() + " as a " +
                         (in->game() ? "mini-game" : "save"));
                incoming.push_back(*in);
            } else {
                note(p.filename().string() + ": " + error);
            }
        } else if (!ec && size == VmuImage::kImageSize) {
            // A whole card: bring in all of its files.
            const auto card = VmuImage::load(p);
            if (!card || !card->valid()) {
                note(p.filename().string() + ": not a formatted VMU image");
                continue;
            }
            for (const auto& f : card->files())
                incoming.push_back(card->take(f));
        } else if (ext == ".dci") {
            if (auto in = VmuImage::parse_dci(read_file(p), error))
                incoming.push_back(*in);
            else
                note(p.filename().string() + ": " + error);
        } else {
            note(p.filename().string() + ": not a VMU save or card image");
        }
    }
    if (incoming.empty()) {
        notify(notes.empty() ? "Nothing to import." : notes);
        return;
    }
    add_incoming(pane, std::move(incoming), notes);
}

// Adds files to a card, asking first if any would replace a save there.
void VmuManager::add_incoming(Pane& pane, std::vector<VmuImage::Incoming> files, std::string notes,
                              std::function<void(const std::vector<std::string>&)> done) {
    std::vector<std::string> clashes;
    for (const auto& f : files)
        if (pane.image.find(f.name()))
            clashes.push_back(f.name());

    auto apply = [this, &pane, files, notes, done](bool replace) {
        int added = 0, replaced = 0;
        std::string errors;
        std::vector<std::string> landed;
        for (const auto& f : files) {
            const bool exists = pane.image.find(f.name()).has_value();
            if (exists && !replace)
                continue;
            std::string error;
            if (pane.image.add(f, replace, error)) {
                ++(exists ? replaced : added);
                landed.push_back(f.name());
            } else {
                errors += (errors.empty() ? "" : "; ") + f.name() + ": " + error;
            }
        }
        if (done)
            done(landed);
        if (added || replaced)
            mark_modified(pane);
        std::string text;
        if (added) text += "Added " + std::to_string(added) + " save(s). ";
        if (replaced) text += "Replaced " + std::to_string(replaced) + " save(s). ";
        if (added || replaced) text += "Press Save on that card to keep the changes. ";
        if (!errors.empty()) text += "Not added: " + errors + ". ";
        if (!notes.empty()) text += "(" + notes + ")";
        notify(text.empty() ? "Nothing was added." : text);
    };
    if (clashes.empty()) {
        apply(false);
        return;
    }
    std::string body = "This card already has ";
    for (std::size_t i = 0; i < clashes.size(); ++i)
        body += (i ? ", " : "") + clashes[i];
    body += ". Replace " + std::string(clashes.size() > 1 ? "them" : "it") +
            " with the new " + (clashes.size() > 1 ? "saves" : "save") + "?";
    alert(*this, "Replace Existing Saves?", body,
          {{"cancel", "Cancel"}, {"skip", "Keep Existing"}, {"replace", "Replace"}}, "replace", nullptr,
          [apply](const std::string& response) {
              if (response == "replace") apply(true);
              else if (response == "skip") apply(false);
          });
}

// ── Cards ─────────────────────────────────────────────────────────────────

void VmuManager::confirm_discard(Pane& pane, std::function<void()> then) {
    if (!pane.modified) {
        then();
        return;
    }
    alert(*this, "Discard Changes?",
          pane.path.filename().string() + " has changes that were not saved.",
          {{"cancel", "Cancel"}, {"discard", "Discard"}}, "discard", nullptr,
          [this, &pane, then](const std::string& response) {
              if (response == "discard") {
                  pane.modified = false;
                  then();
              } else {
                  fill_chooser(pane);   // put the chooser back on this card
              }
          });
}

void VmuManager::save_card(Pane& pane) {
    if (!pane.loaded || !can_write(pane))
        return;
    if (pane.image.save(pane.path)) {
        pane.modified = false;
        refresh(pane);
        notify("Saved " + pane.path.filename().string());
    } else {
        notify("Could not write " + pane.path.string());
    }
}

void VmuManager::save_as(Pane& pane) {
    if (!pane.loaded) { notify("Choose a card first."); return; }
    auto dlg = Gtk::FileDialog::create();
    dlg->set_title("Save Card As (.bin / .vmu raw, .dcm for Nexus)");
    dlg->set_initial_name(pane.path.stem().string() + "_copy.bin");
    dlg->save(*this, [this, dlg, &pane](const Glib::RefPtr<Gio::AsyncResult>& res) {
        try {
            const fs::path path = dlg->save_finish(res)->get_path();
            if (engine_card(path) && m_game_running && m_game_running()) {
                notify("A game is running and has the VMUs plugged in. Close it first.");
                return;
            }
            if (!pane.image.save(path)) {
                notify("Could not write " + path.string());
                return;
            }
            pane.path = path;
            pane.modified = false;
            add_card_path(path);
            refresh(pane);
            notify("Saved as " + path.string() +
                   (VmuImage::format_for(path) == VmuImage::CardFormat::Dcm ? " (Nexus DCM)" : ""));
        } catch (...) {}
    });
}

void VmuManager::open_dialog(Pane& pane) {
    auto dlg = Gtk::FileDialog::create();
    dlg->set_title("Open VMU Card Image");
    auto filters = Gio::ListStore<Gtk::FileFilter>::create();
    auto cards = Gtk::FileFilter::create();
    cards->set_name("VMU card images (.bin, .vmu, .dcm)");
    for (const char* p : {"*.bin", "*.BIN", "*.vmu", "*.VMU", "*.dcm", "*.DCM"})
        cards->add_pattern(p);
    filters->append(cards);
    dlg->set_filters(filters);
    dlg->open(*this, [this, dlg, &pane](const Glib::RefPtr<Gio::AsyncResult>& res) {
        try {
            const fs::path path = dlg->open_finish(res)->get_path();
            std::error_code ec;
            if (fs::file_size(path, ec) != VmuImage::kImageSize) {
                notify(path.filename().string() + " is not a 128 KB VMU image.");
                return;
            }
            confirm_discard(pane, [this, &pane, path] { open_card(pane, path); });
        } catch (...) {}
    });
}

void VmuManager::new_card(Pane& pane) {
    auto dlg = Gtk::FileDialog::create();
    dlg->set_title("Create VMU Image");
    dlg->set_initial_name("vmu_save_new.bin");
    if (!m_dirs.empty())
        dlg->set_initial_folder(Gio::File::create_for_path(m_dirs.front().string()));
    dlg->save(*this, [this, dlg, &pane](const Glib::RefPtr<Gio::AsyncResult>& res) {
        try {
            const fs::path path = dlg->save_finish(res)->get_path();
            if (!VmuImage::formatted().save(path)) {
                notify("Could not create " + path.string());
                return;
            }
            confirm_discard(pane, [this, &pane, path] {
                scan_cards();
                open_card(pane, path);
                notify("Created an empty VMU: " + path.filename().string());
            });
        } catch (...) {}
    });
}

void VmuManager::format_card(Pane& pane) {
    if (pane.path.empty()) { notify("Choose a card first."); return; }
    alert(*this, "Format This Card?",
          "Every file on " + pane.path.filename().string() + " will be erased when you press Save.",
          {{"cancel", "Cancel"}, {"format", "Format"}}, "cancel", nullptr,
          [this, &pane](const std::string& response) {
              if (response != "format")
                  return;
              pane.image  = VmuImage::formatted();
              pane.loaded = true;
              mark_modified(pane);
              notify("Card formatted — press Save to write it, or choose the card again to undo.");
          });
}

} // namespace ds::frontend
