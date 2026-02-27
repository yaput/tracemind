# TraceMind

AI-powered root cause analysis for backend engineers.

Paste a stack trace or error log → get ranked hypotheses with fix suggestions, debug commands, and similar-error references. Works with Python, Go, Node.js traces and any generic log format.

## Features

- **Just works**: `tracemind crash.log` — no subcommand needed
- **Fix suggestions**: Each hypothesis includes actionable fix snippets
- **Debug commands**: Copy-pasteable shell commands to investigate further
- **Interactive mode**: Drill into hypotheses and ask follow-up questions (`-i`)
- **Explain anything**: `tracemind explain "segfault at 0x0"` — no file needed
- **Format-agnostic**: Stack traces (Python/Go/Node.js) and any log format (NGINX, K8s, syslog, custom)
- **Git-aware**: Correlates errors with recent commits, blame, and diffs
- **AST analysis**: Tree-sitter call graph extraction (optional)
- **Multiple outputs**: CLI (colored), Markdown, JSON
- **Local or cloud LLM**: OpenAI, Anthropic, or Ollama

## Installation

### Pre-built Binaries

Download the latest release from [GitHub Releases](https://github.com/tracemind/tracemind/releases):

```bash
# macOS (Apple Silicon)
curl -LO https://github.com/tracemind/tracemind/releases/latest/download/tracemind-macos-arm64.tar.gz
tar -xzf tracemind-macos-arm64.tar.gz
sudo mv tracemind /usr/local/bin/

# macOS (Intel)
curl -LO https://github.com/tracemind/tracemind/releases/latest/download/tracemind-macos-x86_64.tar.gz
tar -xzf tracemind-macos-x86_64.tar.gz
sudo mv tracemind /usr/local/bin/

# Linux (x86_64)
curl -LO https://github.com/tracemind/tracemind/releases/latest/download/tracemind-linux-x86_64.tar.gz
tar -xzf tracemind-linux-x86_64.tar.gz
sudo mv tracemind /usr/local/bin/

# Linux (aarch64)
curl -LO https://github.com/tracemind/tracemind/releases/latest/download/tracemind-linux-aarch64.tar.gz
tar -xzf tracemind-linux-aarch64.tar.gz
sudo mv tracemind /usr/local/bin/
```

#### macOS Security Note

macOS may block the downloaded binary with a "cannot be opened because the developer cannot be verified" error. To fix this:

**Option 1: Remove quarantine attribute**
```bash
xattr -d com.apple.quarantine /usr/local/bin/tracemind
```

**Option 2: Allow in System Preferences**
1. Try to run `tracemind` once (it will be blocked)
2. Go to System Preferences → Privacy & Security
3. Click "Allow Anyway" next to the tracemind message
4. Run `tracemind` again and click "Open"

### Build from Source

#### Prerequisites

- GCC or Clang (C11 support)
- libcurl
- jansson (JSON library)
- libgit2 (optional)
- tree-sitter (optional, for enhanced AST analysis)

#### macOS

```bash
# Install dependencies
brew install curl jansson libgit2 tree-sitter

# Build
make deps-mac
make release
sudo make install
```

#### Linux (Ubuntu/Debian)

```bash
# Install dependencies
sudo apt-get install libcurl4-openssl-dev libjansson-dev libgit2-dev

# Build
make deps-linux
make release
sudo make install
```

## Quick Start

```bash
# Set your API key
export OPENAI_API_KEY="sk-..."

# Analyze a crash log (no subcommand needed)
tracemind crash.log

# Pipe from stderr
python app.py 2>&1 | tracemind -

# Interactive mode — drill into hypotheses, ask follow-ups
tracemind crash.log -i

# Explain an error message without a file
tracemind explain "ENOMEM: cannot allocate 2GB region"

# Output as markdown
tracemind crash.log -o markdown > report.md
```

## Usage

```
tracemind <file>                 Analyze stack trace / log file
tracemind -                      Read from stdin
tracemind explain <error>        Explain an error string (no file needed)
tracemind analyze <file>         Explicit analyze subcommand (also works)

OPTIONS:
    -i, --interactive        Interactive follow-up mode
    -p, --provider <name>    LLM provider: openai, anthropic, local
    -m, --model <name>       Model name (e.g., gpt-4o, claude-sonnet-4-20250514)
    -k, --api-key <key>      API key (or use env var)
    -o, --output <format>    Output: cli (default), markdown, json
    -f, --format <type>      Input format: auto, raw, json, csv, generic
    -a, --analysis <mode>    Analysis mode: auto, trace, log
    -r, --repo <path>        Repository path (auto-detected if omitted)
    -v, --verbose            Enable verbose/debug output
```

## What You Get

Each hypothesis now includes:

| Section | Description |
|---------|-------------|
| **Explanation** | Root cause analysis with confidence score |
| **Suggested Fix** | Code snippet or config change to resolve the issue |
| **Debug Commands** | Shell commands to investigate further |
| **Similar Errors** | Related error patterns and their known causes |
| **Next Step** | Recommended immediate action |

## Log Format Support

TraceMind auto-detects input type. No configuration needed for common formats.

| Input | Detection |
|-------|-----------|
| Python traceback | `Traceback (most recent call last)` |
| Go panic | `goroutine N [running]:` |
| Node.js error | `at Function (file:line:col)` |
| JSON structured | Lines starting with `{` |
| Syslog | RFC 3164/5424 |
| NGINX / Apache | Combined log format |
| Docker / K8s | Container log patterns |

```bash
# Any log file (auto-detect)
tracemind nginx_error.log

# Force generic log mode
tracemind -a log access.log

# Docker / Kubernetes
docker logs my-container 2>&1 | tracemind -
kubectl logs my-pod | tracemind -

# Cloud log exports (GCP, etc.)
tracemind gcp_logs.json -f json
```

## Configuration

Config priority: CLI flags > environment variables > `~/.config/tracemind/config.json`

### Environment Variables

| Variable | Description |
|----------|-------------|
| `OPENAI_API_KEY` | OpenAI API key |
| `ANTHROPIC_API_KEY` | Anthropic API key |
| `TRACEMIND_PROVIDER` | Default provider (`openai`, `anthropic`, `local`) |
| `TRACEMIND_MODEL` | Default model name |
| `TRACEMIND_DEBUG` | Enable debug output (`1` or `true`) |

### Config File

```json
{
  "provider": "openai",
  "model": "gpt-4o",
  "timeout_ms": 60000,
  "temperature": 0.3,
  "max_commits": 20,
  "output_format": "cli",
  "color": true
}
```

### Local LLM (Ollama)

```bash
# Start Ollama
ollama serve

# Use with TraceMind
tracemind crash.log -p local -m llama3
```

## Building

### Dependencies

| Dependency | Required | Purpose |
|-----------|----------|---------|
| libcurl | Yes | HTTP client for LLM APIs |
| jansson | Yes | JSON parsing |
| tree-sitter | No | AST / call graph analysis |
| libgit2 | No | Git context (blame, commits) |

Optional dependencies are auto-detected. Override with `make HAVE_TREE_SITTER=0` or `make HAVE_LIBGIT2=0`.

### macOS

```bash
brew install curl jansson libgit2 tree-sitter
make release
sudo make install
```

### Linux (Ubuntu/Debian)

```bash
sudo apt-get install libcurl4-openssl-dev libjansson-dev libgit2-dev
make release
sudo make install
```

### Build targets

```bash
make              # Release build (default)
make debug        # Debug build with sanitizers
make test         # Run tests
make info         # Show detected features
make help         # All available targets
make uninstall    # Remove from system
```

## Architecture

```
Input (file/stdin/string)
        │
        ▼
┌─────────────────┐
│  Parse & Detect  │  Stack trace parser (Python/Go/Node.js)
│                  │  or generic log classifier
└────────┬────────┘
         │
    ┌────▼────┐  ┌──────────┐
    │ AST/CG  │  │ Git Ctx  │  Optional: tree-sitter, libgit2
    └────┬────┘  └────┬─────┘
         │            │
         ▼            ▼
┌─────────────────────────────┐
│     LLM Hypothesis Engine   │  OpenAI / Anthropic / Ollama
│  → fix suggestions          │
│  → debug commands            │
│  → similar errors            │
└────────────┬────────────────┘
             │
             ▼
┌─────────────────┐
│  Output / REPL   │  CLI (colored) / Markdown / JSON
└─────────────────┘   Interactive follow-up mode
```

## License

MIT License
