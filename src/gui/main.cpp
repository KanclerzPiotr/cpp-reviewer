#include "MainWindow.hpp"
#include "RevisionDialog.hpp"

#include <QApplication>
#include <QCommandLineParser>
#include <QDir>
#include <QMessageBox>
#include <QSettings>
#include <QTimer>

int main(int argc, char** argv)
{
    QApplication app(argc, argv);
    QApplication::setOrganizationName(QStringLiteral("cppreviewer"));
    QApplication::setApplicationName(QStringLiteral("C++ Reviewer"));
    QApplication::setApplicationVersion(QStringLiteral("0.1"));

    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("Semantic code review tool for C++"));
    parser.addHelpOption();
    parser.addVersionOption();
    QCommandLineOption repoOpt({QStringLiteral("C"), QStringLiteral("repo")}, QStringLiteral("Git repository to open."),
                               QStringLiteral("dir"));
    QCommandLineOption prOpt(QStringLiteral("pr"), QStringLiteral("Review GitHub pull request <n>."), QStringLiteral("n"));
    QCommandLineOption remoteOpt(QStringLiteral("remote"), QStringLiteral("Remote for --pr (default: origin)."),
                                 QStringLiteral("name"), QStringLiteral("origin"));
    QCommandLineOption baseOpt(QStringLiteral("base"), QStringLiteral("Base branch for --pr."), QStringLiteral("branch"));
    QCommandLineOption dirsOpt(QStringLiteral("dirs"), QStringLiteral("Compare two directories: OLD NEW."));
    QCommandLineOption shotOpt(QStringLiteral("screenshot"), QStringLiteral("Save a screenshot after the review and quit."),
                               QStringLiteral("file"));
    QCommandLineOption showOpt(QStringLiteral("show-change"),
                               QStringLiteral("With --screenshot: open the comparison of the first change containing <text>."),
                               QStringLiteral("text"));
    QCommandLineOption themeOpt(QStringLiteral("theme"), QStringLiteral("Color theme: system, light or dark."),
                                QStringLiteral("mode"));
    QCommandLineOption switchOpt(QStringLiteral("switch-theme"),
                                 QStringLiteral("With --screenshot: switch to <mode> after the review."), QStringLiteral("mode"));
    switchOpt.setFlags(QCommandLineOption::HiddenFromHelp);
    parser.addOptions({themeOpt, switchOpt});
    shotOpt.setFlags(QCommandLineOption::HiddenFromHelp);
    showOpt.setFlags(QCommandLineOption::HiddenFromHelp);
    parser.addOptions({repoOpt, prOpt, remoteOpt, baseOpt, dirsOpt, shotOpt, showOpt});
    parser.addPositionalArgument(QStringLiteral("base"), QStringLiteral("Base revision (default: HEAD)."), QStringLiteral("[base"));
    parser.addPositionalArgument(QStringLiteral("target"),
                                 QStringLiteral("Target revision: a git revision, WORKTREE or INDEX (default: WORKTREE)."),
                                 QStringLiteral("[target]]"));
    parser.process(app);

    if (parser.isSet(themeOpt))
        QSettings().setValue(QStringLiteral("theme"), parser.value(themeOpt));
    gui::MainWindow::applyAppTheme(QSettings().value(QStringLiteral("theme"), QStringLiteral("system")).toString());
    gui::MainWindow w;
    w.show();
    if (parser.isSet(shotOpt)) {
        const QString file = parser.value(shotOpt);
        const QString change = parser.value(showOpt);
        const QString switchTo = parser.value(switchOpt);
        QObject::connect(&w, &gui::MainWindow::reviewFinished, &w, [&w, file, change, switchTo] {
            if (!switchTo.isEmpty())
                w.setTheme(switchTo);
            if (change.startsWith(QStringLiteral("select:")))
                w.activateChangeByTitle(change.mid(7), false);
            else if (!change.isEmpty())
                w.activateChangeByTitle(change, true);
            QTimer::singleShot(800, &w, [&w, file] {
                w.grab().save(file);
                QApplication::quit();
            });
        });
    }

    const auto args = parser.positionalArguments();
    if (parser.isSet(dirsOpt)) {
        if (args.size() != 2) {
            QMessageBox::critical(&w, QStringLiteral("C++ Reviewer"), QStringLiteral("--dirs needs two directories."));
            return 2;
        }
        w.compareDirectories(args[0], args[1]);
        return app.exec();
    }

    QString repoPath = parser.value(repoOpt);
    if (repoPath.isEmpty())
        repoPath = cr::GitRepo::open(QDir::currentPath().toStdString()) ? QDir::currentPath()
                                                                        : QSettings().value(QStringLiteral("lastRepository")).toString();
    if (repoPath.isEmpty() || !w.openRepository(repoPath))
        return app.exec();

    if (parser.isSet(prOpt)) {
        gui::PullRequestChoice pr;
        pr.remote = parser.value(remoteOpt);
        pr.number = parser.value(prOpt).toInt();
        pr.baseRef = parser.value(baseOpt);
        if (pr.baseRef.isEmpty()) {
            auto repo = cr::GitRepo::open(repoPath.toStdString());
            auto def = repo ? repo->remoteDefaultBranch(pr.remote.toStdString()) : std::string{};
            pr.baseRef = def.empty() ? QStringLiteral("main") : QString::fromStdString(def);
        }
        w.reviewPullRequest(pr);
    } else if (!args.isEmpty()) {
        auto repo = cr::GitRepo::open(repoPath.toStdString());
        auto base = cr::parseRevisionSpec(*repo, args[0].toStdString());
        auto target = cr::parseRevisionSpec(*repo, args.size() > 1 ? args[1].toStdString() : std::string("WORKTREE"));
        if (!base || !target) {
            QMessageBox::critical(&w, QStringLiteral("C++ Reviewer"), QStringLiteral("Unknown revision."));
        } else {
            w.setRevisions(*base, *target);
        }
    } else {
        w.startReview();
    }
    return app.exec();
}
