/****************************************************************************
**
** Copyright (C) 2013 Jolla Ltd.
** Contact: Raine Makelainen <raine.makelainen@jollamobile.com>
**
****************************************************************************/

/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "declarativewebcontainer.h"
#include "declarativetabmodel.h"
#include "declarativewebpage.h"
#include "dbmanager.h"
#include "downloadmanager.h"
#include "declarativewebutils.h"

#include <QPointer>
#include <QTimerEvent>
#include <QQuickWindow>
#include <QDir>
#include <QTransform>
#include <QStandardPaths>
#include <QtConcurrentRun>
#include <QGuiApplication>
#include <QScreen>
#include <QMetaMethod>

#include <qmozcontext.h>
#include <QGuiApplication>

DeclarativeWebContainer::DeclarativeWebContainer(QQuickItem *parent)
    : QQuickItem(parent)
    , m_webPage(0)
    , m_model(0)
    , m_webPageComponent(0)
    , m_foreground(true)
    , m_background(false)
    , m_windowVisible(false)
    , m_backgroundTimer(0)
    , m_active(false)
    , m_popupActive(false)
    , m_portrait(true)
    , m_fullScreenMode(false)
    , m_inputPanelVisible(false)
    , m_inputPanelHeight(0.0)
    , m_inputPanelOpenHeight(0.0)
    , m_toolbarHeight(0.0)
    , m_loading(false)
    , m_loadProgress(0)
    , m_canGoForward(false)
    , m_canGoBack(false)
    , m_realNavigation(false)
    , m_readyToLoad(false)
    , m_maxLiveTabCount(5)
    , m_deferredReload(false)
{
    m_webPages.reset(new WebPages(this));
    setFlag(QQuickItem::ItemHasContents, true);
    if (!window()) {
        connect(this, SIGNAL(windowChanged(QQuickWindow*)), this, SLOT(handleWindowChanged(QQuickWindow*)));
    } else {
        connect(window(), SIGNAL(visibleChanged(bool)), this, SLOT(windowVisibleChanged(bool)));
    }

    connect(&m_screenCapturer, SIGNAL(finished()), this, SLOT(screenCaptureReady()));
    connect(DownloadManager::instance(), SIGNAL(downloadStarted()), this, SLOT(onDownloadStarted()));
    connect(DBManager::instance(), SIGNAL(thumbPathChanged(QString,QString,int)),
            this, SLOT(onPageThumbnailChanged(QString,QString,int)));
    connect(this, SIGNAL(maxLiveTabCountChanged()), this, SLOT(manageMaxTabCount()));
    connect(this, SIGNAL(_readyToLoadChanged()), this, SLOT(onReadyToLoad()));
    connect(this, SIGNAL(heightChanged()), this, SLOT(resetHeight()));

    QString cacheLocation = QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
    QDir dir(cacheLocation);
    if(!dir.exists() && !dir.mkpath(cacheLocation)) {
        qWarning() << "Can't create directory "+ cacheLocation;
        return;
    }

    connect(this, SIGNAL(heightChanged()), this, SLOT(sendVkbOpenCompositionMetrics()));
    connect(this, SIGNAL(widthChanged()), this, SLOT(sendVkbOpenCompositionMetrics()));
}

DeclarativeWebContainer::~DeclarativeWebContainer()
{
    // Disconnect all signal slot connections
    if (m_webPage) {
        disconnect(m_webPage, 0, 0, 0);
    }

    m_screenCapturer.cancel();
    m_screenCapturer.waitForFinished();
}

DeclarativeWebPage *DeclarativeWebContainer::webPage() const
{
    return m_webPage;
}

void DeclarativeWebContainer::setWebPage(DeclarativeWebPage *webPage)
{
    if (m_webPage != webPage) {
        m_webPage = webPage;
        emit contentItemChanged();
        emit titleChanged();
        emit urlChanged();
    }
}

DeclarativeTabModel *DeclarativeWebContainer::tabModel() const
{
    return m_model;
}

void DeclarativeWebContainer::setTabModel(DeclarativeTabModel *model)
{
    if (m_model != model) {
        if (m_model) {
            disconnect(m_model);
        }

        m_model = model;
        if (m_model) {
            connect(m_model, SIGNAL(activeTabChanged(int,int)), this, SLOT(onActiveTabChanged(int,int)));
            connect(m_model, SIGNAL(loadedChanged()), this, SLOT(onModelLoaded()));
            connect(m_model, SIGNAL(tabAdded(int)), this, SLOT(manageMaxTabCount()));
            connect(m_model, SIGNAL(tabClosed(int)), this, SLOT(releasePage(int)));
            // Try to make this to normal direct connection once we have removed QML_BAD_GUI_RENDER_LOOP.
            connect(m_model, SIGNAL(newTabRequested(QString,QString,int)), this, SLOT(onNewTabRequested(QString,QString,int)), Qt::QueuedConnection);
            connect(m_model, SIGNAL(updateActiveThumbnail()), this, SLOT(updateThumbnail()));
        }
        emit tabModelChanged();
    }
}

bool DeclarativeWebContainer::foreground() const
{
    return m_foreground;
}

void DeclarativeWebContainer::setForeground(bool active)
{
    if (m_foreground != active) {
        m_foreground = active;

        if (!m_foreground) {
            // Respect content height when browser brought back from home
            resetHeight(true);
        }
        emit foregroundChanged();
    }
}

bool DeclarativeWebContainer::background() const
{
    return m_background;
}

int DeclarativeWebContainer::loadProgress() const
{
    return m_loadProgress;
}

void DeclarativeWebContainer::setLoadProgress(int loadProgress)
{
    if (m_loadProgress != loadProgress) {
        m_loadProgress = loadProgress;
        emit loadProgressChanged();
    }
}

bool DeclarativeWebContainer::active() const
{
    return m_active;
}

void DeclarativeWebContainer::setActive(bool active)
{
    if (m_active != active) {
        m_active = active;
        emit activeChanged();

        // If dialog has been opened, we need to verify that input panel is not visible.
        // This might happen when the user fills in login details to a form and
        // presses enter to accept the form after which PasswordManagerDialog is pushed to pagestack
        // on top the BrowserPage. Once PassowordManagerDialog is accepted/rejected
        // this condition can be met. If active changes to true before keyboard is fully closed,
        // then the inputPanelVisibleChanged() signal is emitted by setInputPanelHeight.
        if (m_active && m_inputPanelHeight == 0 && m_inputPanelVisible) {
            m_inputPanelVisible = false;
            emit inputPanelVisibleChanged();
        }
    }
}

bool DeclarativeWebContainer::inputPanelVisible() const
{
    return m_inputPanelVisible;
}

qreal DeclarativeWebContainer::inputPanelHeight() const
{
    return m_inputPanelHeight;
}

void DeclarativeWebContainer::setInputPanelHeight(qreal height)
{
    if (m_inputPanelHeight != height) {
        bool imVisibleChanged = false;
        m_inputPanelHeight = height;
        if (m_active) {
            if (m_inputPanelHeight == 0) {
                if (m_inputPanelVisible) {
                    m_inputPanelVisible = false;
                    imVisibleChanged = true;
                }
            } else if (m_inputPanelHeight == m_inputPanelOpenHeight) {
                if (!m_inputPanelVisible) {
                    m_inputPanelVisible = true;
                    imVisibleChanged = true;
                }
            }
        }

        if (imVisibleChanged) {
            emit inputPanelVisibleChanged();
        }

        emit inputPanelHeightChanged();
    }
}

bool DeclarativeWebContainer::canGoForward() const
{
    return m_canGoForward;
}

bool DeclarativeWebContainer::canGoBack() const
{
    return m_canGoBack;
}

QString DeclarativeWebContainer::title() const
{
    return m_webPage ? m_webPage->title() : "";
}

QString DeclarativeWebContainer::url() const
{
    return m_webPage ? m_webPage->url().toString() : "";
}

QString DeclarativeWebContainer::thumbnailPath() const
{
    return m_thumbnailPath;
}

void DeclarativeWebContainer::setThumbnailPath(QString thumbnailPath)
{
    if (m_thumbnailPath != thumbnailPath) {
        m_thumbnailPath = thumbnailPath;
        emit thumbnailPathChanged();
    }
}

bool DeclarativeWebContainer::readyToLoad() const
{
    return m_readyToLoad;
}

void DeclarativeWebContainer::setReadyToLoad(bool readyToLoad)
{
    if (m_readyToLoad != readyToLoad) {
        m_readyToLoad = readyToLoad;
        emit _readyToLoadChanged();
    }
}

bool DeclarativeWebContainer::isActiveTab(int tabId)
{
    return m_webPage && m_webPage->tabId() == tabId;
}

void DeclarativeWebContainer::goForward()
{
    if (m_canGoForward && m_webPage) {
        m_canGoForward = false;
        emit canGoForwardChanged();

        if (!m_canGoBack) {
            m_canGoBack = true;
            emit canGoBackChanged();
        }

        m_model->setBackForwardNavigation(true);
        DBManager::instance()->goForward(m_webPage->tabId());

        if (m_webPage->canGoForward()) {
            m_realNavigation = true;
            m_webPage->goForward();
        } else {
            m_realNavigation = false;
        }
    }
}

void DeclarativeWebContainer::goBack()
{
    if (m_canGoBack && m_webPage) {
        m_canGoBack = false;
        emit canGoBackChanged();

        if (!m_canGoForward) {
            m_canGoForward = true;
            emit canGoForwardChanged();
        }

        m_model->setBackForwardNavigation(true);
        DBManager::instance()->goBack(m_webPage->tabId());

        if (m_webPage->canGoBack()) {
            m_realNavigation = true;
            m_webPage->goBack();
        } else {
            m_realNavigation = false;
        }
    }
}

bool DeclarativeWebContainer::activatePage(int tabId, bool force)
{
    if (!m_model) {
        return false;
    }

    m_webPages->initialize(this, m_webPageComponent.data());
    if ((m_model->loaded() || force) && tabId > 0 && m_webPages->initialized()) {
        WebPageActivationData activationData = m_webPages->page(tabId, m_model->newTabParentId());
        setWebPage(activationData.webPage);
        m_webPage->setChrome(true);
        setLoadProgress(m_webPage->loadProgress());

        connect(m_webPage, SIGNAL(imeNotification(int,bool,int,int,QString)),
                this, SLOT(imeNotificationChanged(int,bool,int,int,QString)), Qt::UniqueConnection);
        connect(m_webPage, SIGNAL(contentHeightChanged()), this, SLOT(resetHeight()), Qt::UniqueConnection);
        connect(m_webPage, SIGNAL(scrollableOffsetChanged()), this, SLOT(resetHeight()), Qt::UniqueConnection);
        connect(m_webPage, SIGNAL(windowCloseRequested()), this, SLOT(closeWindow()), Qt::UniqueConnection);
        connect(m_webPage, SIGNAL(urlChanged()), this, SLOT(onPageUrlChanged()), Qt::UniqueConnection);
        connect(m_webPage, SIGNAL(titleChanged()), this, SLOT(onPageTitleChanged()), Qt::UniqueConnection);
        connect(m_webPage, SIGNAL(domContentLoadedChanged()), this, SLOT(sendVkbOpenCompositionMetrics()), Qt::UniqueConnection);
        return activationData.activated;
    }
    return false;
}

void DeclarativeWebContainer::captureScreen()
{
    if (!m_webPage) {
        return;
    }

    if (m_active && m_webPage->domContentLoaded() && !m_popupActive) {
        int size = QGuiApplication::primaryScreen()->size().width();
        if (!m_portrait && !m_fullScreenMode) {
            size -= m_toolbarHeight;
        }

        qreal rotation = parentItem() ? parentItem()->rotation() : 0;
        captureScreen(url(), size, rotation);
    }
}

void DeclarativeWebContainer::dumpPages() const
{
    m_webPages->dumpPages();
}

void DeclarativeWebContainer::resetHeight(bool respectContentHeight)
{
    if (!m_webPage || !m_webPage->state().isEmpty()) {
        return;
    }

    qreal fullHeight = height();

    // Application active
    if (respectContentHeight) {
        // Handle webPage height over here, BrowserPage.qml loading
        // reset might be redundant as we have also loaded trigger
        // reset. However, I'd leave it there for safety reasons.
        // We need to reset height always back to short height when loading starts
        // so that after tab change there is always initial short composited height.
        // Height may expand when content is moved.
        if (contentHeight() > (fullHeight + m_toolbarHeight) || m_webPage->fullscreen()) {
            m_webPage->setHeight(fullHeight);
        } else {
            m_webPage->setHeight(fullHeight - m_toolbarHeight);
        }
    } else {
        m_webPage->setHeight(fullHeight - m_toolbarHeight);
    }
}

void DeclarativeWebContainer::imeNotificationChanged(int state, bool open, int cause, int focusChange, const QString &type)
{
    Q_UNUSED(open)
    Q_UNUSED(cause)
    Q_UNUSED(focusChange)
    Q_UNUSED(type)

    // QmlMozView's input context open is actually intention (0 closed, 1 opened).
    // cause 3 equals InputContextAction::CAUSE_MOUSE nsIWidget.h
    if (state == 1 && cause == 3) {
        // For safety reset height based on contentHeight before going to "boundHeightControl" state
        // so that when vkb is closed we get correctly reset height back.
        resetHeight(true);
        if (!m_inputPanelVisible) {
            m_inputPanelVisible = true;
            emit inputPanelVisibleChanged();
        }
    }
}

qreal DeclarativeWebContainer::contentHeight() const
{
    if (m_webPage) {
        return m_webPage->contentHeight();
    } else {
        return 0.0;
    }
}

DeclarativeWebContainer::ScreenCapture DeclarativeWebContainer::saveToFile(QString url, QImage image, QRect cropBounds, int tabId, qreal rotate)
{
    QString path = QString("%1/tab-%2-thumb.png").arg(QStandardPaths::writableLocation(QStandardPaths::CacheLocation)).arg(tabId);
    QTransform transform;
    transform.rotate(360 - rotate);
    image = image.transformed(transform);
    image = image.copy(cropBounds);

    ScreenCapture capture;
    if(image.save(path)) {
        capture.tabId = tabId;
        capture.path = path;
        capture.url = url;
    } else {
        capture.tabId = -1;
        qWarning() << Q_FUNC_INFO << "failed to save image" << path;
    }
    return capture;
}

void DeclarativeWebContainer::timerEvent(QTimerEvent *event)
{
    if (m_backgroundTimer == event->timerId()) {
        if (window()) {
            // Guard window visibility change was not cancelled after timer triggered.
            bool tmpVisible = window()->isVisible();
            // m_windowVisible == m_background visibility changed
            if (tmpVisible == m_windowVisible && m_windowVisible == m_background) {
                m_background = !m_windowVisible;
                emit backgroundChanged();
            }
        }
        killTimer(m_backgroundTimer);
    }
}

void DeclarativeWebContainer::windowVisibleChanged(bool visible)
{
    Q_UNUSED(visible);
    if (window()) {
        m_windowVisible = window()->isVisible();
        m_backgroundTimer = startTimer(1000);
    }
}

void DeclarativeWebContainer::handleWindowChanged(QQuickWindow *window)
{
    if (window) {
        connect(window, SIGNAL(visibleChanged(bool)), this, SLOT(windowVisibleChanged(bool)));
    }
}

void DeclarativeWebContainer::onActiveTabChanged(int oldTabId, int activeTabId)
{
    if (activeTabId == 0) {
        setThumbnailPath("");
        return;
    }
    const Tab &tab = m_model->activeTab();
#ifdef DEBUG_LOGS
    qDebug() << "canGoBack = " << m_canGoBack << "canGoForward = " << m_canGoForward << &tab;
#endif

    if (m_canGoForward != (tab.nextLink() > 0)) {
        m_canGoForward = tab.nextLink() > 0;
        emit canGoForwardChanged();
    }

    if (m_canGoBack != (tab.previousLink() > 0)) {
        m_canGoBack = tab.previousLink() > 0;
        emit canGoBackChanged();
    }

    setThumbnailPath(tab.thumbnailPath());

    // Switch to different tab.
    if (oldTabId != activeTabId) {
        if (m_model->hasNewTabData()) {
            return;
        }

        QString tabUrl = tab.url();

        if (activatePage(activeTabId, true) && m_readyToLoad
                && (m_webPage->tabId() != activeTabId || m_webPage->url().toString() != tabUrl)) {
            emit triggerLoad(tabUrl, tab.title());
        }
        manageMaxTabCount();
    }
}

void DeclarativeWebContainer::screenCaptureReady()
{
    ScreenCapture capture = m_screenCapturer.result();
#ifdef DEBUG_LOGS
    qDebug() << capture.tabId << capture.path << capture.url;
#endif
    if (capture.tabId != -1) {
        // Update immediately without dbworker round trip.
        if (isActiveTab(capture.tabId)) {
            setThumbnailPath(capture.path);
        }
        // TODO: Cleanup url.
        DBManager::instance()->updateThumbPath(capture.url, capture.path, capture.tabId);
    }
}

void DeclarativeWebContainer::triggerLoad()
{
    bool realNavigation = m_realNavigation;
    m_realNavigation = false;
    // Back / forward navigation activated and MozView instance cannot be used.
    if (m_webPage && m_model->backForwardNavigation() && !realNavigation && url() != "about:blank") {
        QMetaObject::invokeMethod(this, "load", Qt::DirectConnection,
                                  Q_ARG(QVariant, url()),
                                  Q_ARG(QVariant, title()),
                                  Q_ARG(QVariant, false));
    }
}

void DeclarativeWebContainer::onModelLoaded()
{
    // Load placeholder when no pages exist. If a page exists,
    // it gets initialized by onActiveTabChanged.
    if (!webPage()) {
        activatePage(m_model->nextTabId(), true);
    }
}

void DeclarativeWebContainer::onDownloadStarted()
{
    // This is not 100% solid. A new tab is created for every incoming
    // url. In slow network connectivity one can close previous tab or
    // create a new tab before downloadStarted is emitted
    // by DownloadManager. To get this to the 100%, we would need to
    // pass windowId of the active window when download is started and close
    // the passed windowId instead.
    if (m_model && m_model->hasNewTabData() && m_model->count() > 0 && m_webPage) {
        DeclarativeWebPage *previousWebPage = qobject_cast<DeclarativeWebPage *>(m_model->newTabPreviousPage());
        releasePage(m_webPage->tabId());
        if (previousWebPage) {
            activatePage(previousWebPage->tabId());
        } else if (m_model->count() == 0) {
            // Download doesn't add tab to model. Mimic
            // model change in case tabs count goes to zero.
            emit m_model->countChanged();
        }
    }
}

void DeclarativeWebContainer::onNewTabRequested(QString url, QString title, int parentId)
{
    if (m_active) {
        m_model->newTabData(url, title, webPage(), parentId);
        // This could handle new page creation directly if/
        // when connection helper is accessible from QML.
        emit triggerLoad(url, title);
    } else {
        if (m_webPage) {
            m_webPage->setVisible(false);
            setWebPage(0);
        }
        QMetaObject::invokeMethod(this, "onNewTabRequested", Qt::QueuedConnection,
                                  Q_ARG(QString, url),
                                  Q_ARG(QString, title),
                                  Q_ARG(int, parentId));
    }
}

void DeclarativeWebContainer::onReadyToLoad()
{
    // Triggered when tabs of tab model are available and QmlMozView is ready to load.
    // Load test
    // 1) tabModel.hasNewTabData -> loadTab (already activated view)
    // 2) model has tabs, load active tab -> load (activate view when needed)
    // 3) load home page -> load (activate view when needed)

    // visible could be possible delay property for _readyToLoad if so wanted.
    if (!m_readyToLoad || !m_model) {
        return;
    }

    if (m_model->hasNewTabData()) {
        m_webPage->loadTab(m_model->newTabUrl(), false);
    } else if (m_model->count() > 0) {
        // First tab is actived when tabs are loaded to the tabs tabModel.
        m_model->resetNewTabData();
        const Tab &tab = m_model->activeTab();
        emit triggerLoad(tab.url(), tab.title());
    } else {
        // This can happen only during startup.
        emit triggerLoad(DeclarativeWebUtils::instance()->homePage(), "");
    }
}

/**
 * @brief DeclarativeTab::captureScreen
 * Rotation transformation is applied first, then geometry values on top of it.
 * @param url
 * @param size
 * @param rotate clockwise rotation of the image in degrees
 */
void DeclarativeWebContainer::captureScreen(QString url, int size, qreal rotate)
{
    if (!window() || !window()->isActive() || !m_webPage) {
        return;
    }

    // Cleanup old thumb.
    setThumbnailPath("");

    QImage image = window()->grabWindow();
    QRect cropBounds(0, 0, size, size);

#ifdef DEBUG_LOGS
    qDebug() << "about to set future";
#endif
    // asynchronous save to avoid the slow I/O
    m_screenCapturer.setFuture(QtConcurrent::run(this, &DeclarativeWebContainer::saveToFile, url, image, cropBounds, m_webPage->tabId(), rotate));
}

int DeclarativeWebContainer::parentTabId(int tabId) const
{
    if (m_webPages) {
        return m_webPages->parentTabId(tabId);
    }
    return 0;
}

void DeclarativeWebContainer::releasePage(int tabId, bool virtualize)
{
    if (m_webPages) {
        m_webPages->release(tabId, virtualize);
        // Successfully destroyed. Emit relevant property changes.
        if (!m_webPage) {
            emit contentItemChanged();
            emit titleChanged();
            emit urlChanged();
            setThumbnailPath("");
        }
        m_model->resetNewTabData();
    }
}

void DeclarativeWebContainer::closeWindow()
{
    DeclarativeWebPage *webPage = qobject_cast<DeclarativeWebPage *>(sender());
    if (webPage && m_model) {
        int parentPageTabId = parentTabId(webPage->tabId());
        // Closing only allowed if window was created by script
        if (parentPageTabId > 0) {
            m_model->activateTabById(parentPageTabId);
            m_model->removeTabById(webPage->tabId(), isActiveTab(webPage->tabId()));
        }
    }
}

void DeclarativeWebContainer::onPageUrlChanged()
{
    DeclarativeWebPage *webPage = qobject_cast<DeclarativeWebPage *>(sender());
    if (webPage && m_model) {
        QString url = webPage->url().toString();
        int tabId = webPage->tabId();
        bool activeTab = isActiveTab(tabId);
        m_model->updateUrl(tabId, activeTab, url);

        if (activeTab && webPage == m_webPage) {
            emit urlChanged();
        }
    }
}

void DeclarativeWebContainer::onPageTitleChanged()
{
    DeclarativeWebPage *webPage = qobject_cast<DeclarativeWebPage *>(sender());
    if (webPage && m_model) {
        QString title = webPage->title();
        int tabId = webPage->tabId();
        bool activeTab = isActiveTab(tabId);
        m_model->updateTitle(tabId, activeTab, title);

        if (activeTab && webPage == m_webPage) {
            emit titleChanged();
        }
    }
}

void DeclarativeWebContainer::onPageThumbnailChanged(QString url, QString path, int tabId)
{
    Q_UNUSED(url);
    if (isActiveTab(tabId)) {
        setThumbnailPath(path);
    }

    if (m_model) {
        m_model->updateThumbnailPath(tabId, isActiveTab(tabId), path);
    }
}

void DeclarativeWebContainer::updateThumbnail()
{
    const Tab &tab = m_model->activeTab();
    if (isActiveTab(tab.tabId()) && tab.isValid()) {
        captureScreen();
    }
}

void DeclarativeWebContainer::manageMaxTabCount()
{
    // Minimum is 1 tab.
    if (m_maxLiveTabCount < 1 || !m_model) {
        return;
    }

    const QList<Tab> &tabs = m_model->tabs();

    // ActiveTab + m_maxLiveTabCount -1 == m_maxLiveTabCount
    for (int i = m_maxLiveTabCount - 1; i < tabs.count() && m_webPages && m_webPages->count() > m_maxLiveTabCount; ++i) {
        releasePage(tabs.at(i).tabId(), true);
    }
}

void DeclarativeWebContainer::updateVkbHeight()
{
    qreal vkbHeight = 0;
    // Keyboard rect is updated too late, when vkb hidden we cannot yet get size.
    // We need to send correct information to embedlite-components before virtual keyboard is open
    // so that when input element is focused contect is zoomed to the correct target (available area).
#if 0
    if (qGuiApp->inputMethod()) {
        vkbHeight = qGuiApp->inputMethod()->keyboardRectangle().height();
    }
#else
    // TODO: remove once keyboard height is not zero when hidden and take above #if 0 block into use.
    vkbHeight = 440;
    if (width() > height()) {
        vkbHeight = 340;
    }
#endif
    m_inputPanelOpenHeight = vkbHeight;
}

void DeclarativeWebContainer::sendVkbOpenCompositionMetrics()
{
    updateVkbHeight();

    QVariantMap map;

    // Round values to even numbers.
    int vkbOpenCompositionHeight = height() - m_inputPanelOpenHeight;
    int vkbOpenMaxCssCompositionWidth = width() / QMozContext::GetInstance()->pixelRatio();
    int vkbOpenMaxCssCompositionHeight = vkbOpenCompositionHeight / QMozContext::GetInstance()->pixelRatio();

    map.insert("compositionHeight", vkbOpenCompositionHeight);
    map.insert("maxCssCompositionWidth", vkbOpenMaxCssCompositionWidth);
    map.insert("maxCssCompositionHeight", vkbOpenMaxCssCompositionHeight);

    QVariant data(map);

    if (m_webPage) {
        m_webPage->sendAsyncMessage("embedui:vkbOpenCompositionMetrics", data);
    }
}
