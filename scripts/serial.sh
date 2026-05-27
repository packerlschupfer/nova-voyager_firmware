#!/bin/bash
# Serial helper script - uses SSH to localhost to avoid Claude FD leak bug
# Usage: ./serial.sh [command] [timeout]
#   command: send command and read response
#   (no args): read for 2 seconds

PORT="/dev/ttyNova"
TIMEOUT="${2:-2}"

if [ -n "$1" ]; then
    # Send command and read response
    ssh localhost "echo '$1' > $PORT; timeout $TIMEOUT cat $PORT 2>/dev/null" 2>/dev/null
else
    # Just read
    ssh localhost "timeout $TIMEOUT cat $PORT 2>/dev/null" 2>/dev/null
fi
