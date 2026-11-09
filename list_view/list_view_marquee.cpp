#include "stdafx.h"

#include "list_view.h"

namespace uih {

// ======================================================================================================
// 框選方框（自編版新增，2026-11-09；使用者要求「像 Windows 原生那樣壓住左鍵拉出方形的反白」）
// - 在沒選取的列上壓住拖：跟原本一樣從那一列選到滑鼠所在的列，只是多畫出方框
// - 在清單下面的空白處壓住拖：開始框選（原本只會清掉選取）
// - 按著 Ctrl 拖：方框掃到的列選取狀態反過來，以按下前的選取為底（檔案總管的做法）
// - 在已選取的列上壓住拖：維持拖放，不經過這裡
// 每一列都佔滿清單寬度，所以只看方框的上下範圍決定選到哪幾列。
// 顏色照 Windows 原生（從檔案總管截圖量過）：邊框 1 px＝COLOR_HIGHLIGHT，填色＝COLOR_HOTLIGHT 疊約 33％。
// 起點存「清單內容座標」：拖到上下邊緣自動捲動時，方框起點跟著內容走、游標那一端留在游標上
// ======================================================================================================

namespace {

// 255 × 1/3：從截圖反推，填色三個色版都對得上這個比例
constexpr BYTE marquee_fill_alpha = 85;

} // namespace

bool ListView::can_start_marquee() const
{
    return s_marquee_enabled && m_marquee_allowed && m_selection_mode == SelectionMode::Multiple;
}

void ListView::begin_marquee(POINT pt_client, bool toggle)
{
    const auto items_rect = get_items_rect();

    m_marquee_tracking = true;
    m_marquee_shown = false;
    m_marquee_toggle = toggle;
    m_marquee_initial_client = pt_client;
    m_marquee_current_client = pt_client;
    m_marquee_anchor = {pt_client.x + m_horizontal_scroll_position, pt_client.y - items_rect.top + m_scroll_position};
    m_marquee_first = 1;
    m_marquee_last = 0;
    m_marquee_base_selection.clear();

    // Ctrl 框選要記住按下前的選取，之後每次都從這份去反轉方框內的列（方框縮回去時那些列會恢復原狀）
    if (toggle) {
        const auto count = get_item_count();
        m_marquee_base_selection.resize(count);
        for (size_t index = 0; index < count; ++index)
            m_marquee_base_selection[index] = get_item_selected(index);
    }
}

void ListView::update_marquee(POINT pt_client)
{
    if (!m_marquee_tracking)
        return;

    const auto items_rect = get_items_rect();

    // 游標拖到清單上下外面時自動捲動（沿用原本拖曳選取的計時器）
    if (pt_client.y < items_rect.top)
        create_timer_scroll_up();
    else
        destroy_timer_scroll_up();

    if (pt_client.y >= items_rect.bottom)
        create_timer_scroll_down();
    else
        destroy_timer_scroll_down();

    // 還沒超過拖曳門檻：不畫方框、不動選取（單純點一下跟原本一樣）
    if (!m_marquee_shown) {
        const auto cx_drag = std::abs(GetSystemMetrics(SM_CXDRAG));
        const auto cy_drag = std::abs(GetSystemMetrics(SM_CYDRAG));

        if (std::abs(pt_client.x - m_marquee_initial_client.x) <= cx_drag
            && std::abs(pt_client.y - m_marquee_initial_client.y) <= cy_drag)
            return;

        m_marquee_shown = true;
        hide_tooltip();
    }

    // 方框游標那一端只拉到清單邊緣（檔案總管也是這樣），超出的部分靠自動捲動
    const POINT pt{std::clamp(pt_client.x, items_rect.left, (std::max)(items_rect.left, items_rect.right - 1)),
        std::clamp(pt_client.y, items_rect.top, (std::max)(items_rect.top, items_rect.bottom - 1))};

    // 舊方框與新方框的位置都要重畫
    invalidate_marquee();
    m_marquee_current_client = pt;
    invalidate_marquee();

    // 方框的上下範圍（內容座標）
    const int anchor_y = m_marquee_anchor.y;
    const int current_y = pt.y - items_rect.top + m_scroll_position;
    const int range_top = (std::min)(anchor_y, current_y);
    const int range_bottom = (std::max)(anchor_y, current_y);

    // 找出跟方框上下範圍有重疊的列；列的位置由上往下遞增，用二分搜尋（大清單每次移動才不會整份掃一遍）
    const auto count = get_item_count();

    size_t low = 0;
    size_t high = count;
    while (low < high) {
        const auto middle = low + (high - low) / 2;
        if (get_item_position_bottom(middle) > range_top)
            high = middle;
        else
            low = middle + 1;
    }
    const auto first = low;

    high = count;
    while (low < high) {
        const auto middle = low + (high - low) / 2;
        if (get_item_position(middle) > range_bottom)
            high = middle;
        else
            low = middle + 1;
    }

    const bool has_items = low > first;
    const auto new_first = has_items ? first : size_t{1};
    const auto new_last = has_items ? low - 1 : size_t{0};

    // 掃到的列沒變就不重設選取（選取變更會通知 foobar2000，別每移動一像素就發一次）
    if (new_first == m_marquee_first && new_last == m_marquee_last)
        return;

    m_marquee_first = new_first;
    m_marquee_last = new_last;

    if (m_marquee_toggle) {
        pfc::bit_array_bittable states(count);
        for (size_t index = 0; index < count; ++index) {
            const bool was_selected = index < m_marquee_base_selection.size() && m_marquee_base_selection[index];
            const bool in_marquee = has_items && index >= new_first && index <= new_last;
            states.set(index, was_selected != in_marquee);
        }
        set_selection_state(pfc::bit_array_true(), states);
    } else if (has_items) {
        set_selection_state(pfc::bit_array_true(), pfc::bit_array_range(new_first, new_last - new_first + 1));
    } else {
        set_selection_state(pfc::bit_array_true(), pfc::bit_array_false());
    }

    // 焦點放在游標那一端（跟原本拖曳選取一樣），之後 Shift＋點一下會從這裡接著選
    if (has_items)
        set_focus_item(current_y >= anchor_y ? new_last : new_first);

    m_shift_start.reset();
}

void ListView::update_marquee_from_cursor()
{
    POINT pt{};
    if (!GetCursorPos(&pt) || !ScreenToClient(get_wnd(), &pt))
        return;

    update_marquee(pt);
}

void ListView::end_marquee()
{
    if (!m_marquee_tracking)
        return;

    invalidate_marquee();

    m_marquee_tracking = false;
    m_marquee_shown = false;
    m_marquee_toggle = false;
    m_marquee_base_selection.clear();
    m_marquee_base_selection.shrink_to_fit();
    destroy_timer_scroll_up();
    destroy_timer_scroll_down();
}

RECT ListView::get_marquee_client_rect() const
{
    const auto items_rect = get_items_rect();
    const POINT anchor{
        m_marquee_anchor.x - m_horizontal_scroll_position, m_marquee_anchor.y - m_scroll_position + items_rect.top};

    // 右、下多 1：游標所在的那個像素也算在方框裡（邊框畫得到）
    return RECT{(std::min)(anchor.x, m_marquee_current_client.x), (std::min)(anchor.y, m_marquee_current_client.y),
        (std::max)(anchor.x, m_marquee_current_client.x) + 1, (std::max)(anchor.y, m_marquee_current_client.y) + 1};
}

void ListView::invalidate_marquee()
{
    if (!m_marquee_shown || !get_wnd())
        return;

    const auto items_rect = get_items_rect();
    const auto marquee_rect = get_marquee_client_rect();
    RECT invalidate_rect{};

    if (IntersectRect(&invalidate_rect, &marquee_rect, &items_rect))
        RedrawWindow(get_wnd(), &invalidate_rect, nullptr, RDW_INVALIDATE);
}

void ListView::render_marquee(HDC dc) const
{
    if (!m_marquee_shown)
        return;

    const auto items_rect = get_items_rect();
    const auto marquee_rect = get_marquee_client_rect();
    RECT visible_rect{};

    if (!IntersectRect(&visible_rect, &marquee_rect, &items_rect))
        return;

    // 填色：1×1 的單色點陣圖拉伸到方框大小，以固定透明度疊上去
    wil::unique_hdc memory_dc(CreateCompatibleDC(dc));
    wil::unique_hbitmap pixel(CreateCompatibleBitmap(dc, 1, 1));

    if (memory_dc && pixel) {
        auto _ = wil::SelectObject(memory_dc.get(), pixel.get());
        SetPixelV(memory_dc.get(), 0, 0, GetSysColor(COLOR_HOTLIGHT));

        const BLENDFUNCTION blend{AC_SRC_OVER, 0, marquee_fill_alpha, 0};
        GdiAlphaBlend(dc, visible_rect.left, visible_rect.top, wil::rect_width(visible_rect),
            wil::rect_height(visible_rect), memory_dc.get(), 0, 0, 1, 1, blend);
    }

    // 邊框：畫整個方框再裁到清單範圍內（起點捲出畫面時，那一邊就看不到，跟檔案總管一樣）
    const auto saved_dc = SaveDC(dc);
    IntersectClipRect(dc, items_rect.left, items_rect.top, items_rect.right, items_rect.bottom);

    wil::unique_hbrush border_brush(CreateSolidBrush(GetSysColor(COLOR_HIGHLIGHT)));
    FrameRect(dc, &marquee_rect, border_brush.get());

    RestoreDC(dc, saved_dc);
}

} // namespace uih
