using System;
using System.Runtime.InteropServices;
using System.Threading;
using System.Threading.Tasks;

namespace NxRemotePlay.Client.Platform.Specific.Win
{
    // Native Windows "choose a folder" dialog (SHBrowseForFolder) on its own STA thread, so the render loop never blocks.
    internal static class WinFolderDialog
    {
        [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
        struct BROWSEINFO
        {
            public IntPtr hwndOwner, pidlRoot, pszDisplayName;
            public string lpszTitle;
            public uint ulFlags;
            public IntPtr lpfn, lParam;
            public int iImage;
        }
        delegate int BrowseCallback(IntPtr hwnd, uint msg, IntPtr lParam, IntPtr lpData);

        [DllImport("shell32.dll", CharSet = CharSet.Unicode)] static extern IntPtr SHBrowseForFolderW(ref BROWSEINFO bi);
        [DllImport("shell32.dll", CharSet = CharSet.Unicode)] static extern bool SHGetPathFromIDListW(IntPtr pidl, [Out] char[] path);
        [DllImport("ole32.dll")] static extern void CoTaskMemFree(IntPtr pv);
        [DllImport("user32.dll", CharSet = CharSet.Unicode)] static extern IntPtr SendMessageW(IntPtr hwnd, uint msg, IntPtr wParam, IntPtr lParam);

        const uint BIF_RETURNONLYFSDIRS = 0x1, BIF_EDITBOX = 0x10, BIF_NEWDIALOGSTYLE = 0x40;
        const uint BFFM_INITIALIZED = 1, BFFM_SETSELECTIONW = 0x400 + 103;

        static int Callback(IntPtr hwnd, uint msg, IntPtr lParam, IntPtr lpData)
        {
            if (msg == BFFM_INITIALIZED && lpData != IntPtr.Zero) SendMessageW(hwnd, BFFM_SETSELECTIONW, (IntPtr)1, lpData);
            return 0;
        }

        // Completes with the chosen folder, or null when the dialog was cancelled or is unavailable.
        public static Task<string?> Pick(string title, string? initialFolder)
        {
            var done = new TaskCompletionSource<string?>(TaskCreationOptions.RunContinuationsAsynchronously);
            {}
            var thread = new Thread(() =>
            {
                IntPtr name = Marshal.AllocHGlobal(520), initial = IntPtr.Zero, pidl = IntPtr.Zero;
                try
                {
                    BrowseCallback callback = Callback; // kept alive for the call
                    if (!string.IsNullOrWhiteSpace(initialFolder)) initial = Marshal.StringToHGlobalUni(initialFolder);
                    var info = new BROWSEINFO
                    {
                        pszDisplayName = name,
                        lpszTitle = title,
                        ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE | BIF_EDITBOX,
                        lpfn = Marshal.GetFunctionPointerForDelegate(callback),
                        lParam = initial,
                    };
                    pidl = SHBrowseForFolderW(ref info);
                    if (pidl == IntPtr.Zero) { done.SetResult(null); return; }
                    var path = new char[520];
                    done.SetResult(SHGetPathFromIDListW(pidl, path) ? new string(path).TrimEnd('\0') : null);
                    GC.KeepAlive(callback);
                }
                catch (Exception) { done.TrySetResult(null); }
                finally
                {
                    if (pidl != IntPtr.Zero) CoTaskMemFree(pidl);
                    if (initial != IntPtr.Zero) Marshal.FreeHGlobal(initial);
                    Marshal.FreeHGlobal(name);
                }
            })
            { IsBackground = true, Name = "folder-dialog" };
            thread.SetApartmentState(ApartmentState.STA);
            thread.Start();
            return done.Task;
        }
    }
}
