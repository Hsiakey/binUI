// enum_windows.cpp - 枚举可见窗口标题，调试用
#include <cstdio>
#include <windows.h>

static BOOL CALLBACK EnumProc(HWND hwnd, LPARAM) {
    if (!IsWindowVisible(hwnd)) return TRUE;
    char t[256];
    GetWindowTextA(hwnd, t, sizeof(t));
    if (t[0])
        printf("hwnd=%p title=\"%s\" style=0x%08lX\n", (void*)hwnd, t, GetWindowLongA(hwnd, GWL_STYLE));
    return TRUE;
}

int main() {
    EnumWindows(EnumProc, 0);
    printf("--- done ---\n");
    return 0;
}
