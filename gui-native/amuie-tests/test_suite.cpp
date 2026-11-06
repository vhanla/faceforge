#include "core/arena.hpp"
#include "core/theme.hpp"
#include "core/types.hpp"
#include "widgets/ui_widgets.hpp"

#include <cassert>
#include <cstdio>
#include <cstring>

static void test_click_lifecycle() {
    std::printf("Testing click start/release lifecycle...\n");

    Arena arena = Arena::create(1024 * 1024);
    UIContext ui;
    ui.screen_w = 800;
    ui.screen_h = 600;

    Rect btn_rect{100, 100, 120, 40};

    // Case 1: Standard click: press inside, release inside -> triggers click
    {
        // Frame 1: mouse down inside button
        ui.mouse_x = 110;
        ui.mouse_y = 110;
        ui.mouse_left_down = true;
        ui.mouse_left_clicked = true;
        ui.mouse_left_released = false;
        ui.first_command = nullptr;
        ui.last_command = nullptr;

        bool clicked = ui_button_raw(&arena, &ui, btn_rect, "OK", Color{50, 50, 50, 255}, 4.0f);
        assert(!clicked); // Default trigger is on release
        (void)clicked;
        assert(ui.active_id != 0); // Button captured active_id
        ui_end_frame(&arena, &ui);
        arena.reset();

        // Frame 2: mouse still held down inside
        ui.mouse_left_clicked = false;
        ui.mouse_left_down = true;
        clicked = ui_button_raw(&arena, &ui, btn_rect, "OK", Color{50, 50, 50, 255}, 4.0f);
        assert(!clicked);
        (void)clicked;
        ui_end_frame(&arena, &ui);
        arena.reset();

        // Frame 3: mouse released inside
        ui.mouse_left_down = false;
        ui.mouse_left_released = true;
        clicked = ui_button_raw(&arena, &ui, btn_rect, "OK", Color{50, 50, 50, 255}, 4.0f);
        assert(clicked); // CLICKED!
        (void)clicked;
        ui_end_frame(&arena, &ui);
        assert(ui.active_id == 0); // Active ID cleared after release
        arena.reset();
    }

    // Case 2: Mouse down OUTSIDE, dragged inside -> button must NOT receive down or click!
    {
        // Frame 1: click on empty space (20, 20)
        ui.mouse_x = 20;
        ui.mouse_y = 20;
        ui.mouse_left_down = true;
        ui.mouse_left_clicked = true;
        ui.mouse_left_released = false;
        ui.first_command = nullptr;
        ui.last_command = nullptr;

        bool clicked = ui_button_raw(&arena, &ui, btn_rect, "OK", Color{50, 50, 50, 255}, 4.0f);
        assert(!clicked);
        (void)clicked;
        ui_end_frame(&arena, &ui);
        assert(ui.active_id == UI_UNCLAIMED_ID); // Nobody claimed active_id
        arena.reset();

        // Frame 2: user drags cursor over button (110, 110) while mouse is still held down
        ui.mouse_x = 110;
        ui.mouse_y = 110;
        ui.mouse_left_clicked = false;
        ui.mouse_left_down = true;
        uint64_t id = ui_get_id(&ui, "OK", (uint64_t)(btn_rect.x * 1000.0f + btn_rect.y));
        UIInteraction inter = ui_process_interaction(&ui, id, btn_rect);
        assert(!inter.pressed);  // NOT pressed!
        assert(!inter.down);     // NOT down!
        assert(!inter.hovered);  // NOT hovered because another ID captured!
        (void)inter;
        ui_end_frame(&arena, &ui);
        arena.reset();

        // Frame 3: user releases mouse over button
        ui.mouse_left_down = false;
        ui.mouse_left_released = true;
        clicked = ui_button_raw(&arena, &ui, btn_rect, "OK", Color{50, 50, 50, 255}, 4.0f);
        assert(!clicked); // Must NOT click!
        (void)clicked;
        ui_end_frame(&arena, &ui);
        assert(ui.active_id == 0);
        arena.reset();
    }

    // Case 3: Mouse down INSIDE, dragged outside -> released outside -> cancelled (no click)
    {
        // Frame 1: click inside
        ui.mouse_x = 110;
        ui.mouse_y = 110;
        ui.mouse_left_down = true;
        ui.mouse_left_clicked = true;
        ui.mouse_left_released = false;
        ui_button_raw(&arena, &ui, btn_rect, "OK", Color{50, 50, 50, 255}, 4.0f);
        assert(ui.active_id != 0);
        ui_end_frame(&arena, &ui);
        arena.reset();

        // Frame 2: dragged outside to (500, 500)
        ui.mouse_x = 500;
        ui.mouse_y = 500;
        ui.mouse_left_clicked = false;
        ui.mouse_left_down = true;
        uint64_t id = ui_get_id(&ui, "OK", (uint64_t)(btn_rect.x * 1000.0f + btn_rect.y));
        UIInteraction inter = ui_process_interaction(&ui, id, btn_rect);
        assert(!inter.pressed); // Cursor outside -> not pressed
        assert(inter.dragging); // But still active/dragging
        (void)inter;
        ui_end_frame(&arena, &ui);
        arena.reset();

        // Frame 3: released outside
        ui.mouse_left_down = false;
        ui.mouse_left_released = true;
        bool clicked = ui_button_raw(&arena, &ui, btn_rect, "OK", Color{50, 50, 50, 255}, 4.0f);
        assert(!clicked); // CANCELLED!
        (void)clicked;
        ui_end_frame(&arena, &ui);
        assert(ui.active_id == 0);
        arena.reset();
    }

    // Case 4: Mouse down INSIDE, dragged outside, dragged back INSIDE, released inside -> triggers click!
    {
        // Frame 1: down inside
        ui.mouse_x = 110;
        ui.mouse_y = 110;
        ui.mouse_left_down = true;
        ui.mouse_left_clicked = true;
        ui.mouse_left_released = false;
        ui_button_raw(&arena, &ui, btn_rect, "OK", Color{50, 50, 50, 255}, 4.0f);
        assert(ui.active_id != 0);
        ui_end_frame(&arena, &ui);
        arena.reset();

        // Frame 2: dragged outside
        ui.mouse_x = 500;
        ui.mouse_y = 500;
        ui.mouse_left_clicked = false;
        ui.mouse_left_down = true;
        ui_button_raw(&arena, &ui, btn_rect, "OK", Color{50, 50, 50, 255}, 4.0f);
        ui_end_frame(&arena, &ui);
        arena.reset();

        // Frame 3: dragged back inside
        ui.mouse_x = 115;
        ui.mouse_y = 115;
        uint64_t id = ui_get_id(&ui, "OK", (uint64_t)(btn_rect.x * 1000.0f + btn_rect.y));
        UIInteraction inter = ui_process_interaction(&ui, id, btn_rect);
        assert(inter.pressed); // Pressed again!
        (void)inter;
        ui_end_frame(&arena, &ui);
        arena.reset();

        // Frame 4: released inside
        ui.mouse_left_down = false;
        ui.mouse_left_released = true;
        bool clicked = ui_button_raw(&arena, &ui, btn_rect, "OK", Color{50, 50, 50, 255}, 4.0f);
        assert(clicked); // CLICKED!
        (void)clicked;
        ui_end_frame(&arena, &ui);
        arena.reset();
    }

    // Case 5: Immediate trigger (UIClickTrigger::OnDown)
    {
        ui.mouse_x = 110;
        ui.mouse_y = 110;
        ui.mouse_left_down = true;
        ui.mouse_left_clicked = true;
        ui.mouse_left_released = false;

        bool clicked = ui_button_icon_text_ex(&arena, &ui, btn_rect, 0, "Fast",
                                              Color{50, 50, 50, 255}, 4.0f,
                                              0.0f, 14.0f, false, Color{}, Color{}, 0.0f,
                                              UIBehavior{ .trigger = UIClickTrigger::OnDown });
        assert(clicked); // Fires immediately on down!
        (void)clicked;
        ui_end_frame(&arena, &ui);
        arena.reset();

        // Release frame should not fire again
        ui.mouse_left_down = false;
        ui.mouse_left_released = true;
        ui.mouse_left_clicked = false;
        clicked = ui_button_icon_text_ex(&arena, &ui, btn_rect, 0, "Fast",
                                         Color{50, 50, 50, 255}, 4.0f,
                                         0.0f, 14.0f, false, Color{}, Color{}, 0.0f,
                                         UIBehavior{ .trigger = UIClickTrigger::OnDown });
        assert(!clicked);
        (void)clicked;
        ui_end_frame(&arena, &ui);
        arena.reset();
    }

    arena.destroy();
    std::printf("  -> Click lifecycle tests PASSED!\n");
}

static void test_combobox() {
    std::printf("Testing combobox functional dropdown...\n");

    Arena arena = Arena::create(1024 * 1024);
    UIContext ui;
    ui.screen_w = 800;
    ui.screen_h = 600;

    Rect combo_rect{100, 100, 160, 32};
    static const char* kItems[] = {"Option A", "Option B", "Option C"};
    int selected = 0;

    // Step 1: Click combo box to open it
    ui.mouse_x = 120;
    ui.mouse_y = 110;
    ui.mouse_left_down = true;
    ui.mouse_left_clicked = true;
    ui.mouse_left_released = false;
    ui_combo(&arena, &ui, combo_rect, kItems, 3, &selected, nullptr);
    ui_end_frame(&arena, &ui);
    arena.reset();

    ui.mouse_left_down = false;
    ui.mouse_left_clicked = false;
    ui.mouse_left_released = true;
    ui_combo(&arena, &ui, combo_rect, kItems, 3, &selected, nullptr);
    assert(ui.active_popup_id != 0); // Dropdown opened!
    assert(ui.active_popup_rect.w == combo_rect.w);
    ui_end_frame(&arena, &ui);
    arena.reset();

    // Step 2: Hover and click second item ("Option B" at index 1)
    // Item 1 is at y = popup_y + 4 + 1 * 28 = 100 + 32 + 3 + 4 + 28 = 167
    float item1_y = ui.active_popup_rect.y + 4.0f + 1 * 28.0f + 10.0f;
    ui.mouse_x = 120;
    ui.mouse_y = item1_y;
    ui.mouse_left_down = true;
    ui.mouse_left_clicked = true;
    ui.mouse_left_released = false;
    ui_combo(&arena, &ui, combo_rect, kItems, 3, &selected, nullptr);
    ui_end_frame(&arena, &ui);
    arena.reset();

    ui.mouse_left_down = false;
    ui.mouse_left_clicked = false;
    ui.mouse_left_released = true;
    bool changed = ui_combo(&arena, &ui, combo_rect, kItems, 3, &selected, nullptr);
    assert(changed); // Item selected!
    (void)changed;
    assert(selected == 1); // Selected "Option B"!
    assert(ui.active_popup_id == 0); // Popup closed automatically!
    ui_end_frame(&arena, &ui);
    arena.reset();

    // Step 3: Reopen and click outside to dismiss
    ui.mouse_x = 120;
    ui.mouse_y = 110;
    ui.mouse_left_down = true;
    ui.mouse_left_clicked = true;
    ui.mouse_left_released = false;
    ui_combo(&arena, &ui, combo_rect, kItems, 3, &selected, nullptr);
    ui_end_frame(&arena, &ui);
    arena.reset();
    ui.mouse_left_down = false;
    ui.mouse_left_released = true;
    ui_combo(&arena, &ui, combo_rect, kItems, 3, &selected, nullptr);
    assert(ui.active_popup_id != 0); // Reopened
    ui_end_frame(&arena, &ui);
    arena.reset();

    // Click outside at (500, 500)
    ui.mouse_x = 500;
    ui.mouse_y = 500;
    ui.mouse_left_down = true;
    ui.mouse_left_clicked = true;
    ui.mouse_left_released = false;
    // main loop dismiss logic:
    if (ui.active_popup_id != 0 && (ui.mouse_left_clicked || ui.mouse_right_clicked)) {
        if (!ui_hovered(&ui, ui.active_popup_rect)) {
            ui.active_popup_id = 0;
            ui.active_popup_rect = Rect{};
            ui.mouse_left_clicked = false;
            ui.active_id = UI_UNCLAIMED_ID;
        }
    }
    assert(ui.active_popup_id == 0); // Cleanly dismissed!
    ui_end_frame(&arena, &ui);

    arena.destroy();
    std::printf("  -> Combobox tests PASSED!\n");
}

static void test_context_menu() {
    std::printf("Testing context menu...\n");

    Arena arena = Arena::create(1024 * 1024);
    UIContext ui;
    ui.screen_w = 800;
    ui.screen_h = 600;

    Rect canvas_rect{50, 50, 400, 300};
    uint64_t menu_id = ui_hash_str("test_menu");

    static const MenuItem kItems[] = {
        { .label = "Copy", .shortcut = "Ctrl+C" },
        { .label = "Paste", .shortcut = "Ctrl+V" },
        { .separator = true },
        { .label = "Delete" }
    };

    // Step 1: Right-click inside canvas area
    ui.mouse_x = 200;
    ui.mouse_y = 150;
    ui.mouse_right_down = true;
    ui.mouse_right_clicked = true;
    ui.mouse_right_released = false;
    bool opened = ui_context_menu_area(&ui, menu_id, canvas_rect);
    ui_end_frame(&arena, &ui);
    arena.reset();

    ui.mouse_right_down = false;
    ui.mouse_right_clicked = false;
    ui.mouse_right_released = true;
    opened = ui_context_menu_area(&ui, menu_id, canvas_rect);
    assert(opened); // Right-click opened menu!
    (void)opened;
    assert(ui.active_popup_id == menu_id);
    assert(ui.menu_x == 200);
    assert(ui.menu_y == 150);

    // Call ui_context_menu to layout popup and commands
    int clicked_item = ui_context_menu(&arena, &ui, menu_id, kItems, 4, nullptr);
    assert(clicked_item == -1); // Nothing clicked yet
    assert(ui.popup_first_command != nullptr); // Render commands in popup layer!
    ui_end_frame(&arena, &ui);
    arena.reset();

    // Step 2: Click on item 1 ("Paste")
    // Item 1 y = menu_y + 4 (pad) + 28 (item 0) + 14 = 150 + 4 + 28 + 14 = 196
    ui.mouse_x = 220;
    ui.mouse_y = 196;
    ui.mouse_left_down = true;
    ui.mouse_left_clicked = true;
    ui.mouse_left_released = false;
    clicked_item = ui_context_menu(&arena, &ui, menu_id, kItems, 4, nullptr);
    ui_end_frame(&arena, &ui);
    arena.reset();

    ui.mouse_left_down = false;
    ui.mouse_left_clicked = false;
    ui.mouse_left_released = true;
    clicked_item = ui_context_menu(&arena, &ui, menu_id, kItems, 4, nullptr);
    assert(clicked_item == 1); // Selected "Paste"!
    (void)clicked_item;
    assert(ui.active_popup_id == 0); // Closed!
    ui_end_frame(&arena, &ui);

    arena.destroy();
    std::printf("  -> Context menu tests PASSED!\n");
}

int main() {
    std::printf("=== AMUIE INTERACTION & POPUP TEST SUITE ===\n");
    test_click_lifecycle();
    test_combobox();
    test_context_menu();
    std::printf("=== ALL TESTS PASSED SUCCESSFULLY! ===\n");
    return 0;
}
