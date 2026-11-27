// test_session_tree_removal.cpp
// "Remove from Session" in the Session Data tree (issue #580): the Delete key
// and the context-menu entry reach the window's handler with the selected
// products, the question is asked only when something would be lost, and what
// the handler left in the registry goes from the tree.
//
// A process of its own: it needs QtWidgets, which VisIVOTests does not link.

#include "SessionDataTree.h"

#include "app/ProductRegistry.h"

#include <QAbstractItemModel>
#include <QApplication>
#include <QMenu>
#include <QMessageBox>
#include <QPushButton>
#include <QStandardItemModel>
#include <QTest>
#include <QTimer>
#include <QTreeView>

using namespace Qt::StringLiterals;

// The tree asks the cache for a dataset's thumbnail. The real one grabs OpenGL
// widgets, which this test does not link: no thumbnail is a valid answer.
namespace ThumbnailCache {
QPixmap load(const QString &, int, int)
{
    return QPixmap();
}
} // namespace ThumbnailCache

namespace {

const QString kDataset = u"/data/cube.fits"_s;

/// Answer the question the tree opens: click the button called @p text as soon
/// as a modal message box is up. @p seen tells the test whether one ever was.
void answerWith(const QString &text, bool *seen, int attempts = 40)
{
    QTimer::singleShot(25, [text, seen, attempts]() {
        auto *box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget());
        if (!box) {
            if (attempts > 0)
                answerWith(text, seen, attempts - 1);
            return;
        }
        if (seen)
            *seen = true;
        for (QAbstractButton *b : box->buttons())
            if (b->text() == text || (text.isEmpty() && box->standardButton(b) == QMessageBox::Cancel)) {
                b->click();
                return;
            }
        box->reject();
    });
}

/// A product the way a tool registers it. @p permanent: a Workspace Export.
QString addProduct(const QString &id, bool permanent, const QStringList &dependsOn = {})
{
    AnalysisProduct p;
    p.productId = id;
    p.parentKey = kDataset;
    p.parentLabel = u"cube.fits"_s;
    p.kind = u"moment"_s;
    p.label = id;
    if (permanent)
        p.params.insert(u"Storage"_s, u"Workspace Exports"_s);
    ProductRegistry::instance().registerProduct(p);
    if (!dependsOn.isEmpty())
        ProductRegistry::instance().setDependsOn(id, dependsOn);
    return id;
}

QModelIndex rowOf(QTreeView *view, const QString &productId)
{
    QAbstractItemModel *m = view->model();
    for (int r = 0; r < m->rowCount(); ++r) {
        const QModelIndex root = m->index(r, 0);
        for (int i = 0; i < m->rowCount(root); ++i) {
            const QModelIndex idx = m->index(i, 0, root);
            if (idx.data(Qt::UserRole + 1).toString() == productId)
                return idx;
        }
    }
    return {};
}

void select(QTreeView *view, const QStringList &ids)
{
    view->selectionModel()->clearSelection();
    for (const QString &id : ids) {
        const QModelIndex idx = rowOf(view, id);
        view->selectionModel()->select(idx, QItemSelectionModel::Select | QItemSelectionModel::Rows);
        view->selectionModel()->setCurrentIndex(idx, QItemSelectionModel::NoUpdate);
    }
}

void clearRegistry()
{
    for (const AnalysisProduct &p : ProductRegistry::instance().all())
        ProductRegistry::instance().removeProduct(p.productId);
}

} // namespace

class TestSessionTreeRemoval : public QObject
{
    Q_OBJECT

private:
    SessionDataTree *m_tree = nullptr;
    QTreeView *m_view = nullptr;
    QStringList m_removed;   ///< what the handler was asked to remove, in calls

    void build()
    {
        clearRegistry();
        m_removed.clear();
        delete m_tree;
        m_tree = new SessionDataTree;
        m_tree->setCurrentDataset(kDataset, u"cube.fits"_s, u"CUBE"_s);
        m_tree->resize(300, 300);
        m_tree->show();
        m_view = m_tree->findChild<QTreeView *>();
        QVERIFY(m_view);
    }

    visivo::removal::Handler recordingHandler()
    {
        visivo::removal::Handler h;
        h.remove = [this](const QStringList &ids) { m_removed << ids.join(u","_s); };
        return h;
    }

private slots:
    void cleanup()
    {
        delete m_tree;
        m_tree = nullptr;
        clearRegistry();
    }

    void deleteKey_removesTheSelectedProductsInTreeOrder()
    {
        build();
        addProduct(u"a"_s, true);
        addProduct(u"b"_s, true);
        addProduct(u"c"_s, true);
        m_tree->setRemovalHandler(recordingHandler());

        select(m_view, { u"c"_s, u"a"_s });   // clicked in the other order
        QCOMPARE(m_tree->selectedProductIds(), (QStringList{ u"a"_s, u"c"_s }));
        QTest::keyClick(m_view, Qt::Key_Delete);
        QTRY_COMPARE(m_removed, (QStringList{ u"a,c"_s }));   // saved: nothing asked

        // What the handler left in the registry goes, and the row with it.
        QVERIFY(!ProductRegistry::instance().product(u"a"_s).productId.size());
        QVERIFY(!ProductRegistry::instance().product(u"c"_s).productId.size());
        QVERIFY(!rowOf(m_view, u"a"_s).isValid());
        QVERIFY(!rowOf(m_view, u"c"_s).isValid());
        QVERIFY(rowOf(m_view, u"b"_s).isValid());
    }

    void backspace_doesTheSame()
    {
        build();
        addProduct(u"a"_s, true);
        m_tree->setRemovalHandler(recordingHandler());
        select(m_view, { u"a"_s });
        QTest::keyClick(m_view, Qt::Key_Backspace);
        QTRY_COMPARE(m_removed, (QStringList{ u"a"_s }));
    }

    void withoutAHandler_theKeyDoesNothing()
    {
        build();
        addProduct(u"a"_s, true);
        select(m_view, { u"a"_s });
        QTest::keyClick(m_view, Qt::Key_Delete);
        QTest::qWait(50);
        QVERIFY(rowOf(m_view, u"a"_s).isValid());
    }

    void aProductNotSavedPermanently_asksAndCancelKeepsIt()
    {
        build();
        addProduct(u"m"_s, false);
        m_tree->setRemovalHandler(recordingHandler());
        select(m_view, { u"m"_s });

        bool asked = false;
        answerWith(QString(), &asked);   // Cancel
        QTest::keyClick(m_view, Qt::Key_Delete);
        QTRY_VERIFY(asked);
        QTest::qWait(50);
        QVERIFY(m_removed.isEmpty());
        QVERIFY(rowOf(m_view, u"m"_s).isValid());
    }

    void aProductNotSavedPermanently_asksAndRemoveGoes()
    {
        build();
        addProduct(u"m"_s, false);
        m_tree->setRemovalHandler(recordingHandler());
        select(m_view, { u"m"_s });

        bool asked = false;
        answerWith(u"Remove"_s, &asked);
        QTest::keyClick(m_view, Qt::Key_Delete);
        QTRY_VERIFY(asked);
        QTRY_COMPARE(m_removed, (QStringList{ u"m"_s }));
        QVERIFY(!rowOf(m_view, u"m"_s).isValid());
    }

    void whatWasMadeFromIt_isOfferedAndTheChoiceIsHonoured()
    {
        build();
        addProduct(u"model"_s, true);
        addProduct(u"resid"_s, true, { u"model"_s });
        m_tree->setRemovalHandler(recordingHandler());

        // Only the one asked for.
        select(m_view, { u"model"_s });
        bool asked = false;
        answerWith(u"Remove Only These"_s, &asked);
        QTest::keyClick(m_view, Qt::Key_Delete);
        QTRY_VERIFY(asked);
        QTRY_COMPARE(m_removed, (QStringList{ u"model"_s }));
        QVERIFY(rowOf(m_view, u"resid"_s).isValid());

        // With what was made from it. The model is back for the second try.
        m_removed.clear();
        addProduct(u"model"_s, true);
        addProduct(u"resid"_s, true, { u"model"_s });
        select(m_view, { u"model"_s });
        asked = false;
        answerWith(u"Remove With What Was Made From Them"_s, &asked);
        QTest::keyClick(m_view, Qt::Key_Delete);
        QTRY_VERIFY(asked);
        QTRY_COMPARE(m_removed, (QStringList{ u"model,resid"_s }));
        QVERIFY(!rowOf(m_view, u"resid"_s).isValid());
    }

    void theWindowCanRefuse_andTheUserIsTold()
    {
        build();
        addProduct(u"master"_s, true);
        visivo::removal::Handler h = recordingHandler();
        h.refuse = [](const AnalysisProduct &) { return u"it is the image the window opened on"_s; };
        m_tree->setRemovalHandler(h);
        select(m_view, { u"master"_s });

        bool told = false;
        answerWith(u"OK"_s, &told);
        QTest::keyClick(m_view, Qt::Key_Delete);
        QTRY_VERIFY(told);
        QVERIFY(m_removed.isEmpty());
        QVERIFY(rowOf(m_view, u"master"_s).isValid());
    }

    void aDatasetRoot_isNotAProduct()
    {
        build();
        addProduct(u"a"_s, true);
        m_tree->setRemovalHandler(recordingHandler());
        m_view->selectionModel()->clearSelection();
        m_view->selectionModel()->select(m_view->model()->index(0, 0), QItemSelectionModel::Select);
        QVERIFY(m_tree->selectedProductIds().isEmpty());
        QTest::keyClick(m_view, Qt::Key_Delete);
        QTest::qWait(50);
        QVERIFY(m_removed.isEmpty());
    }

    void menuEntry_actsOnTheSelectionWhenTheRowIsInIt()
    {
        build();
        addProduct(u"a"_s, true);
        addProduct(u"b"_s, true);
        addProduct(u"c"_s, true);
        m_tree->setRemovalHandler(recordingHandler());

        select(m_view, { u"a"_s, u"b"_s });
        QMenu inSelection;
        m_tree->appendRemoveAction(&inSelection, u"b"_s);
        QCOMPARE(inSelection.actions().size(), 1);
        QCOMPARE(inSelection.actions().first()->text(), u"Remove 2 Items from Session"_s);
        inSelection.actions().first()->trigger();
        QCOMPARE(m_removed, (QStringList{ u"a,b"_s }));

        // A row outside the selection is alone.
        build();
        addProduct(u"a"_s, true);
        addProduct(u"c"_s, true);
        m_tree->setRemovalHandler(recordingHandler());
        select(m_view, { u"a"_s });
        QMenu outside;
        m_tree->appendRemoveAction(&outside, u"c"_s);
        QCOMPARE(outside.actions().first()->text(), u"Remove from Session"_s);
        outside.actions().first()->trigger();
        QCOMPARE(m_removed, (QStringList{ u"c"_s }));
    }

    void menuEntry_isAbsentWithoutAHandlerOrForAViewRow()
    {
        build();
        addProduct(u"a"_s, true);
        QMenu none;
        m_tree->appendRemoveAction(&none, u"a"_s);
        QVERIFY(none.isEmpty());

        m_tree->setRemovalHandler(recordingHandler());
        QMenu view;
        m_tree->appendRemoveAction(&view, QString());   // an intrinsic view has no product id
        QVERIFY(view.isEmpty());
    }
};

QTEST_MAIN(TestSessionTreeRemoval)
#include "test_session_tree_removal.moc"
