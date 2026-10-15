// test_cpp: the C++ wrapper API (whaleui.hpp) contract tests.
// The wrapper is the C++ face of the library; every check here mirrors the
// C-API behavior under namespace whaleui types.
#include "whaleui.hpp"

#include <cassert>
#include <cstring>
#include <string>

namespace {

struct ClickLog {
    static int count;
    static std::string last_type;
    static void on_event(whaleui_dom_event_t* ev, void* ud) {
        (void)ud;
        whaleui::Event e(ev);
        ++count;
        last_type = e.Type() ? e.Type() : "";
        assert(static_cast<bool>(e.Target()));
    }
};
int ClickLog::count = 0;
std::string ClickLog::last_type;

/* window-event probe: records the CLOSE cancellation return value */
struct WinLog {
    static int got;
    static int last_event;
    static int on_event(whaleui_window_t* w, whaleui_window_event_t ev, int a,
                        int b, void* ud) {
        (void)w; (void)a; (void)b; (void)ud;
        ++got;
        last_event = static_cast<int>(ev);
        return ev == WHALEUI_WINDOW_EVENT_CLOSE ? 1 : 0; /* cancel close */
    }
};
int WinLog::got = 0;
int WinLog::last_event = -1;

} // namespace

int main(void)
{
    /* app + window (headless: no show, no run) */
    whaleui::App app;
    assert(static_cast<bool>(app));
    app.SetTheme(WHALEUI_THEME_DARK);
    assert(app.Theme() == WHALEUI_THEME_DARK);

    whaleui::Window win = app.CreateWindow("cpp", 800, 600);
    assert(static_cast<bool>(win));
    win.SetTitle("cpp2");
    assert(win.Title() == "cpp2");
    int w = 0, h = 0;
    win.GetSize(&w, &h);
    assert(w == 800 && h == 600);
    win.SetSize(1024, 768);
    win.GetSize(&w, &h);
    assert(w == 1024 && h == 768);
    win.SetEventCallback(WinLog::on_event, nullptr);

    /* content */
    assert(win.LoadHtml(
        "<html><head><title>Doc</title></head><body>"
        "<div id=\"box\" class=\"card big\"><p class=\"note\">Hi</p>"
        "<p>Two</p></div>"
        "<input id=\"in\" value=\"v1\">"
        "<canvas id=\"cv\" width=\"8\" height=\"8\"></canvas>"
        "</body></html>"));
    whaleui::Document doc = win.GetDocument();
    assert(static_cast<bool>(doc));
    assert(doc.Title() == "Doc");
    assert(doc.SetTitle("Doc2"));
    assert(doc.Title() == "Doc2");
    assert(static_cast<bool>(doc.DocumentElement()));
    assert(doc.DocumentElement().TagName() == "html");
    assert(static_cast<bool>(doc.Body()));
    assert(doc.Body().Text().find("Hi") != std::string::npos);

    /* queries */
    whaleui::Element box = doc.GetElementById("box");
    assert(static_cast<bool>(box));
    assert(box.TagName() == "div");
    assert(doc.QuerySelector("#box") == box);
    assert(doc.QuerySelector("div > p") == doc.QuerySelector(".note"));
    assert(doc.QuerySelectorAll("p").size() == 2);
    assert(doc.GetElementsByClassName("note").size() == 1);
    assert(doc.GetElementsByTagName("div").size() == 1);
    assert(box.Matches("div.card.big"));
    assert(!box.Matches("p"));
    {
        whaleui::Element p = box.QuerySelector(".note");
        assert(static_cast<bool>(p));
        assert(p.Closest("div") == box);
        /* element-scoped: descendants only (browser semantics) */
        assert(!box.QuerySelector("div"));
        assert(box.QuerySelectorAll("p").size() == 2);
    }

    /* mutation */
    {
        whaleui::Element el = doc.CreateElement("section");
        assert(static_cast<bool>(el));
        el.SetText("new");
        assert(el.Text() == "new");
        assert(doc.Body().AppendChild(el));
        assert(doc.Body().Contains(el));
        assert(el.Remove());
        el.Destroy();

        whaleui::Element span = doc.CreateElement("span");
        span.SetInnerHTML("<b>x</b>");
        assert(span.InnerHTML() == "<b>x</b>");
        assert(span.OuterHTML() == "<span><b>x</b></span>");
        span.Destroy();
    }

    /* attributes + classList + style */
    assert(box.HasAttribute("class"));
    assert(box.GetAttribute("class") == "card big");
    assert(box.ClassContains("big"));
    assert(box.ClassRemove("big"));
    assert(!box.ClassContains("big"));
    assert(box.ClassToggle("big"));
    assert(box.ClassContains("big"));
    assert(box.ToggleAttribute("hidden", 1)); /* force on: present */
    assert(box.HasAttribute("hidden"));
    assert(!box.ToggleAttribute("hidden", 0)); /* force off: result = absent */
    assert(!box.HasAttribute("hidden"));

    box.SetStyle("width", "100px");
    assert(box.GetStyle("width") == "100px");
    assert(box.StyleCssText().find("width: 100px") != std::string::npos);
    box.SetStyleCssText("color: red; margin: 0");
    assert(box.StyleLength() == 2);
    assert(box.StyleItem(0) == "color");
    assert(box.GetStyle("margin") == "0");

    {
        whaleui::ComputedStyle cs = doc.GetComputedStyle(box);
        assert(static_cast<bool>(cs));
        assert(cs.Get("color") == "red");
        assert(cs.Length() >= 2);
    }

    /* form value + events */
    {
        whaleui::Element in = doc.GetElementById("in");
        assert(in.Value() == "v1");
        assert(in.SetValue("v2"));
        assert(in.Value() == "v2");

        assert(box.AddEventListener("click", ClickLog::on_event, nullptr));
        assert(box.AddEventListener("click", ClickLog::on_event, nullptr)); /* dup */
        assert(box.DispatchEvent("click"));
        assert(ClickLog::count == 1);
        assert(ClickLog::last_type == "click");
        assert(box.RemoveEventListener("click", ClickLog::on_event, nullptr));
        assert(box.DispatchEvent("click"));
        assert(ClickLog::count == 1);
        /* capture-phase registration + currentTarget are exercised in
         * test_dom.cpp; the wrapper passes them through unchanged. */
        auto noop = [](whaleui_dom_event_t*, void*) {};
        assert(box.AddEventListener("focus", noop, nullptr, true));
        assert(box.RemoveEventListener("focus", noop, nullptr));
    }

    /* classList extras + dataset + insertAdjacentHTML + append/prepend */
    {
        whaleui::Element box = doc.GetElementById("box");
        assert(box.ClassLength() >= 1);
        assert(!box.ClassItem(0).empty());
        assert(box.ClassReplace("card", "card2"));
        assert(box.ClassContains("card2"));
        box.ClassAdd("card");
        assert(box.ClassToggleForce("hl", 1));
        assert(box.ClassContains("hl"));

        assert(box.DatasetSet("ownerId", "7"));
        assert(box.DatasetGet("ownerId") == "7");
        assert(box.DatasetHas("ownerId"));
        assert(!box.DatasetHas("nope"));
        assert(box.DatasetRemove("ownerId"));
        assert(!box.DatasetHas("ownerId"));

        assert(box.InsertAdjacentHTML("beforeend", "<u>hi</u>"));
        assert(box.InnerHTML().find("<u>hi</u>") != std::string::npos);
        whaleui::Element child = doc.CreateElement("b");
        assert(box.Prepend(child));
        assert(box.FirstElementChild() == child);
        assert(box.Remove());
    }

    /* DocumentFragment + insertAdjacentText through the wrapper */
    {
        whaleui::Document d2 = doc; /* reuse the app document */
        whaleui::Element host = d2.CreateElement("div");
        whaleui::Element frag = d2.CreateDocumentFragment();
        assert(static_cast<bool>(frag));
        whaleui::Element one = d2.CreateElement("i");
        frag.AppendChild(one);
        assert(frag.ChildElementCount() == 1);
        assert(host.AppendChild(frag));      /* moves children in, empties frag */
        assert(host.ChildElementCount() == 1);
        assert(frag.ChildElementCount() == 0);
        assert(host.FirstElementChild() == one);
        assert(one.InsertAdjacentText("beforeend", "txt"));
        assert(one.Text() == "txt");
    }

    /* canvas interop through the wrapper handle */
    {
        whaleui::Element cv = doc.GetElementById("cv");
        assert(static_cast<bool>(cv));
        int cw = 0, ch = 0;
        cv.GetCanvasSize(&cw, &ch);
        assert(cw == 8 && ch == 8);
        whaleui_canvas_context_t* ctx = cv.GetContext("2d");
        assert(ctx != nullptr);
        assert(whaleui_canvas_set_fill_style(ctx, "#00FF00") == 0);
        assert(whaleui_canvas_fill_rect(ctx, 0, 0, 8, 8) == 0);
        whaleui_image_data_t* img = whaleui_canvas_get_image_data(ctx, 0, 0, 1, 1);
        assert(img && whaleui_image_data_get(img, 0, 0) == 0xFF00FF00u);
        whaleui_image_data_destroy(img);
        assert(cv.GetContext("webgl") == nullptr);
    }

    /* window destroy + app-owned list */
    win.Destroy();
    assert(!static_cast<bool>(win));

    /* app destroy reaps the rest (none left) */
    return 0;
}
