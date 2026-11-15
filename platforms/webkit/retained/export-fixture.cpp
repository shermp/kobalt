#include "publication/local_resources.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QDebug>

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    const auto args = app.arguments();
    if (args.size() != 4) {
        qCritical("usage: kobalt-webkit-export-fixture TYPE_TESTER_EPUB FIXTURE_FONT OUTPUT_DIRECTORY");
        return 2;
    }
    const QDir output(args[3]);
    if (!output.exists())
        return 2;

    QString error;
    auto book = kobalt::openLocalWebResources(args[1], std::make_shared<kobalt::SourceLease>(), &error);
    if (!book) {
        qCritical() << error;
        return 1;
    }
    const QStringList chapters{QStringLiteral("OEBPS/chapter1.xhtml"),
                               QStringLiteral("OEBPS/chapter2.xhtml")};
    for (const auto &chapter : chapters) {
        if (!book->spine().contains(chapter)) {
            qCritical() << "Missing spine chapter" << chapter;
            return 1;
        }
    }
    const auto members = chapters + QStringList{QStringLiteral("OEBPS/styles.css"),
                                                 QStringLiteral("OEBPS/ornament.png")};
    for (const auto &member : members) {
        const auto resource = book->read(book->url(member));
        QFile file(output.filePath(member.section('/', -1)));
        if (resource.bytes.isEmpty() || !file.open(QIODevice::WriteOnly | QIODevice::NewOnly)
            || file.write(resource.bytes) != resource.bytes.size()) {
            qCritical() << "Unable to stage fixture member" << member;
            return 1;
        }
    }
    if (!QFile::copy(args[2], output.filePath(QStringLiteral("font.ttf")))) {
        qCritical("Unable to stage the licensed fixture font");
        return 1;
    }
    qInfo() << "Staged authorized Type Tester resources, fingerprint" << book->fingerprint();
}
