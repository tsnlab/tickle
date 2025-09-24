#!/bin/bash

# Script to remove trailing whitespaces from source files
# Usage: ./remove_trailing_whitespace.sh [directory]

set -e

# Default directory is current directory
TARGET_DIR="${1:-.}"

echo "Removing trailing whitespaces from files in: $TARGET_DIR"

# Find and process C/C++ source files
find "$TARGET_DIR" -type f \( -name "*.c" -o -name "*.h" -o -name "*.cpp" -o -name "*.hpp" -o -name "*.cc" -o -name "*.cxx" \) \
    -not -path "*/build/*" \
    -not -path "*/install/*" \
    -not -path "*/.git/*" \
    -not -path "*/node_modules/*" \
    -exec sed -i 's/[[:space:]]*$//' {} \;

# Find and process CMake files
find "$TARGET_DIR" -type f \( -name "CMakeLists.txt" -o -name "*.cmake" \) \
    -not -path "*/build/*" \
    -not -path "*/install/*" \
    -not -path "*/.git/*" \
    -exec sed -i 's/[[:space:]]*$//' {} \;

# Find and process other text files
find "$TARGET_DIR" -type f \( -name "*.py" -o -name "*.sh" -o -name "*.yml" -o -name "*.yaml" -o -name "*.xml" -o -name "*.json" \) \
    -not -path "*/build/*" \
    -not -path "*/install/*" \
    -not -path "*/.git/*" \
    -not -path "*/node_modules/*" \
    -exec sed -i 's/[[:space:]]*$//' {} \;

echo "Trailing whitespaces removed successfully!"

# Show git status to see what files were modified
echo ""
echo "Modified files:"
git status --porcelain | grep "^ M" || echo "No modified files found"
