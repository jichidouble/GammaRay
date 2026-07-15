# GammaRay MCP Qt Quick Support Design

## Goal

Expose Qt Quick scene inspection and in-process PNG capture through `gammaray-mcp`, and verify the flow against `E:\Dev\qml-rhi`.

## Scope

The MCP server will add three Quick-specific tools while preserving all existing QObject and QWidget tools:

- `gammaray_list_quick_items` returns the active `QQuickWindow` scene's `QQuickItem` hierarchy. Every result includes the matching object-tree `objectPath`, so callers can use the existing property tool without a second identifier type.
- `gammaray_grab_quick_window` returns the active Quick Inspector remote-view frame as `image/png`.
- `gammaray_grab_quick_item` resolves an object-tree `objectPath` to a `QQuickItem`, requests the same frame, and crops it to the item's mapped scene rectangle. The returned metadata identifies both the source window frame and the crop rectangle.

The implementation is intentionally based on the existing Quick Inspector protocol rather than OS screen capture or target-side helper code. It works through the target's injected probe and therefore retains GammaRay's compatibility and security model.

## Architecture

Quick Inspector already registers `com.kdab.GammaRay.QuickItemModel` and `com.kdab.GammaRay.QuickRemoteView`. The MCP server will retrieve those remote objects through `ObjectBroker`, just as it currently accesses the Widget Inspector model and remote view.

For item discovery, the server snapshots the Quick Item model, reads each row's `ObjectId`, and resolves that ID through the QObject-tree model to supply a stable `objectPath`. For capture, the server resolves the requested QObject path to an `ObjectId`, selects the matching Quick Item model row, waits for `QuickRemoteView::frameUpdated`, and converts the resulting `RemoteViewFrame` to PNG. The Quick Inspector places the selected item's `QuickItemGeometry` in `RemoteViewFrame::data`; item capture reads its `itemRect`, maps it from the frame's `sceneRect` to image pixels, clamps the rectangle to the frame bounds, and crops it. Window capture returns the complete frame.

## Error Handling

All Quick tools require a ready session. They report a normal MCP tool error when the Quick Inspector models or remote view are unavailable, the path is invalid or not a current-scene `QQuickItem`, the capture frame is invalid, the selected item has a zero-sized/out-of-frame geometry, or the capture times out. A single shared capture guard prevents overlapping Widget and Quick requests.

## Build and Compatibility

`gammaray-mcp` will link the existing Quick Inspector shared library and `Qt::Quick`, allowing it to use Quick Inspector's public model roles and Qt Quick geometry types. The existing Widget capture implementation and public tool names remain unchanged.

## Verification

Automated MCP coverage will verify tool advertisement, Quick model snapshots, request validation, and capture result metadata. End-to-end validation will build the server, launch `E:\Dev\qml-rhi\build\qml_rhi.exe` with a compatible Qt/MinGW environment and OpenGL RHI backend, confirm the Quick item tree contains the expected scene, obtain a valid window PNG, obtain a non-empty crop for a visible QML item, and disconnect cleanly.
