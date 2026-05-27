#!/bin/bash
# Quick reset script for Nova Voyager board

echo "Resetting Nova Voyager..."
st-flash reset 2>&1 | grep -E "reset|NRST" || \
openocd -f openocd_gd32.cfg -c "init; reset; shutdown" 2>&1 | grep -v "Info\|Debug"

echo "Done!"
