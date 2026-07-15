# GammaRay MCP Qt Quick Support Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox syntax for tracking.

**Goal:** Add MCP tools that inspect a Qt Quick item tree and capture a QQuickWindow or QQuickItem as PNG, then prove them against `E:\Dev\qml-rhi`.

**Architecture:** `McpServer` consumes the Quick Inspector's existing `QuickItemModel` and `QuickRemoteView`. Item discovery maps each Quick-model `ObjectId` to the established QObject-tree path. Capture selects the Quick model row, requests a remote frame, and returns the full frame or a crop derived from the selected item's `QuickItemGeometry` in `RemoteViewFrame::data`.

**Tech Stack:** C++17, Qt 6.11 Quick, GammaRay ObjectBroker/RemoteModel protocol, CMake/Ninja, PowerShell.

## Global Constraints

- Preserve the existing QWidget capture tools and use only GammaRay's in-process protocol.
- Link `gammaray-mcp` with `gammaray_quickinspector_shared` and `Qt::Quick`.
- Return tool errors for unavailable Quick components, stale/non-Quick paths, invalid frames, zero-area crops, and timeouts.
- Validate with `QT_QPA_PLATFORM=offscreen`, `QSG_RHI_BACKEND=opengl`, and `E:\Dev\qml-rhi\build\qml_rhi.exe`.

---

### Task 1: Add the Quick tool surface

**Files:**
- Modify: `mcp/CMakeLists.txt`
- Modify: `mcp/mcpserver.cpp`
- Modify: `mcp/mcpserver.h`
- Create: `scripts/test-mcp-quick-capture.ps1`

- [ ] Write the failing test that starts the MCP server, calls `tools/list`, and requires `gammaray_list_quick_items`, `gammaray_grab_quick_window`, and `gammaray_grab_quick_item`.
- [ ] Run `powershell -ExecutionPolicy Bypass -File .\scripts\test-mcp-quick-capture.ps1`; expect the first missing tool assertion to fail.
- [ ] Add Quick Inspector constants, three dispatch branches, header declarations, and public MCP schemas.  Both capture schemas use `objectPath`, `maxWidth`, `maxHeight`, and `timeoutMs`.
- [ ] Link `gammaray_quickinspector_shared` and `Qt::Quick`; build `gammaray-mcp` and rerun the tool-list test until it passes.
- [ ] Commit with `git add mcp/CMakeLists.txt mcp/mcpserver.cpp mcp/mcpserver.h scripts/test-mcp-quick-capture.ps1; git commit -m "feat: expose Qt Quick MCP tools"`.

### Task 2: Implement Quick Item discovery

**Files:**
- Modify: `mcp/mcpserver.cpp`
- Modify: `mcp/mcpserver.h`
- Test: `scripts/test-mcp-quick-capture.ps1`

- [ ] Write the failing test that launches qml-rhi and calls `gammaray_list_quick_items`; it must return a non-empty item array containing a visible `QQuickItem`/ `TriangleRhiItem` with a QObject `objectPath`.
- [ ] Run the script and confirm it fails with an unknown tool or empty model before the implementation exists.
- [ ] Add `quickItemSnapshot(QAbstractItemModel *quickModel, QAbstractItemModel *objectModel, const QString &pattern, int maxDepth, int limit)`. Recursively read columns 0/1 and `ObjectModel::ObjectIdRole`, map the id through `indexForObjectId(objectModel, id)`, and emit `quickPath`, `objectPath`, `objectId`, `name`, `type`, and `depth`.
- [ ] Add `callListQuickItems()` using `scheduleStableSnapshot()`, build, and rerun the script until it finds the expected qml-rhi item.
- [ ] Commit with `git add mcp/mcpserver.cpp mcp/mcpserver.h scripts/test-mcp-quick-capture.ps1; git commit -m "feat: list Qt Quick items through MCP"`.

### Task 3: Implement Quick frame and item capture

**Files:**
- Modify: `mcp/mcpserver.cpp`
- Modify: `mcp/mcpserver.h`
- Test: `scripts/test-mcp-quick-capture.ps1`

- [ ] Write failing calls for `gammaray_grab_quick_window` and `gammaray_grab_quick_item` using the discovery result's `objectPath`; require one MCP image content item, PNG metadata, and positive dimensions.
- [ ] Run the script and confirm both calls fail before capture is implemented.
- [ ] Add `callGrabQuickImage(const QJsonValue &, const QJsonObject &, bool cropToItem)`. Resolve the QObject path to `ObjectId`, find/select the Quick Item model row, retrieve `com.kdab.GammaRay.QuickRemoteView` as `RemoteViewInterface`, call `setViewActive(true)` and `requestCompleteFrame()`, then disconnect after the first `frameUpdated`.
- [ ] For window capture pass the frame image to `sendImageToolResult()` with `captureKind: quickWindow`. For item capture read `frame.data.value<QuickItemGeometry>().itemRect`, map it from `frame.sceneRect()` to the pixel dimensions of `frame.image()`, intersect it with `frame.image().rect()`, reject an empty rectangle, and return `frame.image().copy(cropRect)` with `captureKind: quickItem` and `cropRect` metadata.
- [ ] Build and rerun the script until both PNG checks pass. Then run `.\scripts\test-mcp-gui-capture.ps1` to prove QWidget regression compatibility.
- [ ] Commit with `git add mcp/mcpserver.cpp mcp/mcpserver.h scripts/test-mcp-quick-capture.ps1; git commit -m "feat: capture Qt Quick scenes through MCP"`.

### Task 4: Document and validate the complete workflow

**Files:**
- Modify: `mcp/README.md`
- Modify: `scripts/test-mcp-quick-capture.ps1`

- [ ] Add a failing final test assertion that `gammaray_disconnect` returns `state: disconnected`.
- [ ] Extend the README's capability, tool, and screenshot sections with the three Quick tools, the Item crop contract, the Quick-specific errors, and the exact validation command.
- [ ] Run `.\scripts\build-qt611-mingw.ps1`, `cmake --build build-qt6.11.1-mingw --target gammaray-mcp`, `powershell -ExecutionPolicy Bypass -File .\scripts\test-mcp-quick-capture.ps1`, and `.\scripts\test-mcp-gui-capture.ps1`.
- [ ] Require all of: non-empty qml-rhi Quick tree, valid Quick-window PNG, valid cropped Quick-item PNG, passing QWidget regression, and clean disconnect.
- [ ] Commit with `git add mcp/README.md scripts/test-mcp-quick-capture.ps1; git commit -m "docs: document Qt Quick MCP validation"`.
