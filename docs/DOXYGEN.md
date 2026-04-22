# API docs generation with Doxygen

This project supports a **hybrid docs workflow**:

- **Authored narrative docs** in Markdown (`README.md`, `docs/*.md`)
- **Auto-generated API reference** from inline comments in public headers (`src/tealet.h`, `src/tealet_extras.h`)

The Doxygen configuration is in `Doxyfile`.

## Tools used

- `doxygen` (required): parses C headers and Markdown, generates HTML docs
- `graphviz` / `dot` (optional but recommended): enables diagram rendering support used by Doxygen

## Install prerequisites

### Linux (Debian/Ubuntu)

```bash
sudo apt update
sudo apt install doxygen graphviz
```

### macOS (Homebrew)

```bash
brew install doxygen graphviz
```

### Windows

Using winget:

```powershell
winget install DimitriVanHeesch.Doxygen
winget install Graphviz.Graphviz
```

Using Chocolatey:

```powershell
choco install doxygen.install graphviz
```

Ensure `doxygen` is available on your `PATH`.

## Local commands

From repository root:

```bash
make docs
```

This generates HTML output at:

- `docs/_build/doxygen/html/index.html`

To remove generated docs:

```bash
make docs-clean
```

To run a warning check locally:

```bash
make docs-check
```

`docs-check` fails if Doxygen warnings are present in `docs/_build/doxygen/warnings.log`.

## Source-of-truth rules

- Keep declaration-level API docs in `src/tealet.h` and `src/tealet_extras.h`.
- Keep implementation notes in `.c` files lightweight and selective.
- Keep section banners non-Doxygen (`/* ... */`) to avoid accidental API extraction.
