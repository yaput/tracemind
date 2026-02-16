# TraceMind

AI-Powered Root Cause Analysis for Backend Engineers

TraceMind analyzes stack traces, call graphs, and git history to provide high-probability root cause hypotheses for debugging backend errors.

## Features

- **Multi-Language Support**: Python, Go, Node.js stack trace parsing
- **Call Graph Analysis**: Tree-sitter based AST parsing for function relationships
- **Git Context**: Recent commits, blame info, and diffs for affected files
- **LLM Integration**: OpenAI and Anthropic API support for hypothesis generation
- **Multiple Output Formats**: CLI (colored), Markdown, JSON

## Installation

### Prerequisites

- GCC or Clang (C11 support)
- libcurl
- jansson (JSON library)
- libgit2
- tree-sitter (with Python, Go, JavaScript grammars)

### macOS

```bash
# Install dependencies
brew install curl jansson libgit2 tree-sitter

# Build
make deps-mac
make release
```

### Linux (Ubuntu/Debian)

```bash
# Install dependencies
sudo apt-get install libcurl4-openssl-dev libjansson-dev libgit2-dev

# Build
make deps-linux
make release
```

## Quick Start

```bash
# Set your API key
export OPENAI_API_KEY="sk-..."

# Analyze a stack trace file
tracemind analyze crash.log

# Pipe from stderr
python app.py 2>&1 | tracemind analyze -

# Output as markdown
tracemind analyze trace.txt -o markdown > report.md
```

## Usage

```
tracemind analyze <file>         Analyze stack trace from file
tracemind analyze -              Read stack trace from stdin
cat trace.txt | tracemind        Pipe stack trace to analyzer

OPTIONS:
    -p, --provider <name>    LLM provider: openai, anthropic, local
    -m, --model <name>       Model name (e.g., gpt-4o, claude-sonnet-4-20250514)
    -k, --api-key <key>      API key (or use env var)
    -o, --output <format>    Output: cli (default), markdown, json
    -r, --repo <path>        Repository path (auto-detected if omitted)
    -v, --verbose            Enable verbose/debug output
```

## Configuration

TraceMind loads configuration from:
1. `~/.config/tracemind/config.json`
2. Environment variables
3. Command-line arguments (highest priority)

### Environment Variables

- `OPENAI_API_KEY` - OpenAI API key
- `ANTHROPIC_API_KEY` - Anthropic API key
- `TRACEMIND_MODEL` - Default model name
- `TRACEMIND_PROVIDER` - Default provider (openai, anthropic, local)
- `TRACEMIND_DEBUG` - Enable debug output (1 or true)

### Config File Example

```json
{
  "provider": "openai",
  "model": "gpt-4o",
  "timeout_ms": 60000,
  "temperature": 0.3,
  "max_commits": 20,
  "max_call_depth": 5,
  "output_format": "cli",
  "color": true
}
```

## Output Example

```
╔══════════════════════════════════════════════════════════════╗
║                    TraceMind Analysis                         ║
╚══════════════════════════════════════════════════════════════╝

📋 Stack Trace Summary
━━━━━━━━━━━━━━━━━━━━━━
  Language: Python
  Frames:   3
  Error:    psycopg2.errors.SyntaxError: syntax error...

🔍 Root Cause Hypotheses
━━━━━━━━━━━━━━━━━━━━━━━━

  ┌─────────────────────────────────────────────────────────┐
  │ #1 LIKELY CAUSE (85% confidence)                        │
  │                                                         │
  │ The SQL query template in _run_query() has a missing    │
  │ comma between column names, causing the syntax error.   │
  │                                                         │
  │ File: handlers.py:203                                   │
  │ Suggested Fix: Add comma between 'name' and 'email'     │
  └─────────────────────────────────────────────────────────┘
```

## Architecture

```
┌──────────────────────────────────────────────────────────────┐
│                        TraceMind                             │
├──────────────────────────────────────────────────────────────┤
│  CLI Interface                                               │
│  ├── Argument Parser                                         │
│  └── Configuration Loader                                    │
├──────────────────────────────────────────────────────────────┤
│  Analysis Pipeline                                           │
│  ├── Stack Trace Parser (Python/Go/Node.js)                  │
│  ├── AST Analyzer (Tree-sitter)                              │
│  ├── Git Context Collector (libgit2)                         │
│  └── Hypothesis Generator (LLM)                              │
├──────────────────────────────────────────────────────────────┤
│  Output Formatter                                            │
│  ├── CLI (ANSI colors)                                       │
│  ├── Markdown                                                │
│  └── JSON                                                    │
└──────────────────────────────────────────────────────────────┘
```

## Development

```bash
# Build debug version
make debug

# Run tests
make test

# Clean
make clean
```

## License

MIT License

## Contributing

Contributions welcome! Please read CONTRIBUTING.md for guidelines.
