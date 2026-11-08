#include "stdafx.h"

#include "list_view.h"

using namespace std::chrono_literals;

namespace uih {

lv::SavedScrollPosition ListView::save_scroll_position() const
{
    // Work out where the scroll position is proportionally in the item at the
    // scroll position, or between the previous and next items if the scroll
    // position is between items

    const auto scroll_position = m_smooth_scroll_helper->current_target(ScrollAxis::Vertical);
    const auto previous_item_index = get_item_at_or_before(scroll_position);
    const auto next_item_index = get_item_at_or_after(scroll_position);
    const auto next_item_bottom = get_item_position_bottom(next_item_index);
    const auto previous_item_top = get_item_position(previous_item_index);

    // If next_item_bottom == previous_item_bottom == 0, there are probably no items
    const auto proportional_position = next_item_bottom != previous_item_top
        ? static_cast<double>(scroll_position - previous_item_top)
            / static_cast<double>(next_item_bottom - previous_item_top)
        : 0.0;

    return {previous_item_index, next_item_index, proportional_position};
}

void ListView::restore_scroll_position(const lv::SavedScrollPosition& position, bool redraw_scroll_bars)
{
    const auto new_next_item_bottom = get_item_position_bottom(position.next_item_index);
    const auto new_previous_item_top = get_item_position(position.previous_item_index);
    const auto new_position
        = position.proportional_position * static_cast<double>(new_next_item_bottom - new_previous_item_top)
        + new_previous_item_top;
    const auto new_position_rounded = gsl::narrow<int>(std::lround(new_position));

    update_scroll_info(true, true, redraw_scroll_bars, new_position_rounded);
}

void ListView::ensure_visible(size_t index, EnsureVisibleMode mode)
{
    if (m_ensure_visible_suspended || index >= m_items.size())
        return;

    const auto item_visibility = get_item_visibility(index);

    const auto item_area_height = get_item_area_height();
    const auto item_height = get_item_height(index);
    const auto item_start_position = get_item_position(index);
    const auto stuck_headers_height = [&] {
        if (!are_group_headers_sticky_active())
            return 0;

        const auto is_new_group = get_is_new_group(index);

        return gsl::narrow<int>(get_cumulative_item_display_group_count(index)) * m_group_height
            + (is_new_group ? get_leaf_group_header_bottom_margin() : get_stuck_leaf_group_header_bottom_margin());
    };
    const auto item_end_position = get_item_position_bottom(index);

    switch (mode) {
    case EnsureVisibleMode::PreferCentringItem:
        switch (item_visibility) {
        case ItemVisibility::ObscuredAbove:
            absolute_scroll(item_start_position - stuck_headers_height());
            break;
        case ItemVisibility::ObscuredBelow:
            absolute_scroll(item_end_position - item_area_height);
            break;
        case ItemVisibility::FullyVisible:
            break;
        default:
            absolute_scroll(item_start_position - (item_area_height - item_height) / 2);
        }
        break;
    case EnsureVisibleMode::PreferMinimalScrolling:
        switch (item_visibility) {
        case ItemVisibility::ObscuredAbove:
        case ItemVisibility::AboveViewport:
            absolute_scroll(item_start_position - stuck_headers_height());
            break;
        case ItemVisibility::ObscuredBelow:
        case ItemVisibility::BelowViewport:
            absolute_scroll(item_end_position - item_area_height);
            break;
        case ItemVisibility::FullyVisible:
            break;
        }
    }
}

void ListView::absolute_scroll(
    int new_position, ScrollAxis axis, bool suppress_smooth_scroll, SmoothScrollHelper::Duration duration)
{
    const auto computed_suppress_smooth_scroll = suppress_smooth_scroll || m_is_smooth_scrolling_suspended
        || (m_use_smooth_scroll && !IsWindowVisible(get_wnd()));

    if (m_use_smooth_scroll && !computed_suppress_smooth_scroll) {
        m_smooth_scroll_helper->absolute_scroll(axis, new_position, duration);
        return;
    }

    m_smooth_scroll_helper->abandon_animation(axis);

    internal_scroll(new_position, axis);
}

void ListView::delta_scroll(int delta, ScrollAxis axis, bool suppress_smooth_scroll)
{
    const auto computed_suppress_smooth_scroll = suppress_smooth_scroll || m_is_smooth_scrolling_suspended
        || (m_use_smooth_scroll && !IsWindowVisible(get_wnd()));

    if (m_use_smooth_scroll && !computed_suppress_smooth_scroll) {
        m_smooth_scroll_helper->delta_scroll(axis, delta);
        return;
    }

    m_smooth_scroll_helper->abandon_animation(axis);

    internal_scroll(get_scroll_position(axis) + delta, axis);
}

void ListView::internal_scroll(int new_position, ScrollAxis axis)
{
    if (!m_initialised)
        return;

    auto& scroll_position = get_scroll_position(axis);
    const int original_scroll_position = scroll_position;

    // 細滑軌：垂直方向的位置自己夾（沒有原生滑軌可以交給 Windows 夾）
    if (axis == ScrollAxis::Vertical && m_thin_scrollbar_active)
        scroll_position = clamp_scroll(axis, new_position);
    else
        scroll_position = set_scroll_position(get_wnd(), axis, original_scroll_position, new_position);

    if (scroll_position == original_scroll_position)
        return;

    const auto items_rect = get_items_rect();
    int dx = 0;
    int dy = 0;
    (axis == ScrollAxis::Vertical ? dy : dx) = original_scroll_position - scroll_position;

    hide_tooltip();

    if (axis == ScrollAxis::Horizontal)
        reposition_header();

    std::vector<RECT> invalidate_after_scroll_window{};

    if (get_show_group_info_area() && m_is_group_info_area_sticky && dy != 0) {
        const auto first_items
            = std::unordered_set{gsl::narrow_cast<size_t>(get_first_or_previous_visible_item(original_scroll_position)),
                gsl::narrow_cast<size_t>(get_first_or_previous_visible_item(m_scroll_position))};

        const auto first_group_items = first_items | ranges::views::transform([this](auto index) -> size_t {
            return std::get<0>(get_item_group_range(index, m_group_count - 1));
        }) | ranges::to<std::unordered_set>;

        for (const auto index : first_group_items) {
            if (index >= m_items.size())
                continue;

            const auto old_rect = get_item_group_info_area_render_rect(index, items_rect, original_scroll_position);
            const RECT adjusted_old_rect{old_rect.left, old_rect.top + dy, old_rect.right, old_rect.bottom + dy};

            const auto new_rect = get_item_group_info_area_render_rect(index, items_rect);

            if (adjusted_old_rect == new_rect)
                continue;

            RECT invalidate_rect{};

            if (IntersectRect(&invalidate_rect, &items_rect, &old_rect))
                RedrawWindow(get_wnd(), &invalidate_rect, nullptr, RDW_INVALIDATE);

            if (IntersectRect(&invalidate_rect, &items_rect, &new_rect))
                invalidate_after_scroll_window.push_back(invalidate_rect);
        }
    }

    RECT clip_rect{items_rect};

    if (are_group_headers_sticky_active() && dy != 0) {
        const auto old_stuck_group_headers_info = get_stuck_group_headers_info(original_scroll_position);
        RECT old_stuck_headers_rect{
            items_rect.left, items_rect.top, items_rect.right, items_rect.top + old_stuck_group_headers_info.height};

        const auto new_stuck_group_headers_info = get_stuck_group_headers_info();
        RECT new_stuck_headers_rect{
            items_rect.left, items_rect.top, items_rect.right, items_rect.top + new_stuck_group_headers_info.height};

        if (old_stuck_group_headers_info != new_stuck_group_headers_info) {
            RECT invalidate_rect{};

            if (IntersectRect(&invalidate_rect, &items_rect, &old_stuck_headers_rect))
                RedrawWindow(get_wnd(), &invalidate_rect, nullptr, RDW_INVALIDATE);

            if (IntersectRect(&invalidate_rect, &items_rect, &new_stuck_headers_rect))
                invalidate_after_scroll_window.push_back(invalidate_rect);
        }

        clip_rect.top += dy > 0 ? old_stuck_group_headers_info.height : new_stuck_group_headers_info.height;
    }

    RECT rc_invalidated{};

    const int rgn_type
        = ScrollWindowEx(get_wnd(), dx, dy, &clip_rect, &clip_rect, nullptr, &rc_invalidated, SW_INVALIDATE);

    const auto skip_update_now = dx == 0 && rgn_type == SIMPLEREGION && rc_invalidated == clip_rect;

    for (const auto& rect : invalidate_after_scroll_window) {
        RedrawWindow(get_wnd(), &rect, nullptr, RDW_INVALIDATE);
    }

    // 細滑軌是畫在清單上的：ScrollWindowEx 會把舊的滑軌像素一起捲走，要整條重畫
    if (axis == ScrollAxis::Vertical && m_thin_scrollbar_active)
        update_thin_overlay();

    if (!skip_update_now)
        RedrawWindow(get_wnd(), nullptr, nullptr, RDW_UPDATENOW | RDW_ALLCHILDREN);
}

void ListView::scroll_from_scroll_bar(short scroll_bar_command, ScrollAxis axis)
{
    const auto si = get_scroll_info(axis);

    const auto unclamped_current_target_position = m_smooth_scroll_helper->current_target(axis);
    const auto current_target_position = std::clamp(unclamped_current_target_position, si.nMin, si.nMax);

    switch (scroll_bar_command) {
    case SB_LINEDOWN:
        delta_scroll(m_item_height, axis);
        break;
    case SB_LINEUP:
        delta_scroll(-m_item_height, axis);
        break;
    case SB_PAGEUP: {
        int delta = -gsl::narrow_cast<int>(si.nPage);

        if (axis == ScrollAxis::Vertical && current_target_position > si.nMin)
            delta += get_stuck_group_headers_height(current_target_position);

        delta_scroll(delta, axis);
        break;
    }
    case SB_PAGEDOWN: {
        int delta = gsl::narrow_cast<int>(si.nPage);

        if (axis == ScrollAxis::Vertical)
            delta -= get_stuck_group_headers_height(std::min(si.nMax, current_target_position + delta));

        delta_scroll(delta, axis);
        break;
    }
    case SB_THUMBTRACK:
    case SB_THUMBPOSITION:
        absolute_scroll(si.nTrackPos, axis, false, 50.ms);
        break;
    case SB_BOTTOM:
        absolute_scroll(si.nMax, axis);
        break;
    case SB_TOP:
        absolute_scroll(si.nMin, axis);
        break;
    default:
        break;
    }
}

int ListView::get_items_viewport_height() const
{
    const auto items_rect = get_items_rect();
    return static_cast<int>(wil::rect_height(items_rect));
}

void ListView::update_vertical_scroll_info(bool redraw, std::optional<int> new_vertical_position)
{
    m_smooth_scroll_helper->abandon_animation(ScrollAxis::Vertical, !new_vertical_position);

    const auto old_scroll_position = m_scroll_position;

    SCROLLINFO scroll{};
    scroll.cbSize = sizeof(SCROLLINFO);
    scroll.fMask = SIF_RANGE | SIF_PAGE | SIF_POS;
    scroll.nMin = 0;

    const auto count = m_items.size();
    scroll.nMax = count > 0 ? get_item_group_bottom(count - 1) : 0;

    // 細滑軌：範圍、頁高、位置都自己存，不碰原生滑軌（不然 SetScrollInfo 會把原生那條又叫出來）
    if (m_thin_scrollbar_active) {
        m_vscroll_max = scroll.nMax;
        m_vscroll_page = get_items_viewport_height();
        m_scroll_position = clamp_scroll(ScrollAxis::Vertical, new_vertical_position.value_or(m_scroll_position));
        update_thin_overlay();

        if (new_vertical_position || m_scroll_position != old_scroll_position)
            invalidate_all();
        return;
    }

    scroll.nPage = get_items_viewport_height();
    scroll.nPos = new_vertical_position.value_or(m_scroll_position);

    m_scroll_position = SetScrollInfo(get_wnd(), SB_VERT, &scroll, redraw);

    if (new_vertical_position || m_scroll_position != old_scroll_position)
        invalidate_all();
}

void ListView::update_horizontal_scroll_info(bool redraw)
{
    m_smooth_scroll_helper->abandon_animation(ScrollAxis::Horizontal);

    auto rc = get_items_rect();

    const auto old_scroll_position = m_horizontal_scroll_position;
    const auto cx = get_columns_display_width() + get_total_indentation();

    SCROLLINFO horizontal_si{};
    horizontal_si.cbSize = sizeof(SCROLLINFO);
    horizontal_si.fMask = SIF_RANGE | SIF_PAGE;
    horizontal_si.nMin = 0;
    horizontal_si.nMax = m_autosize || cx == 0 ? 0 : cx - 1;
    horizontal_si.nPage = m_autosize ? 0 : wil::rect_width(rc);
    bool b_old_show = (GetWindowLongPtr(get_wnd(), GWL_STYLE) & WS_HSCROLL) != 0;

    m_horizontal_scroll_position = SetScrollInfo(get_wnd(), SB_HORZ, &horizontal_si, redraw);

    bool b_show = (GetWindowLongPtr(get_wnd(), GWL_STYLE) & WS_HSCROLL) != 0;

    if (m_horizontal_scroll_position != old_scroll_position) {
        invalidate_all();
        reposition_header();
    }

    if (b_old_show != b_show && m_thin_scrollbar_active) {
        // 水平滑軌出現或消失會改變可視高度，垂直的頁高要跟著變
        m_vscroll_page = get_items_viewport_height();
        m_scroll_position = clamp_scroll(ScrollAxis::Vertical, m_scroll_position);
        update_thin_overlay();
    } else if (b_old_show != b_show) {
        rc = get_items_rect();

        SCROLLINFO vertical_si{};
        vertical_si.cbSize = sizeof(SCROLLINFO);
        vertical_si.fMask = SIF_PAGE;
        vertical_si.nPage = get_items_viewport_height();

        m_scroll_position = SetScrollInfo(get_wnd(), SB_VERT, &vertical_si, redraw);
    }
}

void ListView::update_scroll_info(
    bool b_vertical, bool b_horizontal, bool redraw, std::optional<int> new_vertical_position)
{
    // Scroll bar updates can cause WM_SIZE (if one of the scroll bars was
    // shown or hidden), leading to recursive scroll bar updates and
    // misbehaviour
    if (m_scroll_bar_update_in_progress)
        return;

    m_scroll_bar_update_in_progress = true;
    auto _ = gsl::finally([this] { m_scroll_bar_update_in_progress = false; });

    // Note the dependency between the two scroll bars – showing one can cause the other to be required

    if (b_vertical)
        update_vertical_scroll_info(redraw, new_vertical_position);

    if (b_horizontal)
        update_horizontal_scroll_info(redraw);
}

unsigned ListView::calculate_scroll_timer_speed() const
{
    const auto num_visible_items = get_item_area_height() / get_item_height();
    const auto divisor = (std::max)(num_visible_items - 5, 1);
    return std::clamp(25 * 15 / divisor, 10, 100);
}

void ListView::create_timer_scroll_up()
{
    if (!m_timer_scroll_up) {
        SetTimer(get_wnd(), TIMER_SCROLL_UP, calculate_scroll_timer_speed(), nullptr);
        m_timer_scroll_up = true;
    }
}

void ListView::create_timer_scroll_down()
{
    if (!m_timer_scroll_down) {
        SetTimer(get_wnd(), TIMER_SCROLL_DOWN, calculate_scroll_timer_speed(), nullptr);
        m_timer_scroll_down = true;
    }
}
void ListView::destroy_timer_scroll_up()
{
    if (m_timer_scroll_up) {
        KillTimer(get_wnd(), TIMER_SCROLL_UP);
        m_timer_scroll_up = false;
    }
}
void ListView::destroy_timer_scroll_down()
{
    if (m_timer_scroll_down) {
        KillTimer(get_wnd(), TIMER_SCROLL_DOWN);
        m_timer_scroll_down = false;
    }
}

void ListView::configure_autoscroll(AutoscrollCursorInfo cursor_info)
{
    m_autoscroll_helper.emplace(cursor_info, MSG_AUTOSCROLL_TICK, m_use_dark_mode, [this](int delta_x, int delta_y) {
        if (delta_y != 0)
            delta_scroll(delta_y, ScrollAxis::Vertical, true);

        if (delta_x != 0)
            delta_scroll(delta_x, ScrollAxis::Horizontal, true);
    });
}

// ======================================================================================================
// 細滑軌（自編版新增，2026-11-08）
// 預覽樣式 2：右邊緣一條很細的滑軌（平常 4 px、滑鼠靠近右邊緣 16 px 內變 10 px），不占版面，
// 顏色用清單文字色加透明度（所以深色、淺色都是中性灰）。垂直範圍／頁高／位置由這個類別自己存，視窗沒有 WS_VSCROLL，
// 滾輪、鍵盤、平滑捲動都沿用原本的流程（只是「讀原生滑軌資訊」的幾個地方改走 get_scroll_info／clamp_scroll）。
//
// 滑軌本身畫在一個獨立的透明浮層子視窗（WS_EX_LAYERED、逐像素 alpha，由 DWM 合成），不畫在清單自己的畫面上：
// 第一版畫在清單上，清單捲動（ScrollWindowEx）會把舊的滑軌像素一起捲走、重繪又晚一步，快速拖曳時看得到鬼影與閃動（使用者回報）；
// 浮層不屬於清單的畫面，清單怎麼捲、怎麼重繪都碰不到它。整條軌道（16 DIP 寬）都是 alpha＝1 的「看不見但吃得到滑鼠」的像素，
// 所以滑鼠靠近、按下、拖曳都由浮層自己接，再呼叫清單的捲動函式。靠近時不再鋪軌道底色（使用者覺得灰底不好看）。
// ======================================================================================================

namespace {

// 單位是 96 DPI 的像素，執行時依系統縮放換算
constexpr int thin_strip_width = 16;
constexpr int thin_thumb_right_inset = 3;
constexpr int thin_thumb_rest_width = 4;
constexpr int thin_thumb_near_width = 10;
constexpr int thin_thumb_min_height = 36;

// 不透明度（0～255）：平常 .24、靠近 .42、滑到滑塊上或拖曳中 .62
constexpr BYTE thin_alpha_rest = 61;
constexpr BYTE thin_alpha_near = 107;
constexpr BYTE thin_alpha_hot = 158;

// 按住軌道空白處連續翻頁：第一次延遲 400 ms，之後每 50 ms 翻一頁（跟原生滑軌的手感接近）
constexpr UINT_PTR thin_repeat_timer_id = 1;
constexpr UINT thin_repeat_first_delay_ms = 400;
constexpr UINT thin_repeat_interval_ms = 50;

constexpr wchar_t thin_overlay_class_name[] = L"NocturneThinScrollbarOverlay";

HINSTANCE get_this_module()
{
    HMODULE module{};
    GetModuleHandleEx(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCWSTR>(&get_this_module), &module);
    return module;
}

// 視窗類別只登記一次，模組卸載時（靜態物件解構）才取消登記
class ThinOverlayWindowClass {
public:
    explicit ThinOverlayWindowClass(WNDPROC proc)
    {
        WNDCLASSEX window_class{};
        window_class.cbSize = sizeof(window_class);
        window_class.lpfnWndProc = proc;
        window_class.hInstance = get_this_module();
        window_class.hCursor = LoadCursor(nullptr, IDC_ARROW);
        window_class.lpszClassName = thin_overlay_class_name;
        m_atom = RegisterClassEx(&window_class);
    }

    ~ThinOverlayWindowClass()
    {
        if (m_atom)
            UnregisterClass(MAKEINTATOM(m_atom), get_this_module());
    }

    ThinOverlayWindowClass(const ThinOverlayWindowClass&) = delete;
    ThinOverlayWindowClass& operator=(const ThinOverlayWindowClass&) = delete;

    [[nodiscard]] bool is_registered() const { return m_atom != 0; }

private:
    ATOM m_atom{};
};

// 在預乘 ARGB 的緩衝區裡畫一個兩端全圓的膠囊（邊緣用涵蓋率做抗鋸齒）；涵蓋率為 0 的像素保留原值（alpha＝1 的吃滑鼠像素）
void draw_pill(uint32_t* pixels, int buffer_width, const RECT& rc, COLORREF colour, BYTE alpha)
{
    const auto width = rc.right - rc.left;
    const auto height = rc.bottom - rc.top;

    if (width <= 0 || height <= 0 || alpha == 0)
        return;

    const auto red = GetRValue(colour);
    const auto green = GetGValue(colour);
    const auto blue = GetBValue(colour);

    const auto half_width = static_cast<float>(width) * .5f;
    const auto half_height = static_cast<float>(height) * .5f;
    const auto radius = std::min(half_width, half_height);

    for (int y = 0; y < height; ++y) {
        const auto py = std::abs(static_cast<float>(y) + .5f - half_height) - (half_height - radius);

        for (int x = 0; x < width; ++x) {
            const auto px = std::abs(static_cast<float>(x) + .5f - half_width) - (half_width - radius);
            // 圓角矩形的有號距離：邊界內為負；離邊界半個像素以內做線性過渡
            const auto distance
                = std::hypot(std::max(px, 0.f), std::max(py, 0.f)) + std::min(std::max(px, py), 0.f) - radius;
            const auto coverage = std::clamp(.5f - distance, 0.f, 1.f);
            const auto a = static_cast<uint32_t>(std::lround(alpha * coverage));

            if (a == 0)
                continue;

            // UpdateLayeredWindow 要的是預乘的 ARGB
            pixels[static_cast<size_t>(rc.top + y) * buffer_width + rc.left + x]
                = (a << 24) | (((red * a + 127) / 255) << 16) | (((green * a + 127) / 255) << 8) | ((blue * a + 127) / 255);
        }
    }
}

} // namespace

LRESULT CALLBACK ListView::s_thin_overlay_proc(HWND wnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_MOUSEACTIVATE)
        return MA_NOACTIVATE;

    const auto list_view = reinterpret_cast<ListView*>(GetWindowLongPtr(wnd, GWLP_USERDATA));

    if (list_view)
        return list_view->on_thin_overlay_message(wnd, msg, wp, lp);

    return DefWindowProc(wnd, msg, wp, lp);
}

void ListView::set_thin_scrollbar_allowed(bool allowed)
{
    m_thin_scrollbar_allowed = allowed;
    apply_thin_scrollbar_state();
}

void ListView::s_set_thin_scrollbar_enabled(bool enabled)
{
    if (s_thin_scrollbar_enabled == enabled)
        return;

    s_thin_scrollbar_enabled = enabled;

    // 複製一份再逐一套用（套用過程會重算捲動，不要邊走邊改到原本的清單）
    const auto list_views = s_list_views;

    for (auto* list_view : list_views)
        list_view->apply_thin_scrollbar_state();
}

void ListView::apply_thin_scrollbar_state()
{
    const auto wnd = get_wnd();
    const auto wanted = m_thin_scrollbar_allowed && s_thin_scrollbar_enabled && wnd != nullptr;

    if (wanted == m_thin_scrollbar_active)
        return;

    end_thin_scrollbar_drag(true);

    m_thin_scrollbar_active = wanted;
    m_thin_near = false;
    m_thin_thumb_hot = false;

    // 關掉就把浮層拿掉（再開時會重建）
    if (!wanted)
        destroy_thin_overlay();

    // 視窗還在建立中（WM_CREATE 還沒走完）：只記下模式，結尾的 update_scroll_info 會用新模式算
    if (!m_initialised)
        return;

    if (wanted) {
        // 先把原生垂直滑軌拿掉（改回原生時 SetScrollInfo 會自己把它叫出來）
        const auto style = GetWindowLongPtr(wnd, GWL_STYLE);

        if (style & WS_VSCROLL) {
            SetWindowLongPtr(wnd, GWL_STYLE, style & ~static_cast<LONG_PTR>(WS_VSCROLL));
            SetWindowPos(wnd, nullptr, 0, 0, 0, 0,
                SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
        }
    }

    update_scroll_info(true, true, true);
    on_size();
    invalidate_all();
}

bool ListView::is_vertical_scrollable() const
{
    if (m_thin_scrollbar_active)
        return m_vscroll_page > 0 && static_cast<int64_t>(m_vscroll_max) + 1 > m_vscroll_page;

    return (GetWindowLongPtr(get_wnd(), GWL_STYLE) & WS_VSCROLL) != 0;
}

SCROLLINFO ListView::get_scroll_info(ScrollAxis axis) const
{
    SCROLLINFO si{};
    si.cbSize = sizeof(SCROLLINFO);
    si.fMask = SIF_ALL;

    if (axis == ScrollAxis::Vertical && m_thin_scrollbar_active) {
        si.nMin = 0;
        si.nMax = m_vscroll_max;
        si.nPage = static_cast<UINT>(std::max(m_vscroll_page, 0));
        si.nPos = m_scroll_position;
        si.nTrackPos = m_scroll_position;
        return si;
    }

    GetScrollInfo(get_wnd(), scroll_axis_to_win32_type(axis), &si);
    return si;
}

int ListView::clamp_scroll(ScrollAxis axis, int position) const
{
    if (axis == ScrollAxis::Vertical && m_thin_scrollbar_active) {
        // 跟 Win32 一樣：最大位置＝最大值 − 頁高 + 1
        const auto adjusted_max = std::max(0, m_vscroll_max - m_vscroll_page + (m_vscroll_page > 0 ? 1 : 0));
        return std::clamp(position, 0, adjusted_max);
    }

    return clamp_scroll_position(get_wnd(), axis, position);
}

std::optional<ListView::ThinScrollbarGeometry> ListView::get_thin_scrollbar_geometry() const
{
    if (!m_thin_scrollbar_active || !m_initialised || !is_vertical_scrollable())
        return std::nullopt;

    const auto items_rect = get_items_rect();
    const int track_height = static_cast<int>(wil::rect_height(items_rect));

    if (track_height <= 0)
        return std::nullopt;

    const auto total = static_cast<int64_t>(m_vscroll_max) + 1;

    ThinScrollbarGeometry geometry{};
    geometry.track_top = items_rect.top;
    geometry.track_height = track_height;
    geometry.position_range = static_cast<int>(total - m_vscroll_page);

    const auto proportional_height = static_cast<int>(static_cast<int64_t>(track_height) * m_vscroll_page / total);
    geometry.thumb_height
        = std::clamp(proportional_height, std::min(scale_dpi_value(thin_thumb_min_height), track_height), track_height);

    // 拖曳中滑塊跟著目標位置（平滑捲動還在追的時候，畫在實際位置會慢半拍）
    const auto position = std::clamp((m_thin_dragging || m_thin_track_holding) && m_smooth_scroll_helper
            ? m_smooth_scroll_helper->current_target(ScrollAxis::Vertical)
            : m_scroll_position,
        0, geometry.position_range);

    const auto room = track_height - geometry.thumb_height;
    const auto offset = room > 0 ? static_cast<int>(static_cast<int64_t>(room) * position / geometry.position_range) : 0;

    const auto width = scale_dpi_value(m_thin_near || m_thin_dragging ? thin_thumb_near_width : thin_thumb_rest_width);
    const auto right = items_rect.right - scale_dpi_value(thin_thumb_right_inset);

    geometry.thumb = {right - width, items_rect.top + offset, right, items_rect.top + offset + geometry.thumb_height};
    geometry.strip = {items_rect.right - scale_dpi_value(thin_strip_width), items_rect.top, items_rect.right,
        items_rect.bottom};
    return geometry;
}

void ListView::destroy_thin_overlay()
{
    if (m_thin_overlay) {
        DestroyWindow(m_thin_overlay);
        m_thin_overlay = nullptr;
    }

    m_thin_overlay_shown = false;
    m_thin_tracking_leave = false;
    m_thin_track_holding = false;
}

// 依目前的捲動範圍、位置與滑鼠狀態更新浮層：沒有可捲的內容就藏起來；內容沒變就什麼都不做
void ListView::update_thin_overlay()
{
    const auto wnd = get_wnd();

    if (!wnd || !m_initialised)
        return;

    const auto geometry = m_thin_scrollbar_active ? get_thin_scrollbar_geometry() : std::nullopt;

    if (!geometry) {
        if (m_thin_overlay && m_thin_overlay_shown)
            ShowWindow(m_thin_overlay, SW_HIDE);

        m_thin_overlay_shown = false;
        return;
    }

    static const ThinOverlayWindowClass window_class(&ListView::s_thin_overlay_proc);

    if (!window_class.is_registered())
        return;

    const auto colour = render_get_colour_data().m_text;
    const BYTE alpha = m_thin_dragging || m_thin_thumb_hot ? thin_alpha_hot
        : m_thin_near                                      ? thin_alpha_near
                                                           : thin_alpha_rest;

    ThinOverlayState state{geometry->strip, geometry->thumb, colour, alpha};

    if (m_thin_overlay && m_thin_overlay_shown && state == m_thin_overlay_state)
        return;

    if (!m_thin_overlay) {
        m_thin_overlay = CreateWindowEx(WS_EX_LAYERED, thin_overlay_class_name, L"", WS_CHILD, 0, 0, 0, 0, wnd, nullptr,
            get_this_module(), nullptr);

        if (!m_thin_overlay)
            return;

        SetWindowLongPtr(m_thin_overlay, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
        m_thin_overlay_shown = false;
    }

    const auto width = static_cast<int>(wil::rect_width(state.strip));
    const auto height = static_cast<int>(wil::rect_height(state.strip));

    if (width <= 0 || height <= 0)
        return;

    // 位置或大小變了（或第一次顯示）才動視窗；放最上層，免得被後來建立的兄弟視窗（例如表頭）蓋住
    if (!m_thin_overlay_shown || !EqualRect(&state.strip, &m_thin_overlay_state.strip)) {
        SetWindowPos(m_thin_overlay, HWND_TOP, state.strip.left, state.strip.top, width, height,
            SWP_NOACTIVATE | SWP_SHOWWINDOW);
    }

    // 畫成預乘 ARGB：整條軌道先填 alpha＝1（看不見但吃滑鼠），再把滑塊畫上去
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = width;
    info.bmiHeader.biHeight = -height;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;

    void* bits{};
    wil::unique_hbitmap bitmap{CreateDIBSection(nullptr, &info, DIB_RGB_COLORS, &bits, nullptr, 0)};

    if (!bitmap || !bits)
        return;

    auto* pixels = static_cast<uint32_t*>(bits);
    std::fill_n(pixels, static_cast<size_t>(width) * height, 0x01000000u);

    const RECT thumb_in_overlay{state.thumb.left - state.strip.left, state.thumb.top - state.strip.top,
        state.thumb.right - state.strip.left, state.thumb.bottom - state.strip.top};
    draw_pill(pixels, width, thumb_in_overlay, state.colour, state.alpha);

    const auto screen_dc = wil::GetDC(nullptr);
    wil::unique_hdc memory_dc{CreateCompatibleDC(screen_dc.get())};

    if (!memory_dc)
        return;

    const auto _ = wil::SelectObject(memory_dc.get(), bitmap.get());

    SIZE size{width, height};
    POINT source{0, 0};
    BLENDFUNCTION blend{};
    blend.BlendOp = AC_SRC_OVER;
    blend.SourceConstantAlpha = 255;
    blend.AlphaFormat = AC_SRC_ALPHA;

    if (UpdateLayeredWindow(m_thin_overlay, screen_dc.get(), nullptr, &size, memory_dc.get(), &source, 0, &blend, ULW_ALPHA)) {
        m_thin_overlay_state = state;
        m_thin_overlay_shown = true;
    }
}

void ListView::update_thin_scrollbar_hover(POINT pt)
{
    const auto geometry = get_thin_scrollbar_geometry();

    if (!geometry)
        return;

    // 滑塊的「熱區」用靠近時的寬度算，y 在滑塊範圍內就算（不用精準點到 4 px 細條）
    const bool on_thumb = pt.y >= geometry->thumb.top && pt.y < geometry->thumb.bottom
        && pt.x >= geometry->strip.right - scale_dpi_value(thin_thumb_right_inset + thin_thumb_near_width);

    if (!m_thin_tracking_leave && m_thin_overlay) {
        TRACKMOUSEEVENT track_mouse_event{sizeof(TRACKMOUSEEVENT), TME_LEAVE, m_thin_overlay, 0};
        m_thin_tracking_leave = TrackMouseEvent(&track_mouse_event) != 0;
    }

    if (m_thin_near && on_thumb == m_thin_thumb_hot)
        return;

    m_thin_near = true;
    m_thin_thumb_hot = on_thumb;
    update_thin_overlay();
}

void ListView::drag_thin_scrollbar_to(int mouse_y)
{
    const auto geometry = get_thin_scrollbar_geometry();

    if (!geometry)
        return;

    const auto room = geometry->track_height - geometry->thumb_height;

    if (room <= 0)
        return;

    const auto wanted_top = std::clamp(mouse_y - m_thin_drag_grab - geometry->track_top, 0, room);
    const auto position = static_cast<int>(std::llround(
        static_cast<double>(wanted_top) * static_cast<double>(geometry->position_range) / static_cast<double>(room)));

    // 跟原生滑塊拖曳一樣走 50 ms 的平滑捲動
    absolute_scroll(position, ScrollAxis::Vertical, false, 50.ms);
    update_thin_overlay();
}

// 計時器每次響：游標還在軌道上、而且在滑塊上方或下方，就再翻一頁；滑塊追到游標底下就先不翻（游標移開滑塊又會繼續）
void ListView::repeat_thin_track_page()
{
    const auto geometry = get_thin_scrollbar_geometry();

    if (!geometry || !get_wnd())
        return;

    POINT pt{};
    GetCursorPos(&pt);
    ScreenToClient(get_wnd(), &pt);

    if (!PtInRect(&geometry->strip, pt))
        return;

    if (pt.y < geometry->thumb.top)
        scroll_from_scroll_bar(SB_PAGEUP);
    else if (pt.y >= geometry->thumb.bottom)
        scroll_from_scroll_bar(SB_PAGEDOWN);

    update_thin_overlay();
}

void ListView::end_thin_track_hold()
{
    if (!m_thin_track_holding)
        return;

    m_thin_track_holding = false;

    if (m_thin_overlay) {
        KillTimer(m_thin_overlay, thin_repeat_timer_id);

        if (GetCapture() == m_thin_overlay)
            ReleaseCapture();
    }

    update_thin_overlay();
}

void ListView::end_thin_scrollbar_drag(bool release_capture)
{
    if (!m_thin_dragging)
        return;

    m_thin_dragging = false;

    if (release_capture && m_thin_overlay && GetCapture() == m_thin_overlay)
        ReleaseCapture();

    // 放開後依游標目前的位置決定要不要維持「靠近」的樣子
    POINT pt{};
    GetCursorPos(&pt);

    RECT overlay_rect{};
    const auto over_overlay = m_thin_overlay && m_thin_overlay_shown && GetWindowRect(m_thin_overlay, &overlay_rect)
        && PtInRect(&overlay_rect, pt);

    m_thin_near = false;
    m_thin_thumb_hot = false;

    if (over_overlay && get_wnd()) {
        ScreenToClient(get_wnd(), &pt);
        update_thin_scrollbar_hover(pt);
    }

    update_thin_overlay();
}

LRESULT ListView::on_thin_overlay_message(HWND wnd, UINT msg, WPARAM wp, LPARAM lp)
{
    // 浮層的座標換成清單視窗的座標
    const auto to_list_view = [this](LPARAM l) {
        return POINT{GET_X_LPARAM(l) + m_thin_overlay_state.strip.left, GET_Y_LPARAM(l) + m_thin_overlay_state.strip.top};
    };

    switch (msg) {
    case WM_SETCURSOR: {
        static const auto cursor = LoadCursor(nullptr, IDC_ARROW);
        SetCursor(cursor);
        return TRUE;
    }
    case WM_MOUSEMOVE: {
        const auto pt = to_list_view(lp);

        if (m_thin_dragging) {
            drag_thin_scrollbar_to(pt.y);
            return 0;
        }

        update_thin_scrollbar_hover(pt);
        return 0;
    }
    case WM_MOUSELEAVE:
        m_thin_tracking_leave = false;

        if (!m_thin_dragging && (m_thin_near || m_thin_thumb_hot)) {
            m_thin_near = false;
            m_thin_thumb_hot = false;
            update_thin_overlay();
        }

        return 0;
    case WM_LBUTTONDOWN:
    case WM_LBUTTONDBLCLK: {
        const auto pt = to_list_view(lp);
        const auto geometry = get_thin_scrollbar_geometry();

        if (!geometry)
            return 0;

        // 右邊緣這條是滑軌：不選取底下的項目、不開始框選，也不搶焦點（跟原生滑軌一樣）
        exit_inline_edit();
        hide_tooltip();
        m_thin_near = true;

        if (pt.y >= geometry->thumb.top && pt.y < geometry->thumb.bottom) {
            m_thin_dragging = true;
            m_thin_drag_grab = pt.y - geometry->thumb.top;
            SetCapture(wnd);
        } else {
            // 軌道空白處：先翻一頁，按住不放就連續翻（見 repeat_thin_track_page）
            scroll_from_scroll_bar(pt.y < geometry->thumb.top ? SB_PAGEUP : SB_PAGEDOWN);
            m_thin_track_holding = true;
            SetCapture(wnd);
            SetTimer(wnd, thin_repeat_timer_id, thin_repeat_first_delay_ms, nullptr);
        }

        update_thin_overlay();
        return 0;
    }
    case WM_TIMER:
        if (wp == thin_repeat_timer_id && m_thin_track_holding) {
            SetTimer(wnd, thin_repeat_timer_id, thin_repeat_interval_ms, nullptr);
            repeat_thin_track_page();
            return 0;
        }

        return DefWindowProc(wnd, msg, wp, lp);
    case WM_LBUTTONUP:
        end_thin_scrollbar_drag(true);
        end_thin_track_hold();
        return 0;
    case WM_CAPTURECHANGED:
    case WM_CANCELMODE:
        end_thin_scrollbar_drag(false);
        end_thin_track_hold();
        return 0;
    default:
        return DefWindowProc(wnd, msg, wp, lp);
    }
}

} // namespace uih
