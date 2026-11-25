import QtQuick
import QtQuick.Window
import QtTest
import Veyra

TestCase {
    name: "FullscreenPageAttachment"
    when: windowShown
    Window {
        id: player
        width: 640
        height: 360
        visible: true
        property bool fullscreen: false
        property string shownPage: "pro"
        property string leavingPage: ""
        property real leaveT: 0
        Item {
            id: pages
            anchors.fill: parent
            VPage {
                id: pro
                home: pages
                pageId: "pro"
                property int selectedEffect: 4
            }
        }
    }
    function test_hideAndRestore() {
        compare(pro.parent, pages)
        verify(pro.visible)
        pages.visible = false
        tryCompare(pro, "parent", null)
        verify(!pro.visible)
        compare(pro.shell, player)
        compare(pro.width, 640)
        pages.visible = true
        tryCompare(pro, "parent", pages)
        verify(pro.visible)
        compare(pro.selectedEffect, 4)
        player.leavingPage = "pro"
        player.shownPage = "home"
        compare(pro.parent, pages)
        player.leavingPage = ""
        tryCompare(pro, "parent", null)
    }
}
