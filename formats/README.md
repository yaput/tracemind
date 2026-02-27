# TraceMind Format Definitions

This directory contains format definition files that teach TraceMind how to parse specific log formats.

## Overview

TraceMind uses a plugin-based architecture for log format handling:
- **Built-in detection**: Common formats (JSON structured, syslog, etc.) are auto-detected
- **Custom definitions**: Add YAML files here for application-specific formats

## Directory Locations

TraceMind searches for format definitions in order:
1. `./formats/` (current directory)
2. `~/.config/tracemind/formats/` (user config)
3. `/etc/tracemind/formats/` (system-wide)

## Creating a Format Definition

Create a YAML file with the following structure:

```yaml
name: my_app_log
version: "1.0"
description: "My Application Log Format"

# Regex pattern with named capture groups
patterns:
  line: '^(?P<timestamp>\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2}) \[(?P<level>\w+)\] (?P<message>.*)$'

# Map captured groups to standard fields
field_mappings:
  timestamp: timestamp
  severity: level
  message: message

# Normalize severity levels
severity_mapping:
  err: ERROR
  warning: WARNING

# Keywords that indicate error conditions
error_indicators:
  - "connection failed"
  - "timeout"
```

## Field Reference

### Required Fields
- `name`: Unique identifier for the format
- `patterns.line`: Regex with named groups to parse log lines

### Standard Capture Groups
- `timestamp`: Log entry timestamp
- `level`/`severity`: Log level (ERROR, WARN, INFO, DEBUG)
- `message`: Main log message content
- `source`: Source file/component

### Optional Sections
- `severity_mapping`: Maps custom levels to standard levels
- `error_indicators`: Strings that suggest error conditions
- `context_patterns`: Additional patterns to extract context

## Testing Your Format

```bash
# Test format detection
tracemind --detect-format myapp.log

# Analyze with explicit format
tracemind analyze --format my_app_log myapp.log
```

## Built-in Formats

TraceMind auto-detects these formats without definitions:
- Python/Go/Node.js stack traces
- JSON structured logging (various schemas)
- Syslog (RFC 3164, RFC 5424)
- Generic timestamp + level + message patterns
