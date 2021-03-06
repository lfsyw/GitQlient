#include "GitRepoLoader.h"

#include <GitBase.h>
#include <RevisionsCache.h>
#include <GitRequestorProcess.h>
#include <GitBranches.h>

#include <QLogger.h>
#include <BenchmarkTool.h>

#include <QDir>

using namespace QLogger;
using namespace GitQlientTools;

static const QString GIT_LOG_FORMAT("%m%HX%P%n%cn<%ce>%n%an<%ae>%n%at%n%s%n%b ");

GitRepoLoader::GitRepoLoader(QSharedPointer<GitBase> gitBase, QSharedPointer<RevisionsCache> cache, QObject *parent)
   : QObject(parent)
   , mGitBase(gitBase)
   , mRevCache(std::move(cache))
{
}

bool GitRepoLoader::loadRepository()
{
   BenchmarkStart();

   if (mLocked)
      QLog_Warning("Git", "Git is currently loading data.");
   else
   {
      if (mGitBase->getWorkingDir().isEmpty())
         QLog_Error("Git", "No working directory set.");
      else
      {
         QLog_Info("Git", "Initializing Git...");

         mLocked = true;

         if (configureRepoDirectory())
         {
            mGitBase->updateCurrentBranch();

            QLog_Info("Git", "... Git initialization finished.");

            QLog_Info("Git", "Requesting revisions...");

            requestRevisions();

            BenchmarkEnd();

            return true;
         }
         else
            QLog_Error("Git", "The working directory is not a Git repository.");
      }
   }

   BenchmarkEnd();

   return false;
}

bool GitRepoLoader::configureRepoDirectory()
{
   BenchmarkStart();

   QLog_Debug("Git", "Configuring repository directory.");

   const auto ret = mGitBase->run("git rev-parse --show-cdup");

   if (ret.success)
   {
      QDir d(QString("%1/%2").arg(mGitBase->getWorkingDir(), ret.output.toString().trimmed()));
      mGitBase->setWorkingDir(d.absolutePath());

      BenchmarkEnd();

      return true;
   }

   BenchmarkEnd();

   return false;
}

void GitRepoLoader::loadReferences()
{
   BenchmarkStart();

   QLog_Debug("Git", "Loading references.");

   const auto ret3 = mGitBase->run("git show-ref -d");

   if (ret3.success)
   {
      auto ret = mGitBase->getLastCommit();

      if (ret.success)
         ret.output = ret.output.toString().trimmed();

      QString prevRefSha;
      const auto curBranchSHA = ret.output.toString();

#if QT_VERSION >= QT_VERSION_CHECK(5, 14, 0)
      const auto referencesList = ret3.output.toString().split('\n', Qt::SkipEmptyParts);
#else
      const auto referencesList = ret3.output.toString().split('\n', QString::SkipEmptyParts);
#endif

      for (const auto &reference : referencesList)
      {
         const auto revSha = reference.left(40);
         const auto refName = reference.mid(41);

         if (!refName.startsWith("refs/tags/") || (refName.startsWith("refs/tags/") && refName.endsWith("^{}")))
         {
            auto localBranches = false;
            References::Type type;
            QString name;

            if (refName.startsWith("refs/tags/"))
            {
               type = References::Type::Tag;
               name = refName.mid(10, reference.length());
               name.remove("^{}");
            }
            else if (refName.startsWith("refs/heads/"))
            {
               type = References::Type::LocalBranch;
               name = refName.mid(11);
               localBranches = true;
            }
            else if (refName.startsWith("refs/remotes/") && !refName.endsWith("HEAD"))
            {
               type = References::Type::RemoteBranches;
               name = refName.mid(13);
            }
            else
               continue;

            mRevCache->insertReference(revSha, type, name);

            if (localBranches)
            {
               const auto git = new GitBranches(mGitBase);
               RevisionsCache::LocalBranchDistances distances;

               const auto distToMaster = git->getDistanceBetweenBranches(true, name);
               auto toMaster = distToMaster.output.toString();

               if (!toMaster.contains("fatal"))
               {
                  toMaster.replace('\n', "");
                  const auto values = toMaster.split('\t');
                  distances.behindMaster = values.first().toUInt();
                  distances.aheadMaster = values.last().toUInt();
               }

               const auto distToOrigin = git->getDistanceBetweenBranches(false, name);
               auto toOrigin = distToOrigin.output.toString();

               if (!toOrigin.contains("fatal"))
               {
                  toOrigin.replace('\n', "");
                  const auto values = toOrigin.split('\t');
                  distances.behindOrigin = values.first().toUInt();
                  distances.aheadOrigin = values.last().toUInt();
               }

               mRevCache->insertLocalBranchDistances(name, distances);
            }
         }

         prevRefSha = revSha;
      }
   }

   BenchmarkEnd();
}

void GitRepoLoader::requestRevisions()
{
   BenchmarkStart();

   QLog_Debug("Git", "Loading revisions.");

   const auto baseCmd = QString("git log --date-order --no-color --log-size --parents --boundary -z --pretty=format:")
                            .append(GIT_LOG_FORMAT)
                            .append(mShowAll ? QString("--all") : mGitBase->getCurrentBranch());

   const auto requestor = new GitRequestorProcess(mGitBase->getWorkingDir());
   connect(requestor, &GitRequestorProcess::procDataReady, this, &GitRepoLoader::processRevision);
   connect(this, &GitRepoLoader::cancelAllProcesses, requestor, &AGitProcess::onCancel);

   requestor->run(baseCmd);

   BenchmarkEnd();
}

void GitRepoLoader::processRevision(const QByteArray &ba)
{
   BenchmarkStart();

   QLog_Info("Git", "Revisions received!");

   QLog_Debug("Git", "Processing revisions...");

   const auto &commits = ba.split('\000');

   emit signalLoadingStarted(commits.count());

   const auto wipInfo = processWip();

   mRevCache->setup(wipInfo, commits);

   loadReferences();

   mRevCache->setConfigurationDone();

   mLocked = false;

   BenchmarkEnd();

   emit signalLoadingFinished();
}

WipRevisionInfo GitRepoLoader::processWip()
{
   BenchmarkStart();

   QLog_Debug("Git", QString("Executing processWip."));

   mRevCache->setUntrackedFilesList(getUntrackedFiles());

   const auto ret = mGitBase->run("git rev-parse --revs-only HEAD");

   if (ret.success)
   {
      const auto parentSha = ret.output.toString().trimmed();

      const auto ret3 = mGitBase->run(QString("git diff-index %1").arg(parentSha));
      const auto diffIndex = ret3.success ? ret3.output.toString() : QString();

      const auto ret4 = mGitBase->run(QString("git diff-index --cached %1").arg(parentSha));
      const auto diffIndexCached = ret4.success ? ret4.output.toString() : QString();

      return { parentSha, diffIndex, diffIndexCached };
   }

   BenchmarkEnd();

   return {};
}

void GitRepoLoader::updateWipRevision()
{
   BenchmarkStart();

   if (const auto wipInfo = processWip(); wipInfo.isValid())
      mRevCache->updateWipCommit(wipInfo.parentSha, wipInfo.diffIndex, wipInfo.diffIndexCached);

   BenchmarkEnd();
}

QVector<QString> GitRepoLoader::getUntrackedFiles() const
{
   BenchmarkStart();

   QLog_Debug("Git", QString("Executing getUntrackedFiles."));

   auto runCmd = QString("git ls-files --others");
   const auto exFile = QString(".git/info/exclude");
   const auto path = QString("%1/%2").arg(mGitBase->getWorkingDir(), exFile);

   if (QFile::exists(path))
      runCmd.append(QString(" --exclude-from=$%1$").arg(exFile));

   runCmd.append(QString(" --exclude-per-directory=$%1$").arg(".gitignore"));

#if QT_VERSION >= QT_VERSION_CHECK(5, 14, 0)
   const auto ret = mGitBase->run(runCmd).output.toString().split('\n', Qt::SkipEmptyParts).toVector();
#else
   const auto ret = mGitBase->run(runCmd).output.toString().split('\n', QString::SkipEmptyParts).toVector();
#endif

   BenchmarkEnd();

   return ret;
}
