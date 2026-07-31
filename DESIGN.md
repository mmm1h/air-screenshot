---
version: alpha
name: Capture Console
description: A precise native Windows utility built around a dark command rail, open settings rows, and a visible capture workflow.
colors:
  primary: "#2764E7"
  primaryHover: "#1E55C8"
  primaryDark: "#5B8EFF"
  primaryDarkHover: "#75A1FF"
  accentTextLight: "#FFFFFF"
  accentTextDark: "#07101E"
  canvasLight: "#F6F8FC"
  canvasDark: "#0F141D"
  rail: "#091426"
  railDark: "#070B12"
  railSelectedLight: "#112441"
  railSelectedDark: "#101E36"
  surfaceLight: "#FFFFFF"
  surfaceDark: "#171E29"
  textLight: "#152033"
  textDark: "#F3F6FB"
  mutedLight: "#63708A"
  mutedDark: "#97A3B6"
  borderLight: "#DCE3EE"
  borderDark: "#293241"
  separatorLight: "#E7ECF3"
  separatorDark: "#27303E"
  hoverLight: "#EFF4FD"
  hoverDark: "#1A2433"
  accentSoftLight: "#E8F0FF"
  accentSoftDark: "#1B2D50"
  switchOffLight: "#97A5BC"
  switchOffDark: "#536074"
  railTextLight: "#F5F8FF"
  railTextDark: "#F4F7FC"
  railMutedLight: "#8D9CB5"
  railMutedDark: "#8A96A9"
  disabledLight: "#EDF1F6"
  disabledDark: "#202735"
  success: "#1AAFB5"
  successDark: "#42D5DD"
  danger: "#C8374D"
  dangerDark: "#FF7185"
  dangerSoftLight: "#FCE8EA"
  dangerSoftDark: "#3B1E25"
typography:
  title:
    fontFamily: "Segoe UI"
    fontSize: "22px"
    fontWeight: 600
  section:
    fontFamily: "Segoe UI"
    fontSize: "15px"
    fontWeight: 600
  body:
    fontFamily: "Segoe UI"
    fontSize: "13px"
    fontWeight: 600
  caption:
    fontFamily: "Segoe UI"
    fontSize: "12px"
    fontWeight: 400
  utility:
    fontFamily: "Segoe UI"
    fontSize: "11px"
    fontWeight: 400
  keycap:
    fontFamily: "Consolas"
    fontSize: "12px"
    fontWeight: 500
rounded:
  xs: "2px"
  sm: "4px"
  compact: "6px"
  field: "7px"
  md: "8px"
  lg: "10px"
  switch: "12px"
spacing:
  xs: "4px"
  sm: "8px"
  md: "12px"
  lg: "16px"
  xl: "24px"
  xxl: "32px"
  section: "48px"
components:
  appCanvasLight:
    backgroundColor: "{colors.canvasLight}"
    textColor: "{colors.textLight}"
  appCanvasDark:
    backgroundColor: "{colors.canvasDark}"
    textColor: "{colors.textDark}"
  buttonPrimaryLight:
    backgroundColor: "{colors.primary}"
    textColor: "{colors.accentTextLight}"
    typography: "{typography.body}"
    rounded: "{rounded.md}"
    height: "40px"
  buttonPrimaryDark:
    backgroundColor: "{colors.primaryDark}"
    textColor: "{colors.accentTextDark}"
    typography: "{typography.body}"
    rounded: "{rounded.md}"
    height: "40px"
  buttonSecondaryLight:
    backgroundColor: "{colors.surfaceLight}"
    textColor: "{colors.textLight}"
    typography: "{typography.body}"
    rounded: "{rounded.md}"
    height: "40px"
  buttonSecondaryDark:
    backgroundColor: "{colors.surfaceDark}"
    textColor: "{colors.textDark}"
    typography: "{typography.body}"
    rounded: "{rounded.md}"
    height: "40px"
  buttonDisabledLight:
    backgroundColor: "{colors.disabledLight}"
    rounded: "{rounded.md}"
    height: "40px"
  buttonDisabledDark:
    backgroundColor: "{colors.disabledDark}"
    rounded: "{rounded.md}"
    height: "40px"
  closeHoverLight:
    backgroundColor: "{colors.dangerSoftLight}"
    size: "48px"
  closeHoverDark:
    backgroundColor: "{colors.dangerSoftDark}"
    size: "48px"
  surfaceLight:
    backgroundColor: "{colors.surfaceLight}"
    textColor: "{colors.textLight}"
    rounded: "{rounded.lg}"
    padding: "16px"
  surfaceDark:
    backgroundColor: "{colors.surfaceDark}"
    textColor: "{colors.textDark}"
    rounded: "{rounded.lg}"
    padding: "16px"
  railNavigationLight:
    backgroundColor: "{colors.rail}"
    textColor: "{colors.railMutedLight}"
    typography: "{typography.body}"
    rounded: "{rounded.md}"
    height: "40px"
  railNavigationSelectedLight:
    backgroundColor: "{colors.railSelectedLight}"
    textColor: "{colors.railTextLight}"
    typography: "{typography.body}"
    rounded: "{rounded.md}"
    height: "40px"
  railNavigationDark:
    backgroundColor: "{colors.railDark}"
    textColor: "{colors.railMutedDark}"
    typography: "{typography.body}"
    rounded: "{rounded.md}"
    height: "40px"
  railNavigationSelectedDark:
    backgroundColor: "{colors.railSelectedDark}"
    textColor: "{colors.railTextDark}"
    typography: "{typography.body}"
    rounded: "{rounded.md}"
    height: "40px"
  helperTextLight:
    textColor: "{colors.mutedLight}"
    typography: "{typography.caption}"
  helperTextDark:
    textColor: "{colors.mutedDark}"
    typography: "{typography.caption}"
  rowHoverLight:
    backgroundColor: "{colors.hoverLight}"
    textColor: "{colors.textLight}"
    rounded: "{rounded.compact}"
  rowHoverDark:
    backgroundColor: "{colors.hoverDark}"
    textColor: "{colors.textDark}"
    rounded: "{rounded.compact}"
  groupBorderLight:
    backgroundColor: "{colors.borderLight}"
    height: "1px"
  groupBorderDark:
    backgroundColor: "{colors.borderDark}"
    height: "1px"
  dividerLight:
    backgroundColor: "{colors.separatorLight}"
    height: "1px"
  dividerDark:
    backgroundColor: "{colors.separatorDark}"
    height: "1px"
  statusHealthyLight:
    backgroundColor: "{colors.success}"
    size: "7px"
  statusHealthyDark:
    backgroundColor: "{colors.successDark}"
    size: "7px"
  statusDangerLight:
    backgroundColor: "{colors.danger}"
    size: "7px"
  statusDangerDark:
    backgroundColor: "{colors.dangerDark}"
    size: "7px"
  choiceSelectedLight:
    backgroundColor: "{colors.accentSoftLight}"
    textColor: "{colors.primaryHover}"
    typography: "{typography.body}"
    rounded: "{rounded.field}"
    height: "36px"
  choiceSelectedDark:
    backgroundColor: "{colors.accentSoftDark}"
    textColor: "{colors.primaryDarkHover}"
    typography: "{typography.body}"
    rounded: "{rounded.field}"
    height: "36px"
  appIconChoiceLight:
    backgroundColor: "{colors.surfaceLight}"
    textColor: "{colors.textLight}"
    typography: "{typography.body}"
    rounded: "{rounded.md}"
    width: "192px"
    height: "82px"
  appIconChoiceDark:
    backgroundColor: "{colors.surfaceDark}"
    textColor: "{colors.textDark}"
    typography: "{typography.body}"
    rounded: "{rounded.md}"
    width: "192px"
    height: "82px"
  appIconChoiceSelectedLight:
    backgroundColor: "{colors.accentSoftLight}"
    textColor: "{colors.primaryHover}"
    typography: "{typography.body}"
    rounded: "{rounded.md}"
    width: "192px"
    height: "82px"
  appIconChoiceSelectedDark:
    backgroundColor: "{colors.accentSoftDark}"
    textColor: "{colors.primaryDarkHover}"
    typography: "{typography.body}"
    rounded: "{rounded.md}"
    width: "192px"
    height: "82px"
  switchOnLight:
    backgroundColor: "{colors.primary}"
    rounded: "{rounded.switch}"
    width: "42px"
    height: "24px"
  switchOnDark:
    backgroundColor: "{colors.primaryDark}"
    rounded: "{rounded.switch}"
    width: "42px"
    height: "24px"
  switchOffLight:
    backgroundColor: "{colors.switchOffLight}"
    rounded: "{rounded.switch}"
    width: "42px"
    height: "24px"
  switchOffDark:
    backgroundColor: "{colors.switchOffDark}"
    rounded: "{rounded.switch}"
    width: "42px"
    height: "24px"
  shortcutFieldLight:
    backgroundColor: "{colors.surfaceLight}"
    textColor: "{colors.textLight}"
    typography: "{typography.keycap}"
    rounded: "{rounded.field}"
    width: "186px"
    height: "36px"
  shortcutFieldDark:
    backgroundColor: "{colors.surfaceDark}"
    textColor: "{colors.textDark}"
    typography: "{typography.keycap}"
    rounded: "{rounded.field}"
    width: "186px"
    height: "36px"
  workflowStripLight:
    backgroundColor: "{colors.surfaceLight}"
    textColor: "{colors.textLight}"
    rounded: "{rounded.lg}"
    width: "632px"
    height: "52px"
  workflowStripDark:
    backgroundColor: "{colors.surfaceDark}"
    textColor: "{colors.textDark}"
    rounded: "{rounded.lg}"
    width: "632px"
    height: "52px"
---

## Overview

**Creative North Star: "The Capture Console"**

The Capture Console treats settings as a native instrument panel for the real screenshot pipeline. A persistent near-black command rail sits beside a cool neutral work canvas; related decisions share open row groups, while individual settings never become a wall of detached cards.

The compact `截图 → 标注 → OCR → 输出` strip keeps cause and result visible before users commit changes. The application-icon family extends the same console identity with three genuinely different silhouettes, and its selection remains part of the draft until Save. Light, dark, and Windows High Contrast modes preserve the same semantic hierarchy: cobalt operates controls, cyan confirms healthy or synchronized state or marks a precise locator, and red identifies a problem that needs recovery.

**Key Characteristics:**

- Five stable destinations in a 224-DIP precision rail.
- Open 56-DIP settings rows and compact 40-DIP tool rows.
- Cobalt interaction, cyan health, and red recovery semantics.
- A 64-DIP footer that keeps save validity and draft state visible.
- Three built-in icon silhouettes sharing one palette and centered safe-area discipline.

## Colors

The palette is a semantic light/dark pair rather than a mechanical inversion, with a near-black rail anchoring both themes.

### Primary

- **Operational Cobalt:** The primary pair drives selection, keyboard focus, enabled switches, drag insertion, and the single save action; the hover pair strengthens interactive feedback.
- **Soft Cobalt:** The soft pair marks selected choices, active shortcut capture, and selected toolbar rows without turning them into detached cards.

### Semantic Status

- **Healthy / Locator Cyan:** The cyan pair appears when a workflow result, shortcut set, or saved state is healthy or synchronized, and as the single locator point inside application-icon artwork.
- **Recovery Red:** The danger pair marks shortcut conflicts, download failures, and close-hover risk; the soft danger pair supplies the corresponding hover surface.

### Neutral

- **Precision Rail:** The rail, selected-rail, rail-text, and rail-muted pairs keep navigation dark and legible in both themes.
- **Cool Canvas and Surface:** Canvas separates the work area from raised tonal surfaces; borders and separators define grouping without decorative depth.
- **Text and Muted Text:** Text carries decisions and labels; muted text carries concise dependency, consequence, or recovery guidance.
- **Hover, Switch-Off, and Disabled:** Dedicated pairs prevent interaction states from being derived through opacity or arbitrary blending.

### Named Rules

Application icons reuse Precision Rail ink, Operational Cobalt, and Healthy / Locator Cyan without introducing another palette. Their identity comes from silhouette and geometry, not recoloring.

**The Cobalt/Cyan Separation Rule.** Cobalt always means interaction or draft state; cyan only means healthy, ready, synchronized, or a precise locator point.

**The Semantic Pairing Rule.** Choose the named light or dark role directly; never generate one theme by inverting the other.

## Typography

**Interface Font:** Segoe UI
**Shortcut Font:** Consolas

**Character:** The hierarchy is compact, familiar, and native to Windows. Chinese remains the interface language; English is limited to product identity, font names, OCR labels, and recorded key combinations.

### Hierarchy

- **Title:** Semibold destination headings establish the current task.
- **Section:** Semibold group labels divide related decisions without oversized display type.
- **Body:** Semibold labels carry the actual setting or action.
- **Caption:** Regular helper and status copy explains consequence, dependency, or recovery in one sentence.
- **Utility:** The smallest regular role carries rail subtitles, toolbar state, and compact workflow labels.
- **Keycap:** Medium-weight Consolas records shortcuts and capture prompts.

### Named Rules

**The Chinese-First Utility Rule.** Keep interface copy in concise Chinese and reserve English for product identity or technical values already present in the product.

## Layout

The native renderer uses a 920 × 720-DIP design coordinate space and scales it uniformly to the available monitor work area and per-monitor DPI, leaving an 8-DIP safety margin. It does not introduce web-style responsive breakpoints or rearrange the information architecture at smaller sizes.

The 224-DIP rail and 48-DIP title bar stay fixed in the coordinate system. Main content occupies the 632-DIP span from 256 to 888 DIPs, with a stable left label column and right control column. Standard settings rows are 56 DIPs tall; the toolbar uses a denser 40-DIP pitch, and specialized OCR or shortcut rows expand only when their content requires it.

The bottom 64 DIPs form a persistent footer for status, Cancel, and the one primary Save action. Spacing follows a 4/8-DIP rhythm, with 12-, 16-, 24-, 32-, and 48-DIP steps used for nested gaps and section separation.

Within `应用与外观`, one open group presents three 56-DIP rows for the background service, tray visibility, and startup behavior. The application-icon section sits below the theme group and above the footer. One open group contains three 192 × 82-DIP choice targets with 8-DIP gaps; each target pairs a 40-DIP icon preview with a name and short descriptor while preserving the shared control column and footer boundary.

### Named Rules

**The Open Row Rule.** Place related settings inside one bounded group with internal separators; never wrap every setting in its own card.

**The Stable Control Column Rule.** Labels and helper copy remain left-aligned while controls share a predictable right edge.

## Elevation & Depth

Custom-painted content uses no shadows, blur, glass, glow, or gradients. Depth comes from canvas-to-surface contrast, dedicated hover fills, and 1-DIP borders and separators; only the system-managed outer window shadow remains.

### Named Rules

**The Native Depth Rule.** Keep all interior surfaces flat and tonal; reserve shadow ownership for the Windows window frame.

## Shapes

Geometry follows role rather than one universal radius. Progress tracks and checkboxes use tight corners; toolbar selection and the rail shortcut field are compact; choice and shortcut controls are slightly softer; buttons and navigation use medium corners; row groups, workflow strips, and inspectors use the largest container corner. The 42 × 24-DIP switch alone uses a 12-DIP capsule track.

Circular geometry is functional: switch thumbs, health/error dots, drag-grip dots, and icon details. No other control becomes pill-shaped.

The application-icon family shares a centered safe-area system and the same three colors while keeping truly different contours: Focus Frame uses a rounded tile with four targeting corners, Flow Lens uses a circular orbit and horizontal scan axis, and Pixel Console uses a square 3 × 3 module grid. Optical insets may vary at small sizes to preserve apparent weight.

### Named Rules

**The Nested Radius Rule.** Inner controls must remain equal to or tighter than the 10-DIP group that contains them.

**The Shared Palette, Distinct Silhouette Rule.** Keep the icon palette and safe-area discipline constant, but never reduce the three icon directions to recolors of one outline.

## Components

### Buttons

- Primary and secondary buttons use an 8-DIP radius and semibold body labels; the footer variants are 40 DIPs tall.
- Light primary buttons pair cobalt with light accent text; dark primary buttons pair bright cobalt with the dedicated dark accent foreground.
- Primary hover uses the theme-specific cobalt hover token. Secondary hover uses the hover surface plus a cobalt-hover border.
- Disabled buttons use the disabled surface, neutral border, and muted text. Keyboard focus is a 2-DIP expanded cobalt outline. State changes are immediate; the renderer defines no decorative transition.

### Open Settings Groups and Rows

- A standard group spans 632 DIPs, uses a 10-DIP radius, and carries a 1-DIP border.
- Standard rows are 56 DIPs high with label and helper text on the left and the control anchored to the right.
- Hover fills the row interior; separators begin after the text inset so the group border remains visually continuous.

### Switches

- The visual track is 42 × 24 DIPs with a 12-DIP radius and an 18-DIP circular thumb.
- The full row is the pointer and keyboard target. Disabled switches are removed from pointer hit testing and focus order.
- Hover adds a 1-DIP cobalt-hover outline; enabled-on, enabled-off, and disabled tracks each use dedicated semantic colors.
- Hiding the tray icon requires a confirmation that explains background behavior and the recovery command; cancelling leaves the draft unchanged.

### Choice Controls

- Standard choice controls are 36 DIPs tall with a 7-DIP radius and 1-DIP border.
- Selected choices use soft cobalt with cobalt-hover text and border; unselected choices use the surface, muted text, and neutral border.

### Application Icon Choices

- The open group contains three 192 × 82-DIP targets: **精准取景 / Focus Frame** (default targeting corners), **流光镜 / Flow Lens** (orbiting lens with scan axis), and **像素舱 / Pixel Console** (modular 3 × 3 grid).
- Every preview uses the same Precision Rail, Operational Cobalt, and Healthy / Locator Cyan palette and a centered safe-area discipline, while its outer contour and internal construction remain unmistakably different.
- Hover uses the normal hover surface. Selection uses soft cobalt, a 1.5-DIP cobalt-hover border, and a visible checked mark so state never depends on color alone; keyboard focus retains the 2-DIP expanded cobalt outline.
- The three targets form one keyboard choice group. Left/Right and Up/Down arrows wrap across choices, and pointer or keyboard selection changes only the settings draft.
- Saving applies the chosen resource to the tray and running window/taskbar HICON. The program file remains associated with the default Focus Frame icon in Explorer; the product does not rewrite its executable.
- Each ICO contains dedicated 16, 20, 24, 32, 40, 48, 64, 128, and 256 px images. Small frames are pixel-fitted and optically corrected rather than produced by simple downscaling.

**The Saved Runtime Icon Rule.** Treat the selected icon as draft state until Save; update runtime shell surfaces after Save while leaving the Explorer program-file icon on Focus Frame.

### Shortcut Fields

- Global shortcut fields are 186 × 36 DIPs; compact tool fields are 134 × 30 DIPs. Both use a 7-DIP radius and centered Consolas text.
- Recording changes the field to soft cobalt, strengthens the border to 1.5 DIPs, retains the entered value for correction, and exposes an Esc cancellation prompt without a modal interruption.
- The global group contains screenshot, clipboard pin, global OCR, and selection OCR. Save probes operating-system availability after local syntax and duplicate checks; screenshot remains required while clipboard pin may be empty.

### Navigation

- Five destinations live in the 224-DIP near-black rail. Each destination occupies a 200 × 40-DIP item on a 48-DIP pitch.
- The selected item uses the rail-selected surface, full rail text, and a 3 × 22-DIP cobalt indicator; inactive items use muted rail text.
- The rail also exposes the current screenshot shortcut in a compact bordered field.

### Workflow Strip

- The 632 × 52-DIP strip presents screenshot, annotation, OCR, and output in sequence.
- Active connectors and operational steps use cobalt. The completed output step uses cyan only as a healthy-result signal; disabled stages fall back to muted text and separators.

### Toolbar Reorder List

- Tool rows use a 40-DIP pitch with visible drag grips, an 18-DIP checkbox, a label, and an explicit shown/hidden status.
- Selection uses soft cobalt and drag insertion uses a 2-DIP cobalt line. Up/Down buttons and `Alt + ↑ / ↓` remain visible alternatives to dragging.

### Footer

- The 64-DIP footer uses a surface fill and 1-DIP top border. A 7-DIP dot plus text communicates synchronized, dirty, or invalid state.
- Cancel is a 92 × 40-DIP secondary button; Save is a 112 × 40-DIP primary button and remains disabled until the draft is both changed and valid.

## Do's and Don'ts

### Do:

- **Do** expose the real `截图 → 标注 → OCR → 输出` path and current save state.
- **Do** keep navigation order, visual order, and keyboard focus order aligned.
- **Do** pair health, draft, and error colors with explicit text.
- **Do** include cause and recovery in shortcut conflicts and OCR dependency failures.
- **Do** test light, dark, Windows High Contrast, keyboard-only use, per-monitor DPI, and the 820 × 640 minimum work area.
- **Do** preserve all three icon silhouettes, their shared three-color palette, centered safe-area discipline, and visible selected checkmark.
- **Do** generate every ICO frame at its target size with optical correction for small pixels.

### Don't:

- **Don't** invent destinations, cloud OCR, account state, commercial capabilities, or unsupported output formats.
- **Don't** use gradients, glass, glow, decorative shadows, decorative animation, oversized headlines, or card grids.
- **Don't** reuse cyan as a decorative accent or use rail text as the foreground on bright dark-mode cobalt.
- **Don't** hide critical actions behind hover, drag-only interaction, or disabled hit targets.
- **Don't** describe icon selection as dynamically rewriting the executable or changing its Explorer program-file icon.
- **Don't** create icon variants by recoloring one silhouette or by blindly scaling only the 256 px master.
