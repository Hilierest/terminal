// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "IslandWindow.h"
#include "../types/inc/Viewport.hpp"
#include "resource.h"

extern "C" IMAGE_DOS_HEADER __ImageBase;

using namespace winrt::Windows::UI;
using namespace winrt::Windows::UI::Composition;
using namespace winrt::Windows::UI::Xaml;
using namespace winrt::Windows::UI::Xaml::Hosting;
using namespace winrt::Windows::Foundation::Numerics;
using namespace ::Microsoft::Console::Types;

#define XAML_HOSTING_WINDOW_CLASS_NAME L"CASCADIA_HOSTING_WINDOW_CLASS"

constexpr int RECT_WIDTH(const RECT* const pRect)
{
    return pRect->right - pRect->left;
}
constexpr int RECT_HEIGHT(const RECT* const pRect)
{
    return pRect->bottom - pRect->top;
}

IslandWindow::IslandWindow() noexcept :
    _interopWindowHandle{ nullptr },
    _rootGrid{ nullptr },
    _source{ nullptr },
    _pfnCreateCallback{ nullptr }
{
}

IslandWindow::~IslandWindow()
{
    _source.Close();
}

// Method Description:
// - Create the actual window that we'll use for the application.
// Arguments:
// - <none>
// Return Value:
// - <none>
void IslandWindow::MakeWindow() noexcept
{
    WNDCLASS wc{};
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hInstance = reinterpret_cast<HINSTANCE>(&__ImageBase);
    wc.lpszClassName = XAML_HOSTING_WINDOW_CLASS_NAME;
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WndProc;
    wc.hIcon = LoadIconW(wc.hInstance, MAKEINTRESOURCEW(IDI_APPICON));
    RegisterClass(&wc);
    WINRT_ASSERT(!_window);

    // Create the window with the default size here - During the creation of the
    // window, the system will give us a chance to set its size in WM_CREATE.
    // WM_CREATE will be handled synchronously, before CreateWindow returns.
    WINRT_VERIFY(CreateWindow(wc.lpszClassName,
                              L"Windows Terminal",
                              WS_OVERLAPPEDWINDOW,
                              CW_USEDEFAULT,
                              CW_USEDEFAULT,
                              CW_USEDEFAULT,
                              CW_USEDEFAULT,
                              nullptr,
                              nullptr,
                              wc.hInstance,
                              this));

    WINRT_ASSERT(_window);
}

// Method Description:
// - Called when no tab is remaining to close the window.
// Arguments:
// - <none>
// Return Value:
// - <none>
void IslandWindow::Close()
{
    PostQuitMessage(0);
}

// Method Description:
// - Set a callback to be called when we process a WM_CREATE message. This gives
//   the AppHost a chance to resize the window to the propoer size.
// Arguments:
// - pfn: a function to be called during the handling of WM_CREATE. It takes two
//   parameters:
//      * HWND: the HWND of the window that's being created.
//      * RECT: The position on the screen that the system has proposed for our
//        window.
// Return Value:
// - <none>
void IslandWindow::SetCreateCallback(std::function<void(const HWND, const RECT, winrt::TerminalApp::LaunchMode& launchMode)> pfn) noexcept
{
    _pfnCreateCallback = pfn;
}

// Method Description:
// - Handles a WM_CREATE message. Calls our create callback, if one's been set.
// Arguments:
// - wParam: unused
// - lParam: the lParam of a WM_CREATE, which is a pointer to a CREATESTRUCTW
// Return Value:
// - <none>
void IslandWindow::_HandleCreateWindow(const WPARAM, const LPARAM lParam) noexcept
{
    // Get proposed window rect from create structure
    CREATESTRUCTW* pcs = reinterpret_cast<CREATESTRUCTW*>(lParam);
    RECT rc;
    rc.left = pcs->x;
    rc.top = pcs->y;
    rc.right = rc.left + pcs->cx;
    rc.bottom = rc.top + pcs->cy;

    winrt::TerminalApp::LaunchMode launchMode = winrt::TerminalApp::LaunchMode::DefaultMode;
    if (_pfnCreateCallback)
    {
        _pfnCreateCallback(_window.get(), rc, launchMode);
    }

    int nCmdShow = SW_SHOW;
    if (launchMode == winrt::TerminalApp::LaunchMode::MaximizedMode)
    {
        nCmdShow = SW_MAXIMIZE;
    }

    ShowWindow(_window.get(), nCmdShow);
    UpdateWindow(_window.get());
}

void IslandWindow::Initialize()
{
    const bool initialized = (_interopWindowHandle != nullptr);

    _source = DesktopWindowXamlSource{};

    auto interop = _source.as<IDesktopWindowXamlSourceNative>();
    winrt::check_hresult(interop->AttachToWindow(_window.get()));

    // stash the child interop handle so we can resize it when the main hwnd is resized
    interop->get_WindowHandle(&_interopWindowHandle);

    _rootGrid = winrt::Windows::UI::Xaml::Controls::Grid();
    _source.Content(_rootGrid);
}

void IslandWindow::OnSize(const UINT width, const UINT height)
{
    // update the interop window size
    SetWindowPos(_interopWindowHandle, 0, 0, 0, width, height, SWP_SHOWWINDOW);

    if (_rootGrid)
    {
        const auto size = GetLogicalSize();
        _rootGrid.Width(size.Width);
        _rootGrid.Height(size.Height);
    }
}

[[nodiscard]] LRESULT IslandWindow::MessageHandler(UINT const message, WPARAM const wparam, LPARAM const lparam) noexcept
{
    switch (message)
    {
    case WM_CREATE:
    {
        _HandleCreateWindow(wparam, lparam);
        return 0;
    }
    case WM_SETFOCUS:
    {
        if (_interopWindowHandle != nullptr)
        {
            // TODO GitHub #2447: Properly attach WindowUiaProvider for signaling model
            /*
                // set the text area to have focus for accessibility consumers
                if (_pUiaProvider)
                {
                    LOG_IF_FAILED(_pUiaProvider->SetTextAreaFocus());
                }
                break;
            */

            // send focus to the child window
            SetFocus(_interopWindowHandle);
            return 0; // eat the message
        }
    }

    case WM_NCLBUTTONDOWN:
    case WM_NCLBUTTONUP:
    case WM_NCMBUTTONDOWN:
    case WM_NCMBUTTONUP:
    case WM_NCRBUTTONDOWN:
    case WM_NCRBUTTONUP:
    case WM_NCXBUTTONDOWN:
    case WM_NCXBUTTONUP:
    {
        // If we clicked in the titlebar, raise an event so the app host can
        // dispatch an appropriate event.
        _DragRegionClickedHandlers();
        break;
    }
    case WM_MENUCHAR:
    {
        // GH#891: return this LRESULT here to prevent the app from making a
        // bell when alt+key is pressed. A menu is active and the user presses a
        // key that does not correspond to any mnemonic or accelerator key,
        return MAKELRESULT(0, MNC_CLOSE);
    }
    case WM_CLOSE:
    {
        // If the user wants to close the app by clicking 'X' button,
        // we hand off the close experience to the app layer. If all the tabs
        // are closed, the window will be closed as well.
        _windowCloseButtonClickedHandler();
        return 0;
    }
    }

    // TODO: handle messages here...
    return base_type::MessageHandler(message, wparam, lparam);
}

// Routine Description:
// - Creates/retrieves a handle to the UI Automation provider COM interfaces
// Arguments:
// - <none>
// Return Value:
// - Pointer to UI Automation provider class/interfaces.
IRawElementProviderSimple* IslandWindow::_GetUiaProvider()
{
    if (nullptr == _pUiaProvider)
    {
        try
        {
            _pUiaProvider = WindowUiaProvider::Create(this);
        }
        catch (...)
        {
            LOG_HR(wil::ResultFromCaughtException());
            _pUiaProvider = nullptr;
        }
    }

    return _pUiaProvider;
}

RECT IslandWindow::GetFrameBorderMargins(unsigned int currentDpi)
{
    const auto windowStyle = GetWindowStyle(_window.get());
    const auto targetStyle = windowStyle & ~WS_DLGFRAME;
    RECT frame{};
    AdjustWindowRectExForDpi(&frame, targetStyle, false, GetWindowExStyle(_window.get()), currentDpi);
    return frame;
}

// Method Description:
// - Called when the window has been resized (or maximized)
// Arguments:
// - width: the new width of the window _in pixels_
// - height: the new height of the window _in pixels_
void IslandWindow::OnResize(const UINT width, const UINT height)
{
    if (_interopWindowHandle)
    {
        OnSize(width, height);
    }
}

// Method Description:
// - Called when the window is minimized to the taskbar.
void IslandWindow::OnMinimize()
{
    // TODO GH#1989 Stop rendering island content when the app is minimized.
}

// Method Description:
// - Called when the window is restored from having been minimized.
void IslandWindow::OnRestore()
{
    // TODO GH#1989 Stop rendering island content when the app is minimized.
}

void IslandWindow::SetContent(winrt::Windows::UI::Xaml::UIElement content)
{
    _rootGrid.Children().Clear();
    _rootGrid.Children().Append(content);
}

void IslandWindow::OnAppInitialized()
{
    // Do a quick resize to force the island to paint
    const auto size = GetPhysicalSize();
    OnSize(size.cx, size.cy);
}

// Method Description:
// - Called when the app wants to change its theme. We'll update the root UI
//   element of the entire XAML tree, so that all UI elements get the theme
//   applied.
// Arguments:
// - arg: the ElementTheme to use as the new theme for the UI
// Return Value:
// - <none>
void IslandWindow::UpdateTheme(const winrt::Windows::UI::Xaml::ElementTheme& requestedTheme)
{
    _rootGrid.RequestedTheme(requestedTheme);
    // Invalidate the window rect, so that we'll repaint any elements we're
    // drawing ourselves to match the new theme
    ::InvalidateRect(_window.get(), nullptr, false);
}

void IslandWindow::ToggleFullscreen()
{
    SetIsFullscreen(!_fullscreen);
}

void IslandWindow::SetIsFullscreen(const bool fFullscreenEnabled)
{
    // It is possible to enter SetIsFullScreen even if we're already in full screen.
    // Use the old is in fullscreen flag to gate checks that rely on the current state.
    bool fOldIsInFullscreen = _fullscreen;
    _fullscreen = fFullscreenEnabled;

    if (_ShouldUpdateStylesOnFullscreen())
    {
        HWND const hWnd = GetWindowHandle();

        // First, modify regular window styles as appropriate
        LONG dwWindowStyle = GetWindowLongW(hWnd, GWL_STYLE);
        if (_fullscreen)
        {
            // moving to fullscreen. remove WS_OVERLAPPEDWINDOW, which specifies styles for non-fullscreen windows (e.g.
            // caption bar). add the WS_POPUP style to allow us to size ourselves to the monitor size.
            WI_ClearAllFlags(dwWindowStyle, WS_OVERLAPPEDWINDOW);
            WI_SetFlag(dwWindowStyle, WS_POPUP);
        }
        else
        {
            // coming back from fullscreen. undo what we did to get in to fullscreen in the first place.
            WI_ClearFlag(dwWindowStyle, WS_POPUP);
            WI_SetAllFlags(dwWindowStyle, WS_OVERLAPPEDWINDOW);
        }
        SetWindowLongW(hWnd, GWL_STYLE, dwWindowStyle);

        // Now modify extended window styles as appropriate
        LONG dwExWindowStyle = GetWindowLongW(hWnd, GWL_EXSTYLE);
        if (_fullscreen)
        {
            // moving to fullscreen. remove the window edge style to avoid an ugly border when not focused.
            WI_ClearFlag(dwExWindowStyle, WS_EX_WINDOWEDGE);
        }
        else
        {
            // coming back from fullscreen.
            WI_SetFlag(dwExWindowStyle, WS_EX_WINDOWEDGE);
        }
        SetWindowLongW(hWnd, GWL_EXSTYLE, dwExWindowStyle);
    }

    _BackupWindowSizes(fOldIsInFullscreen);
    _ApplyWindowSize();
}

void IslandWindow::_BackupWindowSizes(const bool fCurrentIsInFullscreen)
{
    if (_fullscreen)
    {
        // Note: the current window size depends on the current state of the window.
        // So don't back it up if we're already in full screen.
        if (!fCurrentIsInFullscreen)
        {
            _rcNonFullscreenWindowSize = GetWindowRect();
        }

        // get and back up the current monitor's size
        HMONITOR const hCurrentMonitor = MonitorFromWindow(GetWindowHandle(), MONITOR_DEFAULTTONEAREST);
        MONITORINFO currMonitorInfo;
        currMonitorInfo.cbSize = sizeof(currMonitorInfo);
        if (GetMonitorInfo(hCurrentMonitor, &currMonitorInfo))
        {
            _rcFullscreenWindowSize = currMonitorInfo.rcMonitor;
        }
    }
}

void IslandWindow::_ApplyWindowSize()
{
    const RECT rcNewSize = _fullscreen ? _rcFullscreenWindowSize : _rcNonFullscreenWindowSize;

    SetWindowPos(GetWindowHandle(),
                 HWND_TOP,
                 rcNewSize.left,
                 rcNewSize.top,
                 RECT_WIDTH(&rcNewSize),
                 RECT_HEIGHT(&rcNewSize),
                 SWP_FRAMECHANGED);

    // SCREEN_INFORMATION& siAttached = GetScreenInfo();
    // siAttached.MakeCurrentCursorVisible();
}

DEFINE_EVENT(IslandWindow, DragRegionClicked, _DragRegionClickedHandlers, winrt::delegate<>);
DEFINE_EVENT(IslandWindow, WindowCloseButtonClicked, _windowCloseButtonClickedHandler, winrt::delegate<>);
