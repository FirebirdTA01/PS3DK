// PPU C++17 runtime/link regression for t_1a75e28a. Run against a rebuilt SDK.
// Every removed path belongs to the directory created by this invocation.
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <system_error>
#include <unistd.h>
#include <sys/process.h>
#include <sys/sys_time.h>
#include <sys/stat.h>

SYS_PROCESS_PARAM(1001, 0x10000);
namespace fs = std::filesystem;
static int failures;
static void check(bool ok, const char *label)
{
    std::printf("%s filesystem: %s\n", ok ? "PASS" : "FAIL", label);
    if (!ok) ++failures;
}

int main()
{
    std::error_code ec;
    const fs::path cwd = fs::current_path(ec);
    check(!ec && !cwd.empty(), "current_path");
    char name[128];
    std::snprintf(name, sizeof name, "/dev_hdd0/tmp/ps3dk-filesystem-%lu-%llu",
                  (unsigned long)sysProcessGetPid(),
                  (unsigned long long)sys_time_get_system_time());
    const fs::path root(name), file = root / "known-entry.txt";
    const fs::path sub = root / "sub", copy = sub / "copy.txt", link = root / "link";
    const bool created = fs::create_directory(root, ec);
    check(created && !ec, "create fresh directory");
    if (!created || ec) return 1;

    FILE *out = std::fopen(file.c_str(), "wb");
    bool wrote = false;
    if (out) {
        wrote = std::fwrite("known", 1, 5, out) == 5;
        wrote = std::fclose(out) == 0 && wrote;
    }
    check(wrote, "create file");
    const auto status = fs::status(file, ec);
    check(!ec && fs::is_regular_file(status), "stat known file");
    bool found = false;
    fs::directory_iterator it(root, ec), end;
    while (!ec && it != end) {
        if (it->path().filename() == "known-entry.txt") found = true;
        it.increment(ec);
    }
    check(!ec && found, "ordinary directory_iterator yields known entry");
    check(fs::create_directory(sub, ec) && !ec, "create child directory");
    check(fs::copy_file(file, copy, ec) && !ec, "copy_file uses supported permission fallback");
    fs::permissions(file, fs::perms::owner_read | fs::perms::owner_write, ec);
    check(!ec, "permissions uses chmod fallback");

    fs::create_symlink(file, link, ec);
    check(ec == std::errc::function_not_supported, "symlink reports unsupported via error_code");
    check(!fs::exists(link, ec) && !ec, "failed symlink created no file");
    (void)fs::read_symlink(file, ec);
    check(ec == std::errc::function_not_supported, "read_symlink reports unsupported via error_code");

    check(pathconf(name, _PC_PATH_MAX) == 1024, "pathconf PATH_MAX");
    check(pathconf(name, _PC_NAME_MAX) == 255, "pathconf NAME_MAX");
    struct stat dot_status;
    errno = 0;
    const int dot_rc = stat(".", &dot_status);
    std::printf("DIAG filesystem: plain stat dot rc=%d errno=%d cwd=%s\n",
                dot_rc, errno, cwd.c_str());
    check(pathconf(".", _PC_PATH_MAX) == 1024, "pathconf relative directory");
    errno = 0;
    check(pathconf(name, -1234) == -1 && errno == EINVAL, "pathconf unsupported selector");
    const fs::path missing = root / "missing";
    errno = 0;
    check(pathconf(missing.c_str(), _PC_PATH_MAX) == -1 && errno == ENOENT, "pathconf missing path");

    check(fs::remove(copy, ec) && !ec, "remove copied file");
    check(fs::remove(sub, ec) && !ec, "remove child directory");
    check(fs::remove(file, ec) && !ec, "remove file");
    check(fs::remove(root, ec) && !ec, "remove owned directory");
    std::printf("FILESYSTEM_RESULT failures=%d\n", failures);
    return failures ? 1 : 0;
}
