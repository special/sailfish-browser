/****************************************************************************
**
** Copyright (C) 2014 Jolla Ltd.
** Contact: Raine Makelainen <raine.makelainen@jolla.com>
**
****************************************************************************/

/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */


import QtQuick 2.0
import Sailfish.Silica 1.0
import Sailfish.Browser 1.0

ApplicationWindow {
    id: window

    property alias webView: webView

    allowedOrientations: Orientation.Portrait
    _defaultPageOrientations: allowedOrientations

    initialPage: Page {
        WebView {
            id: webView

            active: true
            toolbarHeight: 50
            portrait: true

            // Mimic onOpenUrlRequested handler of BrowserPage
            Component.onCompleted: tabModel.newTab(WebUtils.homePage, "")
        }
    }

    cover: undefined
}

