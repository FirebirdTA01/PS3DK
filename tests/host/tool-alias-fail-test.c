/* Harness for tests/host/tool-alias-fail-test.sh: tool-alias.c's fail()
   with a detail that fits and with one far longer than its message buffer.
   stderr is redirected to a file by the script, so fail() takes its
   WriteFile (UTF-8) path.  argv[1]: "short" or "long".  */

#define TOOL_ALIAS_NO_WMAIN
#include "../../tools/tool-alias/tool-alias.c"

int
wmain (int argc, wchar_t **argv)
{
  static wchar_t detail[801];
  if (argc > 1 && wcscmp (argv[1], L"long") == 0)
    {
      wmemset (detail, L'Z', 800);
      detail[800] = 0;
    }
  else
    wcscpy (detail, L"C:/sdk/ppu/bin/powerpc64-ps3-elf-gcc.exe");
  fail (L"missing", detail);
  return 0;
}
