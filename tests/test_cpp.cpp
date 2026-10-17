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

    /* ---- web-platform additions on the C++ face ----
     *
     * These APIs existed only on the C surface. The C++ header is the primary
     * API a C++ program uses, so a capability that is C-only is effectively
     * missing; every check below goes through the C++ path. */
    {
        /* CSS.supports + a programmatic stylesheet (RAII + Apply) */
        assert(whaleui::CssSupports("display", "flex"));
        assert(!whaleui::CssSupports("no-such-property", "1px"));
        whaleui::StyleSheet sheet;
        assert(sheet.Parse(".x{color:#112233}"));
        assert(static_cast<bool>(sheet));
        assert(sheet.Count() == 1);
        assert(whaleui::CssSelector(sheet.rules()) == ".x");
        assert(whaleui::CssProperty(sheet.rules(), "color") == "#112233");
        assert(whaleui::CssHasProperty(sheet.rules(), "color"));
        assert(sheet.Apply(doc));
        whaleui::StyleSheet unparsed;
        assert(!static_cast<bool>(unparsed));
        assert(unparsed.Count() == 0);
        /* applying an empty sheet is the documented "clear the programmatic
         * sheet" operation, not an error */
        assert(unparsed.Apply(doc));
        /* a null/empty document argument is rejected by the C layer */
        assert(!sheet.Apply(whaleui::Document()));

        /* matchMedia */
        {
            whaleui::MediaQueryList mq =
                whaleui::MatchMedia(win, "(min-width: 100px)");
            assert(static_cast<bool>(mq));
            assert(mq.Matches());
            assert(mq.Media() == "(min-width: 100px)");
            whaleui::MediaQueryList narrow =
                whaleui::MatchMedia(win, "(min-width: 100000px)");
            assert(!narrow.Matches());
        }

        /* screen / navigator */
        int sw = 0, sh = 0;
        assert(win.ScreenSize(&sw, &sh));
        assert(sw > 0 && sh > 0);
        int sx = 0, sy = 0, aw = 0, ah = 0;
        assert(win.ScreenWorkArea(&sx, &sy, &aw, &ah));
        assert(aw > 0 && ah > 0);
        assert(win.DevicePixelRatio() > 0.0f);
        assert(!whaleui::Navigator::UserAgent().empty());
        assert(!whaleui::Navigator::Platform().empty());
        assert(!whaleui::Navigator::Language().empty());
        whaleui_visual_viewport_t vv;
        assert(win.VisualViewport(&vv));

        /* Web Storage */
        whaleui::Storage ls = whaleui::LocalStorage();
        whaleui::Storage ss = whaleui::SessionStorage();
        assert(ss.SetItem("k", "v"));
        assert(ss.GetItem("k") == "v");
        assert(ss.Has("k"));
        assert(!ss.Has("missing"));
        assert(ss.Length() == 1);
        assert(ss.Key(0) == "k");
        assert(ss.RemoveItem("k"));
        assert(!ss.Has("k"));
        assert(ss.SetItem("a", "1"));
        assert(ss.Clear());
        assert(ss.Length() == 0);
        /* the two stores are independent */
        assert(ls.SetItem("only-local", "1"));
        assert(ls.Has("only-local"));
        assert(!ss.Has("only-local"));
        assert(ls.RemoveItem("only-local"));

        /* microtasks / idle callbacks are accepted (they run on the loop) */
        struct Task { static void run(void*) {} };
        assert(whaleui::QueueMicrotask(Task::run, nullptr));
        assert(whaleui::RequestIdleCallback(Task::run, nullptr));

        /* MutationObserver / ResizeObserver wrappers */
        struct Obs {
            static void on_mut(whaleui_dom_element_t*, int, const char*, int,
                               int, void*) {}
            static void on_resize(whaleui_dom_element_t*, float, float, void*) {}
        };
        {
            whaleui::MutationObserver mo = whaleui::CreateMutationObserver(doc);
            assert(static_cast<bool>(mo));
            /* el may be empty: a NULL target watches the whole document */
            assert(mo.Observe(whaleui::Element(), Obs::on_mut, nullptr));
            assert(mo.TakeRecords() == 0);
            assert(mo.Disconnect());
        }
        {
            whaleui::ResizeObserver ro = whaleui::CreateResizeObserver(doc);
            assert(static_cast<bool>(ro));
            /* resize observation needs a real element (it samples a box);
             * an empty handle is rejected, unlike the mutation observer's
             * document-wide form */
            assert(!ro.Observe(whaleui::Element(), Obs::on_resize, nullptr));
            whaleui::Element target = doc.Body();
            assert(static_cast<bool>(target));
            assert(ro.Observe(target, Obs::on_resize, nullptr));
            assert(ro.Unobserve(target));
            assert(ro.Disconnect());
        }

        /* theme catalog (static: no instance needed) */
        assert(whaleui::App::ThemeCount() > 0);
        assert(!whaleui::App::ThemeName(0).empty());
        assert(!whaleui::App::ThemeLabel(0).empty());

        /* window state the facade was missing */
        assert(win.SetMinSize(100, 100));
        int mw = 0, mh = 0;
        assert(win.GetMinSize(&mw, &mh));
        assert(mw == 100 && mh == 100);
        assert(win.SetMaxSize(4000, 4000));
        assert(win.GetMaxSize(&mw, &mh));
        assert(mw == 4000 && mh == 4000);
        assert(!win.IsFullscreen());
        assert(win.SetMouseGrab(true));
        assert(win.MouseGrab());
        assert(win.SetMouseGrab(false));
        assert(!win.MouseGrab());
        assert(win.SetKeyboardGrab(true));
        assert(win.KeyboardGrab());
        assert(win.SetKeyboardGrab(false));
        assert(win.SetFocusable(true));
        assert(!win.DragActive());
        assert(!static_cast<bool>(win.DragSource()));
        std::printf("[cpp] web-platform additions OK\n");
    }

    /* window destroy + app-owned list */
    win.Destroy();
    assert(!static_cast<bool>(win));

    /* app destroy reaps the rest (none left) */
    return 0;
}
