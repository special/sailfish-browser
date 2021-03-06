/****************************************************************************
**
** Copyright (C) 2013 Jolla Ltd.
** Contact: Dmitry Rozhkov <dmitry.rozhkov@jollamobile.com>
**
****************************************************************************/

/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

import QtQuick 2.0
import Sailfish.Silica 1.0

Item {

    property bool selectionVisible: false

    property Item _webContent: parent
    property variant _engine: _webContent.child
    // keep selection range we get from engine to move the draggers with
    // selection together when panning or zooming
    property variant _cssRange

    anchors.fill: parent

    function onViewAreaChanged() {
        var newOffset = _engine.scrollableOffset
        var resolution = _engine.resolution

        var diffX = newOffset.x - _cssRange.origOffsetX
        var diffY = newOffset.y - _cssRange.origOffsetY

        start.x = (_cssRange.startX - diffX) * resolution - start.width
        start.y = (_cssRange.startY - diffY) * resolution
        end.x = (_cssRange.endX - diffX) * resolution
        end.y = (_cssRange.endY - diffY) * resolution

        timer.restart()
    }

    function onSelectionRangeUpdated(data) {
        if (!data.updateStart) {
            return
        }

        var resolution = _engine.resolution

        start.x = (data.start.xPos * resolution) - start.width
        start.y = data.start.yPos * resolution
        end.x = data.end.xPos * resolution
        end.y = data.end.yPos * resolution

        _cssRange = {
            "startX": data.start.xPos,
            "startY": data.start.yPos,
            "endX": data.end.xPos,
            "endY": data.end.yPos,
            "origOffsetX": _engine.scrollableOffset.x,
            "origOffsetY": _engine.scrollableOffset.y,
        }

        selectionVisible = true
    }

    function onSelectionCopied(data) {
        selectionVisible = false
        _cssRange = null
        _engine.sendAsyncMessage("Browser:SelectionClose",
                                 {
                                     "clearSelection": true
                                 })
    }

    function onContextMenuRequested(data) {
        if (data.types.indexOf("content-text") !== -1) {
            // we want to select some content text
            _engine.sendAsyncMessage("Browser:SelectionStart", {"xPos": data.xPos, "yPos": data.yPos})
        }
    }

    onSelectionVisibleChanged: {
        if (selectionVisible) {
            _engine.viewAreaChanged.connect(onViewAreaChanged)
        } else {
            _engine.viewAreaChanged.disconnect(onViewAreaChanged)
        }
    }

    TextSelectionHandle {
        id: start

        type: "start"
        content: _webContent
        visible: selectionVisible
    }

    TextSelectionHandle {
        id: end

        type: "end"
        content: _webContent
        visible: selectionVisible
    }

    Component.onCompleted: {
        _webContent.selectionRangeUpdated.connect(onSelectionRangeUpdated)
        _webContent.selectionCopied.connect(onSelectionCopied)
        _webContent.contextMenuRequested.connect(onContextMenuRequested)
    }

    Timer {
        id: timer

        interval: 100

        onTriggered: {
            if (selectionVisible) {
                _engine.sendAsyncMessage("Browser:SelectionUpdate", {})
            }
        }
    }
}
