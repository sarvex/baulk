#include <bela/env.hpp>
#include <bela/path.hpp>
#include <bela/io.hpp>
#include <regutils.hpp>
#include <jsonex.hpp>
#include "compiler.hpp"
#include "baulk.hpp"
#include "fs.hpp"

// C:\Program Files (x86)\Microsoft Visual
// Studio\2019\Community\VC\Auxiliary\Build
namespace baulk::compiler {
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
  bool InitializeWindowsKitEnv(bela::error_code &ec);
  bool InitializeVisualStudioEnv(bela::error_code &ec);
  bool TestJoin(std::wstring &&p, vector_t &vec) {
    if (bela::PathExists(p)) {
      vec.emplace_back(std::move(p));
      return true;
    }
    return false;
  }
  std::wstring CleanupEnv() const {
    bela::env::Derivator dev;
    dev.SetEnv(L"LIB", bela::env::JoinEnv(libs));
    dev.SetEnv(L"INCLUDE", bela::env::JoinEnv(includes));
    dev.SetEnv(L"LIBPATH", bela::env::JoinEnv(libpaths));
    // dev.SetEnv(L"Path", bela::env::InsertEnv(L"Path", paths));
    return dev.CleanupEnv(bela::env::JoinEnv(paths));
  }
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

// process.SetEnv(L"LIB", bela::env::JoinEnv(libs));
// process.SetEnv(L"INCLUDE", bela::env::JoinEnv(includes));
// process.SetEnv(L"LIBPATH", bela::env::JoinEnv(libpaths));
// process.SetEnv(L"Path", bela::env::InsertEnv(L"Path", paths));

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
  baulk::DbgPrint(L"Windows SDK %s InstallationFolder: %s",
                  winsdk->ProductVersion, winsdk->InstallationFolder);
  constexpr std::wstring_view incs[] = {L"\\um", L"\\ucrt", L"\\cppwinrt",
                                        L"\\shared", L"\\winrt"};
  for (auto i : incs) {
    TestJoin(bela::StringCat(winsdk->InstallationFolder, L"\\Include\\",
                             sdkversion, i),
             includes);
  }
  // libs
  TestJoin(bela::StringCat(winsdk->InstallationFolder, L"\\Lib\\", sdkversion,
                           L"\\um\\", arch),
           libs);
  TestJoin(bela::StringCat(winsdk->InstallationFolder, L"\\Lib\\", sdkversion,
                           L"\\ucrt\\", arch),
           libs);
  // Paths
  TestJoin(bela::StringCat(winsdk->InstallationFolder, L"\\bin\\", arch),
           paths);
  TestJoin(bela::StringCat(winsdk->InstallationFolder, L"\\bin\\", sdkversion,
                           L"\\", arch),
           paths);
  // LIBPATHS
  TestJoin(bela::StringCat(winsdk->InstallationFolder, L"\\UnionMetadata\\",
                           sdkversion),
           libpaths);
  TestJoin(bela::StringCat(winsdk->InstallationFolder, L"\\References\\",
                           sdkversion),
           libpaths);
  return true;
}

bool Searcher::InitializeVisualStudioEnv(bela::error_code &ec) {
  auto vsi = LookupVisualStudioInstance(ec);
  if (!vsi) {
    // Visual Studio not install
    return false;
  }
  baulk::DbgPrint(L"Visual Studio %s InstallationPath: %s",
                  vsi->installationVersion, vsi->installationPath);
  auto vcver = LookupVisualCppVersion(vsi->installationPath, ec);
  if (!vcver) {
    return false;
  }
  baulk::DbgPrint(L"Visual C++ %s", *vcver);
  // Libs
  TestJoin(bela::StringCat(vsi->installationPath, LR"(\VC\Tools\MSVC\)", *vcver,
                           LR"(\ATLMFC\include)"),
           includes);
  TestJoin(bela::StringCat(vsi->installationPath, LR"(\VC\Tools\MSVC\)", *vcver,
                           LR"(\include)"),
           includes);
  // Libs
  TestJoin(bela::StringCat(vsi->installationPath, LR"(\VC\Tools\MSVC\)", *vcver,
                           LR"(\ATLMFC\lib\)", arch),
           libs);
  TestJoin(bela::StringCat(vsi->installationPath, LR"(\VC\Tools\MSVC\)", *vcver,
                           LR"(\lib\)", arch),
           libs);
  // Paths
  TestJoin(bela::StringCat(vsi->installationPath, LR"(\VC\Tools\MSVC\)", *vcver,
                           LR"(\bin\Host)", arch, L"\\", arch),
           paths);
  if constexpr (arch == L"x64") {
  } else {
  }
  // IDE tools
  TestJoin(bela::StringCat(vsi->installationPath, LR"(\Common7\IDE)"), paths);
  TestJoin(bela::StringCat(vsi->installationPath, LR"(\Common7\IDE\Tools)"),
           paths);
  // Extension
  TestJoin(bela::StringCat(
               vsi->installationPath,
               LR"(\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin)"),
           paths);
  TestJoin(bela::StringCat(
               vsi->installationPath,
               LR"(\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja)"),
           paths);
  // add libpaths
  TestJoin(bela::StringCat(vsi->installationPath, LR"(\VC\Tools\MSVC\)", *vcver,
                           LR"(\ATLMFC\lib\)", arch),
           libpaths);
  TestJoin(bela::StringCat(vsi->installationPath, LR"(\VC\Tools\MSVC\)", *vcver,
                           LR"(\lib\)", arch),
           libpaths);
  TestJoin(bela::StringCat(vsi->installationPath, LR"(\VC\Tools\MSVC\)", *vcver,
                           LR"(\lib\x86\store\references)"),
           libpaths);
  return true;
}

// $installationPath/VC/Auxiliary/Build/Microsoft.VCToolsVersion.default.txt

bool Executor::Initialize(bela::error_code &ec) {
  Searcher searcher;
  if (!searcher.InitializeVisualStudioEnv(ec)) {
    return false;
  }
  if (!searcher.InitializeWindowsKitEnv(ec)) {
    return false;
  }
  env = searcher.CleanupEnv();
  initialized = true;
  return true;
}
} // namespace baulk::compiler