#ifndef WHALEUI_HPP
#define WHALEUI_HPP

/* =========================================================================
 * WhaleUI C++ API (namespace whaleui).
 *
 * The C++ surface first: this header is the API a C++ program uses; the
 * exported C ABI (whaleui.h) is the underlying layer the wrapper calls
 * through to (and the stable boundary for other languages). Zero-cost by
 * intent: thin inline wrappers over the opaque handles, no virtuals, no
 * exceptions (project rule), std::string/std::vector only at the return
 * boundaries.
 *
 * Ownership mirrors the C contract:
 *   - Document is owned by its Window; Element/Node handles are valid while
 *     their document lives (copy them freely, they are pointers).
 *   - Query results are collected into std::vector<Element> here; the C
 *     snapshot list is destroyed immediately.
 *   - ComputedStyle owns a whaleui_style_declaration_t (freed on destruct).
 *   - Window/App destroy their C object on destruct (RAII, non-copyable).
 *
 * DOM semantics equal the C API (live handles, three-phase events,
 * snapshot lists). See doc/external/0*.md for the behavior reference.
 * ========================================================================= */

#include "whaleui.h"

#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace whaleui {

/* ---- forward / shared types ---- */

class Document;
class Element;
class Event;

inline std::string ToStr(const char* s) { return s ? std::string(s) : std::string(); }

/* ---- Event (transient: valid inside a listener only) ---- */

class Event {
public:
    Event() : h_(nullptr) {}
    explicit Event(whaleui_dom_event_t* h) : h_(h) {}

    const char* Type() const { return h_ ? whaleui_dom_event_type(h_) : nullptr; }
    Element Target() const;
    Element CurrentTarget() const;
    int Phase() const { return h_ ? whaleui_dom_event_phase(h_) : 0; }

    void PreventDefault() const { if (h_) whaleui_dom_event_prevent_default(h_); }
    bool DefaultPrevented() const { return h_ && whaleui_dom_event_default_prevented(h_); }
    void StopPropagation() const { if (h_) whaleui_dom_event_stop_propagation(h_); }

    int KeyCode() const { return h_ ? whaleui_dom_event_key_code(h_) : 0; }
    int MouseX() const { return h_ ? whaleui_dom_event_mouse_x(h_) : 0; }
    int MouseY() const { return h_ ? whaleui_dom_event_mouse_y(h_) : 0; }
    int MouseButton() const { return h_ ? whaleui_dom_event_mouse_button(h_) : 0; }
    float WheelDeltaX() const { return h_ ? whaleui_dom_event_wheel_delta_x(h_) : 0.0f; }
    float WheelDeltaY() const { return h_ ? whaleui_dom_event_wheel_delta_y(h_) : 0.0f; }

    /* modifier keys (KeyboardEvent.getModifierState) */
    int Mods() const { return h_ ? whaleui_dom_event_mods(h_) : 0; }
    bool CtrlKey() const { return h_ && whaleui_dom_event_ctrl_key(h_); }
    bool ShiftKey() const { return h_ && whaleui_dom_event_shift_key(h_); }
    bool AltKey() const { return h_ && whaleui_dom_event_alt_key(h_); }
    bool MetaKey() const { return h_ && whaleui_dom_event_meta_key(h_); }

    /* richer payloads */
    std::string Key() const { return ToStr(whaleui_dom_event_key(h_)); }
    std::string Code() const { return ToStr(whaleui_dom_event_code(h_)); }
    bool Repeat() const { return h_ && whaleui_dom_event_repeat(h_); }
    int Detail() const { return h_ ? whaleui_dom_event_detail(h_) : 0; }
    float MovementX() const { return h_ ? whaleui_dom_event_movement_x(h_) : 0.0f; }
    float MovementY() const { return h_ ? whaleui_dom_event_movement_y(h_) : 0.0f; }
    /* defined after Element (incomplete here) */
    Element RelatedTarget() const;
    /* drag & drop payload (NULL for other event types) */
    whaleui_data_transfer_t* DataTransfer() const {
        return h_ ? whaleui_dom_event_data_transfer(h_) : nullptr;
    }
    std::string ClipboardText() const {
        return ToStr(whaleui_dom_event_clipboard_text(h_));
    }
    bool SetClipboardText(const std::string& text) const {
        return h_ && whaleui_dom_event_set_clipboard_text(h_, text.c_str()) == 0;
    }

    whaleui_dom_event_t* handle() const { return h_; }

private:
    whaleui_dom_event_t* h_;
};

/* ---- Element (a live handle into its document) ---- */

class Element {
public:
    Element() : h_(nullptr) {}
    explicit Element(whaleui_dom_element_t* h) : h_(h) {}
    explicit operator bool() const { return h_ != nullptr; }
    bool operator==(const Element& o) const { return h_ == o.h_; }
    bool operator!=(const Element& o) const { return h_ != o.h_; }
    whaleui_dom_element_t* handle() const { return h_; }

    /* identity / content */
    std::string TagName() const { return ToStr(whaleui_dom_tag_name(h_)); }
    std::string Text() const { return ToStr(whaleui_dom_get_text(h_)); }
    void SetText(const std::string& text) const { whaleui_dom_set_text(h_, text.c_str()); }
    std::string InnerHTML() const { return ToStr(whaleui_dom_get_inner_html(h_)); }
    void SetInnerHTML(const std::string& html) const { whaleui_dom_set_inner_html(h_, html.c_str()); }
    std::string OuterHTML() const { return ToStr(whaleui_dom_get_outer_html(h_)); }
    void SetOuterHTML(const std::string& html) const { whaleui_dom_set_outer_html(h_, html.c_str()); }

    /* attributes */
    void SetAttribute(const std::string& name, const std::string& value) const {
        whaleui_dom_set_attribute(h_, name.c_str(), value.c_str());
    }
    std::string GetAttribute(const std::string& name) const {
        return ToStr(whaleui_dom_get_attribute(h_, name.c_str()));
    }
    bool HasAttribute(const std::string& name) const {
        return whaleui_dom_has_attribute(h_, name.c_str()) != 0;
    }
    bool RemoveAttribute(const std::string& name) const {
        return whaleui_dom_remove_attribute(h_, name.c_str()) == 0;
    }
    bool ToggleAttribute(const std::string& name, int force = -1) const {
        return whaleui_dom_toggle_attribute(h_, name.c_str(), force) > 0;
    }

    /* classList */
    bool ClassAdd(const std::string& cls) const { return whaleui_dom_class_add(h_, cls.c_str()) == 0; }
    bool ClassRemove(const std::string& cls) const { return whaleui_dom_class_remove(h_, cls.c_str()) == 0; }
    bool ClassToggle(const std::string& cls) const { return whaleui_dom_class_toggle(h_, cls.c_str()) != 0; }
    bool ClassContains(const std::string& cls) const { return whaleui_dom_class_contains(h_, cls.c_str()) != 0; }
    size_t ClassLength() const { return whaleui_dom_class_length(h_); }
    std::string ClassItem(size_t i) const {
        char buf[128];
        if (whaleui_dom_class_item(h_, i, buf, sizeof(buf)) != 0) return std::string();
        return std::string(buf);
    }
    bool ClassReplace(const std::string& old_cls, const std::string& new_cls) const {
        return whaleui_dom_class_replace(h_, old_cls.c_str(), new_cls.c_str()) != 0;
    }
    bool ClassToggleForce(const std::string& cls, int force) const {
        return whaleui_dom_class_toggle_force(h_, cls.c_str(), force) != 0;
    }

    /* dataset (camelCase <-> data-kebab, mirrors element.dataset.*) */
    bool DatasetSet(const std::string& key, const std::string& value) const {
        return whaleui_dom_dataset_set(h_, key.c_str(), value.c_str()) == 0;
    }
    std::string DatasetGet(const std::string& key) const {
        return ToStr(whaleui_dom_dataset_get(h_, key.c_str()));
    }
    bool DatasetHas(const std::string& key) const {
        return whaleui_dom_dataset_has(h_, key.c_str()) != 0;
    }
    bool DatasetRemove(const std::string& key) const {
        return whaleui_dom_dataset_remove(h_, key.c_str()) == 0;
    }

    /* inline style (CSSStyleDeclaration subset) */
    void SetStyle(const std::string& property, const std::string& value) const {
        whaleui_dom_set_style(h_, property.c_str(), value.c_str());
    }
    std::string GetStyle(const std::string& property) const {
        return ToStr(whaleui_dom_get_style(h_, property.c_str()));
    }
    std::string StyleCssText() const {
        char buf[1024];
        if (whaleui_dom_style_get_css_text(h_, buf, sizeof(buf)) != 0) return std::string();
        return std::string(buf);
    }
    void SetStyleCssText(const std::string& css) const {
        whaleui_dom_style_set_css_text(h_, css.c_str());
    }
    size_t StyleLength() const { return whaleui_dom_style_length(h_); }
    std::string StyleItem(size_t i) const {
        char buf[128];
        if (whaleui_dom_style_item(h_, i, buf, sizeof(buf)) != 0) return std::string();
        return std::string(buf);
    }

    /* form value (input/textarea/select) */
    std::string Value() const { return ToStr(whaleui_dom_get_value(h_)); }
    bool SetValue(const std::string& v) const { return whaleui_dom_set_value(h_, v.c_str()) == 0; }

    /* queries (descendant scope; std::vector replaces the snapshot list) */
    Element QuerySelector(const std::string& sel) const {
        return Element(whaleui_dom_element_query_selector(h_, sel.c_str()));
    }
    std::vector<Element> QuerySelectorAll(const std::string& sel) const {
        return Drain(whaleui_dom_element_query_selector_all(h_, sel.c_str()));
    }
    Element Closest(const std::string& sel) const { return Element(whaleui_dom_closest(h_, sel.c_str())); }
    bool Matches(const std::string& sel) const { return whaleui_dom_matches(h_, sel.c_str()) != 0; }

    /* traversal */
    Element Parent() const { return Element(whaleui_dom_parent(h_)); }
    Element ParentElement() const { return Element(whaleui_dom_parent_element(h_)); }
    Element FirstChild() const { return Element(whaleui_dom_first_child(h_)); }
    Element LastChild() const { return Element(whaleui_dom_last_child(h_)); }
    Element NextSibling() const { return Element(whaleui_dom_next_sibling(h_)); }
    Element PreviousSibling() const { return Element(whaleui_dom_previous_sibling(h_)); }
    Element FirstElementChild() const { return Element(whaleui_dom_first_element_child(h_)); }
    Element LastElementChild() const { return Element(whaleui_dom_last_element_child(h_)); }
    Element NextElementSibling() const { return Element(whaleui_dom_next_element_sibling(h_)); }
    Element PreviousElementSibling() const { return Element(whaleui_dom_previous_element_sibling(h_)); }
    std::vector<Element> Children() const { return Drain(whaleui_dom_children(h_)); }
    size_t ChildElementCount() const { return whaleui_dom_child_element_count(h_); }

    /* mutation */
    bool AppendChild(Element child) const { return whaleui_dom_append_child(h_, child.h_) == 0; }
    bool RemoveChild(Element child) const { return whaleui_dom_remove_child(h_, child.h_) == 0; }
    bool InsertBefore(Element node, Element ref) const {
        return whaleui_dom_insert_before(h_, node.h_, ref.h_) == 0;
    }
    bool ReplaceChild(Element node, Element old) const {
        return whaleui_dom_replace_child(h_, node.h_, old.h_) == 0;
    }
    bool Remove() const { return whaleui_dom_remove(h_) == 0; }
    Element Clone(bool deep) const { return Element(whaleui_dom_clone(h_, deep ? 1 : 0)); }
    bool Contains(Element other) const { return whaleui_dom_contains(h_, other.h_) != 0; }
    bool HasChildNodes() const { return whaleui_dom_has_child_nodes(h_) != 0; }
    bool IsConnected() const { return whaleui_dom_is_connected(h_) != 0; }
    void Destroy() const { whaleui_dom_element_destroy(h_); }

    /* element.append/prepend/before/after (single-node forms) */
    bool Append(Element node) const { return whaleui_dom_append(h_, node.h_) == 0; }
    bool Prepend(Element node) const { return whaleui_dom_prepend(h_, node.h_) == 0; }
    bool Before(Element node) const { return whaleui_dom_before(h_, node.h_) == 0; }
    bool After(Element node) const { return whaleui_dom_after(h_, node.h_) == 0; }

    /* insertAdjacentHTML: position = beforebegin|afterbegin|beforeend|afterend */
    bool InsertAdjacentHTML(const std::string& position, const std::string& html) const {
        return whaleui_dom_insert_adjacent_html(h_, position.c_str(), html.c_str()) == 0;
    }
    bool InsertAdjacentText(const std::string& position, const std::string& text) const {
        return whaleui_dom_insert_adjacent_text(h_, position.c_str(), text.c_str()) == 0;
    }

    /* focus + geometry */
    bool Focus() const { return whaleui_dom_focus(h_) == 0; }
    bool Blur() const { return whaleui_dom_blur(h_) == 0; }
    bool BoundingClientRect(whaleui_dom_rect_t* out) const {
        return whaleui_dom_get_bounding_client_rect(h_, out) == 0;
    }

    /* events (mirror addEventListener(type, listener[, capture])) */
    bool AddEventListener(const std::string& type, whaleui_event_cb cb, void* ud,
                          bool capture = false) const {
        return whaleui_dom_add_event_listener_ex(h_, type.c_str(), cb, ud,
                                                 capture ? 1 : 0) == 0;
    }
    bool RemoveEventListener(const std::string& type, whaleui_event_cb cb, void* ud) const {
        return whaleui_dom_remove_event_listener(h_, type.c_str(), cb, ud) == 0;
    }
    bool DispatchEvent(const std::string& type) const {
        return whaleui_dom_dispatch_event(h_, type.c_str()) == 0;
    }

    /* canvas interop (this element must be <canvas>) */
    whaleui_canvas_context_t* GetContext(const std::string& type) const {
        return whaleui_canvas_get_context(h_, type.c_str());
    }
    void SetCanvasSize(int w, int h) const { whaleui_canvas_set_size(h_, w, h); }
    void GetCanvasSize(int* w, int* h) const { whaleui_canvas_get_size(h_, w, h); }

private:
    static std::vector<Element> Drain(whaleui_dom_list_t* list) {
        std::vector<Element> out;
        if (!list) return out;
        size_t n = whaleui_dom_list_length(list);
        out.reserve(n);
        for (size_t i = 0; i < n; ++i) out.push_back(Element(whaleui_dom_list_item(list, i)));
        whaleui_dom_list_destroy(list);
        return out;
    }

    whaleui_dom_element_t* h_;
};

inline Element Event::Target() const { return Element(whaleui_dom_event_target(h_)); }
inline Element Event::CurrentTarget() const { return Element(whaleui_dom_event_current_target(h_)); }
inline Element Event::RelatedTarget() const {
    return Element(h_ ? whaleui_dom_event_related_target(h_) : nullptr);
}

/* ---- ComputedStyle (owns the snapshot from getComputedStyle) ---- */

class ComputedStyle {
public:
    ComputedStyle() : h_(nullptr) {}
    ~ComputedStyle() { if (h_) whaleui_style_declaration_destroy(h_); }
    ComputedStyle(const ComputedStyle&) = delete;
    ComputedStyle& operator=(const ComputedStyle&) = delete;
    ComputedStyle(ComputedStyle&& o) : h_(o.h_) { o.h_ = nullptr; }
    ComputedStyle& operator=(ComputedStyle&& o) {
        if (this != &o) {
            if (h_) whaleui_style_declaration_destroy(h_);
            h_ = o.h_;
            o.h_ = nullptr;
        }
        return *this;
    }
    explicit operator bool() const { return h_ != nullptr; }

    size_t Length() const { return h_ ? whaleui_style_declaration_length(h_) : 0; }
    std::string Item(size_t i) const {
        char buf[128];
        if (!h_ || whaleui_style_declaration_item(h_, i, buf, sizeof(buf)) != 0)
            return std::string();
        return std::string(buf);
    }
    std::string Get(const std::string& property) const {
        char buf[256];
        if (!h_ || whaleui_style_declaration_get(h_, property.c_str(), buf, sizeof(buf)) != 0)
            return std::string();
        return std::string(buf);
    }

private:
    friend class Document;
    explicit ComputedStyle(whaleui_style_declaration_t* h) : h_(h) {}
    whaleui_style_declaration_t* h_;
};

/* ---- Document ---- */

/* Range (document.createRange() subset): owns the C range handle. */
class Range {
public:
    Range() : h_(nullptr) {}
    explicit Range(whaleui_dom_range_t* h) : h_(h) {}
    ~Range() { if (h_) whaleui_dom_range_destroy(h_); }
    Range(const Range&) = delete;
    Range& operator=(const Range&) = delete;
    Range(Range&& o) : h_(o.h_) { o.h_ = nullptr; }
    explicit operator bool() const { return h_ != nullptr; }
    whaleui_dom_range_t* handle() const { return h_; }

    bool SetStart(Element container, int offset) const {
        return whaleui_dom_range_set_start(h_, container.handle(), offset) == 0;
    }
    bool SetEnd(Element container, int offset) const {
        return whaleui_dom_range_set_end(h_, container.handle(), offset) == 0;
    }
    Element StartContainer() const {
        return Element(whaleui_dom_range_start_container(h_));
    }
    Element EndContainer() const {
        return Element(whaleui_dom_range_end_container(h_));
    }
    int StartOffset() const { return whaleui_dom_range_start_offset(h_); }
    int EndOffset() const { return whaleui_dom_range_end_offset(h_); }
    bool Collapsed() const { return whaleui_dom_range_collapsed(h_) != 0; }
    std::string ToString() const { return ToStr(whaleui_dom_range_to_string(h_)); }
    bool SelectNodeContents(Element el) const {
        return whaleui_dom_range_select_node_contents(h_, el.handle()) == 0;
    }
    bool DeleteContents() const {
        return whaleui_dom_range_delete_contents(h_) == 0;
    }

private:
    whaleui_dom_range_t* h_;
};

class Document {
public:
    Document() : h_(nullptr) {}
    explicit Document(whaleui_dom_document_t* h) : h_(h) {}
    explicit operator bool() const { return h_ != nullptr; }
    bool operator==(const Document& o) const { return h_ == o.h_; }
    whaleui_dom_document_t* handle() const { return h_; }

    Element DocumentElement() const { return Element(whaleui_dom_document_element(h_)); }
    Element Body() const { return Element(whaleui_dom_body(h_)); }
    Element Head() const { return Element(whaleui_dom_head(h_)); }

    std::string Title() const { return ToStr(whaleui_dom_get_title(h_)); }
    bool SetTitle(const std::string& title) const { return whaleui_dom_set_title(h_, title.c_str()) == 0; }
    Element ActiveElement() const { return Element(whaleui_dom_active_element(h_)); }

    Element GetElementById(const std::string& id) const {
        return Element(whaleui_dom_get_element_by_id(h_, id.c_str()));
    }
    Element QuerySelector(const std::string& sel) const {
        return Element(whaleui_dom_query_selector(h_, sel.c_str()));
    }
    std::vector<Element> QuerySelectorAll(const std::string& sel) const {
        return Drain(whaleui_dom_query_selector_all(h_, sel.c_str()));
    }
    std::vector<Element> GetElementsByClassName(const std::string& cls) const {
        return Drain(whaleui_dom_get_elements_by_class_name(h_, cls.c_str()));
    }
    std::vector<Element> GetElementsByTagName(const std::string& tag) const {
        return Drain(whaleui_dom_get_elements_by_tag_name(h_, tag.c_str()));
    }

    Element CreateElement(const std::string& tag) const {
        return Element(whaleui_dom_create_element(h_, tag.c_str()));
    }
    Element CreateTextNode(const std::string& text) const {
        return Element(whaleui_dom_create_text_node(h_, text.c_str()));
    }
    /* createDocumentFragment: append it to move its children into the tree */
    Element CreateDocumentFragment() const {
        return Element(whaleui_dom_create_document_fragment(h_));
    }
    /* createRange(): endpoints are element + UTF-8 byte offset (see whaleui.h) */
    Range CreateRange() const { return Range(whaleui_dom_create_range(h_)); }

    ComputedStyle GetComputedStyle(Element el, const char* pseudo = nullptr) const {
        return ComputedStyle(whaleui_dom_get_computed_style(el.handle(), pseudo));
    }

    /* document-level events (document.addEventListener mirror): the
     * document's event target is its documentElement */
    bool AddEventListener(const std::string& type, whaleui_event_cb cb, void* ud,
                          bool capture = false) const {
        return whaleui_dom_add_event_listener_ex(
                   reinterpret_cast<whaleui_dom_element_t*>(h_), type.c_str(),
                   cb, ud, capture ? 1 : 0) == 0;
    }
    bool RemoveEventListener(const std::string& type, whaleui_event_cb cb, void* ud) const {
        return whaleui_dom_remove_event_listener(
                   reinterpret_cast<whaleui_dom_element_t*>(h_), type.c_str(),
                   cb, ud) == 0;
    }
    bool DispatchEvent(const std::string& type) const {
        return whaleui_dom_dispatch_event(
                   reinterpret_cast<whaleui_dom_element_t*>(h_), type.c_str()) == 0;
    }

private:
    static std::vector<Element> Drain(whaleui_dom_list_t* list) {
        std::vector<Element> out;
        if (!list) return out;
        size_t n = whaleui_dom_list_length(list);
        out.reserve(n);
        for (size_t i = 0; i < n; ++i) out.push_back(Element(whaleui_dom_list_item(list, i)));
        whaleui_dom_list_destroy(list);
        return out;
    }

    whaleui_dom_document_t* h_;
};

/* ---- Window (RAII: destroys the native window + document) ---- */

class Window {
public:
    Window() : h_(nullptr), owned_(false) {}
    Window(whaleui_window_t* h, bool owned) : h_(h), owned_(owned) {}
    ~Window() { if (h_ && owned_) whaleui_window_destroy(h_); }
    Window(const Window&) = delete;
    Window& operator=(const Window&) = delete;
    Window(Window&& o) : h_(o.h_), owned_(o.owned_) { o.h_ = nullptr; }
    explicit operator bool() const { return h_ != nullptr; }
    whaleui_window_t* handle() const { return h_; }

    /* early teardown (the App otherwise owns the window until its own
     * destroy); after Destroy the handle is empty. */
    void Destroy() {
        if (h_) { whaleui_window_destroy(h_); h_ = nullptr; }
    }

    bool Show() const { return whaleui_window_show(h_) == 0; }
    bool Hide() const { return whaleui_window_hide(h_) == 0; }
    bool Close() const { return whaleui_window_close(h_) == 0; }
    bool IsVisible() const { return whaleui_window_is_visible(h_) != 0; }
    bool IsMinimized() const { return whaleui_window_is_minimized(h_) != 0; }
    bool IsMaximized() const { return whaleui_window_is_maximized(h_) != 0; }
    bool IsFocused() const { return whaleui_window_is_focused(h_) != 0; }
    bool Minimize() const { return whaleui_window_minimize(h_) == 0; }
    bool Maximize() const { return whaleui_window_maximize(h_) == 0; }
    bool Restore() const { return whaleui_window_restore(h_) == 0; }
    bool Raise() const { return whaleui_window_raise(h_) == 0; }

    void SetTitle(const std::string& t) const { whaleui_window_set_title(h_, t.c_str()); }
    std::string Title() const { return ToStr(whaleui_window_get_title(h_)); }
    void SetSize(int w, int hgt) const { whaleui_window_set_size(h_, w, hgt); }
    void GetSize(int* w, int* hgt) const { whaleui_window_get_size(h_, w, hgt); }
    void GetSizeInPixels(int* w, int* hgt) const { whaleui_window_get_size_in_pixels(h_, w, hgt); }
    void SetPosition(int x, int y) const { whaleui_window_set_position(h_, x, y); }
    void GetPosition(int* x, int* y) const { whaleui_window_get_position(h_, x, y); }
    void Center() const { whaleui_window_center(h_); }

    void SetBordered(bool b) const { whaleui_window_set_bordered(h_, b ? 1 : 0); }
    void SetResizable(bool r) const { whaleui_window_set_resizable(h_, r ? 1 : 0); }
    void SetAlwaysOnTop(bool t) const { whaleui_window_set_always_on_top(h_, t ? 1 : 0); }
    void SetOpacity(float o) const { whaleui_window_set_opacity(h_, o); }
    float GetOpacity() const { return whaleui_window_get_opacity(h_); }
    void SetFullscreen(whaleui_fullscreen_mode_t m) const { whaleui_window_set_fullscreen(h_, m); }
    whaleui_fullscreen_mode_t Fullscreen() const { return whaleui_window_get_fullscreen(h_); }
    void Flash(whaleui_flash_t mode) const { whaleui_window_flash(h_, mode); }
    float DisplayScale() const { return whaleui_window_display_scale(h_); }

    void SetEventCallback(whaleui_window_event_cb cb, void* ud) const {
        whaleui_window_set_event_callback(h_, cb, ud);
    }
    void SetHitTest(whaleui_hit_test_fn cb, void* ud) const {
        whaleui_window_set_hit_test(h_, cb, ud);
    }
    void SetKeyCallback(whaleui_key_cb cb, void* ud) const {
        whaleui_window_set_key_callback(h_, cb, ud);
    }
    void SetSelectCallback(whaleui_select_cb cb, void* ud) const {
        whaleui_window_set_select_callback(h_, cb, ud);
    }

    /* --- screen / navigator (window.screen / navigator) --- */
    bool ScreenSize(int* w, int* hgt) const {
        return whaleui_window_screen_size(h_, w, hgt) == 0;
    }
    bool ScreenWorkArea(int* x, int* y, int* w, int* hgt) const {
        return whaleui_window_screen_work_area(h_, x, y, w, hgt) == 0;
    }
    int DisplayId() const { return whaleui_window_display_id(h_); }
    float DevicePixelRatio() const {
        return whaleui_window_device_pixel_ratio(h_);
    }
    bool VisualViewport(whaleui_visual_viewport_t* out) const {
        return whaleui_window_visual_viewport(h_, out) == 0;
    }

    /* --- matchMedia (window.matchMedia) ---
     * The raw list is returned so this method can live before MediaQueryList
     * is declared; the owned wrapper is the free MatchMedia() below. */
    whaleui_media_query_list_t* MatchMediaHandle(const std::string& query) const {
        return whaleui_window_match_media(h_, query.c_str());
    }

    /* --- window state the JS API exposes but the C++ facade lacked --- */
    bool IsFullscreen() const {
        return whaleui_window_get_fullscreen(h_) != WHALEUI_FULLSCREEN_OFF;
    }
    bool SetMinSize(int w, int hgt) const {
        return whaleui_window_set_min_size(h_, w, hgt) == 0;
    }
    bool GetMinSize(int* w, int* hgt) const {
        return whaleui_window_get_min_size(h_, w, hgt) == 0;
    }
    bool SetMaxSize(int w, int hgt) const {
        return whaleui_window_set_max_size(h_, w, hgt) == 0;
    }
    bool GetMaxSize(int* w, int* hgt) const {
        return whaleui_window_get_max_size(h_, w, hgt) == 0;
    }
    bool SetMouseGrab(bool g) const {
        return whaleui_window_set_mouse_grab(h_, g ? 1 : 0) == 0;
    }
    bool MouseGrab() const { return whaleui_window_get_mouse_grab(h_) != 0; }
    bool SetKeyboardGrab(bool g) const {
        return whaleui_window_set_keyboard_grab(h_, g ? 1 : 0) == 0;
    }
    bool KeyboardGrab() const {
        return whaleui_window_get_keyboard_grab(h_) != 0;
    }
    bool SetMousePosition(int x, int y) const {
        return whaleui_window_set_mouse_position(h_, x, y) == 0;
    }
    bool SetFocusable(bool f) const {
        return whaleui_window_set_focusable(h_, f ? 1 : 0) == 0;
    }
    bool SetParent(const Window& parent) const {
        return whaleui_window_set_parent(h_, parent.handle()) == 0;
    }
    bool SetModal(const Window& parent) const {
        return whaleui_window_set_modal(h_, parent.handle()) == 0;
    }
    bool SetIcon(const std::string& uri) const {
        return whaleui_window_set_icon_uri(h_, uri.c_str()) == 0;
    }
    /* drag & drop session state (window.dragActive / dragSource) */
    bool DragActive() const { return whaleui_window_drag_active(h_) != 0; }
    Element DragSource() const {
        return Element(whaleui_window_drag_source(h_));
    }

    bool LoadHtml(const std::string& html) const {
        return whaleui_window_load_html(h_, html.c_str()) == 0;
    }
    bool LoadUri(const std::string& uri) const {
        return whaleui_window_load_uri(h_, uri.c_str()) == 0;
    }
    Document GetDocument() const { return Document(whaleui_window_get_document(h_)); }

    /* --- selection (window.getSelection() subset) --- */
    Element SelectionAnchorElement() const {
        return Element(whaleui_window_sel_anchor_element(h_));
    }
    Element SelectionFocusElement() const {
        return Element(whaleui_window_sel_focus_element(h_));
    }
    int SelectionAnchorOffset() const { return whaleui_window_sel_anchor_offset(h_); }
    int SelectionFocusOffset() const { return whaleui_window_sel_focus_offset(h_); }
    std::string SelectedText() const {
        return ToStr(whaleui_window_selected_text(h_));
    }
    void ClearSelection() const { whaleui_window_clear_selection(h_); }
    void SelectAllOf(Element el) const {
        whaleui_window_select_all_of(h_, el.handle());
    }

private:
    whaleui_window_t* h_;
    bool owned_;
};

/* ---- App (RAII: destroys windows list + GPU device; run() blocks) ---- */

class App {
public:
    App() : h_(whaleui_app_create()) {}
    ~App() { if (h_) whaleui_app_destroy(h_); }
    App(const App&) = delete;
    App& operator=(const App&) = delete;
    explicit operator bool() const { return h_ != nullptr; }
    whaleui_app_t* handle() const { return h_; }

    Window CreateWindow(const std::string& title, int width, int height) {
        /* the C App destroys every window it created on app_destroy; this
         * handle does NOT own (call Destroy() for early teardown). */
        return Window(whaleui_window_create(h_, title.c_str(), width, height), false);
    }

    int Run() { return whaleui_app_run(h_); }
    void Quit() { whaleui_app_quit(h_); }

    void SetTheme(whaleui_theme_t t) { whaleui_app_set_theme(h_, t); }
    whaleui_theme_t Theme() const { return whaleui_app_get_theme(h_); }
    whaleui_theme_t ResolvedTheme() const { return whaleui_app_resolved_theme(h_); }
    void SetAccentColor(const std::string& hex) { whaleui_app_set_accent_color(h_, hex.c_str()); }
    void SetThemeStyle(const std::string& style) { whaleui_app_set_theme_style(h_, style.c_str()); }
    std::string ThemeStyle() const { return ToStr(whaleui_app_get_theme_style(h_)); }
    void SetRenderOption(whaleui_render_option_t opt, int value) {
        whaleui_app_set_render_option(h_, opt, value);
    }
    void SetTextScale(float scale) { whaleui_app_set_text_scale(h_, scale); }
    void SetReducedMotion(bool reduce) { whaleui_app_set_reduced_motion(h_, reduce ? 1 : 0); }

    /* built-in theme styles (the list the JS API exposes as a catalog) */
    static int ThemeCount() { return whaleui_theme_count(); }
    static std::string ThemeName(int index) {
        return ToStr(whaleui_theme_name(index));
    }
    static std::string ThemeLabel(int index) {
        return ToStr(whaleui_theme_label(index));
    }

private:
    whaleui_app_t* h_;
};

/* ---- Clipboard (navigator.clipboard subset) ---- */

class Clipboard {
public:
    /* SDL-backed system clipboard (shared with the desktop). */
    static bool HasText() { return whaleui_clipboard_has_text() != 0; }
    static std::string GetText() { return ToStr(whaleui_clipboard_get_text()); }
    static bool SetText(const std::string& text) {
        return whaleui_clipboard_set_text(text.c_str()) == 0;
    }
    /* Callback forms resolve on the event loop (no promise objects). */
    static bool ReadTextAsync(whaleui_clipboard_text_cb cb, void* ud) {
        return whaleui_clipboard_read_text_async(cb, ud) == 0;
    }
    static bool WriteTextAsync(const std::string& text,
                               whaleui_clipboard_text_cb cb, void* ud) {
        return whaleui_clipboard_write_text_async(text.c_str(), cb, ud) == 0;
    }
};

/* ---- timers / animation frames (free functions, like the browser) ---- */

/* Milliseconds since the application started (performance.now()). */
inline double Now() { return whaleui_now(); }

inline long long SetTimeout(whaleui_timer_cb cb, void* ud, double delay_ms) {
    return whaleui_set_timeout(cb, ud, delay_ms);
}
inline long long SetInterval(whaleui_timer_cb cb, void* ud, double interval_ms) {
    return whaleui_set_interval(cb, ud, interval_ms);
}
inline void ClearTimeout(long long id) { whaleui_clear_timeout(id); }
inline void ClearInterval(long long id) { whaleui_clear_interval(id); }
inline int TimerCount() { return whaleui_timer_count(); }

inline long long RequestAnimationFrame(whaleui_frame_cb cb, void* ud) {
    return whaleui_request_animation_frame(cb, ud);
}
inline void CancelAnimationFrame(long long id) {
    whaleui_cancel_animation_frame(id);
}
inline int AnimationFrameCount() { return whaleui_animation_frame_count(); }

/* ---- Gamepad (navigator.getGamepads subset) ---- */

inline int GamepadCount() { return whaleui_gamepad_count(); }
inline bool GamepadConnected(int index) {
    return whaleui_gamepad_connected(index) != 0;
}
inline std::string GamepadName(int index) {
    return ToStr(whaleui_gamepad_name(index));
}
inline bool GamepadPoll(int index, whaleui_gamepad_state_t* out) {
    return whaleui_gamepad_poll(index, out) == 0;
}
inline bool GamepadRumble(int index, float low, float high,
                          unsigned int duration_ms) {
    return whaleui_gamepad_rumble(index, low, high, duration_ms) == 0;
}

/* ---- drag & drop (DataTransfer / File) ---- */

enum {
    WHALEUI_EFFECT_NONE = 0,
    WHALEUI_EFFECT_COPY = 1,
    WHALEUI_EFFECT_MOVE = 2,
    WHALEUI_EFFECT_LINK = 3
};

class File {
public:
    File() : h_(nullptr) {}
    explicit File(const whaleui_file_t* h) : h_(h) {}
    explicit operator bool() const { return h_ != nullptr; }
    std::string Name() const { return ToStr(whaleui_file_name(h_)); }
    std::string Uri() const { return ToStr(whaleui_file_uri(h_)); }
    long long Size() const { return whaleui_file_size(h_); }
    /* read through the VFS into a std::string (empty on failure) */
    std::string Read() const {
        char* data = nullptr;
        size_t len = 0;
        if (whaleui_file_read(h_, &data, &len) != 0) return std::string();
        std::string out(data, len);
        std::free(data);
        return out;
    }

private:
    const whaleui_file_t* h_;
};

class DataTransfer {
public:
    DataTransfer() : h_(nullptr) {}
    explicit DataTransfer(whaleui_data_transfer_t* h) : h_(h) {}
    explicit operator bool() const { return h_ != nullptr; }
    whaleui_data_transfer_t* handle() const { return h_; }

    bool SetData(const std::string& format, const std::string& data) const {
        return whaleui_data_transfer_set_data(h_, format.c_str(), data.c_str()) == 0;
    }
    std::string GetData(const std::string& format) const {
        return ToStr(whaleui_data_transfer_get_data(h_, format.c_str()));
    }
    bool HasFormat(const std::string& format) const {
        return whaleui_data_transfer_has_format(h_, format.c_str()) != 0;
    }
    bool SetText(const std::string& text) const {
        return whaleui_data_transfer_set_text(h_, text.c_str()) == 0;
    }
    std::string GetText() const {
        return ToStr(whaleui_data_transfer_get_text(h_));
    }
    size_t TypeCount() const { return whaleui_data_transfer_type_count(h_); }
    std::string TypeItem(size_t i) const {
        return ToStr(whaleui_data_transfer_type_item(h_, i));
    }
    size_t FileCount() const { return whaleui_data_transfer_file_count(h_); }
    File FileItem(size_t i) const {
        return File(whaleui_data_transfer_file_item(h_, i));
    }
    int EffectAllowed() const { return whaleui_data_transfer_effect_allowed(h_); }
    void SetEffectAllowed(int e) const { whaleui_data_transfer_set_effect_allowed(h_, e); }
    int DropEffect() const { return whaleui_data_transfer_drop_effect(h_); }
    void SetDropEffect(int e) const { whaleui_data_transfer_set_drop_effect(h_, e); }

private:
    whaleui_data_transfer_t* h_;
};

/* ---- Web Audio (placeholder; always unsupported for now) ---- */

inline bool AudioSupported() { return whaleui_audio_is_supported() != 0; }

/* ---- CSS (CSS.supports + programmatic stylesheets) ---- */

inline bool CssSupports(const std::string& property, const std::string& value) {
    return whaleui_css_supports(property.c_str(), value.c_str()) != 0;
}

/* rule introspection (the rule struct is opaque in the C header) */
inline std::string CssSelector(const whaleui_css_rule_t* rule) {
    return ToStr(whaleui_css_selector(rule));
}
inline std::string CssProperty(const whaleui_css_rule_t* rule,
                               const std::string& name) {
    return ToStr(whaleui_css_get_property(rule, name.c_str()));
}
inline bool CssHasProperty(const whaleui_css_rule_t* rule,
                           const std::string& name) {
    return whaleui_css_has_property(rule, name.c_str()) != 0;
}

/* A parsed stylesheet. RAII over whaleui_css_rule_t* (parse/load + destroy);
 * Apply() attaches it to a Document as that document's programmatic sheet. */
class StyleSheet {
public:
    StyleSheet() : rules_(nullptr), count_(0) {}
    ~StyleSheet() { Clear(); }
    StyleSheet(const StyleSheet&) = delete;
    StyleSheet& operator=(const StyleSheet&) = delete;
    StyleSheet(StyleSheet&& o) : rules_(o.rules_), count_(o.count_) {
        o.rules_ = nullptr;
        o.count_ = 0;
    }
    StyleSheet& operator=(StyleSheet&& o) {
        if (this != &o) {
            Clear();
            rules_ = o.rules_;
            count_ = o.count_;
            o.rules_ = nullptr;
            o.count_ = 0;
        }
        return *this;
    }
    explicit operator bool() const { return rules_ != nullptr; }

    bool Parse(const std::string& css) {
        Clear();
        return whaleui_css_parse(&rules_, &count_, css.c_str(), css.size()) == 0;
    }
    bool Load(const std::string& uri) {
        Clear();
        return whaleui_css_load(&rules_, &count_, uri.c_str()) == 0;
    }
    /* attach to `doc` (replaces any sheet attached earlier); takes effect on
     * the next frame, like any other DOM edit */
    bool Apply(const Document& doc) const {
        return whaleui_css_apply(doc.handle(), rules_, count_) == 0;
    }
    size_t Count() const { return count_; }
    /* The rule array, opaque as in the C header: pair it with CssSelector() /
     * CssProperty() below. Per-rule struct access is a white-box facility -
     * include "style/css.h" for it (see tests/test_style.cpp). */
    const whaleui_css_rule_t* rules() const { return rules_; }

private:
    void Clear() {
        if (rules_) {
            whaleui_css_rules_destroy(rules_, count_);
            rules_ = nullptr;
            count_ = 0;
        }
    }
    whaleui_css_rule_t* rules_;
    size_t count_;
};

/* ---- matchMedia list (RAII) ---- */

class MediaQueryList {
public:
    MediaQueryList() : h_(nullptr) {}
    explicit MediaQueryList(whaleui_media_query_list_t* h) : h_(h) {}
    ~MediaQueryList() { whaleui_media_query_list_destroy(h_); }
    MediaQueryList(const MediaQueryList&) = delete;
    MediaQueryList& operator=(const MediaQueryList&) = delete;
    MediaQueryList(MediaQueryList&& o) : h_(o.h_) { o.h_ = nullptr; }
    explicit operator bool() const { return h_ != nullptr; }
    whaleui_media_query_list_t* handle() const { return h_; }

    bool Matches() const { return whaleui_media_query_list_matches(h_) != 0; }
    std::string Media() const { return ToStr(whaleui_media_query_list_media(h_)); }
    bool AddListener(whaleui_media_query_cb cb, void* ud) const {
        return whaleui_media_query_list_add_listener(h_, cb, ud) == 0;
    }
    bool RemoveListener(whaleui_media_query_cb cb, void* ud) const {
        return whaleui_media_query_list_remove_listener(h_, cb, ud) == 0;
    }

private:
    whaleui_media_query_list_t* h_;
};

/* ---- navigator / Web Storage / microtasks ---- */

namespace Navigator {
inline std::string UserAgent() { return ToStr(whaleui_navigator_user_agent()); }
inline std::string Platform() { return ToStr(whaleui_navigator_platform()); }
inline std::string Language() { return ToStr(whaleui_navigator_language()); }
} // namespace Navigator

/* One global store (the engine has no origins). session=false selects
 * localStorage (persisted), session=true selects sessionStorage (memory). */
class Storage {
public:
    explicit Storage(bool session) : session_(session ? 1 : 0) {}
    size_t Length() const { return whaleui_storage_length(session_); }
    std::string Key(size_t i) const { return ToStr(whaleui_storage_key(session_, static_cast<int>(i))); }
    /* empty string when absent (use Has() to tell "" from missing) */
    std::string GetItem(const std::string& key) const {
        return ToStr(whaleui_storage_get_item(session_, key.c_str()));
    }
    bool Has(const std::string& key) const {
        return whaleui_storage_get_item(session_, key.c_str()) != nullptr;
    }
    bool SetItem(const std::string& key, const std::string& value) const {
        return whaleui_storage_set_item(session_, key.c_str(), value.c_str()) == 0;
    }
    bool RemoveItem(const std::string& key) const {
        return whaleui_storage_remove_item(session_, key.c_str()) == 0;
    }
    bool Clear() const { return whaleui_storage_clear(session_) == 0; }

private:
    int session_;
};

inline Storage LocalStorage() { return Storage(false); }
inline Storage SessionStorage() { return Storage(true); }

inline bool QueueMicrotask(whaleui_web_task_cb cb, void* ud) {
    return whaleui_queue_microtask(cb, ud) == 0;
}
inline bool RequestIdleCallback(whaleui_web_task_cb cb, void* ud) {
    return whaleui_request_idle_callback(cb, ud) == 0;
}

/* ---- MutationObserver / ResizeObserver ---- */

class MutationObserver {
public:
    MutationObserver() : h_(nullptr) {}
    explicit MutationObserver(whaleui_mutation_observer_t* h) : h_(h) {}
    ~MutationObserver() { whaleui_dom_mutation_observer_destroy(h_); }
    MutationObserver(const MutationObserver&) = delete;
    MutationObserver& operator=(const MutationObserver&) = delete;
    MutationObserver(MutationObserver&& o) : h_(o.h_) { o.h_ = nullptr; }
    explicit operator bool() const { return h_ != nullptr; }
    whaleui_mutation_observer_t* handle() const { return h_; }

    /* el may be an empty Element: watches the whole document */
    bool Observe(Element el, whaleui_mutation_cb cb, void* ud) const {
        return whaleui_dom_mutation_observer_observe(h_, el.handle(), cb, ud) == 0;
    }
    bool Disconnect() const {
        return whaleui_dom_mutation_observer_disconnect(h_) == 0;
    }
    int TakeRecords() const {
        return whaleui_dom_mutation_observer_take_records(h_);
    }

private:
    whaleui_mutation_observer_t* h_;
};

class ResizeObserver {
public:
    ResizeObserver() : h_(nullptr) {}
    explicit ResizeObserver(whaleui_resize_observer_t* h) : h_(h) {}
    ~ResizeObserver() { whaleui_dom_resize_observer_destroy(h_); }
    ResizeObserver(const ResizeObserver&) = delete;
    ResizeObserver& operator=(const ResizeObserver&) = delete;
    ResizeObserver(ResizeObserver&& o) : h_(o.h_) { o.h_ = nullptr; }
    explicit operator bool() const { return h_ != nullptr; }
    whaleui_resize_observer_t* handle() const { return h_; }

    bool Observe(Element el, whaleui_resize_cb cb, void* ud) const {
        return whaleui_dom_resize_observer_observe(h_, el.handle(), cb, ud) == 0;
    }
    bool Unobserve(Element el) const {
        return whaleui_dom_resize_observer_unobserve(h_, el.handle()) == 0;
    }
    bool Disconnect() const {
        return whaleui_dom_resize_observer_disconnect(h_) == 0;
    }

private:
    whaleui_resize_observer_t* h_;
};

/* ---- Document extensions that need the classes above ---- */

inline MutationObserver CreateMutationObserver(const Document& doc) {
    return MutationObserver(whaleui_dom_create_mutation_observer(doc.handle()));
}

inline ResizeObserver CreateResizeObserver(const Document& doc) {
    return ResizeObserver(whaleui_dom_create_resize_observer(doc.handle()));
}

inline MediaQueryList MatchMedia(const Window& win, const std::string& query) {
    return MediaQueryList(win.MatchMediaHandle(query));
}

} // namespace whaleui

#endif /* WHALEUI_HPP */
