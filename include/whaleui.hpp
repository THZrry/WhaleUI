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

private:
    whaleui_app_t* h_;
};

} // namespace whaleui

#endif /* WHALEUI_HPP */
