#!/bin/bash
# cleanup.sh
echo "Killing any lingering miniclaw processes..."
taskkill /F /IM miniclaw.exe 2>/dev/null
echo "Cleaning up build artifacts..."
rm -vf hello.exe hello_unique_123.exe hello.cpp
echo "Done. Please RESTART your terminal/IDE now."
