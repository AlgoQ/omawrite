import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Dialogs as Dialogs
import QtQuick.Layouts
import QtQuick.Window
import "EditorMutations.js" as EditorMutations

ApplicationWindow {
    id: win
    width: 1280
    height: 820
    minimumWidth: 720
    minimumHeight: 520
    visible: true
    title: (backend.modified ? "* " : "") + backend.fileName + " - Omawrite"

    readonly property bool darkMode: backend.darkMode
    readonly property color pageColor: backend.themeBackground
    readonly property color textColor: backend.themeForeground
    readonly property color mutedColor: darkMode ? "#909191" : "#aeb1b5"
    readonly property color selectionFill: backend.themeSelection
    readonly property color selectionTextColor: backend.themeSelectionForeground
    // The desktop's text size knob (GNOME's text-scaling-factor, which
    // `omarchy display text size` drives) anchored so its 12px default leaves
    // the app at the sizes it was designed around, times the user's own
    // text size set with Ctrl+= / Ctrl+- / Ctrl+wheel.
    readonly property real textScale: backend.textScale
    readonly property int editorFontPixelSize: scaledSize(20)
    readonly property int footerHeight: scaledSize(32)
    // Never wider than the Flickable's viewport, whatever the floor asks for:
    // a tiling compositor can resize the window below its minimum width.
    // The side panels take room from the page rather than covering it.
    readonly property int sidebarWidth: sidebarOpen ? Math.min(scaledSize(280), Math.round(width * 0.3)) : 0
    readonly property int backlinksWidth: backlinksOpen ? Math.min(scaledSize(380), Math.round(width * 0.45)) : 0
    readonly property int pageWidth: width - sidebarWidth - backlinksWidth
    readonly property int editorWidth: Math.min(
        Math.round(writerFontMetrics.averageCharacterWidth * 65),
        Math.max(360, pageWidth - Math.round(writerFontMetrics.averageCharacterWidth * 20)),
        Math.max(0, pageWidth - 48))
    property bool closeConfirmed: false
    property bool searchOpen: false
    property bool searchUpdating: false
    property var searchMatches: []
    property int searchMatchIndex: -1
    property url pendingOpenUrl
    property string pendingAction: ""
    property bool replaceOpen: false
    property bool awaitingPendingSave: false
    // The picture viewer's shot of the note's images, in document order.
    property var viewerImages: []
    property int viewerIndex: -1

    Material.theme: darkMode ? Material.Dark : Material.Light
    Material.accent: backend.themeAccent
    color: pageColor

    onClosing: function(close) {
        if (closeConfirmed || !backend.modified)
            return;

        close.accepted = false;
        pendingAction = "close";
        if (!unsavedChangesDialog.opened)
            unsavedChangesDialog.open();
    }

    function requestOpen(url) {
        if (!backend.modified) {
            backend.open(url);
            return;
        }
        pendingOpenUrl = url;
        pendingAction = "open";
        unsavedChangesDialog.open();
    }

    function completePendingAction() {
        var action = pendingAction;
        pendingAction = "";
        if (action === "close") {
            closeConfirmed = true;
            close();
        } else if (action === "open") {
            backend.open(pendingOpenUrl);
        }
    }

    function followLinkAt(position) {
        var image = backend.imageUrlAt(position);
        if (image.toString().length > 0) {
            backend.openImage(image);
            return true;
        }

        var wiki = backend.wikiLinkAt(position);
        if (wiki.kind === "file") {
            openInTab(wiki.url);
            return true;
        }
        if (wiki.kind === "missing") {
            backend.reportMissingNote(wiki.target);
            return false;
        }

        var url = backend.linkUrlAt(position);
        if (url.length > 0) {
            backend.openExternalUrl(url);
            return true;
        }
        return false;
    }

    // The viewer is handed a fresh list of the note's pictures and the one
    // whose block was clicked, so an embed moved by typing still opens the
    // picture under the pointer.
    function openImageViewer(blockStart) {
        var images = backend.imageEmbeds();
        for (var i = 0; i < images.length; ++i) {
            if (images[i].blockStart === blockStart) {
                viewerImages = images;
                viewerIndex = i;
                imageViewer.open();
                return;
            }
        }
    }

    function stepImageViewer(direction) {
        var next = viewerIndex + direction;
        if (next < 0 || next >= viewerImages.length)
            return;
        viewerIndex = next;
    }

    function closeImageViewer() {
        // Close on the picture that was last shown, leaving its raw embed
        // under the caret so it can be edited or deleted straight away.
        if (viewerIndex >= 0 && viewerIndex < viewerImages.length)
            editor.cursorPosition = viewerImages[viewerIndex].blockEnd;
        imageViewer.close();
        editor.forceActiveFocus();
    }

    // The unfinished [[link]] around the caret, if there is one. A newline, a
    // closing bracket, an alias or a heading ends the search. Only the tail
    // near the caret is read, so this stays cheap in a long document.
    function wikiContext() {
        var from = Math.max(0, editor.cursorPosition - 256);
        var tail = editor.getText(from, editor.cursorPosition);
        var open = tail.lastIndexOf("[[");
        if (open < 0)
            return null;
        var query = tail.slice(open + 2);
        if (query.indexOf("]]") >= 0 || query.indexOf("\n") >= 0
                || query.indexOf("[[") >= 0 || query.indexOf("|") >= 0
                || query.indexOf("#") >= 0 || backend.insideFence(editor.cursorPosition))
            return null;
        return { start: from + open, query: query };
    }

    function closeWikiPopup() {
        wikiPopup.visible = false;
        wikiPopup.suggestions = [];
        wikiPopup.currentIndex = 0;
        wikiPopup.query = "";
    }

    function refreshWikiPopup() {
        var context = wikiContext();
        if (!context || !editor.activeFocus) {
            closeWikiPopup();
            return;
        }

        if (!wikiPopup.visible)
            backend.refreshNoteIndex();
        var suggestions = backend.noteSuggestions(context.query);
        if (suggestions.length === 0) {
            closeWikiPopup();
            return;
        }

        if (wikiPopup.query !== context.query) {
            wikiPopup.query = context.query;
            wikiPopup.currentIndex = 0;
        }
        wikiPopup.replaceStart = context.start;
        wikiPopup.suggestions = suggestions;
        wikiPopup.visible = true;
        positionWikiPopup();
    }

    function positionWikiPopup() {
        if (!wikiPopup.visible)
            return;
        var rect = editor.cursorRectangle;
        var point = editor.mapToItem(rootItem, rect.x, rect.y + rect.height + 4);
        var popupWidth = Math.min(scaledSize(420), width - 48);
        var popupHeight = wikiPopup.height;
        var x = Math.max(24, Math.min(point.x, width - popupWidth - 24));
        var y = point.y;
        if (y + popupHeight > height - footerHeight - 8) {
            var above = editor.mapToItem(rootItem, rect.x, rect.y).y;
            y = Math.max(8, above - popupHeight - 4);
        }
        wikiPopup.x = x;
        wikiPopup.y = y;
        wikiPopup.width = popupWidth;
    }

    function acceptWikiSuggestion(index) {
        if (!wikiPopup.visible || index < 0 || index >= wikiPopup.suggestions.length)
            return;
        var context = wikiContext();
        if (!context) {
            closeWikiPopup();
            return;
        }

        var start = context.start;
        var end = editor.cursorPosition;
        if (editor.text.slice(end, end + 2) === "]]")
            end += 2;
        var replacement = "[[" + wikiPopup.suggestions[index].insert + "]]";
        backend.replaceRange(start, end, replacement);
        editor.cursorPosition = start + replacement.length;
        closeWikiPopup();
        editor.forceActiveFocus();
    }

    // The file viewer: a VirtFolder-style link tree of the vault ("Links",
    // nested by each note's up: field) or a folder tree of the disk
    // ("Folders"). Rows are rebuilt from disk each time something expands,
    // the mode changes, or the note changes.
    property bool sidebarOpen: false
    property var expandedFolders: ({})
    property var linksExpanded: ({})
    property var treeRows: []

    function rebuildTree() {
        if (!sidebarOpen)
            return;
        if (backend.treeMode === "folders")
            rebuildFolderTree();
        else
            rebuildLinkTree();
        highlightCurrentInTree();
    }

    function rebuildFolderTree() {
        var rows = [];
        var walk = function(relative, depth) {
            var entries = backend.listFolder(relative);
            for (var i = 0; i < entries.length; ++i) {
                var entry = entries[i];
                rows.push({ name: entry.name, path: entry.path, url: entry.url,
                            isDir: entry.isDir, hasChildren: entry.isDir,
                            childCount: 0, depth: depth,
                            expanded: entry.isDir && expandedFolders[entry.path] === true });
                if (entry.isDir && expandedFolders[entry.path] === true)
                    walk(entry.path, depth + 1);
            }
        };
        walk("", 0);
        treeRows = rows;
    }

    // Flattens the nested link tree, hiding the children of folded notes.
    function rebuildLinkTree() {
        var rows = [];
        var walk = function(nodes, depth) {
            for (var i = 0; i < nodes.length; ++i) {
                var node = nodes[i];
                var open = linksExpanded[node.path] === true;
                rows.push({ name: node.name, path: node.path, url: node.url,
                            isDir: false, hasChildren: node.children.length > 0,
                            childCount: node.childCount, tags: node.tags,
                            depth: depth, expanded: open });
                if (open)
                    walk(node.children, depth + 1);
            }
        };
        walk(backend.virtFolderTree(), 0);
        treeRows = rows;
    }

    function highlightCurrentInTree() {
        var current = backend.fileTreeCurrentPath;
        for (var i = 0; i < treeRows.length; ++i) {
            if (!treeRows[i].isDir && treeRows[i].path === current) {
                sidebarList.currentIndex = i;
                return;
            }
        }
    }

    // Opens the way to the open note, so it can be seen in the tree: its
    // folders in folder mode, its ancestors in link mode.
    function revealCurrentInTree() {
        var current = backend.fileTreeCurrentPath;
        if (current === "")
            return;
        if (backend.treeMode === "folders") {
            var parts = current.split("/");
            var expanded = Object.assign({}, expandedFolders);
            var path = "";
            for (var i = 0; i < parts.length - 1; ++i) {
                path = path === "" ? parts[i] : path + "/" + parts[i];
                expanded[path] = true;
            }
            expandedFolders = expanded;
            return;
        }
        var trail = linkAncestors(backend.virtFolderTree(), current, []);
        if (trail === null)
            return;
        var open = Object.assign({}, linksExpanded);
        for (var j = 0; j < trail.length; ++j)
            open[trail[j]] = true;
        linksExpanded = open;
    }

    // The paths above the target in the nested link tree, or null.
    function linkAncestors(nodes, target, trail) {
        for (var i = 0; i < nodes.length; ++i) {
            if (nodes[i].path === target)
                return trail;
            var deeper = linkAncestors(nodes[i].children, target,
                                       trail.concat([nodes[i].path]));
            if (deeper !== null)
                return deeper;
        }
        return null;
    }

    function toggleSidebar() {
        sidebarOpen = !sidebarOpen;
        if (sidebarOpen) {
            // A note may have been filed, renamed, or re-parented since the
            // tree last looked, so reread the vault before showing it.
            backend.refreshNoteIndex();
            revealCurrentInTree();
            rebuildTree();
            Qt.callLater(function() { sidebarList.forceActiveFocus(); });
        } else {
            editor.forceActiveFocus();
        }
    }

    // A folder click folds in Folders mode, as it always has. A note row
    // always opens its note; its children are folded from the chevron, so a
    // parent can be both read and unfolded.
    function activateTreeRow(index) {
        if (index < 0 || index >= treeRows.length)
            return;
        var row = treeRows[index];
        if (backend.treeMode === "folders" && row.isDir) {
            toggleTreeRow(index);
            return;
        }
        openInTab(row.url);
    }

    function toggleTreeRow(index) {
        if (index < 0 || index >= treeRows.length)
            return;
        var row = treeRows[index];
        var foldable = backend.treeMode === "folders" ? row.isDir : row.hasChildren;
        if (!foldable)
            return;

        var expanded = backend.treeMode === "folders"
            ? Object.assign({}, expandedFolders) : Object.assign({}, linksExpanded);
        expanded[row.path] = !expanded[row.path];
        if (backend.treeMode === "folders")
            expandedFolders = expanded;
        else
            linksExpanded = expanded;
        rebuildTree();
        sidebarList.currentIndex = index;
    }

    function setTreeFolderOpen(index, open) {
        if (index < 0 || index >= treeRows.length)
            return;
        var row = treeRows[index];
        var foldable = backend.treeMode === "folders" ? row.isDir : row.hasChildren;
        if (!foldable || row.expanded === open)
            return;
        toggleTreeRow(index);
    }

    // Linked mentions: the notes that link to the open one, as a panel
    // beside the page. Read from disk each time it opens or the note changes.
    property bool backlinksOpen: false
    property var backlinkItems: []
    property var backlinkCollapsed: ({})

    function refreshBacklinks() {
        backlinkItems = backlinksOpen ? backend.backlinks() : [];
    }

    function toggleBacklinks() {
        backlinksOpen = !backlinksOpen;
        if (backlinksOpen) {
            backlinkCollapsed = ({});
            backend.refreshNoteIndex();
        }
        refreshBacklinks();
        if (!backlinksOpen)
            editor.forceActiveFocus();
    }

    function toggleBacklinkSource(path) {
        var collapsed = Object.assign({}, backlinkCollapsed);
        collapsed[path] = !collapsed[path];
        backlinkCollapsed = collapsed;
    }

    // Tabs: each remembers a file and where the caret was. One document is
    // open at a time, so switching a tab opens its file again (auto-save
    // keeps its edits, an unsaved one asks first) and puts the caret back.
    property var tabs: []
    property int activeTab: -1
    // A file about to open in a tab of its own rather than in the current one.
    property string newTabUrl: ""
    // A tab to drop once the file next to it has actually opened.
    property string closingTabUrl: ""

    // Visited files, oldest first, and the one on screen. Opening a file
    // appends to the list; going back or forward moves through it instead.
    // The window keeps this rather than the backend because a tab switch and
    // a link click are the same thing to a writer: another file on screen.
    property var fileHistory: []
    property int historyIndex: -1
    // The entry a back or forward step is waiting to land on. It stays set
    // until that file opens, so a cancelled unsaved prompt changes nothing.
    property int historyPending: -1
    readonly property bool canGoBack: historyIndex > 0
    readonly property bool canGoForward:
        historyIndex >= 0 && historyIndex < fileHistory.length - 1

    function recordHistory(url) {
        var wanted = url.toString();
        if (historyIndex >= 0 && fileHistory[historyIndex] === wanted)
            return;
        var list = fileHistory.slice(0, historyIndex + 1);
        list.push(wanted);
        // A long session should not grow an unbounded list; a hundred notes
        // reaches back far past what anyone steps through by hand.
        if (list.length > 100)
            list.shift();
        fileHistory = list;
        historyIndex = list.length - 1;
    }

    // Called when a file really opened: either a new visit to record, or the
    // end of a back/forward step that is now on screen.
    function noteFileShown(url) {
        var current = url.toString();
        if (historyPending >= 0) {
            if (historyPending < fileHistory.length
                    && fileHistory[historyPending] === current) {
                historyIndex = historyPending;
                historyPending = -1;
                return;
            }
            historyPending = -1;
        }
        recordHistory(current);
    }

    function stepHistory(direction) {
        var target = historyIndex + direction;
        if (target < 0 || target >= fileHistory.length)
            return;
        historyPending = target;
        requestOpen(fileHistory[target]);
    }

    function tabIndexOf(url) {
        for (var i = 0; i < tabs.length; ++i) {
            if (tabs[i].url === url)
                return i;
        }
        return -1;
    }

    function tabTitle(url) {
        var name = decodeURIComponent(url.slice(url.lastIndexOf("/") + 1));
        return name.replace(/\.(md|markdown)$/i, "");
    }

    // Opens a note in its own tab, or switches to the tab it is already in.
    function openInTab(url) {
        var wanted = url.toString();
        if (wanted === backend.fileUrl.toString())
            return;
        if (tabIndexOf(wanted) < 0)
            newTabUrl = wanted;
        requestOpen(url);
    }

    function switchTab(index) {
        if (index < 0 || index >= tabs.length || index === activeTab)
            return;
        requestOpen(tabs[index].url);
    }

    function stepTab(direction) {
        if (tabs.length > 1)
            switchTab((activeTab + direction + tabs.length) % tabs.length);
    }

    function closeTab(index) {
        if (tabs.length <= 1 || index < 0 || index >= tabs.length)
            return;
        if (index === activeTab) {
            // The file stays until the neighbour has opened: an unsaved
            // prompt the writer cancels must not leave a tab-less document.
            closingTabUrl = tabs[index].url;
            requestOpen(tabs[index > 0 ? index - 1 : 1].url);
            return;
        }
        var remaining = tabs.slice();
        remaining.splice(index, 1);
        if (index < activeTab)
            --activeTab;
        tabs = remaining;
    }

    // Keeps the tab list in step with whatever file the backend now holds.
    function syncTabs() {
        var current = backend.fileUrl.toString();
        if (current === "")
            return;

        // Every file that reaches the window joins the back/forward list;
        // a step that is still in flight settles its index here instead.
        noteFileShown(current);

        var list = tabs.slice();
        // The signal fires before the new text loads, so the caret is still
        // the one of the tab being left.
        if (activeTab >= 0 && activeTab < list.length)
            list[activeTab] = { url: list[activeTab].url, caret: editor.cursorPosition };
        var activeUrl = activeTab >= 0 && activeTab < list.length ? list[activeTab].url : "";
        if (closingTabUrl !== "" && closingTabUrl !== current) {
            list = list.filter(function(tab) { return tab.url !== closingTabUrl; });
        }
        closingTabUrl = "";

        var index = -1;
        for (var i = 0; i < list.length; ++i) {
            if (list[i].url === current)
                index = i;
        }
        if (index < 0) {
            var from = -1;
            for (var j = 0; j < list.length; ++j) {
                if (list[j].url === activeUrl)
                    from = j;
            }
            if (from >= 0 && current !== newTabUrl) {
                list[from] = { url: current, caret: 0 };
                index = from;
            } else {
                list.push({ url: current, caret: 0 });
                index = list.length - 1;
            }
        }
        if (current === newTabUrl)
            newTabUrl = "";

        tabs = list;
        activeTab = index;
        // A tab never left has no caret to give back; touching it would only
        // move the writer's caret after the fact.
        var caret = list[index].caret;
        if (caret > 0) {
            Qt.callLater(function() {
                editor.cursorPosition = Math.min(caret, editor.length);
                editorFlick.ensureCursorVisible();
            });
        }
    }

    // The command palette lists the backend's actions and the slipbox's notes
    // in one list. Ctrl+P opens it, and Ctrl+Shift+O opens the same palette in
    // file-only mode, the old quick-open key.
    function openCommandPalette(fileOnly) {
        if (commandPalette.visible && commandPalette.fileMode === Boolean(fileOnly)) {
            commandPalette.close();
            return;
        }
        backend.refreshNoteIndex();
        commandPalette.open(Boolean(fileOnly));
    }

    // Every action, whether it comes from a Shortcut or the palette, lands
    // here. The ids are the backend registry's, so the two paths cannot drift.
    function runAction(id) {
        switch (id) {
        case "save": backend.save(); break;
        case "save-as": backend.saveAsDialog(); break;
        case "open-file": backend.openDialog(); break;
        case "command-palette": openCommandPalette(false); break;
        case "toggle-sidebar": toggleSidebar(); break;
        case "quick-open": openCommandPalette(true); break;
        case "tab-1": switchTab(0); break;
        case "tab-2": switchTab(1); break;
        case "tab-3": switchTab(2); break;
        case "tab-4": switchTab(3); break;
        case "tab-5": switchTab(4); break;
        case "tab-6": switchTab(5); break;
        case "tab-7": switchTab(6); break;
        case "tab-8": switchTab(7); break;
        case "tab-9": switchTab(tabs.length - 1); break;
        case "next-tab": stepTab(1); break;
        case "prev-tab": stepTab(-1); break;
        case "close-tab": closeTab(activeTab); break;
        case "back": stepHistory(-1); break;
        case "forward": stepHistory(1); break;
        case "backlinks": toggleBacklinks(); break;
        case "new-window": backend.newWindow(); break;
        case "print": backend.printDocument(); break;
        case "copy-html": backend.copyAsHtml(); break;
        case "fullscreen": toggleFullScreen(); break;
        case "undo": editor.undo(); break;
        case "redo": editor.redo(); break;
        case "find": openSearch(false); break;
        case "find-replace": openSearch(true); break;
        case "find-next": if (searchOpen) moveSearch(1); break;
        case "bold": editor.wrapSelection("**"); break;
        case "italic": editor.wrapSelection("*"); break;
        case "strikethrough": editor.wrapSelection("~~"); break;
        case "insert-link": editor.insertLink(); break;
        case "follow-link": followLinkAt(editor.cursorPosition); break;
        case "toggle-task": editor.toggleTaskAtCursor(); break;
        case "zoom-in": backend.increaseTextSize(); break;
        case "zoom-out": backend.decreaseTextSize(); break;
        case "zoom-reset": backend.resetTextSize(); break;
        case "shortcuts-dialog": shortcutsDialog.open(); break;
        case "settings": settingsDialog.open(); break;
        case "font": fontPicker.opened ? fontPicker.close() : fontPicker.open(); break;
        }
    }

    FontMetrics {
        id: writerFontMetrics
        font.family: backend.editorFont
        font.pixelSize: win.editorFontPixelSize
    }

    // Every hardcoded size in the interface is expressed at text scale 1.
    function scaledSize(pixels) {
        return Math.max(1, Math.round(pixels * win.textScale));
    }

    function toggleFullScreen() {
        win.visibility = win.visibility === Window.FullScreen
            ? Window.Windowed
            : Window.FullScreen;
    }

    // One binding's worth of the active key for an action, and the list of
    // them. Both read backend.shortcutMap, so editing a key in the settings
    // dialog rebinds every Shortcut below without touching this file.
    function seq(id) {
        var list = backend.shortcutMap[id];
        return list && list.length > 0 ? list[0] : "";
    }

    function seqs(id) {
        var list = backend.shortcutMap[id];
        return list ? list : [];
    }

    // Only the search field and the next and previous controls select a
    // match. When the document changes underneath, the matches follow along
    // without taking the writer's caret.
    function updateSearch(selectMatch) {
        searchDelay.stop();
        var previous = searchMatchIndex >= 0 ? searchMatches[searchMatchIndex] : 0;
        searchMatches = backend.searchMatches(searchField.text);
        searchMatchIndex = selectMatch ? (searchMatches.length > 0 ? 0 : -1)
                                       : matchAtOrAfter(previous);
        showSearchMatch(selectMatch);
    }

    function matchAtOrAfter(position) {
        for (var i = 0; i < searchMatches.length; ++i) {
            if (searchMatches[i] >= position)
                return i;
        }
        return searchMatches.length > 0 ? 0 : -1;
    }

    function showSearchMatch(selectMatch) {
        var start = searchMatchIndex >= 0 ? searchMatches[searchMatchIndex] : -1;
        searchUpdating = true;
        backend.setSearchHighlight(searchField.text, start);
        if (selectMatch && start >= 0) {
            editor.select(start, start + searchField.text.length);
            editorFlick.ensureCursorVisible();
        }
        searchUpdating = false;
    }

    // Typing in the search field waits for a pause before searching, so
    // anything that acts on the matches catches up with the field first.
    function flushSearch() {
        if (searchDelay.running)
            updateSearch(true);
    }

    function moveSearch(direction) {
        // A pending search lands on its first match, which is where the
        // writer meant to go, rather than one past it.
        if (searchDelay.running) {
            updateSearch(true);
            return;
        }
        if (searchMatches.length === 0)
            return;
        searchMatchIndex = (searchMatchIndex + direction + searchMatches.length)
                           % searchMatches.length;
        showSearchMatch(true);
    }

    // The matches stop following the field and the text while the bar is
    // closed, and a search still waiting for a pause is dropped on closing,
    // so opening the bar searches again for what the field holds.
    function openSearch(withReplace) {
        searchOpen = true;
        if (withReplace)
            replaceOpen = true;
        searchField.forceActiveFocus();
        searchField.selectAll();
        // A search still waiting in an open bar lands on its first match.
        updateSearch(searchDelay.running);
    }

    function closeSearch() {
        searchDelay.stop();
        // Hand focus to the editor before hiding the bar. Hiding the field
        // that holds focus leaves the input method with nothing focused for
        // a moment, and fcitx5 can then miss the next Ctrl+F.
        Qt.inputMethod.reset();
        editor.forceActiveFocus();
        searchOpen = false;
        searchUpdating = true;
        backend.setSearchHighlight("", -1);
        editor.deselect();
        searchUpdating = false;
        replaceOpen = false;
    }

    // Restyling a large document for each letter typed makes typing a word
    // stall, so the search waits until the typing pauses.
    Timer {
        id: searchDelay
        interval: 150
        onTriggered: win.updateSearch(true)
    }

    // Every keyboard action goes through runAction, so a palette command and
    // the key beside it are the same piece of code. The ids and their live
    // keys come from the backend's action registry.
    Shortcut {
        sequence: win.seq("save")
        onActivated: win.runAction("save")
    }

    Shortcut {
        objectName: "openFileShortcut"
        sequence: win.seq("open-file")
        onActivated: win.runAction("open-file")
    }

    Shortcut {
        objectName: "commandPaletteShortcut"
        sequence: win.seq("command-palette")
        onActivated: win.runAction("command-palette")
    }

    Shortcut {
        sequence: win.seq("find-replace")
        onActivated: win.runAction("find-replace")
    }

    Shortcut {
        sequence: win.seq("bold")
        context: Qt.WindowShortcut
        onActivated: win.runAction("bold")
    }

    Shortcut {
        sequence: win.seq("italic")
        context: Qt.WindowShortcut
        onActivated: win.runAction("italic")
    }

    Shortcut {
        sequence: win.seq("strikethrough")
        context: Qt.WindowShortcut
        onActivated: win.runAction("strikethrough")
    }

    Shortcut {
        sequence: win.seq("insert-link")
        context: Qt.WindowShortcut
        onActivated: win.runAction("insert-link")
    }

    Shortcut {
        sequences: win.seqs("follow-link")
        context: Qt.WindowShortcut
        onActivated: win.runAction("follow-link")
    }

    Shortcut {
        sequences: win.seqs("toggle-task")
        context: Qt.WindowShortcut
        onActivated: win.runAction("toggle-task")
    }

    Shortcut {
        sequence: win.seq("tab-1")
        context: Qt.WindowShortcut
        onActivated: win.runAction("tab-1")
    }

    Shortcut {
        sequence: win.seq("tab-2")
        context: Qt.WindowShortcut
        onActivated: win.runAction("tab-2")
    }

    Shortcut {
        sequence: win.seq("tab-3")
        context: Qt.WindowShortcut
        onActivated: win.runAction("tab-3")
    }

    Shortcut {
        sequence: win.seq("tab-4")
        context: Qt.WindowShortcut
        onActivated: win.runAction("tab-4")
    }

    Shortcut {
        sequence: win.seq("tab-5")
        context: Qt.WindowShortcut
        onActivated: win.runAction("tab-5")
    }

    Shortcut {
        sequence: win.seq("tab-6")
        context: Qt.WindowShortcut
        onActivated: win.runAction("tab-6")
    }

    Shortcut {
        sequence: win.seq("tab-7")
        context: Qt.WindowShortcut
        onActivated: win.runAction("tab-7")
    }

    Shortcut {
        sequence: win.seq("tab-8")
        context: Qt.WindowShortcut
        onActivated: win.runAction("tab-8")
    }

    Shortcut {
        sequence: win.seq("tab-9")
        context: Qt.WindowShortcut
        onActivated: win.runAction("tab-9")
    }

    Shortcut {
        sequence: win.seq("next-tab")
        context: Qt.WindowShortcut
        onActivated: win.runAction("next-tab")
    }

    Shortcut {
        sequence: win.seq("prev-tab")
        context: Qt.WindowShortcut
        onActivated: win.runAction("prev-tab")
    }

    Shortcut {
        sequence: win.seq("close-tab")
        context: Qt.WindowShortcut
        onActivated: win.runAction("close-tab")
    }

    Shortcut {
        sequences: win.seqs("back")
        context: Qt.WindowShortcut
        onActivated: win.runAction("back")
    }

    Shortcut {
        sequences: win.seqs("forward")
        context: Qt.WindowShortcut
        onActivated: win.runAction("forward")
    }

    Shortcut {
        sequence: win.seq("backlinks")
        context: Qt.WindowShortcut
        onActivated: win.runAction("backlinks")
    }

    Shortcut {
        sequence: win.seq("quick-open")
        context: Qt.WindowShortcut
        onActivated: win.runAction("quick-open")
    }

    Shortcut {
        sequences: win.seqs("shortcuts-dialog")
        onActivated: win.runAction("shortcuts-dialog")
    }

    Shortcut {
        // Ctrl++ is a different shortcut to Qt, not a shifted Ctrl+=, and it is
        // the only one a layout that puts + on its own key can reach.
        sequences: win.seqs("zoom-in")
        onActivated: win.runAction("zoom-in")
    }

    Shortcut {
        sequence: win.seq("zoom-out")
        onActivated: win.runAction("zoom-out")
    }

    Shortcut {
        sequence: win.seq("zoom-reset")
        onActivated: win.runAction("zoom-reset")
    }

    Shortcut {
        objectName: "toggleSidebarShortcut"
        sequence: win.seq("toggle-sidebar")
        onActivated: win.runAction("toggle-sidebar")
    }

    Shortcut {
        sequence: win.seq("new-window")
        onActivated: win.runAction("new-window")
    }

    Shortcut {
        sequence: win.seq("save-as")
        onActivated: win.runAction("save-as")
    }

    Shortcut {
        objectName: "printShortcut"
        sequence: win.seq("print")
        onActivated: win.runAction("print")
    }

    Shortcut {
        sequence: win.seq("copy-html")
        onActivated: win.runAction("copy-html")
    }

    Shortcut {
        sequences: win.seqs("fullscreen")
        onActivated: win.runAction("fullscreen")
    }

    Shortcut {
        sequence: win.seq("undo")
        context: Qt.WindowShortcut
        onActivated: win.runAction("undo")
    }

    Shortcut {
        sequences: win.seqs("redo")
        context: Qt.WindowShortcut
        onActivated: win.runAction("redo")
    }

    Shortcut {
        sequence: win.seq("find")
        onActivated: win.runAction("find")
    }

    Shortcut {
        sequence: win.seq("find-next")
        enabled: win.searchOpen
        onActivated: win.runAction("find-next")
    }

    Connections {
        target: backend

        function onFileUrlChanged() {
            // The folder a note lives in decides which image embeds resolve,
            // so look at the pictures again when it knows where it lives.
            editor.refreshImages();
            win.refreshBacklinks();
            win.syncTabs();
            if (win.sidebarOpen) {
                win.revealCurrentInTree();
                win.rebuildTree();
            }
        }

        // Notes moved or edited outside Omawrite settle here, after the
        // index's watcher has marked its snapshot stale.
        function onNoteIndexChanged() {
            win.refreshBacklinks();
            if (win.sidebarOpen) {
                win.revealCurrentInTree();
                win.rebuildTree();
            }
        }

        function onOpenDialogRequested() {
            // Start at the vault root when the note belongs to one, so the
            // picker reaches any note in the slipbox; otherwise stay beside
            // the open note or in the last save folder.
            var folder = backend.openDialogFolder();
            if (folder.toString().length > 0)
                openFileDialog.currentFolder = folder;
            openFileDialog.open();
        }

        function onSaveDialogRequested(suggestedUrl) {
            saveFileDialog.selectedFile = suggestedUrl;
            saveFileDialog.open();
        }

        function onSaveSucceeded() {
            // A save can change a note's up: or tags, which the link tree
            // draws from the index rather than from disk on each rebuild.
            backend.refreshNoteIndex();
            if (win.sidebarOpen)
                win.rebuildTree();
            // Only a save the writer chose in the prompt carries out the
            // pending close or open, never an auto-save that lands meanwhile.
            const confirmed = win.awaitingPendingSave;
            win.awaitingPendingSave = false;
            if (confirmed && win.pendingAction !== "")
                win.completePendingAction();
        }

        function onSaveFailed() {
            // A failed save leaves the document as it was, so the close or
            // open it was for is off; a later save must not carry it out.
            win.awaitingPendingSave = false;
            win.pendingAction = "";
        }

        function onExternalChangeDetected(deleted, locallyModified) {
            externalChangeDialog.deleted = deleted;
            externalChangeDialog.locallyModified = locallyModified;
            externalChangeDialog.open();
        }
    }

    Dialogs.FileDialog {
        id: openFileDialog
        objectName: "openFileDialog"
        title: "Open File"
        fileMode: Dialogs.FileDialog.OpenFile
        nameFilters: ["Markdown files (*.md *.markdown)", "All files (*)"]
        // A picked file joins the tabs like one chosen from the tree, quick
        // open, or a wiki link, rather than replacing the current note.
        onAccepted: win.openInTab(selectedFile)
    }

    Dialogs.FileDialog {
        id: saveFileDialog
        title: "Save File"
        fileMode: Dialogs.FileDialog.SaveFile
        nameFilters: ["Markdown files (*.md *.markdown)", "All files (*)"]
        onAccepted: backend.saveAs(selectedFile)
        onRejected: {
            win.awaitingPendingSave = false;
            win.pendingAction = "";
            win.historyPending = -1;
        }
    }

    UnsavedChangesDialog {
        id: unsavedChangesDialog
        objectName: "unsavedChangesDialog"
        fileName: backend.fileName
        darkMode: win.darkMode
        textScale: win.textScale
        textColor: win.textColor
        strongTextColor: win.textColor
        activeButtonColor: backend.themeAccent
        containerWidth: win.width
        containerHeight: win.height

        // Discard must still mean discard, so nothing is auto-saved while
        // the writer decides.
        onAboutToShow: backend.holdAutoSave(true)
        onClosed: backend.holdAutoSave(false)

        onDiscardRequested: {
            backend.discardRecovery();
            win.completePendingAction();
        }

        onSaveRequested: {
            win.awaitingPendingSave = true;
            backend.save();
        }
        onCancelRequested: {
            win.pendingAction = "";
            win.historyPending = -1;
        }
    }

    ExternalChangeDialog {
        id: externalChangeDialog
        darkMode: win.darkMode
        textScale: win.textScale
        textColor: win.textColor
        strongTextColor: win.textColor
        activeButtonColor: backend.themeAccent
        containerWidth: win.width
        containerHeight: win.height

        onKeepRequested: backend.keepExternalVersion()
        onReloadRequested: backend.reloadFromDisk()
    }

    SettingsDialog {
        id: settingsDialog
        darkMode: win.darkMode
        textScale: win.textScale
        textColor: win.textColor
        activeButtonColor: backend.themeAccent
        containerWidth: win.width
        containerHeight: win.height
    }

    ShortcutsDialog {
        id: shortcutsDialog
        darkMode: win.darkMode
        textScale: win.textScale
        textColor: win.textColor
        activeButtonColor: backend.themeAccent
        containerWidth: win.width
        containerHeight: win.height
    }

    // Ctrl+P: one list over the action registry and the slipbox's notes.
    // Ctrl+Shift+O opens the same palette with files only.
    CommandPalette {
        id: commandPalette
        anchors.fill: parent
        darkMode: win.darkMode
        textColor: win.textColor
        mutedColor: win.mutedColor
        selectionFill: win.selectionFill
        selectionTextColor: win.selectionTextColor
        textScale: win.textScale
        fontFamily: backend.editorFont

        onCommandChosen: function(id) { win.runAction(id) }
        onFileChosen: function(url) { win.openInTab(url) }
        onClosed: editor.forceActiveFocus()
    }

    Item {
        id: rootItem
        anchors.fill: parent

        Flickable {
            id: editorFlick
            objectName: "editorScroll"
            anchors.fill: parent
            anchors.leftMargin: 24 + win.sidebarWidth
            anchors.rightMargin: 24 + win.backlinksWidth
            // Inset the scrolling area by the footer strip at both ends, so
            // scrolled text never runs under the footer buttons, status, or
            // word count, and stops just as far short of the top edge.
            anchors.topMargin: win.footerHeight
            anchors.bottomMargin: win.footerHeight
            clip: true
            contentWidth: width
            contentHeight: Math.max(height, editor.y + editor.implicitHeight + 220)
            boundsBehavior: Flickable.StopAtBounds
            ScrollBar.vertical: ScrollBar {
                policy: ScrollBar.AsNeeded
                // Wheel scrolling moves contentY directly rather than
                // flicking the Flickable, so the bar has to be told about
                // that activity; linger briefly after the last event.
                active: hovered || pressed || wheelScroll.running || scrollLinger.running
            }

            // Also lingers after paging through the document from the
            // keyboard, so the bar shows where the jump landed. Writing past
            // the bottom edge scrolls too, but leaves the bar hidden.
            Timer {
                id: scrollLinger
                objectName: "scrollLinger"
                interval: 600
            }

            // Flickable turns a wheel notch into a flick sized by the small
            // application font, which crawls next to a browser. Reproduce
            // Chromium's wheel physics instead (cc::ScrollOffsetAnimationCurve):
            // each notch moves 3 lines of 40px towards a running target, the
            // animation gets shorter as the outstanding distance grows, and a
            // notch landing mid-animation carries the current velocity into
            // the new curve, so sustained spinning keeps picking up speed.
            readonly property real wheelStep: win.scaledSize(120) * backend.scrollSpeed

            FrameAnimation {
                id: wheelScroll
                running: false

                property real startY: 0
                property real targetY: 0
                property real duration: 0.2
                // Cubic bezier easing; ease-in-out (0.42, 0, 0.58, 1) for a
                // fresh scroll, with y1 tilted on retarget so the curve's
                // initial slope matches the velocity it inherits.
                property real cx1: 0.42
                property real cy1: 0
                readonly property real cx2: 0.58
                readonly property real cy2: 1

                onTriggered: {
                    var x = elapsedTime / duration;
                    if (x >= 1) {
                        editorFlick.contentY = editorFlick.snapToPixel(targetY);
                        stop();
                        return;
                    }
                    editorFlick.contentY = editorFlick.snapToPixel(
                        startY + (targetY - startY) * curveY(solveCurve(x)));
                }

                function begin(from, to, dur, slope) {
                    startY = from;
                    targetY = to;
                    duration = dur;
                    cx1 = 0.42;
                    cy1 = 0.42 * Math.max(-1000, Math.min(1000, slope));
                    restart();
                }

                function retarget(newTarget) {
                    var s = solveCurve(Math.min(1, elapsedTime / duration));
                    var pos = startY + (targetY - startY) * curveY(s);
                    var delta = newTarget - pos;
                    if (Math.abs(delta) < 0.5) {
                        editorFlick.contentY = newTarget;
                        stop();
                        return;
                    }

                    var velocity = curveDY(s) / Math.max(1e-6, curveDX(s))
                        * (targetY - startY) / duration;
                    var dur = editorFlick.wheelDuration(delta);
                    // When already moving faster than the eased curve would,
                    // bound the duration by the time to target at the current
                    // velocity; the 2.5x covers the ease-out tail.
                    if (velocity !== 0 && delta / velocity > 0)
                        dur = Math.min(dur, delta / velocity * 2.5);
                    begin(pos, newTarget, dur, velocity * dur / delta);
                }

                // Cubic bezier through (0,0), (cx1,cy1), (cx2,cy2), (1,1),
                // evaluated by Newton-solving the curve parameter from x.
                function curveX(s) { return 3 * s * (1 - s) * ((1 - s) * cx1 + s * cx2) + s * s * s; }
                function curveY(s) { return 3 * s * (1 - s) * ((1 - s) * cy1 + s * cy2) + s * s * s; }
                function curveDX(s) { return 3 * (1 - s) * (1 - s) * cx1 + 6 * (1 - s) * s * (cx2 - cx1) + 3 * s * s * (1 - cx2); }
                function curveDY(s) { return 3 * (1 - s) * (1 - s) * cy1 + 6 * (1 - s) * s * (cy2 - cy1) + 3 * s * s * (1 - cy2); }

                function solveCurve(x) {
                    var s = x;
                    for (var i = 0; i < 8; ++i) {
                        var error = curveX(s) - x;
                        if (Math.abs(error) < 0.001)
                            break;
                        var d = curveDX(s);
                        if (Math.abs(d) < 1e-6)
                            break;
                        s = Math.max(0, Math.min(1, s - error / d));
                    }
                    return s;
                }
            }

            WheelHandler {
                // Wayland compositors route every pointer's scroll through
                // one seat device that Qt classifies as a touchpad, so the
                // device type cannot tell a mouse wheel from two-finger
                // scrolling. Distinguish by event shape instead: discrete
                // wheel notches arrive with only angleDelta set, while
                // finger scrolling carries pixel-precise pixelDelta.
                acceptedDevices: PointerDevice.Mouse | PointerDevice.TouchPad
                onWheel: function(wheel) {
                    if (wheel.modifiers & Qt.ControlModifier) {
                        editorFlick.zoomByWheel(wheel);
                        wheel.accepted = true;
                        return;
                    }

                    scrollLinger.restart();
                    if (wheel.pixelDelta.y !== 0)
                        editorFlick.scrollTo(editorFlick.clampContentY(editorFlick.contentY - wheel.pixelDelta.y));
                    else
                        editorFlick.scrollByWheel(wheel);
                    wheel.accepted = true;
                }
            }

            onMovementStarted: wheelScroll.stop()
            onContentYChanged: {
                editor.hoveredLinkUrl = "";
                win.positionWikiPopup();
            }

            // Ctrl+wheel changes the text size, as it does in a browser. A
            // wheel notch is a whole step, but a touchpad trickles in pixels,
            // so bank the fractions and step once a whole one is scrolled.
            property real zoomNotches: 0

            function zoomByWheel(wheel) {
                // Finger scrolling synthesizes an angleDelta too, so pixelDelta
                // is the one to test first, as in onWheel.
                var notches = wheel.pixelDelta.y !== 0
                    ? wheel.pixelDelta.y / 50
                    : wheel.angleDelta.y / 120;
                if (zoomNotches * notches < 0)
                    zoomNotches = 0;

                zoomNotches += notches;
                while (zoomNotches >= 1) {
                    backend.increaseTextSize();
                    zoomNotches -= 1;
                }
                while (zoomNotches <= -1) {
                    backend.decreaseTextSize();
                    zoomNotches += 1;
                }
            }

            function scrollByWheel(wheel) {
                // High-resolution wheels report fractional notches; feed
                // those through the same animated path, like Chromium does
                // for every wheel-source event.
                var notches = wheel.angleDelta.y / 120;
                if (notches === 0)
                    return;

                if (wheelScroll.running) {
                    wheelScroll.retarget(clampContentY(wheelScroll.targetY - notches * wheelStep));
                    return;
                }

                var target = clampContentY(contentY - notches * wheelStep);
                if (target !== contentY)
                    wheelScroll.begin(contentY, target, wheelDuration(target - contentY), 0);
            }

            // Chromium's inverse-delta duration: 200ms for a single notch,
            // ramping down to 100ms once 480px are outstanding.
            function wheelDuration(delta) {
                var pixels = Math.abs(delta) / win.textScale;
                return Math.max(6, Math.min(12, 14 - pixels / 60)) / 60;
            }

            function clampContentY(y) {
                return Math.max(0, Math.min(Math.max(0, contentHeight - height), y));
            }

            // Whole device pixels keep natively hinted glyphs from
            // re-rasterizing mid-animation, which reads as shimmer.
            function snapToPixel(y) {
                return Math.round(y * Screen.devicePixelRatio) / Screen.devicePixelRatio;
            }

            // Jump to a position, abandoning any wheel animation still running.
            function scrollTo(y) {
                wheelScroll.stop();
                contentY = snapToPixel(y);
            }

            // The writing column's resting top, with the properties panel, if
            // any, sitting above the text at this line.
            readonly property int textTop:
                Math.max(0, Math.max(42, Math.round(win.height * 0.05)) - win.footerHeight)

            // Keep the editing caret within the viewport so writing past the
            // bottom edge scrolls the page along with the text.

            function ensureCursorVisible() {
                var margin = win.editorFontPixelSize * 2;
                var cursorTop = editor.y + editor.cursorRectangle.y;
                var cursorBottom = cursorTop + editor.cursorRectangle.height;
                var maxContentY = Math.max(0, contentHeight - height);

                if (cursorBottom + margin > contentY + height)
                    scrollTo(Math.min(maxContentY, cursorBottom + margin - height));
                else if (cursorTop - margin < contentY)
                    scrollTo(Math.max(0, cursorTop - margin));
            }

            // Inline properties, Obsidian-style: the note's frontmatter drawn
            // as rows of values at the top of the page. Tags and up: links are
            // chips; other values edit in place; the raw YAML never reaches
            // the editor, and Backend joins it back on save.
            Item {
                id: propertiesPanel
                objectName: "propertiesPanel"
                visible: backend.hasProperties
                x: Math.round((editorFlick.width - width) / 2)
                y: editorFlick.textTop
                width: win.editorWidth
                height: propertiesColumn.height
                property bool collapsed: false
                property bool addingProperty: false

                Column {
                    id: propertiesColumn
                    width: parent.width
                    height: childrenRect.height
                    spacing: win.scaledSize(4)

                    Row {
                        spacing: win.scaledSize(8)
                        height: win.scaledSize(28)

                        Text {
                            text: "Properties"
                            color: win.textColor
                            font.family: backend.editorFont
                            font.pixelSize: win.scaledSize(15)
                            font.bold: true
                            anchors.verticalCenter: parent.verticalCenter
                        }

                        Text {
                            text: propertiesPanel.collapsed ? "›" : "⌄"
                            color: win.mutedColor
                            font.pixelSize: win.scaledSize(15)
                            anchors.verticalCenter: parent.verticalCenter
                            MouseArea {
                                anchors.fill: parent
                                anchors.margins: -8
                                cursorShape: Qt.PointingHandCursor
                                onClicked: propertiesPanel.collapsed = !propertiesPanel.collapsed
                            }
                        }
                    }

                    Repeater {
                        model: backend.documentProperties

                        Item {
                            id: propertyRow
                            width: propertiesColumn.width
                            height: Math.max(win.scaledSize(26), valueFlow.height)
                            visible: !propertiesPanel.collapsed

                            property var entry: modelData
                            property bool editing: false
                            property bool editingTag: false

                            readonly property bool isTags:
                                entry.kind === "tags"
                            readonly property bool isLinks:
                                entry.kind === "links"

                            Text {
                                id: keyLabel
                                width: Math.min(win.scaledSize(150), propertyRow.width * 0.3)
                                anchors.verticalCenter: parent.verticalCenter
                                text: propertyRow.entry.key
                                color: win.mutedColor
                                font.family: backend.editorFont
                                font.pixelSize: win.scaledSize(13)
                                elide: Text.ElideRight
                            }

                            Flow {
                                id: valueFlow
                                x: keyLabel.width + win.scaledSize(12)
                                width: propertyRow.width - x - win.scaledSize(28)
                                height: childrenRect.height
                                spacing: win.scaledSize(6)
                                anchors.verticalCenter: parent.verticalCenter

                                Repeater {
                                    model: propertyRow.isTags || propertyRow.isLinks
                                           ? propertyRow.entry.values : []

                                    Rectangle {
                                        width: chipRow.width + win.scaledSize(16)
                                        height: win.scaledSize(24)
                                        radius: win.scaledSize(9)
                                        color: win.darkMode ? "#2e2e28" : "#ece9da"
                                        border.color: win.darkMode ? "#474740" : "#d8d4c2"
                                        border.width: 1

                                        Row {
                                            id: chipRow
                                            anchors.centerIn: parent
                                            spacing: win.scaledSize(6)

                                            Text {
                                                text: modelData
                                                color: win.textColor
                                                font.family: backend.editorFont
                                                font.pixelSize: win.scaledSize(12)
                                            }

                                            Text {
                                                visible: propertyRow.isTags || propertyRow.isLinks
                                                text: "×"
                                                color: win.mutedColor
                                                font.pixelSize: win.scaledSize(12)
                                                MouseArea {
                                                    anchors.fill: parent
                                                    anchors.margins: -6
                                                    cursorShape: Qt.PointingHandCursor
                                                    onClicked: {
                                                        if (propertyRow.isTags)
                                                            backend.removeTag(modelData);
                                                        else
                                                            backend.removePropertyValue("up", modelData);
                                                    }
                                                }
                                            }
                                        }
                                    }
                                }

                                Text {
                                    visible: propertyRow.isTags && !propertyRow.editingTag
                                    text: "+"
                                    color: win.mutedColor
                                    font.pixelSize: win.scaledSize(16)
                                    MouseArea {
                                        anchors.fill: parent
                                        anchors.margins: -8
                                        cursorShape: Qt.PointingHandCursor
                                        onClicked: {
                                            propertyRow.editingTag = true;
                                            Qt.callLater(function() { tagField.forceActiveFocus(); });
                                        }
                                    }
                                }

                                TextField {
                                    id: tagField
                                    visible: propertyRow.editingTag
                                    width: win.scaledSize(110)
                                    height: win.scaledSize(24)
                                    color: win.textColor
                                    selectionColor: win.selectionFill
                                    selectedTextColor: win.selectionTextColor
                                    font.family: backend.editorFont
                                    font.pixelSize: win.scaledSize(12)
                                    leftPadding: win.scaledSize(6)
                                    rightPadding: win.scaledSize(6)
                                    topPadding: 0
                                    bottomPadding: 0
                                    background: Rectangle {
                                        color: win.pageColor
                                        border.color: win.mutedColor
                                        border.width: 1
                                        radius: win.scaledSize(6)
                                    }
                                    onAccepted: {
                                        if (backend.addTag(text))
                                            text = "";
                                        propertyRow.editingTag = false;
                                    }
                                    Keys.onEscapePressed: {
                                        propertyRow.editingTag = false;
                                        event.accepted = true;
                                    }
                                }

                                Text {
                                    visible: !propertyRow.isTags && !propertyRow.isLinks
                                             && !propertyRow.editing
                                    text: propertyRow.entry.display.length > 0
                                          ? propertyRow.entry.display : "—"
                                    color: propertyRow.entry.display.length > 0
                                           ? win.textColor : win.mutedColor
                                    font.family: backend.editorFont
                                    font.pixelSize: win.scaledSize(13)
                                    MouseArea {
                                        anchors.fill: parent
                                        anchors.margins: -4
                                        cursorShape: Qt.PointingHandCursor
                                        onClicked: {
                                            if (propertyRow.entry.kind === "checkbox") {
                                                backend.setPropertyValue(
                                                    propertyRow.entry.key,
                                                    propertyRow.entry.display === "true"
                                                        ? "false" : "true");
                                                return;
                                            }
                                            propertyRow.editing = true;
                                            Qt.callLater(function() { valueField.forceActiveFocus(); });
                                        }
                                    }
                                }

                                TextField {
                                    id: valueField
                                    visible: propertyRow.editing
                                    width: Math.max(win.scaledSize(120),
                                                   valueFlow.width - win.scaledSize(8))
                                    height: win.scaledSize(24)
                                    text: propertyRow.entry.display
                                    color: win.textColor
                                    selectionColor: win.selectionFill
                                    selectedTextColor: win.selectionTextColor
                                    font.family: backend.editorFont
                                    font.pixelSize: win.scaledSize(13)
                                    leftPadding: win.scaledSize(6)
                                    rightPadding: win.scaledSize(6)
                                    topPadding: 0
                                    bottomPadding: 0
                                    background: Rectangle {
                                        color: win.pageColor
                                        border.color: win.mutedColor
                                        border.width: 1
                                        radius: win.scaledSize(6)
                                    }
                                    onAccepted: {
                                        backend.setPropertyValue(propertyRow.entry.key, text);
                                        propertyRow.editing = false;
                                    }
                                    Keys.onEscapePressed: {
                                        propertyRow.editing = false;
                                        event.accepted = true;
                                    }
                                }
                            }

                            Text {
                                anchors.right: parent.right
                                anchors.verticalCenter: parent.verticalCenter
                                text: "−"
                                color: win.mutedColor
                                opacity: 0.45
                                font.pixelSize: win.scaledSize(14)
                                MouseArea {
                                    anchors.fill: parent
                                    anchors.margins: -6
                                    cursorShape: Qt.PointingHandCursor
                                    onClicked: backend.removeProperty(propertyRow.entry.key)
                                }
                            }
                        }
                    }

                    Row {
                        visible: !propertiesPanel.collapsed && !propertiesPanel.addingProperty
                        spacing: win.scaledSize(8)
                        height: win.scaledSize(24)

                        Text {
                            text: "+ Add property"
                            color: win.mutedColor
                            font.family: backend.editorFont
                            font.pixelSize: win.scaledSize(13)
                            anchors.verticalCenter: parent.verticalCenter
                            MouseArea {
                                anchors.fill: parent
                                anchors.margins: -6
                                cursorShape: Qt.PointingHandCursor
                                onClicked: {
                                    propertiesPanel.addingProperty = true;
                                    Qt.callLater(function() { propertyKeyField.forceActiveFocus(); });
                                }
                            }
                        }
                    }

                    Row {
                        visible: !propertiesPanel.collapsed && propertiesPanel.addingProperty
                        spacing: win.scaledSize(6)
                        height: win.scaledSize(26)

                        TextField {
                            id: propertyKeyField
                            width: win.scaledSize(120)
                            height: win.scaledSize(24)
                            placeholderText: "property"
                            placeholderTextColor: win.mutedColor
                            color: win.textColor
                            selectionColor: win.selectionFill
                            selectedTextColor: win.selectionTextColor
                            font.family: backend.editorFont
                            font.pixelSize: win.scaledSize(12)
                            leftPadding: win.scaledSize(6)
                            rightPadding: win.scaledSize(6)
                            topPadding: 0
                            bottomPadding: 0
                            background: Rectangle {
                                color: win.pageColor
                                border.color: win.mutedColor
                                border.width: 1
                                radius: win.scaledSize(6)
                            }
                            onAccepted: propertyValueField.forceActiveFocus()
                        }

                        TextField {
                            id: propertyValueField
                            width: win.scaledSize(220)
                            height: win.scaledSize(24)
                            placeholderText: "value"
                            placeholderTextColor: win.mutedColor
                            color: win.textColor
                            selectionColor: win.selectionFill
                            selectedTextColor: win.selectionTextColor
                            font.family: backend.editorFont
                            font.pixelSize: win.scaledSize(12)
                            leftPadding: win.scaledSize(6)
                            rightPadding: win.scaledSize(6)
                            topPadding: 0
                            bottomPadding: 0
                            background: Rectangle {
                                color: win.pageColor
                                border.color: win.mutedColor
                                border.width: 1
                                radius: win.scaledSize(6)
                            }
                            onAccepted: {
                                if (backend.addProperty(propertyKeyField.text, text)) {
                                    propertyKeyField.text = "";
                                    text = "";
                                    propertiesPanel.addingProperty = false;
                                }
                            }
                            Keys.onEscapePressed: {
                                propertiesPanel.addingProperty = false;
                                event.accepted = true;
                            }
                        }
                    }

                    Item { width: 1; height: win.scaledSize(10); visible: !propertiesPanel.collapsed }
                }
            }

            TextEdit {
                id: editor
                objectName: "sourceEditor"
                x: Math.round((editorFlick.width - width) / 2)
                // The resting position counts from the window's top edge, so
                // take back the top inset the Flickable already adds. The
                // properties panel, when present, sits above the text.
                y: editorFlick.textTop
                   + (propertiesPanel.visible
                      ? propertiesPanel.height + win.scaledSize(26) : 0)
                width: win.editorWidth
                height: Math.max(editorFlick.height - y - 96, implicitHeight + 20)
                text: ""
                textFormat: TextEdit.PlainText
                wrapMode: TextEdit.Wrap
                selectByMouse: true
                persistentSelection: true
                activeFocusOnPress: true
                color: win.textColor
                selectedTextColor: win.selectionTextColor
                selectionColor: win.selectionFill
                font.family: backend.editorFont
                font.pixelSize: win.editorFontPixelSize
                font.weight: Font.Normal
                // Native rendering hints glyphs to the pixel grid, which is
                // crispest at whole scale factors but misplaces and unevenly
                // rasterizes glyphs at fractional ones (and goes stale when
                // the compositor delivers the fractional scale after the
                // first frame). Fall back to Qt's scalable renderer there.
                renderType: Screen.devicePixelRatio % 1 === 0 ? TextEdit.NativeRendering : TextEdit.QtRendering
                cursorDelegate: Rectangle {
                    width: 1
                    color: win.textColor
                }
                onCursorRectangleChanged: editorFlick.ensureCursorVisible()
                onCursorPositionChanged: {
                    backend.setCaretPosition(cursorPosition);
                    // The highlighter has just shown or hidden markers on the
                    // line, so let the checkbox overlays measure themselves
                    // again after that relayout.
                    caretRevision++;
                    win.refreshWikiPopup();
                }
                onFontChanged: {
                    backend.documentFontChanged();
                    // The overlay's row heights and column widths were measured
                    // with the old font, and Backend has just re-measured them.
                    refreshRenderedBlocks();
                }
                onActiveFocusChanged: if (!activeFocus) win.closeWikiPopup()

                // One edit command, so a single undo takes the change back.
                function replaceText(start, end, replacement) {
                    backend.replaceRange(start, end, replacement);
                }

                function replaceSelectionWith(replacement) {
                    EditorMutations.replaceRange(editor, selectionStart, selectionEnd, replacement);
                }

                function refreshTaskMarkers() {
                    taskMarkers = backend.taskMarkers();
                }

                // Rows are updated in place rather than replaced, so typing
                // near an image moves it without loading it again.
                function refreshImages() {
                    var images = backend.imageEmbeds();
                    for (var i = 0; i < images.length; ++i) {
                        if (i < imageModel.count)
                            imageModel.set(i, images[i]);
                        else
                            imageModel.append(images[i]);
                    }
                    if (imageModel.count > images.length)
                        imageModel.remove(images.length, imageModel.count - images.length);
                }

                // The blocks Backend can draw as themselves: tables today.
                // Replaced whole because a table's rows and measure change
                // together with the text.
                function refreshRenderedBlocks() {
                    renderedBlocks = backend.renderedBlocks();
                }

                // The widest an image is drawn: the writing column at full
                // width. A narrower window scales images down to fit.
                readonly property int imageAreaWidth:
                    Math.round(writerFontMetrics.averageCharacterWidth * 65)
                onImageAreaWidthChanged: {
                    backend.setImageAreaWidth(imageAreaWidth);
                    refreshImages();
                }

                // Rendered blocks are measured against the editor's own width,
                // which can be narrower than imageAreaWidth in a small window.
                onWidthChanged: {
                    backend.setTableAreaWidth(Math.round(width));
                    refreshRenderedBlocks();
                }

                // Block geometry comes from Backend, so a font or size change
                // has to ask for it again after the document is remeasured.
                property var renderedBlocks: []

                ListModel {
                    id: imageModel
                }

                function toggleTaskAtCursor() {
                    if (backend.toggleTaskAt(cursorPosition))
                        forceActiveFocus();
                }

                function wrapSelection(marker) {
                    forceActiveFocus();
                    EditorMutations.toggleWrap(editor, marker,
                                               backend.emphasisSpanAt(cursorPosition, marker));
                }

                function insertLink() {
                    forceActiveFocus();
                    var start = selectionStart;
                    var end = selectionEnd;
                    var selected = text.slice(start, end);
                    var url = backend.clipboardUrl();
                    var label = selected.length > 0 ? selected : "link text";
                    var markdown = markdownLink(label, url.length > 0 ? url : "https://");
                    var labelLength = escapeMarkdownLinkText(label).length;
                    if (selected.length === 0) {
                        EditorMutations.replaceRange(editor, start, end, markdown,
                                                     1, 1 + labelLength);
                    } else if (url.length === 0) {
                        EditorMutations.replaceRange(editor, start, end, markdown,
                                                     labelLength + 3,
                                                     markdown.length - 1);
                    } else {
                        EditorMutations.replaceRange(editor, start, end, markdown);
                    }
                }

                function smartReturn(softBreak) {
                    if (softBreak) {
                        replaceSelectionWith("\n");
                        return;
                    }
                    var lineStart = text.lastIndexOf("\n", cursorPosition - 1) + 1;
                    var line = text.slice(lineStart, cursorPosition);
                    if (backend.insideFence(cursorPosition)) {
                        replaceSelectionWith("\n");
                        return;
                    }
                    var match = line.match(/^(\s*)([-+*]|\d+[.)]|>+)\s+(.*)$/);
                    if (match) {
                        var marker = match[2];
                        if (/^\d/.test(marker))
                            marker = (parseInt(marker) + 1) + marker.slice(-1);
                        // A task carries an unchecked checkbox to the next
                        // line, and one left empty ends the list like an empty
                        // item does.
                        var task = match[3].match(/^\[[ xX]\](?:[ \t]+(.*)|$)/);
                        if (task) {
                            if (!task[1] || /^\s*$/.test(task[1])) {
                                EditorMutations.replaceRange(editor, lineStart,
                                                             cursorPosition, "\n");
                            } else {
                                replaceSelectionWith("\n" + match[1] + marker + " [ ] ");
                            }
                            return;
                        }
                        if (match[3].length === 0) {
                            EditorMutations.replaceRange(editor, lineStart,
                                                         cursorPosition, "\n");
                        } else {
                            replaceSelectionWith("\n" + match[1] + marker + " ");
                        }
                        return;
                    }
                    // A new paragraph stands apart from the one above it by a
                    // blank line, which is the second break here. A line that
                    // is already blank has nothing to stand apart from, so
                    // that break would only be a gap nobody asked for.
                    // The break lands on what the selection leaves behind, which
                    // is not the caret's line when it was dragged right to left.
                    var start = selectionStart;
                    var end = selectionEnd;
                    var head = text.slice(text.lastIndexOf("\n", start - 1) + 1, start);
                    var lineEnd = text.indexOf("\n", end);
                    var rest = lineEnd < 0 ? text.slice(end)
                                           : text.slice(end, lineEnd);
                    if (/^\s*$/.test(head) && /^\s*$/.test(rest)) {
                        replaceSelectionWith("\n");
                        return;
                    }
                    replaceSelectionWith("\n\n");
                }

                // Tab nests the list items the caret or selection touches four
                // spaces deeper, and Shift+Tab lifts them back out by the same
                // step. Lines that are neither items, their indented
                // continuations, nor blank, and code fences, leave Tab alone.
                function indentListItems(outdent) {
                    // "* * *" and "- - -" are rules, not items.
                    var listItem = function(line) {
                        return /^[ \t]*(?:[-+*]|\d+[.)])[ \t]/.test(line)
                            && !/^ {0,3}([-*])([ \t]*\1){2,}[ \t]*$/.test(line);
                    };
                    // A tab runs to the next four-column stop.
                    var columns = function(line) {
                        return line.match(/^[ \t]*/)[0].split("").reduce(function(column, c) {
                            return c === "\t" ? column + 4 - column % 4 : column + 1;
                        }, 0);
                    };

                    // A selection that ends at the start of a line leaves it be.
                    // lastIndexOf would read -1 as 0, a newline right there.
                    var start = selectionStart > 0 ? text.lastIndexOf("\n", selectionStart - 1) + 1 : 0;
                    var end = text.indexOf("\n", Math.max(start, selectionEnd - 1));
                    if (end < 0)
                        end = text.length;
                    var lines = text.slice(start, end).split("\n");
                    // The selection starts at an item and holds only lines that
                    // belong to one: other items, blanks, and indented lines.
                    // A fence indented under an item moves along with it whole;
                    // a selection that starts or ends inside code would split it.
                    var filled = lines.filter(function(line) { return /\S/.test(line); });
                    if (!listItem(filled[0] || "")
                        || filled.some(function(line) { return /^\S/.test(line) && !listItem(line); })
                        || backend.insideFence(start)
                        || backend.insideFence(end))
                        return false;

                    // Every line moves by the same step, so the nesting within
                    // the selection is kept. Four spaces nest under any marker
                    // up to "99.", and Shift+Tab lifts no further than the
                    // shallowest line can go.
                    var step = outdent ? -Math.min.apply(null, [4].concat(filled.map(columns))) : 4;
                    if (step === 0)
                        return true;
                    var moved = lines.map(function(line) {
                        var lead = line.match(/^[ \t]*/)[0];
                        return /\S/.test(line)
                            ? " ".repeat(columns(line) + step) + line.slice(lead.length)
                            : line;
                    });
                    var nested = moved.join("\n");
                    var from = Math.max(start, selectionStart + moved[0].length - lines[0].length);
                    // Neither end of the selection leaves its line, even when
                    // the indent it sat in is gone.
                    var lastStart = start + nested.length - moved[moved.length - 1].length;
                    var to = Math.max(from, lastStart, selectionEnd + nested.length - (end - start));
                    var forward = cursorPosition === selectionEnd;
                    replaceText(start, end, nested);
                    select(forward ? from : to, forward ? to : from);
                    return true;
                }

                function escapeMarkdownLinkText(linkText) {
                    return linkText.replace(/\\/g, "\\\\")
                                   .replace(/\[/g, "\\[")
                                   .replace(/\]/g, "\\]");
                }

                function escapeMarkdownLinkDestination(linkUrl) {
                    return linkUrl.replace(/\\/g, "\\\\")
                                  .replace(/\(/g, "\\(")
                                  .replace(/\)/g, "\\)");
                }

                function markdownLink(label, url) {
                    return "[" + escapeMarkdownLinkText(label) + "]("
                        + escapeMarkdownLinkDestination(url) + ")";
                }

                function pasteClipboardUrlAsMarkdownLink() {
                    var start = selectionStart;
                    var end = selectionEnd;
                    if (start === end)
                        return false;

                    var url = backend.clipboardUrl();
                    if (url === "")
                        return false;

                    var selected = text.slice(start, end);
                    var leading = selected.match(/^\s*/)[0];
                    var trailing = selected.match(/\s*$/)[0];
                    var linkText = selected.slice(leading.length,
                                                  selected.length - trailing.length);
                    if (linkText === "")
                        return false;

                    replaceSelectionWith(leading + markdownLink(linkText, url) + trailing);
                    return true;
                }

                // An image on the clipboard is saved beside the notes and
                // embedded on a line of its own, where it is drawn.
                function pasteClipboardImage() {
                    var result = backend.pasteClipboardImage();
                    if (!result.handled)
                        return false;
                    if (result.text.length === 0)
                        return true;

                    var start = selectionStart;
                    var end = selectionEnd;
                    var before = start > 0 ? text.charAt(start - 1) : "\n";
                    var after = end < length ? text.charAt(end) : "\n";
                    replaceSelectionWith((before === "\n" ? "" : "\n") + result.text
                                         + (after === "\n" ? "" : "\n"));
                    return true;
                }

                function pasteClipboardAsPlainText() {
                    var pastedText = backend.clipboardText();
                    if (pastedText.length > 0)
                        replaceSelectionWith(pastedText);
                }

                function moveCursorVisibly(direction) {
                    if (selectionStart !== selectionEnd) {
                        cursorPosition = direction > 0 ? selectionEnd : selectionStart;
                        return;
                    }

                    var pos = backend.cursorPositionStep(cursorPosition, direction);
                    cursorPosition = Math.max(0, Math.min(length, pos));
                }

                function movePage(direction, extendSelection) {
                    var pageStep = Math.max(win.editorFontPixelSize,
                                            editorFlick.height - win.editorFontPixelSize * 2);
                    var rect = cursorRectangle;
                    var targetY = rect.y + rect.height / 2 + direction * pageStep;
                    var target = positionAt(rect.x, Math.max(0, targetY));
                    if (extendSelection)
                        moveCursorSelection(target, TextEdit.SelectCharacters);
                    else
                        cursorPosition = target;
                }

                function deleteParagraphBreakBehindCursor() {
                    if (selectionStart !== selectionEnd || cursorPosition < 2)
                        return false;

                    // getText reads just the two characters, where text would
                    // copy the whole document on every Backspace.
                    if (getText(cursorPosition - 2, cursorPosition) !== "\n\n")
                        return false;

                    var start = cursorPosition - 2;
                    var lineEnd = text.indexOf("\n", cursorPosition);
                    var line = lineEnd < 0 ? text.slice(cursorPosition)
                                           : text.slice(cursorPosition, lineEnd);
                    // Something on the caret's line and the pair above it is a
                    // paragraph break and nothing else, either the one Return
                    // wrote to end a paragraph or the one the writer is now
                    // closing to join what it separates. Both breaks go.
                    if (/^\s*$/.test(line)) {
                        // On a blank line Return writes a single break, so up
                        // here the pair may be that break and one that was
                        // already in the document. Above a blank line Return
                        // writes a single break too, and so wrote neither of
                        // these.
                        var above = text.slice(text.lastIndexOf("\n", start - 1) + 1, start);
                        if (/^\s*$/.test(above))
                            return false;
                        // Past that the two Returns leave the same text and the
                        // same caret, and no reading of either says which was
                        // pressed. What decides instead is that the gap is left
                        // standing: both breaks go only while a blank line of
                        // it survives them. The end of the document below the
                        // caret leaves it none...
                        if (lineEnd < 0)
                            return false;
                        // ...and so does the next paragraph, which the pair
                        // would otherwise be pulled up against.
                        var belowEnd = text.indexOf("\n", lineEnd + 1);
                        var below = belowEnd < 0 ? text.slice(lineEnd + 1)
                                                 : text.slice(lineEnd + 1, belowEnd);
                        if (!/^\s*$/.test(below))
                            return false;
                    }

                    remove(start, cursorPosition);
                    cursorPosition = start;
                    return true;
                }

                Keys.priority: Keys.BeforeItem
                Keys.onPressed: function(event) {
                    if (wikiPopup.visible) {
                        if (event.key === Qt.Key_Down) {
                            wikiPopup.moveSelection(1);
                            event.accepted = true;
                            return;
                        }
                        if (event.key === Qt.Key_Up) {
                            wikiPopup.moveSelection(-1);
                            event.accepted = true;
                            return;
                        }
                        if (event.key === Qt.Key_Return || event.key === Qt.Key_Enter
                                || event.key === Qt.Key_Tab) {
                            win.acceptWikiSuggestion(wikiPopup.currentIndex);
                            event.accepted = true;
                            return;
                        }
                        if (event.key === Qt.Key_Escape) {
                            win.closeWikiPopup();
                            event.accepted = true;
                            return;
                        }
                    }

                    var pasteKey = (event.key === Qt.Key_V)
                        && (event.modifiers & Qt.ControlModifier)
                        && !(event.modifiers & (Qt.AltModifier | Qt.MetaModifier | Qt.ShiftModifier));
                    var shiftInsert = (event.key === Qt.Key_Insert)
                        && (event.modifiers & Qt.ShiftModifier)
                        && !(event.modifiers & (Qt.ControlModifier | Qt.AltModifier | Qt.MetaModifier));
                    if (pasteKey || shiftInsert) {
                        if (!pasteClipboardImage() && !pasteClipboardUrlAsMarkdownLink())
                            pasteClipboardAsPlainText();
                        event.accepted = true;
                        return;
                    }

                    var pageKey = event.key === Qt.Key_PageDown || event.key === Qt.Key_PageUp;
                    var documentEdgeKey = (event.key === Qt.Key_Home || event.key === Qt.Key_End)
                        && (event.modifiers & Qt.ControlModifier);
                    if (pageKey || documentEdgeKey)
                        scrollLinger.restart();

                    var returnKey = event.key === Qt.Key_Return || event.key === Qt.Key_Enter;
                    var commandModifier = event.modifiers & (Qt.ControlModifier | Qt.AltModifier | Qt.MetaModifier);
                    if (returnKey && !commandModifier) {
                        smartReturn(event.modifiers & Qt.ShiftModifier);
                        event.accepted = true;
                    } else if (!commandModifier && event.key === Qt.Key_Backspace
                               && deleteParagraphBreakBehindCursor()) {
                        event.accepted = true;
                    } else if (!commandModifier && !(event.modifiers & Qt.ShiftModifier)
                               && event.key === Qt.Key_Right) {
                        moveCursorVisibly(1);
                        event.accepted = true;
                    } else if (!commandModifier && !(event.modifiers & Qt.ShiftModifier)
                               && event.key === Qt.Key_Left) {
                        moveCursorVisibly(-1);
                        event.accepted = true;
                    } else if (!commandModifier
                               && (event.key === Qt.Key_Tab || event.key === Qt.Key_Backtab)
                               && indentListItems(event.key === Qt.Key_Backtab
                                                  || (event.modifiers & Qt.ShiftModifier))) {
                        event.accepted = true;
                    } else if (!commandModifier
                               && (event.key === Qt.Key_PageDown || event.key === Qt.Key_PageUp)) {
                        movePage(event.key === Qt.Key_PageDown ? 1 : -1,
                                 event.modifiers & Qt.ShiftModifier);
                        event.accepted = true;
                    }
                }

                onTextChanged: {
                    hoveredLinkUrl = "";
                    refreshTaskMarkers();
                    refreshImages();
                    refreshRenderedBlocks();
                    // The document re-lays out after this signal; remeasure
                    // the checkboxes once it has.
                    Qt.callLater(function() { caretRevision++; });
                    if (win.searchUpdating)
                        return;
                    var contentChanged = backend.editorTextChanged();
                    if (win.searchOpen && contentChanged)
                        win.updateSearch(false);
                    win.refreshWikiPopup();
                }

                Text {
                    anchors.left: parent.left
                    anchors.top: parent.top
                    text: "# Start writing"
                    visible: editor.length === 0 && !editor.activeFocus
                    color: win.mutedColor
                    font.family: editor.font.family
                    font.pixelSize: editor.font.pixelSize
                    font.weight: editor.font.weight
                }

                property string hoveredLinkUrl: ""
                property real linkHoverX: 0
                property real linkHoverY: 0
                // Task markers in document order, and a counter that changes
                // after the caret restyles a line so their overlays remeasure.
                property var taskMarkers: []
                property int caretRevision: 0

                // positionAt() snaps to the nearest caret, even past the end of a
                // line, so only count a character the pointer is inside: the one
                // on either side of that caret, whichever way the text runs.
                function linkUrlUnder(x, y) {
                    var caret = positionAt(x, y);
                    for (var position = Math.max(0, caret - 1);
                         position <= caret && position < length; ++position) {
                        var box = positionToRectangle(position);
                        var next = positionToRectangle(position + 1);
                        // A hidden marker has no width; only a line's last
                        // character needs an estimate.
                        var edge = next.y === box.y
                            ? next.x : box.x + writerFontMetrics.averageCharacterWidth;
                        if (y >= box.y && y < box.y + box.height
                                && x >= Math.min(box.x, edge) && x < Math.max(box.x, edge))
                            return backend.linkUrlAt(position);
                    }
                    return "";
                }

                // TextEdit takes the pointer for selection, so a plain
                // TapHandler never sees Ctrl+click. This MouseArea consumes
                // only the press that lands on a wikilink and lets every other
                // press through for the caret.
                MouseArea {
                    anchors.fill: parent
                    acceptedButtons: Qt.LeftButton
                    hoverEnabled: true
                    propagateComposedEvents: true
                    preventStealing: true
                    cursorShape: Qt.IBeamCursor
                    onPositionChanged: (mouse) => {
                        editor.linkHoverX = mouse.x;
                        editor.linkHoverY = mouse.y;
                        editor.hoveredLinkUrl = editor.linkUrlUnder(mouse.x, mouse.y);
                        var position = editor.positionAt(mouse.x, mouse.y);
                        cursorShape = (mouse.modifiers & Qt.ControlModifier)
                            && (backend.wikiLinkAt(position).kind !== "none"
                                || backend.linkUrlAt(position).length > 0
                                || backend.imageUrlAt(position).toString().length > 0)
                            ? Qt.PointingHandCursor : Qt.IBeamCursor;
                    }
                    onExited: {
                        editor.hoveredLinkUrl = "";
                        cursorShape = Qt.IBeamCursor;
                    }
                    onPressed: (mouse) => {
                        if ((mouse.modifiers & Qt.ControlModifier)
                                && win.followLinkAt(editor.positionAt(mouse.x, mouse.y))) {
                            mouse.accepted = true;
                            return;
                        }
                        mouse.accepted = false;
                    }
                }

                ToolTip {
                    visible: editor.hoveredLinkUrl.length > 0
                    x: editor.linkHoverX + 12
                    y: editor.linkHoverY + 20
                    contentItem: Label {
                        objectName: "linkTooltipText"
                        // A destination is document text; AutoText would render
                        // one that looks like HTML instead of showing it.
                        textFormat: Text.PlainText
                        text: editor.hoveredLinkUrl
                        font.family: backend.editorFont
                        font.pixelSize: win.scaledSize(12)
                        elide: Text.ElideMiddle
                    }
                    width: Math.min(implicitWidth, win.width - 32)
                }

                // A task item's "[ ]" is hidden by the highlighter and a
                // checkbox is drawn over the width it reserved. The raw
                // marker shows instead while the caret is inside it, so it
                // can still be edited like any other Markdown marker.
                Repeater {
                    model: editor.taskMarkers

                    delegate: Item {
                        id: taskCheckbox
                        objectName: "taskCheckbox"
                        required property var modelData

                        readonly property rect markerRect: {
                            // The highlighter restyles the line before this
                            // counter changes, so the marker has its final
                            // width when it is measured here. contentHeight
                            // catches the layout settling after an edit.
                            var revision = editor.caretRevision;
                            var contentHeight = editor.contentHeight;
                            var contentWidth = editor.contentWidth;
                            var fontFamily = editor.font.family;
                            var left = editor.positionToRectangle(modelData.position);
                            var right = editor.positionToRectangle(modelData.position + 3);
                            var width = editor.width;
                            var pixelSize = editor.font.pixelSize;
                            if (right.x <= left.x || right.y !== left.y) {
                                return Qt.rect(left.x, left.y,
                                               Math.max(10, Math.round(pixelSize * 0.72)),
                                               left.height);
                            }
                            return Qt.rect(left.x, left.y, right.x - left.x, left.height);
                        }
                        readonly property int boxSize: Math.max(8, Math.round(Math.min(
                            markerRect.width, markerRect.height * 0.62)))
                        x: Math.round(markerRect.x + (markerRect.width - width) / 2)
                        y: Math.round(markerRect.y + (markerRect.height - height) / 2)
                        width: boxSize + 8
                        height: boxSize + 8
                        visible: editor.cursorPosition < modelData.position
                                 || editor.cursorPosition >= modelData.position + 3
                        z: 1

                        Rectangle {
                            anchors.centerIn: parent
                            width: taskCheckbox.boxSize
                            height: taskCheckbox.boxSize
                            radius: Math.max(1, Math.round(taskCheckbox.boxSize * 0.15))
                            color: taskCheckbox.modelData.checked ? backend.themeAccent
                                                                  : "transparent"
                            border.width: Math.max(1, Math.round(win.textScale))
                            border.color: taskCheckbox.modelData.checked ? backend.themeAccent
                                                                         : win.mutedColor

                            Canvas {
                                anchors.fill: parent
                                visible: taskCheckbox.modelData.checked
                                onVisibleChanged: if (visible) requestPaint()
                                onPaint: {
                                    var context = getContext("2d");
                                    context.clearRect(0, 0, width, height);
                                    context.strokeStyle = win.pageColor;
                                    context.lineWidth = Math.max(1.5, width * 0.15);
                                    context.lineCap = "round";
                                    context.lineJoin = "round";
                                    context.beginPath();
                                    context.moveTo(width * 0.26, height * 0.52);
                                    context.lineTo(width * 0.43, height * 0.72);
                                    context.lineTo(width * 0.76, height * 0.30);
                                    context.stroke();
                                }
                            }
                        }

                        MouseArea {
                            anchors.fill: parent
                            hoverEnabled: true
                            cursorShape: Qt.PointingHandCursor
                            onClicked: {
                                editor.forceActiveFocus();
                                backend.toggleTaskAt(taskCheckbox.modelData.position);
                            }
                        }
                    }
                }

                // An image line's text is hidden while the caret is away, and
                // the image is drawn in the room Backend reserved below it.
                Repeater {
                    objectName: "imagePreviews"
                    model: imageModel

                    delegate: Image {
                        id: imagePreview
                        objectName: "imagePreview"
                        required property int blockStart
                        required property int blockEnd
                        required property url imageUrl
                        required property int imageWidth
                        required property int imageHeight
                        required property int imagePad

                        readonly property rect lineRect: {
                            var revision = editor.caretRevision;
                            var contentHeight = editor.contentHeight;
                            var editorWidth = editor.width;
                            return editor.positionToRectangle(blockEnd);
                        }
                        x: 0
                        y: Math.round(lineRect.y + lineRect.height + imagePad)
                        width: Math.min(imageWidth, editor.width)
                        height: Math.round(width * imageHeight / Math.max(1, imageWidth))
                        source: imageUrl
                        sourceSize.width: Math.round(imageWidth * Screen.devicePixelRatio)
                        fillMode: Image.PreserveAspectFit
                        asynchronous: true
                        smooth: true
                        mipmap: true

                        MouseArea {
                            anchors.fill: parent
                            hoverEnabled: true
                            cursorShape: Qt.PointingHandCursor
                            // A click opens the picture full-window; Ctrl+click
                            // still hands it to the system viewer. Closing the
                            // viewer leaves the caret on the embed for editing.
                            onClicked: (mouse) => {
                                if (mouse.modifiers & Qt.ControlModifier) {
                                    backend.openImage(imagePreview.imageUrl);
                                    return;
                                }
                                win.openImageViewer(imagePreview.blockStart);
                            }
                        }
                    }
                }

                // A table's raw lines are hidden by the highlighter while the
                // caret is outside it, and Backend has reserved the drawn
                // table's height below the source lines. This draws the table
                // in that room. The same list carries the seam for later
                // blocks: a kind gets a delegate here.
                Repeater {
                    objectName: "renderedBlockOverlays"
                    model: editor.renderedBlocks

                    delegate: Rectangle {
                        id: tableOverlay
                        objectName: "tableOverlay"
                        required property var modelData
                        required property int index

                        readonly property var block: modelData
                        readonly property bool editing:
                            editor.cursorPosition >= block.blockStart
                            && editor.cursorPosition <= block.blockEnd
                        readonly property rect anchor: {
                            // The document re-lays out after a style change;
                            // these make the overlay measure once it has.
                            var revision = editor.caretRevision;
                            var contentHeight = editor.contentHeight;
                            return editor.positionToRectangle(block.blockStart);
                        }

                        visible: block.kind === "table" && !editing
                        x: 0
                        y: Math.round(anchor.y)
                        width: block.kind === "table" ? block.width : 0
                        height: block.kind === "table" ? block.height : 0
                        color: "transparent"
                        border.width: 1
                        border.color: win.darkMode ? "#3f3f3a" : "#d9d8c8"
                        radius: win.scaledSize(3)
                        clip: true
                        z: 1

                        Column {
                            anchors.fill: parent

                            Repeater {
                                model: tableOverlay.block.rows

                                delegate: Item {
                                    id: tableRow
                                    objectName: "tableOverlayRow"
                                    required property var modelData

                                    width: tableOverlay.width
                                    height: tableRow.modelData.height

                                    Rectangle {
                                        anchors.fill: parent
                                        visible: tableRow.modelData.header
                                        color: win.darkMode ? "#14ffffff" : "#08000000"
                                    }

                                    Row {
                                        anchors.fill: parent

                                        Repeater {
                                            model: tableRow.modelData.cells

                                            delegate: Text {
                                                required property var modelData
                                                required property int index

                                                width: tableOverlay.block.columnWidths[index]
                                                height: tableRow.height
                                                text: modelData
                                                textFormat: Text.PlainText
                                                wrapMode: Text.WrapAtWordBoundaryOrAnywhere
                                                verticalAlignment: Text.AlignVCenter
                                                leftPadding: tableOverlay.block.cellPadding
                                                rightPadding: tableOverlay.block.cellPadding
                                                color: win.textColor
                                                font.family: backend.editorFont
                                                font.pixelSize: win.editorFontPixelSize
                                                font.bold: tableRow.modelData.header
                                                horizontalAlignment: {
                                                    var alignment =
                                                        tableOverlay.block.alignments[index];
                                                    return alignment === "center" ? Text.AlignHCenter
                                                        : alignment === "right" ? Text.AlignRight
                                                        : Text.AlignLeft;
                                                }
                                            }
                                        }
                                    }

                                    Rectangle {
                                        anchors.left: parent.left
                                        anchors.right: parent.right
                                        anchors.bottom: parent.bottom
                                        height: 1
                                        color: win.darkMode ? "#2e2e2a" : "#e4e2d4"
                                    }
                                }
                            }
                        }
                    }
                }

                Component.onCompleted: {
                    backend.setImageAreaWidth(imageAreaWidth);
                    backend.setTableAreaWidth(Math.round(width));
                    backend.attachDocument(textDocument);
                    refreshTaskMarkers();
                    refreshImages();
                    refreshRenderedBlocks();
                    forceActiveFocus();
                }
            }
        }

        // [[note]] suggestions, kept outside the Flickable so its clipping
        // cannot cut the list off, and keyboard-driven so the caret never
        // leaves the document.
        Rectangle {
            id: wikiPopup
            objectName: "wikiPopup"
            visible: false
            z: 60
            width: win.scaledSize(420)
            height: Math.min(win.scaledSize(300),
                             Math.max(1, suggestions.length) * rowHeight + 8)
            color: win.darkMode ? "#22221f" : "#fffef2"
            border.color: win.darkMode ? "#4f525a" : "#d9d8c8"
            border.width: 1
            radius: 9
            clip: true

            property var suggestions: []
            property int currentIndex: 0
            property int replaceStart: -1
            property string query: ""
            readonly property int rowHeight: win.scaledSize(34)

            function moveSelection(delta) {
                if (suggestions.length === 0)
                    return;
                currentIndex = (currentIndex + delta + suggestions.length)
                               % suggestions.length;
                list.positionViewAtIndex(currentIndex, ListView.Contain);
            }

            ListView {
                id: list
                anchors.fill: parent
                anchors.margins: 4
                clip: true
                model: wikiPopup.suggestions
                boundsBehavior: Flickable.StopAtBounds
                delegate: Rectangle {
                    id: row
                    required property var modelData
                    required property int index
                    width: ListView.view.width
                    height: wikiPopup.rowHeight
                    radius: 6
                    color: index === wikiPopup.currentIndex
                        ? (win.darkMode ? "#35352f" : "#efeedc")
                        : "transparent"

                    RowLayout {
                        anchors.fill: parent
                        anchors.leftMargin: 8
                        anchors.rightMargin: 8
                        spacing: 8

                        Text {
                            Layout.fillWidth: true
                            text: row.modelData.name
                            color: win.textColor
                            font.family: backend.editorFont
                            font.pixelSize: win.scaledSize(15)
                            elide: Text.ElideRight
                        }

                        Text {
                            readonly property string folder: {
                                var path = row.modelData.path;
                                var slash = path.lastIndexOf("/");
                                return slash >= 0 ? path.slice(0, slash) : "";
                            }
                            visible: folder !== ""
                            text: folder
                            color: win.mutedColor
                            font.family: backend.editorFont
                            font.pixelSize: win.scaledSize(12)
                            elide: Text.ElideRight
                            Layout.maximumWidth: wikiPopup.width * 0.4
                        }
                    }

                    MouseArea {
                        anchors.fill: parent
                        hoverEnabled: true
                        onEntered: wikiPopup.currentIndex = row.index
                        onClicked: win.acceptWikiSuggestion(row.index)
                    }
                }
            }
        }

        Row {
            id: footerStatus
            anchors.left: parent.left
            anchors.bottom: parent.bottom
            anchors.leftMargin: 12
            anchors.bottomMargin: 10
            spacing: 12
            opacity: 0.55

            FooterIconButton {
                objectName: "historyBackButton"
                iconName: "back"
                iconColor: win.mutedColor
                tooltip: "Back to previous file (" + win.seq("back") + ")"
                opacity: win.canGoBack ? 1.0 : 0.35
                onClicked: win.stepHistory(-1)
            }

            FooterIconButton {
                objectName: "historyForwardButton"
                iconName: "forward"
                iconColor: win.mutedColor
                tooltip: "Forward to next file (" + win.seq("forward") + ")"
                opacity: win.canGoForward ? 1.0 : 0.35
                onClicked: win.stepHistory(1)
            }

            FooterIconButton {
                objectName: "saveButton"
                iconName: "save"
                iconColor: win.mutedColor
                tooltip: "Save (" + win.seq("save") + ")"
                onClicked: win.runAction("save")
            }

            FooterIconButton {
                objectName: "openButton"
                iconName: "open"
                iconColor: win.mutedColor
                tooltip: "Open file (" + win.seq("open-file") + ")"
                onClicked: win.runAction("open-file")
            }

            FooterIconButton {
                objectName: "quickOpenButton"
                iconName: "search"
                iconColor: win.mutedColor
                tooltip: "Open other file (" + win.seq("quick-open") + ")"
                onClicked: win.runAction("quick-open")
            }

            FooterIconButton {
                id: fontButton
                objectName: "fontButton"
                iconName: "font"
                iconColor: win.mutedColor
                tooltip: "Font"
                onClicked: win.runAction("font")
            }

            FooterIconButton {
                objectName: "settingsButton"
                iconName: "settings"
                iconColor: win.mutedColor
                tooltip: "Settings"
                onClicked: win.runAction("settings")
            }

            Label {
                text: backend.status
                color: win.mutedColor
                font.family: "IBM Plex Mono"
                font.pixelSize: win.scaledSize(11)
                visible: text !== ""
                elide: Text.ElideRight
                width: Math.min(360, win.width / 3)
                height: win.scaledSize(16)
                verticalAlignment: Text.AlignVCenter
            }
        }

        FontPicker {
            id: fontPicker
            objectName: "fontPicker"
            // Parented to its button, so a press on the button is not a press
            // outside that closes it only for the click to open it again.
            parent: fontButton
            x: -8
            y: -height - 10
            width: win.scaledSize(320)
            maximumHeight: Math.min(win.scaledSize(440), win.height - 80)
            fonts: backend.availableFonts
            currentFont: backend.editorFont
            defaultFont: "IBM Plex Mono"
            darkMode: win.darkMode
            textScale: win.textScale
            textColor: win.textColor
            mutedColor: win.mutedColor
            highlightColor: backend.themeAccent
            onFontChosen: function(family) { backend.editorFont = family; }
            onClosed: editor.forceActiveFocus()
        }

        Label {
            anchors.right: parent.right
            anchors.bottom: parent.bottom
            anchors.rightMargin: 12
            anchors.bottomMargin: 10
            text: backend.wordCount + (backend.wordCount === 1 ? " Word" : " Words")
            color: win.mutedColor
            opacity: 0.75
            font.family: "IBM Plex Mono"
            font.pixelSize: win.scaledSize(11)
        }


        Pane {
            anchors.top: parent.top
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.topMargin: 12
            anchors.leftMargin: 12
            anchors.rightMargin: 12
            height: win.scaledSize(win.replaceOpen ? 104 : 56)
            visible: win.searchOpen
            z: 10
            leftPadding: 16
            rightPadding: 8
            topPadding: 0
            bottomPadding: 0
            Material.elevation: 8
            // Reaches here from either field or the buttons beside them.
            Keys.onEscapePressed: function(event) {
                win.closeSearch();
                event.accepted = true;
            }

            background: Rectangle {
                radius: 9
                color: win.darkMode ? "#22221f" : "#fffef2"
            }

            RowLayout {
                anchors.fill: parent
                spacing: 8

                Item {
                    Layout.fillWidth: true
                    Layout.fillHeight: true

                    TextInput {
                        id: searchField
                        objectName: "searchField"
                        anchors.left: parent.left
                        anchors.right: parent.right
                        anchors.top: parent.top
                        height: win.replaceOpen ? parent.height / 2 : parent.height
                        verticalAlignment: TextInput.AlignVCenter
                        selectByMouse: true
                        activeFocusOnTab: true
                        color: win.textColor
                        selectionColor: win.selectionFill
                        selectedTextColor: win.selectionTextColor
                        font.pixelSize: win.scaledSize(17)
                        clip: true
                        onTextChanged: searchDelay.restart()
                        Keys.onReturnPressed: function(event) {
                            win.moveSearch((event.modifiers & Qt.ShiftModifier) ? -1 : 1);
                            event.accepted = true;
                        }
                    }

                    TextInput {
                        id: replaceField
                        objectName: "replaceField"
                        anchors.left: parent.left
                        anchors.right: parent.right
                        anchors.bottom: parent.bottom
                        height: parent.height / 2
                        visible: win.replaceOpen
                        verticalAlignment: TextInput.AlignVCenter
                        selectByMouse: true
                        activeFocusOnTab: true
                        clip: true
                        color: win.textColor
                        selectionColor: win.selectionFill
                        selectedTextColor: win.selectionTextColor
                        font.pixelSize: win.scaledSize(17)
                        Keys.onReturnPressed: replaceCurrentButton.clicked()
                    }

                    Label {
                        anchors.verticalCenter: replaceField.verticalCenter
                        text: "Replace with"
                        visible: win.replaceOpen && replaceField.text.length === 0
                        color: win.mutedColor
                        font.pixelSize: win.scaledSize(17)
                    }

                    Label {
                        anchors.verticalCenter: searchField.verticalCenter
                        text: "Find"
                        visible: searchField.text.length === 0
                        color: win.mutedColor
                        font.pixelSize: win.scaledSize(17)
                    }
                }

                Label {
                    Layout.preferredWidth: win.scaledSize(58)
                    Layout.fillHeight: true
                    horizontalAlignment: Text.AlignHCenter
                    verticalAlignment: Text.AlignVCenter
                    text: win.searchMatches.length === 0
                        ? "0/0"
                        : (win.searchMatchIndex + 1) + "/" + win.searchMatches.length
                    color: win.darkMode ? win.textColor : "#62635f"
                    font.pixelSize: win.scaledSize(16)
                }

                Button {
                    id: replaceCurrentButton
                    objectName: "replaceButton"
                    visible: win.replaceOpen
                    text: "Replace"
                    onClicked: {
                        win.flushSearch();
                        if (win.searchMatchIndex < 0) return;
                        var start = win.searchMatches[win.searchMatchIndex];
                        var replacement = replaceField.text;
                        EditorMutations.replaceRange(editor, start,
                                                     start + searchField.text.length,
                                                     replacement);
                        // Carry on past what was just written, so a replacement
                        // the query also matches is not found again.
                        win.searchMatches = backend.searchMatches(searchField.text);
                        win.searchMatchIndex = win.matchAtOrAfter(start + replacement.length);
                        win.showSearchMatch(true);
                    }
                }

                Button {
                    objectName: "replaceAllButton"
                    visible: win.replaceOpen
                    text: "All"
                    onClicked: {
                        win.flushSearch();
                        var query = searchField.text;
                        var matches = win.searchMatches;
                        if (query.length === 0 || matches.length === 0) return;
                        // One replacement over the span of every match, so a
                        // single undo takes them all back.
                        var first = matches[0];
                        var replaced = "";
                        var position = first;
                        for (var i = 0; i < matches.length; ++i) {
                            replaced += editor.text.slice(position, matches[i]) + replaceField.text;
                            position = matches[i] + query.length;
                        }
                        EditorMutations.replaceRange(editor, first, position, replaced);
                        win.updateSearch(false);
                    }
                }

                Rectangle {
                    Layout.preferredWidth: 1
                    Layout.preferredHeight: 34
                    color: win.darkMode ? "#6f6f62" : "#d5d56e"
                }

                SearchIconButton {
                    iconName: "up"
                    iconColor: win.darkMode ? win.textColor : "#62635f"
                    onClicked: win.moveSearch(-1)
                }

                SearchIconButton {
                    iconName: "down"
                    iconColor: win.darkMode ? win.textColor : "#62635f"
                    onClicked: win.moveSearch(1)
                }

                SearchIconButton {
                    iconName: "close"
                    iconColor: win.darkMode ? win.textColor : "#62635f"
                    onClicked: win.closeSearch()
                }
            }
        }
    }

    // The file viewer. A plain overlay on the left; the page makes room for it.
    // It wears the page color, so the tree reads as part of the sheet rather
    // than a separate panel.
    Rectangle {
        id: sidebar
        objectName: "fileViewer"
        visible: win.sidebarOpen
        z: 50
        anchors.left: parent.left
        anchors.top: parent.top
        anchors.bottom: parent.bottom
        anchors.topMargin: win.footerHeight
        anchors.bottomMargin: win.footerHeight
        width: win.sidebarWidth
        color: win.pageColor
        border.color: win.darkMode ? "#4f525a" : "#d9d8c8"
        border.width: 1

        ColumnLayout {
            anchors.fill: parent
            spacing: 0

            // Vault name, the Links/Folders source switch, and the collapse
            // button that hides the tree back to a distraction-free page.
            RowLayout {
                Layout.fillWidth: true
                Layout.leftMargin: win.scaledSize(14)
                Layout.rightMargin: win.scaledSize(10)
                Layout.topMargin: win.scaledSize(12)
                Layout.bottomMargin: win.scaledSize(8)
                spacing: win.scaledSize(10)

                Text {
                    objectName: "fileViewerRoot"
                    Layout.fillWidth: true
                    text: {
                        var root = win.sidebarOpen ? backend.fileTreeRoot : "";
                        return root.slice(root.lastIndexOf("/") + 1) || root;
                    }
                    color: win.textColor
                    font.family: backend.editorFont
                    font.pixelSize: win.scaledSize(16)
                    elide: Text.ElideRight
                }

                TreeHeaderButton {
                    objectName: "treeModeLinks"
                    label: "Links"
                    active: backend.treeMode === "links"
                    activeColor: win.textColor
                    inactiveColor: win.mutedColor
                    fontFamily: backend.editorFont
                    pixelSize: win.scaledSize(13)
                    onClicked: { backend.treeMode = "links"; win.rebuildTree(); }
                }

                TreeHeaderButton {
                    objectName: "treeModeFolders"
                    label: "Folders"
                    active: backend.treeMode === "folders"
                    activeColor: win.textColor
                    inactiveColor: win.mutedColor
                    fontFamily: backend.editorFont
                    pixelSize: win.scaledSize(13)
                    onClicked: { backend.treeMode = "folders"; win.rebuildTree(); }
                }

                TreeHeaderButton {
                    objectName: "treeCollapse"
                    label: "«"
                    active: false
                    activeColor: win.mutedColor
                    inactiveColor: win.mutedColor
                    fontFamily: backend.editorFont
                    pixelSize: win.scaledSize(18)
                    onClicked: win.toggleSidebar()
                }
            }

            ListView {
                id: sidebarList
                objectName: "fileViewerList"
                Layout.fillWidth: true
                Layout.fillHeight: true
                clip: true
                boundsBehavior: Flickable.StopAtBounds
                model: win.treeRows
                currentIndex: -1
                ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }

                Keys.onReturnPressed: win.activateTreeRow(currentIndex)
                Keys.onEnterPressed: win.activateTreeRow(currentIndex)
                Keys.onRightPressed: win.setTreeFolderOpen(currentIndex, true)
                Keys.onLeftPressed: win.setTreeFolderOpen(currentIndex, false)
                Keys.onEscapePressed: win.toggleSidebar()

                // The default wheel step crawls next to the editor's eased
                // scrolling, so the tree scrolls at the same speed setting.
                WheelHandler {
                    acceptedDevices: PointerDevice.Mouse | PointerDevice.TouchPad
                    onWheel: function(wheel) {
                        var step = win.scaledSize(120) * backend.scrollSpeed;
                        var y = wheel.pixelDelta.y !== 0
                            ? sidebarList.contentY - wheel.pixelDelta.y * backend.scrollSpeed
                            : sidebarList.contentY - wheel.angleDelta.y / 120 * step;
                        var maxY = Math.max(0, sidebarList.contentHeight - sidebarList.height);
                        sidebarList.contentY = Math.max(0, Math.min(maxY, y));
                        wheel.accepted = true;
                    }
                }

                delegate: Rectangle {
                    id: treeRow
                    required property var modelData
                    required property int index
                    readonly property bool current: !modelData.isDir
                        && modelData.path === backend.fileTreeCurrentPath
                    width: ListView.view.width
                    height: win.scaledSize(30)
                    color: ListView.isCurrentItem && sidebarList.activeFocus
                        ? (win.darkMode ? "#44443b" : "#e2e0c4")
                        : (current || treeMouse.containsMouse || treeChevron.containsMouse
                           ? (win.darkMode ? "#35352f" : "#efeedc") : "transparent")

                    Row {
                        anchors.verticalCenter: parent.verticalCenter
                        anchors.left: parent.left
                        anchors.leftMargin: win.scaledSize(10) + modelData.depth * win.scaledSize(16)
                        anchors.right: parent.right
                        anchors.rightMargin: win.scaledSize(8)
                        spacing: win.scaledSize(6)

                        Text {
                            width: win.scaledSize(14)
                            text: (treeRow.modelData.isDir || treeRow.modelData.hasChildren)
                                ? (treeRow.modelData.expanded ? "⌄" : "›") : ""
                            color: win.mutedColor
                            font.pixelSize: win.scaledSize(15)
                        }

                        Text {
                            width: parent.width - win.scaledSize(20)
                                - (treeRow.modelData.childCount > 0 ? win.scaledSize(30) : 0)
                            text: treeRow.modelData.name
                            color: (treeRow.modelData.isDir || treeRow.modelData.hasChildren
                                    || treeRow.current)
                                ? win.textColor : win.mutedColor
                            font.family: backend.editorFont
                            font.pixelSize: win.scaledSize(14)
                            font.bold: treeRow.current
                            elide: Text.ElideRight
                        }

                        Text {
                            visible: treeRow.modelData.childCount > 0
                            text: treeRow.modelData.childCount
                            color: win.mutedColor
                            font.family: backend.editorFont
                            font.pixelSize: win.scaledSize(12)
                        }
                    }

                    MouseArea {
                        id: treeMouse
                        anchors.fill: parent
                        hoverEnabled: true
                        onClicked: {
                            sidebarList.currentIndex = treeRow.index;
                            win.activateTreeRow(treeRow.index);
                        }
                    }

                    // The chevron is the fold control; the rest of the row
                    // belongs to opening the note, children or not.
                    MouseArea {
                        id: treeChevron
                        objectName: "treeChevron" + treeRow.index
                        visible: treeRow.modelData.isDir || treeRow.modelData.hasChildren
                        x: win.scaledSize(7) + treeRow.modelData.depth * win.scaledSize(16)
                        y: 0
                        width: win.scaledSize(22)
                        height: parent.height
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: {
                            sidebarList.currentIndex = treeRow.index;
                            win.toggleTreeRow(treeRow.index);
                        }
                    }
                }
            }

            Rectangle {
                Layout.fillWidth: true
                height: 1
                color: win.darkMode ? "#3a3a33" : "#e3e2d2"
            }

            Text {
                objectName: "fileViewerBrowse"
                Layout.fillWidth: true
                Layout.margins: win.scaledSize(12)
                text: "Open another location…"
                color: win.mutedColor
                font.family: backend.editorFont
                font.pixelSize: win.scaledSize(13)

                MouseArea {
                    anchors.fill: parent
                    anchors.margins: -6
                    cursorShape: Qt.PointingHandCursor
                    onClicked: backend.openDialog()
                }
            }
        }
    }

    // The open notes, along the top where the page leaves a strip free. Only
    // shown once there is more than one, so a lone note stays distraction-free.
    Row {
        id: tabStrip
        objectName: "tabStrip"
        visible: win.tabs.length > 1
        z: 60
        anchors.top: parent.top
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.leftMargin: 24
        anchors.rightMargin: 24
        height: win.footerHeight
        spacing: 2

        Repeater {
            model: win.tabs

            delegate: Rectangle {
                id: tabItem
                required property var modelData
                required property int index
                readonly property bool active: index === win.activeTab
                objectName: "tab" + index
                width: Math.min(win.scaledSize(190),
                                Math.max(win.scaledSize(70),
                                         (tabStrip.width - 2 * win.tabs.length) / win.tabs.length))
                height: tabStrip.height - 4
                y: 4
                radius: 6
                color: active ? (win.darkMode ? "#35352f" : "#efeedc")
                              : (tabMouse.containsMouse ? (win.darkMode ? "#2a2a26" : "#f6f5e6")
                                                        : "transparent")

                Text {
                    anchors.left: parent.left
                    anchors.right: closeLabel.left
                    anchors.verticalCenter: parent.verticalCenter
                    anchors.leftMargin: win.scaledSize(10)
                    text: win.tabTitle(tabItem.modelData.url)
                    color: tabItem.active ? win.textColor : win.mutedColor
                    font.family: backend.editorFont
                    font.pixelSize: win.scaledSize(13)
                    elide: Text.ElideRight
                }

                MouseArea {
                    id: tabMouse
                    anchors.fill: parent
                    hoverEnabled: true
                    acceptedButtons: Qt.LeftButton | Qt.MiddleButton
                    onClicked: function(mouse) {
                        if (mouse.button === Qt.MiddleButton)
                            win.closeTab(tabItem.index);
                        else
                            win.switchTab(tabItem.index);
                    }
                }

                Text {
                    id: closeLabel
                    anchors.right: parent.right
                    anchors.verticalCenter: parent.verticalCenter
                    anchors.rightMargin: win.scaledSize(8)
                    text: "×"
                    visible: tabItem.active || tabMouse.containsMouse
                    color: win.mutedColor
                    font.pixelSize: win.scaledSize(16)

                    MouseArea {
                        anchors.fill: parent
                        anchors.margins: -4
                        onClicked: win.closeTab(tabItem.index)
                    }
                }
            }
        }
    }

    // Obsidian's "Linked mentions": every note that links here, collapsible
    // to the lines that do. A plain overlay so it never reflows the page.
    Rectangle {
        id: backlinksPanel
        objectName: "backlinksPanel"
        visible: win.backlinksOpen
        z: 50
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.bottom: parent.bottom
        anchors.topMargin: win.footerHeight
        anchors.bottomMargin: win.footerHeight
        width: win.backlinksWidth
        color: win.darkMode ? "#22221f" : "#fffef2"
        border.color: win.darkMode ? "#4f525a" : "#d9d8c8"
        border.width: 1

        ColumnLayout {
            anchors.fill: parent
            spacing: 0

            RowLayout {
                Layout.fillWidth: true
                Layout.margins: win.scaledSize(14)
                spacing: win.scaledSize(8)

                Text {
                    text: "Linked mentions"
                    color: win.textColor
                    font.family: backend.editorFont
                    font.pixelSize: win.scaledSize(18)
                }

                Text {
                    objectName: "backlinksCount"
                    text: {
                        var total = 0;
                        for (var i = 0; i < win.backlinkItems.length; ++i)
                            total += win.backlinkItems[i].count;
                        return total;
                    }
                    color: win.mutedColor
                    font.family: backend.editorFont
                    font.pixelSize: win.scaledSize(14)
                }

                Item { Layout.fillWidth: true }
            }

            Text {
                objectName: "backlinksEmpty"
                visible: win.backlinkItems.length === 0
                Layout.fillWidth: true
                Layout.leftMargin: win.scaledSize(14)
                Layout.rightMargin: win.scaledSize(14)
                text: "No other note links here."
                color: win.mutedColor
                font.family: backend.editorFont
                font.pixelSize: win.scaledSize(14)
                wrapMode: Text.WordWrap
            }

            ListView {
                id: backlinksList
                objectName: "backlinksList"
                Layout.fillWidth: true
                Layout.fillHeight: true
                clip: true
                boundsBehavior: Flickable.StopAtBounds
                model: win.backlinkItems
                ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }

                delegate: Column {
                    id: sourceItem
                    required property var modelData
                    readonly property bool collapsed:
                        win.backlinkCollapsed[modelData.path] === true
                    width: ListView.view.width

                    Rectangle {
                        width: parent.width
                        height: win.scaledSize(38)
                        color: sourceMouse.containsMouse
                            ? (win.darkMode ? "#35352f" : "#efeedc") : "transparent"

                        RowLayout {
                            anchors.fill: parent
                            anchors.leftMargin: win.scaledSize(14)
                            anchors.rightMargin: win.scaledSize(14)
                            spacing: win.scaledSize(8)

                            Text {
                                text: sourceItem.collapsed ? "›" : "⌄"
                                color: win.mutedColor
                                font.pixelSize: win.scaledSize(16)
                            }

                            Text {
                                Layout.fillWidth: true
                                text: sourceItem.modelData.name
                                color: win.textColor
                                font.family: backend.editorFont
                                font.pixelSize: win.scaledSize(15)
                                elide: Text.ElideRight
                            }

                            Text {
                                text: sourceItem.modelData.count
                                color: win.mutedColor
                                font.family: backend.editorFont
                                font.pixelSize: win.scaledSize(13)
                            }
                        }

                        MouseArea {
                            id: sourceMouse
                            anchors.fill: parent
                            hoverEnabled: true
                            onClicked: win.toggleBacklinkSource(sourceItem.modelData.path)
                        }
                    }

                    Repeater {
                        model: sourceItem.collapsed ? [] : sourceItem.modelData.mentions

                        delegate: Rectangle {
                            id: mentionRow
                            required property var modelData
                            width: sourceItem.width
                            height: mentionText.implicitHeight + win.scaledSize(14)
                            color: mentionMouse.containsMouse
                                ? (win.darkMode ? "#35352f" : "#efeedc") : "transparent"

                            Text {
                                id: mentionText
                                anchors.left: parent.left
                                anchors.right: parent.right
                                anchors.verticalCenter: parent.verticalCenter
                                anchors.leftMargin: win.scaledSize(36)
                                anchors.rightMargin: win.scaledSize(14)
                                text: mentionRow.modelData.text
                                color: win.mutedColor
                                font.family: backend.editorFont
                                font.pixelSize: win.scaledSize(14)
                                wrapMode: Text.Wrap
                                maximumLineCount: 3
                                elide: Text.ElideRight
                            }

                            MouseArea {
                                id: mentionMouse
                                anchors.fill: parent
                                hoverEnabled: true
                                onClicked: win.openInTab(sourceItem.modelData.url)
                            }
                        }
                    }
                }
            }
        }
    }

    // Above the whole page, so a picture can use the window's width rather
    // than just the writing column's.
    ImageViewer {
        id: imageViewer
        objectName: "imageViewer"
        anchors.fill: parent
        images: win.viewerImages
        index: win.viewerIndex
        fontFamily: backend.editorFont
        textScale: win.textScale
        onStepRequested: function(direction) { win.stepImageViewer(direction) }
        onCloseRequested: win.closeImageViewer()
    }

    Component.onCompleted: {
        syncTabs();
        // Started without a file, there is nothing to look at but a blank
        // page, so lead with the file viewer.
        if (backend.fileUrl.toString() === "")
            toggleSidebar();
        var geometry = backend.windowGeometry();
        if (geometry.x >= 0) x = geometry.x;
        if (geometry.y >= 0) y = geometry.y;
        width = geometry.width;
        height = geometry.height;
        if (geometry.maximized) showMaximized();
    }

    Component.onDestruction: backend.saveWindowGeometry(x, y, width, height, visibility === Window.Maximized)

}
