/* tool-alias: the short ppu-* and spu-* tool names on Windows.

   PSL1GHT-style Makefiles (ppu_rules, spu_rules and third-party Makefiles
   that set LD := ppu-ld and the like) call the toolchain by its short
   names.  On Linux those are symlinks.  A zip cannot hold symlinks, and a
   full copy of every driver and binutils program under a second name would
   add about 240 MB to the package, so each short name is a copy of this
   small program instead.  It looks at the name it was started as, maps

       ppu-<tool>.exe  ->  powerpc64-ps3-elf-<tool>.exe
       spu-<tool>.exe  ->  spu-elf-<tool>.exe

   in its own directory, runs that program with the original argument text
   unchanged, and exits with its exit code.  The console, standard handles
   and environment are inherited.  */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <wchar.h>

static void
fail (const wchar_t *what, const wchar_t *detail)
{
  HANDLE err = GetStdHandle (STD_ERROR_HANDLE);
  DWORD n;
  wchar_t msg[2 * MAX_PATH + 64];
  int len = _snwprintf (msg, sizeof msg / sizeof msg[0] - 1,
			L"tool-alias: %ls: %ls\r\n", what, detail);
  if (len < 0)
    len = (int) wcslen (msg);
  msg[sizeof msg / sizeof msg[0] - 1] = 0;
  if (GetConsoleMode (err, &n))
    WriteConsoleW (err, msg, (DWORD) len, &n, NULL);
  else
    {
      char buf[4 * (2 * MAX_PATH + 64)];
      int bytes = WideCharToMultiByte (CP_UTF8, 0, msg, len, buf,
				       sizeof buf, NULL, NULL);
      WriteFile (err, buf, (DWORD) bytes, &n, NULL);
    }
}

/* The argument text after the program name, as CreateProcess received it.
   argv[0] ends at the closing quote when it starts with one, otherwise at
   the first space or tab; that is the rule the C runtime itself uses for
   the program name.  */
static const wchar_t *
arguments_after_program_name (const wchar_t *cmd)
{
  if (*cmd == L'"')
    {
      cmd++;
      while (*cmd && *cmd != L'"')
	cmd++;
      if (*cmd == L'"')
	cmd++;
    }
  else
    while (*cmd && *cmd != L' ' && *cmd != L'\t')
      cmd++;
  while (*cmd == L' ' || *cmd == L'\t')
    cmd++;
  return cmd;
}

int
wmain (void)
{
  static wchar_t self[32768], target[32768];
  DWORD len = GetModuleFileNameW (NULL, self, sizeof self / sizeof self[0]);
  if (len == 0 || len >= sizeof self / sizeof self[0])
    {
      fail (L"cannot read own path", L"GetModuleFileName failed");
      return 127;
    }

  wchar_t *base = self + len;
  while (base > self && base[-1] != L'\\' && base[-1] != L'/')
    base--;

  const wchar_t *long_prefix;
  size_t short_len;
  if (_wcsnicmp (base, L"ppu-", 4) == 0)
    long_prefix = L"powerpc64-ps3-elf-", short_len = 4;
  else if (_wcsnicmp (base, L"spu-", 4) == 0)
    long_prefix = L"spu-elf-", short_len = 4;
  else
    {
      fail (base, L"name must start with ppu- or spu-");
      return 127;
    }

  size_t dir_len = (size_t) (base - self);
  size_t need = dir_len + wcslen (long_prefix) + wcslen (base + short_len) + 1;
  if (need > sizeof target / sizeof target[0])
    {
      fail (base, L"path too long");
      return 127;
    }
  wmemcpy (target, self, dir_len);
  target[dir_len] = 0;
  wcscat (target, long_prefix);
  wcscat (target, base + short_len);

  if (GetFileAttributesW (target) == INVALID_FILE_ATTRIBUTES)
    {
      fail (L"missing", target);
      return 127;
    }

  const wchar_t *args = arguments_after_program_name (GetCommandLineW ());
  size_t cmd_len = wcslen (target) + wcslen (args) + 4;
  wchar_t *cmd = HeapAlloc (GetProcessHeap (), 0, cmd_len * sizeof (wchar_t));
  if (!cmd)
    {
      fail (base, L"out of memory");
      return 127;
    }
  _snwprintf (cmd, cmd_len, L"\"%ls\"%ls%ls", target, *args ? L" " : L"", args);
  cmd[cmd_len - 1] = 0;

  STARTUPINFOW si;
  PROCESS_INFORMATION pi;
  ZeroMemory (&si, sizeof si);
  si.cb = sizeof si;
  si.dwFlags = STARTF_USESTDHANDLES;
  si.hStdInput = GetStdHandle (STD_INPUT_HANDLE);
  si.hStdOutput = GetStdHandle (STD_OUTPUT_HANDLE);
  si.hStdError = GetStdHandle (STD_ERROR_HANDLE);

  if (!CreateProcessW (target, cmd, NULL, NULL, TRUE, 0, NULL, NULL, &si, &pi))
    {
      fail (L"cannot start", target);
      return 127;
    }

  /* Ctrl-C reaches the child through the shared console; this process just
     waits for it and reports its status.  */
  SetConsoleCtrlHandler (NULL, TRUE);
  WaitForSingleObject (pi.hProcess, INFINITE);
  DWORD code = 1;
  GetExitCodeProcess (pi.hProcess, &code);
  CloseHandle (pi.hThread);
  CloseHandle (pi.hProcess);
  return (int) code;
}
