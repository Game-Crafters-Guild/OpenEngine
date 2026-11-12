#include "Assets/Polyhaven/PolyhavenService.h"

#include "Editor/Settings/SettingsStore.h"
#include "FileSystem/FileSystem.h"
#include "Logger/Logger.h"
#include "Platform/HttpClient.h"
#include "Types/PathUtils.h"
#include "Types/StringUtils.h"
#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <fstream>
#include <string_view>
#include <thread>

namespace
{

const char* kApiAssets     = "https://api.polyhaven.com/assets";
const char* kApiCategories = "https://api.polyhaven.com/categories";
const char* kApiFiles      = "https://api.polyhaven.com/files/";

constexpr int kMaxRetries    = 5;
constexpr int kRetryDelaySec = 2;

// Writes bytes beside dest, then publishes them at dest. File-existence checks run on the UI thread
// and in other editors while a worker writes, so a reader sees no file or the complete file.
bool WriteAndPublishFile(const std::filesystem::path& dest, std::string_view bytes)
{
    std::error_code ec;
    std::filesystem::create_directories(dest.parent_path(), ec);
    if (ec)
    {
        Logger::Log::Warning("Polyhaven: create_directories('{}') failed: {}",
                                         dest.parent_path().string(), ec.message());
        return false;
    }

    const std::filesystem::path temp = GameEngine::FileSystem::MakeTemporarySiblingPath(dest);
    std::ofstream of(temp, std::ios::binary | std::ios::trunc);
    if (!of)
    {
        Logger::Log::Warning("Polyhaven: cannot open '{}' for writing", temp.string());
        return false;
    }
    of.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    of.close();
    if (!of)
    {
        Logger::Log::Warning("Polyhaven: write failed for '{}'", temp.string());
        std::filesystem::remove(temp, ec);
        return false;
    }

    return GameEngine::FileSystem::PublishFile(temp, dest);
}

bool DownloadFile(const std::string& url, const std::filesystem::path& dest)
{
    auto resp = GameEngine::HttpClient::Get(url, {}, {});
    if (!resp.success || resp.body.empty())
    {
        Logger::Log::Warning("Polyhaven: GET failed for '{}': {}", url,
                                         resp.error.empty() ? "empty body" : resp.error);
        return false;
    }
    return WriteAndPublishFile(dest, resp.body);
}

bool IsCompleteDownloadedFile(const std::filesystem::path& mainFile)
{
    std::error_code ec;
    if (!std::filesystem::is_regular_file(mainFile, ec) ||
        std::filesystem::file_size(mainFile, ec) == 0)
        return false;

    const std::string ext = GameEngine::LowerCaseExtension(mainFile);
    if (ext != ".gltf")
        return true;

    std::ifstream ifs(mainFile);
    if (!ifs.is_open())
        return false;

    nlohmann::json gltf;
    try
    {
        gltf = nlohmann::json::parse(ifs);
    }
    catch (...)
    {
        return false;
    }

    const auto dependencyExists = [&](const nlohmann::json& entry)
    {
        if (!entry.contains("uri") || !entry["uri"].is_string())
            return true;
        const std::string uri = entry["uri"].get<std::string>();
        if (uri.starts_with("data:"))
            return true;

        ec.clear();
        const std::filesystem::path dependency = mainFile.parent_path() / uri;
        return std::filesystem::is_regular_file(dependency, ec) &&
               std::filesystem::file_size(dependency, ec) > 0;
    };

    for (const char* collection : {"buffers", "images"})
    {
        if (!gltf.contains(collection) || !gltf[collection].is_array())
            continue;
        for (const auto& entry : gltf[collection])
            if (!dependencyExists(entry))
                return false;
    }
    return true;
}

std::string NormalizeHDRIResolution(std::string resolution)
{
    resolution = GameEngine::ToLowerAscii(resolution);
    if (resolution.empty())
        return "1k";
    return resolution;
}

std::string GetPreferredTextureResolution()
{
    auto prefs = GameEngine::Editor::OpenEditorPreferences();
    std::string resolution = "1k";
    prefs.TryGetString("onlineAssets.polyHavenTextureResolution", resolution);
    return resolution;
}

} // namespace

namespace GameEngine {

// ---------- virtual path helpers ----------

bool PolyhavenService::ParseVirtualPath(const std::filesystem::path& p,
                                        std::string& outType,
                                        std::string& outCategory)
{
    std::string s = p.generic_string();
    if (s.rfind("polyhaven:/", 0) == 0 && s.rfind("polyhaven://", 0) != 0)
        s.insert(10, "/");
    if (s.size() < 12 || s.compare(0, 12, "polyhaven://") != 0)
        return false;
    size_t slash = s.find('/', 12);
    if (slash == std::string::npos)
        return false;
    outType = s.substr(12, slash - 12);
    outCategory = s.substr(slash + 1);
    return true;
}

std::string PolyhavenService::TypeKeyForType(const std::string& type)
{
    if (type == "hdris")    return "Polyhaven HDRI";
    if (type == "textures") return "Polyhaven Texture";
    if (type == "models")   return "Polyhaven Model";
    return "Polyhaven " + type;
}

// ---------- API ----------

namespace
{

constexpr std::string_view kPngFormatParameter = "format=png";
constexpr std::string_view kPngSignature = "\x89PNG\r\n\x1a\n";
constexpr int kThumbnailAttempts = 2;
constexpr std::chrono::milliseconds kThumbnailRetryDelay{500};

// The CDN encodes a thumbnail in the format its query names. The API names WebP, which the editor
// cannot decode, so the request names PNG: a "format" parameter of any case is replaced, else added
// before the fragment.
std::string ThumbnailUrlRequestingPng(const std::string& url)
{
    const std::vector<std::string_view> targetAndFragment = Split(url, '#');
    const std::string_view target = targetAndFragment.empty() ? std::string_view() : targetAndFragment.front();
    const std::string_view fragment = std::string_view(url).substr(target.size());
    const size_t queryStart = target.find('?');
    if (queryStart == std::string_view::npos)
        return std::string(target) + "?" + std::string(kPngFormatParameter) + std::string(fragment);

    std::vector<std::string_view> parameters;
    bool hasFormat = false;
    for (std::string_view parameter : Split(target.substr(queryStart + 1), '&'))
    {
        const std::vector<std::string_view> nameAndValue = Split(parameter, '=');
        const bool isFormat = !nameAndValue.empty() && EqualsIgnoreCase(nameAndValue.front(), "format");
        hasFormat = hasFormat || isFormat;
        parameters.push_back(isFormat ? kPngFormatParameter : parameter);
    }
    if (!hasFormat)
        parameters.push_back(kPngFormatParameter);
    return std::string(target.substr(0, queryStart + 1)) + Join(parameters, "&") + std::string(fragment);
}

// True for the API's slugs: lowercase letters, digits, '_' and '-'. Other text can name a path
// outside the cache folder.
bool IsPlainSlug(std::string_view slug)
{
    return !slug.empty() && std::all_of(slug.begin(), slug.end(), [](char c)
                                        { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-'; });
}

std::string ContentTypeOf(const HttpClient::HttpResponse& response)
{
    for (const auto& [name, value] : response.headers)
        if (EqualsIgnoreCase(name, "Content-Type"))
            return value;
    return {};
}

// False when this build cannot make HTTP requests at all. The retry loops below
// exist for transient network failures; against a platform with no HTTP backend
// they would spin their full count on every call, and the browser's asset panel
// re-asks, which turns into an endless retry storm in the log.
bool ReportIfOffline()
{
    if (HttpClient::IsAvailable())
        return true;
    static std::atomic<bool> reported{false};
    if (!reported.exchange(true))
    {
        Logger::Log::Warning("Polyhaven: the online asset library is unavailable in this build — "
                             "it has no HTTP backend. The browser will stay empty.");
    }
    return false;
}

} // namespace

PolyhavenService::CategoriesResult PolyhavenService::FetchCategories(const std::string& type)
{
    CategoriesResult out;
    out.typeKey = TypeKeyForType(type);
    if (!ReportIfOffline())
        return out;

    std::string url = std::string(kApiCategories) + "/" + HttpClient::UrlEncode(type);

    for (int attempt = 0; attempt < kMaxRetries; ++attempt)
    {
        if (attempt > 0)
            std::this_thread::sleep_for(std::chrono::seconds(kRetryDelaySec));

        auto resp = HttpClient::Get(url, {}, {});
        if (!resp.success || resp.statusCode != 200 || resp.body.empty())
        {
            Logger::Log::Warning("Polyhaven: categories API failed {} (status {}), attempt {}/{}",
                                 resp.error, resp.statusCode, attempt + 1, kMaxRetries);
            continue;
        }

        nlohmann::json j;
        try { j = nlohmann::json::parse(resp.body); }
        catch (const std::exception& e)
        {
            Logger::Log::Warning("Polyhaven: categories JSON parse failed {}, attempt {}/{}",
                                 e.what(), attempt + 1, kMaxRetries);
            continue;
        }
        if (!j.is_object())
            continue;

        for (auto it = j.begin(); it != j.end(); ++it)
        {
            std::string catSlug = it.key();
            int count = it.value().is_number_integer() ? it.value().get<int>() : 0;
            std::string label = catSlug;
            if (label == "all")
                label = "All";
            else if (!label.empty())
                label[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(label[0])));
            label += " (" + std::to_string(count) + ")";
            out.pathAndLabels.emplace_back("polyhaven://" + type + "/" + catSlug, label);
        }
        if (!out.pathAndLabels.empty())
            break;
    }

    std::sort(out.pathAndLabels.begin(), out.pathAndLabels.end(),
              [](const auto& a, const auto& b) { return a.second < b.second; });
    return out;
}

PolyhavenService::ManifestResult PolyhavenService::FetchManifest(const std::string& type,
                                                                  const std::string& categoryFilter)
{
    ManifestResult out;
    if (type.empty())
        return out;
    out.typeKey = TypeKeyForType(type);
    if (!ReportIfOffline())
        return out;

    std::string url = std::string(kApiAssets) + "?t=" + HttpClient::UrlEncode(type);
    if (!categoryFilter.empty() && categoryFilter != "all")
        url += "&categories=" + HttpClient::UrlEncode(categoryFilter);

    nlohmann::json j;
    for (int attempt = 0; attempt < kMaxRetries; ++attempt)
    {
        if (attempt > 0)
            std::this_thread::sleep_for(std::chrono::seconds(kRetryDelaySec));

        auto resp = HttpClient::Get(url, {}, {});
        if (!resp.success || resp.statusCode != 200 || resp.body.empty())
        {
            Logger::Log::Warning("Polyhaven: assets API failed {} (status {}), attempt {}/{}",
                                 resp.error, resp.statusCode, attempt + 1, kMaxRetries);
            continue;
        }
        try { j = nlohmann::json::parse(resp.body); }
        catch (const std::exception& e)
        {
            Logger::Log::Warning("Polyhaven: assets JSON parse failed {}, attempt {}/{}",
                                 e.what(), attempt + 1, kMaxRetries);
            continue;
        }
        if (j.is_object())
            break;
    }
    if (!j.is_object())
        return out;

    for (auto it = j.begin(); it != j.end(); ++it)
    {
        if (!it.value().is_object())
            continue;
        std::string slug = it.key();
        std::string name;
        std::string thumbUrl;
        if (it.value().contains("name") && it.value()["name"].is_string())
            name = it.value()["name"].get<std::string>();
        if (name.empty())
            name = slug;
        if (it.value().contains("thumbnail_url") && it.value()["thumbnail_url"].is_string())
            thumbUrl = it.value()["thumbnail_url"].get<std::string>();
        if (thumbUrl.empty())
            continue;
        out.manifest.push_back({std::move(slug), std::move(name), std::move(thumbUrl)});
    }
    return out;
}

PolyhavenLoadResult PolyhavenService::MaterializeBatch(
    const std::vector<PolyhavenManifestEntry>& manifest,
    size_t startIndex, size_t endIndex,
    const std::string& typeKey,
    const ThumbnailFetch& fetch)
{
    PolyhavenLoadResult out;
    out.typeKey = typeKey;
    if (startIndex >= manifest.size() || startIndex >= endIndex)
        return out;

    if (GetCacheDir().empty())
        return out;

    size_t refusedCount = 0;
    std::string firstRefusedSlug;
    std::string firstRefusedContentType;
    size_t skippedCount = 0;
    std::string firstSkippedSlug;
    size_t end = std::min(endIndex, manifest.size());
    for (size_t i = startIndex; i < end; ++i)
    {
        const auto& e = manifest[i];
        std::filesystem::path cachePath = GetThumbnailPath(e.slug);
        if (cachePath.empty())
        {
            if (skippedCount++ == 0)
                firstSkippedSlug = e.slug;
            continue;
        }
        if (!std::filesystem::exists(cachePath))
        {
            const std::optional<PolyhavenThumbnailResponse> response = fetch(ThumbnailUrlRequestingPng(e.thumbUrl));
            if (response && !response->body.starts_with(kPngSignature))
            {
                if (refusedCount++ == 0)
                {
                    firstRefusedSlug = e.slug;
                    firstRefusedContentType = response->contentType;
                }
            }
            else if (response)
            {
                WriteAndPublishFile(cachePath, response->body);
            }
        }
        out.pathAndLabels.emplace_back(cachePath, e.name);
    }
    if (refusedCount > 0)
        Logger::Log::Warning("Polyhaven: thumbnails from Poly Haven's server are not PNG data: {} of the assets "
                             "just listed, for example '{}' (content type '{}'). The Assets panel lists these assets "
                             "without a picture. The editor asks for '{}'. Update the editor; if the newest editor "
                             "still shows this, report it.",
                             refusedCount, firstRefusedSlug, firstRefusedContentType, kPngFormatParameter);
    if (skippedCount > 0)
        Logger::Log::Warning("Polyhaven: asset names from Poly Haven's server are not plain file names: {} of the "
                             "assets just listed, for example '{}'. The Assets panel does not list these assets. "
                             "Update the editor; if the newest editor still shows this, report it.",
                             skippedCount, firstSkippedSlug);
    return out;
}

std::optional<PolyhavenThumbnailResponse> PolyhavenService::FetchThumbnail(const std::string& url)
{
    for (int attempt = 0; attempt < kThumbnailAttempts; ++attempt)
    {
        if (attempt > 0)
            std::this_thread::sleep_for(kThumbnailRetryDelay);
        HttpClient::HttpResponse response = HttpClient::Get(url, {}, {});
        if (response.success && response.statusCode == 200 && !response.body.empty())
            return PolyhavenThumbnailResponse{ContentTypeOf(response), std::move(response.body)};
    }
    return std::nullopt;
}

std::filesystem::path PolyhavenService::GetThumbnailPath(const std::string& slug)
{
    if (!IsPlainSlug(slug))
        return {};
    const std::filesystem::path cacheDir = GetCacheDir();
    if (cacheDir.empty())
        return {};
    return cacheDir / "Thumbnails" / (slug + ".png");
}

std::filesystem::path PolyhavenService::GetCacheDir()
{
    std::error_code ec;
    std::filesystem::path tmp = std::filesystem::temp_directory_path(ec);
    if (ec || tmp.empty())
        return {};
    std::filesystem::path cache = tmp / "GameEngine" / "Polyhaven";
    std::filesystem::create_directories(cache, ec);
    return cache;
}

std::filesystem::path PolyhavenService::FindDownloadedFile(const std::string& slug,
                                                            const std::filesystem::path& assetsRoot)
{
    std::filesystem::path destDir = assetsRoot / "Polyhaven" / slug;
    std::error_code ec;
    for (const char* ext : {".gltf", ".glb", ".hdr", ".png", ".jpg"})
    {
        auto candidate = destDir / (slug + ext);
        if (std::filesystem::exists(candidate, ec) && IsCompleteDownloadedFile(candidate))
            return candidate;
    }
    auto hdri1k = destDir / (slug + "_1k.hdr");
    if (std::filesystem::exists(hdri1k, ec))
        return hdri1k;
    const std::string preferredHDRI = PolyhavenService::GetPreferredHDRIResolution();
    for (const std::string& res : {preferredHDRI, std::string("2k"), std::string("4k"), std::string("8k"), std::string("16k")})
    {
        auto candidate = destDir / (slug + "_" + res + ".hdr");
        if (std::filesystem::exists(candidate, ec))
            return candidate;
    }
    return {};
}

std::filesystem::path PolyhavenService::FindDownloadedHDRI(const std::string& slug,
                                                            const std::filesystem::path& assetsRoot,
                                                            const std::string& resolution)
{
    const std::string res = NormalizeHDRIResolution(resolution);
    std::filesystem::path destDir = assetsRoot / "Polyhaven" / slug;
    std::error_code ec;
    auto candidate = destDir / (slug + "_" + res + ".hdr");
    if (std::filesystem::exists(candidate, ec))
        return candidate;
    if (res == "1k")
    {
        candidate = destDir / (slug + ".hdr");
        if (std::filesystem::exists(candidate, ec))
            return candidate;
    }
    return {};
}

std::filesystem::path PolyhavenService::FindOrMoveDownloadedHDRI(const std::string& slug,
                                                                  const std::filesystem::path& assetsRoot,
                                                                  const std::string& resolution)
{
    std::filesystem::path file = FindDownloadedHDRI(slug, assetsRoot, resolution);
    if (!file.empty())
        return file;

    const std::filesystem::path cachedFile = FindCachedDownloadHDRI(slug, resolution);
    if (cachedFile.empty())
        return {};

    const std::filesystem::path projectDir = assetsRoot / "Polyhaven" / slug;
    return MoveDownloadToProject(cachedFile, cachedFile.parent_path(), projectDir);
}

std::filesystem::path PolyhavenService::FindCachedDownloadFile(const std::string& slug)
{
    std::filesystem::path cacheDir = GetCacheDir() / "downloads" / slug;
    std::error_code ec;
    for (const char* ext : {".gltf", ".glb", ".hdr", ".png", ".jpg"})
    {
        auto candidate = cacheDir / (slug + ext);
        if (std::filesystem::exists(candidate, ec) && IsCompleteDownloadedFile(candidate))
            return candidate;
    }
    auto hdri1k = cacheDir / (slug + "_1k.hdr");
    if (std::filesystem::exists(hdri1k, ec))
        return hdri1k;
    const std::string preferredHDRI = PolyhavenService::GetPreferredHDRIResolution();
    for (const std::string& res : {preferredHDRI, std::string("2k"), std::string("4k"), std::string("8k"), std::string("16k")})
    {
        auto candidate = cacheDir / (slug + "_" + res + ".hdr");
        if (std::filesystem::exists(candidate, ec))
            return candidate;
    }
    return {};
}

std::filesystem::path PolyhavenService::FindCachedDownloadHDRI(const std::string& slug,
                                                                const std::string& resolution)
{
    const std::string res = NormalizeHDRIResolution(resolution);
    std::filesystem::path cacheDir = GetCacheDir() / "downloads" / slug;
    std::error_code ec;
    auto candidate = cacheDir / (slug + "_" + res + ".hdr");
    if (std::filesystem::exists(candidate, ec))
        return candidate;
    if (res == "1k")
    {
        candidate = cacheDir / (slug + ".hdr");
        if (std::filesystem::exists(candidate, ec))
            return candidate;
    }
    return {};
}

std::filesystem::path PolyhavenService::MoveDownloadToProject(const std::filesystem::path& mainFile,
                                                               const std::filesystem::path& srcDir,
                                                               const std::filesystem::path& destDir)
{
    // Drag-hover downloads populate srcDir in the background. Do not split a
    // glTF from its still-downloading buffer/textures by moving it early.
    if (!IsCompleteDownloadedFile(mainFile))
        return {};

    std::error_code ec;
    std::filesystem::create_directories(destDir, ec);

    std::filesystem::path newMainFile;
    for (auto& entry : std::filesystem::recursive_directory_iterator(srcDir, ec))
    {
        if (!entry.is_regular_file())
            continue;
        std::filesystem::path relPath = entry.path().lexically_relative(srcDir);
        std::filesystem::path dest = destDir / relPath;
        std::filesystem::create_directories(dest.parent_path(), ec);
        std::filesystem::copy_file(entry.path(), dest,
                                   std::filesystem::copy_options::overwrite_existing, ec);
        if (entry.path().filename() == mainFile.filename())
            newMainFile = dest;
    }
    std::filesystem::remove_all(srcDir, ec);
    return newMainFile;
}

// ---------- downloading ----------

std::filesystem::path PolyhavenService::DownloadAsset(const std::string& slug,
                                                       const std::string& type,
                                                       const std::filesystem::path& destDir,
                                                       const std::function<void(size_t, size_t)>& onProgress)
{
    std::string filesUrl = std::string(kApiFiles) + slug;
    auto resp = HttpClient::Get(filesUrl, {}, {});
    if (!resp.success || resp.statusCode != 200 || resp.body.empty())
    {
        Logger::Log::Warning("Polyhaven download: API failed for '{}'", slug);
        return {};
    }
    nlohmann::json filesJson;
    try { filesJson = nlohmann::json::parse(resp.body); }
    catch (...)
    {
        Logger::Log::Warning("Polyhaven download: JSON parse failed for '{}'", slug);
        return {};
    }

    std::error_code ec;
    std::filesystem::create_directories(destDir, ec);
    std::filesystem::path mainFile;

    if (type == "models")
    {
        if (!filesJson.contains("gltf") || !filesJson["gltf"].is_object())
            return {};
        auto& resolutions = filesJson["gltf"];
        std::string res = resolutions.contains("1k") ? "1k"
            : (resolutions.begin() != resolutions.end() ? resolutions.begin().key() : "");
        if (res.empty() || !resolutions[res].contains("gltf"))
            return {};
        auto& gltfData = resolutions[res]["gltf"];
        std::string mainUrl = gltfData.value("url", "");
        if (mainUrl.empty())
            return {};
        const size_t includeCount = (gltfData.contains("include") && gltfData["include"].is_object())
                                        ? gltfData["include"].size()
                                        : 0;
        const size_t totalFiles = 1 + includeCount;
        size_t doneFiles = 0;
        auto report = [&](size_t done) { if (onProgress) onProgress(done, totalFiles); };
        report(0);
        mainFile = destDir / (slug + ".gltf");
        if (!DownloadFile(mainUrl, mainFile))
            return {};
        report(++doneFiles);
        if (includeCount > 0)
        {
            bool includesComplete = true;
            for (auto it = gltfData["include"].begin(); it != gltfData["include"].end(); ++it)
            {
                std::string incUrl = it.value().value("url", "");
                if (incUrl.empty() || !DownloadFile(incUrl, destDir / it.key()))
                    includesComplete = false;
                report(++doneFiles);
            }
            if (!includesComplete)
                return {};
        }
        if (!IsCompleteDownloadedFile(mainFile))
            return {};
    }
    else if (type == "hdris")
    {
        mainFile = DownloadHDRI(slug, destDir, GetPreferredHDRIResolution());
    }
    else if (type == "textures")
    {
        // Get preferred resolution from settings
        std::string preferredRes = GetPreferredTextureResolution();
        
        for (const char* mapKey : {"Diffuse", "diffuse", "Color", "color"})
        {
            if (!filesJson.contains(mapKey))
                continue;
            auto& mapData = filesJson[mapKey];
            if (!mapData.is_object())
                continue;
            // Use preferred resolution from settings
            std::string res = mapData.contains(preferredRes) ? preferredRes
                : (mapData.contains("4k") ? "4k"
                : (mapData.contains("2k") ? "2k"
                : (mapData.contains("1k") ? "1k"
                : (mapData.begin() != mapData.end() ? mapData.begin().key() : ""))));
            if (res.empty())
                continue;
            for (const char* fmt : {"png", "jpg"})
            {
                if (!mapData[res].contains(fmt))
                    continue;
                std::string fileUrl = mapData[res][fmt].value("url", "");
                if (fileUrl.empty())
                    continue;
                const auto candidate = destDir / (slug + std::string(".") + fmt);
                if (DownloadFile(fileUrl, candidate))
                    mainFile = candidate;
                break;
            }
            if (!mainFile.empty())
                break;
        }
    }

    if (mainFile.empty())
        Logger::Log::Warning("Polyhaven download: no file downloaded for '{}'", slug);
    else
        Logger::Log::Info("Polyhaven: downloaded '{}' to {}", slug, mainFile.string());

    return mainFile;
}

std::filesystem::path PolyhavenService::DownloadHDRI(const std::string& slug,
                                                      const std::filesystem::path& destDir,
                                                      const std::string& resolution)
{
    std::string filesUrl = std::string(kApiFiles) + slug;
    auto resp = HttpClient::Get(filesUrl, {}, {});
    if (!resp.success || resp.statusCode != 200 || resp.body.empty())
    {
        Logger::Log::Warning("Polyhaven HDRI download: API failed for '{}'", slug);
        return {};
    }

    nlohmann::json filesJson;
    try { filesJson = nlohmann::json::parse(resp.body); }
    catch (...)
    {
        Logger::Log::Warning("Polyhaven HDRI download: JSON parse failed for '{}'", slug);
        return {};
    }

    if (!filesJson.contains("hdri") || !filesJson["hdri"].is_object())
        return {};

    auto& hdri = filesJson["hdri"];
    std::string res = NormalizeHDRIResolution(resolution);
    if (!hdri.contains(res))
    {
        Logger::Log::Warning("Polyhaven HDRI download: '{}' has no {} resolution", slug, res);
        return {};
    }
    if (!hdri[res].contains("hdr"))
        return {};

    std::string fileUrl = hdri[res]["hdr"].value("url", "");
    if (fileUrl.empty())
        return {};

    std::error_code ec;
    std::filesystem::create_directories(destDir, ec);
    std::filesystem::path mainFile = destDir / (slug + "_" + res + ".hdr");
    if (!DownloadFile(fileUrl, mainFile))
        return {};

    Logger::Log::Info("Polyhaven: downloaded '{}' {} HDRI to {}", slug, res, mainFile.string());
    return mainFile;
}

std::string PolyhavenService::GetPreferredHDRIResolution()
{
    auto prefs = Editor::OpenEditorPreferences();
    std::string resolution = "4k";
    prefs.TryGetString("onlineAssets.polyHavenHDRIResolution", resolution);
    return NormalizeHDRIResolution(std::move(resolution));
}

// ---------- PBR texture map downloading ----------

bool PolyhavenService::DownloadTextureMaps(const std::string& slug,
                                            const std::filesystem::path& destDir,
                                            const std::string& mapType)
{
    // Fetch file list from API
    std::string url = std::string(kApiFiles) + slug;
    auto resp = HttpClient::Get(url, {}, {});
    if (!resp.success || resp.body.empty())
    {
        Logger::Log::Warning("Polyhaven: failed to fetch file list for '{}'", slug);
        return false;
    }

    nlohmann::json filesJson;
    try { filesJson = nlohmann::json::parse(resp.body); }
    catch (...)
    {
        Logger::Log::Warning("Polyhaven: JSON parse failed for '{}'", slug);
        return false;
    }

    std::error_code ec;
    std::filesystem::create_directories(destDir, ec);

    // Get preferred resolution from settings
    std::string preferredRes = GetPreferredTextureResolution();

    // Define map keys to download based on request
    std::vector<std::string> mapKeys;
    if (mapType.empty() || mapType == "all")
    {
        // Download all available maps
        mapKeys = {"Diffuse", "nor_gl", "nor_dx", "Rough", "Metallic", "AO", "Displacement", "arm", "rough_ao"};
    }
    else
    {
        mapKeys = {mapType};
    }

    bool downloadedAny = false;

    for (const auto& key : mapKeys)
    {
        if (!filesJson.contains(key) || !filesJson[key].is_object())
            continue;

        auto& mapData = filesJson[key];
        
        // Use preferred resolution from settings, fallback to available
        std::string res = mapData.contains(preferredRes) ? preferredRes
            : (mapData.contains("4k") ? "4k"
            : (mapData.contains("2k") ? "2k"
            : (mapData.contains("1k") ? "1k"
            : (mapData.begin() != mapData.end() ? mapData.begin().key() : ""))));

        if (res.empty())
            continue;

        // Download PNG format (or JPG if PNG not available)
        for (const char* fmt : {"png", "jpg"})
        {
            if (!mapData[res].contains(fmt))
                continue;

            std::string fileUrl = mapData[res][fmt].value("url", "");
            if (fileUrl.empty())
                continue;

            // Construct filename: slug_maptype_res.ext
            const std::string mapName = ToLowerAscii(key);
            
            std::string filename = slug + "_" + mapName + "_" + res + "." + fmt;
            std::filesystem::path outFile = destDir / filename;

            if (DownloadFile(fileUrl, outFile))
            {
                Logger::Log::Info("Polyhaven: downloaded {} map '{}' to {}", key, slug, outFile.string());
                downloadedAny = true;
            }
            break; // Only download one format per map
        }
    }

    if (!downloadedAny)
        Logger::Log::Warning("Polyhaven: no texture maps downloaded for '{}'", slug);

    return downloadedAny;
}

// ---------- download verification ----------

bool PolyhavenService::IsFullyDownloaded(const std::string& slug,
                                          const std::filesystem::path& assetsRoot)
{
    auto it = m_DownloadedCache.find(slug);
    if (it != m_DownloadedCache.end())
        return it->second;

    std::filesystem::path downloadDir = assetsRoot / "Polyhaven" / slug;
    std::error_code ec;

    if (!std::filesystem::is_directory(downloadDir, ec))
    {
        m_DownloadedCache[slug] = false;
        return false;
    }

    // Find the main asset file
    std::filesystem::path mainFile;
    for (const char* ext : {".gltf", ".glb", ".hdr", ".png", ".jpg"})
    {
        auto candidate = downloadDir / (slug + ext);
        if (std::filesystem::exists(candidate, ec))
        {
            mainFile = candidate;
            break;
        }
    }

    if (mainFile.empty())
    {
        const std::string preferredHDRI = PolyhavenService::GetPreferredHDRIResolution();
        for (const std::string& res : {preferredHDRI, std::string("1k"), std::string("2k"), std::string("4k"), std::string("8k"), std::string("16k")})
        {
            auto candidate = downloadDir / (slug + "_" + res + ".hdr");
            if (std::filesystem::exists(candidate, ec))
            {
                mainFile = candidate;
                break;
            }
        }
        if (mainFile.empty())
        {
            m_DownloadedCache[slug] = false;
            return false;
        }
    }

    const bool result = IsCompleteDownloadedFile(mainFile);
    m_DownloadedCache[slug] = result;
    return result;
}

} // namespace GameEngine
