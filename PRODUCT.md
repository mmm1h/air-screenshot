# Air Screenshot

## Product

Air Screenshot is a native Windows capture utility for fast region capture, lightweight annotation, local OCR, clipboard/file output, pinned images, and global shortcuts. It is an Operate product: speed, predictability, keyboard access, and trustworthy system state matter more than decorative expression.

## Settings Surface

The settings window configures six product areas:

1. **截图与输出** — annotation behavior, clipboard or PNG output, and completion notifications.
2. **文本与 OCR** — local OCR enablement, the three supported recognition profiles, signed dependency state/download, and annotation typography.
3. **工具栏** — tool visibility and order, with drag, buttons, and keyboard alternatives.
4. **快捷键** — global screenshot, clipboard pin, OCR, and annotation-tool bindings with inline validation and conflict recovery.
5. **应用与外观** — background service, tray visibility, startup behavior, system/light/dark themes, and one of three built-in application icons: 精准取景 (Focus Frame, default), 流光镜 (Flow Lens), or 像素舱 (Pixel Console).
6. **更新与安全** — automatic-update preference, verification guarantees, installation safety, and the explicit boundary between saving a preference and manually checking for an update.

The surface must never imply unsupported output destinations, cloud OCR, account state, or commercial capabilities.

## Experience Contract

- **THESIS:** Make the real `截图 → 标注 → OCR → 输出` path visible and controllable; refuse the generic card-wall settings page.
- **OWN-WORLD:** A near-black precision rail, cool neutral canvas, open setting rows, cobalt interaction states, and cyan only for healthy or synchronized state.
- **STORY:** Users choose a capture result, tune annotation and OCR, arrange tools, record shortcuts, select a built-in application icon, set update policy as part of the draft, and understand exactly what Save will change.
- **FIRST VIEWPORT:** Six destinations live in a 224-DIP rail; the selected task appears beside a compact workflow strip; save state is anchored to a 64-DIP footer.
- **FORM:** `Capture Console`, seeded by Stitch screen `450cf4261edd46d287f5f04e5a5a7428` in project `454227981142275123`; the icon family began in session `8313762202511579579`, with Flow Lens refined in screen `bdd901cfa6904c73b36e187a8a39c930`.

## Interaction Principles

- Preserve product truth and reveal dependent controls progressively.
- Keep one primary action: **保存更改**. Cancel and close discard the draft.
- Show shortcut problems inline, retain the entered value for correction, focus the shortcut destination, and block Save until valid.
- Probe global shortcut availability before Save; keep the screenshot shortcut required and allow clipboard pin to remain unassigned.
- Confirm before hiding the tray icon and explain how to reopen Settings while the background service continues running.
- Show OCR download progress without freezing the rest of the window.
- Keep automatic-update preference separate from the manual tray action. Disabling automatic updates cancels only automatic work and pauses automatic-origin pending installation; it must not erase explicit manual intent.
- Support full keyboard navigation, visible focus, `Ctrl + S`, `Esc`, and `Alt + ↑ / ↓` as a toolbar drag alternative.
- Keep application-icon selection in the settings draft until Save; support hover, visible keyboard focus, and arrow-key movement across the three choices.
- After Save, update the tray icon and running window/taskbar icons. Keep the program file's Explorer icon on the default Focus Frame resource; this feature does not rewrite the executable.
- Remove disabled actions from pointer hit testing and keyboard focus.
- Adapt to Windows High Contrast and per-monitor DPI.

## Quality Bar

- Reference size: 920 × 720 DIPs.
- Minimum useful work area: 820 × 640 without clipping.
- Light and dark modes are designed together; normal text targets WCAG AA contrast.
- Depth comes from surface contrast and 1-DIP borders, not gradients, glass, glow, or decorative shadows.
- Chinese is the primary interface language. English is reserved for product identity and technical values such as font names and shortcuts.
- The native window, real interaction path, and actual configuration schema are the source of truth; the Stitch composition governs structure, not invented semantics.

## Acceptance Evidence

- Debug build succeeds with warnings treated as errors.
- All repository tests pass.
- The six destinations, light/dark modes, 820 × 640 scaling, toolbar drag/visibility, clipboard-pin shortcut, tray visibility, inline shortcut errors, update-policy draft, and the three application-icon choices are visually exercised in the native window.
- The application-icon selector is exercised by pointer, keyboard focus, and Left/Right arrow keys; selection remains dirty until Save and is discarded by Cancel or close.
- Each Focus Frame, Flow Lens, and Pixel Console ICO contains 16, 20, 24, 32, 40, 48, 64, 128, and 256 px images produced with small-size optical correction rather than simple master-image downscaling.
- Saving an icon choice updates the tray and running window/taskbar HICON resources, while Explorer continues to show the default Focus Frame program-file icon.
- `DESIGN.md` is the maintained visual-system contract and must stay aligned with the shipped implementation.
