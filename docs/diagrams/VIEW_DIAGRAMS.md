# How to View PlantUML Diagrams

The PlantUML diagrams in this directory work perfectly online and in modern editors.

## Recommended Methods

### ⭐ Method 1: Online Viewer (Easiest)
**URL:** http://www.plantuml.com/plantuml/uml/

1. Copy entire content of any `.puml` file
2. Paste into online editor
3. View rendered diagram instantly

**Best for:** Quick viewing, no installation needed

### ⭐ Method 2: VS Code (Best for Development)
1. Install "PlantUML" extension
2. Open `.puml` file
3. Press `Alt+D` to preview
4. Live preview as you edit

**Best for:** Development, editing diagrams

### Method 3: IntelliJ/CLion
- Built-in PlantUML support
- Right-click → "Show Diagram"

### Method 4: Command Line
```bash
plantuml docs/diagrams/*.puml  # Generates PNGs
```

**Note:** Requires PlantUML >= 2021. Local version (2020) doesn't support all syntax.

## Viewing Order

**New developers:** 09 → 01 → 02
**Debugging:** 07 → 11 → 12
**Development:** 01 → 08 → 03

---
All diagrams are version-controlled text files and render perfectly online/VS Code.
