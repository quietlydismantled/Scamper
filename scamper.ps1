param(
    [ValidateRange(1, 3600)]
    [int]$IntervalSeconds = 15,

    [ValidateRange(1, 20)]
    [int]$PixelsPerStep = 5,

    # 0 moves all icons; otherwise choose this many distinct icons each tick.
    [ValidateRange(0, 2147483647)]
    [int]$IconsPerTick = 0,

    # Drift only while the foreground app covers an entire monitor.
    [switch]$FullscreenOnly,

    [switch]$Restore
)

$ErrorActionPreference = 'Stop'
$statePath = Join-Path $PSScriptRoot 'DesktopDrift-OriginalLayout.json'

# Give each C# revision its own namespace: dot-sourcing can retain older types.
$nativeSource = @"
using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using System.Text;

namespace DesktopDrift {
    [StructLayout(LayoutKind.Sequential)]
    public struct POINT { public int X; public int Y; }

    [StructLayout(LayoutKind.Sequential)]
    public struct RECT { public int Left, Top, Right, Bottom; }

    [StructLayout(LayoutKind.Sequential)]
    struct MONITORINFO {
        public uint Size;
        public RECT Monitor, WorkArea;
        public uint Flags;
    }

    public sealed class IconPosition {
        public string Key { get; set; }
        public int X { get; set; }
        public int Y { get; set; }
    }

    public sealed class DesktopSnapshot {
        public uint FolderFlags { get; set; }
        public IconPosition[] Items { get; set; }
    }

    [ComImport, Guid("85CB6900-4D95-11CF-960C-0080C7F4EE85"), InterfaceType(ComInterfaceType.InterfaceIsDual)]
    interface IShellWindows {
        [PreserveSig] int GetCount(out int count);
        [PreserveSig] int Item([MarshalAs(UnmanagedType.Struct)] object index, [MarshalAs(UnmanagedType.IDispatch)] out object dispatch);
        [PreserveSig] int _NewEnum(out IntPtr enumVariant);
        [PreserveSig] int Register(IntPtr pid, int hwnd, int swClass, out int cookie);
        [PreserveSig] int RegisterPending(int threadId, ref object location, ref object root, int swClass, out int cookie);
        [PreserveSig] int Revoke(int cookie);
        [PreserveSig] int OnNavigate(int cookie, ref object location);
        [PreserveSig] int OnActivated(int cookie, bool active);
        [PreserveSig] int FindWindowSW(ref object location, ref object root, int swClass, out int hwnd, int options, [MarshalAs(UnmanagedType.IDispatch)] out object dispatch);
        [PreserveSig] int OnCreated(int cookie, IntPtr unknown);
        [PreserveSig] int ProcessAttachDetach(bool attach);
    }

    [ComImport, Guid("6D5140C1-7436-11CE-8034-00AA006009FA"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    interface IServiceProvider {
        [PreserveSig] int QueryService(ref Guid service, ref Guid iid, out IntPtr result);
    }

    [ComImport, Guid("000214E2-0000-0000-C000-000000000046"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    interface IShellBrowser {
        [PreserveSig] int GetWindow(out IntPtr hwnd);
        [PreserveSig] int ContextSensitiveHelp(bool enterMode);
        [PreserveSig] int InsertMenusSB(IntPtr sharedMenu, IntPtr widths);
        [PreserveSig] int SetMenuSB(IntPtr sharedMenu, IntPtr oleMenu, IntPtr activeObject);
        [PreserveSig] int RemoveMenusSB(IntPtr sharedMenu);
        [PreserveSig] int SetStatusTextSB([MarshalAs(UnmanagedType.LPWStr)] string text);
        [PreserveSig] int EnableModelessSB(bool enable);
        [PreserveSig] int TranslateAcceleratorSB(IntPtr msg, ushort id);
        [PreserveSig] int BrowseObject(IntPtr pidl, uint flags);
        [PreserveSig] int GetViewStateStream(uint mode, out IntPtr stream);
        [PreserveSig] int GetControlWindow(uint id, out IntPtr hwnd);
        [PreserveSig] int SendControlMsg(uint id, uint msg, UIntPtr wParam, IntPtr lParam, out IntPtr result);
        [PreserveSig] int QueryActiveShellView(out IntPtr shellView);
        [PreserveSig] int OnViewWindowActive(IntPtr shellView);
        [PreserveSig] int SetToolbarItems(IntPtr buttons, uint count, uint flags);
    }

    [ComImport, Guid("000214F2-0000-0000-C000-000000000046"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    interface IEnumIDList {
        [PreserveSig] int Next(uint count, out IntPtr item, out uint fetched);
        [PreserveSig] int Skip(uint count);
        [PreserveSig] int Reset();
        [PreserveSig] int Clone(out IEnumIDList clone);
    }

    // COM inheritance is flattened here: every native slot must appear in order.
    [ComImport, Guid("1AF3A467-214F-4298-908E-06B03E0B39F9"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    interface IFolderView2 {
        [PreserveSig] int GetCurrentViewMode(out uint mode);
        [PreserveSig] int SetCurrentViewMode(uint mode);
        [PreserveSig] int GetFolder(ref Guid iid, out IntPtr folder);
        [PreserveSig] int Item(int index, out IntPtr pidl);
        [PreserveSig] int ItemCount(uint flags, out int count);
        [PreserveSig] int Items(uint flags, ref Guid iid, [MarshalAs(UnmanagedType.Interface)] out IEnumIDList items);
        [PreserveSig] int GetSelectionMarkedItem(out int index);
        [PreserveSig] int GetFocusedItem(out int index);
        [PreserveSig] int GetItemPosition(IntPtr pidl, out POINT point);
        [PreserveSig] int GetSpacing(out POINT point);
        [PreserveSig] int GetDefaultSpacing(out POINT point);
        [PreserveSig] int GetAutoArrange();
        [PreserveSig] int SelectItem(int index, uint flags);
        [PreserveSig] int SelectAndPositionItems(uint count,
            [In, MarshalAs(UnmanagedType.LPArray, SizeParamIndex=0)] IntPtr[] pidls,
            [In, MarshalAs(UnmanagedType.LPArray, SizeParamIndex=0)] POINT[] points,
            uint flags);
        [PreserveSig] int SetGroupBy(IntPtr key, IntPtr descending);
        [PreserveSig] int GetGroupBy(IntPtr key, IntPtr descending);
        [PreserveSig] int SetViewProperty(IntPtr pidl, IntPtr key, IntPtr value);
        [PreserveSig] int GetViewProperty(IntPtr pidl, IntPtr key, IntPtr value);
        [PreserveSig] int SetTileViewProperties(IntPtr pidl, IntPtr properties);
        [PreserveSig] int SetExtendedTileViewProperties(IntPtr pidl, IntPtr properties);
        [PreserveSig] int SetText(IntPtr type, IntPtr text);
        [PreserveSig] int SetCurrentFolderFlags(uint mask, uint flags);
        [PreserveSig] int GetCurrentFolderFlags(out uint flags);
    }

    public static class Native {
        const uint SVGIO_ALLVIEW = 2;
        const uint SVSI_POSITIONITEM = 0x00000080;
        const uint FWF_AUTOARRANGE = 0x00000001;
        const uint FWF_SNAPTOGRID = 0x00000004;
        const uint POSITION_FLAGS = FWF_AUTOARRANGE | FWF_SNAPTOGRID;
        static readonly Guid SID_STopLevelBrowser = new Guid("4C96BE40-915C-11CF-99D3-00AA004AE837");
        static readonly Guid IID_IShellBrowser = new Guid("000214E2-0000-0000-C000-000000000046");
        static readonly Guid IID_IFolderView2 = new Guid("1AF3A467-214F-4298-908E-06B03E0B39F9");
        static readonly Guid IID_IEnumIDList = new Guid("000214F2-0000-0000-C000-000000000046");
        static readonly Random Randomizer = new Random();

        static bool CoversMonitor(RECT window, RECT monitor) {
            const int tolerance = 2;
            return window.Right > window.Left && window.Bottom > window.Top
                && window.Left <= monitor.Left + tolerance
                && window.Top <= monitor.Top + tolerance
                && window.Right >= monitor.Right - tolerance
                && window.Bottom >= monitor.Bottom - tolerance;
        }

        public static bool IsForegroundFullscreen() {
            IntPtr window = GetForegroundWindow();
            if (window == IntPtr.Zero || window == GetShellWindow() || window == GetDesktopWindow()
                || !IsWindowVisible(window) || IsIconic(window)) return false;
            var className = new StringBuilder(256);
            GetClassName(window, className, className.Capacity);
            string name = className.ToString();
            if (name == "Progman" || name == "WorkerW" || name == "Shell_TrayWnd"
                || name == "Shell_SecondaryTrayWnd") return false;

            // Compare physical screen coordinates even with mixed monitor scaling.
            IntPtr oldDpiContext = SetThreadDpiAwarenessContext(new IntPtr(-4));
            try {
                var monitor = new MONITORINFO { Size = (uint)Marshal.SizeOf(typeof(MONITORINFO)) };
                IntPtr handle = MonitorFromWindow(window, 2);
                if (handle == IntPtr.Zero || !GetMonitorInfo(handle, ref monitor)) return false;
                RECT bounds;
                if (DwmGetWindowAttribute(window, 9, out bounds, Marshal.SizeOf(typeof(RECT))) < 0
                    && !GetWindowRect(window, out bounds)) return false;
                return CoversMonitor(bounds, monitor.Monitor);
            } finally {
                if (oldDpiContext != IntPtr.Zero) SetThreadDpiAwarenessContext(oldDpiContext);
            }
        }

        static List<IconPosition> PickIcons(List<IconPosition> items, int limit) {
            int count = limit == 0 ? items.Count : Math.Min(limit, items.Count);
            // Partial Fisher-Yates shuffle: sample without selecting an icon twice.
            for (int i = 0; i < count; i++) {
                int other = Randomizer.Next(i, items.Count);
                IconPosition saved = items[i];
                items[i] = items[other];
                items[other] = saved;
            }
            return items.GetRange(0, count);
        }

        static void Check(int hr, string action) {
            if (hr < 0) throw new COMException(action + " failed (0x" + hr.ToString("X8") + ").", hr);
        }

        sealed class DesktopConnection : IDisposable {
            public IFolderView2 View;
            public object Windows, Dispatch, Browser;
            public void Dispose() {
                var objects = new List<object>();
                foreach (object value in new object[] { View, Browser, Dispatch, Windows }) {
                    if (value == null || !Marshal.IsComObject(value)) continue;
                    bool seen = objects.Exists(o => Object.ReferenceEquals(o, value));
                    if (!seen) objects.Add(value);
                }
                foreach (object value in objects) Marshal.ReleaseComObject(value);
            }
        }

        static DesktopConnection GetView() {
            var connection = new DesktopConnection();
            IntPtr browserPtr = IntPtr.Zero, shellViewPtr = IntPtr.Zero, viewPtr = IntPtr.Zero;
            try {
                Type windowsType = Type.GetTypeFromCLSID(new Guid("9BA05972-F6A8-11CF-A442-00A0C90A8F39"));
                connection.Windows = Activator.CreateInstance(windowsType);
                object location = 0, root = null;
                int hwnd;
                object desktopDispatch;
                Check(((IShellWindows)connection.Windows).FindWindowSW(ref location, ref root, 8, out hwnd, 1, out desktopDispatch), "Find desktop window");
                if (desktopDispatch == null) throw new InvalidOperationException("Could not find the Windows desktop view.");
                connection.Dispatch = desktopDispatch;
                Guid service = SID_STopLevelBrowser, browserIid = IID_IShellBrowser;
                Check(((IServiceProvider)desktopDispatch).QueryService(ref service, ref browserIid, out browserPtr), "Find desktop shell browser");
                connection.Browser = Marshal.GetObjectForIUnknown(browserPtr);
                Check(((IShellBrowser)connection.Browser).QueryActiveShellView(out shellViewPtr), "Find desktop shell view");
                Guid viewIid = IID_IFolderView2;
                Check(Marshal.QueryInterface(shellViewPtr, in viewIid, out viewPtr), "Get desktop folder view");
                connection.View = (IFolderView2)Marshal.GetObjectForIUnknown(viewPtr);
                return connection;
            } catch {
                connection.Dispose();
                throw;
            } finally {
                if (viewPtr != IntPtr.Zero) Marshal.Release(viewPtr);
                if (shellViewPtr != IntPtr.Zero) Marshal.Release(shellViewPtr);
                if (browserPtr != IntPtr.Zero) Marshal.Release(browserPtr);
            }
        }

        static byte[] CopyPidl(IntPtr pidl) {
            int size = 0;
            while (true) {
                ushort part = unchecked((ushort)Marshal.ReadInt16(pidl, size));
                size += 2;
                if (part == 0) break;
                size += part - 2;
                if (size > 65536) throw new InvalidOperationException("Invalid desktop item identifier.");
            }
            byte[] bytes = new byte[size];
            Marshal.Copy(pidl, bytes, 0, size);
            return bytes;
        }

        static IntPtr MakePidl(string key) {
            byte[] bytes = Convert.FromBase64String(key);
            IntPtr pidl = Marshal.AllocCoTaskMem(bytes.Length);
            Marshal.Copy(bytes, 0, pidl, bytes.Length);
            return pidl;
        }

        static List<IconPosition> ReadPositions(IFolderView2 view) {
            var result = new List<IconPosition>();
            Guid iid = IID_IEnumIDList;
            IEnumIDList enumerator;
            Check(view.Items(SVGIO_ALLVIEW, ref iid, out enumerator), "Enumerate desktop icons");
            try {
                while (true) {
                    IntPtr pidl;
                    uint fetched;
                    int hr = enumerator.Next(1, out pidl, out fetched);
                    if (hr < 0) Marshal.ThrowExceptionForHR(hr);
                    if (fetched == 0) break;
                    try {
                        POINT point;
                        Check(view.GetItemPosition(pidl, out point), "Read desktop icon position");
                        result.Add(new IconPosition { Key = Convert.ToBase64String(CopyPidl(pidl)), X = point.X, Y = point.Y });
                    } finally { ILFree(pidl); }
                }
            } finally { if (enumerator != null && Marshal.IsComObject(enumerator)) Marshal.ReleaseComObject(enumerator); }
            return result;
        }

        static void Apply(IFolderView2 view, List<IconPosition> items) {
            if (items.Count == 0) return;
            var pidls = new IntPtr[items.Count];
            var points = new POINT[items.Count];
            try {
                for (int i = 0; i < items.Count; i++) {
                    pidls[i] = MakePidl(items[i].Key);
                    points[i].X = items[i].X;
                    points[i].Y = items[i].Y;
                }
                Check(view.SelectAndPositionItems((uint)items.Count, pidls, points, SVSI_POSITIONITEM), "Move desktop icons");
            } finally { foreach (IntPtr p in pidls) if (p != IntPtr.Zero) Marshal.FreeCoTaskMem(p); }
        }

        public static DesktopSnapshot Capture() {
            using (var connection = GetView()) {
                uint folderFlags;
                Check(connection.View.GetCurrentFolderFlags(out folderFlags), "Read desktop layout settings");
                return new DesktopSnapshot { FolderFlags = folderFlags, Items = ReadPositions(connection.View).ToArray() };
            }
        }

        public static void BeginDrift() {
            using (var connection = GetView()) {
                Check(connection.View.SetCurrentFolderFlags(POSITION_FLAGS, 0), "Turn off desktop auto-arrange and grid snapping");
            }
        }

        public static int Drift(int maxPixels, int iconsPerTick, bool fullscreenOnly) {
            if (maxPixels < 1) throw new ArgumentOutOfRangeException("maxPixels");
            if (iconsPerTick < 0) throw new ArgumentOutOfRangeException("iconsPerTick");
            if (fullscreenOnly && !IsForegroundFullscreen()) return 0;
            using (var connection = GetView()) {
                IFolderView2 view = connection.View;
                var items = PickIcons(ReadPositions(view), iconsPerTick);
                foreach (var item in items) {
                    int dx = Randomizer.Next(-maxPixels, maxPixels + 1);
                    int dy = Randomizer.Next(-maxPixels, maxPixels + 1);
                    if (dx == 0 && dy == 0) dx = 1;
                    item.X += dx;
                    item.Y += dy;
                }
                // Recheck in case the presenter left fullscreen while enumerating icons.
                if (fullscreenOnly && !IsForegroundFullscreen()) return 0;
                Apply(view, items);
                return items.Count;
            }
        }

        public static void Restore(DesktopSnapshot snapshot) {
            if (snapshot == null) throw new ArgumentNullException("snapshot");
            using (var connection = GetView()) {
                IFolderView2 view = connection.View;
                Check(view.SetCurrentFolderFlags(POSITION_FLAGS, 0), "Disable automatic arrangement for restore");
                Apply(view, new List<IconPosition>(snapshot.Items ?? new IconPosition[0]));
                Check(view.SetCurrentFolderFlags(POSITION_FLAGS, snapshot.FolderFlags & POSITION_FLAGS), "Restore desktop arrangement settings");
            }
        }

        [DllImport("shell32.dll", ExactSpelling=true)]
        static extern void ILFree(IntPtr pidl);

        [DllImport("user32.dll")] static extern IntPtr GetForegroundWindow();
        [DllImport("user32.dll")] static extern IntPtr GetShellWindow();
        [DllImport("user32.dll")] static extern IntPtr GetDesktopWindow();
        [DllImport("user32.dll")] [return: MarshalAs(UnmanagedType.Bool)]
        static extern bool IsWindowVisible(IntPtr hwnd);
        [DllImport("user32.dll")] [return: MarshalAs(UnmanagedType.Bool)]
        static extern bool IsIconic(IntPtr hwnd);
        [DllImport("user32.dll", CharSet=CharSet.Unicode)]
        static extern int GetClassName(IntPtr hwnd, StringBuilder name, int count);
        [DllImport("user32.dll")] static extern IntPtr SetThreadDpiAwarenessContext(IntPtr context);
        [DllImport("user32.dll")] static extern IntPtr MonitorFromWindow(IntPtr hwnd, uint flags);
        [DllImport("user32.dll", CharSet=CharSet.Unicode)] [return: MarshalAs(UnmanagedType.Bool)]
        static extern bool GetMonitorInfo(IntPtr monitor, ref MONITORINFO info);
        [DllImport("user32.dll")] [return: MarshalAs(UnmanagedType.Bool)]
        static extern bool GetWindowRect(IntPtr hwnd, out RECT rect);
        [DllImport("dwmapi.dll")]
        static extern int DwmGetWindowAttribute(IntPtr hwnd, uint attribute, out RECT rect, int size);
    }
}
"@

$hasher = [System.Security.Cryptography.SHA256]::Create()
try {
    $sourceHash = [BitConverter]::ToString($hasher.ComputeHash([Text.Encoding]::UTF8.GetBytes($nativeSource))).Replace('-', '')
} finally {
    $hasher.Dispose()
}
$nativeNamespace = "DesktopDrift.Build_$sourceHash"
$nativeType = "$nativeNamespace.Native" -as [type]
if ($null -eq $nativeType) {
    Add-Type -TypeDefinition $nativeSource.Replace('namespace DesktopDrift {', "namespace $nativeNamespace {")
    $nativeType = "$nativeNamespace.Native" -as [type]
}
$iconPositionType = "$nativeNamespace.IconPosition" -as [type]
$snapshotType = "$nativeNamespace.DesktopSnapshot" -as [type]

if ($Restore) {
    if (-not (Test-Path -LiteralPath $statePath)) {
        throw "No saved layout found at '$statePath'. Run the script once before using -Restore."
    }
    $snapshot = Get-Content -LiteralPath $statePath -Raw | ConvertFrom-Json
    $items = @($snapshot.Items | ForEach-Object {
        $item = $iconPositionType::new()
        $item.Key = [string]$_.Key
        $item.X = [int]$_.X
        $item.Y = [int]$_.Y
        $item
    })
    $nativeSnapshot = $snapshotType::new()
    $nativeSnapshot.FolderFlags = [uint32]$snapshot.FolderFlags
    $nativeSnapshot.Items = $items
    $nativeType::Restore($nativeSnapshot)
    Remove-Item -LiteralPath $statePath -Force
    Write-Host 'Restored the saved desktop icon positions and arrangement settings.'
    return
}

if (-not (Test-Path -LiteralPath $statePath)) {
    $snapshot = $nativeType::Capture()
    $snapshot | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath $statePath -Encoding UTF8
}

$nativeType::BeginDrift()
$countDescription = if ($IconsPerTick -eq 0) { 'all icons' } else { "up to $IconsPerTick randomly chosen icons" }
$conditionDescription = if ($FullscreenOnly) { ' Only ticks while the foreground app fills a monitor; other ticks are skipped.' } else { '' }
Write-Host "Desktop icon drift is running. Every $IntervalSeconds seconds, $countDescription move by up to $PixelsPerStep pixels.$conditionDescription Press Ctrl+C to stop the drift; icons will stay where they are. Run this script with -Restore to put them back."

while ($true) {
    Start-Sleep -Seconds $IntervalSeconds
    [void]$nativeType::Drift($PixelsPerStep, $IconsPerTick, $FullscreenOnly.IsPresent)
}








