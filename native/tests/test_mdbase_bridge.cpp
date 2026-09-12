// SPDX-License-Identifier: GPL-3.0-or-later
#include "storage/mdbase_bridge_client.h"
#include "tb_test_assertions.h"

#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTest>

#include <filesystem>
#include <fstream>
#include <unordered_set>

using namespace todobench::mdbase;

namespace {

std::filesystem::path fixture_root(const std::string& name) {
    // Tests run with build/dev as CWD via CTest; also handle running from repo root.
    const std::vector<std::filesystem::path> candidates = {
        std::filesystem::path("native/tests/fixtures") / name,
        std::filesystem::path("../native/tests/fixtures") / name,
        std::filesystem::path("../../native/tests/fixtures") / name,
    };
    for (auto p : candidates) {
        if (std::filesystem::exists(p / "mdbase.yaml")) return std::filesystem::weakly_canonical(std::filesystem::absolute(p));
    }
    // Fallback: search up from source file location
    return std::filesystem::path(__FILE__).parent_path() / "fixtures" / name;
}

std::string read_bytes(const std::filesystem::path& p) {
    std::ifstream f(p, std::ios::binary);
    return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
}

}  // namespace

class MdbaseBridgeTest : public QObject {
    Q_OBJECT
private slots:
    void bridgeVersionNonEmpty();
    void openAndInspectMinimal();
    void readRawAndEffective();
    void validateAndQuery();
    void resolveLink();
    void invalidFixtureReportsDiagnostics();
    void sourceBytesUnchanged();
    void listTypesAndGetTypes();
};

void MdbaseBridgeTest::bridgeVersionNonEmpty() {
    const auto v = bridge_version();
    TB_VERIFY(!v.isEmpty());
    TB_VERIFY(v.contains(QStringLiteral("0.1")) || v.contains(QStringLiteral("1")) || !v.isEmpty());
}

void MdbaseBridgeTest::openAndInspectMinimal() {
    const auto root = fixture_root("mdbase-v03-minimal");
    TB_VERIFY2(std::filesystem::exists(root / "mdbase.yaml"), qPrintable(QString::fromStdString(root.string())));

    CollectionHandle handle;
    QString err;
    const auto open = open_collection(root, handle, &err);
    TB_VERIFY2(open.valid, qPrintable(err + QStringLiteral(" raw=") + open.raw_json.left(800)));
    TB_VERIFY(handle.is_open());
    TB_COMPARE(open.result.value(QStringLiteral("spec_profile")).toString(), QStringLiteral("v0.3"));

    const auto insp = handle.inspect();
    TB_VERIFY2(insp.valid, qPrintable(QJsonDocument(insp.result).toJson(QJsonDocument::Compact).left(2000)));
    const auto types = insp.result.value(QStringLiteral("types")).toArray();
    TB_COMPARE(types.size(), 2);
    std::unordered_set<std::string> names;
    for (const auto& t : types) names.insert(t.toObject().value(QStringLiteral("name")).toString().toStdString());
    TB_VERIFY(names.contains("task"));
    TB_VERIFY(names.contains("project"));
    // No diagnostics for valid fixture
    const auto diags = insp.result.value(QStringLiteral("diagnostics")).toArray();
    TB_VERIFY(diags.isEmpty());
}

void MdbaseBridgeTest::readRawAndEffective() {
    const auto root = fixture_root("mdbase-v03-minimal");
    CollectionHandle h;
    TB_VERIFY(open_collection(root, h).valid);

    QJsonObject in;
    in[QStringLiteral("path")] = QStringLiteral("tasks/a.md");
    const auto r = h.read(in);
    TB_VERIFY2(r.valid, qPrintable(QJsonDocument(r.result).toJson(QJsonDocument::Compact).left(2000)));
    const auto front = r.result.value(QStringLiteral("frontmatter")).toObject();
    const auto effective = r.result.value(QStringLiteral("effective_frontmatter")).toObject();
    TB_COMPARE(front.value(QStringLiteral("title")).toString(), QStringLiteral("Hello minimal"));
    TB_COMPARE(front.value(QStringLiteral("status")).toString(), QStringLiteral("todo"));
    TB_COMPARE(effective.value(QStringLiteral("status")).toString(), QStringLiteral("todo"));
    TB_COMPARE(effective.value(QStringLiteral("priority")).toString(), QStringLiteral("high"));
    TB_VERIFY(r.result.contains(QStringLiteral("body")));
    TB_VERIFY(r.result.value(QStringLiteral("body")).toString().contains(QStringLiteral("Body hello")));

    // Record that omits status/priority should have them only in effective
    QJsonObject in2;
    in2[QStringLiteral("path")] = QStringLiteral("tasks/needs-default.md");
    const auto r2 = h.read(in2);
    TB_VERIFY2(r2.valid, qPrintable(QJsonDocument(r2.result).toJson(QJsonDocument::Compact).left(2000)));
    const auto front2 = r2.result.value(QStringLiteral("frontmatter")).toObject();
    const auto eff2 = r2.result.value(QStringLiteral("effective_frontmatter")).toObject();
    TB_VERIFY(!front2.contains(QStringLiteral("status")));
    TB_VERIFY(!front2.contains(QStringLiteral("priority")));
    TB_COMPARE(eff2.value(QStringLiteral("status")).toString(), QStringLiteral("todo"));
    TB_COMPARE(eff2.value(QStringLiteral("priority")).toString(), QStringLiteral("normal"));
}

void MdbaseBridgeTest::validateAndQuery() {
    const auto root = fixture_root("mdbase-v03-minimal");
    CollectionHandle h;
    TB_VERIFY(open_collection(root, h).valid);

    QJsonObject vIn;
    vIn[QStringLiteral("path")] = QStringLiteral("tasks/a.md");
    const auto v = h.validate(vIn);
    TB_VERIFY2(v.valid, qPrintable(QJsonDocument(v.result).toJson(QJsonDocument::Compact).left(2000)));

    QJsonObject qIn;
    qIn[QStringLiteral("where")] = QStringLiteral("title == 'Hello minimal'");
    const auto q = h.query(qIn);
    TB_VERIFY2(q.valid, qPrintable(QJsonDocument(q.result).toJson(QJsonDocument::Compact).left(2000)));
    const auto results = q.result.value(QStringLiteral("results")).toArray();
    TB_COMPARE(results.size(), 1);
    TB_COMPARE(results.first().toObject().value(QStringLiteral("path")).toString(), QStringLiteral("tasks/a.md"));
}

void MdbaseBridgeTest::resolveLink() {
    const auto root = fixture_root("mdbase-v03-minimal");
    CollectionHandle h;
    TB_VERIFY(open_collection(root, h).valid);

    QJsonObject in;
    in[QStringLiteral("path")] = QStringLiteral("tasks/a.md");
    in[QStringLiteral("link")] = QStringLiteral("/projects/proj.md");
    const auto r = h.resolve_link(in);
    TB_VERIFY2(r.valid, qPrintable(QJsonDocument(r.result).toJson(QJsonDocument::Compact).left(2000)));
    TB_VERIFY(!r.result.value(QStringLiteral("resolved")).isNull());
    TB_COMPARE(r.result.value(QStringLiteral("resolved")).toString(), QStringLiteral("projects/proj.md"));
    TB_COMPARE(r.result.value(QStringLiteral("candidate")).toString(), QStringLiteral("projects/proj.md"));
    TB_VERIFY(!r.result.value(QStringLiteral("external")).toBool());
}

void MdbaseBridgeTest::invalidFixtureReportsDiagnostics() {
    const auto root = fixture_root("mdbase-v03-invalid");
    TB_VERIFY2(std::filesystem::exists(root / "mdbase.yaml"), qPrintable(QString::fromStdString(root.string())));
    CollectionHandle h;
    const auto open = open_collection(root, h);
    // Collection should still open (spec 0.3); invalid is per-record
    TB_VERIFY(open.valid);

    QJsonObject in;
    in[QStringLiteral("path")] = QStringLiteral("tasks/bad.md");
    const auto r = h.read(in);
    TB_VERIFY2(!r.valid, qPrintable(QJsonDocument(r.result).toJson(QJsonDocument::Compact).left(2000)));
    TB_VERIFY(!r.diagnostics.isEmpty());
    bool hasEnumError = false;
    for (const auto& d : r.diagnostics) {
        if (d.code.contains(QStringLiteral("schema_enum")) || d.code.contains(QStringLiteral("enum")) || d.message.contains(QStringLiteral("INVALID"))) hasEnumError = true;
    }
    TB_VERIFY2(hasEnumError, qPrintable(QJsonDocument(r.result).toJson(QJsonDocument::Compact).left(2000)));

    const auto v = h.validate(in);
    TB_VERIFY(!v.valid);
    TB_VERIFY(!v.diagnostics.isEmpty());
}

void MdbaseBridgeTest::sourceBytesUnchanged() {
    const auto root = fixture_root("mdbase-v03-minimal");
    const auto target = root / "tasks/a.md";
    const auto before = read_bytes(target);
    CollectionHandle h;
    TB_VERIFY(open_collection(root, h).valid);
    QJsonObject in;
    in[QStringLiteral("path")] = QStringLiteral("tasks/a.md");
    (void)h.read(in);
    (void)h.validate(in);
    QJsonObject q;
    q[QStringLiteral("where")] = QStringLiteral("title == 'Hello minimal'");
    (void)h.query(q);
    const auto after = read_bytes(target);
    TB_COMPARE(before, after);
}

void MdbaseBridgeTest::listTypesAndGetTypes() {
    const auto root = fixture_root("mdbase-v03-minimal");
    CollectionHandle h;
    TB_VERIFY(open_collection(root, h).valid);

    const auto lt = h.list_types();
    TB_VERIFY2(lt.valid, qPrintable(QJsonDocument(lt.result).toJson(QJsonDocument::Compact).left(2000)));
    const auto types = lt.result.value(QStringLiteral("types")).toArray();
    TB_VERIFY(types.size() >= 2);

    QJsonObject gtIn;
    gtIn[QStringLiteral("path")] = QStringLiteral("tasks/a.md");
    const auto gt = h.get_types(gtIn);
    TB_VERIFY2(gt.valid, qPrintable(QJsonDocument(gt.result).toJson(QJsonDocument::Compact).left(2000)));
    const auto arr = gt.result.value(QStringLiteral("types")).toArray();
    TB_VERIFY(arr.size() >= 1);
    bool hasTask = false;
    for (const auto& v : arr) if (v.toString() == QStringLiteral("task")) hasTask = true;
    TB_VERIFY(hasTask);
}

QTEST_MAIN(MdbaseBridgeTest)
#include "test_mdbase_bridge.moc"
