#include "HistoryWidget.h"

#include <CommitHistoryModel.h>
#include <CommitHistoryView.h>
#include <RepositoryViewDelegate.h>
#include <BranchesWidget.h>
#include <WipWidget.h>
#include <AmendWidget.h>
#include <CommitInfoWidget.h>
#include <CommitInfo.h>
#include <GitQlientSettings.h>
#include <GitBase.h>
#include <GitBranches.h>
#include <GitRepoLoader.h>
#include <GitRemote.h>
#include <GitMerge.h>
#include <GitLocal.h>
#include <FileEditor.h>
#include <GitQlientSettings.h>
#include <GitQlientStyles.h>

#include <QLogger.h>

#include <QPushButton>
#include <QGridLayout>
#include <QLineEdit>
#include <QStackedWidget>
#include <QCheckBox>
#include <QMessageBox>
#include <QApplication>

using namespace QLogger;

HistoryWidget::HistoryWidget(const QSharedPointer<RevisionsCache> &cache, const QSharedPointer<GitBase> git,
                             QWidget *parent)
   : QFrame(parent)
   , mGit(git)
   , mCache(cache)
   , mRepositoryModel(new CommitHistoryModel(mCache, git))
   , mRepositoryView(new CommitHistoryView(mCache, git))
   , mBranchesWidget(new BranchesWidget(mCache, git))
   , mSearchInput(new QLineEdit())
   , mCommitStackedWidget(new QStackedWidget())
   , mWipWidget(new WipWidget(mCache, git))
   , mAmendWidget(new AmendWidget(mCache, git))
   , mCommitInfoWidget(new CommitInfoWidget(mCache, git))
   , mChShowAllBranches(new QCheckBox(tr("Show all branches")))
   , mGraphFrame(new QFrame())
   , mFileEditor(new FileEditor())
{
   setAttribute(Qt::WA_DeleteOnClose);

   mCommitStackedWidget->setCurrentIndex(0);
   mCommitStackedWidget->addWidget(mCommitInfoWidget);
   mCommitStackedWidget->addWidget(mWipWidget);
   mCommitStackedWidget->addWidget(mAmendWidget);
   mCommitStackedWidget->setFixedWidth(310);

   if (GitQlientSettings settings; !settings.value("isGitQlient", false).toBool())
      connect(mWipWidget, &WipWidget::signalEditFile, this, &HistoryWidget::signalEditFile);
   else
   {
      connect(mWipWidget, &WipWidget::signalEditFile, this, &HistoryWidget::startEditFile);
      connect(mFileEditor, &FileEditor::signalEditionClosed, this, &HistoryWidget::endEditFile);
   }

   connect(mWipWidget, &WipWidget::signalShowDiff, this, &HistoryWidget::signalShowDiff);
   connect(mWipWidget, &WipWidget::signalChangesCommitted, this, &HistoryWidget::signalChangesCommitted);
   connect(mWipWidget, &WipWidget::signalCheckoutPerformed, this, &HistoryWidget::signalUpdateUi);
   connect(mWipWidget, &WipWidget::signalShowFileHistory, this, &HistoryWidget::signalShowFileHistory);
   connect(mWipWidget, &WipWidget::signalUpdateWip, this, &HistoryWidget::signalUpdateWip);
   connect(mWipWidget, &WipWidget::signalCancelAmend, this, &HistoryWidget::onCommitSelected);

   connect(mAmendWidget, &AmendWidget::signalEditFile, this, &HistoryWidget::signalEditFile);
   connect(mAmendWidget, &AmendWidget::signalShowDiff, this, &HistoryWidget::signalShowDiff);
   connect(mAmendWidget, &AmendWidget::signalChangesCommitted, this, &HistoryWidget::signalChangesCommitted);
   connect(mAmendWidget, &AmendWidget::signalCheckoutPerformed, this, &HistoryWidget::signalUpdateUi);
   connect(mAmendWidget, &AmendWidget::signalShowFileHistory, this, &HistoryWidget::signalShowFileHistory);
   connect(mAmendWidget, &AmendWidget::signalUpdateWip, this, &HistoryWidget::signalUpdateWip);
   connect(mAmendWidget, &AmendWidget::signalCancelAmend, this, &HistoryWidget::onCommitSelected);

   connect(mCommitInfoWidget, &CommitInfoWidget::signalOpenFileCommit, this, &HistoryWidget::signalShowDiff);
   connect(mCommitInfoWidget, &CommitInfoWidget::signalShowFileHistory, this, &HistoryWidget::signalShowFileHistory);

   mSearchInput->setPlaceholderText(tr("Press Enter to search by SHA or log message..."));
   connect(mSearchInput, &QLineEdit::returnPressed, this, &HistoryWidget::search);

   connect(mRepositoryView, &CommitHistoryView::signalViewUpdated, this, &HistoryWidget::signalViewUpdated);
   connect(mRepositoryView, &CommitHistoryView::signalOpenDiff, this, &HistoryWidget::signalOpenDiff);
   connect(mRepositoryView, &CommitHistoryView::signalOpenCompareDiff, this, &HistoryWidget::signalOpenCompareDiff);
   connect(mRepositoryView, &CommitHistoryView::clicked, this, &HistoryWidget::commitSelected);
   connect(mRepositoryView, &CommitHistoryView::customContextMenuRequested, this, [this](const QPoint &pos) {
      const auto rowIndex = mRepositoryView->indexAt(pos);
      commitSelected(rowIndex);
   });
   connect(mRepositoryView, &CommitHistoryView::doubleClicked, this, &HistoryWidget::openDiff);
   connect(mRepositoryView, &CommitHistoryView::signalAmendCommit, this, &HistoryWidget::onAmendCommit);
   connect(mRepositoryView, &CommitHistoryView::signalMergeRequired, this, &HistoryWidget::mergeBranch);
   connect(mRepositoryView, &CommitHistoryView::signalCherryPickConflict, this,
           &HistoryWidget::signalCherryPickConflict);
   connect(mRepositoryView, &CommitHistoryView::signalPullConflict, this, &HistoryWidget::signalPullConflict);

   mRepositoryView->setObjectName("historyGraphView");
   mRepositoryView->setModel(mRepositoryModel);
   mRepositoryView->setItemDelegate(mItemDelegate = new RepositoryViewDelegate(cache, git, mRepositoryView));
   mRepositoryView->setEnabled(true);

   connect(mBranchesWidget, &BranchesWidget::signalBranchesUpdated, this, &HistoryWidget::signalUpdateCache);
   connect(mBranchesWidget, &BranchesWidget::signalBranchCheckedOut, this, &HistoryWidget::onBranchCheckout);

   connect(mBranchesWidget, &BranchesWidget::signalSelectCommit, mRepositoryView, &CommitHistoryView::focusOnCommit);
   connect(mBranchesWidget, &BranchesWidget::signalSelectCommit, this, &HistoryWidget::goToSha);
   connect(mBranchesWidget, &BranchesWidget::signalOpenSubmodule, this, &HistoryWidget::signalOpenSubmodule);
   connect(mBranchesWidget, &BranchesWidget::signalMergeRequired, this, &HistoryWidget::mergeBranch);
   connect(mBranchesWidget, &BranchesWidget::signalPullConflict, this, &HistoryWidget::signalPullConflict);

   GitQlientSettings settings;

   const auto cherryPickBtn = new QPushButton(tr("Cherry-pick"));
   cherryPickBtn->setEnabled(false);
   cherryPickBtn->setObjectName("pbCherryPick");
   connect(cherryPickBtn, &QPushButton::clicked, this, &HistoryWidget::cherryPickCommit);
   connect(mSearchInput, &QLineEdit::textChanged, this,
           [cherryPickBtn](const QString &text) { cherryPickBtn->setEnabled(!text.isEmpty()); });

   mChShowAllBranches->setChecked(settings.value("ShowAllBranches", true).toBool());
   connect(mChShowAllBranches, &QCheckBox::toggled, this, &HistoryWidget::onShowAllUpdated);

   const auto graphOptionsLayout = new QHBoxLayout();
   graphOptionsLayout->setContentsMargins(QMargins());
   graphOptionsLayout->setSpacing(10);
   graphOptionsLayout->addWidget(mSearchInput);
   graphOptionsLayout->addWidget(cherryPickBtn);
   graphOptionsLayout->addWidget(mChShowAllBranches);

   const auto viewLayout = new QVBoxLayout(mGraphFrame);
   viewLayout->setContentsMargins(QMargins());
   viewLayout->setSpacing(5);
   viewLayout->addLayout(graphOptionsLayout);
   viewLayout->addWidget(mRepositoryView);

   mFileEditor->setVisible(false);

   const auto layout = new QHBoxLayout();
   layout->setContentsMargins(QMargins());
   layout->setSpacing(15);
   layout->addWidget(mCommitStackedWidget);
   layout->addWidget(mGraphFrame);
   layout->addWidget(mFileEditor);
   layout->addWidget(mBranchesWidget);

   setLayout(layout);
}

HistoryWidget::~HistoryWidget()
{
   delete mItemDelegate;
   delete mRepositoryModel;
}

void HistoryWidget::clear()
{
   mRepositoryView->clear();
   resetWip();
   mBranchesWidget->clear();
   mCommitInfoWidget->clear();
   mAmendWidget->clear();

   mCommitStackedWidget->setCurrentIndex(mCommitStackedWidget->currentIndex());
}

void HistoryWidget::resetWip()
{
   mWipWidget->clear();
}

void HistoryWidget::loadBranches()
{
   mBranchesWidget->showBranches();
}

void HistoryWidget::updateUiFromWatcher()
{
   const auto commitStackedIndex = mCommitStackedWidget->currentIndex();

   if (commitStackedIndex == 1)
      mWipWidget->configure(CommitInfo::ZERO_SHA);
   else if (commitStackedIndex == 2)
      mAmendWidget->reload();
}

void HistoryWidget::focusOnCommit(const QString &sha)
{
   mRepositoryView->focusOnCommit(sha);
}

QString HistoryWidget::getCurrentSha() const
{
   return mRepositoryView->getCurrentSha();
}

void HistoryWidget::onNewRevisions(int totalCommits)
{
   mRepositoryModel->onNewRevisions(totalCommits);

   onCommitSelected(CommitInfo::ZERO_SHA);

   mRepositoryView->selectionModel()->select(
       QItemSelection(mRepositoryModel->index(0, 0), mRepositoryModel->index(0, mRepositoryModel->columnCount() - 1)),
       QItemSelectionModel::Select);
}

void HistoryWidget::search()
{
   const auto text = mSearchInput->text();

   if (!text.isEmpty())
   {
      auto commitInfo = mCache->getCommitInfo(text);

      if (commitInfo.isValid())
         goToSha(text);
      else
      {
         auto selectedItems = mRepositoryView->selectedIndexes();
         auto startingRow = 0;

         if (!selectedItems.isEmpty())
         {
            std::sort(selectedItems.begin(), selectedItems.end(),
                      [](const QModelIndex index1, const QModelIndex index2) { return index1.row() <= index2.row(); });
            startingRow = selectedItems.constFirst().row();
         }

         commitInfo = mCache->getCommitInfoByField(CommitInfo::Field::SHORT_LOG, text, startingRow + 1);

         if (commitInfo.isValid())
            goToSha(commitInfo.sha());
      }
   }
}

void HistoryWidget::goToSha(const QString &sha)
{
   mRepositoryView->focusOnCommit(sha);

   onCommitSelected(sha);
}

void HistoryWidget::commitSelected(const QModelIndex &index)
{
   const auto sha = mRepositoryModel->sha(index.row());

   onCommitSelected(sha);
}

void HistoryWidget::openDiff(const QModelIndex &index)
{
   const auto sha = mRepositoryModel->sha(index.row());

   emit signalOpenDiff(sha);
}

void HistoryWidget::onShowAllUpdated(bool showAll)
{
   GitQlientSettings settings;
   settings.setValue("ShowAllBranches", showAll);

   emit signalAllBranchesActive(showAll);
}

void HistoryWidget::onBranchCheckout()
{
   QScopedPointer<GitBranches> gitBranches(new GitBranches(mGit));
   const auto ret = gitBranches->getLastCommitOfBranch(mGit->getCurrentBranch());

   if (mChShowAllBranches->isChecked())
      mRepositoryView->focusOnCommit(ret.output.toString());

   emit signalUpdateCache();
}

void HistoryWidget::mergeBranch(const QString &current, const QString &branchToMerge)
{
   QApplication::setOverrideCursor(QCursor(Qt::WaitCursor));
   QScopedPointer<GitMerge> git(new GitMerge(mGit, mCache));
   const auto ret = git->merge(current, { branchToMerge });

   QScopedPointer<GitRepoLoader> gitLoader(new GitRepoLoader(mGit, mCache));
   gitLoader->updateWipRevision();

   QApplication::restoreOverrideCursor();

   if (ret.output.toString().contains("merge failed", Qt::CaseInsensitive))
   {
      QMessageBox msgBox(QMessageBox::Critical, tr("Merge failed"),
                         QString("There were problems during the merge. Please, see the detailed description for more "
                                 "information.<br><br>GitQlient will show the merge helper tool."),
                         QMessageBox::Ok, this);
      msgBox.setDetailedText(ret.output.toString());
      msgBox.setStyleSheet(GitQlientStyles::getStyles());
      msgBox.exec();

      emit signalMergeConflicts();
   }
   else
   {
      const auto outputStr = ret.output.toString();

      if (!outputStr.isEmpty())
      {
         if (ret.success)
         {
            emit signalUpdateCache();

            QMessageBox msgBox(
                QMessageBox::Information, tr("Merge successful"),
                QString("The merge was successfully done. See the detailed description for more information."),
                QMessageBox::Ok, this);
            msgBox.setDetailedText(ret.output.toString());
            msgBox.setStyleSheet(GitQlientStyles::getStyles());
            msgBox.exec();
         }
         else
         {
            QMessageBox msgBox(
                QMessageBox::Warning, tr("Merge status"),
                QString(
                    "There were problems during the merge. Please, see the detailed description for more information."),
                QMessageBox::Ok, this);
            msgBox.setDetailedText(ret.output.toString());
            msgBox.setStyleSheet(GitQlientStyles::getStyles());
            msgBox.exec();
         }
      }
   }
}

void HistoryWidget::onCommitSelected(const QString &goToSha)
{
   const auto isWip = goToSha == CommitInfo::ZERO_SHA;
   mCommitStackedWidget->setCurrentIndex(isWip);

   QLog_Info("UI", QString("Selected commit {%1}").arg(goToSha));

   if (isWip)
      mWipWidget->configure(goToSha);
   else
      mCommitInfoWidget->configure(goToSha);
}

void HistoryWidget::onAmendCommit(const QString &sha)
{
   mCommitStackedWidget->setCurrentIndex(2);
   mAmendWidget->configure(sha);
}

void HistoryWidget::startEditFile(const QString &fileName)
{
   mGraphFrame->setVisible(false);

   mFileEditor->editFile(fileName);
   mFileEditor->setVisible(true);
}

void HistoryWidget::endEditFile()
{
   mGraphFrame->setVisible(true);
   mFileEditor->setVisible(false);
}

void HistoryWidget::cherryPickCommit()
{
   if (const auto commit = mCache->getCommitInfo(mSearchInput->text()); commit.isValid())
   {
      const auto git = QScopedPointer<GitLocal>(new GitLocal(mGit));

      const auto ret = git->cherryPickCommit(commit.sha());

      if (ret.success)
      {
         mSearchInput->clear();
         emit signalViewUpdated();
      }
      else
      {
         const auto errorMsg = ret.output.toString();

         if (errorMsg.contains("error: could not apply", Qt::CaseInsensitive)
             && errorMsg.contains("causing a conflict", Qt::CaseInsensitive))
         {
            emit signalCherryPickConflict();
         }
         else
         {
            QMessageBox msgBox(QMessageBox::Critical, tr("Error while cherry-pick"),
                               QString("There were problems during the cherry-pick operation. Please, see the detailed "
                                       "description for more information."),
                               QMessageBox::Ok, this);
            msgBox.setDetailedText(errorMsg);
            msgBox.setStyleSheet(GitQlientStyles::getStyles());
            msgBox.exec();
         }
      }
   }
}
