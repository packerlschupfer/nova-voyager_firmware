#!/bin/bash
# USB/IP connection script for Nova Voyager development
# ST-LINK via USB/IP for flashing, CH340 stays on Pi for serial

PI_HOST="192.168.16.62"
PI_USER="pi"

echo "=== Nova Voyager USB/IP Setup ==="

# Check Pi connectivity
if ! ping -c 1 -W 2 "$PI_HOST" >/dev/null 2>&1; then
    echo "ERROR: Pi not reachable at $PI_HOST"
    exit 1
fi

# Detach any existing connections
echo "Detaching existing USB/IP connections..."
for port in $(sudo usbip port 2>/dev/null | grep "^Port" | awk '{print $2}' | tr -d ':'); do
    sudo usbip detach -p "$port" 2>/dev/null
done

# Ensure Pi has usbipd running and ST-LINK bound (CH340 stays local on Pi)
echo "Setting up Pi ($PI_HOST)..."
ssh "$PI_USER@$PI_HOST" "sudo modprobe usbip-host 2>/dev/null; sudo usbipd -D 2>/dev/null; sudo usbip bind -b 1-1.2 2>/dev/null; sudo usbip unbind -b 1-1.5 2>/dev/null"

# Load VHCI module locally
echo "Loading vhci-hcd module..."
sudo modprobe vhci-hcd

# Attach ST-LINK only
echo "Attaching ST-LINK (1-1.2)..."
sudo usbip attach -r "$PI_HOST" -b 1-1.2

# Wait for device to enumerate
sleep 1

# Verify
echo ""
echo "=== Status ==="
if [ -e /dev/stlink-nova ]; then
    echo "ST-LINK: $(readlink -f /dev/stlink-nova)"
    st-info --probe 2>&1 | grep -E "version|chipid" | head -2
else
    echo "ST-LINK: NOT FOUND"
fi

# Check CH340 on Pi
echo ""
ssh "$PI_USER@$PI_HOST" "ls -la /dev/ttyNova 2>/dev/null && echo 'Serial on Pi: /dev/ttyNova ready' || echo 'Serial on Pi: NOT FOUND'"

echo ""
echo "Done."
echo "  Flash: pio run -e nova_voyager -t upload"
echo "  Serial: ssh $PI_USER@$PI_HOST \"python3 /home/pi/serial_cmd.py COMMAND\""
