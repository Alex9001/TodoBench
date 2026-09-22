// SPDX-License-Identifier: GPL-3.0-or-later
#include "storage/mdbase_markdown.h"
#include "storage/markdown_links.h"

#include <QFile>
#include <QUrl>

#include <filesystem>
#include <QCryptographicHash>
#include <QByteArray>
#include <fstream>
#include <algorithm>
#include <cctype>
#include <optional>

namespace todobench::mdbase_transfer {
namespace {

QString path_string(const std::filesystem::path& path) {
#ifdef _WIN32
    return QString::fromStdWString(path.wstring());
#else
    const auto bytes = path.u8string();
    return QString::fromUtf8(reinterpret_cast<const char*>(bytes.data()), bytes.size());
#endif
}

bool is_remote_url(const std::string& target){
    return target.rfind("http://",0)==0 || target.rfind("https://",0)==0 || target.rfind("data:",0)==0 || target.rfind("ftp://",0)==0;
}

bool is_within_collection(const std::filesystem::path& collectionRoot, const std::filesystem::path& candidate){
    try{
        auto canonRoot = std::filesystem::weakly_canonical(collectionRoot);
        auto canonCand = std::filesystem::weakly_canonical(candidate);
        std::string r = canonRoot.generic_string();
        std::string c = canonCand.generic_string();
        if(c==r) return true;
        if(c.size()>r.size() && c[r.size()]=='/' && c.rfind(r,0)==0) return true;
        return false;
    } catch(...) { return false; }
}

std::optional<QByteArray> file_hash(const std::filesystem::path& path) {
    QFile file(path_string(path));
    if (!file.open(QIODevice::ReadOnly)) return std::nullopt;
    QCryptographicHash hash(QCryptographicHash::Sha256);
    while (!file.atEnd()) {
        const QByteArray chunk = file.read(1 << 20);
        if (chunk.isEmpty() && file.error() != QFileDevice::NoError) return std::nullopt;
        hash.addData(chunk);
    }
    return hash.result();
}

bool copy_file_checked(const std::filesystem::path& source,
                       const std::filesystem::path& destination,
                       TransferCancellation* cancellation,
                       std::string& error) {
    std::ifstream input(source, std::ios::binary);
    std::ofstream output(destination, std::ios::binary | std::ios::trunc);
    if (!input || !output) {
        error = "unable to open attachment for copy";
        return false;
    }
    char buffer[1 << 20];
    while (input) {
        if (cancellation && cancellation->is_cancelled()) {
            error = "cancelled";
            return false;
        }
        input.read(buffer, sizeof(buffer));
        const auto count = input.gcount();
        if (count > 0) output.write(buffer, count);
        if (!output) {
            error = "attachment write failed";
            return false;
        }
    }
    if (!input.eof()) {
        error = "attachment read failed";
        return false;
    }
    output.flush();
    if (!output) {
        error = "attachment flush failed";
        return false;
    }
    const auto source_hash = file_hash(source);
    const auto destination_hash = file_hash(destination);
    if (!source_hash || !destination_hash || *source_hash != *destination_hash) {
        error = "attachment verification failed";
        return false;
    }
    return true;
}

std::string percent_decode(const std::string& s){
    std::string out;
    out.reserve(s.size());
    for(size_t i=0;i<s.size();++i){
        if(s[i]=='%' && i+2<s.size()){
            char hex[3]={s[i+1],s[i+2],0};
            char* e=nullptr;
            long v=strtol(hex,&e,16);
            if(e && *e==0){ out.push_back(char(v)); i+=2; continue; }
        }
        out.push_back(s[i]);
    }
    return out;
}

std::string trim(const std::string& text) {
    size_t start = 0, end = text.size();
    while (start < end && std::isspace(static_cast<unsigned char>(text[start]))) ++start;
    while (end > start && std::isspace(static_cast<unsigned char>(text[end - 1]))) --end;
    return text.substr(start, end - start);
}

using DestinationMap = std::unordered_map<std::string, std::string>;
struct RewriteContext {
    const MarkdownRewriteRequest& request;
    const DestinationMap& destinations;
    MarkdownRewriteResult& result;
    std::string source_path;

    void diagnostic(TransferSeverity severity, const std::string& code,
                    const std::string& message, const std::string& target) {
        result.diagnostics.push_back({severity, code, message, source_path, target, {}});
    }

    std::string relative_destination(const std::string& destination) const {
        const auto current = destinations.find(source_path);
        if (current == destinations.end()) return destination;
        return std::filesystem::relative(request.dest_workspace_root / destination,
            (request.dest_workspace_root / current->second).parent_path()).generic_string();
    }
};

std::optional<std::filesystem::path> asset_destination(
        const std::filesystem::path& source, const std::filesystem::path& directory,
        RewriteContext& context, const std::string& target) {
    std::error_code error;
    std::filesystem::create_directories(directory, error);
    if (error) {
        context.diagnostic(TransferSeverity::Error, "asset_copy_failed", error.message(), target);
        return std::nullopt;
    }
    const auto hash = file_hash(source);
    if (!hash) {
        context.diagnostic(TransferSeverity::Error, "asset_copy_failed", "unable to hash attachment", target);
        return std::nullopt;
    }
    auto destination = directory / source.filename();
    if (!std::filesystem::exists(destination)) return destination;
    const auto existing = file_hash(destination);
    if (!existing) {
        context.diagnostic(TransferSeverity::Error, "asset_copy_failed",
            "unable to verify existing attachment", target);
        return std::nullopt;
    }
    if (*existing != *hash) {
        const auto suffix = hash->toHex().left(12).toStdString();
        destination = directory / (source.stem().string() + "-" + suffix + source.extension().string());
    }
    return destination;
}

std::string copy_asset(const std::filesystem::path& source, const std::string& original,
                       const std::string& target, const std::string& anchor,
                       RewriteContext& context) {
    const auto current = context.destinations.find(context.source_path);
    if (current == context.destinations.end()) return original;
    const auto current_directory =
        (context.request.dest_workspace_root / current->second).parent_path();
    const auto destination = asset_destination(source, current_directory / "assets", context, target);
    if (!destination) return original;
    if (!std::filesystem::exists(*destination)) {
        std::string error;
        if (!copy_file_checked(source, *destination, context.request.cancellation, error)) {
            std::error_code ignored;
            std::filesystem::remove(*destination, ignored);
            const bool cancelled = error == "cancelled";
            context.diagnostic(cancelled ? TransferSeverity::Warning : TransferSeverity::Error,
                cancelled ? "cancelled" : "asset_copy_failed", error, target);
            return original;
        }
        ++context.result.copied_asset_count;
    }
    const auto relative = std::filesystem::relative(*destination, current_directory).generic_string();
    return QUrl::toPercentEncoding(QString::fromStdString(relative), QByteArray("/")).toStdString() + anchor;
}

std::optional<std::string> resolve_target(const std::string& target, RewriteContext& context) {
    const auto& root = context.request.source_collection_root;
    const auto sourceDirectory = root / std::filesystem::path(context.source_path).parent_path();
    const auto candidate = target.starts_with("/") ? root / target.substr(1)
        : sourceDirectory / percent_decode(target);
    std::filesystem::path normalized;
    try { normalized = std::filesystem::weakly_canonical(candidate); }
    catch (...) { normalized = candidate; }
    if (!is_within_collection(root, normalized)) {
        context.diagnostic(TransferSeverity::Warning, "link_outside_collection",
            "link target outside collection, preserving original", target);
        return std::nullopt;
    }
    std::string relative;
    try { relative = std::filesystem::relative(normalized, root).generic_string(); }
    catch (...) { relative.clear(); }
    if (relative.empty()) {
        context.diagnostic(TransferSeverity::Warning, "link_unresolvable",
            "link target not inside collection", target);
        return std::nullopt;
    }
    return relative;
}

std::string rewrite_target(const std::string& original, RewriteContext& context) {
    if (is_remote_url(original)) return original;
    const auto hash = original.find('#');
    const auto anchor = hash == std::string::npos ? "" : original.substr(hash);
    const auto target = trim(original.substr(0, hash));
    if (target.empty()) return original;
    const auto relative = resolve_target(target, context);
    if (!relative) return original;
    const auto mapped = context.destinations.find(*relative);
    if (mapped != context.destinations.end()) {
        return context.relative_destination(mapped->second) + anchor;
    }
    const auto asset = context.request.source_collection_root / *relative;
    std::error_code error;
    if (std::filesystem::is_regular_file(asset, error) && !error) {
        return copy_asset(asset, original, target, anchor, context);
    }
    context.diagnostic(TransferSeverity::Warning, "link_target_missing",
        "link target missing: " + *relative, target);
    return original;
}

std::optional<std::string> wiki_id_destination(const std::string& target, RewriteContext& context) {
    if (target.find('/') != std::string::npos || target.find('.') != std::string::npos) return std::nullopt;
    if (!context.destinations.contains(context.source_path)) return std::nullopt;
    const auto& preview = *context.request.preview;
    for (const auto& entry : preview.source_id_value_to_native) {
        const auto id = entry.second.find(target);
        if (id == entry.second.end()) continue;
        const auto destination = preview.native_id_to_dest_rel.find(id->second);
        if (destination != preview.native_id_to_dest_rel.end()) {
            return context.relative_destination(destination->second);
        }
    }
    return std::nullopt;
}

DestinationMap source_destinations(const TransferPreview& preview) {
    DestinationMap destinations;
    for (const auto& entry : preview.source_path_to_native_id) {
        const auto destination = preview.native_id_to_dest_rel.find(entry.second);
        if (destination != preview.native_id_to_dest_rel.end()) destinations[entry.first] = destination->second;
    }
    return destinations;
}
} // namespace

MarkdownRewriteResult rewrite_markdown_links(const MarkdownRewriteRequest& request) {
    MarkdownRewriteResult result;
    if (!request.preview) {
        result.diagnostics.push_back({TransferSeverity::Error, "bad_request", "preview null", {}, {}, {}});
        result.ok = false;
        return result;
    }
    const auto destinations = source_destinations(*request.preview);
    for (const auto& record : request.preview->records) {
        RewriteContext context{request, destinations, result, record.source_path};
        result.rewritten_body_by_source[record.source_path] = rewrite_link_destinations(record.body,
            [&](const std::string& target, bool wiki) {
                if (wiki) {
                    const auto hash = target.find('#');
                    const auto mapped = wiki_id_destination(target.substr(0, hash), context);
                    if (mapped) return *mapped + (hash == std::string::npos ? "" : target.substr(hash));
                }
                return rewrite_target(target, context);
            }, true);
    }
    result.ok = std::none_of(result.diagnostics.begin(), result.diagnostics.end(), [](const auto& diagnostic) {
        return diagnostic.severity == TransferSeverity::Error ||
            diagnostic.severity == TransferSeverity::Blocking || diagnostic.code == "cancelled";
    });
    return result;
}

} // namespace todobench::mdbase_transfer
