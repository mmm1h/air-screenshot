# Air Screenshot

## Product

Air Screenshot is a native Windows capture utility for fast region capture, lightweight annotation, local OCR, clipboard/file output, pinned images, and global shortcuts. It is an Operate product: speed, predictability, keyboard access, and trustworthy system state matter more than decorative expression.

## Settings Surface

The settings window configures five product areas:

1. **截图与输出** — annotation behavior, clipboard or PNG output, completion notifications, and serial reset.
2. **文本与 OCR** — local OCR enablement, the three supported recognition engines, dependency state/download, and annotation typography.
3. **工具栏** — tool visibility and order, with drag, buttons, and keyboard alternatives.
4. **快捷键** — global and annotation-tool bindings with inline validation and conflict recovery.
5. **应用与外观** — background service/startup behavior and system/light/dark themes.

The surface must never imply unsupported output destinations, cloud OCR, account state, or commercial capabilities.

## Experience Contract

- **THESIS:** Make the real `截图 → 标注 → OCR → 输出` path visible and controllable; refuse the generic card-wall settings page.
- **OWN-WORLD:** A near-black precision rail, cool neutral canvas, open setting rows, cobalt interaction states, and cyan only for healthy or synchronized state.
- **STORY:** Users choose a capture result, tune annotation and OCR, arrange tools, record shortcuts, and understand exactly what Save will change.
- **FIRST VIEWPORT:** Five destinations live in a 224-DIP rail; the selected task appears beside a compact workflow strip; save state is anchored to a 64-DIP footer.
- **FORM:** `Capture Console`, seeded by Stitch screen `450cf4261edd46d287f5f04e5a5a7428` in project `454227981142275123`.

## Interaction Principles

- Preserve product truth and reveal dependent controls progressively.
- Keep one primary action: **保存更改**. Cancel and close discard the draft.
- Show shortcut problems inline, retain the entered value for correction, focus the shortcut destination, and block Save until valid.
- Show OCR download progress without freezing the rest of the window.
- Support full keyboard navigation, visible focus, `Ctrl + S`, `Esc`, and `Alt + ↑ / ↓` as a toolbar drag alternative.
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
- The five destinations, light/dark modes, 820 × 640 scaling, toolbar drag/visibility, and inline shortcut errors are visually exercised in the native window.
- `DESIGN.md` is the maintained visual-system contract and must stay aligned with the shipped implementation.
