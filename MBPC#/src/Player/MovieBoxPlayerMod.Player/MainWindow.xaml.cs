using System;
using System.ComponentModel;
using System.IO;
using System.Runtime.InteropServices;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Controls.Primitives;
using System.Windows.Input;
using System.Windows.Interop;
using System.Windows.Media;
using System.Windows.Media.Animation;
using System.Windows.Threading;
using FlyleafLib.MediaPlayer;

namespace MovieBoxPlayerMod.Player
{
    public partial class MainWindow : Window
    {
        private const int SeekStepMilliseconds = 10_000;
        private const int VolumeStep = 5;

        private const string PlayIcon = "\uE768";
        private const string PauseIcon = "\uE769";
        private const string VolumeIcon = "\uE767";
        private const string MuteIcon = "\uE74F";
        private const string FullscreenIcon = "\uE740";
        private const string RestoreFullscreenIcon = "\uE73F";
        private const string MaximizeIcon = "\uE922";
        private const string RestoreWindowIcon = "\uE923";

        private const uint WM_NCLBUTTONDOWN = 0x00A1;
        private const int WM_GETMINMAXINFO = 0x0024;
        private const uint MONITOR_DEFAULTTONEAREST = 0x00000002;
        private const uint SWP_NOSIZE = 0x0001;
        private const uint SWP_NOMOVE = 0x0002;
        private const uint SWP_NOACTIVATE = 0x0010;
        private const uint SWP_NOOWNERZORDER = 0x0200;
        private static readonly IntPtr HWND_TOPMOST = new IntPtr(-1);
        private static readonly IntPtr HWND_NOTOPMOST = new IntPtr(-2);
        private const int HTCAPTION = 2;
        private const int VK_LBUTTON = 0x01;
        private const int SM_CXDOUBLECLK = 36;
        private const int SM_CYDOUBLECLK = 37;

        private const int DWMWA_USE_IMMERSIVE_DARK_MODE = 20;
        private const int DWMWA_WINDOW_CORNER_PREFERENCE = 33;
        private const int DWMWA_BORDER_COLOR = 34;
        private const int DWMWA_COLOR_NONE = unchecked((int)0xFFFFFFFE);

        private enum DwmWindowCornerPreference
        {
            Default = 0,
            DoNotRound = 1,
            Round = 2,
            RoundSmall = 3
        }

        [StructLayout(LayoutKind.Sequential)]
        private struct NativePoint
        {
            public int X;
            public int Y;
        }

        [StructLayout(LayoutKind.Sequential)]
        private struct NativeRect
        {
            public int Left;
            public int Top;
            public int Right;
            public int Bottom;
        }

        [StructLayout(LayoutKind.Sequential)]
        private struct NativeMinMaxInfo
        {
            public NativePoint Reserved;
            public NativePoint MaxSize;
            public NativePoint MaxPosition;
            public NativePoint MinTrackSize;
            public NativePoint MaxTrackSize;
        }

        [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Auto)]
        private struct NativeMonitorInfo
        {
            public int cbSize;
            public NativeRect rcMonitor;
            public NativeRect rcWork;
            public uint dwFlags;
        }

        [DllImport("dwmapi.dll")]
        private static extern int DwmSetWindowAttribute(
            IntPtr hwnd,
            int dwAttribute,
            ref int pvAttribute,
            int cbAttribute);

        [DllImport("user32.dll")]
        private static extern bool ReleaseCapture();

        [DllImport("user32.dll")]
        private static extern IntPtr SendMessage(IntPtr hWnd, uint msg, IntPtr wParam, IntPtr lParam);

        [DllImport("user32.dll")]
        private static extern bool GetCursorPos(out NativePoint point);

        [DllImport("user32.dll")]
        private static extern bool GetClientRect(IntPtr hWnd, out NativeRect rect);

        [DllImport("user32.dll")]
        private static extern bool ClientToScreen(IntPtr hWnd, ref NativePoint point);

        [DllImport("user32.dll")]
        private static extern short GetAsyncKeyState(int virtualKey);

        [DllImport("user32.dll")]
        private static extern IntPtr SetCursor(IntPtr cursor);

        [DllImport("user32.dll")]
        private static extern uint GetDoubleClickTime();

        [DllImport("user32.dll")]
        private static extern int GetSystemMetrics(int index);

        [DllImport("user32.dll")]
        private static extern IntPtr MonitorFromWindow(IntPtr hWnd, uint dwFlags);

        [DllImport("user32.dll", CharSet = CharSet.Auto)]
        private static extern bool GetMonitorInfo(IntPtr hMonitor, ref NativeMonitorInfo lpmi);

        [DllImport("user32.dll", SetLastError = true)]
        private static extern bool SetWindowPos(
            IntPtr hWnd,
            IntPtr hWndInsertAfter,
            int X,
            int Y,
            int cx,
            int cy,
            uint uFlags);

        private readonly FlyleafLib.MediaPlayer.Player player;
        private readonly DispatcherTimer hideTimer;
        private readonly DispatcherTimer updateTimer;

        private bool isDraggingSlider;
        private bool suppressVolumeChange;
        private bool isFullscreen;
        private bool isMonitorCoverMaximized;
        private bool preFullscreenWasMonitorCoverMaximized;
        private bool isClosing;
        private bool pointerInside;
        private bool customCursorInteractive;
        private bool lastLeftButtonDown;
        private bool hasLastNativeCursor;
        private bool progressRenderSubscribed;
        private bool pointerOverChrome;
        private bool systemCursorSuppressed;
        private bool ownerClientBoundsValid;
        private long lastVideoClickTick = long.MinValue;

        private IntPtr windowHandle;
        private HwndSource? windowSource;
        private NativePoint lastNativeCursor;
        private NativePoint lastVideoClickPoint;
        private NativeRect ownerClientScreenRect;

        private Rect preFullscreenBounds;
        private Rect monitorCoverRestoreBounds;
        private ResizeMode preFullscreenResizeMode = ResizeMode.CanResize;
        private ResizeMode monitorCoverRestoreResizeMode = ResizeMode.CanResize;

        public MainWindow()
        {
            InitializeComponent();

            FlyleafLib.Config config = new FlyleafLib.Config();
            config.Player.AutoPlay = true;
            config.Demuxer.FormatOpt["reconnect"] = "1";
            config.Demuxer.FormatOpt["reconnect_streamed"] = "1";
            config.Demuxer.FormatOpt["reconnect_delay_max"] = "5";

            if (!string.IsNullOrWhiteSpace(App.UserAgent))
                config.Demuxer.FormatOpt["headers"] = "User-Agent: " + App.UserAgent + "\r\n";

            // Empty string lets Flyleaf select the preferred GPU instead of forcing
            // a particular adapter and preserves the behavior of the original player.
            config.Video.GPUAdapter = "";

            player = new FlyleafLib.MediaPlayer.Player(config);
            player.OpenCompleted += Player_OpenCompleted;
            PlayerHost.Player = player;

            hideTimer = new DispatcherTimer
            {
                Interval = TimeSpan.FromSeconds(2.35)
            };
            hideTimer.Tick += HideTimer_Tick;

            updateTimer = new DispatcherTimer
            {
                Interval = TimeSpan.FromMilliseconds(250)
            };
            updateTimer.Tick += UpdateTimer_Tick;
            updateTimer.Start();

            // The render callback is the single high-frequency UI loop for both media
            // progress and physical-pointer sampling. Keeping one cadence avoids a second
            // DispatcherTimer wake-up and keeps cursor motion synchronized to presentation.
            LocationChanged += Window_LocationChanged;
            SizeChanged += Window_SizeChanged;
        }

        private void Window_SourceInitialized(object? sender, EventArgs e)
        {
            windowHandle = new WindowInteropHelper(this).Handle;
            windowSource = HwndSource.FromHwnd(windowHandle);
            windowSource?.AddHook(WindowProc);
            RefreshOwnerClientScreenBounds();
            ApplyDwmWindowAppearance(IsActive);
            UpdateTaskbarCoverState(IsActive);
        }

        private IntPtr WindowProc(IntPtr hwnd, int msg, IntPtr wParam, IntPtr lParam, ref bool handled)
        {
            if (msg != WM_GETMINMAXINFO || lParam == IntPtr.Zero)
                return IntPtr.Zero;

            IntPtr monitor = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
            if (monitor == IntPtr.Zero)
                return IntPtr.Zero;

            NativeMonitorInfo monitorInfo = new NativeMonitorInfo
            {
                cbSize = Marshal.SizeOf<NativeMonitorInfo>()
            };

            if (!GetMonitorInfo(monitor, ref monitorInfo))
                return IntPtr.Zero;

            // WPF's default maximized bounds use rcWork, which deliberately leaves
            // room for the taskbar. The player wants true monitor-covering maximize
            // semantics, so publish rcMonitor as the native maximized rectangle.
            NativeMinMaxInfo minMaxInfo = Marshal.PtrToStructure<NativeMinMaxInfo>(lParam);
            minMaxInfo.MaxPosition.X = 0;
            minMaxInfo.MaxPosition.Y = 0;
            minMaxInfo.MaxSize.X = Math.Max(1, monitorInfo.rcMonitor.Right - monitorInfo.rcMonitor.Left);
            minMaxInfo.MaxSize.Y = Math.Max(1, monitorInfo.rcMonitor.Bottom - monitorInfo.rcMonitor.Top);
            Marshal.StructureToPtr(minMaxInfo, lParam, fDeleteOld: false);
            handled = true;
            return IntPtr.Zero;
        }

        private void Window_Loaded(object sender, RoutedEventArgs e)
        {
            OverlayRoot.Focus();
            RefreshOwnerClientScreenBounds();
            StartPlaybackProgressRendering();
            SyncPointerFromScreen(forceWake: true);

            if (!string.IsNullOrWhiteSpace(App.VideoTitle))
            {
                TitleText.Text = App.VideoTitle;
                Title = App.VideoTitle;
            }

            resumeSeconds = App.ResumeSeconds;
            if (!string.IsNullOrWhiteSpace(App.BridgeName))
            {
                _ = ConnectBridgeAsync();
                RestartHideTimer();
                return;
            }

            if (string.IsNullOrWhiteSpace(App.VideoUri))
            {
                SourceText.Text = "No media supplied";
                ShowStatus("No video URI was provided.", showProgress: false);
                ShowControls();
                return;
            }

            SourceText.Text = GetSourceLabel(App.VideoUri);
            ShowStatus("Opening video…", showProgress: true);

            try
            {
                player.OpenAsync(App.VideoUri);
            }
            catch (Exception ex)
            {
                ShowStatus("Could not open this video: " + ex.Message, showProgress: false);
            }

            RestartHideTimer();
        }

        private void Player_OpenCompleted(object? sender, OpenCompletedArgs e)
        {
            Dispatcher.BeginInvoke(new Action(() =>
            {
                if (isClosing)
                    return;

                if (!e.Success)
                {
                    QueueBridgeAction("failed");
                    ShowStatus("Unable to open this video.", showProgress: false);
                    ShowControls();
                    return;
                }

                mediaReady = true;
                endedSent = false;
                ApplyPendingBaseProfileSettings();
                QueueBridgeAction("opened");
                HideStatus();

                if (resumeSeconds > 0)
                {
                    long requestedMilliseconds = (long)Math.Round(resumeSeconds * 1000.0);
                    SeekToMilliseconds(requestedMilliseconds, showFeedback: false);
                    resumeSeconds = 0;
                }

                if (!resumePlaying) player.Pause();
                RestartHideTimer();
            }));
        }

        private void UpdateTimer_Tick(object? sender, EventArgs e)
        {
            if (isClosing)
                return;

            try
            {
                long durationMilliseconds = player.Duration / 10_000;

                // Playback position is intentionally NOT updated here. The 250 ms timer
                // is too coarse for a media playhead and visibly steps. ProgressSlider
                // and CurTimeText are driven by CompositionTarget.Rendering below so
                // they sample Flyleaf's real playback clock once per presented WPF frame.
                DurationText.Text = PlayerUiLogic.FormatTime(durationMilliseconds);

                suppressVolumeChange = true;
                VolumeSlider.Value = player.Audio.Volume;
                suppressVolumeChange = false;

                TickBridge();

                bool isPlaying = player.Status == Status.Playing;
                PlayPauseGlyph.Text = isPlaying ? PauseIcon : PlayIcon;
                MuteGlyph.Text = player.Audio.Mute || player.Audio.Volume <= 0 ? MuteIcon : VolumeIcon;

                // If playback is paused by the engine or another input path, controls
                // should remain available even after a previous idle fade.
                if (!isPlaying && ControlsLayer.Opacity < 0.99)
                    ShowControls();
            }
            catch
            {
                // Flyleaf can transition internal state while opening/closing. The UI
                // refresh loop should never take the app down during that transition.
                suppressVolumeChange = false;
            }
        }

        private void StartPlaybackProgressRendering()
        {
            if (progressRenderSubscribed)
                return;

            CompositionTarget.Rendering += PlaybackProgress_Rendering;
            progressRenderSubscribed = true;
        }

        private void StopPlaybackProgressRendering()
        {
            if (!progressRenderSubscribed)
                return;

            CompositionTarget.Rendering -= PlaybackProgress_Rendering;
            progressRenderSubscribed = false;
        }

        private void PlaybackProgress_Rendering(object? sender, EventArgs e)
        {
            if (isClosing)
                return;

            // One presentation-synchronized loop owns all high-frequency player UI.
            // Physical pointer state is sampled even while the timeline is being dragged.
            SyncPointerFromScreen(forceWake: false);
            RenderSubtitles();

            if (isDraggingSlider)
                return;

            try
            {
                // Preserve sub-millisecond precision from Flyleaf's 100 ns clock. The
                // display snaps only inside the final ~50 ms so container timestamps that
                // end one frame early still render a visually complete 100% timeline.
                double currentMilliseconds = player.CurTime / 10_000.0;
                double durationMilliseconds = player.Duration / 10_000.0;
                double maximum = Math.Max(1.0, durationMilliseconds);
                double position = PlayerUiLogic.SnapPlaybackPositionToEnd(
                    currentMilliseconds,
                    durationMilliseconds);

                if (!ProgressSlider.Maximum.Equals(maximum))
                    ProgressSlider.Maximum = maximum;

                if (Math.Abs(ProgressSlider.Value - position) > 0.001)
                    ProgressSlider.Value = position;

                UpdateTimelineVisual(position, maximum);

                string currentText = PlayerUiLogic.FormatTime(position);
                if (!string.Equals(CurTimeText.Text, currentText, StringComparison.Ordinal))
                    CurTimeText.Text = currentText;
            }
            catch
            {
                // Opening, seeking and teardown can briefly mutate Flyleaf state. A
                // visual refresh frame must never be able to terminate playback.
            }
        }

        private void UpdateTimelineVisual(double value, double maximum)
        {
            double ratio = maximum <= 0 || double.IsNaN(maximum) || double.IsInfinity(maximum)
                ? 0.0
                : Math.Clamp(value / maximum, 0.0, 1.0);

            // RenderTransform is compositor-friendly and avoids layout invalidation.
            if (Math.Abs(TimelineProgressScale.ScaleX - ratio) > 0.00001)
                TimelineProgressScale.ScaleX = ratio;
        }

        private void HideTimer_Tick(object? sender, EventArgs e)
        {
            hideTimer.Stop();

            // Slider drags and open menus own the interaction layer until they finish.
            // Keeping this check before cursor hiding also keeps the custom cursor visible
            // above the in-overlay episode/season/playback menus.
            if (isDraggingSlider || playbackMenuOpen)
                return;

            // Paused/non-playing media always keeps the player chrome available. The
            // inactivity timer only owns auto-hide while playback is actively running.
            if (player.Status != Status.Playing)
            {
                ShowControls();
                return;
            }

            HideCustomCursor();
            HideControls();
        }


        private void SyncPointerFromScreen(bool forceWake)
        {
            if (isClosing || !IsVisible || !GetCursorPos(out NativePoint nativePoint))
                return;

            bool moved = forceWake ||
                !hasLastNativeCursor ||
                nativePoint.X != lastNativeCursor.X ||
                nativePoint.Y != lastNativeCursor.Y;

            bool inside = IsScreenPointInsideOwnerClient(nativePoint);
            bool leftButtonDown = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
            bool buttonChanged = leftButtonDown != lastLeftButtonDown;

            if (!inside || !IsActive)
            {
                if (pointerInside)
                {
                    pointerInside = false;
                    pointerOverChrome = false;
                    RestoreSystemCursor();
                    HideCustomCursor();
                }

                // Movement outside this player is not player interaction. While playing,
                // leaving the client area hides chrome immediately and continued desktop
                // mouse movement cannot wake it. Paused/non-playing media is the explicit
                // exception: its controls stay visible even with the pointer elsewhere.
                hideTimer.Stop();
                if (player.Status != Status.Playing || isDraggingSlider || playbackMenuOpen)
                {
                    if (!ControlsLayer.IsHitTestVisible)
                        ShowControls();
                }
                else
                {
                    if (ControlsLayer.IsHitTestVisible)
                        HideControls();
                }

                lastLeftButtonDown = leftButtonDown;
                lastNativeCursor = nativePoint;
                hasLastNativeCursor = true;
                return;
            }

            bool justEntered = !pointerInside;
            pointerInside = true;

            // Reassert cursor suppression only when something could have changed the
            // underlying HWND cursor state, rather than doing it on every render frame.
            if (moved || justEntered || buttonChanged)
                HideSystemCursor();

            if (moved || justEntered)
            {
                MoveCustomCursorFromScreen(nativePoint);
                pointerOverChrome = IsPointerOverChrome(nativePoint);
                UpdateCustomCursorAppearance(pointerOverChrome);
                ShowInteractionUi();
            }

            // Physical-button edge detection keeps video clicks independent of Flyleaf's
            // native HWND hit-testing while avoiding any extra routed-event work.
            if (leftButtonDown && !lastLeftButtonDown)
                HandlePhysicalLeftButtonDown(nativePoint);

            lastLeftButtonDown = leftButtonDown;
            lastNativeCursor = nativePoint;
            hasLastNativeCursor = true;
        }

        private bool IsScreenPointInsideOwnerClient(NativePoint screenPoint)
        {
            if (!ownerClientBoundsValid)
                RefreshOwnerClientScreenBounds();

            if (!ownerClientBoundsValid)
                return false;

            return screenPoint.X >= ownerClientScreenRect.Left &&
                   screenPoint.Y >= ownerClientScreenRect.Top &&
                   screenPoint.X < ownerClientScreenRect.Right &&
                   screenPoint.Y < ownerClientScreenRect.Bottom;
        }

        private void RefreshOwnerClientScreenBounds()
        {
            IntPtr hwnd = windowHandle != IntPtr.Zero
                ? windowHandle
                : new WindowInteropHelper(this).Handle;

            ownerClientBoundsValid = false;

            if (hwnd == IntPtr.Zero || !GetClientRect(hwnd, out NativeRect client))
                return;

            NativePoint origin = new NativePoint { X = client.Left, Y = client.Top };
            if (!ClientToScreen(hwnd, ref origin))
                return;

            int width = Math.Max(0, client.Right - client.Left);
            int height = Math.Max(0, client.Bottom - client.Top);

            ownerClientScreenRect = new NativeRect
            {
                Left = origin.X,
                Top = origin.Y,
                Right = origin.X + width,
                Bottom = origin.Y + height
            };
            ownerClientBoundsValid = width > 0 && height > 0;
        }

        private void Window_LocationChanged(object? sender, EventArgs e)
        {
            RefreshOwnerClientScreenBounds();
        }

        private void Window_SizeChanged(object sender, SizeChangedEventArgs e)
        {
            RefreshOwnerClientScreenBounds();
            if (IsEpisodesMenuOpen)
            {
                PositionEpisodesMenuOverlay();
                if (IsSeasonMenuOpen && activeSeasonMenuButton != null)
                    PositionSeasonMenuOverlay(activeSeasonMenuButton);
            }
            if (IsPlaybackMenuOpen)
            {
                PositionPlaybackMenuOverlay();
                if (IsPlaybackSubmenuOpen && activePlaybackMenuButton != null)
                    PositionPlaybackSubmenuOverlay(activePlaybackMenuButton);
            }
        }

        private void HandlePhysicalLeftButtonDown(NativePoint nativePoint)
        {
            ShowCustomCursor();
            RestartHideTimer();

            if (IsEpisodesMenuOpen &&
                !IsScreenPointInsideElement(nativePoint, EpisodesMenuOverlay) &&
                !IsScreenPointInsideElement(nativePoint, SeasonMenuOverlay) &&
                !IsScreenPointInsideElement(nativePoint, EpisodesButton))
            {
                CloseEpisodesMenuOverlay();
                return;
            }

            if (IsPlaybackMenuOpen &&
                !IsScreenPointInsideElement(nativePoint, PlaybackMenuOverlay) &&
                !IsScreenPointInsideElement(nativePoint, PlaybackSubmenuOverlay) &&
                !IsScreenPointInsideElement(nativePoint, PlaybackButton))
            {
                ClosePlaybackMenuOverlay();
                return;
            }

            if (IsPointerOverChrome(nativePoint))
                return;

            OverlayRoot.Focus();

            long now = Environment.TickCount64;
            long elapsed = lastVideoClickTick == long.MinValue ? long.MaxValue : now - lastVideoClickTick;
            int dx = Math.Abs(nativePoint.X - lastVideoClickPoint.X);
            int dy = Math.Abs(nativePoint.Y - lastVideoClickPoint.Y);
            long doubleClickTime = GetDoubleClickTime();
            int doubleClickWidth = Math.Max(4, GetSystemMetrics(SM_CXDOUBLECLK));
            int doubleClickHeight = Math.Max(4, GetSystemMetrics(SM_CYDOUBLECLK));

            bool isDoubleClick = elapsed >= 0 &&
                                 elapsed <= doubleClickTime &&
                                 dx <= doubleClickWidth &&
                                 dy <= doubleClickHeight;

            if (isDoubleClick)
            {
                lastVideoClickTick = long.MinValue;
                ToggleFullscreen();
                return;
            }

            lastVideoClickTick = now;
            lastVideoClickPoint = nativePoint;
            TogglePlayback();
        }

        private bool IsPointerOverChrome(NativePoint screenPoint)
        {
            if (!ControlsLayer.IsHitTestVisible || ControlsLayer.Opacity <= 0.02)
                return false;

            if (TopBar.Visibility == Visibility.Visible &&
                (IsScreenPointInsideElement(screenPoint, TitleDragRegion) ||
                 IsScreenPointInsideElement(screenPoint, WindowControlsPanel)))
            {
                return true;
            }

            if (IsEpisodesMenuOpen && IsScreenPointInsideElement(screenPoint, EpisodesMenuOverlay))
                return true;
            if (IsSeasonMenuOpen && IsScreenPointInsideElement(screenPoint, SeasonMenuOverlay))
                return true;
            if (IsPlaybackMenuOpen && IsScreenPointInsideElement(screenPoint, PlaybackMenuOverlay))
                return true;
            if (IsPlaybackSubmenuOpen && IsScreenPointInsideElement(screenPoint, PlaybackSubmenuOverlay))
                return true;

            return IsScreenPointInsideElement(screenPoint, BottomDock);
        }

        private static bool IsScreenPointInsideElement(NativePoint screenPoint, FrameworkElement element)
        {
            if (!element.IsVisible || element.ActualWidth <= 0 || element.ActualHeight <= 0)
                return false;

            try
            {
                // PointToScreen converts both corners through the element's real HWND/DPI
                // transform. Using screen-space for exclusions means no Flyleaf overlay
                // layout rectangle can create an accidental center dead zone.
                Point topLeft = element.PointToScreen(new Point(0, 0));
                Point bottomRight = element.PointToScreen(new Point(element.ActualWidth, element.ActualHeight));

                double left = Math.Min(topLeft.X, bottomRight.X);
                double right = Math.Max(topLeft.X, bottomRight.X);
                double top = Math.Min(topLeft.Y, bottomRight.Y);
                double bottom = Math.Max(topLeft.Y, bottomRight.Y);

                return screenPoint.X >= left &&
                       screenPoint.X < right &&
                       screenPoint.Y >= top &&
                       screenPoint.Y < bottom;
            }
            catch (InvalidOperationException)
            {
                return false;
            }
        }

        private void Window_Activated(object? sender, EventArgs e)
        {
            RefreshOwnerClientScreenBounds();
            ApplyDwmWindowAppearance(active: true);
            UpdateTaskbarCoverState(active: true);
            SyncPointerFromScreen(forceWake: true);
        }

        private void Window_Deactivated(object? sender, EventArgs e)
        {
            UpdateTaskbarCoverState(active: false);
            ApplyDwmWindowAppearance(active: false);
            RestoreSystemCursor();
            HideCustomCursor();
        }

        private void ShowInteractionUi()
        {
            ShowControls();
            ShowCustomCursor();
            RestartHideTimer();
        }

        private void RestartHideTimer()
        {
            hideTimer.Stop();
            hideTimer.Start();
        }

        private void ShowControls()
        {
            ControlsLayer.IsHitTestVisible = true;
            AnimateChrome(show: true);
        }

        private void HideControls()
        {
            ControlsLayer.IsHitTestVisible = false;
            AnimateChrome(show: false);
        }

        private void AnimateChrome(bool show)
        {
            int durationMs = show ? 145 : 240;
            EasingFunctionBase easing = new CubicEase
            {
                EasingMode = show ? EasingMode.EaseOut : EasingMode.EaseIn
            };

            DoubleAnimation opacityAnimation = new DoubleAnimation
            {
                To = show ? 1.0 : 0.0,
                Duration = TimeSpan.FromMilliseconds(durationMs),
                EasingFunction = easing
            };

            DoubleAnimation dockAnimation = new DoubleAnimation
            {
                To = show ? 0.0 : 16.0,
                Duration = TimeSpan.FromMilliseconds(durationMs),
                EasingFunction = easing
            };

            DoubleAnimation topAnimation = new DoubleAnimation
            {
                To = show ? 0.0 : -10.0,
                Duration = TimeSpan.FromMilliseconds(durationMs),
                EasingFunction = easing
            };

            ControlsLayer.BeginAnimation(OpacityProperty, opacityAnimation, HandoffBehavior.SnapshotAndReplace);
            BottomDockTransform.BeginAnimation(TranslateTransform.YProperty, dockAnimation, HandoffBehavior.SnapshotAndReplace);
            TopBarTransform.BeginAnimation(TranslateTransform.YProperty, topAnimation, HandoffBehavior.SnapshotAndReplace);
        }

        private void MoveCustomCursorFromScreen(NativePoint screenPoint)
        {
            try
            {
                Point position = CursorCanvas.PointFromScreen(new Point(screenPoint.X, screenPoint.Y));
                CustomCursorTranslate.X = position.X - (CustomCursor.Width * 0.5);
                CustomCursorTranslate.Y = position.Y - (CustomCursor.Height * 0.5);
            }
            catch (InvalidOperationException)
            {
                // Overlay can be between HWND/layout transitions during fullscreen toggles.
            }
        }

        private void UpdateCustomCursorAppearance(bool interactive)
        {
            if (interactive == customCursorInteractive)
                return;

            customCursorInteractive = interactive;

            DoubleAnimation scale = new DoubleAnimation
            {
                To = interactive ? 1.04 : 0.74,
                Duration = TimeSpan.FromMilliseconds(110),
                EasingFunction = new CubicEase { EasingMode = EasingMode.EaseOut }
            };

            CustomCursorScale.BeginAnimation(ScaleTransform.ScaleXProperty, scale, HandoffBehavior.SnapshotAndReplace);
            CustomCursorScale.BeginAnimation(ScaleTransform.ScaleYProperty, scale, HandoffBehavior.SnapshotAndReplace);

            CursorHalo.Stroke = interactive
                ? (Brush)FindResource("PlayerAccentBrush")
                : Brushes.White;
            CursorHalo.Opacity = interactive ? 1.0 : 0.78;
            CursorShadow.Opacity = interactive ? 0.9 : 0.62;
        }

        private void ShowCustomCursor()
        {
            if (!pointerInside)
                return;

            CustomCursor.BeginAnimation(OpacityProperty, null);
            CustomCursor.Opacity = 1;
        }

        private void HideCustomCursor()
        {
            AnimateOpacity(CustomCursor, 0.0, 145);
        }

        private static void AnimateOpacity(UIElement element, double targetOpacity, int milliseconds)
        {
            DoubleAnimation animation = new DoubleAnimation
            {
                To = targetOpacity,
                Duration = TimeSpan.FromMilliseconds(milliseconds),
                EasingFunction = new QuadraticEase
                {
                    EasingMode = targetOpacity > element.Opacity ? EasingMode.EaseOut : EasingMode.EaseIn
                }
            };

            element.BeginAnimation(OpacityProperty, animation, HandoffBehavior.SnapshotAndReplace);
        }

        private static string GetSourceLabel(string source)
        {
            if (Uri.TryCreate(source, UriKind.Absolute, out Uri? uri))
            {
                if (uri.IsFile)
                    return Path.GetFileName(uri.LocalPath);

                return string.IsNullOrWhiteSpace(uri.Host) ? source : uri.Host;
            }

            string fileName = Path.GetFileName(source);
            return string.IsNullOrWhiteSpace(fileName) ? source : fileName;
        }

        private void HideSystemCursor()
        {
            if (!systemCursorSuppressed)
            {
                Mouse.OverrideCursor = Cursors.None;
                OverlayRoot.Cursor = Cursors.None;
                OverlayRoot.ForceCursor = true;
                systemCursorSuppressed = true;
            }

            SetCursor(IntPtr.Zero);
        }

        private void RestoreSystemCursor()
        {
            if (!systemCursorSuppressed)
                return;

            OverlayRoot.ForceCursor = false;
            OverlayRoot.Cursor = null;
            if (Mouse.OverrideCursor == Cursors.None)
                Mouse.OverrideCursor = null;

            systemCursorSuppressed = false;
        }

        private void TogglePlayback()
        {
            bool wasPlaying = player.Status == Status.Playing;
            player.TogglePlayPause();
            ShowCenterFeedback(wasPlaying ? "Ⅱ" : "▶");
            ShowControls();
            RestartHideTimer();
        }

        private void ShowCenterFeedback(string text)
        {
            CenterFeedbackText.Text = text;
            CenterFeedback.BeginAnimation(OpacityProperty, null);
            CenterFeedbackScale.BeginAnimation(ScaleTransform.ScaleXProperty, null);
            CenterFeedbackScale.BeginAnimation(ScaleTransform.ScaleYProperty, null);

            CenterFeedback.Opacity = 1;
            CenterFeedbackScale.ScaleX = 0.9;
            CenterFeedbackScale.ScaleY = 0.9;

            CubicEase popEase = new CubicEase { EasingMode = EasingMode.EaseOut };
            DoubleAnimation pop = new DoubleAnimation
            {
                To = 1.0,
                Duration = TimeSpan.FromMilliseconds(150),
                EasingFunction = popEase
            };

            DoubleAnimation fade = new DoubleAnimation
            {
                From = 1,
                To = 0,
                BeginTime = TimeSpan.FromMilliseconds(210),
                Duration = TimeSpan.FromMilliseconds(420),
                EasingFunction = new QuadraticEase { EasingMode = EasingMode.EaseIn }
            };

            CenterFeedbackScale.BeginAnimation(ScaleTransform.ScaleXProperty, pop, HandoffBehavior.SnapshotAndReplace);
            CenterFeedbackScale.BeginAnimation(ScaleTransform.ScaleYProperty, pop, HandoffBehavior.SnapshotAndReplace);
            CenterFeedback.BeginAnimation(OpacityProperty, fade, HandoffBehavior.SnapshotAndReplace);
        }

        private void ShowStatus(string text, bool showProgress)
        {
            StatusText.Text = text;
            StatusProgress.Visibility = showProgress ? Visibility.Visible : Visibility.Collapsed;
            AnimateOpacity(StatusPanel, 1.0, 120);
        }

        private void HideStatus()
        {
            AnimateOpacity(StatusPanel, 0.0, 160);
        }

        private void TopBar_MouseLeftButtonDown(object sender, MouseButtonEventArgs e)
        {
            if (e.ChangedButton != MouseButton.Left || isFullscreen)
                return;

            if (e.ClickCount >= 2)
            {
                ToggleMaximize();
                e.Handled = true;
                return;
            }

            BeginNativeWindowDrag();
            e.Handled = true;
        }

        private void BeginNativeWindowDrag()
        {
            IntPtr hwnd = new WindowInteropHelper(this).Handle;
            if (hwnd == IntPtr.Zero)
                return;

            if (isMonitorCoverMaximized && GetCursorPos(out NativePoint cursorPixels))
            {
                Point cursor = DevicePixelsToDips(cursorPixels);
                Rect restore = monitorCoverRestoreBounds;
                double width = Math.Max(1.0, ActualWidth);
                double ratio = Math.Clamp((cursor.X - Left) / width, 0.08, 0.92);

                ExitMonitorCoverMaximize();

                if (restore.Width > 0 && restore.Height > 0)
                {
                    Width = Math.Max(MinWidth, restore.Width);
                    Height = Math.Max(MinHeight, restore.Height);
                    Left = cursor.X - (Width * ratio);
                    Top = cursor.Y - 18.0;
                }
            }

            ReleaseCapture();
            SendMessage(hwnd, WM_NCLBUTTONDOWN, new IntPtr(HTCAPTION), IntPtr.Zero);
        }

        private Point DevicePixelsToDips(NativePoint point)
        {
            Point converted = new Point(point.X, point.Y);
            PresentationSource? source = PresentationSource.FromVisual(this);
            if (source?.CompositionTarget != null)
                converted = source.CompositionTarget.TransformFromDevice.Transform(converted);

            return converted;
        }

        private void Window_KeyDown(object sender, KeyEventArgs e)
        {
            if (e.Handled)
                return;

            ShowControls();
            RestartHideTimer();

            switch (e.Key)
            {
                case Key.Space:
                case Key.K:
                    TogglePlayback();
                    e.Handled = true;
                    break;

                case Key.Left:
                case Key.J:
                    SeekRelative(-SeekStepMilliseconds);
                    e.Handled = true;
                    break;

                case Key.Right:
                case Key.L:
                    SeekRelative(SeekStepMilliseconds);
                    e.Handled = true;
                    break;

                case Key.Home:
                    SeekToMilliseconds(0, showFeedback: true);
                    e.Handled = true;
                    break;

                case Key.End:
                    SeekToMilliseconds(player.Duration / 10_000, showFeedback: true);
                    e.Handled = true;
                    break;

                case Key.Up:
                    AdjustVolume(VolumeStep);
                    e.Handled = true;
                    break;

                case Key.Down:
                    AdjustVolume(-VolumeStep);
                    e.Handled = true;
                    break;

                case Key.M:
                    ToggleMute();
                    e.Handled = true;
                    break;

                case Key.F:
                case Key.F11:
                    ToggleFullscreen();
                    e.Handled = true;
                    break;

                case Key.Escape:
                    if (IsEpisodesMenuOpen)
                        CloseEpisodesMenuOverlay();
                    else if (IsPlaybackMenuOpen)
                        ClosePlaybackMenuOverlay();
                    else if (isFullscreen)
                        ExitFullscreen();
                    else
                        ShowControls();
                    e.Handled = true;
                    break;
            }
        }

        private void Window_MouseWheel(object sender, MouseWheelEventArgs e)
        {
            if (IsEpisodesMenuOpen)
            {
                ScrollViewer? target = SeasonEpisodesMenuScrollViewer.IsMouseOver ? SeasonEpisodesMenuScrollViewer :
                    EpisodesMenuScrollViewer.IsMouseOver ? EpisodesMenuScrollViewer : null;
                if (target != null)
                {
                    target.ScrollToVerticalOffset(target.VerticalOffset - e.Delta / 3.0);
                    ShowCustomCursor();
                    e.Handled = true;
                    return;
                }
            }

            if (IsPlaybackMenuOpen)
            {
                ScrollViewer? target = PlaybackSubmenuScrollViewer.IsMouseOver ? PlaybackSubmenuScrollViewer :
                    PlaybackMenuScrollViewer.IsMouseOver ? PlaybackMenuScrollViewer : null;
                if (target != null)
                {
                    target.ScrollToVerticalOffset(target.VerticalOffset - e.Delta / 3.0);
                    ShowCustomCursor();
                    e.Handled = true;
                    return;
                }
            }

            AdjustVolume(e.Delta > 0 ? VolumeStep : -VolumeStep);
            e.Handled = true;
        }

        private void SeekRelative(long deltaMilliseconds)
        {
            long currentMilliseconds = player.CurTime / 10_000;
            SeekToMilliseconds(currentMilliseconds + deltaMilliseconds, showFeedback: false);
            ShowCenterFeedback(deltaMilliseconds >= 0 ? "+10" : "−10");
        }

        private void SeekToMilliseconds(long requestedMilliseconds, bool showFeedback)
        {
            long durationMilliseconds = player.Duration / 10_000;
            long targetMilliseconds = PlayerUiLogic.ClampSeekMilliseconds(requestedMilliseconds, durationMilliseconds);
            int flyleafTarget = (int)Math.Min(int.MaxValue, targetMilliseconds);

            player.SeekAccurate(flyleafTarget);

            if (showFeedback)
                ShowCenterFeedback(PlayerUiLogic.FormatTime(targetMilliseconds));
        }

        private void AdjustVolume(int delta)
        {
            int newVolume = Math.Clamp(player.Audio.Volume + delta, 0, 100);
            player.Audio.Volume = newVolume;
            MarkEpisodeSettingsDirty();

            if (newVolume > 0 && player.Audio.Mute)
                player.Audio.Mute = false;

            suppressVolumeChange = true;
            VolumeSlider.Value = newVolume;
            suppressVolumeChange = false;

            MuteGlyph.Text = newVolume <= 0 ? MuteIcon : VolumeIcon;
            ShowCenterFeedback(newVolume + "%");
        }

        private void ToggleMute()
        {
            player.Audio.Mute = !player.Audio.Mute;
            MarkEpisodeSettingsDirty();
            MuteGlyph.Text = player.Audio.Mute ? MuteIcon : VolumeIcon;
            ShowCenterFeedback(player.Audio.Mute ? "MUTE" : player.Audio.Volume + "%");
        }

        private void ToggleFullscreen()
        {
            if (isFullscreen)
                ExitFullscreen();
            else
                EnterFullscreen();
        }

        private void EnterFullscreen()
        {
            if (isFullscreen)
                return;

            preFullscreenWasMonitorCoverMaximized = isMonitorCoverMaximized;
            if (!preFullscreenWasMonitorCoverMaximized)
            {
                preFullscreenBounds = new Rect(Left, Top, ActualWidth, ActualHeight);
                preFullscreenResizeMode = ResizeMode;
            }

            isFullscreen = true;
            WindowState = WindowState.Normal;
            ResizeMode = ResizeMode.NoResize;
            TopBar.Visibility = Visibility.Collapsed;
            ApplyMonitorCoverPlacement(IsActive);
            FullscreenBtn.ToolTip = "Exit fullscreen (F)";
            FullscreenGlyph.Text = RestoreFullscreenIcon;
            ApplyDwmWindowAppearance(IsActive);
            UpdateMaximizeVisualState();
            ShowInteractionUi();
        }

        private void ExitFullscreen()
        {
            if (!isFullscreen)
                return;

            isFullscreen = false;
            WindowState = WindowState.Normal;
            TopBar.Visibility = Visibility.Visible;

            if (preFullscreenWasMonitorCoverMaximized)
            {
                ResizeMode = ResizeMode.NoResize;
                ApplyMonitorCoverPlacement(IsActive);
            }
            else
            {
                ResizeMode = preFullscreenResizeMode;
                ReleaseMonitorCoverZOrder();

                if (preFullscreenBounds.Width > 0 && preFullscreenBounds.Height > 0)
                {
                    Left = preFullscreenBounds.Left;
                    Top = preFullscreenBounds.Top;
                    Width = Math.Max(MinWidth, preFullscreenBounds.Width);
                    Height = Math.Max(MinHeight, preFullscreenBounds.Height);
                }
            }

            preFullscreenWasMonitorCoverMaximized = false;
            FullscreenBtn.ToolTip = "Fullscreen (F)";
            FullscreenGlyph.Text = FullscreenIcon;
            UpdateMaximizeVisualState();
            ApplyDwmWindowAppearance(IsActive);
            ShowInteractionUi();
        }

        private void ToggleMaximize()
        {
            if (isFullscreen)
            {
                ExitFullscreen();
                return;
            }

            if (isMonitorCoverMaximized)
                ExitMonitorCoverMaximize();
            else
                EnterMonitorCoverMaximize();
        }

        private void EnterMonitorCoverMaximize()
        {
            if (isMonitorCoverMaximized)
                return;

            WindowState = WindowState.Normal;
            monitorCoverRestoreBounds = new Rect(Left, Top, ActualWidth, ActualHeight);
            monitorCoverRestoreResizeMode = ResizeMode;
            isMonitorCoverMaximized = true;
            ResizeMode = ResizeMode.NoResize;

            ApplyMonitorCoverPlacement(IsActive);
            UpdateMaximizeVisualState();
            ApplyDwmWindowAppearance(IsActive);
            ShowInteractionUi();
        }

        private void ExitMonitorCoverMaximize()
        {
            if (!isMonitorCoverMaximized)
                return;

            isMonitorCoverMaximized = false;
            WindowState = WindowState.Normal;
            ResizeMode = monitorCoverRestoreResizeMode;
            ReleaseMonitorCoverZOrder();

            if (monitorCoverRestoreBounds.Width > 0 && monitorCoverRestoreBounds.Height > 0)
            {
                Left = monitorCoverRestoreBounds.Left;
                Top = monitorCoverRestoreBounds.Top;
                Width = Math.Max(MinWidth, monitorCoverRestoreBounds.Width);
                Height = Math.Max(MinHeight, monitorCoverRestoreBounds.Height);
            }

            UpdateMaximizeVisualState();
            ApplyDwmWindowAppearance(IsActive);
            ShowInteractionUi();
        }

        private void Window_StateChanged(object? sender, EventArgs e)
        {
            // Windows can still request a native maximize (for example Win+Up).
            // Convert that request into the same explicit monitor-cover mode used
            // by the custom maximize button so the taskbar cannot reserve space.
            if (WindowState == WindowState.Maximized && !isFullscreen && !isMonitorCoverMaximized)
            {
                Rect restore = RestoreBounds;
                monitorCoverRestoreBounds = restore.Width > 0 && restore.Height > 0
                    ? restore
                    : new Rect(Left, Top, ActualWidth, ActualHeight);
                monitorCoverRestoreResizeMode = ResizeMode;
                isMonitorCoverMaximized = true;
                WindowState = WindowState.Normal;
                ResizeMode = ResizeMode.NoResize;
                ApplyMonitorCoverPlacement(IsActive);
                UpdateMaximizeVisualState();
                ApplyDwmWindowAppearance(IsActive);
                return;
            }

            UpdateMaximizeVisualState();
            RefreshOwnerClientScreenBounds();
            ApplyDwmWindowAppearance(IsActive);
            UpdateTaskbarCoverState(IsActive);
        }

        private void UpdateMaximizeVisualState()
        {
            bool maximized = isMonitorCoverMaximized && !isFullscreen;
            MaximizeGlyph.Text = maximized ? RestoreWindowIcon : MaximizeIcon;
            MaximizeBtn.ToolTip = maximized ? "Restore" : "Maximize";
        }

        private bool TryGetCurrentMonitorInfo(out NativeMonitorInfo monitorInfo)
        {
            monitorInfo = new NativeMonitorInfo
            {
                cbSize = Marshal.SizeOf<NativeMonitorInfo>()
            };

            IntPtr hwnd = windowHandle != IntPtr.Zero
                ? windowHandle
                : new WindowInteropHelper(this).Handle;
            if (hwnd == IntPtr.Zero)
                return false;

            IntPtr monitor = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
            return monitor != IntPtr.Zero && GetMonitorInfo(monitor, ref monitorInfo);
        }

        private void ApplyMonitorCoverPlacement(bool active)
        {
            if (!(isFullscreen || isMonitorCoverMaximized) || WindowState == WindowState.Minimized)
                return;

            IntPtr hwnd = windowHandle != IntPtr.Zero
                ? windowHandle
                : new WindowInteropHelper(this).Handle;
            if (hwnd == IntPtr.Zero || !TryGetCurrentMonitorInfo(out NativeMonitorInfo monitorInfo))
                return;

            int width = Math.Max(1, monitorInfo.rcMonitor.Right - monitorInfo.rcMonitor.Left);
            int height = Math.Max(1, monitorInfo.rcMonitor.Bottom - monitorInfo.rcMonitor.Top);

            // This is intentionally not WPF WindowState.Maximized. Windows reserves
            // taskbar space for maximized windows. A true full-monitor player is an
            // explicitly positioned top-level window whose outer rect equals rcMonitor.
            SetWindowPos(
                hwnd,
                active ? HWND_TOPMOST : HWND_NOTOPMOST,
                monitorInfo.rcMonitor.Left,
                monitorInfo.rcMonitor.Top,
                width,
                height,
                SWP_NOACTIVATE | SWP_NOOWNERZORDER);
        }

        private void ReleaseMonitorCoverZOrder()
        {
            IntPtr hwnd = windowHandle != IntPtr.Zero
                ? windowHandle
                : new WindowInteropHelper(this).Handle;
            if (hwnd == IntPtr.Zero)
                return;

            SetWindowPos(
                hwnd,
                HWND_NOTOPMOST,
                0,
                0,
                0,
                0,
                SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
        }

        private void UpdateTaskbarCoverState(bool active)
        {
            if (isFullscreen || isMonitorCoverMaximized)
                ApplyMonitorCoverPlacement(active);
            else
                ReleaseMonitorCoverZOrder();
        }

        private void ApplyDwmWindowAppearance(bool active)
        {
            IntPtr hwnd = new WindowInteropHelper(this).Handle;
            if (hwnd == IntPtr.Zero)
                return;

            try
            {
                int darkMode = 1;
                DwmSetWindowAttribute(
                    hwnd,
                    DWMWA_USE_IMMERSIVE_DARK_MODE,
                    ref darkMode,
                    Marshal.SizeOf<int>());

                int cornerPreference = (int)(isFullscreen || isMonitorCoverMaximized || WindowState == WindowState.Maximized
                    ? DwmWindowCornerPreference.DoNotRound
                    : DwmWindowCornerPreference.Round);
                DwmSetWindowAttribute(
                    hwnd,
                    DWMWA_WINDOW_CORNER_PREFERENCE,
                    ref cornerPreference,
                    Marshal.SizeOf<int>());

                int borderColor = isFullscreen
                    ? DWMWA_COLOR_NONE
                    : active
                        ? ToColorRef(70, 79, 94)
                        : ToColorRef(38, 43, 52);
                DwmSetWindowAttribute(
                    hwnd,
                    DWMWA_BORDER_COLOR,
                    ref borderColor,
                    Marshal.SizeOf<int>());
            }
            catch (DllNotFoundException)
            {
                // Older/non-Windows environments simply keep the default frame behavior.
            }
            catch (EntryPointNotFoundException)
            {
                // DWM attributes are best-effort and must never affect playback startup.
            }
        }

        private static int ToColorRef(byte red, byte green, byte blue)
        {
            return red | (green << 8) | (blue << 16);
        }

        private void PlayPauseBtn_Click(object sender, RoutedEventArgs e)
        {
            TogglePlayback();
        }

        private void SeekBack_Click(object sender, RoutedEventArgs e)
        {
            SeekRelative(-SeekStepMilliseconds);
        }

        private void SeekForward_Click(object sender, RoutedEventArgs e)
        {
            SeekRelative(SeekStepMilliseconds);
        }

        private void MuteBtn_Click(object sender, RoutedEventArgs e)
        {
            ToggleMute();
        }

        private void VolumeSlider_ValueChanged(object sender, RoutedPropertyChangedEventArgs<double> e)
        {
            // XAML can raise ValueChanged while InitializeComponent() is still
            // constructing the window. At that point the Flyleaf player has not
            // been assigned yet, so ignore constructor-time slider notifications.
            if (suppressVolumeChange || player is null)
                return;

            int newVolume = (int)Math.Round(e.NewValue);
            player.Audio.Volume = newVolume;
            MarkEpisodeSettingsDirty();

            if (newVolume > 0 && player.Audio.Mute)
                player.Audio.Mute = false;

            MuteGlyph.Text = newVolume <= 0 ? MuteIcon : VolumeIcon;
        }

        private void ProgressSlider_PreviewMouseDown(object sender, MouseButtonEventArgs e)
        {
            if (e.ChangedButton != MouseButton.Left)
                return;

            isDraggingSlider = true;

            // WPF's stock track click moves by LargeChange. For a media timeline that
            // feels broken, so jump directly to the clicked percentage instead.
            if (ProgressSlider.ActualWidth > 0)
            {
                double ratio = Math.Clamp(e.GetPosition(ProgressSlider).X / ProgressSlider.ActualWidth, 0.0, 1.0);
                ProgressSlider.Value = ProgressSlider.Minimum +
                    ((ProgressSlider.Maximum - ProgressSlider.Minimum) * ratio);
            }

            ShowControls();
            ShowCustomCursor();
            hideTimer.Stop();
        }

        private void ProgressSlider_PreviewMouseUp(object sender, MouseButtonEventArgs e)
        {
            if (!isDraggingSlider)
                return;

            isDraggingSlider = false;
            SeekToMilliseconds((long)Math.Round(ProgressSlider.Value), showFeedback: false);
            RestartHideTimer();
        }

        private void ProgressSlider_ValueChanged(object sender, RoutedPropertyChangedEventArgs<double> e)
        {
            if (!isDraggingSlider)
                return;

            CurTimeText.Text = PlayerUiLogic.FormatTime(e.NewValue);
            UpdateTimelineVisual(e.NewValue, ProgressSlider.Maximum);
        }

        private void Minimize_Click(object sender, RoutedEventArgs e)
        {
            WindowState = WindowState.Minimized;
        }

        private void Maximize_Click(object sender, RoutedEventArgs e)
        {
            ToggleMaximize();
        }

        private void Fullscreen_Click(object sender, RoutedEventArgs e)
        {
            ToggleFullscreen();
        }

        private void Close_Click(object sender, RoutedEventArgs e)
        {
            Close();
        }

        private async void Window_Closing(object? sender, CancelEventArgs e)
        {
            if (isClosing)
                return;

            FlushActivePlayerSettings();

            if (bridge != null && !bridgeCloseFinished)
            {
                e.Cancel = true;
                if (bridgeClosing) return;
                bridgeClosing = true;
                updateTimer.Stop();
                await CloseBridgeAsync();
                bridgeCloseFinished = true;
                Close();
                return;
            }

            isClosing = true;
            hideTimer.Stop();
            updateTimer.Stop();
            StopPlaybackProgressRendering();
            LocationChanged -= Window_LocationChanged;
            SizeChanged -= Window_SizeChanged;
            UpdateTaskbarCoverState(active: false);
            if (windowSource != null)
            {
                windowSource.RemoveHook(WindowProc);
                windowSource = null;
            }
            RestoreSystemCursor();

            player.OpenCompleted -= Player_OpenCompleted;

            try
            {
                player.Dispose();
            }
            catch
            {
                // Closing should continue even if the media backend is already torn down.
            }

            bridge?.Dispose();
        }
    }
}
