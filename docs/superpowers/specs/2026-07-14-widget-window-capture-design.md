# Widget and Window Capture Design

## Goal

Expose GammaRay's QWidget rendering capability through the existing stdio MCP server so an MCP client can inspect a GUI application's live visual state.

## Chosen architecture

`gammaray_grab_widget` and `gammaray_grab_window` both accept an `objectPath` produced by `gammaray_list_objects`. The server resolves its `ObjectId`, selects the same object in GammaRay's remote Widget Tree, then requests an image from the target process.

- Widget capture adds `WidgetInspectorInterface::requestWidgetScreenshot()`. The Widget Inspector renders its current selected QWidget to a `RemoteViewFrame` and sends the frame back over GammaRay's existing remote-object protocol.
- Window capture reuses `com.kdab.GammaRay.WidgetRemoteView`, which already renders the selected widget's top-level QWidget and sends a `RemoteViewFrame` to a remote client.

This avoids temporary files, OS desktop capture permissions, and OCR. It also preserves GammaRay's object-level targeting: a caller can capture a child button or the window containing it from one stable QObject path.

## MCP contract

Both tools are read-only and idempotent. Inputs are `objectPath`, optional `maxWidth` and `maxHeight` (default 1920x1080), and `timeoutMs` (default 10 seconds). Images over the requested bounds are downscaled with their aspect ratio preserved.

The response uses MCP `content` entries: compact text metadata followed by `{ "type": "image", "data": "<base64>", "mimeType": "image/png" }`. `structuredContent` contains `captureKind`, the selected path and object id, original/output dimensions, scaling state, and MIME type. The implementation serializes one capture at a time to avoid conflicting remote-widget selections.

## Failure behavior

The tools report MCP tool errors if no target session is ready, the object path is stale, it is not present in the Widget Tree, the Widget Inspector is unavailable, a remote frame is invalid, or the timeout expires. No target-side file is created in either the success or error path.

## Verification

`gammaray-mcp-gui-test-target` is a visible `QApplication` fixture with a named main window and named button. `scripts/test-mcp-gui-capture.ps1` launches it through MCP, discovers `screenshotButton`, captures the widget and its window, validates PNG signatures and MCP image fields, and confirms the widget source image is shorter than the full window image.
