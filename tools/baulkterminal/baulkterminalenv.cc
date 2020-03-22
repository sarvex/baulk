/// baulk terminal vsenv initialize
#include <bela/io.hpp>
#include <bela/process.hpp> // run vswhere
#include <bela/path.hpp>
#include <filesystem>
#include <regutils.hpp>
#include <jsonex.hpp>
#include "baulkterminal.hpp"

namespace baulkterminal {
#ifdef _M_X64
// Always build x64 binary
[[maybe_unused]] constexpr std::wstring_view arch = L"x64"; // Hostx64 x64
#else
[[maybe_unused]] constexpr std::wstring_view arch = L"x86"; // Hostx86 x86
#endif

struct VisualStudioInstance {
  std::wstring installationPath;
  std::wstring installationVersion;
  std::wstring instanceId;
  std::wstring productId;
  bool isLaunchable{true};
  bool isPrerelease{false};
  bool Encode(const std::string_view result, bela::error_code &ec) {
    try {
      auto j0 = nlohmann::json::parse(result);
      if (!j0.is_array() || j0.empty()) {
        ec = bela::make_error_code(1, L"empty visual studio instance");
        return false;
      }
      baulk::json::JsonAssignor ja(j0[0]);
      installationPath = ja.get("installationPath");
      installationVersion = ja.get("installationVersion");
      instanceId = ja.get("instanceId");
      productId = ja.get("productId");
      isLaunchable = ja.boolean("isLaunchable");
      isPrerelease = ja.boolean("isPrerelease");
    } catch (const std::exception &e) {
      ec = bela::make_error_code(1, bela::ToWide(e.what()));
      return false;
    }
    return true;
  }
};

inline std::optional<std::wstring>
LookupVisualCppVersion(std::wstring_view vsdir, bela::error_code &ec) {
  auto file = bela::StringCat(
      vsdir, L"/VC/Auxiliary/Build/Microsoft.VCToolsVersion.default.txt");
  return bela::io::ReadLine(file, ec);
}

// const fs::path vswhere_exe = program_files_32_bit / "Microsoft Visual Studio"
// / "Installer" / "vswhere.exe";
std::optional<std::wstring> LookupVsWhere() {
  constexpr std::wstring_view relativevswhere =
      L"/Microsoft Visual Studio/Installer/vswhere.exe";
  if (auto vswhere_exe =
          bela::StringCat(bela::GetEnv(L"ProgramFiles(x86)"), relativevswhere);
      bela::PathExists(vswhere_exe)) {
    return std::make_optional(std::move(vswhere_exe));
  }
  if (auto vswhere_exe =
          bela::StringCat(bela::GetEnv(L"ProgramFiles"), relativevswhere);
      bela::PathExists(vswhere_exe)) {
    return std::make_optional(std::move(vswhere_exe));
  }
  return std::nullopt;
}

std::optional<VisualStudioInstance>
LookupVisualStudioInstance(bela::error_code &ec) {
  auto vswhere_exe = LookupVsWhere();
  if (!vswhere_exe) {
    ec = bela::make_error_code(-1, L"vswhere not installed");
    return std::nullopt;
  }
  bela::process::Process process;
  // Force -utf8 convert to UTF8
  if (process.Capture(*vswhere_exe, L"-format", L"json", L"-utf8") != 0) {
    if (ec = process.ErrorCode(); !ec) {
      ec = bela::make_error_code(process.ExitCode(), L"vswhere exit with: ",
                                 process.ExitCode());
    }
    return std::nullopt;
  }
  VisualStudioInstance vsi;
  if (!vsi.Encode(process.Out(), ec)) {
    return std::nullopt;
  }
  return std::make_optional(std::move(vsi));
}
// HKEY_LOCAL_MACHINE\SOFTWARE\WOW6432Node\Microsoft\Microsoft
// SDKs\Windows\v10.0 InstallationFolder ProductVersion

struct Searcher {
  using vector_t = std::vector<std::wstring>;
  vector_t paths;
  vector_t libs;
  vector_t includes;
  vector_t libpaths;
  bool JoinEnvInternal(vector_t &vec, std::wstring &&p) {
    if (bela::PathExists(p)) {
      vec.emplace_back(std::move(p));
      return true;
    }
    return false;
  }
  bool JoinEnv(vector_t &vec, std::wstring_view p) {
    return JoinEnvInternal(vec, std::wstring(p));
  }
  bool JoinEnv(vector_t &vec, std::wstring_view a, std::wstring_view b) {
    auto p = bela::StringCat(a, b);
    return JoinEnvInternal(vec, std::wstring(p));
  }
  bool JoinEnv(vector_t &vec, std::wstring_view a, std::wstring_view b,
               std::wstring_view c) {
    auto p = bela::StringCat(a, b, c);
    return JoinEnvInternal(vec, std::wstring(p));
  }
  bool JoinEnv(vector_t &vec, std::wstring_view a, std::wstring_view b,
               std::wstring_view c, std::wstring_view d) {
    auto p = bela::StringCat(a, b, c, d);
    return JoinEnvInternal(vec, std::wstring(p));
  }
  template <typename... Args>
  bool JoinEnv(vector_t &vec, std::wstring_view a, std::wstring_view b,
               std::wstring_view c, std::wstring_view d, Args... args) {
    auto p = bela::strings_internal::CatPieces({a, b, c, d, args...});
    return JoinEnvInternal(vec, std::wstring(p));
  }
  std::wstring CleanupEnv() const {
    bela::env::Derivator dev;
    if (!libs.empty()) {
      dev.SetEnv(L"LIB", bela::env::JoinEnv(libs));
    }
    if (!includes.empty()) {
      dev.SetEnv(L"INCLUDE", bela::env::JoinEnv(includes));
    }
    if (!libpaths.empty()) {
      dev.SetEnv(L"LIBPATH", bela::env::JoinEnv(libpaths));
    }
    // dev.SetEnv(L"Path", bela::env::InsertEnv(L"Path", paths));
    return dev.CleanupEnv(bela::env::JoinEnv(paths));
  }

  std::wstring MakeEnv() const {
    bela::env::Derivator dev;
    if (!libs.empty()) {
      dev.SetEnv(L"LIB", bela::env::JoinEnv(libs));
    }
    if (!includes.empty()) {
      dev.SetEnv(L"INCLUDE", bela::env::JoinEnv(includes));
    }
    if (!libpaths.empty()) {
      dev.SetEnv(L"LIBPATH", bela::env::JoinEnv(libpaths));
    }
    auto oldpath = bela::GetEnv(L"path");
    std::vector<std::wstring_view> ov = bela::StrSplit(
        oldpath, bela::ByChar(bela::env::Separator), bela::SkipEmpty());
    auto newpath =
        bela::StringCat(bela::env::JoinEnv(paths), bela::env::Separators,
                        bela::env::JoinEnv(ov));
    dev.SetEnv(L"path", newpath);
    return dev.MakeEnv();
  }

  bool InitializeWindowsKitEnv(bela::error_code &ec);
  bool InitializeVisualStudioEnv(bela::error_code &ec);
  bool InitializeBaulk(bela::error_code &ec);
  bool InitializeGit(bool cleanup, bela::error_code &ec);
};

bool SDKSearchVersion(std::wstring_view sdkroot, std::wstring_view sdkver,
                      std::wstring &sdkversion) {
  auto dir = bela::StringCat(sdkroot, L"\\Include");
  for (auto &p : std::filesystem::directory_iterator(dir)) {
    auto filename = p.path().filename().wstring();
    if (bela::StartsWith(filename, sdkver)) {
      sdkversion = filename;
      return true;
    }
  }
  return true;
}

bool Searcher::InitializeWindowsKitEnv(bela::error_code &ec) {
  auto winsdk = baulk::regutils::LookupWindowsSDK(ec);
  if (!winsdk) {
    return false;
  }
  std::wstring sdkversion;
  if (!SDKSearchVersion(winsdk->InstallationFolder, winsdk->ProductVersion,
                        sdkversion)) {
    ec = bela::make_error_code(1, L"invalid sdk version");
    return false;
  }
  constexpr std::wstring_view incs[] = {L"\\um", L"\\ucrt", L"\\cppwinrt",
                                        L"\\shared", L"\\winrt"};
  for (auto i : incs) {
    JoinEnv(includes, winsdk->InstallationFolder, L"\\Include\\", sdkversion,
            i);
  }
  // libs
  JoinEnv(libs, winsdk->InstallationFolder, L"\\Lib\\", sdkversion, L"\\um\\",
          arch);
  JoinEnv(libs, winsdk->InstallationFolder, L"\\Lib\\", sdkversion, L"\\ucrt\\",
          arch);
  // Paths
  JoinEnv(paths, winsdk->InstallationFolder, L"\\bin\\", arch);
  JoinEnv(paths, winsdk->InstallationFolder, L"\\bin\\", sdkversion, L"\\",
          arch);
  // LIBPATHS
  JoinEnv(libpaths, winsdk->InstallationFolder, L"\\UnionMetadata\\",
          sdkversion);
  JoinEnv(libpaths, winsdk->InstallationFolder, L"\\References\\", sdkversion);
  return true;
}

bool Searcher::InitializeVisualStudioEnv(bela::error_code &ec) {
  auto vsi = LookupVisualStudioInstance(ec);
  if (!vsi) {
    // Visual Studio not install
    return false;
  }
  auto vcver = LookupVisualCppVersion(vsi->installationPath, ec);
  if (!vcver) {
    return false;
  }
  // Libs
  JoinEnv(includes, vsi->installationPath, LR"(\VC\Tools\MSVC\)", *vcver,
          LR"(\ATLMFC\include)");
  JoinEnv(includes, vsi->installationPath, LR"(\VC\Tools\MSVC\)", *vcver,
          LR"(\include)");
  // Libs
  JoinEnv(libs, vsi->installationPath, LR"(\VC\Tools\MSVC\)", *vcver,
          LR"(\ATLMFC\lib\)", arch);
  JoinEnv(libs, vsi->installationPath, LR"(\VC\Tools\MSVC\)", *vcver,
          LR"(\lib\)", arch);
  // Paths
  JoinEnv(paths, vsi->installationPath, LR"(\VC\Tools\MSVC\)", *vcver,
          LR"(\bin\Host)", arch, L"\\", arch);
  // if constexpr (arch == L"x64") {
  // } else {
  // }
  // IDE tools
  JoinEnv(paths, vsi->installationPath, LR"(\Common7\IDE)");
  JoinEnv(paths, vsi->installationPath, LR"(\Common7\IDE\Tools)");
  // Extension
  JoinEnv(paths, vsi->installationPath,
          LR"(\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin)");
  JoinEnv(paths, vsi->installationPath,
          LR"(\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja)");
  // add libpaths
  JoinEnv(libpaths, vsi->installationPath, LR"(\VC\Tools\MSVC\)", *vcver,
          LR"(\ATLMFC\lib\)", arch);
  JoinEnv(libpaths, vsi->installationPath, LR"(\VC\Tools\MSVC\)", *vcver,
          LR"(\lib\)", arch);
  JoinEnv(libpaths, vsi->installationPath, LR"(\VC\Tools\MSVC\)", *vcver,
          LR"(\lib\x86\store\references)");
  return true;
}

bool Searcher::InitializeBaulk(bela::error_code &ec) {
  auto exepath = bela::ExecutablePath(ec);
  if (!exepath) {
    return false;
  }
  auto baulkexe = bela::StringCat(*exepath, L"\\baulk.exe");
  if (bela::PathExists(baulkexe)) {
    JoinEnv(paths, *exepath);
    JoinEnv(paths, *exepath, L"\\linkbin");
    return true;
  }
  std::wstring baulkroot(*exepath);
  for (size_t i = 0; i < 5; i++) {
    auto baulkexe = bela::StringCat(baulkroot, L"\\bin\\baulk.exe");
    if (bela::PathExists(baulkexe)) {
      JoinEnv(paths, baulkroot, L"\\bin");
      JoinEnv(paths, baulkroot, L"\\bin\\linkbin");
      return true;
    }
    bela::PathStripName(baulkroot);
  }
  ec = bela::make_error_code(1, L"unable found baulk.exe");
  return false;
}
bool Searcher::InitializeGit(bool cleanup, bela::error_code &ec) {
  std::wstring git;
  if (bela::ExecutableExistsInPath(L"git.exe", git)) {
    if (cleanup) {
      bela::PathStripName(git);
      JoinEnv(paths, git);
    }
    return true;
  }
  auto installPath = baulk::regutils::GitForWindowsInstallPath(ec);
  if (!installPath) {
    return false;
  }
  git = bela::StringCat(*installPath, L"\\cmd\\git.exe");
  if (!bela::PathExists(git)) {
    return false;
  }
  JoinEnv(paths, *installPath, L"\\cmd");
  return true;
}

std::optional<std::wstring> MakeEnv(bool usevs, bool cleanup,
                                    bela::error_code &ec) {
  Searcher searcher;
  if (!searcher.InitializeBaulk(ec)) {
    return std::nullopt;
  }
  searcher.InitializeGit(cleanup, ec);
  if (usevs) {
    if (!searcher.InitializeVisualStudioEnv(ec)) {
      return std::nullopt;
    }
    if (!searcher.InitializeWindowsKitEnv(ec)) {
      return std::nullopt;
    }
  }
  if (cleanup) {
    return std::make_optional(searcher.CleanupEnv());
  }
  return std::make_optional(searcher.MakeEnv());
}

} // namespace baulkterminal